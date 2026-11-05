package main

// theme-editor-kit-plan K10: one library per app, one entry per name.
//
// The hub's library for an app is its shelf themes (themes/, filed by app and baked name)
// and its drafts (designs/<app>/, filed by name), read together and merged by name. An
// entry is a draft (saved, never baked), a theme (baked, carrying its design), or both: a
// theme with unsent changes. The Themes tab and the editor's Start step list the same
// entries from GET /api/library/entries, so "what is in the library" has one answer.
//
// Keeping a theme folds its draft away (keepTheme), because the theme carries the design;
// a draft saved over a theme's name stays beside it until the next send.

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// LibTheme is an entry's baked half: its shelf file, and what the header and the design say.
type LibTheme struct {
	File      string `json:"file"`
	Stem      string `json:"stem"`
	Bytes     int    `json:"bytes"`
	Serial    uint32 `json:"serial"` // the bake's serial, the word the card's row carries too
	Baked     uint32 `json:"baked"`  // when it was baked, Unix seconds; 0 = the baker did not say
	At        int64  `json:"at"`
	MadeWith  string `json:"made_with,omitempty"` // the design's appVersion
	CanReopen bool   `json:"can_reopen"`          // it carries a design the editor can read
	Stale     string `json:"stale,omitempty"`     // why an orb would refuse it: it needs a rebake
}

// LibEntry is one name in an app's library.
type LibEntry struct {
	Name  string     `json:"name"`
	ID    string     `json:"id,omitempty"` // the card stem the orb files it under, to match a card row by
	State string     `json:"state"`        // draft | theme | changed (a theme with unsent changes)
	At    int64      `json:"at"`           // the newest of the two halves, for ordering
	Draft *DesignRow `json:"draft,omitempty"`
	Theme *LibTheme  `json:"theme,omitempty"`
	path  string     // the shelf file, for forget
}

// shelfApp is the app a shelf theme themes: its section slug, or the clock for a theme
// from before sections (FORMAT 2 themed the clock alone).
func shelfApp(t *orbnet.ShelfThemeInfo) string {
	if len(t.Sections) > 0 {
		return t.Sections[0]
	}
	return "clock"
}

// shelfThemes walks one shelf directory and calls fn for each file that is a theme.
func shelfThemes(dir string, fn func(path string, info os.FileInfo, t *orbnet.ShelfThemeInfo)) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return
	}
	for _, e := range entries {
		if e.IsDir() || !strings.EqualFold(filepath.Ext(e.Name()), ".orbtheme") {
			continue
		}
		p := filepath.Join(dir, e.Name())
		body, err := os.ReadFile(p)
		if err != nil {
			continue
		}
		t, err := orbnet.ShelfTheme(body)
		if err != nil {
			continue
		}
		info, _ := e.Info()
		fn(p, info, t)
	}
}

// madeWith reads the release a design names, without reading past its envelope.
func madeWith(design []byte) string {
	var h struct {
		AppVersion string `json:"appVersion"`
	}
	if json.Unmarshal(design, &h) != nil {
		return ""
	}
	return h.AppVersion
}

// entries is app's library: its shelf themes and its drafts, one entry per name, newest first.
func (l *Library) entries(app string) ([]LibEntry, error) {
	byName := map[string]*LibEntry{}
	get := func(n string) *LibEntry {
		if e := byName[n]; e != nil {
			return e
		}
		e := &LibEntry{Name: n}
		byName[n] = e
		return e
	}
	if l.ThemesDir != "" {
		shelfThemes(l.ThemesDir, func(p string, info os.FileInfo, t *orbnet.ShelfThemeInfo) {
			if shelfApp(t) != app {
				return
			}
			e := get(t.Name)
			if e.Theme != nil {
				return // two files under one name: the first listed stands; keepTheme writes one
			}
			th := &LibTheme{File: filepath.Base(p), Stem: strings.TrimSuffix(filepath.Base(p), filepath.Ext(p)),
				Bytes: t.Bytes, Serial: t.Serial, Baked: t.BakedS, CanReopen: t.Design != nil, Stale: t.Stale}
			if info != nil {
				th.At = info.ModTime().UnixMilli()
			}
			if t.Design != nil {
				th.MadeWith = madeWith(t.Design)
			}
			e.Theme, e.path = th, p
		})
	}
	if dir := l.designsDir(app); dir != "" {
		rows, err := listDesigns(dir)
		if err != nil {
			return nil, err
		}
		for i := range rows {
			e := get(rows[i].Name)
			r := rows[i]
			e.Draft = &r
		}
	}
	out := make([]LibEntry, 0, len(byName))
	for _, e := range byName {
		// a card row the orb refuses carries no name, only its stem: the stem is how the
		// face finds this entry's card copy to offer the fix (a rebake, or this bake)
		e.ID, _ = orbnet.ThemeIDOf(app, e.Name)
		switch {
		case e.Theme != nil && e.Draft != nil:
			e.State = "changed"
		case e.Theme != nil:
			e.State = "theme"
		default:
			e.State = "draft"
		}
		if e.Theme != nil {
			e.At = e.Theme.At
		}
		if e.Draft != nil && e.Draft.At > e.At {
			e.At = e.Draft.At
		}
		out = append(out, *e)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].At != out[j].At {
			return out[i].At > out[j].At
		}
		return out[i].Name < out[j].Name
	})
	return out, nil
}

// entry finds one name in app's library, or nil.
func (l *Library) entry(app, name string) *LibEntry {
	all, err := l.entries(app)
	if err != nil {
		return nil
	}
	for i := range all {
		if all[i].Name == name {
			return &all[i]
		}
	}
	return nil
}

func (s *Server) routesLibraryEntries(mux *http.ServeMux) {
	mux.Handle("/api/library/entries", s.requireControl(http.HandlerFunc(s.handleLibEntries)))
	mux.Handle("/api/library/design", s.requireControl(http.HandlerFunc(s.handleLibDesign)))
	mux.Handle("/api/library/keep", s.requireControl(http.HandlerFunc(s.handleLibKeep)))
	mux.Handle("/api/library/forget", s.requireControl(http.HandlerFunc(s.handleLibForget)))
}

func (s *Server) handleLibEntries(w http.ResponseWriter, r *http.Request) {
	app, _, ok := s.designApp(w, r)
	if !ok {
		return
	}
	out, err := s.library().entries(app)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	s.jsonOK(w, out)
}

// handleLibDesign answers an entry's newest design: its draft when it has one, else the
// design its theme carries; &from=theme asks for the theme's even beside a draft (Rebake
// all rebakes what was baked, never unsent changes). The body is the design document.
func (s *Server) handleLibDesign(w http.ResponseWriter, r *http.Request) {
	app, dir, ok := s.designApp(w, r)
	if !ok {
		return
	}
	name, err := designName(r.URL.Query().Get("name"))
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	e := s.library().entry(app, name)
	if e == nil {
		s.jsonErr(w, "no “"+name+"” in the "+app+" library on this hub")
		return
	}
	var body []byte
	if e.Draft != nil && !(r.URL.Query().Get("from") == "theme" && e.Theme != nil) {
		body, err = os.ReadFile(filepath.Join(dir, designFile(name)))
	} else {
		var raw []byte
		if raw, err = os.ReadFile(e.path); err == nil {
			var t *orbnet.ShelfThemeInfo
			if t, err = orbnet.ShelfTheme(raw); err == nil {
				if body = t.Design; body == nil {
					err = orbnet.ErrNoDesign
				}
			}
		}
	}
	if err != nil {
		s.jsonErr(w, strings.TrimPrefix(err.Error(), "orbnet: "))
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_, _ = w.Write(body)
}

// handleLibKeep answers POST /api/library/keep?app= with a baked theme as the body: the
// rebake's door, which files the theme on the shelf by name without sending it to an orb.
// It folds the name's draft like any kept theme, unless &fold=0: a rebake replaces the
// baked half of an entry and leaves its unsent changes standing.
func (s *Server) handleLibKeep(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	app, _, ok := s.designApp(w, r)
	if !ok {
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, int64(orbnet.MaxThemeUpload)+1))
	if err != nil || len(body) > orbnet.MaxThemeUpload {
		s.jsonErr(w, "that theme is over what an orb will accept")
		return
	}
	c, err := orbnet.ContainerName(body)
	if err != nil {
		s.jsonErr(w, strings.TrimPrefix(err.Error(), "orbnet: "))
		return
	}
	if len(c.Sections) == 0 || c.Sections[0] != app {
		s.jsonErr(w, "that theme is not one of the "+app+" app’s")
		return
	}
	dest, err := s.library().keepTheme(body, r.URL.Query().Get("fold") != "0")
	if err != nil {
		s.jsonErr(w, "the theme could not be kept: "+err.Error())
		return
	}
	s.jsonOK(w, map[string]any{"name": c.Name, "file": filepath.Base(dest)})
}

// handleLibForget answers POST /api/library/forget?app=&name=&what=draft|all: discard a
// theme's unsent changes, or take the whole entry out of the library. No orb is touched.
func (s *Server) handleLibForget(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	app, dir, ok := s.designApp(w, r)
	if !ok {
		return
	}
	name, err := designName(r.URL.Query().Get("name"))
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	what := r.URL.Query().Get("what")
	if what != "draft" && what != "all" {
		s.jsonErr(w, "what must be draft or all")
		return
	}
	lib := s.library()
	e := lib.entry(app, name)
	if e == nil {
		s.jsonErr(w, "no “"+name+"” in the "+app+" library on this hub")
		return
	}
	if e.Draft != nil {
		if err := os.Remove(filepath.Join(dir, designFile(name))); err != nil && !errors.Is(err, os.ErrNotExist) {
			s.jsonErr(w, "the draft could not be removed: "+err.Error())
			return
		}
	}
	if what == "all" && e.Theme != nil {
		if _, err := lib.Drop("theme", e.Theme.Stem); err != nil {
			s.jsonErr(w, err.Error())
			return
		}
	}
	s.jsonOK(w, map[string]any{"name": name, "what": what})
}
