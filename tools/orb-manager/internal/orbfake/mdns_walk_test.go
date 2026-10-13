package orbfake

import (
	"errors"
	"fmt"
	"net"
	"strings"
	"sync"
	"testing"
)

// walkRecord is one record read back out of a packet the fake built, by a walker
// written against RFC 6762 rather than against this package's encoder. Sharing the
// encoder's own bookkeeping would make an RDLENGTH that lies impossible to see: the
// writer would believe its own number and so would the test.
type walkRecord struct {
	owner  string
	typ    int
	class  int
	flush  bool
	ttl    uint32
	target string
	port   int
	addr   string
	txt    map[string]string
}

func walkRecords(b []byte) ([]walkRecord, error) {
	if len(b) < 12 {
		return nil, fmt.Errorf("shorter than a header")
	}
	qd := int(b[4])<<8 | int(b[5])
	an := int(b[6])<<8 | int(b[7])
	ns := int(b[8])<<8 | int(b[9])
	ar := int(b[10])<<8 | int(b[11])
	off := 12
	consumed := 12
	for i := 0; i < qd; i++ {
		var err error
		_, off, consumed, err = walkName(b, off, consumed)
		if err != nil {
			return nil, err
		}
		if off+4 > len(b) {
			return nil, fmt.Errorf("question %d has no type/class", i)
		}
		off += 4
		consumed = off
	}
	var out []walkRecord
	for _, n := range []int{an, ns, ar} {
		for i := 0; i < n; i++ {
			owner, next, cons, err := walkName(b, off, consumed)
			if err != nil {
				return nil, err
			}
			off, consumed = next, cons
			if off+10 > len(b) {
				return nil, fmt.Errorf("record %d has no frame", i)
			}
			typ := int(b[off])<<8 | int(b[off+1])
			cls := int(b[off+2])<<8 | int(b[off+3])
			ttl := uint32(b[off+4])<<24 | uint32(b[off+5])<<16 | uint32(b[off+6])<<8 | uint32(b[off+7])
			rl := int(b[off+8])<<8 | int(b[off+9])
			off += 10
			if off+rl > len(b) {
				return nil, fmt.Errorf("record %d claims %d B of rdata it does not have", i, rl)
			}
			r := walkRecord{owner: owner, typ: typ, class: cls & 0x7FFF, flush: cls&0x8000 != 0, ttl: ttl}
			data := b[off : off+rl]
			switch typ {
			case 12: // PTR
				tgt, _, _, err := walkName(b, off, off)
				if err != nil {
					return nil, fmt.Errorf("PTR %d: %w", i, err)
				}
				r.target = tgt
			case 33: // SRV
				if rl < 6 {
					return nil, fmt.Errorf("SRV %d is %d B, shorter than priority/weight/port", i, rl)
				}
				r.port = int(data[4])<<8 | int(data[5])
				tgt, _, _, err := walkName(b, off+6, off+6)
				if err != nil {
					return nil, fmt.Errorf("SRV %d: %w", i, err)
				}
				r.target = tgt
			case 16: // TXT — never expanded, never compressed
				r.txt = map[string]string{}
				for j := 0; j < len(data); {
					n := int(data[j])
					if j+1+n > len(data) {
						return nil, fmt.Errorf("TXT %d: a string runs past the rdata", i)
					}
					s := string(data[j+1 : j+1+n])
					k, v, _ := strings.Cut(s, "=")
					r.txt[k] = v
					j += 1 + n
				}
			case 1: // A
				if rl != 4 {
					return nil, fmt.Errorf("A %d is %d B, not an IPv4 address", i, rl)
				}
				r.addr = net.IP(data).String()
			}
			out = append(out, r)
			off += rl
			if off > consumed {
				consumed = off
			}
		}
	}
	if off != len(b) {
		return nil, fmt.Errorf("%d bytes left after the last record: an RDLENGTH lied", len(b)-off)
	}
	return out, nil
}

// walkName expands a name, allowing only pointers that aim backwards at bytes the
// walk has already read, and returning the offset past the field in the linear
// stream. Pointers inside an rdata are legal for PTR and SRV targets, so this is
// called with the walk's own consumed mark.
func walkName(b []byte, off, consumed int) (string, int, int, error) {
	var labels []string
	hops, fieldEnd := 0, -1
	for {
		if off < 0 || off >= len(b) {
			return "", 0, consumed, fmt.Errorf("name runs past the datagram")
		}
		l := b[off]
		if l == 0 {
			off++
			break
		}
		if l&0xC0 == 0xC0 {
			if off+2 > len(b) {
				return "", 0, consumed, fmt.Errorf("pointer is cut off")
			}
			target := int(l&0x3F)<<8 | int(b[off+1])
			if target >= consumed || hops >= 8 {
				return "", 0, consumed, fmt.Errorf("pointer to %d is not walkable", target)
			}
			if fieldEnd < 0 {
				fieldEnd = off + 2
			}
			off, hops = target, hops+1
			continue
		}
		if l&0xC0 != 0 {
			return "", 0, consumed, fmt.Errorf("reserved label form 0x%02x", l)
		}
		if off+1+int(l) > len(b) {
			return "", 0, consumed, fmt.Errorf("label runs past the datagram")
		}
		labels = append(labels, string(b[off+1:off+1+int(l)]))
		off += 1 + int(l)
		if off > consumed {
			consumed = off
		}
	}
	if fieldEnd < 0 {
		fieldEnd = off
	}
	return strings.ToLower(strings.Join(labels, ".")), fieldEnd, consumed, nil
}

// loopbackConn is the socket a unit test hands the advertiser: it never touches a
// real interface, so the suite passes on a machine with no LAN and no permission to
// bind 5353, and every packet the fake sends is captured for the test to read.
type loopbackConn struct {
	mu     sync.Mutex
	writes [][]byte
	to     []net.Addr
	in     [][]byte
	closed bool
}

var errNoPacket = errors.New("orbfake/test: no packet queued")

func (c *loopbackConn) ReadFrom(p []byte) (int, net.Addr, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return 0, nil, errNoPacket
	}
	if len(c.in) == 0 {
		return 0, nil, errNoPacket
	}
	n := copy(p, c.in[0])
	c.in = c.in[1:]
	return n, &net.UDPAddr{IP: net.ParseIP("127.0.0.1"), Port: 5353}, nil
}

func (c *loopbackConn) WriteTo(p []byte, addr net.Addr) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.writes = append(c.writes, append([]byte(nil), p...))
	c.to = append(c.to, addr)
	return len(p), nil
}

func (c *loopbackConn) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.closed = true
	return nil
}

func (c *loopbackConn) LocalAddr() net.Addr {
	return &net.UDPAddr{IP: net.ParseIP("127.0.0.1"), Port: 5353}
}

// sent snapshots what the fake put on the wire, for a test to assert on.
func (c *loopbackConn) sent() [][]byte {
	c.mu.Lock()
	defer c.mu.Unlock()
	return append([][]byte(nil), c.writes...)
}

// queue loads a query in for the responder loop to read.
func (c *loopbackConn) queue(pkt []byte, from string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.in = append(c.in, append([]byte(nil), pkt...))
	c.to = append(c.to, &net.UDPAddr{IP: net.ParseIP(from), Port: 5353})
}

// --- the SoftAP-only announcement ---------------------------------------------------
//
// A factory-reset orb has no lease to announce: the only address it owns is the one its
// provisioning AP answers on, 192.168.4.1, and the only reason a hub can tell that orb
// from a joined one is the state= word in the TXT. No wired hub can route to that
// address, so the row the hub builds from these bytes has to carry both facts, and the
// bytes themselves have to be checked by a walker that does not trust the encoder —
// which is what this file's walkRecords is for.

func TestAnnounceNamesTheApAddressAndTheProvisioningState(t *testing.T) {
	f := New()
	f.SetAddress("44bd8d782618", "192.168.4.1")
	f.SetFirmware("094fa22")
	// The STA down and the AP up is the whole point: an announcement that leaked the
	// last lease it held would send a wired hub to an address that stopped existing.
	f.SetSTA(false)
	f.SetSoftAP(true, "192.168.4.1")
	f.SetProvisioning(true)

	a := f.Advertise(AdvertiseConfig{Conn: &loopbackConn{}, Port: 80})
	pkt := a.AnnounceBytes(false)
	if pkt == nil {
		t.Fatal("the SoftAP announcement would not encode")
	}
	recs, err := walkRecords(pkt)
	if err != nil {
		t.Fatalf("the SoftAP announcement does not walk: %v", err)
	}
	var txt, addr string
	srvPort := -1
	for _, r := range recs {
		switch r.typ {
		case 16:
			txt = r.txt["state"]
			if r.txt["mac"] != "44bd8d782618" {
				t.Errorf("TXT mac=%q, want the address the orb was given", r.txt["mac"])
			}
		case 1:
			addr = r.addr
		case 33:
			srvPort = r.port
		}
	}
	if txt != "provisioning" {
		t.Errorf("TXT state=%q; a hub cannot tell a factory-reset orb from a joined one without it", txt)
	}
	if addr != "192.168.4.1" {
		t.Errorf("A record %q, want the AP address: a joined orb's lease is what this orb does not have", addr)
	}
	if srvPort != 80 {
		t.Errorf("SRV port %d, want 80 — the provisioning page is on the same httpd", srvPort)
	}
}

// The AP address must not be reachable through the STA field of the same fake: if both
// were up, the sameSubset test in AnswerTo is what picks which address a querier is
// handed, and a provisioning orb that answered a LAN querier with 192.168.4.1 would be
// handing out an address that querier cannot route to.
func TestAnswerToPicksTheAddressOfTheInterfaceTheQuerierUsed(t *testing.T) {
	f := New()
	f.SetAddress("44bd8d782618", "192.168.2.34")
	f.SetFirmware("094fa22")
	f.SetSTA(true)
	f.SetSoftAP(true, "192.168.4.1")
	a := f.Advertise(AdvertiseConfig{Conn: &loopbackConn{}, Port: 80})

	rx := buildQuery(0xC3, mdnsServiceName, 12)

	for _, tc := range []struct {
		from, want string
	}{
		{"192.168.2.48", "192.168.2.34"},
		{"192.168.4.35", "192.168.4.1"},
	} {
		tx := a.AnswerTo(rx, net.ParseIP(tc.from))
		if tx == nil {
			t.Fatalf("from %s: no answer", tc.from)
		}
		recs, err := walkRecords(tx)
		if err != nil {
			t.Fatalf("from %s: the answer does not walk: %v", tc.from, err)
		}
		var addr string
		for _, r := range recs {
			if r.typ == 1 {
				addr = r.addr
			}
		}
		if addr != tc.want {
			t.Errorf("querier on %s was handed A=%s, want %s", tc.from, addr, tc.want)
		}
	}
}
