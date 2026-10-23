package main

import (
	"context"
	"errors"
	"log"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
)

// swapLogOutput points the standard logger at w and returns what was there, so a test can
// read the lines production writes rather than trusting a comment that it writes any.
func swapLogOutput(w *strings.Builder) func() {
	old := log.Writer()
	prevFlags := log.Flags()
	log.SetOutput(w)
	log.SetFlags(0)
	return func() { log.SetOutput(old); log.SetFlags(prevFlags) }
}

// These tests pin the behaviour that stopped an orb being force-rebooted mid-install. They
// sit together rather than in each source file's suite because they are one story: a hub
// that asks an expensive question on a clock, never credits the free evidence that answers
// it, backs off a failure at a fixed rate, and cannot say why it reset a board. Each half
// alone looked reasonable on the bench.

// The storm, pinned. One missed probe is a board that was busy; ten is a link that is
// broken, and asking at a fixed minute each time is what made a flaky USBIP path reset a
// healthy orb once a minute forever. The window must grow, and stop growing somewhere, so
// a wedged lead becomes askable again without a hub restart.
func TestAProbeThatFailsBacksOffFurtherEachTime(t *testing.T) {
	failing := func() (esprom.OrbInfo, error) {
		return esprom.OrbInfo{}, errors.New("esprom: no sync")
	}
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: failing}}
	s := launchHub(t, goldBenchPorts(), p)

	s.identifyCables(context.Background())
	if n := p.count(); n != 1 {
		t.Fatalf("a fresh bench was asked %d times, want 1", n)
	}
	s.identifyCables(context.Background())
	if n := p.count(); n != 1 {
		t.Fatalf("a port inside its backoff was asked again (%d asks)", n)
	}

	// Second miss. The window is now double the first, so ageing past one window is not
	// yet permission to ask.
	s.backdateFailure(reflashPort, failedRetry+time.Second)
	s.identifyCables(context.Background())
	if n := p.count(); n != 2 {
		t.Fatalf("second miss asked %d times, want 2", n)
	}
	s.backdateFailure(reflashPort, failedRetry+time.Second)
	s.identifyCables(context.Background())
	if n := p.count(); n != 2 {
		t.Fatalf("the backoff stayed flat at one window after a second consecutive miss (%d asks)", n)
	}

	// Third miss doubles again: two windows now buy the question.
	s.backdateFailure(reflashPort, 2*failedRetry+time.Second)
	s.identifyCables(context.Background())
	if n := p.count(); n != 3 {
		t.Fatalf("third miss asked %d times, want 3", n)
	}

	// And it stops doubling at the cap, so a lead that returns after a long outage is
	// noticed rather than frozen out.
	s.identities.mu.Lock()
	e := s.identities.byPort[reflashPort]
	e.fails = 30
	s.identities.byPort[reflashPort] = e
	s.identities.mu.Unlock()
	s.backdateFailure(reflashPort, failedRetryMax+time.Second)
	s.identifyCables(context.Background())
	if n := p.count(); n != 4 {
		t.Errorf("a port at the %v cap stayed wedged forever (%d asks, wanted the 4th)",
			failedRetryMax, n)
	}
}

// The storm the flat minute hid. A PAID NEGATIVE is exempt from the cache TTL, so it is
// never reaped, and it is due again every contradictedReprobe forever. On a lead that will
// not answer a probe, a failure keeps that verdict in place rather than replacing it — so
// before the attempt clock, the entry stayed due, the answer never changed, and the board
// was reset once a sweep, five seconds apart. This is the shape of "a healthy orb rebooting
// itself all afternoon".
func TestAPaidNegativeOnADeafLeadIsNotResetEverySweep(t *testing.T) {
	failing := func() (esprom.OrbInfo, error) {
		return esprom.OrbInfo{}, errors.New("esprom: usbip link disturbed")
	}
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: failing}}
	s := launchHub(t, goldBenchPorts(), p)
	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", IsOrb: false, Project: "esp-idf"}, provenProbe)
	// Past the window a negative is trusted for, but still believed: not reaped, and due.
	backdateIdentity(s, reflashPort, contradictedReprobe+time.Minute)

	s.identifyCables(context.Background())
	if n := p.count(); n != 1 {
		t.Fatalf("asked %d times, want 1", n)
	}
	// The verdict survived, because a probe that reached nothing overturns nothing. That
	// is what leaves the port due on the next pass, and what the attempt clock is for.
	info, _, proven, present := s.identities.cachedVerdict(reflashPort)
	if !present || proven != provenProbe || info.IsOrb {
		t.Error("a failed probe replaced the paid negative it was supposed to leave alone")
	}
	// Four more passes back to back, as the five-second watcher runs them.
	for i := 0; i < 4; i++ {
		s.identifyCables(context.Background())
	}
	if n := p.count(); n != 1 {
		t.Errorf("a believed negative on a deaf lead was interrogated %d times: %v", n, p.ports())
	}
	// Backoff, not a freeze: past the window the hub asks again. Age ONLY the attempt
	// clock here. backdateFailure would also move `at`, and a paid negative pushed back
	// inside contradictedReprobe stops being due for a reason unrelated to this assert —
	// the test would pass by accident and then fail to notice the gate going wrong.
	s.identities.mu.Lock()
	e := s.identities.byPort[reflashPort]
	e.lastTry = time.Now().Add(-(failedRetry + time.Second))
	s.identities.byPort[reflashPort] = e
	s.identities.mu.Unlock()
	s.identifyCables(context.Background())
	if n := p.count(); n != 2 {
		t.Error("the attempt clock froze the port permanently rather than backing off")
	}
}

// They sit together rather than in each source file's suite because they are one story: a
// hub that asks an expensive question on a clock, never credits the free evidence that
// answers it, backs off a failure at a fixed rate, and cannot say why it reset a board.
// Each half alone looked reasonable on the bench.

// The reported fault, in one test: an orb alive on the LAN, its cable attached, aged out of
// the identity cache and reset on a timer. launchprobe.go has always said a positive answer
// is corroborated continuously by the free facts and so is not re-paid. Nothing refreshed
// the entry clock, so that sentence described code that did not exist. The liveness dial
// names the same eFuse MAC the probe pays a reboot to read.
func TestALanAnswerRenewsAVerdictWithoutAReboot(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
	s := launchHub(t, goldBenchPorts(), p)
	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", IsOrb: true, Project: "gorb-os", Version: "v1.0.0"}, provenProbe)

	// Stand at the far edge of the TTL, where the sweep would otherwise pay.
	backdateIdentity(s, reflashPort, identityTTL+time.Minute)
	if ask, _ := s.cableNeedsProbe(goldSighting()[0]); !ask {
		t.Fatal("setup: an expired answer must read as due, or this proves nothing")
	}

	port, saved := s.identities.corroborate(macHall, time.Now())
	if port != reflashPort {
		t.Fatalf("corroborate refreshed %q, want %q", port, reflashPort)
	}
	if !saved {
		t.Error("the refresh did not report rescuing a verdict the next sweep would have paid for")
	}
	if ask, why := s.cableNeedsProbe(goldSighting()[0]); ask {
		t.Errorf("a board that answered its own door as an orb was still scheduled for a reboot: %s", why)
	}
	if p.count() != 0 {
		t.Errorf("the sweep reset %v; a live orb answers for itself over HTTP", p.ports())
	}
}

// The renewal is not a free pass for whoever is dialling. A board answering as MAC X says
// nothing about a lead the host attributes to MAC Y, and the case the cache exists for is
// the re-plug: orb A identified on ACM0, A gone, B on the same tty. Corroboration keyed on
// the tty name alone would keep B's absent verdict fresh on A's evidence.
func TestADialForOneChipRenewsNoOtherCablesVerdict(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
	s := launchHub(t, goldBenchPorts(), p)
	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", IsOrb: true, Project: "gorb-os"}, provenProbe)
	backdateIdentity(s, reflashPort, identityTTL+time.Minute)

	if port, _ := s.identities.corroborate("aabbccddeeff", time.Now()); port != "" {
		t.Fatalf("a dial naming another chip refreshed %s", port)
	}
	if ask, _ := s.cableNeedsProbe(goldSighting()[0]); !ask {
		t.Error("a stranger's LAN answer bought this cable immunity it never earned")
	}
}

// A negative is never corroborated. A board claiming to be an orb over unsigned mDNS and
// HTTP is exactly the evidence a paid NOT-an-orb verdict exists to outweigh.
// A negative is never corroborated. A board claiming to be an orb over unsigned mDNS and
// HTTP is exactly the evidence a paid NOT-an-orb verdict exists to outweigh.
func TestAFreeDialNeverRenewsANotOrbVerdict(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	s := launchHub(t, goldBenchPorts(), p)
	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", IsOrb: false, Project: "esp-idf"}, provenProbe)
	backdateIdentity(s, reflashPort, time.Hour)

	if port, _ := s.identities.corroborate(macHall, time.Now()); port != "" {
		t.Fatalf("a LAN dial renewed a paid NOT-an-orb verdict on %s", port)
	}
	if ask, _ := s.cableNeedsProbe(goldSighting()[0]); !ask {
		t.Error("the negative went unasked, which is how a reflash-in-progress becomes permanent")
	}
}

// The install half of the story: a theme crossing the room on HTTP claims the lead that
// same board sits on, so the identity sweep declines it until the transfer ends. Before
// this, the sweep and the install shared no vocabulary at all — one keyed on port, the
// other on address — and the collision deleted the upload's .part file with nothing written
// down about it.
func TestAThemeCallHoldsThePortTheProbeWouldUse(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
	s := launchHub(t, goldSighting(), p)
	// Gold is on the lead, answers at this address, and the hub learned the pairing the
	// way onboarding leaves it: a grant bound to the chip.
	s.grants.learnAs(themeTestIP, macHall)

	release, err := s.holdDevicePort(context.Background(), themeTestIP, time.Minute)
	if err != nil {
		t.Fatalf("the theme call could not hold its own board's lead: %v", err)
	}
	if ask, why := s.cableNeedsProbe(goldSighting()[0]); ask {
		t.Errorf("the sweep reset a lead mid-theme-transfer: %s", why)
	}
	if n := s.identifyCables(context.Background()); n != 0 {
		t.Errorf("the sweep asked %d questions through an install", n)
	}
	// The claim is visible, so a stuck transfer is diagnosable rather than mysterious.
	if kind, busy := s.ops.busy(reflashPort); !busy || kind != opTheme {
		t.Errorf("the port reported kind %q busy=%v; /api/ops must name a theme call", kind, busy)
	}
	release()

	// Released, the lead is askable again — but not yet. The store the hub just sent is
	// applied by a reboot the device performs after it acks, so the silence that follows a
	// release is the install landing, not a board defecting, and the excuse covers it. The
	// test retires the excuse by hand because it has no reboot to wait for: on a real bench
	// the next successful liveness dial clears it (main.go), which is the same line.
	s.clearSelfReboot(macHall)
	if ask, _ := s.cableNeedsProbe(goldSighting()[0]); !ask {
		t.Fatal("the probe stayed deferred after the install finished")
	}
	if n := s.identifyCables(context.Background()); n != 1 {
		t.Errorf("after the release the sweep asked %d questions, want the owed one", n)
	}
}

// The whole reason the theme stopped tearing, stated as a rule. A store is acked before the
// device performs it and reboots the board to draw it, so the lead goes quiet for as much as
// a minute after a call the hub considers finished. Every gate that reads that silence as
// evidence and pays a reset is cutting the install in half, and the operator sees a store
// that staged and never committed. Three separate rules can fire on that one silence — the
// flag set a beat too early, the flag arriving while the board is dark, and the departure
// reaper deleting the entry when the port re-enumerates — so all three are pinned here,
// against one install.
func TestAnInstallRebootIsNotReadAsADefection(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
	s := launchHub(t, goldSighting(), p)
	// Colon on the grant, bare on the sighting: exactly what production holds, because
	// CanonicalMAC strips separators on the way into the cable cache and the API does not on
	// the way into a grant. A resolver that normalizes only one side finds no port and claims
	// nothing, which is how this guard did nothing on a real bench while passing a test that
	// seeded the same constant on both sides.
	s.grants.learnAs(themeTestIP, "44:bd:8d:78:26:18")

	release, err := s.holdDevicePort(context.Background(), themeTestIP, time.Minute)
	if err != nil {
		t.Fatalf("hold: %v", err)
	}
	release()

	// The board is now dark because it is installing. Nobody dialled it; the hub did.
	if ask, why := s.cableNeedsProbe(goldSighting()[0]); ask {
		t.Errorf("the gate reset a board that is rebooting to apply its own install: %s", why)
	}
	// The flag that says "it went quiet" must not buy the reset either.
	s.suspicious = map[string]bool{macHall: true}
	s.owed = map[string]bool{macHall: true}
	if ask, why := s.cableNeedsProbe(goldSighting()[0]); ask {
		t.Errorf("the silence escalation reset a board mid-install: %s", why)
	}
	s.owed = nil
	// The device restarts USB-Serial-JTAG and the port re-enumerates, so the reaper takes
	// the entry with it and the gate reads a board that was never asked.
	s.identities.forget(reflashPort)
	if ask, why := s.cableNeedsProbe(goldSighting()[0]); ask {
		t.Errorf("a re-enumeration during an install cost a second reset: %s", why)
	}

	// And the excuse does not become a permanent shield. Once the board has been heard from,
	// an unexplained silence flags again — that is wiredSilenceDetection's whole job, and a
	// board reflashed abroad must still be caught.
	s.clearSelfReboot(macHall)
	s.noteWiredSilence(themeTestIP)
	s.suspMu.Lock()
	flagged := s.suspicious[macHall]
	s.suspMu.Unlock()
	if !flagged {
		t.Error("the install excuse never retired, so a board reflashed abroad would go unasked forever")
	}
}

// A LAN-only orb has no lead to hold, and no probe can ever reach it. The claim must be a
// no-op rather than a refusal, or the fix would break the common case to save the rare one.
func TestAThemeCallToAnOrbWithNoCableClaimsNothing(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
	s := launchHub(t, nil, p)
	s.grants.learnAs(themeTestIP, macHall)

	release, err := s.holdDevicePort(context.Background(), themeTestIP, time.Minute)
	if err != nil {
		t.Fatalf("an orb with no cable was refused its own theme call: %v", err)
	}
	release()
	if _, busy := s.ops.busy(reflashPort); busy {
		t.Error("a call to a boardless address claimed a port it cannot see")
	}
}

// An unbound grant — an address somebody typed, with no chip learned for it — resolves to
// no lead. Proceeding is correct; refusing would break the Manage card's manual address,
// and there is no cable to protect either way.
func TestAnUnboundAddressClaimsNoPort(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
	s := launchHub(t, goldSighting(), p)
	s.grants.learn(themeTestIP)

	release, err := s.holdDevicePort(context.Background(), themeTestIP, time.Minute)
	if err != nil {
		t.Fatalf("an unbound address was refused: %v", err)
	}
	release()
	if _, busy := s.ops.busy(reflashPort); busy {
		t.Error("an address with no chip behind it claimed a cable")
	}
}

const themeTestIP = "192.0.2.31"

// The fallback, and its boundary. A Windows hub cannot read a USB serial, so `who` is
// empty on every entry it holds and corroboration would never fire — the reboot timer
// would keep running on a supported release platform. The chip's own printed address is
// the only handle there. It must still name THIS entry's chip: a stranger answering the
// door gets no immunity for a board it is not.
func TestAHostThatCannotReadASerialStillCorroborates(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
	s := launchHub(t, goldBenchPorts(), p)
	s.identities.usbMAC = func(string) string { return "" }
	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", IsOrb: true, Project: "gorb-os"}, provenProbe)
	backdateIdentity(s, reflashPort, identityTTL+time.Minute)

	if ask, _ := s.cableNeedsProbe(goldSighting()[0]); !ask {
		t.Fatal("setup: no serial read means no live match, so this must still be due")
	}
	port, saved := s.identities.corroborate(macHall, time.Now())
	if port != reflashPort || !saved {
		t.Fatalf("corroborate refreshed (%q,%v) on a host that cannot read the descriptor", port, saved)
	}
	if ask, why := s.cableNeedsProbe(goldSighting()[0]); ask {
		t.Errorf("the renewal did not reach the gate: %s", why)
	}

	// And it stays keyed to the recorded chip. orb-B's banner says 44:bd:8d:78:26:18; an
	// orb answering as something else must not buy this lead a pass.
	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", IsOrb: true, Project: "gorb-os"}, provenProbe)
	backdateIdentity(s, reflashPort, identityTTL+time.Minute)
	if port, _ := s.identities.corroborate("aabbccddeeff", time.Now()); port != "" {
		t.Errorf("a dial naming another chip refreshed %s on a host with no descriptor read", port)
	}
}

// them can prove the hub calls it, and a method nobody dials is a reboot timer still
// ticking. Same reason launchprobe_test.go asserts on the source of the launch flag: this
// The wiring a suite cannot execute. Every test above calls corroborate directly; none of
// them can prove the hub calls it, and a method nobody dials is a reboot timer still
// ticking. Same reason launchprobe_test.go asserts on the source of the launch flag: this
// is a call site, and a call site can only be pinned by reading the file that holds it.
func TestTheLivenessDialFeedsTheIdentityCache(t *testing.T) {
	src := readFile(t, "main.go")
	i := strings.Index(src, "s.live.probe = func(ip string) bool {")
	if i < 0 {
		t.Fatal("the liveness probe lost the wiring the Forget gate depends on")
	}
	body := src[i:]
	if j := strings.Index(body, "\n\ts.foldProbeStatus"); j > 0 {
		body = body[:j]
	}
	if !strings.Contains(body, "s.identities.corroborate(st.MAC") {
		t.Error("the free status read no longer renews the identity cache, so a live orb ages back into a reboot")
	}
	// The renewal must sit on the success path only. Corroborating on a failed dial would
	// keep a board believed because it stopped answering, which is the reverse of the rule.
	if strings.Contains(src[i:], "corroborate") && strings.Contains(body, "noteWiredSilence") {
		silence := strings.Index(body, "noteWiredSilence")
		renew := strings.Index(body, "corroborate")
		if renew < silence {
			t.Error("the identity renewal moved ahead of the silence branch, so a dead dial would renew the board")
		}
	}
}

// Every paid question must be attributable afterwards. The operator sees the glass go dark,
// and the hub has until now written down nothing about why it did that to a working board.
func TestEveryPaidProbeNamesItsReason(t *testing.T) {
	cases := []struct {
		name  string
		setup func(*Server)
		want  string
	}{
		{"nothing on file", func(*Server) {}, "no answer on file"},
		{"aged positive", func(s *Server) {
			s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
				MAC: "44:bd:8d:78:26:18", IsOrb: true, Project: "gorb-os"}, provenProbe)
			backdateIdentity(s, reflashPort, identityTTL+time.Minute)
		}, "aged out"},
		{"only a cable proof", func(s *Server) {
			s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
				MAC: "44:bd:8d:78:26:18", IsOrb: true}, provenCable)
		}, "PING"},
		{"a silence worth testing", func(s *Server) {
			s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
				MAC: "44:bd:8d:78:26:18", IsOrb: true, Project: "gorb-os"}, provenProbe)
			s.owed = map[string]bool{macHall: true}
		}, "stopped answering"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
			s := launchHub(t, goldSighting(), p)
			tc.setup(s)
			ask, why := s.cableNeedsProbe(goldSighting()[0])
			if !ask {
				t.Fatal("the gate declined, so there was no reason to check")
			}
			if !strings.Contains(why, tc.want) {
				t.Errorf("reason = %q, want it to mention %q", why, tc.want)
			}
		})
	}

	// A declined lead carries no reason, so a log line cannot be mistaken for one.
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
	s := launchHub(t, goldSighting(), p)
	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", IsOrb: true, Project: "gorb-os"}, provenProbe)
	if ask, why := s.cableNeedsProbe(goldSighting()[0]); ask || why != "" {
		t.Errorf("a fresh verdict asked anyway (ask=%v why=%q)", ask, why)
	}
}

// The reason has to reach the log, not just the caller's hand. The sweep runs with nobody
// watching the card, and the report that started this was an operator trying to work out
// after the fact why a board kept going dark.
func TestAPaidProbeIsLoggedWithItsReason(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
	s := launchHub(t, goldSighting(), p)

	var logged strings.Builder
	restore := swapLogOutput(&logged)
	defer restore()

	if n := s.identifyCables(context.Background()); n != 1 {
		t.Fatalf("the sweep asked %d questions, want 1", n)
	}
	out := logged.String()
	for _, want := range []string{"probing", reflashPort, "no answer on file", "answered an orb"} {
		if !strings.Contains(out, want) {
			t.Errorf("the probe log lost %q:\n%s", want, out)
		}
	}
}

// A probe that never reached the chip is as readable as one that did, or the log says the
// hub did something to a board and never says it failed.
func TestAFailedProbeIsLoggedAsFailed(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: func() (esprom.OrbInfo, error) {
		return esprom.OrbInfo{}, errors.New("no sync")
	}}}
	s := launchHub(t, goldSighting(), p)

	var logged strings.Builder
	restore := swapLogOutput(&logged)
	defer restore()

	s.identifyCables(context.Background())
	out := logged.String()
	if !strings.Contains(out, "reached no chip") || !strings.Contains(out, "no sync") {
		t.Errorf("a probe that reached nothing was not logged as one:\n%s", out)
	}
}
