package gitp

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"io"
)

func parsePack(pack []byte, store *objectStore, progress Progress) error {
	algo := store.algo
	if len(pack) < 12+algo.RawSize {
		return fmt.Errorf("packfile too short")
	}
	if !bytes.Equal(pack[:4], []byte("PACK")) {
		return fmt.Errorf("not a packfile")
	}
	version := binary.BigEndian.Uint32(pack[4:8])
	if version != 2 && version != 3 {
		return fmt.Errorf("unsupported packfile version %d", version)
	}
	objectCount := binary.BigEndian.Uint32(pack[8:12])
	if progress != nil {
		progress.Stage("parsing pack")
		progress.PackObjects(0, int(objectCount))
	}
	pos := 12
	for i := uint32(0); i < objectCount; i++ {
		objStart := int64(pos)
		objType, size, err := decodePackObjectHeader(pack, &pos)
		if err != nil {
			return err
		}
		var baseOID OID
		var ofsDelta int64
		if objType == ObjRefDelta {
			if pos+algo.RawSize > len(pack) {
				return fmt.Errorf("truncated ref delta")
			}
			baseOID, err = algo.FromBytes(pack[pos : pos+algo.RawSize])
			if err != nil {
				return err
			}
			pos += algo.RawSize
		} else if objType == ObjOFSDelta {
			ofsDelta, err = decodeOFSDelta(pack, &pos)
			if err != nil {
				return err
			}
		}
		raw, consumed, err := inflateStream(pack[pos:])
		if err != nil {
			return err
		}
		if consumed == 0 || pos+consumed > len(pack) || len(raw) != size {
			return fmt.Errorf("pack object size mismatch")
		}
		pos += consumed
		if objType >= ObjCommit && objType <= ObjTag {
			objTypeName, err := typeName(objType)
			if err != nil {
				return err
			}
			oid, err := algo.HashObject(objTypeName, raw)
			if err != nil {
				return err
			}
			if err := store.Add(objType, oid, raw); err != nil {
				return err
			}
			if err := store.SetOffset(oid, objStart); err != nil {
				return err
			}
			if progress != nil {
				progress.PackObjects(int(i+1), int(objectCount))
			}
			continue
		}
		if objType != ObjRefDelta && objType != ObjOFSDelta {
			return fmt.Errorf("unknown pack object type %d", objType)
		}
		store.AddPendingDelta(&PendingDelta{
			Kind:      objType,
			Offset:    ofsDelta,
			BaseOID:   baseOID,
			DeltaData: raw,
			SrcOffset: objStart,
		})
		if progress != nil {
			progress.PackObjects(int(i+1), int(objectCount))
		}
	}
	if pos+algo.RawSize != len(pack) {
		return fmt.Errorf("packfile trailing data")
	}
	checksum := algo.HashBytes(pack[:pos])
	if !bytes.Equal(checksum, pack[pos:]) {
		return fmt.Errorf("packfile checksum mismatch")
	}
	return nil
}

func resolveDeltas(store *objectStore, reporter Progress) error {
	total := len(store.deltas)
	resolvedCount := 0
	if reporter != nil && total > 0 {
		reporter.Stage("resolving deltas")
		reporter.ResolveDeltas(0, total)
	}
	for {
		madeProgress := false
		for _, delta := range store.deltas {
			if delta == nil || delta.DeltaData == nil {
				continue
			}
			var base *Object
			if delta.Kind == ObjRefDelta {
				base = store.Get(delta.BaseOID)
			} else {
				if delta.SrcOffset < delta.Offset {
					return fmt.Errorf("delta offset out of range")
				}
				base = store.GetByOffset(delta.SrcOffset - delta.Offset)
			}
			if base == nil {
				continue
			}
			resolved, err := applyDelta(base.Data, delta.DeltaData)
			if err != nil {
				return err
			}
			objTypeName, err := typeName(base.Type)
			if err != nil {
				return err
			}
			oid, err := store.algo.HashObject(objTypeName, resolved)
			if err != nil {
				return err
			}
			if err := store.Add(base.Type, oid, resolved); err != nil {
				return err
			}
			if err := store.SetOffset(oid, delta.SrcOffset); err != nil {
				return err
			}
			delta.DeltaData = nil
			resolvedCount++
			if reporter != nil {
				reporter.ResolveDeltas(resolvedCount, total)
			}
			madeProgress = true
		}
		if !madeProgress {
			break
		}
	}
	for _, delta := range store.deltas {
		if delta != nil && delta.DeltaData != nil {
			return fmt.Errorf("unresolvable delta")
		}
	}
	return nil
}

func decodePackObjectHeader(pack []byte, pos *int) (objType int, size int, err error) {
	if *pos >= len(pack) {
		return 0, 0, fmt.Errorf("truncated packfile header")
	}
	c := pack[*pos]
	*pos = *pos + 1
	objType = int((c >> 4) & 0x07)
	accum := uint64(c & 0x0f)
	shift := uint(4)
	for c&0x80 != 0 {
		if *pos >= len(pack) {
			return 0, 0, fmt.Errorf("malformed packfile header")
		}
		c = pack[*pos]
		*pos = *pos + 1
		accum |= uint64(c&0x7f) << shift
		shift += 7
	}
	if accum > uint64(^uint(0)>>1) {
		return 0, 0, fmt.Errorf("pack object size too large")
	}
	return objType, int(accum), nil
}

func decodeOFSDelta(pack []byte, pos *int) (int64, error) {
	var offset int64
	for {
		if *pos >= len(pack) {
			return 0, fmt.Errorf("truncated offset delta")
		}
		c := pack[*pos]
		*pos = *pos + 1
		offset = (offset << 7) | int64(c&0x7f)
		if c&0x80 == 0 {
			return offset, nil
		}
		offset++
	}
}

type sliceByteReader struct {
	data []byte
	off  int
}

func (r *sliceByteReader) Read(p []byte) (int, error) {
	if r.off >= len(r.data) {
		return 0, io.EOF
	}
	n := copy(p, r.data[r.off:])
	r.off += n
	return n, nil
}

func (r *sliceByteReader) ReadByte() (byte, error) {
	if r.off >= len(r.data) {
		return 0, io.EOF
	}
	b := r.data[r.off]
	r.off++
	return b, nil
}

func inflateStream(src []byte) ([]byte, int, error) {
	r := &sliceByteReader{data: src}
	zr, err := zlib.NewReader(r)
	if err != nil {
		return nil, 0, fmt.Errorf("zlib inflateInit failed")
	}
	data, readErr := io.ReadAll(zr)
	closeErr := zr.Close()
	if readErr != nil || closeErr != nil {
		return nil, 0, fmt.Errorf("zlib inflate failed")
	}
	return data, r.off, nil
}
