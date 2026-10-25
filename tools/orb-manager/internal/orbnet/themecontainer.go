package orbnet

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
)

// Reading a .orbtheme container's identity and its two budgets, so the hub ingests the
// name the owner actually baked instead of asking for one it would throw away.
//
// FORMAT 3 is the only dialect any current orb reads. The firmware's FORMAT 2 reader
// island is gone (7c0fd4b), and every surviving door — card_check3, verify_bundle3, the
// store door and the upload door — refuses on the FORMAT word. Before this file moved,
// the hub refused FORMAT 3 just as hard, which made it reject every file its own devices
// would install and forward every file they would not.
//
// D33 keeps the fix small: the hub reads the 128-byte header, the index of 40-byte
// records that header's count implies, and the name — and no art. The pixels stay the
// device's door's business and the browser that wrote them; a Go re-implementor of
// either would be a fourth dialect of one format, and §4.1's mirror rule already pays
// for three.
//
// The layout is defined once, in tools/orbbundle.py, and mirrored in
// main/art/orb_bundle.c. This reads the same words, in the same order, and refuses with
// the same sentences — D26: the owner learns a cost where they already see refusals.
// The refusal ORDER is the firmware's too: shape, magic, format, ABI, the header's own
// checksum, the two budget sums, then the name.

// Header word positions, matching orb_bundle.c's W_* table and tools/orbtheme.py's.
// FORMAT 3 keeps FORMAT 2's header shape exactly and changes the two stamps plus the
// meaning of word 9, which is why a v2 reader refuses on the FORMAT word long before it
// could misread a panel pair as a size.
const (
	themeHdrWords = 32
	themeHdrBytes = themeHdrWords * 4 // 128
	wMagic        = 0
	wFormat       = 1
	wGeomABI      = 2
	wGeomOff      = 4
	wGeomSize     = 5
	wArtSize      = 7
	wNSections    = 9 // FORMAT 2 read this word as scr_w; FORMAT 3 counts sections here
	wSerial       = 11
	wArtOff       = 13
	wName         = 14
	wHdrCRC       = 28
	themeMagic    = 0x474F5254 // 'T','R','O','G' little-endian on the wire
	themeFormat3  = 3          // ORB_BUNDLE3_FORMAT
	themeABI3     = 3          // ORB_BUNDLE3_ABI
	themeABI4     = 4          // ORB_BUNDLE3_ABI2: C4's clock delta dialect. An animated
	// plate bake stamps the header 4; the device door and the
	// browser reader both take 3 and 4, so the hub must too.
	themeNameBytes = 56 // ORB_THEME_NAME_MAX: 55 usable + the NUL
	bundleSectMax  = 8  // ORB_BUNDLE_SECT_MAX: the index's width
	bundleIdxWords = 10 // ORB_BUNDLE_IDX_WORDS: a 40-byte record
	bundleIdxHead  = 8  // n_sections + reserved
	// bundleGeomCap is ORB_BUNDLE_GEOM_CAP (orb_theme.c's SLOT_GEOM_CAP) and
	// bundleArtCap is ORB_BUNDLE_ART_CAP (ORB_THEME_SLOT_CAP, defined once in
	// main/art/orb_bundle.h). Both are INSTALL sums, not file shapes: geometry lands at
	// slot +0x1000 and its erase covers it sector-aligned, so 61,440 B ends exactly on
	// the banks at +0x10000, and the banks have the 8 MB slot less that offset to live
	// in. `== cap` is ACCEPTED at both — the same boundary check_budget() and
	// orb_bundle3_hdr_parse() apply (D29, §22.3). Until C1 this line carried the retired
	// 4 MB A/B pair's figure while the device reported 8 MB (§7.4).
	bundleGeomCap = 0xF000
	bundleArtCap  = 0x7F0000 // 8,323,072 B
	// themeSector is _ot.SECTOR and the flash erase granularity: the art bank starts on a
	// sector boundary after the region, which is what makes a max-size bundle land on the
	// hub's forwarding cap to the byte.
	themeSectorBytes = 4096
)

// ThemeContainer is what the hub needs to know about a file before it pushes it.
type ThemeContainer struct {
	Name      string // the embedded identity, exactly as the device will read it
	ID        string // theme_id(name) — the card stem these bytes will be filed under
	Format    uint32
	ABI       uint32
	Bytes     int
	GeomSize  uint32
	ArtSize   uint32
	Serial    uint32
	NSections uint32
	Sections  []string // the slugs the index names, in index order; nil if unreadable
}

// ContainerName reads the identity and the two budgets out of a .orbtheme payload, and
// reports the slugs its index names. It refuses an under-length or wrongly-magic file,
// the retired FORMAT 2, an ABI this build does not speak, a damaged header, a bundle over
// either D29 cap, and a container with no name — in the device's own words, before any
// byte crosses the room.
//
// ParseContainer is the WIRE surface: it applies the reader's rules, which still walk an
// index of 1..8 sections (orb_bundle.c's parse_core keeps that width, and a two-section
// file stays a valid REJECTION vector worth parsing far enough to name). A caller that
// wants the door's verdict wants ContainerName.
func ParseContainer(body []byte) (*ThemeContainer, error) {
	if len(body) < themeHdrBytes {
		return nil, fmt.Errorf("orbnet: that file is %d B, shorter than the %d B header a .orbtheme starts with", len(body), themeHdrBytes)
	}
	w := func(i int) uint32 { return binary.LittleEndian.Uint32(body[i*4:]) }
	if w(wMagic) != themeMagic {
		return nil, fmt.Errorf("orbnet: not an .orbtheme file: magic is 0x%08X", w(wMagic))
	}
	f := w(wFormat)
	if f != themeFormat3 {
		return nil, fmt.Errorf("orbnet: that is a FORMAT %d theme container and the orb reads FORMAT 3 only — the FORMAT 2 reader is out of the firmware, so re-export it from the Theme Editor", f)
	}
	if w(wGeomABI) != themeABI3 && w(wGeomABI) != themeABI4 {
		return nil, fmt.Errorf("orbnet: bundle geometry v%d; this build reads v%d and v%d — re-bake it", w(wGeomABI), themeABI3, themeABI4)
	}
	// The header's own checksum, over the header's SHAPE: the same 128 bytes with word 28
	// zeroed. That is the torn-copy check, and it does not vary with the dialect inside the
	// header — orb_bundle3_hdr_parse's rule, restated as §22.2 promised.
	if sum := crc32.ChecksumIEEE(headerForCRC(body)); w(wHdrCRC) != sum {
		return nil, fmt.Errorf("orbnet: container header is damaged — re-copy the file onto the card (checksum is 0x%08X, the header's bytes say 0x%08X)", w(wHdrCRC), sum)
	}
	// The D29 budget gate, install end, in the firmware's sums and words. A header over
	// either is refused here, before the name and before a streamed byte.
	geomSize, artSize := w(wGeomSize), w(wArtSize)
	if geomSize > bundleGeomCap {
		return nil, fmt.Errorf("orbnet: geometry region is %d B; the dial's table area holds %d B — geometry would overrun the pixels", geomSize, bundleGeomCap)
	}
	if artSize > bundleArtCap {
		return nil, fmt.Errorf("orbnet: pixel payload is %d B; a theme slot holds %d B — theme is larger than a slot", artSize, bundleArtCap)
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
	c := &ThemeContainer{Name: name, Format: f, ABI: w(wGeomABI),
		Bytes: len(body), GeomSize: geomSize, ArtSize: artSize,
		Serial: w(wSerial), NSections: w(wNSections)}
	// Per-app theming made the index load part of the IDENTITY, not a courtesy: the
	// card stem hashes the OWNING app's slug with the name, and the slug lives here
	// and nowhere else in the header. A container this reader cannot walk the index
	// of has no identity to report, and the walk is refused rather than guessed —
	// an id derived from the wrong slug addresses a file no orb will ever open.
	// A container with more than one section still gets its fields and its first-slug
	// identity here (the index is slug-sorted); ContainerName applies the door's one-
	// section rule over the top, so a half-read multi-section file is nameable in an
	// error report without ever passing for a theme.
	slugs, err := indexSlugs(body, c.NSections, geomSize)
	if err != nil {
		return nil, err
	}
	c.Sections = slugs
	c.ID, err = ThemeIDOf(slugs[0], name)
	if err != nil {
		return nil, err
	}
	return c, nil
}

// ContainerName is the upload gate the room's handlers use. It parses everything
// ParseContainer does, then applies the one rule the device's door applies at the card
// slot (orb_theme.c's one_section_why): a FORMAT 3 file themes EXACTLY one app — the
// card stem carries that app's slug, and no stem covers two — so a second section is
// a file no owner can install or evict. The refusal is the door's sentence, and it lands
// before a byte crosses the room. The WIRE still reads 1..8; only the theme layer narrowed.
func ContainerName(body []byte) (*ThemeContainer, error) {
	c, err := ParseContainer(body)
	if err != nil {
		return nil, err
	}
	if c.NSections != 1 {
		return nil, fmt.Errorf("orbnet: a theme file carries exactly one section — this one carries %d", c.NSections)
	}
	return c, nil
}

// headerForCRC returns the 128 header bytes with the checksum word zeroed — the span both
// _hdr_crc() and orb_bundle3_hdr_parse() sum over.
func headerForCRC(body []byte) []byte {
	out := append([]byte(nil), body[:themeHdrBytes]...)
	for i := wHdrCRC * 4; i < wHdrCRC*4+4; i++ {
		out[i] = 0
	}
	return out
}

// indexSlugs walks the geometry region's index and returns each section's slug, in index
// order. It checks the four things a header can lie about — the count, the region's
// length, the stored offset against the sequential walk, and the slug's own shape — and
// returns the firmware's sentence when one of them does. Blob contents, per-section CRCs
// and the art itself are the device's door, not this reader's.
func indexSlugs(body []byte, n, geomSize uint32) ([]string, error) {
	if n < 1 || n > bundleSectMax {
		return nil, fmt.Errorf("orbnet: index claims %d sections; a bundle holds 1..%d", n, bundleSectMax)
	}
	geomOff := binary.LittleEndian.Uint32(body[wGeomOff*4:])
	if geomOff != themeHdrBytes {
		return nil, fmt.Errorf("orbnet: container parts are not where this format puts them (geometry at %d)", geomOff)
	}
	if int(geomOff)+int(geomSize) > len(body) {
		return nil, fmt.Errorf("orbnet: geometry region runs past the end of the file")
	}
	// card_check3's layout rule: the art starts sector-aligned, at or after the region it
	// follows. A file that breaks it cannot be written to the slot at all, so the hub hears
	// it here rather than after the payload crossed the room.
	artOff := binary.LittleEndian.Uint32(body[wArtOff*4:])
	if artOff%themeSectorBytes != 0 || artOff < geomOff+geomSize || int64(artOff)+int64(binary.LittleEndian.Uint32(body[wArtSize*4:])) > int64(len(body)) {
		return nil, fmt.Errorf("orbnet: container parts are not where this format puts them (art at %d, region ends at %d, file is %d B)", artOff, geomOff+geomSize, len(body))
	}
	region := body[geomOff : geomOff+geomSize]
	if len(region) < bundleIdxHead+int(n)*bundleIdxWords*4 {
		return nil, fmt.Errorf("orbnet: index claims %d sections in %d region bytes — torn file", n, len(region))
	}
	if head := binary.LittleEndian.Uint32(region); head != n {
		return nil, fmt.Errorf("orbnet: header names %d sections, the index holds %d", n, head)
	}
	out := make([]string, 0, n)
	walk := uint32(bundleIdxHead + int(n)*bundleIdxWords*4)
	for i := 0; i < int(n); i++ {
		r := region[bundleIdxHead+i*bundleIdxWords*4:]
		// The record's shape, byte for byte (orb_bundle.c's IDX_BYTES comment, and
		// orbbundle.py's IDX_PACK): four u16s at 0,2,4,6 — slug_off, slug_len, abi_min,
		// abi_max — then seven u32s at 8,12,16,20,24,28,32 — n_elem, geom_off, geom_size,
		// geom_crc, art_off, art_size, art_crc — then two u16s at 36,38 for the section's
		// own panel pair. n_elem sits where a reader written from the field names alone
		// would put geom_off, which is why this comment and the fixture both exist.
		slugOff, slugLen := binary.LittleEndian.Uint16(r), binary.LittleEndian.Uint16(r[2:])
		blobOff, blobSize := binary.LittleEndian.Uint32(r[12:]), binary.LittleEndian.Uint32(r[16:])

		if blobOff != walk {
			return nil, fmt.Errorf("orbnet: section %d stores geom_off %d where the walk says %d — the index and the bytes contradict", i, blobOff, walk)
		}
		if int(blobOff)+int(blobSize) > len(region) {
			return nil, fmt.Errorf("orbnet: section %d's blob range is outside the region", i)
		}
		if int(slugOff)+int(slugLen) > len(region) {
			return nil, fmt.Errorf("orbnet: section %d: the index's slug does not fit the region it lives in", i)
		}
		sb := region[slugOff : slugOff+slugLen]
		if err := checkSlug(sb); err != nil {
			return nil, err
		}
		out = append(out, string(sb))
		walk += blobSize
	}
	return out, nil
}

// checkSlug is orb_bundle_slug_rule, orbapp.check_slug and orbbundle.check_slug — one
// rule for the whole family, stated in §9: lowercase a-z 0-9 _, 1..23 bytes, and a
// FORMAT 3 slug names the app it themes. A slug no app could ever claim is a section no
// app will ever be asked to paint, so it is refused before the bytes are forwarded.
func checkSlug(b []byte) error {
	if len(b) < 1 || len(b) > 23 {
		return fmt.Errorf("orbnet: bundle slug %q is %d bytes, outside 1..23", string(b), len(b))
	}
	for _, c := range b {
		if !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
			return fmt.Errorf("orbnet: bundle slug %q is not lowercase a-z 0-9 _", string(b))
		}
	}
	return nil
}
