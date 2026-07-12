package gitp

import "fmt"

func applyDelta(base, delta []byte) ([]byte, error) {
	pos := 0
	baseSize, err := readDeltaVarint(delta, &pos)
	if err != nil {
		return nil, err
	}
	targetSize, err := readDeltaVarint(delta, &pos)
	if err != nil {
		return nil, err
	}
	if baseSize != len(base) {
		return nil, fmt.Errorf("delta base size mismatch")
	}
	result := make([]byte, 0, targetSize)
	for len(result) < targetSize {
		if pos >= len(delta) {
			return nil, fmt.Errorf("truncated delta")
		}
		cmd := delta[pos]
		pos++
		if cmd&0x80 != 0 {
			offset := 0
			size := 0
			if cmd&0x01 != 0 {
				if pos >= len(delta) {
					return nil, fmt.Errorf("truncated delta")
				}
				offset |= int(delta[pos])
				pos++
			}
			if cmd&0x02 != 0 {
				if pos >= len(delta) {
					return nil, fmt.Errorf("truncated delta")
				}
				offset |= int(delta[pos]) << 8
				pos++
			}
			if cmd&0x04 != 0 {
				if pos >= len(delta) {
					return nil, fmt.Errorf("truncated delta")
				}
				offset |= int(delta[pos]) << 16
				pos++
			}
			if cmd&0x08 != 0 {
				if pos >= len(delta) {
					return nil, fmt.Errorf("truncated delta")
				}
				offset |= int(delta[pos]) << 24
				pos++
			}
			if cmd&0x10 != 0 {
				if pos >= len(delta) {
					return nil, fmt.Errorf("truncated delta")
				}
				size |= int(delta[pos])
				pos++
			}
			if cmd&0x20 != 0 {
				if pos >= len(delta) {
					return nil, fmt.Errorf("truncated delta")
				}
				size |= int(delta[pos]) << 8
				pos++
			}
			if cmd&0x40 != 0 {
				if pos >= len(delta) {
					return nil, fmt.Errorf("truncated delta")
				}
				size |= int(delta[pos]) << 16
				pos++
			}
			if size == 0 {
				size = 0x10000
			}
			if offset < 0 || offset > len(base) || size > len(base)-offset || size > targetSize-len(result) {
				return nil, fmt.Errorf("delta copy out of range")
			}
			result = append(result, base[offset:offset+size]...)
			continue
		}
		if cmd == 0 {
			return nil, fmt.Errorf("invalid delta command")
		}
		size := int(cmd)
		if pos+size > len(delta) {
			return nil, fmt.Errorf("truncated delta")
		}
		if size > targetSize-len(result) {
			return nil, fmt.Errorf("delta insert out of range")
		}
		result = append(result, delta[pos:pos+size]...)
		pos += size
	}
	if len(result) != targetSize || pos != len(delta) {
		return nil, fmt.Errorf("trailing garbage in delta")
	}
	return result, nil
}

func readDeltaVarint(data []byte, pos *int) (int, error) {
	accum := 0
	shift := 0
	for {
		if *pos >= len(data) || shift >= 64 {
			return 0, fmt.Errorf("malformed delta varint")
		}
		c := data[*pos]
		*pos = *pos + 1
		accum |= int(c&0x7f) << shift
		if c&0x80 == 0 {
			return accum, nil
		}
		shift += 7
	}
}
