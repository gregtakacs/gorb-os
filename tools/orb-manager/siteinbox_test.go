package main

// H3 §7.1 hub step 2: the inbox client, run against a fake site that serves the §3.1
// list, the §3.3 pinned bundle and the §3.2 claim doors with counters a test can read, in
// the manner siteimport_test.go stands its site. The hold directory is real disk under a
// temp config dir, because every claim this file makes is about bytes on disk.

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
)

// inboxSite is the fake: the list it answers, the bundles it holds, and one counter per
// door, plus the claim answer a test wants written by hand.
type inboxSite struct {
	items         []siteInboxItem
	bundles       map[string][]byte
	listStatus    int // 0 means 200
	deliverFunc   func(ids []string) (delivered, missing []string)
	listCalls     int
	bundleCalls   int
	deliverCalls  int
	deliverBodies []map[string]any
}

func (f *inboxSite) handler() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet && r.URL.Path == "/api/hub/inbox":
			f.listCalls++
			if f.listStatus != 0 && f.listStatus != http.StatusOK {
				w.WriteHeader(f.listStatus)
				_, _ = w.Write([]byte(`{"error":"nope"}`))
				return
			}
			items := f.items
			if items == nil {
				items = []siteInboxItem{}
			}
			_ = json.NewEncoder(w).Encode(map[string]any{"items": items})
		case r.Method == http.MethodGet && strings.HasPrefix(r.URL.Path, "/api/hub/inbox/"):
			id := strings.TrimSuffix(strings.TrimPrefix(r.URL.Path, "/api/hub/inbox/"), "/bundle")
			if body, ok := f.bundles[id]; ok {
				f.bundleCalls++
				_, _ = w.Write(body)
				return
			}
			w.WriteHeader(http.StatusNotFound)
			_, _ = w.Write([]byte(`{"error":"not_found"}`))
		case r.Method == http.MethodPost && r.URL.Path == "/api/hub/inbox/deliver":
			f.deliverCalls++
			var in map[string]any
			_ = json.NewDecoder(r.Body).Decode(&in)
			f.deliverBodies = append(f.deliverBodies, in)
			var ids []string
			if raw, ok := in["ids"].([]any); ok {
				for _, v := range raw {
					if one, ok := v.(string); ok {
						ids = append(ids, one)
					}
				}
			}
			delivered, missing := ids, []string(nil)
			if f.deliverFunc != nil {
				delivered, missing = f.deliverFunc(ids)
			}
			_ = json.NewEncoder(w).Encode(map[string]any{"delivered": delivered, "missing": missing})
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	})
}

// inboxHub is importIDHub with the fake inbox as its site: a shelf, a configured site
// pointing at it, a stored credential, and the hold directory that lives beside the state
// file the hub was pointed at.
func inboxHub(t *testing.T, f *inboxSite) (*Server, string, *[]siteAsk, string) {
	t.Helper()
	s, _, root := libHub(t)
	srv, asks := siteServer(t, f.handler().ServeHTTP)
	t.Setenv("GORB_SITE", srv.URL)
	s.statePath = filepath.Join(t.TempDir(), "state.json")
	if err := s.saveSiteLink(&siteLink{Token: "bearer-token", Site: siteReleaseBase(), LinkedAt: 1730000000, Handle: "greg"}); err != nil {
		t.Fatalf("saveSiteLink: %v", err)
	}
	return s, root, asks, filepath.Join(filepath.Dir(s.statePath), siteInboxDirName)
}

// inboxID words an item id inside the grammar: "itm" plus nineteen digits.
func inboxID(n int) string {
	return fmt.Sprintf("itm%019d", n)
}

func inboxItem(n, version int) siteInboxItem {
	return siteInboxItem{ID: inboxID(n), DesignID: siteDesignID, Version: version, App: "clock",
		Name: "Skeleton by @greg", SentAt: "2026-11-01T00:00:00Z"}
}

// inboxHold writes a hold file by hand — the way a crash, a restart, or an earlier pass
// leaves one — through the production writer, and answers the bytes it filed.
func inboxHold(t *testing.T, s *Server, dir string, env *siteInboxEnvelope) {
	t.Helper()
	if err := s.siteInboxWriteEnv(dir, env.ItemID, env); err != nil {
		t.Fatalf("write hold %s: %v", env.ItemID, err)
	}
}

func readHold(t *testing.T, dir, id string) *siteInboxEnvelope {
	t.Helper()
	env, err := siteInboxReadEnv(filepath.Join(dir, id+".json"), id)
	if err != nil {
		t.Fatalf("hold %s: %v", id, err)
	}
	return env
}

func holdExists(dir, id string) bool {
	_, err := os.Stat(filepath.Join(dir, id+".json"))
	return err == nil
}

func holdEnvelope(bundle []byte, n int, claimed, imported bool) *siteInboxEnvelope {
	return &siteInboxEnvelope{
		ItemID: inboxID(n), DesignID: siteDesignID, Name: "Skeleton by @greg", App: "clock",
		Version: 4, SentAt: "2026-11-01T00:00:00Z",
		Claimed: claimed, Imported: imported,
		Raw: base64.StdEncoding.EncodeToString(bundle),
	}
}

// The pass that finds an item holds the bundle to disk first and claims it second — that
// order is the whole reason a send cannot be lost between the two halves — and the
// claim's answer lands in the envelope the file ends at, at the modes a bearer's bytes
// deserve.
func TestSiteInboxHoldThenClaim(t *testing.T) {
	bundle := goodBundle(t, siteDesignID, 4, "clock", "Skeleton by @greg", pngArt(9, 9, 1))
	f := &inboxSite{items: []siteInboxItem{inboxItem(1, 4)}, bundles: map[string][]byte{inboxID(1): bundle}}
	var dir0 string
	var order []string
	f.deliverFunc = func(ids []string) ([]string, []string) {
		if holdExists(dir0, ids[0]) {
			order = append(order, "file-on-disk")
		}
		if env, err := siteInboxReadEnv(filepath.Join(dir0, ids[0]+".json"), ids[0]); err == nil && !env.Claimed {
			order = append(order, "unclaimed-hold")
		}
		order = append(order, "deliver")
		return ids, nil
	}
	s, _, asks, dir := inboxHub(t, f)
	dir0 = dir
	if err := s.pollSiteInbox(context.Background()); err != nil {
		t.Fatalf("poll: %v", err)
	}
	if strings.Join(order, ",") != "file-on-disk,unclaimed-hold,deliver" {
		t.Errorf("the claim ran as %v: the hold must be on disk, unclaimed, before the deliver POST", order)
	}
	if f.listCalls != 1 || f.bundleCalls != 1 || f.deliverCalls != 1 {
		t.Errorf("doors asked list=%d bundle=%d deliver=%d, want one each", f.listCalls, f.bundleCalls, f.deliverCalls)
	}
	for _, a := range *asks {
		if strings.HasPrefix(a.path, "/api/hub/theme/") {
			t.Error("the pass fetched the theme door; the inbox bundle door is the one that pins a version")
		}
	}
	env := readHold(t, dir, inboxID(1))
	if !env.Claimed || env.Imported {
		t.Errorf("flags claimed:%v imported:%v, want claimed and not imported", env.Claimed, env.Imported)
	}
	if env.Version != 4 || env.DesignID != siteDesignID || env.Name != "Skeleton by @greg" || env.App != "clock" {
		t.Errorf("envelope identity = %+v, want the pinned identity the list door named", env)
	}
	raw, derr := base64.StdEncoding.DecodeString(env.Raw)
	if derr != nil || string(raw) != string(bundle) {
		t.Errorf("hold raw is not the bundle the door served (decode err %v)", derr)
	}
	fi, err := os.Stat(filepath.Join(dir, inboxID(1)+".json"))
	if err != nil || fi.Mode().Perm() != 0o600 {
		t.Errorf("hold file mode = %v, want the file readable at 0600", fi.Mode().Perm())
	}
	di, err := os.Stat(dir)
	if err != nil || di.Mode().Perm() != 0o700 {
		t.Errorf("hold dir mode = %v, want 0700", di.Mode().Perm())
	}
}

// The list door says version 4; the design has moved to 7 since the send; the bundle door
// answers 4's bytes, and that is what the hold files. §3.3's whole point is this test:
// the hub must fetch the inbox bundle door and never the theme door, which answers
// newest. The fake serves both, and counts which one the hub rang.
func TestSiteInboxServesPinnedVersion(t *testing.T) {
	fourth := goodBundle(t, siteDesignID, 4, "clock", "Skeleton by @greg", pngArt(9, 9, 4))
	seventh := goodBundle(t, siteDesignID, 7, "clock", "Skeleton by @greg", pngArt(9, 9, 7))
	f := &inboxSite{items: []siteInboxItem{inboxItem(1, 4)}, bundles: map[string][]byte{inboxID(1): fourth}}
	themeCalls := 0
	base := f.handler()
	srv, asks := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/api/hub/theme/"+siteDesignID {
			themeCalls++
			_, _ = w.Write(seventh)
			return
		}
		base.ServeHTTP(w, r)
	})
	s, _, root := libHub(t)
	_ = root
	t.Setenv("GORB_SITE", srv.URL)
	s.statePath = filepath.Join(t.TempDir(), "state.json")
	if err := s.saveSiteLink(&siteLink{Token: "bearer-token", Site: siteReleaseBase(), LinkedAt: 1730000000, Handle: "greg"}); err != nil {
		t.Fatal(err)
	}
	dir := filepath.Join(filepath.Dir(s.statePath), siteInboxDirName)
	if err := s.pollSiteInbox(context.Background()); err != nil {
		t.Fatalf("poll: %v", err)
	}
	env := readHold(t, dir, inboxID(1))
	if env.Version != 4 {
		t.Errorf("hold version = %d, want the pinned 4", env.Version)
	}
	raw, _ := base64.StdEncoding.DecodeString(env.Raw)
	if string(raw) != string(fourth) {
		t.Error("hold bytes are not version 4's bundle")
	}
	if themeCalls != 0 {
		t.Errorf("the pass rang the theme door %d times: the inbox bundle door is the one that pins", themeCalls)
	}
	for _, a := range *asks {
		if a.path == "/api/hub/inbox/"+inboxID(1)+"/bundle" && a.head.Get("Authorization") != "Bearer bearer-token" {
			t.Errorf("the bundle door carried no bearer: %q", a.head.Get("Authorization"))
		}
	}
}

// The disk is the crash state: a hold written before the claim died is re-claimed, an
// imported hold whose theme reached the shelf is settled away, and neither costs a
// re-fetch — the bytes survive, so the restart buys the bookkeeping, not the download.
func TestSiteInboxRestartReclaimsAndKeepsHolds(t *testing.T) {
	bundle := goodBundle(t, siteDesignID, 4, "clock", "Skeleton by @greg", pngArt(9, 9, 1))
	f := &inboxSite{} // the list door stays empty: everything this pass does is disk work
	s, root, _, dir := inboxHub(t, f)
	inboxHold(t, s, dir, holdEnvelope(bundle, 1, false, false)) // written, never claimed
	env2 := holdEnvelope(bundle, 2, true, true)                 // imported, keep never came: stays
	env2.Version = 5                                            // the shelf's sidecar names 4: not settled
	inboxHold(t, s, dir, env2)
	inboxHold(t, s, dir, holdEnvelope(bundle, 3, true, false)) // claimed, waiting on Add: stays
	inboxHold(t, s, dir, holdEnvelope(bundle, 4, true, true))  // kept: settles away
	shelf := filepath.Join(root, "themes")
	if err := os.WriteFile(filepath.Join(shelf, "Skeleton by @greg.orbtheme"), []byte("baked"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := writeThemeOrigin(filepath.Join(shelf, "Skeleton by @greg.orbtheme"),
		&ThemeOrigin{Site: siteReleaseBase(), DesignID: siteDesignID, Version: 4, ImportedAt: 1730000000000}); err != nil {
		t.Fatal(err)
	}
	if err := s.pollSiteInbox(context.Background()); err != nil {
		t.Fatalf("poll: %v", err)
	}
	if f.bundleCalls != 0 {
		t.Errorf("the pass fetched %d bundles for ids already on disk", f.bundleCalls)
	}
	if holdExists(dir, inboxID(4)) {
		t.Error("the imported hold whose theme is on the shelf is still standing")
	}
	env1 := readHold(t, dir, inboxID(1))
	if !env1.Claimed {
		t.Error("the unclaimed hold was not re-claimed")
	}
	if string(env1.Raw) != base64.StdEncoding.EncodeToString(bundle) {
		t.Error("the re-claimed hold lost its bytes")
	}
	if !holdExists(dir, inboxID(2)) || !holdExists(dir, inboxID(3)) {
		t.Error("an imported-without-keep or claimed-not-imported hold must stay standing")
	}
	if f.deliverCalls != 1 || len(f.deliverBodies) != 1 {
		t.Fatalf("deliver calls = %d, want the one re-claim", f.deliverCalls)
	}
	if ids, ok := f.deliverBodies[0]["ids"].([]any); !ok || len(ids) != 1 || ids[0] != inboxID(1) {
		t.Errorf("re-claim POST body = %v, want exactly the unclaimed id", f.deliverBodies[0])
	}
}

// A claim answered missing is the site saying this is not ours to deliver: the hold goes,
// and the log says so — the next list is the truth about the item.
func TestSiteInboxClaimMissingDeletesFile(t *testing.T) {
	f := &inboxSite{items: []siteInboxItem{inboxItem(1, 4)},
		bundles: map[string][]byte{inboxID(1): goodBundle(t, siteDesignID, 4, "clock", "Skeleton by @greg", pngArt(9, 9, 1))}}
	f.deliverFunc = func(ids []string) ([]string, []string) { return nil, ids }
	s, _, _, dir := inboxHub(t, f)
	var logOut strings.Builder
	restore := swapLogOutput(&logOut)
	defer restore()
	if err := s.pollSiteInbox(context.Background()); err != nil {
		t.Fatalf("poll: %v", err)
	}
	if holdExists(dir, inboxID(1)) {
		t.Error("a hold the site answered missing is still on disk")
	}
	line := logOut.String()
	if !strings.Contains(line, "site inbox") || !strings.Contains(line, "missing") {
		t.Errorf("the log said %q, want a site inbox line naming the missing answer", line)
	}
}

// Thirty-two holds are the cap: the pass walks the list, counts the directory each
// iteration, and stops fetching without a special case. The extra items stay waiting on
// the site — nothing is dropped, nothing is refused, and the next minute asks again.
func TestSiteInboxFullHoldStopsFetching(t *testing.T) {
	f := &inboxSite{bundles: map[string][]byte{}}
	for n := 1; n <= 34; n++ {
		f.items = append(f.items, inboxItem(n, 4))
	}
	s, _, _, dir := inboxHub(t, f)
	bundle := goodBundle(t, siteDesignID, 4, "clock", "Skeleton by @greg", pngArt(9, 9, 1))
	for n := 1; n <= 32; n++ {
		inboxHold(t, s, dir, holdEnvelope(bundle, n, true, false))
	}
	var logOut strings.Builder
	restore := swapLogOutput(&logOut)
	defer restore()
	if err := s.pollSiteInbox(context.Background()); err != nil {
		t.Fatalf("poll: %v", err)
	}
	if f.bundleCalls != 0 {
		t.Errorf("a full hold still fetched %d bundles; the cap must stop the pass mid-list", f.bundleCalls)
	}
	if got := siteInboxCount(dir); got != 32 {
		t.Errorf("holds on disk = %d, want the 32 that were there", got)
	}
	if f.deliverCalls != 0 {
		t.Errorf("the full pass made %d claim calls with nothing new written", f.deliverCalls)
	}
	if !strings.Contains(logOut.String(), "site inbox") {
		t.Errorf("the pass that stopped at the cap said nothing: %q", logOut.String())
	}
}

// The tray's list door reads the directory and nothing else: no network call, newest-held
// first, unreadable files skipped, a name that is not an item id never opened, and a hub
// that has never held anything answers an empty list rather than an error.
func TestSiteInboxListDoorServesDiskOnly(t *testing.T) {
	bundle := goodBundle(t, siteDesignID, 4, "clock", "Skeleton by @greg", pngArt(9, 9, 1))
	f := &inboxSite{}
	s, _, asks, dir := inboxHub(t, f)
	env := designCall(t, s, http.MethodGet, "/api/site/inbox", nil)
	if env["ok"] != true {
		t.Fatalf("empty inbox = %v", env)
	}
	data := env["data"].(map[string]any)
	if items, ok := data["items"].([]any); !ok || len(items) != 0 {
		t.Fatalf("empty inbox data = %v, want items: []", data)
	}
	inboxHold(t, s, dir, holdEnvelope(bundle, 1, true, false))
	inboxHold(t, s, dir, holdEnvelope(bundle, 2, false, false))
	inboxHold(t, s, dir, holdEnvelope(bundle, 3, true, true))
	if err := os.WriteFile(filepath.Join(dir, "notes.txt"), []byte("not an item"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, inboxID(4)+".json"), []byte("{not json"), 0o600); err != nil {
		t.Fatal(err)
	}
	for n, off := range map[int]int64{1: 3, 2: 2, 3: 1} {
		stamp := time.Now().Add(-time.Duration(off) * time.Hour)
		if err := os.Chtimes(filepath.Join(dir, inboxID(n)+".json"), stamp, stamp); err != nil {
			t.Fatal(err)
		}
	}
	env = designCall(t, s, http.MethodGet, "/api/site/inbox", nil)
	data = env["data"].(map[string]any)
	items := data["items"].([]any)
	if len(items) != 3 {
		t.Fatalf("items = %v, want the three readable holds", items)
	}
	for i, want := range []string{inboxID(3), inboxID(2), inboxID(1)} {
		if got := items[i].(map[string]any)["itemId"]; got != want {
			t.Errorf("row %d = %v, want newest-held-first at %s", i, items[i], want)
		}
	}
	row := items[0].(map[string]any)
	if row["name"] != "Skeleton by @greg" || row["claimed"] != true || row["imported"] != true || row["version"] != float64(4) {
		t.Errorf("row = %v, want the envelope minus its bytes", row)
	}
	if _, leaked := row["raw"]; leaked {
		t.Error("the tray door served held bundle bytes; the tray asks for nothing but the envelope")
	}
	if data["site"] != siteReleaseBase() {
		t.Errorf("site = %v, want the configured site's base for §8's wording", data["site"])
	}
	if len(*asks) != 0 {
		t.Errorf("the tray door made %d site calls: the tray reads disk", len(*asks))
	}
}

// Add runs the held bytes through the unchanged checkBundle and the unchanged ticket
// store, answers handleSiteImport's own success object so the browser's editor hand-off
// is the paste import's flow, notes the hold imported — and the whole keep path then
// runs on the ticket, after which the next pass settles the hold away.
func TestSiteInboxAddAnswersLikeImport(t *testing.T) {
	bundle := goodBundle(t, siteDesignID, 4, "clock", "Skeleton by @greg", pngArt(9, 9, 1))
	f := &inboxSite{}
	s, root, _, dir := inboxHub(t, f)
	inboxHold(t, s, dir, holdEnvelope(bundle, 1, true, false))
	body, _ := json.Marshal(map[string]string{"itemId": inboxID(1)})
	env := designCall(t, s, http.MethodPost, "/api/site/inbox/add", body)
	if env["ok"] != true {
		t.Fatalf("add = %v", env)
	}
	d := env["data"].(map[string]any)
	for k, want := range map[string]any{
		"site": siteReleaseBase(), "id": siteDesignID, "version": float64(4), "app": "clock",
		"name": "Skeleton by @greg", "sharedName": "Skeleton by @greg", "handle": "greg",
	} {
		if d[k] != want {
			t.Errorf("add answer %s = %v, want %v — the same keys handleSiteImport answers", k, d[k], want)
		}
	}
	if _, has := d["schema"]; !has {
		t.Error("add answer carries no schema key: handleSiteImport's success object has one")
	}
	tk, _ := d["ticket"].(string)
	if tk == "" {
		t.Fatalf("no ticket in %v", d)
	}
	back := rawCall(t, s, http.MethodGet, "/api/site/bundle?ticket="+tk, nil)
	if back.Code != http.StatusOK || string(back.Body.Bytes()) != string(bundle) {
		t.Errorf("the held bundle reads back as %d %d bytes, want the served bytes whole", back.Code, back.Body.Len())
	}
	if !readHold(t, dir, inboxID(1)).Imported {
		t.Error("the hold did not note its import")
	}
	// The ticket carries a live import: the same bake the paste flow posts, and the
	// origin record lands beside the theme exactly as §7.1's keep writes it.
	bake := withDesign(orbfake.Container("Skeleton by @greg"), string(designDoc("Skeleton by @greg", "v1.2.30", 1)))
	kept := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+tk, bake)
	if kept["ok"] != true {
		t.Fatalf("keep = %v", kept)
	}
	shelf := filepath.Join(root, "themes", kept["data"].(map[string]any)["file"].(string))
	if o := readThemeOrigin(shelf); o == nil || o.DesignID != siteDesignID || o.Version != 4 {
		t.Errorf("sidecar after the inbox keep = %+v", o)
	}
	// The imported hold whose theme reached the shelf is what step 1 deletes: a full
	// pass later, the byte-holding file is gone and the shelf keeps the theme.
	if err := s.pollSiteInbox(context.Background()); err != nil {
		t.Fatalf("poll: %v", err)
	}
	if holdExists(dir, inboxID(1)) {
		t.Error("the imported-and-kept hold outlived its settle")
	}
	// An unknown id is the paste door's sentence, not a 404: the tray cannot tell the
	// difference and neither can the person clicking.
	ghost, _ := json.Marshal(map[string]string{"itemId": inboxID(9)})
	refused := designCall(t, s, http.MethodPost, "/api/site/inbox/add", ghost)
	if refused["ok"] == true || refused["error"] != siteLinkRefused {
		t.Errorf("add of an unknown id = %v, want the refuse sentence", refused)
	}
}

// Dismiss deletes the hold and tells the site the item is not to be offered any more;
// the claim answer — honestly a missing, since a dismissed item was never delivered — is
// ignored by agreement, and the door still answers ok.
func TestSiteInboxDismiss(t *testing.T) {
	bundle := goodBundle(t, siteDesignID, 4, "clock", "Skeleton by @greg", pngArt(9, 9, 1))
	f := &inboxSite{}
	f.deliverFunc = func(ids []string) ([]string, []string) { return nil, ids }
	s, _, _, dir := inboxHub(t, f)
	inboxHold(t, s, dir, holdEnvelope(bundle, 1, true, false))
	body, _ := json.Marshal(map[string]string{"itemId": inboxID(1)})
	env := designCall(t, s, http.MethodPost, "/api/site/inbox/dismiss", body)
	if env["ok"] != true {
		t.Fatalf("dismiss = %v", env)
	}
	if holdExists(dir, inboxID(1)) {
		t.Error("a dismissed hold is still on disk")
	}
	if f.deliverCalls != 1 || f.deliverBodies[0]["ids"].([]any)[0] != inboxID(1) {
		t.Errorf("the site was told %d times (%v), want the dismissed id once", f.deliverCalls, f.deliverBodies)
	}
	// The tray answer after a dismiss is an empty list, read from disk alone.
	after := designCall(t, s, http.MethodGet, "/api/site/inbox", nil)["data"].(map[string]any)
	if items := after["items"].([]any); len(items) != 0 {
		t.Errorf("tray after dismiss = %v, want nothing waiting", items)
	}
	// A second dismiss of the gone id still answers ok: the state the owner asked for
	// holds, and the site call is not repeated for an id this disk no longer holds.
	again := designCall(t, s, http.MethodPost, "/api/site/inbox/dismiss", body)
	if again["ok"] != true {
		t.Errorf("second dismiss = %v, want ok", again)
	}
	if f.deliverCalls != 1 {
		t.Errorf("the site heard about the dismissed item %d times, want once", f.deliverCalls)
	}
}

// The minute's ask never reaches a network a hub has no business dialing, and a site that
// refuses the bearer on the list door ends the link the way every other door does. An
// unlinked hub is idle, not broken: no asks, no error, nothing to see.
func TestSiteInboxLinkDeadAndIdle(t *testing.T) {
	f := &inboxSite{listStatus: http.StatusUnauthorized}
	s, _, _, _ := inboxHub(t, f)
	var logOut strings.Builder
	restore := swapLogOutput(&logOut)
	if err := s.pollSiteInbox(context.Background()); err != nil {
		t.Fatalf("poll on a refused bearer: %v", err)
	}
	restore()
	if !strings.Contains(logOut.String(), "site inbox") {
		t.Errorf("a refused credential logged %q, want a site inbox line", logOut.String())
	}
	if s.liveSiteLink() != nil {
		t.Error("a 401 at the inbox door left the credential live: the link must be forgotten as the import doors forget it")
	}
	if f.listCalls != 1 {
		t.Errorf("the dead link was asked %d times in one pass, want once", f.listCalls)
	}
	// The next minute asks nothing: a forgotten link is idle, and idle is not an error.
	before := f.listCalls
	if err := s.pollSiteInbox(context.Background()); err != nil {
		t.Fatalf("poll after the forget: %v", err)
	}
	if f.listCalls != before {
		t.Error("the pass after the forget dialed the site again")
	}
	// A hub holding a disconnected-shaped link file, and a hub with no link file at all,
	// are the same shape of nothing.
	if err := s.saveSiteLink(&siteLink{Site: siteReleaseBase(), LinkedAt: 1, Disconnected: true}); err != nil {
		t.Fatal(err)
	}
	if err := s.pollSiteInbox(context.Background()); err != nil {
		t.Fatalf("poll on a disconnected link: %v", err)
	}
	if err := os.Remove(s.siteLinkPath()); err != nil {
		t.Fatal(err)
	}
	if err := s.pollSiteInbox(context.Background()); err != nil {
		t.Fatalf("poll with no link file: %v", err)
	}
}

// Lead review (H3 live check): the site mints inbox ids with Prisma's cuid(), 25 characters,
// so a hub whose grammar demanded the design ids' 22 would skip every real send.
func TestSiteInboxAcceptsTheSitesItemIDs(t *testing.T) {
	const cuid = "cmhx3k2p40000a8z1b2c3d4e5"
	bundle := goodBundle(t, siteDesignID, 4, "clock", "Skeleton by @greg", pngArt(9, 9, 1))
	it := inboxItem(1, 4)
	it.ID = cuid
	f := &inboxSite{items: []siteInboxItem{it}, bundles: map[string][]byte{cuid: bundle}}
	s, _, _, dir := inboxHub(t, f)
	if err := s.pollSiteInbox(context.Background()); err != nil {
		t.Fatalf("poll: %v", err)
	}
	if f.bundleCalls != 1 || f.deliverCalls != 1 {
		t.Fatalf("bundle=%d deliver=%d: a cuid item id must be fetched and claimed", f.bundleCalls, f.deliverCalls)
	}
	if env := readHold(t, dir, cuid); !env.Claimed {
		t.Error("the cuid item's hold is not claimed")
	}
}

// Lead review (a pin: liveSiteLink already holds it): the link's token goes only to the site
// that issued it. A hub whose configured site is not the link's asks nothing at all.
func TestSiteInboxSendsTheTokenOnlyToItsSite(t *testing.T) {
	f := &inboxSite{items: []siteInboxItem{inboxItem(1, 4)}}
	s, _, asks, _ := inboxHub(t, f)
	if err := s.saveSiteLink(&siteLink{Token: "bearer-token", Site: "https://elsewhere.example", LinkedAt: 1730000000, Handle: "greg"}); err != nil {
		t.Fatalf("saveSiteLink: %v", err)
	}
	if err := s.pollSiteInbox(context.Background()); err != nil {
		t.Fatalf("poll: %v", err)
	}
	if f.listCalls != 0 || len(*asks) != 0 {
		t.Errorf("the hub asked %d time(s) (list=%d) with a link to another site", len(*asks), f.listCalls)
	}
}

// Lead review: a bundle the import checks would refuse is never held, so it is never claimed
// and stays waiting on the site.
func TestSiteInboxChecksTheBundleBeforeTheClaim(t *testing.T) {
	other := goodBundle(t, strings.Repeat("Z", 22), 4, "clock", "Skeleton by @greg", pngArt(9, 9, 1))
	f := &inboxSite{items: []siteInboxItem{inboxItem(1, 4)}, bundles: map[string][]byte{inboxID(1): other}}
	s, _, _, dir := inboxHub(t, f)
	if err := s.pollSiteInbox(context.Background()); err != nil {
		t.Fatalf("poll: %v", err)
	}
	if f.deliverCalls != 0 {
		t.Errorf("a bundle for another design was claimed (%d deliver calls)", f.deliverCalls)
	}
	if holdExists(dir, inboxID(1)) {
		t.Error("a bundle for another design was held")
	}
}
