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
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
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

func editorGet(t *testing.T, s *Server, path string, token bool) *httptest.ResponseRecorder {
	t.Helper()
	mux := http.NewServeMux()
	s.routes(mux)
	req := httptest.NewRequest(http.MethodGet, path, nil)
	if token {
		req.Header.Set(tokenHeader, s.token)
	}
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)
	return rec
}

func cookieNamed(rec *httptest.ResponseRecorder, name string) *http.Cookie {
	for _, c := range rec.Result().Cookies() {
		if c.Name == name {
			return c
		}
	}
	return nil
}

// A directory request opens the document the app's own manifest names, and that
// document carries the launch token — the whole reason the editor is same-origin.
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
	if c := cookieNamed(rec, tokenCookie); c == nil || c.Value != s.token {
		t.Error("the entry document was served without the launch token, so it cannot upload")
	}
	if len(*dialed) != 1 || (*dialed)[0] != ip+"/thermo" {
		t.Errorf("hub dialed %v, want one region read for this app", *dialed)
	}
}

// The token goes on the document and NOT on a sibling script. An editor is untrusted
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
	if cookieNamed(editorGet(t, s, "/editor/"+ip+"/thermo/index.html", true), tokenCookie) == nil {
		t.Error("the entry document lost its token")
	}
	js := editorGet(t, s, "/editor/"+ip+"/thermo/assets/b.js", true)
	if c := cookieNamed(js, tokenCookie); c != nil {
		t.Error("a sibling .js was served the launch token; only the document may hold it")
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
		"editor.json": `{"editor":1,"entry":"index.html","baker":3,"runtime":["support"]}`,
		"index.html":  `<body><h1>dialed for real</h1></body>`,
	})
	orb := orbfake.New().WithCardApps(orbfake.CardApp{
		Stem: "clock", Slug: "clock", Title: "Clock", Caps: capThemes,
		Installed: true, Enabled: true, Editor: region})
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
	mux := http.NewServeMux()
	s.routes(mux)
	req := httptest.NewRequest(http.MethodGet, "/editor/"+ip+"/clock/", nil)
	req.Header.Set(tokenHeader, s.token)
	req.Header.Set("Accept", "text/html,application/xhtml+xml")
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)
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
func TestEditorIsServedFromTheLibraryOnlyWhenTheVersionsAgree(t *testing.T) {
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
			Installed: true, Enabled: true, Version: dialStamp, Editor: region,
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
	}

	// Same stamp, both sides. The hub answers from disk.
	run(t, stamp, false)
	// The dial is behind, so the hub's copy is a different program and the board answers.
	editorInvalidate()
	run(t, "v1.0.0", true)
	// The dial reports nothing. An unstamped row is a row that stated no version, and
	// serving hub bytes against a silence is how an operator edits the wrong editor.
	editorInvalidate()
	run(t, "", true)
}
