package orbmdns

import (
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
	MAC       string
	FW        string
	State     string
	IP        string
	Port      uint16
	From      string // who sent it, kept for the log line and nothing else
	Solicited bool   // false when the orb announced itself unasked
	TTL       uint32

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

// Source carries mDNS traffic between the browser and the LAN: it delivers
// datagrams in and takes queries out. Production is one net.PacketConn per
// eligible interface; a test hands in a loopback pair. That is the reason for the
// interface rather than a *net.UDPConn — this bench cannot originate multicast
// onto the LAN, so a browser testable only through a live group join is a browser
// that was never tested. The browser always writes to the group address and has no
// field that aims a query at one host: the bench's unicast workaround stays in the
// bench script that needs it. The gap that unicast was proposed to fill — a hub
// starting after an orb's one-time join announcement — is now closed by a sustained,
// RFC-shaped multicast querier (§6, §7.1, §10) instead, so the boundary holds.
type Source interface {
	ReadFrom(p []byte) (int, net.Addr, error)
	WriteTo(p []byte, addr net.Addr) (int, error)
	Close() error
}

// groupEndpoint is the only destination the browser will ever send to.
var groupEndpoint = &net.UDPAddr{IP: net.ParseIP(GroupAddr), Port: Port}

// partial is an instance heard in a PTR whose SRV, TXT or A has not landed yet. It
// is keyed by instance name because there is no MAC to key by until the TXT
// arrives; the provable rows are keyed by MAC, and only by MAC.
type partial struct {
	instance    string
	srv, txt, a *Record
	from        string
	solicited   bool
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
}

const (
	defaultGrace   = 30 * time.Second
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
	txid     uint16

	now   func() time.Time
	logf  func(string, ...any)
	grace time.Duration
}

// NewBrowser returns an empty browser. A browser with no sockets still answers
// Ingest and Candidates, which is how the wire behaviour gets tested with no
// network under it.
func NewBrowser(cfg Config) *Browser {
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	if cfg.Logf == nil {
		cfg.Logf = func(string, ...any) {}
	}
	if cfg.Grace <= 0 {
		cfg.Grace = defaultGrace
	}
	return &Browser{
		byMAC:   map[string]*Candidate{},
		pending: map[string]*partial{},
		hosts:   map[string]*AHost{},
		now:     cfg.Now,
		logf:    cfg.Logf,
		grace:   cfg.Grace,
		txid:    0xC0FE,
	}
}

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

// browseKnownLocked collects the browse question's RFC 6762 §7.1 known answers:
// one PTR per live instance, carrying its AGED remaining TTL. The caller holds the
// lock. Only instances with more than half their TTL left are listed — a record past
// half is left off so the next answer refreshes it before it can expire, honouring
// §7.1's rule that a known answer younger than a quarter TTL must not suppress.
//
// It is PTR-only on purpose. The browse asks PTR; the A/SRV/TXT the firmware bundles
// into that one reply are additional records a listing of PTRs does not claim to
// hold, and reconstructing a TXT from the Candidate's decoded fields would risk a
// known answer that does not match the responder's byte-for-byte, which defeats the
// suppression it is there to trigger. Naming back the PTRs is the exact, canonical
// case. Instances past the maxKnownAnswers cap go un-named (map order is not sorted
// for this): the only cost is one redundant answer from them, never a missed orb.
//
// Our own firmware reads only a query's question and ignores this section entirely,
// so known-answer suppression buys nothing against an orb. It is here for third-party
// responders sharing the LAN and for spec correctness — and it is what makes the
// timer-driven re-ask (§6) polite rather than a standing 1 q/s broadcast.
func (b *Browser) browseKnownLocked(now time.Time) []Record {
	if len(b.byMAC) == 0 {
		return nil
	}
	out := make([]Record, 0, len(b.byMAC))
	for _, c := range b.byMAC {
		ttl := int64(c.TTL)
		if ttl <= 0 {
			continue
		}
		// True expiry is LastSeen + TTL. Candidate.Expires adds a grace window the
		// row lingers on for the UI; counting that as freshness would list a stale
		// record as a known answer and suppress the refresh that would fix it.
		left := ttl - int64(now.Sub(c.LastSeen).Seconds())
		if left*2 < ttl {
			continue
		}
		out = append(out, Record{
			Name: ServiceName,
			Type: typePTR,
			PTR:  InstanceName(c.Instance),
			TTL:  uint32(left),
		})
		if len(out) >= maxKnownAnswers {
			break
		}
	}
	return out
}

// Ask puts the browse question on the group from every socket, carrying the
// records it already holds as RFC 6762 §7.1 known answers, and follows up on any
// instance still missing its SRV, TXT or A. Listening alone is not enough: an
// announcement comes once, at the orb's join, so a hub started after that would be
// waiting on a rhythm it does not control.
//
// The known answers are what make this safe to do on a timer. §6 requires a querier
// that retransmits to suppress what it already knows, so a sustained browse lists
// its live, still-fresh instances and a responder that has nothing new stays quiet.
// Only records with more than half their TTL left are listed: a record past half is
// deliberately omitted so the next answer refreshes it before it can expire. Every
// packet still leaves for the group and only the group.
func (b *Browser) Ask() error {
	b.mu.Lock()
	srcs := append([]Source(nil), b.sources...)
	b.txid++
	txid := b.txid
	now := b.now()
	known := b.browseKnownLocked(now)
	waiting := make([]string, 0, len(b.pending))
	for k, p := range b.pending {
		if p.srv == nil || p.txt == nil || p.a == nil {
			waiting = append(waiting, k)
		}
	}
	b.mu.Unlock()
	if len(srcs) == 0 {
		return fmt.Errorf("orbmdns: no sockets attached")
	}
	pkts := [][]byte{BuildQueryWithKnown(txid, ServiceName, typePTR, known)}
	for _, inst := range waiting {
		if inst == "" {
			continue
		}
		pkts = append(pkts,
			InstanceQuery(txid, inst, typeSRV),
			InstanceQuery(txid, inst, typeTXT),
			HostQuery(txid, HostnameFor(inst)))
	}
	var firstErr error
	for _, s := range srcs {
		for _, p := range pkts {
			if _, err := s.WriteTo(p, groupEndpoint); err != nil && firstErr == nil {
				firstErr = err
			}
		}
	}
	if firstErr != nil {
		return fmt.Errorf("orbmdns: query: %w", firstErr)
	}
	return nil
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
	src := ""
	if from != nil {
		src = from.String()
	}
	m, err := Parse(pkt)
	if err != nil {
		b.refuse("", src, err.Error())
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
	name := strings.ToLower(rec.Name)
	if rec.TTL == 0 {
		b.mu.Lock()
		delete(b.hosts, name)
		b.mu.Unlock()
		return
	}
	ip, err := ParseOrbAddress(rec.A)
	if err != nil {
		b.refuse(rec.Name, from, err.Error())
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
	for _, c := range b.byMAC {
		if strings.EqualFold(HostnameFor(c.Instance), name) && c.IP != ip {
			c.IP = ip
		}
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
	c.From, c.Solicited = p.from, p.solicited
	c.LastSeen, c.Expires = now, now.Add(time.Duration(p.txt.TTL)*time.Second+b.grace)
	delete(b.pending, instance)
}

// rejectLocked drops an instance that failed a check and keeps its reason.
func (b *Browser) rejectLocked(instance string, p *partial, why string) {
	delete(b.pending, instance)
	b.refusals = append(b.refusals, Refusal{Name: InstanceName(instance), From: p.from, Reason: why, At: b.now()})
	if len(b.refusals) > maxRefusalTail {
		b.refusals = b.refusals[len(b.refusals)-maxRefusalTail:]
	}
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
			b.logf("orbmdns: %s said goodbye", instance)
		}
	}
}

// Candidates returns the live rows, first-seen first, pruning what the clock has
// already outlived. It dials nothing: answering "what orbs are on this network"
// without opening a socket to any of them is the item.
func (b *Browser) Candidates() []Candidate {
	now := b.now()
	b.mu.Lock()
	defer b.mu.Unlock()
	for mac, c := range b.byMAC {
		if now.After(c.Expires) {
			delete(b.byMAC, mac)
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
	b.logf("orbmdns: refused %q from %s: %s", name, from, reason)
}
