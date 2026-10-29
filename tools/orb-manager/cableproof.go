package main

// The free proof: ask the chip on the lead it is already sitting on.
//
// The fleet card hides a board the hub cannot prove is an orb, and until this file
// every proof cost something: a packet, a dial, or a reboot. A board on a cable with
// no Wi-Fi credentials can supply none of them. It has no lease to announce from, no
// address for the hub to dial, and it is already plugged into the one machine that
// could ask it directly. That is the shape of "I have an orb plugged in and nothing
// appears", in either plug order, and it is why a named orb on a lead was offered to
// the New Orb flash button as if it were blank.
//
// So the hub asks the cheap question. `PING` down the console is answered by
// orb_net.c's serial reader (orb_net.c:2822), which exists only in the app: a blank
// chip, a chip in the ROM bootloader, and this hub's own lonely image all sit silent.
// The answer carries the firmware's state token, and the boot banner the same console
// flush prints carries GorbOS-NAME, so one round trip yields the orb verdict and the
// board's own word for itself. On a UART-bridge lead that round trip is free: opening the
// port never touches the chip. On a USB-Serial-JTAG lead it is not free — the host's open
// asserts DTR/RTS and the ROM's auto-reset reads that edge, so one ping can cost one boot.
// Two rules make that acceptable instead of a storm, both enforced below and in
// proveCable: every ping ends with a HardReset (the board is handed back running its app,
// never stranded at the download stub the close's line edges can otherwise leave), and
// the ping claims the reboot it may cause through expectSelfReboot and honors the same
// excuse before asking, so no door — this one included — pulses a board that is on its
// way back up. One sanctioned boot per cable per window, and the cadence is the
// throttle's.
//
// What the proof does NOT do. It does not confirm the MAC: that still rides on the
// USB descriptor, and the badge says so. It reads no firmware stamp, so the port
// entry keeps `probed` false and the Identify button stays on the page. It dials
// nothing, and it never contradicts a lonely mark: that mark says the operator just
// ended this board's orb life, and the lonely image does not answer the door anyway.
//
// Courtesy is a budget: one command per cable, only for a port that needs it —
// attributed, orb-shaped, not lonely, unproven by every other door, idle, and out of
// its retry window. The claim runs through the same operation registry a flash uses,
// so an operator mid-write is never interrupted.

import (
	"context"
	"log"
	"strings"
	"sync"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
)

const (
	// cableProofBudget bounds one PING round trip. The console driver re-pushes the
	// line on a 2 s cadence, so this buys three attempts and a reply, and it stays
	// inside one sweep tick: a wedged port cannot hold a cable for a minute.
	cableProofBudget = 8 * time.Second
	// The post-update serial re-read: the board has just taken a reboot from
	// h_reboot, its CDC endpoint is off the bus for the boot, and the app's
	// console door is live a few seconds after it returns. The first pause
	// covers that boot; the window bounds the whole re-read, after which the
	// ten-second Wi-Fi probe is the reader of record.
	cableAfterUpdatePause  = 6 * time.Second
	cableAfterUpdateTries  = 4
	cableAfterUpdateWindow = 45 * time.Second

	// cableProofEvery is how long a good answer stands before the hub re-asks a board
	// that keeps sitting on the lead. Two an hour, for a chip that is answering.
	cableProofEvery = 10 * time.Minute

	// cableProofRetry is how long a silent board buys the chip some peace once the hub
	// has worn out its patience. A blank Espressif board enumerates like an orb and stays
	// enumerated; asking it every five seconds would put "cmd running on /dev/ttyACM0" in
	// front of anyone with the flash panel open.
	cableProofRetry = 5 * time.Minute

	// cableProofNudge is the short gap the hub keeps on a board it has only just met. A
	// chip plugged a second ago is still in its bootloader: the wire is live, the PING
	// goes down it, and the reader that answers is a task the app has not started yet. A
	// silence that early says nothing about whether an orb lives there, and five minutes
	// of "silent when asked" over a board that booted two seconds later is the same hole
	// this file exists to close, one notch smaller.
	cableProofNudge = 20 * time.Second

	// cableProofNudges is how many short gaps the hub spends on one board before it
	// settles on cableProofRetry. Three covers a minute of boot, which is longer than
	// this firmware's console reader takes; a fourth buys a blank chip nothing.
	cableProofNudges = 3
)

// askResult is what one attempt produced, and the only thing the throttle reads. The
// three cases buy three different waits because they are three different answers, and
// reporting the second and third as one number is what left a board that had never been
// asked looking like a board that had been asked and had refused.
type askResult int

const (
	// askAck: the app answered. The cable is proven, and the board may keep its answer
	// for cableProofEvery.
	askAck askResult = iota
	// askSilent: PING went down a live wire and nothing answered it inside the budget.
	// The honest answer to that is "not proven", so the retry clock applies.
	askSilent
	// askNoWire: the question never left the hub — no port to open, no lock to take, no
	// registration to run under. Nothing was learned about the chip, so the hub asks
	// again soon rather than charging a board for a question it never received.
	askNoWire
)

// cableVerdict is one chip's answer to PING, and the name and running firmware it
// volunteered while the hub was listening. state is the firmware's own token: "b"
// waiting for a network, "c-ap" configured with its AP isolated, "c-sta" configured
// as a station. fw is the stamp the ack carries since the PING reply grew one
// token; a board on older firmware answers without it and the field stays empty.
type cableVerdict struct {
	ack   bool
	sent  bool
	state string
	fw    string
	name  string
}

// cableProof is the throttle and the seam. ping is nil until main wires the console
// round trip, exactly like the cable sweep and the liveness probe, so a hub the suite
// builds opens no port it did not ask to open.
type cableProof struct {
	mu       sync.Mutex
	at       map[string]time.Time
	gap      map[string]time.Duration
	tries    map[string]int
	inFlight map[string]bool
	// gone is when a tracked port was first seen absent; retain ages the clock out
	// cableProofGoneGrace after that stamp, not at the first missed sweep.
	gone map[string]time.Time
	// macOf is the MAC last sighted on a tracked port, so a return under a different
	// MAC is read as a chip swap and the old board's silence does not follow it.
	macOf map[string]string
	ping  func(ctx context.Context, port string) (cableVerdict, error)
}

func newCableProof() *cableProof {
	return &cableProof{at: map[string]time.Time{}, gap: map[string]time.Duration{},
		tries: map[string]int{}, inFlight: map[string]bool{},
		gone: map[string]time.Time{}, macOf: map[string]string{}}
}

// enableCableProof wires the real console round trip. main calls it beside
// enableCables; newHub leaves it off.
func (s *Server) enableCableProof() { s.prover.ping = cableConsolePing }

// due reports whether this port may be asked now, and takes the attempt slot when it
// may: one caller per port, with the clock starting before the round trip so a call that
// never reaches a chip is throttled too — by the short gap settled assigns it, not by the
// five minutes a real silence earns.
func (p *cableProof) due(port string, now time.Time) bool {
	if p == nil || p.ping == nil {
		return false
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.inFlight[port] {
		return false
	}
	if last, seen := p.at[port]; seen {
		gap := p.gap[port]
		if gap == 0 {
			gap = cableProofRetry
		}
		if now.Sub(last) < gap {
			return false
		}
	}
	p.at[port] = now
	p.inFlight[port] = true
	return true
}

// settled releases the port and moves the clock to whatever the answer deserves. A good
// answer buys an hour of quiet and forgets the board's streak of failures. A silence
// re-asks on the short gap for the first few attempts, on the long one after that: a
// board the hub has just met may still be booting, and a board that has refused three
// times is not going to change its mind in the fourth. A question that never left the
// hub is charged the same short gap rather than cleared outright, because a port the OS
// will not open at all would otherwise be opened at every tick forever.
func (p *cableProof) settled(port string, r askResult, now time.Time) {
	if p == nil {
		return
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	delete(p.inFlight, port)
	p.at[port] = now
	if r == askAck {
		p.gap[port] = cableProofEvery
		delete(p.tries, port)
		return
	}
	if p.tries[port] < cableProofNudges {
		p.tries[port]++
		p.gap[port] = cableProofNudge
		return
	}
	p.gap[port] = cableProofRetry
}

// cableProofGoneGrace is how long a departed lead keeps its throttle clock before
// retain treats the board as gone. The old rule — delete the clock the moment the port
// leaves the list — defeated the whole throttle on this bench: a reset takes a
// USB-Serial-JTAG chip off the bus for a few seconds, the departure erased the gap
// settled() had just charged, and the next five-second sweep found a "brand new" board
// with no history and pinged it again. One sanctioned reboot per hub start became a
// reset per sweep — which is what a boot loop looked like from the sofa. Same MAC and
// same port within the grace is the same chip coming back from the reset the hub itself
// asked for; a different MAC (a chip swap) or a departure past the grace still drops the
// clock, which is what this function was written for.
const cableProofGoneGrace = 45 * time.Second

// retain drops the throttle history of every cable that has truly left the bench, and
// keeps it across a blink: the seconds a chip spends off the bus while it reboots.
// The clock is keyed by port name, and a port name outlives the board behind it, so a
// returning sighting whose MAC differs from the one on file drops the history before
// anything else — the next chip to take the lead is asked, not held behind the last
// one's silence. A port mid-round-trip keeps its history — the attempt still holds the
// wire, and settled writes its verdict the moment it lets go.
func (p *cableProof) retain(present []cableSighting) {
	if p == nil {
		return
	}
	keep := make(map[string]string, len(present)) // port -> its MAC on this sweep
	for _, c := range present {
		keep[c.port] = c.mac
	}
	now := time.Now()
	p.mu.Lock()
	defer p.mu.Unlock()
	for port := range p.at {
		if p.inFlight[port] {
			continue
		}
		mac, here := keep[port]
		switch {
		case here && mac != "" && p.macOf[port] != "" && mac != p.macOf[port]:
			p.dropLocked(port) // a different chip took the lead
		case here:
			delete(p.gone, port)
			if mac != "" {
				p.macOf[port] = mac
			}
		default:
			// Absent this sweep: the clock waits out the grace, then goes.
			if since, seen := p.gone[port]; !seen {
				p.gone[port] = now
			} else if now.Sub(since) >= cableProofGoneGrace {
				p.dropLocked(port)
				delete(p.gone, port)
			}
		}
	}
	for port := range p.gone {
		if _, tracked := p.at[port]; !tracked {
			delete(p.gone, port)
		}
	}
}

// dropLocked forgets one port's throttle state. Callers hold p.mu.
func (p *cableProof) dropLocked(port string) {
	delete(p.at, port)
	delete(p.gap, port)
	delete(p.tries, port)
	delete(p.macOf, port)
}

// proveCables walks the bench once and asks every cable that needs asking. Serial on
// purpose: the sweep runs every five seconds, a port wedged half-open in its CDC
// endpoint must not stack a goroutine per tick, and two commands at once would both
// wait on the same chip's console.
func (s *Server) proveCables(ctx context.Context) {
	if s.prover == nil || s.prover.ping == nil || s.cables == nil {
		return
	}
	seen, cerr := s.cables.refresh(time.Now())
	// A sweep that could not enumerate is a bench nobody re-read. Working from the
	// last known set would ping a cable that may already carry another board.
	if cerr != "" {
		return
	}
	// Whatever did leave the bench loses its clock, so the next board to take that lead
	// is asked rather than held behind the last one's silence.
	s.prover.retain(seen)
	proven, storeChanged := false, false
	for _, c := range seen {
		if ctx.Err() != nil {
			return
		}
		ok, changed := s.proveCable(ctx, c)
		proven = proven || ok
		storeChanged = storeChanged || changed
	}
	if !proven {
		return
	}
	if storeChanged {
		s.saveState()
	}
	// The row moved from "no card" to "card", and nothing else wakes the stream: no
	// packet arrived, and the plug digest did not move either.
	s.wakeOrbs()
}

// rereadCableAfterUpdate is the serial re-read that a Wi-Fi update earns. A push
// over the air changes what the chip runs, and every cached answer about the lead
// it sits on — the port identity's version, the row's stamp — now quotes the old
// image. The sweep will not fix that by itself: proveCable refuses to re-ask a
// cable that once proved an orb, which is the read-once rule stated honestly. An
// update is the one event that makes the cached answer wrong by construction, so
// the hub asks the free question again, once, over the same round trip the sweep
// uses — claims, lock, recordCableProof and all. ORB-PING prints the running
// stamp, so the card learns the new version off the wire while the radio is still
// rejoining. A board with no cable, or one that stays silent past the boot window
// (gone from the bench, boot-looping, answered by the Wi-Fi probe first), costs
// this a few log-free returns; the ten-second probe folds the same stamp anyway.
func (s *Server) rereadCableAfterUpdate(mac string) {
	if mac == "" || s.cables == nil || s.prover == nil || s.prover.ping == nil {
		return
	}
	if s.lonely != nil && s.lonely.has(mac) {
		return
	}
	// The board rebooted a heartbeat ago. Its CDC endpoint blinks for the boot,
	// so wait out one window before opening, and keep the whole re-read inside
	// one boot-to-banner budget times a few tries — a board pulled during the
	// update leaves no sighting and dies quietly here.
	pause := s.rereadPause
	if pause == 0 {
		pause = cableAfterUpdatePause
	}
	deadline := time.Now().Add(cableAfterUpdateWindow)
	for attempt := 0; attempt < cableAfterUpdateTries; attempt++ {
		if time.Now().After(deadline) {
			return
		}
		time.Sleep(pause)
		port := s.cables.cablePortFor(mac, time.Now())
		if port == "" {
			continue // re-enumerating, or gone: the Wi-Fi probe owns this board now
		}
		// The same courtesy the sweep pays before touching the wire: the ping can
		// cost the chip a boot, and the excuse keeps a later silence from reading
		// as a reflash nobody ordered.
		s.expectSelfReboot(mac)
		ctx, cancel := context.WithTimeout(context.Background(), cableProofBudget)
		r, changed := s.cableProofRoundTrip(ctx, cableSighting{port: port, isOrb: true, mac: mac})
		cancel()
		s.prover.settled(port, r, time.Now())
		if r == askAck {
			if changed {
				s.saveState()
			}
			s.wakeOrbs()
			return
		}
	}
}

// proveCable asks one sighting and reports whether it proved an orb and whether the
// address book changed. Every gate below is a reason the question is already answered
// or would be rude.
func (s *Server) proveCable(ctx context.Context, c cableSighting) (bool, bool) {
	// Only a chip the host attributes to the orb peripheral, and only one whose MAC it
	// read: a row with no MAC cannot be filed anywhere, and the store keys on the chip.
	if !c.isOrb || c.mac == "" {
		return false, false
	}
	if s.lonely != nil && s.lonely.has(c.mac) {
		return false, false
	}
	// Something else already proves it: the book holds a name, an address, a stamp or
	// a commitment; or a reboot-paid probe already read the firmware, which outranks
	// a console line because it knows more than one.
	if row, known := s.fleetKnown(c.mac); known && storeProvesOrb(row) {
		return false, false
	}
	if info, ok := s.identities.get(c.port); ok && info.IsOrb {
		return false, false
	}
	if _, busy := s.ops.busy(c.port); busy {
		return false, false
	}
	// The excuse the paid door honors (launchprobe.go's gate), mirrored: a board the hub
	// just rebooted — by a probe, a theme store, or this door's own last ping — is a
	// board coming back up, not a board with no answer on file. Without this clause the
	// boot a ping caused reads on a later sweep as a fresh silence, and the ladder that
	// exists to space the asking is spent re-asking a chip that is already booting.
	if s.selfRebootExcused(c.mac) {
		return false, false
	}
	if !s.prover.due(c.port, time.Now()) {
		return false, false
	}
	// Claim the courtesy window before the question, not after: on this bench the open
	// can cost the boot, and the window has to cover it. The excuse retires the moment
	// the board is heard from (clearSelfReboot off the dial), so on a lead where the
	// ping is truly free this buys nothing and costs nothing.
	s.expectSelfReboot(c.mac)
	r, changed := s.cableProofRoundTrip(ctx, c)
	s.prover.settled(c.port, r, time.Now())
	return r == askAck, changed
}

// cableProofRoundTrip claims the cable the way a command does and asks the question.
// The registry entry names it opCmd, so /api/ops says what is on the wire rather than
// merely that something is, and the sweep claim means a flash or join started one tick
// later evicts this round instead of refusing the operator -- the sweep re-arms, and a
// press that queues behind it is a press that never runs.
//
// The result separates the three ways an attempt ends, because the clock must not charge
// a board for a question it never got: a refused registration, a refused lock and a port
// the OS would not open all mean no PING went down the wire.
func (s *Server) cableProofRoundTrip(ctx context.Context, c cableSighting) (askResult, bool) {
	job, octx, oerr := s.ops.startSweep(ctx, c.port, opCmd, cableProofBudget)
	if oerr != nil {
		return askNoWire, false
	}
	defer s.ops.finish(job)
	// SweepPort, not LockPort: this round is background work, so it must decline a
	// cable another hub's operator is writing to, and decline a cable another hub is
	// already sweeping, rather than fight for the flock and lose.
	lock, lerr := orbbind.SweepPort(octx, c.port)
	if lerr != nil {
		return askNoWire, false
	}
	defer lock.Release()

	pctx, cancel := context.WithTimeout(octx, cableProofBudget)
	defer cancel()
	v, err := s.prover.ping(pctx, c.port)
	switch {
	case v.ack:
		return askAck, s.recordCableProof(c.port, c.mac, v.state, v.fw, v.name)
	case err != nil && !v.sent:
		// The question never left the hub — the node is gone mid-ask, or the OS would not
		// open the lead at all. Nothing was learned about the chip on the other end.
		log.Printf("orb-manager: cable proof %s: %v", c.port, err)
		return askNoWire, false
	case err != nil:
		// PING went down a live wire that ran out of budget or broke answering it, which
		// reads as a silence. Silence is the normal answer from a blank chip, so it is a
		// line in the log and nothing on screen.
		log.Printf("orb-manager: cable proof %s: %v", c.port, err)
		return askSilent, false
	}
	return askSilent, false
}

// recordCableProof files an ORB-PING answer where every other orb verdict lives: the
// port identity cache, the lonely marks, and the fleet's cached name and running
// firmware. orbcmd.go calls it too, so a PING the operator sent by hand earns the
// same bookkeeping as one the hub sent on its own.
func (s *Server) recordCableProof(port, mac, state, fw, name string) bool {
	if s.identities == nil {
		return false
	}
	s.identities.doneCable(port, esprom.OrbInfo{
		Port:    port,
		IsOrb:   true,
		MAC:     mac,
		State:   state,
		Version: fw,
		Reason:  "answered ORB-PING over the cable",
	})
	// Said out loud once, because the alternative is a fleet row that appeared by
	// itself and a log with no record of the hub ever having asked.
	log.Printf("orb-manager: %s answered as an orb over %s (%s)", mac, port, orElse(state, "no state token"))
	if mac == "" {
		return false
	}
	// A serial ack is evidence of orb life, the only thing that clears a lonely mark.
	if s.lonely != nil {
		s.lonely.clear(mac)
	}
	// The chip is on this lead and has answered as an orb, which is the sighting the
	// book keeps. Recorded before the name because the book refuses to name a chip it
	// has never met, and at launch the console can answer before any sweep has built a
	// row to write onto.
	s.seeCable(mac, port)
	// The version the same ack carried. This is the only firmware stamp readable from
	// a board with no address to be dialed at and no reboot paid for, and it folds
	// state too, so a board that changed its mind since its last announce is
	// corrected by the cheapest question it can be asked.
	folded := false
	if fw != "" || state != "" {
		if _, changed, err := s.fleet.NoteAnswer(mac, fw, state); err == nil && changed {
			folded = true
		}
	}
	// The boot banner the same console flush carried. The board owns its name; this is
	// the hub caching what it heard, and it is the only name readable from a board
	// with no address to be dialed at.
	if nm := strings.TrimSpace(name); nm != "" {
		folded = s.foldBootName(mac, nm) || folded
	}
	if folded {
		s.saveState()
		s.wakeOrbs()
	}
	return folded
}

// cableConsolePing is the production round trip: open the lead, ask, stop at the first
// ORB-PING. RunCommand holds the port ready with DTR and never touches the RTS pairing
// that means download mode, so nothing here resets a chip, and it returns on the ack —
// normally inside a second, because the app's serial reader answers PING wherever in
// its life the board happens to be.
func cableConsolePing(ctx context.Context, port string) (cableVerdict, error) {
	var out cableVerdict
	pt, oerr := esprom.OpenPort(port)
	if oerr != nil {
		return out, oerr
	}
	defer pt.Close()
	// The same guarantee the paid probe makes (esprom/identity.go): from the moment the
	// hub touches this wire, the chip is owed a boot back. On a USB-Serial-JTAG lead the
	// open's line edges can land the chip anywhere, and the kernel's close is not the
	// place to find out — measured on this bench, a door that opened and closed without
	// the pulse left the chip at "waiting for download": black dial, dead console, a
	// board that reads as bricked from across the room. Defer order is load-bearing:
	// registered after pt.Close() so it fires first, while the fd is still open to drive
	// the lines.
	defer func() { _ = esprom.HardReset(pt) }()
	res, err := esprom.RunCommand(ctx, pt, esprom.SerialLine("PING", ""), []string{"ORB-PING"},
		func(line string) {
			if out.name != "" {
				return
			}
			if nm, ok := esprom.ParseOrbName(line); ok {
				out.name = nm
			}
		})
	if res != nil {
		out.ack = res.Ack
		out.sent = res.Sent
		if res.State != "" {
			out.state = res.State
		}
		if res.FW != "" {
			out.fw = res.FW
		}
	}
	return out, err
}

// stateWords turns the firmware's own token into the clause a row's refusal ends
// with. An unrecognised token is reported as that token rather than guessed at: it is
// the board's word, and the hub does not improve on it.
func stateWords(state string) string {
	switch state {
	case "b":
		return "it waits for a network: provision it to give the row an address"
	case "c-ap":
		return "it holds its AP isolated, so it announces nowhere this hub listens"
	case "c-sta":
		return "it has credentials and has taken no address this hub can hear"
	case "":
		return "it reports no state and no address"
	default:
		return "it reports state " + state + " and no address"
	}
}

// cableProofRefusal is the sentence a row carries once this hub has heard the chip
// speak and still has nothing to dial. The words it replaces said "identify it", and
// the hub has just done that for free, so pointing at the same door again would be the
// card arguing with itself. What is left is the thing the chip asked for.
func cableProofRefusal(port, state string) string {
	return "on a cable at " + port + ", answering the orb console door, " + stateWords(state)
}

// consoleProofs reports which of this sweep's sightings the hub has a standing
// ORB-PING answer for, in one lookup rather than one per row. An answer a reboot-paid
// probe produced needs nothing from here: that row is confirmed already and knows its
// firmware, which this does not.
func (s *Server) consoleProofs(seen []cableSighting) map[string]cableVerdict {
	if s.identities == nil || len(seen) == 0 {
		return nil
	}
	ports := make([]string, 0, len(seen))
	for _, c := range seen {
		ports = append(ports, c.port)
	}
	out := map[string]cableVerdict{}
	for port, ent := range s.identities.all(ports...) {
		if ent.proven == provenCable && ent.info.IsOrb {
			out[port] = cableVerdict{ack: true, state: ent.info.State, fw: ent.info.Version, name: ent.info.Name}
		}
	}
	if len(out) == 0 {
		return nil
	}
	return out
}
