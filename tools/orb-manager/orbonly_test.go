package main

// h3-design.md §7.3, hub step 4: themes that live only on an orb are found by card, asked by
// name, and adopted through the existing apply path. The claims here are mostly about which
// doors were knocked on and what survived afterwards — the request counts off nameSite, the
// card rows off orbfake, the shelf sidecars off disk, and the ledger files under the hub's own
// config dir — because the step is a policy about asking as much as a flow about installing.

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// orbOnlyHub is the fixture every apply test drives: a hub with a live fake orb wearing the
// given card rows, a real shelf in a temp dir, its state file (and so its ledger dir) beside
// it, the fake site configured as the release base, and the bake seam in the shape the browser
// baker answers — test 4 clears that seam to become the release binary's hub.
func orbOnlyHub(t *testing.T, u *nameSite, themes ...orbfake.Theme) (*Server, string, *orbfake.Orb, string) {
	t.Helper()
	fake := orbfake.New(themes...)
	fake.SetFirmware("v1.6.4+0d9a87b")
	s, ip, _, orb := fakeOrbHubWith(t, fake)
	root := t.TempDir()
	s.lib = &Library{AppsDir: filepath.Join(root, "apps"), ThemesDir: filepath.Join(root, "themes")}
	if err := os.MkdirAll(s.lib.ThemesDir, 0o755); err != nil {
		t.Fatal(err)
	}
	s.statePath = filepath.Join(root, "state.json")
	if u != nil {
		t.Setenv("GORB_SITE", u.url())
	}
	s.themeBake = func(_ []byte, shared string) ([]byte, error) {
		return withDesign(orbfake.Container(shared), string(designDoc(shared, "v1.2.30", 3))), nil
	}
	return s, ip, orb, root
}

// orbOnlyWorn is the card row §7.3 stands in for: a theme pushed by hand, stored and worn, on
// the app the fixtures bake for.
func orbOnlyWorn(name string) orbfake.Theme {
	return orbfake.Theme{Name: name, App: "clock", Bytes: 99, Serial: 1, OK: true, Stored: true, Active: true}
}

// orbOnlyCardHub answers the card walk by hand rather than through orbfake, because the blank
// app test 2 needs is firmware too old to answer the app — and orbfake seeds an empty app as
// clock's. It serves one fixed theme list to GET /api/v1/sd and counts the reads it takes,
// under its own lock, as every double here does.
func orbOnlyCardHub(t *testing.T, sd string) *Server {
	t.Helper()
	var mu sync.Mutex
	hits := 0
	orb := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/api/v1/sd" {
			mu.Lock()
			hits++
			mu.Unlock()
			w.Header().Set("Content-Type", "application/json")
			_, _ = fmt.Fprint(w, sd)
			return
		}
		http.NotFound(w, r)
	}))
	t.Cleanup(orb.Close)
	hp := strings.TrimPrefix(orb.URL, "http://")
	i := strings.LastIndex(hp, ":")
	if i < 0 {
		t.Fatalf("test orb URL %q has no port", orb.URL)
	}
	s := newHub()
	s.device = orbnet.NewClient(2 * time.Second)
	s.device.DevicePort = hp[i+1:]
	s.gate = orbnet.NewDialGate()
	s.grants.learn(hp[:i])
	root := t.TempDir()
	s.lib = &Library{AppsDir: filepath.Join(root, "apps"), ThemesDir: filepath.Join(root, "themes")}
	if err := os.MkdirAll(s.lib.ThemesDir, 0o755); err != nil {
		t.Fatal(err)
	}
	s.statePath = filepath.Join(root, "state.json")
	return s
}

// orbOnlyRows calls the enumeration directly: what the GET door answers, minus the envelope.
func orbOnlyRows(t *testing.T, s *Server) []orbOnlyTheme {
	t.Helper()
	rows, err := s.siteOrbOnlyThemes(context.Background())
	if err != nil {
		t.Fatalf("siteOrbOnlyThemes: %v", err)
	}
	return rows
}

// wantLedgerEmpty proves the ledger wrote nothing: no directory, or none with a .json in it.
// "No entry" is not the claim — a hub that answered NotYet was promised not to create the file.
func wantLedgerEmpty(t *testing.T, s *Server) {
	t.Helper()
	files, err := filepath.Glob(filepath.Join(filepath.Dir(s.statePath), "site-orb", "*.json"))
	if err != nil || len(files) != 0 {
		t.Errorf("the ledger holds %v; this pass was supposed to write nothing", files)
	}
}

// orbOnlyDoor answers POST /api/site/orb-themes/adopt through the router the binary serves.
func orbOnlyDoor(t *testing.T, s *Server, body any) map[string]any {
	t.Helper()
	b, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	return designCall(t, s, http.MethodPost, "/api/site/orb-themes/adopt", b)
}

// TestOrbOnlyEnumeratesCards is the walk: a name only an orb's card carries is enumerated, with
// the orbs named as orbWord names them, while a library theme with the same shape is not — a
// tracked theme is step 3's business, and the two paths meeting on the Updates list is why.
func TestOrbOnlyEnumeratesCards(t *testing.T) {
	u := nameSiteStand(t, 6, "Clockface by @greg2")
	s, ip, _, _ := orbOnlyHub(t, u, orbOnlyWorn("Clockface by @greg"))
	shelfThemeFile(t, s, "Skeleton by @g")

	rows := orbOnlyRows(t, s)
	if len(rows) != 1 {
		t.Fatalf("the enumeration drew %d rows, want the one card that is not tracked: %+v", len(rows), rows)
	}
	r := rows[0]
	if r.Name != "Clockface by @greg" || r.App != "clock" || r.Gone {
		t.Errorf("the row is not the card: %+v", r)
	}
	if len(r.Orbs) != 1 || r.Orbs[0] != s.orbWord(ip) {
		t.Errorf("the row does not name the orb wearing it as orbWord names it: %v", r.Orbs)
	}

	// The door and the pass agree, envelope included.
	env := designCall(t, s, http.MethodGet, "/api/site/orb-themes", nil)
	if env["ok"] != true {
		t.Fatalf("GET /api/site/orb-themes: %v", env)
	}
	list, _ := env["data"].(map[string]any)["themes"].([]any)
	if len(list) != 1 {
		t.Fatalf("the door drew %v, want the one row", env["data"])
	}

	// And the ask went out by name and app, to the configured site: the card is not tracked,
	// so a gone answer is the only news the pass could act on, and acting on it needs asking.
	ask, ok := u.lastName()
	if !ok || ask.query.Get("name") != "Clockface by @greg" || ask.query.Get("app") != "clock" {
		t.Fatalf("the fake saw no by-name ask for the card: %+v", ask.query)
	}
}

// TestOrbOnlySkipsHomemadeAndBlankApp is the discretion gate, counted at the site: a homemade
// name, a headless handle, and a row whose device will not name an app none dial anything. A
// hub that dialled once a refresh about `My Doodad` would be spamming a stranger's site about
// the operator's own file.
func TestOrbOnlySkipsHomemadeAndBlankApp(t *testing.T) {
	u := nameSiteStand(t, 6, "whatever by @g")
	s := orbOnlyCardHub(t, `{"mounted":true,"card":"SDSC-8G","themes":[
		{"name":"My Doodad","app":"clock","id":"01020304","ok":true,"stored":true},
		{"name":" by @greg","app":"clock","id":"05060708","ok":true,"stored":true},
		{"name":"Fine by @g","app":"","id":"090a0b0c","ok":true,"stored":true}]}`)
	t.Setenv("GORB_SITE", u.url())

	if rows := orbOnlyRows(t, s); len(rows) != 0 {
		t.Errorf("the enumeration drew %v; none of those names is askable", rows)
	}
	if u.nameAsks()+u.idAsks() != 0 {
		t.Errorf("the fake saw %d asks; the gate was supposed to send none", u.nameAsks()+u.idAsks())
	}
	wantLedgerEmpty(t, s)

	// The press path draws the same gate: a name the site could never have written is refused
	// before anything is dialled, by the door and not only by the walk.
	if got := applyErr(t, orbOnlyDoor(t, s, map[string]any{"name": "My Doodad"})); got != siteLinkRefused {
		t.Errorf("adopting a homemade name refused with %q, want the refuse sentence", got)
	}
	if u.nameAsks() != 0 {
		t.Errorf("the press asked %d times about a name the site could never have written", u.nameAsks())
	}
	wantLedgerEmpty(t, s)
}

// TestOrbOnlyAdoptInstallsAndRemovesOldName is G3's apply assertions on the adopt door: the
// answer is the apply's own payload, the shelf gains the theme with the identity the answer
// named, the orb wears the new name and gives up the old one AFTER the select, and the ledger
// entry a finished adoption makes unnecessary does not outlive it.
func TestOrbOnlyAdoptInstallsAndRemovesOldName(t *testing.T) {
	u := nameSiteStand(t, 6, "Clockface by @greg2")
	s, ip, orb, _ := orbOnlyHub(t, u, orbOnlyWorn("Clockface by @greg"))
	oldID := ""
	for _, th := range orb.Themes() {
		if th.Name == "Clockface by @greg" {
			oldID = th.ID
		}
	}
	if oldID == "" {
		t.Fatal("the fixture orb does not wear the old name")
	}

	d := applyData(t, orbOnlyDoor(t, s, map[string]any{"name": "Clockface by @greg"}))
	if d["oldName"] != "Clockface by @greg" || d["newName"] != "Clockface by @greg2" || d["renamed"] != true {
		t.Errorf("the answer does not name the swap: %v", d)
	}
	if on, _ := d["installedOn"].([]any); len(on) != 1 || on[0] != ip {
		t.Errorf("installedOn is not the one orb that was wearing it: %v", d["installedOn"])
	}
	if _, bad := d["needBake"]; bad {
		t.Errorf("a hub with a baker answered NeedBake: %v", d)
	}

	calls := orb.Calls()
	upload := callFind(calls, "POST /api/v1/sd/theme", "overwrite=1")
	del := callFind(calls, "POST /api/v1/sd/delete?id=")
	if upload < 0 || del < 0 {
		t.Fatalf("the adoption did not walk upload → delete: %v", calls)
	}
	if upload > del {
		t.Errorf("the old card copy was deleted at %d before the new one was written at %d", del, upload)
	}
	if !strings.Contains(calls[del], oldID) {
		t.Errorf("the delete did not name the old card row: %s", calls[del])
	}
	if strings.Contains(calls[upload], "keep=1") {
		t.Error("an orb wearing the old theme was asked to keep the new one unselected: an adoption must move its face")
	}
	rows := orb.Themes()
	if len(rows) != 1 || rows[0].Name != "Clockface by @greg2" || !rows[0].Active {
		t.Errorf("the orb does not wear the adopted theme: %+v", rows)
	}

	files := shelfFiles(t, s.lib.ThemesDir)
	if len(files) != 1 {
		t.Fatalf("the shelf holds %v, want the one adopted theme", files)
	}
	o := readThemeOrigin(files[0])
	if o == nil || o.DesignID != u.designID || o.Version != 6 || o.Site != u.url() || o.ImportedAt == 0 {
		t.Errorf("the shelf theme's origin record is not the answer the site gave: %+v", o)
	}

	wantLedgerEmpty(t, s)
	if rows := orbOnlyRows(t, s); len(rows) != 0 {
		t.Errorf("the adopted theme is still enumerated: %+v", rows)
	}
}

// TestOrbOnlyAdoptNeedBakeKeepsLedger is the release binary's answer — the baker is the
// operator's own editor door away — and the ledger entry is what makes the unfinished adoption
// survive the restart between the hold and the browser's press: the settle runs off disk.
func TestOrbOnlyAdoptNeedBakeKeepsLedger(t *testing.T) {
	u := nameSiteStand(t, 6, "Clockface by @greg2")
	s, ip, orb, _ := orbOnlyHub(t, u, orbOnlyWorn("Clockface by @greg"))
	s.themeBake = nil

	d := applyData(t, orbOnlyDoor(t, s, map[string]any{"name": "Clockface by @greg"}))
	if d["needBake"] != true || d["app"] != "clock" {
		t.Fatalf("the answer is not an import-shaped bake hold: %v", d)
	}
	if d["oldName"] != "Clockface by @greg" || d["newName"] != "Clockface by @greg2" {
		t.Errorf("the held bake does not name the swap: %v", d)
	}
	tk, _ := d["ticket"].(string)
	if tk == "" {
		t.Fatalf("no ticket on the answer: %v", d)
	}
	e := s.orbOnlyReadLedger("Clockface by @greg")
	if e == nil || e.Gone || e.DesignID != u.designID || e.Version != 6 || e.App != "clock" || e.AskedAt == 0 {
		t.Fatalf("the held adoption left no usable ledger entry: %+v", e)
	}

	// The editor's keep lands the bake the ticket held, sidecar and all — and the settle reads
	// its whole state off disk: the ledger entry is the only thing that connected this press to
	// the hold, which is exactly what makes the adoption survive a restart in between.
	env := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+tk,
		withDesign(orbfake.Container("Clockface by @greg2"), string(designDoc("Clockface by @greg2", "v1.2.30", 1))))
	if env["ok"] != true {
		t.Fatalf("the editor's keep refused: %v", env)
	}
	d2 := applyData(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": "Clockface by @greg", "baked": "Clockface by @greg2"}))
	if d2["oldName"] != "Clockface by @greg" || d2["newName"] != "Clockface by @greg2" || d2["renamed"] != true {
		t.Errorf("the settle answer does not name the swap: %v", d2)
	}
	if on, _ := d2["installedOn"].([]any); len(on) != 1 || on[0] != ip {
		t.Errorf("the settle installed on %v, want the one worn copy", d2["installedOn"])
	}
	rows := orb.Themes()
	if len(rows) != 1 || rows[0].Name != "Clockface by @greg2" || !rows[0].Active {
		t.Errorf("the orb does not wear the settled theme: %+v", rows)
	}
	wantLedgerEmpty(t, s)
	if rows := orbOnlyRows(t, s); len(rows) != 0 {
		t.Errorf("the settled theme is still enumerated: %+v", rows)
	}
}

// answerLive moves a nameSite back to a plain 200 after answerGone — the un-retirement, which
// is the half of test 5 that shows a cleared mark means ASKING AGAIN and not holding a grudge.
func (u *nameSite) answerLive() {
	u.mu.Lock()
	defer u.mu.Unlock()
	u.status, u.goneBody, u.notFound = http.StatusOK, false, nil
}

// TestOrbOnlyGoneMarksAndStaysQuiet is review 5's policy on the one answer this step cannot
// hold in memory: gone is news about a name with no shelf file to mark, so the mark is the
// ledger, the ledger is the silence, and the one thing that ends the silence is the library
// theme that outlived it being removed.
func TestOrbOnlyGoneMarksAndStaysQuiet(t *testing.T) {
	u := nameSiteStand(t, 6, "Clockface by @greg2")
	s, _, _, _ := orbOnlyHub(t, u, orbOnlyWorn("Clockface by @greg"))
	u.answerGone(http.StatusGone)

	rows := orbOnlyRows(t, s)
	if len(rows) != 1 || !rows[0].Gone || rows[0].Name != "Clockface by @greg" || len(rows[0].Orbs) != 0 {
		t.Fatalf("a gone answer drew %v, want the one name with no orbs", rows)
	}
	if e := s.orbOnlyReadLedger("Clockface by @greg"); e == nil || !e.Gone || e.App != "clock" || e.AskedAt == 0 {
		t.Fatalf("the gone mark is not on disk: %+v", e)
	}
	asks := u.nameAsks()
	if asks != 1 {
		t.Fatalf("the first enumeration asked %d times, want the one ask that learned the news", asks)
	}

	// Two further enumerations: the same row, and no further dial. A once-public name is not
	// re-asked every minute — that sentence is why this ledger exists.
	orbOnlyRows(t, s)
	orbOnlyRows(t, s)
	if u.nameAsks() != asks {
		t.Errorf("the marked name was asked %d more times; the mark was supposed to end the asking", u.nameAsks()-asks)
	}
	if rows := orbOnlyRows(t, s); len(rows) != 1 || !rows[0].Gone {
		t.Errorf("the marked name drew %v, want the same gone row", rows)
	}
	if got := applyErr(t, orbOnlyDoor(t, s, map[string]any{"name": "Clockface by @greg"})); got != siteLinkRefused {
		t.Errorf("a press on a marked name refused with %q, want the refuse sentence", got)
	}
	if u.nameAsks() != asks {
		t.Errorf("the press asked about a marked name, which the mark forbids")
	}

	// The operator shelves a copy by hand — tracked now, so the walk passes it by without
	// asking — and the library door that removes it takes the mark with it.
	u.moveTo(6, "Clockface by @greg")
	u.answerLive()
	shelf := shelfThemeFile(t, s, "Clockface by @greg")
	if rows := orbOnlyRows(t, s); len(rows) != 0 {
		t.Fatalf("the tracked theme drew %v, want nothing", rows)
	}
	if u.nameAsks() != asks {
		t.Errorf("the walk asked again about a tracked theme")
	}
	base := strings.TrimSuffix(filepath.Base(shelf), ".orbtheme")
	env := designCall(t, s, http.MethodPost, "/api/library/rm?kind=theme&name="+url.QueryEscape(base), nil)
	if env["ok"] != true {
		t.Fatalf("library/rm refused: %v", env)
	}
	wantLedgerEmpty(t, s)

	// Fresh candidate: the next enumeration asks, the site says live again, and the row is
	// back with its orbs and a button.
	rows = orbOnlyRows(t, s)
	if u.nameAsks() != asks+1 {
		t.Errorf("after removing the shelf copy the enumerations asked %d times, want one fresh ask", u.nameAsks()-asks)
	}
	if len(rows) != 1 || rows[0].Gone || rows[0].Name != "Clockface by @greg" || len(rows[0].Orbs) != 1 {
		t.Errorf("the candidate did not come back: %+v", rows)
	}
}

// TestOrbOnlySilentOnNotYet is the other half of the ledger's rule: everything that is not
// gone-marked news writes nothing, and asks again next time. A plain not-found — the site with
// no resolve door — and a site that will not answer at all are the same silence, counted.
func TestOrbOnlySilentOnNotYet(t *testing.T) {
	u := nameSiteStand(t, 6, "Clockface by @greg2")
	s, _, _, _ := orbOnlyHub(t, u, orbOnlyWorn("Clockface by @greg"))
	u.answerPlain(http.StatusNotFound, "text/html", "<h1>404</h1>")

	rows := orbOnlyRows(t, s)
	if len(rows) != 1 || rows[0].Gone {
		t.Fatalf("a NotYet enumeration drew %v, want the same candidate unmarked", rows)
	}
	wantLedgerEmpty(t, s)

	logged := swapHubLog(t)
	got := applyErr(t, orbOnlyDoor(t, s, map[string]any{"name": "Clockface by @greg"}))
	if got != siteLinkRefused {
		t.Errorf("a press into a site with no door refused with %q, want the refuse sentence", got)
	}
	wantLedgerEmpty(t, s)
	if !strings.Contains(logged.String(), "no answer about it yet") {
		t.Errorf("the press logged %q, want the one sentence saying the site had no answer", logged.String())
	}

	// Ask again next time is the whole state: the next enumeration dials, and the count is
	// the proof.
	before := u.nameAsks()
	orbOnlyRows(t, s)
	if u.nameAsks() != before+1 {
		t.Errorf("the enumeration after a NotYet asked %d times, want the same one ask again", u.nameAsks()-before)
	}
	wantLedgerEmpty(t, s)

	// A site that will not answer at all: same rows, same nothing written, one logged line.
	logged = swapHubLog(t)
	t.Setenv("GORB_SITE", "http://127.0.0.1:1")
	env := designCall(t, s, http.MethodGet, "/api/site/orb-themes", nil)
	if env["ok"] != true {
		t.Fatalf("the door failed on a site it could not reach: %v", env)
	}
	list, _ := env["data"].(map[string]any)["themes"].([]any)
	if len(list) != 1 {
		t.Fatalf("an unreachable site drew %v, want the candidate standing unasked", env["data"])
	}
	wantLedgerEmpty(t, s)
	if !strings.Contains(logged.String(), "could not reach") {
		t.Errorf("the unreachable ask logged %q, want the transport sentence", logged.String())
	}
}

// TestOrbOnlyAdoptRefusesUnknownName is the door's own gate: the name it will act on is a name
// the current enumeration just drew, and every press that fails that test costs no dial, no
// ledger file, and no shelf write — the button came from the list, and a request the list did
// not draw is not a press on a button.
func TestOrbOnlyAdoptRefusesUnknownName(t *testing.T) {
	u := nameSiteStand(t, 6, "Clockface by @greg2")
	s, _, _, _ := orbOnlyHub(t, u, orbOnlyWorn("baseface"))

	for _, body := range []any{
		map[string]any{"name": "Notmine by @x"}, // well-formed, and not on any card
		map[string]any{},                        // an empty press
		map[string]any{"name": ""},
		map[string]any{"name": "   "},
		map[string]any{"name": " by @greg"}, // a handle with no name
		map[string]any{"name": "My Doodad"}, // homemade: no handle to ask with
		map[string]any{"name": "Bad/by @x"}, // a name the character rule refuses
	} {
		if got := applyErr(t, orbOnlyDoor(t, s, body)); got != siteLinkRefused {
			t.Errorf("adopt %v refused with %q, want the refuse sentence", body, got)
		}
	}
	rec := rawCall(t, s, http.MethodPost, "/api/site/orb-themes/adopt", []byte("{not json"))
	var env map[string]any
	if json.Unmarshal(rec.Body.Bytes(), &env) != nil || env["ok"] != false || env["error"] != siteLinkRefused {
		t.Errorf("a press that is not JSON refused with %s", rec.Body)
	}
	if u.nameAsks()+u.idAsks() != 0 {
		t.Errorf("the refused presses asked the fake %d times, want zero", u.nameAsks()+u.idAsks())
	}
	wantLedgerEmpty(t, s)
	if files := shelfFiles(t, s.lib.ThemesDir); len(files) != 0 {
		t.Errorf("a refused press filed %v on the shelf", files)
	}
}
