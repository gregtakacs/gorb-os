package main

import (
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
const composerDirRel = "tools/watch-composer"

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
		name = "Watch Composer.dc.html"
	}
	// Reject traversal: only a bare filename inside the composer dir is served.
	if name != path.Base(name) || strings.Contains(name, "..") {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}
	full := filepath.Join(dir, name)
	if st, err := os.Stat(full); err != nil || st.IsDir() {
		http.NotFound(w, r)
		return
	}
	// Content-Type is inferred by ServeFile from the extension.
	http.ServeFile(w, r, full)
}
