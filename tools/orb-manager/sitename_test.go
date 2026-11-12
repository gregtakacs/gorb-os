package main

// h3-design.md §7.2 and §5.4, hub step 3: a shelf theme with no origin record is asked about
// by NAME, and an answer at the version already baked reaches the Updates tab as a rename.
//
// The fake here answers §4's contract for both query shapes — the id path a sidecar drives and
// the name path this step adds — and remembers every request it took, because the claims this
// file makes are mostly about which doors were knocked on: how many times, with what query, to
// what host, and with what credential. The shelf is real disk under a temp dir, the sidecars are
// the ones the hub writes, and the rows are read back off GET /api/updates.

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
)

// nameAsk is one request the fake took: its query, parsed, and its headers.
type nameAsk struct {
	query url.Values
	head  http.Header
}

// nameSite is §4's design site as this step needs it: one set of identity fields, answered for
// both query shapes, with the bundle door agreeing. It can be moved — to a new version, a new
// name, the gone body, a plain not-found, a bare status — between two passes.
type nameSite struct {
	srv      *httptest.Server
	mu       sync.Mutex
	asks     []nameAsk
	version  int
	shared   string
	app      string
	designID string
	status   int
	goneBody bool
	notFound func(http.ResponseWriter, int)
	bundle   func() []byte
}

func (u *nameSite) moveTo(version int, shared string) {
	u.mu.Lock()
	defer u.mu.Unlock()
	u.version, u.shared = version, shared
}

// answerGone moves the door to §4's one not-found body, at the status asked.
func (u *nameSite) answerGone(status int) {
	u.mu.Lock()
	defer u.mu.Unlock()
	u.status, u.goneBody, u.notFound = status, true, nil
}

// answerPlain moves the door to a not-found that says nothing about the design: the page or the
// router's JSON a site with no resolve door answers with, which is NotYet and not Gone.
func (u *nameSite) answerPlain(status int, ctype, body string) {
	u.mu.Lock()
	defer u.mu.Unlock()
	u.status, u.goneBody = status, false
	u.notFound = func(w http.ResponseWriter, _ int) {
		if ctype != "" {
			w.Header().Set("Content-Type", ctype)
		}
		w.WriteHeader(status)
		_, _ = w.Write([]byte(body))
	}
}

// asksOf is every request the fake took, oldest first.
func (u *nameSite) asksOf() []nameAsk {
	u.mu.Lock()
	defer u.mu.Unlock()
	return append([]nameAsk(nil), u.asks...)
}

// nameAsks and idAsks count the two query shapes apart, which is how "not asked at all" and "the
// recorded path was taken" are measured.
func (u *nameSite) nameAsks() int { return u.countAsks(true) }
func (u *nameSite) idAsks() int   { return u.countAsks(false) }

func (u *nameSite) countAsks(byName bool) int {
	n := 0
	for _, a := range u.asksOf() {
		if (a.query.Get("name") != "") == byName {
			n++
		}
	}
	return n
}

// lastName is the most recent by-name request, for the parameter and header assertions.
func (u *nameSite) lastName() (nameAsk, bool) {
	asks := u.asksOf()
	for i := len(asks) - 1; i >= 0; i-- {
		if asks[i].query.Get("name") != "" {
			return asks[i], true
		}
	}
	return nameAsk{}, false
}

func (u *nameSite) url() string { return strings.ToLower(u.srv.URL) }

// nameSiteStand is the fake at one version of one design under `shared`, answering 200 and
// serving a bundle that agrees with what it now says.
func nameSiteStand(t *testing.T, version int, shared string) *nameSite {
	t.Helper()
	u := &nameSite{version: version, shared: shared, app: "clock", designID: siteDesignID,
		status: http.StatusOK}
	u.bundle = func() []byte {
		u.mu.Lock()
		v, s, id, app := u.version, u.shared, u.designID, u.app
		u.mu.Unlock()
		return goodBundle(t, id, v, app, s, pngArt(9, 9, artSeed(s)))
	}
	u.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/hub/resolve" {
			_, _ = w.Write(u.bundle())
			return
		}
		u.mu.Lock()
		u.asks = append(u.asks, nameAsk{query: r.URL.Query(), head: r.Header.Clone()})
		v, s, app, id, st, gone, nf := u.version, u.shared, u.app, u.designID, u.status, u.goneBody, u.notFound
		u.mu.Unlock()
		if st != http.StatusOK {
			if gone {
				w.Header().Set("Content-Type", "application/json")
				w.WriteHeader(st)
				_, _ = w.Write([]byte(`{"gone":true}`))
				return
			}
			if nf == nil {
				nf = resolveGoneBody
			}
			nf(w, st)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = fmt.Fprintf(w, `{"designId":%q,"app":%q,"version":%d,"sharedName":%q}`, id, app, v, s)
	}))
	t.Cleanup(u.srv.Close)
	return u
}

// nameDial is a transport that records where the hub aimed and then refuses to leave the room:
// the one way a test can prove a request would have gone to the public release site without
// sending it there.
type nameDial struct{ got []string }

func (d *nameDial) RoundTrip(r *http.Request) (*http.Response, error) {
	d.got = append(d.got, r.URL.String())
	return nil, fmt.Errorf("this test does not dial %s", r.URL.Host)
}

// shelfThemeFile files one container on the shelf through the library's own keeping door and
// returns its path: a theme that arrived as a file, with no origin record beside it. The test
// fails if keeping it somehow recorded one, because that is the thing under test.
func shelfThemeFile(t *testing.T, s *Server, name string) string {
	t.Helper()
	lib := s.library()
	if lib == nil || lib.ThemesDir == "" {
		t.Fatal("this hub has no theme library to shelve a theme in")
	}
	dest, err := lib.keepTheme(withDesign(orbfake.Container(name), string(designDoc(name, "v1.2.30", 1))), false)
	if err != nil {
		t.Fatalf("shelving %q: %v", name, err)
	}
	if o := readThemeOrigin(dest); o != nil {
		t.Fatalf("%s arrived with an origin record: %+v", name, o)
	}
	return dest
}

// trackedShelfTheme imports a theme through the real doors, so the shelf holds a sidecar the hub
// wrote, and returns the theme's path.
func trackedShelfTheme(t *testing.T, s *Server, u *nameSite, shared string) string {
	t.Helper()
	tk := ticketOf(t, siteImport(t, s, u.url()+"/t/"+u.designID))
	env := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+tk,
		withDesign(orbfake.Container(shared), string(designDoc(shared, "v1.2.30", 1))))
	if env["ok"] != true {
		t.Fatalf("keep: %v", env)
	}
	file, _ := env["data"].(map[string]any)["file"].(string)
	if file == "" {
		t.Fatalf("keep named no shelf file: %v", env)
	}
	return filepath.Join(s.library().ThemesDir, file)
}

// themeRows lists the theme rows GET /api/updates drew, in order.
func themeRows(t *testing.T, s *Server) []updateItem {
	t.Helper()
	var out []updateItem
	for _, it := range updatesOf(t, s).Items {
		if it.Kind == "theme" {
			out = append(out, it)
		}
	}
	return out
}

// wantSilent is the by-name path's silence asserted: no theme row at all, and why.
func wantSilent(t *testing.T, s *Server) {
	t.Helper()
	if rows := themeRows(t, s); len(rows) != 0 {
		t.Fatalf("the by-name path drew %d theme rows; a name the site will not answer draws none: %+v",
			len(rows), rows)
	}
}

// wantAbsent is a file the pass was promised not to write.
func wantAbsent(t *testing.T, path, what string) {
	t.Helper()
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Errorf("the %s is there (%v); this pass was supposed to write nothing", what, err)
	}
}

// --- the row builder's by-name arm -----------------------------------------------------

// nameHub is a hub with a real shelf, its state file in the temp dir beside it, and its
// configured site pointed at the fake. No link is stored: a hub that has never signed in still
// asks, and asks anonymously.
func nameHub(t *testing.T, u *nameSite) (*Server, string) {
	t.Helper()
	s, _, root := libHub(t)
	s.statePath = filepath.Join(root, "state.json")
	t.Setenv("GORB_SITE", u.url())
	return s, root
}

// swapHubLog points the standard logger at a builder, so a claim that one line was logged is
// read off the bytes production writes.
func swapHubLog(t *testing.T) *strings.Builder {
	t.Helper()
	var b strings.Builder
	t.Cleanup(swapLogOutput(&b))
	return &b
}

// TestSiteNameAdopt is §7.2's adoption in one pass: a theme held under a shared name with no
// record of where it came from is asked by name and app, the answer is filed as the origin
// record the answer itself names, and the theme is an ordinary tracked theme from then on — so
// ordinary that the second pass asks by name not at all.
func TestSiteNameAdopt(t *testing.T) {
	u := nameSiteStand(t, 6, "Skeleton by @g") // the retired-handle shape: asked old, answered new
	s, _ := nameHub(t, u)
	shelf := shelfThemeFile(t, s, "Skeleton by @greg")

	// The row the adoption pass drew is the rename the adoption already knows about: the record
	// says v6, the shelf is named for the old handle, and the gate lets an equal version with a
	// different name through as the news it is.
	rows := themeRows(t, s)
	if len(rows) != 1 {
		t.Fatalf("the adoption pass drew %d rows, want the one rename: %+v", len(rows), rows)
	}
	if !rows[0].Renamed || rows[0].From != "Skeleton by @greg" || rows[0].To != "Skeleton by @g" ||
		rows[0].Version != 6 || rows[0].Key != siteDesignID {
		t.Errorf("row = %+v, want the adopted design's rename keyed by its id", rows[0])
	}

	o := originOf(t, shelf)
	if o.Site != siteReleaseBase() || o.DesignID != siteDesignID || o.Version != 6 || o.ImportedAt < 1 {
		t.Errorf("the adoption wrote %+v, want the configured site, this design and the answered "+
			"version, stamped", o)
	}
	if u.nameAsks() != 1 {
		t.Fatalf("the pass sent %d by-name requests, want 1", u.nameAsks())
	}
	ask, _ := u.lastName()
	if ask.query.Get("app") != "clock" {
		t.Errorf("the by-name ask sent app=%q, want the app the theme themes", ask.query.Get("app"))
	}

	// The second pass takes the record path, which means the name door stays dark. What it draws
	// is the same rename, now off an id resolve.
	before := u.nameAsks()
	rows = themeRows(t, s)
	if got := u.nameAsks(); got != before {
		t.Errorf("a theme with a sidecar was asked by name %d more times; it is tracked now", got-before)
	}
	if len(rows) != 1 || rows[0].Key != siteDesignID || !rows[0].Renamed {
		t.Fatalf("the second pass drew %+v, want the same rename off the record", rows)
	}
	if u.idAsks() == 0 {
		t.Error("the recorded path never resolved by id, so the row above came from nowhere")
	}
}

// TestSiteNameNoLink is review 6's other half: a hub that has never been linked still asks its
// configured site about an unrecorded theme, still adopts on the answer, and never sends a
// credential it does not hold.
func TestSiteNameNoLink(t *testing.T) {
	u := nameSiteStand(t, 6, "Skeleton by @greg")
	s, _ := nameHub(t, u)
	if l := s.liveSiteLink(); l != nil {
		t.Fatalf("this hub has a link (%+v) in a test about having none", l)
	}
	shelf := shelfThemeFile(t, s, "Skeleton by @greg")

	wantSilent(t, s) // same version, same name: the adoption itself rows nothing
	o := originOf(t, shelf)
	if o.Version != 6 || o.DesignID != siteDesignID {
		t.Errorf("an unlinked hub adopted %+v, want the answered identity", o)
	}
	ask, ok := u.lastName()
	if !ok {
		t.Fatal("the name door was never knocked on")
	}
	if _, sent := ask.head["Authorization"]; sent {
		t.Errorf("a hub with no link bore %q", ask.head.Get("Authorization"))
	}
}

// TestSiteNameHomemade is the discretion the old nil arm always had, now that there is a reason
// to state it: a title the site could never have written, or a theme that does not say which app
// it themes, is not asked about at all. Each name below misses in its own way — no separator, an
// empty name half, an empty handle, a character the site forbids — and an empty app is the case
// where the question has no shape to ask in.
func TestSiteNameHomemade(t *testing.T) {
	u := nameSiteStand(t, 6, "Skeleton by @g")
	s, _ := nameHub(t, u)
	for _, name := range []string{"My Doodad", "Plain", "weird @@ name", " by @greg", "A by @"} {
		shelfThemeFile(t, s, name)
	}
	wantSilent(t, s)
	if n := u.nameAsks(); n != 0 {
		t.Errorf("the name door saw %d requests about homemade names", n)
	}
	if n := u.idAsks(); n != 0 {
		t.Errorf("the id door saw %d requests, and nothing on this shelf is tracked", n)
	}
	for _, p := range shelfFiles(t, s.library().ThemesDir) {
		if o := readThemeOrigin(p); o != nil {
			t.Errorf("%s was adopted off a name the site could not have written: %+v", filepath.Base(p), o)
		}
		wantAbsent(t, themeGonePath(p), filepath.Base(p)+"'s gone marker")
	}

	// With no GORB_SITE the asking goes to the release base, and to nothing else. It is proven at
	// the transport rather than by dialling: a test run on a machine with a route would otherwise
	// send a real request to a real site, which is not what a test of a target is for.
	t.Setenv("GORB_SITE", "")
	dial := &nameDial{}
	s.siteClient = &http.Client{Transport: dial}
	shelfThemeFile(t, s, "Skeleton by @greg")
	wantSilent(t, s)
	if len(dial.got) != 1 || !strings.HasPrefix(dial.got[0], defaultSiteBase+"/api/hub/resolve?") {
		t.Errorf("the name ask went to %v, want one ask of the release base", dial.got)
	}
	for _, p := range shelfFiles(t, s.library().ThemesDir) {
		if o := readThemeOrigin(p); o != nil {
			t.Errorf("%s was adopted off an answer nobody could have heard: %+v", filepath.Base(p), o)
		}
	}

	// The empty app, which no container on this shelf can be: the listing names the app a theme
	// themes, and a row that does not name one cannot be asked about.
	pl := &LibraryPayload{ThemesDir: s.library().ThemesDir,
		Themes: []LibraryFile{{File: "clock-noapp.orbtheme", Title: "Skeleton by @greg", App: ""}}}
	if o := s.themeAdoptByName(httptest.NewRequest(http.MethodGet, "/api/updates", nil), pl,
		&pl.Themes[0]); o != nil {
		t.Errorf("a theme naming no app was adopted: %+v", o)
	}
	if n := u.nameAsks(); n != 0 {
		t.Errorf("a theme naming no app still knocked on the name door %d times", n)
	}
}

// TestSiteNameGateHost is the two halves of review 6 that a test can reach: the hub asks the one
// site it is configured with, and never a host a name's own handle hints at; and the gate the
// client holds shuts a host outside it before its socket opens.
//
// The row builder's own `siteAllowed` precondition is measured here rather than driven through
// GET /api/updates, and it can only be: siteReleaseBase() sanitises every GORB_SITE into an
// https origin or a loopback one, and siteAllowed lets a host that matches the configured origin
// through — so the configured site is never the one the gate shuts, and the only host a row pass
// can ask about is one it has already passed.
func TestSiteNameGateHost(t *testing.T) {
	asked := nameSiteStand(t, 6, "Skeleton by @someoneelse")
	elsewhere := nameSiteStand(t, 6, "Skeleton by @someoneelse")
	s, _ := nameHub(t, asked)
	shelf := shelfThemeFile(t, s, "Skeleton by @someoneelse")

	wantSilent(t, s) // answered at the name the shelf already holds, at the version adopted
	o := originOf(t, shelf)
	if o.Site != canonicalSiteOrigin(asked.url()) && o.Site != asked.url() {
		t.Errorf("the adoption named %q, want the configured site and not the handle's host", o.Site)
	}
	if asked.nameAsks() != 1 {
		t.Fatalf("the configured site saw %d asks, want the one", asked.nameAsks())
	}
	if n := elsewhere.nameAsks() + elsewhere.idAsks(); n != 0 {
		t.Errorf("a second site was knocked on %d times because a name named another handle", n)
	}
	ask, _ := asked.lastName()
	if got := canonicalSiteOrigin(ask.query.Get("site")); got != canonicalSiteOrigin(asked.url()) {
		t.Errorf("the site parameter named %q, want the configured origin", got)
	}

	// And the gate itself, on a host the hub could be pointed at: refused in words, with no
	// socket opened and nothing learned. The baseline is the two asks the pass above cost — the
	// by-name one and the by-id resolve the adopted theme then went on to make.
	base := asked.nameAsks() + asked.idAsks() + elsewhere.nameAsks() + elsewhere.idAsks()
	for _, host := range []string{"http://not-the-configured.site", "https://evil.example.com",
		"https://orb-os.net:8443"} {
		if _, err := s.resolveName(context.Background(), host, "clock", "Skeleton by @greg"); err == nil {
			t.Errorf("resolveName(%q) was allowed", host)
			continue
		}
		if n := asked.nameAsks() + asked.idAsks() + elsewhere.nameAsks() + elsewhere.idAsks(); n != base {
			t.Fatalf("refusing %q cost %d requests over the baseline %d: the gate sits before the socket",
				host, n-base, base)
		}
	}
}

// TestSiteNameGoneOnce is §7.2's by-name gone answer: the marker is the news, so no row is drawn,
// the marker stands, and every later pass is silent without spending a request on it. Deleting
// the marker is the operator saying ask again, and the hub does.
func TestSiteNameGoneOnce(t *testing.T) {
	u := nameSiteStand(t, 6, "Skeleton by @greg")
	s, _ := nameHub(t, u)
	u.answerGone(http.StatusNotFound)
	shelf := shelfThemeFile(t, s, "Skeleton by @greg")
	marker := themeGonePath(shelf)

	logged := swapHubLog(t)
	wantSilent(t, s)
	if _, err := os.Stat(marker); err != nil {
		t.Fatalf("a gone answer wrote no marker: %v", err)
	}
	if lines := strings.Count(logged.String(), "site resolve"); lines != 1 {
		t.Errorf("the gone answer logged %d lines, want the one: %s", lines, logged.String())
	}
	if !strings.Contains(logged.String(), "Skeleton by @greg") {
		t.Errorf("the line does not name the theme it is about: %s", logged.String())
	}
	wantAbsent(t, themeOriginPath(shelf), "sidecar")

	// Every later pass: asked once, ever.
	asks := u.nameAsks()
	wantSilent(t, s)
	wantSilent(t, s)
	if got := u.nameAsks(); got != asks {
		t.Errorf("a marked theme was asked %d more times; the marker is supposed to be the whole cost", got-asks)
	}

	// The marker's life is the theme's: removeThemeOrigin takes it with the file.
	if err := removeThemeOrigin(shelf); err != nil {
		t.Fatal(err)
	}
	wantAbsent(t, marker, "gone marker after the theme was dropped")
}

// TestSiteResolveName is the by-name client on its own: the query the door requires, the
// credential rule, and the classification table resolveOrigin already kept. Every row of that
// table is a different thing the hub does next, so every row is asserted here rather than
// inferred from one of them having been seen elsewhere.
func TestSiteResolveName(t *testing.T) {
	ctx := context.Background()

	t.Run("answered", func(t *testing.T) {
		u := nameSiteStand(t, 6, "Skeleton by @g")
		t.Setenv("GORB_SITE", u.url())
		s := newHub()
		s.statePath = filepath.Join(t.TempDir(), "state.json")
		ans, err := s.resolveName(ctx, siteReleaseBase(), "clock", "Skeleton by @greg")
		if err != nil {
			t.Fatalf("resolveName: %v", err)
		}
		if !ans.Resolved || ans.DesignID != siteDesignID || ans.Version != 6 ||
			ans.SharedName != "Skeleton by @g" || ans.App != "clock" {
			t.Errorf("answer = %+v, want the design's current version and credited name", ans)
		}
		if u.nameAsks() != 1 {
			t.Fatalf("the door saw %d by-name requests, want 1", u.nameAsks())
		}
		ask, _ := u.lastName()
		if ask.query.Get("app") != "clock" {
			t.Errorf("the request sent app=%q; the door requires it", ask.query.Get("app"))
		}
		if got := ask.query.Get("name"); got != "Skeleton by @greg" {
			t.Errorf("the name arrived as %q, want the bytes the shelf holds", got)
		}
		if got := canonicalSiteOrigin(ask.query.Get("site")); got != canonicalSiteOrigin(u.url()) {
			t.Errorf("the site parameter named %q, want the configured site", got)
		}
		if ask.query.Get("designId") != "" || ask.query.Get("version") != "" {
			t.Errorf("a name ask carried an id-path parameter: %v", ask.query)
		}
		if got := ask.head.Get("Accept"); got != "application/json" {
			t.Errorf("Accept = %q", got)
		}
		if !strings.HasPrefix(ask.head.Get("User-Agent"), "orb-manager/") {
			t.Errorf("User-Agent = %q", ask.head.Get("User-Agent"))
		}
		// Review 6 with no link at all: the ask happens, and it happens anonymously.
		if _, sent := ask.head["Authorization"]; sent {
			t.Errorf("a hub with no link sent Authorization %q", ask.head.Get("Authorization"))
		}
	})

	t.Run("bears the link's token for that site", func(t *testing.T) {
		u := nameSiteStand(t, 6, "Skeleton by @g")
		t.Setenv("GORB_SITE", u.url())
		s := newHub()
		s.statePath = filepath.Join(t.TempDir(), "state.json")
		if err := s.saveSiteLink(&siteLink{Token: "tok-1", Site: siteReleaseBase(),
			LinkedAt: 1730000000, Handle: "greg"}); err != nil {
			t.Fatal(err)
		}
		if _, err := s.resolveName(ctx, siteReleaseBase(), "clock", "Skeleton by @greg"); err != nil {
			t.Fatalf("resolveName: %v", err)
		}
		ask, _ := u.lastName()
		if got := ask.head.Get("Authorization"); got != "Bearer tok-1" {
			t.Errorf("Authorization = %q, want the link's bearer", got)
		}
	})

	// The rest of the table, one subtest per thing the caller has to tell apart.
	t.Run("gone body is Gone", func(t *testing.T) {
		for _, st := range []int{http.StatusNotFound, http.StatusGone} {
			u := nameSiteStand(t, 6, "Skeleton by @g")
			u.answerGone(st)
			t.Setenv("GORB_SITE", u.url())
			s := newHub()
			ans, err := s.resolveName(ctx, siteReleaseBase(), "clock", "Skeleton by @greg")
			if err != nil || !ans.Gone {
				t.Errorf("HTTP %d = %+v, %v, want Gone and no error", st, ans, err)
			}
		}
	})

	t.Run("other not-found is NotYet", func(t *testing.T) {
		for _, one := range []struct{ ctype, body string }{
			{"text/html; charset=utf-8", "<!doctype html><h1>404</h1>"},
			{"application/json", `{"error":"not_found"}`},
			{"", ""},
		} {
			u := nameSiteStand(t, 6, "Skeleton by @g")
			u.answerPlain(http.StatusNotFound, one.ctype, one.body)
			t.Setenv("GORB_SITE", u.url())
			s := newHub()
			ans, err := s.resolveName(ctx, siteReleaseBase(), "clock", "Skeleton by @greg")
			if err != nil || !ans.NotYet || ans.Gone {
				t.Errorf("%q = %+v, %v, want NotYet and no error", one.ctype, ans, err)
			}
		}
	})

	t.Run("refusals are errors", func(t *testing.T) {
		// A 400, a 500, a sign-in page and a 200 that names no design are four ways the site
		// says nothing the hub may act on, and each reads as an error rather than as Gone —
		// which is the difference between asking again next minute and marking a theme dead.
		for _, one := range []struct {
			name string
			h    http.Handler
		}{
			{"400", http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				w.Header().Set("Content-Type", "application/json")
				w.WriteHeader(http.StatusBadRequest)
				_, _ = w.Write([]byte(`{"error":"bad_request"}`))
			})},
			{"500", http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				w.WriteHeader(http.StatusInternalServerError)
			})},
			{"sign-in page", http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				http.Redirect(w, r, "/login", http.StatusFound)
			})},
			{"200 with no identity", http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				w.Header().Set("Content-Type", "application/json")
				_, _ = w.Write([]byte(`{"sharedName":"Skeleton by @g","version":6}`))
			})},
			{"200 that is not JSON", http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				_, _ = w.Write([]byte("pong"))
			})},
		} {
			u := nameSiteStand(t, 6, "Skeleton by @g")
			u.srv.Config.Handler = one.h
			t.Setenv("GORB_SITE", u.url())
			s := newHub()
			ans, err := s.resolveName(ctx, siteReleaseBase(), "clock", "Skeleton by @greg")
			if err == nil {
				t.Errorf("%s: answer = %+v, want an error", one.name, ans)
				continue
			}
			if ans != nil && (ans.Gone || ans.NotYet || ans.Resolved) {
				t.Errorf("%s: %+v came with an error, which is two answers", one.name, ans)
			}
		}
	})

	t.Run("unreachable is an error", func(t *testing.T) {
		closed := httptest.NewServer(http.NotFoundHandler())
		addr := closed.URL
		closed.Close()
		t.Setenv("GORB_SITE", addr)
		s := newHub()
		if _, err := s.resolveName(ctx, siteReleaseBase(), "clock", "Skeleton by @greg"); err == nil {
			t.Error("a closed port answered, which is not a thing it can do")
		}
	})

	t.Run("an empty question is not asked", func(t *testing.T) {
		s := newHub()
		for _, one := range [][3]string{{"", "clock", "A by @greg"},
			{"https://orb-os.net", "", "A by @greg"}, {"https://orb-os.net", "clock", ""}} {
			if _, err := s.resolveName(ctx, one[0], one[1], one[2]); err == nil {
				t.Errorf("resolveName(%q,%q,%q) answered a question that was never asked", one[0], one[1], one[2])
			}
		}
	})
}

// --- the silence, and the relaxed gate -------------------------------------------------

// TestSiteNameSilent is review 5's silence, on both kinds of nothing: a site whose resolve door
// answers a plain 404 (no resolve route yet, a page, a router's JSON) and a site that cannot be
// dialled at all. Neither is news about the design, so neither draws a row, a marker or a sidecar
// — and both are asked again next pass, which is the whole reason the silence is affordable.
func TestSiteNameSilent(t *testing.T) {
	for _, one := range []struct {
		name  string
		setup func(*testing.T, *nameSite) string
	}{
		{"plain 404", func(_ *testing.T, u *nameSite) string {
			u.answerPlain(http.StatusNotFound, "text/html; charset=utf-8", "<!doctype html>404")
			return u.url()
		}},
		{"unreachable", func(_ *testing.T, _ *nameSite) string {
			closed := httptest.NewServer(http.NotFoundHandler())
			addr := closed.URL
			closed.Close()
			return addr
		}},
	} {
		t.Run(one.name, func(t *testing.T) {
			u := nameSiteStand(t, 6, "Skeleton by @greg")
			s, _ := nameHub(t, u)
			t.Setenv("GORB_SITE", one.setup(t, u))
			shelf := shelfThemeFile(t, s, "Skeleton by @greg")
			logged := swapHubLog(t)

			wantSilent(t, s)
			wantAbsent(t, themeOriginPath(shelf), "sidecar")
			wantAbsent(t, themeGonePath(shelf), "gone marker")
			if lines := strings.Count(logged.String(), "site resolve"); lines != 1 {
				t.Errorf("the %s pass logged %d lines, want exactly the one: %s", one.name, lines, logged.String())
			}
			if !strings.Contains(logged.String(), "Skeleton by @greg") {
				t.Errorf("the line does not name the theme it is about: %s", logged.String())
			}

			// Ask again next time, which is what the log line is a promise of. The line is the
			// proof that reaches both cases — a socket that never opened leaves no counter
			// behind at the fake — and where the fake was dialled, its count says so too.
			wantSilent(t, s)
			if lines := strings.Count(logged.String(), "site resolve"); lines != 2 {
				t.Errorf("two %s passes logged %d lines, want one each: a silence that stops asking is a marker",
					one.name, lines)
			}
			if u.nameAsks() > 0 && u.nameAsks() != 2 {
				t.Errorf("two passes knocked on the name door %d times, want one each", u.nameAsks())
			}
		})
	}
}

// TestSiteRenameSameVersionRow is §5.4's relaxed gate on the row side: *Rename them now* leaves
// the version where it was, and the shelf still has to hear about it.
func TestSiteRenameSameVersionRow(t *testing.T) {
	u := nameSiteStand(t, 6, "Skeleton by @greg")
	s, _ := nameHub(t, u)
	trackedShelfTheme(t, s, u, "Skeleton by @greg")

	// The nothing case first, so the row below cannot be read as the gate having been dropped:
	// same version, same name, and the shelf already holds what the site is offering.
	wantSilent(t, s)

	u.moveTo(6, "Skeleton by @g")
	rows := themeRows(t, s)
	if len(rows) != 1 {
		t.Fatalf("a rename at the baked version drew %d rows, want the one: %+v", len(rows), rows)
	}
	it := rows[0]
	if !it.Renamed || it.From != "Skeleton by @greg" || it.To != "Skeleton by @g" ||
		it.Version != 6 || it.Key != siteDesignID || it.Gone {
		t.Errorf("row = %+v, want this design's rename at version 6", it)
	}
	if strings.Join(it.Choices, ",") != "update,skip,keep" {
		t.Errorf("choices = %v: a rename row is an ordinary update row, offered the same three answers", it.Choices)
	}

	// Skip silences it keyed by the version the row names, exactly as a newer version's skip
	// does: the answer is about one version, not about the design forever.
	if env := postRefusal(t, s, "/api/updates/skip",
		map[string]any{"kind": "theme", "key": siteDesignID, "version": 6}); env["ok"] != true {
		t.Fatalf("skip: %v", env)
	}
	wantSilent(t, s)
	if c := choiceInState(t, s, "theme", siteDesignID); c.SkippedVersion != 6 || c.Keep {
		t.Errorf("state choice = %+v, want a same-version skip and no keep", c)
	}

	// And the row is back the moment there is a newer answer to offer: the skip held one version,
	// and the name is still different on the shelf, which a skipped rename leaves true.
	u.moveTo(7, "Skeleton by @g")
	rows = themeRows(t, s)
	if len(rows) != 1 || rows[0].Version != 7 || !rows[0].Renamed {
		t.Fatalf("v7 after a skipped v6 drew %+v, want the update row at the new version", rows)
	}
}

// TestSiteRenameSkip is the sentence §7.2 ends on: a row built from an adopted theme is keyed by
// the design id the answer named, so the skip and keep doors need no new arm of their own. Skip
// is keyed by the version the row names and holds only that version. Keep deletes the record and
// the tracking with it — and because the choice is keyed by an id that has just left the shelf,
// what it cannot do is un-title the file, so the theme pays the one anonymous question every
// unrecorded name pays and the answer is refused rather than adopted.
func TestSiteRenameSkip(t *testing.T) {
	u := nameSiteStand(t, 6, "Skeleton by @g")
	s, _ := nameHub(t, u)
	tracked := shelfThemeFile(t, s, "Skeleton by @greg")

	rows := themeRows(t, s)
	if len(rows) != 1 || rows[0].Key != siteDesignID || rows[0].Version != 6 {
		t.Fatalf("the adoption pass drew %+v, want the rename keyed by the answered design id", rows)
	}

	if env := postRefusal(t, s, "/api/updates/skip",
		map[string]any{"kind": "theme", "key": siteDesignID, "version": 6}); env["ok"] != true {
		t.Fatalf("skip: %v", env)
	}
	wantSilent(t, s)
	if c := choiceInState(t, s, "theme", siteDesignID); c.SkippedVersion != 6 {
		t.Errorf("state choice = %+v, want the skip keyed by the answered version", c)
	}

	// Keep, on the same key: the record goes and the asking stops with it.
	if env := postRefusal(t, s, "/api/updates/keep",
		map[string]any{"kind": "theme", "key": siteDesignID}); env["ok"] != true {
		t.Fatalf("keep: %v", env)
	}
	if o := readThemeOrigin(tracked); o != nil {
		t.Errorf("keep left the adopted record behind: %+v", o)
	}
	wantSilent(t, s)
	// What a keep cannot do is un-title a shelf file, and the design id it keyed itself on is
	// gone from the shelf with the record. So an unrecorded theme is still one anonymous question
	// per pass — which the answer then refuses to adopt, drawing no row and writing no sidecar.
	// The retirement is in the tracking, not in the dialling: a second source of truth on disk to
	// avoid one question is the thing §7.2's marker design avoids on purpose.
	asks := u.nameAsks()
	wantSilent(t, s)
	if got := u.nameAsks(); got != asks+1 {
		t.Errorf("a kept theme cost %d asks; it is unrecorded, so it pays the one every unrecorded name pays", got-asks)
	}
	if o := readThemeOrigin(tracked); o != nil {
		t.Errorf("the answer to a kept theme's name resurrected its record: %+v", o)
	}
}

// TestSiteRenameVersionName is review 14's general arm of the gate: a shared name that moved at
// the version already baked. The site never builds such a version itself (ruling 29), so the hub
// cannot be written to answer only the handle-migration shape — it answers the answer, not the
// history.
func TestSiteRenameVersionName(t *testing.T) {
	u := nameSiteStand(t, 6, "Old by @greg")
	s, _ := nameHub(t, u)
	trackedShelfTheme(t, s, u, "Old by @greg")

	u.moveTo(6, "New by @greg")
	rows := themeRows(t, s)
	if len(rows) != 1 {
		t.Fatalf("a name that moved at the baked version drew %d rows, want the one: %+v", len(rows), rows)
	}
	it := rows[0]
	if !it.Renamed || it.From != "Old by @greg" || it.To != "New by @greg" || it.Version != 6 {
		t.Errorf("row = %+v, want Old by @greg → New by @greg at version 6", it)
	}
}

// sameVersionApplyHub is applyHub's fixture with one thing moved: the site renamed the design and
// left the version where it stood, which is §5.4's row seen from the apply door. Every other part
// of it — the orb wearing the old theme, the bake seam, the bundle door — is applyHub's, so the
// claims below can be the existing apply test's.
func sameVersionApplyHub(t *testing.T, to string) (*Server, *orbfake.Orb, string, *updateSite, string) {
	t.Helper()
	u := updatesSite(t, 1, "Aurora", "art")
	fake := orbfake.New(newAurora(true))
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
	u.moveTo(1, to)
	u.bundle = func() []byte {
		return goodBundle(t, siteDesignID, 1, "clock", to, pngArt(9, 9, artSeed("art v1 renamed")))
	}
	return s, orb, ip, u, old
}

// TestSiteRenameApply is the press, not the row: an equal-version answer naming a different
// shared name runs the same three steps the newer-version rename runs, in the same order, and the
// shelf ends renamed. And the gate is a gate, not a rubber stamp: where neither the version nor
// the name moved, the door still answers today's sentence.
func TestSiteRenameApply(t *testing.T) {
	s, orb, ip, _, old := sameVersionApplyHub(t, "Aurora Fields")
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
	if upload > del {
		t.Errorf("the old theme was deleted at %d before the new one was written at %d: %v", del, upload, calls)
	}
	if !strings.Contains(calls[del], oldID) {
		t.Errorf("the delete did not name the old theme's id %s: %s", oldID, calls[del])
	}
	if strings.Contains(calls[upload], "keep=1") {
		t.Error("an orb wearing the old theme was asked to keep the new one unselected: a rename must move its face")
	}

	files := shelfFiles(t, s.lib.ThemesDir)
	if len(files) != 1 {
		t.Fatalf("the shelf holds %d themes, not the one the update left: %v", len(files), files)
	}
	if filepath.Base(files[0]) == filepath.Base(old) {
		t.Error("the only shelf file is still the old-named one")
	}
	o := readThemeOrigin(files[0])
	if o == nil || o.Version != 1 || o.DesignID != siteDesignID {
		t.Errorf("the new shelf copy has no v1 origin for the design: %+v", o)
	}
	if _, err := os.Stat(old); !os.IsNotExist(err) {
		t.Errorf("the old shelf file is still there: %v", err)
	}
	rows := orb.Themes()
	if len(rows) != 1 || rows[0].Name != "Aurora Fields" || !rows[0].Active {
		t.Errorf("the orb does not wear the new theme: %+v", rows)
	}

	// And the gate still gates: put the answer back to the name that is now baked, so neither the
	// version nor the name moves, and the door refuses in the words it has always used.
	if msg := applyErr(t, applyOnce(t, s, map[string]any{"kind": "theme", "key": siteDesignID})); !strings.Contains(msg, "no newer version") {
		t.Errorf("an apply where nothing moved refused with %q, want the no-newer-version sentence", msg)
	}
}
