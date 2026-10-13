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
	`"free_mb":7100,"source":"sd","active":"aurora","live":"sd","card_count":2,` +
	`"try_id":"f526795c9539","try_why":"art short","damage_id":"","damage_why":"",` +
	`"themes":[{"name":"aurora","id":"9b89025ce7a6","why":"","bytes":123456,"serial":7,"ok":true,"active":true,"changed":false},` +
	`{"name":"broken","id":"f526795c9539","why":"art short","bytes":12,"serial":3,"ok":false,"active":false,"changed":true}]}`

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
// install and delete carry ?id= and nothing else, default and rescan carry nothing, and
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
		func() error { _, err := c.ThemeInstall(ip, "9b89025ce7a6"); return err },
		func() error { _, err := c.ThemeDefault(ip); return err },
		func() error { _, err := c.ThemeDelete(ip, "f893c0d634b3"); return err },
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
		"POST /api/v1/sd/install?id=9b89025ce7a6",
		"POST /api/v1/sd/default",
		"POST /api/v1/sd/delete?id=f893c0d634b3",
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

// The firmware owns the id rule (orb_theme_id_ok in main/art/orb_theme.c): exactly 12
// chars of [0-9a-f]. The client mirrors it so a malformed handle never reaches a device
// whose own first act is to 400 it, and so uppercase or traversal never leaves the box.
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
		"9b89025ce7a6/", "-b89025ce7a6", "_b89025ce7a6"}
	for _, id := range bad {
		if _, err := c.ThemeInstall(ip, id); err == nil {
			t.Errorf("install(%q) was accepted", id)
		}
		if _, err := c.ThemeDelete(ip, id); err == nil {
			t.Errorf("delete(%q) was accepted", id)
		}
	}
	for _, id := range []string{"9b89025ce7a6", "000000000000", "ffffffffffff"} {
		if _, err := c.ThemeInstall(ip, id); err != nil {
			t.Errorf("install(%q) was refused: %v", id, err)
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
		{"aurora", "9b89025ce7a6"},
		{"Aurora", "e9de098c7733"},
		{"a", "ca978112ca1b"},
		{"Neon Clock", "c02019da0ef0"},
		{"Mirage \u2014 Gold", "373b51cc021b"},
		{"\u00fcnt\u00f6me \u2603", "fd5b280e3bfe"},
		{strings.Repeat("x", 55), "d5e285683cd4"},
	} {
		got, err := ThemeIDOf(tc.name)
		if err != nil {
			t.Errorf("ThemeIDOf(%q): %v", tc.name, err)
			continue
		}
		if got != tc.id {
			t.Errorf("ThemeIDOf(%q) = %s, want %s (tools/orbtheme.py's theme_id)", tc.name, got, tc.id)
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
