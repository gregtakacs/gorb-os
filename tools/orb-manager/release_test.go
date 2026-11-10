package main

// orb-os-net §7.6 (Orb Manager row) and §9 (R1): the site release check, run against a
// fake site that is an httptest handler answering exactly the shape R1 answers, and read
// back through the same s.routes(mux) the binary serves.

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
)

// noRelease is the switch that makes the fake answer R1's documented 404.
const noRelease = "-"

// fakeSite is R1's endpoint as a test double: it answers
// GET /api/hub/releases/latest with the published shape, and everything else with a 404
// that is NOT the documented one, so a client dialling the wrong path fails loudly.
type fakeSite struct {
	mu    sync.Mutex
	ver   string
	notes string
	hits  int
	srv   *httptest.Server
}

func newFakeSite(t *testing.T) *fakeSite {
	t.Helper()
	f := &fakeSite{}
	f.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		f.mu.Lock()
		ver, notes := f.ver, f.notes
		f.hits++
		f.mu.Unlock()
		if r.URL.Path != "/api/hub/releases/latest" {
			w.WriteHeader(http.StatusNotFound)
			_, _ = w.Write([]byte(`{"error":"not_found"}`))
			return
		}
		if ver == noRelease {
			w.WriteHeader(http.StatusNotFound)
			_, _ = w.Write([]byte(`{"error":"no_release"}`))
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(f.doc(ver, notes))
	}))
	t.Cleanup(f.srv.Close)
	return f
}

// doc is R1's answer: version, publishedAt, notes, and one asset per platform carrying
// platform, filename, sha256, size and url. The fields the check does not read are there
// on purpose, so the decode is proven against the whole document and not a subset of it.
func (f *fakeSite) doc(ver, notes string) map[string]any {
	link := func(file string) string {
		return f.srv.URL + "/releases/download/" + ver + "/" + file
	}
	return map[string]any{
		"version":     ver,
		"publishedAt": "2026-11-04T09:00:00.000Z",
		"notes":       notes,
		"assets": []any{
			map[string]any{"platform": "windows", "filename": "orb-manager-" + ver + "-windows-amd64.zip",
				"sha256": "aa11", "size": 1048576, "url": link("orb-manager-windows-amd64.zip")},
			map[string]any{"platform": "macos", "filename": "orb-manager-" + ver + ".dmg",
				"sha256": "bb22", "size": 2097152, "url": link("orb-manager.dmg")},
			map[string]any{"platform": "linux", "filename": "orb-manager-" + ver + "-linux-amd64.tar.gz",
				"sha256": "cc33", "size": 3145728, "url": link("orb-manager-linux-amd64.tar.gz")},
		},
	}
}

// answer is what the site publishes from now on. ver == noRelease answers the 404.
func (f *fakeSite) answer(ver, notes string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.ver, f.notes = ver, notes
}

// url is the site's origin, for GORB_SITE and for the expected asset urls.
func (f *fakeSite) url() string { return f.srv.URL }

// hubVersion pins the hub's own build stamp for one test, as an ldflags build does.
func hubVersion(t *testing.T, v string) {
	t.Helper()
	prev := version
	version = v
	t.Cleanup(func() { version = prev })
}

// cachedRelease is the check's cache, taken under the lock that guards it.
func cachedRelease(s *Server) releaseInfo {
	s.relMu.Lock()
	defer s.relMu.Unlock()
	return s.rel
}

// wantAssetURL is the url the fake publishes for this machine's platform.
func wantAssetURL(f *fakeSite, ver string) string {
	file := map[string]string{
		"windows": "orb-manager-windows-amd64.zip",
		"macos":   "orb-manager.dmg",
		"linux":   "orb-manager-linux-amd64.tar.gz",
	}[releasePlatform()]
	return f.url() + "/releases/download/" + ver + "/" + file
}

// A newer site release is the one answer that becomes an offer, and the offer carries the
// asset for THIS platform. An older tag, or the hub's own, is not news and becomes nothing.
func TestReleaseCheckOffersNewerOnly(t *testing.T) {
	hubVersion(t, "v1.0.0")
	f := newFakeSite(t)
	f.answer("v9.9.9", "the shelf learns to sort")
	t.Setenv("GORB_SITE", f.url())
	s := newHub()

	if err := s.runReleaseCheck(context.Background()); err != nil {
		t.Fatalf("runReleaseCheck against a site that answered: %v", err)
	}
	got := cachedRelease(s)
	if got.Verdict != string(firmware.VerdictUpgrade) {
		t.Errorf("verdict = %q against a newer tag (%s), want %q", got.Verdict, got.Latest, firmware.VerdictUpgrade)
	}
	if got.Current != "v1.0.0" || got.Latest != "v9.9.9" {
		t.Errorf("current/latest = %q/%q, want v1.0.0/v9.9.9", got.Current, got.Latest)
	}
	if got.Notes != "the shelf learns to sort" {
		t.Errorf("notes = %q", got.Notes)
	}
	if want := wantAssetURL(f, "v9.9.9"); got.DownloadURL != want {
		t.Errorf("downloadUrl = %q, want this platform's asset %q", got.DownloadURL, want)
	}
	if !got.Fetched || got.CheckedAt == 0 {
		t.Errorf("fetched/checkedAt = %v/%d after a site that answered", got.Fetched, got.CheckedAt)
	}

	// One tag behind: an operator on the newest build has nothing to be offered.
	f.answer("v0.0.1", "ancient")
	if err := s.runReleaseCheck(context.Background()); err != nil {
		t.Fatalf("runReleaseCheck against an older tag: %v", err)
	}
	got = cachedRelease(s)
	if got.Verdict != "" {
		t.Errorf("verdict = %q against an older tag; only an upgrade is an offer", got.Verdict)
	}
	if !got.Fetched {
		t.Error("an answer that offers nothing is still an answer")
	}

	// The hub's own tag: one release, no news, and the notes are the site's words
	// about a build this hub already is.
	f.answer("v1.0.0", "this build")
	if err := s.runReleaseCheck(context.Background()); err != nil {
		t.Fatalf("runReleaseCheck against an equal tag: %v", err)
	}
	if got := cachedRelease(s); got.Verdict != "" {
		t.Errorf("verdict = %q against an equal tag", got.Verdict)
	}
}

// R1's documented 404 is a site that has published nothing: a complete answer, filed as
// one, that offers nothing.
func TestReleaseCheckRefusesNoRelease(t *testing.T) {
	hubVersion(t, "v1.0.0")
	f := newFakeSite(t)
	f.answer(noRelease, "")
	t.Setenv("GORB_SITE", f.url())
	s := newHub()

	if err := s.runReleaseCheck(context.Background()); err != nil {
		t.Fatalf("a site with nothing published answered an error; the 404 it documents is an answer: %v", err)
	}
	got := cachedRelease(s)
	if !got.Fetched {
		t.Error("fetched = false after an answer that said there is no release")
	}
	if got.Verdict != "" {
		t.Errorf("verdict = %q with nothing published", got.Verdict)
	}
	if got.Latest != "" || got.DownloadURL != "" {
		t.Errorf("no_release left latest/downloadUrl = %q/%q", got.Latest, got.DownloadURL)
	}
	if got.CheckedAt == 0 {
		t.Error("no_release did not stamp the check")
	}
}

// The platform asset list is the site's choice, not the hub's: a site that published
// nothing for this machine still has a download page, and the check says the page rather
// than nothing.
func TestReleaseCheckDownloadFallsBackToTheSite(t *testing.T) {
	hubVersion(t, "v1.0.0")
	s := newHub()

	// An asset list with no row for this platform: the documented fallback.
	nofor := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = json.NewEncoder(w).Encode(map[string]any{
			"version": "v9.9.9",
			"assets":  []any{map[string]any{"platform": "tos3", "filename": "orb.zip", "url": "https://orb.example.com/tos3.zip"}},
		})
	}))
	t.Cleanup(nofor.Close)
	t.Setenv("GORB_SITE", nofor.URL)
	if err := s.runReleaseCheck(context.Background()); err != nil {
		t.Fatalf("runReleaseCheck: %v", err)
	}
	if got := cachedRelease(s); got.DownloadURL != nofor.URL+"/download" {
		t.Errorf("downloadUrl = %q, want the site's own download page %q", got.DownloadURL, nofor.URL+"/download")
	}

	// A relative asset url is resolved against the site's origin, not served as a
	// path the browser would read against whatever page asked.
	rel := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = json.NewEncoder(w).Encode(map[string]any{
			"version": "v9.9.9",
			"assets":  []any{map[string]any{"platform": releasePlatform(), "url": "/files/orb.zip"}},
		})
	}))
	t.Cleanup(rel.Close)
	t.Setenv("GORB_SITE", rel.URL)
	if err := s.runReleaseCheck(context.Background()); err != nil {
		t.Fatalf("runReleaseCheck: %v", err)
	}
	if got := cachedRelease(s); got.DownloadURL != rel.URL+"/files/orb.zip" {
		t.Errorf("downloadUrl = %q, want the asset resolved against the origin", got.DownloadURL)
	}
}

// A site the hub cannot reach costs the cache its freshness and its claim, and nothing
// else: an offline hub still says what it last knew, under fetched:false.
func TestReleaseCheckOfflineKeepsLastAnswer(t *testing.T) {
	hubVersion(t, "v1.0.0")
	f := newFakeSite(t)
	f.answer("v9.9.9", "newer than the hub")
	t.Setenv("GORB_SITE", f.url())
	s := newHub()
	if err := s.runReleaseCheck(context.Background()); err != nil {
		t.Fatalf("the fake site refused: %v", err)
	}
	if cachedRelease(s).Verdict == "" {
		t.Fatal("the test's own premise is broken: the newer tag produced no upgrade verdict")
	}

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Skipf("no loopback socket to point at a closed port: %v", err)
	}
	closed := fmt.Sprintf("http://127.0.0.1:%d", ln.Addr().(*net.TCPAddr).Port)
	ln.Close()
	t.Setenv("GORB_SITE", closed)

	if err := s.runReleaseCheck(context.Background()); err == nil {
		t.Fatal("a check against a closed port reported no error")
	}
	got := cachedRelease(s)
	if got.Fetched {
		t.Error("fetched = true on a check that reached nothing")
	}
	if got.Verdict != "" {
		t.Errorf("verdict = %q after a failed check: an unanswered question offers nothing", got.Verdict)
	}
	if got.Latest != "v9.9.9" || got.Notes != "newer than the hub" {
		t.Errorf("the last known answer was thrown away: latest/notes = %q/%q", got.Latest, got.Notes)
	}

	// The door reports the cache as it stands: the last answer, under fetched:false.
	rec := rawCall(t, s, http.MethodGet, "/api/release/latest", nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("GET /api/release/latest = %d", rec.Code)
	}
	data := releaseData(t, rec)
	if data["fetched"] != false {
		t.Errorf("the door said fetched = %v over a closed port", data["fetched"])
	}
	if data["verdict"] != "" {
		t.Errorf("the door carried verdict = %v with no fresh answer behind it", data["verdict"])
	}
	if data["latest"] != "v9.9.9" {
		t.Errorf("the door lost the last known latest: %v", data["latest"])
	}
}

// The door's shape and its guard: every field the Updates list reads, inside {ok,data},
// and nothing at all without this launch's token.
func TestServeReleaseLatestShapes(t *testing.T) {
	hubVersion(t, "v1.2.37")
	s := newHub()
	s.rel = releaseInfo{CheckedAt: 1762260000, Current: "v1.2.37", Latest: "v1.2.40",
		Notes: "a shelf that sorts", DownloadURL: "https://orb.takacsfamily.com/releases/download/v1.2.40/orb-manager.zip",
		Verdict: "upgrade", Fetched: true}

	rec := rawCall(t, s, http.MethodGet, "/api/release/latest", nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("GET /api/release/latest with the token = %d, body %s", rec.Code, rec.Body)
	}
	data := releaseData(t, rec)
	for _, k := range []string{"checkedAt", "current", "latest", "notes", "downloadUrl", "verdict", "fetched"} {
		if _, in := data[k]; !in {
			t.Errorf("the door sends no %q, which g3-2 reads: %s", k, rec.Body)
		}
	}
	if data["verdict"] != "upgrade" || data["latest"] != "v1.2.40" || data["fetched"] != true {
		t.Errorf("the cache did not come through intact: %v", data)
	}
	if data["downloadUrl"] != s.rel.DownloadURL {
		t.Errorf("downloadUrl = %v, want %q", data["downloadUrl"], s.rel.DownloadURL)
	}

	noToken := untokenedCall(t, s, http.MethodGet, "/api/release/latest", nil)
	if noToken.Code != http.StatusForbidden {
		t.Errorf("GET /api/release/latest with no token = %d, want 403 from requireControl", noToken.Code)
	}

	// The door reads a cache and changes nothing, so a write verb has no meaning on it.
	rec = rawCall(t, s, http.MethodPost, "/api/release/latest", nil)
	if rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("POST /api/release/latest = %d, want 405: the answer is read-only", rec.Code)
	}
}

// An untagged build cannot promise an update it cannot order, and a GORB_SITE that would
// dial plaintext is a typo and not an instruction.
func TestReleaseCheckFailsClosedOnStampAndSite(t *testing.T) {
	for _, stamp := range []string{"dev", "8eb4bf0-dirty"} {
		hubVersion(t, stamp)
		f := newFakeSite(t)
		f.answer("v9.9.9", "")
		t.Setenv("GORB_SITE", f.url())
		s := newHub()
		if err := s.runReleaseCheck(context.Background()); err != nil {
			t.Fatalf("runReleaseCheck with stamp %q: %v", stamp, err)
		}
		if got := cachedRelease(s); got.Verdict != "" {
			t.Errorf("build %q claimed verdict %q: an untagged build cannot order itself against a release", stamp, got.Verdict)
		}
	}

	t.Setenv("GORB_SITE", "")
	if got := siteReleaseBase(); got != defaultSiteBase {
		t.Errorf("unset GORB_SITE = %q, want the built-in %q", got, defaultSiteBase)
	}
	for _, bad := range []string{"http://orb.example.com", "orb.takacsfamily.com", "https://",
		"ftp://orb.example.com", "http://10.0.0.5:8080", "https://greg@orb.example.com", "//orb.example.com"} {
		t.Setenv("GORB_SITE", bad)
		if got := siteReleaseBase(); got != defaultSiteBase {
			t.Errorf("GORB_SITE=%q gave %q, want the default origin refused it", bad, got)
		}
	}
	for _, good := range []string{"https://orb-os.net", "https://orb.example.com:8443/", "http://127.0.0.1:4321",
		"http://localhost:3000", "http://[::1]:3000"} {
		t.Setenv("GORB_SITE", good)
		want := strings.TrimRight(good, "/")
		if got := siteReleaseBase(); got != want {
			t.Errorf("GORB_SITE=%q gave %q, want %q", good, got, want)
		}
	}
}

// releaseData is the door's data object, or a failure naming the body it came from.
func releaseData(t *testing.T, rec *httptest.ResponseRecorder) map[string]any {
	t.Helper()
	var env struct {
		Ok   bool           `json:"ok"`
		Data map[string]any `json:"data"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
		t.Fatalf("the door is not the envelope it documents: %v\n%s", err, rec.Body)
	}
	if !env.Ok || env.Data == nil {
		t.Fatalf("the door sent no {ok:true,data:{...}}: %s", rec.Body)
	}
	return env.Data
}
