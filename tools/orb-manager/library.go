package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// The hub's app and theme library (S6, plan §1).
//
// S5 made the first-party containers disk artifacts an operator copies by hand. S6
// replaces that hand step: the hub reads two directories at startup and serves what is
// in them over the device's own upload doors, so "what this hub would put on a card" is
// a question with one answer instead of a build-time embedding.
//
// The directory IS the library. There is no registry, no manifest of manifests, and no
// restart needed to pick up a file an operator dropped in — the listing is read per
// request, because the one thing a curated index would add is the chance of it being
// wrong about a file that is plainly present.
//
// The invariant this file exists to hold is the one //go:embed broke: the hub refuses a
// container it cannot parse, and it NAMES the refusal. A library directory is full of
// files somebody else authored, and a pane that quietly omitted the one that will not
// load is a pane that lies about what the hub can seed.

// libAppsSubdir / libThemesSubdir name the two kinds inside every candidate root. Kept
// apart rather than one directory with a mixed listing because the two upload doors take
// different bodies, different caps and different reboot behaviour, and a listing that
// conflated them would push the difference down into every caller.
const (
	libAppsSubdir   = "apps"
	libThemesSubdir = "themes"
)

// Library is the resolved pair of directories plus what the resolver was told. It is
// resolved once at startup and never mutated: a hub that re-resolved mid-session could
// seed from a directory other than the one /api/status named at launch, and an operator
// reading the header would be reading a stale answer about a live thing.
type Library struct {
	AppsDir   string
	ThemesDir string

	// AppsSource / ThemesSource name WHICH rung won, in the words the status line and
	// the Library pane both print. Two hubs resolving the same filename to different
	// bytes is the failure this field exists to make visible rather than merely
	// expensive to diagnose.
	AppsSource   string
	ThemesSource string
}

// libCandidate is one rung of the ladder: a directory to try, and the words that say
// where it came from.
type libCandidate struct {
	dir  string
	from string
}

// libLadder builds the rungs for one kind, in the order the plan fixed. First hit wins,
// and a rung that exists but is empty WINS — an operator who created apps/ and left it
// empty means "nothing here", and falling through to the clone's copy would seed files
// they just watched themselves decline.
//
// libFlag names the library ROOT; the two kinds sit inside it. One flag therefore points
// at both kinds, so a curator with one library on a NAS names it once, and the
// kind-specific split the plan allowed stays available through a per-kind flag that wins
// when it is set.
//
// exeDir is the directory of the running binary, which is what makes the shipped layout
// (apps/ beside orb-manager.exe) work with no flag at all.
//
// The plan's ladder also had the executable's PARENT directory. It is not here, and the
// reason is the rung's own reach: an exe-dir rung can only ever point at a directory
// someone installed beside the binary, while a parent-of-exe-dir rung on a double-clicked
// release build reaches into $HOME and lets $HOME/apps outrank the library the hub's own
// import button writes to. The dev case that rung existed for — running a build out of
// bin/ — is already covered by the clone rung, because detectRepoDir() walks up from both
// the cwd and the executable to find the checkout. A rung that can only be reached by
// accident is not a resolution order, it is a bug with a path in it.
func libLadder(kind, flagVal, kindFlag, exeDir, userCfg, repoDir string) []libCandidate {
	var out []libCandidate
	add := func(dir, from string) {
		if dir != "" {
			out = append(out, libCandidate{dir: dir, from: from})
		}
	}
	add(kindFlag, "its own flag")
	if flagVal != "" {
		add(filepath.Join(flagVal, kind), "-library flag")
	}
	if exeDir != "" {
		add(filepath.Join(exeDir, kind), "beside the executable")
	}
	if userCfg != "" {
		// Where a browser import files a new app, so the import and the auto-seed can
		// never disagree about where an app the operator kept lives.
		add(filepath.Join(userCfg, "orb-manager", kind), "the user config dir")
	}
	if repoDir != "" {
		add(filepath.Join(repoDir, "tools", "orb-manager", kind), "the clone")
	}
	return out
}

// resolveLibrary walks both ladders and returns the first existing directory for each
// kind. A kind with no candidate that exists resolves to "" and reports "none found" —
// not an error, because a hub installed from a release zip has no clone, and a hub whose
// -library volume is unmounted today still has to boot and serve everything else it can.
func resolveLibrary(libFlag, appsFlag, themesFlag, exeDir, userCfg, repoDir string) *Library {
	lib := &Library{}
	lib.AppsDir, lib.AppsSource = pickLibDir(
		libLadder(libAppsSubdir, libFlag, appsFlag, exeDir, userCfg, repoDir))
	lib.ThemesDir, lib.ThemesSource = pickLibDir(
		libLadder(libThemesSubdir, libFlag, themesFlag, exeDir, userCfg, repoDir))
	return lib
}

func pickLibDir(rungs []libCandidate) (string, string) {
	for _, r := range rungs {
		if st, err := os.Stat(r.dir); err == nil && st.IsDir() {
			return r.dir, r.from
		}
	}
	return "", "none found"
}

// LibraryFile is one parsed library entry. The fields are the ones a listing needs to
// draw a row and to decide whether pressing its button is worth a round trip — which is
// why caps and the editor's presence are here rather than left to a re-parse.
type LibraryFile struct {
	File      string   `json:"file"`
	Slug      string   `json:"slug"`
	Title     string   `json:"title"`
	Version   string   `json:"version,omitempty"`
	Caps      []string `json:"caps,omitempty"`
	Themes    bool     `json:"themes"`
	HasEditor bool     `json:"has_editor"`
	Bytes     int      `json:"bytes"`
	// App names the app a THEME themes, read out of the container's own section index.
	// FORMAT 3 files carry exactly one section, so this is a fact the file states rather
	// than an opinion the hub formed. It is what lets the Library row and the Themes face
	// say "Clock themes" instead of presenting an owner-written name and hoping the
	// operator remembers which app it belongs to. Empty on an app row, where Slug already
	// is the app.
	App string `json:"app,omitempty"`
}

// LibraryRefusal is a file in the library that will not load. It is a listing entry and
// not a log line, on the device's rule: a card refuses an app by SHOWING the reason, and
// a hub that hid an unparseable container would leave an operator staring at a directory
// that clearly contains a file.
type LibraryRefusal struct {
	File string `json:"file"`
	Why  string `json:"why"`
}

// LibraryPayload is GET /api/library's data: both listings, both resolved dirs, and the
// refusals from either side in one list.
type LibraryPayload struct {
	AppsDir      string           `json:"appsDir"`
	ThemesDir    string           `json:"themesDir"`
	AppsSource   string           `json:"appsSource"`
	ThemesSource string           `json:"themesSource"`
	Apps         []LibraryFile    `json:"apps"`
	Themes       []LibraryFile    `json:"themes"`
	Refused      []LibraryRefusal `json:"refused"`
}

// h_library answers GET /api/library. Read-only, and it re-reads the directory every
// time: replacing a file by hand is the supported way to change what the hub can seed,
// and a hub that needed a restart to notice would be a hub with a hidden registry.
func (s *Server) h_library(r *http.Request) (any, error) {
	s.pmu.Lock()
	lib := s.lib
	s.pmu.Unlock()
	if lib == nil {
		lib = &Library{}
	}
	out := &LibraryPayload{AppsDir: lib.AppsDir, ThemesDir: lib.ThemesDir,
		AppsSource: lib.AppsSource, ThemesSource: lib.ThemesSource,
		Apps: []LibraryFile{}, Themes: []LibraryFile{}, Refused: []LibraryRefusal{}}
	if lib.AppsDir != "" {
		listAppLibrary(lib.AppsDir, out)
	}
	if lib.ThemesDir != "" {
		listThemeLibrary(lib.ThemesDir, out)
	}
	return out, nil
}

// listAppLibrary parses every .orbapp in dir into out, appending what loads and
// recording what does not. A directory that cannot be read is reported as one refusal
// named for the directory, because a permission problem on the whole library is not a
// property of any one file in it.
func listAppLibrary(dir string, out *LibraryPayload) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		out.Refused = append(out.Refused, LibraryRefusal{File: dir,
			Why: "the directory cannot be read: " + err.Error()})
		return
	}
	var found []LibraryFile
	for _, e := range entries {
		if e.IsDir() || !strings.EqualFold(filepath.Ext(e.Name()), ".orbapp") {
			continue
		}
		body, err := os.ReadFile(filepath.Join(dir, e.Name()))
		if err != nil {
			out.Refused = append(out.Refused, LibraryRefusal{File: e.Name(),
				Why: "the file cannot be read: " + err.Error()})
			continue
		}
		c, err := orbnet.ParseApp(body)
		if err != nil {
			// Trimmed to the sentence the reader wrote, minus the package prefix an
			// operator has no use for.
			out.Refused = append(out.Refused, LibraryRefusal{File: e.Name(),
				Why: strings.TrimPrefix(err.Error(), "orbnet: ")})
			continue
		}
		// The header's slug has to match the filename, the way the dial's own scan
		// insists. A file named clock.orbapp whose header names itself anders is the
		// library lying about its own contents, and the honest answer is a refusal
		// naming both, not a listing that quietly picked one.
		if stem := strings.TrimSuffix(e.Name(), filepath.Ext(e.Name())); stem != c.Slug {
			out.Refused = append(out.Refused, LibraryRefusal{File: e.Name(),
				Why: fmt.Sprintf("the file is named %s.orbapp and its header names itself %q",
					stem, c.Slug)})
			continue
		}
		found = append(found, LibraryFile{File: e.Name(), Slug: c.Slug, Title: c.Title,
			Version: c.Version, Caps: c.CapNames(), Themes: c.HasThemes(),
			HasEditor: c.HasEditor(), Bytes: len(body)})
	}
	sort.Slice(found, func(i, j int) bool { return found[i].Slug < found[j].Slug })
	out.Apps = append(out.Apps, found...)
}

// listThemeLibrary is the theme side. The container's own embedded name is the identity
// here — a .orbtheme carries its slug inside, so the filename is decoration and is
// listed but never compared, which is the device's rule for this kind and not an
// oversight. A file the door would refuse is reported by ContainerName, the door's own
// verdict, rather than by this listing's opinion of it.
func listThemeLibrary(dir string, out *LibraryPayload) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		out.Refused = append(out.Refused, LibraryRefusal{File: dir,
			Why: "the directory cannot be read: " + err.Error()})
		return
	}
	var found []LibraryFile
	for _, e := range entries {
		if e.IsDir() || !strings.EqualFold(filepath.Ext(e.Name()), ".orbtheme") {
			continue
		}
		body, err := os.ReadFile(filepath.Join(dir, e.Name()))
		if err != nil {
			out.Refused = append(out.Refused, LibraryRefusal{File: e.Name(),
				Why: "the file cannot be read: " + err.Error()})
			continue
		}
		c, err := orbnet.ContainerName(body)
		if err != nil {
			out.Refused = append(out.Refused, LibraryRefusal{File: e.Name(),
				Why: strings.TrimPrefix(err.Error(), "orbnet: ")})
			continue
		}
		// The section slug is the app this theme themes: FORMAT 3 files carry exactly one,
		// which is the same fact the device keys the card stem by. Read off the container
		// rather than the filename, because the filename is decoration here and the header
		// is the identity — the rule this listing already follows for the name.
		app := ""
		if len(c.Sections) > 0 {
			app = c.Sections[0]
		}
		found = append(found, LibraryFile{File: e.Name(), Slug: c.ID, Title: c.Name,
			Bytes: len(body), App: app})
	}
	sort.Slice(found, func(i, j int) bool { return found[i].Slug < found[j].Slug })
	out.Themes = append(out.Themes, found...)
}

// Container reads one container's bytes out of the resolved app library by slug, and
// returns (nil, nil) — "no copy, no opinion" — when the library is absent or the slug is
// not in it. The Themes face's drift check calls this through the client's Containers
// hook, and the nil answer is what makes that face say "cannot compare" rather than
// invent an "up to date".
//
// This is what starterContainerFor was, with the hard-coded two-name table deleted: the
// library's contents are the directory's, so a first-party app an operator added is as
// comparable as the shipped pair.
func (l *Library) Container(slug string) ([]byte, error) {
	if l == nil || l.AppsDir == "" || slug == "" {
		return nil, nil
	}
	body, err := os.ReadFile(filepath.Join(l.AppsDir, slug+".orbapp"))
	if os.IsNotExist(err) {
		return nil, nil
	}
	return body, err
}

// Import (S6, plan §4): the browser file door, on the hub side.
//
// The drop zone already installs a picked container by POSTing it to the device. What it
// could not do was KEEP one — the bytes went through the hub and out again, so a first
// party app an operator downloaded onto a laptop was never in the set the next bare board
// gets seeded from, and the Themes face had no copy to compare it against. keep=1 files
// the bytes; install=1 sends them on to the orb in the same press.
//
// ParseApp runs before anything moves, and a local refusal costs no dial: the hub refuses
// a container it cannot read rather than forwarding a thing it has no opinion about, which
// is the rule //go:embed let it skip for a year.

// h_appsImport answers POST /api/apps/import?ip=&install=1&keep=1. The body is the raw
// .orbapp, the same raw-body shape the device's own door takes and for the same reason: a
// multipart parse here would re-encode bytes the door downstream wants untouched.
// h_libraryRm answers POST /api/library/rm?kind=app|theme&name=<slug|file>. It is the
// only door that deletes a hub-side library file, and it deletes exactly that: the dial
// is not dialed, no row is uninstalled, and no card file is touched. The Apps face owns
// the ordering when an operator asks for the whole erase.
func (s *Server) h_libraryRm(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	q := r.URL.Query()
	dest, err := s.library().Drop(q.Get("kind"), strings.TrimSpace(q.Get("name")))
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	s.jsonOK(w, map[string]any{"removed": dest})
}

func (s *Server) h_appsImport(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	ip := strings.TrimSpace(r.URL.Query().Get("ip"))
	install := r.URL.Query().Get("install") == "1"
	keep := r.URL.Query().Get("keep") == "1"
	body, err := io.ReadAll(io.LimitReader(r.Body, int64(appUploadMax)+1))
	if err != nil {
		s.jsonErr(w, "the upload did not arrive: "+err.Error())
		return
	}
	if len(body) == 0 {
		s.jsonErr(w, "no container in the request body")
		return
	}
	if len(body) > appUploadMax {
		s.jsonErr(w, "that file is over the size the device's own door takes")
		return
	}
	// Parsed locally, first, whatever the caller asked for next. The slug the rest of the
	// run uses comes out of the header rather than out of a filename, which is the device's
	// rule and is why a rename on the way cannot smuggle one container in as another.
	c, perr := orbnet.ParseApp(body)
	if perr != nil {
		s.jsonErr(w, "the hub will not forward it: "+strings.TrimPrefix(perr.Error(), "orbnet: "))
		return
	}
	out := map[string]any{"slug": c.Slug, "title": c.Title, "version": c.Version,
		"kept": "", "keep_refused": "", "installed": false}

	// Keep first, and a failed keep is reported rather than swallowed: a press that asked
	// to keep and silently did not would leave the operator believing a library that is not
	// there. The device's own overwrite rule applies — refuse, and name the path a hand
	// could move instead.
	if keep {
		// A re-import of a slug the library already holds replaces it when the press says
		// replace. That is how a newer build of an app reaches a hub now that the import no
		// longer installs, and the answer names what happened rather than leaving the
		// operator to wonder whether the shelf holds the old bytes or the new.
		_, existed := os.Stat(filepath.Join(s.library().writableAppsDir(), c.Slug+".orbapp"))
		dest, kerr := s.library().KeepOver(c.Slug, body, r.URL.Query().Get("replace") == "1")
		if kerr != nil {
			out["keep_refused"] = kerr.Error()
		} else {
			out["kept"] = dest
			out["overwrote"] = existed == nil
		}
	}
	if install {
		if ip == "" {
			s.jsonErr(w, "install=1 needs an ip")
			return
		}
		if _, ok := s.checkedTarget(w, ip, "", false); !ok {
			return
		}
		release, rerr := s.holdDevicePort(r.Context(), ip, deviceTimeout)
		if rerr != nil {
			s.jsonErr(w, rerr.Error())
			return
		}
		defer release()
		// The bulk client, for the reason installOneApp states at its own call site: this
		// POST is answered only after the device has written the whole container to the card,
		// and the management client's 8 seconds is not a budget for that.
		sent := 0
		res, uerr := s.ota.AppUploadProgress(ip, body, true, func(n int64) { sent = int(n) })
		if uerr != nil {
			if out["kept"] == "" {
				s.jsonErr(w, "writing "+c.Slug+" to the card: "+plainDeviceErr(uerr, sent).Error())
				return
			}
			// The file is in the library; the dial failed. That is a half-done press and it
			// says so — the bytes are safe, the board is not.
			out["install_refused"] = plainDeviceErr(uerr, 0).Error()
		} else {
			out["installed"] = true
			out["rescan"] = res
		}
	}
	if out["kept"] == "" && out["keep_refused"] != "" && !out["installed"].(bool) {
		s.jsonErr(w, "the hub kept nothing: "+out["keep_refused"].(string))
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{"ok": true, "data": out})
}

// appUploadMax is the ceiling the import takes, and it is not invented here: a file bigger
// than the device's own door accepts is refused on this side, where the answer is one
// sentence, instead of after a dial and a partial write.
const appUploadMax = 8 << 20

// Keep files one container in the first WRITABLE library directory, creating that
// directory's apps/ tier if the library root exists without it. Refusing a duplicate is
// the answer to a missing flag, and the sentence names the path it would have used: a hub
// that overwrote a curated library because a dialog ticked a box destroyed work the
// operator did somewhere else.
func (l *Library) Keep(slug string, body []byte) (string, error) {
	return l.KeepOver(slug, body, false)
}

// KeepOver is Keep with the replace decision made explicit. The import needs it: with the
// face's install checkbox gone, importing a newer build of an app the library already holds
// IS how an upgrade arrives at the hub, and a flat "already in the library; move it aside by
// hand" would strand the common case on a manual step. The overwrite is still not silent —
// the caller says so by passing true, and the response reports it.
func (l *Library) KeepOver(slug string, body []byte, overwrite bool) (string, error) {
	if slug == "" {
		return "", fmt.Errorf("the container named no slug to file it under")
	}
	root := l.writableAppsDir()
	if root == "" {
		return "", fmt.Errorf("no writable library directory — the resolved apps dir is %q, and -library names the root an operator can write",
			l.AppsDir)
	}
	dest := filepath.Join(root, slug+".orbapp")
	if _, err := os.Stat(dest); err == nil && !overwrite {
		return "", fmt.Errorf("%s is already in the library; import it again with replace to overwrite it, or move it aside by hand", dest)
	}
	if err := os.MkdirAll(root, 0o755); err != nil {
		return "", fmt.Errorf("the library directory %s could not be created: %w", root, err)
	}
	// Written beside the target and renamed, the store's own discipline: a half-written
	// container left as apps/slug.orbapp by a crash is a library entry that lists as a
	// refusal, and the next seed run has nothing good to send.
	tmp := dest + ".part"
	if err := os.WriteFile(tmp, body, 0o644); err != nil {
		return "", fmt.Errorf("the library file could not be written: %w", err)
	}
	if err := os.Rename(tmp, dest); err != nil {
		_ = os.Remove(tmp)
		return "", fmt.Errorf("the library file could not be filed: %w", err)
	}
	log.Printf("orb-manager: library kept %s (%d B)", dest, len(body))
	return dest, nil
}

// Drop deletes one library file. It is the mirror of Keep and takes the same care about
// the name: the caller passes a slug (apps) or a filename (themes), the hub rebuilds the
// path itself, and the result must sit directly inside a resolved library directory. A
// name that would walk out of one is refused rather than sanitised, because the route
// deletes real files and the only safe reading of a surprising argument is "no".
//
// Deleting here touches the LIBRARY and nothing else. It never dials a board and never
// uninstalls a row: an app on a dial is on the dial, and the face that says otherwise is
// the bug. The caller who wants both asks for both, in that order.
func (l *Library) Drop(kind, name string) (string, error) {
	var dir, suffix string
	switch kind {
	case "app":
		dir, suffix = l.writableAppsDir(), ".orbapp"
		if !orbnet.AppStemOK(name) {
			return "", fmt.Errorf("%q is not an app name the library files under", name)
		}
	case "theme":
		dir, suffix = l.writableThemesDir(), ".orbtheme"
		if !themeFileOK(name) {
			return "", fmt.Errorf("%q is not a theme filename the library files under", name)
		}
	default:
		return "", fmt.Errorf("kind must be app or theme, not %q", kind)
	}
	if dir == "" {
		return "", fmt.Errorf("no writable library directory for %ss — the resolved dirs are %q and %q",
			kind, l.AppsDir, l.ThemesDir)
	}
	dest := filepath.Join(dir, name+suffix)
	if filepath.Dir(dest) != filepath.Clean(dir) {
		return "", fmt.Errorf("%q is not a name inside the library directory", name)
	}
	if err := os.Remove(dest); err != nil {
		if os.IsNotExist(err) {
			return "", fmt.Errorf("%s is not in the library at %s", filepath.Base(dest), dir)
		}
		return "", fmt.Errorf("%s could not be deleted: %w", dest, err)
	}
	log.Printf("orb-manager: library dropped %s", dest)
	return dest, nil
}

// themeFileOK is the theme side of the same one-path-segment rule the app slug carries.
// A theme is identified by its embedded name, so a library theme is filed under whatever
// stem the operator chose; what is NOT negotiable is that it be one plain segment ending
// in the extension Drop itself appends.
//
// The character set is the device's own orb_sd_name_ok — 1..55 bytes of [A-Za-z0-9._-]
// with no leading dot — rather than a stricter hub invention. "skeleton", the shipped
// default's own stem, is a mixed-case-free plain name and a lowercase-only gate would
// have refused it while the firmware listed it happily.
func themeFileOK(s string) bool {
	if s == "" || len(s) > 55 || strings.ContainsAny(s, `/\`+"\x00") {
		return false
	}
	// The extension is Drop's to add, so a name arriving with one is a caller that misread
	// the argument, and joining it would ask for "x.orbtheme.orbtheme".
	if strings.HasPrefix(s, ".") || strings.EqualFold(filepath.Ext(s), ".orbtheme") {
		return false
	}
	for _, r := range s {
		if !(r >= 'a' && r <= 'z') && !(r >= 'A' && r <= 'Z') && !(r >= '0' && r <= '9') &&
			r != '-' && r != '_' && r != '.' {
			return false
		}
	}
	return true
}

// writableAppsDir picks where a Keep lands: the resolved apps dir when it is writable,
// otherwise the user-config rung, which is the directory the import creates and the one an
// operator can always write. A read-only resolved dir is not silently swapped for the
// clone's tree — that would file a personal app into a source checkout.
func (l *Library) writableAppsDir() string {
	if l.AppsDir != "" && dirWritable(l.AppsDir) {
		return l.AppsDir
	}
	if cfg, err := os.UserConfigDir(); err == nil && cfg != "" {
		return filepath.Join(cfg, "orb-manager", libAppsSubdir)
	}
	return ""
}

// writableThemesDir is writableAppsDir's twin on the theme side, and it exists for one
// reason: Drop must refuse to delete from a directory the hub would never have written
// to, and the fallback rung is where an import filed it when the resolved dir was read
// only. The seed themes/ tree is a checked-in artifact, and deleting a theme the operator
// never added is not what "remove from hub library" means.
func (l *Library) writableThemesDir() string {
	if l.ThemesDir != "" && dirWritable(l.ThemesDir) {
		return l.ThemesDir
	}
	if cfg, err := os.UserConfigDir(); err == nil && cfg != "" {
		return filepath.Join(cfg, "orb-manager", libThemesSubdir)
	}
	return ""
}

// dirWritable asks the only question that answers the thing: create a file and remove it.
// A mode-bit check lies on a read-only mount, on Windows ACLs, and across a network share,
// all three of which are exactly how an operator ends up with a library they cannot keep.
func dirWritable(dir string) bool {
	probe := filepath.Join(dir, ".orb-writable-probe")
	f, err := os.OpenFile(probe, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o600)
	if err != nil {
		return false
	}
	_ = f.Close()
	_ = os.Remove(probe)
	return true
}

// firstTheme returns the library's first .orbtheme by name order, refusing loudly when
// the directory holds none. "First" is not a preference ranking — a curated library holds
// the one theme its owner wants a bare dial to wear, and the deterministic order is what
// makes a re-run send the same bytes rather than whichever entry the filesystem surfaced.
//
// A file the door would refuse is skipped with a log line rather than failing the run:
// the seed's job is to clothe a bare dial, and a broken file sitting beside a good one is
// not a reason to leave the board naked. The Library pane names the refusal, so the skip
// is never the only place the truth could be found.
func (l *Library) firstTheme() (string, []byte, error) {
	if l == nil || l.ThemesDir == "" {
		return "", nil, fmt.Errorf("no theme library is resolved, so there is nothing to seed")
	}
	body, err := l.namedTheme("")
	if err != nil {
		return "", nil, err
	}
	return body.name, body.blob, nil
}

// themeBlob is one shelf theme's bytes, addressed by the shelf's file stem (""
// asks the first by name order, which is what firstTheme seeds). D38: the
// Themes face's Install press names one file, so the read is by name, not by
// the directory's accident of sort order. The name is checked to be a plain
// file stem before it reaches a path builder — the same rule h_libraryRm's
// delete gate holds.
type themeBlob struct {
	name string
	blob []byte
}

func (l *Library) namedTheme(stem string) (themeBlob, error) {
	if l == nil || l.ThemesDir == "" {
		return themeBlob{}, fmt.Errorf("no theme library is resolved, so there is nothing to send")
	}
	if stem != "" && !themeFileOK(stem) {
		return themeBlob{}, fmt.Errorf("%q is not a theme filename the library files under", stem)
	}
	entries, err := os.ReadDir(l.ThemesDir)
	if err != nil {
		return themeBlob{}, fmt.Errorf("the theme library at %s cannot be read: %w", l.ThemesDir, err)
	}
	names := []string{}
	for _, e := range entries {
		if !e.IsDir() && strings.EqualFold(filepath.Ext(e.Name()), ".orbtheme") {
			if stem != "" && !strings.EqualFold(strings.TrimSuffix(e.Name(), filepath.Ext(e.Name())), stem) {
				continue
			}
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)
	for _, name := range names {
		body, err := os.ReadFile(filepath.Join(l.ThemesDir, name))
		if err != nil {
			log.Printf("orb-manager: theme read skips %s: %v", name, err)
			continue
		}
		if _, err := orbnet.ContainerName(body); err != nil {
			log.Printf("orb-manager: theme read skips %s: %v", name, err)
			continue
		}
		return themeBlob{name: name, blob: body}, nil
	}
	if len(names) == 0 {
		if stem != "" {
			return themeBlob{}, fmt.Errorf("the theme library at %s holds no %s.orbtheme", l.ThemesDir, stem)
		}
		return themeBlob{}, fmt.Errorf("the theme library at %s holds no .orbtheme", l.ThemesDir)
	}
	return themeBlob{}, fmt.Errorf("every .orbtheme in %s was refused; the Library card names why", l.ThemesDir)
}

// LibStatusLines is what startup prints about the resolution — two lines, one per kind,
// each naming the winning rung rather than just the path, so an operator can see WHY this
// hub seeded from there.
func (l *Library) LibStatusLines() []string {
	line := func(kind, dir, from string) string {
		if dir == "" {
			return fmt.Sprintf("library: no %s directory found — this hub seeds nothing", kind)
		}
		return fmt.Sprintf("library: %s = %s (%s)", kind, dir, from)
	}
	return []string{line(libAppsSubdir, l.AppsDir, l.AppsSource),
		line(libThemesSubdir, l.ThemesDir, l.ThemesSource)}
}
