package main

// h3-design.md §7.3, hub step 4: themes that live ONLY on an orb. A theme pushed to a board by
// hand, or installed from a file years ago, has no library file and no origin record, so nothing
// in the hub knows its design id and the update machinery cannot ask about it. This file finds
// those themes by reading the orbs' own cards (forEachOrbCard — the same walk a rename walks),
// asks the configured site by name + app exactly as a file import is asked, and on an answer
// adopts the theme through the existing apply path: updateBake fetches and bakes, the shelf gains
// the theme with a fresh origin record, the orbs that carry the old card name get the new one,
// and the old card name goes after the install and the select. From the next list onward it is
// an ordinary tracked theme and this enumeration no longer names it.
//
// The answers are hub-3's policy verbatim, with one difference that is this step's whole state
// problem: a by-name gone is durable news about a name no shelf file stands beside, so it lands
// in a ledger of its own (<configDir>/site-orb, the inbox hold's style) rather than as a marker
// on a theme file, and a once-public name is not re-asked every minute. A NotYet or a transport
// failure records nothing — ask again next press is the whole state. And a resolved answer whose
// adoption is unfinished (a hub whose baker is a browser) keeps its ledger entry until the
// browser's second half settles it, so the press survives a restart.
//
// There is no new background loop here: the enumeration runs when the block refreshes and an
// adoption is a press, because the enumeration dials orbs and minute-cadence asking is step 3's
// job. The doors are registered beside the site link's, behind requireControl, and none of them
// on the editor-API listener.

import (
	"context"
	"crypto/sha1"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// orbOnlyDirName is the ledger directory's name under the config dir: <configDir>/site-orb.
const orbOnlyDirName = "site-orb"

// orbOnlyTheme is one row of GET /api/site/orb-themes' answer: the theme's card name, the app
// its row names, the orbs wearing it named as orbWord names them, and the one fact a gone answer
// leaves behind — a gone entry carries no orbs, so the tab can say the sentence and offer nothing.
type orbOnlyTheme struct {
	Name string   `json:"name"`
	App  string   `json:"app"`
	Orbs []string `json:"orbs"`
	Gone bool     `json:"gone"`
}

// orbOnlyLedger is one ledger file: what the site last said about one name, durably. A gone
// entry carries no design id (the site would not name the design it retired); a resolved entry
// carries the identity the answer gave and waits for the adoption it started to finish.
type orbOnlyLedger struct {
	Name     string `json:"name"`
	App      string `json:"app"`
	AskedAt  int64  `json:"askedAt"`
	Gone     bool   `json:"gone"`
	DesignID string `json:"designId,omitempty"`
	Version  int    `json:"version,omitempty"`
}

// orbOnlyDir is the ledger directory: <configDir>/site-orb, derived from statePath the way the
// inbox derives its hold directory. An empty statePath has no config dir, and every door and
// pass then treats the ledger as empty — a hub with nowhere to remember asks again every time,
// which is the pre-ledger behavior and not a failure.
func (s *Server) orbOnlyDir() string {
	if s.statePath == "" {
		return ""
	}
	return filepath.Join(filepath.Dir(s.statePath), orbOnlyDirName)
}

// orbOnlyLedgerPath keys a name by its sha1: shared names carry spaces, "@", and every character
// a filename may not, and the file must sit plainly inside one flat directory.
func orbOnlyLedgerPath(dir, name string) string {
	sum := sha1.Sum([]byte(name))
	return filepath.Join(dir, hex.EncodeToString(sum[:])+".json")
}

// orbOnlyWriteLedger files one entry atomically: directory 0700 on first use, temp file inside
// the directory, fsync, rename, directory fsync — the inbox hold's style, because the fact worth
// keeping (this name is gone; this adoption is half-done) is worth the same survival it gets.
func (s *Server) orbOnlyWriteLedger(e *orbOnlyLedger) error {
	dir := s.orbOnlyDir()
	if dir == "" {
		return errors.New("no config directory: nowhere to keep the orb-theme ledger")
	}
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return err
	}
	body, err := json.Marshal(e)
	if err != nil {
		return err
	}
	to := orbOnlyLedgerPath(dir, e.Name)
	tmp := to + ".tmp"
	f, err := os.OpenFile(tmp, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o600)
	if err != nil {
		return err
	}
	if _, err := f.Write(body); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Close(); err != nil {
		os.Remove(tmp)
		return err
	}
	if err := os.Rename(tmp, to); err != nil {
		os.Remove(tmp)
		return err
	}
	syncSiteInboxDir(dir)
	return nil
}

// orbOnlyReadLedger answers the entry kept under one name, or nil: no directory, no file, a
// damaged file and a file that names a different theme are all the same nothing, because the
// ledger only ever changes what the hub ASKS, and nothing remembered is better than something
// misremembered.
func (s *Server) orbOnlyReadLedger(name string) *orbOnlyLedger {
	dir := s.orbOnlyDir()
	if dir == "" || name == "" {
		return nil
	}
	b, err := os.ReadFile(orbOnlyLedgerPath(dir, name))
	if err != nil {
		return nil
	}
	var e orbOnlyLedger
	if json.Unmarshal(b, &e) != nil || e.Name != name {
		return nil
	}
	return &e
}

// orbOnlyMarkGone durably records that the configured site says one name is retired, in the
// words step 3 chose for the same news about a shelf theme: a marker, not a memory in the
// asking code. A ledger that could not be written is reported and changes the policy not at
// all: the name was asked once, it will be asked again, and the site will say the same thing.
func (s *Server) orbOnlyMarkGone(name, app string) {
	if err := s.orbOnlyWriteLedger(&orbOnlyLedger{Name: name, App: app, AskedAt: time.Now().UnixMilli(), Gone: true}); err != nil {
		log.Printf("site orb themes: %s is gone from %s and the mark could not be kept: %v", name, siteHost(siteReleaseBase()), err)
	}
}

// orbOnlyForget deletes every ledger entry a library theme's removal makes stale: the entry kept
// under the theme's own name (a gone mark, or a pending adoption for the very theme being filed
// out of the library), and — since a resolved entry records the design it was resolved to — the
// entry whose design id the removed sidecar names. Names and design ids rather than a file path
// because the ledger keys by name and the shelf files by stem, and the two meet nowhere but in
// these two facts.
func (s *Server) orbOnlyForget(name, designID string) {
	dir := s.orbOnlyDir()
	if dir == "" || (name == "" && designID == "") {
		return
	}
	files, err := filepath.Glob(filepath.Join(dir, "*.json"))
	if err != nil {
		return
	}
	for _, p := range files {
		b, err := os.ReadFile(p)
		if err != nil {
			continue
		}
		var e orbOnlyLedger
		if json.Unmarshal(b, &e) != nil {
			continue
		}
		if (name != "" && e.Name == name) || (designID != "" && e.DesignID == designID) {
			if err := os.Remove(p); err != nil && !os.IsNotExist(err) {
				log.Printf("site orb themes: %s: the ledger entry outlived its theme: %v", e.Name, err)
			}
		}
	}
}

// orbOnlyThemeFacts reads the two facts a removal must know BEFORE the shelf file goes away —
// its container name (the key the ledger files under) and its recorded design id — because a
// deleted file states neither. Callers take the facts, drop the theme, and then call
// orbOnlyForget with them: a failed drop keeps its ledger entry, as it keeps its theme.
func (s *Server) orbOnlyThemeFacts(p string) (string, string) {
	name, designID := "", ""
	if b, err := os.ReadFile(p); err == nil {
		if c, err := orbnet.ContainerName(b); err == nil {
			name = c.Name
		}
	}
	if o := readThemeOrigin(p); o != nil {
		designID = o.DesignID
	}
	return name, designID
}

// siteOrbOnlyThemes is the enumeration: every orb's own card list walked once, the names that
// could be orb-os.net designs and are not in the library asked of the configured site, and the
// answers turned into the rows §7.3 shows. A candidate whose ask comes back Gone is written to
// the ledger and reported gone — which is why the walk asks at all: the block refreshes, and a
// retired name re-asked at every refresh is the noise step 3's review 5 was written against,
// durable marking being the answer it found. NotYet and transport failures mark nothing: the
// row stands as a candidate, and the next press asks again. A site the hub cannot reach at all
// (no -site, so no configured origin) leaves the candidates unasked and unanswered-for, which
// is the same rows minus the gone marks.
func (s *Server) siteOrbOnlyThemes(ctx context.Context) ([]orbOnlyTheme, error) {
	type card struct {
		app  string
		orbs []string
	}
	found := map[string]*card{}
	var order []string
	err := s.forEachOrbCard(ctx, func(ip string, list *orbnet.ThemeList) error {
		for _, row := range list.Themes {
			if row.Name == "" || !orbOnlyCandidateName(row.Name) {
				continue
			}
			c := found[row.Name]
			if c == nil {
				if row.App == "" {
					// A device too old to name the owning app cannot be asked about on the
					// app the door requires, and guessing one would ask a different
					// question than the one the site answers.
					continue
				}
				c = &card{app: row.App}
				found[row.Name] = c
				order = append(order, row.Name)
			}
			w := s.orbWord(ip)
			seen := false
			for _, o := range c.orbs {
				seen = seen || o == w
			}
			if !seen {
				c.orbs = append(c.orbs, w)
			}
		}
		return nil
	})
	if err != nil {
		return nil, err
	}

	titles := map[string]bool{}
	if lib := s.library(); lib != nil && lib.ThemesDir != "" {
		var pl LibraryPayload
		listThemeLibrary(lib.ThemesDir, &pl)
		for _, t := range pl.Themes {
			titles[t.Title] = true
		}
	}

	site := siteReleaseBase()
	sort.Strings(order)
	out := []orbOnlyTheme{}
	for _, name := range order {
		if titles[name] {
			continue // tracked: step 3's business, not untracked news
		}
		if e := s.orbOnlyReadLedger(name); e != nil && e.Gone {
			out = append(out, orbOnlyTheme{Name: name, Gone: true})
			continue
		}
		if site == "" {
			out = append(out, orbOnlyTheme{Name: name, App: found[name].app, Orbs: found[name].orbs})
			continue
		}
		ans, err := s.resolveName(ctx, site, found[name].app, name)
		switch {
		case err != nil:
			log.Printf("site orb themes: %s: %v", name, err)
		case ans.Gone:
			s.orbOnlyMarkGone(name, found[name].app)
			out = append(out, orbOnlyTheme{Name: name, Gone: true})
			continue
		}
		// A row for every remaining answer: Resolved (the Track button is what the answer is
		// for), and NotYet (the site not having the door yet is not the theme being gone).
		out = append(out, orbOnlyTheme{Name: name, App: found[name].app, Orbs: found[name].orbs})
	}
	return out, nil
}

// orbOnlyCandidateName is the card-name gate the tracked path draws with the same words: a name
// the site could have written at all, in the shared form, with BOTH halves present — a homemade
// `My Doodad` and a headless ` by @greg` are the operator's own business, and dialling a stranger's
// site once a refresh about either is not the hub's.
func orbOnlyCandidateName(name string) bool {
	n, handle, sentence := checkSharedName(name)
	return sentence == "" && n != "" && handle != ""
}

// routesOrbOnly hangs the block's two doors behind the control token, beside the site link's
// and the inbox's, on the control listener only: the enumeration dials the operator's own orbs
// and the operator's site, and an editor-API caller has no business reading either.
func (s *Server) routesOrbOnly(mux *http.ServeMux) {
	mux.Handle("/api/site/orb-themes", s.requireControl(http.HandlerFunc(s.h_orbThemes)))
	mux.Handle("/api/site/orb-themes/adopt", s.requireControl(http.HandlerFunc(s.h_orbThemesAdopt)))
}

// h_orbThemes answers GET /api/site/orb-themes: the enumeration, fresh. An orb that will not
// answer contributes nothing (forEachOrbCard swallows the read), and an enumeration that walked
// nothing answers an empty list — the tab says Nothing untracked rather than an error, because
// "no orbs agree with me" is the ordinary state of a hub with no orbs.
func (s *Server) h_orbThemes(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	rows, err := s.siteOrbOnlyThemes(r.Context())
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	s.jsonOK(w, map[string]any{"themes": rows})
}

// orbOnlyAdopt is the press body, and h_orbThemesAdopt answers POST /api/site/orb-themes/adopt
// with the same applyPayload /api/updates/apply answers — one web flow, not a second.
type orbOnlyAdopt struct {
	Name string `json:"name"`
}

// h_orbThemesAdopt is the Track this theme press. One update in flight at a time, exactly as
// the apply door holds it: the press runs the apply's own three steps.
//
// The refusal sentence is siteLinkRefused for every arm that runs no adoption — a name that is
// not in the current enumeration, a gone answer, a NotYet, a site that would not answer — which
// is the same sentence an unreadable paste gets and honest here: the button was drawn from the
// enumeration that door just refused to recognise. The arms differ underneath the one answer, in
// the ledger and the log: Gone writes the mark, NotYet and transport write nothing, and the
// refusals that ran no dial at all (a name the site could never have written) log nothing either.
func (s *Server) h_orbThemesAdopt(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var in orbOnlyAdopt
	body, err := io.ReadAll(io.LimitReader(r.Body, updateBodyMax+1))
	if err != nil || len(body) > updateBodyMax || json.Unmarshal(body, &in) != nil {
		s.jsonErr(w, siteLinkRefused)
		return
	}
	name := strings.TrimSpace(in.Name)
	if name == "" || !orbOnlyCandidateName(name) {
		s.jsonErr(w, siteLinkRefused)
		return
	}
	if !s.startApply() {
		s.jsonErr(w, "Orb Manager is already applying an update: wait for it and press again")
		return
	}
	defer s.endApply()

	rows, err := s.siteOrbOnlyThemes(r.Context())
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	app := ""
	for _, row := range rows {
		if row.Name == name && !row.Gone {
			app = row.App
			break
		}
	}
	if app == "" {
		s.jsonErr(w, siteLinkRefused)
		return
	}
	site := siteReleaseBase()
	ans, err := s.resolveName(r.Context(), site, app, name)
	if err != nil {
		log.Printf("site orb themes: %s: %v", name, err)
		s.jsonErr(w, siteLinkRefused)
		return
	}
	if ans.Gone {
		s.orbOnlyMarkGone(name, app)
		log.Printf("site orb themes: %s is gone from %s; Orb Manager will stop asking about it", name, siteHost(site))
		s.jsonErr(w, siteLinkRefused)
		return
	}
	if ans.NotYet {
		log.Printf("site orb themes: %s: %s has no answer about it yet", name, siteHost(site))
		s.jsonErr(w, siteLinkRefused)
		return
	}
	lib := s.library()
	if lib == nil || lib.ThemesDir == "" {
		s.jsonErr(w, "there is no theme library to adopt a theme into")
		return
	}
	origin := &ThemeOrigin{Site: site, DesignID: ans.DesignID, Version: ans.Version,
		ImportedAt: s.tickets().clock().UnixMilli()}
	out, err := s.applyUpdateSteps(r, lib, origin, app, name, ans, "", "")
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	if out.NeedBake {
		// The bake is the operator's own editor door away, and the adoption is unfinished until
		// the browser presses apply with the name it kept under. The ledger entry is what makes
		// that press survive a restart: it names the design the shelf must land, under the name
		// the card still wears.
		if err := s.orbOnlyWriteLedger(&orbOnlyLedger{Name: name, App: app, AskedAt: time.Now().UnixMilli(),
			DesignID: ans.DesignID, Version: ans.Version}); err != nil {
			log.Printf("site orb themes: %s: the adoption is held for the hub editor and the ledger entry could not be kept: %v", name, err)
		}
	} else if p, _ := shelfByDesign(lib.ThemesDir, ans.DesignID); p != "" {
		s.orbOnlyForget(name, ans.DesignID)
	}
	s.jsonOK(w, out)
}

// orbOnlySettle is the second half of an adoption arriving at the ordinary apply door: the key
// there is not a design id but the NAME the card still wears (the ticket answer carries no id
// for the browser to hold), and the shelf holds no file for it yet, which is the ordinary
// refusal. It is a settle when the ledger remembers a resolved ask under that name: the design
// the answer named, at the version the answer gave, is then what the bake the shelf now holds
// must match, and applyUpdateSteps does the rest — install, select, remove the old card copies
// — as the press that needed no bake would have done itself. The bool answers "was this press a
// settle at all", so a key that is nobody's settle keeps the ordinary sentence, and a theme name
// can never collide into it: a shared name carries " by @", and no design id does.
func (s *Server) orbOnlySettle(r *http.Request, lib *Library, key, baked string) (*applyPayload, bool, error) {
	if baked == "" {
		return nil, false, nil
	}
	e := s.orbOnlyReadLedger(key)
	if e == nil || e.Gone || e.DesignID == "" || e.App == "" {
		return nil, false, nil
	}
	site := siteReleaseBase()
	if site == "" {
		return nil, false, nil
	}
	out, err := s.applyUpdateSteps(r, lib, &ThemeOrigin{Site: site, DesignID: e.DesignID, Version: e.Version},
		e.App, key, &resolveAnswer{Resolved: true, DesignID: e.DesignID, App: e.App, SharedName: e.Name,
			Version: e.Version}, baked, "")
	if err == nil && !out.NeedBake {
		if p, _ := shelfByDesign(lib.ThemesDir, e.DesignID); p != "" {
			s.orbOnlyForget(key, e.DesignID)
		}
	}
	return out, true, err
}
