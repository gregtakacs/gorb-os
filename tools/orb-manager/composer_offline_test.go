package main

import (
	"crypto/sha256"
	"crypto/sha512"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The Theme Editor reaches outside the machine for three files support.js pulls from
// unpkg.com and for eleven Google Fonts stylesheets. An orb flashed at a bench with no
// network gets a blank editor whose only complaint is a console a chromeless window
// cannot show. cmd/orbvendor puts local copies in the embed; this is the contract that
// makes using them safe.
//
// What is deliberately NOT tested here: any edit to support.js. It is vendor bytes,
// pinned byte-exact by TestEmbeddedComposerMatchesSource and
// TestComposerServesSiblingsUntouched. support.js's own window.__resources hook is the
// supported seam, so rewriting it would be the wrong fix even if it worked today.

func getComposer(t *testing.T, s *Server, p string) *httptest.ResponseRecorder {
	t.Helper()
	rec := httptest.NewRecorder()
	s.serveComposer(rec, httptest.NewRequest(http.MethodGet, p, nil))
	return rec
}

func TestComposerServesOfflineWhenVendored(t *testing.T) {
	if !vendorReady() {
		t.Skip("no vendor tree in the embed; run make vendor-composer")
	}
	s := &Server{} // no repoDir: the end user's machine, served from the binary
	rec := getComposer(t, s, "/composer/")
	if rec.Code != http.StatusOK {
		t.Fatalf("GET /composer/ = %d", rec.Code)
	}
	doc := rec.Body.String()

	// Three things must hold at once, and each has failed on its own before.
	for _, url := range sortedCDNURLs() {
		// The URL may appear exactly once: as the key of its own override entry.
		if n := strings.Count(doc, url); n != 1 {
			t.Errorf("%s appears %d times; it should survive only as an override key", url, n)
		}
		if !strings.Contains(doc, `window.__resources["`+url+`"]="`) {
			t.Errorf("no usable override for %s; support.js falls back to the CDN", url)
		}
		if local := cdnLocal[url]; !strings.Contains(doc, `="`+local+`"`) {
			t.Errorf("override for %s does not name %s", url, local)
		}
	}
	if n := strings.Count(doc, `href="/composer/vendor/fonts.css"`); n != 1 {
		t.Errorf("fonts.css referenced %d times; the eleven Google links collapse to one", n)
	}
	// Nothing may point at Google at all once the faces are local: a stylesheet link
	// would 404 the fonts and a preconnect hint would open a socket to a host a
	// bench-networked orb cannot reach.
	for _, host := range []string{"fonts.googleapis.com", "fonts.gstatic.com"} {
		if n := strings.Count(doc, host); n != 0 {
			t.Errorf("the served document references %s %d times; the offline rewrite missed a link", host, n)
		}
	}
	// The map must be parsed BEFORE support.js, whose loader reads it immediately.
	mapAt, supportAt := strings.Index(doc, "window.__resources"), strings.Index(doc, `src="./support.js"`)
	if mapAt < 0 || supportAt < 0 {
		t.Fatalf("document lost the override map (%d) or the loader tag (%d)", mapAt, supportAt)
	}
	if mapAt > supportAt {
		t.Error("the override map sits AFTER support.js, so the loader never sees it")
	}
	if !strings.Contains(doc, "Orb Manager") {
		t.Error("the back link was dropped by the offline rewrite")
	}
}

// The trap the first implementation fell into: injectOffline is a plain text
// substitution, so running it after the map goes in rewrote the map's own keys and
// produced __resources["/composer/vendor/react…""], which support.js never looks up.
// The symptom was a blank editor that looked offline-capable.
func TestOfflineRewriteDoesNotEatItsOwnOverrideMap(t *testing.T) {
	fixture := `<html><head><script src="./support.js"></script></head><body><x-dc><helmet>` +
		`<link href="` + googleFontsHost + `?family=Oswald" rel="stylesheet">` +
		`</helmet></x-dc></body></html>`

	good := string(injectBeforeSupport(injectOffline([]byte(fixture), true)))
	for _, url := range sortedCDNURLs() {
		if !strings.Contains(good, `window.__resources["`+url+`"]=`) {
			t.Errorf("the map lost its key for %s: the rewrite ran after insertion\n%s", url, good)
		}
	}
	for _, local := range cdnLocal {
		if strings.Contains(good, `window.__resources["`+local+`"]`) {
			t.Errorf("the map is keyed by local path %s instead of the CDN URL", local)
		}
	}
	// The reverse order is what produced the bug. Assert the wrong order differs, so
	// this test cannot be satisfied by a rewrite that silently matches nothing.
	bad := string(injectOffline(injectBeforeSupport([]byte(fixture)), true))
	if bad == good {
		t.Error("the two orderings are indistinguishable, so this test proves nothing")
	}
	// And the wrong ordering must really damage a key, or the fixture is inert and the
	// test above proves nothing.
	if !strings.Contains(bad, `window.__resources["`+cdnLocal["https://unpkg.com/react@18.3.1/umd/react.production.min.js"]+`"]`) {
		t.Error("the wrong ordering left the map intact; injectOffline matched no CDN URL, so this fixture cannot catch the regression")
	}
}

// The offline claim is only as good as the dependency list, and support.js is vendor
// bytes someone can re-copy from upstream at any time. This walks every file the hub
// serves as the composer and pins the complete set of hosts it could ever reach: the
// three unpkg files support.js loads, and Google Fonts the entry links to. A fourth
// script, a fonts CDN, or an analytics beacon anywhere in the tree fails here, rather
// than turning the editor into a half-loaded page on a bench network.
func TestComposerHasNoUnknownExternalDependencies(t *testing.T) {
	allowedHosts := map[string]bool{"unpkg.com": true, "fonts.googleapis.com": true}
	names, err := webFS.ReadDir(composerEmbedDir)
	if err != nil {
		t.Fatalf("read embedded composer dir: %v", err)
	}
	var offenders []string
	seen := map[string]bool{}
	for _, d := range names {
		if d.IsDir() {
			continue
		}
		raw, err := webFS.ReadFile(composerEmbedDir + "/" + d.Name())
		if err != nil {
			t.Fatalf("read %s: %v", d.Name(), err)
		}
		for _, m := range regexp.MustCompile(`https?://[a-zA-Z0-9.-]+`).FindAllString(string(raw), -1) {
			host := strings.TrimPrefix(strings.TrimPrefix(m, "https://"), "http://")
			seen[host] = true
			if !allowedHosts[host] {
				offenders = append(offenders, d.Name()+": "+m)
			}
		}
	}
	if len(offenders) > 0 {
		t.Errorf("composer files reach for %d unknown host(s); cmd/orbvendor must vendor them too:\n  %s",
			len(offenders), strings.Join(offenders, "\n  "))
	}
	// And the known hosts must still be the ones the hub has vendored for.
	if !seen["unpkg.com"] && vendorReady() {
		t.Error("support.js no longer references unpkg.com; cdnLocal is stale and the override map is dead weight")
	}
	if got := len(cdnLocal); got != 3 {
		t.Errorf("cdnLocal covers %d URLs; support.js has exactly three pinned loads", got)
	}
}

// The vendored files must actually be fetchable, or the override map is a promise the
// hub cannot keep — worse than the CDN, because it fails with no message at all.
func TestVendoredAssetsAreServed(t *testing.T) {
	if !vendorReady() {
		t.Skip("no vendor tree in the embed; run make vendor-composer")
	}
	s := &Server{}
	for _, name := range []string{"react.production.min.js", "react-dom.production.min.js", "babel.min.js"} {
		rec := getComposer(t, s, "/composer/vendor/"+name)
		if rec.Code != http.StatusOK {
			t.Errorf("GET /composer/vendor/%s = %d (%s)", name, rec.Code, rec.Body.String())
			continue
		}
		if rec.Body.Len() < 1000 {
			t.Errorf("%s came back as %d B; that is not the real bundle", name, rec.Body.Len())
		}
		if !strings.Contains(rec.Header().Get("Cache-Control"), "immutable") {
			t.Errorf("%s is not cached immutable; 3 MB of Babel should not refetch per reload", name)
		}
		if ct := rec.Header().Get("Content-Type"); !strings.Contains(ct, "javascript") {
			t.Errorf("%s served as %q; a wrong type can stop the script executing", name, ct)
		}
	}

	rec := getComposer(t, s, "/composer/vendor/fonts.css")
	if rec.Code != http.StatusOK {
		t.Fatalf("fonts.css = %d (%s)", rec.Code, rec.Body.String())
	}
	css := rec.Body.String()
	if !strings.Contains(css, "@font-face") {
		t.Error("the vendored fonts.css has no @font-face rules")
	}
	for _, host := range []string{"fonts.gstatic.com", "fonts.googleapis.com"} {
		if strings.Contains(css, host) {
			t.Errorf("fonts.css still references %s; every src must be a local relative URL", host)
		}
	}
	// Fetch one font the way the stylesheet asks for it, so the relative URL and the
	// embed layout are proven to line up rather than assumed.
	name := firstLocalFont(t, css)
	if name == "" {
		t.Fatal("no local font URL found in the vendored CSS")
	}
	fr := getComposer(t, s, "/composer/vendor/"+name)
	if fr.Code != http.StatusOK || fr.Body.Len() < 100 {
		t.Errorf("font %s = %d B, status %d", name, fr.Body.Len(), fr.Code)
	}
}

func firstLocalFont(t *testing.T, css string) string {
	t.Helper()
	i := strings.Index(css, "url(fonts/")
	if i < 0 {
		return ""
	}
	rest := css[i+len("url("):]
	j := strings.IndexAny(rest, ") \t\n\"'")
	if j < 0 {
		return ""
	}
	return rest[:j]
}

// serveVendor resolves a filename out of the URL, so it is the one place in the hub
// where a path could walk out of its subtree.
func TestVendorRouteRefusesTraversal(t *testing.T) {
	s := &Server{}
	for _, p := range []string{
		"/composer/vendor/../../go.mod",
		"/composer/vendor/..%2f..%2fgo.mod",
		"/composer/vendor/fonts/../../../../../etc/passwd",
		"/composer/vendor/",
		"/composer/vendor/fonts",
	} {
		rec := getComposer(t, s, p)
		if rec.Code == http.StatusOK {
			t.Errorf("%s was served: %.120s", p, rec.Body.String())
		}
		body := rec.Body.String()
		if strings.Contains(body, "root:") || strings.Contains(body, "module github.com") {
			t.Errorf("%s leaked a real file out of the project: %.200s", p, body)
		}
	}
}

// The committed tree must still be the bytes that were downloaded and verified, not
// something edited afterwards. This is what makes shipping third-party code in the repo
// defensible: the manifest is the promise, and CI holds it.
func TestVendorTreeMatchesItsManifest(t *testing.T) {
	dir := filepath.Join("web", "composer", vendorDirRel)
	raw, err := os.ReadFile(filepath.Join(dir, "MANIFEST.json"))
	if err != nil {
		t.Skip("no vendored tree committed:", err)
	}
	var m struct {
		Scripts []struct{ URL, Local, SHA256, SRI string }
		Fonts   []struct {
			URL, Local, SHA256 string
			Bytes              int
		}
	}
	if err := json.Unmarshal(raw, &m); err != nil {
		t.Fatalf("MANIFEST.json is not valid JSON: %v", err)
	}
	if len(m.Scripts) != len(cdnLocal) {
		t.Fatalf("manifest records %d scripts, hub expects %d", len(m.Scripts), len(cdnLocal))
	}
	for _, e := range m.Scripts {
		b, err := os.ReadFile(filepath.Join(dir, e.Local))
		if err != nil {
			t.Errorf("%s: %v", e.Local, err)
			continue
		}
		// The digest support.js demands at load time. If it drifts the browser refuses
		// the script and the editor goes blank with no console anyone can open.
		if sum384 := "sha384-" + base64Std(sha512.Sum384(b)); sum384 != e.SRI {
			t.Errorf("%s no longer matches the SRI the composer will demand (%s vs %s)", e.Local, sum384[:24], e.SRI[:24])
		}
		if got := hexOf(sha256.Sum256(b)); got != e.SHA256 {
			t.Errorf("%s sha256 = %s…, manifest says %s… — a vendored file was edited in place", e.Local, got[:12], e.SHA256[:12])
		}
	}
	for _, e := range m.Fonts {
		b, err := os.ReadFile(filepath.Join(dir, e.Local))
		if err != nil {
			t.Errorf("%s: %v", e.Local, err)
			continue
		}
		if hexOf(sha256.Sum256(b)) != e.SHA256 {
			t.Errorf("%s does not match its manifest entry", e.Local)
		}
	}
}

func hexOf(b [sha256.Size]byte) string { return hex.EncodeToString(b[:]) }
func base64Std(b [sha512.Size384]byte) string {
	return base64.StdEncoding.EncodeToString(b[:])
}

// Eleven <link> tags become one stylesheet. If the subtree silently stopped covering a
// family the composer lists, the editor still loads and just looks wrong, so coverage
// is checked against the entry document rather than a number plucked from the air.
func TestVendoredFontsCoverTheComposerFamilies(t *testing.T) {
	dir := filepath.Join("web", "composer", vendorDirRel)
	b, err := os.ReadFile(filepath.Join(dir, "fonts.css"))
	if err != nil {
		t.Skip("no vendored fonts.css:", err)
	}
	css := string(b)
	if n := strings.Count(css, "@font-face"); n < 30 {
		t.Errorf("fonts.css declares %d faces; the composer lists dozens across 36 families", n)
	}

	// Every family the entry asks Google for must be declared locally, or the offline
	// editor silently substitutes a fallback typeface.
	entry, err := webFS.ReadFile(composerEmbedDir + "/" + composerEntry)
	if err != nil {
		t.Fatalf("read embedded composer entry: %v", err)
	}
	var missing []string
	for _, m := range regexp.MustCompile(`family=([^&"]+)`).FindAllStringSubmatch(string(entry), -1) {
		// The value carries an axis spec — "Oswald:wght@400..700" — and may carry
		// several names, though the composer writes one family per link.
		name := strings.SplitN(strings.ReplaceAll(m[1], "+", " "), ":", 2)[0]
		if !strings.Contains(css, "font-family: '"+name+"'") {
			missing = append(missing, name)
		}
	}
	if len(missing) > 0 {
		t.Errorf("vendored fonts.css does not cover %d family(ies) the composer asks for: %s",
			len(missing), strings.Join(missing, ", "))
	}
	// And nothing in it may reach back out to Google.
	for _, host := range []string{"fonts.gstatic.com", "fonts.googleapis.com"} {
		if strings.Contains(css, host) {
			t.Errorf("fonts.css references %s; every src must be a local relative URL", host)
		}
	}
}

// Without the vendor tree the hub must behave exactly as before: leave the CDN URLs in
// place and keep the unpkg.com message, rather than rewriting to files that would 404.
func TestOfflineRewriteIsInertWithoutTheVendorTree(t *testing.T) {
	fixture := `<html><head><script src="./support.js"></script></head><body><x-dc><helmet>` +
		`<link href="` + googleFontsHost + `?family=Oswald" rel="stylesheet">` +
		`</helmet></x-dc></body></html>`

	// injectOffline's second argument is the vendor gate: false is the no-vendor build,
	// and the bytes must come back untouched so the CDN path still works.
	if got := string(injectOffline([]byte(fixture), false)); got != fixture {
		t.Errorf("the not-vendored rewrite changed the document and must not:\n%s", got)
	}
	// A document with no loader tag must come back byte-identical rather than have a
	// script bolted onto the front of it, which is what a blind insert would do.
	if out := string(injectBeforeSupport([]byte("<html><body>nothing to hook</body></html>"))); out != "<html><body>nothing to hook</body></html>" {
		t.Error("injectBeforeSupport modified a document that has no loader tag")
	}
	// cdnLocal is the whole contract with support.js: exactly its three fetches, each
	// keyed by the literal URL and pointing inside the vendor namespace.
	if len(cdnLocal) != 3 {
		t.Errorf("cdnLocal has %d entries; support.js fetches exactly three files", len(cdnLocal))
	}
	for url, local := range cdnLocal {
		if !strings.HasPrefix(url, "https://unpkg.com/") {
			t.Errorf("key %q is not a CDN URL, so support.js will never look it up", url)
		}
		if !strings.HasPrefix(local, "/composer/vendor/") {
			t.Errorf("override for %s points outside the vendor namespace: %s", url, local)
		}
	}
	// The fallback message belongs in every served document, vendored or not: the
	// vendor tree can be absent, and it is the only symptom the user can see.
	if !strings.Contains(hubDepWatch, "unpkg.com") {
		t.Error("hubDepWatch no longer names the CDN, so its message would be useless")
	}
}
