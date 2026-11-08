package orbnet

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

// §60: the shipped default's preview is the whole glass, and the hub reads it whole.
func TestShelfPreviewIsTheWholeGlass(t *testing.T) {
	body, err := os.ReadFile(filepath.Join("..", "..", "themes", "skeleton.orbtheme"))
	if err != nil {
		t.Fatal(err)
	}
	plane, err := ThemePreviewPlane(body)
	if err != nil || plane == nil {
		t.Fatalf("the shipped default's preview did not read: %v", err)
	}
	if w := binary.LittleEndian.Uint16(plane[0:]); w != 466 {
		t.Fatalf("the shipped preview is %d px, want the glass's 466", w)
	}
}

// The orb's enc=z answer inflates into the plain wire plane, and a damaged stream is
// refused rather than drawn.
func TestExpandZPreview(t *testing.T) {
	const side = 64
	disc := make([]byte, discPx(side)*2)
	for i := range disc {
		disc[i] = byte(i % 13)
	}
	var z bytes.Buffer
	zw := zlib.NewWriter(&z)
	zw.Write(disc)
	zw.Close()
	wire := make([]byte, 4, 4+z.Len())
	binary.LittleEndian.PutUint16(wire[0:], side)
	binary.LittleEndian.PutUint16(wire[2:], side|0x8000)
	wire = append(wire, z.Bytes()...)
	out, err := ExpandZPreview(wire)
	if err != nil {
		t.Fatal(err)
	}
	if binary.LittleEndian.Uint16(out[2:]) != side || len(out) != 4+side*side*2 {
		t.Fatalf("expanded to %d B, h %d", len(out), binary.LittleEndian.Uint16(out[2:]))
	}
	sq := make([]byte, side*side*2)
	discExpand(disc, sq, side)
	if !bytes.Equal(out[4:], sq) {
		t.Fatal("the inflated preview is not the disc laid out as its square")
	}
	bad := append([]byte(nil), wire...)
	bad[len(bad)-1] ^= 0xFF // the Adler-32
	if _, err := ExpandZPreview(bad); err == nil {
		t.Fatal("a stream with a bad checksum was accepted")
	}
	short := append([]byte(nil), wire[:len(wire)-20]...)
	if _, err := ExpandZPreview(short); err == nil {
		t.Fatal("a truncated stream was accepted")
	}
}
