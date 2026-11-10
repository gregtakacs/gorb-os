package main

// S2 (design §6.6, ruling 33): an app's editor lives on an origin of its own.
//
// Until now the hub served every editor under /editor/... on the same origin as its
// control API, and that made an editor's JavaScript a control-API caller: the page
// carried the SameSite=Strict cookie the browser attaches to its own origin's fetches,
// and originOK saw the origin it was asked to trust. One app's editor could flash the
// board, erase it, install apps or drive the LAN proxy. Editors run third-party bytes,
// so "same origin as the flasher" was never a safe shape — and A1, which runs outside
// any editor, makes the rule permanent: an editor may only ever reach its own API.
//
// This file is the other listener. It binds loopback on the control port + 1 and
// answers only <app>.localhost:<port>: one origin per app, so one app's editor cannot
// read another's pages or storage, and the control cookie (host 127.0.0.1) is never
// sent here. Its credential is a per-launch, per-app token derived from a key this
// process mints and never shares with the control side: knowing an editor token buys
// one app's editor API and nothing else. What is NOT here is the whole point — no
// cookie is ever set, no control credential is ever read, and no route outside the
// short list in routesEditorListener is answered at all.

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"net/url"
	"strconv"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

const (
	// editorTokenHeader is the editor API credential, sent by the page's fetch shim
	// (editorShim) so an editor's own same-origin fetch carries it without the app
	// being rewritten, and without a cookie a cross-site page could replay.
	editorTokenHeader = "X-Orb-Editor"
	// editorTokenQuery is the same token on the editor document's URL. An iframe src
	// cannot carry a header, and the document load is the one editor request that is
	// NOT same-origin to the page framing it.
	editorTokenQuery = "et"
)

// editorLabelMax bounds a host label. The slugs this hub knows are far shorter; the cap
// is here so a made-up Host cannot name a 4 KB label.
const editorLabelMax = 32

// editorMacSalt prefixes the HMAC message so an editor token is a signature over a
// domain this hub invented, not over a bare slug an unrelated MAC could be re-used for.
const editorMacSalt = "orb-editor\x00"

// editorHostLabel is an app slug as it appears in a hostname: the slug's underscores
// become hyphens, because a DNS label may not carry an underscore. color_picker is
// therefore color-picker.localhost, and the mapping back (a label's hyphens to
// underscores) is only ever a guess — which is why the token, not the label, names the
// app a request is scoped to.
func editorHostLabel(slug string) string {
	return strings.ReplaceAll(slug, "_", "-")
}

// editorOrigin is where this hub serves one app's editor. The port is the bound editor
// port, not the control one: a different port is a different origin and so different
// storage, even for two apps whose labels a browser would resolve alike.
func (s *Server) editorOrigin(slug string) string {
	return "http://" + editorHostLabel(slug) + ".localhost:" + strconv.Itoa(s.editorPort)
}

// editorToken mints one app's credential for this launch: slug + "." + 32 hex, an
// HMAC over the salted slug under a key that dies with the process. Per app, because
// the clock's token must not open the colour picker's API; per launch, because a token
// in a bookmarked page must not outlive the hub that handed it out. It is never the
// control token, and never derivable from it: the mapping is one-way.
func (s *Server) editorToken(slug string) string {
	mac := hmac.New(sha256.New, s.editorKey)
	mac.Write([]byte(editorMacSalt + slug))
	return slug + "." + hex.EncodeToString(mac.Sum(nil))[:32]
}

// editorTokenSlug is the check in the other direction: whose editor does this token
// open? The slug travels in the clear (it is a name, not a secret) and the hex is the
// signature over it, so a tampered token fails here rather than scoping a request to
// the app its own text names. Split at the LAST dot: a slug may not contain one.
func (s *Server) editorTokenSlug(tok string) (string, bool) {
	if len(s.editorKey) == 0 || tok == "" {
		return "", false
	}
	i := strings.LastIndexByte(tok, '.')
	if i <= 0 || i == len(tok)-1 {
		return "", false
	}
	slug := tok[:i]
	if !orbnet.AppStemOK(slug) {
		return "", false
	}
	if subtle.ConstantTimeCompare([]byte(tok), []byte(s.editorToken(slug))) != 1 {
		return "", false
	}
	return slug, true
}

// newEditorKey mints the hub's per-launch editor key. One HMAC key per process is
// enough for the same reason one control token is: everything derived from it is
// scoped to one app and to this launch, and a restart rotates both together.
func newEditorKey() []byte {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		log.Fatalf("orb-manager: cannot mint an editor key: %v", err)
	}
	return b
}

// editorCSP is the document policy every response on this listener carries, refusals
// included. The allowances are an editor's own: inline and eval scripts because the
// hosted page boots a React/Babel runtime that needs them, data:/blob: because a bake
// is built out of both, and 'self' for connect-src because the editor's API is its own
// origin. frame-ancestors names the hub page, the only thing allowed to show an editor.
func (s *Server) editorCSP() string {
	cp := strconv.Itoa(s.controlPort)
	return "default-src 'self'; script-src 'self' 'unsafe-inline' 'unsafe-eval' blob:; " +
		"style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; font-src 'self' data:; " +
		"media-src 'self' data: blob:; connect-src 'self' data: blob:; worker-src 'self' blob:; " +
		"frame-src 'self' blob: data:; object-src 'none'; base-uri 'self'; form-action 'self'; " +
		"frame-ancestors 'self' http://127.0.0.1:" + cp + " http://localhost:" + cp
}

// editorShim is the first script of every editor document. A page's fetches are what
// need the token, and no app author should have to know the hub has one: this wraps
// fetch and XMLHttpRequest once, on the page's own origin, and signs only the calls
// that stay on it. XHR is wrapped because the send flow uploads with it
// (theme@1/send.js): fetch will not tell a page how many body bytes the transport
// took, and the send's commit point needs that number. The token literal is
// json.Marshal'd, so it cannot break out of the script.
func editorShim(token string) []byte {
	lit, err := json.Marshal(token)
	if err != nil {
		return nil
	}
	return []byte("<script>(function(){var T=" + string(lit) +
		",O=location.origin,F=window.fetch,X=window.XMLHttpRequest" +
		",XO=X.prototype.open,XS=X.prototype.send;window.fetch=function(i,o){" +
		"var q=new Request(i,o);try{if(new URL(q.url,location.href).origin===O)" +
		"q.headers.set('" + editorTokenHeader + "',T);}catch(e){}" +
		"return F.call(this,q);};" +
		"X.prototype.open=function(m,u){var s=this;try{s.__ed=new URL(u,location.href).origin===O;}catch(e){s.__ed=false;}" +
		"return XO.apply(s,arguments);};" +
		"X.prototype.send=function(){var s=this;if(s.__ed){try{s.setRequestHeader('" + editorTokenHeader + "',T);}catch(e){}}" +
		"return XS.apply(s,arguments);};})();</script>")
}

// editorDoc hands an HTML document the editor listener serves the bytes it goes out as:
// itself, with the shim first in its head. The template files are NOT edited —
// host@1/page.html ships to orbs and to the migration runner, which refuses a template
// action it does not know — so the insertion happens in the rendered bytes. An empty
// slug means a request the guard never scoped, which is no document to sign.
func (s *Server) editorDoc(slug string, body []byte) []byte {
	if slug == "" {
		return body
	}
	return insertEditorShim(body, editorShim(s.editorToken(slug)))
}

// insertEditorShim puts the script after the first <head ...> tag, after the doctype
// when the document has no head, and at the very start when it has neither.
func insertEditorShim(body, shim []byte) []byte {
	if len(shim) == 0 {
		return body
	}
	at := editorTagEnd(body, "<head")
	if at < 0 {
		if at = editorDoctypeEnd(body); at < 0 {
			at = 0
		}
	}
	out := make([]byte, 0, len(body)+len(shim))
	out = append(out, body[:at]...)
	out = append(out, shim...)
	return append(out, body[at:]...)
}

// editorTagEnd finds the end of the first opening tag whose name starts with the given
// lower-cased prefix, as an index just past its '>'. A substring match is not enough:
// <header> would take the script into the page body, so the name must end here.
// Returns -1 when the tag is absent or never closes.
func editorTagEnd(body []byte, prefix string) int {
	low := bytes.ToLower(body)
	for at := 0; ; {
		j := bytes.Index(low[at:], []byte(prefix))
		if j < 0 || at+j+len(prefix) >= len(body) {
			return -1
		}
		k := at + j + len(prefix)
		switch c := body[k]; c {
		case '>', ' ', '\t', '\n', '\r', '/':
			if e := bytes.IndexByte(body[k:], '>'); e >= 0 {
				return k + e + 1
			}
			return -1
		}
		at = at + j + 1
	}
}

// editorDoctypeEnd is the end of a leading <!doctype ...> declaration, -1 when the
// document does not open with one. Only a leading doctype counts: one later in the file
// is content, not the document's own start.
func editorDoctypeEnd(body []byte) int {
	i := 0
	for i < len(body) && (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' || body[i] == '\r') {
		i++
	}
	lead := body[i:]
	if len(lead) < 9 || !bytes.Equal(bytes.ToLower(lead[:9]), []byte("<!doctype")) {
		return -1
	}
	if e := bytes.IndexByte(lead, '>'); e >= 0 {
		return i + e + 1
	}
	return -1
}

// handleEditorOrigin answers the hub page's question — where do I point this editor's
// iframe, and with what credential — so app.js never assembles an origin or a token
// itself. It is a control route, guarded like its neighbours, and the one door through
// which an editor token leaves the hub.
func (s *Server) handleEditorOrigin(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	slug := r.URL.Query().Get("app")
	if !orbnet.AppStemOK(slug) {
		s.jsonErr(w, "expected ?app=<app>")
		return
	}
	s.jsonOK(w, map[string]any{"origin": s.editorOrigin(slug), "token": s.editorToken(slug)})
}

// editorScope carries the request's scope slug — the app whose Host and credential this
// request passed — for the per-app checks s2-2 adds to the editor API's queries,
// tickets and bodies.
type editorScopeKey struct{}

func editorScopeOf(r *http.Request) string {
	if v, ok := r.Context().Value(editorScopeKey{}).(string); ok {
		return v
	}
	return ""
}

// editorHandler is the whole editor listener: one guard in front of one private mux
// holding nothing but the editor's own routes. Tests drive it directly with a request
// whose Host names an app.
func (s *Server) editorHandler() http.Handler {
	mux := http.NewServeMux()
	s.routesEditorListener(mux)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// The headers go on before the route runs, so a refusal is a response under the
		// same policy as an answer: a hostile page cannot read a 421 differently from a
		// document, and nothing served here can be framed into another origin's page.
		w.Header().Set("Content-Security-Policy", s.editorCSP())
		w.Header().Set("Referrer-Policy", "no-referrer")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		// No response on this listener ever sets a cookie, and the control credential is
		// not read here either — not by this guard, not by any handler below it. That is
		// the point of the port: an editor page cannot be the page that flashes an orb.
		slug, ok := s.editorGuard(w, r)
		if !ok {
			return
		}
		mux.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), editorScopeKey{}, slug)))
	})
}

// editorGuard is everything between the wire and a route: whose port is this, whose
// app, whose credential. It answers in the reject envelope, so the editor's own JS
// reads a reason rather than a bare status, and returns the scope slug when the request
// may proceed.
func (s *Server) editorGuard(w http.ResponseWriter, r *http.Request) (string, bool) {
	// 1. Host. The port answers ONLY <app>.localhost:<port>, which is also what makes a
	// rebinded name (a zone pointing *.evil.example at 127.0.0.1) fail here instead of
	// reading an editor's storage: the label is not a name to resolve, it is the scope.
	label, ok := editorHostLabelOf(r.Host, s.editorPort)
	if !ok {
		reject(w, r, http.StatusMisdirectedRequest,
			"this is Orb Manager's editor port: it answers only <app>.localhost:%d", s.editorPort)
		return "", false
	}
	p := r.URL.Path
	static := strings.HasPrefix(p, "/editor/") || strings.HasPrefix(p, "/runtime/")
	api := !static && (strings.HasPrefix(p, "/api/") || strings.HasPrefix(p, "/orb/"))
	if !static && !api {
		// Not an editor path at all. 404 rather than 403: this port has no route to
		// disclose, and a refusal would name one a probe then knows to try.
		http.NotFound(w, r)
		return "", false
	}
	if static && r.Method != http.MethodGet && r.Method != http.MethodHead {
		w.Header().Set("Allow", "GET, HEAD")
		reject(w, r, http.StatusMethodNotAllowed, "an editor's files are served by GET only")
		return "", false
	}
	origin := "http://" + label + ".localhost:" + strconv.Itoa(s.editorPort)
	slug := ""
	var okCred bool
	if api {
		slug, okCred = s.editorAPICredential(w, r, origin, label)
	} else {
		slug, okCred = s.editorDocCredential(w, r, origin, label)
	}
	if !okCred {
		return "", false
	}
	// 5. A path may only name the app the Host named. The comparison is in label space
	// because a Host cannot carry an underscore: color_picker's page asks for
	// /editor/203.0.113.7/color_picker/ on color-picker.localhost, and that is one app.
	if ps := editorPathSlug(p); ps != "" && editorHostLabel(ps) != label {
		reject(w, r, http.StatusNotFound, "not this app's editor")
		return "", false
	}
	return slug, true
}

// editorDocCredential decides an editor's file and runtime reads: the page's own
// sub-resources plus the document load itself. Any one of four proofs is enough — the
// token in the URL (the document, a cross-site iframe load that cannot carry a header),
// the token in the header (the shim's doing), a same-origin fetch marker, or a Referer
// saying this page asked for its own file. A hostile document on another origin can
// produce none of them, and a token for another app is no proof at all.
func (s *Server) editorDocCredential(w http.ResponseWriter, r *http.Request, origin, label string) (string, bool) {
	for _, tok := range []string{r.URL.Query().Get(editorTokenQuery), r.Header.Get(editorTokenHeader)} {
		if tok == "" {
			continue
		}
		if slug, ok := s.editorScoped(tok, label); ok {
			return slug, true
		}
	}
	if site := strings.ToLower(r.Header.Get("Sec-Fetch-Site")); site == "same-origin" {
		return editorScopeFromPathOrLabel(r.URL.Path, label), true
	}
	if r.Header.Get("Sec-Fetch-Site") == "" && editorRefersFrom(r, origin) {
		return editorScopeFromPathOrLabel(r.URL.Path, label), true
	}
	reject(w, r, http.StatusForbidden, "the editor's files load only inside its own page")
	return "", false
}

// editorAPICredential decides an editor API call. The header only, and only for this
// app: a token in the URL would land in a tile host's Referer, in the hub's request log
// and in session history, and an API call is never a navigation that needs such a
// channel. The origin checks come first, as requireControl orders them, so a request a
// browser was manipulated into sending is refused before its credential is weighed.
func (s *Server) editorAPICredential(w http.ResponseWriter, r *http.Request, origin, label string) (string, bool) {
	if site := r.Header.Get("Sec-Fetch-Site"); site != "" && !strings.EqualFold(site, "same-origin") {
		reject(w, r, http.StatusForbidden,
			"cross-origin request refused: the editor API answers only its own editor page")
		return "", false
	}
	if o := r.Header.Get("Origin"); o != "" && !editorSameOrigin(o, origin) {
		reject(w, r, http.StatusForbidden,
			"cross-origin request refused: the editor API answers only its own editor page")
		return "", false
	}
	slug, ok := s.editorScoped(r.Header.Get(editorTokenHeader), label)
	if !ok {
		reject(w, r, http.StatusForbidden,
			"missing or wrong "+editorTokenHeader+": this route answers only the editor page this hub opened")
		return "", false
	}
	return slug, true
}

// editorScoped is the one place a token becomes a scope: a valid signature, for the app
// the Host named. A clock token on color-picker.localhost is a wrong token rather than
// a right one in the wrong place, so it gets the answer a forgery gets.
func (s *Server) editorScoped(tok, label string) (string, bool) {
	slug, ok := s.editorTokenSlug(tok)
	if !ok || editorHostLabel(slug) != label {
		return "", false
	}
	return slug, true
}

// editorHostLabelOf reads the app label out of a Host header: exactly
// <label>.localhost:<port>, case-insensitive, the label a DNS-safe run of hyphenated
// lowercase alphanumerics. Anything else — an IP, a bare localhost, an external name,
// another port — is not an address this listener answers on.
func editorHostLabelOf(host string, port int) (string, bool) {
	suffix := ".localhost:" + strconv.Itoa(port)
	h := strings.ToLower(host)
	if !strings.HasSuffix(h, suffix) {
		return "", false
	}
	label := strings.TrimSuffix(h, suffix)
	if label == "" || len(label) > editorLabelMax {
		return "", false
	}
	for i := 0; i < len(label); i++ {
		if c := label[i]; !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '-') {
			return "", false
		}
	}
	return label, true
}

// editorPathSlug is the app a path names, "" when it names none. Both editor doors and
// the hub's own door are checked; a runtime or API path carries no app in its address,
// and is scoped by the Host alone.
func editorPathSlug(p string) string {
	segs := strings.Split(p, "/")
	// Both doors name the app in the same position, because both have a component
	// before it: /editor/<ip>/<slug>/… and /editor/hub/<slug>/… . The empty tail a
	// directory path leaves is why a name here needs a fifth segment, not a fourth.
	if len(segs) < 5 || segs[3] == "" {
		return ""
	}
	if strings.HasPrefix(p, "/editor/hub/") || strings.HasPrefix(p, "/editor/") {
		return segs[3]
	}
	return ""
}

// editorScopeFromPathOrLabel names the scope for an allowed load that carried no token.
// The path is the better evidence when it has one (the app's real slug, underscores and
// all); the label is what is left for a runtime file, whose scope is only ever "the page
// that asked".
func editorScopeFromPathOrLabel(p, label string) string {
	if ps := editorPathSlug(p); ps != "" && orbnet.AppStemOK(ps) {
		return ps
	}
	return label
}

// editorSameOrigin compares an Origin header with this request's editor origin. Both
// sides are scheme+host values; the fold is for a Host header typed in another case,
// not for a different name, and an Origin carrying a path is not an Origin at all.
func editorSameOrigin(origin, want string) bool {
	u, err := url.Parse(origin)
	if err != nil || u.Host == "" || u.Path != "" || u.RawQuery != "" {
		return false
	}
	return strings.EqualFold(u.Scheme+"://"+u.Host, want)
}

// editorRefersFrom is the no-Sec-Fetch-Site case: a browser that says nothing about the
// site relationship still says where the page that asked lives, and a Referer from
// another origin is not this editor. A missing Referer fails here, which is the right
// answer for curl and for a page that set no-referrer of its own.
func editorRefersFrom(r *http.Request, origin string) bool {
	ref := r.Header.Get("Referer")
	if ref == "" {
		return false
	}
	u, err := url.Parse(ref)
	if err != nil || u.Host == "" {
		return false
	}
	return strings.EqualFold(u.Scheme+"://"+u.Host, origin)
}

// editorOrbRoutes are the device routes an editor may reach through the hub's proxy:
// the three OS settings the map and weather tools read. Reads only, and exact paths: a
// POST to /home writes an orb, and /sd/ls walks a card the editor has no business
// listing. Everything else under /orb/ is answered 404 here even with a good token —
// the proxy's own allowlist is the second lock, not the first.
var editorOrbRoutes = map[string]bool{
	"/api/v1/home":   true,
	"/api/v1/places": true,
	"/api/v1/units":  true,
}

// routesEditorListener is the editor API: the routes an editor page genuinely calls,
// each one the SAME handler the control mux registers, with the guard in editorHandler
// standing in for requireControl. Nothing else is here. The hub page's own surface —
// /api/flash, /api/provision, the fleet, the library install, an orb command, a firmware
// upload — has no pattern on this mux, so a request for it is a 404 before any handler
// is named, and no editor's script can be the page that erases an orb.
func (s *Server) routesEditorListener(mux *http.ServeMux) {
	// The editor's own files, the runtime they build on, and the hub's copy of either.
	// These three are the existing doors unchanged: they parse their own paths, and the
	// guard has already proved the path names the Host's app.
	mux.Handle("/editor/hub/", editorMethod(http.MethodGet, s.serveHubEditor))
	mux.Handle("/editor/", editorMethod(http.MethodGet, s.serveEditor))
	mux.Handle("/runtime/", editorMethod(http.MethodGet, s.serveRuntime))

	// The library and design surface a theme editor drives. Each one is the SAME shared
	// handler the control mux registers, wrapped in the scope check this listener needs and
	// the hub page does not (the scope section below this function).
	mux.Handle("/api/library/entries", editorMethod(http.MethodGet, s.editorScopeApp(s.handleLibEntries)))
	mux.Handle("/api/library/design", editorMethod(http.MethodGet, s.editorScopeApp(s.handleLibDesign)))
	mux.Handle("/api/library/keep", editorMethod(http.MethodPost,
		s.editorScopeApp(s.editorScopeContainer(s.handleLibKeep))))
	mux.Handle("/api/library/forget", editorMethod(http.MethodPost, s.editorScopeApp(s.handleLibForget)))
	mux.Handle("/api/design/save", editorMethod(http.MethodPost, s.editorScopeDesignSave(s.handleDesignSave)))
	mux.Handle("/api/theme/design", editorMethod(http.MethodGet, s.editorScopeDesignAnswer(s.handleThemeDesign)))
	mux.Handle("/api/theme/stem", editorMethod(http.MethodGet, s.editorScopeSlugDefault(s.handleThemeStem)))
	mux.Handle("/api/editor/source", editorMethod(http.MethodGet, s.editorScopeSlug(s.handleEditorSource)))
	// The send door: an editor's own theme upload, and the progress line it paints.
	mux.Handle("/api/theme/upload", editorMethod(http.MethodPost, s.editorScopeContainer(s.handleThemeUpload)))
	mux.Handle("/api/theme/commit", editorMethod(http.MethodPost, s.handleThemeCommit))
	mux.Handle("/api/theme/upload-status", editorMethod(http.MethodGet, s.handleThemeSendStatus))
	mux.Handle("/api/orb/known", editorMethod(http.MethodGet, s.handleRememberedIP))
	// The site bundle an imported design bakes from, and the keep that files the bake.
	mux.Handle("/api/site/bundle", editorMethod(http.MethodGet, s.editorScopeTicket(s.handleSiteBundle)))
	mux.Handle("/api/site/keep", editorMethod(http.MethodPost, s.editorScopeTicket(s.handleSiteKeep)))
	// The map tool's source read and its tile relay. Setting the source stays a control
	// act: an editor may fetch tiles, not point the hub at a tile server of its own.
	mux.Handle("/api/maps/source", editorMethod(http.MethodGet, s.handleMapsSource))
	mux.Handle("/api/maps/fetch", editorMethod(http.MethodGet, s.handleMapsFetch))
	// Three device reads, through the proxy that still applies its own grant gate.
	mux.Handle("/orb/", editorMethod(http.MethodGet, s.editorOrb))
}

// editorOrb narrows /orb/ to the three reads an editor may relay before handing the
// request to the shared proxy. The proxy is kept whole — it is the code that knows the
// grant set, the lane and the body caps — so it is entered through a door this narrow
// rather than copied into a second shape that could drift from it.
func (s *Server) editorOrb(w http.ResponseWriter, r *http.Request) {
	route := "/" + strings.Trim(strings.TrimPrefix(r.URL.Path, "/orb/"), "/")
	segs := strings.Split(strings.Trim(route, "/"), "/")
	if len(segs) != 4 || !isIPv4(segs[0]) || segs[1] != "api" || segs[2] != "v1" ||
		!editorOrbRoutes["/"+segs[1]+"/"+segs[2]+"/"+segs[3]] {
		reject(w, r, http.StatusNotFound, "not an editor route")
		return
	}
	s.proxyOrb(w, r)
}

// editorMethod enforces the method a route answers on this listener, even where the
// shared handler is lenient about it. The control mux does not care what a POST does to
// a handler written for a GET; on a port any page's script can reach, the method is part
// of the grant, so a verb this route does not answer is refused here rather than
// arriving as a handler that happens to ignore verbs. A GET route also answers HEAD,
// which is the same request but for the body.
func editorMethod(method string, next http.HandlerFunc) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != method && !(method == http.MethodGet && r.Method == http.MethodHead) {
			w.Header().Set("Allow", method)
			reject(w, r, http.StatusMethodNotAllowed, "this editor route answers %s only", method)
			return
		}
		next(w, r)
	})
}

// --- S2-2: one editor, one app -------------------------------------------------------

// The guard proved who a request is; the scope layer below says what it may be about. The
// two are different questions and the answers come from different places: the credential is
// the token and the Host, and the scope is whatever the request itself names — a query, a
// body, a ticket, or the answer a handler is about to write.
//
// Without it, one app's editor holds a hub-wide API: clock's token would list flights'
// library, save a draft into flights' designs, bake a clock theme into flights' name and
// fetch the bundle an import for another app is waiting on. S2-1's origins stop one editor
// reading another's PAGES; this stops it using the shared DATA through its own door.
//
// Every refusal is 403 in the reject envelope, and every refusal says whose editor the
// request was signed in as, so an author sees a rule rather than a wall.

// editorScopeOther is the refusal for a request that named an app and named the wrong one.
func editorScopeOther(w http.ResponseWriter, r *http.Request, scope, app string) {
	reject(w, r, http.StatusForbidden,
		"this editor is the %s app's: it cannot reach the %s app's themes", scope, app)
}

// editorScopeNoApp is the refusal for a request that named no app at all. The hint names
// the parameter the route actually reads: ?app= for the library and design doors, ?slug=
// for the one door this hub already addresses by slug.
func editorScopeNoApp(w http.ResponseWriter, r *http.Request, scope, param string) {
	reject(w, r, http.StatusForbidden,
		"this editor is the %s app's: name its app (?%s=%s)", scope, param, scope)
}

// editorScopeApp requires ?app= and requires it to be this editor's own app. The handlers
// below still check ?app= themselves — they are shared with the control mux, where the hub
// page is trusted with every app — so this is a narrowing of one door, not a replacement.
func (s *Server) editorScopeApp(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		scope := editorScopeOf(r)
		app := r.URL.Query().Get("app")
		switch {
		case app == "":
			editorScopeNoApp(w, r, scope, "app")
		case app != scope:
			editorScopeOther(w, r, scope, app)
		default:
			next(w, r)
		}
	}
}

// editorBodyCapped reads a request body as far as the handler below may see it and puts the
// bytes back on the request. The limit is the handler's own, so a body the handler would
// already refuse as over-large reaches it exactly as over-large.
func editorBodyCapped(r *http.Request, limit int64) []byte {
	body, err := io.ReadAll(io.LimitReader(r.Body, limit+1))
	if err != nil && len(body) == 0 {
		r.Body = io.NopCloser(bytes.NewReader(nil))
		return nil
	}
	r.Body = io.NopCloser(bytes.NewReader(body))
	return body
}

// editorScopeContainer scopes a route whose body IS a theme container: the app the bake
// themes is read out of the container's own header, because that is the app the orb files it
// under. An unparsable body is passed through unread-and-unjudged: the handler below has a
// sentence for a damaged file that this layer would only overwrite with a vaguer one, and a
// precheck body is a head slice no full parse can pass.
func (s *Server) editorScopeContainer(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		scope := editorScopeOf(r)
		c, err := orbnet.ContainerName(editorBodyCapped(r, int64(orbnet.MaxThemeUpload)))
		if err != nil || len(c.Sections) == 0 {
			next(w, r)
			return
		}
		if c.Sections[0] != scope {
			editorScopeOther(w, r, scope, c.Sections[0])
			return
		}
		next(w, r)
	}
}

// editorScopeDesignSave is the ?app= check plus the one thing a saved design can lie about:
// its own envelope's "app". A draft written into another app's folder is a library entry
// that app will never list, and one filed under this app's name with another app's schema is
// a bake that refuses to open, so the request is refused before the write.
func (s *Server) editorScopeDesignSave(next http.HandlerFunc) http.HandlerFunc {
	return s.editorScopeApp(func(w http.ResponseWriter, r *http.Request) {
		scope := editorScopeOf(r)
		if app, ok := editorJSONTopApp(editorBodyCapped(r, int64(designMaxBytes))); ok && app != scope {
			editorScopeOther(w, r, scope, app)
			return
		}
		next(w, r)
	})
}

// editorJSONTopApp is the "app" of a JSON object, when it is a string. ok is false for a
// body that is not an object or does not name an app, which is a passthrough rather than a
// refusal: whose schema a broken document carries is the parser's answer to give.
func editorJSONTopApp(body []byte) (string, bool) {
	if len(body) == 0 {
		return "", false
	}
	var doc map[string]json.RawMessage
	if json.Unmarshal(body, &doc) != nil {
		return "", false
	}
	return siteJSONString(doc["app"])
}

// editorScopeDesignAnswer scopes the one editor route whose answer is another app's bytes:
// a design read out of a theme is that theme's app's design document, and the request only
// names a shelf stem or a card id. The answer is buffered, weighed, and either copied
// through unchanged or replaced by the refusal this request should have got.
func (s *Server) editorScopeDesignAnswer(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		scope := editorScopeOf(r)
		rec := &editorBuffer{hdr: http.Header{}}
		next(rec, r)
		if rec.code == http.StatusOK &&
			strings.HasPrefix(rec.hdr.Get("Content-Type"), "application/json") {
			if app, ok := editorJSONTopApp(rec.body); ok && app != scope {
				editorScopeOther(w, r, scope, app)
				return
			}
		}
		for k, vs := range rec.hdr {
			for _, v := range vs {
				w.Header().Add(k, v)
			}
		}
		w.WriteHeader(rec.code)
		_, _ = w.Write(rec.body)
	}
}

// editorBuffer is one handler's whole response held in memory, for the one editor route
// that has to see an answer before its caller does. It is not httptest.ResponseRecorder:
// that type belongs in tests, and this is in the binary.
type editorBuffer struct {
	hdr   http.Header
	code  int
	body  []byte
	wrote bool
}

func (b *editorBuffer) Header() http.Header { return b.hdr }

func (b *editorBuffer) WriteHeader(code int) {
	if !b.wrote {
		b.code, b.wrote = code, true
	}
}

func (b *editorBuffer) Write(p []byte) (int, error) {
	if !b.wrote {
		b.code, b.wrote = http.StatusOK, true
	}
	b.body = append(b.body, p...)
	return len(p), nil
}

// editorScopeSlug is the ?slug= form of the app check, for the door this hub has always
// addressed by slug. The route cannot answer at all without one, so a request that carried
// none is refused here rather than by the handler's usage line.
func (s *Server) editorScopeSlug(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		scope := editorScopeOf(r)
		slug := r.URL.Query().Get("slug")
		switch {
		case slug == "":
			editorScopeNoApp(w, r, scope, "slug")
		case slug != scope:
			editorScopeOther(w, r, scope, slug)
		default:
			next(w, r)
		}
	}
}

// editorScopeSlugDefault is the same check for the door that has a default: ?theme/stem with
// no slug means the clock to the handler, which is another app's answer as seen from any
// other editor, so the missing parameter is filled in with the scope rather than refused.
// What the caller asked for is left exactly as asked: only the absent case is rewritten.
func (s *Server) editorScopeSlugDefault(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		scope := editorScopeOf(r)
		q := r.URL.Query()
		switch slug := q.Get("slug"); {
		case slug == "":
			q.Set("slug", scope)
			r.URL.RawQuery = q.Encode()
			next(w, r)
		case slug != scope:
			editorScopeOther(w, r, scope, slug)
		default:
			next(w, r)
		}
	}
}

// editorScopeTicket scopes the two doors an import is read through, off the ticket store's
// own record of which app the bundle was fetched for. An unknown ticket is left to the
// handler, whose sentence is the one an editor reload is shown.
func (s *Server) editorScopeTicket(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		scope := editorScopeOf(r)
		if t, ok := s.tickets().get(r.URL.Query().Get("ticket")); ok && t.app != scope {
			reject(w, r, http.StatusForbidden,
				"this editor is the %s app's: that import is for the %s app", scope, t.app)
			return
		}
		next(w, r)
	}
}
