package orbnet

import (
	"encoding/binary"
	"fmt"
)

// Reading the identity OUT of a .orbtheme container, so the hub ingests the name the
// owner actually baked instead of asking for one it would throw away.
//
// FORMAT 2 makes the header's name field the theme's one and only identifier: main/
// art/orb_theme.c files the card under theme_id(name) and refuses a container whose
// name is empty. Under FORMAT 1 a theme carried a slug + title pair and the filename
// was the slug, so a name typed at upload time chose the destination. That is the
// fragmentation this module exists to keep out: an upload whose bytes still say
// FORMAT 1 cannot be filed at all, and the operator has to hear that here, from the
// hub holding the file, rather than from a 422 two hops away after the whole payload
// crossed the room.
//
// The layout is defined once, in tools/orbtheme.py, and mirrored in main/art/orb_theme.c
// — this reads the same four words and must move with them. Only the fields needed to
// speak about identity are parsed; the payload is the device's business.

// Header word positions, matching W_MAGIC/W_FORMAT/W_NAME/NAME_MAX in
// tools/orbtheme.py. The name is a byte array spread over whole words.
const (
	themeHdrWords  = 32
	themeHdrBytes  = themeHdrWords * 4 // 128
	wMagic         = 0
	wFormat        = 1
	themeMagic     = 0x474F5254 // 'T','R','O','G' little-endian on the wire
	themeFormat2   = 2
	themeNameBytes = 56 // ORB_THEME_NAME_MAX: 55 usable + the NUL
	wName          = 14
)

// ThemeContainer is what the hub needs to know about a file before it pushes it.
type ThemeContainer struct {
	Name   string // the embedded identity, exactly as the device will read it
	ID     string // theme_id(name) — the card stem these bytes will be filed under
	Format uint32
	Bytes  int
}

// ContainerName reads the identity out of a .orbtheme payload. It refuses an
// under-length or wrongly-magic file, and it refuses FORMAT 1 outright: the firmware
// has no legacy path (orb_theme.c:262 rejects any format but 2), so a FORMAT-1 pair
// would cross the radio and die at the door.
func ContainerName(body []byte) (*ThemeContainer, error) {
	if len(body) < themeHdrBytes {
		return nil, fmt.Errorf("orbnet: that file is %d B, shorter than the %d B header a .orbtheme starts with", len(body), themeHdrBytes)
	}
	w := binary.LittleEndian.Uint32(body[wMagic*4:])
	if w != themeMagic {
		return nil, fmt.Errorf("orbnet: not a .orbtheme container: magic is 0x%08X, want 0x%08X", w, themeMagic)
	}
	f := binary.LittleEndian.Uint32(body[wFormat*4:])
	if f != themeFormat2 {
		return nil, fmt.Errorf("orbnet: that is a FORMAT %d theme container and the orb reads FORMAT 2 only — its name lives in the filename, which the device no longer trusts; re-export it from the Theme Editor", f)
	}
	// NUL-terminated inside the NAME_MAX-byte field, the same cut _wordstr in
	// tools/orbtheme.py and the C reader both make.
	field := body[wName*4 : wName*4+themeNameBytes]
	end := len(field)
	for i, b := range field {
		if b == 0 {
			end = i
			break
		}
	}
	name := string(field[:end])
	if err := CheckThemeName(name); err != nil {
		return nil, err
	}
	id, err := ThemeIDOf(name)
	if err != nil {
		return nil, err
	}
	return &ThemeContainer{Name: name, ID: id, Format: f, Bytes: len(body)}, nil
}
