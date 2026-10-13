package orbfake

// mdns.go is the fake's _orb._tcp advertiser, for testing the hub's browser and
// for driving a real one by hand from cmd/orbfake.
//
// It is written out of main/net/orb_mdns.c — the encoder transcribed record by
// record, the query classifier transcribed from classify_query, the A-record rule
// transcribed from build_answer — and NOT out of internal/orbmdns, which is the
// code it is here to exercise. The two share no byte of wire format on purpose.
// The package doc gives the reason this double quotes the firmware rather than the
// client, and it applies with extra force here: a fake browser and a fake device
// written in the same afternoon agree with each other about anything, including
// the RDLENGTH that would make a real browser drop every record after it.
//
// What pins it instead lives in mdns_contract_test.go: the bytes this file emits
// are compared against an answer captured off the air from orb-B and read there by
// python-zeroconf, and against the firmware's own format literals lifted out of
// orb_mdns.c. Neither check can pass by both sides being wrong together.

import (
	"fmt"
	"net"
	"strings"
	"sync"
	"time"
)

// The firmware's own numbers, named rather than inlined: an edit here that drifts
// from orb_mdns.c is the drift mdns_contract_test.go exists to catch.
const (
	mdnsGroup   = "224.0.0.251"
	mdnsPort    = 5353
	mdnsTTL     = 120 // MDNS_TTL, the goodbye's is 0
	mdnsBuf     = 512 // a DNS packet over UDP is capped at 512 B
	mdnsHdrLen  = 12
	mdnsTypePTR = 12
	mdnsTypeTXT = 16
	mdnsTypeA   = 1
	mdnsTypeSRV = 33
	// MDNS_CLASS_FLUSH: our names are unique by MAC, so they are stated
	// authoritatively. Without the bit a browser may merge a stranger's record for
	// the same name into its cache beside ours.
	mdnsClassFlush = 0x8001
	mdnsClassIN    = 1

	// announceRepeat is RFC 6762 §8.3's several copies, one per second, and the
	// firmware's ANNOUNCE_REPEAT.
	announceRepeat  = 4
	announceSpacing = time.Second
)

// mdnsID is orb_mdns_id_t: the three names one orb owns, all derived from the same
// base MAC so the instance, the service and the host cannot drift apart.
type mdnsID struct {
	instance     string
	hostname     string
	mac          string
	fw           string
	provisioning bool
}

// newMDNSID forms the identity from the fake's own radio state, the way orb_net.c
// fills the struct once from the reads it already made for /api/v1/status. The
// instance is "orb-" plus the last three bytes of the base MAC — the six %c
// arguments off mac[6..11] in mdns_update — so a fake whose instance and mac
// disagree cannot be built by hand at all.
func newMDNSID(d *device) mdnsID {
	mac := strings.ToLower(d.MAC)
	id := mdnsID{mac: mac, fw: d.FW, provisioning: d.Provisioning}
	if len(mac) >= 6 {
		id.instance = fmt.Sprintf("orb-%s", mac[len(mac)-6:])
		id.hostname = id.instance + ".local"
	}
	return id
}

// AdvertiseConfig tunes one advertiser. A test sets Conn to a socket bound to
// 127.0.0.1 and Port to whatever the fake's HTTP happens to listen on; live runs
// leave both alone and take the real mDNS port.
type AdvertiseConfig struct {
	// Conn is the socket the advertiser answers on. Nil binds 0.0.0.0:5353.
	Conn PacketSock
	// Group is where announcements go. Empty is the mDNS group; a loopback test
	// points it at the browser's own address. It is the announce destination and
	// nothing else — the browser never learns to ask a single host.
	Group string
	// Repeats and Interval are the announce burst's shape; zero takes the
	// firmware's four copies a second apart.
	Repeats  int
	Interval time.Duration
	// ListenPort is the UDP port to bind and answer on. Zero means 5353, the port
	// every real browser asks on and the only one a hub will hear. An ephemeral one
	// is for a second advertiser on a box that already holds 5353, and it is deaf to
	// any browser that is not told about it — including this repo's own hub.
	ListenPort int
	// Port is the SRV port, the one in the announcement, not the one bound here.
	// Zero means 80, the device's httpd.
	Port int
}

// Advertiser is the fake's responder: it owns a socket, answers the four question
// kinds an orb answers, and announces on command. Build it with Orb.Advertise.
type Advertiser struct {
	o   *Orb
	cfg AdvertiseConfig

	staIP net.IP
	apIP  net.IP

	mu      sync.Mutex
	conn    PacketSock
	closing bool
	closed  bool

	// Counters, because "the hub never saw it" needs an answer that is not a
	// guess: deaf responder, malformed query, or a browser that never arrived.
	// refused counts only the silences that mean something is broken; foreign
	// counts the traffic this responder is correct to ignore, which on a real LAN is
	// nearly all of it and would otherwise drown the line that matters.
	seen, answered, refused, foreign int
	lastRefusal                      string
}

// Advertise returns an advertiser for this orb's current radio state. The identity
// is re-read per packet, so a test that moves the lease or opens the SoftAP between
// queries sees the announcement follow it exactly as the device's would.
func (o *Orb) Advertise(cfg AdvertiseConfig) *Advertiser {
	if cfg.Repeats <= 0 {
		cfg.Repeats = announceRepeat
	}
	if cfg.Interval <= 0 {
		cfg.Interval = announceSpacing
	}
	if cfg.Port == 0 {
		cfg.Port = 80
	}
	if cfg.Group == "" {
		cfg.Group = mdnsGroup
	}
	a := &Advertiser{o: o, cfg: cfg}
	a.conn = cfg.Conn
	return a
}

// --- the encoder ------------------------------------------------------------------
//
// A transcription of orb_mdns.c's codec half: same record order (PTR, SRV, TXT, A),
// same compression policy (each name written once, later records pointing at it),
// same RDLENGTH patched after the fact, same rule that anything that would not fit
// drops the WHOLE packet. Half a response parses, and the browser caches the wrong
// thing for the whole TTL — that is why every helper here returns an "would not
// fit" signal rather than writing what it can.

// rrWriter is the firmware's rr_writer_t: the buffer, the write head, and the
// offsets of the three names this message has already spent bytes on (-1 = not
// yet). Four uncompressed copies of three names is what pushes a PTR answer toward
// the 512 B cap the answer must not exceed.
type rrWriter struct {
	t                  []byte
	cap                int
	off                int
	atService          int
	atInstance, atHost int
}

func newRRWriter(cap int) *rrWriter {
	return &rrWriter{t: make([]byte, 0, cap), cap: cap,
		atService: -1, atInstance: -1, atHost: -1}
}

func (w *rrWriter) fits(n int) bool { return w.off+n <= w.cap }

func (w *rrWriter) u16(v uint16) bool {
	if !w.fits(2) {
		return false
	}
	w.t = append(w.t, byte(v>>8), byte(v))
	w.off += 2
	return true
}

func (w *rrWriter) u8(v byte) bool {
	if !w.fits(1) {
		return false
	}
	w.t = append(w.t, v)
	w.off++
	return true
}

// ptr is a compression pointer: 0xC0 plus the 14-bit offset of a name already in
// this message. The high-bit test is what tells it from a length octet.
func (w *rrWriter) ptr(target int) bool {
	if target < 0 || target > 0x3FFF {
		return false
	}
	return w.u16(uint16(0xC000 | target))
}

// name writes "orb-782618.local" as labels + root. An empty label, an over-long
// label or a name that would run past the buffer is a refusal, matching put_name.
func (w *rrWriter) name(dotted string) bool {
	for dotted != "" {
		seg := dotted
		rest := ""
		if i := strings.IndexByte(dotted, '.'); i >= 0 {
			seg, rest = dotted[:i], dotted[i+1:]
		} else {
			dotted = ""
		}
		if seg == "" {
			dotted = rest
			continue
		}
		if len(seg) > 63 || !w.u8(byte(len(seg))) {
			return false
		}
		if !w.fits(len(seg)) {
			return false
		}
		w.t = append(w.t, seg...)
		w.off += len(seg)
		dotted = rest
	}
	return w.u8(0)
}

// frame writes TYPE/CLASS/TTL/RDLENGTH after the owner name has been written, and
// hands back the index of the RDLENGTH octets for the caller to patch.
func (w *rrWriter) frame(typ, class uint16, ttl uint32) (int, bool) {
	if !w.u16(typ) || !w.u16(class) {
		return 0, false
	}
	if !w.fits(4) {
		return 0, false
	}
	w.t = append(w.t, byte(ttl>>24), byte(ttl>>16), byte(ttl>>8), byte(ttl))
	w.off += 4
	rdlenAt := w.off
	if !w.u16(0) {
		return 0, false
	}
	return rdlenAt, true
}

// rr writes the owner — a pointer when this message already carries the name, full
// labels otherwise — then the frame.
func (w *rrWriter) rr(at int, name string, typ, class uint16, ttl uint32) (int, bool) {
	if at >= 0 {
		if !w.ptr(at) {
			return 0, false
		}
	} else {
		if name == "" || !w.name(name) {
			return 0, false
		}
	}
	return w.frame(typ, class, ttl)
}

// patchRDLEN closes a record whose rdata has just been written. A response whose
// length field lies is not slightly wrong: the browser resyncs at the wrong offset
// and drops every record after it, so this arithmetic is the whole ballgame.
func (w *rrWriter) patch(rdlenAt int) bool {
	rd := w.off - (rdlenAt + 2)
	if rd < 0 || rd > 0xFFFF {
		return false
	}
	w.t[rdlenAt] = byte(rd >> 8)
	w.t[rdlenAt+1] = byte(rd)
	return true
}

// txtString writes one length octet then the bytes, never compressed: RFC 6762
// §6.1 forbids compressing inside TXT rdata, and a browser that finds a pointer
// there reads two garbage bytes instead of the key.
func (w *rrWriter) txtString(kv string) bool {
	if len(kv) == 0 || len(kv) > 255 || !w.u8(byte(len(kv))) {
		return false
	}
	if !w.fits(len(kv)) {
		return false
	}
	w.t = append(w.t, kv...)
	w.off += len(kv)
	return true
}

// respBegin writes the twelve-byte header. flags carries QR|AA for an answer and
// zero for an unsolicited announce, and counts are patched by respCounts once the
// record sections exist.
func (w *rrWriter) respBegin(txid, flags uint16, qd int) bool {
	if !w.u16(txid) || !w.u16(flags) || !w.u16(uint16(qd)) {
		return false
	}
	return w.u16(0) && w.u16(0) && w.u16(0)
}

// respCounts patches the two counts the header promised and clears the third.
//
// The additional-record total goes at offset 8, which in a textbook DNS header is
// NSCOUNT, and offset 10 — ARCOUNT — stays zero. That is what the shipped responder
// does (orb_mdns.c:resp_counts), the capture in internal/orbmdns/testdata proves it
// on the wire, and the hub's browser reads ANSWER, AUTHORITY and ADDITIONAL as one
// pool precisely because of it. This file reproduces the shape rather than
// correcting it: a double that emitted the textbook header would let the hub's
// browser pass here and find nothing on the LAN, where the records really do sit in
// the authority field.
func (w *rrWriter) respCounts(an, ar int) {
	w.t[6] = byte(an >> 8)
	w.t[7] = byte(an)
	w.t[8] = byte(ar >> 8)
	w.t[9] = byte(ar)
	w.t[10] = 0
	w.t[11] = 0
}

// emitRecords writes the four records one orb owns, in the order that lets each
// point at the last, with the want_* flags choosing which belong in ANSWER and the
// rest following as ADDITIONAL — so one browse reply carries everything a browser
// needs to dial without asking three more questions.
//
// The instance FQDN is formed once, here. The SRV/TXT owner name and the PTR's
// rdata are the same string, and if each built its own the packet could carry two
// spellings of the instance and the browser would see two services.
func (w *rrWriter) emitRecords(id mdnsID, ip net.IP, wantPTR bool, wantSRV, wantTXT, wantA bool,
	ttl uint32, port int) (int, int, bool) {
	an, ar := 0, 0
	instFQDN := id.instance + "." + mdnsServiceName

	if wantPTR {
		rl, ok := w.rr(w.atService, mdnsServiceName, mdnsTypePTR, mdnsClassIN, ttl)
		if !ok {
			return 0, 0, false
		}
		if w.atInstance < 0 {
			w.atInstance = w.off
			if !w.name(instFQDN) {
				return 0, 0, false
			}
		} else if !w.ptr(w.atInstance) {
			return 0, 0, false
		}
		if !w.patch(rl) {
			return 0, 0, false
		}
		an++
	}

	if wantSRV || wantPTR {
		rl, ok := w.rr(w.atInstance, instFQDN, mdnsTypeSRV, mdnsClassFlush, ttl)
		if !ok {
			return 0, 0, false
		}
		if !w.u16(0) || !w.u16(0) || !w.u16(uint16(port)) { // priority, weight, port
			return 0, 0, false
		}
		if w.atHost < 0 {
			w.atHost = w.off
			if !w.name(id.hostname) {
				return 0, 0, false
			}
		} else if !w.ptr(w.atHost) {
			return 0, 0, false
		}
		if !w.patch(rl) {
			return 0, 0, false
		}
		if wantSRV {
			an++
		} else {
			ar++
		}
	}

	if wantTXT || wantPTR {
		rl, ok := w.rr(w.atInstance, instFQDN, mdnsTypeTXT, mdnsClassFlush, ttl)
		if !ok {
			return 0, 0, false
		}
		state := "normal"
		if id.provisioning {
			state = "provisioning"
		}
		for _, kv := range []string{"mac=" + id.mac, "fw=" + id.fw, "state=" + state} {
			if !w.txtString(kv) {
				return 0, 0, false
			}
		}
		if !w.patch(rl) {
			return 0, 0, false
		}
		if wantTXT {
			an++
		} else {
			ar++
		}
	}

	if wantA || wantSRV || wantTXT || wantPTR {
		if ip == nil || ip.To4() == nil {
			// No address to give on this interface. Emitting 0.0.0.0 would be worse
			// than saying nothing: the browser caches a host with no address for the
			// whole TTL and never falls back to the record it could have used.
			if wantA {
				return 0, 0, false
			}
		} else {
			rl, ok := w.rr(w.atHost, id.hostname, mdnsTypeA, mdnsClassFlush, ttl)
			if !ok {
				return 0, 0, false
			}
			v4 := ip.To4()
			if !w.fits(4) {
				return 0, 0, false
			}
			w.t = append(w.t, v4[0], v4[1], v4[2], v4[3])
			w.off += 4
			if !w.patch(rl) {
				return 0, 0, false
			}
			if wantA {
				an++
			} else {
				ar++
			}
		}
	}
	return an, ar, true
}

// PacketSock is the three socket calls the advertiser uses. Declaring it rather than
// taking a net.PacketConn is what lets a unit test capture the wire on a machine
// that cannot bind 5353 — and it is the same shape internal/orbmdns.Source is, for
// the same reason, arrived at separately.
type PacketSock interface {
	ReadFrom(p []byte) (int, net.Addr, error)
	WriteTo(p []byte, addr net.Addr) (int, error)
	Close() error
}

// mdnsServiceName is the type the hub browses for. Restated here rather than
// imported from internal/orbmdns: the fake and the browser must fail apart when
// either drifts from the firmware, and a shared constant is how they would instead
// agree on a typo.
const mdnsServiceName = "_orb._tcp.local"

// --- the query classifier ----------------------------------------------------------
//
// Its own decoder, from the firmware's classify_query: not the browser's. The
// responder has to be able to read a query a third-party stack wrote, and reading
// it with the code that writes the answer is how both could be wrong together.

// silence is the responder's reason for saying nothing, and whether saying nothing
// was the right call or a packet worth reading. The distinction is the whole point of
// the type: this fake runs on a LAN where every other box talks mDNS too, so "refused
// 8" over a browse means nothing until the 8 are split into neighbours (avahi,
// CUPS, the printer) and something actually broken. The firmware's classify_query
// returns mdns_err_t and logs at three different points for the same reason; here the
// counters are the log.
type silence struct {
	why    string
	defect bool
}

var (
	silentResponse  = silence{"an answer rather than a question", false}
	silentOpcode    = silence{"an opcode other than QUERY", false}
	silentQuestions = silence{"not exactly one question", false}
	silentSections  = silence{"a query carrying answer records", false}
	silentClass     = silence{"a question in a class other than IN", false}
	silentForeign   = silence{"another service's question", false}
	// The two defects: a packet that should have been readable and was not. Both
	// are this fake's own bug report as much as a peer's, which is why they are not
	// allowed to hide among the neighbours.
	silentShort  = silence{"a datagram shorter than a DNS header", true}
	silentName   = silence{"a question name that does not parse", true}
	silentEncode = silence{"the answer would not encode", true}
)

// classifyQuery reports which of the four kinds a query is, or -1 with the reason it
// is not this orb's to answer. Responses, non-QUERY opcodes, multi-question packets
// and foreign classes are all silence rather than a wrong answer.
func classifyQuery(id mdnsID, rx []byte) (int, []byte, silence) {
	const qNone, qPTR, qSRV, qTXT, qA = -1, 0, 1, 2, 3
	if len(rx) < mdnsHdrLen {
		return qNone, nil, silentShort
	}
	if rx[2]&0x80 != 0 {
		return qNone, nil, silentResponse // QR set: that is an answer, and not ours to answer
	}
	if rx[2]&0x78 != 0 {
		return qNone, nil, silentOpcode // opcode must be QUERY
	}
	if rx[4] != 0 || rx[5] != 1 {
		return qNone, nil, silentQuestions // exactly one question
	}
	for _, b := range rx[6:12] {
		if b != 0 {
			return qNone, nil, silentSections // an answer section in a query is not this firmware's shape
		}
	}
	name, after, ok := readQueryName(rx)
	if !ok || after+4 > len(rx) {
		return qNone, nil, silentName
	}
	qtype := uint16(rx[after])<<8 | uint16(rx[after+1])
	class := uint16(rx[after+2])<<8 | uint16(rx[after+3])
	// The top bit of the question class is RFC 6762's unicast-request flag, not
	// part of the class: avahi and zeroconf both ask with 0x8001, so comparing
	// against a bare 1 refuses every real browse on the floor.
	if class&0x7FFF != mdnsClassIN {
		return qNone, nil, silentClass
	}
	instFQDN := id.instance + "." + mdnsServiceName
	end := after + 4
	switch {
	case qtype == mdnsTypePTR && equalName(name, mdnsServiceName):
		return qPTR, rx[:end], silence{}
	case qtype == mdnsTypeSRV && equalName(name, instFQDN):
		return qSRV, rx[:end], silence{}
	case qtype == mdnsTypeTXT && equalName(name, instFQDN):
		return qTXT, rx[:end], silence{}
	case qtype == mdnsTypeA && equalName(name, id.hostname):
		return qA, rx[:end], silence{}
	}
	return qNone, nil, silentForeign
}

// readQueryName walks a query's QNAME from offset 12 and returns it lowercased
// along with the offset just past the root label. Compression is handled — a
// hostile or quirky stack may ask a compressed question, and a responder that
// refuses it reads as deaf — with the same two rules the firmware's walker uses:
// a pointer may only aim backwards into bytes already consumed, and the hops are
// budgeted, because a self-referential pair must cost nothing to reject rather
// than spin the task that reads it.
func readQueryName(rx []byte) (string, int, bool) {
	off, hops, consumed := mdnsHdrLen, 0, mdnsHdrLen
	var labels []string
	linearLayout := true
	for {
		if off < 0 || off >= len(rx) {
			return "", 0, false
		}
		l := rx[off]
		if l == 0 {
			off++
			break
		}
		if l&0xC0 == 0xC0 {
			if off+2 > len(rx) {
				return "", 0, false
			}
			target := int(l&0x3F)<<8 | int(rx[off+1])
			if target >= consumed || hops >= 8 {
				return "", 0, false
			}
			if linearLayout {
				// The field ends with the pointer; everything after it is TYPE/CLASS.
				off, linearLayout = target, false
			} else {
				off = target
			}
			hops++
			continue
		}
		if l&0xC0 != 0 {
			return "", 0, false
		}
		if off+1+int(l) > len(rx) {
			return "", 0, false
		}
		labels = append(labels, strings.ToLower(string(rx[off+1:off+1+int(l)])))
		off += 1 + int(l)
		if off > consumed {
			consumed = off
		}
	}
	// A name reached through a pointer ends where the pointer said; the two
	// octets of the pointer are the whole field for the sake of finding TYPE.
	end := off
	if !linearLayout {
		end = consumed
	}
	return strings.Join(labels, "."), end, true
}

// equalName compares two dotted names case-insensitively, ignoring a trailing
// root dot: DNS names are, and an orb whose TXT only answers to one capitalisation
// is a browse that silently finds nothing depending on how the browser spelled it.
func equalName(a, b string) bool {
	t := func(s string) string {
		s = strings.ToLower(s)
		return strings.TrimSuffix(s, ".")
	}
	return t(a) == t(b)
}

// --- the two builders --------------------------------------------------------------

// AnswerTo encodes the reply to one received query, or returns nil for silence.
//
// srcIP is the QUERIER's address, not ours. The device has no working IP_PKTINFO
// (sockets.h:282 declares the option and sock.c never implements it), so its one
// socket carrying two joins cannot report which interface a datagram arrived on,
// and the querier's subnet is the substitute — a host can only ask from a network
// it is attached to. The fake follows the same rule, because a double that always
// handed back the STA address would let the hub pass a test the SoftAP cannot
// satisfy and vice versa.
func (a *Advertiser) AnswerTo(rx []byte, srcIP net.IP) []byte {
	a.mu.Lock()
	conn := a.conn
	a.mu.Unlock()
	if conn == nil {
		return nil
	}
	id, sta, ap := a.o.advertiseState()
	kind, qbytes, s := classifyQuery(id, rx)
	if kind < 0 {
		a.noteSilence(s)
		return nil
	}
	a.seeQuery()
	ip := sta
	if ap != nil && sameSubset(srcIP, ap) {
		ip = ap
	}
	if sta == nil {
		ip = ap
	}
	w := newRRWriter(mdnsBuf)
	txid := uint16(0)
	if len(rx) >= 2 {
		txid = uint16(rx[0])<<8 | uint16(rx[1])
	}
	// The three ways an answer can fail to be producible are all this responder's
	// fault rather than the querier's, and all three are counted, because a fake
	// that goes quietly silent on its own bug is the failure mode the counters
	// exist to catch.
	if !w.respBegin(txid, 0x8400, 1) {
		a.noteSilence(silentEncode)
		return nil
	}
	// Echo the question verbatim. Beyond twelve the first name written here is the
	// one a record is about, so the offsets below plant it rather than repeat it.
	qb := qbytes[mdnsHdrLen:]
	if len(qb) <= 0 || !w.fits(mdnsHdrLen+len(qb)) {
		a.noteSilence(silentEncode)
		return nil
	}
	w.t = append(w.t, qb...)
	w.off += len(qb)
	switch kind {
	case 0:
		w.atService = mdnsHdrLen
	case 1, 2:
		w.atInstance = mdnsHdrLen
	case 3:
		w.atHost = mdnsHdrLen
	}
	an, ar, ok := w.emitRecords(id, ip, kind == 0, kind == 1, kind == 2, kind == 3, mdnsTTL, a.cfg.Port)
	if !ok || an == 0 {
		a.noteSilence(silentEncode)
		return nil
	}
	w.respCounts(an, ar)
	a.noteAnswered()
	return w.t
}

// AnnouncePacket encodes one unsolicited announcement for one interface, carrying
// THAT interface's A record. goodbye emits TTL 0 so caches drop the orb at once
// rather than holding a ghost until the 120 s expiry.
func AnnouncePacket(id mdnsID, ip net.IP, port int, goodbye bool) []byte {
	w := newRRWriter(mdnsBuf)
	// QR|AA in the announce too, exactly as orb_mdns.c's resp_begin writes it for
	// both paths: an unsolicited record still announces with authority, and a
	// browser that saw QR clear would file it as a question it must answer.
	if !w.respBegin(0, 0x8400, 0) {
		return nil
	}
	ttl := uint32(mdnsTTL)
	if goodbye {
		ttl = 0
	}
	an, _, ok := w.emitRecords(id, ip, true, false, false, false, ttl, port)
	if !ok || an == 0 {
		return nil
	}
	w.respCounts(an, 3)
	return w.t
}

// sameSubset is the /24 test orb_mdns.c does on the raw network-order words: the
// low three octets of a class-C lease are the ones that identify the interface.
func sameSubset(a, b net.IP) bool {
	av, bv := a.To4(), b.To4()
	if av == nil || bv == nil {
		return false
	}
	return av[0] == bv[0] && av[1] == bv[1] && av[2] == bv[2]
}

// advertiseState snapshots what the announcement is built from: the identity the
// radio state implies, plus the two interface addresses. They are read together,
// because an announcement whose instance comes from one lease and whose A record
// comes from the other names an orb that does not exist.
func (o *Orb) advertiseState() (mdnsID, net.IP, net.IP) {
	o.mu.Lock()
	defer o.mu.Unlock()
	d := o.deviceLocked()
	id := newMDNSID(d)
	var sta, ap net.IP
	if d.IP != "" && d.StaUp {
		sta = net.ParseIP(d.IP)
	}
	if d.APOpen && d.ApIP != "" {
		ap = net.ParseIP(d.ApIP)
	}
	return id, sta, ap
}

// serveLoop answers queries on the advertiser's socket until Close. It is the thin
// part: every encoding decision lives in AnswerTo and AnnouncePacket, where the
// host suite can drive it without a socket in sight.
func (a *Advertiser) serveLoop(buf []byte) {
	for {
		n, from, err := a.conn.ReadFrom(buf)
		if err != nil {
			return
		}
		a.mu.Lock()
		done := a.closed || a.closing
		a.mu.Unlock()
		if done || n <= 0 {
			return
		}
		var src net.IP
		if ua, ok := from.(*net.UDPAddr); ok {
			src = ua.IP
		}
		tx := a.AnswerTo(append([]byte(nil), buf[:n]...), src)
		if tx == nil {
			continue
		}
		if _, err := a.conn.WriteTo(tx, from); err != nil {
			return
		}
	}
}

// AnswerOnly serves queries and nothing else: no announce burst, no goodbye. A
// Announce sends the burst — the firmware's four copies a second apart — out one
// socket to the group, then keeps answering questions. The burst is what makes a
// hub that joined the group late see the orb without a query; the firmware sends it
// on every join for the same reason.
func (a *Advertiser) Announce() error {
	if err := a.bind(); err != nil {
		return err
	}
	id, sta, ap := a.o.advertiseState()
	to := &net.UDPAddr{IP: net.ParseIP(a.cfg.Group), Port: mdnsPort}
	a.mu.Lock()
	c := a.conn
	a.mu.Unlock()
	for _, ip := range []net.IP{sta, ap} {
		if ip == nil {
			continue
		}
		for i := 0; i < a.cfg.Repeats; i++ {
			pkt := AnnouncePacket(id, ip, a.cfg.Port, false)
			if pkt == nil {
				return fmt.Errorf("orbfake: announcement would not encode")
			}
			if _, err := c.WriteTo(pkt, to); err != nil {
				return fmt.Errorf("orbfake: announce: %w", err)
			}
			if i+1 < a.cfg.Repeats {
				time.Sleep(a.cfg.Interval)
			}
		}
	}
	return nil
}

// Goodbye emits the TTL-0 pair before the socket goes. Without it a hub that had
// this orb cached keeps offering it until the 120 s expiry, which is the ghost the
// plan refuses to leave in a fleet list.
func (a *Advertiser) Goodbye() {
	id, sta, ap := a.o.advertiseState()
	a.mu.Lock()
	c := a.conn
	a.mu.Unlock()
	if c == nil {
		return
	}
	to := &net.UDPAddr{IP: net.ParseIP(a.cfg.Group), Port: mdnsPort}
	for _, ip := range []net.IP{sta, ap} {
		if ip == nil {
			continue
		}
		if pkt := AnnouncePacket(id, ip, a.cfg.Port, true); pkt != nil {
			_, _ = c.WriteTo(pkt, to)
		}
	}
}

// Serve binds, answers queries and announces once, for a live run of cmd/orbfake.
// It returns after the burst: the answering goes on in its own goroutine until the
// advertiser is closed.
func (a *Advertiser) Serve() error {
	if err := a.bind(); err != nil {
		return err
	}
	go a.serveLoop(make([]byte, mdnsBuf))
	return a.Announce()
}

// test that wants the wire behaviour without a second process putting packets on
// the group uses this.
func (a *Advertiser) AnswerOnly() error {
	if err := a.bind(); err != nil {
		return err
	}
	go a.serveLoop(make([]byte, mdnsBuf))
	return nil
}

// ServeOnce drains whatever a test queued and answers it, on the calling goroutine.
// serveLoop is the same code with a socket blocking behind it; this exists so the
// wire behaviour can be asserted without a scheduler in the way.
func (a *Advertiser) ServeOnce() bool {
	buf := make([]byte, mdnsBuf)
	n, from, err := a.conn.ReadFrom(buf)
	if err != nil || n <= 0 {
		return false
	}
	var src net.IP
	if ua, ok := from.(*net.UDPAddr); ok {
		src = ua.IP
	}
	tx := a.AnswerTo(append([]byte(nil), buf[:n]...), src)
	if tx == nil {
		return true
	}
	_, _ = a.conn.WriteTo(tx, from)
	return true
}

// AnnounceBytes builds this advertiser's current unsolicited announcement, for a
// caller that wants the bytes without a socket: the identity and both interface
// addresses are read from the orb's live state, the same single read a real reply
// would make. Goodbye emits TTL 0, so caches drop the orb at once instead of
// holding a ghost until the expiry.
func (a *Advertiser) AnnounceBytes(goodbye bool) []byte {
	id, sta, ap := a.o.advertiseState()
	if sta == nil {
		sta = ap // a SoftAP-only boot has no lease to name
	}
	return AnnouncePacket(id, sta, a.cfg.Port, goodbye)
}

// Binds reports whether the advertiser has a socket yet.
func (a *Advertiser) Bound() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.conn != nil
}

// bind takes the configured socket, or opens one on the device's arrangement: bound
// to the wildcard at the browser-facing port, one group membership per eligible
// interface (the firmware's two joins, at orb_mdns.c's mdns_start_with), SO_REUSEADDR
// so a responder already on 5353 does not turn this binary into an error message, and
// IP_MULTICAST_LOOP left ON so a browser on the same host as the fake can hear it at
// all. The options are set in mdnssock_unix.go, not through
// net.ListenMulticastUDP, which binds the group but turns the loopback OFF and sets no
// SO_REUSEADDR before the bind — an advertiser built that way is unfoundable on the
// one bench this item is judged on, and would be reported as a LAN with no orbs.
//
// One consequence is worth naming rather than discovering later: the fake answers only
// queries that arrive at the group. A query aimed at one address, such as the bench
// script's --unicast-out trick, reaches a real orb (its responder binds INADDR_ANY)
// and not this.
func (a *Advertiser) bind() error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.conn != nil {
		return nil // the caller's socket: tests inject one, and it is theirs to close.
	}
	group := net.ParseIP(mdnsGroup)
	if a.cfg.Group != "" {
		group = net.ParseIP(a.cfg.Group)
	}
	if group == nil {
		return fmt.Errorf("orbfake: %q is not a group address", a.cfg.Group)
	}
	port := mdnsPort
	if a.cfg.ListenPort != 0 {
		port = a.cfg.ListenPort
	}
	pc, err := groupSock(port, group)
	if err != nil {
		return err
	}
	a.conn = pc
	return nil
}

// LocalAddr reports the socket's bound address, which is how a test learns the
// ephemeral port it was given.
func (a *Advertiser) LocalAddr() net.Addr {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.conn == nil {
		return nil
	}
	// The socket is only asked for its own address by a caller that bound it and
	// wants the ephemeral port back; a test's recorder need not answer.
	l, ok := a.conn.(interface{ LocalAddr() net.Addr })
	if !ok {
		return nil
	}
	return l.LocalAddr()
}

// Close stops the responder and releases the socket it opened.
func (a *Advertiser) Close() error {
	a.mu.Lock()
	if a.closed {
		a.mu.Unlock()
		return nil
	}
	a.closed, a.closing = true, true
	c := a.conn
	a.mu.Unlock()
	if c == nil {
		return nil
	}
	return c.Close()
}

// Stats reports what the responder heard, because "the hub never saw it" has three
// possible causes and a counter is the only way to tell them apart from here. refused
// is the count worth acting on; foreign is the neighbours, and is only interesting
// when it is zero — a responder that has seen nothing at all is deaf, not polite.
func (a *Advertiser) Stats() (seen, answered, refused, foreign int, why string) {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.seen, a.answered, a.refused, a.foreign, a.lastRefusal
}

func (a *Advertiser) seeQuery() {
	a.mu.Lock()
	a.seen++
	a.mu.Unlock()
}

func (a *Advertiser) noteAnswered() {
	a.mu.Lock()
	a.answered++
	a.mu.Unlock()
}

// noteSilence records a datagram the responder did not answer. Whether that is a
// defect or the neighbours is the silence's own call, and the split is the reason the
// counters are readable on a live LAN: see Stats.
func (a *Advertiser) noteSilence(s silence) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.seen++
	if s.defect {
		a.refused++
		a.lastRefusal = s.why
		return
	}
	a.foreign++
}
