package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// The hub binds loopback and holds a flasher. Loopback is not a defence: a page on
// the operator's own machine can POST to 127.0.0.1 without any CORS grant, so before
// these checks one hostile page with no permissions of its own could flash the orb on
// the desk. These tests drive the REAL mux, because a guard wired onto the wrong route
// protects nothing and a unit test of the predicate alone would not notice.

func securedHub(t *testing.T) (*Server, *httptest.Server) {
	t.Helper()
	s := newHub()
	mux := http.NewServeMux()
	s.routes(mux)
	srv := httptest.NewServer(s.log(mux))
	t.Cleanup(srv.Close)
	return s, srv
}

// postReset is the cheap mutating call: it changes which bundle is active, returns
// JSON, and touches no port. A 200 from it means the guard let the request through.
func postReset(t *testing.T, srv *httptest.Server, hdr map[string]string) *http.Response {
	t.Helper()
	req, err := http.NewRequest(http.MethodPost, srv.URL+"/api/firmware/reset", nil)
	if err != nil {
		t.Fatal(err)
	}
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("POST /api/firmware/reset: %v", err)
	}
	return resp
}

// Serving the page is what hands out the credential. If the marker survives, every
// button on the card answers 403 and the hub looks broken rather than guarded, so
// this is checked on the served bytes rather than on a helper's output.
func TestIndexPageCarriesTheLaunchToken(t *testing.T) {
	s, srv := securedHub(t)
	resp, err := http.Get(srv.URL + "/")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	body := readAll(t, resp)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("GET / status = %d", resp.StatusCode)
	}
	if strings.Contains(body, indexMarker) {
		t.Error("the token placeholder survived into the served page: no client request will carry a token")
	}
	// The whole contract in one line: the client reads window.__ORB_TOKEN__, so the
	// IDENTIFIER must survive while its VALUE is replaced. A replace-all of the bare
	// name produced `window.<token> = "<token>"`, which serves a token the client can
	// never see and leaves every button answering 403 — green tests, dead UI.
	if !strings.Contains(body, "window.__ORB_TOKEN__ = \""+s.token+"\"") {
		t.Errorf("served page does not hand the client the token on the property app.js reads; got:\n%s",
			tokenLine(body))
	}
	if !strings.Contains(body, s.token) {
		t.Error("the page does not carry this launch's token")
	}
	sc := resp.Header.Get("Set-Cookie")
	if !strings.Contains(sc, tokenCookie+"="+s.token) {
		t.Errorf("Set-Cookie = %q, want the token cookie", sc)
	}
	if !strings.Contains(sc, "HttpOnly") || !strings.Contains(sc, "SameSite=Strict") {
		t.Errorf("cookie attributes = %q, want HttpOnly and SameSite=Strict so a cross-site POST cannot carry it", sc)
	}
	if cc := resp.Header.Get("Cache-Control"); !strings.Contains(cc, "no-store") {
		t.Errorf("Cache-Control = %q; a cached page would outlive the token it was minted for", cc)
	}
}

// The guard's whole claim. The two hostile cases are the ones that were open before:
// a cross-site POST carrying nothing, and one carrying a plausible Origin but no
// credential this hub ever handed out.
func TestMutatingRoutesRequireTokenAndSameOrigin(t *testing.T) {
	s, srv := securedHub(t)
	same := "http://" + strings.TrimPrefix(srv.URL, "http://")

	cases := []struct {
		name string
		hdr  map[string]string
		want int
	}{
		{"bare POST, no credential", nil, http.StatusForbidden},
		{"same-origin Origin but no token", map[string]string{"Origin": same}, http.StatusForbidden},
		{"right token, hostile Origin", map[string]string{"X-Orb-Token": s.token, "Origin": "http://evil.example"}, http.StatusForbidden},
		{"right token, Sec-Fetch-Site cross-site", map[string]string{"X-Orb-Token": s.token, "Sec-Fetch-Site": "cross-site"}, http.StatusForbidden},
		{"right token, same-origin", map[string]string{"X-Orb-Token": s.token, "Origin": same}, http.StatusOK},
		{"cookie only, same-origin", map[string]string{"Cookie": tokenCookie + "=" + s.token, "Origin": same}, http.StatusOK},
		{"wrong token", map[string]string{"X-Orb-Token": "not-this-launchs-token", "Origin": same}, http.StatusForbidden},
	}
	for _, tc := range cases {
		resp := postReset(t, srv, tc.hdr)
		if resp.StatusCode != tc.want {
			t.Errorf("%s: status = %d, want %d", tc.name, resp.StatusCode, tc.want)
		}
		if tc.want == http.StatusForbidden {
			var env map[string]any
			body := readAll(t, resp)
			if err := json.Unmarshal([]byte(body), &env); err != nil {
				t.Errorf("%s: refusal is not the {ok,error} envelope the client parses: %v (%s)", tc.name, err, body)
			} else if env["ok"] != false || env["error"] == "" {
				t.Errorf("%s: refusal says nothing usable: %v", tc.name, env)
			}
		}
		resp.Body.Close()
	}
}

// A guard fitted to /api/flash and forgotten on /api/provision is the failure that
// matters, so each mutating route is probed rather than one representing the rest.
func TestEveryMutatingRouteRefusesAnUncredentialedPOST(t *testing.T) {
	s, srv := securedHub(t)
	routes := []string{
		"/api/ports/probe", "/api/firmware", "/api/firmware/reset", "/api/flash",
		"/api/flash/cancel", "/api/watch", "/api/provision", "/api/orb/allow",
	}
	for _, r := range routes {
		req, _ := http.NewRequest(http.MethodPost, srv.URL+r, strings.NewReader(`{"port":"/dev/ttyACM0","ip":"192.0.2.9"}`))
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("Origin", "http://attack.example")
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatalf("%s: %v", r, err)
		}
		body := readAll(t, resp)
		resp.Body.Close()
		if resp.StatusCode != http.StatusForbidden {
			t.Errorf("%s accepted a cross-origin uncredentialed POST with %d: %s", r, resp.StatusCode, body)
		}
		if !strings.Contains(body, "token") && !strings.Contains(body, "origin") {
			t.Errorf("%s refused without naming the token or the origin: %s", r, body)
		}
		// The guard must run BEFORE the handler: a refused /api/flash that reached
		// the port claim would have reserved a cable for a request with no business
		// asking for one.
		if live := s.ops.running(); len(live) != 0 {
			t.Errorf("%s claimed a port operation despite being refused: %+v", r, live)
		}
	}
}

// The proxy is a mutating surface: half the device API behind it writes to the orb.
func TestProxyRequiresTheToken(t *testing.T) {
	s, srv := securedHub(t)
	s.grants.learn("192.0.2.7")
	for _, m := range []string{http.MethodGet, http.MethodPost} {
		req, _ := http.NewRequest(m, srv.URL+"/orb/192.0.2.7/api/v1/status", nil)
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		body := readAll(t, resp)
		resp.Body.Close()
		if resp.StatusCode != http.StatusForbidden || !strings.Contains(body, tokenHeader) {
			t.Errorf("%s /orb/ with no token: status = %d body %s, want 403 naming %s", m, resp.StatusCode, body, tokenHeader)
		}
	}
}

// Reads stay readable: the hub's own page polls them on a timer, and a guard that
// swells to GET turns the UI into a spinner.
func TestReadOnlyRoutesStayOpen(t *testing.T) {
	_, srv := securedHub(t)
	for _, r := range []string{"/api/status", "/api/ports", "/api/build", "/api/ops", "/web/app.js"} {
		resp, err := http.Get(srv.URL + r)
		if err != nil {
			t.Fatalf("%s: %v", r, err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Errorf("GET %s = %d, want 200 with no credential", r, resp.StatusCode)
		}
	}
}

// Two launches must not authorise each other, or a second hub on the box would
// accept the first one's page.
func TestLaunchTokensArePerProcess(t *testing.T) {
	a, b := newLaunchToken(), newLaunchToken()
	if a == b {
		t.Fatal("two launches minted the same token")
	}
	if len(a) < 32 {
		t.Errorf("token entropy = %d hex chars, want at least 32", len(a))
	}
	_, srv := securedHub(t)
	resp := postReset(t, srv, map[string]string{"X-Orb-Token": b})
	resp.Body.Close()
	if resp.StatusCode != http.StatusForbidden {
		t.Errorf("another launch's token was accepted with %d", resp.StatusCode)
	}
}

// A Server assembled without a token must refuse everything rather than wave
// requests through: fail closed is the only safe default for an unconfigured guard.
func TestGuardFailsClosedWithoutAToken(t *testing.T) {
	s := newHub()
	s.token = ""
	mux := http.NewServeMux()
	s.routes(mux)
	srv := httptest.NewServer(mux)
	defer srv.Close()
	req, _ := http.NewRequest(http.MethodPost, srv.URL+"/api/firmware/reset", nil)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusForbidden {
		t.Errorf("status = %d, want 403 from a hub with no token minted", resp.StatusCode)
	}
}

// The bind-address rule is stated as a predicate so the refusal in main() and the
// test agree on what "loopback" means, including the spellings that are easy to get
// wrong: an empty host and a name.
func TestIsLoopbackHost(t *testing.T) {
	cases := []struct {
		addr string
		want bool
	}{
		{"127.0.0.1:3779", true},
		{"127.0.0.53:3779", true},
		{"[::1]:3779", true},
		{"localhost:3779", true},
		{":3779", false},                  // every interface
		{"0.0.0.0:3779", false},           // every interface, spelled
		{"192.168.2.9:3779", false},       // a LAN
		{"bench.example.com:3779", false}, // could resolve anywhere
		{"garbage", false},
	}
	for _, tc := range cases {
		if got := isLoopbackHost(tc.addr); got != tc.want {
			t.Errorf("isLoopbackHost(%q) = %v, want %v", tc.addr, got, tc.want)
		}
	}
}

// Only an IP literal enters the grant set. A hostname admitted here would be
// resolved by the proxy's dial, which is the behaviour the gate exists to exclude.
func TestGrantsAcceptOnlyIPLiterals(t *testing.T) {
	g := newIPGrants(ipGrantTTL)
	for _, ok := range []string{"192.168.2.41", "10.0.0.1", "169.254.169.254"} {
		g.learn(ok)
		if !g.allowed(ok) {
			t.Errorf("%s was learned and then refused", ok)
		}
	}
	for _, bad := range []string{"orb.local", "http://10.0.0.1", "10.0.0.1:80", "10.0.0.1/../", "::1", "", "   "} {
		g.learn(bad)
		if g.allowed(bad) {
			t.Errorf("%q entered the grant set; only IP literals belong there", bad)
		}
	}
	// An expired grant is no grant: a hub left running for days must not keep
	// dialing an address it learned this morning indefinitely.
	g2 := newIPGrants(ipGrantTTL)
	g2.learn("10.1.1.1")
	g2.mu.Lock()
	stored := g2.byIP["10.1.1.1"]
	stored.at = stored.at.Add(-2 * ipGrantTTL)
	stored.since = stored.at
	g2.byIP["10.1.1.1"] = stored
	g2.mu.Unlock()
	if g2.allowed("10.1.1.1") {
		t.Error("a grant older than the TTL still authorized the dial")
	}
}

// A nil registry must refuse, so a Server assembled by hand in a test or a future
// handler cannot be an open proxy by omission.
func TestNilGrantsRefuse(t *testing.T) {
	var g *ipGrants
	if g.allowed("10.0.0.1") {
		t.Error("an unconfigured hub authorized a dial")
	}
	g.learn("10.0.0.1") // must not panic
	g.revoke("10.0.0.1")
	if g.list() != nil {
		t.Error("nil registry listed grants")
	}
}

func readAll(t *testing.T, r *http.Response) string {
	t.Helper()
	var b strings.Builder
	buf := make([]byte, 4096)
	for {
		n, err := r.Body.Read(buf)
		b.Write(buf[:n])
		if err != nil {
			return b.String()
		}
	}
}

// tokenLine pulls the injected script line out of a served page, so a failure prints
// the one line that is wrong rather than 150 lines of markup. It matches on the
// `<script>window.` prefix too, because the bug it exists to catch is the injection
// eating the identifier — which leaves no `__ORB_TOKEN__` anywhere to search for.
func tokenLine(page string) string {
	for _, l := range strings.Split(page, "\n") {
		t := strings.TrimSpace(l)
		if strings.HasPrefix(t, "<script>window.") || strings.Contains(t, "__ORB_TOKEN__") {
			return t
		}
	}
	return "(no token line in the page at all)"
}
