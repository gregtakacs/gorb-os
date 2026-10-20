package main

import (
	"context"
	"errors"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
)

// The regression these pin: a board reflashed out of gorb-os is dropped off its cable by
// a paid verdict, and that verdict lived only in memory. Restarting the hub therefore
// restored "Gold on a cable" from a MAC that cannot possibly carry the fact — the user
// watched the same tile come back to life for no reason they could see. The launch sweep
// answers the question again instead of inheriting the descriptor's guess.

// probeScript is the bench the launch sweep meets: scripted answers per port, and a
// record of every port the hub decided to reset.
type probeScript struct {
	mu     sync.Mutex
	asked  []string
	answer map[string]func() (esprom.OrbInfo, error)
	err    error
}

func (p *probeScript) probe(_ context.Context, port string) (esprom.OrbInfo, error) {
	p.mu.Lock()
	p.asked = append(p.asked, port)
	f := p.answer[port]
	p.mu.Unlock()
	if f == nil {
		return esprom.OrbInfo{Port: port}, errors.New("esprom: no answer")
	}
	return f()
}

func (p *probeScript) count() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return len(p.asked)
}

func (p *probeScript) ports() []string {
	p.mu.Lock()
	defer p.mu.Unlock()
	return append([]string(nil), p.asked...)
}

// launchHub builds a hub whose bench is Gold on one lead and an Arduino on another, with
// the address book a finished onboarding would have left behind.
func launchHub(t *testing.T, seen []cableSighting, p *probeScript) *Server {
	t.Helper()
	s := newHub()
	s.statePath = filepath.Join(t.TempDir(), "state.json")
	s.fleet = orbfleet.New(orbfleet.Config{})
	if _, err := s.fleet.Observe(orbfleet.Observation{
		MAC: macHall, Instance: "orb-782618", IP: "192.0.2.31",
		FW: "v1.0.0", State: "ready", Port: 80, At: time.Now().Add(-time.Hour),
	}); err != nil {
		t.Fatalf("seed fleet row: %v", err)
	}
	if _, _, err := s.fleet.NoteDeviceName(macHall, "Gold"); err != nil {
		t.Fatalf("name the board: %v", err)
	}
	if err := s.fleet.MarkRemembered(macHall); err != nil {
		t.Fatalf("mark remembered: %v", err)
	}
	s.cables.ttl = 0
	s.cables.list = func() ([]cableSighting, error) { return seen, nil }
	s.identities.usbMAC = func(port string) string {
		if port == reflashPort {
			return "44:bd:8d:78:26:18"
		}
		return ""
	}
	s.probeOrb = p.probe
	return s
}

func goldBenchPorts() []cableSighting {
	return []cableSighting{{port: reflashPort, isOrb: true, espressif: true, mac: macHall}}
}

func foreignAnswer() (esprom.OrbInfo, error) {
	return esprom.OrbInfo{Port: reflashPort, MAC: "44:bd:8d:78:26:18", IsOrb: false,
		Project: "esp-idf", Reason: "identified as not an orb"}, nil
}

func orbAnswer() (esprom.OrbInfo, error) {
	return esprom.OrbInfo{Port: reflashPort, MAC: "44:bd:8d:78:26:18", IsOrb: true,
		Project: "gorb-os", Version: "v1.0.0", State: "b", Name: "Gold"}, nil
}

// The user's exact report: hub restarted, Gold back on a cable with nothing asked. After
// this sweep the paid verdict exists again and the row is not on a cable.
func TestLaunchIdentifyClearsACableTheDescriptorOnlyClaims(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	s := launchHub(t, goldBenchPorts(), p)

	if _, ok := s.contradicts(reflashPort); ok {
		t.Fatal("setup: a fresh hub holds no paid verdict, which is the whole bug")
	}
	s.probeCablesAtLaunch(context.Background())

	if p.count() != 1 {
		t.Fatalf("the sweep reset %d ports (%v); Gold's lead must be asked once", p.count(), p.ports())
	}
	if _, ok := s.contradicts(reflashPort); !ok {
		t.Fatal("the launch probe filed nothing usable, so the descriptor still owns the row")
	}
	rows, _ := s.roster(nil, time.Now())
	s.annotateOrb(rows)
	r := rowFrom(t, rows)
	if r.OnCable {
		t.Error("a hub restart still paints the board as on a cable")
	}
	if r.Name != "Gold" || !r.Orb {
		t.Errorf("name=%q orb=%v: the board must stay a named card so it can be forgotten", r.Name, r.Orb)
	}
}

// A reset is not free, so it is not spent twice on a settled bench: the second hub start
// inside the window inherits the paid answer rather than rebooting the board again.
func TestLaunchIdentifyAsksEachLeadOncePerWindow(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	s := launchHub(t, goldBenchPorts(), p)

	s.probeCablesAtLaunch(context.Background())
	s.probeCablesAtLaunch(context.Background())
	if got := p.count(); got != 1 {
		t.Errorf("two launch sweeps reset the bench %d times; a fresh paid answer must be honoured", got)
	}

	// The same is true across a restart of the process: the sweep is gated on the
	// cache, which the second hub starts empty, so it asks once and only once more.
	if _, ok := s.contradicts(reflashPort); !ok {
		t.Fatal("the verdict the first sweep paid for did not survive to gate the second")
	}
}

// The other half of the safety promise: the sweep resets the bench the hub can
// attribute, and nothing else. EnterDownloadMode pulses DTR/RTS, so a probe aimed at the
// Arduino on the same hub resets a device nobody asked about.
func TestLaunchIdentifyNeverTouchesAPortItCannotAttribute(t *testing.T) {
	bench := []cableSighting{
		{port: reflashPort, isOrb: true, mac: macHall},
		{port: "/dev/ttyACM1", isOrb: false},                      // an Espressif chip in download mode
		{port: "/dev/ttyUSB7", isOrb: false, mac: "aabbccddeeff"}, // a UART bridge: no descriptor to trust
	}
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	s := launchHub(t, bench, p)

	s.probeCablesAtLaunch(context.Background())

	got := p.ports()
	if len(got) != 1 || got[0] != reflashPort {
		t.Fatalf("reset ports = %v; only the lead the host attributes to the orb peripheral may be pulsed", got)
	}
}

// A lonely mark and a busy lead both mean "do not reset this now": the first is a verdict
// already paid for, and the second is someone writing the chip.
func TestLaunchIdentifyLeavesALonelyAndABusyLeadAlone(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	s := launchHub(t, goldBenchPorts(), p)
	// The lonely state the hub can actually reach on a timer: it asked, got a negative,
	// and marked the board. That answer is on file, so nothing re-prices it on a clock.
	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", Project: "esp-idf"}, provenProbe)
	s.lonely.mark(macHall)
	s.identifyCables(context.Background())
	if p.count() != 0 {
		t.Errorf("a board the hub left lonely was reset on a timer: %v", p.ports())
	}

	// The mark must not become unfalsifiable: once the answer it rests on is due again,
	// the hub asks.
	backdateIdentity(s, reflashPort, time.Hour)
	s.identifyCables(context.Background())
	if p.count() == 0 {
		t.Fatal("a lonely mark made the question unaskable forever")
	}

	job, _, err := s.ops.start(context.Background(), reflashPort, opFlash, time.Minute)
	if err != nil {
		t.Fatalf("hold the lead: %v", err)
	}
	before := p.count()
	s.identifyCables(context.Background())
	if p.count() != before {
		t.Errorf("the sweep reset a lead a flash holds: %v", p.ports())
	}
	s.ops.finish(job)
}

// The one lonely case that is asked immediately: the operator just plugged the board in,
// so the hub has no answer on file and the mark is the only thing it holds.
func TestALonelyMarkYieldsToAFreshPlugIn(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	s := launchHub(t, goldBenchPorts(), p)
	s.lonely.mark(macHall)
	s.identifyCables(context.Background())
	if p.count() != 1 {
		t.Fatalf("a board just plugged in was interrogated %d times; a re-plug voids even a lonely mark", p.count())
	}
}

// A healthy orb must come out of the launch sweep no worse than it went in: it is still
// on its lead, and the version it printed is now on the row.
func TestLaunchIdentifyKeepsAGorbOsBoardOnItsCable(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
	s := launchHub(t, goldBenchPorts(), p)

	s.probeCablesAtLaunch(context.Background())

	rows, _ := s.roster(nil, time.Now())
	r := rowFrom(t, rows)
	if !r.OnCable {
		t.Error("the launch probe stripped the cable off an orb that answered as an orb")
	}
	if _, ok := s.contradicts(reflashPort); ok {
		t.Error("an orb that answered for itself contradicted its own lead")
	}
}

// The hole that would otherwise reappear half an hour later: identityTTL ages every cached
// answer out, and for a negative verdict expiry silently restores the descriptor's claim.
// The watcher re-asks inside the window instead.
func TestAStaleNotOrbVerdictIsReAskedBeforeItExpires(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	s := launchHub(t, goldBenchPorts(), p)

	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", IsOrb: false, Project: "esp-idf"}, provenProbe)
	backdateIdentity(s, reflashPort, time.Hour)

	s.identifyCables(context.Background())
	if p.count() == 0 {
		t.Fatal("an expired not-an-orb verdict was left to rot, which hands the cable back to the MAC")
	}
	if _, ok := s.contradicts(reflashPort); !ok {
		t.Fatal("the re-ask did not leave a usable verdict behind")
	}
	rows, _ := s.roster(nil, time.Now())
	if rowFrom(t, rows).OnCable {
		t.Error("the board is drawn on a cable again after the re-ask")
	}
}

// backdateIdentity ages one cached answer so a test can stand at the far edge of a TTL
// without sleeping through it.
func backdateIdentity(s *Server, port string, by time.Duration) {
	s.identities.mu.Lock()
	defer s.identities.mu.Unlock()
	if e, ok := s.identities.byPort[port]; ok {
		e.at = time.Now().Add(-by)
		s.identities.byPort[port] = e
	}
}

// A fresh negative verdict is not worth a reboot, and neither is a positive one: the free
// facts corroborate those continuously.
func TestTheReaskSpendsNoResetOnAnAnswerThatIsStillTrue(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	s := launchHub(t, goldBenchPorts(), p)
	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", IsOrb: false}, provenProbe)

	s.identifyCables(context.Background())
	if p.count() != 0 {
		t.Errorf("a verdict filed seconds ago was re-paid: %v", p.ports())
	}

	// A fresh positive is the one answer the free facts corroborate continuously, so it
	// buys no reboot: the row already shows what the board said it runs.
	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", IsOrb: true, Project: "gorb-os"}, provenProbe)
	s.identifyCables(context.Background())
	if p.count() != 0 {
		t.Errorf("a fresh orb answer was re-paid on a clock: %v", p.ports())
	}
}

// The mirror case, and the reason the sweep reaps before it decides: an answer past its
// TTL is not a believed answer, and an entry that is neither believed nor questioned is a
// silent permanent claim. On a headless bench, where nothing renders a port list to reap
// it, that claim would block the question forever.
func TestAnExpiredOrbAnswerBuysNoImmunity(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
	s := launchHub(t, goldBenchPorts(), p)
	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", IsOrb: true, Project: "gorb-os", Version: "v0.0.1"}, provenProbe)
	backdateIdentity(s, reflashPort, time.Hour)

	s.identifyCables(context.Background())

	if p.count() == 0 {
		t.Fatal("an answer the hub no longer trusts also refused to be re-asked, which is a guess held forever")
	}
	rows, _ := s.roster(nil, time.Now())
	s.annotateOrb(rows)
	r := rowFrom(t, rows)
	if !r.OnCable {
		t.Error("a board that answered as an orb lost its cable")
	}
}

// The wiring a suite cannot execute: the sweep must be started by main, and must be
// switchable off, because it costs every wired board a reboot at hub start.
func TestTheLaunchIdentifyIsWiredAndSwitchable(t *testing.T) {
	src := readFile(t, "main.go")
	if !strings.Contains(src, `flag.BoolVar(&probeCablesOnLaunch, "probe-cables", true`) {
		t.Error("the launch identify lost its flag or its default-on")
	}
	i := strings.Index(src, "if probeCablesOnLaunch {")
	j := strings.Index(src, "go s.probeCablesAtLaunch(cableCtx)")
	if i < 0 || j < 0 || i > j {
		t.Error("the launch identify is no longer started by main, or no longer honours its flag")
	}
	w := readFile(t, "roster.go")
	if !strings.Contains(w, "s.cables.gone = s.forgetCablePorts") {
		t.Error("the cable sweep never reports a departure, so a re-plug inherits the old verdict")
	}
	if !strings.Contains(w, "s.identifyCables(ctx)") {
		t.Error("the watcher no longer keeps a not-an-orb verdict inside its own expiry window")
	}
}

// The ordering this file must survive: an aged entry is not merely distrusted, it is
// DELETED by the freshness rule, and handlePorts reads the cache on every refresh. So a
// not-an-orb verdict that is left to expire is gone rather than merely stale, and the
// re-ask pass finds nothing to re-ask.
func TestAnExpiredNotOrbVerdictSurvivesToBeReAsked(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	s := launchHub(t, goldBenchPorts(), p)
	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", IsOrb: false, Project: "esp-idf"}, provenProbe)
	backdateIdentity(s, reflashPort, time.Hour)

	// What the SSE stream does on a tick, which is where an aged entry gets reaped.
	s.identities.all()
	s.identifyCables(context.Background())

	if _, ok := s.contradicts(reflashPort); !ok {
		t.Fatal("the verdict expired rather than being re-asked, so the descriptor owns the lead again")
	}
	rows, _ := s.roster(nil, time.Now())
	s.annotateOrb(rows)
	if rowFrom(t, rows).OnCable {
		t.Error("the board is drawn on a cable again an hour after it stopped being an orb")
	}
}

// The re-plug tests: the operator's requirement that pulling a cable and putting it back
// always makes the hub ask again, whatever it believes it already knows.

// bench is a cable layer the test can re-seat boards on.
type bench struct {
	mu   sync.Mutex
	seen []cableSighting
	fail error
}

func (b *bench) list() ([]cableSighting, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.fail != nil {
		return nil, b.fail
	}
	return append([]cableSighting(nil), b.seen...), nil
}

func (b *bench) set(s []cableSighting) {
	b.mu.Lock()
	b.seen = s
	b.mu.Unlock()
}

func (b *bench) unplugged() { b.set(nil) }

func (b *bench) broken(e error) {
	b.mu.Lock()
	b.fail = e
	b.mu.Unlock()
}

func goldSighting() []cableSighting {
	return []cableSighting{{port: reflashPort, isOrb: true, espressif: true, mac: macHall}}
}

// wiredHub is a hub on a bench with Gold in it, its lead already filed as not-an-orb by a
// probe the hub paid for — the settled state a re-plug has to disturb.
func wiredHub(t *testing.T, p *probeScript, b *bench) *Server {
	t.Helper()
	s := launchHub(t, goldSighting(), p)
	b.set(goldSighting())
	s.cables.list = b.list
	s.cables.gone = s.forgetCablePorts
	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", Project: "esp-idf"}, provenProbe)
	if _, ok := s.contradicts(reflashPort); !ok {
		t.Fatal("setup: the hub should start believing this lead runs something else")
	}
	// Lay the baseline the departure is measured against. refresh deliberately treats the
	// first sweep as news to nobody, so a test that wants an unplug to mean something has
	// to have seen the board plugged in first — exactly as the hub does.
	s.cables.refresh(time.Now())
	return s
}

// The exact promise: same board, same port, same MAC, yanked and put back — and the hub
// asks rather than repeating the answer it holds. The serial gate cannot catch this,
// because the serial matches again the moment the board is back.
func TestRePlugMakesTheHubAskAgainAboutTheSameBoard(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	b := &bench{}
	s := wiredHub(t, p, b)

	s.identifyCables(context.Background())
	if p.count() != 0 {
		t.Fatalf("a lead with a fresh verdict on file was reset: %v", p.ports())
	}

	b.unplugged()
	s.cables.force()
	s.cables.refresh(time.Now())
	b.set(goldSighting())
	s.cables.force()
	s.identifyCables(context.Background())

	if p.count() != 1 {
		t.Fatalf("re-plugging the board asked it %d times; the hub must never answer \"what runs here\" from a memory of the last time it was there", p.count())
	}
}

// The departure must actually void the claim, not merely distrust it. A not-an-orb verdict
// is TTL-exempt on purpose, so anything short of a delete would leave it standing forever
// and a board reflashed while unplugged would be filed back as what it ran an hour ago.
func TestADepartureDeletesTheVerdictRatherThanDistrustingIt(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
	b := &bench{}
	s := wiredHub(t, p, b)

	b.unplugged()
	s.cables.force()
	s.cables.refresh(time.Now())
	if _, _, _, present := s.identities.cachedVerdict(reflashPort); present {
		t.Fatal("the answer survived the board leaving the bench")
	}

	// Back it goes, now running gorb-os again after a reflash nobody told the hub about.
	b.set(goldSighting())
	s.cables.force()
	s.identifyCables(context.Background())

	rows, _ := s.roster(nil, time.Now())
	s.annotateOrb(rows)
	r := rowFrom(t, rows)
	if !r.OnCable {
		t.Error("the board came back as an orb and the hub still refuses to believe its own cable")
	}
}

// A host whose enumeration blips does not have an empty bench, it has a bench nobody
// re-read. Faking a departure there would throw away a paid verdict for nothing, and on a
// flaky hub would keep doing it.
func TestABrokenSweepFakesNoDeparture(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	b := &bench{}
	s := wiredHub(t, p, b)

	s.cables.force()
	b.broken(errors.New("usb: sysfs read failed"))
	s.cables.refresh(time.Now())

	if _, ok := s.contradicts(reflashPort); !ok {
		t.Fatal("an enumeration failure cost the hub a verdict it had already paid for")
	}
	b.broken(nil)
	s.identifyCables(context.Background())
	if p.count() != 0 {
		t.Errorf("the hub reset a lead it had no reason to re-ask: %v", p.ports())
	}
}

// The first sweep is a baseline, not a departure: a hub that starts with nothing sighted
// must not treat the empty bench as a board that left.
func TestTheFirstSweepLaysABaselineAndAsksNothingAboutAnEmptyBench(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	b := &bench{}
	s := launchHub(t, nil, p)
	s.cables.list = b.list
	s.cables.gone = s.forgetCablePorts

	s.identifyCables(context.Background())
	if p.count() != 0 {
		t.Errorf("an empty bench was interrogated: %v", p.ports())
	}
}

// A probe that never reached a chip is an absence of evidence. Filed as a verdict, it
// de-claims whatever is actually on the lead — so a flaky USB hub, or another process
// holding the port lock, would strip the cable off a healthy orb.
func TestAProbeThatNeverReachedTheChipClaimsNothing(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){}, err: errors.New("esprom: no sync")}
	b := &bench{}
	s := wiredHub(t, p, b)

	// Not even the hub's own Identify door may talk itself into a verdict this way.
	job, _, err := s.ops.start(context.Background(), reflashPort, opFlash, time.Minute)
	if err != nil {
		t.Fatalf("hold the lead: %v", err)
	}
	s.identities.doneFailed(reflashPort, "a flash is running on that port")
	s.ops.finish(job)

	if _, ok := s.contradicts(reflashPort); !ok {
		t.Fatal("the setup verdict vanished, so this test proves nothing")
	}

	// Now lose the verdict the honest way and let a failed probe take its place.
	b.unplugged()
	s.cables.force()
	s.cables.refresh(time.Now())
	b.set(goldSighting())
	s.cables.force()
	p.err = errors.New("esprom: handshake timed out")
	p.answer[reflashPort] = func() (esprom.OrbInfo, error) { return esprom.OrbInfo{}, p.err }
	s.identifyCables(context.Background())

	if _, ok := s.contradicts(reflashPort); ok {
		t.Fatal("a console that never answered is now a verdict about the board")
	}
	rows, _ := s.roster(nil, time.Now())
	s.annotateOrb(rows)
	if !rowFrom(t, rows).OnCable {
		t.Error("a wedged console stripped the cable off a board the host still sees as an orb")
	}
}

// The backoff must not be forever: a lead that came loose and reseated cleanly becomes
// askable again on its own, without a hub restart.
func TestAFailedProbeIsRetriedAfterABackoffNotForever(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	b := &bench{}
	s := wiredHub(t, p, b)
	b.unplugged()
	s.cables.force()
	s.cables.refresh(time.Now())
	p.err = errors.New("esprom: no sync")
	p.answer[reflashPort] = func() (esprom.OrbInfo, error) { return esprom.OrbInfo{}, p.err }

	b.set(goldSighting())
	s.cables.force()
	s.identifyCables(context.Background())
	first := p.count()
	s.identifyCables(context.Background())
	if p.count() != first {
		t.Errorf("a port the hub could not read was interrogated twice in a row (%d asks); the backoff is gone", p.count())
	}

	s.backdateFailure(reflashPort, failedRetry+time.Second)
	s.identifyCables(context.Background())
	if p.count() == first {
		t.Error("a wedged lead stayed wedged in the hub's memory past its backoff window")
	}
}

// backdateFailure ages a recorded probe failure so a test can stand at the edge of the
// backoff without sleeping through it.
func (s *Server) backdateFailure(port string, by time.Duration) {
	s.identities.mu.Lock()
	defer s.identities.mu.Unlock()
	if e, ok := s.identities.byPort[port]; ok {
		e.at = time.Now().Add(-by)
		s.identities.byPort[port] = e
	}
}

// A probe in flight when its cable is pulled must not keep that port's slot: the claim
// held the lead against a board that arrived seconds later.
func TestAPulledCableReleasesAnInFlightProbe(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	b := &bench{}
	s := wiredHub(t, p, b)
	if !s.identities.begin(reflashPort) {
		t.Fatal("setup: could not claim the port")
	}

	b.unplugged()
	s.cables.force()
	s.cables.refresh(time.Now())

	b.set(goldSighting())
	s.cables.force()
	s.identifyCables(context.Background())
	if p.count() != 1 {
		t.Fatalf("the returning lead was asked %d times; the abandoned claim still held its port", p.count())
	}
}

// Overhearing a PING is not an answer to "what firmware is on this chip". It is the one
// piece of free evidence that survives a reflash cleanly enough to keep asserting "orb",
// so it must not become the reason the hub stops asking.
func TestOverhearingAPingExcusesNoPaidProbe(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	b := &bench{}
	s := launchHub(t, goldSighting(), p)
	s.cables.list = b.list
	s.cables.gone = s.forgetCablePorts
	b.set(goldSighting())
	s.identities.doneCable(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", IsOrb: true, State: "ready"})

	s.identifyCables(context.Background())
	if p.count() != 1 {
		t.Fatalf("a board the hub only ever overheard was asked %d times; a PING is not a firmware reading", p.count())
	}
	if _, ok := s.contradicts(reflashPort); !ok {
		t.Error("the paid probe the free hearing failed to prevent never got filed")
	}
}

// A negative verdict must name the chip it is about. A bootloader that answered the orb
// door, denied being an orb, and printed no MAC describes nobody, so it cannot de-claim
// the board the host can name.
func TestANegativeVerdictAboutNoChipDeClaimsNoBoard(t *testing.T) {
	s := launchHub(t, goldBenchPorts(), &probeScript{})
	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		Project: "esp-idf"}, provenProbe)

	if _, ok := s.contradicts(reflashPort); ok {
		t.Fatal("a verdict with no chip in it contradicted a chip the host can name")
	}
	rows, _ := s.roster(nil, time.Now())
	s.annotateOrb(rows)
	if !rowFrom(t, rows).OnCable {
		t.Error("a stranger's descriptor stripped the cable off the board that belongs there")
	}
}

// The escalation tests: a plugged-in board reflashed outside the hub, its cable never
// disturbed. No physical event reveals it; the free HTTP dial does.

// goldIP is the address launchHub gives the board, and the one the liveness dial dials.
const goldIP = "192.0.2.31"

func wiredBenchHub(t *testing.T, p *probeScript, answer func() (esprom.OrbInfo, error)) (*Server, *bench) {
	t.Helper()
	b := &bench{}
	s := launchHub(t, goldSighting(), p)
	b.set(goldSighting())
	s.cables.list = b.list
	s.cables.gone = s.forgetCablePorts
	p.answer[reflashPort] = answer
	// A fresh paid answer saying it is an orb — the state that makes the hub decline to ask.
	s.identities.record(reflashPort, esprom.OrbInfo{Port: reflashPort,
		MAC: "44:bd:8d:78:26:18", IsOrb: true, Project: "gorb-os", Version: "v1.0.0"}, provenProbe)
	s.cables.refresh(time.Now())
	return s, b
}

// The requirement in one test: nothing was unplugged, nothing expired, the cache is full
// and fresh, and a wired board stopped answering its door. The hub asks.
func TestAWiredBoardThatGoesQuietIsAskedWithoutTheButton(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){}}
	s, _ := wiredBenchHub(t, p, foreignAnswer)

	if n := s.identifyCables(context.Background()); n != 0 {
		t.Fatalf("a fresh orb answer was re-paid on a clock (%d asks); the trigger must be the silence, not a timer", n)
	}
	s.noteWiredSilence(goldIP)
	if n := s.identifyCables(context.Background()); n != 1 {
		t.Fatalf("a wired board that stopped answering was asked %d times", n)
	}
	if _, ok := s.contradicts(reflashPort); !ok {
		t.Fatal("the paid probe the silence provoked filed no verdict")
	}
	rows, _ := s.roster(nil, time.Now())
	s.annotateOrb(rows)
	if rowFrom(t, rows).OnCable {
		t.Error("the board the escalation proved foreign is still drawn on a cable")
	}
}

// One silence episode buys one reset. A board whose network lead is loose fails its dial
// every ten seconds forever, and a flag that survived an examined port would reboot it
// every ten seconds forever in exchange for nothing.
func TestOneSilenceEpisodeBuysExactlyOneReset(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){}}
	s, _ := wiredBenchHub(t, p, orbAnswer)
	s.noteWiredSilence(goldIP)
	s.identifyCables(context.Background())
	s.identifyCables(context.Background())
	if got := p.count(); got != 1 {
		t.Fatalf("one silence cost %d resets; a wedged network lead must not be rebooted on the dial clock", got)
	}
}

// A blip must not de-claim anybody. The dial fails, the escalation asks, the bootloader
// answers gorb-os — and the board keeps its cable.
func TestASilenceThatTurnsOutToBeABlipCostsOnlyAReboot(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){}}
	s, _ := wiredBenchHub(t, p, orbAnswer)
	s.noteWiredSilence(goldIP)
	s.identifyCables(context.Background())
	if _, ok := s.contradicts(reflashPort); ok {
		t.Fatal("a board that answered as an orb contradicted its own lead")
	}
	rows, _ := s.roster(nil, time.Now())
	s.annotateOrb(rows)
	if !rowFrom(t, rows).OnCable {
		t.Error("a network blip stripped the cable off a healthy orb")
	}
}

// The escalation is scoped to boards on a lead. A wireless orb that goes quiet is a dead
// battery or a Wi-Fi problem, and there is no console to interrogate anyway.
func TestSilenceAboutAWirelessBoardAsksNothing(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){}}
	s, b := wiredBenchHub(t, p, foreignAnswer)
	b.unplugged()
	s.cables.force()
	s.cables.refresh(time.Now())

	s.noteWiredSilence(goldIP)
	if n := s.identifyCables(context.Background()); n != 0 {
		t.Fatalf("a quiet wireless board provoked %d resets", n)
	}
}

// Silence earns a question, never an assertion. No door may read it as a verdict.
func TestSilenceAloneClaimsNothingAboutABoard(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){}}
	s, _ := wiredBenchHub(t, p, foreignAnswer)
	s.noteWiredSilence(goldIP)
	if _, ok := s.contradicts(reflashPort); ok {
		t.Fatal("the hub talked itself out of an orb on nothing but a missing HTTP answer")
	}
	rows, _ := s.roster(nil, time.Now())
	s.annotateOrb(rows)
	if !rowFrom(t, rows).OnCable {
		t.Error("a silent dial de-claimed a board the probe never asked")
	}
}

// A port the host cannot attribute to an Espressif chip is refused by the sweep, and there
// is no longer any door that overrides that refusal. This is the safety property, not a gap:
// EnterDownloadMode pulses DTR/RTS, and doing that to someone's Arduino or FTDI breakout
// resets a device the operator never asked about. What the hub cannot name, it does not
// touch — by click, by POST, or by schedule.
func TestAnUnattributablePortIsNeitherResetNorOffered(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){"/dev/ttyUSB9": foreignAnswer}}
	s := launchHub(t, []cableSighting{{port: "/dev/ttyUSB9"}}, p)
	s.noteWiredSilence(goldIP)

	if n := s.identifyCables(context.Background()); n != 0 {
		t.Fatalf("the sweep reset an unattributable port %d times", n)
	}
	if _, ok := s.contradicts("/dev/ttyUSB9"); ok {
		t.Error("a port the hub refused to open filed a verdict about the board on it")
	}
	// Nothing may be left to click instead: an override for the unattributable case is the
	// exact hazard the gate exists to prevent, and a button is only a gate you can ignore.
	for _, f := range []string{"main.go", "web/index.html", "web/app.js"} {
		if strings.Contains(readFile(t, f), "/api/ports/probe") {
			t.Errorf("%s still carries a probe door, so the gate can be stepped around", f)
		}
	}
}

// goldIP is the address launchHub gives the board, and the one the liveness dial dials.
// A board in its downloader enumerates as an Espressif chip and not as an orb. It is ours,
// it takes the reset, and the operator must never have to click to learn what it runs.
func TestABoardInDownloadModeIsIdentifiedUnprompted(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){
		"/dev/ttyACM3": func() (esprom.OrbInfo, error) {
			return esprom.OrbInfo{Port: "/dev/ttyACM3", MAC: "44:bd:8d:78:26:18",
				Project: "esp-idf"}, nil
		}}}
	s := launchHub(t, []cableSighting{{port: "/dev/ttyACM3", espressif: true, mac: macHall}}, p)
	s.identifyCables(context.Background())
	if p.count() != 1 {
		t.Fatalf("a download-mode Espressif board was asked %d times", p.count())
	}
	if _, ok := s.contradicts("/dev/ttyACM3"); !ok {
		t.Error("the probe filed no usable verdict")
	}
}
