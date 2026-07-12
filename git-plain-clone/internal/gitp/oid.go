package gitp

import (
	"crypto/sha1"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"hash"
	"strings"
)

type HashAlgorithm struct {
	Name    string
	RawSize int
	HexSize int
	newHash func() hash.Hash
}

var (
	SHA1 = HashAlgorithm{
		Name:    "sha1",
		RawSize: sha1.Size,
		HexSize: sha1.Size * 2,
		newHash: sha1.New,
	}
	SHA256 = HashAlgorithm{
		Name:    "sha256",
		RawSize: sha256.Size,
		HexSize: sha256.Size * 2,
		newHash: sha256.New,
	}
)

type OID struct {
	Algo HashAlgorithm
	Hex  string
}

func ParseHashAlgorithm(name string) (HashAlgorithm, error) {
	switch name {
	case SHA1.Name:
		return SHA1, nil
	case SHA256.Name:
		return SHA256, nil
	default:
		return HashAlgorithm{}, fmt.Errorf("unsupported object format: %s", name)
	}
}

func hashAlgorithmFromHexSize(hexSize int) (HashAlgorithm, error) {
	switch hexSize {
	case SHA1.HexSize:
		return SHA1, nil
	case SHA256.HexSize:
		return SHA256, nil
	default:
		return HashAlgorithm{}, fmt.Errorf("unsupported object ID length: %d", hexSize)
	}
}

func (a HashAlgorithm) ParseHex(s string) (OID, error) {
	if len(s) != a.HexSize {
		return OID{}, fmt.Errorf("invalid %s hex length", a.Name)
	}
	if _, err := hex.DecodeString(s); err != nil {
		return OID{}, fmt.Errorf("invalid %s hex character", a.Name)
	}
	return OID{Algo: a, Hex: strings.ToLower(s)}, nil
}

func (a HashAlgorithm) FromBytes(raw []byte) (OID, error) {
	if len(raw) != a.RawSize {
		return OID{}, fmt.Errorf("invalid %s raw length", a.Name)
	}
	return OID{Algo: a, Hex: hex.EncodeToString(raw)}, nil
}

func (a HashAlgorithm) ZeroOID() OID {
	return OID{Algo: a, Hex: strings.Repeat("0", a.HexSize)}
}

func (a HashAlgorithm) HashBytes(data []byte) []byte {
	h := a.newHash()
	_, _ = h.Write(data)
	return h.Sum(nil)
}

func (a HashAlgorithm) HashObject(typeName string, raw []byte) (OID, error) {
	h := a.newHash()
	if _, err := fmt.Fprintf(h, "%s %d\x00", typeName, len(raw)); err != nil {
		return OID{}, fmt.Errorf("failed to hash object header: %w", err)
	}
	if _, err := h.Write(raw); err != nil {
		return OID{}, fmt.Errorf("failed to hash object data: %w", err)
	}
	return a.FromBytes(h.Sum(nil))
}

func (o OID) Bytes() ([]byte, error) {
	return hex.DecodeString(o.Hex)
}

func (o OID) IsZero() bool {
	return o.Hex == o.Algo.ZeroOID().Hex
}

func (o OID) String() string {
	return o.Hex
}
