package gitp

import (
	"bytes"
	"fmt"
	"strings"
)

const (
	refMatchNone = iota
	refMatchHead
	refMatchTag
	refMatchBranch
)

type selectedRef struct {
	OID          OID
	Capabilities []string
}

func selectAdvertisedRef(pktStream []byte, wanted string) (selectedRef, error) {
	pos := 0
	firstRef := true
	parsingRefs := false
	selectedMatch := refMatchNone
	var selected selectedRef
	var algo HashAlgorithm

	for pos < len(pktStream) {
		line, flush, err := pktRead(pktStream, &pos)
		if err != nil {
			return selectedRef{}, err
		}
		if flush {
			if !parsingRefs {
				continue
			}
			break
		}
		if len(line) == 0 {
			continue
		}
		if !parsingRefs && bytes.HasPrefix(line, []byte("# service=git-upload-pack")) {
			continue
		}
		if bytes.HasPrefix(line, []byte("version ")) {
			continue
		}

		parsingRefs = true
		space := bytes.IndexByte(line, ' ')
		if space <= 0 {
			return selectedRef{}, fmt.Errorf("malformed ref advertisement")
		}
		oidHex := string(line[:space])
		if firstRef {
			capsStart := line[space+1:]
			nul := bytes.IndexByte(capsStart, 0)
			if nul < 0 {
				return selectedRef{}, fmt.Errorf("malformed first ref advertisement")
			}
			capabilities := parseCapabilities(string(capsStart[nul+1:]))
			var err error
			algo, err = advertisedObjectFormat(capabilities, len(oidHex))
			if err != nil {
				return selectedRef{}, err
			}
			refName := capsStart[:nul]
			if len(refName) == 0 {
				return selectedRef{}, fmt.Errorf("empty ref name")
			}
			match := matchWantedRef(refName, wanted)
			if match != refMatchNone {
				oid, err := algo.ParseHex(oidHex)
				if err != nil {
					return selectedRef{}, err
				}
				selected = selectedRef{OID: oid, Capabilities: capabilities}
				selectedMatch = match
			}
			firstRef = false
			continue
		}

		if algo.Name == "" {
			var err error
			algo, err = hashAlgorithmFromHexSize(len(oidHex))
			if err != nil {
				return selectedRef{}, err
			}
		}
		refName := bytes.TrimSuffix(line[space+1:], []byte("\n"))
		if len(refName) == 0 {
			return selectedRef{}, fmt.Errorf("empty ref name")
		}
		match := matchWantedRef(refName, wanted)
		if match != refMatchNone && match > selectedMatch {
			oid, err := algo.ParseHex(oidHex)
			if err != nil {
				return selectedRef{}, err
			}
			selected.OID = oid
			selectedMatch = match
		}
	}

	if selectedMatch == refMatchNone {
		return selectedRef{}, fmt.Errorf("ref not found: %s", wanted)
	}
	return selected, nil
}

func parseCapabilities(raw string) []string {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return nil
	}
	return strings.Fields(raw)
}

func advertisedObjectFormat(capabilities []string, hexLen int) (HashAlgorithm, error) {
	var advertised *HashAlgorithm
	for _, cap := range capabilities {
		if !strings.HasPrefix(cap, "object-format=") {
			continue
		}
		algoName := strings.TrimPrefix(cap, "object-format=")
		algo, err := ParseHashAlgorithm(algoName)
		if err != nil {
			return HashAlgorithm{}, err
		}
		if advertised == nil {
			advertised = &algo
		}
	}
	fromLen, err := hashAlgorithmFromHexSize(hexLen)
	if err != nil {
		return HashAlgorithm{}, err
	}
	if advertised != nil && advertised.HexSize != fromLen.HexSize {
		return HashAlgorithm{}, fmt.Errorf("ref advertisement object-format mismatch")
	}
	if advertised != nil {
		return *advertised, nil
	}
	return fromLen, nil
}

func matchWantedRef(name []byte, wanted string) int {
	const (
		tagsPrefix  = "refs/tags/"
		headsPrefix = "refs/heads/"
	)
	if wanted == "HEAD" {
		if string(name) == "HEAD" {
			return refMatchHead
		}
		return refMatchNone
	}
	if string(name) == tagsPrefix+wanted {
		return refMatchTag
	}
	if string(name) == headsPrefix+wanted {
		return refMatchBranch
	}
	return refMatchNone
}

func buildUploadPackRequest(want OID) ([]byte, error) {
	var out bytes.Buffer
	capabilities := []string{
		"multi_ack_detailed",
		"side-band-64k",
		"thin-pack",
		"no-progress",
		"include-tag",
		"ofs-delta",
		"deepen-since",
		"deepen-not",
		"agent=git-plain-clone",
	}
	if want.Algo.Name != SHA1.Name {
		capabilities = append(capabilities, "object-format="+want.Algo.Name)
	}
	firstWant := fmt.Sprintf("want %s %s\n", want.Hex, strings.Join(capabilities, " "))
	if err := pktWrite(&out, []byte(firstWant)); err != nil {
		return nil, err
	}
	if err := pktWrite(&out, []byte("deepen 1\n")); err != nil {
		return nil, err
	}
	pktFlush(&out)
	if err := pktWrite(&out, []byte("done\n")); err != nil {
		return nil, err
	}
	return out.Bytes(), nil
}

func extractPack(response []byte) ([]byte, error) {
	var out bytes.Buffer
	pos := 0
	sawPack := false
	for pos < len(response) {
		if bytes.HasPrefix(response[pos:], []byte("PACK")) {
			out.Write(response[pos:])
			sawPack = true
			break
		}
		line, flush, err := pktRead(response, &pos)
		if err != nil {
			return nil, err
		}
		if flush || len(line) == 0 {
			continue
		}
		switch line[0] {
		case 1:
			if len(line) > 1 {
				out.Write(line[1:])
				if out.Len() >= 4 && bytes.Equal(out.Bytes()[:4], []byte("PACK")) {
					sawPack = true
				}
			}
		case 2:
			continue
		case 3:
			msg := strings.TrimSpace(string(line[1:]))
			if msg == "" {
				return nil, fmt.Errorf("remote upload-pack error")
			}
			return nil, fmt.Errorf("remote upload-pack error: %s", msg)
		default:
			if bytes.HasPrefix(line, []byte("ERR ")) {
				return nil, fmt.Errorf(strings.TrimSpace(string(line[4:])))
			}
			if bytes.HasPrefix(line, []byte("PACK")) {
				out.Write(line)
				sawPack = true
			}
		}
	}
	if !sawPack || out.Len() < 4 || !bytes.Equal(out.Bytes()[:4], []byte("PACK")) {
		return nil, fmt.Errorf("no packfile in upload-pack response")
	}
	return out.Bytes(), nil
}
