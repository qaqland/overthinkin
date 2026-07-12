package main

import (
	"context"
	"flag"
	"fmt"
	"os"

	"git-plain-clone/internal/gitp"
)

func usage() {
	fmt.Fprint(os.Stderr, "Usage: gitp [clone] [-q] [-b REF] URL [DIRECTORY]\n")
}

func main() {
	args := os.Args[1:]
	if len(args) > 0 && args[0] == "clone" {
		args = args[1:]
	}

	fs := flag.NewFlagSet("gitp", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	branch := fs.String("branch", "HEAD", "")
	quiet := fs.Bool("quiet", false, "")
	fs.StringVar(branch, "b", "HEAD", "")
	fs.BoolVar(quiet, "q", false, "")
	fs.Usage = usage

	if err := fs.Parse(args); err != nil {
		os.Exit(1)
	}

	if fs.NArg() == 0 {
		die(fmt.Errorf("missing URL"))
	}
	if fs.NArg() > 2 {
		die(fmt.Errorf("unexpected argument: %s", fs.Arg(2)))
	}

	url := fs.Arg(0)
	if !gitp.IsHTTPURL(url) {
		die(fmt.Errorf("only http:// and https:// URLs are supported"))
	}

	outDir := ""
	if fs.NArg() == 2 {
		outDir = fs.Arg(1)
	} else {
		outDir = gitp.DefaultDir(url)
	}
	if outDir == "" {
		die(fmt.Errorf("invalid output directory"))
	}
	if !gitp.OutputDirAvailable(outDir) {
		die(fmt.Errorf("output directory exists and is not empty: %s", outDir))
	}

	opts := gitp.Options{
		URL:    url,
		Ref:    *branch,
		OutDir: outDir,
	}
	if !*quiet && isTTY(os.Stderr) {
		opts.Progress = gitp.NewTerminalProgress(os.Stderr)
	}
	if err := gitp.Clone(context.Background(), opts); err != nil {
		die(err)
	}
}

func isTTY(f *os.File) bool {
	info, err := f.Stat()
	if err != nil {
		return false
	}
	return info.Mode()&os.ModeCharDevice != 0
}

func die(err error) {
	fmt.Fprintf(os.Stderr, "error: %v\n", err)
	os.Exit(1)
}
