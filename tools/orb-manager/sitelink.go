package main

// h2-design.md §5 (H2 hub steps 1–2, lead review points 5, 6 and 9): the hub's half of the
// PKCE connect (RFC 8252 loopback) — the state machine between the press and the death of
// an attempt, the one-time callback door its listener serves, the file the credential
// lives in, and the two doors that read and end the link.
//
// The flow is the whole client minus the UI: POST /api/site/connect mints a verifier and
// a state, opens a loopback listener on a free port, and hands back the /connect URL for
// the browser; the site approves there and redirects a code to 127.0.0.1:<port>/callback;
// the listener exchanges code+verifier at POST /api/hub/token (§2.3) and stores the bearer
// it is given. One listener, one attempt: it dies at the exchange, at a denial, or after
// ten idle minutes, and a second press replaces an open attempt rather than queueing
// behind it. A wrong path, method, Host or state answers 400 and does NOT end the attempt —
// a page that guesses cannot cancel somebody's sign-in.
//
// The token lives in site-link.json and nowhere else: not in state.json (the file is a
// sibling, so TestStateFileHoldsNoSecrets never reads it), not in the log, and not in any
// /api answer. /api/site/link-status says WHICH state the link is in and never the
// credential; the token goes only to the origin recorded in the file, and only while that
// origin still equals siteReleaseBase().
//
// Step 2 is the other half of what a credential is for: GET /api/site/designs proxies §2.5's
// list with that bearer and nothing cached in front of it, and POST /api/site/disconnect ends
// the link from this side. Both obey lead review point 5's rule about the file: a 401 from the
// site rewrites it as disconnected-without-a-token, so the page can still say Disconnected and
// offer Reconnect after a restart, while a disconnect from Orb Manager removes it outright —
// the two differ because one is news the owner is owed and the other is a wish already granted.

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync/atomic"
	"time"
	"unicode"
	"unicode/utf8"
)

// siteConnectIdle is how long an unanswered callback may hold the listener: long enough
// for an owner who alt-tabbed away mid-approval, short enough that a connect abandoned in
// a half-open browser tab stops squatting on a loopback port before the next workday. A
// var so a test can wait on the deadline the shipped hub has, on a scale it can afford.
var siteConnectIdle = 10 * time.Minute

// siteTokenBodyMax bounds the site's answer to the exchange. §2.3's document is a token,
// a link id, an origin and a handle; anything larger is a broken or hostile site, and
// reading more of it buys nothing.
const siteTokenBodyMax = 8 << 10

// siteLinkFileName is the credential's one home, beside state.json in the config dir.
const siteLinkFileName = "site-link.json"

// siteDesignsBodyMax bounds the site's answer to the designs read. §2.5 caps the list at
// five hundred rows; a row of id, app, a 55-byte name, version, visibility and timestamp
// is under 200 bytes of JSON, so the full list is about 100 KB and 256 KB leaves room. An
// answer larger than this is not that list, and the hub does not read the rest of it.
const siteDesignsBodyMax = 256 << 10

// siteCallTimeout is the patience every bearer call to the site gets: long enough for a
// cold query across the internet, short enough that an owner who pressed a button reads an
// answer instead of watching a spinner. A var for the reason siteConnectIdle is one — a
// test pins the timeout case against the shipped deadline at a scale it can afford.
var siteCallTimeout = 10 * time.Second

// The three sentences §5's site doors answer with, one per answer the hub can give. They
// name orb-os.net rather than the configured host because that is the account's own name:
// a hub pointed at a developer's site has a developer's GORB_SITE and its owner is reading
// about the same door either way, and the words the lead review fixed are these.
const (
	siteMsgNotConnected = "not connected to orb-os.net"
	siteMsgDisconnected = "disconnected from orb-os.net"
	siteMsgNoAnswer     = "orb-os.net did not answer"
)

// siteLink is the hub's link to an orb-os.net account, as §5's file shape names it: the
// site origin, the bearer it handed, when the link was made, the owner's handle for the
// page's words, and the disconnected flag a 401 rewrites onto the file (step 2's writer;
// read here so a hub that was disconnected while closed still says so after a restart).
// An empty Token reads as no link at all, never as an empty credential to send.
type siteLink struct {
	Token        string `json:"token,omitempty"`
	Site         string `json:"site"`
	LinkedAt     int64  `json:"linkedAt"`
	Handle       string `json:"handle,omitempty"`
	Disconnected bool   `json:"disconnected,omitempty"`
}

// siteLinkPath is the credential file: a sibling of the state file, derived from its
// directory so a test that moves s.statePath moves this too. An unset statePath has no
// sibling worth naming, and every door on this file reads that as "nothing stored".
func (s *Server) siteLinkPath() string {
	return filepath.Join(filepath.Dir(s.statePath), siteLinkFileName)
}

// loadSiteLink reads the credential file. Missing, unreadable, unparseable or holding no
// token all read as nil: the hub simply has no link, which is the same answer a first run
// gives. Nothing here is fatal — a damaged credential costs a relink, not the hub.
func (s *Server) loadSiteLink() *siteLink {
	if s.statePath == "" {
		return nil
	}
	b, err := os.ReadFile(s.siteLinkPath())
	if err != nil {
		return nil
	}
	var l siteLink
	if err := json.Unmarshal(b, &l); err != nil {
		return nil
	}
	if strings.TrimSpace(l.Token) == "" {
		return nil
	}
	return &l
}

// liveSiteLink is the credential as every caller of it wants it: stored, complete, and
// naming the site this hub is open to today. A file naming another origin is read as no
// link — the token is only ever sent to the site that minted it, and only while that site
// is still the one this hub talks to.
func (s *Server) liveSiteLink() *siteLink {
	l := s.loadSiteLink()
	if l == nil || l.Disconnected || !strings.EqualFold(l.Site, siteReleaseBase()) {
		return nil
	}
	return l
}

// saveSiteLink writes the credential whole: the config dir first (0700, exactly as
// saveState learned — a hub whose statePath names a dir that does not exist yet must see
// the dir before a 0600 file lands in it), then temp file, then rename, so a crash
// mid-write cannot leave a half token that reads as a link to nobody. The mode is 0600
// because the file holds a bearer.
func (s *Server) saveSiteLink(l *siteLink) error {
	if s.statePath == "" || l == nil {
		return errors.New("orb-manager has no config dir to store the site link in")
	}
	b, err := json.MarshalIndent(l, "", "  ")
	if err != nil {
		return err
	}
	dir := filepath.Dir(s.statePath)
	s.siteLinkMu.Lock()
	defer s.siteLinkMu.Unlock()
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return err
	}
	tmp := s.siteLinkPath() + ".tmp"
	if err := os.WriteFile(tmp, b, 0o600); err != nil {
		return err
	}
	if err := os.Rename(tmp, s.siteLinkPath()); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return nil
}

// clearSiteLink forgets the credential. A file that is already gone counts as success:
// the act is "no link on disk", and the disk already agrees.
func (s *Server) clearSiteLink() error {
	if s.statePath == "" {
		return nil
	}
	s.siteLinkMu.Lock()
	defer s.siteLinkMu.Unlock()
	if err := os.Remove(s.siteLinkPath()); err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	return nil
}

// --- the connect URL -------------------------------------------------------------------

// siteChallenge is §2.1's PKCE S256 binding: base64url of the SHA-256 of the verifier's
// ASCII bytes, unpadded — exactly 43 characters of the unreserved base64url set.
func siteChallenge(verifier string) string {
	sum := sha256.Sum256([]byte(verifier))
	return base64.RawURLEncoding.EncodeToString(sum[:])
}

// sitePlatformOf names this machine the way §2.1 spells it: Go says darwin, the site says
// macos, and anything else the site can be told about is linux.
func sitePlatformOf(goos string) string {
	switch goos {
	case "windows":
		return "windows"
	case "darwin":
		return "macos"
	default:
		return "linux"
	}
}

// siteConnectPlatform is sitePlatformOf for the machine this binary is running on.
func siteConnectPlatform() string { return sitePlatformOf(runtime.GOOS) }

// siteLabel is the /connect label off this machine's own name.
func siteLabel() string { return siteLabelOf(os.Hostname()) }

// siteLabelOf turns a hostname into the label §2.1 accepts: no control (Cc), format (Cf,
// which covers the bidi controls and zero-width characters) or line/paragraph separators —
// the site refuses a label carrying one, so the hub never sends one — then trimmed, then
// cut to 64 BYTES on a rune boundary. Nothing left to show, or no hostname at all, labels
// itself "this computer": the line on the approve page must name the machine as well as it
// can, and it must never be blank.
func siteLabelOf(name string, err error) string {
	if err != nil {
		name = ""
	}
	var b strings.Builder
	b.Grow(len(name))
	for _, r := range name {
		if unicode.IsControl(r) || unicode.Is(unicode.Cf, r) || r == '\u2028' || r == '\u2029' {
			continue
		}
		b.WriteRune(r)
	}
	label := strings.TrimSpace(b.String())
	if label == "" {
		return "this computer"
	}
	if len(label) <= 64 {
		return label
	}
	cut := 64
	for cut > 0 && !utf8.RuneStart(label[cut]) {
		cut--
	}
	return label[:cut]
}

// siteConnectURL builds the browser's /connect request. The query is assembled by hand in
// §5's order — install, label, platform, version, redirect, challenge, state — so the
// param order is code and not map luck, and so the wire bytes can be pinned by a test.
func (s *Server) siteConnectURL(port int, verifier, connectState string) string {
	fields := [][2]string{
		{"install", s.installID},
		{"label", siteLabel()},
		{"platform", siteConnectPlatform()},
		{"version", version},
		{"redirect", fmt.Sprintf("http://127.0.0.1:%d/callback", port)},
		{"challenge", siteChallenge(verifier)},
		{"state", connectState},
	}
	parts := make([]string, 0, len(fields))
	for _, f := range fields {
		parts = append(parts, f[0]+"="+url.QueryEscape(f[1]))
	}
	return siteReleaseBase() + "/connect?" + strings.Join(parts, "&")
}

// --- the attempt -----------------------------------------------------------------------

// siteAttempt is one connect in flight: the listener it opened, the two secrets it minted,
// and the flag that says this attempt is over — exchange, denial, replacement or timeout —
// so the listener's second request finds a dead attempt and not a second sign-in.
type siteAttempt struct {
	verifier string
	state    string
	port     int
	host     string
	ln       net.Listener
	srv      *http.Server
	timer    *time.Timer
	done     atomic.Bool
}

// siteRandom n bytes of crypto/rand, or an error. There is no fallback: a verifier or a
// state guessed from a clock is not the secret PKCE and the callback check are for.
func siteRandom(n int) ([]byte, error) {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return nil, err
	}
	return b, nil
}

// openSiteConnect starts one attempt: mint verifier and state, close any attempt already
// open (lead review point 6: a press replaces, it does not queue), bind a free loopback
// port, and serve the callback until the attempt dies. It returns the URL the browser is
// to open and the attempt itself, so the flow is testable at the URL — openBrowser stays
// a release-mode courtesy that never enters a test's way.
func (s *Server) openSiteConnect() (string, *siteAttempt, error) {
	s.closeSiteAttempt()
	vb, err := siteRandom(32)
	if err != nil {
		return "", nil, fmt.Errorf("the connect could not be minted: %w", err)
	}
	sb, err := siteRandom(16)
	if err != nil {
		return "", nil, fmt.Errorf("the connect could not be minted: %w", err)
	}
	att := &siteAttempt{
		verifier: base64.RawURLEncoding.EncodeToString(vb),
		state:    hex.EncodeToString(sb),
	}
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return "", nil, fmt.Errorf("the connect could not open a callback listener: %w", err)
	}
	att.ln = ln
	att.port = ln.Addr().(*net.TCPAddr).Port
	att.host = fmt.Sprintf("127.0.0.1:%d", att.port)
	att.srv = &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		s.serveSiteCallback(att, w, r)
	})}
	// One page per attempt, and the attempt has one: no keep-alive means the connection
	// closes when the receipt is written, so a shut listener leaves no idle socket behind
	// it and nothing has to hang the connection up mid-response to be rid of one.
	att.srv.SetKeepAlivesEnabled(false)
	// The deadline and the pointer are published together, under the mutex endSiteAttempt
	// takes to stop the timer: an attempt whose callback can fire before its own timer is
	// on record is an attempt that stops a field it never read back.
	s.siteLinkMu.Lock()
	att.timer = time.AfterFunc(siteConnectIdle, func() { s.finishSiteAttempt(att) })
	s.siteAttempt = att
	s.siteLinkMu.Unlock()
	go func() {
		// An attempt that ends shuts its own listener, so Serve comes back holding the
		// listener's own "closed" error: expected, not worth a log line.
		if err := att.srv.Serve(ln); err != nil && !errors.Is(err, http.ErrServerClosed) && !errors.Is(err, net.ErrClosed) {
			log.Printf("orb-manager: site callback: %v", err)
		}
	}()
	return s.siteConnectURL(att.port, att.verifier, att.state), att, nil
}

// closeSiteAttempt ends whatever connect is open, if any.
func (s *Server) closeSiteAttempt() {
	s.siteLinkMu.Lock()
	att := s.siteAttempt
	s.siteAttempt = nil
	s.siteLinkMu.Unlock()
	if att != nil {
		s.finishSiteAttempt(att)
	}
}

// finishSiteAttempt shuts one attempt down, once: the listener is shut so no new
// connection is accepted, the timer is stopped, the hub's pointer is dropped if it still
// names this attempt, and the server is closed. A callback racing the shutdown finds done
// already set and does nothing twice.
func (s *Server) finishSiteAttempt(att *siteAttempt) {
	s.endSiteAttempt(att)
	shutdownSiteAttempt(att)
}

// endSiteAttempt is the half a serving callback can afford: the attempt is over, its
// listener shut, its timer stopped, the hub's pointer dropped — but the connection the
// callback arrived on is left open, because the answer to it has not been written yet.
// Closing the server here would close that connection mid-response and hand the browser an
// EOF where the receipt page belongs.
func (s *Server) endSiteAttempt(att *siteAttempt) {
	if !att.done.CompareAndSwap(false, true) {
		return
	}
	s.siteLinkMu.Lock()
	if att.timer != nil {
		att.timer.Stop()
	}
	if s.siteAttempt == att {
		s.siteAttempt = nil
	}
	s.siteLinkMu.Unlock()
	if att.ln != nil {
		_ = att.ln.Close()
	}
}

// shutdownSiteAttempt closes what endSiteAttempt left: the server, and with it any socket
// still tracked. Only the paths with no response in flight call it — a replaced press, an
// expired timer, a hub shutting down — because a callback still writing its page must be
// allowed to finish it. Keep-alives are off on this server, so a callback that ends on its
// own closes its connection when the page is sent and leaves nothing for this to reap.
func shutdownSiteAttempt(att *siteAttempt) {
	if att.srv != nil {
		_ = att.srv.Close()
		return
	}
	if att.ln != nil {
		_ = att.ln.Close()
	}
}

// pendingSiteLink reports a connect in flight, for the status route's "pending".
func (s *Server) pendingSiteLink() bool {
	s.siteLinkMu.Lock()
	defer s.siteLinkMu.Unlock()
	return s.siteAttempt != nil && !s.siteAttempt.done.Load()
}

// --- the callback -----------------------------------------------------------------------

// serveSiteCallback is the attempt's one door: GET /callback on its own Host and with its
// own state, and nothing else. Everything that fails short of that is a 400 that leaves
// the attempt waiting (lead review point 6); a request that finds the attempt dead — the
// browser refresh behind a successful first callback — gets the plain 404.
func (s *Server) serveSiteCallback(att *siteAttempt, w http.ResponseWriter, r *http.Request) {
	if att.done.Load() {
		http.Error(w, "no connect is in progress", http.StatusNotFound)
		return
	}
	q := r.URL.Query()
	switch {
	case r.Method != http.MethodGet || r.URL.Path != "/callback":
		siteCallbackRefuse(w, "this listener answers GET /callback only")
	case !strings.EqualFold(r.Host, att.host):
		siteCallbackRefuse(w, "this callback answers only its own loopback name")
	case q.Get("state") != att.state:
		siteCallbackRefuse(w, "that callback does not match the connect in progress")
	case q.Get("error") != "":
		s.endSiteAttempt(att)
		siteCallbackPage(w, "<h1>Not connected</h1><p>The connect was declined: Orb Manager is not connected.</p>")
	case q.Get("code") != "":
		s.siteFinishWithCode(att, w, q.Get("code"))
	default:
		siteCallbackRefuse(w, "that callback carries neither a code nor a denial")
	}
}

// siteCallbackRefuse answers the 400s that do NOT end the attempt: plain text, one
// sentence, nothing a probe could learn the rules from.
func siteCallbackRefuse(w http.ResponseWriter, msg string) {
	http.Error(w, msg, http.StatusBadRequest)
}

// siteCallbackPage answers the attempt's last page, plain and unstyled: it is a receipt
// for a tab the operator is invited to close, not a product surface. It flushes, because
// every caller shuts the listener down the moment it returns — a buffered page behind a
// closed connection is an EOF where the owner expects an answer.
func siteCallbackPage(w http.ResponseWriter, body string) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	_, _ = io.WriteString(w, "<!doctype html><html><head><title>Orb Manager</title></head><body>"+body+"</body></html>\n")
	if f, ok := w.(http.Flusher); ok {
		f.Flush()
	}
}

// siteFinishWithCode ends the attempt with the §2.3 exchange, whatever the exchange makes
// of the code: one callback, one answer, no second try at either. The attempt is finished
// first — a slow or silent site must not leave a live listener squatting behind an open
// exchange — and the browser sees success only once the new token is on disk.
func (s *Server) siteFinishWithCode(att *siteAttempt, w http.ResponseWriter, code string) {
	// The attempt ends here: the listener is shut so nothing else gets in, but the server
	// waits until the page below is written — a receipt the browser can actually read.
	s.endSiteAttempt(att)
	// The credential the hub held BEFORE this link, read now: the save below overwrites
	// the file, and lead review point 6 wants the old bearer revoked after the exchange.
	prev := s.loadSiteLink()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	tok, handle, err := s.siteTokenExchange(ctx, att.verifier, code)
	if err != nil {
		log.Printf("orb-manager: site connect: the exchange failed: %v", err)
		siteCallbackPage(w, "<h1>Not connected</h1><p>That sign-in did not go through: press Connect again.</p>")
		return
	}
	newLink := &siteLink{Token: tok, Site: siteReleaseBase(), LinkedAt: time.Now().Unix(), Handle: handle}
	if err := s.saveSiteLink(newLink); err != nil {
		log.Printf("orb-manager: site connect: the token could not be stored: %v", err)
		siteCallbackPage(w, "<h1>Not connected</h1><p>That sign-in did not go through: press Connect again.</p>")
		return
	}
	// A relink replaces the link on the SITE; the hub revokes the bearer it held best
	// effort, so an install moved to another account leaves no live token behind. Its
	// outcome never reaches the page — the new link is already stored and already true.
	if prev != nil && prev.Token != "" && prev.Token != tok &&
		strings.EqualFold(prev.Site, siteReleaseBase()) {
		s.revokeSiteToken(ctx, prev.Site, prev.Token)
	}
	siteCallbackPage(w, "<h1>Orb Manager is connected</h1><p>You can close this tab now.</p>")
}

// revokeSiteToken is the best-effort DELETE §2.4 behind the old bearer. The site's answer
// changes nothing: a token the site already forgot is exactly as revoked as one it
// accepted. It hands back the response's status line — the only part of the call either
// caller may write down, the request itself being all credential — or the empty string
// when the call never landed at all.
func (s *Server) revokeSiteToken(ctx context.Context, site, token string) string {
	req, err := http.NewRequestWithContext(ctx, http.MethodDelete, site+"/api/hub/token", nil)
	if err != nil {
		return ""
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("User-Agent", "orb-manager/"+version)
	resp, err := s.siteHTTP().Do(req)
	if err != nil {
		// The error names the URL and never the header: a bearer does not go into a log.
		log.Printf("orb-manager: site link: the token could not be revoked: %v", err)
		return ""
	}
	defer resp.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, siteTokenBodyMax))
	return resp.Status
}

// siteTokenExchange is §2.3: {code, verifier} POSTed as JSON to <site>/api/hub/token,
// through the no-redirects client — a 302 from a hostile site is a failed sign-in, not a
// step to take — with the context's ten-second patience and an eight-kilobyte cap on the
// answer. Only a 200 carrying a token is a token; every other answer, 400 or 401
// invalid_grant or junk, is one failure worded for the log.
func (s *Server) siteTokenExchange(ctx context.Context, verifier, code string) (token, handle string, err error) {
	payload, err := json.Marshal(map[string]string{"code": code, "verifier": verifier})
	if err != nil {
		return "", "", err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, siteReleaseBase()+"/api/hub/token", bytes.NewReader(payload))
	if err != nil {
		return "", "", err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", "orb-manager/"+version)
	resp, err := s.siteHTTP().Do(req)
	if err != nil {
		return "", "", fmt.Errorf("the site could not be reached for the exchange: %w", err)
	}
	defer resp.Body.Close()
	b, err := io.ReadAll(io.LimitReader(resp.Body, int64(siteTokenBodyMax)+1))
	if err != nil {
		return "", "", fmt.Errorf("the site's exchange answer did not arrive: %w", err)
	}
	if len(b) > siteTokenBodyMax {
		return "", "", errors.New("the site's exchange answer was too large to be one")
	}
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return "", "", fmt.Errorf("the site answered HTTP %d", resp.StatusCode)
	}
	var out struct {
		Token  string `json:"token"`
		Handle string `json:"handle"`
	}
	if err := json.Unmarshal(b, &out); err != nil {
		return "", "", fmt.Errorf("the site's exchange answer was not the shape it documents: %w", err)
	}
	if strings.TrimSpace(out.Token) == "" {
		return "", "", errors.New("the site answered the exchange without a token")
	}
	return out.Token, out.Handle, nil
}

// --- the doors ---------------------------------------------------------------------------

// handleSiteConnect answers POST /api/site/connect: start an attempt and hand back its
// URL. A browser that will not open is not this call's failure — the URL is in the answer,
// and step 3's page shows it for pasting.
func (s *Server) handleSiteConnect(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	linkURL, _, err := s.openSiteConnect()
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	go openBrowser(linkURL)
	s.jsonOK(w, map[string]any{"url": linkURL})
}

// handleSiteLinkStatus answers GET /api/site/link-status with the state of the link and
// nothing else (lead review point 5): "pending" while an attempt is open, "linked" with
// the site, handle and timestamp while a token is stored, "disconnected" when the site
// ended the link and the file says so, "none" for a hub that never linked. The token
// never leaves the file through this door or any other.
func (s *Server) handleSiteLinkStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	data := map[string]any{"state": "none"}
	switch {
	case s.pendingSiteLink():
		data["state"] = "pending"
	case s.liveSiteLink() != nil:
		l := s.loadSiteLink()
		data["state"] = "linked"
		data["site"] = l.Site
		data["linkedAt"] = l.LinkedAt
		if l.Handle != "" {
			data["handle"] = l.Handle
		}
	case s.disconnectedSiteLink() != nil:
		l := s.loadSiteLinkRaw()
		data["state"] = "disconnected"
		data["site"] = l.Site
		if l.Handle != "" {
			data["handle"] = l.Handle
		}
	}
	s.jsonOK(w, data)
}

// disconnectedSiteLink is the file a site-side Disconnect (or step 2's 401 rewrite)
// leaves: a named site with the disconnected flag and no token, read as itself — the
// hub's answer to the owner is "Disconnected from orb-os.net" with Reconnect — and not
// as a link.
func (s *Server) disconnectedSiteLink() *siteLink {
	l := s.loadSiteLinkRaw()
	if l == nil || !l.Disconnected || l.Token != "" || !strings.EqualFold(l.Site, siteReleaseBase()) {
		return nil
	}
	return l
}

// loadSiteLinkRaw reads the file whole, token or none, damaged bytes as nil. It is the
// reader disconnectedSiteLink needs because a disconnected-shaped file holds no token,
// and loadSiteLink (correctly) reads that file as no link at all.
func (s *Server) loadSiteLinkRaw() *siteLink {
	if s.statePath == "" {
		return nil
	}
	b, err := os.ReadFile(s.siteLinkPath())
	if err != nil {
		return nil
	}
	var l siteLink
	if err := json.Unmarshal(b, &l); err != nil {
		return nil
	}
	return &l
}

// --- the bearer calls ---------------------------------------------------------------------

// forgetSiteLink is what a 401 from the site leaves behind: lead review point 5's rewrite,
// the same file minus the credential and plus the flag — {site, handle, disconnected: true}
// — so a hub that was revoked while closed still reads as Disconnected after a restart and
// its page can offer Reconnect. That is why this rewrites rather than deletes: the fact
// that the link ended is the fact the owner is owed, and a deleted file cannot say it.
// linkedAt is kept because the link really was made then; the handle is kept because the
// page says whose account it used to be.
func (s *Server) forgetSiteLink(l *siteLink) {
	if l == nil {
		if err := s.clearSiteLink(); err != nil {
			log.Printf("orb-manager: site link: the credential could not be removed: %v", err)
		}
		return
	}
	if err := s.saveSiteLink(&siteLink{Site: l.Site, Handle: l.Handle, LinkedAt: l.LinkedAt, Disconnected: true}); err != nil {
		// The site has refused the bearer whatever this write makes the file say, so the
		// owner's sentence is already true; the log is for the operator who finds a
		// credential still on disk beside it.
		log.Printf("orb-manager: site link: the disconnect could not be recorded: %v", err)
	}
}

// fetchSiteDesigns asks §2.5's door for the owner's designs with the link's bearer, and
// hands back the answer's bytes. Three outcomes: the list; the site refused this bearer
// (refused, and only refused — the caller words the owner's sentence and marks the link);
// or a failure whose sentence is already the answer, because a stall, a 502, an oversized
// answer or an unparseable one is not a revocation and leaves the credential exactly where
// it was. The list is the site's document, read whole or not at all, and the door above
// re-encodes it without a second parse.
func (s *Server) fetchSiteDesigns(ctx context.Context, site, token string) (raw json.RawMessage, refused bool, err error) {
	ctx, cancel := context.WithTimeout(ctx, siteCallTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, site+"/api/hub/designs", nil)
	if err != nil {
		return nil, false, errors.New(siteMsgNoAnswer)
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", "orb-manager/"+version)
	req.Header.Set("Cache-Control", "no-store")
	resp, err := s.siteHTTP().Do(req)
	if err != nil {
		return nil, false, errors.New(siteMsgNoAnswer)
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusUnauthorized {
		return nil, true, nil
	}
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		// Includes the 3xx the no-redirect client hands back as an answer: a site pointing
		// the hub somewhere else is not answering this question.
		return nil, false, errors.New(siteMsgNoAnswer)
	}
	b, err := io.ReadAll(io.LimitReader(resp.Body, int64(siteDesignsBodyMax)+1))
	if err != nil || len(b) > siteDesignsBodyMax {
		return nil, false, errors.New(siteMsgNoAnswer)
	}
	b = bytes.TrimRight(b, " \t\r\n")
	if !siteDesignsObject(b) {
		return nil, false, errors.New(siteMsgNoAnswer)
	}
	return json.RawMessage(b), false, nil
}

// siteDesignsObject is §2.5's one shape: a JSON object whose designs field is an array. The
// rows inside are the site's to word and the hub's to pass on — reading them into structs
// would put a schema the hub must keep in step with a site that is free to add a field,
// where checking the envelope lets a newer answer through untouched.
func siteDesignsObject(b []byte) bool {
	var obj map[string]json.RawMessage
	if json.Unmarshal(b, &obj) != nil {
		return false
	}
	rows, ok := obj["designs"]
	if !ok {
		return false
	}
	return json.Unmarshal(rows, new([]json.RawMessage)) == nil
}

// --- the doors ----------------------------------------------------------------------------

// handleSiteDesigns answers GET /api/site/designs: the owner's design list, passed through
// verbatim as data. Nothing is cached — the list is the page's claim about what exists on
// the account right now, and a stale one would offer an Import that the site has since
// refused. A hub with no credential says so without dialing; a site that refuses the
// credential it was given is believed and the link is marked; every other failure is the
// site's problem and leaves the link alone.
func (s *Server) handleSiteDesigns(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	// A file already marked disconnected — by this door, by another bearer call, or by a
	// site-side Disconnect that landed while the hub was closed — is believed: the hub does
	// not dial with a credential it has been told is dead, and the owner hears the one
	// sentence the page turns into Reconnect.
	if s.disconnectedSiteLink() != nil {
		s.jsonErr(w, siteMsgDisconnected)
		return
	}
	l := s.liveSiteLink()
	if l == nil {
		s.jsonErr(w, siteMsgNotConnected)
		return
	}
	raw, refused, err := s.fetchSiteDesigns(r.Context(), l.Site, l.Token)
	switch {
	case refused:
		s.forgetSiteLink(l)
		s.jsonErr(w, siteMsgDisconnected)
	case err != nil:
		s.jsonErr(w, err.Error())
	default:
		s.jsonOKRaw(w, raw)
	}
}

// handleSiteDisconnect answers POST /api/site/disconnect: the owner ends the link. The site
// is told first, best effort and on a short leash — a 401 means the link is already gone
// there, which is exactly as disconnected as a 200, and a site that says nothing at all
// must not strand an owner who wants out. Then the credential is removed, and the answer is
// the same whichever way the call went: from this hub's point of view the link is over the
// moment it stops carrying its bearer. Nothing but the status line of the DELETE is written
// down; the request it describes is all credential.
func (s *Server) handleSiteDisconnect(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	if l := s.liveSiteLink(); l != nil {
		ctx, cancel := context.WithTimeout(context.Background(), siteCallTimeout)
		defer cancel()
		if status := s.revokeSiteToken(ctx, l.Site, l.Token); status != "" {
			log.Printf("orb-manager: site disconnect: DELETE /api/hub/token: %s", status)
		}
	}
	if err := s.clearSiteLink(); err != nil {
		// The one answer this door does not paper over: a credential the hub cannot remove
		// is a credential still in the file, and saying "disconnected" over that would be a
		// promise the disk does not keep.
		s.jsonErr(w, "the site link could not be forgotten: "+err.Error())
		return
	}
	s.jsonOK(w, map[string]any{"disconnected": true})
}

// routesSiteLink mounts the connect surface: the press, the status read, the design list
// that only a linked hub can ask for, and the door that ends the link — all behind
// requireControl, registered only inside main.go's !s.siteHidden block, and none of them on
// the editor-API listener: an app's page neither links the hub to an account, reads whose
// designs it holds, nor signs it out of one.
func (s *Server) routesSiteLink(mux *http.ServeMux) {
	mux.Handle("/api/site/connect", s.requireControl(http.HandlerFunc(s.handleSiteConnect)))
	mux.Handle("/api/site/link-status", s.requireControl(http.HandlerFunc(s.handleSiteLinkStatus)))
	mux.Handle("/api/site/designs", s.requireControl(http.HandlerFunc(s.handleSiteDesigns)))
	mux.Handle("/api/site/disconnect", s.requireControl(http.HandlerFunc(s.handleSiteDisconnect)))
}
