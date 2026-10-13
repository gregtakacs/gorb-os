package main

import (
	"encoding/hex"
	"encoding/json"
	"io"
	"net"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// Item 5's discovery surface, pinned to the firmware rather than to anybody's
// reading of it, in the arrangement firmware_contract_test.go uses for the theme
// surface and device_contract_test.go for the device verbs.
//
// Three sources of truth are held against each other, and no two of them are
// allowed to be the only pair checked:
//
//	main/net/orb_mdns.c   what the device says it sends, lifted as source
//	testdata/*.json     what the device was caught sending, read off the air by
//	                    python-zeroconf 0.151.5, a third party's RFC 6762 stack
//	the fake, the browser   what the hub's own two halves produce
//
// The advertiser arrived on this ladder from item 3 for exactly this reason: an
// advertiser written before the responder existed could only be tested against a
// browser that also did not exist, and the two of them would have agreed about
// whatever I wrote.

const (
	orbMdnsSource = "main/net/orb_mdns.c"
	benchCapture  = "testdata/mdns-orb-B-2026-10-12.json"
)

type capDoc struct {
	Provenance map[string]string  `json:"provenance"`
	Pairs      map[string]capPair `json:"pairs"`
}

type capPair struct {
	QName  string `json:"qname"`
	QType  int    `json:"qtype"`
	Query  string `json:"query"`
	Answer string `json:"answer"`
	Src    string `json:"src"`
}

func loadBenchCapture(t *testing.T) capDoc {
	t.Helper()
	b, err := os.ReadFile(filepath.FromSlash(benchCapture))
	if err != nil {
		t.Fatalf("no on-air capture: %v", err)
	}
	var c capDoc
	if err := json.Unmarshal(b, &c); err != nil {
		t.Fatalf("the capture is unreadable: %v", err)
	}
	if len(c.Pairs) != 4 {
		t.Fatalf("the capture holds %d query/answer pairs, want 4", len(c.Pairs))
	}
	for _, k := range []string{"mac", "fw", "ip", "instance"} {
		if c.Provenance[k] == "" {
			t.Fatalf("the capture lost its %s, so it pins nothing", k)
		}
	}
	return c
}

func decodeHex(s string) ([]byte, error) { return hex.DecodeString(strings.TrimSpace(s)) }

// nopSock is what a builder-only test hands the advertiser: nothing is read and
// nothing is written, because these tests judge encoded bytes rather than a
// responder loop.
type nopSock struct{}

func (nopSock) ReadFrom(p []byte) (int, net.Addr, error)  { return 0, nil, io.EOF }
func (nopSock) WriteTo(p []byte, _ net.Addr) (int, error) { return len(p), nil }
func (nopSock) Close() error                              { return nil }

// TestAdvertiserSpeaksTheFirmwaresOwnWords lifts the literals out of the C and
// requires the fake to emit each one. A rename in the firmware fails this loudly
// instead of turning the fake into an oracle for its own assumptions.
func TestAdvertiserSpeaksTheFirmwaresOwnWords(t *testing.T) {
	src := firmwareFile(t, orbMdnsSource)
	for _, frag := range []string{
		`#define SERVICE_NAME   "_orb._tcp.local"`,
		`#define MDNS_TTL         120u`,
		`#define MDNS_CLASS_FLUSH (0x8000u | MDNS_CLASS_IN)`,
		`snprintf(kv, sizeof(kv), "mac=%s", id->mac)`,
		`snprintf(kv, sizeof(kv), "fw=%s", id->fw)`,
		`"state=%s"`,
		`id->provisioning ? "provisioning" : "normal"`,
	} {
		if !strings.Contains(collapse(src), collapse(frag)) {
			t.Errorf("%s no longer says %s, so the fake's copy of that rule is invented, not transcribed",
				orbMdnsSource, frag)
		}
	}
	// The instance is derived from the mac, not maintained beside it: the six %c
	// arguments off mac[6..11] in mdns_update. If that ever changes, the fake's
	// derivation and the browser's ExpectInstance check are both wrong together,
	// and this is the assertion that says so.
	if !strings.Contains(collapse(src), collapse(`snprintf(s_id.instance, sizeof(s_id.instance), "orb-%c%c%c%c%c%c",`)) {
		t.Errorf("%s no longer builds the instance as \"orb-\" plus mac[6..11]: the hub's cross-check is now fiction", orbMdnsSource)
	}

	f := orbfake.New()
	f.SetSTA(true)
	f.SetAddress("44bd8d782618", "192.168.2.34")
	f.SetFirmware("094fa22")
	a := f.Advertise(orbfake.AdvertiseConfig{Conn: nopSock{}, Port: 80})
	txt := strings.Join(txtStringsOf(t, a.AnnounceBytes(false)), " ")
	for _, want := range []string{"mac=44bd8d782618", "fw=094fa22", "state=normal"} {
		if !strings.Contains(txt, want) {
			t.Errorf("the fake's TXT is %q and never carries %q, which the firmware does emit", txt, want)
		}
	}
	// And the other direction, without which the test above only proves the
	// firmware said something: a key dropped from the fake must fail here.
	for _, key := range []string{"mac=", "fw=", "state="} {
		if !strings.Contains(txt, key) {
			t.Errorf("the fake's TXT carries no %s key, so no hub test can reach it", key)
		}
	}
}

func collapse(s string) string { return strings.Join(strings.Fields(s), " ") }

// txtStringsOf pulls the TXT strings out of a packet this package built, with a
// walker written here rather than imported from either side of the pairing. The
// fake's own record walk lives in its package tests and the browser has its parser;
// a third, dumb reader is what lets a claim about the TXT be evidence rather than a
// shared assumption.
func txtStringsOf(t *testing.T, pkt []byte) []string {
	t.Helper()
	if len(pkt) < 12 {
		t.Fatalf("packet is %d B, shorter than a header", len(pkt))
	}
	sections := []int{
		int(pkt[6])<<8 | int(pkt[7]),
		int(pkt[8])<<8 | int(pkt[9]),
		int(pkt[10])<<8 | int(pkt[11]),
	}
	off, consumed := 12, 12
	for _, n := range sections {
		for i := 0; i < n; i++ {
			off, consumed = skipName(pkt, off, consumed)
			if off+10 > len(pkt) {
				t.Fatalf("record %d has no frame", i)
			}
			typ := int(pkt[off])<<8 | int(pkt[off+1])
			rl := int(pkt[off+8])<<8 | int(pkt[off+9])
			off += 10
			if off+rl > len(pkt) {
				t.Fatalf("record %d claims %d B it does not have", i, rl)
			}
			if typ == 16 {
				var out []string
				for j := 0; j < rl; {
					l := int(pkt[off+j])
					if l <= 0 || j+1+l > rl {
						t.Fatalf("TXT is not a series of well-formed counted strings")
					}
					out = append(out, string(pkt[off+j+1:off+j+1+l]))
					j += 1 + l
				}
				return out
			}
			off += rl
			if off > consumed {
				consumed = off
			}
		}
	}
	t.Fatal("no TXT record in the packet")
	return nil
}

// skipName walks past one name in the record section and returns the offset just
// past the FIELD — a pointer ends after its two octets, a labelled name after its
// root — plus the furthest byte read linearly, which is what the next name's
// pointer check is made against. Getting the two apart is the whole reason a
// hand-written walker exists: read them as one and every record after the first
// is looked for in the middle of the previous one's rdata.
func skipName(b []byte, off, consumed int) (int, int) {
	hops, fieldEnd := 0, -1
	for {
		if off < 0 || off >= len(b) {
			return fieldEnd, consumed
		}
		l := b[off]
		if l == 0 {
			off++
			break
		}
		if l&0xC0 == 0xC0 {
			if off+2 > len(b) {
				return fieldEnd, consumed
			}
			if fieldEnd < 0 {
				fieldEnd = off + 2
			}
			target := int(l&0x3F)<<8 | int(b[off+1])
			if target >= consumed || hops > 8 {
				return fieldEnd, consumed
			}
			off, hops = target, hops+1
			continue
		}
		if l&0xC0 != 0 {
			return fieldEnd, consumed
		}
		off += 1 + int(l)
		if off > consumed {
			consumed = off
		}
	}
	if fieldEnd < 0 {
		fieldEnd = off
	}
	if fieldEnd > consumed {
		consumed = fieldEnd
	}
	return fieldEnd, consumed
}

// TestFakeAndBrowserAgreeOnTheFirmwaresBytes is the three-way check: the fake's
// answer is compared against the bytes the orb really sent, and the browser is then
// made to read BOTH sets and return the same row. Any pair of those can agree by
// accident or by shared authorship. All three agreeing on bytes a stranger's stack
// parsed is the evidence the plan asks for.
func TestFakeAndBrowserAgreeOnTheFirmwaresBytes(t *testing.T) {
	cap := loadBenchCapture(t)
	f := orbfake.New()
	f.SetSTA(true)
	f.SetAddress(cap.Provenance["mac"], cap.Provenance["ip"])
	f.SetFirmware(cap.Provenance["fw"])
	a := f.Advertise(orbfake.AdvertiseConfig{Conn: nopSock{}, Port: 80})

	for _, kind := range []string{"PTR", "SRV", "TXT", "A"} {
		pair := cap.Pairs[kind]
		rx, err := decodeHex(pair.Query)
		if err != nil {
			t.Fatalf("%s query: %v", kind, err)
		}
		want, err := decodeHex(pair.Answer)
		if err != nil {
			t.Fatalf("%s answer: %v", kind, err)
		}
		if got := a.AnswerTo(rx, net.ParseIP("192.168.2.48")); string(got) != string(want) {
			t.Errorf("%s: the fake has drifted from what the orb puts on the wire\nfake: %x\n orb: %x", kind, got, want)
		}
	}

	realAns, err := decodeHex(cap.Pairs["PTR"].Answer)
	if err != nil {
		t.Fatal(err)
	}
	fakeAns := a.AnswerTo(mustHex(t, cap.Pairs["PTR"].Query), net.ParseIP("192.168.2.48"))
	offAir, fromFake := browseOne(t, realAns), browseOne(t, fakeAns)
	if offAir == nil || fromFake == nil {
		t.Fatalf("the browser listed nothing: off air %v, from the fake %v", offAir, fromFake)
	}
	for _, f := range []struct{ what, a, b string }{
		{"instance", offAir.Instance, fromFake.Instance},
		{"mac", offAir.MAC, fromFake.MAC},
		{"fw", offAir.FW, fromFake.FW},
		{"state", offAir.State, fromFake.State},
		{"address", offAir.IP, fromFake.IP},
	} {
		if f.a != f.b {
			t.Errorf("the browser reads %s=%q off the air and %q from the fake", f.what, f.a, f.b)
		}
	}
	if offAir.Port != 80 {
		t.Errorf("port = %d off the air: the hub dials what a browse promised", offAir.Port)
	}
	if offAir.MAC != cap.Provenance["mac"] {
		t.Errorf("mac = %q, want the base address esptool prints (%s), which is what Item 6 cross-locks",
			offAir.MAC, cap.Provenance["mac"])
	}
}

func mustHex(t *testing.T, s string) []byte {
	t.Helper()
	b, err := decodeHex(s)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func browseOne(t *testing.T, pkt []byte) *orbmdns.Candidate {
	t.Helper()
	b := orbmdns.NewBrowser(orbmdns.Config{})
	b.Ingest(pkt, &net.UDPAddr{IP: net.ParseIP("192.168.2.34"), Port: 5353})
	if r := b.Refusals(); len(r) != 0 {
		t.Fatalf("the browser refused what the orb really sent: %+v", r)
	}
	got := b.Candidates()
	if len(got) != 1 {
		t.Fatalf("%d rows from one announcement: %+v", len(got), got)
	}
	return &got[0]
}

// TestFirmwarePortIsWhatBothSidesSay closes the last gap between the C and the hub:
// the SRV port the firmware emits and the port the hub's dial defaults to must be
// the same number, or a browse promises an address the proxy cannot use.
func TestFirmwarePortIsWhatBothSidesSay(t *testing.T) {
	firmwareFile(t, orbMdnsSource) // skips when the tree is not reachable
	kc, err := os.ReadFile(filepath.Join("..", "main", "Kconfig"))
	if err != nil {
		t.Skipf("no Kconfig: %v", err)
	}
	m := regexp.MustCompile(`(?s)config ORB_NET_HTTPD_PORT.*?default\s+(\d+)`).FindSubmatch(kc)
	if m == nil {
		t.Fatal("CONFIG_ORB_NET_HTTPD_PORT has no default: the SRV port is unpinned")
	}
	want, err := strconv.Atoi(string(m[1]))
	if err != nil {
		t.Fatalf("the port default is not a number: %v", err)
	}
	if orbnet.DefaultPort != want {
		t.Errorf("the hub dials port %d and the firmware advertises %d: a browse would hand out an address nothing dials",
			orbnet.DefaultPort, want)
	}
}
