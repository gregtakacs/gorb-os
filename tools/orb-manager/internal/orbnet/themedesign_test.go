package orbnet

import (
	"bytes"
	"encoding/binary"
	"errors"
	"hash/crc32"
	"testing"
)

// withDesign appends design to an authority-written container and points words 29..31 at
// it, re-sealing the header: the shape tools/orbbundle.py and the baker write (D43 P7).
func withDesign(t *testing.T, body, design []byte) []byte {
	t.Helper()
	out := append(append([]byte{}, body...), design...)
	binary.LittleEndian.PutUint32(out[wDesignOff*4:], uint32(len(body)))
	binary.LittleEndian.PutUint32(out[wDesignSize*4:], uint32(len(design)))
	binary.LittleEndian.PutUint32(out[wDesignCRC*4:], crc32.ChecksumIEEE(design))
	binary.LittleEndian.PutUint32(out[wHdrCRC*4:], crc32.ChecksumIEEE(headerForCRC(out)))
	return out
}

func TestDesignRidesAfterTheBanks(t *testing.T) {
	body := fixture(t, "format3-pair.orbtheme")
	if _, err := ContainerDesign(body); !errors.Is(err, ErrNoDesign) {
		t.Fatalf("a file without a design answered %v, want ErrNoDesign", err)
	}
	design := []byte(`{"format":"wc-design","schema":1}`)
	f := withDesign(t, body, design)
	c, err := ParseContainer(f)
	if err != nil {
		t.Fatalf("a container with a design is refused as a theme: %v", err)
	}
	if c.DesignSize != uint32(len(design)) {
		t.Errorf("DesignSize is %d, want %d", c.DesignSize, len(design))
	}
	got, err := ContainerDesign(f)
	if err != nil || !bytes.Equal(got, design) {
		t.Fatalf("design read back %q, %v; want %q", got, err, design)
	}
	// a damaged design is a sentence about the design, never about the dial
	bad := append([]byte{}, f...)
	bad[len(bad)-1] ^= 0xFF
	if _, err := ParseContainer(bad); err != nil {
		t.Errorf("a damaged design refused the dial: %v", err)
	}
	if _, err := ContainerDesign(bad); err == nil || !bytes.Contains([]byte(err.Error()), []byte("damaged")) {
		t.Errorf("a damaged design answered %v, want a damage sentence", err)
	}
	// a pointer past the file's end (a torn copy) is named, not read
	torn := f[:len(f)-4]
	if _, err := ContainerDesign(torn); err == nil || !bytes.Contains([]byte(err.Error()), []byte("past the end")) {
		t.Errorf("a torn design answered %v", err)
	}
	// a pointer into the banks is refused: the design never overlaps the dial
	in := withDesign(t, body, design)
	binary.LittleEndian.PutUint32(in[wDesignOff*4:], uint32(len(body)-8))
	binary.LittleEndian.PutUint32(in[wHdrCRC*4:], crc32.ChecksumIEEE(headerForCRC(in)))
	if _, err := ContainerDesign(in); err == nil {
		t.Errorf("a design pointing into the banks was read")
	}
}
