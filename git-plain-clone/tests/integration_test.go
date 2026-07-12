package integration_test

import (
	"bufio"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

type gitServer struct {
	url string
}

func (gs *gitServer) URL() string {
	return gs.url
}

func (gs *gitServer) RepoURL(name string) string {
	return fmt.Sprintf("%s/%s", gs.url, name)
}

func startGitServer(t *testing.T, root string) *gitServer {
	t.Helper()

	bin := gitserverBinary(t)
	cmd := exec.Command(bin, "--root", root, "--timeout", "30s")

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatalf("gitserver stdout pipe: %v", err)
	}

	if err := cmd.Start(); err != nil {
		t.Fatalf("start gitserver: %v", err)
	}

	t.Cleanup(func() {
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
	})

	scanner := bufio.NewScanner(stdout)
	if !scanner.Scan() {
		t.Fatalf("gitserver did not print URL")
	}
	url := scanner.Text()
	if url == "" {
		t.Fatalf("gitserver printed empty URL")
	}

	return &gitServer{url: url}
}

func requireBinary(t *testing.T, name, buildSource string) string {
	t.Helper()
	relPath := filepath.Join("..", "build", name)
	abs, err := filepath.Abs(relPath)
	if err != nil {
		t.Fatalf("binary path %q is invalid: %v", relPath, err)
	}
	info, err := os.Stat(abs)
	if err != nil {
		t.Fatalf("binary not found at %q: %v\nbuild it with: go build -o build/%s %s", abs, err, name, buildSource)
	}
	if info.IsDir() {
		t.Fatalf("binary path %q is a directory, not a file", abs)
	}
	return abs
}

func gitpBinary(t *testing.T) string {
	return requireBinary(t, "gitp", "./cmd/gitp")
}

func compatcheckBinary(t *testing.T) string {
	return requireBinary(t, "compatcheck", "compatcheck.go")
}

func gitserverBinary(t *testing.T) string {
	return requireBinary(t, "gitserver", "gitserver.go")
}

func TestClone_HEAD(t *testing.T) {
	t.Parallel()
	_, repoURL := setupSHA1Repo(t)
	out := assertCloneMatchesGit(t, repoURL, "")
	assertNoGitDir(t, out)
}

func TestClone_Main(t *testing.T) {
	t.Parallel()
	_, repoURL := setupSHA1Repo(t)
	out := assertCloneMatchesGit(t, repoURL, "main")
	assertNoGitDir(t, out)
}

func TestClone_Feature(t *testing.T) {
	t.Parallel()
	_, repoURL := setupSHA1Repo(t)
	out := assertCloneMatchesGit(t, repoURL, "feature")
	assertNoGitDir(t, out)
}

func TestClone_AnnotatedTag(t *testing.T) {
	t.Parallel()
	_, repoURL := setupSHA1Repo(t)
	out := assertCloneMatchesGit(t, repoURL, "v1.0")
	assertNoGitDir(t, out)
}

func TestClone_LightweightTag(t *testing.T) {
	t.Parallel()
	_, repoURL := setupSHA1Repo(t)
	out := assertCloneMatchesGit(t, repoURL, "v2.0")
	assertNoGitDir(t, out)
}

func TestClone_BranchWinsOverSameNameTag(t *testing.T) {
	t.Parallel()
	_, repoURL := setupSHA1Repo(t)
	out := assertCloneMatchesGit(t, repoURL, "release")
	assertNoGitDir(t, out)
}

func TestClone_GitlinkSkipped(t *testing.T) {
	t.Parallel()
	_, repoURL := setupSHA1Repo(t)
	gitp := gitpBinary(t)
	out := t.TempDir()
	clone(t, gitp, repoURL, out, "--branch", "with-submodule")
	assertNoGitDir(t, out)
	mustNotExist(t, filepath.Join(out, "deps", "submodule"))
}

func TestClone_MissingRef(t *testing.T) {
	t.Parallel()
	_, repoURL := setupSHA1Repo(t)
	gitp := gitpBinary(t)
	out := t.TempDir()
	res := runAllowError(t, "", gitp, "clone", "--branch", "does-not-exist", repoURL, out)
	mustTrue(t, res.ExitCode != 0, "expected non-zero exit code")
	mustContain(t, res.Output, "ref not found")
}

func TestClone_SHA256Main(t *testing.T) {
	t.Parallel()
	_, repoURL := setupSHA256Repo(t)
	out := assertCloneMatchesGit(t, repoURL, "main")
	assertNoGitDir(t, out)
}

func setupSHA1Repo(t *testing.T) (repo, repoURL string) {
	t.Helper()
	tmp := t.TempDir()
	repo = filepath.Join(tmp, "repo")
	repos := filepath.Join(tmp, "repos")
	bare := filepath.Join(repos, "main.git")
	mustNoError(t, os.MkdirAll(repo, 0o755))
	mustNoError(t, os.MkdirAll(repos, 0o755))

	createRepo(t, repo, readTestFile(t, "sha1.fast-import"))
	run(t, repo, "git", "tag", "v2.0", "feature")
	run(t, repo, "git", "tag", "release", "main")
	createBareRepo(t, repo, bare)

	srv := startGitServer(t, repos)
	return repo, srv.RepoURL("main.git")
}

func setupSHA256Repo(t *testing.T) (repo, repoURL string) {
	t.Helper()
	tmp := t.TempDir()
	repo = filepath.Join(tmp, "repo")
	repos := filepath.Join(tmp, "repos")
	bare := filepath.Join(repos, "sha256.git")
	mustNoError(t, os.MkdirAll(repo, 0o755))
	mustNoError(t, os.MkdirAll(repos, 0o755))

	run(t, repo, "git", "init", "-q", "--object-format=sha256")
	runInput(t, repo, string(readTestFile(t, "sha256.fast-import")), "git", "fast-import", "--quiet")
	run(t, repo, "git", "symbolic-ref", "HEAD", "refs/heads/main")
	createBareRepo(t, repo, bare)

	srv := startGitServer(t, repos)
	return repo, srv.RepoURL("sha256.git")
}

func assertCloneMatchesGit(t *testing.T, repoURL string, ref string, gitpExtraArgs ...string) string {
	t.Helper()
	compatcheck := compatcheckBinary(t)

	outBase := t.TempDir()

	args := []string{compatcheck}
	if ref != "" {
		args = append(args, "--branch", ref)
	}
	args = append(args, gitpExtraArgs...)
	args = append(args, repoURL, outBase)

	cmd := exec.Command(args[0], args[1:]...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("compatcheck failed: %v\n%s", err, out)
	}

	return filepath.Join(outBase, "gitp")
}

func assertNoGitDir(t *testing.T, out string) {
	t.Helper()
	mustNotExist(t, filepath.Join(out, ".git"))
}

func clone(t *testing.T, gitp, repoURL, out string, args ...string) {
	t.Helper()
	fullArgs := append([]string{"clone"}, args...)
	fullArgs = append(fullArgs, repoURL, out)
	run(t, "", gitp, fullArgs...)
}

func run(t *testing.T, dir, name string, args ...string) {
	t.Helper()
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("command failed: %s %v\ndir: %s\nerror: %v\nstdout+stderr:\n%s",
			name, args, dir, err, out)
	}
}

func runInput(t *testing.T, dir, input, name string, args ...string) {
	t.Helper()
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	cmd.Stdin = strings.NewReader(input)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("command failed: %s %v\ndir: %s\nerror: %v\nstdout+stderr:\n%s",
			name, args, dir, err, out)
	}
}

func runAllowError(t *testing.T, dir, name string, args ...string) *execResult {
	t.Helper()
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	exitCode := 0
	if cmd.ProcessState != nil {
		exitCode = cmd.ProcessState.ExitCode()
	}
	return &execResult{
		ExitCode: exitCode,
		Output:   string(out),
		Err:      err,
	}
}

type execResult struct {
	ExitCode int
	Output   string
	Err      error
}

func createRepo(t *testing.T, dir string, fastImport []byte) {
	t.Helper()
	run(t, dir, "git", "init", "-q")
	runInput(t, dir, string(fastImport), "git", "fast-import", "--quiet")
	run(t, dir, "git", "symbolic-ref", "HEAD", "refs/heads/main")
}

func createBareRepo(t *testing.T, src, dst string) {
	t.Helper()
	run(t, src, "git", "clone", "--bare", "-q", src, dst)
	run(t, dst, "git", "config", "http.receivepack", "false")
}

func mustNoError(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
}

func mustTrue(t *testing.T, cond bool, msg string) {
	t.Helper()
	if !cond {
		t.Fatalf("assertion failed: %s", msg)
	}
}

func mustNotExist(t *testing.T, path string) {
	t.Helper()
	_, err := os.Stat(path)
	if err == nil {
		t.Fatalf("expected not to exist: %s", path)
	}
	if !os.IsNotExist(err) {
		t.Fatalf("unexpected stat error for %s: %v", path, err)
	}
}

func mustContain(t *testing.T, s, substr string) {
	t.Helper()
	if !strings.Contains(s, substr) {
		t.Fatalf("expected %q to contain %q", s, substr)
	}
}

func readTestFile(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("testdata", name))
	if err != nil {
		t.Fatalf("cannot read testdata/%s: %v", name, err)
	}
	return b
}
