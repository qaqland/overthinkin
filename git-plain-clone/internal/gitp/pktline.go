package gitp

import (
	"bytes"
	"fmt"
	"strconv"
)

const pktMaxLen = 65535

func pktRead(src []byte, pos *int) ([]byte, bool, error) {
	if *pos > len(src) || len(src)-*pos < 4 {
		return nil, false, fmt.Errorf("truncated pkt-line")
	}
	lineLen, err := strconv.ParseUint(string(src[*pos:*pos+4]), 16, 16)
	if err != nil {
		return nil, false, fmt.Errorf("malformed pkt-line header")
	}
	if lineLen <= 2 {
		*pos += 4
		return nil, true, nil
	}
	if lineLen < 4 || int(lineLen) > len(src)-*pos {
		return nil, false, fmt.Errorf("invalid pkt-line length")
	}
	payloadStart := *pos + 4
	payloadEnd := *pos + int(lineLen)
	*pos = payloadEnd
	return bytes.Clone(src[payloadStart:payloadEnd]), false, nil
}

func pktWrite(dst *bytes.Buffer, data []byte) error {
	if len(data) > pktMaxLen-4 {
		return fmt.Errorf("pkt-line payload too large")
	}
	total := len(data) + 4
	if total > pktMaxLen {
		return fmt.Errorf("pkt-line too large")
	}
	if _, err := fmt.Fprintf(dst, "%04x", total); err != nil {
		return err
	}
	_, err := dst.Write(data)
	return err
}

func pktFlush(dst *bytes.Buffer) {
	dst.WriteString("0000")
}
