//go:build !windows

package gitp

import (
	"os"
	"syscall"
)

func openNoFollow() int {
	return syscall.O_NOFOLLOW
}

func chmodFile(fd *os.File, mode os.FileMode) error {
	return fd.Chmod(mode)
}

func createSymlink(target, path string) error {
	return os.Symlink(target, path)
}
