//go:build ignore

package main

import (
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

func main() {
	fs := flag.NewFlagSet("gitserver", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)

	root := fs.String("root", "", "root directory containing bare repositories")
	timeout := fs.Duration("timeout", 30*time.Second, "auto-shutdown timeout")
	quiet := fs.Bool("quiet", false, "suppress log output")

	if err := fs.Parse(os.Args[1:]); err != nil {
		os.Exit(1)
	}

	if *root == "" {
		fmt.Fprintln(os.Stderr, "error: --root is required")
		os.Exit(1)
	}
	rootAbs, err := filepath.Abs(*root)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: resolve root path: %v\n", err)
		os.Exit(1)
	}

	gitBin := os.Getenv("GIT")
	if gitBin == "" {
		gitBin = "git"
	}
	if _, err := exec.LookPath(gitBin); err != nil {
		fmt.Fprintf(os.Stderr, "error: git command not found: %v\n", err)
		os.Exit(1)
	}

	gs := &gitServer{
		root: rootAbs,
		git:  gitBin,
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/", gs.handler)
	srv := &http.Server{Handler: mux}

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: listen: %v\n", err)
		os.Exit(1)
	}

	baseURL := "http://" + ln.Addr().String()
	fmt.Println(baseURL)

	timer := time.AfterFunc(*timeout, func() {
		if !*quiet {
			fmt.Fprintln(os.Stderr, "gitserver: timeout reached, shutting down")
		}
		_ = srv.Close()
	})

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sig
		timer.Stop()
		_ = srv.Close()
	}()

	if err := srv.Serve(ln); err != nil && err != http.ErrServerClosed {
		fmt.Fprintf(os.Stderr, "error: serve: %v\n", err)
		os.Exit(1)
	}
}

type gitServer struct {
	root string
	git  string
}

func (gs *gitServer) handler(w http.ResponseWriter, r *http.Request) {
	repoPath, ok := gs.repoPath(r.URL.Path)
	if !ok {
		http.NotFound(w, r)
		return
	}

	switch r.Method {
	case http.MethodGet:
		gs.handleInfoRefs(w, r, repoPath)
	case http.MethodPost:
		gs.handleUploadPack(w, r, repoPath)
	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (gs *gitServer) repoPath(urlPath string) (string, bool) {
	var repo string
	switch {
	case strings.HasSuffix(urlPath, "/info/refs"):
		repo = strings.TrimPrefix(urlPath[:len(urlPath)-len("/info/refs")], "/")
	case strings.HasSuffix(urlPath, "/git-upload-pack"):
		repo = strings.TrimPrefix(urlPath[:len(urlPath)-len("/git-upload-pack")], "/")
	default:
		return "", false
	}
	if repo == "" || containsDotDot(repo) {
		return "", false
	}
	repoPath := filepath.Join(gs.root, repo)
	if _, err := os.Stat(repoPath); err != nil {
		return "", false
	}
	return repoPath, true
}

func (gs *gitServer) handleInfoRefs(w http.ResponseWriter, r *http.Request, repo string) {
	q := r.URL.Query()
	if q.Get("service") != "git-upload-pack" {
		http.NotFound(w, r)
		return
	}

	out, err := gs.uploadPack(repo, []string{"--stateless-rpc", "--advertise-refs"}, nil, r.Header.Get("Git-Protocol"))
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "application/x-git-upload-pack-advertisement")
	w.Header().Set("Cache-Control", "no-cache")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte("001e# service=git-upload-pack\n0000"))
	_, _ = w.Write(out)
}

func (gs *gitServer) handleUploadPack(w http.ResponseWriter, r *http.Request, repo string) {
	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	_ = r.Body.Close()

	out, err := gs.uploadPack(repo, []string{"--stateless-rpc"}, body, r.Header.Get("Git-Protocol"))
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "application/x-git-upload-pack-result")
	w.Header().Set("Cache-Control", "no-cache")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(out)
}

func (gs *gitServer) uploadPack(repo string, args []string, body []byte, gitProtocol string) ([]byte, error) {
	cmdArgs := append([]string{"upload-pack"}, args...)
	cmdArgs = append(cmdArgs, repo)
	cmd := exec.Command(gs.git, cmdArgs...)
	if body != nil {
		cmd.Stdin = strings.NewReader(string(body))
	}
	if gitProtocol != "" {
		cmd.Env = append(os.Environ(), "GIT_PROTOCOL="+gitProtocol)
	}
	out, err := cmd.CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("git upload-pack failed: %w\n%s", err, out)
	}
	return out, nil
}

func containsDotDot(path string) bool {
	for _, p := range strings.Split(path, "/") {
		if p == ".." {
			return true
		}
	}
	return false
}
