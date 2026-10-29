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

// installTheme must keep its reboot-retry against the door's conflated refusal, and it
// must keep it CONDITIONED on the mounted re-read — a card that is truly gone has to keep
// the honest error, not spin the board. Text gate, the convention this file's siblings use:
// the bench death (2026-10-29, Bronze: join-time themes died at 503 over a mounted card,
// and three months of empty themes folders traced back to one NULL registry entry).
func TestThemeDoorRetryStaysArmedAndConditional(t *testing.T) {
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
	for _, want := range []string{"no card in the socket", "Reboot(", "installWaitBack(", "Mounted"} {
		if !strings.Contains(fn, want) {
			t.Errorf("installTheme lost %q: the join-time theme step is back to eating the door's lie", want)
		}
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
		{"the theme step waits on the cycle", src, "s.waitOrbCycle(context.Background(), ip, installRebootWait)"},
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
