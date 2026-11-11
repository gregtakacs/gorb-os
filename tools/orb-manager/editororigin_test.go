package main

// S2 ruling 33: an app's editor lives on its own origin, on its own port, behind a
// credential that is neither the launch token nor derivable from it.
//
// The point of the second listener is a sentence about the browser, not about this code:
// an editor's bytes are third-party, and a page that runs third-party bytes must not
// share an origin with the thing that can flash a board. Everything below is machinery
// for that one sentence — who may ask this port (Host), who they ask as (token), which
// paths exist here at all (the route list), and what a document gets in return for
// living here (the fetch shim). These tests drive s.editorHandler() directly, which is
// what the listener's Serve would do, and name the Host header by hand, because the Host
// is the whole grant.

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
)

// editorAPICall hits an editor API route the way the app's own page does: Host, header,
// same-origin markers, nothing else. The scope slug comes from the Host, which is the
// only place an API path can get one.
func editorAPICall(t *testing.T, s *Server, method, path string) *httptest.ResponseRecorder {
	t.Helper()
	var body []byte
	if method != http.MethodGet && method != http.MethodHead {
		body = []byte(`{}`)
	}
	const slug = "clock"
	req := httptest.NewRequest(method, path, strings.NewReader(string(body)))
	req.Host = editorHostLabel(slug) + ".localhost:" + strconv.Itoa(s.editorPort)
	req.Header.Set(editorTokenHeader, s.editorToken(slug))
	req.Header.Set("Origin", s.editorOrigin(slug))
	req.Header.Set("Sec-Fetch-Site", "same-origin")
	rec := httptest.NewRecorder()
	s.editorHandler().ServeHTTP(rec, req)
	return rec
}

// isGuardRefusal is the guard's own vocabulary: the statuses only the guard or the route
// list can produce, which is what separates "this port has no such route" from "the
// handler answered".
func isGuardRefusal(rec *httptest.ResponseRecorder) bool {
	switch rec.Code {
	case http.StatusForbidden, http.StatusUnauthorized, http.StatusMisdirectedRequest,
		http.StatusMethodNotAllowed, http.StatusNotFound:
		return true
	}
	return false
}

// editorAppPath is an editor API path carrying ?app=clock. From S2-2 the scope layer behind
// the guard asks every library and design call whose app it is about, so a test that means
// to be asking about the credential has to name its app first — otherwise the refusal it
// reads is the scope rule, not the one under test.
func editorAppPath(path, q string) string {
	tail := "app=clock"
	if q != "" {
		tail += "&" + q
	}
	if strings.Contains(path, "?") {
		return path + "&" + tail
	}
	return path + "?" + tail
}

func editorJSONQuote(v string) string {
	b, err := json.Marshal(v)
	if err != nil {
		return v
	}
	return string(b)
}

// editorTokenWant is the documented derivation, written out here rather than called, so
// a change in the scheme has to be a decision in two files.
func editorTokenWant(key []byte, slug string) string {
	mac := hmac.New(sha256.New, key)
	mac.Write([]byte(editorMacSalt + slug))
	return slug + "." + hex.EncodeToString(mac.Sum(nil))[:32]
}

// Every app's editor lives on <app>.localhost on the editor port: one origin per app, so
// one app's localStorage, cache and service worker are not another app's, and a hub
// listening on a different port every launch never mixes an editor's storage across
// runs. Underscores become hyphens because a DNS label may not carry one, which is why
// the label is only ever an address and never the scope itself.
func TestEditorOriginIsPerAppAndLoopback(t *testing.T) {
	s := newHub()
	if s.editorPort == s.controlPort {
		t.Fatalf("editor port %d is the control port: one origin means one cookie jar, which is the thing S2 removes",
			s.editorPort)
	}
	cases := map[string]string{
		"clock":        "http://clock.localhost:" + strconv.Itoa(s.editorPort),
		"weather":      "http://weather.localhost:" + strconv.Itoa(s.editorPort),
		"color_picker": "http://color-picker.localhost:" + strconv.Itoa(s.editorPort),
	}
	for slug, want := range cases {
		if got := s.editorOrigin(slug); got != want {
			t.Errorf("editorOrigin(%q) = %q, want %q", slug, got, want)
		}
		if !strings.HasPrefix(s.editorOrigin(slug), "http://") ||
			!strings.HasSuffix(s.editorOrigin(slug), ".localhost:"+strconv.Itoa(s.editorPort)) {
			t.Errorf("editorOrigin(%q) = %q, want a loopback .localhost origin on the editor port", slug, s.editorOrigin(slug))
		}
	}
	if s.editorOrigin("clock") == s.editorOrigin("weather") {
		t.Error("two apps share an editor origin, so one can read the other's storage")
	}

	// The hub page has one way to ask where an editor lives, and it is a control route:
	// the launch token in, the origin and that app's token out.
	rec := controlGet(t, s, http.MethodGet, "/api/editor/origin?app=color_picker", true, nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("origin route: status %d body %s", rec.Code, rec.Body)
	}
	var env struct {
		Data struct {
			Origin string `json:"origin"`
			Token  string `json:"token"`
		} `json:"data"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
		t.Fatalf("origin route is not the hub's JSON envelope: %s", rec.Body)
	}
	if env.Data.Origin != s.editorOrigin("color_picker") {
		t.Errorf("origin = %q, want %q", env.Data.Origin, s.editorOrigin("color_picker"))
	}
	if env.Data.Token != s.editorToken("color_picker") {
		t.Error("the origin route handed out a token that is not this app's")
	}
	if env.Data.Token == s.token {
		t.Error("the origin route handed out the control token")
	}
	// No token, no answer: this route names ports and mints credentials.
	if rec := controlGet(t, s, http.MethodGet, "/api/editor/origin?app=clock", false, nil); rec.Code == http.StatusOK {
		t.Errorf("the origin route answered a request with no control token: status %d", rec.Code)
	}
	for _, app := range []string{"", "Bad%20Name", "no%20such", "..%2f.."} {
		if rec := controlGet(t, s, http.MethodGet, "/api/editor/origin?app="+app, true, nil); rec.Code == http.StatusOK {
			t.Errorf("origin?app=%q answered 200, want a refusal", app)
		}
	}
}

// One app's token opens that app and no other, and a token does not survive the launch
// that minted it. Both halves are load-bearing: per app is what makes the clock's
// credential useless against the colour picker, per launch is what makes a bookmark, a
// history entry and a captured page worth nothing the next time the hub runs.
func TestEditorTokenIsPerAppAndPerLaunch(t *testing.T) {
	s := newHub()
	clock, picker := s.editorToken("clock"), s.editorToken("color_picker")
	if clock == picker {
		t.Fatal("two apps were minted the same editor token")
	}
	for _, c := range []struct{ slug, tok string }{{"clock", clock}, {"color_picker", picker}} {
		if !strings.HasPrefix(c.tok, c.slug+".") {
			t.Errorf("token %q does not name its app in the clear", c.tok)
		}
		if n := len(c.tok) - len(c.slug) - 1; n != 32 {
			t.Errorf("token %q carries %d hex chars, want 32", c.tok, n)
		}
		if slug, ok := s.editorTokenSlug(c.tok); !ok || slug != c.slug {
			t.Errorf("editorTokenSlug(%q) = %q,%v, want %q,true", c.tok, slug, ok, c.slug)
		}
		if again := s.editorToken(c.slug); again != c.tok {
			t.Error("a token is not stable within one launch, so a page would be signed twice")
		}
		if c.tok != editorTokenWant(s.editorKey, c.slug) {
			t.Errorf("token = %q, want the documented HMAC shape %q", c.tok, editorTokenWant(s.editorKey, c.slug))
		}
	}
	// A second hub is a second launch: same app, different credential, different key.
	two := newHub()
	if two.editorToken("clock") == clock {
		t.Error("an editor token survived a restart, so a bookmarked page would still open the hub")
	}
	if string(two.editorKey) == string(s.editorKey) {
		t.Error("two launches minted the same editor key")
	}
	// A key that is gone mints nothing and checks nothing.
	empty := &Server{}
	if _, ok := empty.editorTokenSlug("clock." + strings.Repeat("0", 32)); ok {
		t.Error("a hub with no editor key accepted a token")
	}
	for _, bad := range []string{
		"", "clock", ".00000000000000000000000000000000", "clock.",
		"clock." + strings.Repeat("0", 32), "Clock." + strings.Repeat("0", 32),
		"clock.bad", "no.such.app." + strings.Repeat("0", 32),
	} {
		if slug, ok := s.editorTokenSlug(bad); ok {
			t.Errorf("editorTokenSlug(%q) = %q,true, want a refusal", bad, slug)
		}
	}
	if _, ok := s.editorTokenSlug(s.token); ok {
		t.Error("the control token verified as an editor token; the two must not be related")
	}
}

// The port answers on exactly one kind of address. This is the check that holds the
// listener's whole promise against a browser being pointed somewhere else: a page on
// another origin, a rebinded wildcard domain, a raw IP and the control port's own name
// all arrive here and all get the same short answer.
func TestEditorListenerRefusesForeignHosts(t *testing.T) {
	const ip = "203.0.113.7"
	s, _ := editorHub(t, ip, zipWith(t, map[string]string{"index.html": "<body>mine</body>"}), nil)
	port := strconv.Itoa(s.editorPort)
	doc := "/editor/" + ip + "/clock/index.html?" + editorTokenQuery + "=" + s.editorToken("clock")
	for _, h := range []string{
		"127.0.0.1:" + port,
		"localhost:" + port,
		"clock.localhost",
		"clock.localhost:" + strconv.Itoa(s.controlPort),
		"clock.localhost:9",
		"clock.evil.example:" + port,
		"clock.localhost.evil.example:" + port,
		"127.0.0.1.nip.io:" + port,
		".localhost:" + port,
		"clock.localhost." + port,
		"this-label-is-far-too-long-to-be-a-dns-label.localhost:" + port,
		"clock_live.localhost:" + port,
	} {
		req := httptest.NewRequest(http.MethodGet, doc, nil)
		req.Host = h
		rec := httptest.NewRecorder()
		s.editorHandler().ServeHTTP(rec, req)
		if rec.Code != http.StatusMisdirectedRequest {
			t.Errorf("Host %q: status %d, want %d", h, rec.Code, http.StatusMisdirectedRequest)
		}
		want := "this is Orb Manager's editor port: it answers only <app>.localhost:" + port
		if !strings.Contains(rec.Body.String(), want) {
			t.Errorf("Host %q: body %.200s, want it to say %q", h, rec.Body, want)
		}
		// A refusal is still a response from this listener, so it carries the same policy
		// as an answer: no sniffing, no referrer out, no framing outside the hub page.
		if csp := rec.Header().Get("Content-Security-Policy"); !strings.Contains(csp, "frame-ancestors 'self'") ||
			!strings.Contains(csp, "http://127.0.0.1:"+strconv.Itoa(s.controlPort)) {
			t.Errorf("Host %q: CSP %q, want the editor policy naming the hub page", h, csp)
		}
		if rp := rec.Header().Get("Referrer-Policy"); rp != "no-referrer" {
			t.Errorf("Host %q: Referrer-Policy %q, want no-referrer", h, rp)
		}
		if sn := rec.Header().Get("X-Content-Type-Options"); sn != "nosniff" {
			t.Errorf("Host %q: X-Content-Type-Options %q, want nosniff", h, sn)
		}
		if len(rec.Result().Cookies()) > 0 {
			t.Errorf("Host %q: a refusal on the editor port set a cookie", h)
		}
	}
	// A name that is well-formed but is not an app this hub knows passes the Host gate and
	// then fails the credential, which is the right order: this port answers any
	// <label>.localhost, and it is the token that says whose editor a request is for. A
	// zone rebinded to 127.0.0.1 therefore gets a refusal, not an editor.
	reqHost := httptest.NewRequest(http.MethodGet, doc, nil)
	reqHost.Host = "evil.localhost:" + port
	recHost := httptest.NewRecorder()
	s.editorHandler().ServeHTTP(recHost, reqHost)
	if recHost.Code != http.StatusForbidden {
		t.Errorf("a well-formed label with no credential: status %d, want 403", recHost.Code)
	}

	// The address it does answer, including the case a browser normalises away for it.
	for _, h := range []string{"clock.localhost:" + port, "CLOCK.localhost:" + port} {
		req := httptest.NewRequest(http.MethodGet, doc, nil)
		req.Host = h
		rec := httptest.NewRecorder()
		s.editorHandler().ServeHTTP(rec, req)
		if rec.Code != http.StatusOK {
			t.Errorf("Host %q: status %d body %.160s, want the document", h, rec.Code, rec.Body)
		}
	}
	// A path that is in none of the editor's classes is a 404 and names no route, even
	// with the app's own token in the header: this port has nothing to disclose.
	req := httptest.NewRequest(http.MethodGet, "/api/flash", nil)
	req.Host = "clock.localhost:" + port
	req.Header.Set(editorTokenHeader, s.editorToken("clock"))
	rec := httptest.NewRecorder()
	s.editorHandler().ServeHTTP(rec, req)
	if rec.Code != http.StatusNotFound {
		t.Errorf("a path off the editor's classes: status %d, want 404", rec.Code)
	}
}

// A document is the one editor request a browser makes by navigating, so it proves
// itself with the token in its URL — and every other app of this hub's is refused there.
// The control token is refused too, which is the point of the whole change: an editor
// page cannot reach the hub's power because it does not hold the hub's credential.
func TestEditorDocumentNeedsItsOwnToken(t *testing.T) {
	const ip = "203.0.113.7"
	s, _ := editorHub(t, ip, zipWith(t, map[string]string{
		"editor.json": `{"entry":"index.html"}`,
		"index.html":  "<body>clockdoc</body>",
		"assets/b.js": "var b",
	}), nil)
	docPath := "/editor/" + ip + "/clock/"
	for _, c := range []struct {
		name, q string
		want    bool
	}{
		{"its own token", editorTokenQuery + "=" + s.editorToken("clock"), true},
		{"no token at all", "", false},
		{"another app's token", editorTokenQuery + "=" + s.editorToken("weather"), false},
		{"the control token", editorTokenQuery + "=" + s.token, false},
		{"someone's guess", editorTokenQuery + "=clock." + strings.Repeat("a", 32), false},
	} {
		path := docPath
		if c.q != "" {
			path += "?" + c.q
		}
		rec := editorListenerGet(t, s, path, false, nil)
		if got := rec.Code == http.StatusOK; got != c.want {
			t.Errorf("%s: status %d body %.160s, want served=%v", c.name, rec.Code, rec.Body, c.want)
		}
		if rec.Code == http.StatusOK {
			editorAssertDocCredential(t, s, rec, "clock")
		}
	}
	// A token valid for one app does not open another app's Host: the signature scopes
	// the token and the Host scopes the request, and both have to agree.
	req := httptest.NewRequest(http.MethodGet, docPath, nil)
	req.Host = "weather.localhost:" + strconv.Itoa(s.editorPort)
	req.URL.RawQuery = editorTokenQuery + "=" + s.editorToken("clock")
	rec := httptest.NewRecorder()
	s.editorHandler().ServeHTTP(rec, req)
	if rec.Code == http.StatusOK {
		t.Error("the clock's token opened the weather editor")
	}
	// The fetch shim is the other half of the same fact: the app's token, this origin
	// only, and not a trace of the hub's own credential in the bytes an app can read.
	rec = editorListenerGet(t, s, docPath, true, nil)
	body := rec.Body.String()
	if !strings.Contains(body, editorTokenHeader) || !strings.Contains(body, "window.fetch") {
		t.Errorf("the document carries no fetch shim:\n%.400s", body)
	}
	if strings.Contains(body, s.token) {
		t.Error("the document carries the control token")
	}
	if !strings.Contains(body, s.editorToken("clock")) {
		t.Error("the document's shim does not carry this app's token")
	}
	// A sibling asset of the same app is served on the same four witnesses, and a token
	// for another app is none of them: see TestEditorSubresourcesLoadSameOriginOnly.
	if got := editorListenerGet(t, s, "/editor/"+ip+"/weather/assets/b.js", false,
		map[string]string{editorTokenHeader: s.editorToken("clock")}); got.Code == http.StatusOK {
		t.Error("weather's path was served with clock's token")
	}
}

// The files an editor page loads are its own, and the page that asked is proof enough —
// but only a page on the same origin. These are the four accepted witnesses and the
// shapes that must fail, including the one a hostile page can forge: a Referer of its
// own, with the fetch marker deliberately left out.
func TestEditorSubresourcesLoadSameOriginOnly(t *testing.T) {
	const ip = "203.0.113.7"
	s, _ := editorHub(t, ip, zipWith(t, map[string]string{
		"editor.json":  `{"entry":"index.html"}`,
		"index.html":   "<body>x</body>",
		"assets/b.js":  "var b",
		"assets/c.css": "body{}",
	}), nil)
	origin := s.editorOrigin("clock")
	file := "/editor/" + ip + "/clock/assets/b.js"
	cases := []struct {
		name string
		tok  bool
		hdr  map[string]string
		want bool
	}{
		{"token in the URL", true, nil, true},
		{"token in the header", false, map[string]string{editorTokenHeader: s.editorToken("clock")}, true},
		{"same-origin fetch", false, map[string]string{"Sec-Fetch-Site": "same-origin"}, true},
		{"referer from its own origin", false, map[string]string{"Referer": origin + "/index.html"}, true},
		{"no credential at all", false, nil, false},
		{"cross-site fetch", false, map[string]string{"Sec-Fetch-Site": "cross-site"}, false},
		{"cross-site fetch, own referer", false, map[string]string{"Sec-Fetch-Site": "cross-site", "Referer": origin + "/index.html"}, false},
		{"same-site fetch, own referer", false, map[string]string{"Sec-Fetch-Site": "same-site", "Referer": origin + "/index.html"}, false},
		{"referer from another origin", false, map[string]string{"Referer": "http://evil.example/x"}, false},
		{"referer from the control origin", false, map[string]string{"Referer": "http://127.0.0.1:" + strconv.Itoa(s.controlPort) + "/"}, false},
		{"another app's token", false, map[string]string{editorTokenHeader: s.editorToken("weather")}, false},
		{"the control token", false, map[string]string{editorTokenHeader: s.token}, false},
	}
	for _, c := range cases {
		rec := editorListenerGet(t, s, file, c.tok, c.hdr)
		if got := rec.Code == http.StatusOK; got != c.want {
			t.Errorf("%s: status %d body %.160s, want served=%v", c.name, rec.Code, rec.Body, c.want)
		}
		if rec.Code == http.StatusOK && !strings.Contains(rec.Body.String(), "var b") {
			t.Errorf("%s: served something that is not the file", c.name)
		}
	}
	// A runtime file is the same class of request with the same witnesses, and no app in
	// its address: the Host is the only scope such a request can be given.
	for _, c := range []struct {
		tok  bool
		hdr  map[string]string
		want bool
	}{
		{true, nil, true},
		{false, map[string]string{"Sec-Fetch-Site": "same-origin"}, true},
		{false, map[string]string{"Referer": origin + "/editor/" + ip + "/clock/index.html"}, true},
		{false, nil, false},
		{false, map[string]string{"Sec-Fetch-Site": "cross-site"}, false},
	} {
		rec := editorListenerGet(t, s, "/runtime/baker@1/orbtheme-baker.js", c.tok, c.hdr)
		if got := rec.Code == http.StatusOK; got != c.want {
			t.Errorf("runtime %v: status %d, want served=%v", c.hdr, rec.Code, c.want)
		}
	}
	// The class is enforced by verb as well as by witness: this port hands files out and
	// does not take anything back, whatever the URL says.
	for _, m := range []string{http.MethodPost, http.MethodPut, http.MethodPatch, http.MethodDelete} {
		req := httptest.NewRequest(m, file, strings.NewReader("{}"))
		req.Host = "clock.localhost:" + strconv.Itoa(s.editorPort)
		req.URL.RawQuery = editorTokenQuery + "=" + s.editorToken("clock")
		rec := httptest.NewRecorder()
		s.editorHandler().ServeHTTP(rec, req)
		if rec.Code != http.StatusMethodNotAllowed {
			t.Errorf("%s an editor file: status %d, want 405", m, rec.Code)
		}
		if a := rec.Header().Get("Allow"); !strings.Contains(a, http.MethodGet) || !strings.Contains(a, http.MethodHead) {
			t.Errorf("%s an editor file: Allow %q, want GET and HEAD", m, a)
		}
	}
	// HEAD is the same request minus the body, which is what a browser's cache check
	// sends, and it is answered on the same witnesses.
	head := httptest.NewRequest(http.MethodHead, file, nil)
	head.Host = "clock.localhost:" + strconv.Itoa(s.editorPort)
	head.Header.Set("Sec-Fetch-Site", "same-origin")
	rec := httptest.NewRecorder()
	s.editorHandler().ServeHTTP(rec, head)
	if rec.Code != http.StatusOK {
		t.Errorf("HEAD an editor asset: status %d, want 200", rec.Code)
	}
	// A path naming one app on another app's Host is refused even with the Host's own
	// witness: the file a path asks for is part of the grant, not a detail of the URL.
	cross := httptest.NewRequest(http.MethodGet, "/editor/"+ip+"/weather/assets/b.js", nil)
	cross.Host = "clock.localhost:" + strconv.Itoa(s.editorPort)
	cross.Header.Set("Sec-Fetch-Site", "same-origin")
	crossRec := httptest.NewRecorder()
	s.editorHandler().ServeHTTP(crossRec, cross)
	if crossRec.Code == http.StatusOK {
		t.Error("clock.localhost served the weather app's file")
	}
	// A runtime HTML file is a document on this listener, so it too carries the shim: the
	// rule is about what a page can do once loaded, not about which door it came through.
	doc := editorListenerGet(t, s, "/runtime/host@1/page.html", false, map[string]string{"Sec-Fetch-Site": "same-origin"})
	if doc.Code == http.StatusOK && !strings.Contains(doc.Body.String(), string(editorShim(s.editorToken("clock")))) {
		t.Error("a runtime document was served on the editor port without the fetch shim")
	}
}

// What lives on this port is a list, and everything else about the hub does not: the
// flasher, the provisioner, the library's install door, the fleet and an orb command are
// not patterns on this mux, so a good editor token does not reach them. This is the
// difference between a narrower surface and a smaller blast radius.
func TestEditorAPIOnlyTheListedRoutes(t *testing.T) {
	s := newHub()
	// The list, exactly as the plan names it, with the verb each answers. A wrong verb
	// proves the pattern is mounted here at all: an unmounted route cannot answer 405.
	listed := []struct{ method, path, wrong string }{
		{http.MethodGet, "/api/library/entries", http.MethodPost},
		{http.MethodGet, "/api/library/design", http.MethodPost},
		{http.MethodPost, "/api/design/save", http.MethodGet},
		{http.MethodPost, "/api/library/keep", http.MethodGet},
		{http.MethodPost, "/api/library/forget", http.MethodGet},
		{http.MethodGet, "/api/theme/design", http.MethodPost},
		{http.MethodGet, "/api/theme/stem", http.MethodPost},
		{http.MethodGet, "/api/editor/source", http.MethodPost},
		{http.MethodGet, "/api/orb/known", http.MethodPost},
		{http.MethodPost, "/api/theme/upload", http.MethodGet},
		{http.MethodPost, "/api/theme/commit", http.MethodGet},
		{http.MethodGet, "/api/theme/upload-status", http.MethodPost},
		{http.MethodGet, "/api/site/bundle", http.MethodPost},
		{http.MethodPost, "/api/site/keep", http.MethodGet},
		{http.MethodGet, "/api/maps/source", http.MethodPost},
		{http.MethodGet, "/api/maps/fetch", http.MethodPost},
	}
	for _, c := range listed {
		rec := editorAPICall(t, s, c.wrong, c.path)
		if rec.Code != http.StatusMethodNotAllowed {
			t.Errorf("%s %s: status %d body %.160s, want 405 from a mounted route", c.wrong, c.path, rec.Code, rec.Body)
		}
		if a := rec.Header().Get("Allow"); a != c.method {
			t.Errorf("%s %s: Allow %q, want %q", c.wrong, c.path, a, c.method)
		}
		// The right verb gets through the guard, whatever the handler then decides about
		// a body it was not given.
		if rec := editorAPICall(t, s, c.method, c.path); rec.Code == http.StatusMethodNotAllowed ||
			rec.Code == http.StatusMisdirectedRequest {
			t.Errorf("%s %s: status %d, want the handler to answer", c.method, c.path, rec.Code)
		}
	}
	// The device reads an editor may relay — three of them, exact — and the proxy's own
	// grant gate is what answers afterwards, which is the proof the handoff happened.
	for _, c := range []struct{ method, path string }{
		{http.MethodGet, "/orb/203.0.113.7/api/v1/home"},
		{http.MethodGet, "/orb/203.0.113.7/api/v1/places"},
		{http.MethodGet, "/orb/203.0.113.7/api/v1/units"},
	} {
		rec := editorAPICall(t, s, c.method, c.path)
		if strings.Contains(rec.Body.String(), "not an editor route") {
			t.Errorf("%s was refused by the editor's own list: %s", c.path, rec.Body)
		}
		if rec.Code == http.StatusNotFound {
			t.Errorf("%s: 404, want the proxy to answer for a route it knows", c.path)
		}
	}
	for _, c := range []struct {
		method, path string
		code         int
	}{
		{http.MethodGet, "/orb/203.0.113.7/api/v1/sd/ls", http.StatusNotFound},
		{http.MethodGet, "/orb/203.0.113.7/api/v1/ota", http.StatusNotFound},
		{http.MethodGet, "/orb/203.0.113.7/api/v1/status", http.StatusNotFound},
		{http.MethodGet, "/orb/203.0.113.7/api/v1/", http.StatusNotFound},
		{http.MethodGet, "/orb/203.0.113.7/", http.StatusNotFound},
		{http.MethodGet, "/orb/example/api/v1/home", http.StatusNotFound},
		{http.MethodGet, "/orb/203.0.113.7/api/v1/home/deeper", http.StatusNotFound},
		{http.MethodGet, "/orb/203.0.113.7/api/v2/home", http.StatusNotFound},
		{http.MethodPost, "/orb/203.0.113.7/api/v1/home", http.StatusMethodNotAllowed},
	} {
		rec := editorAPICall(t, s, c.method, c.path)
		if rec.Code != c.code {
			t.Errorf("%s %s: status %d body %.160s, want %d", c.method, c.path, rec.Code, rec.Body, c.code)
		}
	}
	// Everything the hub page can do, an editor cannot, whatever token it carries. A
	// hub power is not a pattern on this mux, so it is a 404 before a handler is named.
	for _, c := range []struct{ method, path string }{
		{http.MethodPost, "/api/flash"},
		{http.MethodPost, "/api/provision"},
		{http.MethodPost, "/api/library/install"},
		{http.MethodPost, "/api/library/delete"},
		{http.MethodPost, "/api/orb/cmd"},
		{http.MethodPost, "/api/orb/allow"},
		{http.MethodPost, "/api/orb/update"},
		{http.MethodPost, "/api/theme/erase"},
		// H2's site surface is a hub power too: an app's page cannot link the hub to an
		// account, read whose designs it holds or sign it out of one — and H4's upload
		// doors are the same kind of power, an account's write and the picker that
		// feeds it — so not one of these patterns is mounted on this mux at all.
		{http.MethodPost, "/api/site/connect"},
		{http.MethodGet, "/api/site/link-status"},
		{http.MethodGet, "/api/site/designs"},
		{http.MethodPost, "/api/site/disconnect"},
		{http.MethodPost, "/api/site/upload"},
		{http.MethodGet, "/api/site/local-designs"},
		{http.MethodGet, "/api/site/import"},
		{http.MethodGet, "/api/status"},
		{http.MethodGet, "/api/state"},
		{http.MethodGet, "/api/fleet"},
		{http.MethodGet, "/api/token"},
		{http.MethodGet, "/api/editor/origin?app=clock"},
		{http.MethodGet, "/api/"},
		{http.MethodGet, "/"},
	} {
		rec := editorAPICall(t, s, c.method, c.path)
		if rec.Code != http.StatusNotFound {
			t.Errorf("%s %s on the editor port: status %d body %.160s, want 404",
				c.method, c.path, rec.Code, rec.Body)
		}
	}
	// One shape of hub power is close enough to an editor route to be mounted here as a
	// read, and it stays a read: setting the tile server is a control act, so the verb
	// refusal is the answer rather than a route that does not exist.
	if rec := editorAPICall(t, s, http.MethodPost, "/api/maps/source"); rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("POST /api/maps/source: status %d, want 405", rec.Code)
	}
}

// The editor API answers one credential in one place. A token in the URL is not it (a
// query lands in a referer, a proxy log and the session history), the hub's own token is
// not it (that is the power this port exists to keep away), and an Origin or
// Sec-Fetch-Site saying another page asked is not it either.
func TestEditorAPIRefusesWithoutItsHeader(t *testing.T) {
	s := newHub()
	if rec := editorAPICall(t, s, http.MethodGet, editorAppPath("/api/library/entries", "")); isGuardRefusal(rec) {
		t.Fatalf("the baseline call with a good header was refused: %d %.200s", rec.Code, rec.Body)
	}
	tok := s.editorToken("clock")
	own := "http://clock.localhost:" + strconv.Itoa(s.editorPort)
	for _, c := range []struct {
		name string
		hdr  map[string]string
		q    string
	}{
		{"no header", nil, ""},
		{"the token in the URL", nil, editorTokenQuery + "=" + tok},
		{"the control token in the header", map[string]string{editorTokenHeader: s.token}, ""},
		{"another app's token", map[string]string{editorTokenHeader: s.editorToken("weather")}, ""},
		{"a forged token", map[string]string{editorTokenHeader: "clock." + strings.Repeat("f", 32)}, ""},
		{"an empty header", map[string]string{editorTokenHeader: ""}, ""},
		{"a good URL token and a junk header", map[string]string{editorTokenHeader: "clock.0"}, editorTokenQuery + "=" + tok},
		{"a cross-origin Origin", map[string]string{editorTokenHeader: tok, "Origin": "http://evil.example"}, ""},
		{"the control page's Origin", map[string]string{editorTokenHeader: tok, "Origin": "http://127.0.0.1:" + strconv.Itoa(s.controlPort)}, ""},
		{"a cross-site fetch marker", map[string]string{editorTokenHeader: tok, "Sec-Fetch-Site": "cross-site"}, ""},
		{"a same-site fetch marker", map[string]string{editorTokenHeader: tok, "Sec-Fetch-Site": "same-site"}, ""},
	} {
		req := httptest.NewRequest(http.MethodGet, editorAppPath("/api/library/entries", c.q), nil)
		req.Host = own[len("http://"):]
		for k, v := range c.hdr {
			req.Header.Set(k, v)
		}
		rec := httptest.NewRecorder()
		s.editorHandler().ServeHTTP(rec, req)
		if rec.Code != http.StatusForbidden {
			t.Errorf("%s: status %d body %.200s, want 403", c.name, rec.Code, rec.Body)
		}
		if !strings.Contains(rec.Body.String(), editorTokenHeader) &&
			!strings.Contains(rec.Body.String(), "cross-origin") {
			t.Errorf("%s: body %.200s, want a refusal naming the header or the origin", c.name, rec.Body)
		}
		if len(rec.Result().Cookies()) > 0 {
			t.Errorf("%s: an API refusal on the editor port set a cookie", c.name)
		}
	}
	// With the header, and the header alone, the same request goes through the guard: the
	// refusals above are about the credential, not about the port being closed.
	rec := editorAPICall(t, s, http.MethodPost, editorAppPath("/api/design/save", ""))
	if rec.Code == http.StatusForbidden || rec.Code == http.StatusMisdirectedRequest {
		t.Errorf("a properly signed call was refused: status %d body %.200s", rec.Code, rec.Body)
	}
	// And the same hub, reached from its own page with its own token, still answers the
	// control route this port refused: one credential did not replace the other.
	if rec := controlGet(t, s, http.MethodGet, "/api/library/entries", true, nil); isControlRefusal(rec) {
		t.Errorf("the control route for the same read: status %d body %.200s", rec.Code, rec.Body)
	}
}

// The hub's control origin served editors until now, and a route left behind would undo
// the change: sharing an origin with the flasher is exactly what an unreviewed app's
// bytes must not do. So this is a test that the old doors are 404, with the app's own
// credential presented, and that the hub page kept everything it genuinely loads.
func TestControlNoLongerServesEditors(t *testing.T) {
	const ip = "203.0.113.7"
	s, _ := editorHub(t, ip, zipWith(t, map[string]string{
		"editor.json": `{"entry":"index.html"}`,
		"index.html":  "<body>should not appear</body>",
		"assets/b.js": "var b",
	}), nil)
	for _, p := range []string{
		"/editor/" + ip + "/clock/",
		"/editor/" + ip + "/clock/index.html",
		"/editor/" + ip + "/clock/assets/b.js",
		"/editor/hub/weather/",
		"/editor/hub/clock/index.html",
		"/editor/",
	} {
		rec := controlGet(t, s, http.MethodGet, p, true, nil)
		if rec.Code != http.StatusNotFound {
			t.Errorf("control %s: status %d body %.160s, want 404: the hub origin must not serve an editor",
				p, rec.Code, rec.Body)
		}
		if strings.Contains(rec.Body.String(), "should not appear") {
			t.Errorf("control %s served the app's document", p)
		}
	}
	// Not even a POST, a HEAD or a token in a cookie reaches an editor's bytes by another
	// door now that the pattern is gone from the mux.
	for _, m := range []string{http.MethodPost, http.MethodHead, http.MethodPut} {
		if rec := controlGet(t, s, m, "/editor/"+ip+"/clock/", true, []byte("{}")); rec.Code != http.StatusNotFound {
			t.Errorf("control %s /editor/…: status %d, want 404", m, rec.Code)
		}
	}
	// What the control origin does keep: the hub page's own runtime files, guarded the way
	// they have always been, and the two editor routes a hub page needs to open one.
	if rec := controlGet(t, s, http.MethodGet, "/runtime/host@1/host.js", true, nil); rec.Code != http.StatusOK {
		t.Errorf("the hub page lost its runtime file: status %d", rec.Code)
	} else if strings.Contains(rec.Body.String(), "window.fetch") {
		t.Error("a runtime script on the control origin was given the editor's fetch shim")
	}
	if rec := controlGet(t, s, http.MethodGet, "/runtime/host@1/host.js", false, nil); rec.Code != http.StatusForbidden {
		t.Errorf("a control runtime file answered with no token: status %d, want 403", rec.Code)
	}
	if rec := controlGet(t, s, http.MethodGet, "/api/editor/source?ip="+ip+"&slug=clock", true, nil); rec.Code != http.StatusOK {
		t.Errorf("the control editor-source route: status %d body %.160s", rec.Code, rec.Body)
	}
	if rec := controlGet(t, s, http.MethodGet, "/api/editor/origin?app=clock", true, nil); rec.Code != http.StatusOK {
		t.Errorf("the control editor-origin route: status %d body %.160s", rec.Code, rec.Body)
	}
	// The two origins are genuinely different, so the 404s above are not one listener
	// answering for both: the hub page's address is an IP and an editor's is a label.
	if s.editorOrigin("clock") == "http://"+controlHostForTest(s) {
		t.Error("the editor origin is the control origin")
	}
	// And the routes do not cross the other way either: an editor port has no hub page,
	// no /web/ assets and no /api/editor/origin to read.
	for _, p := range []string{"/", "/web/index.html", "/api/editor/origin?app=clock"} {
		if rec := editorAPICall(t, s, http.MethodGet, p); rec.Code != http.StatusNotFound {
			t.Errorf("editor port %s: status %d, want 404", p, rec.Code)
		}
	}
}

// An app's editor page is the app's own bytes, and an app that ships a bare
// <body>…</body> with no head and no doctype is legal HTML and ordinary on this shelf.
// The shim still has to be there, because that page is the one that will call Render and
// it cannot reach the hub it was opened from without it — which is why the insertion is
// three cases deep rather than a string replace against a template nobody owns.
func TestLegacyEditorDocumentGetsTheShim(t *testing.T) {
	const ip = "203.0.113.7"
	s, _ := editorHub(t, ip, zipWith(t, map[string]string{
		"editor.json": `{"entry":"index.html"}`,
		"index.html":  "<body><h1>legacy</h1></body>",
		"assets/b.js": "var b",
	}), nil)
	const docPath = "/editor/" + ip + "/clock/"
	rec := editorListenerGet(t, s, docPath, true, nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("status %d body %s", rec.Code, rec.Body)
	}
	body := rec.Body.String()
	shim := string(editorShim(s.editorToken("clock")))
	i := strings.Index(body, shim)
	if i < 0 {
		t.Fatalf("a bare document got no shim:\n%s", body)
	}
	if i != 0 {
		t.Errorf("shim at %d of a headless, doctype-less document, want it first: %.200s", i, body)
	}
	if !strings.Contains(body, "<h1>legacy</h1>") {
		t.Error("the app's own bytes did not survive the insertion")
	}
	if strings.Count(body, shim) != 1 {
		t.Error("the shim was inserted more than once")
	}
	// The shim's own text is what an app's fetch becomes: one wrapper, the app's token as
	// a JSON string literal, this origin only, one header name, and the original fetch
	// still the thing that runs.
	for _, want := range []string{
		"var T=" + editorJSONQuote(s.editorToken("clock")),
		"O=location.origin",
		"window.fetch=function",
		"new Request(i,o)",
		"q.headers.set('" + editorTokenHeader + "',T)",
		"F.call(this,q)",
	} {
		if !strings.Contains(shim, want) {
			t.Errorf("the shim lacks %s:\n%s", want, shim)
		}
	}
	if strings.Contains(shim, tokenHeader) || strings.Contains(shim, s.token) {
		t.Error("the shim mentions the control credential it must never carry")
	}
	// A document with a head keeps it and takes the script inside, before the app's own.
	templ := `<!doctype html><html lang="en"><head><meta charset="utf-8"><title>T</title></head><body>b</body></html>`
	got := string(s.editorDoc("clock", []byte(templ)))
	want := `<!doctype html><html lang="en"><head>` + shim +
		`<meta charset="utf-8"><title>T</title></head><body>b</body></html>`
	if got != want {
		t.Errorf("shim placement in a documented page:\n got %s\nwant %s", got, want)
	}
	// A doctype with no head gets the script after the doctype, where a browser is already
	// parsing a document and an inline script still runs before any markup.
	got3 := string(s.editorDoc("clock", []byte("<!doctype html><b>x</b>")))
	if !strings.HasPrefix(got3, "<!doctype html>"+shim) {
		t.Errorf("doctype-only document: %.200s, want the shim after the doctype", got3)
	}
	// A sibling asset is served as the bytes it is: no script in a .js, and no second
	// copy of a credential an app could read out of its own file.
	js := string(editorListenerGet(t, s, "/editor/"+ip+"/clock/assets/b.js", true, nil).Body.Bytes())
	if strings.Contains(js, shim) || strings.Contains(js, "window.fetch") {
		t.Errorf("a sibling asset was scripted: %s", js)
	}
	// Stripping the one script the hub added leaves the archive's bytes, which is the
	// byte-for-byte claim extract_test.go makes for every app on the shelf.
	if !strings.Contains(string(editorBodyWithoutShim(t, s, "clock", rec)), "<h1>legacy</h1></body>") {
		t.Error("stripping the shim did not leave the app's document")
	}
	// The hosted page — the hub's own template, which no template file was edited for —
	// gets the same treatment on the same door.
	shelf, _, _ := hubShelfHub(t)
	hub := editorListenerGet(t, shelf, "/editor/hub/weather/", true, nil)
	if hub.Code != http.StatusOK {
		t.Fatalf("the hosted door: status %d body %.200s", hub.Code, hub.Body)
	}
	if !strings.Contains(hub.Body.String(), string(editorShim(shelf.editorToken("weather")))) {
		t.Error("the hosted editor page was served without the fetch shim")
	}
}

// isControlRefusal is the control guard's vocabulary only: a handler's own 400 or 500 is
// an answer, and this is used to check a control route was reached at all.
func isControlRefusal(rec *httptest.ResponseRecorder) bool {
	return rec.Code == http.StatusForbidden || rec.Code == http.StatusUnauthorized
}

func controlHostForTest(s *Server) string {
	return "127.0.0.1:" + strconv.Itoa(s.controlPort)
}
