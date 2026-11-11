package main

// orb-os-net design §7.6 (the Updates list), ruling 26, §7.4 (themes from the site):
// one place that gathers every update this hub knows about.
//
// The list is a catalog and not a second way to act. The per-tab amber buttons stay
// exactly where they are; the manager row reports g3-1's cached verdict rather than
// fetching a release of its own; the firmware and app rows restate the reads the fleet
// card and the library already make. Only the site-theme rows live here end to end,
// because nothing else ever resolved an origin.
//
// Two refusals are wired at this step and both are stored in state.json: Skip this
// version, which is an answer about ONE version — the next newer one is offered again —
// and Keep my version, which ends the tracking of an imported theme for good by taking
// its `.origin.json` sidecar away (ruling 26: the copy is left alone, name included).
//
// g3-3 added the third answer, the one that acts: POST /api/updates/apply. Same name is
// the ordinary update — the newer bake replaces the shelf copy and goes where the old one
// was. A new name is the rename of §7.4, in three gated steps (bake into the library,
// install, and only then remove the old) so a failure at either of the first two leaves
// the old theme installed, selected and named exactly as it was.

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// updateBodyMax bounds a refusal's request body. One kind, one key and one version is
// all either door accepts, and a hub should not buffer more than that to be told no.
const updateBodyMax = 4096

// routesUpdates wires the list, its two refusals and the one answer that acts. Every one
// of them is requireControl: the list names the operator's shelf, a refusal rewrites the
// state file, and an apply writes the shelf, the orbs and the sidecars.
func (s *Server) routesUpdates(mux *http.ServeMux) {
	mux.Handle("/api/updates", s.requireControl(http.HandlerFunc(s.serveUpdates)))
	mux.Handle("/api/updates/skip", s.requireControl(http.HandlerFunc(s.h_skip)))
	mux.Handle("/api/updates/keep", s.requireControl(http.HandlerFunc(s.h_keep)))
	mux.Handle("/api/updates/apply", s.requireControl(http.HandlerFunc(s.h_apply)))
}

// updatesPayload is GET /api/updates's data.
type updatesPayload struct {
	Count int          `json:"count"`
	Items []updateItem `json:"items"`
	// DownloadURL is g3-1's cached asset link for this platform, carried once for the
	// manager row's Download button rather than repeated on every item.
	DownloadURL string `json:"downloadUrl,omitempty"`
}

// updateItem is one offer. The empty string and false flags mean "this row says nothing
// about it": a firmware row has no new name, a gone theme has no version to skip.
type updateItem struct {
	Kind    string   `json:"kind"`
	Key     string   `json:"key"`
	Title   string   `json:"title"`
	From    string   `json:"from,omitempty"`
	To      string   `json:"to,omitempty"`
	Version int      `json:"version,omitempty"`
	Renamed bool     `json:"renamed,omitempty"`
	Gone    bool     `json:"gone,omitempty"`
	Note    string   `json:"note,omitempty"`
	Choices []string `json:"choices"`
}

// packedVersion turns a release stamp into the single integer a skip is recorded
// against: v1.2.17 is 1_002_017. An unparsable stamp packs to 0, which is the number no
// offer can carry — so a hub that cannot name what it is offering never records a skip
// against it and never quietly hides the row.
func packedVersion(stamp string) int {
	v, ok := firmware.ParseVersion(strings.TrimSpace(stamp))
	if !ok {
		return 0
	}
	return v.Major*1_000_000 + v.Minor*1_000 + v.Patch
}

// serveUpdates answers GET /api/updates. It dials no orb, fetches no release and reads no
// card: every row is built from what the hub already holds or from a file it owns. A hub
// with nothing to report answers count 0 with an empty list, which is a normal answer and
// not an error.
func (s *Server) serveUpdates(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	out := updatesPayload{Items: []updateItem{}, DownloadURL: s.releaseDownloadURL()}
	var pl *LibraryPayload
	if lib, err := s.h_library(r); err == nil && lib != nil {
		pl, _ = lib.(*LibraryPayload)
	}
	out.Items = append(out.Items, s.managerUpdateRow()...)
	out.Items = append(out.Items, s.firmwareUpdateRows()...)
	if pl != nil {
		out.Items = append(out.Items, s.appUpdateRows(pl.Restorable)...)
		out.Items = append(out.Items, s.themeUpdateRows(r, pl)...)
	}
	out.Count = len(out.Items)
	s.jsonOK(w, out)
}

// releaseDownloadURL is the cached asset link g3-1 settled, read without touching the
// network. An empty string is the honest answer before the first check lands.
func (s *Server) releaseDownloadURL() string {
	s.relMu.Lock()
	defer s.relMu.Unlock()
	return s.rel.DownloadURL
}

// managerUpdateRow is the §7.6 Orb-Manager row, and it is a report of the cache and
// nothing else: the list's own path must not turn a page reload into a dial on the site.
func (s *Server) managerUpdateRow() []updateItem {
	s.relMu.Lock()
	rel := s.rel
	s.relMu.Unlock()
	if rel.Verdict != string(firmware.VerdictUpgrade) {
		return nil
	}
	return []updateItem{{Kind: "manager", Key: "hub", Title: "Orb Manager",
		From: rel.Current, To: rel.Latest, Choices: []string{"download"}}}
}

// firmwareUpdateRows restates what annotateUpdate stamps onto the fleet card: the hub's
// bundle is a decided upgrade over the stamp the orb in hand last answered with. The
// check is the same call with the same fail-closed rule, so the list and the amber dot
// can never disagree about what "newer" means.
//
// Its note is §7.6's pairing sentence, worded out of g3-1's cached latest — the site's
// release number doubles as the firmware it carries, so the sentence costs no fetch.
func (s *Server) firmwareUpdateRows() []updateItem {
	ip := s.currentOrbIP()
	if ip == "" || s.fleet == nil {
		return nil
	}
	b := s.firmwareBundle()
	if b == nil {
		return nil
	}
	var fw string
	for _, o := range s.fleet.Snapshot() {
		if o.IP == ip {
			fw = o.FW
			break
		}
	}
	if fw == "" {
		return nil
	}
	target := b.Release().Version
	if firmware.Compare(firmware.Release{Version: fw}, b.Release()) != firmware.VerdictUpgrade {
		return nil
	}
	to := packedVersion(target)
	if c, ok := s.choiceFor("firmware", "hub"); ok && c.SkippedVersion != 0 && c.SkippedVersion == to {
		return nil
	}
	item := updateItem{Kind: "firmware", Key: "hub", Title: "Firmware",
		From: fw, To: target, Version: to, Choices: []string{"skip"}}
	s.relMu.Lock()
	rel := s.rel
	s.relMu.Unlock()
	if rel.Verdict == string(firmware.VerdictUpgrade) && rel.Latest != "" {
		item.Note = "Orb Manager " + rel.Latest + " brings firmware " + rel.Latest
	}
	return []updateItem{item}
}

// appUpdateRows reflects the shelf's own read: the compiled-in apps this library lacks or
// holds at an older version (seed.go), which is the set the Apps face's Restore door
// answers with. It is a directory read and not a dial, so the row exists with no orb in
// hand, and the amber Update-everything button on the tab stays the way to act on it.
func (s *Server) appUpdateRows(restorable []RestorableApp) []updateItem {
	var out []updateItem
	for _, a := range restorable {
		to := packedVersion(a.Version)
		if c, ok := s.choiceFor("app", a.Slug); ok && c.SkippedVersion != 0 && c.SkippedVersion == to {
			continue
		}
		title := a.Title
		if title == "" {
			title = a.Slug
		}
		out = append(out, updateItem{Kind: "app", Key: a.Slug, Title: title,
			From: a.Have, To: a.Version, Version: to, Choices: []string{"skip"}})
	}
	return out
}

// themeUpdateRows is §7.4's discovery: every shelf theme carrying an origin is resolved
// against the site it came from, and a design whose current version is ahead of the one
// that was baked is offered. A theme the operator kept is never listed at all — its
// sidecar is already gone, so it never even reaches this loop — and one skipped at
// exactly this version is held back until a newer one appears.
//
// A resolve that fails is reported rather than smoothed over: the row says the theme
// could not be checked instead of implying it is current. A resolve the site could not
// answer yet (fu2-6) is a row of its own too — information, nothing to press, nothing
// recorded — and only a site that says "gone" in its own body retires a design.
func (s *Server) themeUpdateRows(r *http.Request, pl *LibraryPayload) []updateItem {
	var out []updateItem
	for _, t := range pl.Themes {
		if t.Origin == nil {
			continue
		}
		if c, ok := s.choiceFor("theme", t.Origin.DesignID); ok && c.Keep {
			continue
		}
		ans, err := s.resolveOrigin(r.Context(), t.Origin)
		if err != nil {
			out = append(out, updateItem{Kind: "theme", Key: t.Origin.DesignID, Title: t.Title,
				Note: "could not be checked: " + err.Error(), Choices: []string{}})
			continue
		}
		if ans.Gone {
			out = append(out, updateItem{Kind: "theme", Key: t.Origin.DesignID, Title: t.Title,
				Gone: true, Choices: []string{}})
			continue
		}
		if ans.NotYet {
			// A site that cannot answer the question yet is not news about the design: the
			// row is information, it offers nothing to press, and it records nothing — so
			// the next check asks this site again. siteHost names which one could not.
			out = append(out, updateItem{Kind: "theme", Key: t.Origin.DesignID, Title: t.Title,
				Note:    fmt.Sprintf("%s can’t check this theme for updates yet", siteHost(t.Origin.Site)),
				Choices: []string{}})
			continue
		}
		if ans.Version <= t.Origin.Version {
			continue
		}
		if c, ok := s.choiceFor("theme", t.Origin.DesignID); ok && c.SkippedVersion == ans.Version {
			continue
		}
		to := ans.SharedName
		if to == "" {
			to = t.Title
		}
		out = append(out, updateItem{Kind: "theme", Key: t.Origin.DesignID, Title: to,
			From: t.Title, To: to, Version: ans.Version,
			Renamed: ans.SharedName != "" && ans.SharedName != t.Title,
			Choices: []string{"update", "skip", "keep"}})
	}
	return out
}

// updateRefusal is either POST body: a kind, a key, and for a skip the version that was
// offered and turned down.
type updateRefusal struct {
	Kind    string `json:"kind"`
	Key     string `json:"key"`
	Version int    `json:"version"`
}

func (s *Server) readRefusal(r *http.Request) (updateRefusal, bool) {
	var in updateRefusal
	b, err := io.ReadAll(io.LimitReader(r.Body, updateBodyMax+1))
	if err != nil || len(b) > updateBodyMax {
		return in, false
	}
	if json.Unmarshal(b, &in) != nil {
		return in, false
	}
	in.Kind = strings.TrimSpace(in.Kind)
	in.Key = strings.TrimSpace(in.Key)
	if in.Kind == "" || in.Key == "" {
		return in, false
	}
	return in, true
}

// h_skip answers POST /api/updates/skip. It records an answer about one version, so the
// row comes back the moment the site (or the shelf, or the site's release) moves on.
func (s *Server) h_skip(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	in, ok := s.readRefusal(r)
	if !ok {
		s.jsonErr(w, "the update to skip was not named")
		return
	}
	switch in.Kind {
	case "theme", "app", "firmware":
	default:
		s.jsonErr(w, "that update cannot be skipped")
		return
	}
	if in.Version < 0 {
		s.jsonErr(w, "the version skipped does not make sense")
		return
	}
	s.mutateChoice(in.Kind, in.Key, func(c *UpdateChoice) { c.SkippedVersion = in.Version })
	s.saveState()
	s.jsonOK(w, map[string]any{"kind": in.Kind, "key": in.Key, "skippedVersion": in.Version})
}

// h_keep answers POST /api/updates/keep, and it is the only refusal that changes a file:
// the theme's origin sidecar goes away, so §7.4 has nothing left to trace and the theme
// is the operator's own copy — bytes and name unchanged (ruling 26). The keep entry is
// written alongside, because a later re-import of the same design must not resurrect the
// tracking of a copy the owner already claimed.
//
// Every shelf theme whose sidecar names that design is untracked, not just the first: two
// copies of one design are two rows the site would update, and a keep pressed on either is
// the operator saying this hub should stop tracking that design.
func (s *Server) h_keep(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	in, ok := s.readRefusal(r)
	if !ok {
		s.jsonErr(w, "the theme to keep was not named")
		return
	}
	if in.Kind != "theme" {
		s.jsonErr(w, "Keep my version is an answer about a theme from orb-os.net")
		return
	}
	dir := s.library().ThemesDir
	if dir == "" {
		s.jsonErr(w, "there is no theme library to keep a theme in")
		return
	}
	entries, err := filepath.Glob(filepath.Join(dir, "*.orbtheme"))
	if err != nil {
		s.jsonErr(w, "the theme library could not be read: "+err.Error())
		return
	}
	var kept []string
	for _, p := range entries {
		o := readThemeOrigin(p)
		if o == nil || o.DesignID != in.Key {
			continue
		}
		if err := removeThemeOrigin(p); err != nil {
			s.jsonErr(w, "the theme stayed tracked: "+err.Error())
			return
		}
		kept = append(kept, filepath.Base(p))
	}
	s.mutateChoice("theme", in.Key, func(c *UpdateChoice) { c.Keep = true })
	s.saveState()
	s.jsonOK(w, map[string]any{"kind": "theme", "key": in.Key, "keep": true, "themes": kept})
}

// --- applying an update (g3-3, §7.4) -------------------------------------------------

// siteBakeFn is the hub's seam onto H1b's own-editor bake: given a design bundle's bytes
// and the name the site is sharing it under, it returns the .orbtheme the editor would have
// posted to /api/site/keep. The bake itself lives in the browser — the hub ships the baker
// as runtime files and no Go copy of it — so a hub built by main leaves this nil and the
// apply door holds the bundle as a ticket and lets the operator's open editor bake it, the
// same half-door the Import button already drives. A test sets it to stand the editor in
// for the browser, exactly as fetchEditor stands a board in for the network.
type siteBakeFn func(raw []byte, sharedName string) ([]byte, error)

// applyPayload is POST /api/updates/apply's data. The four §7.4 fields come back on every
// answer, including the one that only managed to fetch: an apply that renamed nothing says
// so, and one that has not installed anything yet names no orbs.
type applyPayload struct {
	OldName     string   `json:"oldName"`
	NewName     string   `json:"newName"`
	Renamed     bool     `json:"renamed"`
	InstalledOn []string `json:"installedOn"`
	// NeedBake is the hub saying "the design is fetched and held; this hub cannot bake it
	// by itself" — the release binary's answer, since its baker is a browser. Ticket and App
	// are what the page needs to open the editor on it, and it presses apply again with the
	// name the bake kept under.
	NeedBake bool   `json:"needBake,omitempty"`
	Ticket   string `json:"ticket,omitempty"`
	App      string `json:"app,omitempty"`
	// Note carries the one outcome that is neither a refusal nor a clean finish: the rename
	// landed and an old copy is still lying about, which is a manual-remove sentence and an
	// Update button that will finish the job when pressed again.
	Note string `json:"note,omitempty"`
}

// updateApply is the apply request: a kind, a design id, and the name a finished bake kept
// under, which is how the browser-driven second half of an apply tells the hub step 1 is done.
type updateApply struct {
	Kind  string `json:"kind"`
	Key   string `json:"key"`
	Baked string `json:"baked"`
}

// h_apply answers POST /api/updates/apply. One update is in flight at a time — a rename is
// a bake, an install and a removal in that order, and a second press interleaving with the
// first would be one rename half-applied twice — so a press that arrives while another runs
// is refused rather than queued.
func (s *Server) h_apply(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var in updateApply
	body, err := io.ReadAll(io.LimitReader(r.Body, updateBodyMax+1))
	if err != nil || len(body) > updateBodyMax || json.Unmarshal(body, &in) != nil {
		s.jsonErr(w, "the update to apply was not named")
		return
	}
	in.Kind = strings.TrimSpace(in.Kind)
	in.Key = strings.TrimSpace(in.Key)
	in.Baked = strings.TrimSpace(in.Baked)
	if in.Kind != "theme" || in.Key == "" {
		s.jsonErr(w, "Applying an update is an answer about a theme from orb-os.net")
		return
	}
	if !s.startApply() {
		s.jsonErr(w, "Orb Manager is already applying an update: wait for it and press again")
		return
	}
	defer s.endApply()
	out, err := s.applyThemeUpdate(r, in.Key, in.Baked)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	s.jsonOK(w, out)
}

// startApply takes the single in-flight apply, and says no to the second press.
func (s *Server) startApply() bool {
	s.applyMu.Lock()
	defer s.applyMu.Unlock()
	if s.applyBusy {
		return false
	}
	s.applyBusy = true
	return true
}

func (s *Server) endApply() {
	s.applyMu.Lock()
	s.applyBusy = false
	s.applyMu.Unlock()
}

// applyThemeUpdate is §7.4's rename and §7.6's ordinary update, and its order is the whole
// point: the library first, the orbs second, the removals last and never before a new copy
// is standing. A failure in the first two steps returns before anything was destroyed, so
// the operator has the theme they had — same bytes, same name, still worn. A failure in the
// removals leaves the NEW theme installed and selected with the old one still lying about,
// which is the reverse of the dangerous direction, and is said as a note.
func (s *Server) applyThemeUpdate(r *http.Request, key, baked string) (*applyPayload, error) {
	lib := s.library()
	if lib == nil || lib.ThemesDir == "" {
		return nil, errors.New("there is no theme library to update a theme in")
	}
	oldPath, origin := shelfByDesign(lib.ThemesDir, key)
	if oldPath == "" {
		return nil, errors.New("that theme is no longer in Orb Manager’s library")
	}
	oldBlob, err := os.ReadFile(oldPath)
	if err != nil {
		return nil, fmt.Errorf("the shelf copy of that theme could not be read: %w", err)
	}
	oc, err := orbnet.ContainerName(oldBlob)
	if err != nil {
		return nil, fmt.Errorf("the shelf copy of that theme does not parse: %s",
			strings.TrimPrefix(err.Error(), "orbnet: "))
	}
	if len(oc.Sections) == 0 {
		return nil, errors.New("the shelf copy of that theme names no app")
	}
	app, oldName := oc.Sections[0], oc.Name

	ans, err := s.resolveOrigin(r.Context(), origin)
	if err != nil {
		return nil, err
	}
	if ans.Gone {
		return nil, fmt.Errorf("%s is no longer shared on orb-os.net; the theme stays, and Orb Manager "+
			"will stop checking it", oldName)
	}
	if ans.NotYet {
		// The same refusal this row will still be offering nothing about: the site could
		// not say what the design's current version is, which is not a reason to write
		// anything. Nothing on the shelf, on an orb or in the state file changes.
		return nil, fmt.Errorf("%s: %s can’t check for updates yet", oldName, siteHost(origin.Site))
	}
	if ans.Version <= origin.Version {
		return nil, fmt.Errorf("orb-os.net has no newer version of %s than the one that was baked", oldName)
	}
	if ans.App != "" && ans.App != app {
		return nil, fmt.Errorf("%s belongs to the %s app now, and Orb Manager cannot move a theme between apps",
			oldName, ans.App)
	}

	// Step 1. Nothing on any orb has been touched when this returns.
	newPath, newName, ask, err := s.updateBake(r, lib, origin, app, oldName, ans, baked)
	if err != nil {
		return nil, err
	}
	if ask != nil {
		return ask, nil
	}
	out := &applyPayload{OldName: oldName, NewName: newName, Renamed: newName != oldName, InstalledOn: []string{}}

	newBlob, err := os.ReadFile(newPath)
	if err != nil {
		return nil, fmt.Errorf("the new bake could not be read from the shelf: %w", err)
	}
	nc, err := orbnet.ContainerName(newBlob)
	if err != nil {
		return nil, fmt.Errorf("the new bake does not parse: %s", strings.TrimPrefix(err.Error(), "orbnet: "))
	}

	// Step 2: every orb wearing the old name gets the new one — stored and chosen where the
	// old one was the worn face, filed unchosen where it was not. The old copy is still on
	// every one of them when this fails, which is why it comes before the removals.
	var wears []themeWear
	err = s.forEachOrbCard(r.Context(), func(ip string, list *orbnet.ThemeList) error {
		row := themeRowByName(list.Themes, oldName)
		if row == nil {
			return nil
		}
		// Chosen is read off the card row rather than assumed: an orb wearing the old theme
		// gets the new one as its face, and an orb that merely holds it is filed with keep=1
		// so its face is not decided by an update nobody was watching on it. streamThemeStore
		// is the Themes face's own push door and owns that difference, its firmware gate and
		// its store watch; the note it says when it skips is passed along rather than reworded.
		chosen := row.Chosen || row.Active
		var fail, skip string
		send := func(st installStep) {
			switch st.Action {
			case "refused":
				fail = st.Note
			case "skipped":
				skip = st.Note
			}
		}
		if !s.updateInstall(r.Context(), ip, newBlob, newName, nc, !chosen, send) {
			if fail == "" {
				fail = "the newer bake did not land"
			}
			return fmt.Errorf("%s: %s", s.orbWord(ip), fail)
		}
		if skip != "" {
			out.Note = fmt.Sprintf("%s kept its “%s”: %s", s.orbWord(ip), oldName, skip)
			return nil
		}
		out.InstalledOn = append(out.InstalledOn, ip)
		if out.Renamed {
			// Only a rename leaves an old file to remove: an ordinary update pushed over the
			// same name replaced the card's copy under the id it already had, and deleting
			// that id now would delete the bake that just landed.
			wears = append(wears, themeWear{ip: ip, id: row.ID})
		}
		return nil
	})
	if err != nil {
		return nil, err
	}

	// Step 3, reached only with every install landed: the old name goes, card first and then
	// the shelf file with its sidecar.
	var stale []string
	for _, w := range wears {
		// next is asked for rather than guessed: the rename's own store usually already moved
		// the app onto the new bake, so the old row is not worn and deleteNext says so by
		// answering nothing. On a card where the old face IS still the worn one, it proposes
		// the app's shelf default, which is the same sane face the Themes tab's own X offers.
		if _, err := s.device.ThemeDelete(w.ip, w.id, s.deleteNext(w.ip, w.id)); err != nil {
			stale = append(stale, s.orbWord(w.ip))
			continue
		}
		s.dropThemeCache(w.ip)
	}
	if out.Renamed && len(stale) == 0 {
		// The file goes before the sidecar, and only once every card has given its copy up: the
		// file and its record are what keep this design's row on the Updates list, and dropping
		// them alongside a stranded card copy would leave an old theme on an orb with nothing
		// left in the hub to name it, offer it again, or finish removing it.
		if err := os.Remove(oldPath); err != nil && !os.IsNotExist(err) {
			stale = append(stale, "the shelf")
		} else if err := removeThemeOrigin(oldPath); err != nil {
			log.Printf("site update: %q's sidecar outlived its theme: %v", oldName, err)
		}
	}
	if len(stale) > 0 {
		out.Note = fmt.Sprintf("“%s” is installed as “%s”; the old copy is still on %s and needs a manual remove",
			oldName, newName, strings.Join(stale, ", "))
	}
	log.Printf("site update: %s %s v%d -> %q on %d orb(s)", origin.Site, key, ans.Version, newName, len(out.InstalledOn))
	return out, nil
}

// themeWear is one orb's old row, kept so step 3 can name the doomed file by id — the only
// way a card theme is addressed for certain.
type themeWear struct {
	ip string
	id string
}

// updateBake is step 1: the design at the version the site is offering, baked under the name
// it is offered as, filed on the shelf with a fresh origin record at that version. It answers
// the new file's path and name, or — on a hub whose baker is a browser — the payload that
// hands the bake to one. `baked` is that second half arriving: the name the editor kept, which
// the shelf must already hold for this design at this version.
func (s *Server) updateBake(r *http.Request, lib *Library, origin *ThemeOrigin, app, oldName string,
	ans *resolveAnswer, baked string) (string, string, *applyPayload, error) {
	if baked != "" {
		p := shelfThemeNamed(lib.ThemesDir, app, baked)
		if p == "" {
			return "", "", nil, fmt.Errorf("nothing was kept under %q, so this update did not start", baked)
		}
		o := readThemeOrigin(p)
		if o == nil || o.DesignID != origin.DesignID || o.Version < ans.Version {
			return "", "", nil, fmt.Errorf("the shelf’s %q is not this design at the version orb-os.net is offering", baked)
		}
		return p, baked, nil, nil
	}
	b, err := s.fetchCheckedBundle(r, origin)
	if err != nil {
		return "", "", nil, err
	}
	if s.themeBake == nil {
		// This hub has no baker: the design is fetched and held, and the bake is the operator's
		// own editor door away. H1b's import path does the rest and presses this again.
		ticket, err := s.tickets().add(&siteTicket{site: origin.Site, id: origin.DesignID, version: b.Version,
			app: b.App, name: b.Name, sharedName: b.SharedName, handle: b.Handle, raw: b.Raw})
		if err != nil {
			return "", "", nil, fmt.Errorf("the import could not be held: %w", err)
		}
		log.Printf("site update: %s %s v%d -> %q: held for the hub editor to bake",
			origin.Site, origin.DesignID, b.Version, b.SharedName)
		return "", "", &applyPayload{OldName: oldName, NewName: b.SharedName,
			Renamed: b.SharedName != oldName, InstalledOn: []string{},
			NeedBake: true, Ticket: ticket, App: b.App}, nil
	}
	blob, err := s.themeBake(b.Raw, b.SharedName)
	if err != nil {
		// The refusal is the baker's own sentence, never a guess at it.
		return "", "", nil, err
	}
	c, err := orbnet.ContainerName(blob)
	if err != nil {
		return "", "", nil, errors.New(strings.TrimPrefix(err.Error(), "orbnet: "))
	}
	if len(c.Sections) == 0 || c.Sections[0] != b.App {
		return "", "", nil, fmt.Errorf("the bake is not one of the %s app’s", b.App)
	}
	if c.Name != b.SharedName {
		return "", "", nil, fmt.Errorf("the bake is named “%s”, not “%s”", c.Name, b.SharedName)
	}
	// fold=false, exactly as an import keeps: the new bake joins the shelf rather than
	// discarding an unsent draft sitting under its name.
	dest, err := lib.keepTheme(blob, false)
	if err != nil {
		return "", "", nil, fmt.Errorf("the theme could not be kept: %w", err)
	}
	o := &ThemeOrigin{Site: origin.Site, DesignID: origin.DesignID, Version: b.Version,
		ImportedAt: s.tickets().clock().UnixMilli()}
	if err := writeThemeOrigin(dest, o); err != nil {
		return "", "", nil, fmt.Errorf("the theme was kept, but where it came from could not be recorded: %w", err)
	}
	return dest, c.Name, nil, nil
}

// fetchCheckedBundle is the import door's first half read off an origin record rather than a
// pasted link: that design's bundle, checked, with the door's own refusals.
func (s *Server) fetchCheckedBundle(r *http.Request, o *ThemeOrigin) (*siteBundle, error) {
	raw, err := s.fetchSiteBundle(r, o.Site, o.DesignID, "")
	if err != nil {
		return nil, err
	}
	b, err := checkBundle(raw, o.DesignID)
	if err != nil {
		if errors.Is(err, errSiteBundleNo) {
			return nil, fmt.Errorf("%s did not answer with a theme", siteHost(o.Site))
		}
		return nil, fmt.Errorf("the theme from %s is damaged: %w", siteHost(o.Site), err)
	}
	return b, nil
}

// updateInstall is step 2 on one orb: the hub's one theme-push door, held open across the
// copy the way the Themes face's own Update holds it. keep carries the card row's answer to
// "is this the face its app wears?" — a rename that finds the old theme worn installs the new
// one and selects it, and one that finds it unworn files it without touching the face.
func (s *Server) updateInstall(ctx context.Context, ip string, blob []byte, label string,
	c *orbnet.ThemeContainer, keep bool, send func(installStep)) bool {
	release, rerr := s.holdDevicePort(ctx, ip, otaTimeout)
	if rerr != nil {
		send(installStep{Step: c.ID, Action: "refused", Note: rerr.Error()})
		return false
	}
	defer release()
	s.dropThemeCache(ip)
	return s.streamThemeStore(ctx, ip, blob, label, c, true, keep, send)
}

// forEachOrbCard hands one orb's card to fn for every address the hub may dial. An orb that
// will not answer is wearing nothing as far as this update knows, so its read error is passed
// over rather than failed: the rename is about the boards that answer, and the Themes tab is
// where an offline board's own story is told. fn's error ends the walk and becomes the refusal.
func (s *Server) forEachOrbCard(ctx context.Context, fn func(ip string, list *orbnet.ThemeList) error) error {
	for _, ip := range s.grants.list() {
		list, err := s.device.ThemeList(ip)
		if err != nil {
			continue
		}
		if err := fn(ip, list); err != nil {
			return err
		}
	}
	return nil
}

// orbWord names an orb in a sentence the operator reads: the name they gave it when the fleet
// knows one, and its address otherwise — an address they can copy beats a "some orb".
func (s *Server) orbWord(ip string) string {
	if s.fleet != nil {
		for _, o := range s.fleet.Snapshot() {
			if o.IP == ip && o.Name != "" {
				return "“" + o.Name + "” (" + ip + ")"
			}
		}
	}
	return ip
}

// shelfByDesign is the shelf theme whose origin record names this design, the oldest baked
// version first: a shelf holding two copies of one design has two rows the site updates, and
// an apply walks the copy it found. An empty path means the shelf holds none — the sidecar was
// retired by a Keep my version, or the theme was removed since the list was painted.
func shelfByDesign(dir, designID string) (string, *ThemeOrigin) {
	entries, err := filepath.Glob(filepath.Join(dir, "*.orbtheme"))
	if err != nil {
		return "", nil
	}
	best, bestO := "", (*ThemeOrigin)(nil)
	for _, p := range entries {
		o := readThemeOrigin(p)
		if o == nil || o.DesignID != designID {
			continue
		}
		if bestO == nil || o.Version < bestO.Version || (o.Version == bestO.Version && p < best) {
			best, bestO = p, o
		}
	}
	return best, bestO
}

// shelfThemeNamed is the shelf file for one app under one baked name, by the same (app, name)
// identity keepTheme files by. Empty is the shelf not holding it.
func shelfThemeNamed(dir, app, name string) string {
	found := ""
	shelfThemes(dir, func(p string, _ os.FileInfo, t *orbnet.ShelfThemeInfo) {
		if found == "" && shelfApp(t) == app && t.Name == name {
			found = p
		}
	})
	return found
}

// themeRowByName is the card row with this baked name, which is how an update finds the file it
// supersedes: the origin knows the design, the shelf knows the name, and only the card read
// knows the id a delete has to name.
func themeRowByName(rows []orbnet.ThemeEntry, name string) *orbnet.ThemeEntry {
	for i := range rows {
		if rows[i].Name == name {
			return &rows[i]
		}
	}
	return nil
}
