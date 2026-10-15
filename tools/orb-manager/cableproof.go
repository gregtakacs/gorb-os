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
// board's own word for itself. It opens no download mode and forces no reboot — the
// same reason a flash session can stream a boot console it held across its own reset.
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

	// cableProofEvery is how long a good answer stands before the hub re-asks a board
	// that keeps sitting on the lead. Two an hour, for a chip that is answering.
	cableProofEvery = 10 * time.Minute

	// cableProofRetry is how long a silent or failed attempt buys the chip some peace.
	// A blank Espressif board enumerates like an orb and stays enumerated; asking it
	// every five seconds would put "cmd running on /dev/ttyACM0" in front of anyone
	// with the flash panel open.
	cableProofRetry = 5 * time.Minute
)

// cableVerdict is one chip's answer to PING, and the name it volunteered while the
// hub was listening. state is the firmware's own token: "b" waiting for a network,
// "c-ap" configured with its AP isolated, "c-sta" configured as a station.
type cableVerdict struct {
	ack   bool
	state string
	name  string
}

// cableProof is the throttle and the seam. ping is nil until main wires the console
// round trip, exactly like the cable sweep and the liveness probe, so a hub the suite
// builds opens no port it did not ask to open.
type cableProof struct {
	mu       sync.Mutex
	at       map[string]time.Time
	inFlight map[string]bool
	ping     func(ctx context.Context, port string) (cableVerdict, error)
}

func newCableProof() *cableProof {
	return &cableProof{at: map[string]time.Time{}, inFlight: map[string]bool{}}
}

// enableCableProof wires the real console round trip. main calls it beside
// enableCables; newHub leaves it off.
func (s *Server) enableCableProof() { s.prover.ping = cableConsolePing }

// due reports whether this port may be asked now, and takes the attempt slot when it
// may: one caller per port, with the clock starting before the round trip so a
// failure is throttled exactly like a silence.
func (p *cableProof) due(port string, now time.Time) bool {
	if p == nil || p.ping == nil {
		return false
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.inFlight[port] {
		return false
	}
	if last, seen := p.at[port]; seen && now.Sub(last) < cableProofRetry {
		return false
	}
	p.at[port] = now
	p.inFlight[port] = true
	return true
}

// settled releases the port and moves the clock. A good answer pushes the next
// attempt out to cableProofEvery; a silence leaves the retry stamp due took, so a
// board that replies is asked twice an hour and a board that does not once every
// cableProofRetry.
func (p *cableProof) settled(port string, ok bool, now time.Time) {
	if p == nil {
		return
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	delete(p.inFlight, port)
	if ok {
		p.at[port] = now.Add(cableProofEvery - cableProofRetry)
	}
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
	if !s.prover.due(c.port, time.Now()) {
		return false, false
	}
	ok, changed := s.cableProofRoundTrip(ctx, c)
	s.prover.settled(c.port, ok, time.Now())
	return ok, changed
}

// cableProofRoundTrip claims the cable the way a command does and asks the question.
// The registry entry names it opCmd, so /api/ops says what is on the wire rather than
// merely that something is, and a flash started one tick later waits instead of
// colliding.
func (s *Server) cableProofRoundTrip(ctx context.Context, c cableSighting) (bool, bool) {
	job, octx, oerr := s.ops.start(ctx, c.port, opCmd, cableProofBudget)
	if oerr != nil {
		return false, false
	}
	defer s.ops.finish(job)
	lock, lerr := orbbind.LockPort(c.port)
	if lerr != nil {
		return false, false
	}
	defer lock.Release()

	pctx, cancel := context.WithTimeout(octx, cableProofBudget)
	defer cancel()
	v, err := s.prover.ping(pctx, c.port)
	if err != nil && !v.ack {
		// Silence is the normal answer from a blank chip, so it is a line in the log
		// and nothing on screen. A port the OS would not open is the same story.
		log.Printf("orb-manager: cable proof %s: %v", c.port, err)
		return false, false
	}
	if !v.ack {
		return false, false
	}
	return true, s.recordCableProof(c.port, c.mac, v.state, v.name)
}

// recordCableProof files an ORB-PING answer where every other orb verdict lives: the
// port identity cache, the lonely marks, and the fleet's cached name. orbcmd.go calls
// it too, so a PING the operator sent by hand earns the same bookkeeping as one the
// hub sent on its own.
func (s *Server) recordCableProof(port, mac, state, name string) bool {
	if s.identities == nil {
		return false
	}
	s.identities.doneCable(port, esprom.OrbInfo{
		Port:   port,
		IsOrb:  true,
		MAC:    mac,
		State:  state,
		Reason: "answered ORB-PING over the cable",
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
	// The boot banner the same console flush carried. The board owns its name; this is
	// the hub caching what it heard, and it is the only name readable from a board
	// with no address to be dialed at.
	if nm := strings.TrimSpace(name); nm != "" {
		return s.foldBootName(mac, nm)
	}
	return false
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
		if res.State != "" {
			out.state = res.State
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
			out[port] = cableVerdict{ack: true, state: ent.info.State, name: ent.info.Name}
		}
	}
	if len(out) == 0 {
		return nil
	}
	return out
}
