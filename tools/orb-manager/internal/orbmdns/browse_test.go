package orbmdns

import (
	"bytes"
	"net"
	"strings"
	"testing"
	"time"
)

// The rules a browse is judged on. Each one is a failure mode §5.1 of the plan
// named: a ghost that never leaves the list, a row keyed on an address, an orb that
// cannot name its chip listed as if it could, and a listing produced by dialing
// somebody.

// --- a test's own packet writer -----------------------------------------------------
//
// The bytes here are hand-built against RFC 6762, not taken from internal/orbfake.
// The fake is a stand-in for the device and must not also be the source of the
// packets the browser is judged against, or the two would only ever agree with each
// other — the exact arrangement that moved the advertiser off ladder item 3.

type tW struct{ b []byte }

func (w *tW) u16(v uint16) { w.b = append(w.b, byte(v>>8), byte(v)) }
func (w *tW) u32(v uint32) { w.b = append(w.b, byte(v>>24), byte(v>>16), byte(v>>8), byte(v)) }
func (w *tW) raw(s string) { w.b = append(w.b, s...) }
func (w *tW) name(s string) {
	for _, l := range strings.Split(s, ".") {
		if l == "" {
			continue
		}
		w.b = append(w.b, byte(len(l)))
		w.raw(l)
	}
	w.b = append(w.b, 0)
}
func (w *tW) ptr(at int) { w.u16(uint16(0xC000 | at)) }

// frame writes TYPE/CLASS/TTL/RDLENGTH after an owner name and hands back the
// RDLENGTH index for the caller to patch once its rdata exists.
func (w *tW) frame(typ uint16, flush bool, ttl uint32) int {
	w.u16(typ)
	c := uint16(classIN)
	if flush {
		c |= cacheBit
	}
	w.u16(c)
	w.u32(ttl)
	rl := len(w.b)
	w.u16(0)
	return rl
}

func (w *tW) patch(rl int) {
	rd := len(w.b) - rl - 2
	w.b[rl] = byte(rd >> 8)
	w.b[rl+1] = byte(rd)
}

// answerBytes builds the four records one orb owns: PTR under the shared service
// name, then SRV, TXT and A as the records unique to this chip, counted where the
// shipped responder counts them — offset 8, the field a textbook DNS header calls
// NSCOUNT. The hub reads all three record sections as one pool because of that, and
// this builder reproduces it rather than the textbook shape: a test packet the
// device never sends would let the browser pass on a wire it never sees.
func answerBytes(txid uint16, instance, mac, fw, state, ip string, ttl uint32, port uint16) []byte {
	return answerBytesFor(ServiceName, txid, instance, mac, fw, state, ip, ttl, port)
}

// answerBytesFor is the same builder with the service name chosen, so a test can
// hand the browser a well-formed announcement about somebody else's service —
// "_orb._udp" rather than "_orb._tcp" — instead of string-splicing wire bytes whose
// labels are length-prefixed and so cannot be rewritten from the outside.
func answerBytesFor(svc string, txid uint16, instance, mac, fw, state, ip string, ttl uint32, port uint16) []byte {
	w := &tW{}
	w.u16(txid)
	w.u16(0x8400) // QR|AA
	w.u16(0)      // QDCOUNT: an announcement asks nothing
	w.u16(1)      // ANCOUNT: the one PTR
	w.u16(3)      // SRV, TXT and A
	w.u16(0)

	atService := len(w.b)
	_ = atService
	w.name(svc)
	rl := w.frame(typePTR, false, ttl)
	atInstance := len(w.b)
	w.name(instance + "." + svc)
	w.patch(rl)

	w.ptr(atInstance)
	rl = w.frame(typeSRV, true, ttl) // owner is the instance under this service
	w.u16(0)                         // priority
	w.u16(0)                         // weight
	w.u16(port)
	atHost := len(w.b)
	w.name(instance + ".local")
	w.patch(rl)

	w.ptr(atInstance)
	rl = w.frame(typeTXT, true, ttl)
	for _, kv := range []string{"mac=" + mac, "fw=" + fw, "state=" + state} {
		w.b = append(w.b, byte(len(kv)))
		w.raw(kv)
	}
	w.patch(rl)

	w.ptr(atHost)
	rl = w.frame(typeA, true, ttl)
	v4 := net.ParseIP(ip).To4()
	w.b = append(w.b, v4[0], v4[1], v4[2], v4[3])
	w.patch(rl)
	return w.b
}

func hub(t *testing.T) (*Browser, *time.Time) {
	t.Helper()
	now := time.Unix(1760000000, 0)
	return NewBrowser(Config{Now: func() time.Time { return now }}), &now
}

const (
	goodMAC   = "44bd8d782618"
	goodInst  = "orb-782618"
	goodFW    = "094fa22"
	goodIP    = "192.168.2.34"
	otherIP   = "192.168.2.99"
	otherMAC  = "44bd8d782619"
	otherInst = "orb-782619"
)

func from(ip string) net.Addr { return &net.UDPAddr{IP: net.ParseIP(ip), Port: Port} }

func TestOneOrbAnnouncesAsOneRow(t *testing.T) {
	b, _ := hub(t)
	b.Ingest(answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80), from(goodIP))
	got := b.Candidates()
	if len(got) != 1 {
		t.Fatalf("%d candidates, want 1: %+v", len(got), got)
	}
	c := got[0]
	if c.MAC != goodMAC || c.Instance != goodInst || c.FW != goodFW || c.IP != goodIP || c.Port != 80 {
		t.Errorf("the row is wrong: %+v", c)
	}
	if c.Provisioning() {
		t.Error("state=normal read as provisioning")
	}
	if len(b.Refusals()) != 0 {
		t.Errorf("a clean announcement was refused: %+v", b.Refusals())
	}
}

// A renewed lease is one orb that moved, not two orbs. If the row were keyed on the
// address, an operator would manage the old one and the fleet would hold the same
// board twice — which is why the plan refuses to key on IP.
func TestSameMacNewAddressIsStillOneRow(t *testing.T) {
	b, clk := hub(t)
	b.Ingest(answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80), from(goodIP))
	*clk = clk.Add(5 * time.Second)
	b.Ingest(answerBytes(2, goodInst, goodMAC, goodFW, "normal", otherIP, 120, 80), from(otherIP))
	got := b.Candidates()
	if len(got) != 1 {
		t.Fatalf("%d rows for one MAC, want 1: %+v", len(got), got)
	}
	if got[0].IP != otherIP {
		t.Errorf("the row kept %s, the address it first heard, instead of the current %s", got[0].IP, otherIP)
	}
}

// The converse: one address claimed by two MACs is two rows. Two boards cannot share
// an address for long, but a liar can try, and merging on the address would let one
// announce itself into another board's fleet row.
func TestOneAddressTwoMacsAreTwoRows(t *testing.T) {
	b, _ := hub(t)
	b.Ingest(answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80), from(goodIP))
	b.Ingest(answerBytes(2, otherInst, otherMAC, goodFW, "normal", goodIP, 120, 80), from(goodIP))
	if got := b.Candidates(); len(got) != 2 {
		t.Fatalf("%d rows, want 2 — two MACs on one address must not merge: %+v", len(got), got)
	}
}

func TestEmptyMacIsRefusedNotListed(t *testing.T) {
	b, _ := hub(t)
	b.Ingest(answerBytes(1, goodInst, "", goodFW, "normal", goodIP, 120, 80), from(goodIP))
	if got := b.Candidates(); len(got) != 0 {
		t.Fatalf("an orb whose TXT named no MAC was listed: %+v", got)
	}
	if len(b.Refusals()) == 0 {
		t.Fatal("the refusal was silent: an operator cannot tell 'no orb on the LAN' from 'an orb the hub would not name'")
	}
}

// A MAC whose tail does not agree with the instance name is two claims about one chip
// that do not line up. The firmware builds the name out of the address, so the
// mismatch is a stranger wearing an orb's shape — and the cross-lock Item 6 builds
// on this row is worth nothing if the row is half one board and half another.
func TestInstanceMustNameTheMacItCarries(t *testing.T) {
	b, _ := hub(t)
	b.Ingest(answerBytes(1, otherInst, goodMAC, goodFW, "normal", goodIP, 120, 80), from(goodIP))
	if got := b.Candidates(); len(got) != 0 {
		t.Fatalf("instance %q was listed beside a MAC it cannot belong to: %+v", otherInst, got)
	}
	said := false
	for _, r := range b.Refusals() {
		if strings.Contains(r.Reason, "does not name the mac") {
			said = true
		}
	}
	if !said {
		t.Errorf("the refusal never said why: %+v", b.Refusals())
	}
}

// Separators and capitals are the same address. The hub meets both spellings: the
// USB descriptor and the flasher print colon-separated, the device emits the bare
// twelve, and a browser that keyed on the spelling would show two rows for one board.
func TestMacSpellingDoesNotSplitARow(t *testing.T) {
	b, _ := hub(t)
	b.Ingest(answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80), from(goodIP))
	b.Ingest(answerBytes(2, goodInst, "44:BD:8D:78:26:18", goodFW, "normal", goodIP, 120, 80), from(goodIP))
	if got := b.Candidates(); len(got) != 1 {
		t.Fatalf("%d rows for one board spelled two ways: %+v", len(got), got)
	}
}

func TestGoodbyeRemovesTheRowImmediately(t *testing.T) {
	b, _ := hub(t)
	b.Ingest(answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80), from(goodIP))
	if len(b.Candidates()) != 1 {
		t.Fatal("the orb never arrived, so the goodbye would prove nothing")
	}
	b.Ingest(answerBytes(2, goodInst, goodMAC, goodFW, "normal", goodIP, 0, 80), from(goodIP))
	if got := b.Candidates(); len(got) != 0 {
		t.Fatalf("the orb said goodbye and is still being offered: %+v", got)
	}
}

// A hub that only listened would offer an orb for two minutes after its socket
// closed, because an announcement is sent four times at join and never again. The
// row ages out on the TTL the device itself asked for.
func TestRowExpiresOnTheAnnouncedTTL(t *testing.T) {
	b, clk := hub(t)
	b.Ingest(answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80), from(goodIP))
	*clk = clk.Add(119 * time.Second)
	if len(b.Candidates()) != 1 {
		t.Fatal("the orb vanished before its own TTL")
	}
	*clk = clk.Add(3 * time.Minute)
	if got := b.Candidates(); len(got) != 0 {
		t.Fatalf("a row outlived its TTL and its grace: %+v", got)
	}
}

func TestProvisioningOrbIsListedAndLabelled(t *testing.T) {
	b, _ := hub(t)
	b.Ingest(answerBytes(1, goodInst, goodMAC, goodFW, "provisioning", "192.168.4.1", 120, 80), from("192.168.4.35"))
	got := b.Candidates()
	if len(got) != 1 || !got[0].Provisioning() {
		t.Fatalf("the factory-reset case is not tellable from the list: %+v", got)
	}
}

// A query is not a thing a browser answers. The firmware answers questions and
// announces; a hub that also answered would be a second orb on the LAN, and its
// cache-flush records would overwrite the real one in every other browser's cache.
// The packet is written out as bytes here rather than built: the hub's own query
// encoder was deleted with the querier, and a test that re-implements one keeps its
// honesty for free.
func TestTheBrowserIgnoresQueries(t *testing.T) {
	b, _ := hub(t)
	b.Ingest(rawBrowseQuery(), from(goodIP))
	if got := b.Candidates(); len(got) != 0 {
		t.Fatalf("the hub answered a neighbour's question with a listing: %+v", got)
	}
}

// rawBrowseQuery is a PTR question for _orb._tcp.local, by hand: header with
// QDCOUNT=1, uncompressed labels, QTYPE=PTR, QCLASS=IN|QU.
func rawBrowseQuery() []byte {
	name := []byte{4, '_', 'o', 'r', 'b', 4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0}
	hdr := []byte{0xC0, 0xFE, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
	return append(append(hdr, name...), 0x00, 0x0C, 0x80, 0x01) // PTR, IN with QU
}

// Someone else's service table is not ours to list.
func TestForeignServiceIsIgnored(t *testing.T) {
	b, _ := hub(t)
	other := answerBytesFor("_orb._udp.local", 1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80)
	if bytes.Equal(other, answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80)) {
		t.Fatal("the foreign packet is identical to the orb's: this test proves nothing")
	}
	b.Ingest(other, from(goodIP))
	if got := b.Candidates(); len(got) != 0 {
		t.Fatalf("a foreign service was listed as an orb: %+v", got)
	}
}

// Malformed bytes are refused, not panicked on: this parser runs on datagrams any
// host on the LAN can send it, unasked.
func TestMalformedPacketsAreRefusedNotPanickedOn(t *testing.T) {
	b, _ := hub(t)
	good := answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80)
	bad := [][]byte{
		nil,
		good[:5],
		good[:len(good)-1],
		{0x00, 0x01, 0x84, 0x00},
		{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
	}
	for i, p := range bad {
		b.Ingest(p, from(goodIP)) // must not panic
		if _, err := Parse(p); err == nil {
			t.Errorf("malformed packet %d parsed clean", i)
		}
	}
	// A compression pointer that aims forwards, where nothing has committed yet.
	forward := append([]byte(nil), good...)
	forward[headerLen] = 0xC1
	forward[headerLen+1] = 0x40
	if _, err := Parse(forward); err == nil {
		t.Error("a forward pointer parsed: the walk may read bytes no one has committed to")
	}
	// And one that aims at itself, which would otherwise never end.
	self := append([]byte(nil), good...)
	self[headerLen] = 0xC0
	self[headerLen+1] = byte(headerLen)
	if _, err := Parse(self); err == nil {
		t.Error("a self-referential pointer parsed: the walk does not terminate")
	}
	// An RDLENGTH that lies leaves the walk short of the end of the message, and
	// every record after it reads out of the middle of the one before. The last
	// record's low RDLENGTH octet is four bytes from the end; one byte too many
	// must leave the walk a byte short, which Parse refuses outright.
	lie := append([]byte(nil), good...)
	lie[len(lie)-6] = 0x05
	if _, err := Parse(lie); err == nil {
		t.Error("a lying RDLENGTH parsed: the walk is not checked against the end of the message")
	}
}

// The SRV port is what the hub will later dial. A record that names no port is a
// row a click could not act on, so it is refused where it is heard rather than
// surfacing as a failure a user has to interpret.
func TestSRVWithoutAPortIsRefused(t *testing.T) {
	b, _ := hub(t)
	p := answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 0)
	b.Ingest(p, from(goodIP))
	if got := b.Candidates(); len(got) != 0 {
		t.Fatalf("a portless SRV was listed: %+v", got)
	}
}

// A0/0.0.0.0, loopback and the group itself are not orb addresses. Accepting one
// would put an address in the list that the proxy must then refuse to dial, which
// is a worse user experience than not listing it, and accepting 0.0.0.0 in
// particular is exactly what the firmware's own builder refuses to emit.
func TestImpossibleAddressesAreRefused(t *testing.T) {
	for _, ip := range []string{"0.0.0.0", "127.0.0.1", "224.0.0.251"} {
		b, _ := hub(t)
		b.Ingest(answerBytes(1, goodInst, goodMAC, goodFW, "normal", ip, 120, 80), from(goodIP))
		if got := b.Candidates(); len(got) != 0 {
			t.Errorf("A record %s was listed as an orb address: %+v", ip, got)
		}
	}
}

// TXT is the one place the firmware forbids compression, and a browser that expanded
// a 0xC0 pair inside it would read two garbage characters as part of a key.
func TestTXTBytesAreNeverExpanded(t *testing.T) {
	b, _ := hub(t)
	// A fw value that begins with 0xC0 0x27 — a pointer-shaped pair.
	b.Ingest(answerBytes(1, goodInst, goodMAC, "\xc0'", "normal", goodIP, 120, 80), from(goodIP))
	got := b.Candidates()
	if len(got) != 1 {
		t.Fatalf("pointer-shaped TXT bytes refused the whole orb: %+v", got)
	}
	if got[0].FW != "\xc0'" {
		t.Errorf("fw came back %q: something inside the TXT was expanded", got[0].FW)
	}
}

// ---- the wake-up: OnChange -------------------------------------------------------
//
// The card is pushed, and this is the half of the push that decides when it moves. The
// browser's rule is deliberately narrower than "a packet arrived" and deliberately wider than
// "the card's text changed": it reports that the cache moved and leaves the hub to decide
// whether that is worth a frame. A browser that tried to make that second decision would need
// to know the fleet, the grants and the cable MACs, which is the layering this package keeps.

// wakeRecorder counts wakes on a channel nobody blocks on, so a test can assert both that a
// wake came and that no more than one came.
func wakeRecorder() (*Browser, chan int) {
	ch := make(chan int, 16)
	n := 0
	b := NewBrowser(Config{
		Now:      func() time.Time { return time.Unix(1760000000, 0) },
		OnChange: func() { n++; ch <- n },
	})
	return b, ch
}

func wokeRecently(ch chan int) bool {
	select {
	case <-ch:
		return true
	case <-time.After(120 * time.Millisecond):
		return false
	}
}

// One check-in is one wake, however many records it carries. The firmware bundles PTR, SRV,
// TXT and A into every announcement, so a per-record wake would nudge the page four times for
// one row and make the stream's coalescing load-bearing on the first day.
func TestOneCheckInIsOneWake(t *testing.T) {
	b, woke := wakeRecorder()
	b.Ingest(answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80), from(goodIP))
	if !wokeRecently(woke) {
		t.Fatal("a check-in that became a row woke nobody")
	}
	if wokeRecently(woke) {
		t.Error("one check-in woke the card more than once: the wake is per datagram, not per record")
	}
}

// Every mutation the card could render wakes it, including the two that are not a row arriving.
// This is the test that keeps a seventh mutator from being added without one: a browser that
// stopped reporting a goodbye would leave the row on the page until the grace ran out, silently,
// which is the failure the callback exists to prevent.
func TestEveryMutationWakes(t *testing.T) {
	cases := []struct {
		what string
		pkt  []byte
	}{
		{"a row landing", answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80)},
		{"a refusal", answerBytes(1, goodInst, "", goodFW, "normal", goodIP, 120, 80)},
		{"an unparsable packet", []byte{0xc0, 0x1f, 0x00}},
	}
	for _, tc := range cases {
		b, woke := wakeRecorder()
		b.Ingest(tc.pkt, from(goodIP))
		if !wokeRecently(woke) {
			t.Errorf("%s changed what the card says and woke nobody", tc.what)
		}
	}

	// A goodbye removes a row, and the wake has to come from the removal rather from the packet
	// being interesting.
	b, woke := wakeRecorder()
	b.Ingest(answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80), from(goodIP))
	<-woke
	b.Ingest(answerBytes(2, goodInst, goodMAC, goodFW, "normal", goodIP, 0, 80), from(goodIP))
	if !wokeRecently(woke) {
		t.Error("an orb saying goodbye left the card still showing it, with no wake to fix that")
	}

	// Expiry is the third, and the only path that prunes a row no packet mentioned. The clock is
	// the test's, because nothing on a bench moves 20 minutes on its own.
	clock := time.Unix(1760000000, 0)
	woke4 := make(chan int, 4)
	b4 := NewBrowser(Config{
		Now:      func() time.Time { return clock },
		OnChange: func() { woke4 <- 1 },
	})
	b4.Ingest(answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80), from(goodIP))
	<-woke4
	clock = clock.Add(20 * time.Minute)
	b4.Candidates()
	if !wokeRecently(woke4) {
		t.Error("pruning an expired row woke nobody, so the page would keep the ghost until the next resync")
	}
}

// Nothing the card cannot see wakes it. A neighbour's query and an announcement about somebody
// else's service are traffic, and a hub that woke its page for either would push a frame for
// every packet on the LAN and call that live.
func TestUninterestingTrafficDoesNotWake(t *testing.T) {
	cases := []struct {
		what string
		pkt  []byte
	}{
		{"a query, which is ignored", rawBrowseQuery()},
		{"a foreign service", answerBytesFor("_http._tcp", 1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80)},
	}
	for _, tc := range cases {
		b, woke := wakeRecorder()
		b.Ingest(tc.pkt, from(goodIP))
		if wokeRecently(woke) {
			t.Errorf("%s woke the card without changing what it says", tc.what)
		}
	}
}

// The callback runs with no browser lock held, and this is the only thing that proves it. The
// hub's half is a non-blocking send today, so a wake fired under the lock would look harmless;
// it becomes a deadlock the first time a callback reads the browser back, which is the shape an
// obvious refactor takes. The call below would hang forever, so it is raced against a deadline.
func TestWakeFiresOutsideTheBrowserLock(t *testing.T) {
	done := make(chan struct{})
	var b *Browser
	b = NewBrowser(Config{
		Now: func() time.Time { return time.Unix(1760000000, 0) },
		OnChange: func() {
			_ = b.Candidates() // re-enters through mu: only legal if the lock is already gone
			_ = b.Refusals()
			close(done)
		},
	})
	b.Ingest(answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80), from(goodIP))
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("OnChange ran while the browser lock was held: it re-entered Candidates and blocked")
	}
}

// A hub with no callback is a complete browser, not a broken one: everything else is unchanged,
// which is what lets a test feed this one packets with no page attached.
func TestNilCallbackLeavesTheBrowserWhole(t *testing.T) {
	b, _ := hub(t)
	b.Ingest(answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80), from(goodIP))
	if got := b.Candidates(); len(got) != 1 {
		t.Fatalf("a browser built without OnChange listed %d rows", len(got))
	}
}
