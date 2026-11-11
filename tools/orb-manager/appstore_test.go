package main

// A1 §7.1 (docs/specs/a1-design.md): GET /api/site/apps. The hub reads the site's store,
// caches it for five minutes, serves the last copy as stale when the site is unreachable,
// keeps the door behind -site and requireControl, and rides the no-redirect site client.

import (
	"encoding/json"
	"net/http"
	"os"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
)

// catalogueJSON is a store as §4.1 serves it: two orderable apps, plus an unknown field a
// later site might add (which the decoder must swallow).
func catalogueJSON() string {
	return `{"apps":[` +
		`{"slug":"aurora","title":"Aurora","developer":"@gr3g","version":"v1.3.0",` +
		`"stamp":"v1.3.0+f3a2c1d","caps":["net","themes"],"osMin":"v1.2.8",` +
		`"sha256":"` + strings.Repeat("a", 64) + `","size":812345,"hasEditor":true,` +
		`"readers":{"1":"v1.0.0","2":"v1.3.0"},"withdrawn":[{"version":"v1.2.0","reason":"crash on wake"}],"someday":"ignored"},` +
		`{"slug":"weather","title":"Weather","developer":"gorb-os","version":"v2.0.0",` +
		`"stamp":"v2.0.0+aaaaaaa","caps":[],"osMin":"","sha256":"` + strings.Repeat("b", 64) + `",` +
		`"size":1,"hasEditor":false,"readers":{},"withdrawn":[]}` +
		`]}`
}

// appsData reads the {apps,checkedAt,stale} payload of an ok:true /api/site/apps envelope.
func appsData(t *testing.T, env map[string]any) (apps []any, checkedAt float64, stale bool) {
	t.Helper()
	if env["ok"] != true {
		t.Fatalf("the door refused: %v", env)
	}
	data, _ := env["data"].(map[string]any)
	if data == nil {
		t.Fatalf("no data in %v", env)
	}
	arr, _ := data["apps"].([]any)
	if arr == nil {
		arr = []any{}
	}
	ca, _ := data["checkedAt"].(float64)
	st, _ := data["stale"].(bool)
	return arr, ca, st
}

// statusOfCall runs a raw request and returns just the recorder's status code.
func statusOfCall(t *testing.T, s *Server, method, path string, body []byte) *httptest.ResponseRecorder {
	t.Helper()
	return rawCall(t, s, method, path, body)
}

// siteSwitch is a fake site that answers one way until a test flips it: good until broken.
type siteSwitch struct {
	mu      sync.Mutex
	broken  bool
	handler func(w http.ResponseWriter, r *http.Request)
}

func (sw *siteSwitch) serveHTTP(w http.ResponseWriter, r *http.Request) {
	sw.mu.Lock()
	broken, h := sw.broken, sw.handler
	sw.mu.Unlock()
	if broken {
		if h != nil {
			h(w, r)
			return
		}
		w.WriteHeader(http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_, _ = w.Write([]byte(catalogueJSON()))
}

func (sw *siteSwitch) breakWith(h func(w http.ResponseWriter, r *http.Request)) {
	sw.mu.Lock()
	sw.broken, sw.handler = true, h
	sw.mu.Unlock()
}

// The main line: a page asks, the hub reads the site once and answers through the envelope;
// a second press inside the five-minute window rides the cache (the site sees one request),
// and ?refresh=1 forces a second dial.
func TestSiteAppsCatalogueProxiesAndCaches(t *testing.T) {
	s, _, _ := libHub(t)
	srv, asks := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/hub/apps" {
			http.NotFound(w, r)
			return
		}
		if got := r.Header.Get("Authorization"); got != "" {
			t.Errorf("the catalogue request carried a credential: %q", got)
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(catalogueJSON()))
	})
	t.Setenv("GORB_SITE", srv.URL)

	first := designCall(t, s, http.MethodGet, "/api/site/apps", nil)
	apps, checkedAt, stale := appsData(t, first)
	if len(apps) != 2 || checkedAt <= 0 || stale {
		t.Fatalf("first read = %d apps, checkedAt %v, stale %v, want two fresh apps", len(apps), checkedAt, stale)
	}
	if got := len(*asks); got != 1 {
		t.Fatalf("the first read dialed the site %d times, want 1", got)
	}
	// The answer names the configured site, so the withdrawn banner can say whose store it is
	// even for an app the library did not get from the site.
	if d, _ := first["data"].(map[string]any); d == nil || d["site"] != srv.URL {
		t.Errorf("the catalogue answer names site %v, want %q", first["data"].(map[string]any)["site"], srv.URL)
	}
	// A second press inside a minute is the cache, not a second call to orb-os.net.
	second := designCall(t, s, http.MethodGet, "/api/site/apps", nil)
	apps2, checkedAt2, _ := appsData(t, second)
	if len(apps2) != 2 {
		t.Fatalf("cached read = %d apps, want the same two", len(apps2))
	}
	if checkedAt2 != checkedAt {
		t.Errorf("the cached read re-stamped checkedAt (%v then %v); the served copy's time must be its fetch time", checkedAt, checkedAt2)
	}
	if got := len(*asks); got != 1 {
		t.Errorf("two presses dialed the site %d times, want the cache to hold at 1", got)
	}
	// ?refresh=1 forces a re-read of the store.
	if third := designCall(t, s, http.MethodGet, "/api/site/apps?refresh=1", nil); third["ok"] != true {
		t.Fatalf("refresh refused: %v", third)
	}
	if got := len(*asks); got != 2 {
		t.Errorf("?refresh=1 dialed the site %d times in all, want a second request", got)
	}
	// The door answers GET only, the way every other site door says it.
	if rec := rawCall(t, s, http.MethodPost, "/api/site/apps", []byte(`{}`)); rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("POST /api/site/apps = %d, want 405", rec.Code)
	}
}

// A site that is down in any of five ways does not empty the shelf: with a copy held, the
// hub serves it marked stale; with nothing held, it refuses and names the configured host.
// The five: 429, 500, a redirect, a 2 MB body, and garbage JSON.
func TestSiteAppsStaleCacheOnSiteDown(t *testing.T) {
	for _, bad := range []struct {
		name string
		h    func(w http.ResponseWriter, r *http.Request)
	}{
		{"429", func(w http.ResponseWriter, _ *http.Request) {
			w.Header().Set("Retry-After", "30")
			w.WriteHeader(http.StatusTooManyRequests)
			_, _ = w.Write([]byte(`{"error":"too many requests, try again shortly"}`))
		}},
		{"500", func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(http.StatusInternalServerError) }},
		{"302", func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, "http://"+r.Host+"/elsewhere", http.StatusFound)
		}},
		{"2MB", func(w http.ResponseWriter, _ *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			_, _ = w.Write([]byte(strings.Repeat("x", 2<<20)))
		}},
		{"garbage", func(w http.ResponseWriter, _ *http.Request) {
			_, _ = w.Write([]byte(`not json at all`))
		}},
	} {
		t.Run(bad.name, func(t *testing.T) {
			s, _, _ := libHub(t)
			sw := &siteSwitch{}
			srv, _ := siteServer(t, sw.serveHTTP)
			t.Setenv("GORB_SITE", srv.URL)
			first := designCall(t, s, http.MethodGet, "/api/site/apps", nil)
			if _, _, stale := appsData(t, first); stale {
				t.Fatal("a hub's first read of a live site should not be stale")
			}
			sw.breakWith(bad.h)
			second := designCall(t, s, http.MethodGet, "/api/site/apps?refresh=1", nil)
			apps, checkedAt, stale := appsData(t, second)
			if !stale {
				t.Errorf("after a %s the door served stale=false, want the held copy marked stale", bad.name)
			}
			if len(apps) != 2 || checkedAt <= 0 {
				t.Errorf("the stale copy was %d apps at %v, want the two it read while the site lived", len(apps), checkedAt)
			}
		})
	}

	// With no copy ever read, an unreachable site is the ordinary refusal, and the
	// sentence names the host the owner configured.
	s, _, _ := libHub(t)
	down, _ := siteServer(t, func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(http.StatusInternalServerError) })
	t.Setenv("GORB_SITE", down.URL)
	env := designCall(t, s, http.MethodGet, "/api/site/apps", nil)
	if env["ok"] != false {
		t.Fatalf("a never-reachable site answered ok=%v, want the refusal envelope", env["ok"])
	}
	msg, _ := env["error"].(string)
	host := siteHost(strings.TrimPrefix(strings.TrimPrefix(down.URL, "http://"), "https://"))
	if !strings.Contains(msg, host) {
		t.Errorf("refusal %q does not name the site host %q", msg, host)
	}
	if rec := statusOfCall(t, s, http.MethodGet, "/api/site/apps", nil); rec.Code != http.StatusOK {
		t.Errorf("a refusal carried HTTP %d; the hub answers an unreadable store at 200 with ok:false", rec.Code)
	}
}

// TestSiteAppsDecodesNullEntry pins the case §4.1's withdrawn-only entry exists for: a
// store in which one app's seven describe-fields are all JSON null must decode without an
// error, leave the value fields zero, and keep the rest of the object — and the other
// apps' — intact.
func TestSiteAppsDecodesNullEntry(t *testing.T) {
	body := `{"apps":[` +
		`{"slug":"aurora","title":"Aurora","developer":"@gr3g","version":null,"stamp":null,` +
		`"caps":null,"osMin":null,"sha256":null,"size":null,"hasEditor":null,"readers":{},` +
		`"withdrawn":[{"version":"v1.2.0","reason":"crash on wake"},` +
		`{"version":"v1.1.0","reason":"the store gave no reason"}]},` +
		`{"slug":"weather","title":"Weather","developer":"gorb-os","version":"v2.0.0",` +
		`"stamp":"v2.0.0+aaaaaaa","caps":[],"osMin":"","sha256":"` + strings.Repeat("b", 64) + `",` +
		`"size":1,"hasEditor":false,"readers":{},"withdrawn":[]}]}`
	s, _, _ := libHub(t)
	srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(body))
	})
	t.Setenv("GORB_SITE", srv.URL)
	env := designCall(t, s, http.MethodGet, "/api/site/apps", nil)
	apps, _, _ := appsData(t, env)
	if len(apps) != 2 {
		t.Fatalf("a null-version entry broke the decode: %d apps came back, want both", len(apps))
	}
	var entry siteCatalogue
	raw, _ := json.Marshal(apps[0])
	if err := json.Unmarshal(raw, &entry); err != nil {
		t.Fatalf("the served entry does not re-decode: %v", err)
	}
	if entry.Version != "" {
		t.Errorf("a withdrawn-only entry came back with version %q, want the empty string", entry.Version)
	}
	if entry.Title != "Aurora" || entry.Developer != "@gr3g" {
		t.Errorf("the rest of the entry did not survive: %+v", entry)
	}
	if len(entry.Withdrawn) != 2 || entry.Withdrawn[0].Version != "v1.2.0" ||
		entry.Withdrawn[0].Reason != "crash on wake" || entry.Withdrawn[1].Reason != "the store gave no reason" {
		t.Errorf("withdrawn rows came back as %+v, want both rows and both reasons", entry.Withdrawn)
	}
	if entry.orderable() {
		t.Error("a null-version entry reported orderable; the banner reads it and nothing offers it")
	}
	var other siteCatalogue
	raw2, _ := json.Marshal(apps[1])
	if err := json.Unmarshal(raw2, &other); err != nil || other.Version != "v2.0.0" || !other.orderable() {
		t.Errorf("the orderable neighbour decoded as %+v (err %v)", other, err)
	}
}

// A hub started without -site has no /api/site/apps at all: the door is 404, and the site
// is never dialed. This is the new door's twin of TestSiteDoorShutWithoutTheFlag, named so
// the shut /api/site/apps is visible by name in the run log.
func TestSiteAppsDoorsShutWithoutTheFlag(t *testing.T) {
	s, _, _ := libHub(t)
	srv, asks := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		t.Errorf("a shut hub asked the site for %s", r.URL.Path)
	})
	t.Setenv("GORB_SITE", srv.URL)
	s.siteHidden = true
	if rec := rawCall(t, s, http.MethodGet, "/api/site/apps", nil); rec.Code != http.StatusNotFound {
		t.Errorf("GET /api/site/apps on a shut hub = %d, want 404", rec.Code)
	}
	if rec := rawCall(t, s, http.MethodPost, "/api/site/apps", []byte(`{}`)); rec.Code != http.StatusNotFound {
		t.Errorf("POST /api/site/apps on a shut hub = %d, want 404", rec.Code)
	}
	if len(*asks) != 0 {
		t.Errorf("a shut hub dialed the site %d times: %v", len(*asks), *asks)
	}
}

// The catalogue is public, but only the hub's own page may make the call: an editor
// credential is refused at the gate, by name, before the door runs.
func TestSiteAppsDoorsRefuseEditorCreds(t *testing.T) {
	s, _, _ := libHub(t)
	srv, asks := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(catalogueJSON()))
	})
	t.Setenv("GORB_SITE", srv.URL)
	mux := http.NewServeMux()
	s.routes(mux)
	call := func(editorHeader, editorQuery, goodToken bool) *httptest.ResponseRecorder {
		target := "/api/site/apps"
		if editorQuery {
			target += "?et=" + s.editorToken("clock")
		}
		req := httptest.NewRequest(http.MethodGet, target, nil)
		if editorHeader {
			req.Header.Set(editorTokenHeader, s.editorToken("clock"))
		}
		if goodToken {
			req.Header.Set(tokenHeader, s.token)
		}
		rec := httptest.NewRecorder()
		mux.ServeHTTP(rec, req)
		return rec
	}
	if rec := call(true, false, false); rec.Code != http.StatusForbidden {
		t.Errorf("editor header = %d, want 403", rec.Code)
	} else if !strings.Contains(rec.Body.String(), "editor") {
		t.Errorf("the refusal was %s, want it to name the editor credential", rec.Body.String())
	}
	if rec := call(false, true, false); rec.Code != http.StatusForbidden {
		t.Errorf("?et= = %d, want 403", rec.Code)
	}
	if rec := call(false, true, true); rec.Code != http.StatusForbidden {
		t.Errorf("editor token beside a good one = %d, want the editor token to refuse it", rec.Code)
	}
	if rec := call(false, false, true); rec.Code != http.StatusOK {
		t.Errorf("the launch token got %d, want the gate to pass and the door to answer", rec.Code)
	}
	if len(*asks) != 1 {
		t.Errorf("the guarded door reached the site %d times, want only the passed press", len(*asks))
	}
}

// The face-side reader the contract pins: a module cache, a siteApps(force) that asks
// ?refresh=1 or rides the cache, and the same 404-hiding rule loadUpdates follows, so a
// shut door keeps the Get-apps section away rather than reporting a face it never offered.
// Step 1 draws no face; step 3 uses this reader.
func TestSiteAppsFaceReaderExists(t *testing.T) {
	app := readAppJS(t)
	for _, want := range []string{
		"let siteAppsCache = null;",
		"async function siteApps(force)",
		`"/api/site/apps" + (force ? "?refresh=1" : "")`,
		"if (r.status === 404) {",
	} {
		if !strings.Contains(app, want) {
			t.Errorf("app.js no longer holds %q: the store reader keeps cache, refresh and the 404 rule", want)
		}
	}
}

// A1 §7.4/§7.5, the face's half, pinned in the TestSiteUISectionIsThereAndShut style:
// the Get-apps section exists inside the Apps card and ships hidden, its every element
// waits behind the library's site word, and its wording table is the one ruling 18
// bound — the same six lines the site's src/lib/caps.ts carries, the same prefix and
// the same empty-list sentence, so a store row and a store page never disagree.
func TestSiteAppsGetAppsSectionIsThereAndShut(t *testing.T) {
	page, app := readWebFile(t, "index.html"), readAppJS(t)
	tabs := readWebFile(t, "tabs.js")
	card := regionOf(t, page, `<article class="card tabpane" data-tab="apps" id="mn-appscard"`, "</article>")

	// (a) The section lives inside the Apps card, under the import row, and hidden.
	for _, id := range []string{"mn-site-apps", "mn-site-apps-state", "mn-site-apps-list"} {
		if !strings.Contains(card, `id="`+id+`"`) {
			t.Errorf(`#%s is missing from the Apps card: Get apps is half built`, id)
		}
	}
	for _, id := range []string{"mn-site-apps", "mn-site-apps-state"} {
		if b := blockOfLine(t, page, `id="`+id+`"`); !strings.Contains(b, "hidden") {
			t.Errorf("#%s is not hidden in markup, so a shut hub shows a store section it cannot fill", id)
		}
	}
	// A section, not a new card and not a new tab (§7.4): the only <article> in the region
	// is the Apps card the region was cut from.
	if n := strings.Count(card, "<article"); n != 1 {
		t.Errorf("the Apps card region holds %d <article> elements: A1 adds a section inside the existing card, not a card", n)
	}

	// (b) Ruling 18's table, verbatim, and both fallbacks.
	for _, want := range []string{
		`const APP_CAP_WORDS = {`,
		`net: "uses the network",`,
		`location: "reads your location",`,
		`sound: "plays sounds",`,
		`power: "can ask the orb to stay awake or sleep",`,
		`sd_read: "reads files on the orb's card",`,
		`themes: "uses your themes",`,
		`const CAP_UNKNOWN_PREFIX = "uses ";`,
		`const CAP_EMPTY_SENTENCE = "uses nothing beyond its own screen";`,
	} {
		if !strings.Contains(tabs, want) {
			t.Errorf("tabs.js is missing %q: the plain-words table must match the site's line for line", want)
		}
	}
	// An unknown cap is the token through the prefix, never bare; the joiner's branches
	// are the three §6 sentences, each pinned as a branch, not as a wish.
	if !strings.Contains(tabs, `out.push(APP_CAP_WORDS[c] || (CAP_UNKNOWN_PREFIX + c))`) {
		t.Error("an unknown cap is not put through CAP_UNKNOWN_PREFIX in tabs.js")
	}
	cs := blockOf(t, tabs, "function capSentence(caps)")
	for _, want := range []string{
		`if (!w.length) return CAP_EMPTY_SENTENCE;`,
		`if (w.length === 1) return w[0];`,
		`", and " + w[w.length - 1]`,
	} {
		if !strings.Contains(cs, want) {
			t.Errorf("capSentence lacks %q: the joining is pinned clause by clause", want)
		}
	}

	// (c) Get apps: orderable store apps the shelf does not hold, caps said before the
	// press, and the one door the press takes.
	offer := blockOf(t, tabs, "function siteAppsToOffer()")
	if !strings.Contains(offer, `a && a.version && !libEntry(a.slug)`) {
		t.Error("Get apps offers withdrawn-only entries or shelf-held apps: only orderable entries the shelf lacks")
	}
	row := blockOf(t, tabs, "function siteAppRow(a, state)")
	if !strings.Contains(row, `capSentence(a.caps)`) || !strings.Contains(row, `"Add to library"`) {
		t.Error("a Get apps row lost its plain-words caps or its Add to library press")
	}
	if strings.Contains(row, "innerHTML") {
		t.Error("a Get apps row uses innerHTML: catalogue names are foreign bytes")
	}
	if !strings.Contains(app, `"/api/site/apps/add"`) ||
		!strings.Contains(app, `body: JSON.stringify({ slug: slug, version: version })`) {
		t.Error("siteAppAdd does not post {slug, version} to the hub's add door")
	}

	// (d) The read is face-cached, on first paint only, behind the library's site word.
	if !strings.Contains(tabs, `return !!(libCache && libCache.site === true && !siteAppsDoorShut);`) {
		t.Error("the Apps face's store door does not read libCache.site and the reader's shut flag")
	}
	la := blockOf(t, tabs, "async function loadApps()")
	if !strings.Contains(la, `if (typeof siteApps === "function" && siteStoreOpen() && siteAppsCache === null) {`) {
		t.Error("loadApps dials the catalogue without the site door, or on every re-read instead of once")
	}

	// (e) §7.5: the missing-app sentence keeps its words and gains the Get press only
	// when the catalogue has an orderable entry; with none, no button is built.
	fi := blockOf(t, app, "async function finishSiteImport(data)")
	if !strings.Contains(fi, `which Orb Manager doesn\u2019t carry`) ||
		!strings.Contains(fi, `const offer = await siteThemeGetApp(data);`) {
		t.Error("finishSiteImport lost its H1b sentence or the A1 Get-app offer beside it")
	}
	get := blockOf(t, app, "async function siteThemeGetApp(data)")
	for _, want := range []string{
		`"Get " + (ent.title || ent.slug)`,
		`if (!ent || !ent.version) return null;`,
		`await siteAppAdd(ent.slug, ent.version)`,
		`await finishSiteImport(data)`,
	} {
		if !strings.Contains(get, want) {
			t.Errorf("siteThemeGetApp lacks %q: title words, orderable-only, the add door, then the ordinary tail", want)
		}
	}
}

// A1 §7.4's one alarm, pinned in the same style: a banner above the app rows and now-
// where-else, the sentence with the host read off the site-sourced row (never a
// hardcoded orb-os.net, because GORB_SITE points wherever it points), the reason with
// its pre-A1 fallback, and the two branches the entry decides — Update that walks the
// row's own ladder, or the one line with no press at all. Nothing in this face removes
// a version, an app or a file.
func TestSiteAppsWithdrawnBannerOnAppsTab(t *testing.T) {
	page, tabs := readWebFile(t, "index.html"), readWebFile(t, "tabs.js")
	card := regionOf(t, page, `<article class="card tabpane" data-tab="apps" id="mn-appscard"`, "</article>")

	// (a) The banner is in the Apps card, hidden in markup, and above the rows.
	if !strings.Contains(card, `id="mn-site-withdrawn"`) {
		t.Fatal(`#mn-site-withdrawn is not inside the Apps card`)
	}
	if b := blockOfLine(t, page, `id="mn-site-withdrawn"`); !strings.Contains(b, "hidden") {
		t.Error("#mn-site-withdrawn is not hidden in markup, so a store with nothing to report shows an empty alarm")
	}
	if strings.Index(page, `id="mn-site-withdrawn"`) > strings.Index(page, `id="mn-apps-body"`) {
		t.Error("the withdrawn banner sits below #mn-apps-body: the alarm is above the rows, not after them")
	}
	render := blockOf(t, tabs, "function renderSiteWithdrawnBanner()")
	if !strings.Contains(render, "box.hidden = true") || !strings.Contains(render, "if (!hits.length) return") {
		t.Error("the banner does not hide itself when nothing worn is listed withdrawn")
	}
	if !strings.Contains(blockOf(t, tabs, "function renderAppsTab()"), "renderSiteWithdrawnBanner()") {
		t.Error("renderAppsTab never redraws the banner: a stale alarm over a re-read ledger")
	}

	// (b) The match: the dial's running version against the catalogue's withdrawn list,
	// on the vN.N.N head — a +build tail is one build of a release, not another release.
	hits := blockOf(t, tabs, "function siteWithdrawnHits()")
	for _, want := range []string{
		`String(e.installed.version).split("+")[0]`,
		`String(cand.version || "").split("+")[0] === head`,
		`if (!siteStoreOpen()) return hits;`,
	} {
		if !strings.Contains(hits, want) {
			t.Errorf("siteWithdrawnHits lacks %q: head-matched, and only on an open store", want)
		}
	}

	// (c) The sentence, the host and the reason fallback — and the two branches.
	row := blockOf(t, tabs, "function siteWithdrawnRow(h)")
	for _, want := range []string{
		`" was withdrawn by "`,
		`siteHostOf((h.e.lib && h.e.lib.origin && h.e.lib.origin.site) || (siteAppsCache && siteAppsCache.site))`,
		`h.w.reason || "The site gave no reason."`,
	} {
		if !strings.Contains(row, want) {
			t.Errorf("the banner is missing %q: title, head, host off the row, reason or its fallback", want)
		}
	}
	if !strings.Contains(row, `"No other version is offered."`) {
		t.Error("the banner has no no-orderable-live-version branch")
	}
	if !strings.Contains(row, "if (h.ent.version)") {
		t.Error("the banner's Update press does not hang off h.ent.version: no live version, no press")
	}
	// The press is the row's own: the add door, then the same installRun every Update
	// walks (ruling 18: the site adds no install path of its own).
	if !strings.Contains(row, `await siteAppAdd(h.ent.slug, h.ent.version)`) ||
		!strings.Contains(row, `installRun({ slug: h.ent.slug`) {
		t.Error("the banner's press does not run the add door and then the shared ladder")
	}

	// (d) The amber fork stays on the tab (§7.4): the chip names host and version, the
	// gate needs a sidecar and a newer live entry, and the badge counts the fork.
	if !strings.Contains(tabs, `chip("update available \u00b7 " + siteHostOf(f.origin.site) + " " + su.version, "upd",`) {
		t.Error("the site Update chip is not the pinned amber sentence naming host and version")
	}
	gate := blockOf(t, tabs, "function siteUpdateEntry(e)")
	for _, want := range []string{
		`if (!f || !f.origin || !f.origin.site) return null;`,
		`if (!ent || !ent.version) return null;`,
		`verCompare(ent.version, held) <= 0`,
	} {
		if !strings.Contains(gate, want) {
			t.Errorf("siteUpdateEntry lacks %q: sidecar-named site, orderable entry, newer by verCompare", want)
		}
	}
	if !strings.Contains(tabs, `rows.filter((e) => updateGate(e) || siteUpdateEntry(e)).length`) {
		t.Error("the Updates badge ignores the site fork (ruling 26: the amber verbs stay on tabs)")
	}
	if !strings.Contains(blockOf(t, tabs, "function appRow(e, opt)"), `slots[0] = siteUpdateButton(e, txt, su);`) {
		t.Error("the site Update does not sit in the row's own Update column")
	}
}

// A1 §7.5: a bundle's own schema rides through to the import answer, so the Themes face can
// look up the store's first live version that reads it ("is for <Title> vN or later"). A
// schema that is absent, not a whole number, or not a number at all reads as 0 (none named),
// never as a refusal: the theme still imports as it did before A1.
func TestSiteBundleCarriesItsSchema(t *testing.T) {
	id := "AbCdEfGhIjKlMnOpQrStUv"
	for _, c := range []struct {
		schema any
		want   int
	}{{nil, 0}, {2, 2}, {"2", 0}, {1.5, 0}, {0, 0}, {-3, 0}} {
		var doc map[string]any
		if err := json.Unmarshal(goodBundle(t, id, 1, "clock", "Dawn by @greg"), &doc); err != nil {
			t.Fatal(err)
		}
		if c.schema != nil {
			doc["schema"] = c.schema
		}
		b, err := checkBundle(bundleOf(t, doc), id)
		if err != nil {
			t.Fatalf("schema %v: the bundle was refused: %v", c.schema, err)
		}
		if b.Schema != c.want {
			t.Errorf("schema %v read as %d, want %d", c.schema, b.Schema, c.want)
		}
	}
	app := readWebFile(t, "app.js")
	for _, want := range []string{
		`"schema": b.Schema`,
	} {
		if src, _ := readFileString("siteimport.go"); !strings.Contains(src, want) {
			t.Errorf("siteimport.go's import answer no longer carries %s", want)
		}
	}
	for _, want := range []string{
		`const need = await siteThemeNeeds(data);`,
		`+ " or later, and Orb Manager has " + app.version;`,
		`+ " or later, which Orb Manager doesn`,
		`const v = ent && ent.readers && ent.readers[String(data.schema)];`,
	} {
		if !strings.Contains(app, want) {
			t.Errorf("app.js's theme import lost %q", want)
		}
	}
}

func readFileString(name string) (string, error) {
	b, err := os.ReadFile(name)
	return string(b), err
}
