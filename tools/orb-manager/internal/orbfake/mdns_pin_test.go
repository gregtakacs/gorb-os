package orbfake

import (
	"encoding/hex"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The advertiser's loyalty is to the firmware, not to the hub's browser. Three
// checks, each from a different direction, because a fake that only ever agrees
// with the code that tests it is the failure this package exists to avoid:
//
//  1. the bytes. An answer captured off the air from orb-B — read there by
//     python-zeroconf, a stranger's stack — is reproduced byte for byte by the fake
//     given the same identity. Neither this repo's encoder nor this repo's decoder
//     gets a vote.
//  2. the firmware's own words, lifted out of main/net/orb_mdns.c by
//     mdns_contract_test.go and required to appear in what the fake emits.
//  3. the shape a browser judges: record order, the cache-flush bit on the three
//     unique records and off the shared PTR, and a record walk that lands exactly
//     on the end of the message. An RDLENGTH that lies is invisible to everything
//     except a walker that does not trust it.

const capturePath = "../../testdata/mdns-orb-B-2026-10-12.json"

type mdnsCapture struct {
	Provenance map[string]string           `json:"provenance"`
	Pairs      map[string]mdnsCaptureEntry `json:"pairs"`
}

type mdnsCaptureEntry struct {
	QName  string `json:"qname"`
	QType  int    `json:"qtype"`
	Query  string `json:"query"`
	Answer string `json:"answer"`
	Src    string `json:"src"`
}

func loadCapture(t *testing.T) mdnsCapture {
	t.Helper()
	b, err := os.ReadFile(filepath.FromSlash(capturePath))
	if err != nil {
		t.Fatalf("no on-air capture to compare against: %v", err)
	}
	var c mdnsCapture
	if err := json.Unmarshal(b, &c); err != nil {
		t.Fatalf("the capture is unreadable: %v", err)
	}
	if len(c.Pairs) != 4 {
		t.Fatalf("the capture holds %d pairs; it is not the four a browse needs", len(c.Pairs))
	}
	for _, k := range []string{"PTR", "SRV", "TXT", "A"} {
		if _, ok := c.Pairs[k]; !ok {
			t.Fatalf("the capture lost its %s pair", k)
		}
	}
	for _, k := range []string{"mac", "fw", "ip", "instance"} {
		if c.Provenance[k] == "" {
			t.Fatalf("the capture lost the %s it was taken from, so it pins nothing", k)
		}
	}
	return c
}

func mustHex(t *testing.T, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(strings.TrimSpace(s))
	if err != nil {
		t.Fatalf("bad hex in the capture: %v", err)
	}
	return b
}

// orbBAsTheFake builds the double with orb-B's own identity, so the bytes it emits
// are directly comparable with the bytes orb-B emitted. The identity arrives through
// the same setters a hub test uses: there is no advertiser-only back door for it,
// which is what keeps the announcement and /api/v1/status reading one field.
func orbBAsTheFake(t *testing.T, c mdnsCapture) (*Orb, *Advertiser) {
	t.Helper()
	f := New()
	f.SetSTA(true)
	f.SetAddress(c.Provenance["mac"], c.Provenance["ip"])
	f.SetFirmware(c.Provenance["fw"])
	return f, f.Advertise(AdvertiseConfig{Conn: &loopbackConn{}, Port: 80})
}

// TestFakeAnswerIsTheOrbsOwnBytes is the pin. Feed the fake the query that was
// really sent to orb-B and require the answer it builds to be the answer orb-B
// really sent: same transaction id, same echoed question, same four records, same
// bytes — including the header quirk that counts the additional records as
// authority, which is the shape the hub's browser was built to read.
func TestFakeAnswerIsTheOrbsOwnBytes(t *testing.T) {
	c := loadCapture(t)
	_, a := orbBAsTheFake(t, c)
	for _, kind := range []string{"PTR", "SRV", "TXT", "A"} {
		pair := c.Pairs[kind]
		rx := mustHex(t, pair.Query)
		want := mustHex(t, pair.Answer)
		got := a.AnswerTo(rx, net.ParseIP("192.168.2.48"))
		if got == nil {
			t.Fatalf("%s: the fake would not answer a query the real orb answered", kind)
		}
		if !equalBytes(got, want) {
			t.Errorf("%s: the fake's answer is %d B, the orb's was %d B\n got: %s\nwant: %s",
				kind, len(got), len(want), hex.EncodeToString(got), hex.EncodeToString(want))
			for i := 0; i < len(got) && i < len(want); i++ {
				if got[i] != want[i] {
					t.Errorf("%s: first difference at byte %d: got %02x want %02x", kind, i, got[i], want[i])
					break
				}
			}
		}
	}
}

// TestFakeAnnounceIsTheOrbsOwnRecords: an unsolicited announcement has no question
// to echo, so it is pinned record by record against what the orb really sent rather
// than byte for byte. Same four records in the same order, the same TTL, the
// cache-flush bit on the three names unique to this chip and off the shared PTR,
// and the TXT keys the firmware formats.
func TestFakeAnnounceIsTheOrbsOwnRecords(t *testing.T) {
	c := loadCapture(t)
	_, a := orbBAsTheFake(t, c)
	id, sta, _ := a.o.advertiseState()
	pkt := AnnouncePacket(id, sta, 80, false)
	if pkt == nil {
		t.Fatal("the announcement would not encode")
	}
	if pkt[2]&0x80 == 0 {
		t.Error("an unsolicited announcement arrived without QR set: a browser reads it as a question")
	}
	if got := int(pkt[6])<<8 | int(pkt[7]); got != 1 {
		t.Errorf("announcement ANCOUNT = %d, want the one PTR the firmware answers with", got)
	}
	// The three additional records are counted at offset 8, the DNS authority
	// field, with ARCOUNT left zero — exactly what the shipped responder does and
	// what the capture shows on the wire. Correcting it here would be the double
	// quietly disagreeing with the device it stands in for.
	if got := int(pkt[8])<<8 | int(pkt[9]); got != 3 {
		t.Errorf("record count at offset 8 = %d, want 3", got)
	}
	if got := int(pkt[10])<<8 | int(pkt[11]); got != 0 {
		t.Errorf("ARCOUNT = %d, want the zero the firmware leaves there", got)
	}
	recs, err := walkRecords(pkt)
	if err != nil {
		t.Fatalf("the announcement does not walk: %v", err)
	}
	if len(recs) != 4 {
		t.Fatalf("the announcement carries %d records, want PTR, SRV, TXT, A", len(recs))
	}
	want := []int{12, 33, 16, 1}
	for i, r := range recs {
		if r.typ != want[i] {
			t.Errorf("record %d is type %d, want %d: the order is what lets each point at the last", i, r.typ, want[i])
		}
		if r.ttl != mdnsTTL {
			t.Errorf("record %d has ttl %d, want %d", i, r.ttl, mdnsTTL)
		}
		if r.flush != (i != 0) {
			t.Errorf("record %d (type %d) cache-flush = %v, want %v: the PTR is shared by every orb on the LAN", i, r.typ, r.flush, i != 0)
		}
	}
	inst := c.Provenance["instance"]
	if recs[0].target != inst+"._orb._tcp.local" {
		t.Errorf("the PTR points at %q, not at the instance the mac implies", recs[0].target)
	}
	if recs[1].target != inst+".local" || recs[1].port != 80 {
		t.Errorf("SRV is %q:%d, want %s.local:80 — the hub dials what a browse promised",
			recs[1].target, recs[1].port, inst)
	}
	for k, wantKV := range map[string]string{
		"mac":   "mac=" + c.Provenance["mac"],
		"fw":    "fw=" + c.Provenance["fw"],
		"state": "state=normal",
	} {
		if recs[2].txt[k] != strings.TrimPrefix(wantKV, k+"=") {
			t.Errorf("TXT %s = %q, want %q as the firmware spells it", k, recs[2].txt[k], wantKV)
		}
	}
	if len(recs[2].txt) != 3 {
		t.Errorf("TXT holds %d keys, want exactly mac, fw and state: %v", len(recs[2].txt), recs[2].txt)
	}
	if recs[3].addr != c.Provenance["ip"] {
		t.Errorf("A record = %s, want the lease the device self-reports (%s)", recs[3].addr, c.Provenance["ip"])
	}

	// The goodbye: TTL 0 everywhere, so a hub drops the row now rather than
	// offering a board whose socket is gone for the rest of the expiry.
	gb := AnnouncePacket(id, sta, 80, true)
	grecs, err := walkRecords(gb)
	if err != nil {
		t.Fatalf("the goodbye does not walk: %v", err)
	}
	for i, r := range grecs {
		if r.ttl != 0 {
			t.Errorf("goodbye record %d has ttl %d, want 0", i, r.ttl)
		}
	}
}

// TestFakeAnswersOnlyItsOwnNames is the silence half of the contract. A responder
// that answered a question about someone else is worse than one that said nothing:
// it poisons a browser's cache with a name that is not its business.
func TestFakeAnswersOnlyItsOwnNames(t *testing.T) {
	c := loadCapture(t)
	_, a := orbBAsTheFake(t, c)
	foreign := []struct {
		why   string
		name  string
		qtype int
	}{
		{"another service", "_http._tcp.local", 12},
		{"another orb's instance", "orb-000000._orb._tcp.local", 33},
		{"another orb's instance, TXT", "orb-000000._orb._tcp.local", 16},
		{"another host", "printer.local", 1},
	}
	for _, f := range foreign {
		q := buildQuery(0xC1, f.name, f.qtype)
		if got := a.AnswerTo(q, net.ParseIP("192.168.2.48")); got != nil {
			t.Errorf("the fake answered a question about %s (%d B): it should stay silent", f.why, len(got))
		}
	}
	// Responses, not queries, are not ours to answer either — and a responder that
	// answered one would echo every orb's announcement back at itself forever.
	ptr := c.Pairs["PTR"]
	if got := a.AnswerTo(mustHex(t, ptr.Answer), net.ParseIP("192.168.2.48")); got != nil {
		t.Error("the fake answered a response")
	}
	// A truncated query must be refused, not crashed on.
	q := buildQuery(0xC2, mdnsServiceName, 12)
	for n := 1; n < len(q); n++ {
		if got := a.AnswerTo(q[:n], net.ParseIP("192.168.2.48")); got != nil {
			t.Fatalf("a %d-byte truncated query was answered: the bounds check is missing", n)
		}
	}
	// A pointer that aims forwards, or at itself, must be refused rather than
	// walked: the first is a packet that has not committed to what it claims and
	// the second never ends.
}

// buildQuery is the fake's own query encoder, for tests: internal/orbmdns builds
// queries for the hub and the fake must not borrow them to prove it can read them.
func buildQuery(txid byte, name string, qtype int) []byte {
	b := []byte{byte(txid), 0x00, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0}
	for _, lab := range strings.Split(name, ".") {
		if lab == "" {
			continue
		}
		b = append(b, byte(len(lab)))
		b = append(b, lab...)
	}
	b = append(b, 0, 0, byte(qtype), 0x80, 0x01)
	return b
}

func equalBytes(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}
