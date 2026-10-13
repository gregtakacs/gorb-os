package orbmdns

import (
	"encoding/hex"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// The decoder's loyalty is to bytes off the air, not to anything in this repo.
// testdata/orbB-2026-10-12.json holds four query/answer pairs sent to orb-B's
// :5353 and read back there by python-zeroconf 0.151.5 — a third party's RFC 6762
// implementation, which is the reason these bytes count as evidence and a byte
// string produced by internal/orbfake would not.

type capture struct {
	Provenance map[string]string       `json:"provenance"`
	Pairs      map[string]captureEntry `json:"pairs"`
}

type captureEntry struct {
	QName  string `json:"qname"`
	QType  int    `json:"qtype"`
	Query  string `json:"query"`
	Answer string `json:"answer"`
	Src    string `json:"src"`
}

func loadCapture(t *testing.T) capture {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("..", "..", "testdata", "mdns-orb-B-2026-10-12.json"))
	if err != nil {
		t.Fatalf("no on-air capture to decode: %v", err)
	}
	var c capture
	if err := json.Unmarshal(b, &c); err != nil {
		t.Fatalf("the capture is not readable: %v", err)
	}
	if len(c.Pairs) != 4 {
		t.Fatalf("the capture holds %d pairs, want the four a browse needs", len(c.Pairs))
	}
	return c
}

func unhex(t *testing.T, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatalf("bad hex in the capture: %v", err)
	}
	return b
}

// TestRealAnswerBytesParseIntoTheOrbTheyName is the pin that keeps this package
// honest: parse the shipped firmware's own answer and require every field the hub
// will display or key on. An instance name, a mac, a fw stamp, a state word, an
// address and a port — all six from bytes a stranger's stack already agreed were
// well-formed.
func TestRealAnswerBytesParseIntoTheOrbTheyName(t *testing.T) {
	c := loadCapture(t)
	now := time.Unix(1760000000, 0)
	b := NewBrowser(Config{Now: func() time.Time { return now }})

	for _, kind := range []string{"PTR", "SRV", "TXT", "A"} {
		pair, ok := c.Pairs[kind]
		if !ok {
			t.Fatalf("the capture lost its %s pair", kind)
		}
		q := unhex(t, pair.Query)
		qm, err := Parse(q)
		if err != nil {
			t.Fatalf("%s query does not parse: %v", kind, err)
		}
		if len(qm.Questions) != 1 || qm.Questions[0].Name != pair.QName {
			t.Errorf("%s query asked %v, want %s", kind, qm.Questions, pair.QName)
		}
		if !qm.Questions[0].QU {
			t.Errorf("%s query carries no unicast-request bit, so the orb would answer to the group", kind)
		}
		a := unhex(t, pair.Answer)
		m, err := Parse(a)
		if err != nil {
			t.Fatalf("%s answer does not parse: %v", kind, err)
		}
		if !m.Response {
			t.Errorf("%s answer has QR clear: it is a question", kind)
		}
		if m.ID != qm.ID {
			t.Errorf("%s answer id %04x does not echo the query id %04x", kind, m.ID, qm.ID)
		}
		// The firmware counts its additional records in NSCOUNT, so a walk that
		// trusts section names alone finds nothing here. Records() is what makes
		// the four arrive whole.
		if len(m.Records()) == 0 {
			t.Errorf("%s answer holds no records at all", kind)
		}
		b.Ingest(a, &net.UDPAddr{IP: net.ParseIP(pair.Src), Port: 5353})
	}

	got := b.Candidates()
	if len(got) != 1 {
		t.Fatalf("the real announcement produced %d candidates, want 1: %+v", len(got), got)
	}
	cand := got[0]
	if cand.Instance != c.Provenance["instance"] {
		t.Errorf("instance = %q, want %q off the air", cand.Instance, c.Provenance["instance"])
	}
	if cand.MAC != c.Provenance["mac"] {
		t.Errorf("mac = %q, want %q off the air", cand.MAC, c.Provenance["mac"])
	}
	if cand.FW != c.Provenance["fw"] {
		t.Errorf("fw = %q, want %q off the air", cand.FW, c.Provenance["fw"])
	}
	if cand.State != "normal" {
		t.Errorf("state = %q, want normal on a joined orb", cand.State)
	}
	if cand.IP != c.Provenance["ip"] {
		t.Errorf("address = %q, want %q off the air", cand.IP, c.Provenance["ip"])
	}
	if cand.Port != 80 {
		t.Errorf("port = %d, want the 80 the device's httpd listens on", cand.Port)
	}
	if cand.Provisioning() {
		t.Error("a joined orb read as provisioning")
	}
	if len(b.Refusals()) != 0 {
		t.Errorf("the real orb was refused: %+v", b.Refusals())
	}
}

// --- the neighbours that are not orbs -----------------------------------------------
//
// The browser shares a subnet with stacks that never heard of this project: Home
// Assistant probes names with a pointer at the end of the question's own name, a
// Chromecast announces reverse-DNS records, a printer answers AAAA. Two of those
// shapes were refused here until the name walker kept its field end separate from its
// walk — refused loudly, into the card an operator reads when an orb is missing. These
// bytes came off the bench LAN rather than out of this repo's encoder, which is the
// only reason they can prove the point.

type neighbourCapture struct {
	Provenance map[string]string          `json:"provenance"`
	Packets    map[string]neighbourPacket `json:"packets"`
}

type neighbourPacket struct {
	Src string `json:"src"`
	Hex string `json:"hex"`
}

func loadNeighbours(t *testing.T) neighbourCapture {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("..", "..", "testdata", "mdns-neighbours-2026-10-12.json"))
	if err != nil {
		t.Fatalf("no neighbour capture to decode: %v", err)
	}
	var c neighbourCapture
	if err := json.Unmarshal(b, &c); err != nil {
		t.Fatalf("the neighbour capture is not readable: %v", err)
	}
	if len(c.Packets) < 6 {
		t.Fatalf("%d neighbour packets is too thin a sample of a real LAN to prove anything", len(c.Packets))
	}
	if c.Provenance["captured"] == "" || c.Provenance["who"] == "" {
		t.Fatal("the neighbour capture lost its provenance, so it pins nothing")
	}
	return c
}

func TestNeighbourStackTrafficIsParsedNotRefused(t *testing.T) {
	c := loadNeighbours(t)
	b := NewBrowser(Config{Now: func() time.Time { return time.Unix(1760000000, 0) }})
	for name, p := range c.Packets {
		pkt, err := hex.DecodeString(p.Hex)
		if err != nil {
			t.Fatalf("%s: the capture is corrupt: %v", name, err)
		}
		if _, err := Parse(pkt); err != nil {
			t.Errorf("%s from %s: a well-formed neighbour packet was refused: %v", name, p.Src, err)
		}
		b.Ingest(pkt, &net.UDPAddr{IP: net.ParseIP(p.Src), Port: Port})
	}
	if got := b.Candidates(); len(got) != 0 {
		t.Errorf("a neighbour became an orb row: %+v", got)
	}
	if r := b.Refusals(); len(r) != 0 {
		t.Errorf("%d refusals on a LAN of well-formed neighbours; the card would blame the orb:\n  %v",
			len(r), r)
	}
}

// The defect itself, without the capture file: a question whose name is literal
// labels followed by a pointer. The walk ends at the target's zero label; the FIELD
// ends two bytes past the pointer. Confusing them leaves the cursor inside the
// expanded name, and the next read takes a byte of ASCII as a label length.
func TestPointerAtTheEndOfANameEndsTheFieldNotTheWalk(t *testing.T) {
	// One question for the service, then one whose name is
	// "home._home-assistant._tcp" + a pointer to the "_tcp.local" already written.
	atTCP := 12 + 1 + 8 /*instance*/ + 1 + 4 /*_tcp*/
	b := []byte{0x00, 0x11, 0x84, 0x00, 0, 2, 0, 0, 0, 0, 0, 0}
	b = append(b, 8)
	b = append(b, []byte("instance")...)
	b = append(b, 4)
	b = append(b, []byte("_tcp")...)
	b = append(b, 5)
	b = append(b, []byte("local")...)
	b = append(b, 0)
	b = append(b, 0, 12, 0, 1) // PTR, class IN
	b = append(b, 4)
	b = append(b, []byte("home")...)
	b = append(b, 15)
	b = append(b, []byte("_home-assistant")...)
	b = append(b, 4)
	b = append(b, []byte("_tcp")...)
	b = append(b, byte(0xC0|(atTCP>>8)), byte(atTCP&0xFF))
	b = append(b, 0, 33, 0, 1) // SRV, class IN
	m, err := Parse(b)
	if err != nil {
		t.Fatalf("a legal question with a trailing pointer was refused: %v", err)
	}
	if len(m.Questions) != 2 {
		t.Fatalf("%d questions parsed, want 2", len(m.Questions))
	}
	if m.Questions[1].Name != "home._home-assistant._tcp.local" {
		t.Errorf("question 2 expanded to %q", m.Questions[1].Name)
	}
	if m.Questions[1].Type != 33 {
		t.Errorf("question 2 read TYPE %d, want 33 — the cursor stopped inside the name", m.Questions[1].Type)
	}
}
