//go:build ignore

package main

import (
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

func usage() {
	fmt.Fprint(os.Stderr, "Usage: compatcheck [-b REF] [-k] [-q] URL [DIRECTORY]\n")
	fmt.Fprint(os.Stderr, "\ncompatcheck expects a gitp binary in the same directory as itself.\n")
	fmt.Fprint(os.Stderr, "Build both with:\n")
	fmt.Fprint(os.Stderr, "  go build -o build/gitp ./cmd/gitp\n")
	fmt.Fprint(os.Stderr, "  go build -o build/compatcheck compatcheck.go\n")
}

func gitpNearSelf() string {
	self, err := os.Executable()
	if err != nil {
		self = os.Args[0]
	}
	abs, err := filepath.Abs(self)
	if err != nil {
		return ""
	}
	dir := filepath.Dir(abs)
	name := "gitp"
	if runtime.GOOS == "windows" {
		name = "gitp.exe"
	}
	candidate := filepath.Join(dir, name)
	if info, err := os.Stat(candidate); err == nil && !info.IsDir() {
		return candidate
	}
	return ""
}

func main() {
	fs := flag.NewFlagSet("compatcheck", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	branch := fs.String("branch", "", "branch or tag to clone")
	quiet := fs.Bool("quiet", false, "suppress output on success")
	keep := fs.Bool("keep", false, "keep temporary directories for inspection")
	fs.StringVar(branch, "b", "", "branch or tag to clone")
	fs.BoolVar(quiet, "q", false, "suppress output on success")
	fs.Usage = usage

	if err := fs.Parse(os.Args[1:]); err != nil {
		os.Exit(1)
	}

	if fs.NArg() == 0 {
		usage()
		os.Exit(1)
	}
	if fs.NArg() > 2 {
		fmt.Fprintln(os.Stderr, "error: unexpected argument:", fs.Arg(2))
		os.Exit(1)
	}

	gitpBin := gitpNearSelf()
	if gitpBin == "" {
		fmt.Fprintln(os.Stderr, "error: cannot find gitp binary next to compatcheck")
		fmt.Fprintln(os.Stderr, "build both with:")
		fmt.Fprintln(os.Stderr, "  go build -o build/gitp ./cmd/gitp")
		fmt.Fprintln(os.Stderr, "  go build -o build/compatcheck compatcheck.go")
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

	url := fs.Arg(0)
	baseDir := ""
	if fs.NArg() == 2 {
		baseDir = fs.Arg(1)
		if err := os.MkdirAll(baseDir, 0o755); err != nil {
			fmt.Fprintf(os.Stderr, "error: cannot create output directory %q: %v\n", baseDir, err)
			os.Exit(1)
		}
	} else {
		d, err := os.MkdirTemp("", "gitp-compat-")
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: create temp dir: %v\n", err)
			os.Exit(1)
		}
		baseDir = d
		if !*keep {
			defer os.RemoveAll(baseDir)
		}
	}

	gitpDir := filepath.Join(baseDir, "gitp")
	gitDir := filepath.Join(baseDir, "git")

	if err := gitpClone(gitpBin, url, *branch, gitpDir); err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
	if err := gitClone(gitBin, url, *branch, gitDir); err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}

	diffs, err := compareGitpWorkTree(gitBin, gitDir, gitpDir)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: compare directories: %v\n", err)
		os.Exit(1)
	}

	if len(diffs) > 0 {
		fmt.Fprintln(os.Stderr, "gitp output differs from git:")
		fmt.Fprintln(os.Stderr, strings.Join(diffs, "\n"))
		if !*quiet {
			fmt.Fprintf(os.Stderr, "\ngitp dir:  %s\n", gitpDir)
			fmt.Fprintf(os.Stderr, "git dir:   %s\n", gitDir)
		}
		os.Exit(1)
	}

	if !*quiet {
		fmt.Println("ok: gitp output matches git")
		fmt.Fprintf(os.Stdout, "gitp dir:  %s\n", gitpDir)
		fmt.Fprintf(os.Stdout, "git dir:   %s\n", gitDir)
	}
}

func gitpClone(gitpBin, url, ref, out string) error {
	args := []string{"clone"}
	if ref != "" {
		args = append(args, "--branch", ref)
	}
	args = append(args, url, out)
	cmd := exec.Command(gitpBin, args...)
	outBytes, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("gitp clone failed: %w\n%s", err, outBytes)
	}
	return nil
}

func gitClone(gitBin, url, ref, out string) error {
	args := []string{"clone"}
	if runtime.GOOS == "windows" {
		args = append(args, "-c", "core.symlinks=false")
	}
	if ref != "" {
		args = append(args, "--branch", ref)
	}
	args = append(args, url, out)
	cmd := exec.Command(gitBin, args...)
	outBytes, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("git clone failed: %w\n%s", err, outBytes)
	}
	return nil
}

func compareGitpWorkTree(gitBin, gitDir, workTree string) ([]string, error) {
	env := append(os.Environ(),
		"GIT_DIR="+filepath.Join(gitDir, ".git"),
		"GIT_WORK_TREE="+workTree,
	)

	cmd := exec.Command(gitBin, "diff", "--no-color", "--no-ext-diff", "--ignore-submodules=all")
	cmd.Env = env
	diffOut, err := cmd.CombinedOutput()
	if err != nil {
		var exitErr *exec.ExitError
		if !errors.As(err, &exitErr) || exitErr.ExitCode() != 1 {
			return nil, fmt.Errorf("git diff failed: %w\n%s", err, diffOut)
		}
	}

	cmd = exec.Command(gitBin, "status", "--porcelain", "--untracked-files=all", "--ignore-submodules=all")
	cmd.Env = env
	statusOut, err := cmd.CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("git status failed: %w\n%s", err, statusOut)
	}

	if len(diffOut) == 0 && len(statusOut) == 0 {
		return nil, nil
	}

	var diffs []string
	if len(diffOut) > 0 {
		diffs = append(diffs, string(diffOut))
	}
	if len(statusOut) > 0 {
		diffs = append(diffs, "status:\n"+string(statusOut))
	}
	return diffs, nil
}
