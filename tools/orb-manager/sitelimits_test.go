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
	"net/http"
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
	}
	for _, c := range cases {
		t.Setenv("GORB_SITE", c.env)
		if got := siteAllowed(c.site); got != c.want {
			t.Errorf("GORB_SITE=%q siteAllowed(%q) = %v, want %v", c.env, c.site, got, c.want)
		}
	}
}

func TestSiteAllowedRefusalSentences(t *testing.T) {
	t.Run("the default site names itself and orb-os.net", func(t *testing.T) {
		t.Setenv("GORB_SITE", "")
		want := "Orb Manager imports themes only from orb.takacsfamily.com and orb-os.net, not from evil.example"
		if got := siteNotAllowed("https://evil.example").Error(); got != want {
			t.Errorf("sentence = %q, want %q", got, want)
		}
	})
	t.Run("a bench site names its own port", func(t *testing.T) {
		t.Setenv("GORB_SITE", "https://bench.example:8443")
		want := "Orb Manager imports themes only from bench.example:8443 and orb-os.net, not from evil.example:9"
		if got := siteNotAllowed("https://evil.example:9").Error(); got != want {
			t.Errorf("sentence = %q, want %q", got, want)
		}
	})
	t.Run("once the site has moved, the sentence names one place", func(t *testing.T) {
		t.Setenv("GORB_SITE", "https://orb-os.net")
		want := "Orb Manager imports themes only from orb-os.net, not from evil.example"
		if got := siteNotAllowed("https://evil.example").Error(); got != want {
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
		{"data:image/jpeg;base64", []byte("no header"), ""},
		{"data:image/gif;base64", []byte{1, 2, 3}, ""},
		{"data:image/png;base64", []byte("not an image at all, and long enough to check"), artRefusalUnreadable},
		{"data:image/svg+xml;base64", pngArt(9, 9, 5), artRefusalType},
		{"data:image/png;base64", nil, ""}, // too short to name a size, whatever it is
		{"", pngArt(9, 9, 6), artRefusalType},
		{"DATA:IMAGE/PNG;BASE64", pngArt(9, 9, 7), ""},
	}
	for _, c := range cases {
		if got := artRefusal(c.prefix, c.b); got != c.want {
			t.Errorf("artRefusal(%q, %d bytes) = %q, want %q", c.prefix, len(c.b), got, c.want)
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
		"https://not-the-configured-site.test:8443/t/" + siteDesignID,
	} {
		got := importErr(t, s, link)
		want := "Orb Manager imports themes only from orb.takacsfamily.com and orb-os.net, not from " +
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
	if got, want := importErr(t, s, siteLink(srv, siteDesignID)), size(2000, 17); !strings.HasSuffix(got, want) ||
		!strings.HasPrefix(got, "the theme from "+siteHostOf(srv)+" can't be imported: ") {
		t.Errorf("oversized sidecar art: error = %q, want the size sentence", got)
	}
	sidecar(map[string]any{"prefix": "data:image/png;base64"}, []byte("not an image at all, and long enough"))
	if got := importErr(t, s, siteLink(srv, siteDesignID)); !strings.HasSuffix(got, artRefusalUnreadable) {
		t.Errorf("unreadable sidecar art: error = %q", got)
	}
	sidecar(map[string]any{"mimetype": "image/png"}, pngArt(4, 4, 1))
	if got := importErr(t, s, siteLink(srv, siteDesignID)); !strings.HasSuffix(got, artRefusalType) {
		t.Errorf("sidecar row with no prefix: error = %q", got)
	}
	set(map[string]any{"format": "wc-design", "icon": "data:image/svg+xml,<svg/>"}, nil)
	if got := importErr(t, s, siteLink(srv, siteDesignID)); !strings.HasSuffix(got, artRefusalType) {
		t.Errorf("inline svg: error = %q", got)
	}
	set(map[string]any{"format": "wc-design", "icon": "data:image/png;base64,not base64 !!"}, nil)
	if got := importErr(t, s, siteLink(srv, siteDesignID)); !strings.HasSuffix(got, artRefusalUnreadable) {
		t.Errorf("inline art that is not base64: error = %q", got)
	}
	set(map[string]any{"format": "wc-design",
		"parts": []any{map[string]any{"fill": "data:image/png;base64," +
			base64.StdEncoding.EncodeToString(pngArt(4000, 4000, 9))}}}, nil)
	if got, want := importErr(t, s, siteLink(srv, siteDesignID)), size(4000, 4000); !strings.HasSuffix(got, want) {
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
	tk := ticketOf(t, siteImport(t, s, siteLink(srv, siteDesignID)))
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
	ticketOf(t, siteImport(t, s, siteLink(srv, siteDesignID)))
}

// The update check asks the same gate the import does: an origin sidecar naming another
// host is refused before its socket opens, in the import's own sentence.
func TestResolveOriginRefusesAnUnknownSite(t *testing.T) {
	t.Setenv("GORB_SITE", "")
	s, _, _ := libHub(t)
	srv, asks := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		t.Errorf("the update check dialed a host the gate refuses: %s", r.URL.Path)
	})
	_ = srv
	_, err := s.resolveOrigin(t.Context(), &ThemeOrigin{
		Site: "https://evil.example", DesignID: siteDesignID, Version: 3,
	})
	want := "Orb Manager imports themes only from orb.takacsfamily.com and orb-os.net, not from evil.example"
	if err == nil || err.Error() != want {
		t.Fatalf("resolveOrigin error = %v, want %q", err, want)
	}
	if n := len(*asks); n != 0 {
		t.Errorf("a refused origin made %d requests to the test site", n)
	}
}

// roundTripFunc lets a test stand in for the network with a function.
type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }
