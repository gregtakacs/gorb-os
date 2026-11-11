package main

// h2-design.md §5, H2 hub step 1: the connect state machine, its loopback callback and
// site-link.json — driven through the same s.routes(mux) the binary serves, against a
// fake site written from §2 of the design alone. The fake verifies sha256(verifier) ==
// challenge before it hands out a token: that is the wire contract doing its PKCE job
// in CI. openBrowser never runs here — the URL the hub RETURNS is the tested artifact,
// and the test drives the callback URL itself.

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"reflect"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// --- the fake site ---------------------------------------------------------------------

// hubGrant is §2.2's stored row: the challenge it was approved against, the install it
// names, and the single-use stamps.
type hubGrant struct {
	challenge string
	install   string
	label     string
	at        time.Time
	used      bool
}

// hubSite is the fake orb-os.net: the approve page's rules (§2.1 + lead review 1), the
// decision endpoint (§2.2, 303 per lead review 2), the exchange with the S256 check and
// the same-install replacement (§2.3, refusal 400 invalid_grant per lead review 3), the
// bearer's own revoke (§2.4), a designs read (§2.5) to prove a stored token is live, and
// H4 §2's upload door on that read's path.
type hubSite struct {
	t            *testing.T
	srv          *httptest.Server
	mu           sync.Mutex
	grants       map[string]*hubGrant // sha256(code) hex -> the grant
	links        map[string]string    // installId -> live token
	owners       map[string]string    // token -> installId
	replacements int                  // same-install rows the exchange replaced (§2.3 transaction)
	revokes      []string             // bearers presented to DELETE /api/hub/token
	revoked      int                  // DELETEs that found a live row
	seenMax      int                  // most links ever live at once
	events       []string

	// The §2.5 door, as step 2's caller needs to see it: the document a live bearer is
	// handed, the bearer every ask carried, and a hook for the answers the honest site never
	// gives — a 502, a stall, a body that is not JSON — which is how a test pins that only a
	// 401 costs the hub its credential.
	designBody string
	designSeen []string
	designHook func(w http.ResponseWriter, r *http.Request)

	// The §2 upload door, same shape: the asks are recorded apart from the read's
	// because "one attempt, no retry" is a claim about this door's count alone. The
	// hook answers for it when a test needs a status or a body the honest fake never
	// gives; otherwise a live bearer gets uploadStatus (201 unless the test set one)
	// and uploadResp, and a dead one gets the same 401 §2.5's answers.
	uploadSeen   []string
	uploadQuery  []string
	uploadBody   []byte
	uploadHook   func(w http.ResponseWriter, r *http.Request)
	uploadResp   string
	uploadStatus int

	// The §2.3 answer, the same way for H4's scopes: tokenScopes is the "scopes" member
	// the exchange answers with, encoded verbatim so a test can hand null, [], junk or
	// duplicates, and tokenScopesSet is what distinguishes that from the key being absent
	// entirely — an H2 site's answer, which every older test still gets. tokenNoToken
	// drops the token from an otherwise-good 200, the one refusal a scope answer must not
	// talk the hub into ignoring.
	tokenScopes    any
	tokenScopesSet bool
	tokenNoToken   bool
}

func hubSiteServer(t *testing.T) *hubSite {
	t.Helper()
	hs := &hubSite{t: t, grants: map[string]*hubGrant{}, links: map[string]string{}, owners: map[string]string{}}
	hs.srv = httptest.NewServer(http.HandlerFunc(hs.handle))
	t.Cleanup(hs.srv.Close)
	t.Cleanup(func() { hs.srv.CloseClientConnections() })
	t.Setenv("GORB_SITE", hs.srv.URL)
	return hs
}

// seedLink files a link the site already had before the test started, so a hub credential
// can pre-exist with its site-side twin.
func (hs *hubSite) seedLink(install, token string) {
	hs.mu.Lock()
	defer hs.mu.Unlock()
	hs.links[install] = token
	hs.owners[token] = install
	hs.noteMax()
}

func (hs *hubSite) noteMax() {
	if len(hs.links) > hs.seenMax {
		hs.seenMax = len(hs.links)
	}
}

var (
	hubInstallRe   = regexp.MustCompile(`^[0-9a-f]{32}$`)
	hubVersionRe   = regexp.MustCompile(`^[0-9A-Za-z.+-]{1,32}$`)
	hubChallengeRe = regexp.MustCompile(`^[A-Za-z0-9_-]{43}$`)
	hubStateRe     = regexp.MustCompile(`^[A-Za-z0-9_-]{16,128}$`)
	hubRedirectRe  = regexp.MustCompile(`^http://127\.0\.0\.1:([0-9]{4,5})/callback$`)
	hubVerifierRe  = regexp.MustCompile(`^[A-Za-z0-9\-._~]{43,128}$`)
)

// acceptParams is §2.1 as lead review 1 tightened it: the whole set of rules a /connect
// request must pass before the site shows a form.
func acceptParams(q url.Values) bool {
	if !hubInstallRe.MatchString(q.Get("install")) ||
		!hubVersionRe.MatchString(q.Get("version")) ||
		!hubChallengeRe.MatchString(q.Get("challenge")) ||
		!hubStateRe.MatchString(q.Get("state")) {
		return false
	}
	label := strings.TrimSpace(q.Get("label"))
	if label == "" || len(label) > 64 {
		return false
	}
	for _, r := range label {
		if r < 0x20 || r == 0x7f || (r >= 0x80 && r <= 0x9f) ||
			(r >= 0x200b && r <= 0x200f) || (r >= 0x2028 && r <= 0x202a) || r == 0x2060 || r == 0xfeff {
			return false
		}
	}
	if p := hubRedirectRe.FindStringSubmatch(q.Get("redirect")); p == nil {
		return false
	} else if port, err := strconv.Atoi(p[1]); err != nil || port < 1024 {
		return false
	}
	switch q.Get("platform") {
	case "windows", "macos", "linux":
	default:
		return false
	}
	return true
}

func (hs *hubSite) handle(w http.ResponseWriter, r *http.Request) {
	switch {
	case r.Method == http.MethodGet && r.URL.Path == "/connect":
		hs.connectPage(w, r)
	case r.Method == http.MethodPost && r.URL.Path == "/api/connect":
		hs.decide(w, r)
	case r.Method == http.MethodPost && r.URL.Path == "/api/hub/token":
		hs.token(w, r)
	case r.Method == http.MethodDelete && r.URL.Path == "/api/hub/token":
		hs.revoke(w, r)
	case r.Method == http.MethodGet && r.URL.Path == "/api/hub/designs":
		hs.designs(w, r)
	case r.Method == http.MethodPost && r.URL.Path == "/api/hub/designs":
		hs.upload(w, r)
	default:
		http.NotFound(w, r)
	}
}

func (hs *hubSite) connectPage(w http.ResponseWriter, r *http.Request) {
	if !acceptParams(r.URL.Query()) {
		hs.record("connect-refused")
		w.WriteHeader(http.StatusBadRequest)
		_, _ = io.WriteString(w, "That is not a valid Orb Manager connect request.")
		return
	}
	hs.record("connect-ok")
	w.Header().Set("Content-Security-Policy", "frame-ancestors 'none'")
	w.Header().Set("X-Frame-Options", "DENY")
	_, _ = io.WriteString(w, "<h1>Link Orb Manager to orb-os.net</h1>")
}

// decide is §2.2: allow stores a single-use 60-second grant and answers 303 to the
// redirect with code and state (lead review point 2); deny answers 303 with
// error=access_denied and stores nothing.
func (hs *hubSite) decide(w http.ResponseWriter, r *http.Request) {
	var in map[string]string
	b, err := io.ReadAll(io.LimitReader(r.Body, 16384))
	if err != nil || json.Unmarshal(b, &in) != nil {
		http.Error(w, `{"error":"invalid_request"}`, http.StatusBadRequest)
		return
	}
	q := url.Values{}
	for k, v := range in {
		q.Set(k, v)
	}
	if !acceptParams(q) {
		hs.record("decide-refused")
		http.Error(w, `{"error":"invalid_request"}`, http.StatusBadRequest)
		return
	}
	if in["decision"] == "deny" {
		hs.record("deny")
		w.Header().Set("Location", q.Get("redirect")+"?error=access_denied&state="+url.QueryEscape(q.Get("state")))
		w.WriteHeader(http.StatusSeeOther)
		return
	}
	code := hubRandom(32)
	hs.mu.Lock()
	hs.grants[hubHash(code)] = &hubGrant{challenge: q.Get("challenge"), install: q.Get("install"),
		label: q.Get("label"), at: time.Now()}
	hs.mu.Unlock()
	hs.record("allow")
	w.Header().Set("Location", q.Get("redirect")+"?code="+url.QueryEscape(code)+"&state="+url.QueryEscape(q.Get("state")))
	w.WriteHeader(http.StatusSeeOther)
}

// token is §2.3: the grant lookup by sha256(code), unexpired and single-use, the
// verifier's charset, and the S256 check — the fake site doing the PKCE job for CI.
// Every failure answers the same way (lead review point 3: 400 invalid_grant). Success
// replaces any live link for the same install in the transaction, then mints.
func (hs *hubSite) token(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Code     string `json:"code"`
		Verifier string `json:"verifier"`
	}
	b, err := io.ReadAll(io.LimitReader(r.Body, 16384))
	if err != nil || json.Unmarshal(b, &in) != nil || in.Code == "" {
		hs.refuseGrant(w)
		return
	}
	hs.mu.Lock()
	g := hs.grants[hubHash(in.Code)]
	bad := g == nil || g.used || time.Since(g.at) > 60*time.Second ||
		!hubVerifierRe.MatchString(in.Verifier) || siteChallenge(in.Verifier) != g.challenge
	if bad {
		hs.mu.Unlock()
		hs.refuseGrant(w)
		return
	}
	g.used = true
	if old, ok := hs.links[g.install]; ok {
		delete(hs.owners, old)
		delete(hs.links, g.install)
		hs.replacements++
		hs.recordLocked("site-replace " + g.install)
	}
	tok := hubRandom(32)
	hs.links[g.install] = tok
	hs.owners[tok] = g.install
	hs.noteMax()
	hs.recordLocked("link " + g.install)
	hs.mu.Unlock()
	w.Header().Set("Content-Type", "application/json")
	ans := map[string]any{"hubId": "link-" + g.install, "site": hs.srv.URL, "handle": "greg"}
	if !hs.tokenNoToken {
		ans["token"] = tok
	}
	if hs.tokenScopesSet {
		ans["scopes"] = hs.tokenScopes
	}
	_ = json.NewEncoder(w).Encode(ans)
}

func (hs *hubSite) refuseGrant(w http.ResponseWriter) {
	hs.record("invalid_grant")
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusBadRequest)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": "invalid_grant"})
}

// revoke is §2.4: the bearer's own link dies. An already-gone bearer answers 401, which
// a best-effort hub reads as done.
func (hs *hubSite) revoke(w http.ResponseWriter, r *http.Request) {
	tok := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
	hs.mu.Lock()
	hs.revokes = append(hs.revokes, tok)
	install, ok := hs.owners[tok]
	if ok {
		delete(hs.owners, tok)
		delete(hs.links, install)
		hs.revoked++
		hs.recordLocked("hub-revoke " + install)
	}
	hs.mu.Unlock()
	if !ok {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusUnauthorized)
		_ = json.NewEncoder(w).Encode(map[string]string{"error": "unauthenticated"})
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_, _ = io.WriteString(w, `{"ok":true}`)
}

// designs is §2.5, as far as this fake goes: the bearer is recorded, a hook can answer for
// it, and otherwise a live link gets the list designBody carries (an empty one by default)
// and anything else gets the 401 §2.5's caller is required to believe.
func (hs *hubSite) designs(w http.ResponseWriter, r *http.Request) {
	tok := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
	hs.mu.Lock()
	hs.designSeen = append(hs.designSeen, tok)
	_, ok := hs.owners[tok]
	hook, body := hs.designHook, hs.designBody
	hs.mu.Unlock()
	if hook != nil {
		hs.record("designs")
		hook(w, r)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	if !ok {
		hs.record("designs-401")
		w.WriteHeader(http.StatusUnauthorized)
		_ = json.NewEncoder(w).Encode(map[string]string{"error": "unauthenticated"})
		return
	}
	hs.record("designs")
	if body == "" {
		body = `{"designs":[]}`
	}
	_, _ = io.WriteString(w, body)
}

// designAsks is how many times the §2.5 door was dialed, whatever it answered: the count
// behind "the hub caches nothing" and behind a shut door that asked for nothing.
func (hs *hubSite) designAsks() int {
	hs.mu.Lock()
	defer hs.mu.Unlock()
	return len(hs.designSeen)
}

// designBearer is the bearer the nth designs ask carried, header and all: the test that
// claims an exact Authorization header reads the exact bytes back.
func (hs *hubSite) designBearer(n int) string {
	hs.mu.Lock()
	defer hs.mu.Unlock()
	if n >= len(hs.designSeen) {
		return ""
	}
	return hs.designSeen[n]
}

// designDown kills the §2.5 door for every later ask, answering status with a body that is
// not the §2.5 document: the shape of a site having a bad day, which must cost the hub
// nothing it stored.
func (hs *hubSite) designDown(status int) {
	hs.mu.Lock()
	defer hs.mu.Unlock()
	hs.designHook = func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(status)
		_, _ = io.WriteString(w, "<html>the site is having a moment</html>")
	}
}

// designStall hangs the §2.5 door past the hub's patience for as many asks as it names.
func (hs *hubSite) designStall(asks int, hold time.Duration) {
	hs.mu.Lock()
	left := asks
	hs.designHook = func(w http.ResponseWriter, r *http.Request) {
		hs.mu.Lock()
		if left > 0 {
			left--
			hs.mu.Unlock()
			time.Sleep(hold)
			return
		}
		hs.mu.Unlock()
	}
	hs.mu.Unlock()
}

func (hs *hubSite) record(event string) {
	hs.mu.Lock()
	hs.events = append(hs.events, event)
	hs.mu.Unlock()
}

// upload is §2's door as step 2's caller needs to see it: the bearer, the query and the
// design bytes every ask carried, and a hook for the answers the honest site never gives
// — a 409 with a clash, a 429, a stall. A live bearer otherwise gets uploadStatus (201
// unless the test set one) and uploadResp; a dead one gets the same 401 §2.5 answers,
// because §2 puts the credential check in front of everything the body could say.
func (hs *hubSite) upload(w http.ResponseWriter, r *http.Request) {
	tok := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
	b, _ := io.ReadAll(io.LimitReader(r.Body, 16<<20))
	hs.mu.Lock()
	hs.uploadSeen = append(hs.uploadSeen, tok)
	hs.uploadQuery = append(hs.uploadQuery, r.URL.RawQuery)
	hs.uploadBody = b
	hook, status, resp := hs.uploadHook, hs.uploadStatus, hs.uploadResp
	_, ok := hs.owners[tok]
	hs.mu.Unlock()
	if hook != nil {
		hs.record("upload")
		hook(w, r)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	if !ok {
		hs.record("upload-401")
		w.WriteHeader(http.StatusUnauthorized)
		_ = json.NewEncoder(w).Encode(map[string]string{"error": "unauthenticated"})
		return
	}
	hs.record("upload")
	if status == 0 {
		status = http.StatusCreated
	}
	if resp == "" {
		resp = `{"id":"d-1","app":"clock","name":"Skeleton","version":1,"visibility":"private","url":"https://orb-os.net/t/d-1"}`
	}
	w.WriteHeader(status)
	_, _ = io.WriteString(w, resp)
}

// uploadAsks is how many times the §2 door was dialed, whatever it answered: the count
// behind "one attempt, no retry" and behind every local refusal that asked for nothing.
func (hs *hubSite) uploadAsks() int {
	hs.mu.Lock()
	defer hs.mu.Unlock()
	return len(hs.uploadSeen)
}

// uploadAsk is the nth ask: the bearer it carried, the query it rode, and the document it
// posted. The claim it serves is exact bytes, exact headers, one question.
type hubUploadAsk struct {
	bearer string
	query  string
	body   []byte
}

func (hs *hubSite) uploadAsk(n int) (hubUploadAsk, bool) {
	hs.mu.Lock()
	defer hs.mu.Unlock()
	if n >= len(hs.uploadSeen) {
		return hubUploadAsk{}, false
	}
	return hubUploadAsk{bearer: hs.uploadSeen[n], query: hs.uploadQuery[n], body: hs.uploadBody}, true
}

// recordLocked is record for a caller already holding hs.mu.
func (hs *hubSite) recordLocked(event string) {
	hs.events = append(hs.events, event)
}

func (hs *hubSite) saw(event string) bool {
	hs.mu.Lock()
	defer hs.mu.Unlock()
	for _, e := range hs.events {
		if e == event {
			return true
		}
	}
	return false
}

func (hs *hubSite) tokenFor(install string) string {
	hs.mu.Lock()
	defer hs.mu.Unlock()
	return hs.links[install]
}

// count is how many requests the fake site answered, for a test whose whole claim is that
// the hub asked for nothing.
func (hs *hubSite) count() int {
	hs.mu.Lock()
	defer hs.mu.Unlock()
	return len(hs.events)
}

// hubRandom is the site's own mint: 32 random bytes base64url, the shape §2.2's code and
// §2.3's mintToken share.
func hubRandom(n int) string {
	b, err := siteRandom(n)
	if err != nil {
		panic(err)
	}
	return base64.RawURLEncoding.EncodeToString(b)
}

func hubHash(code string) string {
	sum := sha256.Sum256([]byte(code))
	return hex.EncodeToString(sum[:])
}

// --- the browser's walk ------------------------------------------------------------------

// noRedirectClient follows nothing: the test reads the site's 303 Location itself, the
// way the browser would, and dials the callback URL on purpose.
var noRedirectClient = &http.Client{
	Timeout: 5 * time.Second,
	CheckRedirect: func(*http.Request, []*http.Request) error {
		return http.ErrUseLastResponse
	},
}

// siteApprove walks the browser's half of the flow for one connect URL: GET /connect
// (the fake applies §2.1's rules to the hub's own bytes), POST the allow decision, and
// hand back the Location the browser would land on.
func siteApprove(t *testing.T, hs *hubSite, connectURL string) string {
	t.Helper()
	resp, err := http.Get(connectURL)
	if err != nil {
		t.Fatalf("GET /connect: %v", err)
	}
	body, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("the hub's connect URL was refused by the site: %d %s", resp.StatusCode, body)
	}
	return siteDecide(t, hs, connectURL, "allow")
}

// siteDeny walks the deny half: the same form with decision=deny.
func siteDeny(t *testing.T, hs *hubSite, connectURL string) string {
	t.Helper()
	return siteDecide(t, hs, connectURL, "deny")
}

func siteDecide(t *testing.T, hs *hubSite, connectURL, decision string) string {
	t.Helper()
	u, err := url.Parse(connectURL)
	if err != nil {
		t.Fatal(err)
	}
	in := map[string]string{"decision": decision}
	for k, vs := range u.Query() {
		in[k] = vs[0]
	}
	payload, _ := json.Marshal(in)
	resp, err := noRedirectClient.Post(hs.srv.URL+"/api/connect", "application/json", strings.NewReader(string(payload)))
	if err != nil {
		t.Fatalf("POST /api/connect: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusSeeOther {
		t.Fatalf("the %s decision answered %d, want 303", decision, resp.StatusCode)
	}
	loc := resp.Header.Get("Location")
	if loc == "" {
		t.Fatalf("the %s decision carried no Location", decision)
	}
	return loc
}

// fetchPage dials a URL over real HTTP and returns status and body: the callback door is
// the hub's own live listener, and this is the browser's hand.
func fetchPage(t *testing.T, url string) (int, string) {
	t.Helper()
	resp, err := http.Get(url)
	if err != nil {
		t.Fatalf("GET %s: %v", url, err)
	}
	defer resp.Body.Close()
	b, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		t.Fatal(err)
	}
	return resp.StatusCode, string(b)
}

// okEnv reports an {ok:true} envelope, so a status read that failed says so as a shape
// failure and not as a confusing nil map below it.
func okEnv(env map[string]any) bool {
	ok, _ := env["ok"].(bool)
	return ok && env["data"] != nil
}

// fetchPageDead dials an attempt's callback port and reports whether anything is still
// listening: after the exchange, or after a denial, the door must be a refused socket and
// not a page with a second attempt behind it.
func fetchPageDead(t *testing.T, att *siteAttempt) (int, bool) {
	t.Helper()
	resp, err := noRedirectClient.Get("http://" + att.host + "/callback")
	if err != nil {
		return 0, false
	}
	defer resp.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 4096))
	return resp.StatusCode, true
}

// linkHub is a hub with its state file under a fresh temp dir, an install id, and the
// site door open — the shape armState leaves a real hub in.
func linkHub(t *testing.T) *Server {
	t.Helper()
	s := newHub()
	s.statePath = filepath.Join(t.TempDir(), "state.json")
	s.installID = mintInstallID()
	if s.installID == "" {
		t.Fatal("no install id to link with")
	}
	return s
}

// hubBearer asks the fake site's designs door with a bearer, and reports the status: the
// proof a stored token is live, or dead.
func hubBearer(t *testing.T, hs *hubSite, token string) int {
	t.Helper()
	req, err := http.NewRequest(http.MethodGet, hs.srv.URL+"/api/hub/designs", nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Authorization", "Bearer "+token)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("GET /api/hub/designs: %v", err)
	}
	defer resp.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 4096))
	return resp.StatusCode
}

// --- the tests ---------------------------------------------------------------------------

// The credential file is the hub's only copy of a bearer: 0600 in a 0700 dir, written
// whole, read gently, cleared idempotently — and it is a sibling of state.json, never a
// guest inside it, which is what keeps TestStateFileHoldsNoSecrets green with the new
// file existing.
func TestSiteLinkFileRoundTrip(t *testing.T) {
	s := linkHub(t)
	t.Setenv("GORB_SITE", "https://orb.example")
	if base := filepath.Base(s.siteLinkPath()); base != "site-link.json" {
		t.Errorf("the credential file is named %q, beside %q", base, s.statePath)
	}
	if filepath.Dir(s.siteLinkPath()) != filepath.Dir(s.statePath) {
		t.Error("site-link.json is not a sibling of state.json")
	}

	// A fresh dir under a statePath that does not exist yet: saveState learned
	// MkdirAll-0700, and the credential file must have learned it too.
	s.statePath = filepath.Join(t.TempDir(), "cfg", "state.json")
	want := &siteLink{Token: "tok-roundtrip", Site: "https://orb.example", LinkedAt: 1700000000, Handle: "greg"}
	if err := s.saveSiteLink(want); err != nil {
		t.Fatalf("saveSiteLink: %v", err)
	}
	fi, err := os.Stat(s.siteLinkPath())
	if err != nil {
		t.Fatal(err)
	}
	if fi.Mode().Perm() != 0o600 {
		t.Errorf("the credential file's mode is %o, want 0600", fi.Mode().Perm())
	}
	di, err := os.Stat(filepath.Dir(s.statePath))
	if err != nil {
		t.Fatal(err)
	}
	if di.Mode().Perm() != 0o700 {
		t.Errorf("the config dir's mode is %o, want 0700", di.Mode().Perm())
	}
	got := s.loadSiteLink()
	if got == nil || !reflect.DeepEqual(got, want) {
		t.Errorf("loadSiteLink = %+v, want the round trip of %+v", got, want)
	}

	// The state file never carries it: saveState writes, the token's name is absent.
	s.saveState()
	b, err := os.ReadFile(s.statePath)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(b), want.Token) {
		t.Error("state.json carries the site token")
	}

	// H4's scopes ride the same file and the same write path: both words round-trip, and
	// the key is spelled exactly as §6 spells it.
	scoped := &siteLink{Token: "tok-scoped", Site: "https://orb.example", LinkedAt: 1700000001,
		Handle: "greg", Scopes: []string{"designs:read", "designs:write"}}
	if err := s.saveSiteLink(scoped); err != nil {
		t.Fatalf("saveSiteLink scoped: %v", err)
	}
	sb, err := os.ReadFile(s.siteLinkPath())
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(sb), `"scopes": [`) || !strings.Contains(string(sb), `"designs:read"`) ||
		!strings.Contains(string(sb), `"designs:write"`) {
		t.Errorf("the scopes key is not the one §6 spells:\n%s", sb)
	}
	if got := s.loadSiteLink(); got == nil || !reflect.DeepEqual(got, scoped) {
		t.Errorf("scoped round trip = %+v, want %+v", got, scoped)
	}

	// The read-only DEFAULT writes no key at all: a link whose Scopes is the empty value —
	// an H2 file, or any writer that carries no consent — lands on disk as an H2 hub's
	// bytes. (A fresh link the site ANSWERED read-only carries the word explicitly, because
	// the exchange stores what it was told; TestSiteExchangeScopesAreTheAnswer pins that.)
	for _, ro := range []*siteLink{
		{Token: "tok-plain", Site: "https://orb.example", LinkedAt: 1700000002, Handle: "greg"},
		{Token: "tok-empty", Site: "https://orb.example", LinkedAt: 1700000003, Handle: "greg", Scopes: []string{}},
	} {
		if err := s.saveSiteLink(ro); err != nil {
			t.Fatalf("saveSiteLink read-only: %v", err)
		}
		rb, err := os.ReadFile(s.siteLinkPath())
		if err != nil {
			t.Fatal(err)
		}
		if strings.Contains(string(rb), "scopes") {
			t.Errorf("the read-only default wrote a scopes key:\n%s", rb)
		}
		if got := s.loadSiteLink(); got == nil || !slices.Equal(got.Scopes, ro.Scopes) {
			t.Errorf("read-only round trip %v loaded as %+v", ro.Scopes, got)
		}
	}

	// Unparseable bytes read as no link, never as a crash and never as an empty bearer.
	if err := os.WriteFile(s.siteLinkPath(), []byte(`{"token":`), 0o600); err != nil {
		t.Fatal(err)
	}
	if l := s.loadSiteLink(); l != nil {
		t.Errorf("a damaged file read as %+v, want nil", l)
	}

	// A file with no token — step 2's disconnected shape — reads as no link, while the
	// status door still reads the disconnection after a restart (lead review point 5).
	disc := `{"site":"https://orb.example","linkedAt":0,"handle":"greg","disconnected":true}`
	if err := os.WriteFile(s.siteLinkPath(), []byte(disc), 0o600); err != nil {
		t.Fatal(err)
	}
	if l := s.loadSiteLink(); l != nil {
		t.Errorf("a tokenless file read as %+v, want nil", l)
	}
	if l := s.disconnectedSiteLink(); l == nil || l.Site != "https://orb.example" {
		t.Errorf("disconnectedSiteLink = %+v, want the disconnected file read as itself", l)
	}
	env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil)
	if env["data"].(map[string]any)["state"] != "disconnected" {
		t.Errorf("the status of a disconnected file = %v, want state disconnected", env["data"])
	}
	if strings.Contains(fmt.Sprint(env), want.Token) {
		t.Error("the status answer carries the token")
	}

	// A file naming another site is read as no link at all — the token is only ever sent
	// to the site that minted it, and only while that site is the one this hub speaks to.
	if err := s.saveSiteLink(&siteLink{Token: "tok-other", Site: "https://elsewhere.example", LinkedAt: 1}); err != nil {
		t.Fatal(err)
	}
	if l := s.liveSiteLink(); l != nil {
		t.Errorf("a file naming another site read as %+v, want no live link", l)
	}
	if env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil); env["data"].(map[string]any)["state"] != "none" {
		t.Errorf("a file naming another site read as %v, want none", env["data"])
	}

	// Unreadable bytes read as no link too.
	if err := os.WriteFile(s.siteLinkPath(), []byte(`{"token":"x"}`), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(s.siteLinkPath(), 0o000); err != nil {
		t.Fatal(err)
	}
	defer func() { _ = os.Chmod(s.siteLinkPath(), 0o600) }()
	if l := s.loadSiteLink(); l != nil {
		t.Errorf("an unreadable file read as %+v, want nil", l)
	}

	// Clear is idempotent: the act is "no link on disk", and the disk agreeing twice is
	// still agreement.
	if err := s.clearSiteLink(); err != nil {
		t.Fatalf("clearSiteLink: %v", err)
	}
	if err := s.clearSiteLink(); err != nil {
		t.Fatalf("clearSiteLink on a gone file: %v", err)
	}
	if l := s.loadSiteLink(); l != nil {
		t.Errorf("after clearing, the file reads as %+v, want nil", l)
	}

	// A hub with no statePath has no sibling to name and stores nothing, loudly rather
	// than silently into some working directory.
	bare := newHub()
	if l := bare.loadSiteLink(); l != nil {
		t.Error("a hub with no statePath found a credential")
	}
	if err := bare.saveSiteLink(want); err == nil {
		t.Error("a hub with no statePath stored a credential")
	}
	if err := bare.clearSiteLink(); err != nil {
		t.Errorf("clearing nothing: %v", err)
	}
}

// The connect URL is the wire contract: §5's exact param order, the site's spelling of
// the platform, the label as §2.1 accepts it, and the S256 challenge over the real
// verifier — pinned with fixed inputs so a reordered map or a reworded value breaks
// this test and not somebody's sign-in.
func TestSiteConnectURLIsTheContract(t *testing.T) {
	s := linkHub(t)
	s.installID = "0123456789abcdef0123456789abcdef"
	t.Setenv("GORB_SITE", "https://orb.example")
	// RFC 7636 Appendix B's own worked pair: this verifier's S256 challenge is that
	// answer, so the challenge line is checked against the RFC and not against itself.
	verifier := "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"
	wantChallenge := "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
	connectState := "0f4c9a1b2c3d4e5f60718293a4b5c6d7"

	got := s.siteConnectURL(41234, verifier, connectState)
	base, raw, found := strings.Cut(got, "?")
	if !found || base != "https://orb.example/connect" {
		t.Fatalf("siteConnectURL = %q, want it to start at https://orb.example/connect?", got)
	}
	var keys []string
	vals := map[string]string{}
	for _, kv := range strings.Split(raw, "&") {
		k, v, _ := strings.Cut(kv, "=")
		keys = append(keys, k)
		u, err := url.QueryUnescape(v)
		if err != nil {
			t.Fatalf("param %s is not cleanly escaped: %v", k, err)
		}
		vals[k] = u
	}
	wantKeys := []string{"install", "label", "platform", "version", "scope", "redirect", "challenge", "state"}
	if strings.Join(keys, ",") != strings.Join(wantKeys, ",") {
		t.Errorf("param order = %v, want %v", keys, wantKeys)
	}
	if vals["install"] != s.installID {
		t.Errorf("install = %q, want the hub's id", vals["install"])
	}
	if vals["version"] != version {
		t.Errorf("version = %q, want the build stamp %q", vals["version"], version)
	}
	if vals["platform"] != siteConnectPlatform() {
		t.Errorf("platform = %q, want this machine's word %q", vals["platform"], siteConnectPlatform())
	}
	// H4 §6: the hub always asks for both words, space-separated, QueryEscaped — so the
	// wire carries exactly this, and the field sits between version and redirect.
	if vals["scope"] != "designs:read designs:write" {
		t.Errorf("scope = %q, want the two known words joined with one space", vals["scope"])
	}
	if vals["redirect"] != "http://127.0.0.1:41234/callback" {
		t.Errorf("redirect = %q", vals["redirect"])
	}
	if vals["challenge"] != wantChallenge {
		t.Errorf("challenge = %q, want the RFC 7636 B answer %q", vals["challenge"], wantChallenge)
	}
	if vals["state"] != connectState {
		t.Errorf("state = %q", vals["state"])
	}
	// The whole wire at once, so a reordering fails HERE rather than in a review: every
	// field the contract fixes is literal bytes in its literal place, and only the three
	// environmental values — this machine's label, its platform, this build's stamp — are
	// interpolated as themselves. The scope segment reads designs%3Aread+designs%3Awrite
	// because QueryEscape turns the join's space into a + and each colon into %3A.
	wantRaw := "install=" + s.installID + "&label=" + url.QueryEscape(siteLabel()) +
		"&platform=" + siteConnectPlatform() + "&version=" + url.QueryEscape(version) +
		"&scope=designs%3Aread+designs%3Awrite&redirect=http%3A%2F%2F127.0.0.1%3A41234%2Fcallback" +
		"&challenge=" + wantChallenge + "&state=" + connectState
	if raw != wantRaw {
		t.Errorf("the wire is\n got %q\nwant %q", raw, wantRaw)
	}
	hostname, hnErr := os.Hostname()
	if want := siteLabelOf(hostname, hnErr); vals["label"] != want {
		t.Errorf("label = %q, want the machine's own %q", vals["label"], want)
	}
	q := url.Values{}
	for k, v := range vals {
		q.Set(k, v)
	}
	if !acceptParams(q) {
		t.Errorf("the hub's own connect URL would be refused by the site's §2.1 rules: %v", vals)
	}

	// The platform map, as a table rather than as this machine's accident.
	for goos, want := range map[string]string{"windows": "windows", "darwin": "macos", "linux": "linux", "freebsd": "linux"} {
		if got := sitePlatformOf(goos); got != want {
			t.Errorf("sitePlatformOf(%q) = %q, want %q", goos, got, want)
		}
	}

	// The label: cut to 64 BYTES on a rune boundary, sanitized, and never blank.
	long := strings.Repeat("é", 40) // 80 bytes, 40 runes: the site's line holds 64 bytes
	if cut := siteLabelOf(long, nil); cut != strings.Repeat("é", 32) {
		t.Errorf("a long multi-byte label cut to %q, want its first 32 runes", cut)
	} else if len(cut) != 64 {
		t.Errorf("the cut label is %d bytes, want exactly 64", len(cut))
	}
	if got := siteLabelOf("", errors.New("hostname: no such host")); got != "this computer" {
		t.Errorf("a failing hostname labeled %q", got)
	}
	if got := siteLabelOf("   ", nil); got != "this computer" {
		t.Errorf("a blank hostname labeled %q", got)
	}
	if got := siteLabelOf("orb\u200bpc\u2028", nil); got != "orbpc" {
		t.Errorf("a label with zero-width and line-separator characters became %q, want the strip", got)
	}
	if got := siteLabelOf("bad\nname\ttab", nil); got != "badnametab" {
		t.Errorf("a label with controls became %q, want them gone", got)
	}
	if got := siteLabelOf("  padded  ", nil); got != "padded" {
		t.Errorf("a padded hostname became %q, want the trim", got)
	}
}

// The whole flow, over real sockets: press → pending → the site's form accepts the hub's
// own bytes → the browser lands on the loopback door → the exchange runs with the real
// verifier → the token is on disk at 0600 → the status says linked with the handle and
// never the bearer → the stored bearer is live at the site → the attempt is dead so a
// refresh cannot sign anybody in twice.
func TestSiteConnectEndToEnd(t *testing.T) {
	hs := hubSiteServer(t)
	s := linkHub(t)
	s.saveState()

	env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil)
	if !okEnv(env) || env["data"].(map[string]any)["state"] != "none" {
		t.Fatalf("a hub that never linked says %v, want state none", env["data"])
	}

	connectURL, att, err := s.openSiteConnect()
	if err != nil {
		t.Fatalf("openSiteConnect: %v", err)
	}
	t.Cleanup(func() { s.closeSiteAttempt() })
	if !strings.HasPrefix(connectURL, hs.srv.URL+"/connect?") {
		t.Fatalf("connect URL = %.120q, want it at the site this hub speaks to", connectURL)
	}
	if env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil); env["data"].(map[string]any)["state"] != "pending" {
		t.Errorf("an open attempt says %v, want state pending", env["data"])
	}

	loc := siteApprove(t, hs, connectURL)
	if !hs.saw("connect-ok") || !hs.saw("allow") {
		t.Fatal("the fake site refused or never saw the hub's own connect request")
	}
	status, page := fetchPage(t, loc)
	if status != http.StatusOK {
		t.Fatalf("the callback answered %d: %.200s", status, page)
	}
	if !strings.Contains(page, "Orb Manager is connected") {
		t.Errorf("the success page reads %.200s", page)
	}
	if !att.done.Load() {
		t.Error("a callback that reached the exchange left the attempt open")
	}
	if _, still := fetchPageDead(t, att); still {
		t.Error("the callback listener is still answering after the exchange")
	}

	stored := s.loadSiteLink()
	if stored == nil {
		t.Fatal("a completed connect stored no credential")
	}
	if stored.Token == "" || stored.Site != siteReleaseBase() || stored.LinkedAt <= 0 || stored.Handle != "greg" {
		t.Errorf("stored link = %+v, want the site's token, origin, timestamp and handle", stored)
	}
	if fi, err := os.Stat(s.siteLinkPath()); err != nil || fi.Mode().Perm() != 0o600 {
		t.Errorf("the credential file's mode or presence: %v %v", fi, err)
	}
	if live := s.liveSiteLink(); live == nil || live.Token != stored.Token {
		t.Error("the stored link does not read as live against the site this hub speaks to")
	}
	if code := hubBearer(t, hs, stored.Token); code != http.StatusOK {
		t.Errorf("the stored bearer is not live at the site: designs answered %d", code)
	}
	// The site filed the link under THIS hub's install id, and the token it minted is the
	// one on disk: the same-install key §2.3 will later replace.
	if got := hs.tokenFor(s.installID); got != stored.Token {
		t.Errorf("the site's link for install %s is %q, want the token it handed the hub", s.installID, got)
	}

	// The status answer describes the link and never carries the credential.
	env = designCall(t, s, http.MethodGet, "/api/site/link-status", nil)
	data := env["data"].(map[string]any)
	if data["state"] != "linked" || data["site"] != siteReleaseBase() || data["handle"] != "greg" {
		t.Errorf("a linked hub says %v", data)
	}
	if data["linkedAt"] == nil || data["linkedAt"].(float64) <= 0 {
		t.Errorf("a linked hub's status carries no linkedAt: %v", data)
	}
	if strings.Contains(fmt.Sprint(env), stored.Token) {
		t.Error("the status answer leaked the token")
	}
	// Neither door's answer leaks it either, and neither does the log of a failed attempt.
	if strings.Contains(page, stored.Token) {
		t.Error("the callback page leaked the token")
	}
}

// TestSiteLinkFullPress is step 3's contract minus the pixels: the UI does exactly this, and
// nothing more. It boots the hub's real handlers, presses Connect THROUGH POST /api/site/connect
// (not openSiteConnect — the route, the one the button hits, which also fires openBrowser), walks
// the browser's approval, dials the callback the hub is listening on, then reads /api/site/link-status
// for the flip to linked and /api/site/designs for the list. If these five doors line up end to
// end against a fake site, the page has something true to draw for each of its states.
func TestSiteLinkFullPress(t *testing.T) {
	hs := hubSiteServer(t)
	hs.designBody = `{"designs":[{"id":"` + siteDesignID + `","app":"clock","name":"Plain Six","version":4,` +
		`"visibility":"private","updatedAt":"2026-11-10T09:00:00.000Z"}]}`
	s := linkHub(t)
	s.saveState()
	t.Cleanup(func() { s.closeSiteAttempt() })

	// A route press opens a browser through the hub's own openBrowser (exec of xdg-open on
	// this box). Point PATH at an empty dir so that exec cannot find a browser: the URL the
	// hub returns is the tested artifact, not whether a headless CI box can paint a window.
	t.Setenv("PATH", t.TempDir())

	// A hub that never linked reads none.
	if st := linkStatusState(t, s); st["state"] != "none" {
		t.Fatalf("a hub that never linked says %v, want state none", st)
	}

	// Press connect THROUGH the door the button hits, and take the URL it hands back.
	env := designCall(t, s, http.MethodPost, "/api/site/connect", nil)
	if env["ok"] != true {
		t.Fatalf("POST /api/site/connect: %v", env)
	}
	connectURL, _ := env["data"].(map[string]any)["url"].(string)
	if !strings.HasPrefix(connectURL, hs.srv.URL+"/connect?") {
		t.Fatalf("route press returned URL %.120q, want it at the site this hub speaks to", connectURL)
	}
	// The route press left a real attempt standing: the status the page would poll now reads pending.
	if st := linkStatusState(t, s); st["state"] != "pending" {
		t.Errorf("after the press the status reads %v, want pending", st)
	}

	// The browser's half, driven by the test: approve at the fake site, then dial the callback
	// the hub opened — the exchange and the store are the hub's, reached over real loopback HTTP.
	loc := siteApprove(t, hs, connectURL)
	status, page := fetchPage(t, loc)
	if status != http.StatusOK || !strings.Contains(page, "Orb Manager is connected") {
		t.Fatalf("the callback answered %d: %.200s", status, page)
	}
	if !hs.saw("connect-ok") || !hs.saw("allow") {
		t.Fatal("the fake site never saw the hub's own connect request")
	}

	// The flip the page redraws on, and the list it then reads. Neither answer carries the token.
	st := linkStatusState(t, s)
	if st["state"] != "linked" || st["handle"] != "greg" || st["site"] != siteReleaseBase() {
		t.Errorf("after the callback the status is %v, want linked/greg/this site", st)
	}
	if fi, err := os.Stat(s.siteLinkPath()); err != nil || fi.Mode().Perm() != 0o600 {
		t.Errorf("the credential file's mode or presence: %v %v", fi, err)
	}
	denv := designCall(t, s, http.MethodGet, "/api/site/designs", nil)
	if denv["ok"] != true {
		t.Fatalf("a linked hub's /api/site/designs: %v", denv)
	}
	designs, _ := denv["data"].(map[string]any)["designs"].([]any)
	if len(designs) != 1 {
		t.Fatalf("the design list has %d rows, want the one the site holds: %v", len(designs), denv["data"])
	}
	row, _ := designs[0].(map[string]any)
	if row["name"] != "Plain Six" || row["app"] != "clock" || row["id"] != siteDesignID {
		t.Errorf("the passed-through row is %v, want the site's Plain Six clock design", row)
	}
	if l := s.loadSiteLink(); l != nil && strings.Contains(fmt.Sprint(denv), l.Token) {
		t.Error("the design answer leaked the token")
	}
}

// Lead review point 6, the half that protects an attempt from a guesser: a request the
// listener refuses for path, method, Host, state or shape is a 400 that leaves the
// attempt waiting, so a probe cannot cancel somebody's sign-in — and the real callback
// afterwards still completes it.
func TestSiteConnectWrongStateKeepsWaiting(t *testing.T) {
	hs := hubSiteServer(t)
	s := linkHub(t)
	_, att, err := s.openSiteConnect()
	if err != nil {
		t.Fatalf("openSiteConnect: %v", err)
	}
	t.Cleanup(func() { s.closeSiteAttempt() })
	base := "http://" + att.host

	// The state is the listener's only name for this attempt: a callback with another
	// one — or none — is refused and changes nothing.
	for _, probe := range []string{
		base + "/callback?state=00000000000000000000000000000000&code=" + hubRandom(32),
		base + "/callback?code=" + hubRandom(32),
		base + "/callback?state=" + att.state + "&code=",
		base + "/callback",
		base + "/Callback?state=" + att.state,
		base + "/callback/extra?state=" + att.state,
		"http://localhost:" + strconv.Itoa(att.port) + "/callback?state=" + att.state,
	} {
		status, body := fetchPage(t, probe)
		if status != http.StatusBadRequest {
			t.Errorf("%s answered %d %.120s, want 400", probe, status, body)
		}
		if !s.pendingSiteLink() {
			t.Fatalf("%s ended the attempt; the status now reads %v", probe, s.loadSiteLink())
		}
		if env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil); env["data"].(map[string]any)["state"] != "pending" {
			t.Fatalf("after %s the hub says %v, want still pending", probe, env["data"])
		}
		if l := s.loadSiteLink(); l != nil {
			t.Fatalf("a refused callback stored %v", l)
		}
	}

	// A POST is a 400 too, and a wrong-Host GET is a 400: the listener answers one name.
	req, _ := http.NewRequest(http.MethodPost, base+"/callback?state="+att.state, strings.NewReader("code=x"))
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	resp, err := noRedirectClient.Do(req)
	if err != nil {
		t.Fatalf("POST /callback: %v", err)
	}
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 4096))
	resp.Body.Close()
	if resp.StatusCode != http.StatusBadRequest {
		t.Errorf("POST /callback answered %d, want 400", resp.StatusCode)
	}
	if !s.pendingSiteLink() {
		t.Fatal("a POST ended the attempt")
	}

	// The hub's own status door does not leak the state either: an open attempt's answer
	// names no secret the site could replay.
	if env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil); strings.Contains(fmt.Sprint(env), att.state) {
		t.Errorf("the status answer carries the attempt's state: %v", env)
	}

	// And the attempt the probes could not end still ends right: approve it properly.
	loc := siteApprove(t, hs, s.siteConnectURL(att.port, att.verifier, att.state))
	status, page := fetchPage(t, loc)
	if status != http.StatusOK || !strings.Contains(page, "Orb Manager is connected") {
		t.Fatalf("after the probes the real callback answered %d %.200s", status, page)
	}
	if s.loadSiteLink() == nil {
		t.Error("a completed connect stored no credential")
	}
}

// A denial is an answer, not a crash: the attempt's last page says the owner is not
// connected, nothing is stored, the listener closes, and the hub is exactly where it was
// before the press — so the next press works.
func TestSiteConnectStateMismatchStoresNothing(t *testing.T) {
	hs := hubSiteServer(t)
	s := linkHub(t)
	_, att, err := s.openSiteConnect()
	if err != nil {
		t.Fatalf("openSiteConnect: %v", err)
	}

	loc := siteDeny(t, hs, s.siteConnectURL(att.port, att.verifier, att.state))
	if !strings.Contains(loc, "error=access_denied") {
		t.Fatalf("the deny Location is %.200s, want error=access_denied", loc)
	}
	status, page := fetchPage(t, loc)
	if status != http.StatusOK {
		t.Fatalf("the denial page answered %d", status)
	}
	if !strings.Contains(page, "Not connected") {
		t.Errorf("the denial page reads %.200s", page)
	}
	if s.loadSiteLink() != nil {
		t.Errorf("a denial stored %v", s.loadSiteLink())
	}
	if env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil); env["data"].(map[string]any)["state"] != "none" {
		t.Errorf("after a denial the hub says %v, want state none", env["data"])
	}
	if s.pendingSiteLink() {
		t.Error("a denial left the attempt open")
	}
	if _, alive := fetchPageDead(t, att); alive {
		t.Error("the listener still answers after a denial")
	}
	// The attempt closed, so the same callback again is a 404 and not a second denial
	// page — the state it carried names nothing any more.
	if code, alive := fetchPageDead(t, att); alive || code == http.StatusOK {
		t.Errorf("a replayed denial answered %d (alive %v), want a refused socket or a 404", code, alive)
	}
	if hs.saw("link") {
		t.Error("the site minted a token for a denied connect")
	}

	// And the hub relinks: press again, approve again, token again.
	connectURL, _, err := s.openSiteConnect()
	if err != nil {
		t.Fatalf("the second press: %v", err)
	}
	defer s.closeSiteAttempt()
	if _, page := fetchPage(t, siteApprove(t, hs, connectURL)); !strings.Contains(page, "Orb Manager is connected") {
		t.Errorf("a hub that had just been refused could not relink: %.200s", page)
	}
}

// One attempt, one callback, one exchange. A browser refresh behind a finished connect
// finds a dead listener and a 404; the code the site handed cannot be spent twice at
// §2.3; and a second press while an attempt is open replaces the first rather than
// queueing a second listener behind it (lead review point 6).
func TestSiteConnectSingleUse(t *testing.T) {
	hs := hubSiteServer(t)
	s := linkHub(t)
	connectURL, att, err := s.openSiteConnect()
	if err != nil {
		t.Fatalf("openSiteConnect: %v", err)
	}
	defer s.closeSiteAttempt()

	loc := siteApprove(t, hs, connectURL)
	redir, err := url.Parse(loc)
	if err != nil {
		t.Fatal(err)
	}
	got := redir.Query()
	if _, page := fetchPage(t, loc); !strings.Contains(page, "Orb Manager is connected") {
		t.Fatalf("the first callback did not complete: %.200s", page)
	}

	// The refresh: same URL, attempt gone. The listener is shut, so the browser is
	// refused a connection; whatever answers, the connect does not happen twice.
	if status, alive := fetchPageDead(t, att); alive && status != http.StatusNotFound {
		t.Errorf("a replayed callback answered %d, want 404 from a dead attempt", status)
	}

	// The code is spent at the site, too: replaying it at §2.3 is an invalid_grant, so a
	// stolen code cannot mint a second bearer.
	body, _ := json.Marshal(map[string]string{"code": got.Get("code"), "verifier": att.verifier})
	resp, err := noRedirectClient.Post(hs.srv.URL+"/api/hub/token", "application/json", strings.NewReader(string(body)))
	if err != nil {
		t.Fatalf("replaying the code: %v", err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
	if resp.StatusCode != http.StatusBadRequest || !strings.Contains(string(b), "invalid_grant") {
		t.Errorf("a spent code answered %d %.120s, want 400 invalid_grant", resp.StatusCode, b)
	}

	// And the hub's answer after that is still the one link it made: the replay stored
	// nothing and cost nothing.
	first := s.loadSiteLink()
	if first == nil {
		t.Fatal("the replay cost the hub its link")
	}
	if env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil); env["data"].(map[string]any)["state"] != "linked" {
		t.Errorf("after the replay the hub says %v, want still linked", env["data"])
	}

	// A second press replaces an open attempt rather than queueing behind it: the first
	// press's listener is shut, the hub's pointer names the second, and the status still
	// reads pending — one attempt open, never two.
	_, attA, err := s.openSiteConnect()
	if err != nil {
		t.Fatalf("second press: %v", err)
	}
	_, attB, err := s.openSiteConnect()
	if err != nil {
		t.Fatalf("third press: %v", err)
	}
	if _, alive := fetchPageDead(t, attA); alive {
		t.Error("the replaced attempt's listener is still answering")
	}
	if !attA.done.Load() {
		t.Error("the replaced attempt was left open")
	}
	s.siteLinkMu.Lock()
	open := s.siteAttempt
	s.siteLinkMu.Unlock()
	if open != attB {
		t.Error("the hub's pointer does not name the attempt the last press opened")
	}
	if env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil); env["data"].(map[string]any)["state"] != "pending" {
		t.Errorf("two presses read as %v, want one pending attempt", env["data"])
	}
	// Shutting the replaced attempt again — the timer firing late, say — must not disturb
	// the one the hub is actually holding.
	s.finishSiteAttempt(attA)
	if !s.pendingSiteLink() {
		t.Error("finishing the replaced attempt ended the open one")
	}
	// And the hub lets go of both: after the last close no attempt is open anywhere, and
	// the link the first half of this test made is still the hub's answer.
	s.closeSiteAttempt()
	if s.pendingSiteLink() {
		t.Error("an attempt survived the close")
	}
	if env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil); env["data"].(map[string]any)["state"] != "linked" {
		t.Errorf("with no attempt open the hub says %v, want linked", env["data"])
	}
}

// A relink replaces the link ON THE SITE (§2.3's transaction: the old row dies when the
// new one is filed). Lead review point 6 asks the hub to revoke the bearer it held, best
// effort, once the new one is stored — so an install moved to another account, or a
// machine rebuilt with a token still on disk, leaves no live credential behind.
func TestSiteConnectRevokesTheOldToken(t *testing.T) {
	hs := hubSiteServer(t)
	s := linkHub(t)

	// A hub that already had a link: the file on disk and the site's row for it.
	old := hubRandom(32)
	if err := s.saveSiteLink(&siteLink{Token: old, Site: siteReleaseBase(), LinkedAt: 1, Handle: "greg"}); err != nil {
		t.Fatal(err)
	}
	hs.seedLink(s.installID, old)
	if code := hubBearer(t, hs, old); code != http.StatusOK {
		t.Fatalf("the seeded bearer is not live at the site: %d", code)
	}

	connectURL, _, err := s.openSiteConnect()
	if err != nil {
		t.Fatalf("openSiteConnect: %v", err)
	}
	defer s.closeSiteAttempt()
	if _, page := fetchPage(t, siteApprove(t, hs, connectURL)); !strings.Contains(page, "Orb Manager is connected") {
		t.Fatalf("the relink did not complete: %.200s", page)
	}

	fresh := s.loadSiteLink()
	if fresh == nil || fresh.Token == old {
		t.Fatalf("after a relink the stored token = %+v, want a new one", fresh)
	}
	if got := hs.tokenFor(s.installID); got != fresh.Token {
		t.Errorf("the site's live link is %q, want the new token", got)
	}
	if code := hubBearer(t, hs, old); code == http.StatusOK {
		t.Error("the old bearer is still live at the site: the hub revoked nothing")
	}
	// The hub's own half: one DELETE, presented with the old bearer and nothing else.
	// Whether the site still had the row to kill is §2.3's business — its exchange already
	// replaced it — and the hub's answer to "401, never heard of it" is the same as its
	// answer to "ok": the old bearer is dead either way, which is what the line above reads.
	if len(hs.revokes) != 1 || hs.revokes[0] != old {
		t.Errorf("the hub presented %v to DELETE /api/hub/token, want exactly the old bearer", hs.revokes)
	}
	if hs.revoked+hs.replacements < 1 {
		t.Errorf("the old link survived both the exchange (replacements %d) and the revoke (revoked %d)",
			hs.replacements, hs.revoked)
	}
	if hs.replacements != 1 {
		t.Errorf("the site replaced %d rows, want the same-install replacement §2.3 promises", hs.replacements)
	}
	// Never more than one live link for one install, at any moment: a hub that revoked
	// before storing would drop below, a hub that stored twice would peak above.
	if hs.seenMax != 1 {
		t.Errorf("the site saw %d live links at once for one install", hs.seenMax)
	}
	if env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil); env["data"].(map[string]any)["state"] != "linked" {
		t.Errorf("a relinked hub says %v, want linked", env["data"])
	}
}

// A fresh link has nothing to revoke: the hub must not fire a DELETE at the site for a
// token it never held, and the connect completes exactly as any other first one.
func TestSiteConnectFirstLinkRevokesNothing(t *testing.T) {
	hs := hubSiteServer(t)
	s := linkHub(t)
	connectURL, _, err := s.openSiteConnect()
	if err != nil {
		t.Fatalf("openSiteConnect: %v", err)
	}
	defer s.closeSiteAttempt()
	if _, page := fetchPage(t, siteApprove(t, hs, connectURL)); !strings.Contains(page, "Orb Manager is connected") {
		t.Fatalf("the connect did not complete: %.200s", page)
	}
	if len(hs.revokes) != 0 {
		t.Errorf("a first link presented %v to the revoke door", hs.revokes)
	}
	if s.loadSiteLink() == nil {
		t.Error("a first link stored nothing")
	}
}

// Every door on the link sits behind requireControl like every other control route, and the
// editor credential — which opens the editor API and nothing else — is refused at the gate
// with the sentence that names the mistake. Linking the hub to an account, reading whose
// designs it holds and signing it out of one are hub powers; an app's page cannot ask for
// any of them, and none of these four patterns is mounted on the editor listener at all.
func TestSiteLinkRoutesRefuseEditorCreds(t *testing.T) {
	hs := hubSiteServer(t)
	s := linkHub(t)
	mux := http.NewServeMux()
	s.routes(mux)

	call := func(method, path string, editorHeader, editorQuery, goodToken bool) *httptest.ResponseRecorder {
		t.Helper()
		target := path
		if editorQuery {
			target += "?et=" + s.editorToken("clock")
		}
		req := httptest.NewRequest(method, target, nil)
		if editorHeader {
			req.Header.Set(editorTokenHeader, s.editorToken("clock"))
		}
		if goodToken {
			req.Header.Set(tokenHeader, s.token)
		}
		rec := httptest.NewRecorder()
		mux.ServeHTTP(rec, req)
		return rec
	}

	for _, c := range []struct{ method, path string }{
		{http.MethodPost, "/api/site/connect"},
		{http.MethodGet, "/api/site/link-status"},
		{http.MethodGet, "/api/site/designs"},
		{http.MethodPost, "/api/site/disconnect"},
		{http.MethodPost, "/api/site/upload"},
		{http.MethodGet, "/api/site/local-designs"},
	} {
		for _, editor := range []bool{true, false} {
			rec := call(c.method, c.path, editor, false, false)
			if rec.Code != http.StatusForbidden {
				t.Errorf("%s %s with no control credential = %d, want 403", c.method, c.path, rec.Code)
			}
			if editor && !strings.Contains(rec.Body.String(), "editor") {
				t.Errorf("%s %s refused an editor token as %s, want the refusal to name it",
					c.method, c.path, rec.Body.String())
			}
		}
		// The editor credential in the query channel is refused too, and is refused even
		// beside a good token: an app that learned to send both got that bug from us.
		if rec := call(c.method, c.path, false, true, false); rec.Code != http.StatusForbidden {
			t.Errorf("%s %s with ?et= = %d, want 403", c.method, c.path, rec.Code)
		}
		if rec := call(c.method, c.path, false, true, true); rec.Code != http.StatusForbidden {
			t.Errorf("%s %s with both credentials = %d, want the editor token to refuse it", c.method, c.path, rec.Code)
		}
		// The launch token gets through the gate on both doors. On the press that means a
		// 405 from the handler — a GET cannot press, and a POST would send a browser at a
		// site nobody is watching; on the status read it means the envelope the page wants.
		if rec := call(http.MethodGet, c.path, false, false, true); rec.Code == http.StatusForbidden {
			t.Errorf("GET %s with the launch token = 403, want the gate to pass", c.path)
		}
	}
	if rec := call(http.MethodGet, "/api/site/link-status", false, false, true); rec.Code != http.StatusOK {
		t.Fatalf("GET /api/site/link-status with the launch token = %d, want 200", rec.Code)
	}
	// Behind the gate the status read is honest about an attempt the hub itself opened:
	// the press's own 200-with-URL answer is TestSiteLinkDoorVerbsAndEmptyState's ground.
	if _, _, err := s.openSiteConnect(); err != nil {
		t.Fatalf("openSiteConnect: %v", err)
	}
	defer s.closeSiteAttempt()
	if rec := call(http.MethodGet, "/api/site/link-status", false, false, true); !strings.Contains(rec.Body.String(), "pending") {
		t.Errorf("an open attempt reads %s, want state pending", rec.Body.String())
	}
	s.closeSiteAttempt()
	// Refused at the gate, and stopped there: nothing reached the site, no attempt leaked,
	// and no credential appeared on disk.
	if hs.count() != 0 {
		t.Errorf("the guarded doors reached the site %d times", hs.count())
	}
	if s.loadSiteLink() != nil {
		t.Error("the guarded press stored a credential")
	}
}

// The wrong verb on a mounted door is a 405, not a silent fallthrough, and a hub with no
// link answers "none" rather than an error — the shape of a page's first paint.
func TestSiteLinkDoorVerbsAndEmptyState(t *testing.T) {
	t.Setenv("GORB_SITE", "https://orb.example")
	s := linkHub(t)
	if rec := rawCall(t, s, http.MethodGet, "/api/site/connect", nil); rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("GET /api/site/connect = %d, want 405", rec.Code)
	}
	if rec := rawCall(t, s, http.MethodPost, "/api/site/link-status", nil); rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("POST /api/site/link-status = %d, want 405", rec.Code)
	}
	if env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil); !okEnv(env) {
		t.Fatalf("an unlinked hub's status is not an {ok,data} envelope: %v", env)
	} else if env["data"].(map[string]any)["state"] != "none" {
		t.Errorf("an unlinked hub says %v, want state none", env["data"])
	}

	// The scopes key belongs to the linked answer alone (h4-design.md §6): where there is
	// no consent to name — no link, an attempt in flight, a link the site ended — the door
	// answers no scopes key at all, as it answers no handle when there is none.
	if st := linkStatusState(t, s); st["state"] != "none" {
		t.Fatalf("an unlinked hub says %v, want none", st)
	} else if _, has := st["scopes"]; has {
		t.Errorf("a none answer carries a scopes key: %v", st)
	}
	if _, _, err := s.openSiteConnect(); err != nil {
		t.Fatalf("openSiteConnect: %v", err)
	}
	t.Cleanup(func() { s.closeSiteAttempt() })
	if st := linkStatusState(t, s); st["state"] != "pending" {
		t.Fatalf("an open attempt says %v, want pending", st)
	} else if _, has := st["scopes"]; has {
		t.Errorf("a pending answer carries a scopes key: %v", st)
	}
	s.closeSiteAttempt()
	disc := `{"site":"https://orb.example","linkedAt":0,"handle":"greg","disconnected":true}`
	if err := os.WriteFile(s.siteLinkPath(), []byte(disc), 0o600); err != nil {
		t.Fatal(err)
	}
	if st := linkStatusState(t, s); st["state"] != "disconnected" {
		t.Fatalf("a disconnected file says %v, want disconnected", st)
	} else if _, has := st["scopes"]; has {
		t.Errorf("a disconnected answer carries a scopes key: %v", st)
	}
}

// A site that will not complete the exchange costs the hub nothing but the attempt: the
// page says so in one sentence, nothing is stored, the status is still none, and the
// listener shuts. Every failure of the dependency lands on the same three facts, whether
// the site answers 400 invalid_grant, answers 200 without a token, or is gone.
func TestSiteConnectExchangeFailuresStoreNothing(t *testing.T) {
	s := linkHub(t)

	for _, site := range []struct{ name, answer string }{
		{"invalid_grant", `{"error":"invalid_grant"}`},
		{"tokenless", `{"hubId":"link-x","site":"https://orb.example"}`},
		{"junk", "not json at all"},
	} {
		srv, hits := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			if site.answer == `{"error":"invalid_grant"}` {
				w.WriteHeader(http.StatusBadRequest)
			}
			_, _ = io.WriteString(w, site.answer)
		})
		t.Setenv("GORB_SITE", srv.URL)
		_, att, err := s.openSiteConnect()
		if err != nil {
			t.Fatalf("%s: openSiteConnect: %v", site.name, err)
		}
		status, page := fetchPage(t, "http://"+att.host+"/callback?state="+att.state+"&code="+hubRandom(32))
		if status != http.StatusOK {
			t.Fatalf("%s: the hub answered its own callback page with %d", site.name, status)
		}
		if !strings.Contains(page, "Not connected") {
			t.Errorf("%s: the failure page reads %.200s", site.name, page)
		}
		if s.loadSiteLink() != nil {
			t.Errorf("%s: a failed exchange stored %v", site.name, s.loadSiteLink())
		}
		if s.pendingSiteLink() {
			t.Errorf("%s: a failed exchange left the attempt open", site.name)
		}
		if env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil); env["data"].(map[string]any)["state"] != "none" {
			t.Errorf("%s: after a failed exchange the hub says %v, want none", site.name, env["data"])
		}
		// The exchange was really attempted, with the attempt's verifier: the failure is
		// the site's answer, not the hub forgetting to ask.
		asked := false
		for _, a := range *hits {
			if a.path == "/api/hub/token" {
				asked = true
			}
		}
		if !asked {
			t.Errorf("%s: the hub never POSTed /api/hub/token: %v", site.name, *hits)
		}
		if _, alive := fetchPageDead(t, att); alive {
			t.Errorf("%s: the listener still answers after the attempt ended", site.name)
		}
	}

	// A site that is gone at all fails the same way, straight through the client.
	t.Setenv("GORB_SITE", "http://127.0.0.1:1")
	if _, _, _, err := s.siteTokenExchange(context.Background(), "v", "c"); err == nil {
		t.Error("an unreachable site read as a successful exchange")
	}
}

// jsonStrings reads a door's answer back as the strings it holds, [] for a key that was
// absent or not an array — the shape a test compares with what the hub meant to answer.
func jsonStrings(v any) []string {
	arr, _ := v.([]any)
	out := make([]string, 0, len(arr))
	for _, e := range arr {
		sv, _ := e.(string)
		out = append(out, sv)
	}
	return out
}

// An H2 file — the exact bytes H2's own round-trip test left on disk, with no scopes key —
// is a complete credential to an H4 hub: it loads whole, it lives, and every reader of its
// consent comes away with the one word the file implies. Loading does not repair it: the
// bytes on disk after the read are the bytes before, because a hub that rewrote a
// credential it merely looked at could rewrite a good one it misread.
func TestSiteLinkFileWithoutScopesStillLoads(t *testing.T) {
	s := linkHub(t)
	t.Setenv("GORB_SITE", "https://orb.example")
	h2 := "{\n  \"token\": \"tok-h2\",\n  \"site\": \"https://orb.example\",\n  \"linkedAt\": 1700000000,\n  \"handle\": \"greg\"\n}"
	if err := os.WriteFile(s.siteLinkPath(), []byte(h2), 0o600); err != nil {
		t.Fatal(err)
	}
	l := s.loadSiteLink()
	if l == nil || l.Token != "tok-h2" || l.Site != "https://orb.example" || l.LinkedAt != 1700000000 || l.Handle != "greg" {
		t.Fatalf("the H2 file loaded as %+v, want it whole", l)
	}
	if len(l.Scopes) != 0 {
		t.Errorf("loading gave the file scopes it never had: %v", l.Scopes)
	}
	if live := s.liveSiteLink(); live == nil || live.Token != "tok-h2" {
		t.Errorf("liveSiteLink = %+v, want the H2 file read as the live link it is", live)
	}
	if got := siteLinkScopes(l); !slices.Equal(got, []string{"designs:read"}) {
		t.Errorf("siteLinkScopes = %v, want the read-only default", got)
	}
	if got := siteLinkScopes(nil); got != nil {
		t.Errorf("siteLinkScopes(nil) = %v, want no scopes for no link", got)
	}
	st := linkStatusState(t, s)
	if st["state"] != "linked" {
		t.Fatalf("an H2 file says %v, want linked", st)
	}
	if got := jsonStrings(st["scopes"]); !slices.Equal(got, []string{"designs:read"}) {
		t.Errorf("link-status scopes = %v, want the read-only array the page can draw", st["scopes"])
	}
	if b, err := os.ReadFile(s.siteLinkPath()); err != nil || string(b) != h2 {
		t.Errorf("reading the credential file changed it: %q err %v", b, err)
	}
	// The transcription above is the bytes an H2 hub really wrote, not a guess at them:
	// this hub's own writer, handed the same row with no consent on it, produces the same
	// bytes — which is the whole claim about what scopes did to the file's shape.
	if err := s.saveSiteLink(&siteLink{Token: "tok-h2", Site: "https://orb.example", LinkedAt: 1700000000, Handle: "greg"}); err != nil {
		t.Fatal(err)
	}
	if b, err := os.ReadFile(s.siteLinkPath()); err != nil || string(b) != h2 {
		t.Errorf("the H2 shape is not what this hub writes for a link with no consent:\n got %q\nwant %q (err %v)", b, h2, err)
	}
}

// The exchange stores the site's ANSWER, not the hub's ask (h4-design.md §6): whatever the
// fake site puts in the scopes member — both words, one, none, null, empty, junk,
// duplicates — the file ends up with normalizeSiteScopes' reading of it. Nothing here may
// cost a connect: a smaller answer is a smaller link, the page still says Orb Manager is
// connected, and the one answer that does fail the flow, a 200 without a token, fails it
// exactly as it always did and stores nothing.
func TestSiteExchangeScopesAreTheAnswer(t *testing.T) {
	both := []string{"designs:read", "designs:write"}
	readOnly := []string{"designs:read"}
	for _, c := range []struct {
		name    string
		scopes  any      // the scopes member the fake answers with, encoded verbatim
		set     bool     // false: the key is absent, an H2 site's answer
		noToken bool     // a 200 that carries no token at all
		want    []string // the stored file's scopes; nil means "no file written"
	}{
		{"both", both, true, false, both},
		{"read-only", readOnly, true, false, readOnly},
		{"absent", nil, false, false, readOnly},
		{"null", nil, true, false, readOnly},
		{"empty", []string{}, true, false, readOnly},
		{"write-only", []string{"designs:write"}, true, false, readOnly},
		// A write-only answer is the site refusing designs:read whatever else rides along,
		// and the hub reads it as the conservative thing it is (§10's first answer).
		{"junk-write-only", []string{"bogus:all", "designs:write", "designs:write"}, true, false, readOnly},
		// Unknown words and duplicates drop, and what remains sorts into the one spelling.
		{"junk-both-duplicated", []string{"bogus:all", "designs:read", "designs:write", "designs:write"}, true, false, both},
		{"no-token", both, true, true, nil},
	} {
		t.Run(c.name, func(t *testing.T) {
			hs := hubSiteServer(t)
			hs.tokenScopes, hs.tokenScopesSet, hs.tokenNoToken = c.scopes, c.set, c.noToken
			s := linkHub(t)
			connectURL, att, err := s.openSiteConnect()
			if err != nil {
				t.Fatalf("openSiteConnect: %v", err)
			}
			t.Cleanup(func() { s.closeSiteAttempt() })
			loc := siteApprove(t, hs, connectURL)
			status, page := fetchPage(t, loc)
			if status != http.StatusOK {
				t.Fatalf("the callback answered %d: %.200s", status, page)
			}
			if !att.done.Load() {
				t.Error("the callback left the attempt open")
			}
			if c.want == nil {
				if b := readSiteLinkFile(t, s); b != nil {
					t.Errorf("a tokenless 200 stored a file anyway: %s", b)
				}
				if !strings.Contains(page, "Not connected") {
					t.Errorf("the failure page reads %.200s, want the old sentence", page)
				}
				if st := linkStatusState(t, s); st["state"] != "none" {
					t.Errorf("a failed exchange says %v, want none", st)
				}
				return
			}
			if !strings.Contains(page, "Orb Manager is connected") {
				t.Errorf("%s: the page reads %.200s, want the success sentence: less than the ask is still a link", c.name, page)
			}
			l := s.loadSiteLink()
			if l == nil {
				t.Fatalf("%s: a completed connect stored no credential", c.name)
			}
			if !slices.Equal(l.Scopes, c.want) {
				t.Errorf("%s: stored scopes = %v, want the answer %v", c.name, l.Scopes, c.want)
			}
			if l.Token == "" || l.Handle != "greg" || l.Site != siteReleaseBase() {
				t.Errorf("%s: the scope answer disturbed the credential itself: %+v", c.name, l)
			}
			st := linkStatusState(t, s)
			if st["state"] != "linked" {
				t.Fatalf("%s: after storing, link-status says %v", c.name, st)
			}
			if got := jsonStrings(st["scopes"]); !slices.Equal(got, c.want) {
				t.Errorf("%s: link-status scopes = %v, want %v", c.name, st["scopes"], c.want)
			}
		})
	}
}

// The third death, the one with nobody at the keyboard: a connect abandoned in a half-open
// tab stops squatting on a loopback port when its patience runs out. The shipped patience
// is ten minutes; this waits on that same timer at a scale a test can afford, and finds the
// listener shut, the hub's pointer gone, and the status back to "none" — so the next press
// is a fresh attempt and not a stale verifier.
func TestSiteConnectIdleTimeoutEndsTheAttempt(t *testing.T) {
	hs := hubSiteServer(t)
	prev := siteConnectIdle
	siteConnectIdle = 60 * time.Millisecond
	t.Cleanup(func() { siteConnectIdle = prev })

	s := linkHub(t)
	_, att, err := s.openSiteConnect()
	if err != nil {
		t.Fatalf("openSiteConnect: %v", err)
	}
	if env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil); env["data"].(map[string]any)["state"] != "pending" {
		t.Fatalf("a fresh attempt reads %v, want pending", env["data"])
	}

	deadline := time.Now().Add(5 * time.Second)
	for s.pendingSiteLink() && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	if s.pendingSiteLink() {
		t.Fatal("the idle timer never ended the attempt")
	}
	if !att.done.Load() {
		t.Error("the attempt survived its own deadline")
	}
	s.siteLinkMu.Lock()
	open := s.siteAttempt
	s.siteLinkMu.Unlock()
	if open != nil {
		t.Error("the hub still holds an attempt the timer ended")
	}
	if _, alive := fetchPageDead(t, att); alive {
		t.Error("the expired attempt's listener is still answering")
	}
	if env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil); env["data"].(map[string]any)["state"] != "none" {
		t.Errorf("an expired attempt leaves the hub saying %v, want none", env["data"])
	}
	if s.loadSiteLink() != nil {
		t.Error("an unanswered connect stored a credential")
	}
	if hs.count() != 0 {
		t.Errorf("an abandoned attempt reached the site %d times", hs.count())
	}

	// And the press after a timeout works: the expired attempt cost nothing but itself.
	connectURL, _, err := s.openSiteConnect()
	if err != nil {
		t.Fatalf("the press after a timeout: %v", err)
	}
	defer s.closeSiteAttempt()
	if _, page := fetchPage(t, siteApprove(t, hs, connectURL)); !strings.Contains(page, "Orb Manager is connected") {
		t.Errorf("a hub whose last attempt expired could not link: %.200s", page)
	}
}

// --- step 2: the designs read, the disconnect, and what a 401 costs -------------------------

// linkedSiteHub files a link on both sides of the wire without walking a browser through the
// connect: step 2's doors are about a credential the hub already holds, and the credential's
// origin — the PKCE exchange — is TestSiteConnectEndToEnd's ground. It returns the hub and
// the bearer it now carries.
func linkedSiteHub(t *testing.T, hs *hubSite, handle string) (*Server, string) {
	t.Helper()
	s := linkHub(t)
	tok := hubRandom(32)
	hs.seedLink(s.installID, tok)
	if err := s.saveSiteLink(&siteLink{Token: tok, Site: siteReleaseBase(), LinkedAt: 1730000000, Handle: handle}); err != nil {
		t.Fatalf("saveSiteLink: %v", err)
	}
	return s, tok
}

// readSiteLinkFile is the credential as bytes on disk, for a claim about what a door left
// behind: an absent file reads as nil, and anything else must be readable to be evidence.
func readSiteLinkFile(t *testing.T, s *Server) []byte {
	t.Helper()
	b, err := os.ReadFile(s.siteLinkPath())
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		t.Fatalf("the credential file: %v", err)
	}
	return b
}

// linkStatusState asks the status door what state the link is in, the answer step 3's page
// is built on: never the token, always one of none / pending / linked / disconnected.
func linkStatusState(t *testing.T, s *Server) map[string]any {
	t.Helper()
	env := designCall(t, s, http.MethodGet, "/api/site/link-status", nil)
	if env["ok"] != true {
		t.Fatalf("link-status: %v", env)
	}
	d, _ := env["data"].(map[string]any)
	if d == nil {
		t.Fatalf("link-status has no data: %v", env)
	}
	if _, leaked := d["token"]; leaked {
		t.Fatalf("link-status answered with a token: %v", d)
	}
	return d
}

// The owner's design list is passed through and nothing else: the exact bearer on the one
// request the door makes, the site's document byte-for-byte inside the hub's envelope, and
// two asks for two reads because a list that arrives stale is worse than none.
func TestSiteDesignsProxiesAndSendsBearer(t *testing.T) {
	hs := hubSiteServer(t)
	hs.designBody = `{"designs":[{"id":"` + siteDesignID + `","app":"clock","name":"Plain Six","version":4,` +
		`"visibility":"private","updatedAt":"2026-11-10T09:00:00.000Z"},` +
		`{"id":"ZzYyXxWwVvUuTtSsRrQqPp","app":"weather","name":"Cloud","version":12,` +
		`"visibility":"unlisted","updatedAt":"2026-11-09T18:21:05.000Z"}]}`
	s, tok := linkedSiteHub(t, hs, "greg")
	rec := rawCall(t, s, http.MethodGet, "/api/site/designs", nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("GET /api/site/designs = %d %s", rec.Code, rec.Body)
	}
	if got, want := rec.Body.String(), `{"data":`+hs.designBody+`,"ok":true}`; got != want {
		t.Errorf("the list did not arrive byte-equal:\n got %s\nwant %s", got, want)
	}
	if got := hs.designBearer(0); got != tok {
		t.Errorf("the door sent bearer %q, want exactly the stored token", got)
	}
	if hs.count() != 1 {
		t.Errorf("one read made %d requests to the site, want one", hs.count())
	}
	// Freshness is the whole point of the door: the hub holds no list of its own, so a
	// second read is a second ask and not a replay of the first.
	if rec := rawCall(t, s, http.MethodGet, "/api/site/designs", nil); rec.Code != http.StatusOK {
		t.Fatalf("the second read = %d %s", rec.Code, rec.Body)
	}
	if got := hs.designAsks(); got != 2 {
		t.Errorf("two reads asked the site %d times, want twice", got)
	}
	if got := hs.designBearer(1); got != tok {
		t.Errorf("the second ask carried %q, want the same bearer", got)
	}
	if l := s.loadSiteLink(); l == nil || l.Token != tok {
		t.Errorf("a good read disturbed the credential: %+v", l)
	}
}

// A 401 is the site saying the link is over, and lead review point 5 says exactly what that
// leaves on disk: the file rewritten without its bearer and flagged disconnected, so the
// page still has a Disconnected to show and a Reconnect to offer after a restart. The
// sentence is the one the door answers with, and no later call dials with a dead bearer.
func TestSiteDesigns401ClearsTheLink(t *testing.T) {
	hs := hubSiteServer(t)
	s, tok := linkedSiteHub(t, hs, "greg")
	// The site-side Disconnect: the row dies while the hub still has the bearer in its file.
	hs.mu.Lock()
	delete(hs.owners, tok)
	delete(hs.links, s.installID)
	hs.mu.Unlock()
	if env := designCall(t, s, http.MethodGet, "/api/site/designs", nil); env["ok"] != false ||
		env["error"] != "disconnected from orb-os.net" {
		t.Fatalf("a refused bearer = %v, want the one disconnected sentence", env)
	}
	if hs.designAsks() != 1 {
		t.Errorf("the 401 made %d asks, want the one that earned it", hs.designAsks())
	}
	b := readSiteLinkFile(t, s)
	if b == nil {
		t.Fatal("the 401 deleted the file, so a restart has nothing left to say disconnected")
	}
	var l siteLink
	if err := json.Unmarshal(b, &l); err != nil {
		t.Fatalf("the rewritten file is not JSON: %s", b)
	}
	if l.Token != "" || !l.Disconnected || l.Handle != "greg" || l.LinkedAt != 1730000000 ||
		!strings.EqualFold(l.Site, siteReleaseBase()) {
		t.Errorf("the rewritten file = %+v, want {site, handle, linkedAt, disconnected: true} and no token", l)
	}
	if fi, err := os.Stat(s.siteLinkPath()); err != nil || fi.Mode().Perm() != 0o600 {
		t.Errorf("the rewritten file's mode = %v (%v), want the credential file's 0600", fi.Mode(), err)
	}
	if st := linkStatusState(t, s); st["state"] != "disconnected" || st["site"] != siteReleaseBase() ||
		st["handle"] != "greg" {
		t.Errorf("link-status after the 401 = %v, want disconnected with the site and the handle", st)
	}
	// Believed the moment it is written: the next read answers from the file, and the site is
	// asked nothing again by a hub that knows it has no bearer.
	if env := designCall(t, s, http.MethodGet, "/api/site/designs", nil); env["error"] != "disconnected from orb-os.net" {
		t.Errorf("the read after the disconnect = %v, want the same sentence", env)
	}
	if hs.designAsks() != 1 {
		t.Errorf("a hub with a disconnected file asked the site %d times, want once", hs.designAsks())
	}
	// And the bearer is gone rather than merely unread: the hub's own reader of the file
	// finds no link to send anywhere.
	if l := s.loadSiteLink(); l != nil {
		t.Errorf("the file still carries a live credential: %+v", l)
	}
}

// One call, one answer, no retry and no confusion: every failure that is not a 401 is the
// site having a bad day, and a hub whose credential still works must keep it. A stall, a
// redirect, a junk body and an answer too large to be a list all leave the token in place;
// only the 401 above costs the hub anything, and each of these is a different way for a
// working link to look broken.
func TestSiteDesignsKeepsTheTokenOnAnyOtherFailure(t *testing.T) {
	big, err := json.Marshal(map[string]any{"designs": []any{
		map[string]any{"id": siteDesignID, "name": strings.Repeat("n", siteDesignsBodyMax/2)},
		map[string]any{"id": siteDesignID, "name": strings.Repeat("m", siteDesignsBodyMax/2)},
	}})
	if err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		name  string
		setup func(t *testing.T, hs *hubSite)
	}{
		{"502", func(_ *testing.T, hs *hubSite) { hs.designDown(http.StatusBadGateway) }},
		{"500", func(_ *testing.T, hs *hubSite) { hs.designDown(http.StatusInternalServerError) }},
		{"403", func(_ *testing.T, hs *hubSite) { hs.designDown(http.StatusForbidden) }},
		{"302", func(_ *testing.T, hs *hubSite) { hs.designDown(http.StatusFound) }},
		{"a 200 that is not JSON", func(_ *testing.T, hs *hubSite) { hs.designBody = `<html>nope</html>` }},
		{"a 200 with no list in it", func(_ *testing.T, hs *hubSite) { hs.designBody = `{"design":"one"}` }},
		{"an answer over the cap", func(_ *testing.T, hs *hubSite) { hs.designBody = string(big) }},
		{"a stall past the deadline", func(t *testing.T, hs *hubSite) {
			old := siteCallTimeout
			t.Cleanup(func() { siteCallTimeout = old })
			siteCallTimeout = 75 * time.Millisecond
			hs.designStall(1, 400*time.Millisecond)
		}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			hs := hubSiteServer(t)
			s, tok := linkedSiteHub(t, hs, "greg")
			c.setup(t, hs)
			env := designCall(t, s, http.MethodGet, "/api/site/designs", nil)
			if env["ok"] != false || env["error"] != "orb-os.net did not answer" {
				t.Fatalf("%s: got %v, want the one not-answering sentence", c.name, env)
			}
			l := s.loadSiteLink()
			if l == nil || l.Token != tok {
				t.Errorf("%s: the credential did not survive: %+v", c.name, l)
			}
			if st := linkStatusState(t, s); st["state"] != "linked" {
				t.Errorf("%s: link-status = %v, want the link still live", c.name, st)
			}
		})
	}
}

// Disconnect from this side is the owner ending it, so the file goes rather than being
// flagged: nothing on disk may outlive a link the hub chose to drop. The site is told first
// and believed later — a DELETE that lands 200, 401 or nothing at all all end the same way,
// because the outcome the owner asked for is a hub that no longer carries a bearer.
func TestSiteDisconnectForgetsTheToken(t *testing.T) {
	t.Run("the site takes the DELETE", func(t *testing.T) {
		hs := hubSiteServer(t)
		s, tok := linkedSiteHub(t, hs, "greg")
		rec := rawCall(t, s, http.MethodPost, "/api/site/disconnect", nil)
		if rec.Code != http.StatusOK {
			t.Fatalf("POST /api/site/disconnect = %d %s, want 200", rec.Code, rec.Body)
		}
		if got, want := rec.Body.String(), `{"data":{"disconnected":true},"ok":true}`+"\n"; got != want {
			t.Errorf("the disconnect answered %s, want the envelope §5 words exactly", got)
		}
		if got := hs.revokes; len(got) != 1 || got[0] != tok {
			t.Errorf("the site saw DELETE /api/hub/token with %v, want the one bearer it had issued", got)
		}
		if code := hubBearer(t, hs, tok); code != http.StatusUnauthorized {
			t.Errorf("the site still answers the bearer live: %d", code)
		}
		if b := readSiteLinkFile(t, s); b != nil {
			t.Errorf("disconnect left a file behind: %s", b)
		}
		if st := linkStatusState(t, s); st["state"] != "none" {
			t.Errorf("link-status after the disconnect = %v, want none", st)
		}
		if env := designCall(t, s, http.MethodGet, "/api/site/designs", nil); env["error"] != "not connected to orb-os.net" {
			t.Errorf("the next read = %v, want the hub to say it is not connected", env)
		}
	})
	// A site that cannot be reached must not strand an owner who is done with it. The
	// credential is the hub's to forget whatever the site says, so this is the same answer
	// and the same empty disk with a call that never landed.
	t.Run("the site is unreachable", func(t *testing.T) {
		dead := httptest.NewServer(http.NotFoundHandler())
		deadURL := dead.URL
		dead.Close()
		t.Setenv("GORB_SITE", deadURL)
		s := linkHub(t)
		if err := s.saveSiteLink(&siteLink{Token: hubRandom(32), Site: deadURL, LinkedAt: 1730000000, Handle: "greg"}); err != nil {
			t.Fatal(err)
		}
		old := siteCallTimeout
		t.Cleanup(func() { siteCallTimeout = old })
		siteCallTimeout = 2 * time.Second
		env := designCall(t, s, http.MethodPost, "/api/site/disconnect", nil)
		if env["ok"] != true {
			t.Fatalf("disconnect from a dead site = %v, want the same ok answer it gives from a live one", env)
		}
		if b := readSiteLinkFile(t, s); b != nil {
			t.Errorf("an unreachable site kept the hub holding a credential: %s", b)
		}
	})
	// The door is idempotent, and it is quiet when it has nothing to do: no bearer stored
	// means no request sent, since a DELETE with no Authorization header is a hub dialing
	// out to report a link it never had.
	t.Run("nothing to forget", func(t *testing.T) {
		hs := hubSiteServer(t)
		s := linkHub(t)
		if env := designCall(t, s, http.MethodPost, "/api/site/disconnect", nil); env["ok"] != true {
			t.Fatalf("disconnect on a hub with no link = %v, want the ok it gives twice", env)
		}
		if env := designCall(t, s, http.MethodPost, "/api/site/disconnect", nil); env["ok"] != true {
			t.Fatalf("a second disconnect = %v, want the ok it gives twice", env)
		}
		if hs.count() != 0 {
			t.Errorf("a hub with no credential reached the site %d times", hs.count())
		}
		// A file already flagged disconnected by someone else's 401 has no bearer to send
		// and is exactly what disconnect is for: removed outright, so the page stops
		// offering Reconnect over a link the owner has just declined.
		if err := s.saveSiteLink(&siteLink{Site: siteReleaseBase(), Handle: "greg", Disconnected: true}); err != nil {
			t.Fatal(err)
		}
		if env := designCall(t, s, http.MethodPost, "/api/site/disconnect", nil); env["ok"] != true {
			t.Fatalf("disconnect on a disconnected file = %v, want the ok", env)
		}
		if b := readSiteLinkFile(t, s); b != nil {
			t.Errorf("a disconnect left the disconnected file behind: %s", b)
		}
		if hs.count() != 0 {
			t.Errorf("a bearerless disconnect reached the site %d times", hs.count())
		}
	})
}

// The two new doors answer one verb each, and a credential stored for another site is no
// credential at all: the hub says it is not connected rather than sending a bearer to the
// site the file happens to name, and it says so without dialing.
func TestSiteDesignsAndDisconnectDoorShapes(t *testing.T) {
	hs := hubSiteServer(t)
	s, tok := linkedSiteHub(t, hs, "greg")
	for _, c := range []struct{ method, path string }{
		{http.MethodPost, "/api/site/designs"},
		{http.MethodDelete, "/api/site/designs"},
		{http.MethodGet, "/api/site/disconnect"},
	} {
		if rec := rawCall(t, s, c.method, c.path, nil); rec.Code != http.StatusMethodNotAllowed {
			t.Errorf("%s %s = %d %s, want 405 from a mounted route", c.method, c.path, rec.Code, rec.Body)
		}
	}
	if got := hs.designAsks(); got != 0 {
		t.Errorf("the wrong verbs asked the site %d times, want none", got)
	}
	// A file naming a site this hub is not open to reads as no link, and the token in it
	// goes nowhere.
	other := httptest.NewServer(http.NotFoundHandler())
	t.Cleanup(other.Close)
	if err := s.saveSiteLink(&siteLink{Token: tok, Site: other.URL, LinkedAt: 1730000000, Handle: "greg"}); err != nil {
		t.Fatal(err)
	}
	if env := designCall(t, s, http.MethodGet, "/api/site/designs", nil); env["error"] != "not connected to orb-os.net" {
		t.Errorf("a file naming another site = %v, want the hub to say it is not connected", env)
	}
	if got := hs.count(); got != 0 {
		t.Errorf("a foreign-site credential produced %d asks, want none", got)
	}
	if st := linkStatusState(t, s); st["state"] != "none" {
		t.Errorf("link-status for a foreign-site file = %v, want none", st)
	}
	// The disconnect still works, because that answer is the owner's and never the site's.
	if env := designCall(t, s, http.MethodPost, "/api/site/disconnect", nil); env["ok"] != true {
		t.Errorf("disconnect on a foreign-site file = %v, want the ok", env)
	}
	if b := readSiteLinkFile(t, s); b != nil {
		t.Errorf("the disconnect left a file behind: %s", b)
	}
}
