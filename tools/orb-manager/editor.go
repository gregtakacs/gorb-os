package main

// D37 §5.3: the hub serves an app's editor to the browser, same-origin.
//
// The chain is short and every link is in this file: dial GET
// /api/v1/apps/editor?id=slug, read the region the device streamed, treat those
// bytes as a ZIP, and serve a member from it under /editor/<ip>/<slug>/<file>. The
// hub is the only origin that can both hold the launch token and reach the orb, so
// a same-origin document is the only shape an editor can arrive in that keeps
// /api/theme/upload working without adding a CORS header to a device that has none.
//
// What is NOT here matters as much. The heavy runtime — React, ReactDOM, Babel — is
// served by the hub from its own vendor tree and never travels inside a container
// (§5.1). An app's ZIP carries its UI and its manifest: a few dozen KB. That is what
// keeps an editor under the 2 MiB region cap for a runtime the app does not own.
//
// The region is re-read from the device and nothing is written to disk. The card is
// the store; the hub is a window onto it. A long cache would be a copy of the card
// that the card does not know about.

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"html/template"
	"io"
	"mime"
	"net/http"
	"path"
	"strings"
	"sync"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// editorMaxEntry bounds one served member. The region is capped upstream at
// orbnet.MaxEditorRegion; this is the per-file limit, and it exists because a ZIP
// member's declared size is attacker-chosen until the bytes have actually been read.
const editorMaxEntry = 4 << 20

// editorManifestName is the file that names an editor's entry document. §5.1 leaves
// the layout to the app and asks only for this one file, so an app that arranges its
// files differently still opens.
const editorManifestName = "editor.json"

// editorFetchTTL is how long one region read is reused. An editor page pulls
// several members, and each would otherwise cost a dial to a board over a radio; a
// short window turns that into one fetch without becoming a copy nobody invalidates.
const editorFetchTTL = 15 * time.Second

// editorErrRetryTTL is how long a FAILED read stays memoised. A good region and a
// named "this app ships no editor" are stable answers worth caching whole; a
// transport failure is neither. The 15 s window turned one deadline into fifteen
// seconds in which the Retry button re-served the same error and the pane looked
// bricked — the report of 2026-10-27. Two seconds still debounces a double-click
// and a pageful of sibling members hammering a dead board, and after that the
// next dial is real.
const editorErrRetryTTL = 2 * time.Second

// editorDialRetryPause is the beat before the region dial's one retry. Radio
// hiccups the orb's own socket purge causes drain in hundreds of ms; anything
// longer belongs to the memo, not to a handler holding the door open.
const editorDialRetryPause = 400 * time.Millisecond

func (s *Server) routesEditor(mux *http.ServeMux) {
	// requireControl and not a read-guard: this hub has no read tier, and every
	// theme-surface GET that dials a device is guarded the same way for the same
	// reason — the token is what says the requester is a page this hub served.
	mux.Handle("/editor/", s.requireControl(http.HandlerFunc(s.serveEditor)))
}

// editorKey is one board's one app. The ip is part of it because two orbs can carry
// the same slug with different bytes on each.
type editorKey struct{ ip, slug string }

type editorSlot struct {
	zr      *zip.Reader
	err     error
	fetched time.Time
}

// editorRegions memoises region reads per hub, behind the fetch lane so two browsers
// opening one editor do not open two dials for the same bytes.
type editorRegions struct {
	mu    sync.Mutex
	slots map[editorKey]*editorSlot
}

var regions = &editorRegions{slots: map[editorKey]*editorSlot{}}

// editorRegion returns one app's editor archive, fetching it when the memo has
// nothing fresh. s.editorFetch does the dial so a test can stand in for a board.
// A fresh transport failure is retried once before it is recorded: the dial is an
// idempotent GET, and the failure the owner met (a read torn by the orb's socket
// purge under load) is exactly the kind a second dial clears. ErrNoEditor is never
// retried — it is a stable answer, and dialling twice just pays for it twice.
func (s *Server) editorRegion(ip, slug string) (*zip.Reader, error) {
	k := editorKey{ip, slug}
	regions.mu.Lock()
	defer regions.mu.Unlock()
	if sl, ok := regions.slots[k]; ok {
		ttl := editorFetchTTL
		if sl.err != nil && !errors.Is(sl.err, orbnet.ErrNoEditor) {
			ttl = editorErrRetryTTL
		}
		if time.Since(sl.fetched) < ttl {
			return sl.zr, sl.err
		}
	}
	raw, err := s.fetchEditor(ip, slug)
	if err != nil && !errors.Is(err, orbnet.ErrNoEditor) {
		time.Sleep(editorDialRetryPause)
		raw, err = s.fetchEditor(ip, slug)
	}
	var zr *zip.Reader
	if err == nil {
		zr, err = zip.NewReader(bytes.NewReader(raw), int64(len(raw)))
		if err != nil {
			err = fmt.Errorf("the editor region for %s is not a ZIP archive: %w", slug, err)
		}
	}
	regions.slots[k] = &editorSlot{zr: zr, err: err, fetched: time.Now()}
	return zr, err
}

// editorFetch is the dial. It is a field rather than a call so a test can stand in
// for a board without one; newHub wires it to the real client. Splitting it out is
// the same reason the hub holds s.device as an interface-shaped pointer: the device
// is the part of the system a hub test cannot conjure, and a handler that dials
// inline cannot be tested at all.
type editorFetchFn func(ip, slug string) ([]byte, error)

// editorInvalidate drops every memoised region. A theme upload or an app install
// can replace the container an editor came out of, and after either the next open
// must re-read rather than serve the archive the card no longer holds.
func editorInvalidate() {
	regions.mu.Lock()
	defer regions.mu.Unlock()
	regions.slots = map[editorKey]*editorSlot{}
}

// serveEditor answers GET /editor/<ip>/<slug>/<file>. A request naming no file
// resolves the entry document from the app's own editor.json, so an app may arrange
// its files as it likes and the hub still knows where to start.
func (s *Server) serveEditor(w http.ResponseWriter, r *http.Request) {
	rest := strings.TrimPrefix(r.URL.Path, "/editor/")
	ip, rest, ok := strings.Cut(rest, "/")
	if !ok || !isIPv4(ip) {
		http.Error(w, "expected /editor/<ipv4>/<slug>/<file>", http.StatusBadRequest)
		return
	}
	// The same learned-address gate every theme dial passes: the hub reaches orbs it
	// has seen, not addresses a page invented.
	t, okTarget := s.checkedTarget(w, ip, "", false)
	if !okTarget {
		return
	}
	slug, file, ok := strings.Cut(rest, "/")
	if !ok {
		slug, file = rest, ""
	}
	if !orbnet.AppStemOK(slug) {
		http.Error(w, "not an app name", http.StatusNotFound)
		return
	}
	if strings.Contains(file, "..") || strings.HasPrefix(file, "/") {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}
	file = path.Clean("/" + strings.ReplaceAll(file, "\\", "/"))[1:]

	zr, err := s.editorRegion(t.IP, slug)
	if err != nil {
		editorRefused(w, r, err)
		return
	}
	if file == "" || file == "." {
		file = editorManifestName
	}
	// The manifest is served as itself when asked for by name, and consulted when
	// the caller asked for the editor rather than a file in it.
	if file == editorManifestName {
		if raw, found := editorMember(zr, editorManifestName); found {
			if entry := rawEntryName(raw); entry != "" {
				file = entry
			} else {
				// A manifest with no usable entry is the app's problem to fix,
				// not the hub's to guess past: say which, rather than opening a
				// different file and letting the author wonder.
				w.Header().Set("Content-Type", "application/json")
				w.Header().Set("Cache-Control", "no-store")
				_, _ = w.Write(raw)
				return
			}
		} else {
			// No manifest at all: index.html is what an archive without one means.
			file = "index.html"
		}
	}
	data, found, err := editorBytes(zr, file)
	if err != nil {
		editorRefused(w, r, err)
		return
	}
	if !found {
		http.Error(w, "this app's editor ships no "+file, http.StatusNotFound)
		return
	}
	// Only the entry document receives the launch token. It is the document that
	// will make the upload; a sibling .js has no business holding a credential, and
	// handing one to every member of an unreviewed archive would be a wider grant
	// than the composer page has today.
	if isHTMLDoc(file) {
		setLaunchToken(w, s.token)
	}
	w.Header().Set("Cache-Control", "no-store")
	if mt := mime.TypeByExtension(path.Ext(file)); mt != "" {
		w.Header().Set("Content-Type", mt)
	}
	http.ServeContent(w, r, path.Base(file), time.Time{}, bytes.NewReader(data))
}

// editorRefused answers a failed region read. "This app ships no editor" is the
// ordinary state for every app predating D37, so it is a quiet 404 in the device's
// own words; everything else is a board that would not answer, which is a 502 and
// worth seeing.
//
// WHO is asking changes the SHAPE. A member fetch (a .js, a .css) wants the JSON
// the bridge can parse. A document navigation — the frame opening the editor —
// renders whatever bytes come back, and an iframe full of raw Go text is the
// unrecoverable screen the owner reported on 2026-10-27: no button, no name for
// what happened, close-the-pane-and-hope as the only exit. For Accept: text/html
// this answers a real page instead: the plain sentence, the reason it may be
// worth trying again, and a Retry anchor that re-navigates the frame. The page
// carries no token — a failure screen has no business holding a credential — and
// no-store, so a Retry cannot re-read a cached refusal.
func editorRefused(w http.ResponseWriter, r *http.Request, err error) {
	code := http.StatusBadGateway
	if errors.Is(err, orbnet.ErrNoEditor) {
		code = http.StatusNotFound
	}
	if strings.Contains(r.Header.Get("Accept"), "text/html") {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(code)
		escaped := template.HTMLEscapeString(err.Error())
		_, _ = fmt.Fprintf(w, `<!doctype html><meta charset="utf-8"><title>Editor did not load</title>`+
			`<style>body{font:15px/1.5 system-ui,sans-serif;margin:2rem;color:#222}code{background:#f2f2f2;padding:.1em .3em;border-radius:3px}`+
			`a.button{display:inline-block;margin-top:1rem;padding:.5em 1.2em;background:#3b5bdb;color:#fff;border-radius:6px;text-decoration:none}</style>`+
			`<h1>The editor did not load</h1><p>The hub asked this orb for its editor and got nothing usable back:</p>`+
			`<p><code>%s</code></p>`+
			`<p>A board mid-reboot, a radio that dropped the stream, or a card still waking all answer like this. `+
			`The read retried once on its own; this page did not corrupt anything.</p>`+
			`<a class="button" href="">Retry</a>`, escaped)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)
	_, _ = fmt.Fprintf(w, `{"ok":false,"error":%q}`, err.Error())
}

// editorMember returns one member's bytes by exact name.
func editorMember(zr *zip.Reader, name string) ([]byte, bool) {
	b, found, err := editorBytes(zr, name)
	return b, found && err == nil
}

// editorBytes reads one member, refusing a declared size past editorMaxEntry before
// allocating: a ZIP header is attacker-chosen, and "uncompressed size" is a promise
// and not a fact until the bytes are in hand.
func editorBytes(zr *zip.Reader, name string) ([]byte, bool, error) {
	for _, f := range zr.File {
		if f.Name != name {
			continue
		}
		if f.UncompressedSize64 > uint64(editorMaxEntry) {
			return nil, false, fmt.Errorf("the editor member %q claims %d B, past the %d B one file may ship",
				name, f.UncompressedSize64, editorMaxEntry)
		}
		rc, err := f.Open()
		if err != nil {
			return nil, true, err
		}
		defer rc.Close()
		b, err := io.ReadAll(io.LimitReader(rc, int64(editorMaxEntry)+1))
		if err != nil {
			return nil, true, err
		}
		if len(b) > editorMaxEntry {
			return nil, true, fmt.Errorf("the editor member %q is larger than the %d B one file may ship",
				name, editorMaxEntry)
		}
		return b, true, nil
	}
	return nil, false, nil
}

// rawEntryName lifts the "entry" field out of an editor.json body, "" when there is
// no usable one.
func rawEntryName(raw []byte) string {
	var m map[string]any
	if err := json.Unmarshal(raw, &m); err != nil {
		return ""
	}
	s, _ := m["entry"].(string)
	s = strings.TrimSpace(strings.TrimPrefix(s, "./"))
	// An entry that is not a plain relative html file is not a hole in the archive
	// to hand a browser: it is a malformed manifest, and the caller's fallback.
	if s == "" || strings.Contains(s, "..") || strings.HasPrefix(s, "/") || !isHTMLDoc(s) {
		return ""
	}
	return s
}

// isHTMLDoc names the members that are documents rather than assets. A .dc.html
// counts: it is the extension the composer's own entry file carries.
func isHTMLDoc(name string) bool {
	low := strings.ToLower(name)
	return strings.HasSuffix(low, ".html") || strings.HasSuffix(low, ".htm")
}
