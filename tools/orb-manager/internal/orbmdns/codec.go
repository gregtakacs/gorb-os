// Package orbmdns browses _orb._tcp on the LAN: it asks, it listens, and it
// reports what answered. It never dials what it found.
//
// That last sentence is the design constraint, and it is enforced twice: this
// package imports no net/http and no orbnet (imports_test.go parses its own
// import blocks to prove it), and an announcement that cannot name its chip is
// refused rather than listed. An address out of a browse is a lead, not a
// permission — the proxy still dials only what ipGrants holds, and Item 6's MAC
// cross-lock is what turns a lead into a fleet row.
//
// Why the wire format is hand-rolled: the hub carries exactly one dependency
// (go.bug.st/serial) and builds offline against a committed vendor tree, and the
// other end of this wire already speaks a hand-rolled dialect
// (main/net/orb_mdns.c). A browser that has to agree with that file is reading
// the right specification.
//
// Proof, and what it is proof of: testdata holds answers captured OFF THE AIR
// from orb-B and read there by python-zeroconf 0.151.5. The decoder is tested
// against THOSE bytes — not against bytes this package or internal/orbfake
// invented. My parser agreeing with my encoder is exactly the self-agreement the
// ladder refuses.
package orbmdns

import (
	"encoding/binary"
	"errors"
	"fmt"
	"net"
	"strings"
)

const (
	// ServiceName is the DNS-SD type the firmware advertises (orb_mdns.c's
	// SERVICE_NAME). The trailing dot is a wire form, not a spelling: names are
	// compared after readName strips it.
	ServiceName = "_orb._tcp.local"
	// GroupAddr is RFC 6762's mDNS group. Written as dotted-quad so a transposed
	// nibble cannot silently bind to nothing.
	GroupAddr = "224.0.0.251"
	// Port is mDNS's UDP port. A querier sends from it and hears the unicast
	// answer on it, because the firmware replies to the querier's socket.
	Port = 5353

	typeA, typePTR, typeTXT, typeSRV = 1, 12, 16, 33

	classIN  = 1
	classQU  = 0x8000
	cacheBit = 0x8000

	headerLen   = 12
	maxNameLen  = 255
	maxNameHops = 8
	// maxRecords bounds the whole section count. One orb's answer is four
	// records; anything hungrier is a crafted packet, and an unbounded loop here
	// is a hub UI task spinning on someone else's bytes.
	maxRecords = 64
	// maxRDLen is a UDP/mDNS datagram's practical ceiling. A 512 B cap is what
	// the firmware encodes to; 1500 leaves room for a browser from another stack
	// without ever letting an RDLENGTH address past the buffer.
	maxRDLen = 1500
)

// ErrShort is the refusal for a datagram that ends inside a header, a label or an
// rdata. A half-read answer caches a wrong orb for the whole TTL, which is worse
// than no orb at all, so Parse never returns a partial Message.
var ErrShort = errors.New("orbmdns: packet ends inside a field")

// Record is one resource record with its owner name already expanded. Class has
// the cache-flush bit masked off and CacheFlush carries it: a record that sets it
// claims uniqueness, and a cache must replace rather than merge on that claim
// (RFC 6762 §6.3).
type Record struct {
	Name       string
	Type       uint16
	Class      uint16
	CacheFlush bool
	TTL        uint32

	// PTR is a PTR record's rdata name — the instance a service has.
	PTR string
	// SRVTarget and SRVPort are a SRV's host and port. Priority and weight are
	// read and dropped: one orb, one port, nothing to choose between.
	SRVTarget string
	SRVPort   uint16
	// TXT holds the decoded key=value pairs with keys lowercased.
	TXT map[string]string
	// A is an A record's IPv4 address.
	A net.IP
}

// Question is one question-section entry.
type Question struct {
	Name  string
	Type  uint16
	Class uint16
	// QU is RFC 6762's unicast-request bit, read out of the class and not part
	// of it. The firmware tolerates it for the same reason (orb_mdns.c masks
	// 0x7FFF) and so must the browser, which sets it when it asks.
	QU bool
}

// Message is a parsed datagram. The three record sections are kept apart because
// the header's counts are evidence: the firmware's responder writes its
// additional-record total into the NSCOUNT field (orb_mdns.c:resp_counts patches
// offset 8), so an orb's SRV, TXT and A arrive counted as authority. A browser
// that read only ANSWER plus ADDITIONAL would find an orb with no address on
// every single browse. Browse reads all three; that decision is pinned to the
// capture in testdata, not to a reading of the C.
type Message struct {
	ID          uint16
	Response    bool
	Opcode      uint8
	Questions   []Question
	Answers     []Record
	Authorities []Record
	Additionals []Record
}

// Records pools the three sections, which is what a candidate needs and what the
// NSCOUNT note above makes necessary.
func (m *Message) Records() []Record {
	out := make([]Record, 0, len(m.Answers)+len(m.Authorities)+len(m.Additionals))
	out = append(out, m.Answers...)
	out = append(out, m.Authorities...)
	out = append(out, m.Additionals...)
	return out
}

// Parse decodes one mDNS datagram, strictly. Every offset is checked before it is
// read; a compression pointer may only point backwards into bytes already
// consumed, which is what makes the name walk terminate without a visited set;
// reserved label forms are refused; and the record walk must land exactly on the
// end of the message. That last rule is the one nothing else catches: an RDLENGTH
// that lies leaves a browser resyncing in the middle of a record, and a parser
// that tolerates it reports an orb no other stack can see.
func Parse(b []byte) (*Message, error) {
	if len(b) < headerLen {
		return nil, ErrShort
	}
	m := &Message{
		ID:       binary.BigEndian.Uint16(b[0:]),
		Response: b[2]&0x80 != 0,
		Opcode:   (b[2] >> 3) & 0xF,
	}
	if m.Opcode != 0 {
		return nil, fmt.Errorf("orbmdns: opcode %d is not QUERY", m.Opcode)
	}
	qd := int(binary.BigEndian.Uint16(b[4:]))
	counts := []int{
		int(binary.BigEndian.Uint16(b[6:])),
		int(binary.BigEndian.Uint16(b[8:])),
		int(binary.BigEndian.Uint16(b[10:])),
	}
	if qd+counts[0]+counts[1]+counts[2] > maxRecords {
		return nil, fmt.Errorf("orbmdns: %d section entries over the %d bound",
			qd+counts[0]+counts[1]+counts[2], maxRecords)
	}
	// The sections begin after the twelve-octet header; a cursor started at 0
	// would read the transaction ID as the first label's length, and on a packet
	// whose id begins with 0xFE that is a "pointer" to nowhere.
	r := &cursor{b: b, off: headerLen, consumed: headerLen}
	for i := 0; i < qd; i++ {
		name, err := r.name()
		if err != nil {
			return nil, err
		}
		qt, err := r.u16()
		if err != nil {
			return nil, err
		}
		qc, err := r.u16()
		if err != nil {
			return nil, err
		}
		m.Questions = append(m.Questions, Question{
			Name: name, Type: qt, Class: qc &^ classQU, QU: qc&classQU != 0,
		})
	}
	sections := []*[]Record{&m.Answers, &m.Authorities, &m.Additionals}
	for s, n := range counts {
		for i := 0; i < n; i++ {
			rec, err := r.record()
			if err != nil {
				return nil, err
			}
			*sections[s] = append(*sections[s], *rec)
		}
	}
	if r.off != len(b) {
		return nil, fmt.Errorf("orbmdns: %d bytes left over: a record's RDLENGTH lied", len(b)-r.off)
	}
	return m, nil
}

// cursor walks one datagram. consumed is the furthest byte read linearly, and it
// is what makes a pointer's target checkable: a pointer into bytes that have not
// been read yet has not committed to what it is claiming.
type cursor struct {
	b        []byte
	off      int
	consumed int
}

func (c *cursor) need(n int) error {
	if n < 0 || c.off+n > len(c.b) {
		return ErrShort
	}
	return nil
}

func (c *cursor) advance(n int) {
	c.off += n
	if c.off > c.consumed {
		c.consumed = c.off
	}
}

func (c *cursor) u16() (uint16, error) {
	if err := c.need(2); err != nil {
		return 0, err
	}
	v := binary.BigEndian.Uint16(c.b[c.off:])
	c.advance(2)
	return v, nil
}

func (c *cursor) u32() (uint32, error) {
	if err := c.need(4); err != nil {
		return 0, err
	}
	v := binary.BigEndian.Uint32(c.b[c.off:])
	c.advance(4)
	return v, nil
}

// name expands a wire name into a lowercased dotted string. Case is folded here,
// once, because DNS names are case-insensitive and a browse that depends on how
// a responder spelled an instance finds nothing half the day.
func (c *cursor) name() (string, error) {
	var labels []string
	off, hops := c.off, 0
	// fieldEnd is where this name's FIELD ends for the next read, which is not where
	// the walk ends once a pointer is followed: the walk jumps back into bytes already
	// read and stops at their zero label, while the field stops two bytes past the
	// pointer. Conflating them reads the middle of an expanded label as the next
	// record's TYPE — and a byte above 0xC0 in a name is a "reserved label form",
	// which is how two Home Assistant query shapes on this LAN came to be refused.
	fieldEnd := -1
	for {
		if off < 0 || off >= len(c.b) {
			return "", ErrShort
		}
		l := c.b[off]
		if l == 0 {
			off++
			break
		}
		if l&0xC0 == 0xC0 {
			if off+2 > len(c.b) {
				return "", ErrShort
			}
			target := int(l&0x3F)<<8 | int(c.b[off+1])
			if target >= c.consumed || hops >= maxNameHops {
				return "", fmt.Errorf("orbmdns: pointer to %d from %d is not walkable", target, off)
			}
			if fieldEnd < 0 {
				fieldEnd = off + 2 // the first pointer is what ends the field
			}
			off, hops = target, hops+1
			continue
		}
		if l&0xC0 != 0 {
			return "", fmt.Errorf("orbmdns: reserved label form 0x%02x", l)
		}
		if off+1+int(l) > len(c.b) {
			return "", ErrShort
		}
		labels = append(labels, strings.ToLower(string(c.b[off+1:off+1+int(l)])))
		off += 1 + int(l)
		if off > c.consumed {
			c.consumed = off
		}
	}
	if fieldEnd < 0 {
		fieldEnd = off // literal labels all the way: the walk ended past the zero label
	}
	if fieldEnd > c.off {
		c.advance(fieldEnd - c.off)
	}
	out := strings.Join(labels, ".")
	if len(out) > maxNameLen {
		return "", fmt.Errorf("orbmdns: name of %d B over the %d bound", len(out), maxNameLen)
	}
	return out, nil
}

// record reads one RR: the owner name, the TYPE/CLASS/TTL/RDLENGTH frame, and the
// rdata the frame's length promises. The rdata is read out of the whole message,
// never out of a slice of itself: a PTR or SRV target inside it may be a
// compression pointer back into the names already walked, and slicing first is how
// that pointer would be read as literal label bytes.
func (c *cursor) record() (*Record, error) {
	name, err := c.name()
	if err != nil {
		return nil, err
	}
	typ, err := c.u16()
	if err != nil {
		return nil, err
	}
	cls, err := c.u16()
	if err != nil {
		return nil, err
	}
	ttl, err := c.u32()
	if err != nil {
		return nil, err
	}
	rdlen, err := c.u16()
	if err != nil {
		return nil, err
	}
	if int(rdlen) > maxRDLen {
		return nil, fmt.Errorf("orbmdns: RDLENGTH %d over the %d bound", rdlen, maxRDLen)
	}
	if err := c.need(int(rdlen)); err != nil {
		return nil, err
	}
	rec := &Record{Name: name, Type: typ, Class: cls &^ cacheBit,
		CacheFlush: cls&cacheBit != 0, TTL: ttl}
	dataOff := c.off
	body := c.b[c.off : c.off+int(rdlen)]
	c.advance(int(rdlen))
	if err := rec.fill(body, dataOff, c.b); err != nil {
		return nil, err
	}
	return rec, nil
}

// fill decodes an rdata by record type. Names are expanded against the whole
// message from the rdata's own offset. TXT is the exception and the point: RFC
// 6762 §6.1 forbids compression inside TXT rdata, so its strings are read as
// literal bytes and a 0xC0 pair there stays two garbage characters in a value
// rather than becoming a pointer that lends this record someone else's name.
func (r *Record) fill(body []byte, off int, whole []byte) error {
	switch r.Type {
	case typePTR:
		n, err := nameAt(whole, off)
		if err != nil {
			return err
		}
		r.PTR = n
	case typeSRV:
		if len(body) < 6 {
			return ErrShort
		}
		r.SRVPort = binary.BigEndian.Uint16(body[4:])
		n, err := nameAt(whole, off+6)
		if err != nil {
			return err
		}
		r.SRVTarget = n
	case typeTXT:
		t, err := decodeTXT(body)
		if err != nil {
			return err
		}
		r.TXT = t
	case typeA:
		if len(body) != net.IPv4len {
			return fmt.Errorf("orbmdns: A rdata of %d B is not an IPv4 address", len(body))
		}
		// The four bytes are kept verbatim, including 0.0.0.0. A placeholder
		// address is well-formed DNS — it is what an iOS device puts in its own A
		// record before DHCP hands it a lease — and this parser refuses the whole
		// datagram when one record fails. Refusing 0.0.0.0 here turned a neighbour's
		// pre-DHCP announce into an unnamed packet refusal on the hub's card, the
		// same trap the empty TXT above already sidestepped. Whether an address may
		// stand for an orb is a judgment about the orb, not the encoding, so it lives
		// one level up in ParseOrbAddress, which still refuses 0.0.0.0, loopback and
		// the group before any of them reaches a row.
		r.A = net.IP(append([]byte(nil), body...))
	default:
		// An unknown kind parses and is kept: a browse of one service type rides
		// alongside whatever else a responder felt like including, and dropping
		// the packet over a record the hub has no use for would lose the orb.
	}
	return nil
}

// nameAt expands a name at an absolute offset without moving a cursor. It is how
// an rdata's embedded name is read against the message it came from.
func nameAt(b []byte, off int) (string, error) {
	c := &cursor{b: b, off: off, consumed: off}
	return c.name()
}

// decodeTXT splits TXT rdata into its length-prefixed strings and then into
// key=value pairs, lowercasing keys.
//
// What it tolerates, and why the tolerance is not a softening: this runs over every
// announcement on the subnet, and Parse refuses the WHOLE datagram when one record
// fails — so a content rule here decides whether a neighbour's traffic lands on the
// hub's card as a defect. A zero-length string (RFC 6762 §6.1's empty TXT, what an
// Apple device sends for _services._dns-sd._udp) and a flag with no "=" (RFC 6763
// §6.1, read as an empty value) are both well-formed DNS, and refusing them refused
// the entire packet around them: an iPad on this LAN produced thirty refusals and no
// orb. What the browser still insists on lives one level up, where it can be stated
// about the orb rather than about the encoding — promote() refuses any candidate
// whose TXT does not carry a mac this instance can have, and that refusal is tested
// in browse_test.go under TestEmptyMacIsRefusedNotListed.
func decodeTXT(body []byte) (map[string]string, error) {
	out := map[string]string{}
	for i := 0; i < len(body); {
		n := int(body[i])
		if i+1+n > len(body) {
			return nil, ErrShort
		}
		if n == 0 {
			i++ // an empty character-string: legal, and it names nothing
			continue
		}
		s := string(body[i+1 : i+1+n])
		k, v, found := strings.Cut(s, "=")
		k = strings.ToLower(strings.TrimSpace(k))
		if !found {
			v = "" // a flag string, per RFC 6763 §6.1
		}
		if k != "" {
			out[k] = v
		}
		i += 1 + n
	}
	return out, nil
}
