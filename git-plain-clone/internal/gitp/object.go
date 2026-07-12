package gitp

import "fmt"

const (
	ObjCommit   = 1
	ObjTree     = 2
	ObjBlob     = 3
	ObjTag      = 4
	ObjOFSDelta = 6
	ObjRefDelta = 7
)

type Object struct {
	Type       int
	OID        OID
	Data       []byte
	PackOffset int64
}

type PendingDelta struct {
	Kind      int
	Offset    int64
	BaseOID   OID
	DeltaData []byte
	SrcOffset int64
}

type objectStore struct {
	algo     HashAlgorithm
	byOID    map[string]*Object
	byOffset map[int64]*Object
	deltas   []*PendingDelta
}

func newObjectStore(algo HashAlgorithm) *objectStore {
	return &objectStore{
		algo:     algo,
		byOID:    make(map[string]*Object),
		byOffset: make(map[int64]*Object),
	}
}

func (s *objectStore) Add(objType int, oid OID, data []byte) error {
	if objType == 0 {
		return fmt.Errorf("obj_store_add: invalid argument")
	}
	if _, ok := s.byOID[oid.Hex]; ok {
		return fmt.Errorf("duplicate object %s", oid.Algo.Name)
	}
	copyData := append([]byte(nil), data...)
	s.byOID[oid.Hex] = &Object{
		Type: objType,
		OID:  oid,
		Data: copyData,
	}
	return nil
}

func (s *objectStore) SetOffset(oid OID, offset int64) error {
	obj := s.Get(oid)
	if obj == nil {
		return fmt.Errorf("obj_store_set_offset: object not found")
	}
	obj.PackOffset = offset
	s.byOffset[offset] = obj
	return nil
}

func (s *objectStore) Get(oid OID) *Object {
	return s.byOID[oid.Hex]
}

func (s *objectStore) GetByOffset(offset int64) *Object {
	return s.byOffset[offset]
}

func (s *objectStore) AddPendingDelta(delta *PendingDelta) {
	s.deltas = append(s.deltas, delta)
}

func typeName(objType int) (string, error) {
	switch objType {
	case ObjCommit:
		return "commit", nil
	case ObjTree:
		return "tree", nil
	case ObjBlob:
		return "blob", nil
	case ObjTag:
		return "tag", nil
	default:
		return "", fmt.Errorf("unknown pack object type %d", objType)
	}
}
