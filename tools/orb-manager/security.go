package main

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/url"
	"sort"
	"strings"
	"sync"
	"time"
)

// The local HTTP surface: who may ask the hub to act, and where the hub is willing
// to dial on their behalf.
//
// Binding to 127.0.0.1 is NOT a trust boundary. Any page a browser visits can POST
// to http://127.0.0.1:3779/api/flash, because a cross-origin POST needs no CORS
// grant in order to be *sent* — only to read the reply. Before these checks, a
// hostile page with no permissions of its own could flash the orb sitting on the
// operator's desk, wipe its NVS, or drive /api/v1/sd/delete against an orb on the
// LAN through the hub's own proxy. The hub holds a flasher and a cable; that is what
// needs defending, not the operator's identity.

const (
	// tokenHeader is the explicit credential, for an agent or a scripted run.
	tokenHeader = "X-Orb-Token"
	// tokenCookie carries the same value for the browser, so the injected client
	// and the composer page (whose JS is vendor bytes this repo must not edit) both
	// authenticate without either holding the secret in its source.
	tokenCookie = "orb_token"
)

// newLaunchToken mints the per-launch secret. One per process is enough: it only has
// to outlive the browser that got it, and a value changing under an open tab would
// break its own buttons.
func newLaunchToken() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		log.Fatalf("orb-manager: cannot mint a control token: %v", err)
	}
	return hex.EncodeToString(b[:])
}

// setLaunchToken hands the token to the browser by the one channel a cross-site page
// cannot read back: a SameSite=Strict cookie the browser attaches to its own
// same-origin POSTs and withholds from everyone else's. HttpOnly is deliberate — the
// value belongs on the wire, not in whatever script runs in the page.
func setLaunchToken(w http.ResponseWriter, token string) {
	http.SetCookie(w, &http.Cookie{
		Name:     tokenCookie,
		Value:    token,
		Path:     "/",
		HttpOnly: true,
		SameSite: http.SameSiteStrictMode,
	})
}

// requireControl guards a route that changes something: a flash, a provision, an
// erase, a device reboot, a file written into the repo. Two independent checks stand
// between the wire and that work, and both must pass:
//
//  1. The token proves the requester got it from THIS hub on THIS launch, so a page
//     that never loaded from us cannot produce it.
//  2. The origin check refuses a request a browser was manipulated into sending from
//     somewhere else, even one that somehow carries a token.
//
// A request with neither Origin nor Sec-Fetch-Site is not exempt from (1): that is
// the scripted-agent case, and it has to pass the header token.
func (s *Server) requireControl(h http.HandlerFunc) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !s.originOK(r) {
			reject(w, r, http.StatusForbidden,
				"cross-origin request refused: this hub acts only on requests from its own page")
			return
		}
		if !s.tokenOK(r) {
			reject(w, r, http.StatusForbidden,
				"missing or wrong "+tokenHeader+": reload the Orb Manager page to pick up this launch's token, or send the header when scripting the hub")
			return
		}
		h(w, r)
	})
}

// tokenOK constant-time-compares the credential from either channel against the one
// this process minted. An unset token refuses everything rather than failing open:
// only main() sets it, so a Server built without one has no business serving the
// mutating routes.
func (s *Server) tokenOK(r *http.Request) bool {
	got := r.Header.Get(tokenHeader)
	if got == "" {
		if c, err := r.Cookie(tokenCookie); err == nil {
			got = c.Value
		}
	}
	if got == "" || s.token == "" {
		return false
	}
	return subtle.ConstantTimeCompare([]byte(got), []byte(s.token)) == 1
}

// originOK accepts same-origin browser traffic, and clients that send no origin
// information at all (curl, an agent, a test), leaving the token to do that work.
func (s *Server) originOK(r *http.Request) bool {
	if site := strings.ToLower(r.Header.Get("Sec-Fetch-Site")); site != "" {
		switch site {
		case "same-origin", "same-site":
			return true
		default: // cross-site, cross-origin, or none: no mutating route is a navigation target in this UI
			return false
		}
	}
	for _, h := range []string{"Origin", "Referer"} {
		v := r.Header.Get(h)
		if v == "" {
			continue
		}
		u, err := url.Parse(v)
		if err != nil || u.Host == "" {
			return false
		}
		if !strings.EqualFold(u.Host, r.Host) {
			return false
		}
	}
	return true
}

// reject answers a refusal in the envelope every client here already parses, so a
// blocked flash reports a reason instead of a bare 403 that app.js would render as
// "HTTP 403" with no clue what was refused.
func reject(w http.ResponseWriter, r *http.Request, code int, format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	log.Printf("orb-manager: refused %s %s from %s: %s", r.Method, r.URL.Path, r.RemoteAddr, msg)
	if strings.HasPrefix(r.URL.Path, "/api/") || strings.HasPrefix(r.URL.Path, "/orb/") {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(code)
		_ = json.NewEncoder(w).Encode(map[string]any{"ok": false, "error": msg})
		return
	}
	http.Error(w, msg, code)
}

// --- where the hub will dial ----------------------------------------------------

// ipGrants is the set of orb addresses the hub has reason to talk to. The proxy is
// the only route that reaches off the box, so it is limited to these: an address
// learned over the USB cable, one the hub was asked to dial for a status check, or
// one the operator committed to in the Manage card. Without it the hub is an open
// SSRF pivot out of 127.0.0.1: any LAN host, and any cloud metadata address,
// dialable by any page the browser happens to visit.
type ipGrants struct {
	mu   sync.Mutex
	byIP map[string]time.Time
	ttl  time.Duration
}

func newIPGrants(ttl time.Duration) *ipGrants {
	return &ipGrants{byIP: map[string]time.Time{}, ttl: ttl}
}

// learn records an address the hub has legitimate business with. Only an IP literal
// is admissible: a hostname would be resolved by the proxy's dial, which is exactly
// the behaviour this set exists to exclude.
func (g *ipGrants) learn(ip string) {
	if g == nil || net.ParseIP(ip) == nil || !strings.Contains(ip, ".") {
		return
	}
	g.mu.Lock()
	defer g.mu.Unlock()
	g.byIP[ip] = time.Now()
}

func (g *ipGrants) allowed(ip string) bool {
	// A nil registry means the Server was assembled without one, and the safe
	// answer to "may I dial this" from an uninitialised hub is no. Failing open
	// here would make every hand-built Server an open proxy.
	if g == nil {
		return false
	}
	g.mu.Lock()
	defer g.mu.Unlock()
	at, ok := g.byIP[ip]
	if !ok {
		return false
	}
	if g.ttl > 0 && time.Since(at) > g.ttl {
		delete(g.byIP, ip)
		return false
	}
	return true
}

func (g *ipGrants) revoke(ip string) {
	if g == nil {
		return
	}
	g.mu.Lock()
	defer g.mu.Unlock()
	delete(g.byIP, ip)
}

func (g *ipGrants) list() []string {
	if g == nil {
		return nil
	}
	g.mu.Lock()
	defer g.mu.Unlock()
	out := make([]string, 0, len(g.byIP))
	for ip := range g.byIP {
		out = append(out, ip)
	}
	sort.Strings(out)
	return out
}

// --- the device route allowlist ---------------------------------------------------

// deviceRoutes mirrors the URI table the firmware registers — main/net/orb_net.c and
// main/theme_app.c — and nothing else. It answers the way "the hub will dial that IP
// for you" turns into "the hub will dial that IP and run whatever path you name
// there". Two device routes are deliberately ABSENT:
//
//   - /api/v1/ota: it takes a raw image and writes the inactive slot. The hub's rule
//     is that firmware comes from a validated bundle and the whole manifest is always
//     written, so handing the device's bare-app path to a browser page could strand
//     an orb on a partial app. Flash from the Onboard card instead.
//   - the captive-portal probes (/generate_204, /hotspot-detect.html,
//     /connect/test.php): the UI never asks for them, and an allowlist that admits
//     routes nobody uses is a denylist with extra steps.
var deviceRoutes = map[string][]string{
	"/api/v1/status":     {http.MethodGet},
	"/api/v1/screen":     {http.MethodGet},
	"/api/v1/sd":         {http.MethodGet},
	"/themes":            {http.MethodGet},
	"/timezone":          {http.MethodGet},
	"/api/v1/config":     {http.MethodGet, http.MethodPost},
	"/api/v1/reboot":     {http.MethodPost},
	"/api/v1/wifi/scan":  {http.MethodGet},
	"/api/v1/wifi/join":  {http.MethodPost},
	"/api/v1/wifi/ap":    {http.MethodPost},
	"/api/v1/sd/install": {http.MethodPost},
	"/api/v1/sd/default": {http.MethodPost},
	"/api/v1/sd/delete":  {http.MethodPost},
	"/api/v1/sd/theme":   {http.MethodPost},
	"/api/v1/sd/rescan":  {http.MethodPost},
}

// Byte caps for one proxied exchange. The orb's httpd is real and sometimes slow, so
// the dial timeout stays orbnet's business; these cap BYTES, which is what stops a
// route that streams from turning the hub into an unbounded proxy for another host.
const (
	proxyResponseCap = 8 << 20 // device body copied back to the browser
	proxyRequestCap  = 4 << 20 // browser body forwarded to the device
	ipGrantTTL       = 6 * time.Hour
)

// routeAllowed reports whether path+method is a device route the hub will forward.
func routeAllowed(path, method string) bool {
	methods, ok := deviceRoutes[path]
	if !ok {
		return false
	}
	for _, m := range methods {
		if m == method {
			return true
		}
	}
	return false
}

// --- bind address ---------------------------------------------------------------

// isLoopbackHost reports whether addr binds to this machine only. An empty host
// (":3779") binds every interface and is therefore NOT loopback; neither is a
// hostname, which the resolver may point anywhere.
func isLoopbackHost(addr string) bool {
	host := addr
	if h, _, err := net.SplitHostPort(addr); err == nil {
		host = h
	}
	if host == "" {
		return false
	}
	if strings.EqualFold(host, "localhost") {
		return true
	}
	ip := net.ParseIP(host)
	if ip == nil {
		return false
	}
	return ip.IsLoopback()
}

// handleAllowIP registers an orb address the operator just committed to in the
// Manage card, which is what lets /orb/ dial it. It is the deliberate alternative to
// the hub ever guessing: the operator names the address, the hub records it, and the
// proxy dials only recorded ones. Rejecting anything that is not an IPv4 literal is
// the point — a hostname here would make the proxy a resolver-then-dial oracle for
// whatever a page puts in that box.
func (s *Server) handleAllowIP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		IP string `json:"ip"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		s.jsonErr(w, "bad request: "+err.Error())
		return
	}
	ip := strings.TrimSpace(req.IP)
	if net.ParseIP(ip) == nil || strings.Contains(ip, ":") {
		s.jsonErr(w, "not an IPv4 address: "+ip)
		return
	}
	s.grants.learn(ip)
	s.setOrbIP(ip)
	s.saveState()
	s.jsonOK(w, map[string]any{"ip": ip, "allowed": true})
}

// handleForgetIP drops a remembered address, so an operator can take an orb out of
// the dialable set without restarting the hub.
func (s *Server) handleForgetIP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		IP string `json:"ip"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		s.jsonErr(w, "bad request: "+err.Error())
		return
	}
	ip := strings.TrimSpace(req.IP)
	s.grants.revoke(ip)
	s.saveState()
	s.jsonOK(w, map[string]any{"ip": ip, "allowed": false})
}

// handleRememberedIP answers the Manage card's pre-fill: the address this hub (or a
// previous run of it) last had an orb at.
func (s *Server) handleRememberedIP(w http.ResponseWriter, r *http.Request) {
	s.jsonOK(w, map[string]any{
		"ip":          s.currentOrbIP(),
		"learned":     s.grants.list(),
		"stateError":  errText(s.stateErr),
		"stateFile":   s.statePath,
		"deviceReady": s.device.Port(),
	})
}

func errText(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}

// setOrbIP records the address the onboarding flow last learned, for /api/status's
// default target. Mirrors are the UI's convenience, not a security decision: the
// proxy consults grants, never this field.
func (s *Server) setOrbIP(ip string) {
	if ip = strings.TrimSpace(ip); ip == "" {
		return
	}
	s.imu.Lock()
	s.orbIP = ip
	s.imu.Unlock()
}
