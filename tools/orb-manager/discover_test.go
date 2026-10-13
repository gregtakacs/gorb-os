package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
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
			Instance  string `json:"instance"`
			MAC       string `json:"mac"`
			IP        string `json:"ip"`
			Port      int    `json:"port"`
			FW        string `json:"fw"`
			State     string `json:"state"`
			Dialable  bool   `json:"dialable"`
			Refusal   string `json:"refusal"`
			Name      string `json:"name"`
			Room      string `json:"room"`
			Cable     bool   `json:"cable"`
			Contended bool   `json:"contended"`
			Shared    bool   `json:"shared"`
			Bound     bool   `json:"bound"`
		} `json:"orbs"`
		Refused   []map[string]any `json:"refused"`
		Listening bool             `json:"listening"`
		Error     string           `json:"error"`
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

// Asking again is an outbound action and is therefore token-guarded like every other
// one. Without the guard a page the operator never visited could set the multicast
// rhythm on the hub's own subnet.
func TestBrowseAskRequiresTheLaunchToken(t *testing.T) {
	s := newHub()
	s.token = "test-token"
	// A browser with no sockets would report "no sockets attached" through the
	// guard rather than letting the test see whether the guard ran at all, so the
	// ask is stubbed to a recorder and the token question is the only variable.
	s.browse = &okDiscovery{}
	mux := http.NewServeMux()
	s.routes(mux)
	// No token: refused.
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/api/orbs/ask", nil))
	if rec.Code != http.StatusForbidden {
		t.Errorf("an untokened ask -> %d, want 403", rec.Code)
	}
	// With it: allowed, and the payload is the envelope app.js parses.
	req := httptest.NewRequest(http.MethodPost, "/api/orbs/ask", nil)
	req.Header.Set("X-Orb-Token", "test-token")
	rec = httptest.NewRecorder()
	mux.ServeHTTP(rec, req)
	if rec.Code != 200 {
		t.Errorf("a tokened ask -> %d: %s", rec.Code, rec.Body.String())
	}
}

// A hub whose browser cannot ask anything still serves the list: an unbindable 5353
// degrades the browse, not the product.
func TestAskFailureIsReportedNotFatal(t *testing.T) {
	s := newHub()
	s.browse = &brokenDiscovery{}
	rec := httptest.NewRecorder()
	s.handleBrowseNow(rec, httptest.NewRequest(http.MethodPost, "/api/orbs/ask", nil))
	if rec.Code == 200 && strings.Contains(rec.Body.String(), `"ok":true`) {
		t.Error("a failing Ask reported success")
	}
	s.dmu.Lock()
	asked := s.browseAskErr
	s.dmu.Unlock()
	if asked == "" {
		t.Error("the failure was swallowed: the card would show a healthy list forever")
	}
}

// okDiscovery answers Ask so the guard, not the network, is what the test measures.
type okDiscovery struct{ asks int }

func (d *okDiscovery) Ask() error                      { d.asks++; return nil }
func (d *okDiscovery) Candidates() []orbmdns.Candidate { return nil }
func (d *okDiscovery) Refusals() []orbmdns.Refusal     { return nil }
func (d *okDiscovery) Close() error                    { return nil }

type brokenDiscovery struct{}

func (*brokenDiscovery) Ask() error                      { return errAsk }
func (*brokenDiscovery) Candidates() []orbmdns.Candidate { return nil }
func (*brokenDiscovery) Refusals() []orbmdns.Refusal     { return nil }
func (*brokenDiscovery) Close() error                    { return nil }

var errAsk = errors.New("no route to group")

// --- the builders the tests above need ---------------------------------------------
//
// The packets are hand-encoder-free: internal/orbmdns exposes the announcement
// builder it judges with, so these tests build wire bytes through the same
// transcription of the C codec the fake ships. The alternative — literal hex — would
// pin the hub to a snapshot of the encoder rather than to the encoder.

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

// Asking the LAN a question is an outbound action on the hub's own subnet, so it is
// guarded like the allow rather than like the read: no token, and a page the operator
// never opened sets no multicast rhythm.
func TestBrowseAskRefusesCrossOriginEvenWithTheToken(t *testing.T) {
	s, srv := securedHub(t)
	s.token = "ask-token"
	s.browse = &okDiscovery{}
	origin := "http://" + strings.TrimPrefix(srv.URL, "http://")

	if code, body := postHub(t, srv, "/api/orbs/ask", nil, ""); code != http.StatusForbidden {
		t.Errorf("an untokened ask -> %d, want 403: %s", code, body)
	}
	if code, body := postHub(t, srv, "/api/orbs/ask",
		map[string]string{"X-Orb-Token": "ask-token", "Origin": "http://evil.example"}, ""); code != http.StatusForbidden {
		t.Errorf("a cross-origin tokened ask -> %d, want 403: %s", code, body)
	}
	if code, body := postHub(t, srv, "/api/orbs/ask",
		map[string]string{"X-Orb-Token": "ask-token", "Origin": origin}, ""); code != http.StatusOK {
		t.Fatalf("a tokened same-origin ask -> %d: %s", code, body)
	}
}

// countDiscovery is the discovery a browseLoop test drives: every Ask pings a channel
// so the test observes the query count and the gaps without a socket, and it returns
// the four interface methods a no-op for everything Ask is not.
type countDiscovery struct{ asks chan time.Time }

func (d *countDiscovery) Ask() error                    { d.asks <- time.Now(); return nil }
func (*countDiscovery) Candidates() []orbmdns.Candidate { return nil }
func (*countDiscovery) Refusals() []orbmdns.Refusal     { return nil }
func (*countDiscovery) Close() error                    { return nil }

// browseLoop must fire the whole §6 startup burst — one query per browseBurstDelays
// entry, the first immediate — then hold at the steady interval. The steady ticker is
// 45 s, far outside this test, so anything past the burst that arrives quickly is a bug
// (a burst that loops, or a zero steady interval).
func TestBrowseLoopFiresTheStartupBurst(t *testing.T) {
	saved := browseBurstDelays
	browseBurstDelays = []time.Duration{0, 10 * time.Millisecond, 20 * time.Millisecond, 40 * time.Millisecond}
	defer func() { browseBurstDelays = saved }()

	s := newHub()
	d := &countDiscovery{asks: make(chan time.Time, len(browseBurstDelays)+1)}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	done := make(chan struct{})
	go func() { s.browseLoop(ctx, d); close(done) }()

	// All four burst probes, in order, within a window far shorter than the steady
	// interval. The first must be near-instant (delay 0).
	start := time.Now()
	stamps := make([]time.Time, 0, len(browseBurstDelays))
	for i := range browseBurstDelays {
		select {
		case ts := <-d.asks:
			stamps = append(stamps, ts)
		case <-time.After(time.Second):
			t.Fatalf("burst probe %d never fired", i)
		}
	}
	if stamps[0].Sub(start) > 15*time.Millisecond {
		t.Errorf("first probe waited %v; §6 wants the first query issued immediately", stamps[0].Sub(start))
	}

	// Nothing else within 5× the longest (now-shrunk) steady wait: the burst is exactly
	// its length, then the loop is parked on the real 45 s ticker, not firing again.
	select {
	case <-d.asks:
		t.Fatal("a query arrived after the burst but well inside the steady interval; the ticker or burst cadence is wrong")
	case <-time.After(300 * time.Millisecond):
	}

	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("browseLoop ignored the cancellation")
	}
}

// Cancelling during the burst must stop it before the first query: sleepCtx on the
// zero delay still checks ctx, so a shutdown that races startup issues no stray query.
func TestBrowseLoopHonoursCancelBeforeFirstProbe(t *testing.T) {
	saved := browseBurstDelays
	browseBurstDelays = []time.Duration{time.Hour} // parked: the only probe is a way-off delay
	defer func() { browseBurstDelays = saved }()

	s := newHub()
	d := &countDiscovery{asks: make(chan time.Time, 4)}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { s.browseLoop(ctx, d); close(done) }()
	cancel()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("browseLoop did not stop on a cancellation issued during the burst")
	}
	select {
	case <-d.asks:
		t.Fatal("a query was issued after the burst was cancelled at its first delay")
	default:
	}
}
