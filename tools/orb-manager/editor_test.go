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
	"strings"
	"testing"

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
