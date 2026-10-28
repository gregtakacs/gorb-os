package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// The session's card memory and the read-ahead pass. These tests speak in fake
// orb calls, the way the theme suite always has: the claim being pinned is not
// "the JSON looked right" but "the hub stopped walking the board for answers it
// already had", and only the call log can say that.

// countCalls counts fake-orb request lines matching a predicate, so a test can
// say "the card walk happened exactly once" rather than trust a comment.
func countCalls(f *orbfake.Orb, want func(line string) bool) int {
	n := 0
	for _, l := range f.Calls() {
		if want(l) {
			n++
		}
	}
	return n
}

func sdWalks(f *orbfake.Orb) int {
	// The fake records method, path and query verbatim; the sd list read is the
	// one bare "GET /api/v1/sd" line, which separates it from the preview and
	// rescan doors hanging off the same path.
	return countCalls(f, func(l string) bool { return l == "GET /api/v1/sd" })
}

func planeDials(f *orbfake.Orb) int {
	return countCalls(f, func(l string) bool { return strings.HasPrefix(l, "GET /api/v1/sd/preview") })
}

// ledgerWalks counts the app-ledger reads, the enumeration's other half. A paint
// owes exactly one: the face wants the ledger once, and a second walk of it is a
// second wait the operator pays for before the first thumbnail appears.
func ledgerWalks(f *orbfake.Orb) int {
	return countCalls(f, func(l string) bool { return l == "GET /api/v1/apps" })
}

func warmGet(t *testing.T, s *Server, mux *http.ServeMux, path string) *httptest.ResponseRecorder {
	t.Helper()
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, path, nil)
	req.Header.Set(tokenHeader, s.token)
	mux.ServeHTTP(rec, req)
	return rec
}

// waitForWarm polls the status route (which dials nothing) until the pass
// reports done or failed. The bound prices fake dials on a slow CI, not any
// device patience.
func waitForWarm(t *testing.T, s *Server, mux *http.ServeMux, ip string) map[string]any {
	t.Helper()
	deadline := time.Now().Add(8 * time.Second)
	for time.Now().Before(deadline) {
		rec := warmGet(t, s, mux, "/api/theme/warm-status?ip="+ip)
		var env map[string]any
		if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
			t.Fatalf("warm-status body: %v", err)
		}
		data, _ := env["data"].(map[string]any)
		if data != nil && (data["phase"] == "done" || data["phase"] == "failed") {
			return data
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("the read-ahead pass never finished")
	return nil
}

func cardRowID(t *testing.T, s *Server, ip string) any {
	t.Helper()
	env := decodeEnv(t, postTheme(t, s, "/api/theme/list", themeBody(ip, "")))
	data, _ := env["data"].(map[string]any)
	rows, _ := data["themes"].([]any)
	if len(rows) == 0 {
		t.Fatalf("the seeded card holds no rows")
	}
	row, _ := rows[0].(map[string]any)
	return row["id"]
}

func bytesLike(a, b []byte) bool { return string(a) == string(b) }

// The Theme page's first two reads are the same two card walks every open paid
// before: the sd list and the enumeration. The memory answers the second open
// with zero dials, and the route's envelope is identical either way, so the
// page cannot tell a replay from a relay — and neither can the board.
func TestThemeListIsRememberedUntilAWrite(t *testing.T) {
	s, ip, _, f := fakeOrbHub(t)
	if got := sdWalks(f); got != 0 {
		t.Fatalf("the hub dialed the card %d times before anyone asked", got)
	}
	first := postTheme(t, s, "/api/theme/list", themeBody(ip, ""))
	rec2 := postTheme(t, s, "/api/theme/list", themeBody(ip, ""))
	if rec2.Code != http.StatusOK {
		t.Fatalf("second list = %d", rec2.Code)
	}
	if got := sdWalks(f); got != 1 {
		t.Errorf("two paints walked the card %d times, want exactly the first one", got)
	}
	env1, env2 := decodeEnv(t, first), decodeEnv(t, rec2)
	d1, _ := env1["data"].(map[string]any)
	d2, _ := env2["data"].(map[string]any)
	if d1["active"] != d2["active"] || d2["active"] != "aurora" {
		t.Errorf("the replay changed the answer: %v vs %v", d1["active"], d2["active"])
	}

	// A delete is a card mutation: the memory retires, and the next read walks.
	id := asString(cardRowID(t, s, ip))
	if rec := postTheme(t, s, "/api/theme/delete", themeBody(ip, id)); rec.Code != http.StatusOK {
		t.Fatalf("delete = %d %s", rec.Code, rec.Body)
	}
	before := sdWalks(f)
	if rec := postTheme(t, s, "/api/theme/list", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("list after delete = %d", rec.Code)
	}
	if got := sdWalks(f) - before; got != 1 {
		t.Errorf("after a delete the next list dialed %d times, want one honest re-read", got)
	}
}

// The read-ahead pass is the operator's ask in one route: after the list lands,
// every plane on it fills the hub's cache before the presses that would pay
// for it. The pass must dial each plane exactly once, report its progress, and
// leave the page's own fetches served from the cache it filled.
// TestOnePaintWalksTheCardOnce pins the shape of a cold first paint: the page
// reads the enumeration and then the rows, and the two must cost ONE walk of the
// card between them, not two.
//
// The enumeration wants the app ledger and the theme rows, and the rows are the
// expensive half — GET /api/v1/sd walks the FAT on the device's one httpd task.
// When the enumeration dialed that walk inside itself, the page paid it once for
// the tabs and again for the rows a moment later, and the second walk is pure
// waste: same card, same instant, same answer. On a board already serving a plane
// each walk ran seconds, and the pair of them is what the owner reported as a
// Themes page that spun until the browser gave up on it (2026-10-28, "signal
// timed out" — Chrome's own words for a fetch the hub never got to answer).
func TestOnePaintWalksTheCardOnce(t *testing.T) {
	s, ip, _, f := fakeOrbHub(t)
	mux := http.NewServeMux()
	s.routes(mux)

	if rec := warmGet(t, s, mux, "/api/theme/apps?ip="+ip); rec.Code != http.StatusOK {
		t.Fatalf("apps = %d %s", rec.Code, rec.Body)
	}
	if got := sdWalks(f); got != 1 {
		t.Errorf("the enumeration walked the card %d times, want the one walk it can share", got)
	}
	if got := ledgerWalks(f); got != 1 {
		t.Errorf("the enumeration walked the ledger %d times, want one", got)
	}
	if rec := postTheme(t, s, "/api/theme/list", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("list = %d %s", rec.Code, rec.Body)
	}
	if got := sdWalks(f); got != 1 {
		t.Errorf("after the rows read the card had been walked %d times, want the enumeration's one walk to have served both", got)
	}

	// The shared walk must still carry the enumeration its counts, or the saving
	// is a lie told to the call log: the tabs are drawn from those rows.
	env := decodeEnv(t, warmGet(t, s, mux, "/api/theme/apps?ip="+ip))
	d, _ := env["data"].(map[string]any)
	apps, _ := d["apps"].([]any)
	if len(apps) == 0 {
		t.Fatalf("the enumeration named no apps off the shared card read: %v", env)
	}
	clock, _ := apps[0].(map[string]any)
	if clock["slug"] != "clock" || clock["count"] != float64(1) {
		t.Errorf("the shared read lost the counts: %v", clock)
	}
}

// mustRead reads a source file the test inspects as text, not as a fixture.
func mustRead(t *testing.T, path string) []byte {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("%s: %v", path, err)
	}
	return b
}

// jsCallBudget pulls one route's per-request fetch budget out of app.js in
// milliseconds, matching the call site rather than a frozen literal — so the test
// keeps holding after the next retune instead of passing against a stale number.
func jsCallBudget(t *testing.T, js, callSite string) time.Duration {
	t.Helper()
	m := regexp.MustCompile(regexp.QuoteMeta(callSite) + `,\s*(\d+)\)`).FindStringSubmatch(js)
	if m == nil {
		t.Fatalf("no fetch budget found for %s — app.js changed its call shape", callSite)
	}
	ms, err := strconv.Atoi(m[1])
	if err != nil {
		t.Fatalf("fetch budget for %s is not a number: %v", callSite, err)
	}
	return time.Duration(ms) * time.Millisecond
}

// goDurationConst pulls a `= N * time.Second` constant out of Go source.
func goDurationConst(t *testing.T, src, name string) time.Duration {
	t.Helper()
	m := regexp.MustCompile(regexp.QuoteMeta(name+" = ") + `([0-9 *]*time\.Second)`).FindStringSubmatch(src)
	if m == nil {
		t.Fatalf("%s not found in the hub's source", name)
	}
	n := regexp.MustCompile(`(\d+)`).FindString(m[1])
	v, err := strconv.Atoi(n)
	if err != nil {
		t.Fatalf("%s: %v", name, err)
	}
	return time.Duration(v) * time.Second
}

func TestTheHubAnswersBeforeThePageGivesUp(t *testing.T) {
	theme := string(mustRead(t, "theme.go"))
	js := string(mustRead(t, filepath.Join("web", "app.js")))
	pageRide := goDurationConst(t, theme, "pageRideBudget")
	rescanRide := goDurationConst(t, theme, "rescanRideBudget")

	for _, p := range []struct {
		name      string
		hub, page time.Duration
	}{
		{"the card read (list)", pageRide, jsCallBudget(t, js, `themePost("/api/theme/list", { ip }`)},
		{"the enumeration (apps)", pageRide, jsCallBudget(t, js, `themeGet("/api/theme/apps?ip=" + encodeURIComponent(ip)`)},
		{"the card re-read (rescan)", rescanRide, jsCallBudget(t, js, `themePost("/api/theme/rescan", { ip: themeIP }`)},
	} {
		if p.hub >= p.page {
			t.Fatalf("%s: the hub rides for %v but the page aborts at %v — the browser times "+
				"out first and the card prints its own AbortError where the orb's reason "+
				"belongs. Raise the page budget or shorten the ride.", p.name, p.hub, p.page)
		}
		if p.page-p.hub < 10*time.Second {
			t.Fatalf("%s: the hub rides for %v and the page aborts at %v — under 10 s of "+
				"daylight leaves the browser first on any hub slower than the bound it was "+
				"priced at", p.name, p.hub, p.page)
		}
	}
	// The failure Debounce must not swallow the page's retry: a retry that echoes a
	// fresh failure instead of dialing is what made the retry button look broken.
	if sdFailWindow >= 2300*time.Millisecond {
		t.Fatalf("the failure Debounce of %v swallows the page's 2.3 s retry; keep it shorter "+
			"so a retry after a real failure actually dials", sdFailWindow)
	}
}

// mustConst pulls the integer the page source passes at a call site, in
// milliseconds, so the test compares the hub's constants against the file the
// browser actually loads rather than against a number copied into a test.
func mustConst(t *testing.T, src, marker string) int {
	t.Helper()
	i := strings.Index(src, marker)
	if i < 0 {
		t.Fatalf("the page no longer contains %q; the budget pairing this test exists to enforce cannot be read", marker)
	}
	rest := src[i+len(marker):]
	j := 0
	for j < len(rest) && rest[j] >= '0' && rest[j] <= '9' {
		j++
	}
	if j == 0 {
		t.Fatalf("no millisecond constant follows %q in the page", marker)
	}
	n, err := strconv.Atoi(rest[:j])
	if err != nil {
		t.Fatalf("%q: %v", marker, err)
	}
	return n
}

func TestThemeWarmFillsThePlanesBeforeThePresses(t *testing.T) {
	f := orbfake.New(
		orbfake.Theme{Name: "aurora", Bytes: 99, Serial: 1, OK: true, Active: true},
		orbfake.Theme{Name: "lattice", Bytes: 120, Serial: 2, OK: true},
	)
	s, ip, _, fake := fakeOrbHubWith(t, f)
	mux := http.NewServeMux()
	s.routes(mux)
	plane := append([]byte{2, 0, 1, 0}, []byte{0x12, 0x34, 0x12, 0x34}...)
	for _, name := range []string{"aurora", "lattice"} {
		id, err := orbnet.ThemeIDOf("clock", name)
		if err != nil {
			t.Fatal(err)
		}
		fake.SetPreview(id, plane)
	}

	if rec := postTheme(t, s, "/api/theme/list", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("list = %d", rec.Code)
	}
	if got := planeDials(fake); got != 0 {
		t.Fatalf("the list itself dialed %d planes; the page queue owns that, not the list", got)
	}
	if rec := postTheme(t, s, "/api/theme/warm", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("warm = %d %s", rec.Code, rec.Body)
	}
	view := waitForWarm(t, s, mux, ip)
	if view["phase"] != "done" {
		t.Fatalf("pass ended in %v (%v), want done", view["phase"], view["note"])
	}
	if got := planeDials(fake); got != 2 {
		t.Errorf("the pass dialed %d planes for a two-row card, want exactly 2", got)
	}

	// The press the pass existed for: the row's fetch answers from the cache,
	// and the board sees nothing.
	id, _ := orbnet.ThemeIDOf("clock", "aurora")
	before := len(fake.Calls())
	rec := warmGet(t, s, mux, "/api/theme/preview?ip="+ip+"&id="+id)
	if rec.Code != http.StatusOK || !bytesLike(rec.Body.Bytes(), plane) {
		t.Fatalf("warmed preview = %d %d B, want the cached plane", rec.Code, rec.Body.Len())
	}
	if got := len(fake.Calls()) - before; got != 0 {
		t.Errorf("a warmed press still dialed the orb %d times", got)
	}

	// Kicking a card that is already warm costs bookkeeping, not the lane: an
	// idempotent route the page may knock on every paint.
	if rec := postTheme(t, s, "/api/theme/warm", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("second warm = %d", rec.Code)
	}
	waitForWarm(t, s, mux, ip)
	if got := planeDials(fake); got != 2 {
		t.Errorf("the second pass re-dialed planes: %d dials, want still 2", got)
	}
}

// The pass and the page want the same plane in the same second. The single
// flight turns the collision into one dial and many answers; without it, every
// thumbnail pressed while the hub is already ahead of the press costs the card
// a second stall for the same bytes.
func TestAWarmedPressRidesThePassNotASecondDial(t *testing.T) {
	f := orbfake.New(orbfake.Theme{Name: "aurora", Bytes: 99, Serial: 1, OK: true, Active: true})
	s, ip, _, fake := fakeOrbHubWith(t, f)
	mux := http.NewServeMux()
	s.routes(mux)
	id, _ := orbnet.ThemeIDOf("clock", "aurora")
	fake.SetPreview(id, append([]byte{2, 0, 1, 0}, []byte{0x9a, 0xff, 0x9a, 0xff}...))
	if rec := postTheme(t, s, "/api/theme/list", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("list = %d", rec.Code)
	}
	if rec := postTheme(t, s, "/api/theme/warm", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("warm = %d", rec.Code)
	}

	var wg sync.WaitGroup
	codes := make([]int, 8)
	for i := range codes {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			codes[i] = warmGet(t, s, mux, "/api/theme/preview?ip="+ip+"&id="+id).Code
		}(i)
	}
	wg.Wait()
	for i, c := range codes {
		if c != http.StatusOK {
			t.Fatalf("fetch %d answered %d, want every rider to get the plane", i, c)
		}
	}
	if got := planeDials(fake); got != 1 {
		t.Errorf("simultaneous fetches walked the card %d times, want one", got)
	}
}

// Rescan is the operator's word for "the card changed in ways I have not told
// the hub". It answers fresh, retires the memory, and refills the list itself:
// the page's next list read must cost zero walks because the rescan handler
// just paid one on the press's behalf.
func TestRescanRefillsTheMemoryItJustEmptied(t *testing.T) {
	s, ip, _, fake := fakeOrbHub(t)
	mux := http.NewServeMux()
	s.routes(mux)
	if rec := postTheme(t, s, "/api/theme/list", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("list = %d", rec.Code)
	}
	if rec := postTheme(t, s, "/api/theme/rescan", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("rescan = %d %s", rec.Code, rec.Body)
	}
	waitForWarm(t, s, mux, ip)
	before := sdWalks(fake)
	if rec := postTheme(t, s, "/api/theme/list", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("list after rescan = %d", rec.Code)
	}
	if got := sdWalks(fake) - before; got != 0 {
		t.Errorf("the list after a rescan walked the card %d more times; the rescan's refill should have covered it", got)
	}
}

// ---- the scheduler: order, boost, yield, speculation ---------------------------

// firstCallIdx reports the position of the first recorded call matching want, or
// -1 when the board never saw one. Order claims are the whole subject here, so
// the tests speak in positions rather than counts.
func firstCallIdx(f *orbfake.Orb, want func(line string) bool) int {
	for i, l := range f.Calls() {
		if want(l) {
			return i
		}
	}
	return -1
}

func isPlaneDial(line string) bool {
	return strings.HasPrefix(line, "GET /api/v1/sd/preview")
}

func isLedgerDial(line string) bool { return line == "GET /api/v1/apps" }

func warmPost(t *testing.T, s *Server, body string) {
	t.Helper()
	if rec := postTheme(t, s, "/api/theme/warm", body); rec.Code != http.StatusOK {
		t.Fatalf("warm = %d %s", rec.Code, rec.Body)
	}
}

// The row the glass is wearing is the row the next click touches. The pass draws
// its plane before any other thumbnail, whatever order the card lists them in.
func TestTheWarmPassDrawsTheActiveRowFirst(t *testing.T) {
	f := orbfake.New(
		orbfake.Theme{Name: "boring", Bytes: 99, Serial: 1, OK: true},
		orbfake.Theme{Name: "aurora", Bytes: 99, Serial: 1, OK: true, Active: true},
	)
	s, ip, _, fake := fakeOrbHubWith(t, f)
	mux := http.NewServeMux()
	s.routes(mux)
	idAurora, _ := orbnet.ThemeIDOf("clock", "aurora")
	idBoring, _ := orbnet.ThemeIDOf("clock", "boring")
	plane := append([]byte{2, 0, 1, 0}, []byte{0x9a, 0xff, 0x9a, 0xff}...)
	fake.SetPreview(idAurora, plane)
	fake.SetPreview(idBoring, plane)
	if rec := postTheme(t, s, "/api/theme/list", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("list = %d", rec.Code)
	}
	warmPost(t, s, `{"ip":"`+ip+`","face":"themes"}`)
	waitForWarm(t, s, mux, ip)
	if got := firstCallIdx(fake, func(l string) bool { return isPlaneDial(l) && strings.Contains(l, "id="+idAurora) }); got < 0 {
		t.Fatalf("the active row's plane was never dialed: %#v", fake.Calls())
	}
	if got := firstCallIdx(fake, func(l string) bool { return isPlaneDial(l) && strings.Contains(l, "id="+idBoring) }); got == 0 || got == firstCallIdx(fake, isPlaneDial) {
		t.Errorf("the pass drew an inactive plane (%d) before/at the active one — the row on the glass goes first", got)
	}
}

// The face under the hand outranks the pass's own guess: a press on the Apps
// face makes the pass run its enumeration walk before the next plane, and a
// press on Themes keeps the planes in front of the enumeration. Same kick,
// different body, provably different order on the wire.
func TestTheFaceUnderTheHandReordersThePass(t *testing.T) {
	for _, tc := range []struct {
		face         string
		ledgerBefore bool
	}{
		{face: "apps", ledgerBefore: true},
		{face: "themes", ledgerBefore: false},
	} {
		f := orbfake.New(orbfake.Theme{Name: "aurora", Bytes: 99, Serial: 1, OK: true, Active: true})
		s, ip, _, fake := fakeOrbHubWith(t, f)
		mux := http.NewServeMux()
		s.routes(mux)
		if rec := postTheme(t, s, "/api/theme/list", themeBody(ip, "")); rec.Code != http.StatusOK {
			t.Fatalf("list = %d", rec.Code)
		}
		warmPost(t, s, `{"ip":"`+ip+`","face":"`+tc.face+`"}`)
		waitForWarm(t, s, mux, ip)
		led, pv := firstCallIdx(fake, isLedgerDial), firstCallIdx(fake, isPlaneDial)
		if led < 0 || pv < 0 {
			t.Fatalf("face %q: ledger at %d, plane at %d — the pass owes both reads: %#v", tc.face, led, pv, fake.Calls())
		}
		if tc.ledgerBefore && led > pv {
			t.Errorf("face %q: the pass dialed the plane (%d) before the ledger (%d) — the pressed face goes first", tc.face, pv, led)
		}
		if !tc.ledgerBefore && pv > led {
			t.Errorf("face %q: the pass dialed the ledger (%d) before the plane (%d) — themes outrank regions by default", tc.face, led, pv)
		}
	}
}

// A page with a live preview queue is already fetching every plane, through the
// same single flight, caching every 200 the pass would have cached. The pass's
// plane picks at that moment are pure contention, so they come off the list and
// the pass moves to the regions it alone can do.
func TestALivePageQueueEndsThePassPlanePhase(t *testing.T) {
	f := orbfake.New(
		orbfake.Theme{Name: "aurora", Bytes: 99, Serial: 1, OK: true, Active: true},
		orbfake.Theme{Name: "boring", Bytes: 99, Serial: 2, OK: true},
	)
	s, ip, _, fake := fakeOrbHubWith(t, f)
	mux := http.NewServeMux()
	s.routes(mux)
	if rec := postTheme(t, s, "/api/theme/list", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("list = %d", rec.Code)
	}
	// The page's queue: a grammar-valid id the card does not carry answers a
	// quiet 404 and still stamps the page's demand — which is the half the pass
	// reads. It lands BEFORE the kick, and the kick must not wash it away: a
	// queue that stamped seconds ago is a queue still streaming, and
	// clearPageDemand keeps a fresh plane demand for exactly this (see its
	// comment). The press asked for the pass; the queue still owns the planes.
	idGone, _ := orbnet.ThemeIDOf("watch", "noon")
	if rec := warmGet(t, s, mux, "/api/theme/preview?ip="+ip+"&id="+idGone); rec.Code != http.StatusNotFound {
		t.Fatalf("preview of an uncarried id = %d, want the quiet 404", rec.Code)
	}
	warmPost(t, s, `{"ip":"`+ip+`","face":"themes"}`)
	data := waitForWarm(t, s, mux, ip)
	if got := planeDials(fake); got != 1 {
		t.Errorf("the pass dialed %d planes beside the page's own queue, want only the page's 1", got)
	}
	if note, _ := data["note"].(string); !strings.Contains(note, "page") {
		t.Errorf("the pass closed with note %q, want the sentence naming the page's queue", note)
	}
}

// A board the sweep just watched come alive is a board someone is about to
// open. The speculative read fills the card memory before any page asks, so the
// first Themes paint costs the operator zero walks — and a second knock while
// the answers are fresh dials nothing at all.
func TestALiveBoardIsWarmedBeforeAnyoneAsks(t *testing.T) {
	s, ip, _, fake := fakeOrbHub(t)
	mux := http.NewServeMux()
	s.routes(mux)
	s.warmSpeculative(ip, false)
	waitForWarm(t, s, mux, ip)
	if got := sdWalks(fake); got != 1 {
		t.Fatalf("the speculative door walked the card %d times, want the one read", got)
	}
	before := sdWalks(fake)
	if rec := postTheme(t, s, "/api/theme/list", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("list = %d", rec.Code)
	}
	if got := sdWalks(fake) - before; got != 0 {
		t.Errorf("the first page read after a speculative warm walked the card %d more times, want 0", got)
	}
	s.warmSpeculative(ip, false) // fresh answers: the door must be a glance, not a dial
	if got := sdWalks(fake); got != before {
		t.Errorf("the second speculative knock redialed the card: %d walks", got)
	}
}

// The sweeper's edge is unknown-or-dead-to-alive: a board the hub never asked
// about (launch) and a board that came back (wake, reboot) both knock once each,
// and an alive board that merely answers again buys no re-warm.
func TestTheLiveEdgeKnocksOncePerReturn(t *testing.T) {
	c := newLiveCache()
	alive := true
	c.probe = func(string) (bool, bool) { return alive, true }
	var hits []string
	c.onLive = func(ip string) { hits = append(hits, ip) }
	c.check("192.0.2.7") // unknown -> alive: the launch edge
	c.check("192.0.2.7") // alive -> alive: no news
	alive = false
	c.check("192.0.2.7") // alive -> dead: dead news is not a warm reason
	alive = true
	c.check("192.0.2.7") // dead -> alive: the wake edge
	if len(hits) != 2 {
		t.Fatalf("onLive knocked %d times for unknown->alive then dead->alive, want 2: %v", len(hits), hits)
	}
}

// The exact crash the live orb caught. Two rows and one editable app, kicked
// with NO face hint: the plane queue drains, the regions queue is not empty, and
// the loop must walk into the regions — the first scheduler revision indexed
// planes[0] on that transition and panicked the process, which is a dead hub and
// a browser that says "Failed to fetch" to everything after it. The fakes never
// caught it because the seeded card app ships no editor: with an empty regions
// queue the pass broke out one step early. A test hub that panics fails this
// test by dying, which is the loudest honest assertion available here.
func TestThePassWalksRegionsAfterPlanesWithNoHint(t *testing.T) {
	region := zipWith(t, map[string]string{
		"editor.json": `{"editor":1,"entry":"index.html","baker":3,"runtime":["support"]}`,
		"index.html":  `<body><h1>woken early</h1></body>`,
	})
	f := orbfake.New(
		orbfake.Theme{Name: "boring", Bytes: 99, Serial: 1, OK: true},
		orbfake.Theme{Name: "aurora", Bytes: 99, Serial: 2, OK: true, Active: true},
	).WithCardApps(orbfake.CardApp{
		Stem: "clock", Slug: "clock", Title: "Clock", Caps: capThemes,
		Installed: true, Enabled: true, Editor: region,
	})
	s, ip, _, fake := fakeOrbHubWith(t, f)
	mux := http.NewServeMux()
	s.routes(mux)
	idAurora, _ := orbnet.ThemeIDOf("clock", "aurora")
	idBoring, _ := orbnet.ThemeIDOf("clock", "boring")
	plane := append([]byte{2, 0, 1, 0}, []byte{0x9a, 0xff, 0x9a, 0xff}...)
	fake.SetPreview(idAurora, plane)
	fake.SetPreview(idBoring, plane)
	if rec := postTheme(t, s, "/api/theme/list", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("list = %d", rec.Code)
	}
	warmPost(t, s, `{"ip":"`+ip+`"}`) // no face: the plain likelihood order
	data := waitForWarm(t, s, mux, ip)
	if data["phase"] != "done" {
		t.Fatalf("the no-hint pass closed %q (%v), want it to walk its regions and close done", data["phase"], data["note"])
	}
	if got := countCalls(fake, func(l string) bool { return strings.HasPrefix(l, "GET /api/v1/apps/editor") }); got == 0 {
		t.Errorf("the no-hint pass ran no region dial — the planes-to-regions transition is where the hub used to panic: %#v", fake.Calls())
	}
}

// The zone-table failure this pass was written against, in one test: the hub
// boots, the speculative walk takes the lane, the operator opens the orb and
// the orb's config read meets either a busy decline or a deadline behind a
// walk it cannot outrun. The claim pinned here is the fix's whole shape — a
// page-facing dial INTERRUPTS the insurance rather than queueing behind it,
// and the interrupted pass is not struck for a failure it did not commit.
func TestAZoneTableReadCutsTheSpeculativeWalkBehindIt(t *testing.T) {
	f := orbfake.New(orbfake.Theme{Name: "aurora", Bytes: 99, Serial: 1, OK: true, Active: true})
	s, ip, _, fake := fakeOrbHubWith(t, f)
	mux := http.NewServeMux()
	s.routes(mux)
	f.SDWait = 5 * time.Second // a cold walk on a big card, genuinely in flight
	go s.warmSpeculative(ip, true)

	// Catch the walk open: the sd dial is recorded the instant it is dialed,
	// and the walk will not answer for five seconds unless it is cut.
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) && sdWalks(fake) == 0 {
		time.Sleep(5 * time.Millisecond)
	}
	if sdWalks(fake) == 0 {
		t.Fatalf("the speculative walk never dialed the card: %#v", fake.Calls())
	}

	// The orb face's config read, through the hub's proxy exactly as the page
	// dials it. The answer's CONTENT does not matter; its LATENCY and status do.
	began := time.Now()
	rec := warmGet(t, s, mux, "/orb/"+ip+"/api/v1/config")
	elapsed := time.Since(began)
	if rec.Code != http.StatusOK {
		t.Fatalf("the config read behind the walk = %d (%s), want 200", rec.Code, rec.Body.String())
	}
	if elapsed > 1500*time.Millisecond {
		t.Fatalf("the config read waited %s behind a speculative walk, want the walk cut inside a second and a half", elapsed)
	}
	if got := sdWalks(fake); got != 1 {
		t.Fatalf("the walk was dialed %d times: the interrupt must end the ONE walk, not spawn retries", got)
	}
}

// The same claim from the pass's side: a cut dial does not count against the
// card. Three strikes retire the pass; a hub that struck for its own
// interruptions would answer a Themes press after heavy page traffic with an
// idle card and a note blaming it.
func TestThePassIsNotStruckForADemandThatCutIt(t *testing.T) {
	f := orbfake.New(
		orbfake.Theme{Name: "aurora", Bytes: 99, Serial: 1, OK: true, Active: true},
		orbfake.Theme{Name: "boring", Bytes: 99, Serial: 2, OK: true},
	)
	s, ip, _, fake := fakeOrbHubWith(t, f)
	mux := http.NewServeMux()
	s.routes(mux)
	if rec := postTheme(t, s, "/api/theme/list", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("list = %d", rec.Code)
	}
	f.PreviewWait = 5 * time.Second
	warmPost(t, s, `{"ip":"`+ip+`","face":"themes"}`)
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) && planeDials(fake) == 0 {
		time.Sleep(5 * time.Millisecond)
	}
	if planeDials(fake) == 0 {
		t.Fatalf("the pass never dialed a plane: %#v", fake.Calls())
	}
	// The demand: a config read through the proxy, mid-plane.
	if rec := warmGet(t, s, mux, "/orb/"+ip+"/api/v1/config"); rec.Code != http.StatusOK {
		t.Fatalf("config behind the pass = %d", rec.Code)
	}
	// The demand that cut the plane parks the pass at the next pick — that is
	// the design's yield, not a bug — and the assertion is only that the pass
	// is NOT retired failed: the hub cut its own dial, and the card takes no
	// strike for it. waitForWarm would wait for the 45 s park to age; a single
	// status read says everything this test needs.
	notStruckUntil := time.Now().Add(2 * time.Second)
	for time.Now().Before(notStruckUntil) {
		d := decodeEnv(t, warmGet(t, s, mux, "/api/theme/warm-status?ip="+ip))["data"].(map[string]any)
		if d["phase"] == "failed" {
			t.Fatalf("the pass retired failed after the hub cut its own dial: %#v", d)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

// A press AWAY from the card faces must stand the pass down, not start it.
// The old tabs.js posted every face; pressing Wi-Fi while a Themes queue
// streamed re-armed the very contention the operator was fleeing.
func TestAPressAwayFromTheCardFacesDoesNotStartCardWork(t *testing.T) {
	f := orbfake.New(orbfake.Theme{Name: "aurora", Bytes: 99, Serial: 1, OK: true, Active: true})
	s, ip, _, fake := fakeOrbHubWith(t, f)
	before := sdWalks(fake)
	warmPost(t, s, `{"ip":"`+ip+`","face":"wifi"}`)
	time.Sleep(300 * time.Millisecond)
	if got := sdWalks(fake); got != before {
		t.Fatalf("a Wi-Fi press walked the card %d times over %d: a face with no card work may not start a read-ahead", got, before)
	}
	rec := warmGet(t, s, http.NewServeMux(), "/api/theme/warm-status?ip="+ip) // mux-less: route absent; use below
	_ = rec
	status, err := http.NewRequest(http.MethodGet, "/api/theme/warm-status?ip="+ip, nil)
	if err != nil {
		t.Fatal(err)
	}
	status.Header.Set(tokenHeader, s.token)
	mux := http.NewServeMux()
	s.routes(mux)
	if d := decodeEnv(t, warmGet(t, s, mux, "/api/theme/warm-status?ip="+ip))["data"].(map[string]any); d["phase"] != "idle" {
		t.Fatalf("a Wi-Fi press left the pass at %v, want idle (parked, never started)", d["phase"])
	}
}

// The park and the resume: a demand stamps while a pass is between picks, the
// pass waits with its queues intact, and the moment the lane goes quiet it
// finishes the SAME queue — not a restarted one, not an abandoned one.
func TestThePassParksForADemandAndResumesItsOwnQueue(t *testing.T) {
	f := orbfake.New(
		orbfake.Theme{Name: "aurora", Bytes: 99, Serial: 1, OK: true, Active: true},
		orbfake.Theme{Name: "boring", Bytes: 99, Serial: 2, OK: true},
	)
	s, ip, _, fake := fakeOrbHubWith(t, f)
	mux := http.NewServeMux()
	s.routes(mux)
	if rec := postTheme(t, s, "/api/theme/list", themeBody(ip, "")); rec.Code != http.StatusOK {
		t.Fatalf("list = %d", rec.Code)
	}
	s.recordPageDemand(ip)                            // the Wi-Fi press's claim, without a kick behind it
	warmPost(t, s, `{"ip":"`+ip+`","face":"themes"}`) // the kick clears the stamp —
	s.recordPageDemand(ip)                            // and a demand raised AFTER it is the one that parks
	time.Sleep(120 * time.Millisecond)
	rec := warmGet(t, s, mux, "/api/theme/warm-status?ip="+ip)
	d := decodeEnv(t, rec)["data"].(map[string]any)
	if n, _ := d["note"].(string); !strings.Contains(n, "page") {
		t.Fatalf("the pass beside a live demand reported note %q, want the sentence naming the page", n)
	}
	// Age the demand out: the pass must then finish its plane queue on its own.
	s.previewMu.Lock()
	s.pagePvAt[ip] = warmStamp{at: time.Now().Add(-2 * demandStampLive), kind: 'r'}
	s.previewMu.Unlock()
	data := waitForWarm(t, s, mux, ip)
	if data["phase"] != "done" {
		t.Fatalf("after the demand aged out the pass sits at %v, want done", data["phase"])
	}
	if got := planeDials(fake); got != 2 {
		t.Fatalf("the resumed pass dialed %d planes, want the two-row queue it parked in front of: %#v", got, fake.Calls())
	}
}
