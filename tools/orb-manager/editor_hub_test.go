package main

// H1b ruling 32: the hub's own editor door. `/editor/hub/<slug>/<file>` serves the region
// inside the `.orbapp` this hub's app library holds, and it is the door Import from
// orb-os.net bakes with — an import cannot be made to wait for an orb that runs the app.
//
// What these tests are mostly about is what this door must NOT do: no address is checked,
// no ledger is read, no dial is opened. The hub here is built with no orbfake at all, with
// every one of its orb clients rewired to a counting sinkhole, so "nothing was dialled" is
// an observation and not a reading of the source.

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"errors"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// hubShelfHub is a hub with a library and no orbs: the shipped weather app and the sentinel
// fixture on its shelf, a token of its own, and every orb client pointed at a server that
// counts requests, so a dial this door has no business making is counted rather than assumed.
func hubShelfHub(t *testing.T) (*Server, string, *int32) {
	t.Helper()
	editorInvalidate()
	t.Cleanup(editorInvalidate)
	dir := t.TempDir()
	for _, src := range []string{
		filepath.Join("apps", "weather.orbapp"),
		filepath.Join("..", "..", "tests", "fixtures", "sentinel.orbapp"),
	} {
		body, err := os.ReadFile(src)
		if err != nil {
			t.Skipf("no shipped app at %s: %v", src, err)
		}
		name := strings.TrimSuffix(filepath.Base(src), ".orbapp")
		if err := os.WriteFile(filepath.Join(dir, name+".orbapp"), body, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	s := newHub()
	s.lib = &Library{AppsDir: dir}

	var dialed int32
	sink := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&dialed, 1)
		http.Error(w, "an orb is not here", http.StatusNotFound)
	}))
	t.Cleanup(sink.Close)
	port := strconv.Itoa(sink.Listener.Addr().(*net.TCPAddr).Port)
	for _, c := range []*orbnet.Client{s.device, s.orb, s.ota, s.preview} {
		if c != nil {
			c.DevicePort = port
		}
	}
	s.fetchEditor = func(ip, slug string) ([]byte, error) {
		atomic.AddInt32(&dialed, 1)
		return nil, errors.New("the hub door dialled " + ip + "/" + slug)
	}
	// Arm the trap before trusting it: one deliberate management dial must reach the
	// sinkhole, so a zero at the end of a test is an observation and not a blind counter.
	if _, err := s.device.AppsList("127.0.0.1"); err == nil {
		t.Fatal("the sinkhole answered an AppsList as if it were an orb")
	}
	if atomic.LoadInt32(&dialed) == 0 {
		t.Fatal("a deliberate dial never reached the counting sinkhole")
	}
	atomic.StoreInt32(&dialed, 0)
	return s, dir, &dialed
}

// hubEditorSlot is the memo's own answer for one app behind the hub door.
func hubEditorSlot(slug string) *editorSlot {
	k := editorKey{hubEditorIP, slug}
	regions.mu.Lock()
	defer regions.mu.Unlock()
	return regions.slots[k]
}

// editorAcceptGet is editorGet for the cases where the Accept header decides the shape
// of a refusal: same editor listener, same Host, plus the header a browser would send.
func editorAcceptGet(t *testing.T, s *Server, path, accept string) *httptest.ResponseRecorder {
	t.Helper()
	return editorListenerGet(t, s, path, true, map[string]string{"Accept": accept})
}

// regionMember opens one file of an app container's editor region, the way the door does.
func regionMember(t *testing.T, body []byte, name string) []byte {
	t.Helper()
	region, err := orbnet.AppEditorRegion(body)
	if err != nil || len(region) == 0 {
		t.Skipf("this container carries no editor region: %v", err)
	}
	zr, err := zip.NewReader(bytes.NewReader(region), int64(len(region)))
	if err != nil {
		t.Fatal(err)
	}
	member, found := editorMember(zr, name)
	if !found {
		t.Fatalf("the region has no %s", name)
	}
	return member
}

// The door works with no orb on the network: the hosted page for the shipped weather app,
// with the launch token on the document and not on its modules, and the sentinel fixture's
// own entry page served byte for byte out of the region.
func TestHubEditorServesTheLibrarysEditorWithNoOrb(t *testing.T) {
	s, dir, dialed := hubShelfHub(t)

	rec := editorGet(t, s, "/editor/hub/weather/", true)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d body %s", rec.Code, rec.Body)
	}
	if ct := rec.Header().Get("Content-Type"); !strings.HasPrefix(ct, "text/html") {
		t.Errorf("Content-Type = %q, want the hosted page as HTML", ct)
	}
	editorAssertDocCredential(t, s, rec, "weather")
	if cc := rec.Header().Get("Cache-Control"); cc != "no-store" {
		t.Errorf("Cache-Control = %q, want no-store", cc)
	}
	if body := rec.Body.String(); !strings.Contains(body, `src="weather-editor.js"`) {
		t.Errorf("the page does not name the app's module:\n%.400s", body)
	}

	js := editorGet(t, s, "/editor/hub/weather/weather-editor.js", true)
	if js.Code != http.StatusOK || !strings.Contains(js.Body.String(), "OrbHost.register") {
		t.Errorf("the module file: status %d", js.Code)
	}
	if cookieNamed(js, tokenCookie) != nil {
		t.Error("a sibling module carried the launch token")
	}

	// the sentinel fixture ships its own entry page, and the region is what the door opened
	body, err := os.ReadFile(filepath.Join(dir, "sentinel.orbapp"))
	if err != nil {
		t.Fatal(err)
	}
	want := regionMember(t, body, "index.html")
	doc := editorGet(t, s, "/editor/hub/sentinel/", true)
	if doc.Code != http.StatusOK {
		t.Fatalf("sentinel: status %d body %s", doc.Code, doc.Body)
	}
	// The region's bytes are what came out of the shelf, plus the one thing the hub adds
	// to an HTML document on its way out the editor port (S2).
	got := bytes.ReplaceAll(doc.Body.Bytes(), editorShim(s.editorToken("sentinel")), nil)
	if !bytes.Equal(got, want) {
		t.Errorf("sentinel entry = %q, want the region's index.html %q", got, want)
	}
	// A runtime refusal names the two releases it is comparing, and for this door there is
	// no orb release to name: an ip that is no board must not read a ledger.
	for _, ip := range []string{hubEditorIP, ""} {
		if v := s.editorVersions(ip, "weather"); v.orb != "" || v.hub == "" {
			t.Errorf("editorVersions(%q, weather) = %+v, want the shelf's release alone", ip, v)
		}
	}
	if n := atomic.LoadInt32(dialed); n != 0 {
		t.Errorf("%d dials from a door that has no orb", n)
	}
}

// Everything the hub does not carry answers at the door, in the hub's own words, and no
// path shape gets past the name checks.
func TestHubEditorRefusesWhatItDoesNotCarry(t *testing.T) {
	s, dir, dialed := hubShelfHub(t)

	rec := editorGet(t, s, "/editor/hub/nosuchapp/", true)
	if rec.Code != http.StatusNotFound {
		t.Errorf("status = %d body %s, want 404", rec.Code, rec.Body)
	}
	if !strings.Contains(rec.Body.String(), "Orb Manager carries no nosuchapp app") {
		t.Errorf("body = %s, want the sentence naming the missing app", rec.Body)
	}
	// the same refusal at a browser is a page, in the same words, with no token on it
	page := editorAcceptGet(t, s, "/editor/hub/stillmissing/", "text/html")
	if page.Code != http.StatusNotFound {
		t.Errorf("the HTML refusal's status = %d, want 404", page.Code)
	}
	if !strings.Contains(page.Body.String(), "Orb Manager carries no stillmissing app") {
		t.Errorf("the refusal page does not carry the sentence:\n%.300s", page.Body)
	}
	if cookieNamed(page, tokenCookie) != nil {
		t.Error("a failure page carried the launch token")
	}

	// The traversal check is the handler's own: a mux that cleans a raw `..` out of the
	// path before the door sees it is fine, and %2e%2e is the shape that reaches it.
	for _, p := range []string{"/editor/hub/Bad%20Name/", "/editor/hub/%2e%2e/",
		"/editor/hub/weather/../../x", "/editor/hub/weather/%2e%2e/x",
		"/editor/hub/weather/a/../../etc/passwd"} {
		if got := editorGet(t, s, p, true); got.Code == http.StatusOK {
			t.Errorf("%s served a document", p)
		}
	}
	for _, p := range []string{"/editor/hub/"} {
		if got := editorGet(t, s, p, true); got.Code != http.StatusNotFound {
			t.Errorf("%s: status %d, want 404 for a request that names no app", p, got.Code)
		}
	}
	if got := editorGet(t, s, "/editor/hub/weather/", false); got.Code == http.StatusOK {
		t.Error("the door served a document to a request with no token")
	}
	// an app on the shelf that ships no editor is refused in the hub's own words, and one
	// that is not a container at all is the hub's disk talking: a 502, not a 404
	cloneFixture(t, dir, "plain")
	noEditor := editorGet(t, s, "/editor/hub/plain/", true)
	if noEditor.Code != http.StatusNotFound ||
		!strings.Contains(noEditor.Body.String(), "Orb Manager’s copy of the plain app has no editor") {
		t.Errorf("an app with no editor: status %d body %s", noEditor.Code, noEditor.Body)
	}
	if err := os.WriteFile(filepath.Join(dir, "junk.orbapp"), []byte("not a container"), 0o644); err != nil {
		t.Fatal(err)
	}
	corrupt := editorGet(t, s, "/editor/hub/junk/", true)
	if corrupt.Code != http.StatusBadGateway {
		t.Errorf("a file that will not parse: status %d body %s, want 502", corrupt.Code, corrupt.Body)
	}
	// a refusal is never memoised: filing the app on the shelf is enough, no restart
	shelf, err := os.ReadFile(filepath.Join(dir, "sentinel.orbapp"))
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "nosuchapp.orbapp"), shelf, 0o644); err != nil {
		t.Fatal(err)
	}
	if got := editorGet(t, s, "/editor/hub/nosuchapp/", true); got.Code != http.StatusOK {
		t.Errorf("after the app arrived: status %d body %s, want the door to open", got.Code, got.Body)
	}
	if n := atomic.LoadInt32(dialed); n != 0 {
		t.Errorf("%d dials while refusing what it does not carry", n)
	}
}

// A rebuilt `.orbapp` is served on the next request: the memo slot is keyed on the library
// file's stamp, exactly as the per-orb door keys a region it read from the shelf.
func TestHubEditorLetsGoWhenTheFileChanges(t *testing.T) {
	s, dir, dialed := hubShelfHub(t)
	first := editorGet(t, s, "/editor/hub/sentinel/", true)
	if first.Code != http.StatusOK {
		t.Fatalf("status = %d body %s", first.Code, first.Body)
	}
	held := hubEditorSlot("sentinel")
	if held == nil || held.zr == nil {
		t.Fatal("the door did not memoise the region it read")
	}
	if again := editorGet(t, s, "/editor/hub/sentinel/", true); again.Code != http.StatusOK ||
		!bytes.Equal(again.Body.Bytes(), first.Body.Bytes()) {
		t.Fatalf("an unchanged file was not re-served (status %d)", again.Code)
	}
	if hubEditorSlot("sentinel") != held {
		t.Error("an unchanged library file re-read the container")
	}

	// age the slot past the window and the same file is read again, memo and all
	held.fetched = time.Now().Add(-editorFetchTTL - time.Second)
	if aged := editorGet(t, s, "/editor/hub/sentinel/", true); aged.Code != http.StatusOK ||
		!bytes.Equal(aged.Body.Bytes(), first.Body.Bytes()) {
		t.Fatalf("an aged slot: status %d, want the same document read again", aged.Code)
	}
	if hubEditorSlot("sentinel") == held {
		t.Error("a slot older than the fetch window was reused")
	}

	weather, err := os.ReadFile(filepath.Join(dir, "weather.orbapp"))
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "sentinel.orbapp")
	if err := os.WriteFile(path, weather, 0o644); err != nil {
		t.Fatal(err)
	}
	later := time.Now().Add(2 * time.Second)
	if err := os.Chtimes(path, later, later); err != nil {
		t.Fatal(err)
	}
	next := editorGet(t, s, "/editor/hub/sentinel/", true)
	if next.Code != http.StatusOK {
		t.Fatalf("after the rewrite: status %d body %s", next.Code, next.Body)
	}
	if bytes.Equal(next.Body.Bytes(), first.Body.Bytes()) {
		t.Error("a rewritten .orbapp was still served from the memo")
	}
	if !strings.Contains(next.Body.String(), "weather-editor.js") {
		t.Errorf("the new bytes were not served:\n%.300s", next.Body)
	}
	if n := atomic.LoadInt32(dialed); n != 0 {
		t.Errorf("%d dials while the shelf changed under the door", n)
	}
}

// A shelf theme that came from a site wears its origin record on its /api/library row, so a
// listing with no orb in hand can still say where a theme came from. A theme the hub never
// imported carries no key at all, and a sidecar is never a row or a refusal.
func TestLibraryOriginOnThemeRows(t *testing.T) {
	root := t.TempDir()
	themes := filepath.Join(root, "themes")
	if err := os.MkdirAll(themes, 0o755); err != nil {
		t.Fatal(err)
	}
	s := newHub()
	s.lib = &Library{AppsDir: filepath.Join(root, "apps"), ThemesDir: themes}
	put := func(file string, body []byte) string {
		t.Helper()
		p := filepath.Join(themes, file)
		if err := os.WriteFile(p, body, 0o644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	imported := put("tourbillon.orbtheme", orbfake.Container("Tourbillon"))
	put("aurora.orbtheme", orbfake.Container("Aurora"))
	cloneFixture(t, filepath.Join(root, "apps"), "sentinel")
	if err := writeThemeOrigin(imported, &ThemeOrigin{
		Site: "https://orb-os.net", DesignID: siteDesignID, Version: 9, ImportedAt: 1730000000000,
	}); err != nil {
		t.Fatal(err)
	}

	rec := rawCall(t, s, http.MethodGet, "/api/library", nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("GET /api/library: %d %s", rec.Code, rec.Body)
	}
	var env struct {
		OK   bool `json:"ok"`
		Data struct {
			Apps    []map[string]any `json:"apps"`
			Themes  []map[string]any `json:"themes"`
			Refused []struct {
				File string `json:"file"`
			} `json:"refused"`
		} `json:"data"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil || !env.OK {
		t.Fatalf("envelope: %v (%s)", err, rec.Body)
	}
	byName := map[string]map[string]any{}
	for _, th := range env.Data.Themes {
		byName[th["file"].(string)] = th
	}
	if len(byName) != 2 {
		t.Fatalf("themes = %v, want two rows and no sidecar among them", env.Data.Themes)
	}
	o, ok := byName["tourbillon.orbtheme"]["origin"].(map[string]any)
	if !ok {
		t.Fatalf("the imported row = %v, want an origin object", byName["tourbillon.orbtheme"])
	}
	if o["site"] != "https://orb-os.net" || o["designId"] != siteDesignID || o["version"] != float64(9) {
		t.Errorf("origin = %v, want the site, the design id and the version imported", o)
	}
	if at, isNum := o["importedAt"].(float64); !isNum || at == 0 {
		t.Errorf("origin = %v, want when it arrived", o)
	}
	if raw, bad := byName["aurora.orbtheme"]["origin"]; bad {
		t.Errorf("a theme the hub never imported carries %v, want no origin key", raw)
	}
	for _, th := range env.Data.Themes {
		if strings.Contains(th["file"].(string), ".origin.json") {
			t.Errorf("a sidecar was listed as a theme: %v", th)
		}
	}
	if len(env.Data.Apps) == 0 {
		t.Fatal("the shelf's app row vanished from the listing")
	}
	for _, app := range env.Data.Apps {
		if raw, bad := app["origin"]; bad {
			t.Errorf("the app row %v carries %v; an origin is a theme's record", app["file"], raw)
		}
	}
	for _, r := range env.Data.Refused {
		if strings.Contains(r.File, ".origin.json") {
			t.Errorf("a sidecar was refused: %s", r.File)
		}
	}
}
