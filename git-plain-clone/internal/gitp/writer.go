package gitp

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

type treeEntry struct {
	Name string
	Mode uint32
	OID  OID
}

func materialize(store *objectStore, treeOID OID, outDir string, progress Progress) (int, error) {
	total, err := countMaterializedEntries(store, treeOID)
	if err != nil {
		return 0, err
	}
	if progress != nil {
		progress.Stage("writing files")
		progress.Writing(0, total)
	}
	if err := ensureDir(outDir); err != nil {
		return 0, err
	}
	written := 0
	if err := materializeTree(store, treeOID, outDir, progress, total, &written); err != nil {
		return 0, err
	}
	return total, nil
}

func materializeTree(store *objectStore, treeOID OID, dir string, progress Progress, total int, written *int) error {
	tree := store.Get(treeOID)
	if tree == nil || tree.Type != ObjTree {
		return fmt.Errorf("tree object not found")
	}
	pos := 0
	for {
		entry, nextPos, err := nextTreeEntry(tree.Data, pos, store.algo)
		if err != nil {
			return err
		}
		if entry == nil {
			return nil
		}
		pos = nextPos
		if !validEntryName(entry.Name) {
			return fmt.Errorf("invalid tree entry name")
		}
		path := filepath.Join(dir, entry.Name)
		switch entry.Mode {
		case 0o040000:
			if err := ensureDir(path); err != nil {
				return err
			}
			if err := materializeTree(store, entry.OID, path, progress, total, written); err != nil {
				return err
			}
		case 0o100644, 0o100755:
			blob := store.Get(entry.OID)
			if err := writeBlob(blob, path, entry.Mode == 0o100755); err != nil {
				return err
			}
			*written = *written + 1
			if progress != nil {
				progress.Writing(*written, total)
			}
		case 0o120000:
			blob := store.Get(entry.OID)
			if err := writeSymlink(blob, path); err != nil {
				return err
			}
			*written = *written + 1
			if progress != nil {
				progress.Writing(*written, total)
			}
		default:
			continue
		}
	}
}

func countMaterializedEntries(store *objectStore, treeOID OID) (int, error) {
	tree := store.Get(treeOID)
	if tree == nil || tree.Type != ObjTree {
		return 0, fmt.Errorf("tree object not found")
	}
	count := 0
	pos := 0
	for {
		entry, nextPos, err := nextTreeEntry(tree.Data, pos, store.algo)
		if err != nil {
			return 0, err
		}
		if entry == nil {
			return count, nil
		}
		pos = nextPos
		switch entry.Mode {
		case 0o040000:
			nested, err := countMaterializedEntries(store, entry.OID)
			if err != nil {
				return 0, err
			}
			count += nested
		case 0o100644, 0o100755, 0o120000:
			count++
		}
	}
}

func nextTreeEntry(data []byte, pos int, algo HashAlgorithm) (*treeEntry, int, error) {
	if pos >= len(data) {
		return nil, pos, nil
	}
	space := bytes.IndexByte(data[pos:], ' ')
	if space < 0 {
		return nil, 0, fmt.Errorf("malformed tree entry: missing mode terminator")
	}
	space += pos
	modeBytes := data[pos:space]
	if len(modeBytes) == 0 || len(modeBytes) >= 7 {
		return nil, 0, fmt.Errorf("malformed tree entry: invalid mode length")
	}
	var mode uint32
	for _, c := range modeBytes {
		if c < '0' || c > '7' {
			return nil, 0, fmt.Errorf("malformed tree entry: invalid mode digit")
		}
		mode = (mode << 3) | uint32(c-'0')
	}
	nul := bytes.IndexByte(data[space+1:], 0)
	if nul < 0 {
		return nil, 0, fmt.Errorf("malformed tree entry: missing name terminator")
	}
	nul += space + 1
	name := string(data[space+1 : nul])
	if name == "" {
		return nil, 0, fmt.Errorf("malformed tree entry: empty name")
	}
	oidStart := nul + 1
	oidEnd := oidStart + algo.RawSize
	if oidEnd > len(data) {
		return nil, 0, fmt.Errorf("malformed tree entry: truncated object ID")
	}
	oid, err := algo.FromBytes(data[oidStart:oidEnd])
	if err != nil {
		return nil, 0, err
	}
	return &treeEntry{Name: name, Mode: mode, OID: oid}, oidEnd, nil
}

func validEntryName(name string) bool {
	if name == "" || name == "." || name == ".." {
		return false
	}
	return !strings.ContainsRune(name, '/')
}

func ensureDir(path string) error {
	if err := os.Mkdir(path, 0o755); err == nil {
		return nil
	} else if !os.IsExist(err) {
		return fmt.Errorf("cannot create directory %s", path)
	}
	info, err := os.Lstat(path)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return fmt.Errorf("%s exists and is not a directory", path)
	}
	return nil
}

func writeBlob(blob *Object, path string, executable bool) error {
	if blob == nil || blob.Type != ObjBlob {
		return fmt.Errorf("write_blob: invalid blob")
	}
	mode := os.FileMode(0o644)
	if executable {
		mode = 0o755
	}
	fd, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_TRUNC|openNoFollow(), mode)
	if err != nil {
		return fmt.Errorf("cannot create file %s", path)
	}
	if _, err := io.Copy(fd, bytes.NewReader(blob.Data)); err != nil {
		fd.Close()
		return fmt.Errorf("write failed for %s", path)
	}
	if err := chmodFile(fd, mode); err != nil {
		fd.Close()
		return fmt.Errorf("fchmod failed for %s", path)
	}
	if err := fd.Close(); err != nil {
		return fmt.Errorf("close failed for %s", path)
	}
	return nil
}

func writeSymlink(blob *Object, path string) error {
	if blob == nil || blob.Type != ObjBlob {
		return fmt.Errorf("write_symlink: invalid blob")
	}
	if len(blob.Data) == 0 || bytes.IndexByte(blob.Data, 0) >= 0 {
		return fmt.Errorf("write_symlink: invalid symlink target")
	}
	if err := createSymlink(string(blob.Data), path); err != nil {
		return fmt.Errorf("cannot create symlink %s", path)
	}
	return nil
}

func IsHTTPURL(url string) bool {
	return strings.HasPrefix(url, "http://") || strings.HasPrefix(url, "https://")
}

func DefaultDir(url string) string {
	trimmed := strings.TrimRight(url, "/")
	trimmed = strings.TrimSuffix(trimmed, ".git")
	trimmed = strings.TrimRight(trimmed, "/")
	idx := strings.LastIndexByte(trimmed, '/')
	if idx >= 0 {
		return trimmed[idx+1:]
	}
	return trimmed
}

func OutputDirAvailable(path string) bool {
	info, err := os.Stat(path)
	if err != nil {
		return true
	}
	if !info.IsDir() {
		return false
	}
	entries, err := os.ReadDir(path)
	if err != nil {
		return false
	}
	return len(entries) == 0
}
