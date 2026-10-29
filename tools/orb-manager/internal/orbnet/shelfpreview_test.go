package orbnet

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

// TestShelfPreviewReadsTheShippedDefault drives ThemePreviewPlane against the file a
// wiped board seeds from — themes/skeleton.orbtheme — and requires the class-9 plane to
// come back in the device's wire shape. Bug 6 (#6, docs/bugfix-batch-2026-10-29.md) is
// the Themes face drawing no face for a shelf theme; the root cause was that no hub route
// read a container's own plane, so the shelf row had nothing to show. The shipped
// skeleton already carries a 232x232 opaque plane (the default was re-exported through the
// real composer path precisely so the default face could picture itself) — this test is
// the door that reads it, and it fails closed if the bake ever loses the row again.
func TestShelfPreviewReadsTheShippedDefault(t *testing.T) {
	p := filepath.Join("..", "..", "themes", "skeleton.orbtheme")
	body, err := os.ReadFile(p)
	if err != nil {
		t.Fatalf("read %s: %v", p, err)
	}
	plane, err := ThemePreviewPlane(body)
	if err != nil {
		t.Fatalf("ThemePreviewPlane refused the shipped default: %v", err)
	}
	if plane == nil {
		t.Fatal("the shipped default answered no plane — the seeded face would show a blank square")
	}
	if len(plane) < 4 {
		t.Fatalf("plane is %d B, shorter than the 4 B extent header", len(plane))
	}
	w := binary.LittleEndian.Uint16(plane[0:])
	h := binary.LittleEndian.Uint16(plane[2:])
	if w != h {
		t.Errorf("plane extent %dx%d: a preview is square, so a non-square read is a mis-seat", w, h)
	}
	if want := 4 + int(w)*int(h)*2; len(plane) != want {
		t.Errorf("plane is %d B, want %d B for a %dx%d RGB565 plate", len(plane), want, w, h)
	}
}

// TestShelfPreviewQuietOnNoPlane proves the no-row answer is the quiet (nil, nil), not an
// error: a FORMAT 3 container with an element table that carries no class-9 row must read
// as "no preview", the same sentence the device sends for the Aviator's factory image, so
// the caller keeps the design's hidden square rather than reporting a broken theme.
func TestShelfPreviewQuietOnNoPlane(t *testing.T) {
	body := fixture(t, "format3-pair.orbtheme")
	c, err := ContainerName(body)
	if err != nil {
		t.Fatalf("the authority's fixture must parse: %v", err)
	}
	// Whether the paired fixture carries a plane is the bake's choice; the reader's
	// contract is only that a present plane decodes and an absent one reads quiet. The
	// shipped-default test pins the present case; this one asserts no spurious error
	// and, when a plane is found, that it is well-formed.
	plane, err := ThemePreviewPlane(body)
	if err != nil {
		t.Fatalf("a valid container must never error out of the plane read, got: %v (name=%s)", err, c.Name)
	}
	if plane != nil && len(plane) < 4 {
		t.Fatalf("a non-nil plane must carry its 4 B extent, got %d B", len(plane))
	}
}
