package main

// l1-design.md §4.5, L1 hub step 2: the library doors on the editor port and the
// GorbHost line. The fake site is this file's own §2.4 answer — the same shape
// sitelink_test.go's hubSite gives the designs read (recorded bearers, a hook for the
// answers an honest site never gives), and the door is driven through
// editorAPICall, the same way every other editor-port claim is tested: the Host
// names the app, the header signs the call. The interesting absence in several of
// these is the dial — a hub with no credential must cost the site nothing.

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
)

// --- the fake site's two asset doors ---------------------------------------------------

// assetAsk is one recorded request: enough to pin the verb, the exact URI, the bearer
// and the four headers every bearer call carries, and nothing more.
type assetAsk struct {
	method string
	uri    string
	bearer string
	accept string
	ua     string
	cache  string
}

// assetSite is the two §2.4 bearer doors. The list door answers listBody (an empty one
// by default, as an empty library is a real state); the bytes door answers the file the
// map holds under the hash asked for and 404s for any other. Either hook replaces its
// door's whole answer, which is how a test pins that only a 401 costs the hub its
// credential and that a redirect is not an answer.
type assetSite struct {
	t    *testing.T
	srv  *httptest.Server
	mu   sync.Mutex
	asks []assetAsk

	listBody    []byte
	listHook    func(w http.ResponseWriter, r *http.Request)
	assetBodies map[string][]byte
	assetHook   func(w http.ResponseWriter, r *http.Request)
}

func assetSiteServer(t *testing.T) *assetSite {
	t.Helper()
	as := &assetSite{t: t, assetBodies: map[string][]byte{}}
	as.srv = httptest.NewServer(http.HandlerFunc(as.handle))
	t.Cleanup(as.srv.Close)
	t.Cleanup(as.srv.CloseClientConnections)
	t.Setenv("GORB_SITE", as.srv.URL)
	return as
}

func (as *assetSite) handle(w http.ResponseWriter, r *http.Request) {
	as.mu.Lock()
	as.asks = append(as.asks, assetAsk{
		method: r.Method,
		uri:    r.URL.RequestURI(),
		bearer: strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer "),
		accept: r.Header.Get("Accept"),
		ua:     r.Header.Get("User-Agent"),
		cache:  r.Header.Get("Cache-Control"),
	})
	hook := as.assetHook
	switch r.URL.Path {
	case "/api/hub/assets":
		hook = as.listHook
	}
	body, bodies := as.listBody, as.assetBodies
	as.mu.Unlock()
	if hook != nil {
		hook(w, r)
		return
	}
	switch r.URL.Path {
	case "/api/hub/assets":
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		if len(body) == 0 {
			body = []byte(`{"assets":[]}`)
		}
		_, _ = w.Write(body)
	case "/api/hub/asset":
		b, ok := bodies[r.URL.Query().Get("sha256")]
		if !ok {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusNotFound)
			_, _ = w.Write([]byte(`{"error":"unauthenticated","ok":false}`))
			return
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		_, _ = w.Write(b)
	default:
		http.NotFound(w, r)
	}
}

func (as *assetSite) dials() int {
	as.mu.Lock()
	defer as.mu.Unlock()
	return len(as.asks)
}

func (as *assetSite) ask(n int) assetAsk {
	as.mu.Lock()
	defer as.mu.Unlock()
	if n >= len(as.asks) {
		return assetAsk{}
	}
	return as.asks[n]
}

// statusHook answers status with a body that is neither door's document: the shape of a
// site having a bad day, or of a site pointing the hub somewhere else.
func (as *assetSite) statusHook(status int, body string) {
	as.mu.Lock()
	defer as.mu.Unlock()
	h := func(w http.ResponseWriter, r *http.Request) {
		if loc := strings.TrimPrefix(body, "Location:"); loc != body {
			w.Header().Set("Location", loc)
			w.WriteHeader(http.StatusFound)
			return
		}
		w.WriteHeader(status)
		_, _ = io.WriteString(w, body)
	}
	as.listHook, as.assetHook = h, h
}

// linkedAssetHub files a live link to this fake with a fresh hub, without walking the
// connect: the doors are about a credential the hub already holds. The bearer it now
// carries comes back for the header assertions.
func linkedAssetHub(t *testing.T, as *assetSite) (*Server, string) {
	t.Helper()
	s := linkHub(t)
	tok := hubRandom(32)
	if err := s.saveSiteLink(&siteLink{Token: tok, Site: siteReleaseBase(), LinkedAt: 1730000000, Handle: "greg"}); err != nil {
		t.Fatal(err)
	}
	return s, tok
}

// hubEnvelope is an editor-port answer read as the envelope every hub door writes.
func hubEnvelope(t *testing.T, rec *httptest.ResponseRecorder) map[string]any {
	t.Helper()
	var env map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
		t.Fatalf("not a JSON answer: %s", rec.Body.String())
	}
	return env
}

// --- the tests -------------------------------------------------------------------------

// No link, no dial: both doors say the one sentence the page turns into Connect, and the
// site hears nothing from a hub that has no credential to spend.
func TestSiteAssetsNotLinked(t *testing.T) {
	as := assetSiteServer(t)
	s := linkHub(t) // statePath set, nothing under it
	for _, p := range []string{"/api/assets", "/api/asset?sha256=" + strings.Repeat("a", 64)} {
		rec := editorAPICall(t, s, http.MethodGet, p)
		if rec.Code != http.StatusBadRequest {
			t.Errorf("GET %s: status %d body %.200s, want 400", p, rec.Code, rec.Body)
		}
		if env := hubEnvelope(t, rec); env["ok"] != false || env["error"] != siteMsgNotConnected {
			t.Errorf("GET %s = %v, want the one not-connected sentence", p, env)
		}
	}
	if got := as.dials(); got != 0 {
		t.Errorf("an unlinked hub asked the site %d times, want not once", got)
	}
}

// The list door is a window, not a reader: the exact bearer on the one request it makes,
// the site's document byte-for-byte inside the hub's own envelope (no re-encode of the
// rows), and one ask per read because a library that arrives stale is worse than none.
func TestSiteAssetsPassThrough(t *testing.T) {
	const h = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
	as := assetSiteServer(t)
	body := `{"assets":[{"sha256":"` + h + `","kind":"picture","mediaType":"image/png","prefix":"data:image/png;base64",` +
		`"size":5349,"name":"Hall clock","width":512,"height":512,"seconds":null,"uploadedAt":null,` +
		`"usedAt":"2026-11-10T09:00:00.000Z","designs":[{"id":"` + siteDesignID + `","app":"clock","name":"Hall clock","current":true}]}],` +
		`"newerField":{"a":[1,2]}}`
	as.listBody = []byte(body)
	s, tok := linkedAssetHub(t, as)
	rec := editorAPICall(t, s, http.MethodGet, "/api/assets")
	if rec.Code != http.StatusOK {
		t.Fatalf("GET /api/assets = %d %s", rec.Code, rec.Body)
	}
	if got, want := rec.Body.String(), `{"data":`+body+`,"ok":true}`; got != want {
		t.Errorf("the list did not arrive byte-equal:\n got %s\nwant %s", got, want)
	}
	if got := as.dials(); got != 1 {
		t.Fatalf("one read made %d requests to the site, want one", got)
	}
	a := as.ask(0)
	if a.method != http.MethodGet || a.uri != "/api/hub/assets" {
		t.Errorf("the door made %s %s, want GET /api/hub/assets", a.method, a.uri)
	}
	if a.bearer != tok {
		t.Errorf("the door sent bearer %q, want exactly the stored token", a.bearer)
	}
	if a.accept != "application/json" || a.ua != "orb-manager/"+version || a.cache != "no-store" {
		t.Errorf("the call carried Accept %q, UA %q, Cache-Control %q, want the four headers every bearer call has",
			a.accept, a.ua, a.cache)
	}
	// Freshness again: a second read is a second ask, not a replay.
	if rec := editorAPICall(t, s, http.MethodGet, "/api/assets"); rec.Code != http.StatusOK {
		t.Fatalf("the second read = %d %s", rec.Code, rec.Body)
	}
	if got := as.dials(); got != 2 {
		t.Errorf("two reads asked the site %d times, want twice", got)
	}
	if l := s.loadSiteLink(); l == nil || l.Token != tok {
		t.Errorf("a good read disturbed the credential: %+v", l)
	}
}

// A revoked link is news the owner is owed: the 401 is believed, the file is rewritten
// disconnected-without-a-token (lead review point 5's rule the designs door already keeps),
// the sentence is the one the page turns into Reconnect — and no later door dials again
// with a bearer the site has already refused.
func TestSiteAssetsRevoked(t *testing.T) {
	as := assetSiteServer(t)
	s, tok := linkedAssetHub(t, as)
	// The site-side Disconnect: the row dies while the hub still has the bearer in its file.
	as.mu.Lock()
	as.listHook = func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusUnauthorized)
		_, _ = io.WriteString(w, `{"error":"unauthenticated"}`)
	}
	as.mu.Unlock()
	rec := editorAPICall(t, s, http.MethodGet, "/api/assets")
	if env := hubEnvelope(t, rec); env["ok"] != false || env["error"] != siteMsgDisconnected {
		t.Fatalf("a refused bearer = %v, want the one disconnected sentence", env)
	}
	if as.dials() != 1 {
		t.Errorf("the 401 made %d asks, want the one that earned it", as.dials())
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
	// Believed the moment it is written: both doors answer from the file, without the site.
	for _, p := range []string{"/api/assets", "/api/asset?sha256=" + strings.Repeat("a", 64)} {
		if env := hubEnvelope(t, editorAPICall(t, s, http.MethodGet, p)); env["error"] != siteMsgDisconnected {
			t.Errorf("%s after the disconnect = %v, want the same sentence", p, env)
		}
	}
	if as.dials() != 1 {
		t.Errorf("a hub with a disconnected file asked the site %d times, want once", as.dials())
	}
	if as.ask(0).bearer != tok {
		t.Errorf("the one ask carried %q, want the dead bearer it was refused for", as.ask(0).bearer)
	}
}

// One call, one answer, no retry and no confusion: every failure that is not a 401 is
// the site having a bad day, and the credential of a hub whose link still works must
// survive it. A 500, a redirect, a body that is not JSON, a body whose assets is not an
// array and a body too large to be a library all answer the one sentence and leave the
// token exactly where it was — the designs door's rule, kept for the list door.
func TestSiteAssetsBadAnswer(t *testing.T) {
	big := `{"assets":["` + strings.Repeat("x", 5<<20) + `"]}`
	cases := []struct {
		name  string
		setup func(t *testing.T, as *assetSite)
	}{
		{"500", func(_ *testing.T, as *assetSite) {
			as.statusHook(http.StatusInternalServerError, "<html>the site is having a moment</html>")
		}},
		{"302", func(_ *testing.T, as *assetSite) {
			as.statusHook(http.StatusFound, "Location: https://orb-os.net/login")
		}},
		{"a 200 that is not JSON", func(_ *testing.T, as *assetSite) { as.listBody = []byte("<html>nope</html>") }},
		{`{"assets":{}}`, func(_ *testing.T, as *assetSite) { as.listBody = []byte(`{"assets":{}}`) }},
		{"a 5 MB body", func(_ *testing.T, as *assetSite) { as.listBody = []byte(big) }},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			as := assetSiteServer(t)
			s, tok := linkedAssetHub(t, as)
			c.setup(t, as)
			rec := editorAPICall(t, s, http.MethodGet, "/api/assets")
			if env := hubEnvelope(t, rec); env["ok"] != false || env["error"] != siteMsgNoAnswer {
				t.Fatalf("%s: got %v, want the one not-answering sentence", c.name, env)
			}
			if l := s.loadSiteLink(); l == nil || l.Token != tok {
				t.Errorf("%s: the credential did not survive: %+v", c.name, l)
			}
		})
	}
}

// The bytes door hands over one file and the one promise that makes it worth having: the
// SHA-256 of what arrives is the hash that was asked for. Three headers ride out with the
// bytes, nothing is stored on disk, the site's 404 is echoed as a 404 rather than turned
// into a hub failure, a lying body is the site's bad day and not a revocation — and a
// hash that is not a hash never reaches the network at all.
func TestSiteAssetBytes(t *testing.T) {
	payload := append([]byte("\x89PNG\r\n\x1a\n fake but honest bytes "),
		strings.Repeat("p", 300)...)
	wantSum := sha256.Sum256(payload)
	want := hex.EncodeToString(wantSum[:])

	t.Run("the bytes come through", func(t *testing.T) {
		as := assetSiteServer(t)
		s, tok := linkedAssetHub(t, as)
		as.mu.Lock()
		as.assetBodies[want] = payload
		as.mu.Unlock()
		rec := editorAPICall(t, s, http.MethodGet, "/api/asset?sha256="+want)
		if rec.Code != http.StatusOK {
			t.Fatalf("GET /api/asset = %d %s", rec.Code, rec.Body)
		}
		if !bytes.Equal(rec.Body.Bytes(), payload) {
			t.Error("the bytes did not arrive whole")
		}
		for _, c := range []struct{ header, want string }{
			{"Content-Type", "application/octet-stream"},
			{"X-Content-Type-Options", "nosniff"},
			{"Cache-Control", "no-store"},
		} {
			if got := rec.Header().Get(c.header); got != c.want {
				t.Errorf("%s = %q, want %q", c.header, got, c.want)
			}
		}
		if got := as.dials(); got != 1 {
			t.Fatalf("one read made %d requests to the site, want one", got)
		}
		a := as.ask(0)
		if a.method != http.MethodGet || a.uri != "/api/hub/asset?sha256="+want {
			t.Errorf("the door made %s %s, want the exact GET", a.method, a.uri)
		}
		if a.bearer != tok || a.accept != "*/*" || a.ua != "orb-manager/"+version || a.cache != "no-store" {
			t.Errorf("the call carried bearer %q, Accept %q, UA %q, Cache-Control %q", a.bearer, a.accept, a.ua, a.cache)
		}
		// Nothing was kept: the hub has no asset cache, and the config dir holds only the
		// two files a linked hub always has.
		names, err := os.ReadDir(filepath.Dir(s.statePath))
		if err != nil {
			t.Fatal(err)
		}
		for _, de := range names {
			if de.Name() != "state.json" && de.Name() != siteLinkFileName {
				t.Errorf("the hub stored %s while passing bytes through, want nothing", de.Name())
			}
		}
	})

	t.Run("a body whose hash is wrong", func(t *testing.T) {
		as := assetSiteServer(t)
		s, tok := linkedAssetHub(t, as)
		as.mu.Lock()
		as.assetBodies[want] = []byte("the site lied about these bytes")
		as.mu.Unlock()
		rec := editorAPICall(t, s, http.MethodGet, "/api/asset?sha256="+want)
		if env := hubEnvelope(t, rec); env["ok"] != false || env["error"] != siteMsgNoAnswer {
			t.Fatalf("a lying body = %v, want the one not-answering sentence", env)
		}
		if l := s.loadSiteLink(); l == nil || l.Token != tok {
			t.Errorf("the credential did not survive a wrong body: %+v", l)
		}
	})

	t.Run("404", func(t *testing.T) {
		as := assetSiteServer(t)
		s, tok := linkedAssetHub(t, as)
		rec := editorAPICall(t, s, http.MethodGet, "/api/asset?sha256="+want) // the fake holds nothing
		if rec.Code != http.StatusNotFound {
			t.Fatalf("status %d body %.200s, want the site's 404 echoed as a 404", rec.Code, rec.Body)
		}
		var env map[string]any
		if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
			t.Fatalf("the 404 is not the envelope: %s", rec.Body)
		}
		if env["ok"] != false || env["error"] != "no such asset" {
			t.Errorf("the 404 said %v, want {ok:false, error:\"no such asset\"}", env)
		}
		if l := s.loadSiteLink(); l == nil || l.Token != tok {
			t.Errorf("a missing file disturbed the credential: %+v", l)
		}
	})

	t.Run("a bad hash never dials", func(t *testing.T) {
		as := assetSiteServer(t)
		s, _ := linkedAssetHub(t, as)
		up := strings.ToUpper(want)
		for _, q := range []string{
			"/api/asset?sha256=" + want[:63],                // 63 hex
			"/api/asset?sha256=" + up,                       // upper case
			"/api/asset?sha256=" + want + "&app=clock",      // an extra parameter
			"/api/asset?sha256=" + want + "&sha256=" + want, // the same parameter twice
			"/api/asset",                      // no parameter at all
			"/api/asset?sha256=",              // empty
			"/api/asset?app=clock",            // some other parameter
			"/api/asset?sha256=" + want + "0", // 65 hex
		} {
			rec := editorAPICall(t, s, http.MethodGet, q)
			if rec.Code != http.StatusBadRequest {
				t.Errorf("GET %s: status %d body %.200s, want 400", q, rec.Code, rec.Body)
			}
			if env := hubEnvelope(t, rec); env["ok"] != false || env["error"] != siteMsgNoAsset {
				t.Errorf("GET %s = %v, want the one no-such-asset sentence", q, env)
			}
			if got := as.dials(); got != 0 {
				t.Fatalf("GET %s asked the site %d times, want nothing dialed", q, got)
			}
		}
	})
}

// The GorbHost line is the document half of the same fact the doors are: an editor may
// offer "Your library" exactly when the hub can answer for it. It lands right after the
// shim in the editor's own entry document while a live link exists — a constant, with no
// request value in it — the unlinked document is byte-for-byte the one every test before
// this step compared against, and when the link dies the line dies with it, on the same
// read that rewrites the file. And no document read ever dials the site for it: the flag
// is read off the disk, not off the wire.
func TestEditorDocumentGorbHostWhenLinked(t *testing.T) {
	as := assetSiteServer(t) // only its GORB_SITE matters here: no dial may happen
	const ip = "203.0.113.7"
	s, _ := editorHub(t, ip, zipWith(t, map[string]string{
		"editor.json": `{"entry":"index.html"}`,
		"index.html":  `<!doctype html><html lang="en"><head><meta charset="utf-8"><title>E</title></head><body>e</body></html>`,
	}), nil)
	s.statePath = filepath.Join(t.TempDir(), "state.json")
	const docPath = "/editor/" + ip + "/clock/"
	const line = `<script>window.GorbHost={"kind":"hub","library":true}</script>`
	shim := string(editorShim(s.editorToken("clock")))

	doc := func() string {
		rec := editorListenerGet(t, s, docPath, true, nil)
		if rec.Code != http.StatusOK {
			t.Fatalf("the editor document: status %d body %.200s", rec.Code, rec.Body)
		}
		return rec.Body.String()
	}

	// Unlinked: the shim, and nothing about a library.
	unlinked := doc()
	if !strings.Contains(unlinked, shim) {
		t.Fatal("the document lost its shim")
	}
	if strings.Contains(unlinked, "GorbHost") {
		t.Error("an unlinked hub told an editor it had a library")
	}

	// Linked: the line sits right after the shim, the app's own bytes survive, and taking
	// the one appended line away leaves the unlinked document exactly as it was.
	if err := s.saveSiteLink(&siteLink{Token: hubRandom(32), Site: siteReleaseBase(), LinkedAt: 1, Handle: "greg"}); err != nil {
		t.Fatal(err)
	}
	linked := doc()
	if !strings.Contains(linked, shim+line) {
		t.Errorf("the GorbHost line did not land right after the shim:\n%s", linked)
	}
	if !strings.Contains(linked, "<body>e</body>") {
		t.Error("the app's own bytes did not survive the insertion")
	}
	if strings.Count(linked, line) != 1 {
		t.Error("the GorbHost line was inserted more than once")
	}
	if got := strings.Replace(linked, shim+line, shim, 1); got != unlinked {
		t.Errorf("with the line taken back the document is not the unlinked one:\n got %s\nwant %s", got, unlinked)
	}

	// The 401 rewrite takes the flag away too: the file is disconnected-shaped, so the
	// next document offers no library.
	if err := s.saveSiteLink(&siteLink{Site: siteReleaseBase(), LinkedAt: 1, Handle: "greg", Disconnected: true}); err != nil {
		t.Fatal(err)
	}
	if got := doc(); got != unlinked {
		t.Errorf("after the link was revoked the document still said something about a library:\n%s", got)
	}
	if got := as.dials(); got != 0 {
		t.Errorf("serving documents asked the site %d times, want not once", got)
	}
}

// The doors are the editor port's and only the editor port's: on the control mux they are
// not patterns at all, so the hub's own token reads a 404; on this port they stand behind
// the same guard as every other editor API route, so a call with no credential is the
// same 403 naming the header, and the site is asked nothing either way.
func TestSiteAssetsEditorPortOnly(t *testing.T) {
	as := assetSiteServer(t)
	s, _ := linkedAssetHub(t, as)
	paths := []string{"/api/assets", "/api/asset?sha256=" + strings.Repeat("a", 64)}
	for _, p := range paths {
		if rec := rawCall(t, s, http.MethodGet, p, nil); rec.Code != http.StatusNotFound {
			t.Errorf("control GET %s: status %d body %.160s, want 404 from a mux that has no such pattern", p, rec.Code, rec.Body)
		}
		if rec := rawCall(t, s, http.MethodPost, p, []byte(`{}`)); rec.Code != http.StatusNotFound {
			t.Errorf("control POST %s: status %d, want 404", p, rec.Code)
		}
		rec := httptest.NewRequest(http.MethodGet, p, nil)
		rec.Host = editorHostLabel("clock") + ".localhost:" + strconv.Itoa(s.editorPort)
		rec.Header.Set("Origin", s.editorOrigin("clock"))
		rec.Header.Set("Sec-Fetch-Site", "same-origin")
		rr := httptest.NewRecorder()
		s.editorHandler().ServeHTTP(rr, rec)
		if rr.Code != http.StatusForbidden {
			t.Errorf("editor port with no token: %s status %d body %.200s, want the guard's 403", p, rr.Code, rr.Body)
		}
		if !strings.Contains(rr.Body.String(), editorTokenHeader) {
			t.Errorf("editor port with no token: body %.200s, want the refusal naming %s", rr.Body, editorTokenHeader)
		}
	}
	if got := as.dials(); got != 0 {
		t.Errorf("refused and absent-port calls asked the site %d times, want not once", got)
	}
}
