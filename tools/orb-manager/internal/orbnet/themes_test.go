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
// is to say what the orb said. These pin the routes it dials, the names it refuses
// before they leave, and that a device refusal arrives as the firmware's own words
// rather than a paraphrase.

func fakeOrbServer(t *testing.T, handler http.HandlerFunc) *httptest.Server {
	t.Helper()
	srv := httptest.NewServer(handler)
	t.Cleanup(srv.Close)
	return srv
}

func fakeIP(srv *httptest.Server) string { return strings.TrimPrefix(srv.URL, "http://") }

const sdReply = `{"mounted":true,"card":"SDSC","uploading":false,"total_mb":7581,` +
	`"free_mb":7100,"source":"sd","active":"aurora","live":"aurora","card_count":2,` +
	`"try_slug":"","try_why":"","damage_slug":"","damage_why":"",` +
	`"themes":[{"name":"aurora","title":"Aurora","why":"","bytes":123456,"serial":7,"ok":true,"active":true},` +
	`{"name":"broken","title":"Broken","why":"art short","bytes":12,"serial":3,"ok":false,"active":false}]}`

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
	if len(l.Themes) != 2 {
		t.Fatalf("themes = %d, want 2", len(l.Themes))
	}
	if l.Themes[0].Name != "aurora" || !l.Themes[0].OK || !l.Themes[0].Active || l.Themes[0].Bytes != 123456 {
		t.Errorf("first row = %+v, want aurora ok active 123456", l.Themes[0])
	}
	if l.Themes[1].OK || l.Themes[1].Why != "art short" {
		t.Errorf("second row = %+v, want the not-loadable row and its reason", l.Themes[1])
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
// install and delete carry ?name=, default and rescan carry nothing, upload sends raw
// bytes with a Content-Length. A drift here is a 404 that only shows up on glass.
func TestThemeCallsMatchTheFirmwareRoutes(t *testing.T) {
	var seen []string
	var gotBody, gotCT string
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		seen = append(seen, r.Method+" "+r.URL.RequestURI())
		if r.Method == http.MethodPost {
			b := make([]byte, r.ContentLength)
			_, _ = r.Body.Read(b)
			gotBody, gotCT = string(b), r.Header.Get("Content-Type")
		}
		_, _ = w.Write([]byte(`{"state":"ok"}`))
	})
	ip := fakeIP(srv)
	c := NewClient(2 * time.Second)
	steps := []func() error{
		func() error { _, err := c.ThemeList(ip); return err },
		func() error { _, err := c.ThemeInstall(ip, "aurora"); return err },
		func() error { _, err := c.ThemeDefault(ip); return err },
		func() error { _, err := c.ThemeDelete(ip, "old_theme"); return err },
		func() error { _, err := c.ThemeRescan(ip); return err },
		func() error { _, err := c.ThemeUpload(ip, "aurora", []byte("BINBIN")); return err },
	}
	for i, fn := range steps {
		if err := fn(); err != nil {
			t.Fatalf("step %d: %v", i, err)
		}
	}
	want := []string{
		"GET /api/v1/sd",
		"POST /api/v1/sd/install?name=aurora",
		"POST /api/v1/sd/default",
		"POST /api/v1/sd/delete?name=old_theme",
		"POST /api/v1/sd/rescan",
		"POST /api/v1/sd/theme?name=aurora",
	}
	if strings.Join(seen, " | ") != strings.Join(want, " | ") {
		t.Errorf("device saw\n  %v\nwant\n  %v", seen, want)
	}
	if gotBody != "BINBIN" {
		t.Errorf("upload body = %q, want the bytes verbatim", gotBody)
	}
	if gotCT != "application/octet-stream" {
		t.Errorf("upload Content-Type = %q, want octet-stream", gotCT)
	}
}

// The firmware owns the slug rule (orb_sd_name_ok in main/sd/orb_sd.c). The client
// mirrors it so a name that would become a path component under /sd/themes/ never
// leaves the box, and the operator reads the reason here instead of after a dial.
func TestThemeNamesAreCheckedBeforeDialing(t *testing.T) {
	dialed := false
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		dialed = true
		_, _ = w.Write([]byte(`{}`))
	})
	ip := fakeIP(srv)
	c := NewClient(2 * time.Second)
	for _, n := range []string{"", "..", "../etc", "a/b", "a\\b", "has space", ".hidden", strings.Repeat("x", 32), "ünïcode"} {
		if _, err := c.ThemeInstall(ip, n); err == nil {
			t.Errorf("install(%q) was accepted", n)
		}
		if _, err := c.ThemeDelete(ip, n); err == nil {
			t.Errorf("delete(%q) was accepted", n)
		}
		if _, err := c.ThemeUpload(ip, n, []byte("x")); err == nil {
			t.Errorf("upload(%q) was accepted", n)
		}
	}
	for _, n := range []string{"aurora", "A_b-9.0", strings.Repeat("x", 31)} {
		if _, err := c.ThemeInstall(ip, n); err != nil {
			t.Errorf("install(%q) was refused: %v", n, err)
		}
	}
	if !dialed {
		t.Error("nothing reached the device; the good names were refused too")
	}
}
