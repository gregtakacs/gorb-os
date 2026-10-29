package main

import (
	"context"
	"errors"
	"log"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// This file exists because a USB descriptor cannot tell an orb from a board that used
// to be one.
//
// The two fields the cable sweep decides on are the product id and the serial. The
// serial IS the eFuse base MAC, so it is identical whatever image is running, and the
// product id is whatever USB function the running image exposes, which a stock
// Espressif CDC demo matches by accident. So the descriptor's claim that an orb sits on
// a lead survives a reflash out of gorb-os, a factory erase, and a reboot of the hub.
// Every door downstream reads that claim as fact: the row says "on cable", the New Orb
// picker refuses the board, and the Forget gate refuses on "the orb is on a cable at
// …". A paid verdict is the only thing that contradicts it, and a paid verdict lived
// only in memory, so the hub came back up after a restart holding the guess and nothing
// else.
//
// The fix is to stop guessing at all where a question can be asked: at launch, and
// periodically after, the hub pays a probe for every cable it can attribute, and the
// answer — orb or not — is filed before any door is allowed to reason from the
// descriptor. A board on a lead the hub cannot attribute is never touched:
// EnterDownloadMode pulses DTR/RTS, and doing that to an Arduino on the same bench
// resets a device nobody asked about.
//
// What the sweep does NOT do is pay for an answer the board gives for free. Before a
// reset is pulsed the hub asks the one question the reboot exists to answer, of the
// chip's own /api/v1/status door, at the address the fleet already holds for that MAC
// (lanProof). Only gorb-os serves that door, so an answer naming this board's eFuse MAC
// is the same fact a reboot buys, priced at one GET and with no blank glass. The reboot
// stays the only way to learn the opposite, so it stays owed to three cases: a chip the
// fleet does not know, a known chip whose door is silent, and a door answering as some
// other board.
//
// That is what the launch pulse stopped costing a healthy orb. The paid answer lived
// only in memory, so every hub restart re-pulsed every board on a lead: eight seconds
// of black display each, before the operator had touched anything.

const (
	// contradictedReprobe bounds how long a paid NOT-an-orb verdict is trusted before
	// the hub pays for it again. It must stay under identityTTL: identityTTL ages any
	// cached answer out, and an aged-out negative leaves the descriptor's claim as the
	// only answer on file, which resurrects the cable the operator just watched drop.
	// Re-asking inside the window keeps the verdict true instead of letting it expire
	// into a guess.
	contradictedReprobe = 10 * time.Minute

	// launchProbeTries bounds how long the launch sweep waits for the first
	// enumeration. A host whose sysfs read fails once has a bench nobody re-read, not
	// an empty bench, and the sweep must not start from the previous run's cable list.
	launchProbeTries = 3

	// failedRetry bounds how long the hub backs off a port whose probe never reached a
	// chip. A wedged console or a foreign process holding the lock is not a board that
	// is slow to answer, so the retry is patient; it is not forever, because a lead that
	// came loose and re-seated cleanly has to become askable again without a restart.
	// It is the FIRST gap only: identityCache.nextAsk doubles it per consecutive miss.
	failedRetry = 60 * time.Second

	// failedRetryMax caps that doubling. A link that is genuinely broken — a flaky
	// USBIP stub, a tired hub — answers no probe at any interval, and each unanswered
	// attempt is still a reset pulsed into the board. Ten minutes is patient enough to
	// stop being a storm and short enough that a lead which comes back is noticed
	// inside a coffee break.
	failedRetryMax = 10 * time.Minute

	// launchProbeWait is the gap between those attempts. It is short because the
	// alternative is a hub that boots a card nobody has identified.
	launchProbeWait = 2 * time.Second

	// selfRebootWindow bounds how long a board's silence is excused as the install it was
	// told to do. The firmware acks a store before performing it and reboots to draw it, so
	// the deaf span covers the copy, the restart, and the rejoin: orb-B's measured store of a
	// 3.6 MB theme took 32 s to ack plus 11 s dark, and the same door on a slower card runs
	// longer. A minute is the least window that covers a store and a rejoin on any bench, and
	// it stays far inside identityTTL, so a board that really was reflashed abroad is still
	// asked about on the next sweep rather than excused indefinitely.
	selfRebootWindow = 90 * time.Second
)

// probeCablesAtLaunch asks every attributable cable on the bench what it is running,
// once, after the hub starts serving. Serial on purpose, matching proveCables: one chip
// reset at a time on a bench, and a wedged console must not stack a goroutine per port.
//
// It runs in a goroutine and never blocks a render. The first paint therefore shows
// whatever the address book remembered, and one repaint later the rows carry the
// bench's own answers — including the one that matters, a board that is no longer an
// orb and has to stop being drawn as one.
func (s *Server) probeCablesAtLaunch(ctx context.Context) {
	if _, ok := s.launchSights(ctx); !ok {
		return
	}
	if n := s.identifyCables(ctx); n > 0 {
		log.Printf("orb-manager: identified %d board(s) on a lead at launch", n)
	}
}

// launchSights is the sweep's view of the bench, retried until the host actually

// enumerated. It reads the RAW sighting rather than the contradicted view on purpose:
// the contradicted view exists to downgrade a claim the hub already holds evidence
// against, and the whole job here is to go and get that evidence.
func (s *Server) launchSights(ctx context.Context) ([]cableSighting, bool) {
	for try := 0; try < launchProbeTries; try++ {
		if ctx.Err() != nil {
			return nil, false
		}
		s.cables.force()
		seen, cerr := s.cables.refresh(time.Now())
		if cerr == "" {
			return seen, true
		}
		if try == launchProbeTries-1 {
			log.Printf("orb-manager: launch identify skipped, the bench would not enumerate: %s", cerr)
			return nil, false
		}
		select {
		case <-ctx.Done():
			return nil, false
		case <-time.After(launchProbeWait):
		}
	}
	return nil, false
}

// cableNeedsProbe is the gate on resetting a chip nobody asked about, and the answer to
// "what does the hub owe this lead". Every clause is a reason the question is already
// answered, unsafe, or rude. It reports the reason it said yes so the caller can log the
// reboot it is about to cause: a hub that resets boards for reasons it never states is
// how a periodic sweep reads as a haunting.
//
// The policy is deliberately asymmetric between the two kinds of answer on file:
//
//   - nothing on file, whether never asked, voided by a departure, or only ever answered
//     by a free PING: the hub knows what a MAC implies plus what it overheard, which is
//     the guess this whole file exists to replace, so it asks;
//   - a fresh answer that the lead runs an orb: never re-paid on a clock. The free facts
//     corroborate it continuously — identityCache.corroborate is that sentence made
//     true, refreshed by every liveness dial that names this chip — so a board answering
//     its own door on the LAN ages out only when it stops answering;
//   - an answer that the lead does NOT run an orb: re-paid inside contradictedReprobe,
//     because that verdict is exempt from the cache TTL and would otherwise become a
//     permanent claim the hub never tests again;
//   - a probe that never reached a chip: an absence of evidence, retried on a backoff
//     that doubles (identityCache.nextAsk) rather than treated as a verdict or dropped.
//
// A yes here is not yet a reset. The caller (identifyCables) puts one question in front
// of this one — lanProof, the board's own status door — and pulsed the lead only if that
// comes back silent, foreign, or naming a different chip. So the answer a LAN dial filed
// (provenLan) reads here as a positive on file that ages on the ordinary TTL: it is not
// the reboot-paid answer this gate refuses to re-ask on a clock, and it is not the free
// PING it refuses to trust at all, because unlike a PING it cannot survive a reflash out
// of gorb-os.
func (s *Server) cableNeedsProbe(c cableSighting) (bool, string) {
	// Only a chip the host can attribute to a device, and only one whose serial it read:
	// an answer filed against no MAC describes nobody. The gate takes any Espressif chip
	// rather than only one enumerating as an orb, which is what makes a board in its
	// downloader or mid boot-loop a question the hub asks unprompted instead of one the
	// operator has to click for.
	if !(c.isOrb || c.espressif) || c.mac == "" {
		return false, ""
	}
	// Something is writing that wire — a flash, a console, or an install the operator
	// started from the Theme card (theme.go claims the port for exactly this).
	if _, busy := s.ops.busy(c.port); busy {
		return false, ""
	}
	// And the hub is not still backing off a question this same lead failed to answer.
	// This clause is what stops a disturbed USBIP path from turning one unlucky probe
	// into a reset per sweep: a port that will not sync today will not sync in five
	// seconds, and every attempt spends a reboot to learn that again.
	if wait := s.identities.nextAsk(c.port, time.Now()); wait > 0 {
		return false, ""
	}

	info, at, proven, present := s.identities.cachedVerdict(c.port)
	if !present {
		// A board the hub just rebooted itself is not a board with no answer on file.
		// USB-Serial-JTAG re-enumerates when the chip restarts, the port vanishes from the
		// host's list for a few seconds, and the departure reaper (identity.go's all)
		// deletes the entry it was holding. The answer is gone, the chip is the same, and
		// the gate's own rule for a missing answer is to pay a probe — which pulses the lead
		// again, re-enumerates it again, and deletes the fresh answer again. That loop is
		// why one install cost several resets rather than the one it was promised. Until a
		// restart the hub asked for has finished and been answered, this lead is not owed a
		// question it cannot use.
		if s.selfRebootExcused(c.mac) {
			return false, ""
		}
		// Nothing on file — never asked, or voided by a departure. This is the case that
		// overrides a lonely mark. That mark is a verdict the hub paid for at some point
		// and is exempt from the TTL so it never decays, which means the only thing that
		// can retire it is a new question; and a board the operator has just plugged in
		// is a new question by definition, since it may have been reflashed while it sat
		// off the bench. The mark still stands until the answer overturns it, and an
		// answer always arrives here: foldProbeIdentity clears the mark when the chip
		// speaks as an orb.
		return true, "no answer on file for this cable"
	}
	// A wired dial that just went quiet is worth a reset even when the cache says the
	// answer is still good, because the answer being stale is precisely what the silence
	// suggests. This is the only rule that outranks freshness, and it is spent on arrival.
	// The one silence it must not buy is the one the hub caused: the flag can have been set
	// a beat before the install was sent, and a pulse then lands inside the store's staged
	// window and tears the install. The excuse is consulted here for that race, and only
	// for that race — a silence the hub did not cause still costs exactly one reset.
	if s.owed[c.mac] {
		if s.selfRebootExcused(c.mac) {
			return false, ""
		}
		return true, "the orb stopped answering the address it was seen at"
	}
	if proven == provenFailed {
		return true, "the last probe never reached a chip"
	}
	// A cable proof is the hub hearing a PING come back, not the hub reading a chip. It
	// proves the board answers to an orb, and it is the exact evidence that survives a
	// reflash out of gorb-os being half-finished, so it never excuses the paid question.
	if proven == provenCable {
		return true, "only the free PING has answered, which survives a partial reflash"
	}
	// A negative verdict about a different chip answers nothing about this one: the board
	// it was about has left the bench, and a lead with a stranger on it is owed a question
	// however the stranger was labelled.
	if !info.IsOrb && info.MAC != "" && !sameChip(info.MAC, c.mac) {
		return true, "the verdict on file names a different chip than this cable"
	}
	// A fresh paid answer that is still doing its job is the only thing a lonely mark has
	// to stand on, so the mark is consulted here rather than above: it protects a board
	// from being reset on a timer, and it must not also protect itself from ever being
	// tested. Ordering matters and this is the whole reason — a not-an-orb answer is
	// TTL-exempt, so a mark checked against mere presence would be unfalsifiable forever.
	if s.lonely != nil && s.lonely.has(c.mac) && !verdictDue(at, info) {
		return false, ""
	}
	if !verdictDue(at, info) {
		return false, ""
	}
	if info.IsOrb {
		return true, "the paid answer aged out without a free fact to renew it"
	}
	return true, "a not-an-orb verdict aged out of the window it is trusted for"
}

// verdictDue is the point of no longer believing an answer enough to leave a chip alone:
// a positive that has expired with the cache, or a negative that has outlived the window
// the hub trusts it for.
func verdictDue(at time.Time, info esprom.OrbInfo) bool {
	if info.IsOrb {
		return time.Since(at) > identityTTL
	}
	return time.Since(at) >= contradictedReprobe
}

// sameChip answers whether two spellings of a hardware address name the same board.
//
// The two views are spelled differently by design: a bootloader prints colon-separated
// upper case and the cable sweep stores the fleet's canonical twelve bare digits, so a
// naive comparison rejects every pair and the guard it feeds silently never re-asks
// anything. An address that will not canonicalize names no board, which is a mismatch.
func sameChip(a, b string) bool {
	ca, why := orbmdns.CanonicalMAC(a)
	if why != "" {
		return false
	}
	cb, why := orbmdns.CanonicalMAC(b)
	if why != "" {
		return false
	}
	return ca == cb
}

// lanProof asks the free question, files the answer, and reports whether the hub may
// leave this lead alone. It runs between the gate deciding a reset is owed and the
// reset being pulsed, so it costs a dial only on a lead the hub had already decided to
// reboot — never on a board whose answer is fresh, and never twice inside one TTL.
//
// The question it asks is the one the reboot was going to buy: does this chip run
// gorb-os. /api/v1/status answers only on gorb-os, so a reply is that answer. What it
// is not is a description of the *cable*: two boards can sit at one address, and a
// board with no lease can be dialled at somebody else's. So the reply must name the
// MAC the host read off the descriptor, the one fact that ties the network answer to
// this lead. A stranger's orb answering here proves nothing about this board, and the
// reset stays owed.
//
// Three refusals fall through to the paid probe rather than filing anything:
//   - no row, or a row with no address: an AP-isolated board has no lease to dial, and
//     a chip the hub has never met has no claim to be believed about;
//   - a door that will not answer: silence is a boot-loop as often as it is a reflash,
//     and only the bootloader console tells those apart;
//   - a door that names a different chip.
//
// On a refusal this function writes nothing at all. An unanswered LAN question is an
// absence of evidence, and filing it would let the absence speak as a verdict.
//
// It returns (proved, deferred). deferred is the fourth refusal with a different
// consequence: the hub's own bus declined the dial because a transfer to this orb is
// mid-flight (ErrLaneBusy). That orb answered this hub minutes ago at worst — the
// in-flight request IS this hub's request — so the probe stays owed and NO reset is
// pulsed this cycle; the debt re-asks on the next one. Confusing queue with silence
// was how a hub could reboot its own board for holding a thumbnail the operator asked
// for, which the 45 s preview stall of 2026-10-26 made reachable.
func (s *Server) lanProof(c cableSighting) (bool, bool) {
	if s.lanDial == nil || s.identities == nil || c.mac == "" {
		return false, false
	}
	// fleetKnown, not a raw row read: it refuses a board this hub left lonely, whose
	// firmware has no Wi-Fi to answer on, and it is the same question the card asks.
	// The address is the fleet's current one, which mDNS keeps honest — a board whose
	// lease moved announces the new address and the row follows the MAC — so a stale
	// entry in the file is not what gets dialled.
	row, known := s.fleetKnown(c.mac)
	if !known || row.IP == "" || softAPAddr(row.IP) {
		return false, false
	}
	// Claim the lead for the length of the question. The dial opens no port, but the
	// answer it files does: record releases the claim a probe holds, so a door answer
	// landing during a manual Identify would free the lead for a second reset while the
	// first was still reading the console. A question that costs nothing can wait for
	// one that costs a reboot.
	if !s.identities.begin(c.port) {
		return false, false
	}
	st, err := s.lanDial(row.IP)
	if errors.Is(err, orbnet.ErrLaneBusy) {
		// Declined by the bus, not by the board: keep the debt, do not pulse,
		// and say so — a queue stall that produces a silent `continue` reads
		// later like a probe that was never attempted.
		log.Printf("orb-manager: %s holds our bus mid-transfer, so the %s probe stays owed rather than being paid in reset",
			row.IP, c.port)
		s.identities.release(c.port)
		return false, true
	}
	if err != nil || st == nil {
		s.identities.release(c.port)
		return false, false
	}
	if !sameChip(st.MAC, c.mac) {
		log.Printf("orb-manager: %s answers %s as chip %q, not the board on this lead; the probe stays owed",
			row.IP, c.port, st.MAC)
		s.identities.release(c.port)
		return false, false
	}
	// doneLan records the answer, and record is what gives the claim back.
	s.identities.doneLan(c.port, esprom.OrbInfo{
		Port:    c.port,
		IsOrb:   true,
		MAC:     c.mac,
		Version: st.FW,
		State:   st.State,
		Name:    st.Name,
		IDF:     st.IDF,
		IP:      row.IP,
		Reason:  "answered /api/v1/status at " + row.IP + ": no reset needed",
	})
	// The facts the door carried belong on the row as much as on the port: fw, state
	// and the board's own name, folded and persisted by the same routine the ten-second
	// sweeper uses, so the card and the state file cannot disagree about who said what.
	s.foldProbeStatus(row.IP, st)
	log.Printf("orb-manager: %s is an orb running %s over the LAN, so %s keeps its cable without a reset",
		c.mac, st.FW, c.port)
	return true, false
}

// probeCableNow pays one reboot and files the answer. It claims the cable exactly as the
// Identify door does — registered as opProbe, holding orbbind's cross-process lock — so
// a flash started mid-sweep cancels this probe at its next block boundary and takes the
// cable (see opRegistry.start: a write press evicts an identify rather than queueing
// behind a sweep that never stops re-arming), and /api/ops says what was on the wire.
//
// It reports whether the hub learned anything. An error is filed as a reason on the port
// rather than dropped: a board that will not answer to a probe is information the card
// should show, and the reason it is missing.
//
// why is the rule that decided to ask, and it is logged before the reset is pulsed
// rather than after. The hub is taking something the operator can see — the glass goes
// blank for a few seconds — on a rule the operator cannot see; the log is the only place
// those two halves can be read together. A probe that dies partway leaves the reason
// behind it, which is the difference between "the hub did this and said why" and a board
// that rebooted itself for nothing.
func (s *Server) probeCableNow(ctx context.Context, c cableSighting, why string) bool {
	log.Printf("orb-manager: probing %s (chip %s): %s", c.port, c.mac, why)
	if !s.identities.begin(c.port) {
		return false
	}
	job, pctx, oerr := s.ops.start(ctx, c.port, opProbe, probeBudget)
	if oerr != nil {
		log.Printf("orb-manager: probe of %s abandoned before the reset: %s", c.port, oerr)
		s.identities.doneFailed(c.port, oerr.Error())
		return false
	}
	defer s.ops.finish(job)

	lock, lerr := orbbind.LockPort(c.port)
	if lerr != nil {
		log.Printf("orb-manager: probe of %s abandoned before the reset: the port is held (%s)",
			c.port, lerr)
		s.identities.doneFailed(c.port, "port busy: "+lerr.Error())
		return false
	}
	// The courtesy window before the pulse, not after. This probe's own reboot
	// re-enumerates the chip, and a departure voids the cached answer (roster.go's
	// forgetCablePorts): without this stamp the gate's next look reads the deletion as
	// "never asked" and pays a second reboot the first never got to answer for. The
	// window the paid door consults (selfRebootExcused, the !present branch of the gate
	// above) only protects a reboot somebody announced, and until here nobody announced
	// this one — which is how one sanctioned identify cost several resets on the bench.
	s.expectSelfReboot(c.mac)
	info, err := s.probeOrb(pctx, c.port)
	lock.Release()
	if err != nil {
		log.Printf("orb-manager: probe of %s reached no chip: %s", c.port, err)
		s.identities.doneFailed(c.port, err.Error())
		return false
	}
	// The verdict in the same breath as the reason, so one line is enough to judge
	// whether the reboot was worth it.
	if info.IsOrb {
		log.Printf("orb-manager: probe of %s answered an orb running %s%s", c.port,
			info.Project, versionWord(info.Version))
	} else {
		log.Printf("orb-manager: probe of %s answered NOT an orb: %s", c.port, info.Reason)
	}
	s.identities.done(c.port, info)
	s.foldProbeIdentity(info)
	// Re-arm the courtesy window when the pulse is DONE, not only before it. The probe
	// spends up to ten seconds talking the ROM handshake off the front of the window,
	// and a board pulled into download and released into its app needs a full cold boot
	// — through DHCP — before its dial answers again. A window measured from the start
	// of the handshake is short by the handshake, and the liveness sweeper hits that
	// deficit inside its first ten-second tick: the silence the hub caused flags the
	// board, and the flag buys a fresh reset. One reboot per hub start only means one,
	// so the clock starts when the wire lets go.
	s.expectSelfReboot(c.mac)
	return true
}

// versionWord keeps a probe log line to one line: " v1.4.0" when there is a stamp, ""
// when the bootloader did not print one. A blank here is a fact, not a formatting bug,
// and printing "v" alone would read as a truncated version.
func versionWord(v string) string {
	if v == "" {
		return ""
	}
	return " " + v
}

// identifyCables asks every attributable lead the hub owes a question to, and is the body
// the launch sweep and the watcher tick share. It is serial on purpose, matching
// proveCables: one chip reset at a time on a bench, and a wedged console must not stack a
// goroutine per port.
//
// Running it on the tick rather than only at launch is what makes a re-plug always
// re-ask. A departure voids the cached answer (roster.go's forgetCablePorts), so the
// returning board is a sighting with nothing on file, which the gate above calls owed.
// That is the operator's own signal — pulling a cable is how you say "look again" — and it
// outranks every caching rule in the hub, because the chip the verdict was about was
// physically off the bench in the interval and may have been reflashed while it was.
//
// The pass still outranks every caching rule in the hub — the gate is consulted before
// any exemption, and no filed answer hides a board from the question. What changed is
// what the question costs. A re-plugged board answers it over the LAN if it can: back on
// the bench, on its lease, quoting its own MAC through /api/v1/status, it is filed as an
// orb within one sweep tick and pulsed never. The board that cannot answer — off the
// network, in its downloader, or written over with something that serves no such door —
// is the one the reboot is for, and it pays within the same tick. It asks nothing twice:
// a filed answer of either kind takes the lead out of the owed set.
func (s *Server) identifyCables(ctx context.Context) int {
	if s.probeOrb == nil || s.cables == nil || s.identities == nil {
		return 0
	}
	seen, cerr := s.cables.refresh(time.Now())
	// A sweep that could not enumerate is a bench nobody re-read. Working from the last
	// known set would reset whatever took the place of a board that left.
	if cerr != "" {
		return 0
	}
	// Reap before deciding, exactly as the port listing does. cachedVerdict reports raw
	// presence so the gate can tell "never asked" from "answered and no longer true", and
	// that distinction only works if entries that stopped being true are gone first. A hub
	// serving no browser reaps nothing on its own, so without this a thirty-minute-old
	// positive answer blocks the question indefinitely on a headless bench.
	ports := make([]string, 0, len(seen))
	for _, c := range seen {
		ports = append(ports, c.port)
	}
	s.identities.all(ports...)

	// Taken once per pass and hung on the server so the gate can see it, rather than
	// passed as an argument: every port asked this round, however the question ended, has
	// its flag spent before the next pass looks at it.
	flags := s.takeSuspicious()
	s.owed = flags
	defer func() { s.owed = nil }()
	if len(flags) > 0 {
		defer s.consumeSilenceFlags(seen, flags)
	}

	var asked int
	for _, c := range seen {
		if ctx.Err() != nil {
			break
		}
		ask, why := s.cableNeedsProbe(c)
		if !ask {
			continue
		}
		// The gate owes this lead a question. Ask it over the network first: a board
		// that answers its own door as this MAC has already said what the reset was
		// going to hear, and a hub that pulsed anyway would be paying eight seconds of
		// blank display for a fact it was holding. Silence here is not a refusal to
		// answer — it is the answer to the harder question, so the probe goes ahead.
		proved, deferred := s.lanProof(c)
		if proved {
			asked++
			continue
		}
		if deferred {
			// The bus is mid-transfer with this orb. The debt survives to the
			// next sweep; the reset stays holstered.
			continue
		}
		if s.probeCableNow(ctx, c, why) {
			asked++
		}
	}
	if asked > 0 {
		s.wakeOrbs()
	}
	return asked
}

// noteWiredSilence records that a board the hub can see on a lead failed the HTTP door it
// is remembered answering, and is the trigger that makes the identity button unnecessary
// for the one case no physical event reveals: a plugged-in board reflashed with esptool or
// orbflash, its cable never disturbed.
//
// Nothing about that board changes on the USB side. Its descriptor still matches, its MAC
// still matches, and its last paid answer still says orb — so every cache rule in the hub
// says do not ask again, while the thing the answer was about is exactly what just changed.
// The address book does change: firmware that is not gorb-os has no /api/v1/status, so the
// dial that used to answer stops answering.
//
// The inference is deliberately weak, because the same silence comes from a boot-loop, a
// DHCP renewal, and a yanked network cable. It is therefore never a verdict: it cannot
// strip a cable, de-claim a fleet member, or arm a Forget gate. It only flags that the one
// question which does separate those cases — read the bootloader — is now worth a reboot,
// and the paid answer settles it either way. An orb that was merely blipping comes back as
// an orb and the flag is spent.
func (s *Server) noteWiredSilence(ip string) {
	if s.fleet == nil || ip == "" || s.cables == nil {
		return
	}
	for _, r := range s.fleet.Every() {
		if r.IP != ip || r.MAC == "" {
			continue
		}
		if s.cables.cablePortFor(r.MAC, time.Now()) == "" {
			continue
		}
		// The hub caused this silence. A store the operator started is acked before the
		// device performs it and reboots the board to draw it, and the dial that just
		// failed is exactly what that restart looks like from out here. Flagging it buys
		// a pulse that lands inside the store's staged window and tears the install the
		// hub itself asked for, which is the whole of the reboot-loop-and-lose-the-theme
		// report. The excuse is spent on arrival, and it never outranks a real answer:
		// a board that comes back speaking still renews its own clock as usual.
		if s.selfRebootExcused(r.MAC) {
			log.Printf("orb-manager: %s is quiet because it is installing what the hub sent; no probe owed", ip)
			return
		}
		// The hub holds positive disproof that this chip is an orb, so its silence at
		// the address is the expected answer and not a question. The flag below exists
		// to buy the one read that separates a reflash from a boot-loop, and the paid
		// probe already delivered that read: the bootloader named the chip and said NOT
		// an orb. Re-arming here therefore purchases a repeat of a fact on hand, and it
		// repeats forever, because the row keeps the dead address the sweep dials and
		// every failed tick re-lights the flag. Measured on the bench, 2026-10-26: a
		// lonely board on /dev/ttyACM1 took eight hard resets in eleven minutes, each
		// one logged with the gate's honest reason, "the orb stopped answering the
		// address it was seen at". The board never announced from that address — it runs
		// a stub that serves no door. A contradiction retires the escalation; the row
		// keeps its dead address on the card as the refusal the operator can read, and
		// one Forget press ends it now that a disproven chip clears the Forget gate.
		if (s.lonely != nil && s.lonely.has(r.MAC)) || s.provenNotOrb(r.MAC) {
			log.Printf("orb-manager: %s is quiet because %s is proven not to run gorb-os; no probe owed", ip, r.MAC)
			return
		}
		s.suspMu.Lock()
		if s.suspicious == nil {
			s.suspicious = map[string]bool{}
		}
		s.suspicious[macNorm(r.MAC)] = true
		s.suspMu.Unlock()
		return
	}
}

// expectSelfReboot records that the hub just sent this chip something it will restart to
// apply, for as long as that restart plausibly takes. Callers are the theme doors, which
// are the only ones that reboot a board without the operator pressing Reset.
func (s *Server) expectSelfReboot(mac string) {
	if s.expectedSelfReboot == nil {
		s.expectedSelfReboot = map[string]time.Time{}
	}
	s.expectMu.Lock()
	s.expectedSelfReboot[macBare(mac)] = time.Now().Add(selfRebootWindow)
	s.expectMu.Unlock()
}

// selfRebootExcused reports whether this chip's silence is still covered by a store the
// hub asked for. It does NOT consume: the cable sweep runs every five seconds and three
// separate rules consult the excuse, so a consuming check would be spent by whichever
// looked first and leave the other two to pulse a board that is still rebooting. The
// excuse ends when the board is heard from again (clearSelfReboot, off the liveness dial
// that proves it is alive and still an orb) or when the window lapses, whichever is first.
// It keys on macBare, not macNorm: the callers hold a colon form from a grant on one side
// and a bare form from a fleet row or a cable sighting on the other, and a key that
// preserved the separator would let the two miss each other — the same class of miss that
// made the port lookup fail, and the reason that guard did nothing on a real bench.
func (s *Server) selfRebootExcused(mac string) bool {
	key := macBare(mac)
	s.expectMu.Lock()
	defer s.expectMu.Unlock()
	deadline, ok := s.expectedSelfReboot[key]
	if !ok {
		return false
	}
	if time.Now().After(deadline) {
		delete(s.expectedSelfReboot, key)
		return false
	}
	return true
}

// clearSelfReboot retires the silence excuse once the chip has proven itself alive. The
// same free dial that corroborates the identity verdict is what shows the install finished,
// so the two retire together and a later silence is judged on its own again.
func (s *Server) clearSelfReboot(mac string) {
	s.expectMu.Lock()
	delete(s.expectedSelfReboot, macBare(mac))
	s.expectMu.Unlock()
}

// suspiciousPorts snapshots and clears the escalation flags, so one silence episode costs
// at most one reset. Without the consume the flag would survive a failed probe and turn a
// wedged board into a reboot loop every ten seconds, which is the same trap the free PING
// path escapes by refusing to treat silence as evidence.
func (s *Server) takeSuspicious() map[string]bool {
	s.suspMu.Lock()
	defer s.suspMu.Unlock()
	out := s.suspicious
	s.suspicious = nil
	return out
}

// consumeSilenceFlags drops the escalation for every attributable lead the pass looked at,
// whether or not it reset that lead. A flag that survives an examined port is a flag that
// will make the next pass reset the same board for the same old silence, and a board whose
// network cable is loose fails its dial forever.
func (s *Server) consumeSilenceFlags(seen []cableSighting, flags map[string]bool) {
	for _, c := range seen {
		if c.mac != "" {
			delete(flags, macNorm(c.mac))
		}
	}
}
