package gitp

import (
	"bytes"
	"context"
	"fmt"
)

type Options struct {
	URL      string
	Ref      string
	OutDir   string
	Progress Progress
}

func Clone(ctx context.Context, opts Options) error {
	defer closeProgress(opts.Progress)
	reportStage(opts.Progress, "resolving refs")
	transport := newTransport(opts.URL)
	refsBuf, err := transport.GetRefs(ctx)
	if err != nil {
		return err
	}
	selected, err := selectAdvertisedRef(refsBuf, opts.Ref)
	if err != nil {
		return err
	}
	req, err := buildUploadPackRequest(selected.OID)
	if err != nil {
		return err
	}
	reportStage(opts.Progress, "downloading pack")
	response, err := transport.UploadPack(ctx, req, opts.Progress)
	if err != nil {
		return err
	}
	pack, err := extractPack(response)
	if err != nil {
		return err
	}
	store := newObjectStore(selected.OID.Algo)
	if err := parsePack(pack, store, opts.Progress); err != nil {
		return err
	}
	if err := resolveDeltas(store, opts.Progress); err != nil {
		return err
	}
	wanted := store.Get(selected.OID)
	if wanted == nil {
		return fmt.Errorf("selected ref object not found")
	}
	commit, err := peelToCommit(store, wanted)
	if err != nil {
		return err
	}
	treeOID, err := parseHeaderOID(commit, "tree ")
	if err != nil {
		return err
	}
	if treeOID.IsZero() {
		return fmt.Errorf("commit has no tree")
	}
	files, err := materialize(store, treeOID, opts.OutDir, opts.Progress)
	if err != nil {
		return err
	}
	finishProgress(opts.Progress, files)
	return nil
}

func peelToCommit(store *objectStore, obj *Object) (*Object, error) {
	for depth := 0; obj != nil && depth < 8; depth++ {
		if obj.Type == ObjCommit {
			return obj, nil
		}
		if obj.Type != ObjTag {
			return nil, fmt.Errorf("selected ref does not point to a commit")
		}
		targetOID, err := parseHeaderOID(obj, "object ")
		if err != nil {
			return nil, err
		}
		obj = store.Get(targetOID)
	}
	return nil, fmt.Errorf("selected ref does not point to a commit")
}

func parseHeaderOID(obj *Object, key string) (OID, error) {
	oidLen := obj.OID.Algo.HexSize
	keyBytes := []byte(key)
	data := obj.Data
	for len(data) > 0 {
		nl := bytes.IndexByte(data, '\n')
		if nl < 0 {
			return OID{}, fmt.Errorf("malformed object header")
		}
		line := data[:nl]
		if len(line) == 0 {
			break
		}
		if len(line) == len(keyBytes)+oidLen && bytes.HasPrefix(line, keyBytes) {
			return obj.OID.Algo.ParseHex(string(line[len(keyBytes):]))
		}
		data = data[nl+1:]
	}
	return OID{}, fmt.Errorf("missing object header: %s", key)
}

func reportStage(progress Progress, name string) {
	if progress != nil {
		progress.Stage(name)
	}
}

func finishProgress(progress Progress, files int) {
	if progress != nil {
		progress.Finish(files)
	}
}

func closeProgress(progress Progress) {
	if progress != nil {
		progress.Close()
	}
}
