package main

import (
	"io/fs"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"testing/fstest"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// Two shipped containers stand in for two releases of one app: the clock's and the
// picker's stamps, ordered by the stamps themselves (whichever app last bumped is the
// newer), and the seed compares only their version stamps.
func seedApps(t *testing.T) (newer, older []byte) {
	t.Helper()
	a, err1 := os.ReadFile(filepath.Join("apps", "clock.orbapp"))
	b, err2 := os.ReadFile(filepath.Join("apps", "color_picker.orbapp"))
	if err1 != nil || err2 != nil {
		t.Skipf("no shipped apps to seed from: %v %v", err1, err2)
	}
	ca, errA := orbnet.ParseApp(a)
	cb, errB := orbnet.ParseApp(b)
	if errA != nil || errB != nil {
		t.Fatalf("shipped apps do not parse: %v %v", errA, errB)
	}
	if c, ok := orbnet.AppVersionCompare(ca.Version, cb.Version); !ok || c == 0 {
		// Every shipped app at one release (v1.6.0): the older one is the color picker as
		// it shipped before, kept in testdata for exactly this.
		b, err2 = os.ReadFile(filepath.Join("testdata", "color_picker-v1.3.1.orbapp"))
		if err2 != nil {
			t.Fatalf("the older app fixture: %v", err2)
		}
		if cb, errB = orbnet.ParseApp(b); errB != nil {
			t.Fatalf("the older app fixture does not parse: %v", errB)
		}
	}
	switch c, ok := orbnet.AppVersionCompare(ca.Version, cb.Version); {
	case !ok || c == 0:
		t.Fatalf("clock %q and color_picker %q do not order", ca.Version, cb.Version)
	case c < 0:
		return b, a
	}
	return a, b
}

// The shipped clock, for the Restore tests: they name the slug, not a release order.
func shippedClock(t *testing.T) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("apps", "clock.orbapp"))
	if err != nil {
		t.Skipf("no shipped clock to restore: %v", err)
	}
	return b
}

func shelf(app, theme []byte) fs.FS {
	return fstest.MapFS{
		"apps/clock.orbapp":        {Data: app},
		"themes/skeleton.orbtheme": {Data: theme},
	}
}

func seedAll(t *testing.T, src fs.FS, user string) []string {
	t.Helper()
	lines, err := seedUserLibrary(src, user, []string{libAppsSubdir, libThemesSubdir})
	if err != nil {
		t.Fatal(err)
	}
	return lines
}

func readUser(user string, p ...string) (string, bool) {
	b, err := os.ReadFile(filepath.Join(append([]string{user}, p...)...))
	return string(b), err == nil
}

// The compiled-in shelf is what a release seeds from, and it carries the shipped files.
func TestShippedLibIsCompiledIn(t *testing.T) {
	for _, p := range []string{"apps/clock.orbapp", "themes/skeleton.orbtheme"} {
		if b, err := fs.ReadFile(shippedLib, p); err != nil || len(b) == 0 {
			t.Errorf("%s is not compiled in: %v", p, err)
		}
	}
}

func TestSeedFillsAnEmptyLibraryOnce(t *testing.T) {
	newer, _ := seedApps(t)
	user := t.TempDir()
	if lines := seedAll(t, shelf(newer, []byte("skeleton v1")), user); len(lines) != 2 {
		t.Errorf("a first launch should seed both files, said %v", lines)
	}
	if got, _ := readUser(user, "apps", "clock.orbapp"); got != string(newer) {
		t.Errorf("the app was not seeded")
	}
	if again := seedAll(t, shelf(newer, []byte("skeleton v1")), user); len(again) != 0 {
		t.Errorf("a second launch seeded again: %v", again)
	}
}

// The owner's question: seeded, then deleted from the library, then the hub restarts.
func TestSeedNeverRespawnsADeletedFile(t *testing.T) {
	newer, _ := seedApps(t)
	user := t.TempDir()
	seedAll(t, shelf(newer, []byte("skeleton v1")), user)
	os.Remove(filepath.Join(user, "apps", "clock.orbapp"))
	os.Remove(filepath.Join(user, "themes", "skeleton.orbtheme"))
	if lines := seedAll(t, shelf(newer, []byte("skeleton v2")), user); len(lines) != 0 {
		t.Errorf("a restart respawned deleted files, even across an update: %v", lines)
	}
	if _, ok := readUser(user, "apps", "clock.orbapp"); ok {
		t.Errorf("the deleted app came back")
	}
	if _, ok := readUser(user, "themes", "skeleton.orbtheme"); ok {
		t.Errorf("the deleted theme came back")
	}
	// removing the whole library, ledger and all, is the way back to the shipped shelf
	os.RemoveAll(user)
	if lines := seedAll(t, shelf(newer, []byte("skeleton v2")), user); len(lines) != 2 {
		t.Errorf("a removed library was not reseeded: %v", lines)
	}
}

func TestSeedUpdatesOnlyUntouchedCopies(t *testing.T) {
	newer, older := seedApps(t)
	user := t.TempDir()
	seedAll(t, shelf(older, []byte("skeleton v1")), user)
	// a release with a newer app and a re-baked theme replaces both untouched copies
	seedAll(t, shelf(newer, []byte("skeleton v2")), user)
	if got, _ := readUser(user, "apps", "clock.orbapp"); got != string(newer) {
		t.Errorf("an untouched app was not updated to the newer release")
	}
	if got, _ := readUser(user, "themes", "skeleton.orbtheme"); got != "skeleton v2" {
		t.Errorf("an untouched theme was not updated, got %q", got)
	}
	// a later release that ships an OLDER app never downgrades
	seedAll(t, shelf(older, []byte("skeleton v2")), user)
	if got, _ := readUser(user, "apps", "clock.orbapp"); got != string(newer) {
		t.Errorf("an older release downgraded the app")
	}
	// the owner edits the theme; the next release leaves it alone
	os.WriteFile(filepath.Join(user, "themes", "skeleton.orbtheme"), []byte("my skeleton"), 0o644)
	seedAll(t, shelf(newer, []byte("skeleton v3")), user)
	if got, _ := readUser(user, "themes", "skeleton.orbtheme"); got != "my skeleton" {
		t.Errorf("an owner-changed theme was overwritten, got %q", got)
	}
}

// oldFormat is the shipped clock with its FORMAT word set one back, as a release before a
// format change left it on the shelf. ParseApp refuses it, which is the point.
func oldFormat(t *testing.T, b []byte) []byte {
	t.Helper()
	o := append([]byte(nil), b...)
	o[4]-- // word 1, little-endian: the FORMAT
	if _, err := orbnet.ParseApp(o); err == nil || !orbnet.AppFormatOlder(o) {
		t.Fatalf("the stand-in old container should refuse as an older FORMAT: %v", err)
	}
	return o
}

// A shelf app from before a FORMAT change cannot be read to compare versions, and the hub
// kept it: v1.2.32 over v1.2.16 left clock, HSB, Flights and Weather as FORMAT 3 files no
// v1.2.32 orb runs, with no Install or Update on their rows (owner, 2026-11-08). The
// shipped copy replaces an older FORMAT whether the hub placed it or it predates the ledger.
func TestSeedReplacesAnOlderFormat(t *testing.T) {
	cur := shippedClock(t)
	old := oldFormat(t, cur)
	user := t.TempDir()
	seedAll(t, shelf(old, []byte("skeleton")), user) // the old release placed it
	seedAll(t, shelf(cur, []byte("skeleton")), user)
	if got, _ := readUser(user, "apps", "clock.orbapp"); got != string(cur) {
		t.Errorf("a hub-placed app of an older FORMAT was not replaced")
	}
	pre := t.TempDir() // before any ledger
	os.MkdirAll(filepath.Join(pre, "apps"), 0o755)
	os.WriteFile(filepath.Join(pre, "apps", "clock.orbapp"), old, 0o644)
	seedAll(t, shelf(cur, []byte("skeleton")), pre)
	if got, _ := readUser(pre, "apps", "clock.orbapp"); got != string(cur) {
		t.Errorf("a pre-ledger app of an older FORMAT was not replaced")
	}
	// not an app at all: not the seed's to judge, it stays
	junk := t.TempDir()
	os.MkdirAll(filepath.Join(junk, "apps"), 0o755)
	os.WriteFile(filepath.Join(junk, "apps", "clock.orbapp"), []byte("not an app"), 0o644)
	seedAll(t, shelf(cur, []byte("skeleton")), junk)
	if got, _ := readUser(junk, "apps", "clock.orbapp"); got != "not an app" {
		t.Errorf("a file that is not an app was overwritten")
	}
}

func TestSeedLeavesFilesItDidNotPlace(t *testing.T) {
	newer, older := seedApps(t)
	user := t.TempDir()
	os.MkdirAll(filepath.Join(user, "themes"), 0o755)
	os.MkdirAll(filepath.Join(user, "apps"), 0o755)
	os.WriteFile(filepath.Join(user, "themes", "skeleton.orbtheme"), []byte("the owner's own"), 0o644)
	os.WriteFile(filepath.Join(user, "apps", "clock.orbapp"), older, 0o644)
	seedAll(t, shelf(newer, []byte("skeleton v1")), user)
	if got, _ := readUser(user, "themes", "skeleton.orbtheme"); got != "the owner's own" {
		t.Errorf("a theme the owner held before the seed was overwritten")
	}
	// an older shipped app that predates the ledger is still upgraded
	if got, _ := readUser(user, "apps", "clock.orbapp"); got != string(newer) {
		t.Errorf("a pre-ledger older app was not upgraded")
	}
	// and the owner's theme, once deleted, does not turn into a seeded one
	os.Remove(filepath.Join(user, "themes", "skeleton.orbtheme"))
	if lines := seedAll(t, shelf(newer, []byte("skeleton v1")), user); len(lines) != 0 {
		t.Errorf("a deleted owner file was replaced by the shipped one: %v", lines)
	}
}

// Only the user-config rung (or no library at all) is seeded: a flag, a library beside the
// executable and a clone keep the hub's hands off the user config dir.
func TestSeedKindsFollowTheLadder(t *testing.T) {
	for _, c := range []struct {
		apps, themes string
		want         int
	}{
		{"none found", "none found", 2},
		{"the user config dir", "none found", 2},
		{"beside the executable", "beside the executable", 0},
		{"the clone", "the clone", 0},
		{"-library flag", "the user config dir", 1},
	} {
		if got := seedKinds(&Library{AppsSource: c.apps, ThemesSource: c.themes}); len(got) != c.want {
			t.Errorf("%s/%s: seeded %v, want %d kinds", c.apps, c.themes, got, c.want)
		}
	}
}

// The Restore press offers a shipped app the library lacks or holds only older; a copy at
// the same or a newer version, or one whose version cannot be compared, stays the owner's.
func TestRestorableAppsOffersOnlyMissingOrOlder(t *testing.T) {
	newer := shippedClock(t)
	src := shelf(newer, nil)
	c0, err := orbnet.ParseApp(newer)
	if err != nil {
		t.Fatal(err)
	}
	for _, c := range []struct {
		have []LibraryFile
		want string // "" = not offered, "-" = offered as missing, else the Have version
	}{
		{nil, "-"},
		{[]LibraryFile{{Slug: "clock", Version: "v1.0.0"}}, "v1.0.0"},
		{[]LibraryFile{{Slug: "clock", Version: c0.Version}}, ""},
		{[]LibraryFile{{Slug: "clock", Version: "v9.0.0"}}, ""},
		{[]LibraryFile{{Slug: "clock", Version: ""}}, ""},
	} {
		got := restorableApps(src, c.have)
		switch {
		case c.want == "" && len(got) != 0:
			t.Errorf("have %v: offered %v, want nothing", c.have, got)
		case c.want == "-" && (len(got) != 1 || got[0].Slug != "clock" || got[0].Have != ""):
			t.Errorf("have nothing: offered %v, want the clock as missing", got)
		case c.want != "" && c.want != "-" && (len(got) != 1 || got[0].Have != c.want):
			t.Errorf("have %v: offered %v, want the clock over %s", c.have, got, c.want)
		}
	}
}

func TestRestoreShippedAppsRefilesAndRecords(t *testing.T) {
	newer := shippedClock(t)
	src := shelf(newer, []byte("skeleton"))
	user := t.TempDir()
	seedAll(t, src, user)
	apps := filepath.Join(user, "apps")
	lib := &Library{AppsDir: apps}

	// deleted, then restored: the file is back and the ledger knows it is shipped again,
	// so the next launch neither respawns-on-delete wrongly nor treats it as the owner's
	os.Remove(filepath.Join(apps, "clock.orbapp"))
	got, err := restoreShippedApps(src, lib, user)
	if err != nil || len(got) != 1 || got[0] != "clock" {
		t.Fatalf("restore = %v, %v; want [clock]", got, err)
	}
	if b, _ := os.ReadFile(filepath.Join(apps, "clock.orbapp")); string(b) != string(newer) {
		t.Errorf("the restored file is not the shipped one")
	}
	led, _ := loadSeedLedger(user)
	if led["apps/clock.orbapp"] != sha256Hex(newer) {
		t.Errorf("the ledger does not record the restored copy as seeded: %q", led["apps/clock.orbapp"])
	}
	// nothing left to restore: a second press files nothing
	if again, _ := restoreShippedApps(src, lib, user); len(again) != 0 {
		t.Errorf("a second restore filed %v", again)
	}
	// a broken copy is replaced
	os.WriteFile(filepath.Join(apps, "clock.orbapp"), []byte("not a container"), 0o644)
	if got, _ := restoreShippedApps(src, lib, user); len(got) != 1 {
		t.Errorf("a broken copy was not replaced: %v", got)
	}
	// the restored file is not touched by the next launch's seed
	if lines := seedAll(t, src, user); len(lines) != 0 {
		t.Errorf("the seed acted on a restored file: %v", lines)
	}
}

func TestLibraryRestoreRouteIsWiredAndGated(t *testing.T) {
	seedApps(t)
	s := newHub()
	dir := t.TempDir()
	s.lib = &Library{AppsDir: dir}
	mux := http.NewServeMux()
	s.routes(mux)

	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/api/library/restore", nil))
	if rec.Code == http.StatusOK {
		t.Error("GET restored something; the route is POST-only")
	}
	rec = httptest.NewRecorder()
	mux.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/api/library/restore", nil))
	if rec.Code != http.StatusForbidden {
		t.Errorf("a tokenless POST = %d, want 403 before the hub writes a file", rec.Code)
	}
	// the listing offers every shipped app to an empty library
	rec = httptest.NewRecorder()
	mux.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/api/library", nil))
	if !strings.Contains(rec.Body.String(), `"restorable":[{"slug":"clock"`) {
		t.Errorf("the listing does not offer the clock: %s", rec.Body.String())
	}
	rec = httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/library/restore", nil)
	req.Header.Set(tokenHeader, s.token)
	req.Header.Set("Sec-Fetch-Site", "same-origin")
	mux.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"clock"`) {
		t.Fatalf("restore = %d %s", rec.Code, rec.Body.String())
	}
	if _, err := os.Stat(filepath.Join(dir, "clock.orbapp")); err != nil {
		t.Errorf("the clock was not filed: %v", err)
	}
	rec = httptest.NewRecorder()
	mux.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/api/library", nil))
	if !strings.Contains(rec.Body.String(), `"restorable":[]`) {
		t.Errorf("after a restore the listing still offers: %s", rec.Body.String())
	}
}
