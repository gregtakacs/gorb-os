package orbmdns

import (
	"bytes"
	"context"
	"net"
	"strconv"
	"testing"
	"time"
)

// These cover RFC 6762 §7.1 known-answer suppression on the outbound query — the
// mechanism that makes the timer-driven re-ask a sustained querier instead of a
// standing flood — and that it never turns the browser into a responder. Same-package
// helpers reused: answerBytes/goodInst/goodMAC/from build a live candidate; errIdle is
// the sentinel that lets a fake Source's pump exit.

// captureSource is the Source a known-answer test hands the browser: it stores every
// packet's full bytes (recordingSource keeps only the length) and delivers nothing, so
// the only traffic is what Ask chose to emit, in the order it emitted it.
type captureSource struct{ sent [][]byte }

func (c *captureSource) ReadFrom([]byte) (int, net.Addr, error) {
	return 0, nil, errIdle // pump exits; this test is about the write side
}
func (c *captureSource) WriteTo(p []byte, addr net.Addr) (int, error) {
	if ua, ok := addr.(*net.UDPAddr); !ok || ua.IP.String() != GroupAddr {
		return 0, net.ErrClosed // a non-group write is the bug under test; fail it loudly
	}
	c.sent = append(c.sent, append([]byte(nil), p...))
	return len(p), nil
}
func (c *captureSource) Close() error { return nil }

// liveHub is a started browser holding one fully-resolved orb, with the clock pointer
// the caller advances. Ingest promotes the orb into the candidate table the known
// answers are read from, so Ask has something to name back.
func liveHub(t *testing.T, now time.Time) (*Browser, *time.Time, *captureSource) {
	t.Helper()
	clock := now
	b := NewBrowser(Config{Now: func() time.Time { return clock }})
	cap := &captureSource{}
	if err := b.Start(context.Background(), cap); err != nil {
		t.Fatalf("start: %v", err)
	}
	t.Cleanup(func() { _ = b.Close() })
	b.Ingest(answerBytes(1, goodInst, goodMAC, goodFW, "normal", goodIP, 120, 80), from(goodIP))
	return b, &clock, cap
}

// A live, freshly-heard orb is listed back as a known answer of the browse query:
// QDCOUNT stays 1 and ANCOUNT carries the PTR naming the instance we already hold, at
// its ORIGINAL remaining TTL because nothing has aged yet.
func TestAskListsAFreshOrbAsAKnownAnswer(t *testing.T) {
	start := time.Unix(1760000000, 0)
	b, _, cap := liveHub(t, start)
	if err := b.Ask(); err != nil {
		t.Fatalf("ask: %v", err)
	}
	if len(cap.sent) == 0 {
		t.Fatal("Ask emitted nothing")
	}
	m, err := Parse(cap.sent[0])
	if err != nil {
		t.Fatalf("parse the browse query: %v", err)
	}
	if m.Response {
		t.Fatal("Ask emitted a response, not a query: a browser that answers is a responder")
	}
	if len(m.Questions) != 1 || m.Questions[0].Name != ServiceName {
		t.Fatalf("the browse query asked %v, want one %s", m.Questions, ServiceName)
	}
	if len(m.Answers) != 1 {
		t.Fatalf("known answers = %d, want the one live instance: %+v", len(m.Answers), m.Answers)
	}
	ka := m.Answers[0]
	if ka.Type != typePTR || ka.PTR != InstanceName(goodInst) {
		t.Errorf("known answer is %v not a PTR for %s", ka, InstanceName(goodInst))
	}
	if ka.TTL != 120 {
		t.Errorf("known answer TTL %d, want the full 120 the orb has not yet aged", ka.TTL)
	}
}

// The whole point of the aged TTL: as time passes the known answer shrinks toward the
// half-TTL floor, and the instant it drops below half it is omitted so the next real
// answer refreshes the row before it can expire. A known answer frozen at the original
// TTL would claim permanent freshness and suppress every update forever.
func TestKnownAnswersAgeAndThenGoQuiet(t *testing.T) {
	start := time.Unix(1760000000, 0)
	b, clk, cap := liveHub(t, start)

	*clk = start.Add(45 * time.Second) // 75 s left, above half: still listed, aged down
	_ = b.Ask()
	m, err := Parse(cap.sent[len(cap.sent)-1])
	if err != nil || len(m.Answers) != 1 {
		t.Fatalf("at 45 s a fresh record should be listed once: err=%v answers=%+v", err, m.Answers)
	}
	if m.Answers[0].TTL != 75 {
		t.Errorf("known answer TTL %d, want 75 (120 minus the 45 s aged)", m.Answers[0].TTL)
	}

	*clk = start.Add(70 * time.Second) // 50 s left, below half 60: omitted
	_ = b.Ask()
	m2, err := Parse(cap.sent[len(cap.sent)-1])
	if err != nil {
		t.Fatalf("parse query after ageing: %v", err)
	}
	if len(m2.Answers) != 0 {
		t.Fatalf("a record under half TTL was still listed as known: it suppresses the very refresh meant to save it (%+v)", m2.Answers)
	}
}

// A query is never a response on the way out, and known answers must not change that:
// Ingest drops anything with QR clear, so a browser that listed its own cache back into
// a query would feed itself if the QR bit ever flipped. The empty-known query is also
// asserted byte-identical to the plain browse, so the fast path stays the fast path.
func TestKnownAnswerQueryIsStillOnlyAQuery(t *testing.T) {
	known := []Record{{
		Name: ServiceName, Type: typePTR, TTL: 100,
		PTR: InstanceName(goodInst),
	}}
	q := BuildQueryWithKnown(0xBEEF, ServiceName, typePTR, known)
	m, err := Parse(q)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if m.Response {
		t.Error("a known-answer query came back flagged as a response")
	}
	// No known answers: byte-for-byte the plain BrowseQuery, so an idle hub's query is
	// not merely equivalent, it is identical.
	if empty := BuildQueryWithKnown(0xBEEF, ServiceName, typePTR, nil); !bytes.Equal(empty, BrowseQuery(0xBEEF)) {
		t.Error("an empty known-answer query differs byte-wise from the plain browse")
	}
}

// §7 caps a known-answer list by nothing; a browser that trusted its own cache size
// could build a query past one datagram. The encoder must stop at maxKnownAnswers and
// stay parseable, not emit a truncated or oversized message.
func TestKnownAnswersAreCappedNotOverflowed(t *testing.T) {
	start := time.Unix(1760000000, 0)
	b := NewBrowser(Config{Now: func() time.Time { return start }})
	cap := &captureSource{}
	if err := b.Start(context.Background(), cap); err != nil {
		t.Fatalf("start: %v", err)
	}
	defer b.Close()
	const n = maxKnownAnswers + 5
	for i := 0; i < n; i++ {
		// ExpectInstance ties the leftmost label to the MAC's last six digits, so
		// the fake announcements must honour it or every one is refused and the
		// browser has nothing to name. Suffix is hex-clean digits, lower-cased.
		suffix := strconv.FormatInt(int64(i), 16)
		for len(suffix) < 6 {
			suffix = "0" + suffix
		}
		mac := "44bd8d" + suffix
		inst := "orb-" + suffix
		b.Ingest(answerBytes(uint16(i+1), inst, mac, goodFW, "normal", goodIP, 120, 80), from(goodIP))
	}
	if err := b.Ask(); err != nil {
		t.Fatalf("ask: %v", err)
	}
	m, err := Parse(cap.sent[0])
	if err != nil {
		t.Fatalf("a capped query failed to parse: %v", err)
	}
	if len(m.Answers) != maxKnownAnswers {
		t.Errorf("known answers %d, want exactly the cap %d with %d live instances", len(m.Answers), maxKnownAnswers, n)
	}
}
