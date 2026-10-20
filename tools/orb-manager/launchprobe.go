package main

import (
	"context"
	"log"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
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
// descriptor. This costs each board on a lead one reboot per hub launch, which is the
// trade the operator asked for out of the two states the hub can be wrong about. A
// board on a lead the hub cannot attribute is never touched: EnterDownloadMode pulses
// DTR/RTS, and doing that to an Arduino on the same bench resets a device nobody asked
// about.

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
	failedRetry = 60 * time.Second

	// launchProbeWait is the gap between those attempts. It is short because the
	// alternative is a hub that boots a card nobody has identified.
	launchProbeWait = 2 * time.Second
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
// answered, unsafe, or rude.
//
// The policy is deliberately asymmetric between the two kinds of answer on file:
//
//   - nothing on file, whether never asked, voided by a departure, or only ever answered
//     by a free PING: the hub knows what a MAC implies plus what it overheard, which is
//     the guess this whole file exists to replace, so it asks;
//   - a fresh answer that the lead runs an orb: never re-paid on a clock. The free facts
//     corroborate it continuously, and an orb on a lead has no reason to be reset every
//     half hour for a version string the row already carries;
//   - an answer that the lead does NOT run an orb: re-paid inside contradictedReprobe,
//     because that verdict is exempt from the cache TTL and would otherwise become a
//     permanent claim the hub never tests again;
//   - a probe that never reached a chip: an absence of evidence, retried after a
//     backoff rather than treated as a verdict or dropped silently.
func (s *Server) cableNeedsProbe(c cableSighting) bool {
	// Only a chip the host can attribute to a device, and only one whose serial it read:
	// an answer filed against no MAC describes nobody. The gate takes any Espressif chip
	// rather than only one enumerating as an orb, which is what makes a board in its
	// downloader or mid boot-loop a question the hub asks unprompted instead of one the
	// operator has to click for.
	if !(c.isOrb || c.espressif) || c.mac == "" {
		return false
	}
	// Something is writing that wire.
	if _, busy := s.ops.busy(c.port); busy {
		return false
	}

	info, at, proven, present := s.identities.cachedVerdict(c.port)
	if !present {
		// Nothing on file — never asked, or voided by a departure. This is the case that
		// overrides a lonely mark. That mark is a verdict the hub paid for at some point
		// and is exempt from the TTL so it never decays, which means the only thing that
		// can retire it is a new question; and a board the operator has just plugged in
		// is a new question by definition, since it may have been reflashed while it sat
		// off the bench. The mark still stands until the answer overturns it, and an
		// answer always arrives here: foldProbeIdentity clears the mark when the chip
		// speaks as an orb.
		return true
	}
	// A wired dial that just went quiet is worth a reset even when the cache says the
	// answer is still good, because the answer being stale is precisely what the silence
	// suggests. This is the only rule that outranks freshness, and it is spent on arrival.
	if s.owed[c.mac] {
		return true
	}
	if proven == provenFailed {
		return time.Since(at) >= failedRetry
	}
	// A cable proof is the hub hearing a PING come back, not the hub reading a chip. It
	// proves the board answers to an orb, and it is the exact evidence that survives a
	// reflash out of gorb-os being half-finished, so it never excuses the paid question.
	if proven == provenCable {
		return true
	}
	// A negative verdict about a different chip answers nothing about this one: the board
	// it was about has left the bench, and a lead with a stranger on it is owed a question
	// however the stranger was labelled.
	if !info.IsOrb && info.MAC != "" && !sameChip(info.MAC, c.mac) {
		return true
	}
	// A fresh paid answer that is still doing its job is the only thing a lonely mark has
	// to stand on, so the mark is consulted here rather than above: it protects a board
	// from being reset on a timer, and it must not also protect itself from ever being
	// tested. Ordering matters and this is the whole reason — a not-an-orb answer is
	// TTL-exempt, so a mark checked against mere presence would be unfalsifiable forever.
	if s.lonely != nil && s.lonely.has(c.mac) && !verdictDue(at, info) {
		return false
	}
	return verdictDue(at, info)
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

// probeCableNow pays one reboot and files the answer. It claims the cable exactly as the
// Identify door does — registered as opProbe, holding orbbind's cross-process lock — so
// a flash started mid-sweep waits rather than collides, and /api/ops can say what is on
// the wire.
//
// It reports whether the hub learned anything. An error is filed as a reason on the port
// rather than dropped: a board that will not answer to a probe is information the card
// should show, and the reason it is missing.
func (s *Server) probeCableNow(ctx context.Context, c cableSighting) bool {
	if !s.identities.begin(c.port) {
		return false
	}
	job, pctx, oerr := s.ops.start(ctx, c.port, opProbe, probeBudget)
	if oerr != nil {
		s.identities.doneFailed(c.port, oerr.Error())
		return false
	}
	defer s.ops.finish(job)

	lock, lerr := orbbind.LockPort(c.port)
	if lerr != nil {
		s.identities.doneFailed(c.port, "port busy: "+lerr.Error())
		return false
	}
	info, err := s.probeOrb(pctx, c.port)
	lock.Release()
	if err != nil {
		s.identities.doneFailed(c.port, err.Error())
		return false
	}
	s.identities.done(c.port, info)
	s.foldProbeIdentity(info)
	return true
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
// The pass costs a re-plugged board one reboot within one sweep tick of going back in, and
// a board that was reflashed away from the hub the same, which is the delay accepted for
// that case. It asks nothing twice: a filed answer takes the lead out of the owed set.
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
		if !s.cableNeedsProbe(c) {
			continue
		}
		if s.probeCableNow(ctx, c) {
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
		s.suspMu.Lock()
		if s.suspicious == nil {
			s.suspicious = map[string]bool{}
		}
		s.suspicious[macNorm(r.MAC)] = true
		s.suspMu.Unlock()
		return
	}
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
