package main

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// The rule these pin: a board the network can see does not get reset to learn what it is
// already saying. The launch sweep and the re-plug sweep both paid a reboot for every
// attributable lead, because the identity cache is memory and a hub restart empties it —
// so a healthy orb on a lead lost eight seconds of display every time the hub restarted,
// on a question its own /api/v1/status door answers for free.
//
// The exemptions are asymmetric on purpose, and the failure directions are separate
// tests: an orb that answers must not be reset, a board that does not answer must still
// be, and a door that answers as somebody else's chip proves nothing about this cable.

// dialScript is the bench's LAN side: the answer the hub gets when it dials a row's
// address, and how many times it asked.
type dialScript struct {
	dials  []string
	status *orbnet.Status
	err    error
}

func (d *dialScript) dial(ip string) (*orbnet.Status, error) {
	d.dials = append(d.dials, ip)
	return d.status, d.err
}

func (d *dialScript) count() int { return len(d.dials) }

func orbStatusAnswer() *orbnet.Status {
	return &orbnet.Status{MAC: "44:bd:8d:78:26:18", FW: "v1.4.0", State: "connected",
		Name: "Gold", IDF: "v5.4", Slot: "a", IP: "192.0.2.31"}
}

// lanHub is the operator's bench at the moment the hub restarts: Gold on a lead, Gold in
// the address book at an address, and the paid-probe script ready to record a reset.
// lanHub is the operator's bench at the moment the hub restarts: Gold on a lead, Gold in
// the address book at an address, and the paid-probe script ready to record a reset.
func lanHub(t *testing.T, p *probeScript, d *dialScript) *Server {
	t.Helper()
	s := launchHub(t, goldSighting(), p)
	s.lanDial = d.dial
	return s
}

// The other side of the rule. A board that is not answering its door is the case a
// bootloader console exists for: silence covers a boot-loop, a loose network cable, and a
// reflash out of gorb-os, and only the reset tells those apart.
func TestASilentBoardOnALeadStillPaysTheReset(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
	d := &dialScript{err: errors.New("orbnet: no answer")}
	s := lanHub(t, p, d)

	s.probeCablesAtLaunch(context.Background())

	if d.count() != 1 {
		t.Fatalf("the gate dialed %v times; a row with an address is worth one ask", d.count())
	}
	if p.count() != 1 {
		t.Fatalf("the sweep reset %v ports; a silent board is what the reset is for", p.ports())
	}
	if _, _, proven, _ := s.identities.cachedVerdict(reflashPort); proven != provenProbe {
		t.Errorf("the paid answer was filed as %q", proven)
	}
}

// Two boards, one address. Whoever answers speaks for itself and for nobody else, and a
// hub that filed a stranger's orb as this cable's identity would both report the wrong
// version and hide the board from the one question that can name it.
func TestADoorNamingAnotherChipBuysThisCableNothing(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	other := orbStatusAnswer()
	other.MAC = "44:bd:8d:00:00:01"
	d := &dialScript{status: other}
	s := lanHub(t, p, d)

	s.probeCablesAtLaunch(context.Background())

	if p.count() != 1 {
		t.Fatalf("the sweep reset %v ports; a door answering as a different chip is no answer", p.ports())
	}
	if _, _, proven, _ := s.identities.cachedVerdict(reflashPort); proven == provenLan {
		t.Error("a stranger's status payload was filed as this cable's identity")
	}
}

// The hub does not go looking for addresses. A chip no row names — a board flashed on
// another bench and brought here on a lead, or a board that is no orb at all — has no
// claim to be believed about, and dialing into the dark is how a printer answering on :80
// ends up exempting something from a question.
func TestAChipNoRowNamesIsNotDialedAtAll(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: foreignAnswer}}
	d := &dialScript{status: orbStatusAnswer()}
	s := launchHub(t, goldSighting(), p)
	s.lanDial = d.dial
	if _, err := s.fleet.Forget(macHall); err != nil {
		t.Fatalf("forget the row: %v", err)
	}

	s.probeCablesAtLaunch(context.Background())

	if d.count() != 0 {
		t.Errorf("the gate dialed %v for a chip no row names; there is no address to trust", d.dials)
	}
	if p.count() != 1 {
		t.Errorf("an unknown chip reset %v ports; it is the paid case by definition", p.ports())
	}
}

// The hub has no route to the firmware's own SoftAP subnet, so a dial there measures the
// routing table and not the board. An orb sitting in AP mode is a board with no network to
// rely on — the paid case the operator described, not an exemption.
func TestASoftAPAddressIsNeverDialledForAnExemption(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
	d := &dialScript{status: orbStatusAnswer()}
	s := lanHub(t, p, d)
	if _, err := s.fleet.Observe(orbfleet.Observation{MAC: macHall, Instance: "orb-782618",
		IP: "192.168.4.21", Port: 80, At: time.Now()}); err != nil {
		t.Fatalf("move the row onto the SoftAP subnet: %v", err)
	}

	s.probeCablesAtLaunch(context.Background())

	if d.count() != 0 {
		t.Errorf("the gate dialed %v; 192.168.4.x is unroutable from the hub by design", d.dials)
	}
	if p.count() != 1 {
		t.Errorf("an AP-isolated orb reset %v ports; it should pay exactly one", p.ports())
	}
}

// After the launch sweep the hub must hold an orb answer, and the lead must never have
// been pulsed.
func TestALaunchSweepAsksTheNetworkBeforeItResetsAVisibleOrb(t *testing.T) {
	p := &probeScript{}
	d := &dialScript{status: orbStatusAnswer()}
	s := lanHub(t, p, d)

	s.probeCablesAtLaunch(context.Background())

	if d.count() != 1 {
		t.Fatalf("the sweep dialed %v; the one lead owed a question is one dial", d.dials)
	}
	if p.count() != 0 {
		t.Errorf("the sweep reset %v; an orb answering its door over the LAN owes no reboot", p.ports())
	}
	info, _, proven, present := s.identities.cachedVerdict(reflashPort)
	if !present {
		t.Fatal("the LAN answer was never filed, so the next pass asks again")
	}
	if proven != provenLan {
		t.Errorf("the answer was filed as %q, not as a door answer", proven)
	}
	if !info.IsOrb || info.Version != "v1.4.0" || info.MAC == "" {
		t.Errorf("the filed answer lost what the board quoted: %+v", info)
	}
}

// The same board pulled and put back — the operator's own "look again". The departure
// voids the answer, the sweep re-asks, and the free answer still settles it.
func TestARepluggedOrbTheNetworkCanSeeIsStillNotReset(t *testing.T) {
	p := &probeScript{}
	d := &dialScript{status: orbStatusAnswer()}
	s := lanHub(t, p, d)
	s.identities.drop(reflashPort)

	if ask, _ := s.cableNeedsProbe(goldSighting()[0]); !ask {
		t.Fatal("setup: a departed board must read as owed, which is the rule being amended")
	}
	if n := s.identifyCables(context.Background()); n != 1 {
		t.Errorf("the pass reported %d answers; the LAN answer is one", n)
	}
	if p.count() != 0 {
		t.Errorf("a re-plug reset %v while the board was answering on the LAN", p.ports())
	}
	if _, _, proven, _ := s.identities.cachedVerdict(reflashPort); proven != provenLan {
		t.Errorf("the re-plug filed %q instead of a fresh door answer", proven)
	}
}

// An answer that aged out is the case the reboot existed for. With the door answering
// there is still nothing to buy: the stamp on file is the one the board quoted, and the
// clock that ages it is restarted by the same free answer.
func TestAnExpiredAnswerIsRenewedByTheDoorRatherThanByAReset(t *testing.T) {
	p := &probeScript{answer: map[string]func() (esprom.OrbInfo, error){reflashPort: orbAnswer}}
	d := &dialScript{status: orbStatusAnswer()}
	s := lanHub(t, p, d)
	s.identities.done(reflashPort, esprom.OrbInfo{Port: reflashPort, IsOrb: true,
		MAC: "44:bd:8d:78:26:18", Project: "gorb-os", Version: "v1.0.0"})
	// Stand at the far edge of the TTL, where the sweep would otherwise pay.
	backdateIdentity(s, reflashPort, identityTTL+time.Minute)

	if ask, why := s.cableNeedsProbe(goldSighting()[0]); !ask {
		t.Fatalf("setup: an aged answer must read as due (%s)", why)
	}
	s.identifyCables(context.Background())

	if p.count() != 0 {
		t.Errorf("the sweep reset %v for an orb whose door answers; an aged answer is not a stranger", p.ports())
	}
	if _, _, proven, _ := s.identities.cachedVerdict(reflashPort); proven != provenLan {
		t.Errorf("the renewal filed %q; no reboot was paid, so none may be claimed", proven)
	}
}

// A door answer is a fact about the board and not a licence to strip the cable, so the
// two strongest uses of a cached answer must both refuse to read one: the contradiction
// that takes a board off a cable, and the failed-probe record that is supposed to replace
// an answer with an explanation of why there is none.
func TestADoorAnswerIsNeverReadAsAVerdictItDidNotEarn(t *testing.T) {
	s := newHub()
	s.statePath = ""
	s.identities.usbMAC = func(port string) string {
		if port == reflashPort {
			return "44:bd:8d:78:26:18"
		}
		return ""
	}
	s.identities.doneLan(reflashPort, esprom.OrbInfo{Port: reflashPort, IsOrb: true,
		MAC: "44:bd:8d:78:26:18", Version: "v1.4.0"})

	if _, ok := s.contradicts(reflashPort); ok {
		t.Error("a board answering as an orb was read as a contradiction, which drops it off its cable")
	}
	if wait := s.identities.nextAsk(reflashPort, time.Now()); wait > 0 {
		t.Errorf("a door answer armed a probe backoff of %s; nothing failed", wait)
	}

	// The paid question is asked anyway and never reaches a chip — a wedged console, a
	// lock held by esptool. The lead is deaf; the board is not gone, and the answer the
	// network gave stands over the failure of a door the hub could not open.
	s.identities.doneFailed(reflashPort, "port busy: esptool")
	info, _, proven, present := s.identities.cachedVerdict(reflashPort)
	if !present || !info.IsOrb {
		t.Fatalf("a failed probe erased a live orb's answer: present=%v info=%+v", present, info)
	}
	if proven != provenLan {
		t.Errorf("the entry became %q; a failure record may not claim to know more than the answer", proven)
	}
}

// What the hub publishes, and what a reboot it did not pay for may not be reported as.
// The Identify button is the operator's way of buying the fields a door cannot give —
// project name, build stamp, ELF digest — and a card that called a door answer a probe
// would hide the only control that gets them.
func TestAPortEntrySaysHowItLearnedWhatItKnows(t *testing.T) {
	src := readFile(t, "identity.go")
	i := strings.Index(src, `rebootPaid := ent.proven != provenCable && ent.proven != provenLan`)
	if i < 0 {
		t.Fatal("the port entry no longer treats a door answer as unpaid")
	}
	block := src[i:]
	if j := strings.Index(block, `\n\t\t}`); j > 0 {
		block = block[:j]
	}
	if !strings.Contains(block, `e["lanProof"]`) {
		t.Error("a door answer is filed with no field for the page to label it by")
	}
	for _, unpaid := range []string{`e["compileTime"]`, `e["elfSha256"]`, `e["probedAt"]`} {
		if at := strings.Index(block, unpaid); at >= 0 && at < strings.Index(block, `if ent.proven == provenLan`) {
			t.Errorf("%s is inside the reboot-only block no more; a door answer cannot quote it", unpaid)
		}
	}
}

// The wiring is the half no test can exercise from the inside: the seam is nil in a
// hand-built hub so the suite never dials a network, which means a main() that forgot it
// ships a hub that resets every orb on the bench and passes the whole suite green.
func TestTheRealHubWiresTheQuestionItIsNowOwedToAsk(t *testing.T) {
	src := readFile(t, "main.go")
	if !strings.Contains(src, `s.lanDial = func(ip string) (*orbnet.Status, error)`) {
		t.Error("main never wires s.lanDial, so the shipped hub asks nothing over the LAN")
	}
}
