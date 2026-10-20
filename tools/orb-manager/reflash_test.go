package main

import (
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
)

// The bench these tests describe is the one a reflash leaves behind: a board this hub
// named, addressed and flashed, then flashed again with firmware that is not gorb-os.
// Nothing the host reads for free about that lead changes when it happens. The descriptor
// serial IS the eFuse base MAC, so the MAC survives; the product id is whatever USB
// function the running image exposes, so a stock Espressif CDC demo keeps matching. The
// hub therefore kept saying three true-sounding things at once — the row is on a cable,
// the chip has been an orb, the board may not be forgotten — and the operator was left
// with a tile that could not be forgotten and a New Orb picker that would not take it.
// The paid verdict is the only thing in the system that knows better, and these tests pin
// the rule that it outranks the rest.

const reflashPort = "/dev/ttyACM0"

// goldBench scripts the cable sweep and the identity cache. verdict is what the hub
// believes is running on the lead, and proven is how it learned that, because the
// distinction is the entire rule: a paid answer contradicts and a free silence does not.
func goldBench(t *testing.T, verdict esprom.OrbInfo, proven string) *Server {
	t.Helper()
	s := newHub()
	s.statePath = filepath.Join(t.TempDir(), "state.json")
	s.fleet = orbfleet.New(orbfleet.Config{})
	// The address book the onboarding left behind: an address, a firmware stamp, a
	// name, and the remembered mark a flash writes. Every one of these is what
	// storeProvesOrb reads as "this chip has been an orb", which is precisely the
	// history a reflash must be allowed to overrule.
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
	s.cables.list = func() ([]cableSighting, error) {
		return []cableSighting{{port: reflashPort, isOrb: true, mac: macHall}}, nil
	}
	s.identities.usbMAC = func(string) string { return "44:bd:8d:78:26:18" }
	s.identities.record(reflashPort, verdict, proven)
	return s
}

func rowFor(t *testing.T, s *Server, now time.Time) orbRow {
	t.Helper()
	rows, _ := s.roster(nil, now)
	for _, r := range rows {
		if r.MAC == macHall {
			return r
		}
	}
	t.Fatalf("no row carried macHall in %d rows", len(rows))
	return orbRow{}
}

// The reported symptom, stated as an assertion: a board running firmware the hub can prove
// is not gorb-os must not be drawn as an orb on a lead.
func TestAContradictedCableIsNotAnOrbOnACable(t *testing.T) {
	now := time.Now()
	s := goldBench(t, esprom.OrbInfo{
		Port: reflashPort, MAC: "44:bd:8d:78:26:18", IsOrb: false,
		Project: "esp32-s3-touch-amoled-brookesia", Version: "unknown",
		Reason: "identified as not an orb",
	}, provenProbe)

	if _, ok := s.contradicts(reflashPort); !ok {
		t.Fatal("the hub held a paid not-an-orb verdict and still believed the descriptor")
	}
	r := rowFor(t, s, now)
	if r.OnCable {
		t.Error("a board proven to run foreign firmware was still drawn as on a cable")
	}
	if len(r.Ports) != 0 {
		t.Errorf("ports = %v: the lead must not be claimed as an orb's while it is contradicted", r.Ports)
	}
	// The history survives. The operator named this board, and the card that shows it
	// is the card that offers Forget; deleting the row over a firmware question would
	// trade one dead end for a vanished board.
	rows, _ := s.roster(nil, now)
	s.annotateOrb(rows)
	f := rowFrom(t, rows)
	if f.Name != "Gold" {
		t.Errorf("name = %q: the address book's word about the chip was thrown away", f.Name)
	}
	if !f.Orb {
		t.Error("a contradicted board was dropped off the fleet list, so the operator cannot select it to forget it")
	}
}

// The other half of the report: the same board has to be forgettable by the plain door,
// because nothing can rebuild its row — the thing that would rebuild it is the orb that
// is no longer on the chip.
func TestAContradictedBoardIsPlainForgettable(t *testing.T) {
	now := time.Now()
	s := goldBench(t, esprom.OrbInfo{
		Port: reflashPort, MAC: "44:bd:8d:78:26:18", IsOrb: false,
		Project: "esp32-s3-touch-amoled-brookesia", Reason: "identified as not an orb",
	}, provenProbe)
	// The row keeps the address the board held as an orb, so the gate asks about it
	// before it offers to delete anything. The board runs a demo and answers nothing.
	// File the dead verdict the ten-second sweeper would have landed, because a render
	// path refuses to guess without one and that refusal is correct, not the bug.
	s.live.record("192.0.2.31", false, now)

	if p := s.cableOf(macHall, now); p != "" {
		t.Errorf("cableOf = %q: the Forget gate is about to refuse on the strength of a descriptor", p)
	}
	rows, _ := s.roster(nil, now)
	s.annotateForget(rows, now)
	r := rowFrom(t, rows)
	if !r.Forgettable {
		t.Errorf("not forgettable, and the card said: %s", r.ForgetWhy)
	}
}

// rowFrom is rowFor for a test that annotated the set it already holds: re-running
// roster would hand back an unannotated copy and the assertions below would read the
// hub's silence as a verdict.
func rowFrom(t *testing.T, rows []orbRow) orbRow {
	t.Helper()
	for _, r := range rows {
		if r.MAC == macHall {
			return r
		}
	}
	t.Fatalf("no row carried macHall in %d rows", len(rows))
	return orbRow{}
}

// The control that keeps the rule from becoming "deny the descriptor whenever anything is
// cached". A board that answered as an orb is on a cable, and is not plain forgettable,
// exactly as before.
func TestAGorbOsVerdictStillClaimsTheCable(t *testing.T) {
	now := time.Now()
	s := goldBench(t, esprom.OrbInfo{
		Port: reflashPort, MAC: "44:bd:8d:78:26:18", IsOrb: true,
		Project: "gorb-os", Version: "v1.0.0",
	}, provenProbe)

	if _, ok := s.contradicts(reflashPort); ok {
		t.Fatal("an orb that answered for itself contradicted its own cable")
	}
	r := rowFor(t, s, now)
	if !r.OnCable {
		t.Error("a board that answered as an orb lost its cable")
	}
	rows, _ := s.roster(nil, now)
	s.annotateForget(rows, now)
	f := rowFrom(t, rows)
	if f.Forgettable {
		t.Error("a live orb on a lead was marked plain forgettable")
	}
	if !strings.Contains(f.ForgetWhy, reflashPort) {
		t.Errorf("forgetWhy = %q: a refused orb must still be told where it is", f.ForgetWhy)
	}
}

// The dangerous direction: a PING that got no answer. Silence means the hub did not hear
// it, which is equally true of an orb that is asleep, boot-looping, or sitting in its
// downloader. Reading that as "not an orb" would strip the cable off a healthy board and
// hand out a Forget button on an orb that is fine.
func TestSilenceOnAPingNeverContradictsAPort(t *testing.T) {
	now := time.Now()
	s := goldBench(t, esprom.OrbInfo{
		Port: reflashPort, MAC: "44:bd:8d:78:26:18", IsOrb: false,
	}, provenCable)

	if _, ok := s.contradicts(reflashPort); ok {
		t.Fatal("a cable proof that heard nothing contradicted the descriptor")
	}
	r := rowFor(t, s, now)
	if !r.OnCable {
		t.Error("a board the hub merely failed to hear lost its cable")
	}
}

// A contradicted sighting keeps the port and its MAC. The lead is how the board gets
// flashed back into an orb, so downgrading the claim must not lose the address of it.
func TestAContradictedSightingKeepsItsPort(t *testing.T) {
	s := goldBench(t, esprom.OrbInfo{
		Port: reflashPort, MAC: "44:bd:8d:78:26:18", IsOrb: false, Project: "some-demo",
	}, provenFlash)

	seen := s.cableSights(time.Now())
	if len(seen) != 1 {
		t.Fatalf("%d sightings; the port disappeared rather than being downgraded", len(seen))
	}
	if seen[0].isOrb {
		t.Error("the sighting still claims an orb")
	}
	if seen[0].port != reflashPort || seen[0].mac != macHall {
		t.Errorf("port=%q mac=%q: the picker needs both to offer the lead", seen[0].port, seen[0].mac)
	}
}

// contradict must not write through the slice the cable cache hands out, because that
// slice is what the next sweep compares its plug digest against. A rewrite there means a
// board that is physically unchanged reads as unplugged-and-replugged on every render.
func TestContradictDoesNotWriteThroughTheCableCache(t *testing.T) {
	s := goldBench(t, esprom.OrbInfo{
		Port: reflashPort, MAC: "44:bd:8d:78:26:18", IsOrb: false, Project: "some-demo",
	}, provenProbe)

	raw, _ := s.cables.refresh(time.Now())
	if len(raw) != 1 || !raw[0].isOrb {
		t.Fatal("setup: the cache no longer holds the descriptor's own answer")
	}
	down := s.contradict(raw)
	if down[0].isOrb {
		t.Error("the contradicted view still claims an orb")
	}
	if !raw[0].isOrb {
		t.Error("contradict wrote through into the cached sweep")
	}
	// And the common path costs nothing: nothing to downgrade returns the same slice.
	if got := s.contradict([]cableSighting{{port: "/dev/ttyACM7", isOrb: false, mac: "aabbccddeeff"}}); len(got) != 1 {
		t.Error("a sighting needing no downgrade was dropped")
	}
}

// The rule at the door that writes it, pinned at the call site the way the suite pins the
// other writes a suite without a chip cannot reach: the flash must record what it
// installed, and must not stamp a foreign chip as an orb the hub has remembered.
func TestAFlashRecordsWhatItActuallyInstalled(t *testing.T) {
	src := readFile(t, "flash.go")
	if !strings.Contains(src, "s.identities.doneFlash(req.Port") {
		t.Error("a flash no longer records the firmware it wrote, so only a reboot can tell what is on the chip")
	}
	if !strings.Contains(src, "IsOrb:   !b.Foreign") {
		t.Error("the flash record stopped distinguishing gorb-os from an image the operator named by hand")
	}
	// The orb-only stamping must sit inside the not-foreign branch.
	i := strings.Index(src, "if b.Foreign {")
	j := strings.Index(src, "s.fleet.MarkRemembered")
	if i < 0 || j < 0 || i > j {
		t.Error("a foreign flash still marks the chip as a remembered orb, which is what shut the New Orb door")
	}
	k := strings.Index(src, "s.markCable")
	if i < 0 || k < 0 || i > k {
		t.Error("a foreign flash still files the chip as a cable sighting")
	}
	if !strings.Contains(src, `req.Name != "" && !b.Foreign`) {
		t.Error("the hub still asks a board running a demo to acknowledge an orb name")
	}
}

// The same precedence the hub already states upward — a chip that answered for itself
// outranks the address book — has to hold downward, and it has to be stated in the entry
// the page reads. Pinned at the block because no test without a chip can reach it.
func TestAPortEntryLetsANegativeVerdictClearOrbSeen(t *testing.T) {
	src := readFile(t, "identity.go")
	i := strings.Index(src, `if !info.IsOrb && ent.proven != provenCable {`)
	if i < 0 {
		t.Fatal("the port entry no longer reads a negative verdict at all")
	}
	block := src[i:]
	if j := strings.Index(block, "\n\t\t\t}"); j > 0 {
		block = block[:j]
	}
	for _, want := range []string{`e["orbSeen"] = false`, `e["notOrb"] = true`, `e["inFleet"] = false`} {
		if !strings.Contains(block, want) {
			t.Errorf("the negative-verdict block lost %s; the board falls between the fleet row and the New Orb door without it", want)
		}
	}
	if !strings.Contains(src, `provenFlash = "flash"`) {
		t.Error("the flash verdict lost its own provenance and would be reported as a reboot it did not pay for")
	}
}
