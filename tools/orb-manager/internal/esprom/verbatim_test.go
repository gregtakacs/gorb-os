package esprom

import (
	"bytes"
	"strings"
	"testing"
)

// A merged whole-flash image and a lone bootloader are indistinguishable to the
// offset-0 rewrite: both are written at 0x0, both open with 0xE9, and a gorb-os
// build stamps both with an appended digest. The rewrite's only cue that the bytes
// are a whole device are the Verbatim flag, so these pin the flag to the exact
// damage it prevents rather than to a bool nobody can read.

// mergedShapedBlob is the hazard in one value: a real S3 image header that
// declares an appended SHA256, five KB of body, and a final 32 bytes that belong
// to the art bank and are no digest at all.
func mergedShapedBlob() []byte {
	b := espImageAt(s3ImageChipID)
	b[2] = 0x02
	b[3] = 0x4f
	b[8+15] = 1
	b = append(b, bytes.Repeat([]byte{0x11}, 5*1024)...)
	return append(b, bytes.Repeat([]byte{0xEE}, 32)...)
}

func mergedPlan() FlashPlan { return FlashPlan{FlashMode: 0, FlashSize: 0, FlashFreq: 0xFF} }

// The flag's whole job: a byte-for-byte write. Every one of the 5160 bytes must
// arrive as it was, including the two the rewrite would otherwise overwrite.
func TestVerbatimImageSurvivesTheOffsetZeroRewrite(t *testing.T) {
	raw := mergedShapedBlob()
	out, err := prepareImage(Image{Offset: 0, Name: "merged.bin", Data: raw, Verbatim: true}, mergedPlan())
	if err != nil {
		t.Fatalf("prepareImage: %v", err)
	}
	if !bytes.Equal(out, raw) {
		t.Errorf("a Verbatim image was rewritten: %d bytes differ", diffBytes(out, raw))
	}
	if !bytes.Equal(out[len(out)-32:], bytes.Repeat([]byte{0xEE}, 32)) {
		t.Error("the tail of a Verbatim blob was overwritten with a recomputed digest, which is the assets partition's last 32 bytes on the orb")
	}
}

// The negative half: without the flag the same bytes do get rewritten, so the
// test above proves the flag and not some accident of the fixture.
func TestSameBlobWithoutVerbatimGetsTheDigestRewrite(t *testing.T) {
	raw := mergedShapedBlob()
	out, err := prepareImage(Image{Offset: 0, Name: "bootloader.bin", Data: raw}, mergedPlan())
	if err != nil {
		t.Fatalf("prepareImage: %v", err)
	}
	tail := out[len(out)-32:]
	if bytes.Equal(tail, raw[len(raw)-32:]) {
		t.Fatal("the rewrite left the tail alone, so the Verbatim test proves nothing")
	}
	if !bytes.Equal(tail, sha256Of(out[:len(out)-32])) {
		t.Error("the rewrite produced something other than a digest of the whole blob, which is the bug Verbatim exists for")
	}
}

// Verbatim must not become a licence to write garbage at 0x0: the header check is
// a separate gate that still runs, and it is the same one an ordinary image passes
// — a blob that cannot even hold an esp_image_header_t is refused before the first
// write, and refused by name.
func TestVerbatimStillRefusesABlobTooShortForAHeader(t *testing.T) {
	_, err := prepareImage(Image{Offset: 0, Name: "merged.bin", Data: []byte("nope"), Verbatim: true}, mergedPlan())
	if err == nil {
		t.Fatal("a 4-byte blob was accepted at 0x0")
	}
	if !strings.Contains(err.Error(), "merged.bin") {
		t.Errorf("error = %v, want it to name the image", err)
	}
}

func diffBytes(a, b []byte) int {
	n := 0
	for i := range a {
		if i >= len(b) {
			break
		}
		if a[i] != b[i] {
			n++
		}
	}
	return n + absInt(len(a)-len(b))
}

func absInt(n int) int {
	if n < 0 {
		return -n
	}
	return n
}
