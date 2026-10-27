package orbnet

import (
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
)

// D37: reading an .orbapp container's identity, in the hub, off the bytes the author
// shipped.
//
// Until now the hub treated a container as an opaque blob. That was affordable while the
// only apps it ever shipped sat in a hub seed it pushed itself; S5 made them disk
// artifacts (starter.go), and it is
// not affordable for D37, because the Themes face must list every theme surface this hub
// can open an editor for BEFORE anything is installed on a board. That is a question
// about the hub's own files, so the hub has to read them: slug, title, caps, serial and
// editor presence, in its own code, on its own disk.
//
// Several readers now speak this layout and one writer. tools/orbapp.py is the authority
// and the only writer; main/app/orb_app_manifest.c is the door on the dial; this file is
// the hub's read of a file it will forward but never run. The theme family already holds
// under the same condition (see themecontainer.go's header) and the discipline is the
// same: the layout is defined once, the refusals are phrased as the firmware phrases
// them, and the acceptance side is pinned by tests/fixtures/sentinel.orbapp and its
// sidecar rather than by an expectation hand-typed into a Go test. A drift here fails
// against a number the writer produced.
//
// The words are ordered by derivation, never spelled twice, exactly as the writer derives
// them: the title field follows the slug field by the slug's width, and the editor triple
// follows the code quadruple. tools/host_app_fixture_test.py pins the C mirror's agreement
// with this file's table the way host_orbapp_reader_test.py pins the dial's.

// AppGeomABI is the dialect this build speaks — orbapp.GEOM_ABI and
// ORBAPP_GEOM_ABI. A container outside its own abi_min..abi_max range is refused with
// C4's sentence, not installed and silently ignored.
const AppGeomABI = 3

const (
	// appHdrBytes is orbapp.HDR_BYTES == ORBAPP_HDR_WORDS * 4. A .orbapp is a header
	// and then payloads at offsets the header names, so nothing can be read at all
	// from fewer bytes than this.
	appHdrBytes   = 204
	appMagic      = 0x00415247 // 'G','R','A' — deliberately unlike the theme's magic
	appFormat     = 2          // ORBAPP_FORMAT; a reader refuses others, in its own words
	appSlugBytes  = 24         // ORBAPP_SLUG_MAX, NUL included
	appTitleBytes = 32         // ORBAPP_TITLE_MAX, NUL included

	// appGeomCap is ORBAPP_GEOM_BYTES_MAX; appEditorCap is ORBAPP_EDITOR_MAX. Both are
	// quoted here because this reader refuses past them the way the writer refuses to
	// emit past them: the hub's job is to name a cost before a byte crosses the room,
	// not to forward an upload the dial is certain to reject.
	appGeomCap   = 0x40000  // 256 KB — the apps-partition slot stride depends on this
	appEditorCap = 0x200000 // 2 MiB, and no more. Uninstalled, so it costs no flash.
)

// The fixed word positions, all prefixed appW because themecontainer.go already owns
// wMagic/wFormat/wGeomSize/etc. for the THEME header, where the same field names sit at
// different indices. Two layouts, two tables, one prefix each.
//
// Everything from appWTitle on is derived, matching orbapp.py's W_* table and
// orb_app_manifest.h's ORBAPP_W_* enum.
const (
	appWMagic     = 0
	appWFormat    = 1
	appWGeomABI   = 2
	appWHdrSize   = 3
	appWSerial    = 4
	appWBakedMs   = 5
	appWABIMin    = 6
	appWABIMax    = 7
	appWSig       = 8
	appWAppID     = 9
	appWSlug      = 10
	appWTitle     = appWSlug + (appSlugBytes+3)/4
	appWBehaviour = appWTitle + (appTitleBytes+3)/4
	appWArtReq    = appWBehaviour + 1
	appWNLayer    = appWArtReq + 1
	appWNComp     = appWNLayer + 1
	appWNAux      = appWNComp + 1
	appWNNBank    = appWNAux + 1
	appWPeriodMS  = appWNNBank + 1
	appWCaps      = appWPeriodMS + 1
	appWNSettings = appWCaps + 1
	appWSetOff    = appWNSettings + 1
	appWGeomOff   = appWSetOff + 1
	appWGeomSize  = appWGeomOff + 1
	appWGeomCRC   = appWGeomSize + 1
	appWArtOff    = appWGeomCRC + 1
	appWArtSize   = appWArtOff + 1
	appWArtCRC    = appWArtSize + 1
	// P7's code quadruple, then D37's editor triple, both INSIDE the header CRC's span.
	// A gate word placed outside hdr_crc could be rewritten on the card without
	// disturbing any checksum, and the editor is the one region a stranger is invited to
	// author — which is exactly why D37 put it in rather than after the CRC.
	appWCodeOff  = appWArtCRC + 1
	appWCodeSize = appWCodeOff + 1
	appWCodeCRC  = appWCodeSize + 1
	appWWasmMem  = appWCodeCRC + 1
	appWEditOff  = appWWasmMem + 1
	appWEditSize = appWEditOff + 1
	appWEditCRC  = appWEditSize + 1
	appWHdrCRC   = appWEditCRC + 1
	appWAssetOff = appWHdrCRC + 1
	appWNAssets  = appWAssetOff + 1
	appWHdrWords = appWHdrCRC + 4 // asset_off, n_assets, and one word held free
)

// Owner-grantable capability bits, numbered by orb_svc.h's orb_cap_t. The four free caps
// (time, config, ui, art) occupy 0..3 and are declarable NEVER, so a container naming one
// is refused below rather than silently honoured: orbapp.py's check_caps does the same,
// and the D19 consent screen a wrong bit would mislabel is why these numbers are
// load-bearing rather than decorative.
const (
	AppCapPower  = 1 << 4
	AppCapNet    = 1 << 5
	AppCapSDRead = 1 << 6
	AppCapThemes = 1 << 7

	appCapOwnerAll = AppCapPower | AppCapNet | AppCapSDRead | AppCapThemes
)

// AppCapNames is orbapp.CAP_OWNER in orb_cap_t order, so a caps value names itself the
// way the dial's own disclosure string does.
var AppCapNames = []struct {
	bit  uint32
	name string
}{
	{AppCapPower, "power"},
	{AppCapNet, "net"},
	{AppCapSDRead, "sd_read"},
	{AppCapThemes, "themes"},
}

// AppContainer is one .orbapp's identity: what the hub shows an operator, and the fields
// the drift comparison reads. The CRCs are the answer to "is this the same app that is on
// that orb" — serial alone is a knob the author turns and can forget to turn.
type AppContainer struct {
	Slug   string `json:"slug"`
	Title  string `json:"title"`
	Caps   uint32 `json:"caps"`
	Serial uint32 `json:"serial"`
	Baked  uint32 `json:"baked_ms"`
	ABI    uint32 `json:"abi"`
	ABIMin uint32 `json:"abi_min"`
	ABIMax uint32 `json:"abi_max"`
	Format uint32 `json:"format"`
	AppID  uint32 `json:"app_id"`

	// The identity tuple orb_app_install.c matches an upload against, plus the D37
	// region's checksum. Together they say which BUILD these bytes are, which is what
	// lets the hub call a sidecar file current or stale against a board rather than
	// merely present.
	GeomCRC   uint32 `json:"geom_crc"`
	CodeCRC   uint32 `json:"code_crc"`
	CodeSize  uint32 `json:"code_size"`
	EditorCRC uint32 `json:"editor_crc"`

	// EditorSize is the region's byte count, and 0 is the honest answer for an app that
	// ships no editor. Presence is what the Themes face asks. The hub serves the region
	// and the dial never installs it, so a container can legitimately carry an editor
	// this dial would not know what to do with.
	EditorSize uint32 `json:"editor_size"`
	// EditorOff is the region's byte offset inside the file — the header's own
	// word, exposed so a host caller can slice the region out of a container it
	// has already parsed. ParseApp refuses a range that runs past the file.
	EditorOff uint32 `json:"editor_off"`
	Bytes      int    `json:"bytes"`
}

// HasThemes reports whether this app can hold a theme at all. That is the cap question,
// and it is the only truthful way to decide whether a Themes pane should exist for an app
// that has never had a theme written for it — the inversion D37 exists to close.
func (a *AppContainer) HasThemes() bool { return a.Caps&AppCapThemes != 0 }

// HasEditor reports whether the container carries an authoring payload.
func (a *AppContainer) HasEditor() bool { return a.EditorSize > 0 }

// CapNames lists the declared owner caps in orb_cap_t order, as the device's own
// disclosure string lists them.
func (a *AppContainer) CapNames() []string {
	out := []string{}
	for _, c := range AppCapNames {
		if a.Caps&c.bit != 0 {
			out = append(out, c.name)
		}
	}
	return out
}

// ParseApp reads an .orbapp header and refuses, in the dial's own words, what the dial
// would refuse on the same bytes. The order is the firmware's (orb_app_manifest.c's
// orb_app_manifest_parse): shape, magic, format, the ABI ladder, the header's own
// checksum, the signature word, then the identity fields. Keeping that order is not
// style — it is what makes the sentence an operator reads describe the FIRST thing wrong
// with their file rather than the last.
//
// The hub reads a file it is about to forward, so a refusal here saves an operator a dial
// round trip and a card a write. It does not replace the door's check: the device re-reads
// every byte on the upload that follows, and for a container shipping an editor region the
// door is the authority and this is the courtesy.
func ParseApp(body []byte) (*AppContainer, error) {
	if len(body) < appHdrBytes {
		return nil, fmt.Errorf("orbnet: that file is %d B, shorter than the %d B header a .orbapp starts with",
			len(body), appHdrBytes)
	}
	w := func(i int) uint32 { return binary.LittleEndian.Uint32(body[i*4:]) }
	if w(appWMagic) != appMagic {
		return nil, fmt.Errorf("orbnet: not an .orbapp file: magic is 0x%08X", w(appWMagic))
	}
	if f := w(appWFormat); f != appFormat {
		return nil, fmt.Errorf("orbnet: that is a FORMAT %d app container and the orb reads FORMAT %d only — re-package it",
			f, appFormat)
	}
	// C4's ladder, stated as the dial states it. The writer stamps abi == abi_min ==
	// abi_max unless told otherwise, so a range that excludes this build's dialect names
	// a file built for a different dial, and the fix is a re-package rather than a flash.
	abi, lo, hi := w(appWGeomABI), w(appWABIMin), w(appWABIMax)
	if abi != AppGeomABI || !(lo <= uint32(AppGeomABI) && uint32(AppGeomABI) <= hi) {
		return nil, fmt.Errorf("orbnet: app needs geometry ABI %d..%d; this build speaks %d — re-package it (C4)",
			lo, hi, AppGeomABI)
	}
	// The header's own checksum, over the header with the checksum word zeroed — the rule
	// the theme container shares, and the reason a torn copy is caught by the first 204
	// bytes instead of after a megabyte of payload has streamed. The sentence names which
	// repair applies: re-copy means the bytes were damaged on the way here, re-bake means
	// they were never right.
	if sum := appHdrCRC(body); w(appWHdrCRC) != sum {
		return nil, fmt.Errorf("orbnet: container header checksum mismatch — re-copy the file onto the card (header says 0x%08X, its bytes say 0x%08X)",
			w(appWHdrCRC), sum)
	}
	if w(appWSig) != 0 {
		return nil, errors.New("orbnet: sig is non-zero and this build verifies no signatures (C3): refusing to install bytes nobody vouched for")
	}
	slug, err := appField(body, appWSlug, appSlugBytes, "slug")
	if err != nil {
		return nil, err
	}
	if slug == "" {
		return nil, errors.New("orbnet: app declares no slug in its header — re-package it")
	}
	// checkSlug is the family rule, shared with the theme index by design: lowercase
	// a-z 0-9 _, 1..23 bytes. The slug is the NVS namespace, the ?to= route and the
	// filename stem at once, so a character any of the three treats specially installs an
	// app nobody can find again.
	if err := checkSlug([]byte(slug)); err != nil {
		return nil, err
	}
	title, err := appField(body, appWTitle, appTitleBytes, "title")
	if err != nil {
		return nil, err
	}
	if title == "" {
		return nil, errors.New("orbnet: app declares no title: the install confirm and the carousel row both read this field")
	}
	caps := w(appWCaps)
	if caps&^appCapOwnerAll != 0 {
		return nil, fmt.Errorf("orbnet: header caps 0x%X are not a legal declaration — owner caps only (power, net, sd_read, themes), and nothing at or above CAP_N",
			caps)
	}
	// app_id is sha256(slug)'s first four bytes big-endian, and the one cheap defence
	// against a file whose slug was edited after it was named. The firmware runs the same
	// cross-check for the same reason; a hub that skipped it would forward the file and
	// let the dial find it.
	if want := AppIDOf(slug); w(appWAppID) != want {
		return nil, fmt.Errorf("orbnet: app_id is 0x%08X and slug %q hashes to 0x%08X — the header disagrees with its own name",
			w(appWAppID), slug, want)
	}
	if g := w(appWGeomSize); g > appGeomCap {
		return nil, fmt.Errorf("orbnet: geometry region is %d B; a slot holds %d B — re-bake the app smaller",
			g, appGeomCap)
	}
	edSize := w(appWEditSize)
	if edSize > appEditorCap {
		return nil, fmt.Errorf("orbnet: editor region is %d B; the format caps it at %d B — re-bake the editor smaller",
			edSize, appEditorCap)
	}
	// A declared region has to actually be in the file. This is the check the dial performs
	// by streaming and the hub can perform for free off a length it already holds, and it
	// is the one that separates "truncated in transit" from "authored wrong".
	if off := w(appWEditOff); edSize > 0 && int64(off)+int64(edSize) > int64(len(body)) {
		return nil, fmt.Errorf("orbnet: header names a %d B editor region at %d and the file ends at %d B — re-copy it",
			edSize, off, len(body))
	}
	return &AppContainer{
		Slug: slug, Title: title, Caps: caps,
		Serial: w(appWSerial), Baked: w(appWBakedMs),
		ABI: abi, ABIMin: lo, ABIMax: hi, Format: w(appWFormat), AppID: w(appWAppID),
		GeomCRC: w(appWGeomCRC), CodeCRC: w(appWCodeCRC), CodeSize: w(appWCodeSize),
		EditorCRC: w(appWEditCRC), EditorSize: edSize,
		// EditorOff mirrors the firmware's header word so a host caller (the
		// starter-freshness test) can slice the region out of a file without
		// re-deriving the ladder; ParseApp validated it in range already.
		EditorOff: w(appWEditOff), Bytes: len(body),
	}, nil
}

// AppEditorRegion slices an authored region out of a container body, and returns nil with
// no error for an app that ships none. The caller gets the bytes the header's editor_crc
// was computed over, which is the only way a hub-side bake can check its own output
// through this reader rather than by hand-slicing at a constant offset — the mistake that
// puts a region's bytes somewhere its own header does not name.
//
// The region is not installed and not interpreted here: the dial never reads it (D37's
// whole pricing rule is that it costs no flash), and the hub unpacks it only when a
// browser asks for one file of it.
func AppEditorRegion(body []byte) ([]byte, error) {
	c, err := ParseApp(body)
	if err != nil {
		return nil, err
	}
	if c.EditorSize == 0 {
		return nil, nil
	}
	off := int(binary.LittleEndian.Uint32(body[appWEditOff*4:]))
	region := body[off : off+int(c.EditorSize)]
	if sum := crc32.ChecksumIEEE(region); sum != c.EditorCRC {
		return nil, fmt.Errorf("orbnet: editor region's checksum is 0x%08X and its %d bytes say 0x%08X — re-bake the editor (D37)",
			c.EditorCRC, len(region), sum)
	}
	return region, nil
}

// AppIDOf is orbapp.app_id_of and orb_app_manifest.c's derivation: the first four bytes of
// sha256(UTF-8 slug), big-endian. One rule, three readers.
func AppIDOf(slug string) uint32 {
	d := sha256.Sum256([]byte(slug))
	return binary.BigEndian.Uint32(d[:4])
}

// appHdrCRC is orbapp._hdr_crc: crc32/IEEE over the header words with the checksum word
// itself zeroed. It covers the manifest, so a word that moved must move here too.
func appHdrCRC(body []byte) uint32 {
	z := make([]byte, appHdrBytes)
	copy(z, body[:appHdrBytes])
	binary.LittleEndian.PutUint32(z[appWHdrCRC*4:], 0)
	return crc32.ChecksumIEEE(z)
}

// appField reads a NUL-terminated field out of whole words, as orbapp._getstr does. The
// difference from a plain byte slice is the termination rule, and it matters: a field with
// no NUL anywhere in its width is damage, not a long name, and reading past it would print
// the following field's bytes as part of the app's title.
func appField(body []byte, base, nbytes int, what string) (string, error) {
	out := make([]byte, 0, nbytes)
	terminated := false
	for i := 0; i < nbytes; i++ {
		c := body[(base+i/4)*4+i%4]
		if c == 0 {
			terminated = true
			break
		}
		out = append(out, c)
	}
	if !terminated && len(out) == nbytes {
		return "", fmt.Errorf("orbnet: the %q field fills its whole %d B width with no NUL — the header is damaged",
			what, nbytes)
	}
	return string(out), nil
}

// IdentityDiff compares this container against one installed row's identity tuple and
// names every word that disagrees. It returns comparable=false when the row sent no tuple
// at all — a compiled row, or a dial older than the fields — because "I could not check"
// and "it matches" are different sentences to show an operator, and the second one ends
// the argument.
//
// The words are the dial's own tuple plus D37's region checksum: geom_crc, the code pair,
// serial, editor_crc. Comparing all five rather than trusting serial is the point. Serial
// is a knob the author turns and can forget to turn, and geom_crc is what actually changed
// when a manifest moved; an operator who is told "serial 3 is not serial 4" learns less
// than one who is told the geometry table differs.
//
// Direction is only ever claimed for serial, the one word with an ordering. The rest are
// identities, and calling one "older" would be a guess about which of two files the author
// touched last.
func (a *AppContainer) IdentityDiff(r AppInstalledRow) (drift []string, comparable bool) {
	if !r.CarriesIdentity() {
		return nil, false
	}
	if a.Slug != r.Slug {
		// Not a drift case: a row for a different app. Refusing to compare is the honest
		// answer, and the caller found the row by slug, so it should not be reachable.
		return []string{"slug " + r.Slug + " is not " + a.Slug}, true
	}
	for _, f := range []struct {
		word   string
		mine   uint32
		theirs uint32
	}{
		{"geometry", a.GeomCRC, r.GeomCRC},
		{"code", a.CodeCRC, r.CodeCRC},
		{"code size", a.CodeSize, r.CodeSize},
		{"serial", a.Serial, r.Serial},
		{"editor", a.EditorCRC, r.EditorCRC},
	} {
		if f.mine != f.theirs {
			drift = append(drift, fmt.Sprintf("%s is %d on the orb and %d here",
				f.word, f.theirs, f.mine))
		}
	}
	return drift, true
}
