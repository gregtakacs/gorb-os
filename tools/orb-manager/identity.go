package main

import (
	"fmt"
	"net/http"
	"sync"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// Identity is opt-in and expensive: a probe reboots the orb to catch the
// bootloader printing its own descriptor. So the hub never probes on its own —
// the Onboard card offers one button per port, and the answer is cached here so a
// reload of the page does not cost another reboot.
//
// One thing IS automatic, and it is not a probe: the hub asks an unproven board on
// a lead the free question, PING, and files the answer here marked proven=cable
// (cableproof.go). That entry says only what the chip said — an orb app is running,
// and here is its state token. It says nothing about the firmware, so it does not
// earn the `probed` field, which reports a reboot this hub paid for.
//
// The cache key is the CABLE, not the tty name: the entry remembers the USB serial
// the host could see when the probe ran, and an answer is only handed back while
// that serial is still what is behind the port. Keying on the path alone was wrong
// in the way that matters most on a bench — orb A is identified on /dev/ttyACM0,
// orb A is unplugged, orb B is plugged into the same socket, and the picker keeps
// asserting B is running the version that belongs to A. A tty name is a lease from
// the OS, not an identity.
type identityEntry struct {
	info esprom.OrbInfo
	who  string    // the USB serial seen when this was recorded, "" if the host could not attribute the cable
	at   time.Time // when it was recorded, for the TTL
	// proven records HOW the answer was got, because the page must not present a
	// free console read as a reboot it never paid for. "probe" is the answer to a
	// click; "cable" is the hub's own PING (cableproof.go). Empty means "probe", so
	// every hand-built entry in the suite keeps reporting the way it always did.
	proven string
	// fails counts consecutive probes that never reached a chip. It is the shape of
	// the backoff on launchprobe.go: one missed probe is a board that was busy, ten is
	// a link that is broken, and asking them at the same rate is what turns a flaky
	// USBIP path into a reset storm. Cleared by any answer, including a free one.
	fails int
	// lastTry is when this port was last asked, whether or not the answer survived to
	// become the entry. The belief clock (`at`) must not be moved by a probe that
	// reached no chip, so the backoff needs a clock of its own.
	lastTry time.Time
}

// The three ways an entry gets into this cache. They are strings rather than a bool so
// a third door — a flash's boot verdict, say — can name itself instead of borrowing
// one of these two words.
const (
	provenProbe = "probe"
	provenCable = "cable"
	// provenFailed is a probe that never reached a chip: a lock held by another
	// process, a budget that ran out, a console that wedged. It is recorded so the card
	// can say why the port is unknown, and it must never be read as a verdict. The old
	// code filed these as provenProbe, which made a flaky USB hub strip the cable off a
	// healthy orb — the exact failure the cable-proof refusal above exists to prevent,
	// arriving by a different door.
	provenFailed = "probe-failed"
	// provenFlash records what this hub just wrote into the chip, which it knows
	// without a reboot because it chose the bytes. It is the strongest present-tense
	// evidence the hub holds: a probe reads what the bootloader volunteered after a
	// reset the hub paid for, and a flash record states what the hub itself installed.
	provenFlash = "flash"
	// provenLan is the same question asked over the network instead of over a lead:
	// the chip's own /api/v1/status door answered, and named this chip's MAC. Only
	// gorb-os serves that door, so one GET is present-tense proof the board runs
	// gorb-os — the fact a reboot is otherwise priced to buy.
	//
	// It is proof in one direction only. Silence on the LAN comes from a lost lease, a
	// boot-loop and a reflash alike, so this provenance never claims a board is NOT an
	// orb (contradicts refuses it), and it is not a paid verdict, so it is not exempt
	// from the TTL. What makes it worth filing is that the TTL it ages on is renewed
	// for free by every liveness dial that keeps answering (identityCache.corroborate):
	// an orb the network can see talks itself out of its next reset, forever, and a
	// board that stops talking is asked about on the paid schedule again by itself.
	provenLan = "lan"
)

type identityCache struct {
	mu       sync.Mutex
	byPort   map[string]identityEntry
	ttl      time.Duration
	inFlight map[string]bool
	// usbMAC is the host's free view of what cable is in a port. It is a field so
	// a test can replay a re-plug without a cable; production uses ClassifyPort.
	usbMAC func(port string) string
}

// identityTTL bounds how long an answer may be trusted without being paid for
// again. It is a reboot behind the user, so an answer from last hour is not a
// description of the orb now: an orb that has since OTA'd or been reflashed from
// another tool would still be reported with the old version.
const identityTTL = 30 * time.Minute

// newHub builds a Server in the state main() leaves it in, so a test cannot
// accidentally exercise a half-wired hub with a nil cache or a nil registry.
func newHub() *Server {
	s := &Server{identities: newIdentityCache(), ops: newOpRegistry(),
		probeOrb: esprom.ProbeOrb,
		token:    newLaunchToken(), grants: newIPGrants(ipGrantTTL),
		cables: newCableCache(), prover: newCableProof(), live: newLiveCache(),
		lonely:    newLonelyMark(),
		slate:     newCleanSlate(),
		orb:       orbnet.NewClient(scanTimeout),
		device:    orbnet.NewClient(deviceTimeout),
		ota:       orbnet.NewClient(otaTimeout),
		preview:   orbnet.NewClient(previewTimeout),
		previews:  map[string]previewHit{},
		pvFly:     map[string]chan struct{}{},
		cardReads: map[string]cardRead{},
		warmPass:  map[string]*warmRun{},
		warmSeq:   map[string]uint64{},
		fleet:     orbfleet.New(orbfleet.Config{}), boots: map[string]*bootVerdict{}}
	// The editor region dial, wired at the hub's own construction. S3 shipped
	// this field set only by its tests, and every live /editor/ request since
	// panicked on the nil; S4 is what first sent a real browser here, because it
	// is the first release a device can report a region from. Tests still stand
	// a fake in over this line — the point is that the real hub has a working
	// default, not that a test must use it.
	//
	// It rides the PREVIEW client, not the management one. An editor region is
	// the same shape of work as a plane: one task on a single-httpd board walks
	// its art and streams up to 2 MiB after the answer starts. The 8 s
	// management budget cut exactly this class of read mid-body — the owner saw
	// "context deadline exceeded ... while reading body" open the clock editor
	// on 2026-10-27 — which is the same mistake previewTimeout (45 s) and
	// otaTimeout document above. firstByte 8 s still kills a board that opens
	// the socket and says nothing, so a wedge costs a dial, not a pane.
	s.fetchEditor = func(ip, slug string) ([]byte, error) { return s.preview.EditorRegion(ip, slug) }
	return s
}

func newIdentityCache() *identityCache {
	return &identityCache{
		byPort:   map[string]identityEntry{},
		ttl:      identityTTL,
		inFlight: map[string]bool{},
		usbMAC:   hostUSBSerial,
	}
}

// hostUSBSerial is the serial number the OS reports for the USB device behind a
// port, which for the ESP32-S3 native USB peripheral is the chip's base MAC. An
// empty string means the host cannot attribute the cable at all — the case on
// Windows without a friendly-name read, and on macOS whenever ioreg has nothing to
// say about the port. Refusing to name a MAC is always safe here; guessing one is
// what the gate in esprom/identity.go exists to prevent.
func hostUSBSerial(port string) string {
	u := esprom.ClassifyPort(port)
	if !u.IsOrbDevice() {
		return ""
	}
	return u.Serial
}

// get returns the cached answer for port, and nothing when the cable behind it has
// turned out to be a different orb, when the answer has aged out, or when the port
// has disappeared. Reporting "no answer" is always the right answer to a question
// about a device the hub can no longer prove is the one it measured.
func (c *identityCache) get(port string) (esprom.OrbInfo, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	e, ok := c.byPort[port]
	if !ok {
		return esprom.OrbInfo{}, false
	}
	if !c.freshLocked(port, e) {
		delete(c.byPort, port)
		return esprom.OrbInfo{}, false
	}
	return e.info, true
}

// freshLocked is the staleness rule, stated once for get and for the listing path.
func (c *identityCache) freshLocked(port string, e identityEntry) bool {
	// A paid NOT-an-orb verdict does not age out. The TTL exists to stop a card printing
	// a firmware version nobody quoted, and an entry saying "this chip runs something
	// else" makes no such claim to go stale. Expiring one is not a neutral loss either:
	// the descriptor's claim is then the only fact left about the lead, so a board the
	// operator watched stop being an orb is drawn onto a cable again, silently, at the
	// 30-minute mark. The re-ask pass (launchprobe.go) re-pays these inside the window
	// they are trusted for, which is what keeps the exemption from hardening into a
	// verdict a later reflash cannot undo. A positive answer still ages normally: the
	// free facts corroborate it continuously, and its version must be allowed to go
	// stale rather than freeze on the row.
	if c.ttl > 0 && time.Since(e.at) > c.ttl && !e.paidNegative() {
		return false
	}
	return e.who == c.serialOf(port)
}

// paidNegative is the one cached answer that gets stronger with age rather than weaker:
// an orb or a flash the hub itself wrote, both saying the chip does not run gorb-os.
func (e identityEntry) paidNegative() bool {
	if e.proven != provenProbe && e.proven != provenFlash {
		return false
	}
	// A negative verdict must name the chip it is about. A bootloader that answered the
	// orb door and printed no MAC is a stranger, not a verdict, and a stranger must not
	// cost a known board its cable.
	return !e.info.IsOrb && e.info.MAC != ""
}

// drop voids any answer on file for a port that has physically left the bench, and
// reports whether the hub had a not-an-orb verdict to lose.
//
// The serial gate in freshLocked cannot do this job, and the reason is the whole point of
// this method: that gate compares the chip behind a port against the chip an answer was
// filed about, so it clears a verdict when a DIFFERENT board is plugged in and silently
// restores it when the SAME board comes back. A cable pulled and re-seated is the
// operator saying "ask again", and the answer on file is exactly what that question tests
// — the board may have been reflashed while it sat unplugged, which no cached verdict and
// no MAC can reveal. So a departure deletes rather than distrusts, and the next answer is
// priced as the fresh question it is.
// release gives a port back to the bench after a probe that will never report: the cable
// was pulled out from under it. Without this the port's in-flight slot is held against
// the next board to arrive until the probe's own budget runs out.
func (c *identityCache) release(port string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	delete(c.inFlight, port)
}

func (c *identityCache) drop(port string) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	e, ok := c.byPort[port]
	if !ok {
		return false
	}
	delete(c.byPort, port)
	return e.paidNegative()
}

func (c *identityCache) serialOf(port string) string {
	if c.usbMAC == nil {
		return ""
	}
	return c.usbMAC(port)
}

// begin claims the port for probing, reporting false if one is already running.
func (c *identityCache) begin(port string) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.inFlight[port] {
		return false
	}
	c.inFlight[port] = true
	return true
}

// done records an answer a reboot paid for, tagged with who the host believes is
// behind the port at the moment the probe finished — the same instant that produced
// the answer, so a cable swapped between those two events is caught rather than
// inherited.
func (c *identityCache) done(port string, info esprom.OrbInfo) {
	c.record(port, info, provenProbe)
}

// doneFailed records that a probe was asked and never answered, which is an absence of
// evidence and not a piece of it. It exists so the seven failure paths can tell the
// operator what went wrong on the port without that explanation being mistaken for a
// statement about the board: nothing reached a chip, so nothing is known about one.
func (c *identityCache) doneFailed(port string, reason string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	e, ok := c.byPort[port]
	fails := nextFail(e)
	// A failure keeps whatever an earlier success proved. Overwriting a paid verdict
	// because the door would not open a moment later is the same error the provenFailed
	// tag exists to prevent, one level down: the hub would trade a fact for nothing, and
	// the trade is always a board losing its cable.
	if ok && (e.paidNegative() || ((e.proven == provenProbe || e.proven == provenLan) && e.info.IsOrb)) {
		// The verdict stands; the attempt clock does not. A port holding a believed
		// negative is due again every contradictedReprobe forever, so this path is taken
		// repeatedly on a lead that will not answer. It needs its own timer because the
		// belief clock never moves here — and the belief clock alone cannot answer "this
		// lead is deaf; when is another reset worth it?"
		//
		// A LAN answer counts here for the reason the tag exists at all: the paid probe
		// that just failed reached no chip, which says something about the lead and
		// nothing about the board. Overwriting a live orb's answer with this record
		// would take the board off its cable because a console wedged, and then buy a
		// second reset for the same board on the next pass.
		e.lastTry = time.Now()
		e.fails = fails
		c.byPort[port] = e
		// And the probe's claim on the port is released. It was taken by begin, and only
		// record cleared it, so this path — the one path that files no record — left the
		// port claimed by a probe that had already returned. Every later pass then read
		// the port as busy and asked nothing: a wedged board wearing the costume of a
		// patient hub, and a bench that silently stops being interrogated until restart.
		delete(c.inFlight, port)
		return
	}
	delete(c.inFlight, port)
	c.byPort[port] = identityEntry{
		info:    esprom.OrbInfo{Port: port, Reason: reason},
		who:     c.serialOf(port),
		at:      time.Now(),
		proven:  provenFailed,
		fails:   fails,
		lastTry: time.Now(),
	}
}

// nextFail is the one rule for counting a missed probe: a streak continues off whatever
// the entry already carries, and a first miss is one. Any recorded answer zeroes fails, so
// a nonzero streak always means the last thing this port did was not answer.
//
// Both doors that file a failure read this, because they hold different entries — one
// keeps a paid verdict and so never wears the provenFailed tag, the other IS the failure
// record — and a rule spelled twice in two places is two rules, free to drift.
func nextFail(e identityEntry) int {
	if e.fails > 0 || e.proven == provenFailed {
		return e.fails + 1
	}
	return 1
}

// doneCable records the answer to the free question: the hub opened the lead, sent
// PING, and the app answered. It carries the state token and no firmware stamp, and
// the port entry reports it as a cable proof rather than a probe (handlePorts), so
// the picker can say what is known and what was merely heard.
func (c *identityCache) doneCable(port string, info esprom.OrbInfo) {
	c.record(port, info, provenCable)
}

// doneLan records the answer to the same question asked without touching the lead: the
// board's own /api/v1/status answered, and named this port's chip. It excuses the
// reboot and nothing else — see provenLan for the direction the proof runs, and
// handlePorts for the fields a door answer is allowed to publish.
func (c *identityCache) doneLan(port string, info esprom.OrbInfo) {
	c.record(port, info, provenLan)
}

// doneFlash records what this hub just wrote into the chip at this port. It is the
// only entry that can carry a NEGATIVE verdict, and that is the whole reason it
// exists: a board reflashed with firmware that is not gorb-os keeps the same eFuse
// MAC and often the same USB descriptor, so every free fact the host can read about
// the cable keeps asserting "orb" after the orb is gone. Only the act of writing the
// chip knows the truth, and it knows it without paying a reboot.
func (c *identityCache) doneFlash(port string, info esprom.OrbInfo) {
	c.record(port, info, provenFlash)
}

// contradicts reports the hub's present-tense answer to "is an orb running on this
// lead", returning false only when the hub holds fresh evidence that something else
// is running. It is the mirror of the rule already stated in handlePorts — a chip that
// has answered for itself outranks the address book — applied in the direction nobody
// had implemented: a chip that has answered for itself as NOT an orb outranks it too.
//
// Two evidence sources qualify, and everything else is refused by construction:
//
//   - a paid probe (provenProbe): the bootloader banner read after a reset this hub
//     caused, which is the only free-standing way to learn a chip runs foreign code;
//   - a flash record (provenFlash): what this hub itself just installed.
//
// A cable proof (provenCable) never qualifies. It is the answer to PING, and a PING
// that returns nothing is filed as no entry at all rather than as a miss — so an
// absence there means "we did not hear it", which is also true of an orb that is
// asleep, boot-looping, or sitting in its downloader. Treating that silence as
// "not an orb" would strip a cable from a healthy board. A door answer (provenLan)
// never qualifies for the same reason seen from the other side: it is only ever filed
// on an answer, so it can state that a board IS an orb and has no way to state the
// opposite about one that was reflashed.
//
// The cache already enforces the freshness and the cable-swap rules (identityTTL, and
// the USB serial an entry must still match), so a verdict about a board that has since
// left the lead cannot linger here and contradict whatever took its place.
func (s *Server) contradicts(port string) (esprom.OrbInfo, bool) {
	if s.identities == nil || port == "" {
		return esprom.OrbInfo{}, false
	}
	info, ok := s.identities.get(port)
	if !ok || info.IsOrb {
		return esprom.OrbInfo{}, false
	}
	proven := s.identities.provenOf(port)
	if proven == provenCable || proven == provenFailed || proven == provenLan {
		return esprom.OrbInfo{}, false
	}
	if info.MAC == "" {
		return esprom.OrbInfo{}, false
	}
	return info, true
}

// provenNotOrb is contradicts asked the way the fleet keys: by MAC rather than by lead.
// The cache itself is per-lead, so this is the one join the Forget gate needs from it —
// does ANY lead this hub has paid for hold a fresh verdict that this chip runs no
// gorb-os. It reuses paidNegative rather than restating the qualification, because the
// two questions have the same answer for the same reasons: only a probe this hub reset
// for, or a flash this hub wrote, can say a chip is not an orb, and the verdict must
// name the chip it is about or it is about nobody.
func (s *Server) provenNotOrb(mac string) bool {
	if s.identities == nil || mac == "" {
		return false
	}
	want, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		return false
	}
	for _, e := range s.identities.all() {
		if !e.paidNegative() {
			continue
		}
		if got, bad := orbmdns.CanonicalMAC(e.info.MAC); bad == "" && got == want {
			return true
		}
	}
	return false
}

// provenOf reports how the still-valid answer for port was obtained, and "" when
// there is none. The listing path gets provenance free from all(); callers that ask
// about one port (roster, liveness) need this rather than the whole map.
func (c *identityCache) provenOf(port string) string {
	c.mu.Lock()
	defer c.mu.Unlock()
	e, ok := c.byPort[port]
	if !ok || !c.freshLocked(port, e) {
		return ""
	}
	return e.proven
}

// cachedVerdict is the raw state of the one answer on file for a port: what the hub
// learned, when it learned it, how, and whether anything is there at all.
//
// The last field is the one that cannot be recovered from the others, and the launch and
// re-plug sweep keys on it. "No answer" and "an answer that is no longer fresh" look the
// same to a caller asking only whether the answer holds, but they are opposite questions:
// an answer that went stale is one the free facts keep corroborating, so re-paying it on a
// clock would reboot a healthy orb forever; an answer that was never filed, or that a
// physical departure voided, is the one case a reset is unambiguously owed. Policy lives in
// cableNeedsProbe; this reports facts.
func (c *identityCache) cachedVerdict(port string) (info esprom.OrbInfo, at time.Time, proven string, present bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	e, ok := c.byPort[port]
	if !ok {
		return esprom.OrbInfo{}, time.Time{}, "", false
	}
	return e.info, e.at, e.proven, true
}
func (c *identityCache) record(port string, info esprom.OrbInfo, proven string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	delete(c.inFlight, port)
	fails := 0
	if proven == provenFailed {
		fails = nextFail(c.byPort[port])
	}
	c.byPort[port] = identityEntry{info: info, who: c.serialOf(port), at: time.Now(),
		proven: proven, fails: fails, lastTry: time.Now()}
}

// nextAsk is when this port may be asked again, as a duration still to wait: zero once
// the backoff is spent. It is the rate limit for a question that went unanswered, in one
// place, so a stale verdict and a failed probe cannot each invent their own cadence.
//
// It gates ONLY a port whose last probe never reached a chip (fails > 0). A port that
// answered is governed by the belief rules in cableNeedsProbe — a fresh verdict is not
// re-asked and an expired one is, on their own clocks — and clamping those to a second
// window would make a re-plug or a paid negative wait for a backoff nobody owes it. The
// failure clock exists because the belief clock cannot answer the question "this lead
// will not talk to me; when is it worth another reset?"
//
// One miss waits failedRetry. Each consecutive one doubles it, capped at failedRetryMax,
// because a link that is genuinely broken answers no probe at any interval and every
// unanswered attempt still costs the board a reboot.
func (c *identityCache) nextAsk(port string, now time.Time) time.Duration {
	if c == nil {
		return 0
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	e, ok := c.byPort[port]
	if !ok || e.fails < 1 || e.lastTry.IsZero() {
		return 0
	}
	wait := failedRetry
	if e.fails > 1 {
		// Doubling, clamped two ways: the shift is bounded so the arithmetic cannot
		// wrap into a small number and re-open the storm it is meant to end, and the
		// result is capped so a lead that stays deaf becomes askable again someday.
		shift := min(e.fails-1, 6)
		wait = failedRetry << shift
		if wait > failedRetryMax {
			wait = failedRetryMax
		}
	}
	if left := e.lastTry.Add(wait).Sub(now); left > 0 {
		return left
	}
	return 0
}

// corroborate refreshes the clock on a cached positive answer using free evidence, and
// reports the port it refreshed. The caller is the liveness dial (main.go wires it into
// live.probe), which already reads the board's own `mac` from /api/v1/status ten times a
// minute for reasons that have nothing to do with identity.
//
// This is the line that makes the stated policy true rather than merely asserted. A
// positive answer is supposed to be "corroborated continuously" by the free facts and so
// never re-paid on a clock (launchprobe.go). Until now nothing refreshed `at` except a
// paid probe, so the belief expired at identityTTL anyway and a board that had answered
// every one of those dials as itself was still reset to ask the same question again. The
// dial names the chip by its eFuse MAC — the same fact the probe pays the reboot to read
// — so a match is not a hint, it is the same answer got for free.
//
// Deliberately narrow. It only moves a clock, never a claim: the entry keeps its
// provenance, so a card that reports a reboot-paid stamp still reports the reboot that
// bought it. It only touches an entry the host can still attribute to this cable, and it
// only refreshes a positive: a LAN answer is evidence that an orb is running, which says
// nothing useful about a verdict that says one is not.
func (c *identityCache) corroborate(mac string, at time.Time) (string, bool) {
	if c == nil || mac == "" {
		return "", false
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	for port, e := range c.byPort {
		if !e.info.IsOrb || e.proven == provenFailed {
			continue
		}
		// Prefer the host's own view of the cable over the answer's. A dial proving chip
		// X is an orb must not renew what the hub believes about a port holding chip Y.
		if sameChip(e.who, mac) {
			// matched on the live descriptor
		} else if e.who == "" && sameChip(e.info.MAC, mac) {
			// The host cannot read a USB serial behind a ttyUSB or on Windows, so the
			// only address on file is the one the chip printed itself. That is still this
			// port's answer, and a board that leaves the bench has the entry deleted
			// outright rather than distrusted (roster.go's departure reaper), so a re-plug
			// cannot be laundered through this branch. Without it, the free renewal that
			// keeps a live orb off the reboot timer would work only on the one platform
			// that can read sysfs, and the Windows hub would keep rebooting healthy boards.
		} else {
			continue
		}
		// The rescue the caller may announce: this answer was already past its TTL, so
		// the next sweep would have paid a reboot for it. Everything else is the routine
		// ten-second renewal, which must stay silent or the log buries the news.
		saved := c.ttl > 0 && time.Since(e.at) > c.ttl
		e.at = at
		e.fails = 0
		c.byPort[port] = e
		return port, saved
	}
	return "", false
}

// all returns the still-true answers with their provenance, keyed by port, and drops
// what is no longer true: entries whose port vanished from the enumeration
// (unplugged, so the next device to take that name starts clean) and entries whose
// cable is a different orb. It returns the whole entry rather than the OrbInfo
// because two of its readers must tell a reboot-paid probe from a free cable read:
// the port list, and the roster deciding whether a descriptor row is a card.
func (c *identityCache) all(present ...string) map[string]identityEntry {
	c.mu.Lock()
	defer c.mu.Unlock()
	seen := make(map[string]bool, len(present))
	for _, p := range present {
		seen[p] = true
	}
	out := make(map[string]identityEntry, len(c.byPort))
	for port, e := range c.byPort {
		if len(present) > 0 && !seen[port] {
			delete(c.byPort, port)
			continue
		}
		if !c.freshLocked(port, e) {
			delete(c.byPort, port)
			continue
		}
		out[port] = e
	}
	return out
}

// forget drops a port's cached answer, which is what makes a re-probe after a
// flash meaningful instead of returning the pre-flash version forever.
func (c *identityCache) forget(port string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	delete(c.byPort, port)
}

// handlePorts answers the Onboard picker. Each entry carries the cached identity
// when there is one, so the card renders the MAC beside the port without having
// to ask the chip again.
func (s *Server) handlePorts(w http.ResponseWriter, r *http.Request) {
	ports, err := esprom.ListPorts()
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	if ports == nil {
		ports = []string{}
	}
	cached := s.identities.all(ports...)
	entries := make([]map[string]any, 0, len(ports))
	for _, p := range ports {
		// Two separate facts, because the client needs them for different things:
		// "attributed" is what the host can say about the cable for free (sysfs or
		// the registry, no port open, no reset), and "probed" is whether someone has
		// paid a reboot to ask the chip. Collapsing them once hid the free MAC from
		// the picker label, which is the only thing that tells two cables apart.
		u := esprom.ClassifyPort(p)
		e := map[string]any{
			"port":       p,
			"hint":       usbLabel(u),
			"attributed": u.Known,
			"probed":     false,
			// "espressif" is the New Orb gate: a genuine Espressif chip (VID
			// 303A) whatever it currently enumerates as. The page asks it of a
			// board that may be blank, boot-looping, or in download mode, and
			// only the host can answer without touching the chip. The product
			// id decides nothing here: blank and flashed boards report
			// differently, and a gate keyed on the product id would hide a
			// board precisely when the operator most needs the door open.
			"espressif": u.IsEspressif(),
			// "orbSeen" is the other half of that gate, and the mirror of the
			// row field of the same meaning: the hub has evidence this chip has
			// been an orb, so the board belongs on a card and not in the New
			// Orb picker. It is NOT row existence — the roster writes a row for
			// a bare cable sighting, so existence would make every board that
			// has ever been plugged in un-adoptable forever, which is the bug
			// this field replaces. Always written, so a hub that cannot answer
			// the question reads as a no rather than a hole.
			"orbSeen": false,
		}
		// The MAC the host read off the descriptor, as opposed to the one a probe
		// heard on the console. Kept separate so the client never has to unpick a
		// display string to label a cable.
		if u.IsOrbDevice() && u.Serial != "" {
			e["usbMac"] = u.Serial
			// A board this hub left lonely is a blank Espressif chip with a
			// memory, not an orb: the entry says so, and the picker labels it
			// "left lonely" instead of letting the descriptor silently re-orb it.
			if s.lonely.has(macNorm(u.Serial)) {
				e["lonely"] = true
			} else {
				// Two facts the page must not conflate, so they stay two fields:
				// the address book holds this chip at all ("inFleet", what the
				// add-by-IP door refuses on), and the book proves it has been an
				// orb ("orbSeen", what hides it from the New Orb door). A row that
				// only ever recorded a cable gives the first without the second.
				if row, known := s.fleetKnown(macNorm(u.Serial)); known {
					e["inFleet"] = true
					if storeProvesOrb(row) {
						e["orbSeen"] = true
					}
				}
			}
		}

		if ent, ok := cached[p]; ok {
			info := ent.info
			// "probed" keeps one meaning: a reboot this hub paid for, and a firmware
			// stamp read off the bootloader in the window that follows. The free
			// cable answer is a different fact and gets its own field, because a
			// picker that called a PING a probe would report a reboot nobody asked
			// for and would hide the Identify button that settles the version. A door
			// answer is the third case and takes the same treatment for the same
			// reason: it cost one GET, so it must not be reported as a reset.
			rebootPaid := ent.proven != provenCable && ent.proven != provenLan
			e["probed"] = rebootPaid
			if ent.proven == provenCable {
				e["cableProof"] = info.IsOrb
			}
			if ent.proven == provenLan {
				e["lanProof"] = info.IsOrb
			}
			e["isOrb"] = info.IsOrb
			// A chip that has answered for itself as an orb is the strongest
			// proof the hub keeps, and it outranks the address book: a board
			// identified after a wipe is known even before it has a name. The
			// console answer counts here whatever it cost, which is the point of
			// asking it: an orb on a lead stops reading as a blank to flash.
			if info.IsOrb {
				e["orbSeen"] = true
			}
			// The same authority read the other way. A paid verdict that this is
			// NOT an orb is present-tense fact, and it outranks every historical
			// record the address book holds about this chip — which matters
			// because the descriptor that attributed the chip to the address book
			// is an eFuse MAC that survives any reflash. Without this the board
			// falls between the two floors: the row still calls it "on cable" so
			// the fleet keeps it, and orbSeen still says "been an orb" so the New
			// Orb door keeps it. The board is then neither manageable nor
			// flashable, which is the bug this field ends.
			if !info.IsOrb && ent.proven != provenCable {
				e["orbSeen"] = false
				e["inFleet"] = false
				e["notOrb"] = true
				e["project"] = info.Project
				e["version"] = info.Version
			}
			e["mac"] = info.MAC
			e["reason"] = info.Reason
			// The chip's own state token ("b", "c-ap", "c-sta"), which is the whole
			// news a board with no lease can give and the reason its row says
			// "waiting for a network" rather than "no address yet".
			e["orbState"] = info.State
			if rebootPaid {
				e["project"] = info.Project
				e["version"] = info.Version
				// The same stamp as structure, so a card can print v1.0.0 without
				// parsing the string. Legacy boards come back as legacy: true.
				e["semver"] = semverPayload(info.Version)
				e["compileTime"] = info.CompileTime
				e["compiledAt"] = info.CompiledAt
				e["elfSha256"] = info.ElfSHA256
				e["idf"] = info.IDF
				e["ip"] = info.IP
				e["probedAt"] = info.ProbedAt
				e["verdict"] = string(s.verdictFor(info))
			}
			// What a door answer publishes, and what it may not. The version, the
			// address and the state token came back in the payload, so the card may
			// print them and the comparator may judge them. The project name, the
			// build stamp and the ELF digest are bootloader facts the door does not
			// carry, and an empty field is the honest report rather than a gap the
			// page fills in by guessing — which is why this block is not the one above
			// with two lines deleted.
			if ent.proven == provenLan {
				e["version"] = info.Version
				e["semver"] = semverPayload(info.Version)
				e["ip"] = info.IP
				e["verdict"] = string(s.verdictFor(info))
			}
		}
		entries = append(entries, e)
	}
	// Same object wrapper the bare list used: app.js destructures { ports }, and
	// a bare array once left the picker showing an error instead of a cable.
	s.jsonOK(w, map[string]any{"ports": entries, "count": len(entries)})
}

// usbLabel is what the host already knows about the device behind a port, without
// touching it: the Espressif peripheral names itself, and its serial is the MAC.
// It is deliberately a display string, not something to parse decisions out of —
// the machine-readable form goes out as usbMac.
func usbLabel(u esprom.USBClass) string {
	switch {
	case u.IsOrbDevice():
		if u.Serial != "" {
			return "orb-device:" + u.Serial
		}
		return "orb-device"
	case !u.Known:
		return ""
	default:
		return fmt.Sprintf("usb-%04x:%04x", u.Vendor, u.Product)
	}
}

// foldProbeIdentity files what a reboot-paid probe learned. Every probe the hub runs
// — launch, re-plug, expiry, the silence escalation — lands here, so no caller gets to
// decide on its own what an answer means.
//
// A bootloader descriptor read down the USB lead is the strongest thing the hub can
// say about a board: it is the chip answering for itself, not a name claimed on the
// LAN, so the fleet holds that sighting above every announcement. The sighting it
// holds is the one the chip gave: a chip that answers as NOT an orb is not evidence of
// an orb on the lead, so nothing is filed and nothing is refreshed.
//
// Confirming here used to happen before the verdict was read, and it cost the operator
// the only door they had left (bench, 2026-10-26). A board ended with the lonely write
// enumerates the same eFuse MAC, so the next paid probe re-Confirmed its deleted row
// and stamped the presence clock — which is the fact the Forget gate refuses on, worded
// as the board "announcing itself". The row therefore came back every two minutes, and
// the card refused Forget for a board that had announced nothing since it stopped being
// an orb: a clock the hub winds for the refusal is a gate that never opens. The flash
// path already refused this stamp for a foreign image, for the same reason.
func (s *Server) foldProbeIdentity(info esprom.OrbInfo) {
	if info.MAC == "" {
		return
	}
	// A console that answered the orb door ends any lonely mark on the chip, whoever
	// re-flashed it. Without this the mark only ever cleared on this hub's own acts,
	// so a board revived on another bench — or by the CLI — stayed suppressed here
	// forever while probing back as an orb.
	if !info.IsOrb {
		// The chip answered for itself and said it runs no gorb-os. That ends
		// the board's orb life at this hub too, wherever the erase happened:
		// the fleet row, the name, the grant and the liveness verdict all go
		// now (wipe.go's endProvenNotOrb), which is the lonely door's promise
		// kept for boards written over by hands this hub did not hold. The
		// port keeps its contradictory stamp: it is the evidence that holds
		// the descriptor down and keeps the next sweep from re-filing the row.
		s.endProvenNotOrb(info)
		return
	}
	s.markCable(info.MAC)
	s.lonely.clear(info.MAC)
	// The descriptor answered for itself, so the running version it printed belongs
	// on the row: this is the firmware stamp for a board with no lease, read at the
	// price of one reboot the hub paid.
	if s.fleet != nil && (info.Version != "" || info.State != "") {
		if _, changed, err := s.fleet.NoteAnswer(info.MAC, info.Version, info.State); err == nil && changed {
			s.saveState()
			s.wakeOrbs()
		}
	}
	// The banner named itself, so the card can stop being blank. This is the only
	// read path for a board with no lease: the console is open right now, and the
	// serial door writes names without ever reading them back.
	if s.foldBootName(info.MAC, info.Name) {
		s.saveState()
	}
}

// probeBudget bounds one identity probe. The probe itself reads a 9 s boot window
// plus the ROM handshake; this is the outer ceiling a Cancel or a forced quit stops
// against, so a wedged console cannot hold a cable indefinitely.
const probeBudget = 40 * time.Second

// verdictFor compares the running firmware a probe found against the firmware the
// hub would flash right now — the built-in bundle, or whatever the user picked.
// A port that is not an orb gets no verdict: the question does not apply.
func (s *Server) verdictFor(info esprom.OrbInfo) firmware.Verdict {
	if !info.IsOrb {
		return firmware.VerdictUnknown
	}
	b := s.firmwareBundle()
	if b == nil {
		return firmware.VerdictUnknown
	}
	return firmware.Compare(orbRelease(info), b.Release())
}

func orbRelease(info esprom.OrbInfo) firmware.Release {
	r := firmware.Release{Version: info.Version, ElfSHA256: info.ElfSHA256}
	if info.CompiledAt != "" {
		if t, err := time.Parse(time.RFC3339, info.CompiledAt); err == nil {
			r.CompiledAt = t
		}
	}
	return r
}
