package main

import (
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// deviceHub points a fresh hub's device client at a fake that answers the two doors verifySeed
// reads: /api/v1/apps (the ledger) and /api/v1/sd (the theme card count). appsJSON and sdJSON are
// the exact bodies the fake serves, so a test says what the dial said and nothing else.
func deviceHub(t *testing.T, appsJSON, sdJSON string) (*Server, *[]string) {
	t.Helper()
	var paths []string
	h := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		paths = append(paths, r.URL.Path)
		w.Header().Set("Content-Type", "application/json")
		switch r.URL.Path {
		case "/api/v1/apps":
			w.Write([]byte(appsJSON))
		case "/api/v1/sd":
			w.Write([]byte(sdJSON))
		default:
			w.Write([]byte(`{}`))
		}
	})
	srv := httptest.NewServer(h)
	t.Cleanup(srv.Close)
	s := newHub()
	s.device = orbnet.NewClient(deviceTimeout)
	s.device.DevicePort = portOf(srv.URL)
	return s, &paths
}

const (
	goodApps = `{"mounted":true,"apps":[{"stem":"clock","ok":true}],` +
		`"installed":[{"slug":"clock","enabled":true}]}`
	goodSD = `{"mounted":true,"card_count":1,"active":"skeleton"}`
)

// A ledger that claims a clock row but has no card theme behind it is exactly the board the owner
// reported: Apps says installed, the glass shows no face. verifySeed must refuse that, not bless it.
func TestVerifySeedRefusesTheFacelessRow(t *testing.T) {
	s, _ := deviceHub(t, goodApps, `{"mounted":true,"card_count":0,"active":"Aviator"}`)
	err := s.verifySeedRun(context.Background(), "127.0.0.1", func(installStep) {})
	if err == nil {
		t.Fatal("a clock with no theme on the card verified, which is the false-green this gate exists to stop")
	}
}

// An installed-and-disabled row is symptom 2: the row stands, the enable bit is off, nothing draws.
func TestVerifySeedRefusesTheDisabledRow(t *testing.T) {
	s, _ := deviceHub(t,
		`{"mounted":true,"apps":[{"stem":"clock","ok":true}],`+
			`"installed":[{"slug":"clock","enabled":false}]}`, goodSD)
	err := s.verifySeedRun(context.Background(), "127.0.0.1", func(installStep) {})
	if err == nil {
		t.Fatal("a disabled clock row verified; it cannot be on the glass while switched off")
	}
}

// The whole point of the fix: a clean board with an enabled clock and a card theme verifies.
func TestVerifySeedAcceptsAGreenBoard(t *testing.T) {
	s, _ := deviceHub(t, goodApps, goodSD)
	seen := ""
	err := s.verifySeedRun(context.Background(), "127.0.0.1", func(st installStep) { seen = st.Action })
	if err != nil {
		t.Fatalf("a seeded board was refused: %v", err)
	}
	if seen != "verified" {
		t.Errorf("the run reported %q, not a verified step", seen)
	}
}

// liveIPforMAC is the answer "Open this orb" asks after the theme reboot moved the lease. The MAC
// is stable, so a fleet row that re-announced at a new address must win over what the dialog holds.
func TestWhereResolvesTheChipToItsLiveAddress(t *testing.T) {
	s := newHub()
	if _, err := s.fleet.Observe(orbfleet.Observation{
		MAC: "44bd8d608088", Instance: "orb-608088", IP: "192.168.2.32",
		FW: installMinFW, State: "connected", At: time.Now(),
	}); err != nil {
		t.Fatalf("observing the fleet row: %v", err)
	}
	if got := s.liveIPforMAC("44:BD:8D:60:80:88"); got != "192.168.2.32" {
		t.Errorf("a dashed MAC resolved to %q, not the dashed-free fleet row", got)
	}
	if got := s.liveIPforMAC("44bd8d608088"); got != "192.168.2.32" {
		t.Errorf("the bare MAC resolved to %q", got)
	}
	if got := s.liveIPforMAC("aabbccddee00"); got != "" {
		t.Errorf("an unknown chip resolved to %q; no row must answer empty", got)
	}
}

// hollowApps is the live Bronze's own /api/v1/apps body from 2026-10-29, verbatim in shape: a
// compiled menu, a clock row switched off with zero card files behind it. The old shelf gate
// counted that hollow row as "what the owner put there", the automatic arm skipped to done,
// and the page headline claimed the glass wore a clock. A hollow, disabled row is leftover,
// not curatorship: the gate must answer false so the ladder refills the promise.
const hollowApps = `{"mounted":true,"apps":[],"installed":[` +
	`{"slug":"menu","title":"Menu","enabled":true,"failed":false,"walkable":true,"on_glass":true,"compiled":true},` +
	`{"slug":"clock","title":"Clock","enabled":false,"failed":false,"walkable":true,"on_glass":false,"code_size":0,"serial":10129,"version":"v1.1.29"}]}`

func TestShelfGateRefusesToCountTheHollowRow(t *testing.T) {
	s, _ := deviceHub(t, hollowApps, `{"mounted":true,"card_count":0,"active":"Skeleton","themes":[]}`)
	held, err := s.shelfHoldsOwnersContent("127.0.0.1")
	if err != nil {
		t.Fatalf("asking the shelf: %v", err)
	}
	if held {
		t.Fatal("a disabled row over an empty card counted as the owner's shelf; that is the exact body the skip-lie was fed")
	}
}

// The rule the hollow-row test must not break: a clock row that is enabled and has its card
// copy present IS the owner's doing (or a finished seed's), and the automatic arm still
// stands down before it — earning its done by verify rather than by the row's bare existence.
func TestShelfGateHonoursARealShelf(t *testing.T) {
	s, _ := deviceHub(t, goodApps, goodSD)
	held, err := s.shelfHoldsOwnersContent("127.0.0.1")
	if err != nil {
		t.Fatalf("asking the shelf: %v", err)
	}
	if !held {
		t.Fatal("an enabled clock row with a readable card copy did not count as owner content; D38's shelf rule is dead")
	}
}

// installTheme must NOT reboot the board to arm the upload door. The retry shipped as a
// mitigation for a door that conflated a missing card with an uninitialized clock
// registry (bench, 2026-10-29, Bronze: join-time themes died at 503 over a mounted card),
// and firmware v1.1.33 pays that debt on the theme boot rung. The owner retired the
// mitigation the same day: every board runs 1.1.33 or newer, so a 503 here is a card
// genuinely absent, and a retry would buy one honest failure a second boot and a second
// 3.7 MB upload. Text gate, the convention this file's siblings use.
func TestThemeDoorCarriesNoRebootRetry(t *testing.T) {
	src, err := os.ReadFile("installflow.go")
	if err != nil {
		t.Fatalf("installflow.go: %v", err)
	}
	fn := string(src)
	at := strings.Index(fn, "func (s *Server) installTheme")
	if at < 0 {
		t.Fatal("installTheme is gone")
	}
	fn = fn[at:]
	if n := strings.Index(fn, "\n}\n"); n > 0 {
		fn = fn[:n]
	}
	if strings.Contains(fn, "s.device.Reboot(") {
		t.Error("installTheme reboots the board again: v1.1.33 fixed the door, and the fleet has no pre-1.1.33 board to mitigate for")
	}
	if n := strings.Count(fn, "doUp()"); n != 1 {
		t.Errorf("the theme upload runs %d times where the ladder promises one: a retry is back, %s", n, "and every join pays it twice")
	}
}

// waitFake answers /api/v1/status as a healthy orb and counts the probes, for the
// waits that must find a board rather than merely answer it.
func waitFake(t *testing.T) (*httptest.Server, *int) {
	t.Helper()
	var probes int
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/api/v1/status" {
			probes++
			w.Header().Set("Content-Type", "application/json")
			w.Write([]byte(`{"state":"normal","fw":"v1.1.16+91aaaaa","slot":"A","mac":"44:bd:8d:78:26:18"}`))
			return
		}
		w.WriteHeader(http.StatusNotFound)
	}))
	t.Cleanup(srv.Close)
	return srv, &probes
}

// TestWaitOrbBackFollowsTheMovedLease is the owner's afternoon in one function
// (report, 2026-10-29 PM): the ladder's reboots rejoin the board's WiFi, the
// renew hands it a new address, and every wait that dials only the address it
// started with dies at a ghost and calls the run a failure. The wait must ask the
// fleet where the CHIP answers now, find it at the new address, and report that
// address back for every dial that follows.
func TestWaitOrbBackFollowsTheMovedLease(t *testing.T) {
	srv, probes := waitFake(t)
	s := newHub()
	s.orb = orbnet.NewClient(deviceTimeout)
	s.orb.DevicePort = portOf(srv.URL)
	const mac = "44bd8d782618"
	// The chip left its first flash bound to the old address, and re-announced at
	// the fake's 127.0.0.1 — the moved lease exactly as the fleet records it.
	s.grants.learnAs("192.0.2.32", mac)
	if _, err := s.fleet.Observe(orbfleet.Observation{
		MAC: mac, Instance: "orb-782618", IP: "127.0.0.1", FW: "v1.1.16+91aaaaa",
		State: "normal", At: time.Now(),
	}); err != nil {
		t.Fatalf("seeding the row: %v", err)
	}
	live, ok := s.waitOrbBack(context.Background(), "192.0.2.32", 5*time.Second)
	if !ok {
		t.Fatal("the board answered at its live address and the wait died anyway")
	}
	if live != "127.0.0.1" {
		t.Errorf("the wait answered %s; the chip lives at 127.0.0.1 and that is the address every later dial must use", live)
	}
	if *probes != 1 {
		t.Errorf("the wait spent %d probes; following the lease answers on the first", *probes)
	}
	if s.expectedSelfReboot[macBare(mac)].IsZero() {
		t.Error("the wait stopped arming the self-reboot excuse; the sweep will pulse a board it rebooted")
	}
}

// TestWaitOrbBackRefusesOnBudgetAndHonorsCtx is the other edge: a board that never
// comes back must end the wait at the budget with the caller's address, and a
// departed browser must end it sooner, never later.
func TestWaitOrbBackRefusesOnBudgetAndHonorsCtx(t *testing.T) {
	s := newHub()
	s.orb = orbnet.NewClient(300 * time.Millisecond)
	s.grants.learnAs("192.0.2.32", "44bd8d782618")
	dead := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))
	deadPort := portOf(dead.URL)
	dead.Close() // closed now: every dial to it is refused, the truth of a board that stayed down
	s.orb.DevicePort = deadPort
	start := time.Now()
	live, ok := s.waitOrbBack(context.Background(), "192.0.2.32", time.Second)
	if ok {
		t.Fatal("a refused port answered the wait; that port is closed")
	}
	if live != "192.0.2.32" {
		t.Errorf("a wait that never relocated must hand back the caller's address, got %s", live)
	}
	if d := time.Since(start); d < time.Second {
		t.Errorf("the wait died at %s on a 1 s budget; the budget is the promise", d)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, ok := s.waitOrbBack(ctx, "192.0.2.32", time.Hour); ok {
		t.Error("an ended context must end the wait false at once, not at the hour")
	}
}

// TestThePatientWaitSpeaksWhileItWaits proves the beat fires: a board that stays down
// earns a spoken round every ~20 s (the fix for a frozen provisioning window), the beat
// carries the elapsed, the whole budget and the error the last probe took, and a board
// that answers at once earns NO beat — a wait that never waited must not print one.
func TestThePatientWaitSpeaksWhileItWaits(t *testing.T) {
	dead := httptest.NewServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {}))
	deadPort := portOf(dead.URL)
	dead.Close() // closed now: every dial to it is refused, the truth of a board still down
	s := newHub()
	s.orb = orbnet.NewClient(300 * time.Millisecond)
	s.orb.DevicePort = deadPort
	s.grants.learnAs("192.0.2.32", "44bd8d782618")

	type beatRec struct {
		elapsed, budget time.Duration
		err             error
	}
	var beats []beatRec
	budget := 45 * time.Second // enough for the 20 s and 40 s beats, not the third
	s.waitOrbBack(context.Background(), "192.0.2.32", budget, func(el, b time.Duration, err error) {
		beats = append(beats, beatRec{el, b, err})
	})

	// A 45 s silence at one beat per 20 s is at least two. Zero beats is the frozen
	// screen the operator reported; this is the assertion that stops it coming back.
	if len(beats) < 2 {
		t.Fatalf("a %v wait spoke %d times; the window froze on one sentence again", budget, len(beats))
	}
	for i, b := range beats {
		if want := time.Duration(i+1) * waitBeatEvery; b.elapsed < want {
			t.Errorf("beat %d reported elapsed %v, under its %v mark", i, b.elapsed, want)
		}
		if b.budget != budget {
			t.Errorf("beat %d reported budget %v, want the whole %v", i, b.budget, budget)
		}
		if b.err == nil {
			t.Errorf("beat %d carried no error; the wait must say why it is still waiting", i)
		}
	}

	// An answering board earns no beat at all.
	srv, _ := waitFake(t)
	s.orb = orbnet.NewClient(deviceTimeout)
	s.orb.DevicePort = portOf(srv.URL)
	n := len(beats)
	if _, ok := s.waitOrbBack(context.Background(), "127.0.0.1", budget, func(time.Duration, time.Duration, error) {
		beats = append(beats, beatRec{})
	}); !ok {
		t.Fatal("an answering board was refused")
	}
	if len(beats) != n {
		t.Errorf("an instant answer beat %d times; only a real wait may speak", len(beats)-n)
	}
}

// TestWhereAnswersByAddress is the door the bounced page needed: a join over the
// wired line can end holding an address and no MAC, and the page's only handle is
// then the fleet itself. Asked by ip, /api/orb/where answers the chip that lives at
// that address — the same reverse lookup cleanSlate consults — so the Open-orb
// press arrives knowing the MAC that keeps it findable across the next reboot.
func TestWhereAnswersByAddress(t *testing.T) {
	s := newHub()
	if _, err := s.fleet.Observe(orbfleet.Observation{
		MAC: "44bd8d782618", Instance: "orb-782618", IP: "192.0.2.44", FW: "v1.1.0+aaaaaaa",
		State: "normal", At: time.Now(),
	}); err != nil {
		t.Fatalf("seeding the row: %v", err)
	}
	rec := httptest.NewRecorder()
	s.h_orbWhere(rec, httptest.NewRequest(http.MethodGet, "/api/orb/where?ip=192.0.2.44", nil))
	body := rec.Body.String()
	if !strings.Contains(body, `"ok":true`) || !strings.Contains(body, "44bd8d782618") {
		t.Errorf("asked by address, where answered: %s", body)
	}
	// A stale or unknown address refuses rather than pointing at the wrong board.
	rec = httptest.NewRecorder()
	s.h_orbWhere(rec, httptest.NewRequest(http.MethodGet, "/api/orb/where?ip=192.0.2.99", nil))
	if strings.Contains(rec.Body.String(), `"ok":true`) {
		t.Error("where answered an address no live row claims")
	}
}

// TestTheLadderIsPatientAcrossItsOwnReboots is the text gate for the whole PM batch
// of 2026-10-29: the reboot the ladder causes cuts the board's WiFi for longer than
// one dial, and every wait that read that silence as death is now a wait that rides
// it out on the CHIP. Each site carries its own proof; losing any one of them puts
// the owner back in the afternoon they just lived through.
func TestTheLadderIsPatientAcrossItsOwnReboots(t *testing.T) {
	src := readFile(t, "installflow.go")
	themeSrc := readFile(t, "theme.go")
	app := readFile(t, "web/app.js")
	tabs := readFile(t, "web/tabs.js")
	checks := []struct{ where, src, want string }{
		{"the ladder's head", src, "waiting through its boot before the run"},
		{"the reboot budget", src, "installRebootWait = 3 * time.Minute"},
		{"the verify gate", src, "if live, ok := s.waitOrbBack(ctx, ip, installRebootWait); ok {"},
		{"the slate's erase", src, "_, e = s.device.SDRemove(ip, tree, true)"},
		{"the store wait's follow", themeSrc, `if live := s.dialLive(ip); live != "" && live != ip {`},
		{"the wait that follows the chip", src, "func (s *Server) waitOrbBack("},
		{"the cycle behind every caused reboot", src, "func (s *Server) waitOrbCycle("},
		{"the theme step waits on the cycle", src, "s.waitOrbCycle(context.Background(), ip, installRebootWait, beats...)"},
	}
	for _, c := range checks {
		if !strings.Contains(c.src, c.want) {
			t.Errorf("%s lost its patience: %q is gone", c.where, c.want)
		}
	}
	// The page opens the board it holds an address for, learns the MAC off the
	// board's own reply, follows the lease from its wake retry, and gives the
	// ledger re-read one more chance before printing a failure.
	for _, want := range []string{
		`v === "orb" && (current.mac || current.ip)`,
		"if (!current.mac && st.mac) {",
		`"/api/orb/where?" + q`,
	} {
		if !strings.Contains(app, want) {
			t.Errorf("app.js lost %q: the page is back to bouncing operators off boards it can still reach", want)
		}
	}
	if !strings.Contains(tabs, "trying once more as it comes back") {
		t.Error("the ledger re-read lost its second try: a mid-boot silence is printing failures over installed apps again")
	}
	// The same report's second half: the join-time seed froze one sentence on a dialog
	// whose every button was disabled, because its steps went to the Library view's
	// hidden <pre> and nothing bounded the wait on the stream. Each string below is one
	// half of the fix; losing any re-freezes the window the operator just reported.
	for _, want := range []string{
		"onStep(j, t)",           // every hub step reaches the join dialog's relay
		"watchLog(t)",            // and lands on the visible console trail
		"released = true",        // a dead stream releases the window, fetch intact
		"4 * 60 * 1000",          // the stall budget that ends it
		`"Seeding its baseline"`, // the headline names the phase, not the old one
	} {
		if !strings.Contains(app, want) {
			t.Errorf("app.js lost %q: the provisioning window is back to freezing on one sentence", want)
		}
	}
	// The hub now speaks through every patient wait, so the freeze cannot be pushed back
	// onto the server: the beat type, its interval, and the beat helper must all stand.
	for _, want := range []string{
		"const waitBeatEvery = 20 * time.Second",
		"func beatFor(",
	} {
		if !strings.Contains(src, want) {
			t.Errorf("installflow.go lost %q: the waits are silent again", want)
		}
	}
}

// TestNoElementLookupsLoseTheirHash is the sweep for the one-character class of bug that
// re-created the Open-orb bounce after it had been fixed (owner report, 2026-10-29 PM):
// $("ob-name") asks querySelector for a TAG named ob-name, finds none, and the null deref
// kills the press — mid-handler, after the dialog closed — dumping the operator on the
// fleet list. The page's two lookup helpers take CSS selectors, so every string they are
// handed must start with an id (#), a class (.), or be an attribute/element selector the
// call site can defend. This gate scans the whole shipped script, because an editor shows
// none of these and the suite otherwise never presses a button.
func TestNoElementLookupsLoseTheirHash(t *testing.T) {
	for _, file := range []string{"web/app.js", "web/tabs.js"} {
		src := readFile(t, file)
		for _, m := range idlessLookups(src) {
			t.Errorf("%s: %s lacks a leading # or . — querySelector reads it as a tag name", file, m)
		}
	}
	// And the press itself must still navigate: the handler opens the board by
	// address after reading the name field the right way.
	app := readFile(t, "web/app.js")
	press := blockOf(t, app, `$("#ob-manage").onclick`)
	for _, want := range []string{`$("#ob-name")`, "openOrbByIP(ip, provMac"} {
		if !strings.Contains(press, want) {
			t.Errorf("the Open-orb press lost %q: it is back to dying before it navigates", want)
		}
	}
}

// idlessLookups returns every $("...")/$$("...") argument in src whose selector begins
// with a bare letter — the tag-name reads that throw on the id lookups this page makes.
// Element selectors written on purpose (querySelectorAll("div")) would match too, and the
// page has none; a deliberate one joins an allowlist here, not a gate exemption there.
func idlessLookups(src string) []string {
	var out []string
	for _, call := range []string{`$(`, `$$(`} {
		rest := src
		for {
			i := strings.Index(rest, call)
			if i < 0 {
				break
			}
			rest = rest[i+len(call):]
			if !strings.HasPrefix(rest, `"`) {
				continue
			}
			end := strings.IndexByte(rest[1:], '"')
			if end < 0 {
				continue
			}
			sel := rest[1 : 1+end]
			if sel == "" {
				continue
			}
			c := sel[0]
			if (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') {
				out = append(out, call+`"`+sel+`"`)
			}
		}
	}
	return out
}

// TestWaitOrbCycleDemandsTheSilenceFirst pins the corpse probe: the firmware
// answers the reboot door — and the beat of traffic behind it — while it is dying,
// so the post-reboot wait must see the silence before it believes any answer
// (bench, Bronze, 2026-10-29 PM: the theme door's retry dialled a board mid-shutdown
// this way and ate a second "no card" that cost the seed). The fake is alive for two
// seconds, dead for four, alive after that: a wait that trusted the first answer
// would return before the death it is supposed to wait through.
func TestWaitOrbCycleDemandsTheSilenceFirst(t *testing.T) {
	start := time.Now()
	var probes int
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/v1/status" {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		probes++
		if d := time.Since(start); d < 2*time.Second || d > 6*time.Second {
			w.Header().Set("Content-Type", "application/json")
			w.Write([]byte(`{"state":"normal","fw":"v1.1.16+91aaaaa","slot":"A"}`))
			return
		}
		w.WriteHeader(http.StatusServiceUnavailable)
	}))
	t.Cleanup(srv.Close)
	s := newHub()
	s.orb = orbnet.NewClient(deviceTimeout)
	s.orb.DevicePort = portOf(srv.URL)
	live, ok := s.waitOrbCycle(context.Background(), "127.0.0.1", 30*time.Second)
	if !ok || live != "127.0.0.1" {
		t.Fatalf("the cycle gave up on a board that rebooted and came back (probes %d)", probes)
	}
	if probes < 4 {
		t.Errorf("the cycle trusted an answer after %d probes; the silence must be observed", probes)
	}
	if d := time.Since(start); d < 2*time.Second {
		t.Errorf("the cycle returned at %s — before the board even died", d)
	}
}
