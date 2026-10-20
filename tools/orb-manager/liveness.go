package main

// Forget, armed the minute a board is gone.
//
// Forget drops the hub's row for one MAC. It does nothing to the board. So the button
// is only honest when the board cannot hand the hub a new reason to list it, and there
// are exactly three ways that happens: the current USB sweep attributes a port to that
// MAC, the board announced itself inside the last minute, or its remembered address
// still answers an HTTP dial. While any is true, Forget hides a row for one check-in
// and teaches the operator that the button does nothing.
//
// The clock behind the announce rule is the time the browser LAST HEARD a packet, never
// the time a card rebuild re-folded a cached record — fleet.go's orbmdnsToObservation
// carries Candidate.LastSeen into the store for exactly that reason. A presence clock
// the hub winds from its own cache is a gate that never opens, which is what this rule
// looked like before.
//
// The dial verdict is kept honest the same way: swept every liveSweepEvery (ten seconds,
// a sixth of an announce interval, one bounded fan-out over every remembered address),
// trusted for liveVerdictStale, and an address with no fresh verdict is neither alive
// nor dead — the render path says "the hub is asking" and the door pays for the dial
// itself, once per press, and acts on the fresh answer. A board that leaves the bench by
// USB gets the same treatment without waiting a tick: roster.go's sweep diff dials its
// remembered address the second the lead goes quiet, so a board that moved to the LAN
// keeps its row by verdict and a board that is gone starts its minute-timer toward
// Forget immediately. Both the button and the door call forgetGate, so they cannot
// disagree about the rule; the endpoint is the authority and the button is a courtesy.

import (
	"context"
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
)

const (
	// liveSweepEvery is how often the sweeper re-asks every remembered address. Ten
	// seconds is a sixth of an announce interval: a board unplugged from the LAN is
	// called dead within one tick, so the minute the Forget gate opens, the button is
	// already armed. Each board is asked exactly once per tick and the fan-out is
	// bounded, so the traffic stays one GET per orb per ten seconds.
	liveSweepEvery = 10 * time.Second

	// liveVerdictStale is how much a verdict is trusted after it was paid for: six
	// sweeps' worth. A cached alive must not hold the gate shut for minutes after the
	// board died, and a laptop that woke from sleep re-asks before it re-answers. Past
	// this the answer is "unknown", which the gate fail-closes to "the hub is asking".
	liveVerdictStale = 45 * time.Second

	// liveProbeWorkers bounds how many addresses a fan-out dials at once, at startup
	// and on every tick alike. It is small because the point is latency, not
	// throughput: eight boards is a normal bench, and a hub that opened every socket
	// at once would measure its own network rather than the boards.
	liveProbeWorkers = 8

	// liveAnnounceGrace is how recent an mDNS check-in has to be to count as
	// "connected", and so it is exactly the Forget rule the operator can read on the
	// card and wait out: one minute quiet, and the button arms unless the address
	// verdict or a cable says otherwise. It is one full MDNS_ADV_REFRESH_S, so a board
	// that lost one packet is refused by the ten-second address sweep rather than by a
	// clock nobody can check.
	liveAnnounceGrace = 60 * time.Second
)

// liveMark is one address as the hub last saw it. There is no error field on purpose:
// every failure to get an orb-shaped answer is the same answer to this question, which
// is "did the board at this address just not prove itself".
type liveMark struct {
	alive bool
	at    time.Time
}

// liveCache holds the per-address verdicts. probe is nil until main wires it, exactly
// like the cable sweep, so a test hub dials nothing it did not ask for and a hand-built
// Server reports every address as unknown rather than pretending to have asked.
type liveCache struct {
	mu    sync.Mutex
	byIP  map[string]liveMark
	probe func(ip string) bool
	stale time.Duration
}

func newLiveCache() *liveCache { return &liveCache{stale: liveVerdictStale} }

// verdict reports what the hub last learned about ip, and whether that is still a
// verdict at all. known=false covers both "never asked" and "asked too long ago", and a
// caller must not read it as "not answering" — that is why forgetGate asks.
func (c *liveCache) verdict(ip string, now time.Time) (alive, known bool) {
	if c == nil || ip == "" {
		return false, false
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	m, ok := c.byIP[ip]
	if !ok || m.at.IsZero() || now.Sub(m.at) > c.stale {
		return false, false
	}
	return m.alive, true
}

// record files a verdict and says whether it changed the answer, which is what decides
// whether an open stream needs waking.
func (c *liveCache) record(ip string, alive bool, at time.Time) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.byIP == nil {
		c.byIP = map[string]liveMark{}
	}
	prev, had := c.byIP[ip]
	c.byIP[ip] = liveMark{alive: alive, at: at}
	return had && prev.alive != alive
}

// check asks about one address now and files the answer. It is the synchronous half: the
// forget door calls it when the cached verdict is unknown, because that door is one dial
// and happens once per button press.
func (c *liveCache) check(ip string) bool {
	if c == nil || c.probe == nil || ip == "" {
		return false
	}
	alive := c.probe(ip)
	c.record(ip, alive, time.Now())
	return alive
}

// probeAll dials every target once, bounded-concurrent, and reports whether any
// verdict moved. The startup burst and the ten-second tick are the same question at
// different speeds, so they share this: serial dials made a dead address in the queue
// hold the whole fleet's answer behind its own timeout, and one dial per board per tick
// is what keeps an orb's single-socket httpd measuring the hub rather than its own
// backlog.
func (c *liveCache) probeAll(ctx context.Context, targets []string, workers int) (flipped bool) {
	if c == nil || c.probe == nil || len(targets) == 0 {
		return false
	}
	if workers <= 0 {
		workers = liveProbeWorkers
	}
	var wg sync.WaitGroup
	var moved int32
	sem := make(chan struct{}, workers)
	for _, ip := range targets {
		if ctx.Err() != nil {
			break
		}
		wg.Add(1)
		sem <- struct{}{}
		go func(ip string) {
			defer wg.Done()
			defer func() { <-sem }()
			before, known := c.verdict(ip, time.Now())
			alive := c.check(ip)
			// A verdict that only aged out rather than changed is not news to the card.
			if !known || before != alive {
				atomic.StoreInt32(&moved, 1)
			}
		}(ip)
	}
	wg.Wait()
	return atomic.LoadInt32(&moved) != 0
}

// annotateReachability settles one verdict per row — Connected — and keeps the
// fields that feed it honest. Every row with an address is asked, not just the
// remembered ones: a board that loses power stops answering within the ten-second
// sweep long before its cached announce ages out, and a tile that kept claiming
// on-LAN across that gap was the card believing its own cache over the wire. The
// row appears at first paint, and one repaint later it says whether the board is
// there — which is the difference between the file being a list of stale strings
// and being the fast start the operator asked for.
//
// A dial answer is NOT folded into the row's presence clock. LastSeen means "announced",
// and a board that answers HTTP without announcing is connected but not broadcasting —
// writing the dial into that field would let a row claim an announce it never sent and
// would make the Forget gate refuse on the hub's own fabrication. The alive address is
// refused by the gate's third condition instead, from the verdict, which is the truth.
func (s *Server) annotateReachability(rows []orbRow, now time.Time) {
	for i := range rows {
		r := &rows[i]
		// A lead into this hub is a present-tense connection, and an announce the
		// browser still lists was paid for by a packet inside its TTL. Either one
		// says connected until something fresher says otherwise.
		r.Connected = r.OnCable || hasStr(r.Via, "lan")
		if r.IP == "" || s.fleet == nil || s.live == nil {
			continue
		}
		alive, known := s.live.verdict(r.IP, now)
		if !known {
			// No fresh verdict is not evidence of absence. A row the wire or the
			// bench is producing keeps the claim it earned; a remembered row says
			// the hub has not asked yet.
			if r.Reachable != "" && !r.OnCable {
				r.Reachable = "unknown"
				r.Connected = false
			}
			continue
		}
		if alive {
			r.Reachable = "alive"
			r.Connected = true
			// Same decision the proxy will make if this row is clicked, so the card
			// cannot promise a dial the request would refuse.
			if may, why := s.mayDial(r.IP); may {
				r.Dialable, r.Refusal = true, ""
				r.addVia("lan")
			} else {
				r.Refusal = why
			}
			continue
		}
		// A fresh dead verdict outranks every cached claim above it. The row keeps
		// its address — a rebooted orb that took a fresh lease re-earns the claim
		// on the next packet or the next dial that answers — and drops the
		// present-tense claim, and Dialable with it: a click here can only fail.
		// A lead into this hub is not an HTTP timeout, though: a board sitting on
		// a cable is connected whatever its stale address does, so OnCable holds
		// the verdict true and only the lan half of the claim goes.
		r.Reachable = "dead"
		r.Connected = r.OnCable
		r.Dialable = false
		r.dropVia("lan")
		r.Refusal = "no answer at " + r.IP + ": this board is not reachable where it was " +
			"last seen, which is why Forget is offered"
	}
}

// hasStr is the one pass a string slice needs; the stdlib's slices.Contains is
// fine, but this file predates its import here and the call reads better than the
// import does.
func hasStr(hay []string, needle string) bool {
	for _, h := range hay {
		if h == needle {
			return true
		}
	}
	return false
}

// forgetGate is the whole rule, called by the row annotation that decides whether to
// draw the button and by the door that decides whether to delete the row. confirm is
// true only at the door: it authorises the synchronous dial when the cached verdict is
// unknown, which is a cost the render path must not take.
func (s *Server) forgetGate(mac string, now time.Time, confirm bool) (bool, string) {
	if strings.TrimSpace(mac) == "" {
		return false, "this row has no mac yet, so there is no fleet entry to forget"
	}
	if port := s.cableOf(mac, now); port != "" {
		return false, "the orb is on a cable at " + port + ": a board the hub can reach over " +
			"USB re-earns its row on the next sweep — open Forget for the reset choices over that lead"
	}
	if s.fleet == nil {
		return true, ""
	}
	row, known := s.fleet.Look(mac)
	if !known {
		// Nothing stored and no objection to say: the door reads this as a no-op, and a
		// row the hub never kept has no address for it to revoke.
		return true, ""
	}
	if !row.LastSeen.IsZero() {
		if quiet := now.Sub(row.LastSeen); quiet <= liveAnnounceGrace {
			return false, row.DisplayName() + " announced itself " + agoWords(quiet) +
				" ago: an orb that is broadcasting rebuilds this row on its next check-in" +
				" — open Forget for the reset choices"
		}
	}
	if row.IP != "" && !softAPAddr(row.IP) {
		alive, wasKnown := s.live.verdict(row.IP, now)
		if !wasKnown {
			// No fresh verdict is not evidence of absence. The render path says so and
			// waits — with the ten-second sweep and the launch fan-out there is a real
			// answer within a tick of every wake — and only the door, where something
			// is about to be deleted, pays for the dial itself. A hub with no probe
			// wired (a test hub, a hidden stream) dials nothing, files nothing, and the
			// door falls through to the other two facts exactly as it always did.
			if !confirm {
				return false, row.IP + " has no fresh verdict: the hub is asking it now " +
					"and will say alive or dead on the next frame"
			}
			alive = s.live.check(row.IP)
		}
		if alive {
			return false, row.IP + " still answers as an orb, so it is connected and the next " +
				"check-in puts the row back — open Forget for the reset choices, both of which " +
				"need the board on a lead of this hub"
		}
	}
	return true, ""
}

// kickLostCables re-prices the addresses of boards that just left the bench. The cable
// sweep is the fastest fact the hub has; without this, an unplug that also cost the
// board its LAN announce would wait on the announce clock and the sweep tick to be
// called dead. This asks the remembered address the second the lead goes quiet: a board
// that moved to Wi-Fi keeps its row on the answer, and one that is gone starts its
// minute-timer toward Forget now. roster() calls it on every successful sweep; the
// first sweep only lays the baseline, and an enumeration failure keeps the previous
// sight on purpose (refresh's rule), so a flaky hub cannot fake an unplug.
func (s *Server) kickLostCables(seen []cableSighting) {
	if s.live == nil || s.live.probe == nil {
		return
	}
	now := make(map[string]bool, len(seen))
	for _, c := range seen {
		if c.isOrb && c.mac != "" {
			now[c.mac] = true
		}
	}
	s.kickMu.Lock()
	prev := s.lastPlug
	s.lastPlug = now
	s.kickMu.Unlock()
	if prev == nil {
		return
	}
	for mac := range prev {
		if now[mac] {
			continue
		}
		if s.fleet == nil {
			continue
		}
		row, ok := s.fleet.Look(mac)
		if !ok || row.IP == "" || softAPAddr(row.IP) {
			continue
		}
		ip := row.IP
		go func() {
			s.live.check(ip)
			s.wakeOrbs()
		}()
	}
}

// cableOf answers "is this board on a lead right now" from the same TTL-cached sweep the
// roster builds rows from, so the button and the row cannot disagree about the cable. Keyed
// on the MAC the host read off the USB descriptor, which is the same key the roster folds
// OnCable on. A host that cannot read the serial cannot key a refusal, and the sweep says
// so as an unattributed row rather than borrowing an old socket path to refuse with.
func (s *Server) cableOf(mac string, now time.Time) string {
	if mac == "" {
		return ""
	}
	if s.lonely.has(mac) {
		return ""
	}
	for _, c := range s.cableSights(now) {
		if c.isOrb && c.mac == mac {
			return c.port
		}
	}
	return ""
}

// cableSights is the one read of the cached sweep the gate depends on. An error is dropped
// on purpose: it means "the hub could not check", and every caller treats an empty list as
// no objection. That is the fail-open direction, and it is a deliberate trade: a hub whose
// USB enumeration is broken still has to be able to forget the dead board on the shelf, and
// the door re-asks on the press. The rule below is where that choice is recorded.
//
// Every caller gets the contradicted view, because that is the whole point of routing the
// read through here: a sighting is the descriptor's claim, and the claim must not survive
// as an orb wherever the hub holds better evidence about the same lead.
func (s *Server) cableSights(now time.Time) []cableSighting {
	seen, _ := s.cableSightsErr(now)
	return seen
}

// cableSightsErr is cableSights with the enumeration failure left intact, for the roster,
// which must not conclude "no cables" from a sweep that failed.
func (s *Server) cableSightsErr(now time.Time) ([]cableSighting, string) {
	if s.cables == nil {
		return nil, ""
	}
	seen, err := s.cables.refresh(now)
	return s.contradict(seen), err
}

// contradict clears the orb claim off any sighting that fresh present-tense firmware
// evidence contradicts, and leaves the port and its MAC standing so the Onboard picker
// still offers the lead to reflash.
//
// It exists because a cable sighting is a USB descriptor read, and the two fields it
// decides on — the product id and the serial — are not firmware facts in the way the
// gate needs them to be. The serial IS the eFuse base MAC, so it is the same whatever
// image is running, and the product id is whatever USB function the running image
// exposes, which a stock Espressif CDC demo matches by accident. So a board reflashed
// out of gorb-os keeps producing a sighting that says, in the hub's own words, "an orb
// with this MAC sits on this lead", and every door downstream — the fleet row, the
// Forget gate, the New Orb picker — inherits that sentence as truth. The board then
// falls between the floors: still on a cable so the card will not forget it, still an
// orb so the picker will not take it.
//
// This returns a copy whenever anything changes, because cableCache hands out its
// stored slice rather than a clone and the verdict must not rewrite what the next
// sweep compares plug-digests against.
func (s *Server) contradict(seen []cableSighting) []cableSighting {
	out := seen
	copied := false
	for i, c := range seen {
		if !c.isOrb {
			continue
		}
		if _, ok := s.contradicts(c.port); !ok {
			continue
		}
		if !copied {
			out = append([]cableSighting(nil), seen...)
			copied = true
		}
		out[i].isOrb = false
	}
	return out
}

// livenessTargets are the addresses worth dialing: every IP a fleet row still points
// at, live or stale, plus the address the onboarding flow last learned. The remembered
// rows are the interesting half — a board that is announcing is already refused by the
// announce rule and does not need a dial to prove it.
func (s *Server) livenessTargets() []string {
	seen := map[string]bool{}
	var out []string
	add := func(ip string) {
		if ip == "" || seen[ip] || softAPAddr(ip) {
			return
		}
		seen[ip] = true
		out = append(out, ip)
	}
	if s.fleet != nil {
		for _, r := range s.fleet.Every() {
			add(r.IP)
		}
	}
	s.imu.Lock()
	learned := s.orbIP
	s.imu.Unlock()
	add(learned)
	return out
}

// probeLiveness is the background half: every liveSweepEvery it re-prices every
// remembered address and wakes the card only when a verdict actually moved, so an idle
// fleet costs the traffic and not a repaint. A sweep that outlives its interval is
// harmless — a ticker drops ticks rather than queueing them.
func (s *Server) probeLiveness(ctx context.Context) {
	t := time.NewTicker(liveSweepEvery)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			if s.live == nil || s.live.probe == nil {
				continue
			}
			if s.fanoutLiveness(ctx) {
				s.wakeOrbs()
			}
		}
	}
}

// fanoutLiveness asks every remembered address once and reports whether any verdict
// moved. probeNow and the sweeper share it so the launch burst and the tick cannot drift
// apart about what "ask the book" means.
func (s *Server) fanoutLiveness(ctx context.Context) bool {
	ips := s.livenessTargets()
	if len(ips) == 0 {
		return false
	}
	return s.live.probeAll(ctx, ips, liveProbeWorkers)
}

// probeNow asks every saved address at once, once, and repaints the card if any answer
// moved a verdict. It is the reason the address book is worth keeping: a launch shows the
// fleet from the file and has the boards' own answers on screen in about one dial timeout
// instead of waiting up to a full announce interval (MDNS_ADV_REFRESH_S, 60 s) for each
// board to think of announcing itself.
//
// Concurrent on purpose, unlike the sweeper. probeLiveness dials serially because it can
// ask one board the same question twice a minute and that board's httpd is one socket
// deep; this asks every board once, and serially it would cost a fleet of eight dead
// addresses eight dial timeouts before the card told the operator anything.
//
// It never blocks a render. Call it in a goroutine after the hub starts serving: the
// first paint shows the remembered rows as "unknown", and one repaint later they are
// alive or dead. A dial answer is a verdict, not an announce: it never touches the
// presence clock, and the gate refuses a live address on the verdict itself.
func (s *Server) probeNow(ctx context.Context) {
	if s.live == nil || s.live.probe == nil || s.fleet == nil {
		return
	}
	s.fanoutLiveness(ctx)
	if ctx.Err() != nil {
		return
	}
	s.wakeOrbs()
}

// softAPAddr is the firmware's own AP subnet (orb_net.c hands out 192.168.4.x). The hub
// has no route to it, so a dial there measures the hub's routing table and not the orb.
// discover.go already refuses to dial one; the sweeper must not knock on it either, and
// must not file its failure as a verdict about the board.
func softAPAddr(ip string) bool { return strings.HasPrefix(ip, "192.168.4.") }

// annotateForget stamps the Forget verdict onto every row the card is about to show.
// confirm is false here: a render path must not dial, so a row whose address has no
// fresh verdict yet is refused with "the hub is asking" rather than handed a button that
// is a guess; the sweep lands the answer within ten seconds and the wake repaints. It runs over every row including a cable-only one, because
// "no mac yet" is itself a verdict the row should carry rather than render as a live
// button that can only fail.
func (s *Server) annotateForget(rows []orbRow, now time.Time) {
	for i := range rows {
		ok, why := s.forgetGate(rows[i].MAC, now, false)
		rows[i].Forgettable, rows[i].ForgetWhy = ok, why
		// A live board whose plain Forget the gate refused is not a board the
		// operator is stuck with: the page opens the forget dialog instead,
		// which ends the board's orb life through the actions rather than by
		// deleting a row that rebuilds itself. A row with no mac gets nothing:
		// there is no chip to act on. LivePort carries the lead the actions
		// need, and an empty one tells the dialog to say so plainly.
		rows[i].LiveForget = !ok && rows[i].MAC != ""
		rows[i].LivePort = s.cableOf(rows[i].MAC, now)
	}
	s.annotateReachability(rows, now)
	s.annotateUpdate(rows)
	s.annotateOrb(rows)
}

// annotateOrb stamps the one fact the page's two gates read. It runs last so the
// verdict sees the reachability the annotate above just settled, and it is a
// plain read of the row rather than a new enquiry: no dial, no probe, nothing
// that costs the board a reboot. See orbRow.Orb and provesOrb for why this
// exists as one predicate instead of two rules that can drift apart.
func (s *Server) annotateOrb(rows []orbRow) {
	for i := range rows {
		rows[i].Orb = rows[i].provesOrb()
	}
}

// annotateUpdate stamps each row with whether the firmware this hub carries is a
// decided upgrade over the stamp the row wears. It is the card's amber dot and
// the manage view's Update button, and it is the hub's answer rather than the
// page's guess for the same reason Forget is: the comparison engine lives here,
// it knows the bundle the hub would actually flash, and a page that compared
// version strings itself would put a different meaning on "newer" than the flash
// gate uses the first time either side grows a rule.
//
// A row with no parseable stamp gets no flag. That is the same fail-closed rule
// firmware.Compare applies to a legacy board: the absence of evidence is not
// evidence of an upgrade, and a badge that promises an update the flash gate
// would refuse is a badge that teaches people to ignore badges.
func (s *Server) annotateUpdate(rows []orbRow) {
	b := s.firmwareBundle()
	if b == nil {
		return
	}
	for i := range rows {
		if rows[i].FW == "" {
			continue
		}
		// The row's stamp against the hub's bundle, through the one comparator.
		// Elf digests do not travel on a row, so this weighs declared
		// versions and the timestamps the stamps carry — the half of Compare
		// that is decidable from what a browse packet holds.
		if firmware.Compare(firmware.Release{Version: rows[i].FW}, b.Release()) == firmware.VerdictUpgrade {
			rows[i].Update = true
		}
	}
}

// agoWords keeps a refusal readable at a glance. The number is the fact the operator
// needs — how long ago the hub last heard the board — so it is never rounded away.
func agoWords(d time.Duration) string {
	if d < 0 {
		d = 0
	}
	switch {
	case d < time.Minute:
		return fmt.Sprintf("%ds", int(d.Seconds()))
	case d < time.Hour:
		return fmt.Sprintf("%dm%02ds", int(d.Minutes()), int(d.Seconds())%60)
	default:
		return fmt.Sprintf("%dh%02dm", int(d.Hours()), int(d.Minutes())%60)
	}
}
