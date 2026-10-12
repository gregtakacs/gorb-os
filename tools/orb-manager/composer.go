package main

import (
	"bytes"
	"fmt"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"strings"
	"time"
)

// The Theme Editor is the existing watch-composer: "Watch Composer.dc.html" plus
// four sibling .js files it loads by relative URL.
//
// Two sources, chosen per request. When the gorb-os clone is reachable the hub
// serves the composer straight from tools/watch-composer, which keeps one source
// of truth for the developer: edits appear on the next reload with no rebuild.
// When it is not — which is every end user, who has no clone and never will — the
// copy compiled into the binary is served instead, so the card works.
// `make composer` refreshes that copy; TestEmbeddedComposerMatchesSource pins it
// against the repo so the two cannot drift silently.
//
// index.html is remapped to the .dc.html so the hub's /composer/index.html link
// resolves.
const (
	composerDirRel   = "tools/watch-composer"
	composerEntry    = "Watch Composer.dc.html"
	composerBodyCap  = 1 << 16 // how far into the file to look for the <body> tag
	composerEmbedDir = "web/composer"
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
	name := strings.TrimPrefix(r.URL.Path, "/composer/")
	if name == "" || name == "index.html" {
		name = composerEntry
	}
	// Reject traversal: only a bare filename inside the composer dir is served.
	if name != path.Base(name) || strings.Contains(name, "..") {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}

	raw, modTime, source, err := s.readComposerFile(name)
	if err != nil {
		// Name the directory that was searched. A -repo value mangled by a shell
		// (cmd.exe eats one leading backslash from a UNC path) otherwise reads as
		// "the editor does not exist" rather than "I looked in the wrong place".
		http.Error(w, fmt.Sprintf("theme editor unavailable: no %s under %s", name, source), http.StatusServiceUnavailable)
		return
	}
	if name != composerEntry {
		// The sibling .js files load by relative URL and must stay byte-exact.
		http.ServeContent(w, r, name, modTime, bytes.NewReader(raw))
		return
	}
	// ServeContent, not ServeFile: ServeFile answers a request whose path ends
	// in index.html with a 301 to the directory, and a card click that bounces
	// through a redirect is a card click that can go wrong. Content type and
	// range handling are inferred from the name's extension either way, and the
	// file's own mtime keeps If-Modified-Since honest so a reload picks up edits.
	http.ServeContent(w, r, name, modTime, bytes.NewReader(injectAfterBody(raw, hubBackLink+hubDepWatch)))
}

// readComposerFile returns one composer file and where it came from. The clone
// wins when it has the file, so a developer's edits are live; the copy compiled
// into the binary answers when the clone is absent or incomplete, which is the
// normal state for a downloaded app.
func (s *Server) readComposerFile(name string) (raw []byte, modTime time.Time, source string, err error) {
	if s.repoDir != "" {
		dir := filepath.Join(s.repoDir, composerDirRel)
		full := filepath.Join(dir, name)
		if st, serr := os.Stat(full); serr == nil && !st.IsDir() {
			data, rerr := os.ReadFile(full)
			if rerr != nil {
				return nil, time.Time{}, dir, rerr
			}
			return data, st.ModTime(), dir, nil
		}
		// A named clone that lacks the file is a mistake worth surfacing, not a
		// silent fallback to a possibly older embedded copy.
		return nil, time.Time{}, dir, os.ErrNotExist
	}
	data, serr := webFS.ReadFile(composerEmbedDir + "/" + name)
	if serr != nil {
		return nil, time.Time{}, "the copy built into this binary", serr
	}
	return data, embeddedComposerTime(), "built into orb-manager", nil
}

// embeddedComposerTime is a fixed stamp for embedded files: they cannot change
// under a running process, and a stable mtime lets If-Modified-Since work.
func embeddedComposerTime() time.Time {
	return time.Unix(0, 0).UTC()
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
