package orbnet

import "testing"

// The disc rule is the one main/art/orb_disc.h and theme_schema.disc_span apply: these
// counts are theme_schema.disc_px's, so a drift between the three readers fails here.
func TestDiscRuleMatchesTheSchema(t *testing.T) {
	for _, c := range []struct{ side, px uint32 }{{232, 42932}, {128, 13200}, {466, 171868}} {
		if got := discPx(c.side); got != c.px {
			t.Errorf("discPx(%d) = %d, theme_schema.disc_px says %d", c.side, got, c.px)
		}
	}
	if x0, x1 := discSpan(232, 0); x0 != 97 || x1 != 134 {
		t.Errorf("discSpan(232, 0) = %d..%d", x0, x1)
	}
}

// A disc laid back out keeps every kept pixel where it was and blacks the corners.
func TestDiscExpandRestoresTheSquare(t *testing.T) {
	const side = 128
	sq := make([]byte, side*side*2)
	for i := range sq {
		sq[i] = byte(i*7 + 1)
	}
	disc := make([]byte, 0, discPx(side)*2)
	for y := uint32(0); y < side; y++ {
		x0, x1 := discSpan(side, y)
		disc = append(disc, sq[(y*side+x0)*2:(y*side+x1+1)*2]...)
	}
	out := make([]byte, side*side*2)
	discExpand(disc, out, side)
	for y := uint32(0); y < side; y++ {
		x0, x1 := discSpan(side, y)
		for x := uint32(0); x < side; x++ {
			i := (y*side + x) * 2
			in := x >= x0 && x <= x1
			if in && (out[i] != sq[i] || out[i+1] != sq[i+1]) {
				t.Fatalf("(%d,%d) inside the disc moved", x, y)
			}
			if !in && (out[i] != 0 || out[i+1] != 0) {
				t.Fatalf("(%d,%d) outside the disc is not black", x, y)
			}
		}
	}
}
