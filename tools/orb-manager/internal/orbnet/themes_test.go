package orbnet

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// The theme client is the hub's only door to the device theme API, and its whole job
// is to say what the orb said. These pin the routes it dials, the ids it refuses before
// they leave, and that a device refusal arrives as the firmware's own words rather than
// a paraphrase.
//
// The route spellings here are copied from main/net/orb_net.c's uri table and its
// h_theme_* handlers, NOT from this client. When the earlier version of this file was
// written it repeated what themes.go was already sending — ?name= — and so the suite
// watched a client dial a route no orb would answer. Read the expectations below against
// the firmware and expect to be annoyed when they stop matching it.

func fakeOrbServer(t *testing.T, handler http.HandlerFunc) *httptest.Server {
	t.Helper()
	srv := httptest.NewServer(handler)
	t.Cleanup(srv.Close)
	return srv
}

func fakeIP(srv *httptest.Server) string { return strings.TrimPrefix(srv.URL, "http://") }

// The ids here are theme_id(name) as tools/orbtheme.py computes it, so a fixture row's
// id agrees with its own name the way the device insists they must.
const sdReply = `{"mounted":true,"card":"SDSC","uploading":false,"total_mb":7581,` +
	`"free_mb":7100,"source":"slot","active":"aurora","card_count":2,` +
	`"try_id":"f526795c9539","try_why":"art short","staged_id":"","staged_why":"",` +
	`"themes":[{"name":"aurora","id":"9b89025ce7a6","why":"","bytes":123456,"serial":7,"ok":true,"active":true,"changed":false,"stored":true},` +
	`{"name":"broken","id":"f526795c9539","why":"art short","bytes":12,"serial":3,"ok":false,"active":false,"changed":true,"stored":false}]}`

func TestThemeListParsesTheDeviceRowShape(t *testing.T) {
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/v1/sd" || r.Method != http.MethodGet {
			t.Errorf("device saw %s %s, want GET /api/v1/sd", r.Method, r.URL.Path)
		}
		_, _ = w.Write([]byte(sdReply))
	})
	l, err := NewClient(2 * time.Second).ThemeList(fakeIP(srv))
	if err != nil {
		t.Fatalf("ThemeList: %v", err)
	}
	if !l.Mounted || l.Active != "aurora" || l.FreeMB != 7100 {
		t.Errorf("card state lost: %+v", l)
	}
	// The refusal strings key on id, because that is the only handle that names one row
	// for certain: two rows can share a name across a re-bake, never an id.
	if l.TryID != "f526795c9539" || l.TryWhy != "art short" {
		t.Errorf("last refused install lost: try_id=%q try_why=%q", l.TryID, l.TryWhy)
	}
	if len(l.Themes) != 2 {
		t.Fatalf("themes = %d, want 2", len(l.Themes))
	}
	if l.Themes[0].Name != "aurora" || l.Themes[0].ID != "9b89025ce7a6" || !l.Themes[0].OK || !l.Themes[0].Active {
		t.Errorf("first row = %+v, want aurora/9b89025ce7a6 ok active", l.Themes[0])
	}
	if l.Themes[1].OK || l.Themes[1].Why != "art short" {
		t.Errorf("second row = %+v, want the not-loadable row and its reason", l.Themes[1])
	}
	// changed is the row telling you the card holds bytes newer than what is drawn.
	// Dropping the field silently is how an overwritten theme keeps claiming to be the
	// one on the glass.
	if !l.Themes[1].Changed {
		t.Errorf("second row changed = false, want the overwritten flag parsed")
	}
	// stored is Migration 1's row witness: a copy sits in the single flash slot.
	// The active row has one by definition; the broken card-file does not.
	if !l.Themes[0].Stored || l.Themes[1].Stored {
		t.Errorf("stored witnesses = %v/%v, want the active row stored and the card-only row not",
			l.Themes[0].Stored, l.Themes[1].Stored)
	}
	if l.Source != "slot" {
		t.Errorf("source = %q, want the firmware's own word for the drawn slot", l.Source)
	}
}

// An empty list must arrive as an empty slice: the client takes .length on it, and
// null is the shape that once turned a picker into an error message.
func TestThemeListNeverHandsBackNil(t *testing.T) {
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{"mounted":true,"themes":null}`))
	})
	l, err := NewClient(2 * time.Second).ThemeList(fakeIP(srv))
	if err != nil {
		t.Fatal(err)
	}
	raw, _ := json.Marshal(l)
	if !strings.Contains(string(raw), `"themes":[]`) {
		t.Errorf("marshalled list = %s, want themes as []", raw)
	}
}

// The verbs and the query spelling belong to the firmware, not to this client:
// install and delete carry ?id= and nothing else, rescan carries nothing, and
// upload sends raw bytes with a Content-Length and NO name. A drift here is a 400 that
// only shows up on glass — which is exactly the bug this list used to encode.
func TestThemeCallsMatchTheFirmwareRoutes(t *testing.T) {
	var seen []string
	var gotBody, gotCT string
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		seen = append(seen, r.Method+" "+r.URL.RequestURI())
		if r.Method == http.MethodPost && r.URL.Path == "/api/v1/sd/theme" {
			b := make([]byte, r.ContentLength)
			_, _ = r.Body.Read(b)
			gotBody, gotCT = string(b), r.Header.Get("Content-Type")
		}
		_, _ = w.Write([]byte(`{"state":"ok"}`))
	})
	ip := fakeIP(srv)
	c := NewClient(2 * time.Second)
	// An upload has to be a container the device could file, because the client now
	// refuses one it could not. Building it here keeps that gate from being tested with a
	// string that would never occur on disk.
	uploadable := fakeContainer(t, "aurora")
	steps := []func() error{
		func() error { _, err := c.ThemeList(ip); return err },
		func() error { _, err := c.ThemeStore(ip, "clock-aurora-9b89025ce7a6"); return err },
		func() error { _, err := c.ThemeDelete(ip, "weather-x-f893c0d634b3"); return err },
		func() error { _, err := c.ThemeRescan(ip); return err },
		func() error { _, err := c.ThemeUpload(ip, uploadable, false); return err },
		func() error { _, err := c.ThemeUpload(ip, uploadable, true); return err },
	}
	for i, fn := range steps {
		if err := fn(); err != nil {
			t.Fatalf("step %d: %v", i, err)
		}
	}
	want := []string{
		"GET /api/v1/sd",
		"POST /api/v1/sd/bundle?id=clock-aurora-9b89025ce7a6",
		"POST /api/v1/sd/delete?id=weather-x-f893c0d634b3",
		"POST /api/v1/sd/rescan",
		"POST /api/v1/sd/theme",
		"POST /api/v1/sd/theme?overwrite=1",
	}
	if strings.Join(seen, " | ") != strings.Join(want, " | ") {
		t.Errorf("device saw\n  %v\nwant\n  %v", seen, want)
	}
	if gotBody != string(uploadable) {
		t.Errorf("upload body = %d B, want the %d B container verbatim", len(gotBody), len(uploadable))
	}
	if gotCT != "application/octet-stream" {
		t.Errorf("upload Content-Type = %q, want octet-stream", gotCT)
	}
}

// An upload must never carry a name, in either spelling. The firmware files an upload by
// the name inside the container and reads no query name at all, so a client that sends
// one is describing a theme the device will not name that way. This is the assertion the
// old suite's absence let the ?name= contract survive.
func TestThemeUploadCarriesNoName(t *testing.T) {
	var seen []string
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		seen = append(seen, r.URL.RequestURI())
		_, _ = w.Write([]byte(`{"state":"stored","bytes":6}`))
	})
	ip := fakeIP(srv)
	c := NewClient(2 * time.Second)
	for _, ow := range []bool{false, true} {
		if _, err := c.ThemeUpload(ip, fakeContainer(t, "aurora"), ow); err != nil {
			t.Fatalf("upload: %v", err)
		}
	}
	for _, u := range seen {
		if strings.Contains(u, "name") || strings.Contains(u, "slug") || strings.Contains(u, "title") {
			t.Errorf("upload dialled %q; the name comes from the container, never the query", u)
		}
	}
}

// The firmware owns the id rule (orb_theme_id_ok in main/art/orb_theme.c): the compound
// stem "<slug>-<token>-<12 hex>" inside the one FATFS name rule. The client mirrors it so
// a malformed handle never reaches a device whose own first act is to 400 it, and so
// uppercase, a bare hex tail, or traversal never leaves the box. A lone 12-hex id is
// refused here: per-app theming made the slug part of the handle.
func TestThemeIDsAreCheckedBeforeDialing(t *testing.T) {
	dialed := false
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		dialed = true
		_, _ = w.Write([]byte(`{}`))
	})
	ip := fakeIP(srv)
	c := NewClient(2 * time.Second)
	bad := []string{"", " ", "9B89025CE7A6", "9b89025ce7a", "9b89025ce7a6f",
		"gb89025ce7a6", "../etc", "a/b/c/d/e/f", "9b8902 5ce7a6", "aurora",
		"9b89025ce7a6", // the FORMAT 2 era's whole stem: a hex tail names no app now
		"-b89025ce7a6", "_b89025ce7a6",
		"clock-x-9B89025CE7A6",   // uppercase tail
		"clocK-x-a1b2c3d4e5f6",   // uppercase in the slug
		"clock-.x-a1b2c3d4e5f6",  // dot inside the token's leading edge
		"clock-x.y-a1b2c3d4e5f6", // dot inside the token
		"clock--a1b2c3d4e5f6"}    // empty token
	for _, id := range bad {
		if _, err := c.ThemeStore(ip, id); err == nil {
			t.Errorf("store(%q) was accepted", id)
		}
		if _, err := c.ThemeDelete(ip, id); err == nil {
			t.Errorf("delete(%q) was accepted", id)
		}
	}
	for _, id := range []string{"clock-aurora-9b89025ce7a6", "weather-x-000000000000",
		"menu-a-ffffffffffff", "my_app-x-9b89025ce7a6", "clock-Aurora-9b89025ce7a6"} {
		// The last two probe the rule's edges: '_' is a legal slug byte, and the
		// token is decoration — a hand rename keeping the case is still addressable.
		if _, err := c.ThemeStore(ip, id); err != nil {
			t.Errorf("store(%q) was refused: %v", id, err)
		}
	}
	if !dialed {
		t.Error("nothing reached the device; the good ids were refused too")
	}
}

// theme_id() has three implementations that must never disagree: the firmware's, the
// Python packer's (which is the definition) and this one, which is what the hub shows an
// operator. The rows the hub displays are read from the device, so this client is not on
// the install path — but a hub that printed a different id than the card filed the file
// under would send an operator looking at the wrong row. These vectors are the output of
// tools/orbtheme.py's theme_id(), including the boundary cases the host suite cares
// about: UTF-8 multibyte, the 55-byte cap, and case.
func TestThemeIDOfMatchesTheCanonicalPacker(t *testing.T) {
	for _, tc := range []struct {
		name, id string
	}{
		{"aurora", "958c30f62484"},
		{"Aurora", "fede927ce7de"},
		{"a", "45b613c4cf4a"},
		{"Neon Clock", "6c55873ea699"},
		{"Mirage \u2014 Gold", "fe74010a96bc"},
		{"\u00fcnt\u00f6me \u2603", "abbca87ae206"},
		{strings.Repeat("x", 55), "9d42d2fec21b"},
	} {
		got, err := ThemeIDHexOf("clock", tc.name)
		if err != nil {
			t.Errorf("ThemeIDHexOf(\"clock\", %q): %v", tc.name, err)
			continue
		}
		if got != tc.id {
			t.Errorf("ThemeIDHexOf(\"clock\", %q) = %s, want %s (tools/orbtheme.py's theme_id)", tc.name, got, tc.id)
		}
	}
}

// A name is now free text, and the cap is in the unit the header measures: UTF-8 bytes,
// 55 of them, because the 56th is the NUL. FORMAT 1's ASCII-only slug rule refused
// legitimate names, and the failure mode this pins is the character/byte confusion that
// once made the printed card id hash a different string than the header stored.
func TestThemeNamesFollowTheFORMAT2Rule(t *testing.T) {
	good := []string{"aurora", "Mirage \u2014 Gold", "\u00fcnt\u00f6me \u2603",
		"../etc", "a/b", ".hidden", " ", strings.Repeat("x", 55), "A_b-9.0"}
	for _, n := range good {
		if err := CheckThemeName(n); err != nil {
			t.Errorf("CheckThemeName(%q) refused a legal FORMAT-2 name: %v", n, err)
		}
	}
	bad := []string{"", strings.Repeat("x", 56), strings.Repeat("\u00fc", 28), "a\x00b"}
	for _, n := range bad {
		if err := CheckThemeName(n); err == nil {
			t.Errorf("CheckThemeName(%q) was accepted", n)
		}
	}
	// 27 \u00fc is 54 bytes: legal. 28 is 56: not. The boundary is bytes, not characters.
	if err := CheckThemeName(strings.Repeat("\u00fc", 27)); err != nil {
		t.Errorf("27 two-byte characters (54 B) refused: %v", err)
	}
	// A name is not a path: nothing about traversal matters once the card filename is
	// hash hex, so the FORMAT-1 slug rule's path paranoia must not come back as a name
	// rule. This is the assertion that fails if someone "hardens" the name gate again.
	if err := CheckThemeName("../../etc/passwd"); err != nil {
		t.Errorf("a free-text name was refused as if it were a path: %v", err)
	}
}

// A 4x2 plane on the wire: the LE extent, then 8 texels, one carrying a pure-565
// red so the suite can prove the body crossed unexamined rather than re-encoded.
func previewPlane4x2() []byte {
	b := make([]byte, 4+4*2*2)
	b[0] = 4
	b[2] = 2
	b[4] = 0xF8
	return b
}

// The route spelled the way h_sd_preview registers it, and the plane arriving byte
// for byte: the client exists to say what the orb said, and here what the orb said
// is not JSON at all.
func TestThemePreviewDialsTheDeviceRoute(t *testing.T) {
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/v1/sd/preview" || r.Method != http.MethodGet {
			t.Errorf("device saw %s %s, want GET /api/v1/sd/preview", r.Method, r.URL.Path)
		}
		if got := r.URL.Query().Get("id"); got != "clock-aurora-9b89025ce7a6" {
			t.Errorf("query id = %q, want the stem the caller named", got)
		}
		_, _ = w.Write(previewPlane4x2())
	})
	body, code, err := NewClient(2 * time.Second).ThemePreview(fakeIP(srv), "clock-aurora-9b89025ce7a6")
	if err != nil || code != http.StatusOK || len(body) != 20 || body[4] != 0xF8 {
		t.Fatalf("ThemePreview = %d B at %d, err %v", len(body), code, err)
	}
}

// The design's quiet answer (docs/theme-preview-plan.md §1): a theme with no
// preview row is not a failed dial, and a caller that branches on the code must
// be able to. Turning the 404 into an error is how an optional plane becomes a
// red line on a card.
func TestThemePreviewQuietFortyFourIsNotAnError(t *testing.T) {
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNotFound)
		_, _ = w.Write([]byte(`{"error":"this theme carries no preview"}`))
	})
	_, code, err := NewClient(2 * time.Second).ThemePreview(fakeIP(srv), "clock-aurora-9b89025ce7a6")
	if err != nil {
		t.Fatalf("an absent preview read as a failed dial: %v", err)
	}
	if code != http.StatusNotFound {
		t.Errorf("status = %d, want the device's 404 relayed", code)
	}
}

// The id gate runs before the socket opens, the same rule as the calls beside it:
// a stem this device could not have issued is refused here and never reaches a card.
func TestThemePreviewRefusesAForeignIdBeforeDialing(t *testing.T) {
	dialed := false
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) { dialed = true })
	if _, _, err := NewClient(2 * time.Second).ThemePreview(fakeIP(srv), "../../passwd"); err == nil {
		t.Fatal("want the id gate to refuse a stem no device issued")
	}
	if dialed {
		t.Fatal("the call dialed with an id the gate refuses")
	}
}
