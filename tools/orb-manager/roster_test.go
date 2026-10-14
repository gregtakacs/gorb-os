package main

import (
	"context"
	"errors"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
)

// The merge, tested as one list. A fake port sweep stands in for the host: the thing
// under test is what the roster does with a sighting, not whether sysfs was readable,
// and no test here opens a port or resets a chip. The mDNS half comes through the real
// browser fed by Ingest, so no row exists because a test declared one.

// fakeSweep is a plug set a test moves by hand, plus the enumeration failure it can be
// made to report. It satisfies the cableCache seam exactly as hostCableSweep does. It
// holds the hub so a set or a fail can expire the sweep cache: the fake stands in for a
// host whose plug state just changed, and a real change must be visible to the very
// next read rather than at the mercy of a ttl.
type fakeSweep struct {
	s     *Server
	mu    sync.Mutex
	seen  []cableSighting
	err   error
	calls int
}

func (f *fakeSweep) list() ([]cableSighting, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls++
	return f.seen, f.err
}

// stale forces the next read to re-sweep, which is what "the bench just changed" means.
func (f *fakeSweep) stale() {
	f.s.cables.mu.Lock()
	f.s.cables.at = time.Time{}
	f.s.cables.mu.Unlock()
}

func (f *fakeSweep) set(seen ...cableSighting) {
	f.mu.Lock()
	f.seen, f.err = seen, nil
	f.mu.Unlock()
	// Outside f.mu: list runs under the cableCache lock, and a set that held f.mu
	// while taking that one would invert the order against a sweep mid-call.
	f.stale()
}

func (f *fakeSweep) fail(why string) {
	f.mu.Lock()
	f.seen, f.err = nil, errors.New(why)
	f.mu.Unlock()
	f.stale()
}

// plugCables wires a hub to a fake sweep and returns it.
func plugCables(s *Server, wake func()) *fakeSweep {
	f := &fakeSweep{s: s}
	s.enableCables(f.list, wake)
	return f
}

func orbAt(port, mac string) cableSighting { return cableSighting{port: port, isOrb: true, mac: mac} }

func unattributed(port string) cableSighting { return cableSighting{port: port, isOrb: true} }

func strangerAt(port string) cableSighting {
	return cableSighting{port: port, isOrb: false}
}

// The feature named: one board, two doors, one row.
func TestRosterMergesOneBoardOnBothDoors(t *testing.T) {
	s := newHub()
	s.browse = filled(t, candidate("44bd8d782618", "192.168.2.34"))
	f := plugCables(s, nil)
	f.set(orbAt("/dev/ttyACM0", "44bd8d782618"))
	got := getDiscovered(t, s)
	if len(got.Data.Orbs) != 1 {
		t.Fatalf("one board on both doors made %d rows: %+v", len(got.Data.Orbs), got.Data.Orbs)
	}
	r := got.Data.Orbs[0]
	if !r.OnCable {
		t.Error("the merged row does not know it is on a cable")
	}
	if strings.Join(r.Via, ",") != "lan,usb" {
		t.Errorf("via = %v, want lan,usb", r.Via)
	}
	if strings.Join(r.Ports, ",") != "/dev/ttyACM0" {
		t.Errorf("ports = %v", r.Ports)
	}
	if r.Identity != "descriptor" {
		t.Errorf("identity = %q: a descriptor MAC is not a chip-read confirmation", r.Identity)
	}
	if r.IP != "192.168.2.34" || r.Dialable {
		t.Errorf("the cable half leaked an address or a dial: ip=%q dialable=%v", r.IP, r.Dialable)
	}
}

// The row the old list could not make at all: nothing announced, so the card was empty,
// so the board sitting plugged into the machine did not exist.
func TestRosterListsCableOnlyBoardWithNoAddress(t *testing.T) {
	s := newHub()
	s.browse = filled(t)
	f := plugCables(s, nil)
	f.set(orbAt("/dev/ttyACM0", "44bd8d608088"))
	got := getDiscovered(t, s)
	if len(got.Data.Orbs) != 1 {
		t.Fatalf("the cable-only board never reached the list: %+v", got.Data.Orbs)
	}
	r := got.Data.Orbs[0]
	if r.MAC != "44bd8d608088" || r.IP != "" || r.Dialable {
		t.Errorf("a cable invented an address or made itself clickable: %+v", r)
	}
	if r.Instance != "orb-608088" {
		t.Errorf("instance = %q, want the device's own rule orb-608088", r.Instance)
	}
	if !strings.Contains(r.Refusal, "no address") {
		t.Errorf("the row is not clickable but does not say why: %q", r.Refusal)
	}
	if got.Data.CableError != "" {
		t.Errorf("cableError = %q on a good sweep", got.Data.CableError)
	}
	// Hybrid persistence: a descriptor sighting is a row, never a fleet store row.
	if _, ok := s.fleet.ByMAC("44bd8d608088"); ok {
		t.Error("a cable sighting reached the fleet store without a probe or a packet")
	}
}

// The bug the merge exists to kill, pinned directly: the hub could not bind a multicast
// socket, and the card answered "no orbs" while one was plugged into the machine.
func TestRosterShowsCablesWithNoListener(t *testing.T) {
	s := newHub()
	s.setDiscovery(nil, "no interface could join 224.0.0.251")
	f := plugCables(s, nil)
	f.set(orbAt("/dev/ttyACM0", "44bd8d608088"))
	got := getDiscovered(t, s)
	if got.Data.Listening || got.Data.Error == "" {
		t.Errorf("the card still claims a listener it does not have: %+v", got.Data)
	}
	if len(got.Data.Orbs) != 1 || got.Data.Orbs[0].MAC != "44bd8d608088" {
		t.Fatalf("a deaf listener hid the cable: %+v", got.Data.Orbs)
	}
	if got.Data.Orbs == nil {
		t.Error("orbs must be a list, not absent")
	}
}

// Two ttys, one board: the merge key is the MAC the host read, not the plug count.
func TestRosterMergesTwoPortsIntoOneBoard(t *testing.T) {
	s := newHub()
	s.browse = filled(t)
	f := plugCables(s, nil)
	f.set(orbAt("/dev/ttyACM7", "44bd8d608088"), orbAt("/dev/ttyACM3", "44bd8d608088"))
	got := getDiscovered(t, s)
	if len(got.Data.Orbs) != 1 {
		t.Fatalf("one MAC on two ports made %d rows", len(got.Data.Orbs))
	}
	if got.Data.Orbs[0].Identity != "descriptor" {
		t.Errorf("identity = %q, want descriptor", got.Data.Orbs[0].Identity)
	}
	if strings.Join(got.Data.Orbs[0].Ports, ",") != "/dev/ttyACM3,/dev/ttyACM7" {
		t.Errorf("ports = %v: two ttys on one board must sort, not follow enumeration", got.Data.Orbs[0].Ports)
	}
}

// A VID:PID that says orb and a descriptor that says nothing is a row with a tty path
// and no MAC. It must not merge with the other unidentified cable on the bench, and it
// must not take a name.
func TestRosterKeepsUnattributedCablesApart(t *testing.T) {
	s := newHub()
	s.browse = filled(t)
	f := plugCables(s, nil)
	f.set(unattributed("/dev/ttyACM1"), unattributed("/dev/ttyACM2"), strangerAt("/dev/ttyUSB0"))
	got := getDiscovered(t, s)
	if len(got.Data.Orbs) != 2 {
		t.Fatalf("want two unattributed rows and no UART-bridge stranger, got %+v", got.Data.Orbs)
	}
	keys := []string{got.Data.Orbs[0].Key, got.Data.Orbs[1].Key}
	if keys[0] == keys[1] {
		t.Errorf("two unknown cables collapsed onto one key %q", keys[0])
	}
	for _, r := range got.Data.Orbs {
		if r.MAC != "" {
			t.Errorf("a row with nothing to read grew a mac anyway: %+v", r)
		}
		if r.Identity != "unknown" {
			t.Errorf("identity = %q, want unknown", r.Identity)
		}
		if !strings.Contains(r.Refusal, "identify") {
			t.Errorf("the refusal does not name the way out: %q", r.Refusal)
		}
	}
}

// The store's word about an orb the cable currently sees wins over "no address": a board
// flashed five minutes ago is on a lead AND has an address, and a card that showed only
// the lead would tell the operator the wrong thing about their own fleet. The address is
// still not clickable unless the grant says so.
func TestRosterFoldsStoreAddressIntoCableRow(t *testing.T) {
	s := newHub()
	s.browse = filled(t)
	s.fleet.Restore([]orbfleet.Orb{{
		MAC: "44bd8d608088", Name: "Nursery", IP: "192.168.2.50", Port: 80,
		Cable: true, LastSeen: time.Now(),
	}})
	f := plugCables(s, nil)
	f.set(orbAt("/dev/ttyACM0", "44bd8d608088"))
	got := getDiscovered(t, s)
	if len(got.Data.Orbs) != 1 {
		t.Fatalf("the store row and the cable row did not merge: %+v", got.Data.Orbs)
	}
	r := got.Data.Orbs[0]
	if r.Name != "Nursery" || !r.OnCable || !r.Cable {
		t.Errorf("the merged row lost a half: %+v", r)
	}
	if r.IP != "192.168.2.50" || r.Dialable {
		t.Errorf("ip=%q dialable=%v: the cable row must carry the store address and still refuse to dial it ungranted", r.IP, r.Dialable)
	}
	if r.Identity != "confirmed" {
		t.Errorf("identity = %q: the store says this MAC was read off the chip", r.Identity)
	}
	if strings.Join(r.Via, ",") != "usb,lan" {
		t.Errorf("via = %v, want both paths with usb first", r.Via)
	}
}

// The version digest is what makes the stream worth its wire, and this is the one
// promise the merge adds: a plug-in is news. An announce that changed nothing is not.
func TestRosterVersionMovesOnPlugAndUnplug(t *testing.T) {
	s := newHub()
	s.browse = filled(t, candidate("44bd8d782618", "192.168.2.34"))
	f := plugCables(s, nil)
	_, v0 := s.orbCard(time.Now())
	f.set(orbAt("/dev/ttyACM0", "44bd8d782618"))
	_, v1 := s.orbCard(time.Now())
	if v1 == v0 {
		t.Fatal("plugging an orb in did not move the digest, so the stream would never push it")
	}
	_, v2 := s.orbCard(time.Now())
	if v2 != v1 {
		t.Error("an unchanged bench moved the digest between two reads")
	}
	f.set()
	_, v3 := s.orbCard(time.Now())
	if v3 != v0 {
		t.Error("unplugging did not return the digest to the cable-free value")
	}
}

// The push, end to end: a plug-in with no mDNS packet behind it reaches an open stream.
// This is the case the old wake-on-check-in could not cover — a factory-fresh board has
// no Wi-Fi credentials and will never announce anything. The watch loop is the thing
// under test, so it runs for real on a shrunk interval rather than a wake being fired by
// hand: a test that called wakeOrbs itself would pass on a hub that could never wake.
func TestStreamPushesOnPlugChange(t *testing.T) {
	saved := cableSweepTTL
	cableSweepTTL = 20 * time.Millisecond
	defer func() { cableSweepTTL = saved }()
	f := openFeed(t)
	first := f.frame(t)
	if len(first.Data.Orbs) != 0 {
		t.Fatalf("the opening frame listed %d rows", len(first.Data.Orbs))
	}
	sw := plugCables(f.s, f.s.wakeOrbs)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go f.s.watchCables(ctx)
	// Enabling a sweep must not, by itself, count as a plug change: the first sweep
	// only records, or every hub startup with a cable in it would push a free frame.
	// The bench starts empty, so give the watch one tick to record it.
	time.Sleep(80 * time.Millisecond)
	f.staysQuiet(t, 100*time.Millisecond)
	sw.set(orbAt("/dev/ttyACM0", "44bd8d608088"))
	got := f.frame(t)
	if len(got.Data.Orbs) != 1 || got.Data.Orbs[0].MAC != "44bd8d608088" {
		t.Fatalf("the plug-in never reached the open card: %+v", got.Data.Orbs)
	}
	if !got.Data.Orbs[0].OnCable {
		t.Error("the pushed row does not know it is on a cable")
	}
}

// A failed enumeration is not an empty bench. The card keeps the last plug set it read
// and says the sweep broke — the alternative is that ls fails once and the operator
// watches their orbs vanish.
func TestRosterSweepFailureKeepsTheLastBench(t *testing.T) {
	s := newHub()
	s.browse = filled(t)
	f := plugCables(s, nil)
	f.set(orbAt("/dev/ttyACM0", "44bd8d608088"))
	if got := getDiscovered(t, s); len(got.Data.Orbs) != 1 {
		t.Fatalf("the bench itself did not list: %+v", got.Data.Orbs)
	}
	f.fail("udev said no")
	got := getDiscovered(t, s)
	if len(got.Data.Orbs) != 1 {
		t.Errorf("a failed sweep reported an empty bench: %+v", got.Data.Orbs)
	}
	if !strings.Contains(got.Data.CableError, "udev") {
		t.Errorf("cableError = %q: the card must carry the reason, not the silence", got.Data.CableError)
	}
}

// Naming a cable-only board is the refusal the merge has to earn the right to word
// correctly. "Never announced" is what the old message claimed about a board the hub is
// holding in its hand; the new one names the plug and still refuses, because hybrid
// persistence means no store row and no device to write to.
func TestNamingACableOnlyOrbRefusesByNameOfTheCable(t *testing.T) {
	s := newHub()
	s.token = "tk"
	s.browse = filled(t)
	f := plugCables(s, nil)
	f.set(orbAt("/dev/ttyACM0", "44bd8d608088"))
	rec := postFleet(t, s, "/api/fleet/set",
		map[string]string{"mac": "44:BD:8D:60:80:88", "name": "Nursery"},
		map[string]string{"X-Orb-Token": "tk"})
	body := rec.Body.String()
	if rec.Code == http.StatusOK {
		t.Fatalf("naming an address-less cable-only board succeeded: %s", body)
	}
	if !strings.Contains(body, "ttyACM0") {
		t.Errorf("the refusal does not name the cable the board is on: %s", body)
	}
	if strings.Contains(body, "never announced") {
		t.Errorf("the refusal still claims the board was never seen: %s", body)
	}
	if _, ok := s.fleet.ByMAC("44bd8d608088"); ok {
		t.Error("a refused rename still created a fleet row")
	}
}

// The merge is the list's contract now, and the docs and the UI are where the next
// reader and the next operator learn it. A README that still describes a LAN-only card
// is the doc equivalent of the early-return bug this whole change exists to kill: the
// feature is there and the paper says it is not.
func TestRosterIsDocumentedAndBadged(t *testing.T) {
	readme := readUpFile(t, "README.md")
	for _, want := range []string{
		"one merged list",
		"never an address",
		"never reaches `state.json`",
		"`cable` (chip-confirmed", // the two cable facts stay named apart, in the doc too
	} {
		if !strings.Contains(readme, want) {
			t.Errorf("README no longer explains the merged list (%q missing)", want)
		}
	}
	plan := readUpFile(t, "..", "..", "docs", "orb-manager-plan.md")
	if !strings.Contains(plan, "one list, both doors") {
		t.Error(`docs/orb-manager-plan.md lost its "one list, both doors" spec`)
	}
	page := readWebFile(t, "index.html")
	if !strings.Contains(page, "<h3>Orbs</h3>") {
		t.Error("the card is no longer titled Orbs; it lists more than the LAN now")
	}
	js := readWebFile(t, "app.js")
	for _, want := range []string{
		"function viaBadge", // the merge has a widget, not just fields
		"o.key",
		"dataset.mac", // a cable-only row cannot be gated on a mac it does not have
		"Identify",
	} {
		if !strings.Contains(js, want) {
			t.Errorf("app.js no longer contains %q", want)
		}
	}
}

// readUpFile walks out of the package dir — the README lives one level up, the plan two.
func readUpFile(t *testing.T, parts ...string) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(parts...))
	if err != nil {
		t.Fatalf("read %v: %v", parts, err)
	}
	return string(b)
}
