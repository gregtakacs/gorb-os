package main

// The free cable read, tested against a chip the test drives. Each case answers PING the
// way the board on the lead would, then asks one question about what the hub reports. A
// hub that pinged a board it had already proven, that filed a silence as a verdict, that
// asked again every five seconds, or that dressed a console answer up as a reboot it never
// paid for, fails one of these. No test opens a real port: newHub leaves the seam unwired,
// and proveCables returns when nothing is wired into it.

import (
	"context"
	"encoding/json"
	"errors"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
)

// proofHub is a hub with the cable view and the PING seam wired, an address book in a
// tempdir, and a deaf-on-purpose browse so the row under test can only be the cable's.
// The counter is how many times the fake chip was actually asked.
func proofHub(t *testing.T, answer func() (cableVerdict, error), seen ...cableSighting) (*Server, *int) {
	t.Helper()
	now := time.Now()
	s := newHub()
	s.fleet = orbfleet.New(orbfleet.Config{Now: func() time.Time { return now }})
	s.statePath = filepath.Join(t.TempDir(), "state.json")
	s.browse = filled(t)
	calls := 0
	s.prover.ping = func(_ context.Context, _ string) (cableVerdict, error) {
		calls++
		if answer == nil {
			return cableVerdict{ack: true}, nil
		}
		return answer()
	}
	stubCables(s, seen...)
	return s, &calls
}

func orbOnACable() cableSighting {
	return cableSighting{port: "/dev/ttyACM0", isOrb: true, mac: macHall}
}

// The bug this exists for. A flashed, named orb with no credentials announces nothing, so
// the descriptor was all the hub knew, the row was filtered out of the fleet, and the only
// door the page offered was the one that flashes over it. Either plug order, same silence.
func TestACabledOrbThatAnswersBecomesACardWithNoReboot(t *testing.T) {
	s, calls := proofHub(t, func() (cableVerdict, error) {
		return cableVerdict{ack: true, state: "b", name: "Bronze"}, nil
	}, orbOnACable())
	now := time.Now()

	s.proveCables(context.Background())
	if *calls != 1 {
		t.Fatalf("%d pings for one board on one lead: a sweep asks each cable once", *calls)
	}

	rows, _ := s.roster(nil, now)
	s.annotateOrb(rows)
	if len(rows) != 1 {
		t.Fatalf("%d rows for the one board on the lead", len(rows))
	}
	r := rows[0]
	if !r.ConsoleOrb || !r.Orb {
		t.Fatalf("the row is still filtered from the fleet after the chip answered: %+v", r)
	}
	if r.Identity != "descriptor" {
		t.Errorf("identity = %q: the console proved the app, not the MAC the descriptor named", r.Identity)
	}
	if r.Name != "Bronze" {
		t.Errorf("name = %q, want the word the board's own banner carried", r.Name)
	}
	if !strings.Contains(r.Refusal, "provision") {
		t.Errorf("refusal = %q: it still points at the door the hub just walked through", r.Refusal)
	}

	info, ok := s.identities.get("/dev/ttyACM0")
	if !ok || !info.IsOrb {
		t.Fatal("the answer was not filed against the cable it came down")
	}
	if info.State != "b" {
		t.Errorf("state = %q, want the token the chip sent", info.State)
	}
	if info.Version != "" || info.ElfSHA256 != "" {
		t.Error("a PING was allowed to invent a firmware stamp")
	}
	// The name came off the board, so the address book holds it. It does not ride the
	// state file: a row with no address is not an address-book entry, and the next
	// launch re-asks the cable and hears the same word.
	if row, ok := s.fleet.ByMAC(macHall); !ok || row.Name != "Bronze" {
		t.Errorf("book = %+v ok=%v, want the word the board's own banner carried", row, ok)
	}
}

// A silent chip is not a verdict. It is one question, then a pause, and a row that stays
// out of the fleet exactly as it was before the hub opened anything.
func TestASilentBoardIsAskedOnceAndThenLeftAlone(t *testing.T) {
	s, calls := proofHub(t, func() (cableVerdict, error) {
		return cableVerdict{}, errors.New("esprom: console read: timeout")
	}, orbOnACable())
	now := time.Now()

	s.proveCables(context.Background())
	if *calls != 1 {
		t.Fatalf("%d attempts on a chip that said nothing", *calls)
	}
	s.proveCables(context.Background())
	s.proveCables(context.Background())
	if *calls != 1 {
		t.Errorf("%d attempts: a board that does not answer is re-asked on the retry clock, not every tick", *calls)
	}

	rows, _ := s.roster(nil, now)
	s.annotateOrb(rows)
	if len(rows) != 1 || rows[0].Orb || rows[0].ConsoleOrb {
		t.Error("a chip that answered nothing was recorded as an orb")
	}
	if !strings.Contains(rows[0].Refusal, "identify it") {
		t.Errorf("refusal = %q: an unproven row still points at the one door that can prove it", rows[0].Refusal)
	}

	// Out of the window it is worth asking again: the board may have been flashed since.
	s.prover.at["/dev/ttyACM0"] = time.Now().Add(-cableProofRetry - time.Second)
	s.proveCables(context.Background())
	if *calls != 2 {
		t.Errorf("%d attempts after the retry window, want the board asked again", *calls)
	}
}

// The four reasons not to open a port at all: something else already proves the board, a
// reboot-paid probe already knows more, the operator's own hand is on the wire, or this hub
// left the chip lonely minutes ago.
func TestTheHubAsksOnlyTheCableThatNeedsAsking(t *testing.T) {
	ack := func() (cableVerdict, error) { return cableVerdict{ack: true, state: "c-sta"}, nil }

	s, calls := proofHub(t, ack, orbOnACable())
	// The chip must be in the book before it can be named, which is the same order the
	// sweep imposes on a board the hub has only ever seen on a lead.
	if _, err := s.fleet.Confirm(macHall); err != nil {
		t.Fatalf("seed: %v", err)
	}
	if _, _, err := s.fleet.NoteDeviceName(macHall, "kitchen"); err != nil {
		t.Fatalf("seed name: %v", err)
	}
	s.proveCables(context.Background())
	if *calls != 0 {
		t.Errorf("%d pings at a board the address book already names", *calls)
	}

	s2, calls2 := proofHub(t, ack, orbOnACable())
	s2.identities.done("/dev/ttyACM0", esprom.OrbInfo{Port: "/dev/ttyACM0", IsOrb: true, MAC: macHall, Version: "v1.1.0"})
	s2.proveCables(context.Background())
	if *calls2 != 0 {
		t.Errorf("%d pings behind a probe that already read the firmware", *calls2)
	}

	s3, calls3 := proofHub(t, ack, orbOnACable())
	job, _, oerr := s3.ops.start(context.Background(), "/dev/ttyACM0", opFlash, time.Minute)
	if oerr != nil {
		t.Fatalf("claim: %v", oerr)
	}
	s3.proveCables(context.Background())
	if *calls3 != 0 {
		t.Errorf("%d pings down a wire a flash holds", *calls3)
	}
	s3.ops.finish(job)

	s4, calls4 := proofHub(t, ack, orbOnACable())
	s4.lonely.mark(macHall)
	s4.proveCables(context.Background())
	if *calls4 != 0 {
		t.Errorf("%d pings at a chip this hub marked lonely", *calls4)
	}

	s5, calls5 := proofHub(t, ack, cableSighting{port: "/dev/ttyUSB9", isOrb: false})
	s5.proveCables(context.Background())
	if *calls5 != 0 {
		t.Errorf("%d pings at a port that is not the orb peripheral", *calls5)
	}

	s6, calls6 := proofHub(t, ack, cableSighting{port: "/dev/ttyACM1", isOrb: true, mac: ""})
	s6.proveCables(context.Background())
	if *calls6 != 0 {
		t.Errorf("%d pings at a cable whose MAC nobody knows, where the answer has nowhere to be filed", *calls6)
	}
}

// A hub the suite built has no console seam, and a hub shutting down has no business
// opening one. Both are a no-op rather than a panic or a port open.
func TestNothingIsPingedWithoutAWiredSeamOrALivingContext(t *testing.T) {
	s := newHub()
	stubCables(s, orbOnACable())
	s.proveCables(context.Background()) // prover.ping is nil: nothing to ask with

	s2, calls := proofHub(t, ackAlways, orbOnACable())
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	s2.proveCables(ctx)
	if *calls != 0 {
		t.Errorf("%d pings after the hub began to quit", *calls)
	}
}

func ackAlways() (cableVerdict, error) { return cableVerdict{ack: true, state: "c-sta"}, nil }

// Two fields rather than one, because the picker may say an orb sits on this cable and
// must not say anybody rebooted it to find that out.
func TestAConsoleAnswerNeverReadsAsAProbe(t *testing.T) {
	s, _ := proofHub(t, func() (cableVerdict, error) {
		return cableVerdict{ack: true, state: "c-sta"}, nil
	}, orbOnACable())
	s.recordCableProof("/dev/ttyACM0", macHall, "c-sta", "")

	got := s.consoleProofs([]cableSighting{orbOnACable()})
	if len(got) != 1 {
		t.Fatalf("consoleProofs = %+v, want the standing answer for this cable", got)
	}
	if !got["/dev/ttyACM0"].ack || got["/dev/ttyACM0"].state != "c-sta" {
		t.Errorf("consoleProofs = %+v: the chip's own token did not survive the filing", got)
	}

	// The same reader refuses an answer a probe produced: different provenance, and that
	// row is already proven by its confirmed identity.
	s2, _ := proofHub(t, nil)
	s2.identities.done("/dev/ttyACM0", esprom.OrbInfo{Port: "/dev/ttyACM0", IsOrb: true, MAC: macHall})
	if got := s2.consoleProofs([]cableSighting{orbOnACable()}); len(got) != 0 {
		t.Errorf("consoleProofs = %+v: a reboot-paid probe was reported as a free cable read", got)
	}

	// The port list must keep the two fields apart. This is the half handlePorts owns, so
	// it is pinned at the source rather than through a live enumeration.
	src := readFile(t, "identity.go")
	if !strings.Contains(src, `e["probed"] = rebootPaid`) {
		t.Error(`handlePorts no longer derives probed from provenance, so a PING reports a reboot`)
	}
	if !strings.Contains(src, `e["cableProof"]`) {
		t.Error(`handlePorts stopped writing cableProof, so the picker cannot say what it knows`)
	}
}

// An operator who pings the board from the forget dialog produced the same evidence, and
// the hub keeps it: the lonely mark stops standing, and the row stops being a mystery.
func TestAHandSentPingCountsTheSameWay(t *testing.T) {
	s, _ := proofHub(t, nil, orbOnACable())
	s.lonely.mark(macHall)
	if changed := s.recordCableProof("/dev/ttyACM0", macHall, "b", "   "); changed {
		t.Error("a blank name on a first sighting is not a change to the book")
	}
	if s.lonely.has(macHall) {
		t.Error("the chip answered the orb door and the lonely mark still stands")
	}
	if got := s.consoleProofs([]cableSighting{orbOnACable()}); len(got) != 1 {
		t.Error("the hand-sent answer was not filed against its cable")
	}
	// The port the operator named is the port the answer is filed against, and the MAC
	// the dialog sent is the one the book keys on.
	if info, ok := s.identities.get("/dev/ttyACM0"); !ok || info.MAC != macHall {
		t.Errorf("filed against the wrong cable or the wrong chip: %+v", info)
	}
}

// A console answer changes what a cable row claims about itself. It changes nothing about
// the lead being a way to reach the board, which is the fact Forget is gated on and the
// forget dialog acts on: the proven row must still refuse the plain button, still carry
// the dialog verdict, and still name the lead.
func TestAProvenCableRowIsStillNotPlainForgettable(t *testing.T) {
	now := time.Now()
	s, _ := proofHub(t, func() (cableVerdict, error) {
		return cableVerdict{ack: true, state: "b", name: "Bronze"}, nil
	}, orbOnACable())
	s.proveCables(context.Background())

	rows, _ := s.roster(nil, now)
	s.annotateForget(rows, now)
	s.annotateOrb(rows)
	if len(rows) != 1 {
		t.Fatalf("%d rows", len(rows))
	}
	if !rows[0].Orb {
		t.Fatal("the proven board is not a card, so the dialog that ends it is unreachable")
	}
	if rows[0].Forgettable {
		t.Error("a board on a lead this hub can reach was marked plain-forgettable")
	}
	if !rows[0].LiveForget || rows[0].LivePort != "/dev/ttyACM0" {
		t.Errorf("liveForget=%v livePort=%q: the endings need the lead they drive",
			rows[0].LiveForget, rows[0].LivePort)
	}
	if !strings.Contains(rows[0].ForgetWhy, "/dev/ttyACM0") {
		t.Errorf("forgetWhy = %q: the reason must name the lead", rows[0].ForgetWhy)
	}
}

// The empty list's one job is to say what the hub did with the cable it can see. A board
// that answered is in the list; the sentence below it is only ever about the ones that did
// not answer, so the field it counts on has to keep arriving.
func TestAnUnprovenCableRowStillArrivesWithItsVerdict(t *testing.T) {
	s, calls := proofHub(t, func() (cableVerdict, error) {
		return cableVerdict{}, errors.New("no answer")
	}, orbOnACable())
	s.proveCables(context.Background())
	if *calls != 1 {
		t.Fatalf("%d attempts", *calls)
	}
	rows, _ := s.roster(nil, time.Now())
	s.annotateOrb(rows)
	body, _ := s.orbCard(time.Now())
	wire, _ := json.Marshal(body["orbs"])
	if !strings.Contains(string(wire), `"consoleOrb":false`) {
		t.Errorf("consoleOrb absent from the wire row, so the page would read a hole as a maybe: %s", wire)
	}
	if !strings.Contains(string(wire), `"orb":false`) {
		t.Errorf("orb absent: %s", wire)
	}
	_ = rows
}

// The clock belongs to the board on the lead, not to the name the OS gave the lead. A chip
// unplugged mid-ask leaves no answer and must leave no penalty either: the board that takes
// that lead seconds later is a board nobody has asked.
func TestABoardNewToALeadIsNotHeldBehindTheLastBoardsSilence(t *testing.T) {
	s := newHub()
	s.fleet = orbfleet.New(orbfleet.Config{})
	s.statePath = filepath.Join(t.TempDir(), "state.json")
	s.browse = filled(t)
	s.cables.ttl = 0
	var bench []cableSighting
	var answer func() (cableVerdict, error)
	s.cables.list = func() ([]cableSighting, error) { return bench, nil }
	calls := 0
	s.prover.ping = func(context.Context, string) (cableVerdict, error) {
		calls++
		return answer()
	}

	// Four refusals, which is what it takes the hub to settle on the long gap. The clock
	// is driven rather than waited on: this test is about which gap is armed.
	bench = []cableSighting{orbOnACable()}
	answer = func() (cableVerdict, error) {
		return cableVerdict{sent: true}, errors.New("esprom: console read: i/o timeout")
	}
	for i := 0; i <= cableProofNudges; i++ {
		s.prover.at["/dev/ttyACM0"] = time.Now().Add(-cableProofRetry - time.Second)
		s.proveCables(context.Background())
	}
	if calls != cableProofNudges+1 {
		t.Fatalf("%d asks to arm the long gap, want %d", calls, cableProofNudges+1)
	}
	if got := s.prover.gap["/dev/ttyACM0"]; got != cableProofRetry {
		t.Fatalf("gap = %v after %d refusals, want the long quiet", got, calls)
	}

	// The lead empties, then carries an orb that answers. Not one second of the last
	// board's clock survives, so the very next sweep asks it and the row lands proven.
	bench = nil
	s.proveCables(context.Background())
	if len(s.prover.at) != 0 {
		t.Fatalf("the vanished lead kept its clock: %v", s.prover.at)
	}
	bench = []cableSighting{orbOnACable()}
	answer = func() (cableVerdict, error) { return cableVerdict{ack: true, state: "b"}, nil }
	s.proveCables(context.Background())
	if calls != cableProofNudges+2 {
		t.Fatalf("%d asks after the replug, want the new board asked at once", calls)
	}
	rows, _ := s.roster(nil, time.Now())
	s.annotateOrb(rows)
	if len(rows) != 1 || !rows[0].ConsoleOrb {
		t.Errorf("the board that replaced it was left unproven: %+v", rows)
	}
}

// The ladder itself, at the seam rather than through a sweep: which gap each kind of answer
// arms, and how long the hub then keeps its hands off the wire.
func TestTheGapAnAnswerArms(t *testing.T) {
	p := newCableProof()
	p.ping = func(context.Context, string) (cableVerdict, error) { return cableVerdict{}, nil }
	now := time.Now()
	const port = "/dev/ttyACM0"

	// A silence on a board this hub has only just met may be a board still booting, so it
	// is re-asked on the short gap until the hub's patience is spent.
	for i := 1; i <= cableProofNudges+1; i++ {
		if !p.due(port, now) {
			t.Fatalf("attempt %d refused: a fresh lead is asked", i)
		}
		p.settled(port, askSilent, now)
		want := cableProofNudge
		if i > cableProofNudges {
			want = cableProofRetry
		}
		if got := p.gap[port]; got != want {
			t.Errorf("after silence %d the gap is %v, want %v", i, got, want)
		}
		if p.due(port, now.Add(want-time.Second)) {
			t.Errorf("asked %v early, inside the %v that answer armed", want-time.Second, want)
		}
		now = now.Add(want + time.Second)
	}

	// An answer outranks the silences before it, buys the long quiet, and forgets the
	// streak that came before it.
	if !p.due(port, now) {
		t.Fatal("a board out of its gap is asked again")
	}
	p.settled(port, askAck, now)
	if got := p.gap[port]; got != cableProofEvery {
		t.Errorf("gap = %v after an answer, want %v", got, cableProofEvery)
	}
	if len(p.tries) != 0 {
		t.Errorf("tries = %v after an answer, want the failures forgotten", p.tries)
	}
	if p.due(port, now.Add(cableProofEvery-time.Minute)) {
		t.Error("a board that answered was re-asked inside the hour")
	}

	// A question that never left the hub learned nothing, and the lead it meant is gone.
	p.retain([]cableSighting{{port: "/dev/ttyACM1", isOrb: true, mac: "aabbccddeeff"}})
	if len(p.at) != 0 || len(p.gap) != 0 {
		t.Errorf("the abandoned lead kept history: at=%v gap=%v", p.at, p.gap)
	}
	if !p.due(port, now.Add(time.Minute)) {
		t.Error("a port with no history was refused")
	}
	p.settled(port, askNoWire, now.Add(time.Minute))
	if got := p.gap[port]; got != cableProofNudge {
		t.Errorf("gap = %v after a port that would not open, want the short gap", got)
	}
	// Mid-round-trip is the one time a vanished lead keeps its clock: settled has not run,
	// and dropping the stamp would let a replug double-ping a wire still held.
	p.at["/dev/ttyACM1"] = now
	p.inFlight["/dev/ttyACM1"] = true
	p.retain(nil)
	if p.at["/dev/ttyACM1"].IsZero() || !p.inFlight["/dev/ttyACM1"] {
		t.Error("a round trip still on the wire had its history dropped")
	}
	p.retain([]cableSighting{{port: "/dev/ttyACM1", isOrb: true, mac: "aabbccddeeff"}})
	if p.at["/dev/ttyACM1"].IsZero() {
		t.Error("a lead still on the bench lost its clock")
	}
}
