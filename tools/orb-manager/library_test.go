package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// Library tests (S6, plan §8.1). Three things are under test, and each is the failure
// mode the design is exposed to rather than a happy path:
//
//   - the resolution ladder's ORDER, because the order is the whole behaviour — a hub
//     that found the clone's copy when the operator named a directory silently seeds
//     someone else's bytes;
//   - the naming of a refusal, because a listing that drops an unparseable file is a
//     listing that lies about a directory that plainly contains it;
//   - the route contract, because the pane fetches /api/library while a sibling fetches
//     /api/status, and a 404 or a wrong shape reads in the browser as "no apps".

// cloneFixture copies one committed .orbapp fixture into dir under a name its own header
// agrees with. The fixtures are the pair every reader test points at, and the listing's
// stem-must-equal-slug rule means a fixture can only be filed under its own slug — which
// is the point: a test that renamed one would be testing the refusal, not the listing.
func cloneFixture(t *testing.T, dir, slug string) string {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	src := filepath.Join("..", "..", "tests", "fixtures", slug+".orbapp")
	body, err := os.ReadFile(src)
	if err != nil {
		t.Skipf("no fixture at %s: %v", src, err)
	}
	path := filepath.Join(dir, slug+".orbapp")
	if err := os.WriteFile(path, body, 0o644); err != nil {
		t.Fatalf("write: %v", err)
	}
	return path
}

// TestLibraryResolutionOrder is the plan's §8.1 resolution case, built as a five-level
// stack so every rung is present and the winner is the one the order names rather than
// the only one that exists.
func TestLibraryResolutionOrder(t *testing.T) {
	root := t.TempDir()
	mk := func(parts ...string) string {
		d := filepath.Join(parts...)
		if err := os.MkdirAll(d, 0o755); err != nil {
			t.Fatal(err)
		}
		return d
	}
	// -library names a ROOT; the kinds live inside it.
	flagRoot := mk(root, "flagged")
	flagDir := mk(flagRoot, libAppsSubdir)
	exeDir := mk(root, "exe")
	exeApps := mk(exeDir, libAppsSubdir)
	cfg := mk(root, "cfg")
	repoRoot := mk(root, "repo")
	repo := mk(repoRoot, "tools", "orb-manager", libAppsSubdir)

	// Flag first, with every other rung present.
	lib := resolveLibrary(flagRoot, "", "", exeDir, cfg, repoRoot)
	if lib.AppsDir != flagDir {
		t.Errorf("flag rung lost: got %q want %q", lib.AppsDir, flagDir)
	}
	if !strings.Contains(lib.AppsSource, "flag") {
		t.Errorf("the winning rung is not reported: %q", lib.AppsSource)
	}

	// No flag: beside the executable wins over the config dir and the clone.
	lib = resolveLibrary("", "", "", exeDir, cfg, repoRoot)
	if lib.AppsDir != exeApps {
		t.Errorf("exe rung lost: got %q want %q", lib.AppsDir, exeApps)
	}

	// A per-kind flag beats the root flag, so the split-kinds case stays available.
	other := mk(root, "kindflag")
	lib = resolveLibrary(flagRoot, other, "", exeDir, cfg, repo)
	if lib.AppsDir != other {
		t.Errorf("-library-apps lost to -library: got %q want %q", lib.AppsDir, other)
	}

	// Nothing but the clone: the last rung still resolves, and names itself.
	lib = resolveLibrary("", "", "", mk(root, "nowhere"), cfg, repoRoot)
	if lib.AppsDir != repo {
		t.Errorf("clone rung lost: got %q want %q", lib.AppsDir, repo)
	}
	if !strings.Contains(lib.AppsSource, "clone") {
		t.Errorf("the winning rung is not reported: %q", lib.AppsSource)
	}

	// Nothing anywhere: empty, and reported as such rather than as an error. A hub on a
	// machine with no clone and no library directory still has to start.
	lib = resolveLibrary("", "", "", mk(root, "empty-exe"), mk(root, "empty-cfg"),
		mk(root, "empty-repo"))
	if lib.AppsDir != "" || lib.AppsSource != "none found" {
		t.Errorf("a hub with no library must resolve to nothing, got %q / %q",
			lib.AppsDir, lib.AppsSource)
	}
}

// TestLibraryNamesEveryRefusal is the invariant //go:embed broke, stated as an
// assertion: a file the hub cannot parse is present in the answer with a reason, and
// never simply absent. Three ways a file can fail are exercised together because they
// fail in different places — unreadable bytes, a header the reader refuses, and a header
// that disagrees with the filename it arrived under.
func TestLibraryNamesEveryRefusal(t *testing.T) {
	dir := t.TempDir()
	cloneFixture(t, dir, "plain")

	// A header the reader refuses: right magic, wrong format word.
	bad := make([]byte, len(appHdrBytesForTest))
	copy(bad, appHdrBytesForTest)
	bad[4] = 9 // W_FORMAT: a format no reader in the family speaks
	if err := os.WriteFile(filepath.Join(dir, "broken.orbapp"), bad, 0o644); err != nil {
		t.Fatal(err)
	}
	// A short file: not a container at all.
	if err := os.WriteFile(filepath.Join(dir, "stub.orbapp"), []byte("hi"), 0o644); err != nil {
		t.Fatal(err)
	}
	// A container filed under someone else's name.
	renamed, err := os.ReadFile(filepath.Join("..", "..", "tests", "fixtures", "sentinel.orbapp"))
	if err != nil {
		t.Skipf("no sentinel fixture: %v", err)
	}
	if err := os.WriteFile(filepath.Join(dir, "clock.orbapp"), renamed, 0o644); err != nil {
		t.Fatal(err)
	}

	out, err := (&Server{lib: &Library{AppsDir: dir}}).h_library(nil)
	if err != nil {
		t.Fatal(err)
	}
	p := out.(*LibraryPayload)
	if len(p.Apps) != 1 || p.Apps[0].Slug != "plain" {
		t.Errorf("the loadable file did not list beside the broken ones: %+v", p.Apps)
	}
	byFile := map[string]string{}
	for _, r := range p.Refused {
		byFile[r.File] = r.Why
	}
	for _, f := range []string{"broken.orbapp", "stub.orbapp", "clock.orbapp"} {
		why, ok := byFile[f]
		if !ok {
			t.Errorf("%s was dropped rather than refused; refusals were %v", f, byFile)
			continue
		}
		if strings.TrimSpace(why) == "" {
			t.Errorf("%s refused with no reason", f)
		}
	}
	if w := byFile["clock.orbapp"]; !strings.Contains(w, "sentinel") {
		t.Errorf("the mismatch refusal does not name the slug it found: %q", w)
	}
}

// appHdrBytesForTest is one header's width, taken from the fixture rather than a second
// constant: a test that hard-coded 236 would keep passing after a layout change that made
// the real width 244, and the corruption it performs lands on the wrong word.
var appHdrBytesForTest = func() []byte {
	b, err := os.ReadFile(filepath.Join("..", "..", "tests", "fixtures", "plain.orbapp"))
	if err != nil {
		return nil
	}
	return b[:256]
}()

// TestLibraryRouteIsWiredAndShaped is the plan's §8.1 route-contract case, the sibling of
// the /api/fleet contract test and for the same reason: the pane fetches /api/library
// while a sibling fetches /api/status, and a 404 or a shape the JS does not expect reads
// in the browser as "no apps" rather than as a missing route.
func TestLibraryRouteIsWiredAndShaped(t *testing.T) {
	s := newHub()
	s.lib = &Library{} // nothing resolved: the empty answer must still be a well-formed one
	mux := http.NewServeMux()
	s.routes(mux)

	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/api/library", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("GET /api/library: %d, body %s", rec.Code, rec.Body.String())
	}
	var env struct {
		OK   bool           `json:"ok"`
		Data map[string]any `json:"data"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
		t.Fatalf("envelope: %v (%s)", err, rec.Body.String())
	}
	if !env.OK {
		t.Fatalf("the route answered not-ok: %s", rec.Body.String())
	}
	// Every field app.js reads has to be present and of the type it assumes. The three
	// arrays being non-null is the specific rule: app.js calls .length on each, and a
	// JSON null throws where an empty array draws an empty row.
	for _, k := range []string{"apps", "themes", "refused"} {
		v, ok := env.Data[k]
		if !ok {
			t.Errorf("data.%s absent", k)
			continue
		}
		if v == nil {
			t.Errorf("data.%s is null; the client reads .length on it", k)
			continue
		}
		if _, isList := v.([]any); !isList {
			t.Errorf("data.%s is %T, not a list", k, v)
		}
	}
	for _, k := range []string{"appsDir", "themesDir", "appsSource", "themesSource"} {
		if _, ok := env.Data[k].(string); !ok {
			t.Errorf("data.%s is %#v, not a string the pane can print", k, env.Data[k])
		}
	}
}

// TestLibraryListingMatchesTheShippedArtifacts is the freshness case for the rename: the
// clone's own apps/ and themes/ are what a development hub serves, and a listing that
// came back empty would mean the ladder points at a directory the artifacts no longer
// live in. It is the Go mirror of the sentinel that guards the same files in Python.
func TestLibraryListingMatchesTheShippedArtifacts(t *testing.T) {
	repo := filepath.Join("..", "..")
	lib := resolveLibrary("", "", "", filepath.Join(repo, "no-such-exe-dir"),
		filepath.Join(repo, "no-such-config-dir"), repo)
	if lib.AppsDir == "" {
		t.Fatal("the clone's apps/ did not resolve — the directory the artifacts live in moved")
	}
	out, err := (&Server{lib: lib}).h_library(nil)
	if err != nil {
		t.Fatal(err)
	}
	p := out.(*LibraryPayload)
	if len(p.Apps) < 2 {
		t.Errorf("the shipped library listed %d apps; the pair is the floor", len(p.Apps))
	}
	if len(p.Themes) < 1 {
		t.Errorf("the shipped library listed %d themes; the seed is the floor", len(p.Themes))
	}
	if len(p.Refused) != 0 {
		t.Errorf("the shipped library refused a file: %+v", p.Refused)
	}
	// S6's whole point for a first-party app: the stamp the hub serves is readable, so a
	// version the operator can see is on the row.
	for _, a := range p.Apps {
		if a.Version == "" {
			t.Errorf("%s carries no version; tools/make_factory_apps.py stamps version.txt", a.File)
		}
	}
	// A theme themes ONE app, and the row has to say which: the Themes face's pane is
	// addressed by app, and a library row that names only the owner's title leaves the
	// operator guessing. The value comes from the container's own section index, so it is
	// the file's statement and not a table.
	for _, th := range p.Themes {
		if th.App == "" {
			t.Errorf("%s listed no app; every FORMAT 3 theme names the one it themes", th.File)
			continue
		}
		// The invariant the Aviator rename bought, pinned as a rule rather than as a
		// literal: a shipped theme's filename stem matches its own embedded title,
		// case-folded. The device files a theme under a stem derived from the NAME and
		// ignores the filename entirely, so a seed whose two disagree was a library row
		// no Themes face could match to an installed row. Asserting the relationship
		// instead of the string means the next theme added to the shelf is held to it too.
		stem := strings.TrimSuffix(th.File, ".orbtheme")
		if !strings.EqualFold(stem, th.Title) {
			t.Errorf("%s is titled %q; a shipped theme's filename must match its own name, "+
				"or the Themes face can never line the row up with the card stem the device derives",
				th.File, th.Title)
		}
		if th.App == "clock" && !strings.EqualFold(th.Title, "Aviator") {
			t.Errorf("the clock's shipped theme is titled %q; the seed renamed to Aviator in S6", th.Title)
		}
	}
	// App rows carry no App: the slug already is the app, and a duplicate field there is
	// a second answer to the same question waiting to disagree.
	for _, a := range p.Apps {
		if a.App != "" {
			t.Errorf("app row %s filled App=%q; that field is a theme's answer", a.File, a.App)
		}
	}
}

// TestLibraryAnEmptyRungWins is the case the plan called out explicitly: an operator who
// created apps/ and left it empty means "nothing here". Falling through to the clone
// would seed files they just watched themselves decline, so the empty rung must win.
func TestLibraryAnEmptyRungWins(t *testing.T) {
	root := t.TempDir()
	exeDir := filepath.Join(root, "exe")
	if err := os.MkdirAll(filepath.Join(exeDir, libAppsSubdir), 0o755); err != nil {
		t.Fatal(err)
	}
	clone := filepath.Join(root, "repo", "tools", "orb-manager", libAppsSubdir)
	cloneFixture(t, clone, "plain")

	lib := resolveLibrary("", "", "", exeDir, filepath.Join(root, "cfg"), clone)
	if lib.AppsDir != filepath.Join(exeDir, libAppsSubdir) {
		t.Fatalf("an empty library fell through to the clone: %q", lib.AppsDir)
	}
	out, err := (&Server{lib: lib}).h_library(nil)
	if err != nil {
		t.Fatal(err)
	}
	p := out.(*LibraryPayload)
	if len(p.Apps) != 0 {
		t.Errorf("the empty library listed %d apps; the clone's bytes leaked through",
			len(p.Apps))
	}
}

// TestImportRefusesLocallyAndKeepsOnDisk is plan §8.1's corrupt-during-import case and
// the keep half of §4 together. The refusal must cost no dial — the hub parsed the body and
// said no — and the accepted one must land in the library named after its HEADER, not after
// whatever the browser called the file. Those two facts are the whole reason the route
// exists instead of the drop zone dialing the device directly.
func TestImportRefusesLocallyAndKeepsOnDisk(t *testing.T) {
	dir := t.TempDir()
	src := cloneFixture(t, dir, "plain")
	body, err := os.ReadFile(src)
	if err != nil {
		t.Fatal(err)
	}
	// A second directory to keep into, so the kept file is never the fixture the test just
	// read out of. Keeping into the same directory would report a duplicate refusal for the
	// wrong reason and the assertion would pass on a bug.
	keep := filepath.Join(dir, "keep")
	if err := os.MkdirAll(keep, 0o755); err != nil {
		t.Fatal(err)
	}
	s := newHub()
	s.lib = &Library{AppsDir: keep}
	s.noAutoInstall = true
	s.token = "tk"
	mux := http.NewServeMux()
	s.routes(mux)

	// A corrupt body is refused before any dial is attempted. No ip is supplied at all, so
	// a route that parsed lazily and forwarded would fail in a way this assertion cannot
	// tell apart from a network error; refusing on the parse is the only way through.
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, importReq("keep=1", strings.NewReader("not a container")))
	if rec.Code == http.StatusOK {
		t.Errorf("the hub forwarded an unparseable body: %s", rec.Body.String())
	}
	if !strings.Contains(rec.Body.String(), "will not forward") {
		t.Errorf("the refusal is not the local parse's sentence: %s", rec.Body.String())
	}

	// The real container, kept. The file comes out named plain.orbapp whatever the browser
	// called it, because the header is the identity and the filename is decoration.
	rec = httptest.NewRecorder()
	mux.ServeHTTP(rec, importReq("keep=1", bytes.NewReader(body)))
	if rec.Code != http.StatusOK {
		t.Fatalf("keep refused a good container: %d %s", rec.Code, rec.Body.String())
	}
	var env struct {
		OK   bool `json:"ok"`
		Data struct {
			Slug string `json:"slug"`
			Kept string `json:"kept"`
		} `json:"data"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
		t.Fatal(err)
	}
	if !env.OK || env.Data.Slug != "plain" {
		t.Fatalf("the import answered %s", rec.Body.String())
	}
	if filepath.Base(env.Data.Kept) != "plain.orbapp" {
		t.Errorf("the kept file is named %q, not after its header's slug", env.Data.Kept)
	}
	if _, err := os.Stat(env.Data.Kept); err != nil {
		t.Errorf("the route reported a path it did not write: %v", err)
	}
	// The temp-file-then-rename discipline: no .part left behind means a crash cannot leave
	// a library holding a file that lists as a refusal forever.
	if left, _ := filepath.Glob(filepath.Join(keep, "*.part")); len(left) != 0 {
		t.Errorf("the import left a partial file behind: %v", left)
	}

	// The duplicate press refuses rather than overwriting, and names the path so a hand
	// knows what to move. Silent overwrite of a curated library is the failure here.
	rec = httptest.NewRecorder()
	mux.ServeHTTP(rec, importReq("keep=1", bytes.NewReader(body)))
	if strings.Contains(rec.Body.String(), `"ok":true`) {
		t.Error("a second keep overwrote the library file instead of refusing")
	}
	if !strings.Contains(rec.Body.String(), "already in the library") {
		t.Errorf("the duplicate refusal does not name why: %s", rec.Body.String())
	}
}

// TestInstallRouteIsWiredAndGated checks the press exists and is gated, without dialing
// anything: a GET and an address the hub never learned both have to be refused before the
// hub touches a board.
func TestInstallRouteIsWiredAndGated(t *testing.T) {
	s := newHub()
	s.lib = &Library{}
	mux := http.NewServeMux()
	s.routes(mux)

	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/api/library/install?ip=10.0.0.5", nil))
	if rec.Code == http.StatusOK {
		t.Error("GET seeded something; the route is POST-only")
	}
	rec = httptest.NewRecorder()
	mux.ServeHTTP(rec, installReq("/api/library/install?ip=203.0.113.9"))
	if rec.Code == http.StatusOK {
		t.Error("the seed route accepted an address the hub never learned")
	}
}

// TestAutoInstallOffIsRefused is `-no-autoinstall`'s promise: the automatic arm is off and the
// press still works, which is the difference between a bench switch and a removed feature.
func TestAutoInstallOffIsRefused(t *testing.T) {
	s := newHub()
	s.lib = &Library{}
	s.noAutoInstall = true
	s.token = "tk"
	mux := http.NewServeMux()
	s.routes(mux)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, installReq("/api/library/install?ip=203.0.113.9&only=clock&auto=1"))
	if strings.Contains(rec.Body.String(), `"ok":true`) {
		t.Error("`-no-autoinstall` let the automatic arm through")
	}

}

// importReq and installReq build the gated POSTs the tests below send. The token header and
// the same-origin Origin are not decoration: requireControl refuses the call without them,
// and a test that skipped them would be asserting on a 403 it mistook for the handler's own
// refusal.
func importReq(query string, body io.Reader) *http.Request {
	r := httptest.NewRequest(http.MethodPost, "/api/apps/import?"+query, body)
	r.Header.Set("X-Orb-Token", "tk")
	r.Header.Set("Sec-Fetch-Site", "same-origin")
	return r
}

func installReq(path string) *http.Request {
	r := httptest.NewRequest(http.MethodPost, path, nil)
	r.Header.Set("X-Orb-Token", "tk")
	r.Header.Set("Sec-Fetch-Site", "same-origin")
	return r
}

// TestInstallFloorNamesTheReleaseThatReadsFormat3 is the crosscheck between three numbers that
// drifted apart on real hardware and produced "writing clock to the card: rejected: app format
// 3; this build reads format 2" after 660 KB had already crossed the room.
//
// The invariant: installMinFW names the release whose reader accepts the format the hub ships.
// The floor cannot be read off the tree's version — that was the error made twice. It is
// derived here from what the reader in this tree actually accepts, and from whether that
// reader is released or still uncommitted:
//
//   - every artifact the hub ships must carry the format this tree's reader reads;
//   - if the reader is AHEAD of the last committed reader (the format bump is sitting in the
//     working tree, unreleased), then the stamp must have moved with it — .clinerules and
//     docs/architecture.md both require an ABI change to bump version.txt — so the floor must
//     equal the tree's own version, and the tree's version must be strictly above HEAD's;
//   - if the reader is committed at this format, the floor must not exceed the tree's version,
//     or the hub refuses to install onto the image `make firmware` produces.
//
// The earlier version of this test only checked the floor against version.txt, which is how a
// floor of v1.1.27 passed while v1.1.27 was a FORMAT 2 release. Checking the floor against the
// reader's actual format is what catches that.
func TestInstallFloorNamesTheReleaseThatReadsFormat3(t *testing.T) {
	root := filepath.Join("..", "..")

	// The reader this tree builds, straight from the firmware header.
	treeFmt := appFormatIn(t, filepath.Join(root, "main", "app", "orb_app_manifest.h"))
	headHdr := gitShow(t, root, "main/app/orb_app_manifest.h")
	headFmt := appFormatFrom(t, headHdr)

	// What every shipped artifact actually is, from its own header word rather than its name.
	for _, p := range []string{"apps/clock.orbapp", "apps/color_picker.orbapp"} {
		b, err := os.ReadFile(filepath.Join(p))
		if err != nil {
			t.Fatalf("read %s: %v", p, err)
		}
		if len(b) < 8 {
			t.Fatalf("%s is too short to carry a header", p)
		}
		got := binary.LittleEndian.Uint32(b[4:8])
		if got != treeFmt {
			t.Errorf("%s is FORMAT %d but this tree's reader accepts FORMAT %d: the hub would ship "+
				"a file its own firmware refuses", p, got, treeFmt)
		}
	}

	floor, ok := firmware.ParseVersion(installMinFW)
	if !ok {
		t.Fatalf("installMinFW does not parse: %q", installMinFW)
	}
	treeV := readTreeVersion(t, root)

	if treeFmt > headFmt {
		// The format bump is unreleased. The floor must be the NEXT stamp, and the stamp must
		// actually have moved: a reader change behind an unchanged version.txt lets the floor
		// name a release that cannot read the files, which is the exact bug reported.
		headV, ok := firmware.ParseVersion(headVersion(t, root))
		if !ok {
			t.Fatalf("HEAD's version.txt does not parse: %q", headVersion(t, root))
		}
		if floor.Compare(treeV) != 0 {
			t.Errorf("installMinFW = %s but the FORMAT %d reader is uncommitted and this tree is %s: "+
				"the floor must be the release that will carry the reader", installMinFW, treeFmt, treeV.Raw)
		}
		if floor.Compare(headV) <= 0 {
			t.Errorf("the reader moved to FORMAT %d but installMinFW = %s is at or behind the last "+
				"committed release %s, which reads FORMAT %d: bump version.txt with the reader and set "+
				"the floor to the new stamp", treeFmt, installMinFW, headV.Raw, headFmt)
		}
		return
	}
	if floor.Compare(treeV) > 0 {
		t.Errorf("installMinFW = %s is ahead of the tree's own %s: no build anyone can make satisfies "+
			"the floor, so a freshly flashed board is refused by its own hub", installMinFW, treeV.Raw)
	}
}

// appFormatIn reads the ORBAPP_FORMAT a header declares.
func appFormatIn(t *testing.T, path string) uint32 {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	return appFormatFrom(t, string(b))
}

func appFormatFrom(t *testing.T, src string) uint32 {
	t.Helper()
	m := regexp.MustCompile(`(?m)^#define\s+ORBAPP_FORMAT\s+(\d+)u`).FindStringSubmatch(src)
	if m == nil {
		t.Fatal("no #define ORBAPP_FORMAT found — the reader's format moved somewhere unexpected")
	}
	n, err := strconv.ParseUint(m[1], 10, 32)
	if err != nil {
		t.Fatalf("ORBAPP_FORMAT is not a number: %q", m[1])
	}
	return uint32(n)
}

// readTreeVersion is version.txt, the stamp the next idf.py build carries.
func readTreeVersion(t *testing.T, root string) firmware.Version {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(root, "version.txt"))
	if err != nil {
		t.Skipf("no version.txt reachable from this package: %v", err)
	}
	v, ok := firmware.ParseVersion(strings.TrimSpace(string(b)))
	if !ok {
		t.Fatalf("version.txt does not parse: %q", strings.TrimSpace(string(b)))
	}
	return v
}

func headVersion(t *testing.T, root string) string {
	t.Helper()
	return strings.TrimSpace(gitShow(t, root, "version.txt"))
}

// gitShow reads a path as of HEAD. The path is repo-root-relative because that is what
// `git show HEAD:` speaks; window_test.go already shells out to git, so this is the
// established way a hub test looks at committed state. It skips rather than fails when the
// checkout has no git behind it, so a source tarball still runs the suite.
func gitShow(t *testing.T, root, rel string) string {
	t.Helper()
	abs, err := filepath.Abs(root)
	if err != nil {
		t.Skipf("cannot resolve the repo root: %v", err)
	}
	cmd := exec.Command("git", "show", "HEAD:"+rel)
	cmd.Dir = abs
	out, err := cmd.Output()
	if err != nil {
		t.Skipf("no git to read HEAD:%s: %v", rel, err)
	}
	return string(out)
}

// TestOldFirmwareIsRefusedBeforeTheUpload is the gate that would have saved the minute of WiFi
// this session cost. A board reporting a firmware below installMinFW cannot read the library's
// containers, so the run must say so out of the status read it already does and never post the
// file. The version floor is a proxy — the status block carries no format field to ask — so
// the proxy is only worth anything if it acts before the bytes, which is what this pins.
func TestOldFirmwareIsRefusedBeforeTheUpload(t *testing.T) {
	var sawUpload bool
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/api/v1/sd/app" {
			sawUpload = true
		}
		switch r.URL.Path {
		case "/api/v1/status":
			// A real FORMAT 2 stamp, one patch behind the floor, as the board on the bench said.
			w.Write([]byte(`{"state":"associated","ip":"127.0.0.1","fw":"v1.1.27"}`))
		case "/api/v1/apps":
			// Mounted, and empty until the upload lands: with the gate in place this never
			// fills, and with the gate removed the run completes on the row rather than
			// sitting on its 90-second hard stop, so the mutant fails on an assertion
			// instead of a timeout.
			if sawUpload {
				w.Write([]byte(`{"mounted":true,"apps":[],"installed":[{"slug":"clock","enabled":true}]}`))
				return
			}
			w.Write([]byte(`{"mounted":true,"apps":[],"installed":[]}`))
		default:
			w.Write([]byte(`{}`))
		}
	}))
	defer srv.Close()

	s := newHub()
	s.lib = &Library{AppsDir: "apps"}
	s.device = orbnet.NewClient(deviceTimeout)
	s.device.DevicePort = portOf(srv.URL)
	s.ota = orbnet.NewClient(otaTimeout)
	s.ota.DevicePort = portOf(srv.URL)

	err := s.installRun(context.Background(), "127.0.0.1", "clock", false, false, func(installStep) {})
	if err == nil {
		t.Fatal("a FORMAT 2 board passed the firmware floor")
	}
	if !strings.Contains(err.Error(), installMinFW) {
		t.Errorf("the refusal does not name the firmware to flash: %s", err)
	}
	if !strings.Contains(err.Error(), "flash") {
		t.Errorf("the refusal states a fact without the next action: %s", err)
	}
	if sawUpload {
		t.Error("the hub pushed the container anyway; the floor exists to stop that before the wire")
	}
}

// TestAppUploadRidesTheBulkClient is the regression for the refusal this session was filed
// with: "writing clock to the card: orbnet: 192.168.2.32:80 unreachable: Post
// \"http://...\": context deadline exceeded". h_app_upload answers only after it has written
// the whole container to the card, so dialing it on the 8-second management client killed a
// healthy board's upload and reported the board as the fault. The theme upload beside it was
// already on the bulk client; this pins that the app upload is too.
//
// It is proved structurally rather than by sleeping past a timeout: two fake devices listen,
// the management client is aimed at one and the bulk client at the other, and the test reads
// which one the upload actually reached.
func TestAppUploadRidesTheBulkClient(t *testing.T) {
	blob := cloneFixture(t, t.TempDir(), "sentinel")
	body, err := os.ReadFile(blob)
	if err != nil {
		t.Fatal(err)
	}

	// ledger answers the reads installOneApp does first (the app list, then the install and
	// the enable) and records nothing about the upload; bulk records the upload. A hub that
	// still dialed the upload on the management client leaves bulk empty.
	var sawUploadOnLedger, sawUploadOnBulk bool
	// installed flips when the install is queued, so the ledger answers the poll the way a
	// real board does instead of making the run sit on its 90-second hard stop.
	var installedNow bool
	handler := func(hit *bool) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if r.URL.Path == "/api/v1/sd/app" {
				*hit = true
				io.Copy(io.Discard, r.Body)
				w.Header().Set("Content-Type", "application/json")
				w.Write([]byte(`{"ok":true}`))
				return
			}
			switch r.URL.Path {
			case "/api/v1/apps":
				if installedNow {
					w.Write([]byte(`{"apps":[],"installed":[{"slug":"sentinel","enabled":true}]}`))
					return
				}
				w.Write([]byte(`{"apps":[],"installed":[]}`))
			case "/api/v1/apps/install":
				installedNow = true
				io.Copy(io.Discard, r.Body)
				w.Write([]byte(`{"ok":true,"queued":true}`))
			case "/api/v1/apps/enable":
				w.Write([]byte(`{"ok":true}`))
			default:
				w.Write([]byte(`{}`))
			}
		})
	}
	ledger := httptest.NewServer(handler(&sawUploadOnLedger))
	defer ledger.Close()
	bulk := httptest.NewServer(handler(&sawUploadOnBulk))
	defer bulk.Close()

	s := newHub()
	s.lib = &Library{AppsDir: filepath.Dir(blob)}
	// One fake answers the ledger reads; the other must be the only thing that sees the
	// container. DevicePort is per-client, which is what makes the two separable.
	s.device = orbnet.NewClient(deviceTimeout)
	s.ota = orbnet.NewClient(otaTimeout)
	s.ota.DevicePort = portOf(bulk.URL)
	s.device.DevicePort = portOf(ledger.URL)

	var steps []installStep
	send := func(st installStep) { steps = append(steps, st) }
	if err := s.installOneApp(context.Background(), "127.0.0.1", "sentinel", body, false, send); err != nil {
		t.Fatalf("the install run refused: %v", err)
	}
	if sawUploadOnLedger {
		t.Error("the app upload went out on the 8-second management client")
	}
	if !sawUploadOnBulk {
		t.Fatal("no upload reached the bulk client at all")
	}

	// The same run must stream the byte progress that fills a bar. An upload step is the
	// modal's only mid-transfer word, and a run that went silent until the end is the
	// complaint that came in with the timeout.
	sawProgress := false
	for _, st := range steps {
		if st.Action == "uploading" {
			sawProgress = true
		}
	}
	if !sawProgress {
		t.Errorf("the run streamed no `uploading` step, so a press has nothing to show while "+
			"the container crosses the room: %v", steps)
	}
}

// TestInstallOneAppReEnablesADisabledRow pins the leg the library row's Install door is
// relied on for a board whose app sits in a slot with the enable word off -- P4a's default
// for a fresh install, and the state a card-scan re-adoption lands in. The row must answer
// with one enable POST and nothing else: no card write (the bulk client stays silent), no
// install queue, no reboot. The UI offers Install for exactly this state -- the toggle is
// gone by ruling, so if this leg ever stopped writing the enable word, the switched-off row
// would become the dead-end the bench hit today with a v1.1.28 clock and no door back on.
func TestInstallOneAppReEnablesADisabledRow(t *testing.T) {
	blob := cloneFixture(t, t.TempDir(), "sentinel")
	body, err := os.ReadFile(blob)
	if err != nil {
		t.Fatal(err)
	}
	var sawEnable, sawInstall, sawUploadOnBulk bool
	handler := func(isEnable, isInstall *bool) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/api/v1/apps":
				// The ledger's answer under test: the row IS on the dial, switched off.
				w.Write([]byte(`{"apps":[],"installed":[{"slug":"sentinel","enabled":false}]}`))
			case "/api/v1/apps/enable":
				*isEnable = true
				io.Copy(io.Discard, r.Body)
				w.Write([]byte(`{"ok":true}`))
			case "/api/v1/apps/install":
				*isInstall = true
				io.Copy(io.Discard, r.Body)
				w.Write([]byte(`{"ok":true,"queued":true}`))
			case "/api/v1/sd/app":
				// An upload reaching this fake means the card was rewritten.
				sawUploadOnBulk = true
				io.Copy(io.Discard, r.Body)
				w.Write([]byte(`{"ok":true}`))
			default:
				w.Write([]byte(`{}`))
			}
		})
	}
	ledger := httptest.NewServer(handler(&sawEnable, &sawInstall))
	defer ledger.Close()
	s := newHub()
	s.lib = &Library{AppsDir: filepath.Dir(blob)}
	s.device = orbnet.NewClient(deviceTimeout)
	s.ota = orbnet.NewClient(otaTimeout)
	s.ota.DevicePort = portOf(ledger.URL)
	s.device.DevicePort = portOf(ledger.URL)

	var steps []installStep
	send := func(st installStep) { steps = append(steps, st) }
	if err := s.installOneApp(context.Background(), "127.0.0.1", "sentinel", body, false, send); err != nil {
		t.Fatalf("the enable run refused: %v", err)
	}
	if !sawEnable {
		t.Error("installOneApp left a switched-off row off, so the Install press rewrote nothing and enabled nothing")
	}
	if sawInstall {
		t.Error("installOneApp queued a card install for a row already in a slot")
	}
	if sawUploadOnBulk {
		t.Error("installOneApp rewrote the container for a row already in a slot")
	}
	sawEnabledStep := false
	sawSkipStep := false
	for _, st := range steps {
		if st.Action == "enabled" {
			sawEnabledStep = true
		}
		if st.Action == "skipped" {
			sawSkipStep = true
		}
	}
	if !sawSkipStep || !sawEnabledStep {
		t.Errorf("the run streamed %v; the panel must say skipped then enabled", steps)
	}
}

// TestPlainDeviceErrRefusesToShowGoPlumbing is the second half of the same complaint. The
// sentence the operator reads must not carry a transport name, a URL, or Go's own error
// value — the reported text was "Post \"http://192.168.2.32:80/api/v1/sd/app?overwrite=1\":
// context deadline exceeded (Client.Timeout exceeded while awaiting headers)".
func TestPlainDeviceErrRefusesToShowGoPlumbing(t *testing.T) {
	// The exact shape orbnet raises: its own wrapper, a url.Error inside, a timeout inside
	// that. Built here rather than caught from a live dial so the assertion is about wording.
	_, err := orbnet.NewClient(10 * time.Millisecond).Status("127.0.0.1:1")
	if err == nil {
		t.Skip("no local listener to refuse the dial; wording covered by the cases below")
	}
	out := plainDeviceErr(err, 0).Error()
	for _, banned := range []string{"Post \"", "context deadline", "Client.Timeout", "awaiting headers", "orbnet:"} {
		if strings.Contains(out, banned) {
			t.Errorf("the refusal still carries %q: %s", banned, out)
		}
	}
	// A timeout with a known byte count says the useful thing: the write often finishes
	// anyway, so the next move is to read the ledger rather than re-send 660 KB.
	msg := plainDeviceErr(fmt.Errorf("orbnet: 1.2.3.4:80 unreachable: Post \"http://1.2.3.4/api/v1/sd/app\": "+
		"context deadline exceeded (Client.Timeout exceeded while awaiting headers)"), 300*1024).Error()
	for _, banned := range []string{"Post \"", "context deadline", "Client.Timeout", "awaiting headers"} {
		if strings.Contains(msg, banned) {
			t.Errorf("the upload refusal still carries %q: %s", banned, msg)
		}
	}
	if !strings.Contains(msg, "300 KB") || !strings.Contains(msg, "before pressing Install again") {
		t.Errorf("the upload refusal does not state the bytes sent and the next move: %s", msg)
	}
}

// portOf strips the scheme and host from an httptest address, leaving the port — the half a
// Client.DevicePort wants.
func portOf(addr string) string {
	i := strings.LastIndex(addr, ":")
	if i < 0 {
		return addr
	}
	return addr[i+1:]
}

// h_libraryRm is the only door that deletes a hub-side file, and it is the newest
// destructive route on a hub that runs on a home network. These cases are the whole reason
// it can exist: the name arrives as a query parameter, and the handler rebuilds the path.
func TestLibraryDropTakesOnePlainNameAndNothingElse(t *testing.T) {
	dir := t.TempDir()
	lib := &Library{AppsDir: dir, ThemesDir: dir}
	write := func(n string) {
		if err := os.WriteFile(filepath.Join(dir, n), []byte("bytes"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("clock.orbapp")
	write("night.orbtheme")

	for _, bad := range []string{"", "../outside", "a/b", "..", ".", "Clock", "clock.txt", "clock.orbapp", "/etc/passwd"} {
		if _, err := lib.Drop("app", bad); err == nil {
			t.Errorf("Drop(app, %q) succeeded; a name that is not one plain slug must be refused", bad)
		}
	}
	// The theme side takes the device's filename grammar, which admits upper case and an
	// interior dot. The shipped seed's stem ("aviator") is plain, and a gate narrower than
	// orb_sd_name_ok would have the hub refuse a file it lists.
	for _, good := range []string{"aviator", "Night", "a.b", "a-b_c"} {
		if _, err := lib.Drop("theme", good); err == nil ||
			!strings.Contains(err.Error(), "not in the library") {
			t.Errorf("Drop(theme, %q) = %v, want the absent-file refusal rather than a name refusal", good, err)
		}
	}
	for _, bad := range []string{"../etc/passwd", "a/b", ".hidden", "night.orbtheme", ""} {
		if _, err := lib.Drop("theme", bad); err == nil {
			t.Errorf("Drop(theme, %q) succeeded; traversal and a doubled extension must be refused", bad)
		}
	}
	if _, err := os.Stat(filepath.Join(dir, "clock.orbapp")); err != nil {
		t.Errorf("a refused name deleted the library file: %v", err)
	}
	if _, err := lib.Drop("bogus", "clock"); err == nil {
		t.Error("Drop accepted a kind that is not app or theme")
	}
	// A name the library does not hold is an error naming that fact, not a silent success:
	// a face that reports "removed" over a file that is still on disk is the lie this whole
	// change exists to stop telling.
	if _, err := lib.Drop("app", "nosuch"); err == nil || !strings.Contains(err.Error(), "not in the library") {
		t.Errorf("Drop of an absent file = %v, want a refusal naming the library", err)
	}
	if got, err := lib.Drop("app", "clock"); err != nil {
		t.Errorf("Drop(app, clock) = %v", err)
	} else if !strings.HasSuffix(got, "clock.orbapp") {
		t.Errorf("Drop returned %q", got)
	}
	if _, err := os.Stat(filepath.Join(dir, "clock.orbapp")); !os.IsNotExist(err) {
		t.Error("the file is still on disk after a successful Drop")
	}
	if _, err := lib.Drop("theme", "night"); err != nil {
		t.Errorf("Drop(theme, night) = %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "night.orbtheme")); !os.IsNotExist(err) {
		t.Error("the theme side did not delete")
	}
}

// The route is the one thing standing between a page and the hub's disk, so it is gated
// like every other mutating door and it refuses a traversal name before it touches a
// file. Mirrors TestInstallRouteIsWiredAndGated's shape: routes on a mux, no board dialed.
func TestLibraryRmRouteIsWiredAndGated(t *testing.T) {
	s := newHub()
	s.lib = &Library{}
	mux := http.NewServeMux()
	s.routes(mux)

	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/api/library/rm?kind=app&name=clock", nil))
	if rec.Code == http.StatusOK {
		t.Error("GET deleted something; the route is POST-only")
	}
	req := httptest.NewRequest(http.MethodPost, "/api/library/rm?kind=app&name=clock", nil)
	rec = httptest.NewRecorder()
	mux.ServeHTTP(rec, req)
	if rec.Code != http.StatusForbidden {
		t.Errorf("a tokenless POST = %d, want 403 before the hub touches a file", rec.Code)
	}
	req = httptest.NewRequest(http.MethodPost, "/api/library/rm?kind=app&name=../../etc/passwd", nil)
	req.Header.Set(tokenHeader, s.token)
	rec = httptest.NewRecorder()
	mux.ServeHTTP(rec, req)
	if rec.Code == http.StatusOK {
		t.Error("the route accepted a traversal name")
	}
}

// KeepOver is how a newer build reaches a hub now that import no longer installs. The
// default stays refuse-on-exists: an overwrite has to be asked for by name.
func TestLibraryKeepOverOnlyReplacesWhenAsked(t *testing.T) {
	dir := t.TempDir()
	lib := &Library{AppsDir: dir}
	if _, err := lib.KeepOver("clock", []byte("first"), false); err != nil {
		t.Fatal(err)
	}
	if _, err := lib.KeepOver("clock", []byte("second"), false); err == nil {
		t.Error("KeepOver replaced an existing file without being asked to")
	}
	if _, err := lib.KeepOver("clock", []byte("second"), true); err != nil {
		t.Errorf("KeepOver(overwrite=true) = %v", err)
	}
	b, _ := os.ReadFile(filepath.Join(dir, "clock.orbapp"))
	if string(b) != "second" {
		t.Errorf("the shelf holds %q after an overwrite", b)
	}
}
