package main

import (
	"bufio"
	"context"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"reflect"
	"regexp"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
)

// Item 5's promise, tested where the hub actually keeps it: the browse lists, and
// nothing in the listing reaches the orb.

// fakeDiscovery is a browser whose cache a test filled by hand, with the socket left
// out. It is the same type the hub runs — *orbmdns.Browser — fed through Ingest, so
// the test cannot pass by stubbing the thing under test.
func filled(t *testing.T, cs ...orbmdns.Candidate) *orbmdns.Browser {
	t.Helper()
	b := orbmdns.NewBrowser(orbmdns.Config{Now: func() time.Time { return time.Unix(1760000000, 0) }})
	for i, c := range cs {
		b.Ingest(announceFor(t, c), &net.UDPAddr{IP: net.ParseIP(c.From), Port: orbmdns.Port})
		_ = i
	}
	return b
}

func candidate(mac, ip string) orbmdns.Candidate {
	return orbmdns.Candidate{MAC: mac, IP: ip}
}

type discoveredBody struct {
	OK   bool `json:"ok"`
	Data struct {
		Orbs []struct {
			Instance  string   `json:"instance"`
			MAC       string   `json:"mac"`
			IP        string   `json:"ip"`
			Port      int      `json:"port"`
			FW        string   `json:"fw"`
			State     string   `json:"state"`
			Dialable  bool     `json:"dialable"`
			Refusal   string   `json:"refusal"`
			Name      string   `json:"name"`
			Cable     bool     `json:"cable"`
			OnCable   bool     `json:"onCable"`
			Key       string   `json:"key"`
			Via       []string `json:"via"`
			Ports     []string `json:"ports"`
			Identity  string   `json:"identity"`
			Contended bool     `json:"contended"`
			Shared    bool     `json:"shared"`
			Bound     bool     `json:"bound"`
		} `json:"orbs"`
		Refused    []map[string]any `json:"refused"`
		Listening  bool             `json:"listening"`
		Error      string           `json:"error"`
		CableError string           `json:"cableError"`
	} `json:"data"`
}

func getDiscovered(t *testing.T, s *Server) discoveredBody {
	t.Helper()
	rec := httptest.NewRecorder()
	s.handleDiscovered(rec, httptest.NewRequest(http.MethodGet, "/api/orbs", nil))
	if rec.Code != 200 {
		t.Fatalf("GET /api/orbs -> %d: %s", rec.Code, rec.Body.String())
	}
	var out discoveredBody
	if err := json.Unmarshal(rec.Body.Bytes(), &out); err != nil {
		t.Fatalf("the payload is not the envelope the UI parses: %v (%s)", err, rec.Body.String())
	}
	if !out.OK {
		t.Fatalf("the listing came back not-ok: %s", rec.Body.String())
	}
	return out
}

// A browse lists the orb and, in the same breath, refuses to make it reachable. The
// allowlist is what keeps the hub from being an SSRF pivot out of 127.0.0.1, and
// discovery is the first feature that hands the hub an address nobody typed.
func TestDiscoveredAddressIsNotDialable(t *testing.T) {
	s := newHub()
	s.browse = filled(t, candidate("44bd8d782618", "192.168.2.34"))
	got := getDiscovered(t, s)
	if len(got.Data.Orbs) != 1 {
		t.Fatalf("the card lists %d orbs: %s", len(got.Data.Orbs), got.Data.Error)
	}
	row := got.Data.Orbs[0]
	if row.MAC != "44bd8d782618" || row.IP != "192.168.2.34" {
		t.Errorf("the row is wrong: %+v", row)
	}
	if row.Dialable {
		t.Error("a discovered address reported itself as dialable before anyone allowed it")
	}
	// The real gate, not the readout: the proxy must refuse it too.
	rec := httptest.NewRecorder()
	s.proxyOrb(rec, httptest.NewRequest(http.MethodGet, "/orb/192.168.2.34/api/v1/status", nil))
	if rec.Code != http.StatusForbidden {
		t.Errorf("the proxy forwarded to a discovered address without an allow: %d %s", rec.Code, rec.Body.String())
	}
	// And it stays refused after the listing was read: a browse must not learn a
	// grant as a side effect of being useful.
	if s.grants.allowed("192.168.2.34") {
		t.Error("listing an orb taught the hub to dial it")
	}
}

// The operator's click on a listed orb is the same deliberate commitment as typing
// the address, so it goes through the door that already exists and the proxy opens
// for exactly that address.
func TestAllowingADiscoveredAddressOpensOnlyThatAddress(t *testing.T) {
	s := newHub()
	s.browse = filled(t, candidate("44bd8d782618", "192.168.2.34"))
	getDiscovered(t, s)
	s.grants.learn("192.168.2.34")
	if !s.grants.allowed("192.168.2.34") {
		t.Fatal("the allow did not take")
	}
	if s.grants.allowed("192.168.2.35") {
		t.Error("allowing one orb opened its whole /24")
	}
}

// A hub with no sockets — a machine where 5353 is taken, a sandbox, CI — still
// answers the card, and answers it with a reason rather than an empty list that
// looks like an empty LAN.
func TestDiscoveryUnavailableSaysSo(t *testing.T) {
	s := newHub()
	s.setDiscovery(nil, "no interface could join 224.0.0.251")
	got := getDiscovered(t, s)
	if got.Data.Listening || got.Data.Error == "" {
		t.Errorf("the card reported a healthy listener that does not exist: %+v", got.Data)
	}
	if got.Data.Orbs == nil || got.Data.Refused == nil {
		t.Error("the slices were nil: app.js destructures them and reads .length")
	}
}

// The refused set is the difference between "no orb on the LAN" and "an orb the hub
// would not name", and an operator cannot tell those apart from a blank card.
func TestRefusedAnnouncementsReachTheCard(t *testing.T) {
	b := orbmdns.NewBrowser(orbmdns.Config{})
	b.Ingest(answerWithMAC(t, "", "192.168.2.34"), &net.UDPAddr{IP: net.ParseIP("192.168.2.34"), Port: orbmdns.Port})
	s := newHub()
	s.browse = b
	got := getDiscovered(t, s)
	if len(got.Data.Orbs) != 0 {
		t.Fatalf("a nameless announcement was listed: %+v", got.Data.Orbs)
	}
	if len(got.Data.Refused) == 0 {
		t.Fatal("the refusal never reached the card")
	}
	if !strings.Contains(got.Data.Refused[0]["reason"].(string), "mac") {
		t.Errorf("the refusal does not say what was wrong: %v", got.Data.Refused[0])
	}
}

// The hub has no route that sends anything, and this pins that rather than pinning
// the guard around a send. /api/orbs/ask used to sit here behind the launch token;
// the querier it drove is gone, so the route is gone, and a POST to it must 404
// rather than be re-added behind a token by the next person who misses it.
func TestHubHasNoRouteThatAsks(t *testing.T) {
	s := newHub()
	s.token = "test-token"
	mux := http.NewServeMux()
	s.routes(mux)
	req := httptest.NewRequest(http.MethodPost, "/api/orbs/ask", nil)
	req.Header.Set("X-Orb-Token", "test-token")
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)
	if rec.Code != http.StatusNotFound {
		t.Errorf("POST /api/orbs/ask -> %d, want 404: the hub sends no query, so the route must not exist", rec.Code)
	}
	// And the surface has no way to ask either: reflection on the concrete browser,
	// so a method revived there fails this rather than only its own test.
	if _, ok := reflect.TypeOf(&orbmdns.Browser{}).MethodByName("Ask"); ok {
		t.Error("orbmdns.Browser grew an Ask method back: discovery is check-in only")
	}
}

// upkeepLoop is housekeeping, so the test drives it on a shrunk interval: it must
// tick over the cache on schedule, hold nothing when ctx is cancelled, and get no
// chance to send because the interface it reads has no send.
func TestUpkeepLoopTicksAndStops(t *testing.T) {
	saved := upkeepInterval
	upkeepInterval = 10 * time.Millisecond
	defer func() { upkeepInterval = saved }()

	s := newHub()
	d := &tickDiscovery{ticks: make(chan struct{}, 8)}
	s.browse = d
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { s.upkeepLoop(ctx); close(done) }()

	select {
	case <-d.ticks:
	case <-time.After(time.Second):
		t.Fatal("the upkeep tick never read the cache")
	}
	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("upkeepLoop ignored the cancellation")
	}
}

// tickDiscovery reports how many upkeep ticks reached it and nothing else. It cannot
// record a send because the discovery interface has no method that could send one.
type tickDiscovery struct{ ticks chan struct{} }

func (d *tickDiscovery) Candidates() []orbmdns.Candidate {
	select {
	case d.ticks <- struct{}{}:
	default:
	}
	return nil
}
func (*tickDiscovery) Refusals() []orbmdns.Refusal { return nil }
func (*tickDiscovery) Close() error                { return nil }

// The card's promise is a number: "check-ins arrive every 60 s". That number lives
// in the firmware, so the firmware source gets the vote. It skips when the tree is
// not reachable, which is the shipped-app case.
func TestCheckinCadenceMatchesFirmware(t *testing.T) {
	src := firmwareFile(t, "main/net/orb_mdns.c")
	get := func(name string) int {
		m := regexp.MustCompile(`#define\s+` + name + `\s+(\d+)`).FindStringSubmatch(src)
		if m == nil {
			t.Fatalf("main/net/orb_mdns.c no longer defines %s: the hub's copy of the cadence is meaningless", name)
		}
		n, err := strconv.Atoi(m[1])
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		return n
	}
	if got := get("MDNS_ADV_REFRESH_S"); got != checkinSeconds {
		t.Errorf("firmware re-announces every %d s, the hub promises the UI %d s: one of the two moved and the card now lies", got, checkinSeconds)
	}
	// The row lives TTL + grace, not TTL alone: that sum is how much announce loss a
	// card absorbs before a live orb blinks out. One full interval of slack is the
	// floor — it buys exactly one retry — and the grace the browser prunes with is
	// hub config the UI's promise depends on, so it is read from the same place.
	ttl := get("MDNS_TTL")
	if ttl+int(orbmdns.DefaultGrace/time.Second) < 2*checkinSeconds {
		t.Errorf("a row survives %d s (TTL %d + grace %d) against a %d s check-in: one lost announce drops a live orb off the card",
			ttl+int(orbmdns.DefaultGrace/time.Second), ttl, int(orbmdns.DefaultGrace/time.Second), checkinSeconds)
	}
}

// --- the builders the tests above need ---------------------------------------------
//
// The packets are hand-encoder-free: internal/orbmdns exposes the announcement
// builder it judges with, so these tests build wire bytes through the same
// transcription of the C codec the fake ships. The alternative — literal hex — would
// pin the hub to a snapshot of the encoder rather than to the encoder.

// ---- the pushed card: /api/orbs/stream --------------------------------------------
//
// The card moves when an orb checks in. These tests drive that end to end: a real httptest
// listener, the real browser the hub ships (s.newBrowser, wiring and all), and a real
// announcement from internal/orbfake fed in through Ingest — the same call the socket pump makes.
// A test cannot pass here by firing the callback by hand, which is the difference between testing
// the push and testing a function that pushes.

// feed is one open /api/orbs/stream and the reader that owns its bytes. The bufio.Reader lives on
// the feed rather than inside whichever assertion is reading, because the hub flushes a frame the
// moment a check-in lands and two frames can arrive in one chunk: a reader built per call throws
// the second one away, and the test then waits for a frame that already came.
type feed struct {
	s   *Server
	b   *orbmdns.Browser
	res *http.Response
	// lines is the stream's lines, pumped by one goroutine for the life of the feed. A reader
	// per assertion would fight over the same buffered body: the second assertion to read the
	// stream would find the first one's goroutine still holding it, and the test would report a
	// card that never moves when the fault is the harness.
	lines chan string
}

// openFeed starts a hub with its own browser wired up and opens the stream against it, with no
// token presented: the route answers the same bytes the unguarded read does, so it is guarded the
// same way, which is to say not at all.
func openFeed(t *testing.T) *feed {
	t.Helper()
	s := newHub()
	b := s.newBrowser()
	s.setDiscovery(b, "")
	mux := http.NewServeMux()
	s.routes(mux)
	srv := httptest.NewServer(mux)
	req, err := http.NewRequest(http.MethodGet, srv.URL+"/api/orbs/stream", nil)
	if err != nil {
		t.Fatal(err)
	}
	res, err := srv.Client().Do(req)
	if err != nil {
		t.Fatalf("GET /api/orbs/stream: %v", err)
	}
	t.Cleanup(func() { _ = res.Body.Close(); srv.Close(); _ = b.Close() })
	if res.StatusCode != 200 {
		t.Fatalf("the stream answered %d, not 200", res.StatusCode)
	}
	if got := res.Header.Get("Content-Type"); got != "application/x-ndjson" {
		t.Errorf("the stream is served as %q, not the line format the client reads", got)
	}
	f := &feed{s: s, b: b, res: res, lines: make(chan string, 32)}
	go f.pump(bufio.NewReader(res.Body))
	return f
}

// pump reads the stream's non-empty lines into the feed's channel until the body ends. One
// goroutine for the life of the feed on one bufio.Reader: two readers over one body lose the
// bytes the first buffered, and the loss looks exactly like a card that never moved.
func (f *feed) pump(br *bufio.Reader) {
	defer close(f.lines)
	for {
		l, err := br.ReadString('\n')
		if err != nil {
			return
		}
		if l = strings.TrimSpace(l); l != "" {
			f.lines <- l
		}
	}
}

// frame reads the next data frame, skipping the keepalive comments the way the page does, and
// fails rather than blocking forever: a card that never moved is the finding, not the wait.
func (f *feed) frame(t *testing.T) discoveredBody {
	t.Helper()
	for {
		select {
		case l, ok := <-f.lines:
			if !ok {
				t.Fatal("the stream closed before it answered")
			}
			if strings.HasPrefix(l, ":") {
				continue
			}
			var out discoveredBody
			if err := json.Unmarshal([]byte(l), &out); err != nil {
				t.Fatalf("frame is not the envelope the UI parses: %v (%s)", err, l)
			}
			if !out.OK {
				t.Fatalf("frame came back not-ok: %s", l)
			}
			return out
		case <-time.After(3 * time.Second):
			t.Fatal("no frame within 3 s: the card did not move")
		}
	}
}

// staysQuiet asserts that nothing but comment lines arrives for the given wait. It is the only
// way "a refresh that changed nothing pushed nothing" means anything: the absence of a frame is
// only observable by waiting long enough for one to have shown up.
func (f *feed) staysQuiet(t *testing.T, wait time.Duration) {
	t.Helper()
	deadline := time.After(wait)
	for {
		select {
		case l, ok := <-f.lines:
			if !ok {
				return // the hub hung up; nothing is being pushed either
			}
			if strings.HasPrefix(l, ":") {
				continue
			}
			if strings.Contains(l, `"orbs"`) {
				t.Errorf("a check-in that changed nothing pushed a whole card: %.200s", l)
				return
			}
		case <-deadline:
			return
		}
	}
}

// checkIn feeds one announcement into the hub's own browser through Ingest, which is the entry
// point the socket pump uses. What moves the card is the packet, not a callback the test fired.
func checkIn(t *testing.T, b *orbmdns.Browser, mac, ip, state string) {
	t.Helper()
	b.Ingest(announceFor(t, orbmdns.Candidate{MAC: mac, IP: ip, State: state, FW: "094fa22"}),
		&net.UDPAddr{IP: net.ParseIP(ip), Port: orbmdns.Port})
}

// The claim the feature is named after: an orb checks in and the page moves. Not on a timer, and
// not on a click — there is no button left to press.
func TestCheckInMovesTheOpenCard(t *testing.T) {
	f := openFeed(t)
	if first := f.frame(t); len(first.Data.Orbs) != 0 {
		t.Fatalf("the opening frame listed %d rows before anything announced", len(first.Data.Orbs))
	}
	checkIn(t, f.b, "44bd8d782618", "192.168.2.34", "normal")
	second := f.frame(t)
	if len(second.Data.Orbs) != 1 {
		t.Fatalf("the check-in produced a frame with %d rows", len(second.Data.Orbs))
	}
	if second.Data.Orbs[0].MAC != "44bd8d782618" {
		t.Errorf("the pushed row is mac %s", second.Data.Orbs[0].MAC)
	}
	if second.Data.Orbs[0].Dialable {
		t.Error("a pushed row arrived already dialable, so the card would be granting by arriving")
	}
	// The same connection keeps moving: a second orb is a second frame, not a stream that served
	// its one snapshot and stopped.
	checkIn(t, f.b, "44bd8daabbcc", "192.168.2.35", "normal")
	if third := f.frame(t); len(third.Data.Orbs) != 2 {
		t.Fatalf("the second check-in produced %d rows", len(third.Data.Orbs))
	}
}

// Every frame is what the read answers, so the page has one renderer and its first paint is not a
// second code path. The comparison is on the decoded payload, which drops the one field that moves
// by itself, so two answers microseconds apart have to agree on everything else.
func TestStreamFrameIsWhatTheGetAnswers(t *testing.T) {
	f := openFeed(t)
	f.frame(t) // the opening snapshot; the frame under test is the one the check-in earns
	checkIn(t, f.b, "44bd8d782618", "192.168.2.34", "normal")
	pushed := f.frame(t)
	polled := getDiscovered(t, f.s)
	if !reflect.DeepEqual(pushed.Data, polled.Data) {
		a, _ := json.Marshal(pushed.Data)
		c, _ := json.Marshal(polled.Data)
		t.Errorf("a row means one thing pushed and another polled:\n push %s\n  get %s", a, c)
	}
}

// A refresh is not a change. The firmware re-announces every 60 s whether or not anything moved,
// and a stream that counted that as news would repaint the page once a minute per orb forever —
// the polling design with extra steps. The row still has to move when the orb's state differs, or
// this rule is just a stream that quietly stopped working.
func TestQuietCheckInPushesNoFrame(t *testing.T) {
	f := openFeed(t)
	f.frame(t)
	checkIn(t, f.b, "44bd8d782618", "192.168.2.34", "normal")
	f.frame(t)
	checkIn(t, f.b, "44bd8d782618", "192.168.2.34", "normal")
	f.staysQuiet(t, 500*time.Millisecond)
	checkIn(t, f.b, "44bd8d782618", "192.168.2.34", "provisioning")
	got := f.frame(t)
	if len(got.Data.Orbs) != 1 || got.Data.Orbs[0].State != "provisioning" {
		t.Errorf("an orb that began provisioning never reached the card: %+v", got.Data.Orbs)
	}
}

// One slot per open page, and a page that stopped draining is skipped rather than waited on. A
// blocking send here would let one hung browser tab stop the goroutine reading UDP 5353, which is
// the hub going deaf on every orb on the LAN because one page stopped caring. This test drives the
// fan-out directly rather than through a stream, because the thing under test is what happens when
// nobody is reading.
func TestWakeNeverWaitsOnASubscriber(t *testing.T) {
	s := newHub()
	stalled := s.subscribeOrbs()
	another := s.subscribeOrbs()
	for i := 0; i < 50; i++ {
		done := make(chan struct{})
		go func() { s.wakeOrbs(); close(done) }()
		select {
		case <-done:
		case <-time.After(time.Second):
			t.Fatalf("wakeOrbs blocked on a full slot, on wake %d", i)
		}
	}
	select {
	case <-stalled:
	default:
		t.Error("the first wake never reached the first subscriber")
	}
	select {
	case <-another:
	default:
		t.Error("one slow page starved a second subscriber, so the fan-out is not independent")
	}
	s.unsubscribeOrbs(stalled)
	s.omu.Lock()
	n := len(s.orbSubs)
	s.omu.Unlock()
	if n != 1 {
		t.Errorf("unsubscribing left %d subscribers, so a closed page would keep being woken", n)
	}
}

// The route exists and takes one method. The unguarded half is proved by openFeed, which opens
// this exact route with no token: what this pins is registration, because a page whose stream 404s
// shows a card that never moves and says nothing about why.
func TestStreamRouteShape(t *testing.T) {
	s := newHub()
	mux := http.NewServeMux()
	s.routes(mux)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/api/orbs/stream", nil))
	if rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("POST /api/orbs/stream -> %d, want 405: the route exists and takes one method", rec.Code)
	}
}

// The hub is still a listener. Adding a pushed read must not be the occasion for reviving the
// querier, so the old refusal is re-asserted here rather than left to the one test above: a stream
// is a longer-lived way of saying what the cache already says, and nothing more.
func TestStreamSendsNoQueryEither(t *testing.T) {
	f := openFeed(t)
	f.frame(t)
	checkIn(t, f.b, "44bd8d782618", "192.168.2.34", "normal")
	f.frame(t)
	if _, ok := reflect.TypeOf(&orbmdns.Browser{}).MethodByName("Ask"); ok {
		t.Error("orbmdns.Browser grew an Ask method back while the card was being made live")
	}
}

// announceFor renders one candidate as the packet a browser would have to have heard
// to produce it, which is how a test seeds a listing without a network.
func announceFor(t *testing.T, c orbmdns.Candidate) []byte {
	t.Helper()
	f := orbfake.New()
	f.SetSTA(true)
	mac := c.MAC
	if mac == "" {
		mac = "44bd8d782618"
	}
	f.SetAddress(mac, c.IP)
	f.SetFirmware(orElse(c.FW, "094fa22"))
	f.SetProvisioning(c.State == "provisioning")
	a := f.Advertise(orbfake.AdvertiseConfig{Conn: nopSock{}, Port: int(orElseUint16(c.Port, 80))})
	p := a.AnnounceBytes(false)
	if p == nil {
		t.Fatal("the announcement would not encode")
	}
	return p
}

// answerWithMAC builds an announcement whose TXT mac= is whatever the test wants,
// including the empty value a lying or half-initialised responder would send.
func answerWithMAC(t *testing.T, mac, ip string) []byte {
	t.Helper()
	f := orbfake.New()
	f.SetSTA(true)
	f.SetAddress("44bd8d782618", ip)
	f.SetFirmware("094fa22")
	a := f.Advertise(orbfake.AdvertiseConfig{Conn: nopSock{}, Port: 80})
	if mac != "" {
		t.Fatalf("answerWithMAC is only used for the empty-mac case: %q", mac)
	}
	// The mac is blanked rather than the identity rebuilt around a missing one: the
	// instance is derived from the address, so erasing the field is the only way to
	// hand the browser an announcement that claims a chip without naming it.
	return swapMAC(t, a.AnnounceBytes(false))
}

// swapMAC rewrites the TXT mac= string's value in place. The substitution is
// length-preserving — twelve hex digits to twelve spaces — so no RDLENGTH in the
// packet has to be recomputed to make the field empty, which is what keeps this a
// test of the browser's rule and not of the builder's arithmetic.
func swapMAC(t *testing.T, pkt []byte) []byte {
	t.Helper()
	i := indexOf(pkt, []byte("mac="))
	if i < 0 {
		t.Fatal("no mac= in the announcement the builder produced")
	}
	out := append([]byte(nil), pkt...)
	for j := i + 4; j < i+16 && j < len(out); j++ {
		out[j] = ' '
	}
	return out
}

func indexOf(h, n []byte) int {
	for i := 0; i+len(n) <= len(h); i++ {
		match := true
		for j := range n {
			if h[i+j] != n[j] {
				match = false
				break
			}
		}
		if match {
			return i
		}
	}
	return -1
}

func orElseUint16(v uint16, alt int) uint16 {
	if v == 0 {
		return uint16(alt)
	}
	return v
}

// The factory-reset shape item 10 depends on, taken all the way from the fake's
// encoder through the browser to the card: a SoftAP-only orb is listed, is labelled
// provisioning, and is not dialable by this hub, because 192.168.4.1 is an address no
// wired box has a route to. A row that looked like every other row would be a click
// that hangs until the dial times out.
func TestProvisioningApRowIsLabelledAndNotDialable(t *testing.T) {
	f := orbfake.New()
	f.SetAddress("44bd8d782618", "")
	f.SetFirmware("094fa22")
	f.SetSTA(false)
	f.SetSoftAP(true, "192.168.4.1")
	f.SetProvisioning(true)
	a := f.Advertise(orbfake.AdvertiseConfig{Conn: nopSock{}, Port: 80})
	pkt := a.AnnounceBytes(false)
	if pkt == nil {
		t.Fatal("the SoftAP announcement would not encode")
	}

	b := orbmdns.NewBrowser(orbmdns.Config{})
	b.Ingest(pkt, &net.UDPAddr{IP: net.ParseIP("192.168.4.35"), Port: orbmdns.Port})
	cands := b.Candidates()
	if len(cands) != 1 {
		t.Fatalf("the SoftAP orb was not listed (%d rows): %+v", len(cands), cands)
	}
	if !cands[0].Provisioning() {
		t.Errorf("state=%q; the browser lost the one word that says this orb is unjoined", cands[0].State)
	}
	if cands[0].IP != "192.168.4.1" {
		t.Errorf("A=%s, want the AP address the orb owns", cands[0].IP)
	}

	s := newHub()
	s.browse = b
	rec := httptest.NewRecorder()
	s.handleDiscovered(rec, httptest.NewRequest(http.MethodGet, "/api/orbs", nil))
	var body struct {
		OK   bool `json:"ok"`
		Data struct {
			Orbs []struct {
				IP           string `json:"ip"`
				State        string `json:"state"`
				Provisioning bool   `json:"provisioning"`
				Dialable     bool   `json:"dialable"`
			} `json:"orbs"`
		} `json:"data"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatalf("the payload is not the envelope the UI parses: %v (%s)", err, rec.Body.String())
	}
	if len(body.Data.Orbs) != 1 {
		t.Fatalf("the card lists %d orbs", len(body.Data.Orbs))
	}
	row := body.Data.Orbs[0]
	if !row.Provisioning || row.State != "provisioning" {
		t.Errorf("the card lost the provisioning label: %+v", row)
	}
	if row.IP != "192.168.4.1" || row.Dialable {
		t.Errorf("the card presented an unroutable AP address as dialable: %+v", row)
	}
	// The gate, not the readout: the proxy must refuse the AP address too.
	pr := httptest.NewRecorder()
	s.proxyOrb(pr, httptest.NewRequest(http.MethodGet, "/orb/192.168.4.1/api/v1/status", nil))
	if pr.Code != http.StatusForbidden {
		t.Errorf("the proxy forwarded to a discovered AP address without an allow: %d %s", pr.Code, pr.Body.String())
	}
}

// --- the click is the commitment, and the commitment is guarded ---------------------
//
// A row in the list is a claim the orb made about itself; dialing it is the operator's
// decision. web/app.js implements that by routing the row's button through
// scan(), which POSTs /api/orb/allow and then proxies — the same two calls the typed
// address makes. These two tests are what keep that from quietly becoming a shortcut:
// the allow must go through the guarded route, and a discovered address must stay
// undialable until it does.

func postHub(t *testing.T, srv *httptest.Server, path string, hdr map[string]string, body string) (int, string) {
	t.Helper()
	req, err := http.NewRequest(http.MethodPost, srv.URL+path, strings.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Content-Type", "application/json")
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("POST %s: %v", path, err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, string(b)
}

// The button on a discovered row is not a lesser door than the Orb IP box: without
// this launch's credential it opens nothing, and the hub does not learn the address as
// a side effect of the list having been read.
func TestDiscoveredRowIsOnlyDialableThroughTheGuardedAllow(t *testing.T) {
	s, srv := securedHub(t)
	s.token = "click-token"
	s.browse = filled(t, candidate("44bd8d782618", "192.168.2.34"))
	origin := "http://" + strings.TrimPrefix(srv.URL, "http://")

	// The click, minus the token: refused, and the proxy stays shut.
	if code, body := postHub(t, srv, "/api/orb/allow", nil, `{"ip":"192.168.2.34"}`); code != http.StatusForbidden {
		t.Errorf("an untokened allow of a discovered address -> %d, want 403: %s", code, body)
	}
	if s.grants.allowed("192.168.2.34") {
		t.Fatal("the refused allow still taught the hub to dial the orb")
	}
	// Tokened but from a page the operator never opened: same refusal, same nothing.
	if code, body := postHub(t, srv, "/api/orb/allow",
		map[string]string{"X-Orb-Token": "click-token", "Origin": "http://evil.example"},
		`{"ip":"192.168.2.34"}`); code != http.StatusForbidden {
		t.Errorf("a cross-origin tokened allow -> %d, want 403: %s", code, body)
	}
	if s.grants.allowed("192.168.2.34") {
		t.Fatal("a cross-origin allow opened the address")
	}

	// The real click: token, same origin, and the card's own address.
	if code, body := postHub(t, srv, "/api/orb/allow",
		map[string]string{"X-Orb-Token": "click-token", "Origin": origin},
		`{"ip":"192.168.2.34"}`); code != http.StatusOK {
		t.Fatalf("the guarded allow -> %d: %s", code, body)
	}
	if !s.grants.allowed("192.168.2.34") {
		t.Fatal("the allow did not take")
	}
	if s.grants.allowed("192.168.2.35") {
		t.Error("allowing one orb opened its whole /24")
	}
	// The readout follows the gate rather than leading it.
	rec := httptest.NewRecorder()
	s.handleDiscovered(rec, httptest.NewRequest(http.MethodGet, "/api/orbs", nil))
	if !strings.Contains(rec.Body.String(), `"dialable":true`) {
		t.Errorf("the card still reports the allowed orb undialable: %s", rec.Body.String())
	}
}
