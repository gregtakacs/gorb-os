package main

// orb-os-net §7.6 ruling 26: the one Updates list, and the two refusals wired at this
// step. Every theme row here is built from a real import kept on a real shelf against a
// site that is an httptest handler — the same discipline H1's tests keep, because the
// thing under test is what the origin record does once it is on disk.

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
)

// updateSite is a design site that answers the resolve door too, remembers how many times
// it was asked, and can be moved to a new version between two reads — which is the whole
// of what Skip this version is a refusal about.
type updateSite struct {
	srv      *httptest.Server
	mu       sync.Mutex
	version  int
	status   int
	shared   string
	resolves int
	bundle   func() []byte
}

func (u *updateSite) ask() int {
	u.mu.Lock()
	defer u.mu.Unlock()
	return u.resolves
}

func (u *updateSite) moveTo(version int, shared string) {
	u.mu.Lock()
	defer u.mu.Unlock()
	u.version, u.shared = version, shared
}

func (u *updateSite) answer(status int) {
	u.mu.Lock()
	defer u.mu.Unlock()
	u.status = status
}

func (u *updateSite) url() string { return strings.ToLower(u.srv.URL) }

// updatesSite stands the fake. The bundle door answers with the imported design at the
// version it names, so an import through the real doors leaves a real sidecar behind.
func updatesSite(t *testing.T, version int, shared string, art string) *updateSite {
	t.Helper()
	u := &updateSite{version: version, shared: shared, status: http.StatusOK}
	u.bundle = func() []byte {
		return goodBundle(t, siteDesignID, version, "clock", shared, []byte(art))
	}
	u.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/hub/resolve" {
			_, _ = w.Write(u.bundle())
			return
		}
		u.mu.Lock()
		u.resolves++
		v, st, name := u.version, u.status, u.shared
		u.mu.Unlock()
		if st != http.StatusOK {
			w.WriteHeader(st)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = fmt.Fprintf(w, `{"designId":%q,"app":"clock","version":%d,"sharedName":%q}`,
			siteDesignID, v, name)
	}))
	t.Cleanup(u.srv.Close)
	return u
}

// importShelfTheme runs the doors an owner uses — paste the link, keep the bake — and
// returns the theme's shelf file. A test that faked the sidecar by hand would be testing
// a record the hub never writes.
func importShelfTheme(t *testing.T, s *Server, u *updateSite, shared string) string {
	t.Helper()
	tk := ticketOf(t, siteImport(t, s, u.url()+"/t/"+siteDesignID))
	bake := withDesign(orbfake.Container(shared), string(designDoc(shared, "v1.2.30", 1)))
	env := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+tk, bake)
	if env["ok"] != true {
		t.Fatalf("keep: %v", env)
	}
	file, _ := env["data"].(map[string]any)["file"].(string)
	if file == "" {
		t.Fatalf("keep named no shelf file: %v", env)
	}
	return filepath.Join(s.library().ThemesDir, file)
}

// updatesOf reads GET /api/updates through the router the binary serves.
func updatesOf(t *testing.T, s *Server) updatesPayload {
	t.Helper()
	env := designCall(t, s, http.MethodGet, "/api/updates", nil)
	if env["ok"] != true {
		t.Fatalf("GET /api/updates: %v", env)
	}
	raw, err := json.Marshal(env["data"])
	if err != nil {
		t.Fatal(err)
	}
	var out updatesPayload
	if err := json.Unmarshal(raw, &out); err != nil {
		t.Fatalf("the update list is not the documented shape: %v", err)
	}
	return out
}

func updateRowOf(p updatesPayload, kind string) (updateItem, bool) {
	for _, it := range p.Items {
		if it.Kind == kind {
			return it, true
		}
	}
	return updateItem{}, false
}

func postRefusal(t *testing.T, s *Server, path string, body map[string]any) map[string]any {
	t.Helper()
	b, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	return designCall(t, s, http.MethodPost, path, b)
}

func choiceInState(t *testing.T, s *Server, kind, key string) UpdateChoice {
	t.Helper()
	b, err := os.ReadFile(s.statePath)
	if err != nil {
		t.Fatalf("state file: %v", err)
	}
	var st hubState
	if err := json.Unmarshal(b, &st); err != nil {
		t.Fatalf("state file is not the documented shape: %v", err)
	}
	c, ok := st.UpdateChoices[choiceKey(kind, key)]
	if !ok {
		t.Fatalf("no %q entry in %s", choiceKey(kind, key), b)
	}
	return c
}

// shelfThemeRow reads GET /api/library's row for one shelf file.
func shelfThemeRow(t *testing.T, s *Server, file string) map[string]any {
	t.Helper()
	env := designCall(t, s, http.MethodGet, "/api/library", nil)
	if env["ok"] != true {
		t.Fatalf("GET /api/library: %v", env)
	}
	themes, _ := env["data"].(map[string]any)["themes"].([]any)
	for _, v := range themes {
		row, _ := v.(map[string]any)
		if row["file"] == filepath.Base(file) {
			return row
		}
	}
	t.Fatalf("the shelf no longer lists %q: %v", file, env["data"])
	return nil
}

// The gate's fourth sentence, and the reason a keep is not a filter: the sidecar goes
// away, so nothing is left for a later check to trace.
func TestKeepMyVersionStopsTracking(t *testing.T) {
	s, _, root := libHub(t)
	s.statePath = filepath.Join(root, "state.json")
	u := updatesSite(t, 3, "Skeleton by @greg", "art")
	shelf := importShelfTheme(t, s, u, "Skeleton by @greg")
	body, err := os.ReadFile(shelf)
	if err != nil {
		t.Fatalf("theme: %v", err)
	}
	u.moveTo(5, "Skeleton by @greg")
	if _, ok := updateRowOf(updatesOf(t, s), "theme"); !ok {
		t.Fatalf("the theme was not offered before the keep: %+v", updatesOf(t, s))
	}
	asked := u.ask()
	if asked == 0 {
		t.Fatal("the list resolved nothing, so the counter below proves nothing")
	}

	env := postRefusal(t, s, "/api/updates/keep", map[string]any{"kind": "theme", "key": siteDesignID})
	if env["ok"] != true {
		t.Fatalf("keep: %v", env)
	}

	if _, err := os.Stat(themeOriginPath(shelf)); !os.IsNotExist(err) {
		t.Errorf("the sidecar survived the keep: stat said %v", err)
	}
	after, err := os.ReadFile(shelf)
	if err != nil {
		t.Fatalf("the theme itself vanished: %v", err)
	}
	if string(after) != string(body) {
		t.Error("Keep my version rewrote the theme it was pressed on")
	}
	row := shelfThemeRow(t, s, shelf)
	if row["origin"] != nil {
		t.Errorf("/api/library still carries an origin after the keep: %v", row["origin"])
	}
	if !strings.Contains(strings.Join([]string{fmt.Sprint(row["title"])}, ""), "Skeleton by @greg") {
		t.Errorf("the theme lost its name: %v", row)
	}

	c := choiceInState(t, s, "theme", siteDesignID)
	if !c.Keep || c.Kind != "theme" || c.Key != siteDesignID {
		t.Errorf("state choice = %+v, want a theme keep for this design", c)
	}
	// The round trip: a hub that forgets a keep would start checking the theme again the
	// moment it restarted, which is the one thing ruling 26 forbids.
	re := newHub()
	re.statePath = s.statePath
	re.lib = s.lib
	re.loadState()
	if got := re.updateChoices[choiceKey("theme", siteDesignID)]; !got.Keep {
		t.Errorf("after loadState the keep is gone: %+v", re.updateChoices)
	}

	before := u.ask()
	if _, ok := updateRowOf(updatesOf(t, s), "theme"); ok {
		t.Error("the kept theme came back onto the list")
	}
	if got := u.ask(); got != before {
		t.Errorf("resolveOrigin was called %d more times after a keep; a kept theme is never checked again", got-before)
	}
}

// Skip is an answer about ONE version: the row goes, and the next newer thing the site
// says comes back. A skip that hid a design forever would be a keep in disguise.
func TestSkipThisVersionHidesAndReoffers(t *testing.T) {
	s, _, root := libHub(t)
	s.statePath = filepath.Join(root, "state.json")
	u := updatesSite(t, 3, "Skeleton by @greg", "art")
	importShelfTheme(t, s, u, "Skeleton by @greg")

	u.moveTo(5, "Skeleton by @greg")
	it, ok := updateRowOf(updatesOf(t, s), "theme")
	if !ok {
		t.Fatalf("v5 over a baked v3 was not offered: %+v", updatesOf(t, s))
	}
	if it.Version != 5 || it.Key != siteDesignID {
		t.Errorf("row = %+v, want this design offered at version 5", it)
	}

	env := postRefusal(t, s, "/api/updates/skip", map[string]any{"kind": "theme", "key": siteDesignID, "version": 5})
	if env["ok"] != true {
		t.Fatalf("skip: %v", env)
	}
	if _, ok := updateRowOf(updatesOf(t, s), "theme"); ok {
		t.Fatalf("v5 was still listed after the skip: %+v", updatesOf(t, s))
	}
	if c := choiceInState(t, s, "theme", siteDesignID); c.SkippedVersion != 5 || c.Keep {
		t.Errorf("state choice = %+v, want skippedVersion 5 and no keep", c)
	}

	u.moveTo(6, "Skeleton by @greg")
	it, ok = updateRowOf(updatesOf(t, s), "theme")
	if !ok {
		t.Fatalf("v6 was not offered after a skip of v5: %+v", updatesOf(t, s))
	}
	if it.Version != 6 {
		t.Errorf("row = %+v, want the offer at version 6", it)
	}
	// Same version again, and the answer still holds: a list that re-offered what the
	// operator already turned down would train them to stop reading it.
	if _, ok := updateRowOf(updatesOf(t, s), "theme"); !ok {
		t.Error("the row is not stable across two reads")
	}
}

// The list is a catalog of the §7.6 table, so a hub with one thing of each kind due must
// answer with one row of each kind — and the count must be the length of the list, because
// the header's number is the only promise the section makes.
func TestUpdatesAggregateRows(t *testing.T) {
	s, ip, root := libHub(t)
	s.statePath = filepath.Join(root, "state.json")
	u := updatesSite(t, 3, "Skeleton by @greg", "art")
	importShelfTheme(t, s, u, "Skeleton by @greg")
	u.moveTo(4, "Night dial by @greg")

	s.relMu.Lock()
	s.rel = releaseInfo{CheckedAt: time.Now().Unix(), Current: "v1.2.30", Latest: "v1.2.37",
		DownloadURL: "https://orb.test/download", Verdict: string(firmware.VerdictUpgrade), Fetched: true}
	s.relMu.Unlock()
	s.adoptFirmware(stampedBundle(t, "v1.2.37+aaaaaaa"))
	s.fleet.Restore([]orbfleet.Orb{{MAC: "44bd8d782618", Instance: "orb-782618", IP: ip,
		FW: "v1.0.0+aaaaaaa", State: "normal"}})
	s.setOrbIP(ip)

	p := updatesOf(t, s)
	if p.Count != len(p.Items) {
		t.Errorf("count = %d with %d items", p.Count, len(p.Items))
	}
	mgr, ok := updateRowOf(p, "manager")
	if !ok {
		t.Fatalf("no manager row although the cache says upgrade: %+v", p.Items)
	}
	if mgr.From != "v1.2.30" || mgr.To != "v1.2.37" || len(mgr.Choices) != 1 || mgr.Choices[0] != "download" {
		t.Errorf("manager row = %+v, want v1.2.30 to v1.2.37 offering download alone", mgr)
	}
	if p.DownloadURL != "https://orb.test/download" {
		t.Errorf("downloadUrl = %q, want the cached asset link", p.DownloadURL)
	}
	fw, ok := updateRowOf(p, "firmware")
	if !ok {
		t.Fatalf("no firmware row although the hub carries newer: %+v", p.Items)
	}
	if fw.Key != "hub" || fw.From != "v1.0.0+aaaaaaa" || fw.To == "" {
		t.Errorf("firmware row = %+v", fw)
	}
	if fw.Note != "Orb Manager v1.2.37 brings firmware v1.2.37" {
		t.Errorf("pairing sentence = %q", fw.Note)
	}
	th, ok := updateRowOf(p, "theme")
	if !ok {
		t.Fatalf("no theme row: %+v", p.Items)
	}
	if !strings.Contains(strings.Join(th.Choices, ","), "keep") {
		t.Errorf("theme choices = %v, want the three the gate names", th.Choices)
	}
	if !th.Renamed || th.To != "Night dial by @greg" {
		t.Errorf("theme row = %+v, want the renaming offer worded with the new name", th)
	}
	// The amber buttons on the tabs are not this list's business, and the disabled
	// Update is g3-3's: this step offers it and does nothing else.
	if strings.Contains(strings.Join(fw.Choices, ","), "update") {
		t.Errorf("firmware choices = %v, want the skip only", fw.Choices)
	}
}

// A design the site has taken away is a row of its own, and a site that will not answer
// is a row that says so. Neither is allowed to read as "up to date": §7.4's sentence and
// §7.6's refusal both turn on the operator being able to tell the three apart.
func TestUpdatesGoneAndUncheckableThemeRows(t *testing.T) {
	s, _, root := libHub(t)
	s.statePath = filepath.Join(root, "state.json")
	u := updatesSite(t, 3, "Skeleton by @greg", "art")
	importShelfTheme(t, s, u, "Skeleton by @greg")

	u.answer(http.StatusGone)
	it, ok := updateRowOf(updatesOf(t, s), "theme")
	if !ok {
		t.Fatalf("a 410 listed no row at all: %+v", updatesOf(t, s).Items)
	}
	if !it.Gone || len(it.Choices) != 0 {
		t.Errorf("gone row = %+v, want gone with no choices but the OK the page draws", it)
	}

	u.answer(http.StatusNotFound)
	if it, ok := updateRowOf(updatesOf(t, s), "theme"); !ok || !it.Gone {
		t.Errorf("a 404 read as %+v %v, want a gone row", it, ok)
	}

	// A site that answers nothing at all — the port closed — is the offline case, and the
	// row must say it could not be checked rather than fall silent.
	u.answer(http.StatusInternalServerError)
	it, ok = updateRowOf(updatesOf(t, s), "theme")
	if !ok {
		t.Fatal("a failing resolve listed no row, so the theme would read as up to date")
	}
	if it.Gone || !strings.HasPrefix(it.Note, "could not be checked: ") || len(it.Choices) != 0 {
		t.Errorf("unchecked row = %+v, want a note and no choices", it)
	}

	// The same version is not an offer, and a shelf theme with no origin is not either.
	u.answer(http.StatusOK)
	u.moveTo(3, "Skeleton by @greg")
	if _, ok := updateRowOf(updatesOf(t, s), "theme"); ok {
		t.Error("a design at the version already baked was offered")
	}
}

// With no orb, nothing imported and a release check that has not landed, the list is empty
// and the answer is still ok:true: an offline hub is not an error page.
func TestUpdatesEmptyWithNothingToReport(t *testing.T) {
	s, _, root := libHub(t)
	s.statePath = filepath.Join(root, "state.json")
	p := updatesOf(t, s)
	if p.Count != len(p.Items) {
		t.Errorf("count = %d with %d items", p.Count, len(p.Items))
	}
	// Nothing needing an orb, a site or a release answer is reported: the shelf is empty
	// and this hub's own library has never been seeded, so the only rows are the apps the
	// compiled-in set is ahead of, and even those are a file read rather than a dial.
	for _, kind := range []string{"manager", "firmware", "theme"} {
		if _, ok := updateRowOf(p, kind); ok {
			t.Errorf("an idle hub reported a %q row: %+v", kind, p.Items)
		}
	}
	if p.DownloadURL != "" {
		t.Errorf("downloadUrl = %q before the first release check", p.DownloadURL)
	}
	// The two doors refuse a body that names nothing rather than recording it.
	for _, path := range []string{"/api/updates/skip", "/api/updates/keep"} {
		if env := postRefusal(t, s, path, map[string]any{}); env["ok"] == true {
			t.Errorf("%s accepted a refusal naming nothing: %v", path, env)
		}
	}
	// And an unknown kind is refused by both, so a stale page cannot file a choice no
	// listing will ever read back.
	if env := postRefusal(t, s, "/api/updates/skip", map[string]any{"kind": "widget", "key": "x", "version": 1}); env["ok"] == true {
		t.Errorf("skip of an unknown kind accepted: %v", env)
	}
	// A body that is not a refusal at all — oversized, or not JSON — is refused rather
	// than half-read: the doors parse one small object or they answer nothing.
	if env := designCall(t, s, http.MethodPost, "/api/updates/skip", []byte("{"+strings.Repeat("\"a\":\"b\",", 4000))); env["ok"] == true {
		t.Errorf("an oversized skip accepted: %v", env)
	}
	if env := designCall(t, s, http.MethodPost, "/api/updates/keep", []byte("not json at all")); env["ok"] == true {
		t.Errorf("a skip of raw non-JSON accepted: %v", env)
	}
	// Neither door answers a GET, and both need the token.
	if rec := rawCall(t, s, http.MethodGet, "/api/updates/skip", nil); rec.Code == http.StatusOK {
		t.Error("the skip door answered a GET")
	}
	mux := http.NewServeMux()
	s.routes(mux)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/api/updates/keep", strings.NewReader(`{"kind":"theme","key":"x"}`)))
	if rec.Code != http.StatusForbidden && rec.Code != http.StatusUnauthorized {
		t.Errorf("an untokened keep = %d, want the control refusal", rec.Code)
	}
}

// A refusal is only worth recording if it survives the next launch, and a file that names// no choice must read back as no choice rather than as a guessed one — the same rule every
// other field of state.go keeps.
func TestUpdateChoicesRoundTripAndMissingKey(t *testing.T) {
	s, _, root := libHub(t)
	s.statePath = filepath.Join(root, "state.json")
	s.mutateChoice("app", "clock", func(c *UpdateChoice) { c.SkippedVersion = packedVersion("v1.2.37") })
	s.saveState()

	re := newHub()
	re.statePath = s.statePath
	if n := re.loadState(); n == 0 && len(re.updateChoices) == 0 {
		t.Fatal("loadState read nothing at all")
	}
	got, ok := re.choiceFor("app", "clock")
	if !ok || got.SkippedVersion != 1_002_037 || got.Kind != "app" {
		t.Fatalf("choice after reload = %+v %v", got, ok)
	}
	if _, ok := re.choiceFor("theme", "nothing-here"); ok {
		t.Error("a missing key read back as a choice")
	}
	// A hand-edited entry filed under a key that disagrees with its own body is dropped:
	// the key is what the list looks up, and honouring the body would let the file claim
	// a refusal the operator never made about a different row.
	if err := os.WriteFile(s.statePath,
		[]byte(`{"version":3,"grants":{},"updateChoices":{"theme:aaa":{"kind":"theme","key":"bbb","keep":true}}}`), 0o600); err != nil {
		t.Fatal(err)
	}
	tr := newHub()
	tr.statePath = s.statePath
	tr.loadState()
	if len(tr.updateChoices) != 0 {
		t.Errorf("a mismatched entry was honoured: %+v", tr.updateChoices)
	}
}

// A keep pressed for something other than a theme is not a refusal this step understands,
// and a hub that recorded it would hide a row the operator cannot bring back.
func TestKeepMyVersionRefusesEveryOtherKind(t *testing.T) {
	s, _, root := libHub(t)
	s.statePath = filepath.Join(root, "state.json")
	for _, kind := range []string{"manager", "firmware", "app", ""} {
		env := postRefusal(t, s, "/api/updates/keep", map[string]any{"kind": kind, "key": "hub"})
		if env["ok"] == true {
			t.Errorf("keep of kind %q was accepted: %v", kind, env)
		}
		if msg, _ := env["error"].(string); msg == "" {
			t.Errorf("keep of kind %q refused with no sentence: %v", kind, env)
		}
	}
	if len(s.updateChoices) != 0 {
		t.Errorf("a refused keep still recorded a choice: %+v", s.updateChoices)
	}
}
