package orbnet

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"hash/crc32"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
)

// The hub's FORMAT 3 gate, pinned against the file the authority wrote.
//
// Four readers now speak one layout: tools/orbbundle.py (the authority), main/art/
// orb_bundle.c (the device's door), this file (the hub's pre-gate) and the Theme Editor's
// writer in the browser. D33 rules that the browser owns the format, which makes a Go
// re-implementation of the writer the thing to avoid — but it does not excuse a hub that
// refuses what its own devices install, which is the live bug this file fixes. So the
// accept side is not tested against a Go-written fixture: it is tested against
// tests/fixtures/format3-pair.orbtheme, the bytes orbbundle.py wrote, with the numbers
// that script read back out of its own reader. A drift here fails against a number.

func fixture(t *testing.T, name string) []byte {
	t.Helper()
	p := filepath.Join("..", "..", "..", "..", "tests", "fixtures", name)
	b, err := os.ReadFile(p)
	if err != nil {
		t.Fatalf("read %s: %v", p, err)
	}
	return b
}

func fixtureSidecar(t *testing.T) map[string]any {
	t.Helper()
	var m map[string]any
	b := fixture(t, "format3-pair.json")
	if err := json.Unmarshal(b, &m); err != nil {
		t.Fatal(err)
	}
	return m
}

// The hub parses the authority's file and agrees with it on every field the gate reads.
func TestContainerParsesTheAuthoritysBundle(t *testing.T) {
	side := fixtureSidecar(t)
	body := fixture(t, "format3-pair.orbtheme")
	c, err := ContainerName(body)
	if err != nil {
		t.Fatalf("the hub refused the file the authority wrote: %v", err)
	}
	if c.Name != side["name"] {
		t.Errorf("name is %q, want %q from the sidecar", c.Name, side["name"])
	}
	// The hub files a bundle under its FIRST section's stem (slug-sorted index):
	// the sidecar holds one stem per app the fixture themes.
	stems := side["card_stems"].(map[string]any)
	if c.ID != stems[side["slugs"].([]any)[0].(string)] {
		t.Errorf("card stem is %q, want %q", c.ID, stems[side["slugs"].([]any)[0].(string)])
	}
	if int64(c.Format) != int64(3) || int64(c.ABI) != int64(side["abi"].(float64)) {
		t.Errorf("format/abi = %d/%d, want 3/%v", c.Format, c.ABI, side["abi"])
	}
	if int64(c.GeomSize) != int64(side["geom_size"].(float64)) {
		t.Errorf("geom_size is %d, want %v", c.GeomSize, side["geom_size"])
	}
	if int64(c.ArtSize) != int64(side["art_size"].(float64)) {
		t.Errorf("art_size is %d, want %v", c.ArtSize, side["art_size"])
	}
	if int64(c.Serial) != int64(side["serial"].(float64)) {
		t.Errorf("serial is %d, want %v", c.Serial, side["serial"])
	}
	if int64(c.NSections) != int64(side["n_sections"].(float64)) {
		t.Errorf("n_sections is %d, want %v", c.NSections, side["n_sections"])
	}
	if len(c.Sections) != len(side["slugs"].([]any)) {
		t.Fatalf("the index walk found %d sections, want %d", len(c.Sections), len(side["slugs"].([]any)))
	}
	for i, s := range side["slugs"].([]any) {
		if c.Sections[i] != s.(string) {
			t.Errorf("section %d is %q, want %q", i, c.Sections[i], s)
		}
	}
	if c.Bytes != int(side["bytes"].(float64)) {
		t.Errorf("file is %d B, want %v", c.Bytes, side["bytes"])
	}
}

// The same file, POSTed to the device's door over the route the firmware registers: the
// pre-gate and the door agree on one set of bytes, and both land on one id. This is the
// pair the live bug inverted, so it is the test that pins the fix.
func TestHubAndDeviceDoorAgreeOnTheSameBundle(t *testing.T) {
	body := fixture(t, "format3-pair.orbtheme")
	c, err := ContainerName(body)
	if err != nil {
		t.Fatalf("hub refused the file the device took: %v", err)
	}
	f := orbfake.New()
	srv := httptest.NewServer(f)
	t.Cleanup(srv.Close)
	resp, err := http.Post(srv.URL+"/api/v1/sd/theme", "application/octet-stream", bytes.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	out, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("the device's door refused the file the hub accepted: %d %s", resp.StatusCode, out)
	}
	rows := f.Themes()
	if len(rows) != 1 {
		t.Fatalf("the card holds %d rows after one upload", len(rows))
	}
	if rows[0].Name != c.Name || rows[0].ID != c.ID {
		t.Errorf("door filed %s/%q, hub read %s/%q", rows[0].ID, rows[0].Name, c.ID, c.Name)
	}
}

// putWord stamps one header word and fixes nothing else, which is the point: each case
// below breaks exactly one rule and must hear that rule's sentence, in the order the
// firmware checks them.
func putWord(b []byte, word int, v uint32) { binary.LittleEndian.PutUint32(b[word*4:], v) }

// Every refusal the gate owns, and the order they fire in. The sentences are the
// firmware's, not paraphrases — an operator who later sees one on the dial has to hear the
// same words from the hub that held the file.
func TestContainerRefusals(t *testing.T) {
	for _, tc := range []struct {
		what  string
		poke  func(b []byte)
		want  string
		crcOK bool // whether the case leaves the header's own checksum true
	}{
		{"a retired format", func(b []byte) { putWord(b, 1, 2) },
			"reads FORMAT 3 only", false},
		{"a pre-FORMAT-2 artifact", func(b []byte) { putWord(b, 1, 1) },
			"reads FORMAT 3 only", false},
		{"a foreign ABI, one stamp past the delta dialect", func(b []byte) { putWord(b, 2, 5) },
			"reads v3 and v4", false},
		{"a pre-delta ABI", func(b []byte) { putWord(b, 2, 2) },
			"reads v3 and v4", false},
		{"a torn header", func(b []byte) { putWord(b, 3, 129) },
			"container header is damaged", false},

		{"geometry over the cap", func(b []byte) { putWord(b, 5, bundleGeomCap+1) },
			"geometry region is 61441 B", true},
		{"pixels over the cap", func(b []byte) { putWord(b, 7, bundleArtCap+1) },
			"pixel payload is 8323073 B", true},
		{"no name in the header", func(b []byte) {
			for i := wName * 4; i < wName*4+themeNameBytes; i++ {
				b[i] = 0
			}
		}, "no name", true},
	} {
		body := fixture(t, "format3-pair.orbtheme")
		t.Run(tc.what, func(t *testing.T) {
			tc.poke(body)
			// A case that reaches the checksum gate has to leave the checksum true, or
			// it would refuse for the wrong reason and still pass the substring test.
			// The format and ABI cases need no re-stamp: both fire first.
			if tc.crcOK {
				putWord(body, wHdrCRC, crc32.ChecksumIEEE(headerForCRC(body)))
			}
			_, err := ContainerName(body)

			if err == nil {
				t.Fatalf("%s was accepted", tc.what)
			}
			if !strings.Contains(err.Error(), tc.want) {
				t.Errorf("%s: err = %v, want it to say %q", tc.what, err, tc.want)
			}
		})
	}
}

// The delta dialect the C4/C5 animated-plate bake stamps on the header is v4, and the
// device door takes 3 and 4. The hub must forward a v4 container, not refuse a theme its
// own device installs — the §4.1 mirror rule that this gate drifted from. Re-stamp the
// authority's fixture header to v4, re-seat its checksum (the ABI word is inside the CRC
// span), and the gate must open and carry the stamp through.
func TestContainerAcceptsTheDeltaDialect(t *testing.T) {
	body := fixture(t, "format3-pair.orbtheme")
	putWord(body, wGeomABI, themeABI4)
	putWord(body, wHdrCRC, crc32.ChecksumIEEE(headerForCRC(body)))
	c, err := ContainerName(body)
	if err != nil {
		t.Fatalf("the hub refused a v4 delta container the device installs: %v", err)
	}
	if c.ABI != themeABI4 {
		t.Errorf("ABI = %d, want the v4 delta stamp it carries", c.ABI)
	}
}

// D29's boundary is `== cap` accepted, at both sums, because both are install sums and
// not file shapes: geometry's sector-aligned erase ends exactly on the banks, and the
// banks live in the slot's remainder. A gate that read either as `< cap` would refuse the
// largest file the dial can hold, and the owner would never find out why.
func TestContainerAcceptsBothCapsExactly(t *testing.T) {
	for _, tc := range []struct {
		word int
		cap  uint32
	}{
		{wGeomSize, bundleGeomCap},
		{wArtSize, bundleArtCap},
	} {
		body := fixture(t, "format3-pair.orbtheme")
		putWord(body, tc.word, tc.cap)
		// The reader walks the layout the firmware's door walks: a geom_size at the
		// cap moves the art start with it, or the file contradicts itself before the
		// caps ever get asked. The art-cap case cannot fit in a 1.2 MB fixture at
		// all, so its honest assertion is that the hub refused the LAYOUT and not
		// the budget gate, whose rule is `== cap` accepted.
		if tc.word == wGeomSize {
			putWord(body, wArtOff, 65536)
			putWord(body, wArtSize, 4096) // the fixture's own bank cannot span a moved art start
		}
		putWord(body, wHdrCRC, crc32.ChecksumIEEE(headerForCRC(body)))
		c, err := ContainerName(body)
		if tc.word == wArtSize {
			if err == nil || strings.Contains(err.Error(), "a theme slot holds") {
				t.Errorf("art at exactly the cap: err = %v, want a layout refusal, not the budget gate", err)
			}
			continue
		}
		if err != nil {
			t.Errorf("word %d at exactly %d B was refused: %v", tc.word, tc.cap, err)
			continue
		}
		if c.GeomSize != tc.cap {
			t.Errorf("geom_size read back as %d", c.GeomSize)
		}
	}
}

// The double's writer and the hub's reader close the same loop on a smaller file: the
// fake's one-section bundle must parse to one section named "clock", or the fixtures every
// other theme test relies on are bytes nothing reads. Both sides were written against
// orbbundle.py independently, so this fails when either drifts from the layout — which is
// how the u16 index head (slug_off, slug_len, abi_min, abi_max before n_elem) got caught
// the first time.
func TestDoubleWriterAndHubReaderCloseOnOneRecord(t *testing.T) {
	body := orbfake.Container("Fake Pair")
	c, err := ContainerName(body)
	if err != nil {
		t.Fatalf("the hub refused the double's own container: %v", err)
	}
	if len(c.Sections) != 1 || c.Sections[0] != "clock" {
		t.Errorf("sections = %v, want [clock]", c.Sections)
	}
	if c.NSections != 1 || c.GeomSize <= uint32(bundleIdxHead+bundleIdxWords*4) {
		t.Errorf("header reads n_sections=%d geom_size=%d; the fake writes one section and a %d-byte index head plus a record plus a blob",
			c.NSections, c.GeomSize, bundleIdxHead)
	}
}

// The largest bundle the format can legally describe is exactly the largest body the hub
// forwards. Geometry at its 61,440-byte cap pushes the sector-aligned art start to 65,536,
// and the art cap of 8,323,072 (ORB_THEME_SLOT_CAP) lands the last bank byte on
// 8,388,608 — the same number
// security.go's proxyRequestCap holds, to the byte. That is not a coincidence to be
// grateful for; it is a boundary, and a bundle one byte larger is both legal by the format
// and refused by the transport. If either constant moves, this fails and says which.
func TestLargestLegalBundleIsExactlyTheProxyCap(t *testing.T) {
	const proxyRequestCap = 8 << 20 // orb-manager/security.go, restated to keep this honest
	artStart := (uint32(themeHdrBytes) + bundleGeomCap + themeSectorBytes - 1) /
		themeSectorBytes * themeSectorBytes
	if max := artStart + bundleArtCap; max != proxyRequestCap {
		t.Errorf("the largest legal bundle is %d B against a proxy cap of %d B; a bundle in the larger of the two is refused by the wrong door",
			max, proxyRequestCap)
	}
	if artStart != 65536 {
		t.Errorf("sector-aligned art start is %d, want 65536", artStart)
	}
}

// A container whose index lies about its own count is not a container: per-app
// theming reads the index for the identity itself, so forwarding the file would
// forward an id the hub could not vouch for. The device's parse_core refuses the
// identical file with the identical sentence; the hub's door says it first.
func TestContainerReportsNoSectionsOnALiedIndex(t *testing.T) {
	body := fixture(t, "format3-pair.orbtheme")
	putWord(body, wNSections, 3) // the index head still says 1
	putWord(body, wHdrCRC, crc32.ChecksumIEEE(headerForCRC(body)))
	c, err := ContainerName(body)
	if err == nil {
		t.Fatalf("a miscounted index sailed through the hub: %+v", c)
	}
	if !strings.Contains(err.Error(), "header names 3 sections, the index holds 1") {
		t.Errorf("err = %v, want the firmware's header-vs-index sentence", err)
	}
}

// The authority ships a second file on purpose: the same bytes plus a weather section
// written through the writer's rejection-vector bypass. ContainerName refuses it with
// the device's door sentence (orb_theme.c's one_section_why), and ParseContainer still
// walks it — the wire reads 1..8, so a multi-section file stays nameable in a refusal
// and is never guessed past its first slug.
func TestContainerRefusesTheRejectionVector(t *testing.T) {
	body := fixture(t, "format3-two-app.orbtheme")
	c, err := ContainerName(body)
	if err == nil {
		t.Fatalf("a two-app container passed the hub gate: %+v", c)
	}
	if !strings.Contains(err.Error(), "a theme file carries exactly one section — this one carries 2") {
		t.Errorf("err = %v, want the door's one-section sentence", err)
	}
	p, perr := ParseContainer(body)
	if perr != nil {
		t.Fatalf("the wire reader could not walk the rejection vector: %v", perr)
	}
	if p.NSections != 2 || p.Sections[0] != "clock" {
		t.Errorf("parse fields = n=%d first=%q, want n=2 first=clock", p.NSections, p.Sections[0])
	}
	side := fixtureSidecar(t)
	stems := side["card_stems"].(map[string]any)
	if p.ID != stems["clock"].(string) {
		t.Errorf("first-slug id is %q, want the pair fixture's clock stem %v (same name, same slug, same hash)", p.ID, stems["clock"])
	}
}
