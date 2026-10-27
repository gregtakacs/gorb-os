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

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
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
//
// A grant may also carry the MAC it was committed FOR. That is item 6's cross-lock:
// "dial 192.168.2.31" and "dial the orb whose chip is 44bd8d782618" are one request
// when they agree and a security hole when they do not, because the address is a
// lease from a DHCP pool and the MAC is the board. A grant with no MAC is the older,
// weaker kind — the operator typed an address and the hub cannot cross-check it — and
// it is labelled as such on the card rather than being silently upgraded.
type ipGrants struct {
	mu      sync.Mutex
	byIP    map[string]grant
	ttl     time.Duration
	ceiling time.Duration
}

// grant is one remembered address. at is what the TTL measures from, and since is
// when the commitment first happened, which the renewal cannot move: an orb that
// announces forever must still be re-confirmed by a human eventually.
type grant struct {
	at    time.Time
	since time.Time
	mac   string // canonical 12-hex, or "" for a grant nobody cross-locked
}

func newIPGrants(ttl time.Duration) *ipGrants {
	return &ipGrants{byIP: map[string]grant{}, ttl: ttl, ceiling: grantCeiling}
}

// learn records an address the hub has legitimate business with, with no board
// attached to it.
func (g *ipGrants) learn(ip string) { g.learnAt(ip, "") }

// learnAs records an address AND the board the operator meant to be there. The MAC is
// canonicalized or dropped: a garbage MAC must not be stored as a binding, because
// then nothing at that address could ever match it and the grant becomes undialable
// forever. Losing the binding is the safe failure; the address itself still works.
func (g *ipGrants) learnAs(ip, mac string) { g.learnAt(ip, mac) }

func (g *ipGrants) learnAt(ip, mac string) {
	if g == nil || net.ParseIP(ip) == nil || !strings.Contains(ip, ".") {
		return
	}
	c, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		c = ""
	}
	g.mu.Lock()
	defer g.mu.Unlock()
	now := time.Now()
	prev, had := g.byIP[ip]
	gr := grant{at: now, since: now, mac: c}
	if had {
		gr.since = prev.since
	}
	g.byIP[ip] = gr
}

// bound names the board a grant was committed for, or "" for an unbound one.
func (g *ipGrants) bound(ip string) string {
	if g == nil {
		return ""
	}
	g.mu.Lock()
	defer g.mu.Unlock()
	return g.byIP[ip].mac
}

// renew extends a grant because the board it was committed for is still announcing
// itself at it. That is the whole of "liveness comes from announcements": an orb the
// operator pointed at once does not fall off the hub mid-session. Only the committed
// MAC renews its own grant, and only while the grant is still live: an mDNS MAC is
// broadcast unsigned, so a repeat of it must not resurrect a decision a human already
// let expire, or whoever seized the address could restore the dial by replaying it.
func (g *ipGrants) renew(ip, mac string) bool {
	if g == nil {
		return false
	}
	c, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		return false
	}
	g.mu.Lock()
	defer g.mu.Unlock()
	gr, ok := g.byIP[ip]
	if !ok || gr.mac == "" || gr.mac != c {
		return false
	}
	if g.ttl > 0 && time.Since(gr.at) > g.ttl {
		delete(g.byIP, ip)
		return false
	}
	if g.ceiling > 0 && !gr.since.IsZero() && time.Since(gr.since) > g.ceiling {
		delete(g.byIP, ip)
		return false
	}
	gr.at = time.Now()
	g.byIP[ip] = gr
	return true
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
	gr, ok := g.byIP[ip]
	if !ok {
		return false
	}
	if g.ttl > 0 && time.Since(gr.at) > g.ttl {
		delete(g.byIP, ip)
		return false
	}
	if g.ceiling > 0 && !gr.since.IsZero() && time.Since(gr.since) > g.ceiling {
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
	"/api/v1/status": {http.MethodGet},
	"/api/v1/screen": {http.MethodGet},
	"/api/v1/sd":     {http.MethodGet},
	// The retained log tail, plain text. Read-only and guarded on the device by the
	// same token as /screen; the hub's diagnose flow reads it after any failed call,
	// so a hub that cannot dial it makes the firmware's own witness unreachable.
	"/api/v1/log":    {http.MethodGet},
	"/themes":        {http.MethodGet},
	"/timezone":      {http.MethodGet},
	"/api/v1/config": {http.MethodGet, http.MethodPost},
	// The device owns its name, so the hub's rename button has to be able to put it
	// there. POST only: the value is already on /api/v1/status, and a GET door on this
	// path would be a second answer to the same question that could disagree with the
	// first. Refused before it is ever dialed if the name will not pass the device's
	// own rules — see orbfleet.CheckNameASCII.
	"/api/v1/name":   {http.MethodPost},
	"/api/v1/reboot": {http.MethodPost},
	// Recovery, not a reboot: arms D2's one-shot intent and comes back in its SoftAP with
	// the stored credential intact. Allowlisted because the hub's Manage card has to be
	// able to reach an orb it can no longer configure any other way — a wrong SSID or a
	// dead router leaves a credentialed orb connected to nothing, and every other verb on
	// this list needs that connection. It is one boot's worth of damage at worst: the D4
	// sojourn bounds the session and the boot branch consumes the flag on read.
	"/api/v1/reprov":    {http.MethodPost},
	"/api/v1/wifi/scan": {http.MethodGet},
	"/api/v1/wifi/join": {http.MethodPost},
	"/api/v1/wifi/ap":   {http.MethodPost},
	// The mode door, allowlisted because it is the ONLY exit from a state the hub can
	// put an orb into. AP-isolation is persistent: the orb never associates, so it has
	// no lease, no LAN address and no mDNS announcement — the single address that
	// answers is its own 192.168.4.1. A hub that could drive /wifi/ap but not /wifi/mode
	// could open an isolation it could never close, which is a trap and not a feature.
	// POST only; the current mode already reads off /api/v1/status.
	"/api/v1/wifi/mode": {http.MethodPost},
	// h_fb: the one-shot frame capture over the PSRAM mirror, same gate as /api/v1/screen.
	// It reads pixels and changes nothing. P6 removed the FORMAT 2 install door, and
	// Migration 1 removed the flip and default doors with the A/B pair: /api/v1/sd/bundle
	// is now the only theme write verb, and delete stays the one unqueued outcome.
	"/api/v1/fb": {http.MethodGet},
	// h_sd_preview: one theme's preview plane, read-only, one card file, no verb
	// beside it that writes. The Theme card's thumbnails dial it through
	// /api/theme/preview, which is the only place the hub learns of it.
	"/api/v1/sd/preview": {http.MethodGet},
	"/api/v1/sd/delete":  {http.MethodPost},
	"/api/v1/sd/theme":   {http.MethodPost},
	// Migration 1's store door: the one press that puts a card theme on the glass.
	// It queues, acks reboot:automatic, and reboots onto the new face when the copy
	// verifies — the single store verb after the flip/default pair was retired with
	// the A/B choice, each pinned by the firmware comment beside its handler.
	"/api/v1/sd/bundle": {http.MethodPost},
	"/api/v1/sd/rescan": {http.MethodPost},
	// P4's card-app verbs, added because the merge that landed them in firmware left
	// them out of this map: a device feature the hub cannot dial is a feature nobody
	// can manage. None of the five reboots, so none inherits the arm/disarm gate that
	// /api/v1/menu will need (plan 5.1 decision 1) — install, enable and uninstall all
	// answer for the running image and leave the next boot to settle it on its own.
	// sd/app is the upload half (browser -> /sd staging) and apps/install is the
	// commit half, the same two-step the theme upload uses, so the hub never streams a
	// container straight into the apps partition.
	"/api/v1/apps": {http.MethodGet},
	// D37: an installed app's EDITOR region, streamed off the card. Read-only, and
	// the only device door that reaches bytes the firmware never installs or runs
	// (§5.2), which is why it takes GET alone. It addresses a container by card
	// stem, the same handle apps/install takes, because the region lives in the
	// card file and not in the installed prefix.
	"/api/v1/apps/editor":    {http.MethodGet},
	"/api/v1/apps/install":   {http.MethodPost},
	"/api/v1/apps/enable":    {http.MethodPost},
	"/api/v1/apps/uninstall": {http.MethodPost},
	// 7c9ff63's recovery door. clear faults a QUARANTINED built-in without the
	// RESET ALL collateral: a compiled tier's menu that panicked at boot reads
	// installed false, so both door_open gates refuse the enable revive and this is
	// the only way back short of wiping the owner's WiFi. The reboot is the
	// firmware's, not the proxy's arm. D36 deleted the factory door with the
	// in-image tier: the hub's /api/orb/seed is the restore path now, and it
	// reaches the device through the ordinary sd/app and apps/install doors below.
	"/api/v1/apps/clear": {http.MethodPost},
	"/api/v1/sd/app":     {http.MethodPost},
	// v1.1.18's file-manager doors and the LAN twin of serial RESET ALL. The
	// proxy forwards these verbatim (token, caps, lane) and owns none of the
	// policy: the device already refuses traversal, the device-owned
	// directories, and non-empty trees with its own sentences, and a hub that
	// filtered paths a second way would be a second authority that can
	// disagree. /factory is allowed because the Firmware tab's Reset button
	// must reach a board on a wall — that is its whole point — and it is
	// POST-only, confirm=ALL, with the typed confirmation living in the page.
	"/api/v1/factory":  {http.MethodPost},
	"/api/v1/sd/ls":    {http.MethodGet},
	"/api/v1/sd/mkdir": {http.MethodPost},
	"/api/v1/sd/rm":    {http.MethodPost},
	"/api/v1/sd/write": {http.MethodPost},
}

// Byte caps for one proxied exchange. The orb's httpd is real and sometimes slow, so
// the dial timeout stays orbnet's business; these cap BYTES, which is what stops a
// route that streams from turning the hub into an unbounded proxy for another host.
const (
	proxyResponseCap = 8 << 20 // device body copied back to the browser
	// fmUploadCap is the ONE proxied request body that streams (see proxyOrb):
	// the file manager's upload, capped at the device's own write gate
	// (ORB_SD_FM_WRITE_MAX in orb_sd.h) so the hub never accepts bytes the
	// device will refuse.
	fmUploadCap     = 128 << 20
	proxyRequestCap = 8 << 20 // browser body forwarded to the device: the largest legal
	// bundle is art 0x7F0000 (ORB_THEME_SLOT_CAP) behind a sector-aligned art start of
	// 0x10000 — exactly 8 MB. The theme upload door on the device takes 0x800000+0x20000
	// (ORB_THEME_UPLOAD_MAX); a hub that forwards less than the format can legally
	// describe refuses a bundle the device would install, in the wrong door, with the
	// wrong sentence. themecontainer_test.go pins this boundary to the byte.
	ipGrantTTL = 6 * time.Hour
	// grantCeiling caps how far a live announcement stream may stretch a commitment,
	// measured from the first time the operator committed. Without it an orb that
	// announces forever holds a dial open on a decision made months ago; with the TTL
	// alone an orb the operator uses every day falls off mid-afternoon.
	grantCeiling = 30 * 24 * time.Hour
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
		IP  string `json:"ip"`
		MAC string `json:"mac"`
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
	// The MAC is optional and it is not trusted: it is what the operator clicked, and
	// the hub has no key that proves it. Recording it is what lets the cross-lock
	// notice later that a DIFFERENT mac announced this address, and refuse. A typed
	// address arrives with none and stays an unbound grant the card labels honestly.
	s.grants.learnAs(ip, strings.TrimSpace(req.MAC))
	s.setOrbIP(ip)
	s.saveState()
	m := s.grants.bound(ip)
	s.jsonOK(w, map[string]any{"ip": ip, "allowed": true, "mac": m, "bound": m != ""})
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
