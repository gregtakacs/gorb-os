package main

// D37 §5.3/§5.4: the hub serving an app's editor out of its container region.
//
// These tests stand in for a board by replacing s.editorFetch — the one line that
// dials — rather than by teaching the handler to accept a made-up answer. What is
// under test is the handler's own decisions: which member a bare directory request
// opens, who gets the launch token, and what a refusal looks like going out.

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"net/url"
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

// zipWith builds the bytes a device would stream for one app.
func zipWith(t *testing.T, files map[string]string) []byte {
	t.Helper()
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	names := make([]string, 0, len(files))
	for n := range files {
		names = append(names, n)
	}
	for _, name := range names {
		f, err := zw.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := f.Write([]byte(files[name])); err != nil {
			t.Fatal(err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// editorHub returns a hub wired to a fixed region reader and the list of dials it
// saw. The memo is cleared around every case: a cached region from one test would
// make the next test's board answer for the wrong bytes.
func editorHub(t *testing.T, ip string, raw []byte, err error) (*Server, *[]string) {
	t.Helper()
	editorInvalidate()
	t.Cleanup(editorInvalidate)
	s := newHub()
	dialed := &[]string{}
	s.fetchEditor = func(gotIP, slug string) ([]byte, error) {
		*dialed = append(*dialed, gotIP+"/"+slug)
		return raw, err
	}
	s.grants.learn(ip)
	return s, dialed
}

// editorGet drives the EDITOR listener the way an app's own page drives it (S2): the
// Host names the app, and the credential that path's class requires comes along when
// token is true — ?et= for a document or one of its files, the X-Orb-Editor header for
// an API call. A token in a URL is no longer an API credential at all, so the helper
// has to know the classes it is asking about.
func editorGet(t *testing.T, s *Server, path string, token bool) *httptest.ResponseRecorder {
	t.Helper()
	return editorListenerGet(t, s, path, token, nil)
}

// editorListenerGet is editorGet with the extra headers an origin test cares to name,
// which is the only way a test can lie about Sec-Fetch-Site on purpose.
func editorListenerGet(t *testing.T, s *Server, path string, token bool, hdr map[string]string) *httptest.ResponseRecorder {
	t.Helper()
	u, err := url.Parse(path)
	if err != nil {
		t.Fatalf("test path %q: %v", path, err)
	}
	slug := editorTestSlug(u)
	req := httptest.NewRequest(http.MethodGet, path, nil)
	req.Host = editorHostLabel(slug) + ".localhost:" + strconv.Itoa(s.editorPort)
	if token {
		if strings.HasPrefix(u.Path, "/editor/") || strings.HasPrefix(u.Path, "/runtime/") {
			q := req.URL.Query()
			q.Set(editorTokenQuery, s.editorToken(slug))
			req.URL.RawQuery = q.Encode()
		} else {
			req.Header.Set(editorTokenHeader, s.editorToken(slug))
		}
	}
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	rec := httptest.NewRecorder()
	s.editorHandler().ServeHTTP(rec, req)
	return rec
}

// editorBodyWithoutShim is the bytes a hub document would have carried before S2, which
// is what a byte-for-byte comparison against an extracted shelf can mean: the hub adds
// the fetch shim to an HTML document and to nothing else, so stripping exactly the one
// script it adds leaves the archive's own bytes. A body that carries no shim is returned
// as served — an asset's claim must still be tested against the file, not against an
// allowance.
func editorBodyWithoutShim(t *testing.T, s *Server, slug string, rec *httptest.ResponseRecorder) []byte {
	t.Helper()
	shim := editorShim(s.editorToken(slug))
	body := rec.Body.Bytes()
	if !bytes.Contains(body, shim) {
		return body
	}
	if n := bytes.Count(body, shim); n != 1 {
		t.Fatalf("the document carried %d shims, want the one the hub adds", n)
	}
	if len(body) == len(shim) {
		t.Fatal("a document was served as nothing but a shim")
	}
	return bytes.ReplaceAll(body, shim, nil)
}

// editorTestSlug is the scope a test path is addressed to: the app its query names, the app
// its path names, and otherwise the slug the runtime files are served for, since a
// /runtime/… request carries no app and is scoped by the Host alone. The query comes first
// because S2-2 refuses an editor API call whose subject is not the app it is signed in as:
// a test asking about sentinel has to knock on sentinel's door, and a helper that signed
// every /api/ call as clock would test the scope rule instead of the route.
func editorTestSlug(u *url.URL) string {
	q := u.Query()
	for _, k := range []string{"app", "slug"} {
		if v := q.Get(k); v != "" && orbnet.AppStemOK(v) {
			return v
		}
	}
	if slug := editorPathSlug(u.Path); slug != "" && orbnet.AppStemOK(slug) {
		return slug
	}
	return "clock"
}

// controlGet drives the CONTROL mux, which after S2 answers no editor path at all. It
// is the shape the pre-S2 editorGet had, and it is what a test uses to prove exactly
// that: a token-bearing request to the hub for an editor route is now a 404.
func controlGet(t *testing.T, s *Server, method, path string, token bool, body []byte) *httptest.ResponseRecorder {
	t.Helper()
	mux := http.NewServeMux()
	s.routes(mux)
	req := httptest.NewRequest(method, path, bytes.NewReader(body))
	if token {
		req.Header.Set(tokenHeader, s.token)
	}
	// The hub page's own origin, on both sides of the comparison originOK makes, so a
	// control call reads as same-origin browser traffic and not as a forged header.
	req.Host = "127.0.0.1:" + strconv.Itoa(s.controlPort)
	req.Header.Set("Origin", "http://"+req.Host)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)
	return rec
}

// editorAssertDocCredential is the S2 replacement for the launch-token cookie an editor
// document used to carry: no cookie anywhere on the response, and the document's own
// token inside the fetch shim, which is how it signs the calls it makes.
func editorAssertDocCredential(t *testing.T, s *Server, rec *httptest.ResponseRecorder, slug string) {
	t.Helper()
	if c := cookieNamed(rec, tokenCookie); c != nil {
		t.Errorf("an editor document set the %s cookie; the editor port must never carry the control credential", tokenCookie)
	}
	if len(rec.Result().Cookies()) > 0 {
		t.Errorf("an editor document set %d cookie(s); it sets none", len(rec.Result().Cookies()))
	}
	if tok := s.editorToken(slug); !strings.Contains(rec.Body.String(), tok) {
		t.Errorf("the document carries no fetch shim for %s:\n%.300s", slug, rec.Body)
	}
}

func cookieNamed(rec *httptest.ResponseRecorder, name string) *http.Cookie {
	for _, c := range rec.Result().Cookies() {
		if c.Name == name {
			return c
		}
	}
	return nil
}

// the shim's second wrapper: the send flow uploads the bake over XHR (it is the only
// browser request that reports bytes-onto-the-wire, and the commit point needs it), so
// the page's own-origin XHRs must carry the header the fetch shim gives its fetches —
// or the upload door the hub opened with one hand, it slams in the other.
func TestEditorShimSignsXHRToo(t *testing.T) {
	body := string(editorShim("clock.0123456789abcdef"))
	for _, want := range []string{
		"X.prototype.open=function", "X.prototype.send=function",
		"setRequestHeader('" + editorTokenHeader + "',T)",
	} {
		if !strings.Contains(body, want) {
			t.Errorf("the shim does not wrap XHR as the send flow needs: %.60s missing from\n%s", want, body)
		}
	}
}

// A directory request opens the document the app's own manifest names, and that
// document carries its own app's editor token in the fetch shim — the S2 shape of the
// same fact the launch-token cookie used to express: the page the hub opened can call
// the hub back, and nothing else on the machine can.
func TestEditorServesManifestEntryWithToken(t *testing.T) {
	const ip = "203.0.113.7"
	raw := zipWith(t, map[string]string{
		"editor.json":   `{"entry":"ui/panel.html","name":"Thermo"}`,
		"ui/panel.html": `<body><h1>Thermo</h1></body>`,
	})
	s, dialed := editorHub(t, ip, raw, nil)
	rec := editorGet(t, s, "/editor/"+ip+"/thermo/", true)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d body %s", rec.Code, rec.Body)
	}
	if !strings.Contains(rec.Body.String(), "Thermo") {
		t.Errorf("body = %s, want the manifest's entry document", rec.Body)
	}
	editorAssertDocCredential(t, s, rec, "thermo")
	if len(*dialed) != 1 || (*dialed)[0] != ip+"/thermo" {
		t.Errorf("hub dialed %v, want one region read for this app", *dialed)
	}
}

// The credential goes on the document and NOT on a sibling script. An editor is untrusted
// app content; handing its credential to every member of the archive would be a
// wider grant than the composer page carries today.
func TestEditorKeepsTokenOffSiblingAssets(t *testing.T) {
	const ip = "203.0.113.7"
	raw := zipWith(t, map[string]string{
		"editor.json": `{"entry":"index.html"}`,
		"index.html":  `<body>x</body>`,
		"assets/b.js": `var x`,
	})
	s, _ := editorHub(t, ip, raw, nil)
	if !strings.Contains(editorGet(t, s, "/editor/"+ip+"/thermo/index.html", true).Body.String(), s.editorToken("thermo")) {
		t.Error("the entry document lost its editor token")
	}
	js := editorGet(t, s, "/editor/"+ip+"/thermo/assets/b.js", true)
	if c := cookieNamed(js, tokenCookie); c != nil {
		t.Error("a sibling .js was served a cookie; the editor port sets none")
	}
	if strings.Contains(js.Body.String(), s.editorToken("thermo")) {
		t.Error("a sibling .js carried the editor token; only the document may hold it")
	}
}

// An archive with no manifest opens index.html rather than failing: the manifest is
// the app's convenience, not a precondition the hub invents.
func TestEditorFallsBackToIndexWithoutManifest(t *testing.T) {
	const ip = "203.0.113.8"
	raw := zipWith(t, map[string]string{"index.html": `<body>plain</body>`})
	s, _ := editorHub(t, ip, raw, nil)
	rec := editorGet(t, s, "/editor/"+ip+"/plain/", true)
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), "plain") {
		t.Fatalf("status %d body %s, want index.html served", rec.Code, rec.Body)
	}
}

// A device that answers 404 says "this app ships no editor", which is the ordinary
// state of every app predating D37. It must arrive as a quiet 404 and not a 502, or
// the face cannot tell an ordinary app from a broken board.
func TestEditorNoEditorIsQuietNotFound(t *testing.T) {
	const ip = "203.0.113.9"
	s, _ := editorHub(t, ip, nil, orbnet.ErrNoEditor)
	rec := editorGet(t, s, "/editor/"+ip+"/menu/ui.html", true)
	if rec.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", rec.Code)
	}
	if !strings.Contains(rec.Body.String(), "ships no editor") {
		t.Errorf("body = %s, want the device's own sentence", rec.Body)
	}
}

// A board that will not answer is a different fact and gets a different status.
func TestEditorDialFailureIsBadGateway(t *testing.T) {
	const ip = "203.0.113.9"
	s, _ := editorHub(t, ip, nil, errors.New("connection refused"))
	rec := editorGet(t, s, "/editor/"+ip+"/thermo/ui.html", true)
	if rec.Code != http.StatusBadGateway {
		t.Fatalf("status = %d, want 502", rec.Code)
	}
	if !strings.Contains(rec.Body.String(), "connection refused") {
		t.Errorf("body = %s, want the device's failure surfaced verbatim", rec.Body)
	}
}

// The address gate is the learned one every theme dial passes, and a traversal in
// the file part is refused rather than resolved against the archive's parent.
func TestEditorGatesAddressAndPath(t *testing.T) {
	s, _ := editorHub(t, "203.0.113.7", zipWith(t, map[string]string{"index.html": "x"}), nil)
	for _, p := range []string{
		"/editor/198.51.100.6/thermo/index.html",
		"/editor/not-an-ip/thermo/index.html",
		"/editor/203.0.113.7/thermo/a/../../etc/passwd",
		"/editor/203.0.113.7/%2e%2e/thermo/index.html",
	} {
		if rec := editorGet(t, s, p, true); rec.Code == http.StatusOK {
			t.Errorf("%s served a document", p)
		}
	}
}

// G4 ruling 36, hub half: `_fixtures/` rides in the region so the migration check can read
// it, and neither serving door opens it. A fixture is evidence about past schemas, not code
// an editor session loads, so the door answers for it with the response it already gives a
// member the app does not ship — a 404 in the same words, which says no more than "not
// here" and so leaks nothing about what the folder holds. One guard in serveEditorFile
// covers both doors because both call it, so both are driven here.
func TestEditorDoorRefusesFixtures(t *testing.T) {
	const ip = "203.0.113.7"
	const fixture = `{"schema":1,"marker":"fixture bytes must never travel"}`
	const nested = `{"schema":2,"marker":"nested fixture bytes must never travel"}`
	raw := zipWith(t, map[string]string{
		"editor.json":                     `{"entry":"index.html","runtime":["support@1"]}`,
		"index.html":                      `<body><h1>the entry document</h1></body>`,
		"assets/b.js":                     `var b`,
		"_fixtures/schema-1.orbdesign":    fixture,
		"_fixtures/nested/deep.orbdesign": nested,
		// names that merely contain the directory's name: the refused prefix is the whole
		// path step `_fixtures/`, and the bare name `_fixtures`, and none of these three.
		"my_fixtures.js":   `var mine`,
		"a/_fixtures.json": `{"not":"a fixture"}`,
		"_fixturesX/y":     `a folder that is not the folder`,
	})
	s, _ := editorHub(t, ip, raw, nil)

	for _, c := range []struct {
		file   string // as the door's cleaned path names it
		onWire string // what the request carries, when that differs
		leaks  string // member bytes that must not come back
	}{
		{"_fixtures", "", ""},
		{"_fixtures", "_fixtures/", ""},               // a trailing slash cleans to the same name
		{"_fixtures/schema-1.orbdesign", "", fixture}, // the g4-1 member by its real name
		{"_fixtures/nested/deep.orbdesign", "", nested},
		{"_fixtures/schema-9.orbdesign", "", ""}, // absent for another reason, refused the same way
		{"_fixtures/schema-1.orbdesign", "%5Ffixtures/schema-1.orbdesign", fixture},
	} {
		onWire := c.file
		if c.onWire != "" {
			onWire = c.onWire
		}
		rec := editorGet(t, s, "/editor/"+ip+"/thermo/"+onWire, true)
		if rec.Code != http.StatusNotFound {
			t.Errorf("%s: status = %d body %.200s, want 404 from the per-orb door", onWire, rec.Code, rec.Body)
			continue
		}
		if want := "this app's editor ships no " + c.file; !strings.Contains(rec.Body.String(), want) {
			t.Errorf("%s: body = %.200s, want the ordinary no-such-member sentence %q", onWire, rec.Body, want)
		}
		if c.leaks != "" && strings.Contains(rec.Body.String(), c.leaks) {
			t.Errorf("%s: the refusal echoed the fixture's bytes", onWire)
		}
	}
	// A fixture reached through a step that is not a fixture is still the `..` refusal the
	// door has always made, and a fixture reached from one that is is still a 404.
	for _, p := range []string{"_fixtures/../../index.html", "_fixtures/%2e%2e/index.html", "./_fixtures/schema-1.orbdesign"} {
		if rec := editorGet(t, s, "/editor/"+ip+"/thermo/"+p, true); rec.Code == http.StatusOK {
			t.Errorf("%s: status 200, want the traversal gate or the fixture refusal to answer", p)
		}
	}
	// The names that only look like the folder are still ordinary members.
	for name, want := range map[string]string{
		"my_fixtures.js":   `var mine`,
		"a/_fixtures.json": `{"not":"a fixture"}`,
		"_fixturesX/y":     `a folder that is not the folder`,
	} {
		rec := editorGet(t, s, "/editor/"+ip+"/thermo/"+name, true)
		if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), want) {
			t.Errorf("%s: status %d body %.120s, want the member served as itself", name, rec.Code, rec.Body)
		}
	}
	// and nothing else about the region changed shape: the manifest's entry document and a
	// plain sibling asset still come out.
	if rec := editorGet(t, s, "/editor/"+ip+"/thermo/", true); rec.Code != http.StatusOK ||
		!strings.Contains(rec.Body.String(), "the entry document") {
		t.Errorf("the entry document stopped serving: status %d body %.200s", rec.Code, rec.Body)
	}
	if rec := editorGet(t, s, "/editor/"+ip+"/thermo/assets/b.js", true); rec.Code != http.StatusOK {
		t.Errorf("assets/b.js: status %d, want a sibling asset still served", rec.Code)
	}

	// The hub door, on the shelf's own g4-1 container: same shared gate, same answer, and
	// the fixture it refuses is a real member of the region behind the door.
	hs, dir, dialed := hubShelfHub(t)
	body, err := os.ReadFile(filepath.Join(dir, "weather.orbapp"))
	if err != nil {
		t.Fatal(err)
	}
	shipped := regionMember(t, body, "_fixtures/schema-1.orbdesign")
	for _, file := range []string{"_fixtures", "_fixtures/schema-1.orbdesign"} {
		rec := editorGet(t, hs, "/editor/hub/weather/"+file, true)
		if rec.Code != http.StatusNotFound {
			t.Errorf("hub door %s: status = %d body %.200s, want 404", file, rec.Code, rec.Body)
			continue
		}
		if !strings.Contains(rec.Body.String(), "this app's editor ships no _fixtures") {
			t.Errorf("hub door %s: body = %.200s, want the same sentence the member refusal uses", file, rec.Body)
		}
		if bytes.Contains(rec.Body.Bytes(), shipped) {
			t.Errorf("hub door %s: the refusal carried the shelf's fixture bytes", file)
		}
	}
	if rec := editorGet(t, hs, "/editor/hub/weather/", true); rec.Code != http.StatusOK {
		t.Errorf("the hub door's own entry page: status %d body %.200s, want it still served", rec.Code, rec.Body)
	}
	if n := atomic.LoadInt32(dialed); n != 0 {
		t.Errorf("%d dials from a door that has no orb", n)
	}

	// An app whose manifest names a file in the fixtures folder as its entry is an app that
	// means to have one served as code. rawEntryName only lets an .html through, so this is
	// the shape that reaches the member read at all — and the guard runs on the resolved
	// name, after the manifest has had its say, so the directory request is refused rather
	// than walked into the folder.
	t.Run("a manifest that names a fixture as its entry", func(t *testing.T) {
		const other = "203.0.113.11"
		const inFixtures = `<body>a document parked in the evidence folder</body>`
		door, _ := editorHub(t, other, zipWith(t, map[string]string{
			"editor.json":                  `{"entry":"_fixtures/entry.html","runtime":["support@1"]}`,
			"_fixtures/entry.html":         inFixtures,
			"_fixtures/schema-1.orbdesign": fixture,
			"index.html":                   `<body>not the entry</body>`,
		}), nil)
		rec := editorGet(t, door, "/editor/"+other+"/thermo/", true)
		if rec.Code != http.StatusNotFound || strings.Contains(rec.Body.String(), inFixtures) {
			t.Errorf("status %d body %.200s, want a 404 that carries none of the fixture", rec.Code, rec.Body)
		}
	})
}

// A region that is not an archive is a corrupt app and must name what failed to
// parse: the container declared a ZIP and the bytes were not one.
func TestEditorRejectsNonArchiveRegion(t *testing.T) {
	const ip = "203.0.113.10"
	s, _ := editorHub(t, ip, []byte("not a zip at all"), nil)
	rec := editorGet(t, s, "/editor/"+ip+"/thermo/index.html", true)
	if rec.Code == http.StatusOK {
		t.Fatal("a non-archive region served a document")
	}
	if !strings.Contains(strings.ToLower(rec.Body.String()), "zip") {
		t.Errorf("body = %s, want the refusal to name what failed to parse", rec.Body)
	}
}

// No token, no route: the editor dial reaches a device and carries the same guard
// every other /api/ surface carries.
func TestEditorRequiresToken(t *testing.T) {
	s, _ := editorHub(t, "203.0.113.7", zipWith(t, map[string]string{"index.html": "x"}), nil)
	if rec := editorGet(t, s, "/editor/203.0.113.7/thermo/", false); rec.Code == http.StatusOK {
		t.Fatal("the editor route served a document to a request with no token")
	}
}

// The wiring itself. S3 shipped fetchEditor set only by editor_test's own stub,
// so every live /editor/ request nil-panicked until S4 put a region on a real
// card and the browser came knocking. This test takes the hub exactly as newHub
// builds it — no stub over the field — and demands the real route reach the real
// door: a fake dial answering GET /api/v1/apps/editor with the region it holds.
func TestProductionHubDialsTheEditorDoorWithoutAStub(t *testing.T) {
	editorInvalidate()
	t.Cleanup(editorInvalidate)
	region := zipWith(t, map[string]string{
		"editor.json": `{"editor":1,"entry":"index.html","baker":3,"runtime":["support@1"]}`,
		"index.html":  `<body><h1>dialed for real</h1></body>`,
	})
	orb := orbfake.New().WithCardApps(orbfake.CardApp{
		Stem: "clock", Slug: "clock", Title: "Clock", Caps: capThemes,
		Installed: true, Editor: region})
	s, ip, _, f := fakeOrbHubWith(t, orb)
	rec := editorGet(t, s, "/editor/"+ip+"/clock/", true)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d body %s", rec.Code, rec.Body)
	}
	if !strings.Contains(rec.Body.String(), "dialed for real") {
		t.Errorf("body = %s, want the region the fake board holds", rec.Body)
	}
	var dialed bool
	for _, c := range f.Calls() {
		if c == "GET /api/v1/apps/editor?id=clock" {
			dialed = true
		}
	}
	if !dialed {
		t.Error("the hub never dialed the device's editor door")
	}
}

// A torn read (the orb's socket purge under load, a radio hiccup) is the failure
// the owner met on 2026-10-27, and a second dial clears it. The dial is an
// idempotent GET, so the handler retries once before it records a failure — and
// never retries the stable ErrNoEditor answer.
func TestEditorRetriesATransportFailureOnceThenServes(t *testing.T) {
	const ip = "203.0.113.7"
	raw := zipWith(t, map[string]string{"index.html": "<body>second dial won</body>"})
	editorInvalidate()
	t.Cleanup(editorInvalidate)
	s := newHub()
	var dials int
	s.fetchEditor = func(string, string) ([]byte, error) {
		dials++
		if dials == 1 {
			return nil, errors.New("connection reset by peer")
		}
		return raw, nil
	}
	s.grants.learn(ip)
	rec := editorGet(t, s, "/editor/"+ip+"/clock/", true)
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), "second dial won") {
		t.Fatalf("status=%d body=%s, want the retry's archive served", rec.Code, rec.Body)
	}
	if dials != 2 {
		t.Errorf("dials = %d, want exactly 2 (one retry, no loop)", dials)
	}
}

func TestEditorNoEditorIsNeverRetried(t *testing.T) {
	const ip = "203.0.113.7"
	s, dialed := editorHub(t, ip, nil, orbnet.ErrNoEditor)
	if rec := editorGet(t, s, "/editor/"+ip+"/clock/", true); rec.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", rec.Code)
	}
	if len(*dialed) != 1 {
		t.Errorf("dials = %d, want 1: a stable refusal retried is a second wasted board visit", len(*dialed))
	}
}

// The failure page exists because the JSON error the owner saw had no way out:
// no name for the failure, no button, a pane that could only be closed. A
// document navigation gets a real page with a Retry; the Retry must actually be
// fresh (no-store), and the page must carry none of the hub's credentials.
func TestEditorFailurePageIsRetryableHTMLForNavigations(t *testing.T) {
	const ip = "203.0.113.9"
	s, _ := editorHub(t, ip, nil, errors.New("context deadline exceeded"))
	req := httptest.NewRequest(http.MethodGet, "/editor/"+ip+"/clock/", nil)
	req.Host = "clock.localhost:" + strconv.Itoa(s.editorPort)
	req.URL.RawQuery = editorTokenQuery + "=" + s.editorToken("clock")
	req.Header.Set("Accept", "text/html,application/xhtml+xml")
	rec := httptest.NewRecorder()
	s.editorHandler().ServeHTTP(rec, req)
	if rec.Code != http.StatusBadGateway {
		t.Fatalf("status = %d, want 502", rec.Code)
	}
	body := rec.Body.String()
	if ct := rec.Header().Get("Content-Type"); !strings.Contains(ct, "text/html") {
		t.Errorf("Content-Type = %q, want the failure rendered for a frame", ct)
	}
	if !strings.Contains(body, "Retry") || !strings.Contains(body, "did not load") {
		t.Errorf("body = %s, want a named failure and a Retry anchor", body)
	}
	if !strings.Contains(body, "deadline") {
		t.Errorf("body = %s, want the underlying reason surfaced", body)
	}
	if cc := rec.Header().Get("Cache-Control"); cc != "no-store" {
		t.Errorf("Cache-Control = %q, want no-store so Retry cannot re-read a cached refusal", cc)
	}
	if strings.Contains(body, s.token) {
		t.Error("the failure page carries the launch token")
	}
}

// A transport failure memoises for two seconds, not fifteen: the old shape made
// every Retry inside the window re-serve the same dead error and the pane looked
// bricked even though the board had recovered. Age the memo past the short TTL
// and the next request dials for real.
func TestEditorTransportFailureMemoExpiresFast(t *testing.T) {
	const ip = "203.0.113.7"
	editorInvalidate()
	t.Cleanup(editorInvalidate)
	s := newHub()
	var dials int
	var good bool
	goodRaw := zipWith(t, map[string]string{"index.html": "<body>recovered</body>"})
	s.fetchEditor = func(string, string) ([]byte, error) {
		dials++
		if good {
			return goodRaw, nil
		}
		return nil, errors.New("connection reset by peer")
	}
	s.grants.learn(ip)
	if rec := editorGet(t, s, "/editor/"+ip+"/clock/", true); rec.Code != http.StatusBadGateway {
		t.Fatalf("status = %d, want the failure answered first", rec.Code)
	}
	dialsAfterFirst := dials
	if rec := editorGet(t, s, "/editor/"+ip+"/clock/", true); rec.Code != http.StatusBadGateway || dials != dialsAfterFirst {
		t.Fatalf("within the short TTL the memo must answer without dialling (dials %d)", dials)
	}
	// Age the failed slot out of its short window and let the board recover.
	regions.mu.Lock()
	for _, sl := range regions.slots {
		sl.fetched = sl.fetched.Add(-3 * time.Second)
	}
	regions.mu.Unlock()
	good = true
	rec := editorGet(t, s, "/editor/"+ip+"/clock/", true)
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), "recovered") {
		t.Fatalf("status=%d body=%s, want the aged memo to re-dial a recovered board", rec.Code, rec.Body)
	}
}

// §4: an app the dial and the shelf agree on is edited out of the hub's own file. The
// region is the same bytes the device would stream, and the orb's one HTTP lane is the
// scarcest resource on the network: every asset a composer session pulls afterwards is a
// dial the board does not answer. The gate is the version stamp alone, so the test's whole
// value is in the pair of cases — agree, and the dial never rings; disagree, and it rings.
// Owner, 2026-11-05: the hub's editor for the orb's own release; an orb behind the shelf
// keeps its own editor until its app is updated.
func TestEditorIsServedFromTheLibraryWhenItIsTheOrbsRelease(t *testing.T) {
	const stamp = "v1.4.2-rc.1+build.7" // the sentinel fixture's own header stamp
	editorInvalidate()
	t.Cleanup(editorInvalidate)

	run := func(t *testing.T, dialStamp string, wantDialed bool) {
		t.Helper()
		dir := t.TempDir()
		cloneFixture(t, dir, "sentinel")
		body, err := os.ReadFile(dir + "/sentinel.orbapp")
		if err != nil {
			t.Skipf("no sentinel fixture: %v", err)
		}
		// The fixture's editor region is what both sides would serve, so the byte-for-byte
		// comparison below proves the hub answered from its own disk and not a stub.
		region, err := orbnet.AppEditorRegion(body)
		if err != nil || len(region) == 0 {
			t.Skipf("the sentinel fixture carries no editor region: %v", err)
		}
		orb := orbfake.New().WithCardApps(orbfake.CardApp{
			Stem: "sentinel", Slug: "sentinel", Title: "Sentinel",
			Installed: true, Version: dialStamp, Editor: region,
		})
		s, ip, _, f := fakeOrbHubWith(t, orb)
		s.lib = &Library{AppsDir: dir}

		rec := editorGet(t, s, "/editor/"+ip+"/sentinel/", true)
		if rec.Code != http.StatusOK {
			t.Fatalf("status = %d body %s", rec.Code, rec.Body)
		}
		var dialed bool
		for _, c := range f.Calls() {
			if strings.Contains(c, "/api/v1/apps/editor") {
				dialed = true
			}
		}
		if dialed != wantDialed {
			t.Errorf("editor door dialed = %v, want %v (dial stamp %q against shelf %q)",
				dialed, wantDialed, dialStamp, stamp)
		}
		// and the source route says the same thing the door just did, with both releases
		var env struct {
			Data map[string]any `json:"data"`
		}
		src := editorGet(t, s, "/api/editor/source?ip="+ip+"&slug=sentinel", true)
		if err := json.Unmarshal(src.Body.Bytes(), &env); err != nil {
			t.Fatalf("source: %s", src.Body)
		}
		wantSrc := "library"
		if wantDialed {
			wantSrc = "orb"
		}
		if env.Data["source"] != wantSrc || env.Data["hub_version"] != stamp || (dialStamp != "" && env.Data["orb_version"] != dialStamp) {
			t.Errorf("source = %v, want %s with hub %s and orb %q", env.Data, wantSrc, stamp, dialStamp)
		}
	}

	// Same stamp, both sides. The hub answers from disk.
	run(t, stamp, false)
	// The dial is behind: its own editor answers, whose bakes the app it runs can read;
	// /api/editor/source still names the hub's newer release.
	editorInvalidate()
	run(t, "v1.0.0", true)
	// Another build of the shelf's release: still the hub's.
	editorInvalidate()
	run(t, "v1.4.2-rc.1+other", false)
	// The dial is AHEAD of the shelf: the hub's older editor would drop what the orb's
	// release added, so the board answers.
	editorInvalidate()
	run(t, "v9.0.0", true)
	// The dial reports nothing. An unstamped row is a row that stated no version, and
	// serving hub bytes against a silence is how an operator edits the wrong editor.
	editorInvalidate()
	run(t, "", true)
}

// A region served from the library holds only while that file is the one on disk: a rebuilt
// .orbapp is served on the next request, not after the memo's ten minutes (a bench rebuilding
// the clock's editor kept getting the old one, 2026-11-08). The rewrite here is not an app, so
// the door falls through to the dial, and the dial ringing is the proof the memo let go.
func TestEditorLetsGoOfALibraryRegionWhenTheFileChanges(t *testing.T) {
	const stamp = "v1.4.2-rc.1+build.7"
	editorInvalidate()
	t.Cleanup(editorInvalidate)
	dir := t.TempDir()
	cloneFixture(t, dir, "sentinel")
	body, err := os.ReadFile(dir + "/sentinel.orbapp")
	if err != nil {
		t.Skipf("no sentinel fixture: %v", err)
	}
	region, err := orbnet.AppEditorRegion(body)
	if err != nil || len(region) == 0 {
		t.Skipf("the sentinel fixture carries no editor region: %v", err)
	}
	orb := orbfake.New().WithCardApps(orbfake.CardApp{
		Stem: "sentinel", Slug: "sentinel", Title: "Sentinel", Installed: true, Version: stamp, Editor: region,
	})
	s, ip, _, f := fakeOrbHubWith(t, orb)
	s.lib = &Library{AppsDir: dir}
	if rec := editorGet(t, s, "/editor/"+ip+"/sentinel/", true); rec.Code != http.StatusOK {
		t.Fatalf("status = %d body %s", rec.Code, rec.Body)
	}
	if n := calls(f, "/api/v1/apps/editor"); n != 0 {
		t.Fatalf("the shelf's own release dialed %d times", n)
	}
	if rec := editorGet(t, s, "/editor/"+ip+"/sentinel/", true); rec.Code != http.StatusOK || calls(f, "/api/v1/apps/editor") != 0 {
		t.Fatalf("an unchanged file was not served from the memo (status %d)", rec.Code)
	}
	later := time.Now().Add(2 * time.Second)
	if err := os.WriteFile(dir+"/sentinel.orbapp", []byte("rebuilt, and not an app"), 0o644); err != nil {
		t.Fatal(err)
	}
	_ = os.Chtimes(dir+"/sentinel.orbapp", later, later)
	editorGet(t, s, "/editor/"+ip+"/sentinel/", true)
	if calls(f, "/api/v1/apps/editor") == 0 {
		t.Error("a changed library file was still served from the memo")
	}
}
