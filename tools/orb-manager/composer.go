package main

import (
	"bytes"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"strings"
)

// The Theme Editor is the existing watch-composer, which lives in the repo as
// "Watch Composer.dc.html" plus four sibling .js files it loads by relative
// URL. Serving the directory (not embedding a copy) keeps one source of truth:
// edits to the composer show up on next reload with no rebuild. index.html is
// remapped to the .dc.html so the hub's /composer/index.html link resolves.
const (
	composerDirRel  = "tools/watch-composer"
	composerEntry   = "Watch Composer.dc.html"
	composerBodyCap = 1 << 16 // how far into the file to look for the <body> tag
)

// hubBackLink lands right after the composer's <body> tag when the hub serves
// it. The app window is chromeless: no address bar, no back button, so a
// one-way card into the editor would strand the user there. The file on disk is
// never touched — opened from the filesystem the composer is the same
// standalone tool it always was.
const hubBackLink = `<a href="/" style="position:fixed;right:10px;bottom:10px;z-index:2147483647;` +
	`padding:6px 10px;border:1px solid #c9a25a;border-radius:6px;background:#12110f;` +
	`color:#c9a25a;font:12px/1.2 system-ui,sans-serif;text-decoration:none;opacity:.75">← Orb Manager</a>`

// hubDepWatch names the one failure the hub can see coming out of the composer
// and the app window cannot show: the editor pulls React, ReactDOM, and Babel
// off unpkg.com, and when that fetch is blocked it logs to a console nobody can
// open in a chromeless window and paints nothing. After a few seconds with no
// React, say what is missing instead of leaving a blank page.
const hubDepWatch = `<script>setTimeout(function(){` + "\n" +
	`  if (window.React) return;` + "\n" +
	`  document.body.insertAdjacentHTML("beforeend",` + "\n" +
	`    '<div style="position:fixed;left:10px;bottom:10px;z-index:2147483647;max-width:28em;' +` + "\n" +
	`      'padding:10px 12px;border:1px solid #c9a25a;border-radius:6px;background:#12110f;' +` + "\n" +
	`      'color:#f0d79a;font:13px/1.4 system-ui,sans-serif">The Theme Editor could not load ' +` + "\n" +
	`      'React from unpkg.com. It needs internet the first time it opens.</div>');` + "\n" +
	`},6000);</script>`

func (s *Server) routesComposer(mux *http.ServeMux) {
	mux.HandleFunc("/composer/", s.serveComposer)
}

func (s *Server) serveComposer(w http.ResponseWriter, r *http.Request) {
	if s.repoDir == "" {
		http.Error(w, "theme editor unavailable: not running inside a gorb-os clone", http.StatusServiceUnavailable)
		return
	}
	dir := filepath.Join(s.repoDir, composerDirRel)
	name := strings.TrimPrefix(r.URL.Path, "/composer/")
	if name == "" || name == "index.html" {
		name = composerEntry
	}
	// Reject traversal: only a bare filename inside the composer dir is served.
	if name != path.Base(name) || strings.Contains(name, "..") {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}
	full := filepath.Join(dir, name)
	st, err := os.Stat(full)
	if err != nil || st.IsDir() {
		http.NotFound(w, r)
		return
	}
	if name != composerEntry {
		// The sibling .js files load by relative URL and must stay byte-exact.
		http.ServeFile(w, r, full)
		return
	}
	// ServeContent, not ServeFile: ServeFile answers a request whose path ends
	// in index.html with a 301 to the directory, and a card click that bounces
	// through a redirect is a card click that can go wrong. Content type and
	// range handling are inferred from the name's extension either way, and the
	// file's own mtime keeps If-Modified-Since honest so a reload picks up edits.
	raw, err := os.ReadFile(full)
	if err != nil {
		http.Error(w, "theme editor unavailable: "+err.Error(), http.StatusServiceUnavailable)
		return
	}
	http.ServeContent(w, r, name, st.ModTime(), bytes.NewReader(injectAfterBody(raw, hubBackLink+hubDepWatch)))
}

// injectAfterBody drops snippet immediately after the opening <body> tag. With
// no tag to be found, the document is returned untouched: serving the editor
// without its link beats refusing to serve it.
func injectAfterBody(raw []byte, snippet string) []byte {
	i := bytes.Index(bytes.ToLower(raw[:min(len(raw), composerBodyCap)]), []byte("<body"))
	if i < 0 {
		return raw
	}
	end := bytes.IndexByte(raw[i:], '>')
	if end < 0 {
		return raw
	}
	j := i + end + 1
	out := make([]byte, 0, len(raw)+len(snippet))
	out = append(out, raw[:j]...)
	out = append(out, snippet...)
	return append(out, raw[j:]...)
}
