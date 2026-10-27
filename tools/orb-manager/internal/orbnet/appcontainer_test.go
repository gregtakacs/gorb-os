package orbnet

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The hub's .orbapp reader, pinned against the file the authority wrote.
//
// tools/orbapp.py owns this layout; main/app/orb_app_manifest.c reads it on the dial and
// tools/host_orbapp_reader_test.py pins that mirror; this file is the hub's read, pinned
// the same way the theme container is — against tests/fixtures/sentinel.orbapp and the
// sidecar tools/make_app_fixture.py wrote beside it. The accept side is NOT tested against
// a Go-written fixture: a fixture written by the reader meant to check it proves nothing,
// which is why themecontainer_test.go also reaches for the Python writer's bytes.

func appFixture(t *testing.T, name string) []byte {
	t.Helper()
	p := filepath.Join("..", "..", "..", "..", "tests", "fixtures", name)
	b, err := os.ReadFile(p)
	if err != nil {
		t.Fatalf("read %s: %v", p, err)
	}
	return b
}

func appSidecar(t *testing.T) map[string]any {
	t.Helper()
	var m map[string]any
	if err := json.Unmarshal(appFixture(t, "app-pair.json"), &m); err != nil {
		t.Fatal(err)
	}
	return m
}

func sideApp(t *testing.T, m map[string]any, slug string) map[string]any {
	t.Helper()
	apps, _ := m["apps"].(map[string]any)
	a, ok := apps[slug].(map[string]any)
	if !ok {
		t.Fatalf("sidecar has no entry for %q; re-run tools/make_app_fixture.py", slug)
	}
	return a
}

func num(a map[string]any, k string) uint32 {
	v, _ := a[k].(float64)
	return uint32(int64(v))
}

// The hub parses the authority's file and agrees with it on every field it reads.
func TestParseAppAgreesWithTheAuthority(t *testing.T) {
	side := appSidecar(t)
	for _, slug := range []string{"sentinel", "plain"} {
		want := sideApp(t, side, slug)
		body := appFixture(t, slug+".orbapp")
		got, err := ParseApp(body)
		if err != nil {
			t.Fatalf("%s: the hub refused the file the authority wrote: %v", slug, err)
		}
		for _, f := range []struct {
			name string
			have uint32
		}{
			{"caps", got.Caps}, {"serial", got.Serial}, {"baked_ms", got.Baked},
			{"abi", got.ABI}, {"abi_min", got.ABIMin}, {"abi_max", got.ABIMax},
			{"format", got.Format}, {"app_id", got.AppID},
			{"geom_crc", got.GeomCRC}, {"code_crc", got.CodeCRC},
			{"code_size", got.CodeSize}, {"editor_crc", got.EditorCRC},
			{"editor_size", got.EditorSize},
		} {
			if n := num(want, f.name); f.have != n {
				t.Errorf("%s: %s is %d, authority read back %d", slug, f.name, f.have, n)
			}
		}
		if got.Slug != want["slug"] {
			t.Errorf("%s: slug is %q, sidecar says %v", slug, got.Slug, want["slug"])
		}
		if got.Title != want["title"] {
			t.Errorf("%s: title is %q, sidecar says %v", slug, got.Title, want["title"])
		}
		if int64(got.Bytes) != int64(want["bytes"].(float64)) {
			t.Errorf("%s: file is %d B, sidecar says %v", slug, got.Bytes, want["bytes"])
		}
		// caps_named is orbapp.caps_names and CapNames is this reader's version of the
		// same list. Two spellings of one disclosure, so the ORDER is load-bearing.
		var named []string
		for _, s := range want["caps_named"].([]any) {
			named = append(named, s.(string))
		}
		if h := got.CapNames(); strings.Join(h, ",") != strings.Join(named, ",") {
			t.Errorf("%s: caps read as %v, authority lists %v", slug, h, named)
		}
	}
}

// The two shapes the hub exists to tell apart: a themed app carrying a region, and a plain
// app carrying neither. Every S4/S5 consumer branches on exactly these two booleans.
func TestParseAppSeparatesTheTwoShapes(t *testing.T) {
	side := appSidecar(t)
	themed, err := ParseApp(appFixture(t, "sentinel.orbapp"))
	if err != nil {
		t.Fatal(err)
	}
	plain, err := ParseApp(appFixture(t, "plain.orbapp"))
	if err != nil {
		t.Fatal(err)
	}
	if !themed.HasThemes() || !themed.HasEditor() {
		t.Errorf("sentinel: HasThemes=%v HasEditor=%v, sidecar says a themes cap and a %d B region",
			themed.HasThemes(), themed.HasEditor(), num(sideApp(t, side, "sentinel"), "editor_size"))
	}
	if plain.HasThemes() || plain.HasEditor() {
		t.Errorf("plain: HasThemes=%v HasEditor=%v; a zeroed triple must read as neither",
			plain.HasThemes(), plain.HasEditor())
	}
	if themed.Serial == themed.Baked {
		t.Error("fixture is useless: serial == baked_ms would let a reader that mixed the two words pass")
	}
}

// Every refusal below is one the dial also performs. The sentences are quoted from
// orb_app_manifest.c and orbapp.py rather than invented, so an operator who reads one at
// the hub and one on the dial is reading the same instruction (D26).
func TestParseAppRefusesWhatTheDialRefuses(t *testing.T) {
	// accept is the sentinel want string meaning "this mutation must still parse".
	const accept = "\x00accept"
	good := appFixture(t, "sentinel.orbapp")
	if _, err := ParseApp(good[:100]); err == nil ||
		!strings.Contains(err.Error(), "shorter than the 204 B header") {
		t.Errorf("a 100 B file got: %v", err)
	}
	// stamp sets one header word and re-derives the header checksum over the result.
	// Every case below except the checksum's own needs it: the gate words all sit inside
	// hdr_crc's span by D37's rule, so an unstamped mutation is refused for the wrong
	// reason and the test would pass while the check it names stayed unreachable. The
	// dial reaches the same checks by the same route — its door re-verifies the checksum
	// first, then the fields.
	stamp := func(b []byte, word int, v uint32) {
		binary.LittleEndian.PutUint32(b[word*4:], v)
		binary.LittleEndian.PutUint32(b[appWHdrCRC*4:], appHdrCRC(b))
	}
	cases := []struct {
		name  string
		patch func(b []byte)
		want  string
	}{
		{"magic", func(b []byte) { stamp(b, appWMagic, 0xDEADBEEF) },
			"not an .orbapp file"},
		{"format", func(b []byte) { stamp(b, appWFormat, 1) },
			"FORMAT 1 app container"},
		{"abi above this build", func(b []byte) { stamp(b, appWGeomABI, 4) },
			"geometry ABI 3..3; this build speaks 3"},
		{"abi_min past this build", func(b []byte) { stamp(b, appWABIMin, 9) },
			"re-package it (C4)"},
		// The checksum case is the one mutation deliberately left unstamped, and it has
		// to be a header byte: a payload byte is outside the checksum's span by design,
		// so a reader that refused one would be refusing a file the dial installs.
		{"header torn", func(b []byte) { b[100] ^= 0x01 }, "checksum mismatch"},
		// ACCEPT case, and the reason it sits in a refusal table: the header checksum
		// covers the header and the manifest, not the payloads, so a bit flip in the
		// editor region must NOT be caught here. The region's own crc catches it at the
		// door, and a hub that refused on the header checksum would be refusing files
		// the dial installs happily.
		{"payload byte outside the span",
			func(b []byte) { b[len(b)-1] ^= 0x01 }, accept},
		{"sig nonzero", func(b []byte) { stamp(b, appWSig, 7) },
			"verifies no signatures"},
		{"empty slug", func(b []byte) { stamp(b, appWSlug, 0) }, "declares no slug"},
		{"bad slug char", func(b []byte) { b[appWSlug*4] = 'A'; stamp(b, appWSlug, binary.LittleEndian.Uint32(b[appWSlug*4:])) }, "not lowercase"},
		{"empty title", func(b []byte) { stamp(b, appWTitle, 0) }, "declares no title"},
		{"free cap declared",
			func(b []byte) { stamp(b, appWCaps, AppCapThemes|0x1) },
			"not a legal declaration"},
		{"cap above the top bit",
			func(b []byte) { stamp(b, appWCaps, 1<<9) },
			"not a legal declaration"},
		{"app_id disagrees",
			func(b []byte) { stamp(b, appWAppID, 0x12345678) },
			"hashes to"},
		{"geometry over slot",
			func(b []byte) { stamp(b, appWGeomSize, appGeomCap+1) },
			"re-bake the app smaller"},
		{"region over cap",
			func(b []byte) { stamp(b, appWEditSize, appEditorCap+1) },
			"caps it at 2097152 B"},
		{"region past eof",
			func(b []byte) { stamp(b, appWEditSize, uint32(len(good))) },
			"re-copy it"},
	}
	for _, c := range cases {
		b := append([]byte(nil), good...)
		c.patch(b)
		_, err := ParseApp(b)
		if c.want == accept {
			if err != nil {
				t.Errorf("%s: refused a file the dial accepts: %v", c.name, err)
			}
			continue
		}
		if err == nil {
			t.Errorf("%s: accepted a container the dial refuses", c.name)
			continue
		}
		if !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: said %q, want it to contain %q", c.name, err, c.want)
		}
	}
}

// The word map is derived, and the derivation is the thing that breaks. The fixture pins
// the VALUES; this pins the SHAPE, so a change to a field width moves the test rather than
// silently sliding the title over the behaviour byte the way FORMAT 1's theme header once
// did (orbapp.py's W_* comment records exactly that bug). The wanted numbers are orbapp.py's
// own, read out of the module — hand-typing them here would make this test a second
// opinion rather than a cross-check.
// TestAppHeaderWordsMatchTheWriter compares this file's derived table word for word
// against the sidecar, which the generator lifted out of orbapp.py. Nothing is typed
// twice, so a writer change fails here rather than quietly moving the reader.
func TestAppHeaderWordsMatchTheWriter(t *testing.T) {
	side := appSidecar(t)
	words := map[string]int{}
	for k, v := range side["words"].(map[string]any) {
		words[k] = int(v.(float64))
	}
	if len(words) == 0 {
		t.Fatal("sidecar carries no word table; re-run tools/make_app_fixture.py")
	}
	for _, c := range []struct {
		name string
		have int
	}{
		{"magic", appWMagic}, {"format", appWFormat}, {"geom_abi", appWGeomABI},
		{"hdr_size", appWHdrSize}, {"serial", appWSerial}, {"baked_ms", appWBakedMs},
		{"abi_min", appWABIMin}, {"abi_max", appWABIMax}, {"sig", appWSig},
		{"app_id", appWAppID}, {"slug", appWSlug}, {"title", appWTitle},
		{"behaviour", appWBehaviour}, {"art_req", appWArtReq},
		{"n_layer", appWNLayer}, {"n_comp", appWNComp}, {"n_aux", appWNAux},
		{"n_bank", appWNNBank}, {"period_ms", appWPeriodMS}, {"caps", appWCaps},
		{"n_settings", appWNSettings}, {"set_off", appWSetOff},
		{"geom_off", appWGeomOff}, {"geom_size", appWGeomSize},
		{"geom_crc", appWGeomCRC}, {"art_off", appWArtOff}, {"art_size", appWArtSize},
		{"art_crc", appWArtCRC}, {"code_off", appWCodeOff},
		{"code_size", appWCodeSize}, {"code_crc", appWCodeCRC},
		{"wasm_mem", appWWasmMem}, {"editor_off", appWEditOff},
		{"editor_size", appWEditSize}, {"editor_crc", appWEditCRC},
		{"hdr_crc", appWHdrCRC}, {"asset_off", appWAssetOff},
		{"n_assets", appWNAssets},
	} {
		w, ok := words[c.name]
		if !ok {
			t.Errorf("sidecar names no word %q; the generator's WORD_KEYS and this table disagree", c.name)
			continue
		}
		if c.have != w {
			t.Errorf("appW%s is word %d, orbapp.py says %d", c.name, c.have, w)
		}
		if hw := words["hdr_crc"] + 4; c.name == "n_assets" && appWHdrWords != hw {
			t.Errorf("header is %d words and hdr_crc sits at %d; those derive apart", appWHdrWords, hw)
		}
	}
	if appHdrBytes != appWHdrWords*4 {
		t.Errorf("header is %d B across %d words; those disagree", appHdrBytes, appWHdrWords)
	}
	if hdr := int(side["hdr_bytes"].(float64)); hdr != appHdrBytes {
		t.Errorf("header is %d B here and %d B in the writer", appHdrBytes, hdr)
	}
	// The three words a caller reads to decide whether a container is editable must sit
	// INSIDE the checksum's span, or a stranger could rewrite them on the card and leave
	// every checksum intact. orbapp.py's P7 comment is the whole argument, restated as an
	// assertion because it is a placement rule and placement is what regresses.
	if appWEditCRC >= appWHdrCRC || appWCodeOff >= appWHdrCRC {
		t.Errorf("gate words at code %d / editor %d must precede hdr_crc at %d",
			appWCodeOff, appWEditCRC, appWHdrCRC)
	}
}

// AppIDOf is a hash the whole family shares: orbapp.app_id_of, orb_app_manifest.c's
// derivation, and the .clinerules rule both obey. Two answers come from the sidecar and
// one from the shipped seed, so a change in the rule fails here rather than at install time
// on a dial across the room.
func TestAppIDOfMatchesKnownAnswers(t *testing.T) {
	side := appSidecar(t)
	for _, slug := range []string{"sentinel", "plain"} {
		if got, want := AppIDOf(slug), num(sideApp(t, side, slug), "app_id"); got != want {
			t.Errorf("AppIDOf(%q) = 0x%08X, authority says 0x%08X", slug, got, want)
		}
	}
	// tools/orb-manager/seed/clock.orbapp stamps this value today, and seed.go names the
	// same app by literal — the pairing D37 removes and the reason it is worth pinning now.
	if got := AppIDOf("clock"); got != 0xD8198EFA {
		t.Errorf("AppIDOf(\"clock\") = 0x%08X, not the 0xD8198EFA the seed container stamps", got)
	}
}

// The region round-trips through its own checksum, and only through it. This is the
// property D37 prices: the region is not part of the installed bill, so the sole thing
// standing between an author's bytes and a browser's DOM is editor_crc.
func TestAppEditorRegionRoundTripsItsOwnChecksum(t *testing.T) {
	side := appSidecar(t)
	body := appFixture(t, "sentinel.orbapp")
	region, err := AppEditorRegion(body)
	if err != nil {
		t.Fatalf("the hub refused its own region: %v", err)
	}
	if want := int(num(sideApp(t, side, "sentinel"), "editor_size")); len(region) != want {
		t.Fatalf("region is %d B, header names %d B", len(region), want)
	}
	zr, err := zip.NewReader(bytes.NewReader(region), int64(len(region)))
	if err != nil {
		t.Fatalf("region is not a readable zip: %v", err)
	}
	var names []string
	for _, f := range zr.File {
		// STORE, per plan §5.1: the hub seeks one member at a time, and a bake that
		// deflated would still open here — so the flag is asserted rather than assumed.
		if f.Method != zip.Store {
			t.Errorf("%s is deflated; the format's rule is STORE so the hub can seek it", f.Name)
		}
		names = append(names, f.Name)
	}
	want := sideApp(t, side, "sentinel")["editor_names"].([]any)
	if strings.Join(names, ",") != joinAny(want) {
		t.Errorf("region holds %v, the authority listed %v", names, want)
	}

	// A byte flipped INSIDE the region leaves hdr_crc intact by design. Only the
	// region's own CRC can catch it, and a reader that skipped that check would hand a
	// browser a corrupted DOM while every header checksum still read clean.
	torn := append([]byte(nil), body...)
	off := int64(num(sideApp(t, side, "sentinel"), "editor_off"))
	torn[off+3] ^= 0x01
	if _, err := ParseApp(torn); err != nil {
		t.Errorf("header refused a file whose header it never read: %v", err)
	}
	if _, err := AppEditorRegion(torn); err == nil ||
		!strings.Contains(err.Error(), "re-bake the editor") {
		t.Errorf("a flipped region byte got: %v", err)
	}

	// An app that ships nothing answers nil, not an error: every container that ever
	// shipped before D37 is in that set, and the Themes face has to ask this question of
	// all of them.
	if r, err := AppEditorRegion(appFixture(t, "plain.orbapp")); err != nil || r != nil {
		t.Errorf("plain.orbapp: region=%d B err=%v, want no bytes and no error", len(r), err)
	}
}

func joinAny(in []any) string {
	out := make([]string, 0, len(in))
	for _, v := range in {
		out = append(out, v.(string))
	}
	return strings.Join(out, ",")
}
