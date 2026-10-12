package main

import (
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// The /orb proxy is the browser's only route to an orb, and the UI parses every
// reply as the device API's JSON. A hub-side failure (bad path, unreachable
// orb) must therefore be a JSON body too — the old plaintext http.Error surfaced
// only as a cryptic "is not valid JSON" in the browser.

// proxyWith returns a hub whose dial client points at backend, plus the one learned
// address the proxy will accept. Every case has to say what it learned, because an
// unlearned address is refused before the dial — that is the point. The backend's
// port goes into Server.devicePort rather than the URL: the browser never picks a
// port, which is what keeps this from becoming a port scanner on the orb's host.
func proxyWith(t *testing.T, backend *httptest.Server) *Server {
	s := &Server{device: orbnet.NewClient(2 * time.Second), grants: newIPGrants(ipGrantTTL), ops: newOpRegistry()}
	if backend != nil {
		host, port, err := net.SplitHostPort(strings.TrimPrefix(backend.URL, "http://"))
		if err != nil {
			t.Fatalf("backend URL %q: %v", backend.URL, err)
		}
		if host != testOrbIP {
			t.Fatalf("test backend must sit on %s so the learned address matches; got %s", testOrbIP, host)
		}
		s.grants.learn(host)
		c := orbnet.NewClient(2 * time.Second)
		c.DevicePort = port
		s.device = c
	}
	return s
}

// testOrbIP is the address every proxy test "learns": httptest binds loopback, and
// the hub only ever dials an address it was handed, so the test has to hand it one.
const testOrbIP = "127.0.0.1"

func TestProxyBadPathIsJSON(t *testing.T) {
	s := proxyWith(t, nil)
	rr := httptest.NewRecorder()
	// Host is not a bare IPv4, so it is rejected before any network call.
	req := httptest.NewRequest(http.MethodGet, "/orb/10.0.0/2/api/v1/wifi/scan", nil)
	s.proxyOrb(rr, req)

	if rr.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rr.Code)
	}
	var body map[string]string
	if err := json.Unmarshal(rr.Body.Bytes(), &body); err != nil {
		t.Fatalf("error body is not JSON (%q): %v", rr.Body.String(), err)
	}
	if body["error"] == "" {
		t.Errorf("error field empty; body = %q", rr.Body.String())
	}
}

func TestProxyUnreachableIsJSON(t *testing.T) {
	// 192.0.2.1 is TEST-NET-1 (RFC 5737), never routable, so the dial fails fast
	// and deterministically — exactly the wired-hub-can't-reach-the-AP case.
	s := proxyWith(t, nil)
	s.grants.learn("192.0.2.1") // learned, so the request reaches the dial at all
	rr := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/orb/192.0.2.1/api/v1/wifi/scan", nil)
	s.proxyOrb(rr, req)

	if rr.Code != http.StatusBadGateway {
		t.Fatalf("status = %d, want 502", rr.Code)
	}
	if ct := rr.Header().Get("Content-Type"); ct != "application/json" {
		t.Errorf("Content-Type = %q, want application/json", ct)
	}
	var body map[string]string
	if err := json.Unmarshal(rr.Body.Bytes(), &body); err != nil {
		t.Fatalf("error body is not JSON (%q): %v", rr.Body.String(), err)
	}
	if !strings.HasPrefix(body["error"], "orb unreachable:") {
		t.Errorf("error = %q, want it to start with 'orb unreachable:'", body["error"])
	}
}

// The old suite never proved a good forward forwards: with only refusal cases, a
// proxy that answered 200-empty for everything would have passed. This is the round
// trip — device body, method, path and query must arrive intact both ways.
func TestProxyForwardsAGoodRequest(t *testing.T) {
	var gotPath, gotMethod, gotQuery, gotBody string
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotPath, gotMethod, gotQuery = r.URL.Path, r.Method, r.URL.RawQuery
		b, _ := io.ReadAll(r.Body)
		gotBody = string(b)
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"fw":"476b13c"}`))
	}))
	defer backend.Close()

	s := proxyWith(t, backend)
	rr := httptest.NewRecorder()
	s.proxyOrb(rr, httptest.NewRequest(http.MethodPost,
		"/orb/"+testOrbIP+"/api/v1/config?src=ui", strings.NewReader(`{"tz":"EST5EDT"}`)))

	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d body %s, want 200", rr.Code, rr.Body)
	}
	if rr.Body.String() != `{"fw":"476b13c"}` {
		t.Errorf("forwarded body = %q, want the device's body verbatim", rr.Body.String())
	}
	if gotPath != "/api/v1/config" || gotMethod != http.MethodPost {
		t.Errorf("device saw %s %s, want POST /api/v1/config", gotMethod, gotPath)
	}
	if gotQuery != "src=ui" {
		t.Errorf("device saw query %q, want src=ui — /wifi/scan?fresh=1 depends on it", gotQuery)
	}
	if gotBody != `{"tz":"EST5EDT"}` {
		t.Errorf("device saw body %q, want the browser's body forwarded", gotBody)
	}
	if ct := rr.Header().Get("Content-Type"); ct != "application/json" {
		t.Errorf("Content-Type = %q, want the device's mirrored", ct)
	}
}

// The proxy is the hub's one route off the box, so it dials ONLY addresses this hub
// learned over USB or the operator handed it. Before this gate, any page the browser
// visited could have the hub knock on a router, a neighbour's machine, or a cloud
// metadata address and read the answer back as a same-origin reply.
func TestProxyRefusesAnAddressItNeverLearned(t *testing.T) {
	for _, ip := range []string{"192.168.1.1", "10.0.0.5", "169.254.169.254", "127.0.0.1"} {
		s := proxyWith(t, nil) // grants empty: nothing was learned over any cable
		rr := httptest.NewRecorder()
		s.proxyOrb(rr, httptest.NewRequest(http.MethodGet, "/orb/"+ip+"/api/v1/status", nil))
		if rr.Code != http.StatusForbidden {
			t.Errorf("%s: status = %d, want 403 from the learned-address gate", ip, rr.Code)
		}
		if !strings.Contains(rr.Body.String(), "not an orb address") {
			t.Errorf("%s: body = %s, want it to explain the address is not learned", ip, rr.Body)
		}
	}
}

// A client cannot choose the port the hub dials. The /orb/ path takes an IPv4
// literal and nothing else: 127.0.0.1:6379 is not an address this gate accepts, so
// the learned-address allowlist cannot be turned into a port scanner on the orb's
// host by way of the URL.
func TestProxyRefusesAClientChosenPort(t *testing.T) {
	s := proxyWith(t, nil)
	s.grants.learn(testOrbIP)
	for _, u := range []string{
		"/orb/127.0.0.1:1/api/v1/status",
		"/orb/" + testOrbIP + ":80/api/v1/status",
		"/orb/localhost/api/v1/status",
		"/orb/0x7f000001/api/v1/status",
	} {
		rr := httptest.NewRecorder()
		s.proxyOrb(rr, httptest.NewRequest(http.MethodGet, u, nil))
		if rr.Code == http.StatusOK {
			t.Errorf("%s was forwarded; the hub must dial only the port it chooses", u)
		}
	}
}

// The allowlist mirrors the firmware's own URI table. What matters is the route the
// device serves but the hub refuses to expose, and the methods no route takes.
func TestProxyRouteAndMethodAllowlist(t *testing.T) {
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{}`))
	}))
	defer backend.Close()
	addr := testOrbIP

	cases := []struct {
		method, path string
		want         int
	}{
		{http.MethodGet, "/api/v1/status", http.StatusOK},
		{http.MethodGet, "/api/v1/sd", http.StatusOK},
		{http.MethodGet, "/api/v1/wifi/scan", http.StatusOK},
		{http.MethodPost, "/api/v1/sd/install", http.StatusOK},
		{http.MethodPost, "/api/v1/reboot", http.StatusOK},
		{http.MethodPost, "/api/v1/config", http.StatusOK},
		// The device's raw-image OTA endpoint. The hub's rule is that firmware comes
		// from a validated bundle and the whole manifest is always written.
		{http.MethodPost, "/api/v1/ota", http.StatusForbidden},
		// Not a route the firmware registers, in any spelling.
		{http.MethodGet, "/api/v1/secrets", http.StatusForbidden},
		{http.MethodGet, "/api/v1/sd/install", http.StatusForbidden},
		// Methods the firmware registers for none of these.
		{http.MethodDelete, "/api/v1/status", http.StatusForbidden},
		{http.MethodPut, "/api/v1/config", http.StatusForbidden},
		{http.MethodOptions, "/api/v1/status", http.StatusForbidden},
	}
	for _, tc := range cases {
		s := proxyWith(t, backend)
		rr := httptest.NewRecorder()
		s.proxyOrb(rr, httptest.NewRequest(tc.method, "/orb/"+addr+tc.path, nil))
		if rr.Code != tc.want {
			t.Errorf("%s %s: status = %d, want %d (%s)", tc.method, tc.path, rr.Code, tc.want, rr.Body.String())
		}
	}
}

// Device response headers must not ride through to the browser: only the content
// type is mirrored.
func TestProxyMirrorsOnlyContentType(t *testing.T) {
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Server", "esp-httpd/xyz")
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{}`))
	}))
	defer backend.Close()
	s := proxyWith(t, backend)
	rr := httptest.NewRecorder()
	s.proxyOrb(rr, httptest.NewRequest(http.MethodGet, "/orb/"+testOrbIP+"/api/v1/status", nil))
	if got := rr.Header().Get("Access-Control-Allow-Origin"); got != "" {
		t.Errorf("device CORS header leaked to the browser: %q", got)
	}
	if got := rr.Header().Get("Server"); got != "" {
		t.Errorf("device Server header leaked: %q", got)
	}
	if got := rr.Header().Get("Content-Type"); got != "application/json" {
		t.Errorf("Content-Type = %q, want application/json", got)
	}
}
