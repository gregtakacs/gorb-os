package main

import (
	"bytes"
	"fmt"
	"mime"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
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

// hubDepWatch names the one failure the hub can see coming out of the composer and
// the app window cannot show: the editor pulls React, ReactDOM, and Babel from
// unpkg.com through support.js, and when that fetch is blocked it logs to a console
// nobody can open in a chromeless window and paints nothing. After a few seconds with
// no React, say what is missing instead of leaving a blank page.
//
// With the vendor tree present (see cmd/orbvendor) the hub rewrites those three URLs
// to /composer/vendor/ and this never fires; it stays as the honest message for the
// no-vendor build, where the CDN really is still required.
const hubDepWatch = `<script>setTimeout(function(){` + "\n" +
	`  if (window.React) return;` + "\n" +
	`  document.body.insertAdjacentHTML("beforeend",` + "\n" +
	`    '<div style="position:fixed;left:10px;bottom:10px;z-index:2147483647;max-width:28em;' +` + "\n" +
	`      'padding:10px 12px;border:1px solid #c9a25a;border-radius:6px;background:#12110f;' +` + "\n" +
	`      'color:#f0d79a;font:13px/1.4 system-ui,sans-serif">The Theme Editor could not load ' +` + "\n" +
	`      'React from unpkg.com. It needs internet the first time it opens.</div>');` + "\n" +
	`},6000);</script>`

// cdnLocal maps each CDN URL support.js reaches for to its vendored sibling under
// /composer/vendor/. The URLs are the exact strings support.js compares against, so
// the map is the whole contract and a typo is a silent miss rather than an error.
var cdnLocal = map[string]string{
	"https://unpkg.com/react@18.3.1/umd/react.production.min.js":         "/composer/vendor/react.production.min.js",
	"https://unpkg.com/react-dom@18.3.1/umd/react-dom.production.min.js": "/composer/vendor/react-dom.production.min.js",
	"https://unpkg.com/@babel/standalone@7.29.0/babel.min.js":            "/composer/vendor/babel.min.js",
}

// googleFontsHost is the stylesheet origin the composer's <helmet> links point at.
// The link tags live inside the <x-dc> template support.js parses, so the rewrite is a
// plain text substitution on the served bytes rather than a DOM edit.
const googleFontsHost = "https://fonts.googleapis.com/css2"

// vendorDirRel is where cmd/orbvendor writes the offline copies, under web/ so the
// existing //go:embed web directive picks them up with no new directive to forget.
const vendorDirRel = "vendor"

// vendorNeeded lists the files that must all be present before the hub claims the
// editor can work offline. A partial tree is worse than none: rewriting three CDN
// URLs where only two files landed turns a clear "no internet" message into a blank
// editor with no explanation, so readiness is all-or-nothing by construction.
var vendorNeeded = []string{
	"vendor/react.production.min.js",
	"vendor/react-dom.production.min.js",
	"vendor/babel.min.js",
	"vendor/fonts.css",
}

// vendorReady reports whether every file the offline rewrite points at is in the
// binary. It reads the embedded FS rather than the disk, because the shipped binary is
// the thing that has to work without a clone.
func vendorReady() bool {
	for _, name := range vendorNeeded {
		f, err := webFS.Open(composerEmbedDir + "/" + name)
		if err != nil {
			return false
		}
		_ = f.Close()
	}
	return true
}

// vendorFontsCSS reports whether the fonts specifically were vendored. The scripts and
// the fonts are separable failures: React is required for the editor to appear at all,
// while a missing fonts.css just means faces fall back, so the font rewrite is gated
// on its own file rather than on the whole tree.
func vendorFontsCSSPresent() bool {
	f, err := webFS.Open(composerEmbedDir + "/" + vendorDirRel + "/fonts.css")
	if err != nil {
		return false
	}
	_ = f.Close()
	return true
}

// injectOffline rewrites the CDN references the page would otherwise reach for, and
// prelude is injected ahead of support.js so its loader sees the override map before
// it asks for anything.
func injectOffline(raw []byte, withFonts bool) []byte {
	s := string(raw)
	for url, local := range cdnLocal {
		s = strings.ReplaceAll(s, url, local)
	}
	if withFonts {
		s = regexp.MustCompile(`href="`+regexp.QuoteMeta(googleFontsHost)+`[^"]*"`).
			ReplaceAllString(s, `href="/composer/vendor/fonts.css"`)
		// A preconnect hint to the font host would still open a socket to Google on a
		// machine with no route to it; with the faces vendored the hint is pure cost.
		s = regexp.MustCompile(`(?m)^<link rel="preconnect" href="https://fonts\.(googleapis|gstatic)\.com">\n`).
			ReplaceAllString(s, "")
		// Collapse the duplicates: the same stylesheet referenced eleven times is
		// eleven requests for one file.
		seen := false
		var keep []string
		for _, line := range strings.Split(s, "\n") {
			if strings.Contains(line, `href="/composer/vendor/fonts.css"`) {
				if seen {
					continue
				}
				seen = true
			}
			keep = append(keep, line)
		}
		s = strings.Join(keep, "\n")
	}
	return []byte(s)
}

func (s *Server) routesComposer(mux *http.ServeMux) {
	mux.HandleFunc("/composer/", s.serveComposer)
}

func (s *Server) serveComposer(w http.ResponseWriter, r *http.Request) {
	name := strings.TrimPrefix(r.URL.Path, "/composer/")
	if name == "" || name == "index.html" {
		name = composerEntry
	}
	// The vendored offline copies are a separate namespace, served only from the
	// embed and never from the clone: they are generated bytes with a manifest and a
	// sha256, and a developer's repo must not be able to shadow them. Each component
	// is checked against traversal rather than relying on a Base() comparison that
	// would also reject every legitimate path in the subtree.
	if strings.HasPrefix(name, vendorDirRel+"/") {
		s.serveVendor(w, r, strings.TrimPrefix(name, vendorDirRel+"/"))
		return
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
		// The sibling .js files are served byte-exact, support.js included: it is
		// vendor bytes, pinned by TestEmbeddedComposerMatchesSource and by
		// TestComposerServesSiblingsUntouched. Making the editor offline does NOT
		// require rewriting it — support.js consults window.__resources for each CDN
		// URL before falling back, and that map is injected into the entry document.
		// Touching this file would be the wrong fix even if it worked today.
		//
		// no-cache, not immutable: the bare <script src> tags carry no version query,
		// and the 10/21 hub bake failure was exactly this skew — a fresh page against
		// a heuristic-cached engine from before D34, crashing on a function the new
		// page calls. Always-revalidate costs a 304 per reload and makes the skew
		// impossible. The 3 MB of vendor keeps its immutable header (see serveVendor).
		w.Header().Set("Cache-Control", "no-cache")
		http.ServeContent(w, r, name, modTime, bytes.NewReader(raw))
		return
	}
	// ServeContent, not ServeFile: ServeFile answers a request whose path ends
	// in index.html with a 301 to the directory, and a card click that bounces
	// through a redirect is a card click that can go wrong. Content type and
	// range handling are inferred from the name's extension either way, and the
	// file's own mtime keeps If-Modified-Since honest so a reload picks up edits.
	body := raw
	if vendorReady() {
		// Order matters and has broken before: the CDN URL rewrite must run FIRST,
		// because it is a plain text substitution and would otherwise rewrite the
		// URLs inside the override map it just inserted — turning
		// __resources["https://unpkg.com/react…"] into a key nothing looks up, and
		// silently leaving the editor online-only. Then the map goes in ahead of the
		// support.js tag, whose loader reads it the moment it runs.
		body = injectBeforeSupport(injectOffline(raw, true))
	}
	// Same always-revalidate rule as the sibling scripts: an entry page cached
	// against a revalidated engine is the mirror-image of the same skew.
	w.Header().Set("Cache-Control", "no-cache")
	http.ServeContent(w, r, name, modTime, bytes.NewReader(injectAfterBody(body, hubBackLink+hubDepWatch)))
}

// supportSrc is the relative script tag support.js is loaded by in the composer entry.
// The offline map has to be parsed before it.
const supportSrc = "support.js"

// injectBeforeSupport drops the __resources override map in front of the support.js
// script tag. If that tag is not where it is expected — a re-vendored composer that
// renamed the loader, say — the document is returned unchanged, which leaves the
// editor online-only rather than serving it with the mapping in the wrong place.
func injectBeforeSupport(raw []byte) []byte {
	if vendorOfflineMap == "" {
		return raw
	}
	m := regexp.MustCompile(`<script[^>]*src="\./?` + regexp.QuoteMeta(supportSrc) + `"[^>]*></script>`)
	loc := m.FindIndex(raw)
	if loc == nil {
		return raw
	}
	out := make([]byte, 0, len(raw)+len(vendorOfflineMap)+1)
	out = append(out, raw[:loc[0]]...)
	out = append(out, vendorOfflineMap...)
	out = append(out, '\n')
	out = append(out, raw[loc[0]:]...)
	return out
}

// vendorOfflineMap is the <script> that sets window.__resources, built once at
// package init so each request does not rebuild and re-sort the same string.
var vendorOfflineMap = func() string {
	if len(cdnLocal) == 0 {
		return ""
	}
	var b strings.Builder
	b.WriteString("<script>window.__resources=window.__resources||{};")
	for _, url := range sortedCDNURLs() {
		fmt.Fprintf(&b, "window.__resources[%s]=%s;", strconv.Quote(url), strconv.Quote(cdnLocal[url]))
	}
	b.WriteString("</script>")
	return b.String()
}()

func sortedCDNURLs() []string {
	out := make([]string, 0, len(cdnLocal))
	for u := range cdnLocal {
		out = append(out, u)
	}
	sort.Strings(out)
	return out
}

// serveVendor hands out the offline copies from the embed only. The path is resolved
// inside web/composer/vendor/ and every element is checked, so a request cannot walk
// out of that subtree; a name that is not vendored answers 404 rather than being
// resolved against the caller's directory.
func (s *Server) serveVendor(w http.ResponseWriter, r *http.Request, rel string) {
	// Reject an attempt to leave the subtree explicitly rather than letting Clean
	// quietly normalise it away: a silently-rewritten path is one whose intent nobody
	// can read out of a log line, and "not vendored" is the right answer to a probe.
	if strings.Contains(rel, "..") || strings.HasPrefix(rel, "/") {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}
	full := composerEmbedDir + "/" + vendorDirRel + "/" + path.Clean("/" + strings.ReplaceAll(rel, "\\", "/"))[1:]
	data, err := webFS.ReadFile(full)
	if err != nil {
		// embed.FS.ReadFile reports an error for a directory too, so one check covers
		// "never vendored" and "asked for the folder instead of a file in it".
		http.Error(w, "not vendored: run make vendor-composer", http.StatusNotFound)
		return
	}
	// These bytes are immutable for this binary, so let the browser cache them hard:
	// the 3 MB of Babel is exactly the file nobody wants to refetch per reload.
	w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
	if mt := mime.TypeByExtension(path.Ext(full)); mt != "" {
		w.Header().Set("Content-Type", mt)
	}
	// Served under its own name so the browser resolves the woff2 URLs inside
	// fonts.css, which are relative to it.
	http.ServeContent(w, r, path.Base(full), embeddedComposerTime(), bytes.NewReader(data))
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
