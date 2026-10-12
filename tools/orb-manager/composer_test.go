package main

import (
	"bytes"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The Theme Editor card once carried data-view="composer", and the hub's click
// handler answered every data-view click with preventDefault + an in-page view
// switch. There is no #composer view, so the click cancelled the navigation to
// /composer/ AND hid the hub: the card looked dead. These tests pin the card to
// a real link, pin every remaining data-view to a view that exists, and pin the
// route that serves the editor.

func hubIndexHTML(t *testing.T) string {
	t.Helper()
	raw, err := webFS.ReadFile("web/index.html")
	if err != nil {
		t.Fatalf("read embedded index.html: %v", err)
	}
	return string(raw)
}

func TestThemeEditorCardIsARealLink(t *testing.T) {
	html := hubIndexHTML(t)
	m := regexp.MustCompile(`(?s)<a\s[^>]*href="(/composer[^"]*)"[^>]*>`).FindString(html)
	if m == "" {
		t.Fatal("no card links to /composer: the Theme Editor is unreachable from the hub")
	}
	if strings.Contains(m, "data-view") {
		t.Errorf("composer card carries data-view; the click handler would cancel its navigation:\n%s", m)
	}
}

func TestEveryDataViewCardHasAView(t *testing.T) {
	html := hubIndexHTML(t)
	views := regexp.MustCompile(`data-view="([^"]+)"`).FindAllStringSubmatch(html, -1)
	if len(views) == 0 {
		t.Fatal("no data-view cards at all: the hub front page lost its navigation")
	}
	for _, v := range views {
		id := v[1]
		// show() reveals an element by id, so an id-less name is a blank screen.
		if !regexp.MustCompile(`id="` + regexp.QuoteMeta(id) + `"`).MatchString(html) {
			t.Errorf("card data-view=%q has no element with that id: clicking it blanks the hub", id)
		}
	}
}

// repoComposerDir is this clone's root as seen from the package dir, the same
// layout detectRepoDir finds at runtime.
func repoComposerDir(t *testing.T) string {
	t.Helper()
	full := filepath.Join("..", "..", composerDirRel, composerEntry)
	if _, err := os.Stat(full); err != nil {
		t.Skipf("composer source missing (%v): not running inside the clone", err)
	}
	return filepath.Join("..", "..")
}

func TestComposerServesEditorWithAWayHome(t *testing.T) {
	s := &Server{repoDir: repoComposerDir(t)}
	rr := httptest.NewRecorder()
	s.serveComposer(rr, httptest.NewRequest(http.MethodGet, "/composer/", nil))

	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (body: %.200s)", rr.Code, rr.Body.String())
	}
	body := rr.Body.String()
	if !strings.Contains(body, "Orb Manager") {
		t.Error("the hub-injected back link is missing: a chromeless window would trap the user in the editor")
	}
	if !strings.Contains(body, "unpkg.com") {
		t.Error("the hub-injected React diagnostic is missing: a blocked CDN looks like a blank page with no console to read")
	}
	if strings.Count(body, "<script") < strings.Count(hubDepWatch, "<script") {
		t.Error("injected script did not survive into the served body")
	}
	if !strings.Contains(body, "watch-engine.js") {
		t.Error("served body is not the composer: its own script tag is absent")
	}
	if ct := rr.Header().Get("Content-Type"); !strings.Contains(ct, "text/html") {
		t.Errorf("Content-Type = %q, want text/html so the browser renders it", ct)
	}
}

func TestComposerIndexHTMLDoesNotRedirect(t *testing.T) {
	// ServeFile answers a path ending index.html with a 301 to "./"; the card's
	// href and any hand-typed URL must land on the editor directly.
	s := &Server{repoDir: repoComposerDir(t)}
	rr := httptest.NewRecorder()
	s.serveComposer(rr, httptest.NewRequest(http.MethodGet, "/composer/index.html", nil))
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d (Location %q), want a direct 200", rr.Code, rr.Header().Get("Location"))
	}
}

func TestComposerServesSiblingsUntouched(t *testing.T) {
	repo := repoComposerDir(t)
	s := &Server{repoDir: repo}
	for _, name := range []string{"support.js", "dial-renderer.js", "watch-engine.js", "orbtheme-baker.js"} {
		rr := httptest.NewRecorder()
		s.serveComposer(rr, httptest.NewRequest(http.MethodGet, "/composer/"+name, nil))
		if rr.Code != http.StatusOK {
			t.Fatalf("%s: status = %d, want 200", name, rr.Code)
		}
		raw, err := os.ReadFile(filepath.Join(repo, composerDirRel, name))
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if !bytes.Equal(rr.Body.Bytes(), raw) {
			t.Errorf("%s: served body differs from the file on disk; the injected link belongs in the HTML only", name)
		}
	}
}

func TestComposerRefusesTraversal(t *testing.T) {
	s := &Server{repoDir: repoComposerDir(t)}
	// Only a bare filename in the composer dir is ever legitimate: the editor
	// loads its siblings by relative name, so nothing nests and nothing escapes.
	for _, path := range []string{
		"/composer/../../Makefile",
		"/composer/%2e%2e/main.go",
		"/composer/sub/dial-renderer.js",
		"/composer/_selftest/..%2f..%2fgo.mod",
	} {
		rr := httptest.NewRecorder()
		s.serveComposer(rr, httptest.NewRequest(http.MethodGet, path, nil))
		if rr.Code == http.StatusOK {
			t.Errorf("%s: served 200, want a refusal", path)
		}
	}
}

func TestComposerNeedsAClone(t *testing.T) {
	s := &Server{} // no repoDir: the binary ran outside any gorb-os clone
	rr := httptest.NewRecorder()
	s.serveComposer(rr, httptest.NewRequest(http.MethodGet, "/composer/", nil))
	if rr.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503 with an explanation", rr.Code)
	}
	if !strings.Contains(rr.Body.String(), "clone") {
		t.Errorf("body = %q, want it to say the editor needs a clone", rr.Body.String())
	}
}

func TestInjectAfterBody(t *testing.T) {
	tests := []struct {
		name string
		in   string
		want string
	}{
		{"plain body tag", "<html><body><x-dc>", `<html><body>` + hubBackLink + `<x-dc>`},
		{"body tag with attributes", `<body class="dc"><main>`, `<body class="dc">` + hubBackLink + `<main>`},
		{"uppercase tag", "<BODY><p>", "<BODY>" + hubBackLink + "<p>"},
		{"no body tag at all", "<html><div>hi</div>", "<html><div>hi</div>"},
		{"unclosed body tag", "<html><body", "<html><body"},
	}
	for _, tc := range tests {
		got := string(injectAfterBody([]byte(tc.in), hubBackLink))
		if got != tc.want {
			t.Errorf("%s:\n got %q\nwant %q", tc.name, got, tc.want)
		}
	}
}

// The link must sit inside the body, not in the head: a stray anchor before the
// composer's own markup would render above its canvas and shift the layout.
func TestInjectAfterBodyKeepsHeadIntact(t *testing.T) {
	in := []byte("<html><head><style>a{color:red}</style></head><body><x-dc></x-dc></body></html>")
	got := string(injectAfterBody(in, hubBackLink))
	if !strings.HasPrefix(got, "<html><head><style>a{color:red}</style></head><body>") {
		t.Errorf("head or the body tag was altered: %.80s", got)
	}
	if !strings.HasSuffix(got, "<x-dc></x-dc></body></html>") {
		t.Errorf("document tail was altered: %.80s", got)
	}
}
