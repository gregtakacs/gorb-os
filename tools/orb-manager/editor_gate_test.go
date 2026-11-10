package main

// S2 step 2: the gate tests for an editor that runs on its own origin.
//
// S2-1 moved an app's editor to <app>.localhost and gave it a per-app credential. These six
// are the consequences: what that credential may NOT open (1), what the control side must
// refuse even when a real credential is offered alongside it (2), what is not mounted on the
// editor port at all (3), the scope every editor API call must carry (4), which names the
// control port answers as (5), and who may frame the hub page (6).
//
// Each is written as a claim about the wire, not about a function: they drive the two
// listeners through the same handlers a browser reaches, and the interesting assertion in
// several of them is the absence of a dial — an orb that was never asked is the proof an
// upload or a command never got through.

import (
	"bytes"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// gateControlCall hits a control route as a browser on the hub's own machine does, with
// whatever headers the case cares to name — which is the whole point of tests 1 and 2, so the
// helper names none of them itself. The Host is loopback's, so the request reaches the handler
// the case is about rather than the name check, which has its own test.
func gateControlCall(t *testing.T, s *Server, method, path string, body []byte, hdr map[string]string) *httptest.ResponseRecorder {
	t.Helper()
	mux := http.NewServeMux()
	s.routes(mux)
	req := httptest.NewRequest(method, path, bytes.NewReader(body))
	req.Host = "127.0.0.1:" + strconv.Itoa(s.controlPort)
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)
	return rec
}

// gateEditorCall hits the editor listener as <slug>.localhost's own page does: Host, token and
// Origin all naming that app, and the body a POST carries. Nothing else comes along, so a case
// that adds a header is adding the one thing it is about.
func gateEditorCall(t *testing.T, s *Server, slug, method, path string, body []byte, hdr map[string]string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(method, path, bytes.NewReader(body))
	req.Host = editorHostLabel(slug) + ".localhost:" + strconv.Itoa(s.editorPort)
	req.Header.Set(editorTokenHeader, s.editorToken(slug))
	req.Header.Set("Origin", s.editorOrigin(slug))
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	rec := httptest.NewRecorder()
	s.editorHandler().ServeHTTP(rec, req)
	return rec
}

// gateRefused is the shape of every refusal in this file: the status, and a body that says why
// in the envelope every client here already parses.
func gateRefused(t *testing.T, rec *httptest.ResponseRecorder, code int, want, what string) {
	t.Helper()
	if rec.Code != code {
		t.Errorf("%s: status %d body %.200s, want %d", what, rec.Code, rec.Body, code)
		return
	}
	if !strings.Contains(rec.Body.String(), want) {
		t.Errorf("%s: body %.200s, want it to say %q", what, rec.Body, want)
	}
}

// gateDesignDoc is designDoc for a case that must name an app other than clock's: the same
// envelope, whose "app" is the field the scope layer reads.
func gateDesignDoc(name, app, ver string) []byte {
	return []byte(`{"format":"wc-design","schema":1,"app":"` + app + `","appVersion":"` + ver +
		`","made":"2026-11-04T00:00:00Z","dial":"skeleton","themeName":"` + name +
		`","assets":[{"id":"bg-1","dataUrl":"data:image/png;base64,AAAA"}],` +
		`"saved":{"name":"ignored","at":1,"extra":{"dial":"skeleton"}}}`)
}

// gateShelfTheme files a theme on the hub's own shelf, carrying a design whose envelope names
// designApp, and returns the stem it is filed under. This is how a test arranges a theme an
// editor could ask to reopen, without a card or a dial.
func gateShelfTheme(t *testing.T, s *Server, slug, name, designApp string) string {
	t.Helper()
	stem, err := orbnet.ThemeIDOf(slug, name)
	if err != nil {
		t.Fatal(err)
	}
	body := orbfake.Container(name)
	if designApp != "" {
		body = withDesign(body, string(gateDesignDoc(name, designApp, "v1.2.12")))
	}
	if err := os.WriteFile(filepath.Join(s.library().ThemesDir, stem+".orbtheme"), body, 0o644); err != nil {
		t.Fatal(err)
	}
	return stem
}

// gateRoute is one control route together with the answer the EDITOR listener owes for the
// same request, so the two halves of the surface are described in one place and cannot drift.
type gateRoute struct {
	method string
	path   string
	// editor is gateNoPattern when routesEditorListener carries no pattern for this path at
	// all, gateAnswered when it is one of this port's own routes and the shared handler is
	// what answers (what it says there is test 4's business), or the status this port's own
	// door returns for a path whose pattern it does carry.
	editor int
}

const (
	gateNoPattern = -1
	gateAnswered  = -2
)

// gateControlRoutes is every route registered with requireControl in routes() and the routesX
// functions it calls — as of this writing 55 of them, counted by
//
//	grep -rh 'mux.Handle("[^"]*", s.requireControl' *.go | wc -l
//
// and pinned below, so the day a control route is added this table fails loudly instead of
// quietly testing one route fewer. The two prefix registrations are sent as a concrete path
// each, since a bare "/orb/" is not a request anyone makes, and /orb/ is sent twice: a device
// read the proxy's own allowlist refuses, and a device write the method door refuses. ip is
// the orb the hub has been granted, so the proxy rows are real paths rather than placeholders.
func gateControlRoutes(t *testing.T, ip string) []gateRoute {
	t.Helper()
	routes := []gateRoute{
		{http.MethodPost, "/api/apps/import", gateNoPattern},
		{http.MethodGet, "/api/console", gateNoPattern},
		{http.MethodPost, "/api/design/save?app=clock", gateAnswered},
		{http.MethodGet, "/api/editor/origin?app=clock", gateNoPattern},
		{http.MethodGet, "/api/editor/source?slug=clock", gateAnswered},
		{http.MethodPost, "/api/firmware", gateNoPattern},
		{http.MethodPost, "/api/firmware/reset", gateNoPattern},
		{http.MethodPost, "/api/flash", gateNoPattern},
		{http.MethodPost, "/api/flash/cancel", gateNoPattern},
		{http.MethodPost, "/api/fleet/forget", gateNoPattern},
		{http.MethodPost, "/api/fleet/set", gateNoPattern},
		{http.MethodGet, "/api/library/design?app=clock", gateAnswered},
		{http.MethodGet, "/api/library/entries?app=clock", gateAnswered},
		{http.MethodPost, "/api/library/forget", gateAnswered},
		{http.MethodPost, "/api/library/install", gateNoPattern},
		{http.MethodPost, "/api/library/keep", gateAnswered},
		{http.MethodPost, "/api/library/push", gateNoPattern},
		{http.MethodPost, "/api/library/restore", gateNoPattern},
		{http.MethodPost, "/api/library/rm", gateNoPattern},
		{http.MethodPost, "/api/lonely", gateNoPattern},
		{http.MethodGet, "/api/maps/fetch", gateAnswered},
		{http.MethodGet, "/api/maps/source", gateAnswered},
		{http.MethodPost, "/api/orb/add", gateNoPattern},
		{http.MethodPost, "/api/orb/allow", gateNoPattern},
		{http.MethodPost, "/api/orb/cmd", gateNoPattern},
		{http.MethodPost, "/api/orb/forget", gateNoPattern},
		{http.MethodPost, "/api/orb/update", gateNoPattern},
		{http.MethodGet, "/api/orb/update/progress", gateNoPattern},
		{http.MethodGet, "/api/orb/where", gateNoPattern},
		{http.MethodPost, "/api/provision", gateNoPattern},
		{http.MethodGet, "/api/release/latest", gateNoPattern},
		{http.MethodGet, "/api/site/bundle", gateAnswered},
		{http.MethodPost, "/api/site/import", gateNoPattern},
		{http.MethodPost, "/api/site/keep", gateAnswered},
		{http.MethodGet, "/api/theme/apps", gateNoPattern},
		{http.MethodPost, "/api/theme/commit", gateAnswered},
		{http.MethodPost, "/api/theme/delete", gateNoPattern},
		{http.MethodGet, "/api/theme/design", gateAnswered},
		{http.MethodPost, "/api/theme/install", gateNoPattern},
		{http.MethodGet, "/api/theme/list?ip=" + ip, gateNoPattern},
		{http.MethodGet, "/api/theme/preview", gateNoPattern},
		{http.MethodPost, "/api/theme/rescan", gateNoPattern},
		{http.MethodGet, "/api/theme/shelf-preview", gateNoPattern},
		{http.MethodGet, "/api/theme/stem", gateAnswered},
		{http.MethodPost, "/api/theme/upload", gateAnswered},
		{http.MethodGet, "/api/theme/upload-status", gateAnswered},
		{http.MethodPost, "/api/theme/warm", gateNoPattern},
		{http.MethodGet, "/api/theme/warm-status", gateNoPattern},
		{http.MethodGet, "/api/updates", gateNoPattern},
		{http.MethodPost, "/api/updates/apply", gateNoPattern},
		{http.MethodPost, "/api/updates/keep", gateNoPattern},
		{http.MethodPost, "/api/updates/skip", gateNoPattern},
		{http.MethodGet, "/api/watch?ip=" + ip, gateNoPattern},
		{http.MethodGet, "/orb/" + ip + "/api/v1/status", http.StatusNotFound},
		{http.MethodPost, "/orb/" + ip + "/api/v1/sd/delete", http.StatusMethodNotAllowed},
		{http.MethodGet, "/runtime/maps@1/orbmap.js", gateAnswered},
	}
	if len(routes) != 56 {
		t.Fatalf("this table has %d rows for the hub's 55 control routes, want 56 (55 patterns, /orb/ sent twice): %s",
			len(routes), `grep -rh 'mux.Handle("[^"]*", s.requireControl' *.go | wc -l`)
	}
	return routes
}

// 1. The editor credential opens the editor API and nothing else, so the control side refuses
// it before it weighs any other credential on the request — and says which credential it saw,
// rather than reporting a control token as merely missing.
func TestEditorTokenOpensNoControlRoute(t *testing.T) {
	s, ip, _, orb := fakeOrbHubWith(t, orbfake.New())
	tok := s.editorToken("clock")
	// howEditorSends is the request a browser on <app>.localhost makes to the CONTROL port:
	// the editor's own Origin, the relationship a browser really records for a different port
	// on a host it counts as the same site, and no hub cookie — an editor document was never
	// given one. Each case below adds the one credential it is about.
	howEditorSends := func(extra map[string]string) map[string]string {
		h := map[string]string{
			"Origin":         s.editorOrigin("clock"),
			"Sec-Fetch-Site": "cross-site",
		}
		for k, v := range extra {
			h[k] = v
		}
		return h
	}
	// gateControlRoutes is every route the hub guards with requireControl, with what the
	// editor listener answers for the same request. Test 3 walks the same table.
	routes := gateControlRoutes(t, ip)
	before := len(orb.Calls())
	for _, c := range routes {
		what := c.method + " " + c.path
		rec := gateControlCall(t, s, c.method, c.path, []byte(`{}`),
			howEditorSends(map[string]string{editorTokenHeader: tok}))
		gateRefused(t, rec, http.StatusForbidden, "editor API", what+" with the editor header")
		// (b) The editor's token written into the CONTROL header, which is what a page that
		// guessed the two secrets were interchangeable sends. Which of the two guards catches
		// it is an implementation detail — the origin check sees an editor Origin first, the
		// token compare would catch the rest — so this pins the status, not the sentence.
		swapped := gateControlCall(t, s, c.method, c.path, []byte(`{}`),
			howEditorSends(map[string]string{tokenHeader: tok}))
		if swapped.Code != http.StatusForbidden {
			t.Errorf("%s with the editor token in the control header: status %d body %.200s, want 403",
				what, swapped.Code, swapped.Body)
		}
		// Signed with BOTH credentials it is still refused, and the refusal has to name the
		// editor credential: honouring the stronger of two credentials on a request would turn
		// a mislabeled call into a hub-wide one, and report it as a missing token.
		both := gateControlCall(t, s, c.method, c.path, []byte(`{}`),
			howEditorSends(map[string]string{tokenHeader: s.token, editorTokenHeader: tok}))
		gateRefused(t, both, http.StatusForbidden, "editor API", what+" signed with both tokens")
		if strings.Contains(both.Body.String(), "reload the Orb Manager page") {
			t.Errorf("%s: %.200s, want a refusal naming the editor credential, not a missing control token",
				what, both.Body)
		}
		// (c) The same shape in a URL, where an editor document carries its token.
		sep := "?"
		if strings.Contains(c.path, "?") {
			sep = "&"
		}
		q := gateControlCall(t, s, c.method, c.path+sep+editorTokenQuery+"="+tok, []byte(`{}`),
			howEditorSends(nil))
		gateRefused(t, q, http.StatusForbidden, "editor API", what+" with the editor token in the URL")
	}
	if after := len(orb.Calls()); after != before {
		t.Errorf("the orb saw %d calls across a pass over every control route with an editor credential, want none: %v",
			after-before, orb.Calls()[before:])
	}
	// The counter works: the hub's own read of the same device does dial, so the silence above
	// is the guards refusing and not a double wired to nothing.
	before = len(orb.Calls())
	if rec := controlGet(t, s, http.MethodGet, "/orb/"+ip+"/api/v1/home", true, nil); rec.Code != http.StatusOK {
		t.Fatalf("the hub's own device read: status %d body %.200s", rec.Code, rec.Body)
	}
	if len(orb.Calls()) == before {
		t.Error("the control proxy dialed nothing, so the no-dial claim above is not evidence")
	}
	// The gate is about the credential, not about the routes: the same reads with the hub's own
	// token are answered, by header or by cookie.
	for _, c := range []struct{ method, path string }{
		{http.MethodGet, "/api/editor/origin?app=clock"},
		{http.MethodGet, "/api/library/entries?app=clock"},
	} {
		if rec := controlGet(t, s, c.method, c.path, true, nil); isControlRefusal(rec) {
			t.Errorf("the control route %s with its own token: status %d body %.200s", c.path, rec.Code, rec.Body)
		}
		cookied := gateControlCall(t, s, c.method, c.path, nil,
			map[string]string{"Cookie": tokenCookie + "=" + s.token})
		if isControlRefusal(cookied) {
			t.Errorf("the control route %s with the hub page's cookie: status %d body %.200s", c.path, cookied.Code, cookied.Body)
		}
	}
	if rec := gateEditorCall(t, s, "clock", http.MethodGet, "/api/library/entries?app=clock", nil, nil); rec.Code == http.StatusForbidden {
		t.Errorf("the editor API with its own token: status %d body %.200s, want the handler to answer", rec.Code, rec.Body)
	}
}

// 2. An editor origin is not the hub's origin, whatever it presents. This is the hole the
// separate port exists to close, and the reason "same-site" left the accepted set: browsers
// compare a SITE, host minus port, so clock.localhost and 127.0.0.1 are neighbours to them,
// and a site-blind rule hands an app's page the control API as soon as it can attach a cookie
// meant for the hub. Every call here carries the real control credential in both channels, so
// the only thing that differs from a working call is where the request says it came from.
func TestEditorOriginRefusedEvenWithTheControlCookie(t *testing.T) {
	s := newHub()
	hub := "http://" + controlHostForTest(s)
	edited := s.editorOrigin("clock")
	strong := func(hdr map[string]string) map[string]string {
		out := map[string]string{tokenHeader: s.token, "Cookie": tokenCookie + "=" + s.token}
		for k, v := range hdr {
			out[k] = v
		}
		return out
	}
	for _, c := range []struct {
		name string
		hdr  map[string]string
	}{
		{"the editor page's Origin", map[string]string{"Origin": edited}},
		// The port is what makes an editor origin an editor origin; a name that is not the
		// hub's is not the hub's whatever it claims about a port.
		{"an Origin whose port is the hub's", map[string]string{"Origin": "http://clock.localhost:" + strconv.Itoa(s.controlPort)}},
		{"a same-site marker, which S2 stopped accepting", map[string]string{"Sec-Fetch-Site": "same-site"}},
		{"a cross-site marker", map[string]string{"Sec-Fetch-Site": "cross-site"}},
		{"a cross-origin marker", map[string]string{"Sec-Fetch-Site": "cross-origin"}},
		{"no marker and a stranger's Origin", map[string]string{"Origin": "http://evil.example:3779"}},
		{"an editor page's Referer and no Origin", map[string]string{"Referer": edited + "/index.html"}},
	} {
		for _, route := range []struct{ method, path string }{
			{http.MethodPost, "/api/flash"},
			{http.MethodGet, "/api/library/entries?app=clock"},
		} {
			rec := gateControlCall(t, s, route.method, route.path, []byte(`{"ip":"203.0.113.7"}`), strong(c.hdr))
			gateRefused(t, rec, http.StatusForbidden, "cross-origin", route.method+" "+route.path+" from "+c.name)
		}
	}
	// The hub page itself — same origin, same port, cookie attached — is unaffected: this
	// removed one accepted relationship and not the one that matters.
	if rec := gateControlCall(t, s, http.MethodGet, "/api/library/entries?app=clock", nil,
		strong(map[string]string{"Sec-Fetch-Site": "same-origin", "Origin": hub})); isControlRefusal(rec) {
		t.Errorf("a same-origin control call: status %d body %.200s", rec.Code, rec.Body)
	}
	if rec := controlGet(t, s, http.MethodPost, "/api/fleet/set", true,
		[]byte(`{"ip":"203.0.113.7","name":"Hall"}`)); isControlRefusal(rec) {
		t.Errorf("the hub page's own mutating call: status %d body %.200s", rec.Code, rec.Body)
	}
	// And clock's editor, addressed as what it is, still reaches its own API: the refusals
	// above are about crossing origins, not about an app's page being locked out.
	if rec := gateEditorCall(t, s, "clock", http.MethodGet, "/api/library/entries?app=clock", nil, nil); rec.Code == http.StatusForbidden {
		t.Errorf("clock's own editor API call: status %d body %.200s", rec.Code, rec.Body)
	}
}

// 3. The editor port answers a list of routes and nothing else, so a control call signed with
// an editor token is not refused by a policy — there is no route there to refuse it. That is
// the difference between a narrower surface and a smaller blast radius: a route missing from
// one mux cannot be reached by a credential that got it wrong, and cannot drift back into
// existence by editing a guard. The walk is over the same table test 1 used, minus the rows
// this port legitimately answers, which are named in the table rather than left out of it. The
// dial counter is the second half of the claim, so the test ends by dialing on purpose — a
// counter that never moves proves nothing.
func TestEditorListenerServesNoControlRoute(t *testing.T) {
	s, ip, _, orb := fakeOrbHubWith(t, orbfake.New())
	before := len(orb.Calls())
	asked, answered := 0, 0
	for _, c := range gateControlRoutes(t, ip) {
		if c.editor == gateAnswered {
			answered++
			continue
		}
		want := c.editor
		if want == gateNoPattern {
			want = http.StatusNotFound
		}
		asked++
		rec := gateEditorCall(t, s, "clock", c.method, c.path, []byte(`{}`), nil)
		if rec.Code != want {
			t.Errorf("%s %s on the editor port: status %d body %.200s, want %d",
				c.method, c.path, rec.Code, rec.Body, want)
		}
	}
	if asked != 40 || answered != 16 {
		t.Errorf("this walk covered %d control routes and passed over %d the port answers itself, want 40 and 16",
			asked, answered)
	}
	// The rest of what this port must not answer: the hub page's own files and a device read
	// the proxy allowlist does not carry, none of them in the control table above.
	for _, c := range []struct {
		method, path string
		code         int
	}{
		{http.MethodGet, "/api/orb/ls?ip=" + ip, http.StatusNotFound},
		{http.MethodGet, "/orb/" + ip + "/api/v1/sd/ls", http.StatusNotFound},
		{http.MethodGet, "/orb/" + ip + "/api/v1/ota", http.StatusNotFound},
		{http.MethodPost, "/orb/" + ip + "/api/v1/home", http.StatusMethodNotAllowed},
		{http.MethodGet, "/", http.StatusNotFound},
		{http.MethodGet, "/index.html", http.StatusNotFound},
		{http.MethodGet, "/app.js", http.StatusNotFound},
	} {
		rec := gateEditorCall(t, s, "clock", c.method, c.path, []byte(`{}`), nil)
		if rec.Code != c.code {
			t.Errorf("%s %s on the editor port: status %d body %.200s, want %d",
				c.method, c.path, rec.Code, rec.Body, c.code)
		}
	}
	if after := len(orb.Calls()); after != before {
		t.Errorf("the orb saw %d calls across a pass over the control routes, want none: %v",
			after-before, orb.Calls()[before:])
	}
	// One of the three device reads an editor MAY relay does reach the board, which is the
	// counter working: the silence above is about the routes, not about the fake.
	if rec := gateEditorCall(t, s, "clock", http.MethodGet, "/orb/"+ip+"/api/v1/home", nil, nil); rec.Code != http.StatusOK {
		t.Fatalf("GET /orb/%s/api/v1/home: status %d body %.200s, want the allowlisted read relayed", ip, rec.Code, rec.Body)
	}
	if len(orb.Calls()) == before {
		t.Error("the allowlisted device read did not dial, so the no-dial claim above is not evidence")
	}
	// The hub page still holds every route the editor port refused: what narrowed is the
	// editor's reach, not the hub's.
	for _, c := range []struct{ method, path string }{
		{http.MethodGet, "/api/editor/origin?app=clock"},
		{http.MethodGet, "/api/theme/list?ip=" + ip},
		{http.MethodGet, "/api/updates"},
		{http.MethodGet, "/api/library/entries?app=clock"},
	} {
		if rec := controlGet(t, s, c.method, c.path, true, nil); isControlRefusal(rec) {
			t.Errorf("the control route %s: status %d body %.200s", c.path, rec.Code, rec.Body)
		}
	}
}

// gateLibHub is a hub with a shelf, a card and a dial counter: enough to arrange a theme an
// editor could ask about, and enough to see whether an orb was reached.
func gateLibHub(t *testing.T) (*Server, string, *orbfake.Orb) {
	t.Helper()
	s, ip, _, orb := fakeOrbHubWith(t, orbfake.New())
	root := t.TempDir()
	s.lib = &Library{AppsDir: filepath.Join(root, "apps"), ThemesDir: filepath.Join(root, "themes")}
	if err := os.MkdirAll(s.lib.ThemesDir, 0o755); err != nil {
		t.Fatal(err)
	}
	return s, ip, orb
}

// 4. The guard proves who a request is; this is the other question, what it may be about. The
// credential is per-app, so the subject must be too: one app's editor holds its own library,
// its own drafts, its own bakes and its own imports, and the hub's shared data is reached
// through a scope this request either carries or does not. The four places a subject can hide
// are all here — a query, a body, a ticket, and the answer a handler was about to write.
func TestEditorAPIScopeIsItsApp(t *testing.T) {
	s, ip, orb := gateLibHub(t)
	const (
		own      = "clock"
		other    = "weather"
		otherMsg = "this editor is the clock app's: it cannot reach the weather app's themes"
		ownMsg   = "this editor is the clock app's: name its app"
		// flippedMsg is otherMsg for a request signed as the other app's editor: the rule
		// reads the same from both sides of the pair.
		flippedMsg = "this editor is the weather app's: it cannot reach the clock app's themes"
	)
	dials := func() int { return len(orb.Calls()) }

	// A call that names no app is refused, and says which parameter to fill in.
	for _, c := range []struct{ method, path string }{
		{http.MethodGet, "/api/library/entries"},
		{http.MethodGet, "/api/library/design"},
		{http.MethodPost, "/api/library/keep"},
		{http.MethodPost, "/api/library/forget"},
		{http.MethodPost, "/api/design/save"},
	} {
		rec := gateEditorCall(t, s, own, c.method, c.path, []byte(`{}`), nil)
		gateRefused(t, rec, http.StatusForbidden, ownMsg, c.method+" "+c.path+" with no app")
	}
	// A call that names the other app is refused by name, whichever way the pair runs.
	for _, c := range []struct{ method, path string }{
		{http.MethodGet, "/api/library/entries?app=" + other},
		{http.MethodGet, "/api/library/design?app=" + other + "&name=Sunny"},
		{http.MethodPost, "/api/library/keep?app=" + other},
		{http.MethodPost, "/api/library/forget?app=" + other},
		{http.MethodPost, "/api/design/save?app=" + other},
	} {
		rec := gateEditorCall(t, s, own, c.method, c.path, []byte(`{}`), nil)
		gateRefused(t, rec, http.StatusForbidden, otherMsg, c.method+" "+c.path)
	}
	// The same list signed as weather's editor, naming clock's app: the rule is an editor's
	// own app, not one app being privileged.
	gateRefused(t, gateEditorCall(t, s, other, http.MethodGet, "/api/library/entries?app=clock", nil, nil),
		http.StatusForbidden, flippedMsg,
		"weather's editor asking for clock's library")
	for _, slug := range []string{own, other} {
		rec := gateEditorCall(t, s, slug, http.MethodGet, "/api/library/entries?app="+slug, nil, nil)
		if rec.Code == http.StatusForbidden {
			t.Errorf("%s's own library call: status %d body %.200s", slug, rec.Code, rec.Body)
		}
	}

	// A design document may not be filed under one app while naming another: a write is the
	// kind of damage that outlives the request.
	good := gateEditorCall(t, s, own, http.MethodPost, "/api/design/save?app=clock&name=Sunny",
		gateDesignDoc("Sunny", "clock", "v1.2.12"), nil)
	if good.Code == http.StatusForbidden {
		t.Errorf("clock's own draft save: status %d body %.200s", good.Code, good.Body)
	}
	lying := gateEditorCall(t, s, own, http.MethodPost, "/api/design/save?app=clock&name=Lies",
		gateDesignDoc("Lies", other, "v1.2.12"), nil)
	gateRefused(t, lying, http.StatusForbidden, otherMsg, "a draft whose envelope names another app")
	if es := entriesOf(t, s, own); es["Lies"].Draft != nil {
		t.Error("the refused draft was filed anyway")
	}
	if es := entriesOf(t, s, other); len(es) != 0 {
		t.Errorf("the other app's library = %v, want nothing filed there", es)
	}

	// A theme container carries its app in its own header, which is the app the orb files it
	// under, so that is what a keep and an upload are weighed against. orbfake's container is
	// clock's, so it passes clock's door and is refused at weather's — and the refusal lands
	// before the hub dials, which is what checking the header first buys.
	before := dials()
	keep := gateEditorCall(t, s, own, http.MethodPost, "/api/library/keep?app=clock",
		orbfake.Container("Sunny"), nil)
	if keep.Code == http.StatusForbidden {
		t.Errorf("clock's editor keeping a clock theme: status %d body %.200s", keep.Code, keep.Body)
	}
	gateRefused(t, gateEditorCall(t, s, other, http.MethodPost, "/api/library/keep?app=weather",
		orbfake.Container("Sunny"), nil),
		http.StatusForbidden, flippedMsg, "weather's editor keeping a clock theme")
	gateRefused(t, gateEditorCall(t, s, other, http.MethodPost, "/api/theme/upload?ip="+ip,
		orbfake.Container("Sunny"), nil),
		http.StatusForbidden, flippedMsg, "weather's editor uploading a clock theme")
	if dials() != before {
		t.Errorf("the scoped-out keep and upload let the hub dial %d times, want none", dials()-before)
	}
	// A body this layer cannot read is the handler's to judge, not a scope refusal: a precheck
	// body is a head slice by design, and no full parse can pass one.
	head := gateEditorCall(t, s, own, http.MethodPost, "/api/theme/upload?ip="+ip+"&precheck=1",
		orbfake.Container("Sunny")[:200], nil)
	if head.Code == http.StatusForbidden && strings.Contains(head.Body.String(), "cannot reach") {
		t.Errorf("a truncated precheck body was refused as a scope violation: %.200s", head.Body)
	}

	// The one route whose subject is in its ANSWER: a design read out of a theme belongs to the
	// app the theme was baked for, whatever folder it was filed under.
	stem := gateShelfTheme(t, s, own, "Tourbillon", other)
	gateRefused(t, gateEditorCall(t, s, own, http.MethodGet, "/api/theme/design?name="+stem, nil, nil),
		http.StatusForbidden, otherMsg, "clock's editor reopening a theme whose design names weather")
	named := gateEditorCall(t, s, other, http.MethodGet, "/api/theme/design?name="+stem, nil, nil)
	if named.Code != http.StatusOK || !strings.Contains(named.Body.String(), `"weather"`) {
		t.Errorf("the design read as its own app: status %d body %.200s", named.Code, named.Body)
	}
	plain := gateShelfTheme(t, s, own, "Aurora", own)
	if rec := gateEditorCall(t, s, own, http.MethodGet, "/api/theme/design?name="+plain, nil, nil); rec.Code != http.StatusOK {
		t.Errorf("a theme whose design agrees with its shelf: status %d body %.200s", rec.Code, rec.Body)
	}

	// The two doors that name their app in a slug rather than an ?app=, one of which has a
	// default the scope layer fills in instead of refusing.
	for _, c := range []struct{ path, msg string }{
		{"/api/editor/source?ip=" + ip, "name its app (?slug=clock)"},
		{"/api/editor/source?ip=" + ip + "&slug=" + other, otherMsg},
	} {
		gateRefused(t, gateEditorCall(t, s, own, http.MethodGet, c.path, nil, nil),
			http.StatusForbidden, c.msg, "GET "+c.path)
	}
	stemless := gateEditorCall(t, s, other, http.MethodGet, "/api/theme/stem?name=Sunny", nil, nil)
	if stemless.Code == http.StatusForbidden {
		t.Errorf("weather's editor asking for a stem with no slug: status %d body %.200s", stemless.Code, stemless.Body)
	}
	if !strings.Contains(stemless.Body.String(), "weather") {
		t.Errorf("the defaulted stem was %.200s, want weather's own", stemless.Body)
	}
	gateRefused(t, gateEditorCall(t, s, other, http.MethodGet, "/api/theme/stem?name=Sunny&slug=clock", nil, nil),
		http.StatusForbidden, flippedMsg,
		"weather's editor asking for a clock stem")

	// An import is scoped by the store's own record of which app it was fetched for, so a bake
	// cannot land in the app that did not import it.
	ticket, err := s.tickets().add(&siteTicket{site: "https://example.dev", app: other,
		name: "Meadow", sharedName: "Meadow", raw: []byte(`{"site":"https://example.dev"}`)})
	if err != nil {
		t.Fatal(err)
	}
	gateRefused(t, gateEditorCall(t, s, own, http.MethodGet, "/api/site/bundle?ticket="+ticket, nil, nil),
		http.StatusForbidden, "that import is for the weather app", "clock's editor reading weather's bundle")
	gateRefused(t, gateEditorCall(t, s, own, http.MethodPost, "/api/site/keep?ticket="+ticket,
		orbfake.Container("Meadow"), nil),
		http.StatusForbidden, "that import is for the weather app", "clock's editor keeping weather's bake")
	bundle := gateEditorCall(t, s, other, http.MethodGet, "/api/site/bundle?ticket="+ticket, nil, nil)
	if bundle.Code != http.StatusOK || !bytes.Contains(bundle.Body.Bytes(), []byte("example.dev")) {
		t.Errorf("the bundle read as its own app: status %d body %.200s", bundle.Code, bundle.Body)
	}
	// A ticket that does not exist is the handler's sentence to give, not a scope refusal.
	gone := gateEditorCall(t, s, own, http.MethodGet, "/api/site/bundle?ticket=00000000000000000000000000000000", nil, nil)
	if gone.Code == http.StatusForbidden && strings.Contains(gone.Body.String(), "import is for") {
		t.Errorf("a ticket that does not exist was refused as another app's: %.200s", gone.Body)
	}
}

// 5. The control port answers as the names a loopback hub is reached on, and as nothing else.
// The bind restricts which INTERFACE takes a connection; this restricts what a connection may
// be ADDRESSED as, which is the rebinding case a bind cannot see: a zone repointed at
// 127.0.0.1 arrives on loopback with a Host of evil.example and is, from the socket's side,
// indistinguishable from the operator's own tab. Every S2 editor origin is that shape — a name
// that resolves here and is not the hub — so a valid token must not be able to buy a wrong name.
func TestControlHostGuard(t *testing.T) {
	s := newHub()
	call := func(host string, token bool) *httptest.ResponseRecorder {
		t.Helper()
		mux := http.NewServeMux()
		s.routes(mux)
		req := httptest.NewRequest(http.MethodGet, "/api/library/entries?app=clock", nil)
		req.Host = host
		if token {
			req.Header.Set(tokenHeader, s.token)
		}
		req.Header.Set("Cookie", tokenCookie+"="+s.token)
		rec := httptest.NewRecorder()
		s.controlHostGuard(mux).ServeHTTP(rec, req)
		return rec
	}
	for _, host := range []string{
		"127.0.0.1:" + strconv.Itoa(s.controlPort),
		"127.0.0.1",
		"localhost:" + strconv.Itoa(s.controlPort),
		"LOCALHOST",
		"[::1]:" + strconv.Itoa(s.controlPort),
		"::1",
	} {
		if rec := call(host, true); rec.Code == http.StatusMisdirectedRequest {
			t.Errorf("Host %q: refused a name a loopback hub is reached on", host)
		}
	}
	for _, host := range []string{
		"clock.localhost:" + strconv.Itoa(s.controlPort),
		"clock.localhost:" + strconv.Itoa(s.editorPort),
		"evil.example",
		"orb.example.com:" + strconv.Itoa(s.controlPort),
		"127.0.0.1.evil.example",
		"localhost.example.com",
		"",
	} {
		for _, token := range []bool{true, false} {
			rec := call(host, token)
			gateRefused(t, rec, http.StatusMisdirectedRequest, "127.0.0.1:"+strconv.Itoa(s.controlPort),
				"Host "+strconv.Quote(host)+" with a valid token")
		}
	}
	// A hub asked to be reachable from elsewhere is asked for by name, and answers to it.
	s.allowRemote = true
	for _, host := range []string{"orb.example.com:" + strconv.Itoa(s.controlPort), "192.168.2.10:3779"} {
		if rec := call(host, true); rec.Code == http.StatusMisdirectedRequest {
			t.Errorf("Host %q on a hub started with -allow-remote: refused, want the name check off", host)
		}
	}
	s.allowRemote = false
	// The guard is a gate on the name and nothing else: a request it lets through is answered
	// by the route it addresses, credential and all — and a request with no credential at all
	// is refused by the guard below, not by this one.
	plain := http.NewServeMux()
	s.routes(plain)
	nocred := httptest.NewRequest(http.MethodGet, "/api/library/entries?app=clock", nil)
	nocred.Host = "127.0.0.1:" + strconv.Itoa(s.controlPort)
	nocredRec := httptest.NewRecorder()
	s.controlHostGuard(plain).ServeHTTP(nocredRec, nocred)
	if !isControlRefusal(nocredRec) {
		t.Errorf("a nameless-credential call through the guard: status %d body %.200s, want the guard below to answer", nocredRec.Code, nocredRec.Body)
	}
}

// 6. The hub page is the one page that may drive the control API, so nothing else may frame it.
// An app's iframe is the nearest candidate: a page that framed the hub could size a Flash
// button under its own click.
func TestHubIndexRefusesFraming(t *testing.T) {
	s := newHub()
	rec := controlGet(t, s, http.MethodGet, "/", true, nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("GET /: status %d body %.200s", rec.Code, rec.Body)
	}
	csp := rec.Header().Get("Content-Security-Policy")
	if !strings.Contains(csp, "frame-ancestors 'self'") {
		t.Errorf("index CSP = %.200s, want frame-ancestors 'self'", csp)
	}
	if xfo := rec.Header().Get("X-Frame-Options"); xfo != "SAMEORIGIN" {
		t.Errorf("index X-Frame-Options = %q, want SAMEORIGIN", xfo)
	}
	// The hub's editors stay framable BY the hub: the two policies are one rule seen from each
	// side, and tightening the page's own must not lock the operator's tab out of its editors.
	erec := gateEditorCall(t, s, "clock", http.MethodGet, "/editor/hub/clock/index.html", nil, nil)
	ecsp := erec.Header().Get("Content-Security-Policy")
	if !strings.Contains(ecsp, "http://127.0.0.1:"+strconv.Itoa(s.controlPort)) {
		t.Errorf("editor CSP = %.200s, want the hub's origin still an ancestor", ecsp)
	}
}
