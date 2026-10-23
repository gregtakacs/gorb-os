package main

// The Forget gate, tested as the rule it is. Every case below starts from the same fleet
// row — one board, remembered at an address, gone quiet long enough that the hub has
// genuinely stopped hearing it — and changes exactly one fact. A row that stops being
// forgettable when one of three things turns true is the whole point of the rule, so a
// test that changed two of them at once would pass with two of the three checks missing.

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// gateClock returns a hub whose fleet runs on a clock the test drives, plus the instant
// to ask the gate at. The store's injected Now and the now passed to forgetGate are the
// same value, so no test can age a row just by running slow.
func gateClock(at time.Time) (*Server, func() time.Time) {
	s := newHub()
	s.fleet = orbfleet.New(orbfleet.Config{Now: func() time.Time { return at }})
	return s, func() time.Time { return at }
}

// quietRow is a board last heard from safely outside the announce grace.
func quietRow(s *Server, mac, ip string, now time.Time) {
	s.fleet.Observe(obsAt(mac, ip, now.Add(-3*time.Minute)))
}

// stubCables wires the cable sweep the gate consults, with no host to enumerate.
func stubCables(s *Server, seen ...cableSighting) {
	s.cables.list = func() ([]cableSighting, error) { return seen, nil }
	s.cables.ttl = time.Hour
}

// The row the gate refuses is the row the dialog is for: annotate carries both
// halves of the answer — plain Forget is off, live endings are on — and the lead
// the endings drive. A plain false without the dialog verdict is the dead button
// this feature exists to remove.
func TestARefusedLiveRowCarriesTheDialogVerdict(t *testing.T) {
	now := time.Now()
	s, _ := gateClock(now)
	s.fleet.Observe(obsAt(macHall, "192.0.2.31", now.Add(-10*time.Second)))
	stubCables(s, cableSighting{port: "/dev/ttyUSB0", isOrb: true, mac: macHall})
	rows := []orbRow{{MAC: macHall}}
	s.annotateForget(rows, now)
	if rows[0].Forgettable {
		t.Fatal("an announcing board on a lead was marked plain-forgettable")
	}
	if !rows[0].LiveForget {
		t.Error("the refused live row carries no dialog verdict — its Forget button is decoration")
	}
	if rows[0].LivePort != "/dev/ttyUSB0" {
		t.Errorf("LivePort = %q, want the lead the endings drive", rows[0].LivePort)
	}

	// A quiet row keeps the plain path and gets no dialog flag. The dead verdict
	// is what makes the gate honest on the render path (no dial, no guessing).
	s2, _ := gateClock(now)
	quietRow(s2, macHall, "192.0.2.31", now)
	s2.live.probe = func(string) bool { return false }
	s2.live.check("192.0.2.31")
	rows2 := []orbRow{{MAC: macHall}}
	s2.annotateForget(rows2, now)
	if !rows2[0].Forgettable || rows2[0].LiveForget {
		t.Error("a quiet row lost its plain Forget or grew a dialog it does not need")
	}

	// A row with no mac is no chip to act on: neither verdict.
	rows3 := []orbRow{{MAC: ""}}
	s.annotateForget(rows3, now)
	if rows3[0].LiveForget {
		t.Error("a cable with no mac was marked live-forgettable")
	}
}

// The unplug window, pinned as the row the page has to render. A board the hub just
// watched leave the bench is refused by the announce clock, not by the cable: the row
// carries no lead, and the refusal is the minute the operator can wait out. This is the
// frame that once opened a dialog offering two serial endings for a board with no serial
// — the reason forgetMode keys the cable body on livePort alone.
func TestTheRowOfABoardThatJustLeftTheBenchNamesNoLead(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	// Heard from 20 seconds ago, and the unplug kick already dialled it dead.
	s.fleet.Observe(obsAt(macHall, "192.168.2.31", now.Add(-20*time.Second)))
	s.live.probe = func(string) bool { return false }
	s.live.check("192.168.2.31")
	stubCables(s) // the bench is empty: the sweep sees nothing, as after a real unplug
	rows := []orbRow{{MAC: macHall}}
	s.annotateForget(rows, clock())
	if rows[0].Forgettable {
		t.Error("a board quiet 20s was plain-forgettable: the minute is the promise")
	}
	if !rows[0].LiveForget {
		t.Error("the refused row carries no dialog verdict")
	}
	if rows[0].LivePort != "" {
		t.Errorf("LivePort = %q for a board off every lead: the dialog would name a cable the hub does not hold", rows[0].LivePort)
	}
	if !strings.Contains(rows[0].ForgetWhy, "announced") {
		t.Errorf("the refusal is not the wait the operator can read: %q", rows[0].ForgetWhy)
	}
}

func TestAQuietRowIsForgettable(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	quietRow(s, macHall, "192.168.2.31", now)
	if ok, why := s.forgetGate(macHall, clock(), true); !ok {
		t.Fatalf("a board unheard for three minutes was refused Forget: %s", why)
	}
}

// The cable case, first because it is the one an operator can fix by hand: the board is
// on a lead, unplugging it is the action, and the refusal has to name the port.
func TestACabledOrbIsNotForgettable(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	quietRow(s, macHall, "192.168.2.31", now)
	stubCables(s, cableSighting{port: "/dev/ttyUSB0", isOrb: true, mac: macHall})
	ok, why := s.forgetGate(macHall, clock(), true)
	if ok {
		t.Fatal("a board on a cable was forgettable: the sweep re-earns its row on the next tick")
	}
	if !strings.Contains(why, "/dev/ttyUSB0") {
		t.Errorf("the refusal does not name the port the cable is in: %q", why)
	}
	if !strings.Contains(why, "reset choices") {
		t.Errorf("the refusal does not point at the Forget dialog's endings: %q", why)
	}
	// Someone else's cable must not hold this row hostage, or one board on a bench would
	// disable Forget for every other board the hub remembers.
	if ok, _ := s.forgetGate(macBed, clock(), true); !ok {
		t.Error("another board's cable made this row unforgettable")
	}
}

// The broadcast case: the row is in the store, its board is announcing, and no entry the
// hub deletes will stop the orb from resending it.
func TestAnAnnouncingOrbIsNotForgettable(t *testing.T) {
	for _, quiet := range []time.Duration{0, 30 * time.Second, 60 * time.Second, liveAnnounceGrace} {
		now := time.Now()
		s, clock := gateClock(now)
		s.fleet.Observe(obsAt(macHall, "192.168.2.31", now.Add(-quiet)))
		ok, why := s.forgetGate(macHall, clock(), true)
		if ok {
			t.Errorf("a board heard %v ago was forgettable", quiet)
			continue
		}
		if !strings.Contains(why, "check-in") {
			t.Errorf("the refusal does not say the orb will announce itself back: %q", why)
		}
	}
}

// The stale-address case, the one the first two checks cannot see: a board that stopped
// announcing but still answers HTTP is connected, and will re-announce. A dial is the only
// way to know, so this is also where the confirm flag's meaning is pinned.
func TestAnAnsweringAddressIsNotForgettable(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	quietRow(s, macHall, "192.168.2.31", now)
	dials := 0
	s.live.probe = func(ip string) bool {
		dials++
		return ip == "192.168.2.31"
	}

	// The door asks, because confirm is set.
	ok, why := s.forgetGate(macHall, clock(), true)
	if ok {
		t.Fatal("a row whose address answers as an orb was forgettable")
	}
	if !strings.Contains(why, "192.168.2.31") {
		t.Errorf("the refusal does not name the address that answered: %q", why)
	}
	if !strings.Contains(why, "reset choices") {
		t.Errorf("the refusal offers no way to actually clear a live board: %q", why)
	}
	if dials != 1 {
		t.Errorf("%d dials for one gated question; the answer should be paid for once and cached", dials)
	}

	// The render path must not pay for that dial: it reads the verdict the door filed. A
	// card rebuilt on every stream wake would otherwise dial the fleet twice a second.
	before := dials
	if ok, _ := s.forgetGate(macHall, clock(), false); ok {
		t.Error("the render path called a live board forgettable off the cached verdict")
	}
	if dials != before {
		t.Errorf("drawing the card dialled %d times; rendering must not probe", dials-before)
	}
}

// The same dial's other outcome. A dead address is why the button exists, so the row must
// be forgettable and the verdict must be cached rather than re-paid on every frame.
func TestADeadAddressIsForgettable(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	quietRow(s, macHall, "192.168.2.31", now)
	dials := 0
	s.live.probe = func(string) bool { dials++; return false }
	for i := 0; i < 5; i++ {
		if ok, why := s.forgetGate(macHall, clock(), true); !ok {
			t.Fatalf("a board that does not answer was refused Forget: %s", why)
		}
	}
	if dials != 1 {
		t.Errorf("%d dials for five reads of one address: the verdict is not being cached", dials)
	}
	if _, known := s.live.verdict("192.168.2.31", clock()); !known {
		t.Error("the dead verdict was not filed, so every card rebuild re-asks it")
	}
}

// A verdict the hub has not paid for is not evidence of absence. The render path dials
// nothing and does not read a blank as a no: it says "the hub is asking". The launch
// fan-out and the ten-second sweep file the answer within a frame or two of any wake
// that matters, so the pause is one beat of honesty, not a missing button. The door,
// which is about to delete, pays for the dial itself and acts on the fresh answer.
func TestACardNeverDialsAndNeverGuesses(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	quietRow(s, macHall, "192.168.2.31", now)
	dials := 0
	s.live.probe = func(string) bool { dials++; return false }
	if ok, why := s.forgetGate(macHall, clock(), false); ok || !strings.Contains(why, "asking") {
		t.Errorf("the render path returned %v with %q: an unpriced address must fail closed "+
			"saying the hub is asking", ok, why)
	}
	if dials != 0 {
		t.Errorf("%d dials from the render path; drawing a card authorises no dial", dials)
	}
	if ok, why := s.forgetGate(macHall, clock(), true); !ok {
		t.Errorf("the door refused to price the question it is about to act on: %q", why)
	}
	if dials != 1 {
		t.Errorf("%d dials for one press; the door asks exactly once", dials)
	}
}

// A provisioning AP address (192.168.4.x) is unreachable by design, not because the board
// is down. Dialing it would file a permanent false "dead" against an orb that is alive on
// its own network, and that verdict would then justify deleting its row.
func TestASoftAPAddressIsNeverDialed(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	s.fleet.Observe(obsAt(macHall, "192.168.4.1", now.Add(-3*time.Minute)))
	dials := 0
	s.live.probe = func(string) bool { dials++; return true }
	if ok, _ := s.forgetGate(macHall, clock(), true); !ok {
		t.Error("an unreachable AP address was held against the row")
	}
	if dials != 0 {
		t.Errorf("%d dials at 192.168.4.1, which the hub has no route to", dials)
	}
	if _, known := s.live.verdict("192.168.4.1", clock()); known {
		t.Error("the hub filed a verdict about an address it cannot route to")
	}
}

// A row with no MAC has no fleet entry to forget. That is the unknown-cable row, and the
// gate says so rather than letting the door report a silent no-op.
func TestAnUnattributedRowHasNothingToForget(t *testing.T) {
	s, clock := gateClock(time.Now())
	for _, mac := range []string{"", "  "} {
		if ok, why := s.forgetGate(mac, clock(), true); ok || why == "" {
			t.Errorf("mac %q: allowed with no reason (%q)", mac, why)
		}
	}
}

// macFor builds a distinct valid base MAC per index, so the round-robin test can have a
// fleet without hand-writing twenty literals. CanonicalMAC wants twelve hex digits.
func macFor(i int) string { return fmt.Sprintf("44bd8d%06x", i) }

// The sweeper is what keeps a cached verdict honest, so it is tested against a fake
// network rather than the gate: a fleet the operator is looking at has to converge, and
// it converges by every remembered address being asked once per tick.
func TestTheSweeperDialsEveryRememberedAddressOnce(t *testing.T) {
	s := newHub()
	var mu sync.Mutex
	asked := map[string]int{}
	s.live.probe = func(ip string) bool {
		mu.Lock()
		asked[ip]++
		mu.Unlock()
		return false
	}
	for i := 0; i < 20; i++ {
		s.fleet.Observe(obsAt(macFor(i), fmt.Sprintf("192.168.9.%d", i+1), time.Now().Add(-time.Hour)))
	}
	targets := s.livenessTargets()
	if len(targets) != 20 {
		t.Fatalf("%d targets for 20 rows: stale rows are being skipped, and a stale row is "+
			"the only kind worth asking about", len(targets))
	}
	if !s.live.probeAll(context.Background(), targets, 0) {
		t.Fatal("the first verdicts (there was no answer before them) did not count as a change")
	}
	mu.Lock()
	if len(asked) != 20 {
		t.Errorf("one tick dialled %d of 20 addresses; a tick covers the whole book", len(asked))
	}
	for ip, n := range asked {
		if n != 1 {
			t.Errorf("%s probed %d times in one tick", ip, n)
		}
	}
	mu.Unlock()
	if s.live.probeAll(context.Background(), targets, 0) {
		t.Error("unchanged verdicts were reported as news, so the stream repaints for nothing")
	}
	mu.Lock()
	for ip, n := range asked {
		if n != 2 {
			t.Errorf("%s asked %d times after two ticks", ip, n)
		}
	}
	mu.Unlock()
	s.live.probe = func(ip string) bool { return ip == "192.168.9.1" }
	if !s.live.probeAll(context.Background(), targets, 0) {
		t.Error("a board that came back to life did not count as a change")
	}
}

// A sweep must wake the card only when a verdict moved, or the pushed stream repaints a
// live page once a tick for no reason the operator can see.
func TestTheSweeperWakesOnlyOnAChange(t *testing.T) {
	s := newHub()
	live := true
	s.live.probe = func(string) bool { return live }
	s.fleet.Observe(obsAt(macHall, "192.168.2.31", time.Now().Add(-time.Hour)))
	targets := s.livenessTargets()
	if !s.live.probeAll(context.Background(), targets, 0) {
		t.Fatal("the first verdict (there was no answer before it) did not count as a change")
	}
	if s.live.probeAll(context.Background(), targets, 0) {
		t.Error("an unchanged verdict was reported as news")
	}
	live = false
	if !s.live.probeAll(context.Background(), targets, 0) {
		t.Error("a board that went silent did not count as a change")
	}
}

// The soft-AP rule belongs to the target list too: the address a board held on its own
// provisioning network is not a dial target, and dialing it files a false verdict.
func TestTheSweeperSkipsUnreachableAddresses(t *testing.T) {
	s := newHub()
	s.live.probe = func(string) bool { return true }
	s.fleet.Observe(obsAt(macHall, "192.168.4.1", time.Now().Add(-time.Hour)))
	s.fleet.Observe(obsAt(macBed, "192.168.2.40", time.Now().Add(-time.Hour)))
	s.orbIP = "192.168.4.1"
	got := s.livenessTargets()
	if len(got) != 1 || got[0] != "192.168.2.40" {
		t.Errorf("targets %v: the unrouteable AP address must not be dialled and a real one must "+
			"not be dropped", got)
	}
}

// Everything above is the card being helpful. This is the security property: a page that
// ignores the disabled state still cannot delete a board the hub can reach, because the
// door re-asks the same question the button was disabled for.
func TestTheDoorRefusesWhatTheCardWouldHaveRefused(t *testing.T) {
	now := time.Now()
	s, _ := gateClock(now)
	s.fleet.Observe(obsAt(macHall, "192.168.2.31", now.Add(-10*time.Second)))
	s.live.probe = func(string) bool { return true }
	rec := postFleet(t, s, "/api/fleet/forget", map[string]string{"mac": macHall},
		map[string]string{"X-Orb-Token": s.token})
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("the door accepted a forget the gate forbids: %d %s", rec.Code, rec.Body.String())
	}
	if _, ok := s.fleet.Look(macHall); !ok {
		t.Fatal("the refused forget still deleted the row")
	}
	if !strings.Contains(rec.Body.String(), "announced itself") {
		t.Errorf("the door gave no reason: %s", rec.Body.String())
	}

	// The same board, aged out of the grace and dead at its address, is accepted by the
	// same door: the refusal is about liveness, not about this MAC being unwelcome.
	s2, _ := gateClock(now)
	quietRow(s2, macHall, "192.168.2.31", now)
	s2.grants.learnAs("192.168.2.31", macHall)
	s2.live.probe = func(string) bool { return false }
	rec = postFleet(t, s2, "/api/fleet/forget", map[string]string{"mac": macHall},
		map[string]string{"X-Orb-Token": s2.token})
	if rec.Code != http.StatusOK {
		t.Fatalf("a genuinely dead board could not be forgotten: %d %s", rec.Code, rec.Body.String())
	}
	if _, ok := s2.fleet.Look(macHall); ok {
		t.Error("the accepted forget left the row behind")
	}
}

// A row's verdict must ride the card, or the page cannot render the rule and every button
// on it is a guess. Checked at the seam both transports use.
func TestTheCardCarriesTheForgetVerdict(t *testing.T) {
	now := time.Now()
	s, _ := gateClock(now)
	s.fleet.Observe(obsAt(macHall, "192.168.2.31", now.Add(-10*time.Second)))
	s.browse = filled(t, orbmdns.Candidate{
		MAC: macHall, Instance: inst(macHall), IP: "192.168.2.31", Port: 80,
		From: "192.168.2.31", LastSeen: now, Expires: now.Add(2 * time.Minute),
	})
	body, _ := s.orbCard(now)
	rows, _ := body["orbs"].([]orbRow)
	if len(rows) != 1 {
		t.Fatalf("%d rows, want the one announced board", len(rows))
	}
	if rows[0].Forgettable {
		t.Error("a board announcing itself right now is drawn as forgettable")
	}
	if rows[0].ForgetWhy == "" {
		t.Error("the row is not forgettable and carries no reason, so the button is a mystery")
	}
}

// The age inside a refusal is the useful part of the message and also the part that moves
// on its own. If it reached the stream digest the pushed card would repaint once a frame
// to change a number nobody reads — AgeS's problem exactly.
func TestTheRefusalTextDoesNotChurnTheStream(t *testing.T) {
	rows := []orbRow{
		{Key: macHall, MAC: macHall, IP: "192.168.2.31", Forgettable: false, ForgetWhy: "announced itself 30s ago"},
		{Key: macBed, MAC: macBed, IP: "192.168.2.40", Forgettable: false, ForgetWhy: "announced itself 31s ago"},
	}
	a := orbVersion(rows, nil, nil)
	if a == "" {
		t.Fatal("no digest")
	}
	rows[0].ForgetWhy = "announced itself 91s ago"
	rows[1].ForgetWhy = "on a cable at /dev/ttyUSB7"
	if orbVersion(rows, nil, nil) != a {
		t.Error("the digest moved on a refusal's wording alone, so the stream repaints for an age")
	}
	rows[0].Forgettable = true
	if orbVersion(rows, nil, nil) == a {
		t.Error("the digest ignored a verdict that changed, so a row keeps the wrong button")
	}
}

// The Forget rule the operator can read off the card and wait out: one minute after
// the last-heard packet, with the address also answered dead, the button is armed. The
// grace is exactly one MDNS_ADV_REFRESH_S because the verdict — re-priced every ten
// seconds — is what covers a single lost packet now; the old lower bound on the grace
// existed only because the clock was the sole connected-or-not signal. It stays under
// the browser's row life so a visible row can still become forgettable.
func TestForgetArmsTheMinuteABoardGoesQuiet(t *testing.T) {
	if liveAnnounceGrace != 60*time.Second {
		t.Fatalf("grace is %v; the card promises Forget one minute after quiet", liveAnnounceGrace)
	}
	if rowLife := 120*time.Second + orbmdns.DefaultGrace; liveAnnounceGrace >= rowLife {
		t.Errorf("grace %v is at or over the row's own life (%v): no visible row could ever be "+
			"forgettable", liveAnnounceGrace, rowLife)
	}
	for _, quiet := range []time.Duration{0, 30 * time.Second, 59 * time.Second, liveAnnounceGrace} {
		now := time.Now()
		s, clock := gateClock(now)
		s.fleet.Observe(obsAt(macHall, "192.168.2.31", now.Add(-quiet)))
		s.live.record("192.168.2.31", false, now)
		if ok, why := s.forgetGate(macHall, clock(), false); ok || !strings.Contains(why, "announced") {
			t.Errorf("a board quiet %v was %v (%q): the minute is the promise", quiet, ok, why)
		}
	}
	for _, quiet := range []time.Duration{liveAnnounceGrace + time.Second, 3 * time.Hour} {
		now := time.Now()
		s, clock := gateClock(now)
		s.fleet.Observe(obsAt(macHall, "192.168.2.31", now.Add(-quiet)))
		s.live.record("192.168.2.31", false, now)
		if ok, why := s.forgetGate(macHall, clock(), false); !ok {
			t.Errorf("a board quiet %v with a dead verdict on file was refused Forget: %q", quiet, why)
		}
	}
}

// A cached alive verdict holds the gate while fresh and never past its staleness: a
// board that died one sweep after the last good dial is re-priced by the door and
// forgettable, not held shut on stale trust.
func TestAFreshVerdictHoldsTheGateAndAnOldOneDoesNot(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	quietRow(s, macHall, "192.168.2.31", now)
	s.live.record("192.168.2.31", true, now.Add(-30*time.Second))
	if ok, why := s.forgetGate(macHall, clock(), false); ok || !strings.Contains(why, "answers") {
		t.Errorf("a fresh alive verdict let the card draw Forget (%v, %q)", ok, why)
	}
	s.live.record("192.168.2.31", true, now.Add(-liveVerdictStale-time.Minute))
	s.live.probe = func(string) bool { return false }
	if ok, why := s.forgetGate(macHall, clock(), true); !ok {
		t.Errorf("a stale alive verdict held the door shut un-re-priced: %q", why)
	}
}

// The second the lead goes quiet, the cable kick re-dials the departed board's
// remembered address: it may have moved to Wi-Fi, and its disappearance from the bench
// alone must neither freeze its verdict nor start its Forget clock on a maybe. The
// first sweep only lays a baseline — a hub that has never seen the bench cannot know an
// unplug happened.
func TestAnUnplugRedialsTheRememberedAddress(t *testing.T) {
	s := newHub()
	var mu sync.Mutex
	var dialed []string
	s.live.probe = func(ip string) bool {
		mu.Lock()
		dialed = append(dialed, ip)
		mu.Unlock()
		return true
	}
	s.fleet.Observe(obsAt(macHall, "192.168.2.31", time.Now()))
	f := plugCables(s, nil)
	f.set(orbAt("/dev/ttyACM0", macHall))
	now := time.Now()
	s.roster(nil, now)
	if snap := dialedSnapshot(&mu, &dialed); len(snap) != 0 {
		t.Fatalf("the baseline sweep dialled %v: a first sight is not an unplug", snap)
	}
	f.set() // the lead is gone
	s.roster(nil, now)
	for i := 0; i < 100 && len(dialedSnapshot(&mu, &dialed)) == 0; i++ {
		time.Sleep(10 * time.Millisecond)
	}
	mu.Lock()
	defer mu.Unlock()
	if len(dialed) != 1 || dialed[0] != "192.168.2.31" {
		t.Fatalf("unplugging dialled %v: the lost board's remembered address must be re-priced "+
			"exactly once, at once", dialed)
	}
	f.set(orbAt("/dev/ttyACM1", macBed))
	s.roster(nil, now)
	f.set()
	s.roster(nil, now)
	time.Sleep(50 * time.Millisecond)
	if n := len(dialed); n != 1 {
		t.Errorf("%d dials: an address-less cable board must not be dialled at its unplug", n)
	}
}

// dialedSnapshot copies the probe log under the mutex the probe closure appends under.
// The unplug kick checks on its own goroutine (liveness.go kickLostCables), so a bare
// read of the slice in this test is a race the -race detector rightly fails: the
// snapshot is the same question, asked with the lock held.
func dialedSnapshot(mu *sync.Mutex, dialed *[]string) []string {
	mu.Lock()
	defer mu.Unlock()
	return append([]string(nil), *dialed...)
}

// The refusal has to survive the wire, and this is the regression it did not: the gate
// answered "on a cable" and the page still lit the button. The two ends disagree only
// through the JSON, so the test marshals the annotated row exactly as the card sends it and
// reads the bytes back. A bool tagged omitempty drops its false, and the false is the
// refusal, so the payload must carry it literally. Checking the Go struct field — which is
// what the other tests here do — cannot see this class of bug at all: the field is correct
// in memory and gone on the wire.
func TestTheRefusalSurvivesTheWire(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	quietRow(s, macHall, "192.168.2.31", now)
	stubCables(s, cableSighting{port: "/dev/ttyACM0", isOrb: true, mac: macHall})
	rows := []orbRow{{Key: macHall, MAC: macHall, IP: "192.168.2.31"}}
	s.annotateForget(rows, clock())
	raw, err := json.Marshal(rows[0])
	if err != nil {
		t.Fatalf("the card row cannot marshal: %v", err)
	}
	if !strings.Contains(string(raw), `"forgettable":false`) {
		t.Fatalf("the refusal never reached the wire, so the page cannot tell it from a yes: %s", raw)
	}
	if !strings.Contains(string(raw), `"forgetWhy":"`) {
		t.Fatalf("the row was refused with no reason on the wire: %s", raw)
	}
	// Now the page's side of the contract, on the decoded bytes rather than the struct.
	// app.js enables Forget only when the field is literally true; anything else, an
	// absent key included, leaves the button dead.
	var onPage map[string]any
	if err := json.Unmarshal(raw, &onPage); err != nil {
		t.Fatalf("the card row does not decode: %v", err)
	}
	if onPage["forgettable"] == true {
		t.Error("the page's predicate sees permission where the hub refused")
	}
}

// The other half of the same contract: a row the hub really did clear has to say so on the
// wire in a form the page lights up for, or the fix above disables Forget everywhere and
// the dead board on the shelf becomes undeletable.
func TestThePermissionSurvivesTheWire(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	quietRow(s, macHall, "192.168.2.31", now)
	// The verdict the ten-second sweep has already priced and filed. A render never
	// dials, and an unpriced row says "the hub is asking" — the button this test pins
	// travels with the answer the hub has actually got.
	s.live.record("192.168.2.31", false, now)
	rows := []orbRow{{Key: macHall, MAC: macHall, IP: "192.168.2.31"}}
	s.annotateForget(rows, clock())
	if !rows[0].Forgettable {
		t.Fatal("a quiet, uncabled, unanswered row was refused: the button would never appear")
	}
	raw, err := json.Marshal(rows[0])
	if err != nil {
		t.Fatalf("the card row cannot marshal: %v", err)
	}
	if !strings.Contains(string(raw), `"forgettable":true`) {
		t.Fatalf("a cleared row does not say so on the wire: %s", raw)
	}
}

// The cable rule keyed on the MAC the host read off the USB descriptor, which is the same
// The cable refusal comes from the live sweep, and only from the sweep. A sighting the
// host cannot attribute to a MAC is not an objection to forgetting one particular board:
// the store used to bridge that gap with a remembered socket path, and that memory is
// gone — an address book keeps where a board is reachable, not which port held it last.
// So the test of the refusal is whether the sweep can name the MAC, which it can on a
// host that exposes the serial descriptor.
func TestTheCableRefusalComesFromAKeyedSweep(t *testing.T) {
	ask := time.Now()
	s := newHub()
	// Four minutes behind, so the announce-grace branch is out and only the cable can refuse.
	s.fleet = orbfleet.New(orbfleet.Config{Now: func() time.Time { return ask.Add(-4 * time.Minute) }})
	if _, err := s.fleet.Confirm(macHall); err != nil {
		t.Fatalf("the store would not record the board: %v", err)
	}

	// A sighting with no mac: the sweep cannot tie it to this board, so Forget stands.
	stubCables(s, cableSighting{port: "/dev/ttyACM1", isOrb: true, mac: ""})
	if ok, why := s.forgetGate(macHall, ask, true); !ok {
		t.Errorf("an unattributable cable blocked a Forget the hub cannot tie to this board: %q", why)
	}

	// The same board on a sighting the host CAN key on MAC: the refusal returns, and it
	// names the socket. A fresh hub, because stubCables arms a TTL cache and re-stubbing
	// the same one reads the first sweep back.
	keyed := newHub()
	keyed.fleet = orbfleet.New(orbfleet.Config{Now: func() time.Time { return ask.Add(-4 * time.Minute) }})
	if _, err := keyed.fleet.Confirm(macHall); err != nil {
		t.Fatalf("confirm: %v", err)
	}
	stubCables(keyed, cableSighting{port: "/dev/ttyACM1", isOrb: true, mac: macHall})
	if ok, why := keyed.forgetGate(macHall, ask, true); ok {
		t.Error("a board the sweep puts on a cable the host keyed to its mac was forgettable")
	} else if !strings.Contains(why, "/dev/ttyACM1") {
		t.Errorf("the port refusal does not name the socket: %q", why)
	}
}

// The power-off bug, pinned. A board seen on the LAN and then unplugged from the
// wall stops answering HTTP long before its cached announce ages out, and the card
// used to believe the cache: annotateReachability only asked the rows the address
// book produced, so a browse row kept Via ["lan"] — and the tile kept its on-LAN
// claim — for the rest of the TTL plus grace, with the sweep's dead verdict
// already in hand. The verdict is the freshest fact on the row and outranks the
// record; the row keeps its address and loses its present tense.
func TestAPoweredOffOrbLosesItsLanClaimToTheDeadVerdict(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	s.fleet.Observe(obsAt(macHall, "192.168.2.31", now.Add(-20*time.Second)))
	s.live.record("192.168.2.31", false, now.Add(-time.Second))
	// The row the browse tier builds: announced, listed, Via lan, no remembered-
	// tier Reachable word. This is the exact shape that used to skip annotation.
	rows := []orbRow{{
		Key: macHall, MAC: macHall, IP: "192.168.2.31", Port: 80,
		Via: []string{"lan"}, Dialable: true,
	}}
	s.annotateReachability(rows, clock())
	r := rows[0]
	if r.Connected {
		t.Error("the tile still reads connected while its address is known dead")
	}
	if r.Reachable != "dead" {
		t.Errorf("Reachable = %q, want dead on the announced row the sweep answered for", r.Reachable)
	}
	if r.Dialable {
		t.Error("the row stays clickable onto an address the sweep just called dead")
	}
	for _, v := range r.Via {
		if v == "lan" {
			t.Errorf("Via still claims the LAN the verdict denies: %v", r.Via)
		}
	}
}

// The other side of the same line: absence of a verdict is absence of evidence,
// not evidence of absence. A row the wire is producing with no dial paid for yet
// keeps its connected read, and a remembered row says the hub is asking rather
// than deciding on a hunch.
func TestNoVerdictContradictsNoRow(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	s.fleet.Observe(obsAt(macHall, "192.168.2.31", now.Add(-5*time.Second)))
	rows := []orbRow{
		{Key: macHall, MAC: macHall, IP: "192.168.2.31", Via: []string{"lan"}},
		{Key: "other", MAC: "44bd8d782619", IP: "192.168.2.32", Reachable: "unknown"},
	}
	s.annotateReachability(rows, clock())
	if !rows[0].Connected || rows[0].Reachable != "" {
		t.Errorf("an unasked-about announced row lost its earned claim: %+v", rows[0])
	}
	if rows[1].Connected || rows[1].Reachable != "unknown" {
		t.Errorf("a remembered row claims a present the hub has not asked about: %+v", rows[1])
	}
}

// A lead is not an HTTP timeout. A board sitting on a cable whose remembered
// address does not answer is connected — over that lead — and only the lan half
// of its claim goes.
func TestADeadAddressOnALiveLeadStillReadsConnected(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	s.fleet.Observe(obsAt(macHall, "192.168.2.31", now.Add(-2*time.Minute)))
	s.live.record("192.168.2.31", false, now)
	rows := []orbRow{{
		Key: macHall, MAC: macHall, IP: "192.168.2.31",
		OnCable: true, Via: []string{"usb", "lan"},
	}}
	s.annotateReachability(rows, clock())
	r := rows[0]
	if !r.Connected {
		t.Error("a board on a lead read disconnected because an old address timed out")
	}
	if !hasStr(r.Via, "usb") || hasStr(r.Via, "lan") {
		t.Errorf("Via = %v, want usb kept and the dead lan claim dropped", r.Via)
	}
}

// The status body the sweep already pays for carries fw, state and the board's
// name, and foldProbeStatus is what stopped discarding it. Two rules keep the
// fold honest, and both are pinned here: a dial renews facts and never the
// presence clock — the Forget gate must not be fed by the hub dialing its own
// address book — and the answer folds onto the MAC the BODY named, so a board
// that moved addresses re-earns its own row rather than buying a stranger's.
func TestAStatusAnswerFoldsItsFactsAndNothingElse(t *testing.T) {
	now := time.Now()
	s, _ := gateClock(now)
	if _, err := s.fleet.Observe(obsAt(macHall, "192.168.2.31", now.Add(-90*time.Second))); err != nil {
		t.Fatal(err)
	}
	before, _ := s.fleet.Look(macHall)
	if before.FW != "094fa22" {
		t.Fatalf("the seeded row does not carry the stamp the test replaces: %+v", before)
	}

	s.foldProbeStatus("192.168.2.31", &orbnet.Status{
		MAC: macHall, FW: "v1.3.0+deadbee", State: "normal", Name: "Hall",
	})
	after, _ := s.fleet.Look(macHall)
	if after.FW != "v1.3.0+deadbee" || after.State != "normal" {
		t.Errorf("the answer did not fold: %+v", after)
	}
	if after.Name != "Hall" {
		t.Errorf("the name a successful read reported did not fold: %+v", after)
	}
	if !after.LastSeen.Equal(before.LastSeen) {
		t.Errorf("the dial renewed the presence clock: %v -> %v", before.LastSeen, after.LastSeen)
	}

	// An answer with no stamp is old firmware, which is no news: it must not wipe
	// the version the announce already carried.
	s.foldProbeStatus("192.168.2.31", &orbnet.Status{MAC: macHall})
	after, _ = s.fleet.Look(macHall)
	if after.FW != "v1.3.0+deadbee" {
		t.Errorf("a stamp-less answer erased a known version: %+v", after)
	}

	// A body naming a MAC the store has never met is a stranger at a remembered
	// address. Adopt and the browse are the doors that introduce boards; a sweep
	// reply is not, and it must not create a row.
	s.foldProbeStatus("192.168.2.31", &orbnet.Status{MAC: "de ad be ef 00 11", FW: "v9"})
	if _, ok := s.fleet.Look("deadbeef0011"); ok {
		t.Error("a stranger's answer at a known address created a fleet row")
	}

	// And the same stranger's answer folds onto nothing when its own row lives at
	// a different address: the cross-lock is the announce's to move, not the dial's.
	if _, err := s.fleet.Observe(obsAt(macBed, "192.168.2.99", now.Add(-time.Minute))); err != nil {
		t.Fatal(err)
	}
	s.foldProbeStatus("192.168.2.31", &orbnet.Status{MAC: macBed, FW: "v9"})
	if bed, _ := s.fleet.Look(macBed); bed.FW == "v9" {
		t.Error("a fold landed on a row whose address is not the one the hub dialed")
	}
}
