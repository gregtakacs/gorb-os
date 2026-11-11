package main

// g3-3's apply door, end to end: a shelf holding one theme imported from a site that has
// since moved it, one orb wearing that theme, and the bake the hub was told to do. Every
// claim here is checked on the wire the hub dialed and on the disk it wrote, because the
// thing under test is an ORDER — library, orbs, removals — and an order is only visible in
// what got called when.

import (
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
)

// newAurora is the card row an orb wears (or only holds) before the update: the imported
// name, on the app the fixtures bake for.
func newAurora(worn bool) orbfake.Theme {
	return orbfake.Theme{Name: "Aurora", App: "clock", Bytes: 99, Serial: 1, OK: true,
		Stored: true, Active: worn}
}

// applyHub is the fixture every apply test drives: a hub with a real shelf and a real fake
// orb, holding "Aurora" imported from a site at v1, which now answers resolve with v2 under
// `to`. The bake seam is the test's, in the shape the browser baker answers: the container the
// shared name names, with the design riding it. worn decides whether the orb WEARS the theme
// or only has it on its card, and the firmware is v1.6.4 so keep=1 means what it says.
func applyHub(t *testing.T, to string, worn bool) (*Server, *orbfake.Orb, string, *updateSite, string) {
	t.Helper()
	u := updatesSite(t, 1, "Aurora", "art")
	fake := orbfake.New(newAurora(worn))
	fake.SetFirmware("v1.6.4+0d9a87b")
	s, ip, _, orb := fakeOrbHubWith(t, fake)
	root := t.TempDir()
	s.lib = &Library{AppsDir: filepath.Join(root, "apps"), ThemesDir: filepath.Join(root, "themes")}
	if err := os.MkdirAll(s.lib.ThemesDir, 0o755); err != nil {
		t.Fatal(err)
	}
	s.themeBake = func(_ []byte, shared string) ([]byte, error) {
		return withDesign(orbfake.Container(shared), string(designDoc(shared, "v1.2.30", 3))), nil
	}
	old := importShelfTheme(t, s, u, "Aurora")
	u.moveTo(2, to)
	// g3-2's fixture answers the bundle door with the version it was stood up at; an apply
	// reads the bundle rather than only the resolve answer, so the fixture moves it too. The
	// bytes differ from v1's on purpose: an update that shipped the same file would prove
	// nothing about which bake landed on which card.
	u.bundle = func() []byte {
		return goodBundle(t, siteDesignID, 2, "clock", to, pngArt(9, 9, artSeed("art v2")))
	}
	return s, orb, ip, u, old
}

func applyOnce(t *testing.T, s *Server, body map[string]any) map[string]any {
	t.Helper()
	return postRefusal(t, s, "/api/updates/apply", body)
}

func applyData(t *testing.T, env map[string]any) map[string]any {
	t.Helper()
	if env["ok"] != true {
		t.Fatalf("apply refused: %v", env)
	}
	d, _ := env["data"].(map[string]any)
	if d == nil {
		t.Fatalf("apply answered with no data: %v", env)
	}
	return d
}

func applyErr(t *testing.T, env map[string]any) string {
	t.Helper()
	if env["ok"] == true {
		t.Fatalf("apply was accepted: %v", env)
	}
	e, _ := env["error"].(string)
	return e
}

// callFind answers the first index of a recorded request line carrying every part. The lines
// are what the double saw: "POST /api/v1/sd/bundle?id=…".
func callFind(calls []string, parts ...string) int {
	for i, l := range calls {
		hit := true
		for _, p := range parts {
			if !strings.Contains(l, p) {
				hit = false
				break
			}
		}
		if hit {
			return i
		}
	}
	return -1
}

func shelfFiles(t *testing.T, dir string) []string {
	t.Helper()
	out, err := filepath.Glob(filepath.Join(dir, "*.orbtheme"))
	if err != nil {
		t.Fatal(err)
	}
	return out
}

// originOf is a shelf file's origin record, with the test failing rather than a nil spreading
// into whichever assertion is next.
func originOf(t *testing.T, file string) *ThemeOrigin {
	t.Helper()
	o := readThemeOrigin(file)
	if o == nil {
		t.Fatalf("%s has no origin record", filepath.Base(file))
	}
	return o
}

// TestRenameUpdateSwapsAndKeepsSelected is the whole of §7.4 in one press: the design was
// renamed on the site, and after the update the NEW theme is on the orb and worn, the OLD one
// is off the orb and off the shelf, and nothing was removed before it landed.
func TestRenameUpdateSwapsAndKeepsSelected(t *testing.T) {
	s, orb, ip, _, old := applyHub(t, "Aurora Fields", true)
	oldID := ""
	for _, th := range orb.Themes() {
		if th.Name == "Aurora" {
			oldID = th.ID
		}
	}
	if oldID == "" {
		t.Fatal("the fixture orb does not hold the old theme at all")
	}

	d := applyData(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID}))
	if d["oldName"] != "Aurora" || d["newName"] != "Aurora Fields" || d["renamed"] != true {
		t.Errorf("the answer does not name the swap: %v", d)
	}
	if on, _ := d["installedOn"].([]any); len(on) != 1 || on[0] != ip {
		t.Errorf("installedOn is not the one orb that was wearing it: %v", d["installedOn"])
	}

	calls := orb.Calls()
	upload := callFind(calls, "POST /api/v1/sd/theme", "overwrite=1")
	del := callFind(calls, "POST /api/v1/sd/delete?id=")
	if upload < 0 || del < 0 {
		t.Fatalf("the swap did not walk upload → delete: %v", calls)
	}
	// The store is not a call this makes: the hub's theme-push door uploads with
	// ?overwrite=1 and the DEVICE queues the store that follows a verified bundle
	// (h_theme_upload's post_bundle tail), which is what selects it. The double lands that
	// queue synchronously, so the worn row below is the store's witness, and the wire's
	// witness that nothing was removed early is this:
	if upload > del {
		t.Errorf("the old theme was deleted at %d before the new one was written at %d: %v", del, upload, calls)
	}
	if !strings.Contains(calls[del], oldID) {
		t.Errorf("the delete did not name the old theme's id %s: %s", oldID, calls[del])
	}
	if strings.Contains(calls[upload], "keep=1") {
		t.Error("an orb wearing the old theme was asked to keep the new one unselected: a rename must move its face")
	}

	// The shelf, after: the new bake stands at v2 with its own origin, and neither the old
	// file nor its sidecar is left to be mistaken for a theme the hub still tracks.
	files := shelfFiles(t, s.lib.ThemesDir)
	if len(files) != 1 {
		t.Fatalf("the shelf holds %d themes, not the one the update left: %v", len(files), files)
	}
	if filepath.Base(files[0]) == filepath.Base(old) {
		t.Error("the only shelf file is still the old-named one")
	}
	o := readThemeOrigin(files[0])
	if o == nil || o.Version != 2 || o.DesignID != siteDesignID {
		t.Errorf("the new shelf copy has no v2 origin for the design: %+v", o)
	}
	if _, err := os.Stat(old); !os.IsNotExist(err) {
		t.Errorf("the old shelf file is still there: %v", err)
	}
	if readThemeOrigin(old) != nil {
		t.Error("the old theme's sidecar outlived its file")
	}
	rows := orb.Themes()
	if len(rows) != 1 || rows[0].Name != "Aurora Fields" || !rows[0].Active {
		t.Errorf("the orb does not wear the new theme: %+v", rows)
	}
}

// TestApplyUpdateRemovalFailureSaysTheOldCopyStands is step 3's failure, the one outcome of a
// rename that is neither a refusal nor a clean finish: the new theme is installed and worn, the
// old copy could not be removed, and the answer says which orb still carries it rather than
// pretending the run stopped half-way. The dangerous direction — a card left with nothing to
// wear — is what this pins shut, and the shelf keeping its old file with its sidecar is what
// leaves the row something to press again.
func TestApplyUpdateRemovalFailureSaysTheOldCopyStands(t *testing.T) {
	s, orb, ip, _, old := applyHub(t, "Aurora Fields", true)
	before := s.device.DevicePort
	stubborn := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost && strings.HasPrefix(r.URL.Path, "/api/v1/sd/delete") {
			http.Error(w, `{"error":"the card will not hear of it"}`, http.StatusInternalServerError)
			return
		}
		orb.ServeHTTP(w, r)
	}))
	t.Cleanup(stubborn.Close)
	hp := strings.TrimPrefix(stubborn.URL, "http://")
	s.device.DevicePort = hp[strings.LastIndex(hp, ":")+1:]

	d := applyData(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID}))
	note, _ := d["note"].(string)
	if !strings.Contains(note, "manual remove") || !strings.Contains(note, ip) {
		t.Errorf("a stuck removal was not said as one, naming the orb: %q", note)
	}
	if on, _ := d["installedOn"].([]any); len(on) != 1 || on[0] != ip {
		t.Errorf("the new bake did not count as installed on the orb that got it: %v", d["installedOn"])
	}
	if _, err := os.Stat(old); err != nil {
		t.Errorf("the old shelf file went away when its removal failed: %v", err)
	}
	if o := originOf(t, old); o.Version != 1 {
		t.Errorf("the stranded copy's origin moved to v%d, so the retry has no version to compare against", o.Version)
	}
	worn, holds := "", 0
	for _, r := range orb.Themes() {
		if r.Active {
			worn = r.Name
		}
		if r.Name == "Aurora" || r.Name == "Aurora Fields" {
			holds++
		}
	}
	if worn != "Aurora Fields" {
		t.Errorf("the orb was left wearing %q while the rename half-failed: %+v", worn, orb.Themes())
	}
	if holds != 2 {
		t.Errorf("the card holds %d of the old and new copies, and the half-landed state is the point: %+v",
			holds, orb.Themes())
	}

	// The retry: with the card answering again, the same press on the same row finishes what the
	// note asked for, quietly, and the shelf is left holding one theme.
	s.device.DevicePort = before
	if n := applyData(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID}))["note"]; n != nil && n != "" {
		t.Errorf("the retry left a note behind: %v", n)
	}
	if _, err := os.Stat(old); !os.IsNotExist(err) {
		t.Errorf("the retry did not finish the removal: %v", err)
	}
	if rows := orb.Themes(); len(rows) != 1 || rows[0].Name != "Aurora Fields" || !rows[0].Active {
		t.Errorf("the retry left the card oddly dressed: %+v", rows)
	}
}

// TestRenameUpdateFailedInstallLeavesOldTheme is the gate the whole design turns on: a store
// that tore means the removals never happen. The orb still holds and wears the old theme, the
// shelf still holds its file and its sidecar, and the refusal names the orb.
func TestRenameUpdateFailedInstallLeavesOldTheme(t *testing.T) {
	s, orb, ip, _, old := applyHub(t, "Aurora Fields", true)
	orb.TearStore = true

	msg := applyErr(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID}))
	if !strings.Contains(msg, ip) {
		t.Errorf("the refusal does not name the orb that failed: %q", msg)
	}
	calls := orb.Calls()
	if callFind(calls, "POST /api/v1/sd/delete") >= 0 {
		t.Errorf("a failed install was followed by a removal: %v", calls)
	}
	if _, err := os.Stat(old); err != nil {
		t.Errorf("the old shelf file went away with the update failing: %v", err)
	}
	o := readThemeOrigin(old)
	if o == nil {
		t.Fatal("the sidecar went with it, so nothing is left to offer the retry")
	}
	if o.Version != 1 {
		t.Errorf("the origin moved to v%d while the update failed: the row claims a version it does not have", o.Version)
	}
	worn := ""
	for _, r := range orb.Themes() {
		if r.Active {
			worn = r.Name
		}
	}
	if worn != "Aurora" {
		t.Errorf("the orb stopped wearing the theme it was wearing: %q, rows %+v", worn, orb.Themes())
	}
}

// TestRenameUpdateNotWornDoesNotSelect: a card holding the old theme unchosen is filed with
// the new one and left wearing what it wore. An update that puts a face on an orb whose owner
// chose a different one is worse than an update that skipped it.
func TestRenameUpdateNotWornDoesNotSelect(t *testing.T) {
	s, orb, ip, _, _ := applyHub(t, "Aurora Fields", false)

	d := applyData(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID}))
	if on, _ := d["installedOn"].([]any); len(on) != 1 || on[0] != ip {
		t.Errorf("an orb holding the old theme was left out of installedOn: %v", d)
	}
	calls := orb.Calls()
	up := callFind(calls, "POST /api/v1/sd/theme")
	if up < 0 {
		t.Fatalf("nothing was sent to the orb: %v", calls)
	}
	if !strings.Contains(calls[up], "keep=1") {
		t.Errorf("the unchosen copy was pushed without keep=1: %s", calls[up])
	}
	if callFind(calls, "POST /api/v1/sd/bundle?id=") >= 0 {
		t.Errorf("an unchosen theme was stored, which selects it: %v", calls)
	}
	if callFind(calls, "POST /api/v1/sd/delete") < 0 {
		t.Error("the rename left the old copy on a card that has been given the new one")
	}
	if n := d["note"]; n != nil && n != "" {
		t.Errorf("a clean file is not a half-landed one: %v", d)
	}
	rows := orb.Themes()
	if len(rows) != 1 || rows[0].Name != "Aurora Fields" {
		t.Errorf("the card does not hold the new bake: %+v", rows)
	}
	if rows[0].Active {
		t.Errorf("the filed copy became the orb's face: %+v", rows)
	}
}

// TestOrdinaryUpdateSameName is §7.6's plain case: a newer version of the same design under
// the same name. The card copy is replaced under the id it already had, so there is nothing to
// delete and nothing to rename, and the shelf file is the same file at a new version.
func TestOrdinaryUpdateSameName(t *testing.T) {
	s, orb, ip, _, old := applyHub(t, "Aurora", true)

	d := applyData(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID}))
	if d["renamed"] != false || d["newName"] != "Aurora" {
		t.Errorf("a same-name update reported a rename: %v", d)
	}
	if on, _ := d["installedOn"].([]any); len(on) != 1 || on[0] != ip {
		t.Errorf("installedOn is not the orb that wears it: %v", d)
	}
	calls := orb.Calls()
	if callFind(calls, "POST /api/v1/sd/theme", "overwrite=1") < 0 {
		t.Errorf("the card's older copy was not replaced: %v", calls)
	}
	if callFind(calls, "POST /api/v1/sd/delete") >= 0 {
		t.Errorf("a same-name update deleted the theme it had just replaced: %v", calls)
	}
	if _, err := os.Stat(old); err != nil {
		t.Errorf("the shelf file went away on a same-name update: %v", err)
	}
	o := readThemeOrigin(old)
	if o == nil || o.Version != 2 {
		t.Errorf("the shelf's copy is tracked at %+v, not at v2", o)
	}
	if files := shelfFiles(t, s.lib.ThemesDir); len(files) != 1 {
		t.Errorf("a same-name update left %d files on the shelf: %v", len(files), files)
	}
}

// TestApplyUpdateBakeFailureAborts is step 1's failure: the bake refused, so the library has
// not changed and no orb has been dialed at all. The refusal is the baker's own sentence — the
// hub does not guess at what the editor would have said.
func TestApplyUpdateBakeFailureAborts(t *testing.T) {
	s, orb, _, _, old := applyHub(t, "Aurora Fields", true)
	const why = "this design was saved by a newer editor and this Orb Manager cannot open it"
	s.themeBake = func(_ []byte, _ string) ([]byte, error) { return nil, errors.New(why) }

	msg := applyErr(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID}))
	if !strings.Contains(msg, why) {
		t.Errorf("the refusal is not the baker's sentence: %q", msg)
	}
	if callFind(orb.Calls(), "POST /api/v1/sd/theme") >= 0 {
		t.Errorf("an orb was dialed after the bake refused: %v", orb.Calls())
	}
	if o := readThemeOrigin(old); o == nil || o.Version != 1 {
		t.Errorf("the shelf's origin changed although the update never got past the bake: %+v", o)
	}
	if files := shelfFiles(t, s.lib.ThemesDir); len(files) != 1 || filepath.Base(files[0]) != filepath.Base(old) {
		t.Errorf("the bake failure moved the shelf: %v", files)
	}
}

// TestApplyUpdateWrongBakedNameRefuses is the same gate from the other side: a bake that came
// back under a name other than the one the site shares is not the update that was offered.
func TestApplyUpdateWrongBakedNameRefuses(t *testing.T) {
	s, orb, _, _, old := applyHub(t, "Aurora Fields", true)
	s.themeBake = func(_ []byte, _ string) ([]byte, error) {
		return withDesign(orbfake.Container("Something Else"), string(designDoc("Something Else", "v1.2.30", 3))), nil
	}
	msg := applyErr(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID}))
	if !strings.Contains(msg, "Something Else") || !strings.Contains(msg, "Aurora Fields") {
		t.Errorf("the refusal does not name both the bake it got and the name that was offered: %q", msg)
	}
	if callFind(orb.Calls(), "POST /api/v1/sd/theme") >= 0 || callFind(orb.Calls(), "POST /api/v1/sd/delete") >= 0 {
		t.Errorf("a misnamed bake was installed anyway: %v", orb.Calls())
	}
	if _, err := os.Stat(old); err != nil {
		t.Errorf("the old theme went away with a refused bake: %v", err)
	}
}

// TestApplyUpdateGoneRowRefuses: the row on the page is stale, the design was retired since it
// was painted, and the press spends §7.4's sentence rather than destroying anything.
func TestApplyUpdateGoneRowRefuses(t *testing.T) {
	s, orb, _, u, old := applyHub(t, "Aurora Fields", true)
	u.answer(http.StatusGone)
	s.themeBake = func(_ []byte, _ string) ([]byte, error) {
		t.Error("a gone design was fetched and baked anyway")
		return nil, errors.New("unreachable")
	}
	msg := applyErr(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID}))
	if !strings.Contains(msg, "is no longer shared on orb-os.net; the theme stays, and Orb Manager will stop checking it") {
		t.Errorf("the gone refusal is not §7.4's sentence: %q", msg)
	}
	if callFind(orb.Calls(), "POST /api/v1/sd") >= 0 {
		t.Errorf("a refused apply touched a card: %v", orb.Calls())
	}
	if _, err := os.Stat(old); err != nil {
		t.Errorf("the shelf copy went away with the refusal: %v", err)
	}
}

// TestApplyUpdateNotNewerRefuses is the stale row that is not a retirement: the shelf already
// has the version the site is offering, which is exactly what the same row says one press after
// it landed, because the list the operator is reading was painted before it did.
func TestApplyUpdateNotNewerRefuses(t *testing.T) {
	s, orb, _, u, _ := applyHub(t, "Aurora Fields", true)
	applyData(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID}))
	s.themeBake = func(_ []byte, _ string) ([]byte, error) {
		t.Error("an offer the shelf already had was fetched and baked again")
		return nil, errors.New("unreachable")
	}
	u.moveTo(2, "Aurora Fields")

	msg := applyErr(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID}))
	if !strings.Contains(msg, "no newer version") {
		t.Errorf("the press did not say the site has nothing newer: %q", msg)
	}
	if got := strings.Count(strings.Join(orb.Calls(), "\n"), "POST /api/v1/sd/delete"); got != 1 {
		t.Errorf("the retired theme was deleted %d times, not once: %v", got, orb.Calls())
	}
	if files := shelfFiles(t, s.lib.ThemesDir); len(files) != 1 ||
		!strings.Contains(filepath.Base(files[0]), "aurora-fields") {
		t.Errorf("the second press moved the shelf: %v", files)
	}
}

// TestApplyUpdateInFlightRefusesSecondPress is the single lock: the door that dials orbs is
// open once at a time, and a second press is refused in the sentence that tells the operator to
// wait rather than queueing a second copy of itself.
func TestApplyUpdateInFlightRefusesSecondPress(t *testing.T) {
	s, _, _, _, _ := applyHub(t, "Aurora Fields", true)
	if !s.startApply() {
		t.Fatal("the door's own lock refused a press with nothing in flight")
	}
	env := applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID})
	if env["ok"] == true || !strings.Contains(applyErr(t, env), "already applying an update") {
		t.Errorf("a second press while one was in flight was not refused: %v", env)
	}
	s.endApply()
	if env := applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID}); env["ok"] != true {
		t.Errorf("the door stayed shut after the apply that held it finished: %v", env)
	}
}

// TestApplyUpdateRefusesAnotherKind: the apply door is about a theme from the site and nothing
// else. The amber per-tab buttons are the other updates' doors, and a press routed here would
// apply something nobody checked.
func TestApplyUpdateRefusesAnotherKind(t *testing.T) {
	s, orb, _, _, _ := applyHub(t, "Aurora Fields", true)
	for _, body := range []map[string]any{
		{"kind": "firmware", "key": "127.0.0.1"},
		{"kind": "app", "key": "clock"},
		{"kind": "theme", "key": ""},
		{"kind": "", "key": siteDesignID},
	} {
		msg := applyErr(t, applyOnce(t, s, body))
		if !strings.Contains(msg, "theme") {
			t.Errorf("%v was refused with something besides a word about themes: %q", body, msg)
		}
	}
	if callFind(orb.Calls(), "POST /api/v1/sd") >= 0 {
		t.Errorf("a refused press dialed a card anyway: %v", orb.Calls())
	}
}

// TestApplyUpdateNeedsTheEditorBake is the shipped binary's answer: main() leaves the bake seam
// nil, because its baker is a browser. The press fetches the new version, holds it under a
// ticket, and hands back the editor door's own arguments — the shape H1b's import already
// answers, so the page's existing path is what finishes the job. The second press, arriving with
// the name the editor kept, does steps 2 and 3 without fetching or baking again.
func TestApplyUpdateNeedsTheEditorBake(t *testing.T) {
	s, orb, _, _, old := applyHub(t, "Aurora Fields", true)
	s.themeBake = nil

	d := applyData(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID}))
	if d["needBake"] != true || d["ticket"] == "" || d["app"] != "clock" {
		t.Fatalf("the hold answer is not the editor door's: %v", d)
	}
	if d["newName"] != "Aurora Fields" || d["renamed"] != true {
		t.Errorf("the held answer does not say what the bake will be named: %v", d)
	}
	if callFind(orb.Calls(), "POST /api/v1/sd") >= 0 {
		t.Errorf("an orb was dialed before a bake existed: %v", orb.Calls())
	}
	if _, err := os.Stat(old); err != nil {
		t.Errorf("the update's own shelf copy went away while it waited for a bake: %v", err)
	}

	// H1b's own keep door, with the ticket the apply handed back: this is the editor's half.
	tk, _ := d["ticket"].(string)
	bake := withDesign(orbfake.Container("Aurora Fields"), string(designDoc("Aurora Fields", "v1.2.30", 3)))
	if env := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+tk, bake); env["ok"] != true {
		t.Fatalf("the editor's keep was refused: %v", env)
	}
	d2 := applyData(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID, "baked": "Aurora Fields"}))
	if d2["needBake"] == true || d2["renamed"] != true || d2["newName"] != "Aurora Fields" {
		t.Errorf("the resumed press did not finish the update: %v", d2)
	}
	if _, err := os.Stat(old); !os.IsNotExist(err) {
		t.Errorf("the resumed press left the old shelf file standing: %v", err)
	}
	rows := orb.Themes()
	if len(rows) != 1 || rows[0].Name != "Aurora Fields" || !rows[0].Active {
		t.Errorf("the resumed press left the orb wearing the retired theme: %+v", rows)
	}
}

// TestApplyUpdateBakedLieRefuses guards the resume: the name handed back must be this design at
// the version the site offered, or the press that claims to finish an update is installing
// whatever else is lying on the shelf under that name.
func TestApplyUpdateBakedLieRefuses(t *testing.T) {
	s, orb, _, u, old := applyHub(t, "Aurora Fields", true)
	s.themeBake = nil
	applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID})
	// A shelf file with the name and the design, still recorded at the OLD version: the bake
	// the page would hand back from an import that never landed.
	u.moveTo(2, "Aurora Fields")
	keep := importShelfTheme(t, s, u, "Aurora Fields")
	if o := readThemeOrigin(keep); o == nil || o.Version != 2 {
		t.Fatalf("the fixture keep recorded %+v", o)
	}
	if err := writeThemeOrigin(keep, &ThemeOrigin{Site: originOf(t, old).Site,
		DesignID: siteDesignID, Version: 1, ImportedAt: 1}); err != nil {
		t.Fatal(err)
	}
	msg := applyErr(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID, "baked": "Aurora Fields"}))
	if !strings.Contains(msg, "Aurora Fields") || !strings.Contains(msg, "version") {
		t.Errorf("the stale bake was refused without saying which name and why: %q", msg)
	}
	if callFind(orb.Calls(), "POST /api/v1/sd/delete") >= 0 {
		t.Errorf("a refused resume still removed the old theme: %v", orb.Calls())
	}
	if _, err := os.Stat(old); err != nil {
		t.Errorf("a refused resume destroyed the shelf copy: %v", err)
	}
}
