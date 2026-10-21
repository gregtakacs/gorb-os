package orbmdns

import (
	"bytes"
	"context"
	"fmt"
	"net"
	"sort"
	"strings"
	"sync"
	"time"
)

// Candidate is one orb a browse heard about. It is a listing, not a connection:
// nothing here has been dialed, and nothing here authorises a dial. The proxy
// still reaches only addresses ipGrants holds, and Item 6 is what cross-locks a
// candidate's MAC against what the orb claims about itself.
type Candidate struct {
	// Instance is the leftmost label, "orb-782618". The firmware derives it from
	// the base MAC and ExpectInstance re-derives it here: the two spellings are
	// checked against each other, not maintained beside each other.
	Instance string
	// MAC is the chip's BASE address as twelve lowercase hex digits with no
	// separators — the field h_status reports and the USB serial descriptor
	// carries. It is the row's key. An orb that changes address is still the same
	// orb; an orb that changes MAC is a different board.
	MAC   string
	FW    string
	State string
	IP    string
	Port  uint16
	// From is who sent the packet, kept for the log line. It is not part of the
	// row's identity and never has been: neither it nor the A record is anything
	// but a claim, and only the TXT mac has a hardware fact behind it.
	From string
	TTL  uint32

	FirstSeen time.Time
	LastSeen  time.Time
	Expires   time.Time
}

// Provisioning reports the state= word, defaulted the way the firmware defaults
// it: a record that said nothing was not announcing a factory-reset orb.
func (c Candidate) Provisioning() bool { return c.State == "provisioning" }

// Refusal is one announcement the browser would not list, with its reason.
// Silently dropping a malformed orb is the failure mode this file exists to
// avoid: the hub looks healthy, the orb looks absent, and nobody can tell which
// one lied.
type Refusal struct {
	Name   string // the owner name as heard
	From   string
	Reason string
	At     time.Time
}

// Source carries mDNS traffic between the LAN and the browser, in one direction
// only: it delivers datagrams in, and that is all that exists. The browser has no
// write method to reach — it sends nothing, ever. Discovery rides the orb's own
// check-ins: the firmware re-announces every MDNS_ADV_REFRESH_S (60 s, under half
// the record TTL), so a hub that joined late waits one interval, not a reboot.
// Production is one read-only view of a multicast socket per eligible interface;
// a test hands in a loopback pair or a memory feed.
//
// The interface is ReadFrom-and-Close by decision, not by omission. The hub used
// to query on a timer and answer its own traffic's absence with retries; that
// whole path — burst, ticker, known-answer suppression, follow-ups for half-heard
// instances — is gone, and a Source that cannot be written through means the
// querier cannot come back by accident. The one boundary that held then still
// holds: no field, argument, or option here can aim traffic at a single host.
type Source interface {
	ReadFrom(p []byte) (int, net.Addr, error)
	Close() error
}

// partial is an instance heard in a PTR whose SRV, TXT or A has not landed yet. It
// is keyed by instance name because there is no MAC to key by until the TXT
// arrives; the provable rows are keyed by MAC, and only by MAC. A partial is not a
// problem to chase: the firmware bundles all four records in every check-in, so
// whatever is missing arrives with the next one.
type partial struct {
	instance    string
	srv, txt, a *Record
	from        string
	seen        time.Time
}

// AHost is an A record waiting for the SRV that points at it.
type AHost struct {
	IP net.IP
	// TTL is what the record claimed, kept so a row promoted from a cached
	// address expires on the device's schedule rather than the hub's guess.
	TTL     uint32
	Expires time.Time
}

// Config tunes a browser. Zero values are the production answers; a test sets Now
// to drive the clock and changes nothing else.
type Config struct {
	Now  func() time.Time
	Logf func(string, ...any)
	// Grace extends a row's life past its TTL before pruning, so an orb that
	// misses one refresh window under a busy radio task does not blink out of the
	// list. Zero means the default.
	Grace time.Duration
	// OnChange is told that the cache moved, in words of one syllable and with no
	// payload: the hub's half re-reads Candidates and decides whether the answer
	// actually differs. It exists so the card follows the orb's check-in instead of
	// asking about it on a timer, which is the same decision the hub made when it
	// stopped querying — the device owns the rhythm, so the hub follows it rather
	// than sampling it.
	//
	// It fires from the goroutine that read the datagram, at most once per datagram
	// however many records that datagram carried, and never with the browser's lock
	// held. It must not block and must not call back into the Browser: a hub that
	// wants to stop a stream unsubscribes rather than stalling this one. The hub's
	// implementation is a non-blocking send on a one-slot channel.
	//
	// Nil is the default and is complete: Candidates stays the source of truth, and
	// a hub built without a callback simply re-reads on its own clock.
	OnChange func()
}

const (
	// DefaultGrace is exported because the hub's card promises a row's life in
	// seconds, and that promise is TTL + this. TestCheckinCadenceMatchesFirmware
	// reads it from out here so the firmware, the browser and the copy cannot each
	// keep their own idea of how long an orb may stay listed.
	DefaultGrace   = 30 * time.Second
	maxRefusalTail = 32
)

// Browser holds the discovered set and the sockets feeding it. Build one with
// NewBrowser; the zero value is not useful.
type Browser struct {
	mu       sync.Mutex
	byMAC    map[string]*Candidate
	pending  map[string]*partial
	hosts    map[string]*AHost
	refusals []Refusal
	sources  []Source

	now   func() time.Time
	logf  func(string, ...any)
	grace time.Duration

	// rev counts the cache mutations that could change what Candidates or Refusals
	// answer. The wake-up path compares it across one datagram instead of threading
	// a flag through the six places a record can land, which is what makes the two
	// properties that matter fall out: a packet that half-parsed and changed nothing
	// wakes nobody, and a check-in that carries all four records wakes the card once.
	rev     uint64
	onEvent func()
}

// NewBrowser returns an empty browser. A browser with no sockets still answers
// Ingest and Candidates, which is how the wire behaviour gets tested with no
// network under it. The browser takes traffic in and puts none out: check
// BrowserNeverSends in the suite, which fails the package if a write path ever
// reappears in it.
func NewBrowser(cfg Config) *Browser {
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	if cfg.Logf == nil {
		cfg.Logf = func(string, ...any) {}
	}
	if cfg.Grace <= 0 {
		cfg.Grace = DefaultGrace
	}
	return &Browser{
		byMAC:   map[string]*Candidate{},
		pending: map[string]*partial{},
		hosts:   map[string]*AHost{},
		now:     cfg.Now,
		logf:    cfg.Logf,
		grace:   cfg.Grace,
		onEvent: cfg.OnChange,
	}
}

// revision reads the mutation counter, and is separate from wokeUp because the
// reading sites take it before the work and the firing sites compare it after.
func (b *Browser) revision() uint64 {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.rev
}

// wokeUp fires OnChange when the cache moved since `before`. It takes no lock of
// its own around the callback: every caller runs it after dropping the browser lock,
// which is the whole reason a hub that stopped draining a wake-up cannot wedge the
// reader goroutine that produces one.
func (b *Browser) wokeUp(before uint64) {
	if b.onEvent == nil || b.revision() == before {
		return
	}
	b.onEvent()
}

// mark says the card's answer moved. Call it with b.mu held, at the mutation and
// not at the callback: the counter is what lets a caller decide once per datagram
// whether anything reached the page, and the six mutators below are the whole list
// of what can. Adding a seventh without marking it costs one stale frame, which is
// the failure mode OnChange was chosen to avoid, so TestEveryMutationWakes pins it
// from the other side instead of trusting this comment.
func (b *Browser) mark() { b.rev++ }

// Start attaches sockets and reads them until ctx is cancelled. It owns what it is
// handed: a socket that comes in here is closed on shutdown, because a hub that
// quit mid-browse with 5353 still held is a hub the next launch cannot start.
func (b *Browser) Start(ctx context.Context, sources ...Source) error {
	b.mu.Lock()
	if len(b.sources) > 0 {
		b.mu.Unlock()
		return fmt.Errorf("orbmdns: browser already started")
	}
	b.sources = append([]Source(nil), sources...)
	b.mu.Unlock()
	for _, s := range sources {
		go b.pump(s)
	}
	go func() {
		<-ctx.Done()
		_ = b.Close()
	}()
	return nil
}

// Close releases every socket. Safe to call twice.
func (b *Browser) Close() error {
	b.mu.Lock()
	srcs := b.sources
	b.sources = nil
	b.mu.Unlock()
	for _, s := range srcs {
		_ = s.Close()
	}
	return nil
}

// pump moves one socket's datagrams into Ingest until it is closed. A read error is
// the expected exit — Close breaks the blocking read — and earns no log line.
func (b *Browser) pump(s Source) {
	buf := make([]byte, maxRDLen+256)
	for {
		n, from, err := s.ReadFrom(buf)
		if err != nil {
			return
		}
		if n <= 0 {
			continue
		}
		b.Ingest(append([]byte(nil), buf[:n]...), from)
	}
}

// orbServiceWire is the _orb._tcp service name as it appears on the wire: two
// length-prefixed labels, not the dotted spelling. bytes.Contains cannot be fed the
// dotted name, because the dot is a label boundary (a length byte), not a literal.
var orbServiceWire = []byte{4, '_', 'o', 'r', 'b', 4, '_', 't', 'c', 'p'}

// packetMentionsOrbService is a cheap, parse-independent scan of a datagram for the
// orb service label. It exists for one decision: a packet that failed to Parse at
// all is refused, but the refusal tail answers "why is that orb missing", and a
// truncated or malformed packet from a Chromecast that never named the orb service
// is not evidence about any orb. Names are case-insensitive in DNS, so the scan
// lowercases a copy first. A datagram too mangled to hold the label in order does
// not match — which is the correct conservative answer for a neighbour's noise and
// a safe miss for a real orb, whose four records the firmware builds in order.
func packetMentionsOrbService(pkt []byte) bool {
	return bytes.Contains(bytes.ToLower(pkt), orbServiceWire)
}

// Ingest folds one received datagram into the discovered set. from is the packet's
// source address, kept for the log line: it says who announced, not what was
// announced, and it is not an identity. Neither is the A record — both are claims
// an orb makes about itself, and only the TXT mac has a hardware fact behind it.
//
// Queries are ignored, never answered. This is a browser. A hub that answered a
// query would be a second, lazier orb on the LAN, and its cache-flush records
// would overwrite the real one's in every browser's cache.
func (b *Browser) Ingest(pkt []byte, from net.Addr) {
	// One wake-up per datagram, taken before any of it lands and fired on the way
	// out. Every mutator below does its own locking, so this deferred comparison is
	// the only place the whole packet is judged, and it is what stops a check-in
	// bundling PTR/SRV/TXT/A from nudging the card four times for one row.
	before := b.revision()
	defer func() { b.wokeUp(before) }()

	src := ""
	if from != nil {
		src = from.String()
	}
	m, err := Parse(pkt)
	if err != nil {
		// A packet that will not parse is a defect only when it could have been an
		// orb. Anything else — a neighbour's truncated response, a malformed
		// announcement for some other service — is the LAN working, not a missing
		// orb, and a tail full of it would bury the one refusal that matters and
		// evict real rows from the bounded tail. It still reaches the log, so an
		// operator can find it; it does not reach the card.
		if packetMentionsOrbService(pkt) {
			b.refuse("", src, err.Error())
			return
		}
		b.logf("orbmdns: ignored a non-orb packet from %s: %s", src, err.Error())
		return
	}
	if !m.Response {
		return // a neighbour's question is not our business
	}
	for _, rec := range m.Records() {
		r := rec
		switch rec.Type {
		case typePTR:
			if !strings.EqualFold(rec.Name, ServiceName) {
				continue // someone else's service table
			}
			inst, ok := ServiceInstance(rec.PTR)
			if !ok {
				b.refuse(rec.PTR, src, "a PTR that does not name an _orb._tcp instance")
				continue
			}
			if rec.TTL == 0 {
				b.drop(inst)
				continue
			}
			b.note(inst, src, func(p *partial) {})
		case typeSRV:
			inst, ok := ServiceInstance(rec.Name)
			if !ok {
				continue
			}
			if rec.TTL == 0 {
				b.drop(inst)
				continue
			}
			b.note(inst, src, func(p *partial) { p.srv = &r })
		case typeTXT:
			inst, ok := ServiceInstance(rec.Name)
			if !ok {
				continue
			}
			if rec.TTL == 0 {
				b.drop(inst)
				continue
			}
			b.note(inst, src, func(p *partial) { p.txt = &r })
		case typeA:
			b.cacheHost(&r, src)
		}
	}
}

// note records one half of an instance's announcement, then tries to name it.
func (b *Browser) note(instance, from string, set func(*partial)) {
	b.mu.Lock()
	p := b.pending[instance]
	if p == nil {
		p = &partial{instance: instance, from: from, seen: b.now()}
		b.pending[instance] = p
	}
	if p.from == "" {
		p.from = from
	}
	set(p)
	b.mu.Unlock()
	b.promote(instance)
}

// cacheHost holds an A record until an instance points at it. A host record names
// no instance, so it can arrive before, after or without the SRV that asks for it,
// and discarding the early one is what would make a browse depend on the order
// four records happened to be sent in.
func (b *Browser) cacheHost(rec *Record, from string) {
	instance, ok := OrbHost(rec.Name)
	if !ok {
		// An A record for somebody else's host: an iPad, a printer, a Chromecast. It
		// can never point an orb's SRV at a row, and its address — least of all a
		// placeholder 0.0.0.0 — is not a defect the card should report. The question
		// the refusal tail answers is "why is that orb missing", not "everything
		// broadcasting on the subnet", so a stranger's host record is dropped here in
		// silence rather than cached or refused.
		return
	}
	name := strings.ToLower(rec.Name)
	if rec.TTL == 0 {
		b.mu.Lock()
		delete(b.hosts, name)
		b.mu.Unlock()
		return
	}
	ip, err := ParseOrbAddress(rec.A)
	if err != nil {
		// A host named like an orb but carrying an address no orb can have. The
		// instance is known, so this refusal names the orb it was meant to become.
		b.refuse(InstanceName(instance), from, err.Error())
		return
	}
	now := b.now()
	filled := make([]string, 0, 2)
	b.mu.Lock()
	b.hosts[name] = &AHost{IP: net.ParseIP(ip).To4(), TTL: rec.TTL, Expires: now.Add(time.Duration(rec.TTL) * time.Second)}
	for _, p := range b.pending {
		if p.srv == nil || p.a != nil || !strings.EqualFold(p.srv.SRVTarget, rec.Name) {
			continue
		}
		cp := *rec
		cp.A = net.ParseIP(ip).To4()
		p.a = &cp
		filled = append(filled, p.instance)
	}
	b.mu.Unlock()
	// The address is often the last of the four to arrive, so its arrival is what
	// completes a row. Promoting outside the lock keeps one locking path, and
	// skipping it is how an orb whose A record came one datagram late never
	// appeared in the list at all.
	for _, inst := range filled {
		b.promote(inst)
	}
	// And a row already promoted is refreshed rather than left holding the address
	// it first announced. The cached hosts[] entry is older than this packet by
	// definition, and a record that arrived with the address before the TXT would
	// otherwise have a promote() run pull the stale one out of that cache.
	b.mu.Lock()
	moved := false
	for _, c := range b.byMAC {
		if strings.EqualFold(HostnameFor(c.Instance), name) && c.IP != ip {
			c.IP = ip
			moved = true
		}
	}
	if moved {
		b.mark()
	}
	b.mu.Unlock()
}

// promote turns a complete, self-consistent instance into a row. Every refusal
// here is a claim that failed its check, so it is reported rather than absorbed:
// an orb announcing a mac its instance cannot have is not a row with a cosmetic
// problem, it is a stranger, and the row that mixes the two is what a fleet list
// gets wrong.
func (b *Browser) promote(instance string) {
	now := b.now()
	b.mu.Lock()
	defer b.mu.Unlock()
	p := b.pending[instance]
	if p == nil || p.srv == nil || p.txt == nil {
		return
	}
	if p.a == nil {
		if h, ok := b.hosts[strings.ToLower(HostnameFor(instance))]; ok && now.Before(h.Expires) {
			p.a = &Record{Name: HostnameFor(instance), Type: typeA, Class: classIN, TTL: h.TTL, A: h.IP}
		}
	}
	if p.a == nil {
		return // no address yet: nothing to list, and nothing to dial
	}
	mac, why := CanonicalMAC(p.txt.TXT["mac"])
	if why != "" {
		b.rejectLocked(instance, p, why)
		return
	}
	if err := ExpectInstance(instance, mac); err != nil {
		b.rejectLocked(instance, p, err.Error())
		return
	}
	if p.srv.SRVPort == 0 {
		b.rejectLocked(instance, p, "a SRV that names no port")
		return
	}
	if !strings.EqualFold(p.srv.SRVTarget, HostnameFor(instance)) {
		b.rejectLocked(instance, p, "a SRV targeting "+p.srv.SRVTarget+", not its own host")
		return
	}
	ip, err := ParseOrbAddress(p.a.A)
	if err != nil {
		b.rejectLocked(instance, p, err.Error())
		return
	}
	c, ok := b.byMAC[mac]
	if !ok {
		c = &Candidate{FirstSeen: now}
		b.byMAC[mac] = c
	}
	// The row is keyed on the MAC, so a renewed lease that moves the address
	// refreshes one row instead of leaving two orbs that are one board. The old
	// address is replaced, not aged: an A record is a current claim, and the hub
	// has no basis for preferring the older of two.
	c.Instance, c.MAC = instance, mac
	c.FW = p.txt.TXT["fw"]
	c.State = p.txt.TXT["state"]
	if c.State == "" {
		c.State = "normal"
	}
	c.IP, c.Port, c.TTL = ip, p.srv.SRVPort, p.txt.TTL
	c.From = p.from
	c.LastSeen, c.Expires = now, now.Add(time.Duration(p.txt.TTL)*time.Second+b.grace)
	delete(b.pending, instance)
	b.mark()
}

// rejectLocked drops an instance that failed a check and keeps its reason.
func (b *Browser) rejectLocked(instance string, p *partial, why string) {
	delete(b.pending, instance)
	b.refusals = append(b.refusals, Refusal{Name: InstanceName(instance), From: p.from, Reason: why, At: b.now()})
	if len(b.refusals) > maxRefusalTail {
		b.refusals = b.refusals[len(b.refusals)-maxRefusalTail:]
	}
	b.mark()
	b.logf("orbmdns: refused %s: %s", instance, why)
}

// drop removes an instance on a TTL-0 record. Holding the row until its TTL would
// be the ghost §5.1 refuses to leave in a fleet list: the orb's socket is already
// closed and no browse will reach it again.
func (b *Browser) drop(instance string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	delete(b.pending, instance)
	for mac, c := range b.byMAC {
		if c.Instance == instance {
			delete(b.byMAC, mac)
			b.mark()
			b.logf("orbmdns: %s said goodbye", instance)
		}
	}
}

// Candidates returns the live rows, first-seen first, pruning what the clock has
// already outlived. It dials nothing: answering "what orbs are on this network"
// without opening a socket to any of them is the item.
func (b *Browser) Candidates() []Candidate {
	before := b.revision()
	// Registered before the unlock defer, so LIFO ordering runs it after the lock is
	// gone: a card built from this call is what fires the stream, and firing under the
	// lock would let the hub's HTTP goroutine block the reader that holds it.
	defer func() { b.wokeUp(before) }()

	now := b.now()
	b.mu.Lock()
	defer b.mu.Unlock()
	for mac, c := range b.byMAC {
		if now.After(c.Expires) {
			delete(b.byMAC, mac)
			b.mark()
			b.logf("orbmdns: %s expired unheard", c.Instance)
		}
	}
	for h, a := range b.hosts {
		if now.After(a.Expires) {
			delete(b.hosts, h)
		}
	}
	out := make([]Candidate, 0, len(b.byMAC))
	for _, c := range b.byMAC {
		out = append(out, *c)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].FirstSeen.Equal(out[j].FirstSeen) {
			return out[i].MAC < out[j].MAC
		}
		return out[i].FirstSeen.Before(out[j].FirstSeen)
	})
	return out
}

// Refusals snapshots what the browser declined to list, oldest first, as a bounded
// tail. Unbounded would be a memory leak with a hostile remote end, and the list
// is a diagnostic rather than a ledger.
func (b *Browser) Refusals() []Refusal {
	b.mu.Lock()
	defer b.mu.Unlock()
	return append([]Refusal(nil), b.refusals...)
}

// refuse records a packet-level refusal — one that never parsed into records at
// all. The reason is kept verbatim, because "why is that orb not in the list" is
// the question this exists to answer.
func (b *Browser) refuse(name, from, reason string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.refusals = append(b.refusals, Refusal{Name: name, From: from, Reason: reason, At: b.now()})
	if len(b.refusals) > maxRefusalTail {
		b.refusals = b.refusals[len(b.refusals)-maxRefusalTail:]
	}
	b.mark()
	b.logf("orbmdns: refused %q from %s: %s", name, from, reason)
}
