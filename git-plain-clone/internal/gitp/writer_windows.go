//go:build windows

package gitp

import "os"

// openNoFollow returns 0 on Windows because O_NOFOLLOW is not supported.
func openNoFollow() int {
	return 0
}

// chmodFile is a no-op on Windows because there are no Unix-style permission
// bits. The read-only attribute is not meaningful for our use case.
func chmodFile(fd *os.File, mode os.FileMode) error {
	return nil
}

// createSymlink writes the symlink target as a regular file on Windows.
// Creating real symbolic links on Windows requires elevated privileges or
// Developer Mode, so we degrade gracefully to a plain file containing the
// target path.
func createSymlink(target, path string) error {
	return os.WriteFile(path, []byte(target), 0o644)
}
