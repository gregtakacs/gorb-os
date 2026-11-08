package main

import (
	"net/http"
	"strings"
	"testing"
)

// theme-editor-kit-plan §8: an editor.json with "host": 1 names no page. The hub serves
// Orb Manager's own page at the editor's address, with the app's module after the runtime,
// and the launch token on it, as on any editor's entry document.
func TestHostedEditorServesTheHubsPageWithTheAppsModule(t *testing.T) {
	const ip = "203.0.113.7"
	raw := zipWith(t, map[string]string{
		"editor.json": `{"editor":1,"host":1,"module":["radar-bake.js","radar-editor.js"],"runtime":["baker@1","maps@1","host@1","theme@1","studio@1","ui@1","fonts@1","react@18.3.1"]}`,
		"radar-bake.js":   `var b`,
		"radar-editor.js": `OrbHost.register({})`,
		"orb-app.js":      `window.__ORB_APP = {}`,
	})
	s, _ := editorHub(t, ip, raw, nil)
	rec := editorGet(t, s, "/editor/"+ip+"/radar/", true)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d body %s", rec.Code, rec.Body)
	}
	body := rec.Body.String()
	host := strings.Index(body, `src="/runtime/host@1/host.js"`)
	bake := strings.Index(body, `src="radar-bake.js"`)
	mod := strings.Index(body, `src="radar-editor.js"`)
	if host < 0 || bake < 0 || mod < 0 || !(host < bake && bake < mod) {
		t.Errorf("the page loads host@1, then the module in its order; got host %d bake %d module %d:\n%s", host, bake, mod, body)
	}
	if !strings.Contains(body, `src="/runtime/maps@1/orbmap.js"`) {
		t.Error("maps@1 is listed, so the page loads it")
	}
	if c := cookieNamed(rec, tokenCookie); c == nil || c.Value != s.token {
		t.Error("the hosted page was served without the launch token, so it cannot upload")
	}
	if ct := rec.Header().Get("Content-Type"); !strings.HasPrefix(ct, "text/html") {
		t.Errorf("Content-Type = %q", ct)
	}
	// a module member is served as itself, without the token
	js := editorGet(t, s, "/editor/"+ip+"/radar/radar-editor.js", true)
	if js.Code != http.StatusOK || !strings.Contains(js.Body.String(), "OrbHost.register") || cookieNamed(js, tokenCookie) != nil {
		t.Errorf("the module file: status %d, token %v", js.Code, cookieNamed(js, tokenCookie) != nil)
	}
	// an app that does not list maps@1 gets no map script
	raw2 := zipWith(t, map[string]string{
		"editor.json": `{"host":1,"module":["a.js"],"runtime":["host@1"]}`,
		"a.js":        `x`,
	})
	s2, _ := editorHub(t, ip, raw2, nil)
	if b := editorGet(t, s2, "/editor/"+ip+"/radar/", true).Body.String(); strings.Contains(b, "orbmap.js") {
		t.Error("maps@1 is not listed, so the page should not load it")
	}
}

// A hosted manifest that cannot be served is refused by name, never served a page that
// would load a 404 and paint nothing.
func TestHostedEditorRefusesAManifestItCannotServe(t *testing.T) {
	const ip = "203.0.113.7"
	cases := []struct{ name, manifest, want string }{
		{"newer contract", `{"host":2,"module":["a.js"],"runtime":["host@1"]}`, "host contract 2"},
		{"no host@1", `{"host":1,"module":["a.js"],"runtime":["theme@1"]}`, "lists host@1"},
		{"no module", `{"host":1,"runtime":["host@1"]}`, "names its module"},
		{"module missing", `{"host":1,"module":["gone.js"],"runtime":["host@1"]}`, "gone.js is not in the editor"},
		{"module path", `{"host":1,"module":["../a.js"],"runtime":["host@1"]}`, "not a plain .js file"},
		{"module page", `{"host":1,"module":["a.html"],"runtime":["host@1"]}`, "not a plain .js file"},
		{"unknown runtime", `{"host":1,"module":["a.js"],"runtime":["host@1","warp@9"]}`, "warp@9"},
	}
	for _, c := range cases {
		raw := zipWith(t, map[string]string{"editor.json": c.manifest, "a.js": "x", "a.html": "<p>"})
		s, _ := editorHub(t, ip, raw, nil)
		rec := editorGet(t, s, "/editor/"+ip+"/radar/", true)
		if rec.Code == http.StatusOK || !strings.Contains(rec.Body.String(), c.want) {
			t.Errorf("%s: status %d body %q, want a refusal naming %q", c.name, rec.Code, rec.Body.String(), c.want)
		}
		if cookieNamed(rec, tokenCookie) != nil {
			t.Errorf("%s: a refusal carried the launch token", c.name)
		}
	}
}
