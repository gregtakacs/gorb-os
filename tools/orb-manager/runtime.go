package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"html/template"
	"io/fs"
	"mime"
	"net/http"
	"path"
	"sort"
	"strings"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// The OS theme runtime (D43 §9). An app's editor authors content; the hub builds the
// theme file from it, and the pieces every editor builds with are the hub's, not the
// app's: the dc-runtime (support.js), the theme builder (orbtheme-baker.js), and the
// third-party libraries both load (React, ReactDOM, Babel, the font faces). They are
// compiled into the binary under web/runtime/ and served at
//
//	/runtime/<name>@<version>/<file>
//
// An app's editor.json names the runtime it needs as those ids ("baker@1", ...), and
// its documents reference these paths directly. The hub serves an id it holds and
// refuses an editor naming one it does not, in a sentence that names the id: an
// editor running against a runtime it was not written for is the silent failure this
// door exists to prevent. A breaking change to a runtime's interface is a new major
// id, and the hub keeps the old one for as long as an app on its shelf or a known orb
// still names it.
//
// The vendored libraries are one tree written by cmd/orbvendor (web/runtime/vendor/);
// the catalog below maps each versioned id onto the files it owns, so
// /runtime/babel@7.29.0/react.production.min.js is a 404 rather than an alias.
const runtimeEmbedDir = "web/runtime"

type runtimeEntry struct {
	dir    string   // under web/runtime/
	files  []string // the files this id serves; empty = every file under dir
	prefix string   // with files: a subtree this id also serves (the fonts' faces)
	vendor bool     // third-party bytes, immutable for a given version
}

var runtimeCatalog = map[string]runtimeEntry{
	"support@1":    {dir: "support@1"},
	"baker@1":      {dir: "baker@1"},
	"ui@1":         {dir: "ui@1"},    // the hub's look for an editor (D43 §4.2)
	"theme@1":      {dir: "theme@1"}, // the editor services: send (theme-editor-kit-plan K1)
	"react@18.3.1": {dir: "vendor", files: []string{"react.production.min.js", "react-dom.production.min.js"}, vendor: true},
	"babel@7.29.0": {dir: "vendor", files: []string{"babel.min.js"}, vendor: true},
	"fonts@1":      {dir: "vendor", files: []string{"fonts.css"}, prefix: "fonts/", vendor: true},
}

// runtimeHas reports whether this binary carries the id, every listed file included.
// A no-vendor build (web/runtime/vendor deleted) lacks the library ids and says so
// rather than serving an editor that will paint nothing.
func runtimeHas(id string) bool {
	e, ok := runtimeCatalog[id]
	if !ok {
		return false
	}
	if len(e.files) == 0 {
		entries, err := fs.ReadDir(webFS, runtimeEmbedDir+"/"+e.dir)
		return err == nil && len(entries) > 0
	}
	for _, f := range e.files {
		if _, err := fs.Stat(webFS, runtimeEmbedDir+"/"+e.dir+"/"+f); err != nil {
			return false
		}
	}
	return true
}

// runtimeIDs lists the ids this hub can serve, sorted, for the refusal sentence.
func runtimeIDs() []string {
	var out []string
	for id := range runtimeCatalog {
		if runtimeHas(id) {
			out = append(out, id)
		}
	}
	sort.Strings(out)
	return out
}

func (s *Server) routesRuntime(mux *http.ServeMux) {
	// Guarded like /editor/: the documents that load these are the hub's own frames,
	// which hold the launch token, and nothing else has a reason to ask.
	mux.Handle("/runtime/", s.requireControl(http.HandlerFunc(s.serveRuntime)))
}

func (s *Server) serveRuntime(w http.ResponseWriter, r *http.Request) {
	rest := strings.TrimPrefix(r.URL.Path, "/runtime/")
	id, rel, ok := strings.Cut(rest, "/")
	e, known := runtimeCatalog[id]
	if !ok || !known || rel == "" || strings.Contains(rel, "..") || strings.HasPrefix(rel, "/") || strings.Contains(rel, "\\") {
		http.Error(w, "not a runtime file", http.StatusNotFound)
		return
	}
	rel = path.Clean("/" + rel)[1:]
	if len(e.files) > 0 {
		allowed := e.prefix != "" && strings.HasPrefix(rel, e.prefix)
		for _, f := range e.files {
			allowed = allowed || rel == f
		}
		if !allowed {
			http.Error(w, "not a file of "+id, http.StatusNotFound)
			return
		}
	}
	data, err := webFS.ReadFile(runtimeEmbedDir + "/" + e.dir + "/" + rel)
	if err != nil {
		http.Error(w, "this hub does not carry "+id+"/"+rel, http.StatusNotFound)
		return
	}
	if e.vendor {
		// Third-party bytes at a versioned path never change: cache them hard. The 3 MB
		// of Babel is the file nobody wants to refetch per reload.
		w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
	} else {
		// The hub's own runtime moves with hub releases inside one major id, so it
		// revalidates: a fresh editor against a heuristically cached baker is the skew
		// the old composer route shipped once (2026-10-21).
		w.Header().Set("Cache-Control", "no-cache")
	}
	if mt := mime.TypeByExtension(path.Ext(rel)); mt != "" {
		w.Header().Set("Content-Type", mt)
	}
	http.ServeContent(w, r, path.Base(rel), time.Unix(0, 0).UTC(), bytes.NewReader(data))
}

// editorRuntimeMissing reads an editor.json and returns the runtime ids it names that
// this hub cannot serve. An entry with no version is an editor built before D43's
// runtime existed (it loads the retired /composer/ paths), and is missing by
// definition. A manifest with no runtime list asks for nothing and passes.
func editorRuntimeMissing(manifest []byte) []string {
	var m struct {
		Runtime []string `json:"runtime"`
	}
	if err := json.Unmarshal(manifest, &m); err != nil {
		return nil
	}
	var missing []string
	for _, id := range m.Runtime {
		if !strings.Contains(id, "@") || !runtimeHas(id) {
			missing = append(missing, id)
		}
	}
	return missing
}

// editorVersions are the two release stamps behind a refusal: the app the orb runs and
// the one on this hub's shelf. Either is "" when it cannot be read.
type editorVersionPair struct{ orb, hub string }

func (s *Server) editorVersions(ip, slug string) editorVersionPair {
	var v editorVersionPair
	if row := s.installedRow(ip, slug); row != nil {
		v.orb = row.Version
	}
	if lib := s.library(); lib != nil && lib.AppsDir != "" {
		if body, err := lib.Container(slug); err == nil {
			if c, err := orbnet.ParseApp(body); err == nil {
				v.hub = c.Version
			}
		}
	}
	return v
}

// runtimeRefused answers an editor this hub cannot run: a page for the frame, JSON for
// anything else. When the cause is an orb running an older release of the app than the
// hub's shelf carries — the ordinary case after a hub update — the page says so in those
// words and offers the fix: a button that asks the hub page (the frame's parent) to run
// the Apps face's own Update for this app on this orb, then reopen the editor.
func runtimeRefused(w http.ResponseWriter, r *http.Request, slug string, missing []string, v editorVersionPair) {
	have := strings.Join(runtimeIDs(), ", ")
	sentence := fmt.Sprintf("the %s editor needs %s, and this hub carries %s", slug, strings.Join(missing, ", "), have)
	cmp, comparable := orbnet.AppVersionCompare(v.hub, v.orb)
	orbOlder := comparable && cmp > 0
	w.Header().Set("Cache-Control", "no-store")
	if strings.Contains(r.Header.Get("Accept"), "text/html") {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.WriteHeader(http.StatusConflict)
		// The frame fills the hub window (D43 §4.2), so this page wears the hub's colours
		// and carries its own way back: the editor's back arrow is not here to press.
		css := `<style>body{font:15px/1.5 system-ui,sans-serif;margin:0;padding:2rem;background:#0e1116;color:#e6edf3;max-width:720px}` +
			`code{background:#161b22;border:1px solid #232c36;padding:.1em .3em;border-radius:3px}` +
			`button{margin:1rem .5rem 0 0;padding:.55em 1.2em;background:#1f6feb;color:#fff;border:1px solid #1f6feb;border-radius:6px;font:inherit;cursor:pointer}` +
			`button.back{background:none;color:#e6edf3;border-color:#232c36}button[disabled]{opacity:.6;cursor:default}.why{color:#8b98a5;font-size:13px;margin-top:1.5rem}</style>`
		back := `<button class="back" onclick="try{parent.postMessage({type:'theme.close'},location.origin)}catch(e){}">Back to themes</button>`
		if orbOlder {
			esc := template.HTMLEscapeString
			_, _ = fmt.Fprintf(w, `<!doctype html><meta charset="utf-8"><title>Update the %[1]s app on this orb</title>%[4]s`+
				`<h1>This orb's %[1]s app is older than the hub's</h1>`+
				`<p>The orb runs <b>%[1]s %[2]s</b>; this hub has <b>%[1]s %[3]s</b>. The editor comes from the app, so update the app on the orb to open the current editor.</p>`+
				`<button id="u">Update %[1]s on this orb</button>%[7]s<p id="s"></p>`+
				`<p class="why">Why: <code>%[5]s</code>.</p>`+
				`<script>document.getElementById('u').onclick=function(){this.disabled=true;`+
				`document.getElementById('s').textContent='Asking the hub to update the app… its progress shows at the foot of this window.';`+
				`try{parent.postMessage({type:'theme.update',app:%[6]q},location.origin);}catch(e){document.getElementById('s').textContent='The hub page did not answer: update it from the Apps page.';}};</script>`,
				esc(slug), esc(v.orb), esc(v.hub), css, esc(sentence), slug, back)
			return
		}
		_, _ = fmt.Fprintf(w, `<!doctype html><meta charset="utf-8"><title>Editor needs a newer hub or app</title>%s`+
			`<h1>This editor and this hub do not match</h1><p><code>%s</code>.</p>`+
			`<p>An entry with no version (no <code>@</code>) is an editor from before the hub served its own theme runtime: `+
			`install the current release of the app. A version this hub does not carry means the app is newer than the hub: update the hub.</p>%s`,
			css, template.HTMLEscapeString(sentence), back)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusConflict)
	_, _ = fmt.Fprintf(w, `{"ok":false,"error":%q,"missing":%s,"orb_version":%q,"hub_version":%q}`, sentence, mustJSON(missing), v.orb, v.hub)
}

func mustJSON(v any) string {
	b, _ := json.Marshal(v)
	return string(b)
}
