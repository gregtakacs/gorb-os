package main

// S3 (ruling 35): the two bounds "Import from orb-os.net" keeps — whose links the hub
// opens at all, and what a bundle's pictures may be — and the header reader the second
// one is measured with. The door tests here ride the same s.routes(mux) the binary
// serves, the way siteimport_test.go's do.

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// --- the allow-list -------------------------------------------------------------------

func TestSiteAllowedMatchesConfiguredSiteAndOrbOSNet(t *testing.T) {
	cases := []struct {
		env  string // GORB_SITE for the case; "" means unset, which is the default
		site string
		want bool
	}{
		{"", "https://orb.takacsfamily.com", true},
		{"", "https://orb.takacsfamily.com:443", true},
		{"", "HTTPS://Orb.TakacsFamily.COM", true},
		{"", "http://orb.takacsfamily.com", false},
		{"", "https://orb-os.net", true},
		{"", "https://orb-os.net:443", true},
		{"", "https://orb-os.net:8443", false},
		{"", "http://orb-os.net", false},
		{"", "https://evil.orb-os.net", false},
		{"", "https://orb-os.net.evil.example", false},
		// fu2-3 (ruling 35, as amended 2026-11-11): the site's other public name is the
		// same door, and every other subdomain — or suffix look-alike — is still refused.
		{"", "https://www.orb-os.net", true},
		{"", "https://www.orb-os.net:443", true},
		{"", "HTTPS://WWW.Orb-OS.NET", true},
		{"", "https://www.orb-os.net:8443", false},
		{"", "http://www.orb-os.net", false},
		{"", "ftp://www.orb-os.net", false},
		{"", "https://api.orb-os.net", false},
		{"", "https://www.orb-os.net.evil.example", false},
		{"", "https://evil.www.orb-os.net", false},
		{"", "https://evil.example", false},
		{"", "http://localhost:3000", true},
		{"", "http://127.0.0.1:8080", true},
		{"", "http://[::1]:9", true},
		{"", "https://localhost", true},
		{"", "ftp://orb-os.net", false},
		{"", "", false},
		{"", "not a url", false},
		{"", "https://user@evil.example", false},
		{"https://bench.example:8443", "https://bench.example:8443", true},
		{"https://bench.example:8443", "https://bench.example", false},
		{"https://bench.example:8443", "https://orb.takacsfamily.com", false},
		{"https://orb-os.net", "https://orb-os.net", true},
		{"https://orb-os.net", "https://orb.takacsfamily.com", false},
		{"https://www.orb-os.net", "https://www.orb-os.net", true},
		{"https://www.orb-os.net", "https://orb-os.net", true},
		{"https://www.orb-os.net", "https://api.orb-os.net", false},
	}
	for _, c := range cases {
		t.Setenv("GORB_SITE", c.env)
		if got := siteAllowed(c.site); got != c.want {
			t.Errorf("GORB_SITE=%q siteAllowed(%q) = %v, want %v", c.env, c.site, got, c.want)
		}
	}
}

func TestSiteAllowedRefusalSentences(t *testing.T) {
	t.Run("the default site names itself and both spellings of orb-os.net", func(t *testing.T) {
		t.Setenv("GORB_SITE", "")
		want := "Orb Manager imports themes only from orb.takacsfamily.com and orb-os.net/www.orb-os.net, not from evil.example"
		if got := siteNotAllowed("https://evil.example").Error(); got != want {
			t.Errorf("sentence = %q, want %q", got, want)
		}
	})
	t.Run("a bench site names its own port", func(t *testing.T) {
		t.Setenv("GORB_SITE", "https://bench.example:8443")
		want := "Orb Manager imports themes only from bench.example:8443 and orb-os.net/www.orb-os.net, not from evil.example:9"
		if got := siteNotAllowed("https://evil.example:9").Error(); got != want {
			t.Errorf("sentence = %q, want %q", got, want)
		}
	})
	t.Run("once the site has moved, the sentence names one place", func(t *testing.T) {
		t.Setenv("GORB_SITE", "https://orb-os.net")
		want := "Orb Manager imports themes only from orb-os.net/www.orb-os.net, not from evil.example"
		if got := siteNotAllowed("https://evil.example").Error(); got != want {
			t.Errorf("sentence = %q, want %q", got, want)
		}
	})
	t.Run("the site moved under its www name and the sentence still names one place", func(t *testing.T) {
		t.Setenv("GORB_SITE", "https://www.orb-os.net")
		want := "Orb Manager imports themes only from orb-os.net/www.orb-os.net, not from api.orb-os.net"
		if got := siteNotAllowed("https://api.orb-os.net").Error(); got != want {
			t.Errorf("sentence = %q, want %q", got, want)
		}
	})
}

// --- the header reader -----------------------------------------------------------------

func TestImageHeaderPNG(t *testing.T) {
	h, ok := readImageHeader(pngArt(9, 9, 1))
	if !ok || h.Type != "png" || h.Width != 9 || h.Height != 9 {
		t.Errorf("pngArt(9,9) = %+v, %v", h, ok)
	}
	h, ok = readImageHeader(pngArt(1024, 640, 2))
	if !ok || h.Width != 1024 || h.Height != 640 {
		t.Errorf("pngArt(1024,640) = %+v, %v", h, ok)
	}
	if _, ok := readImageHeader(pngArt(0, 0, 3)); ok {
		t.Error("a PNG header naming no pixels read as a header")
	}
	if _, ok := readImageHeader(pngArt(9, 9, 4)[:20]); ok {
		t.Error("a PNG cut out of its own IHDR read as a header")
	}
	if _, ok := readImageHeader([]byte("hello, not a png at all")); ok {
		t.Error("bytes with no PNG signature read as a header")
	}
}

func TestImageHeaderGIF(t *testing.T) {
	gif := func(screenW, screenH int, flags byte, frames ...[5]int) []byte {
		b := []byte{'G', 'I', 'F', '8', '9', 'a',
			byte(screenW), byte(screenW >> 8), byte(screenH), byte(screenH >> 8), flags, 0, 0}
		if flags&0x80 != 0 {
			for i := 0; i < 3*(1<<((flags&7)+1)); i++ {
				b = append(b, byte(i))
			}
		}
		for _, f := range frames {
			b = append(b, 0x2c,
				byte(f[0]), byte(f[0]>>8), byte(f[1]), byte(f[1]>>8),
				byte(f[2]), byte(f[2]>>8), byte(f[3]), byte(f[3]>>8), byte(f[4]))
			if f[4]&0x80 != 0 {
				for i := 0; i < 3*(1<<((f[4]&7)+1)); i++ {
					b = append(b, byte(i))
				}
			}
			b = append(b, 8, 1, 0, 0) // an LZW minimum, one sub-block, terminated
		}
		return append(b, 0x3b)
	}
	h, ok := readImageHeader(gif(4, 3, 0))
	if !ok || h.Type != "gif" || h.Width != 4 || h.Height != 3 {
		t.Errorf("plain 4x3 gif = %+v, %v", h, ok)
	}
	// A frame hanging off the screen grows the canvas a browser draws onto.
	h, ok = readImageHeader(gif(10, 10, 0, [5]int{5, 5, 20, 20, 0}))
	if !ok || h.Width != 25 || h.Height != 25 {
		t.Errorf("frame-growing gif = %+v, %v", h, ok)
	}
	// A global colour table is bytes the scan blocks sit behind.
	h, ok = readImageHeader(gif(4, 3, 0x82, [5]int{}))
	if !ok || h.Width != 4 || h.Height != 3 {
		t.Errorf("gif with a colour table = %+v, %v", h, ok)
	}
	if _, ok := readImageHeader([]byte("GIF89a\x04\x00\x03\x00\x00")); ok {
		t.Error("a gif with no trailer read as a header")
	}
}

func TestImageHeaderJPEG(t *testing.T) {
	sof := func(marker byte, w, h int) []byte {
		return []byte{0xff, 0xd8, 0xff, 0xe0, 0x00, 0x08, 'J', 'F', 'I', 'F', 0, 0,
			0xff, marker, 0x00, 0x0b, 8, byte(h >> 8), byte(h), byte(w >> 8), byte(w), 1, 0x11, 0, 0}
	}
	hh, ok := readImageHeader(sof(0xc0, 5, 7))
	if !ok || hh.Type != "jpeg" || hh.Width != 5 || hh.Height != 7 {
		t.Errorf("baseline jpeg = %+v, %v", hh, ok)
	}
	if _, ok := readImageHeader(sof(0xc4, 5, 7)); ok {
		t.Error("a DHT read as a frame header")
	}
	scanFirst := append([]byte{0xff, 0xd8, 0xff, 0xda, 0x00, 0x04, 1, 2}, sof(0xc0, 5, 7)[2:]...)
	if _, ok := readImageHeader(scanFirst); ok {
		t.Error("a jpeg whose scan came before its frame header read as a header")
	}
	if _, ok := readImageHeader([]byte{0xff, 0xd8, 0xff}); ok {
		t.Error("a jpeg cut at its first marker read as a header")
	}
}

func TestImageHeaderWebP(t *testing.T) {
	pad := func(b []byte, to int) []byte {
		for len(b) < to {
			b = append(b, 0)
		}
		return b
	}
	extended := pad([]byte{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P',
		'V', 'P', '8', 'X', 10, 0, 0, 0, 0, 0, 0, 0,
		0, 0x04, 0, 0x1e, 0, 0}, 32) // extents 1024 × 30 → 1025 × 31
	if hh, ok := readImageHeader(extended); !ok || hh.Type != "webp" || hh.Width != 1025 || hh.Height != 31 {
		t.Errorf("VP8X 1024×30 extents = %+v, %v; want 1025×31", hh, ok)
	}
	// lossless: one 32-bit word packing w-1 and h-1 after the 0x2f signature
	lossless := pad([]byte{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P',
		'V', 'P', '8', 'L', 4, 0, 0, 0, 0x2f, 0x05, 0x80, 0, 0}, 28)
	if hh, ok := readImageHeader(lossless); !ok || hh.Width != 6 || hh.Height != 3 {
		t.Errorf("VP8L 6×3 = %+v, %v", hh, ok)
	}
	// lossy: the 3 sync bytes, then u16le width and height masked to 14 bits
	lossy := pad([]byte{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P',
		'V', 'P', '8', ' ', 0, 0, 0, 0, 0, 0, 0, 0x9d, 0x01, 0x2a, 0x0a, 0, 0x05, 0}, 32)
	if hh, ok := readImageHeader(lossy); !ok || hh.Width != 10 || hh.Height != 5 {
		t.Errorf("VP8  10×5 = %+v, %v", hh, ok)
	}
	unknown := pad([]byte{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P',
		'A', 'N', 'I', 'M', 0, 0, 0, 0}, 24)
	if _, ok := readImageHeader(unknown); ok {
		t.Error("a WEBP whose first chunk is ANIM read as a header")
	}
	if _, ok := readImageHeader([]byte("RIFF\x10\x00\x00\x00AVI ")); ok {
		t.Error("a RIFF that is not a WEBP read as a header")
	}
}

func TestImageHeaderAnythingElse(t *testing.T) {
	for _, b := range [][]byte{
		{}, nil, {0x00}, []byte("a"), []byte("<svg xmlns='http://www.w3.org/2000/svg'/>"),
		{0xff, 0xd8, 0xff, 0xdb, 0x00, 0x02},
		pngArt(9, 9, 1)[:8],
	} {
		if _, ok := readImageHeader(b); ok {
			t.Errorf("readImageHeader(%q) read as a header", b)
		}
	}
}

// --- the art rules ---------------------------------------------------------------------

func TestArtRefusalSentences(t *testing.T) {
	size := func(w, h int) string {
		return fmt.Sprintf("an image in it is %d × %d pixels, and a design's pictures can be at most %d × %d", w, h, 1024, 1024)
	}
	cases := []struct {
		prefix string
		b      []byte
		want   string
	}{
		{"data:image/png;base64", pngArt(9, 9, 1), ""},
		{"data:image/png;base64", pngArt(1024, 1024, 2), ""},
		{"data:image/png;base64", pngArt(1025, 1024, 3), size(1025, 1024)},
		{"data:image/png;base64", pngArt(1024, 1, 4), ""},
		{"data:image/png;base64", pngArt(1, 1, 11), ""},                       // a 1 × 1 picture is a picture
		{"data:image/png;base64", mustArt(t, designArt(1)), ""},               // a real 1 × 1 PNG, CRCs and all, off the fixture helper
		{"data:image/jpeg;base64", []byte("no header"), artRefusalUnreadable}, // 9 bytes: under the floor (fu2-2)
		{"data:image/gif;base64", []byte{1, 2, 3}, artRefusalUnreadable},      // 3 bytes: under the floor
		{"data:image/png;base64", []byte("0123456789"), artRefusalUnreadable}, // exactly the floor, and no header in it
		{"data:image/png;base64", []byte("not an image at all, and long enough to check"), artRefusalUnreadable},
		{"data:image/svg+xml;base64", pngArt(9, 9, 5), svgRefusalNotText}, // binary bytes fail the text rule before the root check
		{"", pngArt(9, 9, 6), artRefusalType},
		{"DATA:IMAGE/PNG;BASE64", pngArt(9, 9, 7), ""},
	}
	for _, c := range cases {
		if got := artRefusal(c.prefix, c.b); got != c.want {
			t.Errorf("artRefusal(%q, %d bytes) = %q, want %q", c.prefix, len(c.b), got, c.want)
		}
	}
}

// TestArtPrefixRefusalSentences: a reused art ref — a data URL with no bytes in hand —
// is judged by its prefix alone and never for length (fu2-2, ruling 35 as amended).
func TestArtPrefixRefusalSentences(t *testing.T) {
	for _, prefix := range append(append([]string{}, artPrefixes...), svgArtPrefix, "DATA:IMAGE/SVG+XML;BASE64") {
		if got := artPrefixRefusal(prefix); got != "" {
			t.Errorf("artPrefixRefusal(%q) = %q, want kept", prefix, got)
		}
	}
	// the old artRefusal(_, nil) rows, which were the prefix-only pass, live here now
	if got := artPrefixRefusal(svgArtPrefix); got != "" {
		t.Errorf("nil SVG = %q, want the bytes checked next, not the prefix refused", got)
	}
	if got := artPrefixRefusal("data:image/png;base64"); got != "" {
		t.Errorf("nil PNG = %q, want a ref never refused for length", got)
	}
	for _, prefix := range []string{"", "data:image/bmp;base64", "data:text/html;base64", "DATA:IMAGE_PNG;BASE64"} {
		if got := artPrefixRefusal(prefix); got != artRefusalType {
			t.Errorf("artPrefixRefusal(%q) = %q, want the type sentence", prefix, got)
		}
	}
}

// mustArt decodes a fixture's base64 body for a row that wants the real bytes.
func mustArt(t *testing.T, s string) []byte {
	t.Helper()
	b, err := base64.StdEncoding.DecodeString(s)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// --- fu2-1: the SVG rules, checked by parsing ------------------------------------------
//
// The vectors live in one file, testdata/svg-vectors.json, which the studio's own selftest
// (_selftest/svg-vectors.js) and the site's svg-art tests read too: one rule set, held to
// one answer per input. A vector is {name, svg, rule} and the rule is null for an input all
// three accept; a body that is not valid UTF-8 travels as {name, svg_base64, rule} instead,
// since a JSON string cannot hold those bytes. The size ceiling is the one rule no entry in
// the file can ask, so TestCheckSvgArtSize builds that body from bytes here.

// svgVector is one line of the shared file. The two body fields are pointers because the
// file distinguishes a vector whose body is the empty string from one that carries no svg
// field at all, and an empty body is one of the inputs the rules have an answer for.
type svgVector struct {
	Name  string  `json:"name"`
	Svg   *string `json:"svg"`
	Svg64 *string `json:"svg_base64"`
	Want  *string `json:"rule"`
}

func (v svgVector) rule() string {
	if v.Want == nil {
		return ""
	}
	return *v.Want
}

// bytes is the vector's body, decoded when it arrived as base64.
func (v svgVector) bytes() []byte {
	if v.Svg64 != nil {
		b, err := base64.StdEncoding.DecodeString(*v.Svg64)
		if err != nil {
			return nil
		}
		return b
	}
	if v.Svg != nil {
		return []byte(*v.Svg)
	}
	return nil
}

// svgVectorsFile is the shared set, read off disk.
func svgVectorsFile(t *testing.T) []svgVector {
	t.Helper()
	b, err := os.ReadFile("testdata/svg-vectors.json")
	if err != nil {
		t.Fatalf("the shared SVG vectors are not there to read: %v", err)
	}
	var vectors []svgVector
	if err := json.Unmarshal(b, &vectors); err != nil {
		t.Fatalf("the shared SVG vector file does not parse: %v", err)
	}
	return vectors
}

// TestSvgVectorsFile is the shared set against the hub's checker: one line per vector, and
// the failure names the vector, the rule expected and the rule given.
func TestSvgVectorsFile(t *testing.T) {
	vectors := svgVectorsFile(t)
	if len(vectors) < 70 {
		t.Errorf("the shared vector file holds %d vectors; it is meant to hold at least 70", len(vectors))
	}
	for _, v := range vectors {
		if v.Svg == nil && v.Svg64 == nil {
			t.Errorf("%s: carries neither an svg nor an svg_base64 body", v.Name)
			continue
		}
		body := v.bytes()
		if body == nil && v.Svg64 != nil {
			t.Errorf("%s: its svg_base64 body is not base64 at all", v.Name)
			continue
		}
		rule, sentence := checkSvgArtRule(body)
		if rule != v.rule() {
			t.Errorf("%s: rule = %q, want %q (sentence %q)", v.Name, rule, v.rule(), sentence)
			continue
		}
		if want := svgRuleSentence(rule); sentence != want {
			t.Errorf("%s: sentence = %q, want the hub's wording %q", v.Name, sentence, want)
		}
	}
}

// TestSvgVectorsFileShape is the shape of the shared set: the names are the ids every runner
// prints, one of the two body fields is filled, and every rule but the size ceiling — the one
// a file entry cannot carry — is asked at least once.
func TestSvgVectorsFileShape(t *testing.T) {
	seen := map[string]int{}
	names := map[string]bool{}
	for _, v := range svgVectorsFile(t) {
		if names[v.Name] {
			t.Errorf("two vectors are named %s; the runners report by name", v.Name)
		}
		names[v.Name] = true
		if v.Name == "" || (v.Svg == nil) == (v.Svg64 == nil) {
			t.Errorf("a vector does not name its body once and only once: %+v", v)
		}
		switch v.rule() {
		case "", svgRuleTooBig, svgRuleNotUTF8, svgRuleNotWellFormed, svgRuleMarkup, svgRuleRoot,
			svgRuleElement, svgRuleForeignElement, svgRuleEventAttr, svgRuleLink, svgRuleCSS:
			seen[v.rule()]++
		default:
			t.Errorf("%s: %q is not one of the rule ids", v.Name, v.rule())
		}
	}
	if n := seen[svgRuleTooBig]; n != 0 {
		t.Errorf("%d vectors ask rule 1; the size ceiling is not something a string in a file can be", n)
	}
	for _, rule := range []string{svgRuleNotUTF8, svgRuleNotWellFormed, svgRuleMarkup, svgRuleRoot,
		svgRuleElement, svgRuleForeignElement, svgRuleEventAttr, svgRuleLink, svgRuleCSS, ""} {
		if seen[rule] == 0 {
			t.Errorf("no vector asks rule %q", rule)
		}
	}
}

// the two refusals the door's cases name, as the rule set words them now
var (
	svgRefusalHandler = svgRuleSentence(svgRuleEventAttr)
	svgRefusalLink    = svgRuleSentence(svgRuleLink)
)

func TestCheckSvgArtSize(t *testing.T) {
	pad := func(n int) []byte {
		b := []byte(`<svg xmlns="http://www.w3.org/2000/svg">`)
		for len(b) < n-6 {
			b = append(b, ' ')
		}
		return append(b, []byte("</svg>")...)
	}
	if rule, _ := checkSvgArtRule(pad(svgMaxBytes)); rule != "" {
		t.Errorf("an SVG of exactly %d bytes refused: rule %q", svgMaxBytes, rule)
	}
	if rule, got := checkSvgArtRule(pad(svgMaxBytes + 1)); rule != svgRuleTooBig ||
		got != fmt.Sprintf(svgRefusalSizeFormat, 257) {
		t.Errorf("checkSvgArt(256 KB + 1 byte) = %q/%q, want %q/%q", rule, got, svgRuleTooBig,
			fmt.Sprintf(svgRefusalSizeFormat, 257))
	}
	if rule, got := checkSvgArtRule(pad(3 * 1024 * 1024)); rule != svgRuleTooBig ||
		got != fmt.Sprintf(svgRefusalSizeFormat, 3072) {
		t.Errorf("a 3 MB SVG = %q/%q, want svg_too_big/3072 KB", rule, got)
	}
}

func TestCheckSvgArtBytes(t *testing.T) {
	// Rule 2 is about the bytes, so it is asked before anything is parsed: a body that is
	// not UTF-8, and one with a NUL in it, both read as the text refusal. A leading BOM is
	// allowed and belongs to no markup, and a literal U+FFFD is a character like any other.
	cases := []struct {
		name string
		b    []byte
		rule string
	}{
		{"not utf-8", []byte(svgArtHead + "\xff</svg>"), svgRuleNotUTF8},
		{"lone continuation byte", []byte(svgArtHead + "\xbc</svg>"), svgRuleNotUTF8},
		{"nul byte", []byte(svgArtHead + "\x00</svg>"), svgRuleNotUTF8},
		{"empty", nil, svgRuleNotWellFormed},
		{"bom and a file", append([]byte("\ufeff"), []byte(validSvgArt)...), ""},
		{"ufffd is a character", []byte(strings.Replace(validSvgArt, "<rect", "<!-- \ufffd --><rect", 1)), ""},
	}
	for _, c := range cases {
		rule, sentence := checkSvgArtRule(c.b)
		if rule != c.rule {
			t.Errorf("%s: rule = %q, want %q (sentence %q)", c.name, rule, c.rule, sentence)
		}
	}
	if got := checkSvgArt([]byte(svgArtHead + "\xff</svg>")); got != svgRefusalNotText {
		t.Errorf("a body that is not UTF-8 = %q, want the text sentence", got)
	}
}

// validSvgArt is the smallest file a design may hold, and svgArtHead the tag it opens with.
const (
	validSvgArt = `<svg xmlns="http://www.w3.org/2000/svg"><rect width="2" height="2"/></svg>`
	svgArtHead  = `<svg xmlns="http://www.w3.org/2000/svg">`
)

func TestCheckSvgArtRefusedBeforeParsed(t *testing.T) {
	// Weight first: a body over the ceiling never gets asked whether it holds a handler, so
	// the heaviest file gets the one sentence that describes it.
	text := `<svg xmlns="http://www.w3.org/2000/svg" onload="x">` + strings.Repeat(" ", svgMaxBytes) + `</svg>`
	if rule, _ := checkSvgArtRule([]byte(text)); rule != svgRuleTooBig {
		t.Errorf("an oversized SVG with a handler = rule %q, want %q", rule, svgRuleTooBig)
	}
	if got := checkSvgArt([]byte(text)); got != fmt.Sprintf(svgRefusalSizeFormat, (len(text)+1023)/1024) {
		t.Errorf("an oversized SVG with a handler = %q, want the size sentence", got)
	}
}

// TestSvgArtFilesInApps is the rule set against the art the shelf actually carries: every
// .svg under apps/ is a picture a design holds today, so every one of them has to answer
// the parse the way a picture must.
func TestSvgArtFilesInApps(t *testing.T) {
	var n, bad int
	err := filepath.WalkDir("../../apps", func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() || !strings.HasSuffix(path, ".svg") {
			return nil
		}
		b, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		n++
		if rule, sentence := checkSvgArtRule(b); rule != "" {
			bad++
			t.Errorf("%s: rule %q (%s)", path, rule, sentence)
		}
		return nil
	})
	if err != nil {
		t.Fatalf("apps/ could not be walked: %v", err)
	}
	if n == 0 {
		t.Fatal("no SVG art under apps/ to check")
	}
	if bad != 0 {
		t.Errorf("%d of %d of the shelf's SVGs are refused by the rules", bad, n)
	}
}

// TestSvgRuleSentences keeps the hub's wording pinned. The rule ids are what the shared
// vectors assert; these are the sentences a design's owner reads, four of them the ones
// S3b wrote and the rest the holds sentence worded the same way.
func TestSvgRuleSentences(t *testing.T) {
	holds := func(what string) string { return fmt.Sprintf(svgRefusalHoldsFormat, what) }
	cases := []struct{ rule, want string }{
		{"", ""},
		{svgRuleTooBig, ""}, // worded with the file's own weight by its caller
		{svgRuleNotUTF8, svgRefusalNotText},
		{svgRuleRoot, svgRefusalNotSvg},
		{svgRuleNotWellFormed, holds("XML that does not parse")},
		{svgRuleMarkup, holds("a DOCTYPE, a CDATA section, a processing instruction or a comment a design's SVGs can't hold")},
		{svgRuleElement, holds("an element a design's SVGs can't hold")},
		{svgRuleForeignElement, holds("an element from another namespace")},
		{svgRuleEventAttr, svgRefusalHandler},
		{svgRuleLink, svgRefusalLink},
		{svgRuleCSS, holds("CSS that can reach outside the file")},
	}
	for _, c := range cases {
		if got := svgRuleSentence(c.rule); got != c.want {
			t.Errorf("svgRuleSentence(%q) = %q, want %q", c.rule, got, c.want)
		}
	}
}

// TestSvgScanMarkup is the rule-4 walk on its own, since it is the one part of the check the
// parser cannot be asked about the same way in all three checkers: a CDATA section looks
// like text to encoding/xml, and a bogus declaration looks like a DOCTYPE.
func TestSvgScanMarkup(t *testing.T) {
	cases := []struct {
		name, text, want string
	}{
		{"clean", validSvgArt, ""},
		{"leading decl", `<?xml version="1.0"?>` + validSvgArt, ""},
		{"clean comment", strings.Replace(validSvgArt, svgArtHead, svgArtHead+"<!-- drawn by hand -->", 1), ""},
		{"comment holding a tag", strings.Replace(validSvgArt, svgArtHead, svgArtHead+`<!-- <image href="//x"> -->`, 1), svgRuleMarkup},
		{"comment holding --", strings.Replace(validSvgArt, svgArtHead, svgArtHead+"<!-- a--b -->", 1), svgRuleNotWellFormed},
		{"unterminated comment", validSvgArt + "<!--", svgRuleNotWellFormed},
		{"cdata", strings.Replace(validSvgArt, svgArtHead, svgArtHead+"<![CDATA[x]]>", 1), svgRuleMarkup},
		{"unclosed cdata", strings.Replace(validSvgArt, svgArtHead, svgArtHead+"<![CDATA[x]", 1), svgRuleNotWellFormed},
		{"doctype", "<!DOCTYPE svg>" + validSvgArt, svgRuleMarkup},
		{"doctype with a subset", `<!DOCTYPE svg [<!ENTITY a "aa">]>` + validSvgArt, svgRuleMarkup},
		{"unclosed doctype", `<!DOCTYPE svg [<!ENTITY a "aa">` + validSvgArt, svgRuleNotWellFormed},
		{"bogus declaration", strings.Replace(validSvgArt, svgArtHead, svgArtHead+"<!x>", 1), svgRuleNotWellFormed},
		{"stylesheet instruction", `<?xml-stylesheet href="a.css"?>` + validSvgArt, svgRuleMarkup},
		{"second declaration", `<?xml version="1.0"?><?xml version="1.0"?>` + validSvgArt, svgRuleMarkup},
		{"instruction after the root", strings.Replace(validSvgArt, svgArtHead, svgArtHead+`<?php ?>`, 1), svgRuleMarkup},
		{"a > inside a value", strings.Replace(validSvgArt, `width="2"`, `aria-label="a>b" width="2"`, 1), ""},
		{"a < inside a value", strings.Replace(validSvgArt, `width="2"`, `aria-label="a<b" width="2"`, 1), svgRuleNotWellFormed},
		{"a declaration inside a value", strings.Replace(validSvgArt, `width="2"`, `aria-label="<!DOCTYPE svg>" width="2"`, 1), svgRuleNotWellFormed},
		{"unclosed tag", `<svg xmlns="http://www.w3.org/2000/svg"><rect`, svgRuleNotWellFormed},
		{"bom", "\ufeff" + validSvgArt, ""},
	}
	for _, c := range cases {
		if got := svgScanMarkup(c.text); got != c.want {
			t.Errorf("%s: svgScanMarkup = %q, want %q", c.name, got, c.want)
		}
	}
}

// TestSvgScanTag is the quoted-value half of that walk, which is what keeps the review's
// d4 vector a CSS refusal rather than an escape out of a tag.
func TestSvgScanTag(t *testing.T) {
	whole := func(text string) bool {
		n, ok := svgScanTag(text)
		return ok && n == len(text)
	}
	for _, text := range []string{
		`<rect width="2" height="2"/>`,
		`<rect width="2" aria-label="a>b"/>`, // the > is inside a value, not the tag's end
		`<image href='a'/>`,
	} {
		if !whole(text) {
			t.Errorf("svgScanTag(%q) did not read the tag to its end", text)
		}
	}
	for _, text := range []string{
		`<rect width=2/>`,             // an unquoted value is not XML
		`<rect width="a<b"/>`,         // a literal < in a value has to be &lt;
		`<rect width="a" width="b"/>`, // one attribute name, twice
		`<rect width="a`,              // a value that never closes
		`< rect/>`,                    // no element name
		`<rect width="a"b="c"/>`,      // an attribute straight after a value
		`<rect/`,                      // a tag that never closes
	} {
		if _, ok := svgScanTag(text); ok {
			t.Errorf("svgScanTag(%q) read a tag a strict parser stops at", text)
		}
	}
}

// --- the door -------------------------------------------------------------------------

// The whole point of the host gate is that a refused link costs the hub no request: an
// unknown host learns neither that this owner imports from orb-os.net nor which design
// they were after. So the refusal sentence must be the gate's own, and the test site —
// stood up here even though the link never names it — must record nothing.
func TestSiteImportRefusesAnotherHostBeforeAnyFetch(t *testing.T) {
	t.Setenv("GORB_SITE", "")
	s, _, _ := libHub(t)
	srv, asks := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		t.Errorf("the hub dialed a host the gate refuses: %s", r.URL.Path)
	})
	_ = srv
	// Any request at all, to any host, goes through this client and is counted: the gate is
	// before the fetch, so it must stay at zero (no dial, no DNS).
	var dials int
	s.siteClient = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		dials++
		return nil, errors.New("dialled " + r.URL.Host)
	})}
	for _, link := range []string{
		"https://evil.example/t/" + siteDesignID,
		"https://orb-os.net.evil.example/t/" + siteDesignID,
		"https://api.orb-os.net/t/" + siteDesignID,
		"https://www.orb-os.net.evil.example/t/" + siteDesignID,
		"https://not-the-configured-site.test:8443/t/" + siteDesignID,
	} {
		got := importErr(t, s, link)
		want := "Orb Manager imports themes only from orb.takacsfamily.com and orb-os.net/www.orb-os.net, not from " +
			siteHost(strings.TrimSuffix(link[:strings.Index(link, "/t/")], "https://"))
		if got != want {
			t.Errorf("import of %q: error = %q, want %q", link, got, want)
		}
	}
	if n := len(*asks); n != 0 {
		t.Errorf("a refused paste made %d requests to the test site", n)
	}
	if dials != 0 {
		t.Errorf("a refused paste went through the site client %d times", dials)
	}
	if n := len(s.tickets().live); n != 0 {
		t.Errorf("a refused paste was held anyway: %d tickets live", n)
	}
}

// Art outside ruling 35's bounds is not damage — the bundle is exactly what its hashes
// say — so it comes back under its own sentence, with the site's own reason. Sidecar
// art and inline art clear the same bar.
func TestSiteImportRefusesArtOutsideTheLimits(t *testing.T) {
	s, _, _ := libHub(t)
	var doc map[string]any
	srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		b, err := json.Marshal(doc)
		if err != nil {
			t.Error(err)
		}
		_, _ = w.Write(b)
	})
	set := func(d map[string]any, a map[string]string) {
		doc = map[string]any{
			"format": "orb-design-bundle", "bundle": 1, "id": siteDesignID, "version": 3,
			"app": "clock", "sharedName": "Skeleton by @greg", "name": "Skeleton by @greg",
			"handle": "greg", "design": d, "assets": a,
		}
	}
	size := func(w, h int) string {
		return fmt.Sprintf("an image in it is %d × %d pixels, and a design's pictures can be at most %d × %d", w, h, 1024, 1024)
	}
	// sidecar serves one row over one body, hashed honestly: the bundle is whole in
	// every case here, and only its art is out of bounds.
	sidecar := func(row map[string]any, body []byte) {
		sumB := sha256.Sum256(body)
		sha := hex.EncodeToString(sumB[:])
		row["sha256"] = sha
		set(map[string]any{"format": "wc-design", "orbAssets": []any{row}},
			map[string]string{sha: base64.StdEncoding.EncodeToString(body)})
	}
	sidecar(map[string]any{"prefix": "data:image/png;base64"}, pngArt(2000, 17, 7))
	if got, want := importErr(t, s, siteTestLink(srv, siteDesignID)), size(2000, 17); !strings.HasSuffix(got, want) ||
		!strings.HasPrefix(got, "the theme from "+siteHostOf(srv)+" can't be imported: ") {
		t.Errorf("oversized sidecar art: error = %q, want the size sentence", got)
	}
	sidecar(map[string]any{"prefix": "data:image/png;base64"}, []byte("not an image at all, and long enough"))
	if got := importErr(t, s, siteTestLink(srv, siteDesignID)); !strings.HasSuffix(got, artRefusalUnreadable) {
		t.Errorf("unreadable sidecar art: error = %q", got)
	}
	sidecar(map[string]any{"mimetype": "image/png"}, pngArt(4, 4, 1))
	if got := importErr(t, s, siteTestLink(srv, siteDesignID)); !strings.HasSuffix(got, artRefusalType) {
		t.Errorf("sidecar row with no prefix: error = %q", got)
	}
	set(map[string]any{"format": "wc-design", "icon": "data:image/svg+xml,<svg/>"}, nil)
	if got := importErr(t, s, siteTestLink(srv, siteDesignID)); !strings.HasSuffix(got, artRefusalType) {
		t.Errorf("inline svg: error = %q", got)
	}
	set(map[string]any{"format": "wc-design", "icon": "data:image/png;base64,not base64 !!"}, nil)
	if got := importErr(t, s, siteTestLink(srv, siteDesignID)); !strings.HasSuffix(got, artRefusalUnreadable) {
		t.Errorf("inline art that is not base64: error = %q", got)
	}
	set(map[string]any{"format": "wc-design",
		"parts": []any{map[string]any{"fill": "data:image/png;base64," +
			base64.StdEncoding.EncodeToString(pngArt(4000, 4000, 9))}}}, nil)
	if got, want := importErr(t, s, siteTestLink(srv, siteDesignID)), size(4000, 4000); !strings.HasSuffix(got, want) {
		t.Errorf("inline oversized art: error = %q, want the size sentence", got)
	}
}

// A design may fill an orb with an svg and it is none of the import's business — until
// it is shaped like a data URL, which is where the site draws its own line.
func TestSiteImportPassesArtItHasNoOpinionAbout(t *testing.T) {
	s, _, _ := libHub(t)
	srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write(bundleOf(t, map[string]any{
			"format": "orb-design-bundle", "bundle": 1, "id": siteDesignID, "version": 3,
			"app": "clock", "sharedName": "Skeleton by @greg", "name": "Skeleton by @greg",
			"handle": "greg",
			"design": map[string]any{"format": "wc-design", "icon": "none",
				"body": "<svg xmlns='http://www.w3.org/2000/svg'/>"},
			"assets": nil,
		}))
	})
	tk := ticketOf(t, siteImport(t, s, siteTestLink(srv, siteDesignID)))
	if tk == "" {
		t.Fatal("no ticket")
	}
}

// The bound is exactly the bound: a picture naming 1024 × 1024 imports.
func TestSiteImportKeepsArtExactlyAtTheBound(t *testing.T) {
	s, _, _ := libHub(t)
	srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write(goodBundle(t, siteDesignID, 3, "clock", "Skeleton by @greg", pngArt(1024, 1024, 3)))
	})
	ticketOf(t, siteImport(t, s, siteTestLink(srv, siteDesignID)))
}

// The update check asks the same gate the import does: an origin sidecar naming another
// host is refused before its socket opens, in the import's own sentence. fu2-3 (ruling 35,
// as amended): a subdomain of the site's own domain is that host, www or not.
func TestResolveOriginRefusesAnUnknownSite(t *testing.T) {
	t.Setenv("GORB_SITE", "")
	s, _, _ := libHub(t)
	srv, asks := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		t.Errorf("the update check dialed a host the gate refuses: %s", r.URL.Path)
	})
	_ = srv
	for _, refused := range []string{"https://evil.example", "https://api.orb-os.net", "https://orb-os.net.evil.example"} {
		_, err := s.resolveOrigin(t.Context(), &ThemeOrigin{
			Site: refused, DesignID: siteDesignID, Version: 3,
		})
		want := "Orb Manager imports themes only from orb.takacsfamily.com and orb-os.net/www.orb-os.net, not from " + siteHost(refused)
		if err == nil || err.Error() != want {
			t.Fatalf("resolveOrigin(%q) error = %v, want %q", refused, err, want)
		}
	}
	if n := len(*asks); n != 0 {
		t.Errorf("a refused origin made %d requests to the test site", n)
	}
}

// roundTripFunc lets a test stand in for the network with a function.
type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

// --- S3b: the name rules ---------------------------------------------------------------

func TestCheckThemeNameBasicCharacters(t *testing.T) {
	for _, n := range []string{
		"Grand Prix", "grand-prix", "Grand_Prix.2", "a", "7", "Sub Dial 01",
		"Grand Prix by @greg", "a by @abc", "z by @" + strings.Repeat("x", 24),
	} {
		got, err := checkThemeName(n)
		if err != nil || got != n {
			t.Errorf("checkThemeName(%q) = %q, %v; want kept as written", n, got, err)
		}
	}
}

func TestCheckThemeNameRefusesBeyondTheBasics(t *testing.T) {
	for _, n := range []string{
		"Big—Sub", "Grüezi", "dial one", "a/b", "sub#", "a@b", "zero​width", "big<sub>",
		"tab\there", "newline\nhere", "Grand Prix by @Greg", "Grand Prix by @g", "Grand Prix by @greg!",
		" by @greg", "Grand Prix by @", "Grand Prix by @@greg",
	} {
		if got, err := checkThemeName(n); err == nil {
			t.Errorf("checkThemeName(%q) = %q, %v; want a refusal", n, got, err)
		}
	}
}

func TestCheckThemeNameTrimsAndRefusesEmpty(t *testing.T) {
	if got, err := checkThemeName("  Grand Prix  "); err != nil || got != "Grand Prix" {
		t.Errorf("checkThemeName(±space) = %q, %v; want %q", got, err, "Grand Prix")
	}
	if _, err := checkThemeName("   "); err == nil || err.Error() != "a design needs a name" {
		t.Errorf("empty name = %v; want the empty sentence", err)
	}
	if _, err := checkThemeName("Big—Sub"); err == nil || err.Error() != themeNameRefusalChars {
		t.Errorf("checkThemeName(Big—Sub) = %v; want the character sentence", err)
	}
}

func TestCheckSharedNameSplitsAtTheFirstSeparator(t *testing.T) {
	name, handle, sentence := checkSharedName("Grand Prix by @greg")
	if sentence != "" || name != "Grand Prix" || handle != "greg" {
		t.Errorf(`checkSharedName("Grand Prix by @greg") = %q, %q, %q`, name, handle, sentence)
	}
	if _, _, s := checkSharedName(" by @greg"); s == "" {
		t.Error("a shared name with no theme part was accepted")
	}
	// A bundle for a design nobody shared serves its plain name as its shared name, and
	// that is what the update flow's fixtures carry: the character rule reads it, and the
	// handle half stays empty.
	if n, h, s := checkSharedName("Grand Prix"); s != "" || n != "Grand Prix" || h != "" {
		t.Errorf(`checkSharedName("Grand Prix") = %q, %q, %q; want the name and no handle`, n, h, s)
	}
	if _, _, s := checkSharedName("Grand—Prix by @greg"); s != themeNameRefusalChars {
		t.Errorf("a shared name whose theme part is not basic = %q; want the character sentence", s)
	}
	if _, _, s := checkSharedName("Grand Prix by @Greg"); s == "" || s == themeNameRefusalChars {
		t.Errorf("a shared name whose handle is not the site's shape = %q; want the handle sentence", s)
	}
	// A name part may itself hold the letters of the separator, as long as the first one
	// splits into a handle the site would sign: " by @" inside a theme name is not basic.
	if _, _, s := checkSharedName("by @ by @greg"); s == "" {
		t.Error("a handle holding a space was accepted")
	}
}

func TestCheckSharedNameRefusalIsNotDamage(t *testing.T) {
	// A bundle with a name the hub cannot file is whole, not damaged: checkBundle answers
	// with the same error kind bad art answers with, so the owner hears "can't be imported".
	srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(goodBundle(t, siteDesignID, 2, "clock", "Big—Sub by @greg"))
	})
	s, _, _ := libHub(t)
	got := importErr(t, s, siteTestLink(srv, siteDesignID))
	if !strings.HasPrefix(got, "the theme from "+siteHostOf(srv)+" can't be imported: ") {
		t.Errorf("shared-name refusal = %q; want the can't-be-imported wrapper", got)
	}
	if !strings.HasSuffix(got, themeNameRefusalChars) {
		t.Errorf("shared-name refusal = %q; want it to end with the character sentence", got)
	}
}
