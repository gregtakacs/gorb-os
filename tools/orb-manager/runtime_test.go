package main

import (
	"bytes"
	"io/fs"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// hubIndexHTML is the embedded front page, as the hub serves it before the token.
func hubIndexHTML(t *testing.T) string {
	t.Helper()
	raw, err := webFS.ReadFile("web/index.html")
	if err != nil {
		t.Fatalf("read embedded index.html: %v", err)
	}
	return string(raw)
}

// Every id the catalog names is in this binary, and each serves exactly its own bytes.
func TestRuntimeServesEveryCatalogID(t *testing.T) {
	s, _ := editorHub(t, "203.0.113.9", nil, nil)
	cases := map[string]string{
		"support@1/support.js":                     "web/runtime/support@1/support.js",
		"baker@1/orbtheme-baker.js":                "web/runtime/baker@1/orbtheme-baker.js",
		"ui@1/ui.css":                              "web/runtime/ui@1/ui.css",
		"react@18.3.1/react.production.min.js":     "web/runtime/vendor/react.production.min.js",
		"react@18.3.1/react-dom.production.min.js": "web/runtime/vendor/react-dom.production.min.js",
		"babel@7.29.0/babel.min.js":                "web/runtime/vendor/babel.min.js",
		"fonts@1/fonts.css":                        "web/runtime/vendor/fonts.css",
	}
	for url, embed := range cases {
		want, err := webFS.ReadFile(embed)
		if err != nil {
			t.Fatalf("%s is not in the binary: %v", embed, err)
		}
		rec := editorGet(t, s, "/runtime/"+url, true)
		if rec.Code != http.StatusOK || !bytes.Equal(rec.Body.Bytes(), want) {
			t.Errorf("/runtime/%s: status %d, %d B, want %d B of %s", url, rec.Code, rec.Body.Len(), len(want), embed)
		}
	}
	for id := range runtimeCatalog {
		if !runtimeHas(id) {
			t.Errorf("the catalog names %s and the binary does not carry it", id)
		}
	}
	// a face the fonts sheet points at is served under the fonts id
	faces, _ := fs.ReadDir(webFS, "web/runtime/vendor/fonts")
	if len(faces) == 0 {
		t.Fatal("no vendored font faces")
	}
	if rec := editorGet(t, s, "/runtime/fonts@1/fonts/"+faces[0].Name(), true); rec.Code != http.StatusOK {
		t.Errorf("a font face under fonts@1: status %d", rec.Code)
	}
}

// One id cannot reach another's files, a path cannot leave its id, and an unknown id
// or version is a 404 — the version in the path is a promise, not decoration.
func TestRuntimeRefusesWhatAnIDDoesNotOwn(t *testing.T) {
	s, _ := editorHub(t, "203.0.113.9", nil, nil)
	for _, url := range []string{
		"/runtime/babel@7.29.0/react.production.min.js",
		"/runtime/react@18.3.1/babel.min.js",
		"/runtime/fonts@1/babel.min.js",
		"/runtime/baker@2/orbtheme-baker.js",
		"/runtime/baker/orbtheme-baker.js",
		"/runtime/support@1/../baker@1/orbtheme-baker.js",
		"/runtime/support@1/",
		"/runtime/",
	} {
		// a dot-dot path never reaches the handler: ServeMux cleans it and redirects to
		// the canonical URL, which is then judged as itself
		if rec := editorGet(t, s, url, true); rec.Code == http.StatusOK {
			t.Errorf("%s: served 200, want a refusal", url)
		}
	}
	if rec := editorGet(t, s, "/runtime/baker@1/orbtheme-baker.js", false); rec.Code == http.StatusOK {
		t.Error("the runtime served without the launch token")
	}
}

// D43 ruling 4: /composer/ is gone, and nothing the hub serves still points at it.
func TestTheComposerRouteIsRetired(t *testing.T) {
	s, _ := editorHub(t, "203.0.113.9", nil, nil)
	for _, url := range []string{"/composer/", "/composer/support.js", "/composer/vendor/babel.min.js"} {
		if rec := editorGet(t, s, url, true); rec.Code == http.StatusOK {
			t.Errorf("%s still answers 200", url)
		}
	}
	_ = fs.WalkDir(webFS, "web", func(p string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() || strings.HasPrefix(p, "web/runtime/vendor/") {
			return err
		}
		raw, _ := webFS.ReadFile(p)
		if bytes.Contains(raw, []byte("/composer/")) {
			t.Errorf("%s still names /composer/", p)
		}
		return nil
	})
	if strings.Contains(hubIndexHTML(t), `href="/composer`) {
		t.Error("the front page links the retired composer")
	}
}

func TestEditorRuntimeMissingNamesWhatTheHubLacks(t *testing.T) {
	cases := []struct {
		manifest string
		want     string
	}{
		{`{"entry":"index.html","runtime":["support@1","baker@1","react@18.3.1","babel@7.29.0","fonts@1"]}`, ""},
		{`{"entry":"index.html"}`, ""},
		{`{"entry":"index.html","runtime":["baker@9"]}`, "baker@9"},
		{`{"entry":"index.html","runtime":["react","babel","baker","support"]}`, "react,babel,baker,support"},
	}
	for _, c := range cases {
		if got := strings.Join(editorRuntimeMissing([]byte(c.manifest)), ","); got != c.want {
			t.Errorf("%s: missing %q, want %q", c.manifest, got, c.want)
		}
	}
}

// The door: an editor naming a runtime the hub lacks is refused by name, as a page the
// frame can show; one naming what the hub carries is served; a sibling asset of either
// is not gated (it is the document that runs, and it is refused already).
func TestEditorRefusesARuntimeTheHubDoesNotCarry(t *testing.T) {
	const ip = "203.0.113.8"
	old := zipWith(t, map[string]string{
		"editor.json": `{"entry":"index.html","runtime":["react","babel","baker","support"]}`,
		"index.html":  `<body><script src="/composer/support.js"></script></body>`,
	})
	s, _ := editorHub(t, ip, old, nil)
	req := editorGet(t, s, "/editor/"+ip+"/clock/", true)
	if req.Code != http.StatusConflict || !strings.Contains(req.Body.String(), "baker") {
		t.Fatalf("an unversioned editor: status %d body %s", req.Code, req.Body)
	}
	cur := zipWith(t, map[string]string{
		"editor.json": `{"entry":"index.html","runtime":["support@1","baker@1"]}`,
		"index.html":  `<body><script src="/runtime/support@1/support.js"></script></body>`,
	})
	s2, _ := editorHub(t, ip, cur, nil)
	if rec := editorGet(t, s2, "/editor/"+ip+"/clock/", true); rec.Code != http.StatusOK {
		t.Fatalf("a current editor: status %d body %s", rec.Code, rec.Body)
	}
}

// The runtime's own scripts reach for exactly the CDN files the hub vendors, and no
// other host. Ported from the retired composer suite's
// TestComposerHasNoUnknownExternalDependencies: support.js pulls React, ReactDOM and
// Babel by URL unless an editor's window.__resources names a local path, and the clock
// region names each of these. A fourth URL appearing here is the alarm — vendor it with
// cmd/orbvendor and add it to the catalog, or every editor silently needs internet again.
func TestRuntimeScriptsReachOnlyVendoredURLs(t *testing.T) {
	vendored := map[string]string{
		"https://unpkg.com/react@18.3.1/umd/react.production.min.js":         "react@18.3.1/react.production.min.js",
		"https://unpkg.com/react-dom@18.3.1/umd/react-dom.production.min.js": "react@18.3.1/react-dom.production.min.js",
		"https://unpkg.com/@babel/standalone@7.29.0/babel.min.js":            "babel@7.29.0/babel.min.js",
	}
	urlRe := regexp.MustCompile(`https?://[a-zA-Z0-9.-]+[^\s"'` + "`" + `)]*`)
	seen := map[string]bool{}
	for _, f := range []string{"web/runtime/support@1/support.js", "web/runtime/baker@1/orbtheme-baker.js"} {
		raw, err := webFS.ReadFile(f)
		if err != nil {
			t.Fatalf("read %s: %v", f, err)
		}
		for _, u := range urlRe.FindAllString(string(raw), -1) {
			if _, ok := vendored[u]; ok {
				seen[u] = true
				continue
			}
			if strings.HasPrefix(u, "https://unpkg.com/") || !strings.Contains(u, ".") {
				t.Errorf("%s reaches for %s, which the runtime does not vendor", f, u)
			}
		}
	}
	s, _ := editorHub(t, "203.0.113.9", nil, nil)
	for u, local := range vendored {
		if !seen[u] {
			t.Errorf("support.js no longer references %s; the vendored copy and the region's map are stale", u)
		}
		if rec := editorGet(t, s, "/runtime/"+local, true); rec.Code != http.StatusOK {
			t.Errorf("%s maps to /runtime/%s, which answers %d", u, local, rec.Code)
		}
	}
}

// The shipped clock editor, end to end against this hub: its region (out of the
// library's own clock.orbapp) is served through /editor/, and every runtime URL its
// entry document reaches for — the two script tags, the three CDN targets in its
// window.__resources map, the font sheet and a face that sheet names — answers 200.
// A rename on either side (the region builder or the catalog) fails here, not as a
// blank editor on an orb.
func TestTheShippedClockEditorRunsAgainstThisHubsRuntime(t *testing.T) {
	body, err := os.ReadFile(filepath.Join("apps", "clock.orbapp"))
	if err != nil {
		t.Fatalf("read the library's clock.orbapp: %v", err)
	}
	region, err := orbnet.AppEditorRegion(body)
	if err != nil {
		t.Fatalf("clock.orbapp carries no editor region: %v", err)
	}
	const ip = "203.0.113.11"
	s, _ := editorHub(t, ip, region, nil)
	rec := editorGet(t, s, "/editor/"+ip+"/clock/", true)
	if rec.Code != http.StatusOK {
		t.Fatalf("the clock editor: status %d body %.300s", rec.Code, rec.Body)
	}
	entry := rec.Body.String()
	refs := regexp.MustCompile(`(?:src|href)="(/runtime/[^"]+)"|"(/runtime/[^"]+)"`).FindAllStringSubmatch(entry, -1)
	if len(refs) < 6 {
		t.Fatalf("the entry names %d runtime URLs, want the 2 scripts, 3 CDN targets and the font sheet", len(refs))
	}
	for _, m := range refs {
		u := m[1] + m[2]
		if r := editorGet(t, s, u, true); r.Code != http.StatusOK {
			t.Errorf("the clock editor reaches for %s: status %d", u, r.Code)
		}
	}
	css := editorGet(t, s, "/runtime/fonts@1/fonts.css", true).Body.String()
	face := regexp.MustCompile(`url\((fonts/[^)]+)\)`).FindStringSubmatch(css)
	if face == nil {
		t.Fatal("fonts.css names no local face")
	}
	if r := editorGet(t, s, "/runtime/fonts@1/"+face[1], true); r.Code != http.StatusOK {
		t.Errorf("a face fonts.css names (%s): status %d", face[1], r.Code)
	}
	if strings.Contains(entry, "/composer/") || strings.Contains(entry, "unpkg.com/react@") && !strings.Contains(entry, "__resources") {
		t.Error("the clock editor still reaches for the retired route or the CDN with no local map")
	}
}

// An orb running an older release of the app than the shelf gets a page that says so in
// those words and offers the Apps face's Update, through the pane (D43, 2026-11-02).
func TestRuntimeRefusalOffersTheUpdateWhenTheOrbIsBehind(t *testing.T) {
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/editor/203.0.113.8/clock/", nil)
	req.Header.Set("Accept", "text/html")
	runtimeRefused(rec, req, "clock", []string{"react", "baker"}, editorVersionPair{orb: "v1.1.43", hub: "v1.1.45"})
	body := rec.Body.String()
	for _, want := range []string{"v1.1.43", "v1.1.45", "Update clock on this orb", "type:'theme.update'", `app:"clock"`, "type:'theme.close'", "Back to themes"} {
		if !strings.Contains(body, want) {
			t.Errorf("the behind-the-shelf page lacks %q", want)
		}
	}
	// a hub that cannot tell which is newer keeps the general sentence and no button
	rec2 := httptest.NewRecorder()
	runtimeRefused(rec2, req, "clock", []string{"baker@9"}, editorVersionPair{orb: "v1.1.45", hub: ""})
	if strings.Contains(rec2.Body.String(), "theme.update") || !strings.Contains(rec2.Body.String(), "update the hub") || !strings.Contains(rec2.Body.String(), "Back to themes") {
		t.Error("with no shelf version the page must not offer an app update")
	}
	// the JSON answer carries both stamps
	rec3 := httptest.NewRecorder()
	runtimeRefused(rec3, httptest.NewRequest(http.MethodGet, "/editor/203.0.113.8/clock/x.js", nil), "clock", []string{"react"}, editorVersionPair{orb: "v1.1.43", hub: "v1.1.45"})
	if !strings.Contains(rec3.Body.String(), `"orb_version":"v1.1.43"`) || !strings.Contains(rec3.Body.String(), `"hub_version":"v1.1.45"`) {
		t.Errorf("JSON refusal = %s", rec3.Body)
	}
}

// After an install the card holds new containers, so the editor the hub remembered for
// that orb is stale: the card-cache drop every install path makes drops it too.
func TestAnInstallForgetsTheEditorItRemembered(t *testing.T) {
	const ip = "203.0.113.12"
	raw := zipWith(t, map[string]string{"editor.json": `{"entry":"index.html"}`, "index.html": `<body>old</body>`})
	s, dialed := editorHub(t, ip, raw, nil)
	editorGet(t, s, "/editor/"+ip+"/clock/", true)
	editorGet(t, s, "/editor/"+ip+"/clock/", true)
	if len(*dialed) != 1 {
		t.Fatalf("the region is memoised: %d dials for two reads, want 1", len(*dialed))
	}
	s.dropThemeCache(ip)
	editorGet(t, s, "/editor/"+ip+"/clock/", true)
	if len(*dialed) != 2 {
		t.Errorf("after the cache drop the editor must be read again: %d dials, want 2", len(*dialed))
	}
}

// D43 §4.2: an editor framed by the hub wears the hub's look, so ui@1's tokens are the
// hub page's own. A token changed on one side and not the other is the clash this sheet
// exists to end.
func TestUISheetTokensAreTheHubsOwn(t *testing.T) {
	tokens := func(path string) map[string]string {
		raw, err := webFS.ReadFile(path)
		if err != nil {
			t.Fatalf("read %s: %v", path, err)
		}
		root := regexp.MustCompile(`(?s):root\{(.*?)\}`).FindStringSubmatch(string(raw))
		if root == nil {
			t.Fatalf("%s has no :root block", path)
		}
		out := map[string]string{}
		for _, m := range regexp.MustCompile(`(--[a-z]+):([^;}]+)`).FindAllStringSubmatch(root[1], -1) {
			out[m[1]] = strings.TrimSpace(m[2])
		}
		return out
	}
	hub, ui := tokens("web/app.css"), tokens("web/runtime/ui@1/ui.css")
	if len(hub) < 8 {
		t.Fatalf("the hub sheet carries only %d tokens", len(hub))
	}
	for k, v := range hub {
		if ui[k] != v {
			t.Errorf("token %s: hub %q, ui@1 %q", k, v, ui[k])
		}
	}
}
