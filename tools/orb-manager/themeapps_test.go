package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// D37's enumeration tests. The old Themes face derived its app chooser from the
// theme rows already on the card, so an installed, enabled, theme-capable app with
// no theme written yet had no pane and looked theme-blind. Everything here drives
// the route against the double with exactly that app present.

// Owner caps at their REAL bit positions (orb_cap_t: CAP_POWER=4 .. CAP_THEMES=7).
// Bits 0..3 are the free caps and the firmware never prints them, so a fixture that
// wrote 0x8 for "themes" would be testing a table the device does not use.
const (
	capPower  = 1 << 4
	capSdRead = 1 << 6
	capThemes = 1 << 7
)

type themeAppsRow struct {
	Slug       string   `json:"slug"`
	Title      string   `json:"title"`
	Caps       string   `json:"caps"`
	Count      int      `json:"count"`
	Active     bool     `json:"active"`
	HasEditor  bool     `json:"has_editor"`
	EditorURL  string   `json:"editor_url"`
	Drift      []string `json:"drift"`
	Comparable bool     `json:"comparable"`
}

type themeAppsOut struct {
	OK       bool           `json:"ok"`
	Error    string         `json:"error"`
	Apps     []themeAppsRow `json:"apps"`
	Mounted  bool           `json:"mounted"`
	AppErr   string         `json:"app_error"`
	ThemeErr string         `json:"theme_error"`
}

// getThemeApps dials the route with a token, the way the page does, and returns the
// envelope's data with the status it came back at.
func getThemeApps(t *testing.T, s *Server, ip string) (int, themeAppsOut) {
	t.Helper()
	mux := http.NewServeMux()
	s.routes(mux)
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/api/theme/apps?ip="+ip, nil)
	req.Header.Set(tokenHeader, s.token)
	mux.ServeHTTP(rec, req)
	var env struct {
		OK    bool            `json:"ok"`
		Error string          `json:"error"`
		Data  json.RawMessage `json:"data"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
		t.Fatalf("body is not the {ok,...} envelope: %v (%s)", err, rec.Body)
	}
	out := themeAppsOut{OK: env.OK, Error: env.Error}
	if len(env.Data) > 0 {
		if err := json.Unmarshal(env.Data, &out); err != nil {
			t.Fatalf("data is not the enumeration: %v (%s)", err, env.Data)
		}
	}
	return rec.Code, out
}

func appRow(rows []themeAppsRow, slug string) *themeAppsRow {
	for i := range rows {
		if rows[i].Slug == slug {
			return &rows[i]
		}
	}
	return nil
}

// A theme-aware app with nothing on the card still gets a row, with an explicit
// zero — the invisibility this slice exists to remove.
func TestThemeAppsListsThemeCapableAppWithZeroThemes(t *testing.T) {
	orb := orbfake.New(
		orbfake.Theme{Name: "aurora", Bytes: 99, Serial: 1, OK: true, Active: true},
	).WithCardApps(
		orbfake.CardApp{Stem: "hue", Slug: "hue", Title: "Hue", Caps: capThemes, Installed: true, Enabled: true},
	)
	s, ip, _, _ := fakeOrbHubWith(t, orb)
	code, got := getThemeApps(t, s, ip)
	if code != http.StatusOK || !got.OK {
		t.Fatalf("route refused a valid enumeration: %d %s", code, got.Error)
	}
	row := appRow(got.Apps, "hue")
	if row == nil {
		t.Fatalf("an installed, enabled app declaring `themes` is missing: %+v", got.Apps)
	}
	if row.Count != 0 {
		t.Errorf("count = %d, want 0; the app owns no themes and must say so rather than vanish", row.Count)
	}
	if row.Title != "Hue" {
		t.Errorf("title = %q, want the ledger's own %q", row.Title, "Hue")
	}
	if row.HasEditor {
		t.Error("an app the hub has no editor for reported has_editor, so the face would draw a dead button")
	}
}

// The cap filter is the whole point: an app that does not declare `themes` must not
// appear, and a theme-capable app that is installed but switched off must still
// appear rather than hide the themes on its card.
func TestThemeAppsFiltersByCapAndKeepsDisabled(t *testing.T) {
	orb := orbfake.New(
		orbfake.Theme{Name: "aurora", Bytes: 99, Serial: 1, OK: true, Active: true},
	).WithCardApps(
		orbfake.CardApp{Stem: "hue", Slug: "hue", Title: "Hue", Caps: capThemes, Installed: true, Enabled: true},
		orbfake.CardApp{Stem: "lamp", Slug: "lamp", Title: "Lamp", Caps: capPower, Installed: true, Enabled: true},
		orbfake.CardApp{Stem: "thermo", Slug: "thermo", Title: "Thermo", Caps: capSdRead | capThemes, Installed: true, Enabled: false},
	)
	s, ip, _, _ := fakeOrbHubWith(t, orb)
	_, got := getThemeApps(t, s, ip)
	if appRow(got.Apps, "lamp") != nil {
		t.Error("a power-only app appeared in the theme enumeration")
	}
	if appRow(got.Apps, "hue") == nil {
		t.Error("the theme-capable app is missing")
	}
	if appRow(got.Apps, "thermo") == nil {
		t.Error("a DISABLED theme-capable app vanished; its card themes would be unreachable, " +
			"and D37 keeps the pane so an author can still work")
	}
}

// A theme filed under an app the ledger does not name must stay reachable: real
// bytes on a real card, deletable from the pane that lists them.
func TestThemeAppsShowsOrphanCardGroup(t *testing.T) {
	orb := orbfake.New(
		orbfake.Theme{Name: "stray", App: "ghost", Bytes: 44, Serial: 1, OK: true},
	).WithCardApps(
		orbfake.CardApp{Stem: "hue", Slug: "hue", Title: "Hue", Caps: capThemes, Installed: true, Enabled: true},
	)
	s, ip, _, _ := fakeOrbHubWith(t, orb)
	_, got := getThemeApps(t, s, ip)
	row := appRow(got.Apps, "ghost")
	if row == nil {
		t.Fatalf("a theme filed under an unlisted app vanished from the face: %+v", got.Apps)
	}
	if row.Count != 1 {
		t.Errorf("orphan count = %d, want 1", row.Count)
	}
	if row.HasEditor {
		t.Error("an orphan group reported an editor, with no app behind it to ship one")
	}
}

// The double's cap table must be the FIRMWARE's, not a plausible-looking one.
// D37's enumeration keys off the literal word "themes", so a double that printed
// owner caps from bits 0..3 would let the hub pass a suite while every real board
// said something else — the exact generated-from-the-client trap these suites exist
// to close. Pinned against orb_svc.h and orb_app_manifest.c, read from the source.
func TestFakeCapTableMatchesFirmware(t *testing.T) {
	svc := firmwareFile(t, filepath.Join("main", "app", "orb_svc.h"))
	man := firmwareFile(t, filepath.Join("main", "app", "orb_app_manifest.c"))
	for _, want := range []string{"CAP_POWER = 4", "CAP_THEMES = 7"} {
		if !strings.Contains(svc, want) {
			t.Errorf("orb_svc.h no longer says %q; the fake's cap bits need re-deriving by hand", want)
		}
	}
	if !strings.Contains(man, "1u << (CAP_POWER + i)") {
		t.Error("orbapp_caps_names no longer indexes owner caps off CAP_POWER; the fake's loop is wrong")
	}
	// The fake must iterate the same base the firmware does.
	fake := hubSourceFile(t, filepath.Join("internal", "orbfake", "apps.go"))
	if !strings.Contains(fake, "1<<uint(4+i)") {
		t.Error("orbfake's capsNames is back to bits 0..3: it would render \"time\" as \"power\"")
	}
}

// hubSourceFile reads a file from this module's own tree, so a pin can name the
// hub's source the way firmwareFile names the firmware's. Tests run in the package
// directory, so the path is relative to it; a missing file skips rather than fails.
func hubSourceFile(t *testing.T, rel string) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(".", rel))
	if err != nil {
		t.Skipf("no hub source at %s: %v", rel, err)
	}
	return string(b)
}

// The clock is the one app the hub can serve an editor for before GEOM_ABI 3 lands,
// and it is the HUB saying so: the web layer must not carry that slug itself.
func TestThemeAppsClockEditorComesFromTheHub(t *testing.T) {
	orb := orbfake.New().WithCardApps(
		orbfake.CardApp{Stem: "clock", Slug: "clock", Title: "Clock", Caps: capThemes, Installed: true, Enabled: true},
	)
	s, ip, _, _ := fakeOrbHubWith(t, orb)
	_, got := getThemeApps(t, s, ip)
	row := appRow(got.Apps, "clock")
	if row == nil {
		for _, a := range got.Apps {
			t.Logf("enumeration named %s caps=%q", a.Slug, a.Caps)
		}
		t.Fatal("clock missing from the enumeration")
	}
	if !row.HasEditor || row.EditorURL == "" {
		t.Fatalf("clock reported no editor, hiding the one door that works: %+v", row)
	}
	if !strings.HasPrefix(row.EditorURL, "/") {
		t.Errorf("editor_url = %q, want a same-origin hub path", row.EditorURL)
	}
}

// The learned-address gate guards a dial like every other theme call, and a
// hostname is refused rather than resolved. The route reads a query rather than a
// body, so it gets its own sweep beside the one the POST routes share.
func TestThemeAppsRefusesAnUnlearnedAddress(t *testing.T) {
	for _, ip := range []string{"203.0.113.9", "orb.local", "10.0.0.1:80", "[::1]", ""} {
		s, _, called, _ := fakeOrbHub(t)
		code, got := getThemeApps(t, s, ip)
		if code == http.StatusOK || got.OK {
			t.Errorf("%q was accepted as an orb address: %d %v", ip, code, got)
		}
		if c := called(); c.path != "" {
			t.Errorf("%q dialled the orb anyway: %+v", ip, c)
		}
	}
}

// D37's disclosure: an app whose CONTAINER carries a region gets the pane pointed at the
// app's own bytes, not at anything the hub happens to host. hue has no hub-hosted editor,
// so before the region existed this app had no pane at all even with an editor on board.
func TestThemeAppsOpensTheContainersEditorWhenTheDeviceReportsOne(t *testing.T) {
	orb := orbfake.New(orbfake.Theme{Name: "aurora", Bytes: 99, Serial: 1, OK: true}).
		WithCardApps(orbfake.CardApp{Stem: "hue", Slug: "hue", Title: "Hue", Caps: capThemes,
			Installed: true, Enabled: true, Serial: 7, GeomCRC: 0xabcd, Editor: []byte("zip")})
	s, ip, _, _ := fakeOrbHubWith(t, orb)
	_, got := getThemeApps(t, s, ip)
	row := appRow(got.Apps, "hue")
	if row == nil {
		t.Fatal("theme-capable app missing from the enumeration")
	}
	if !row.HasEditor || row.EditorURL != "/editor/hue/" {
		t.Errorf("an app reporting a region got has_editor=%v url=%q, want the container route",
			row.HasEditor, row.EditorURL)
	}
}

// The other direction, and the one that could regress the shipped clock: a container that
// reports NO region does not take away an editor the hub hosts. The clock's editor moves
// into its container in S4; until then its container is empty of one and /composer/ is the
// truth, so a dial saying editor_crc=0 must leave the pane open rather than blank it.
func TestThemeAppsKeepsTheHostedEditorWhenTheContainerCarriesNone(t *testing.T) {
	orb := orbfake.New().WithCardApps(
		orbfake.CardApp{Stem: "clock", Slug: "clock", Title: "Clock", Caps: capThemes,
			Installed: true, Enabled: true, Serial: 3, GeomCRC: 0x1234})
	s, ip, _, _ := fakeOrbHubWith(t, orb)
	_, got := getThemeApps(t, s, ip)
	row := appRow(got.Apps, "clock")
	if row == nil {
		t.Fatal("clock missing")
	}
	if !row.HasEditor || row.EditorURL != "/composer/" {
		t.Errorf("a clock whose container holds no region got has_editor=%v url=%q; the hub still serves its editor",
			row.HasEditor, row.EditorURL)
	}
}

// Drift is the reason the tuple reaches the hub at all. Against the seed's own container
// the seeded clock compares; against an orb a half-version behind, the face has to be able
// to say which words disagree rather than "installed".
func TestThemeAppsReportsDriftAgainstTheHubsOwnContainer(t *testing.T) {
	// Slug must match: the comparison refuses across names rather than reporting a
	// difference nobody could resolve by re-pushing a file.
	orb := orbfake.New().WithCardApps(
		orbfake.CardApp{Stem: "sentinel", Slug: "sentinel", Title: "Sentinel", Caps: capThemes,
			Installed: true, Enabled: true, Serial: 1, GeomCRC: 0x1111,
			Editor: []byte("an editor the hub's copy does not have")})
	s, ip, _, _ := fakeOrbHubWith(t, orb)
	s.device.Containers = func(slug string) ([]byte, error) {
		if slug != "sentinel" {
			return nil, nil
		}
		return appFixtureBytes(t, "sentinel.orbapp"), nil
	}
	_, got := getThemeApps(t, s, ip)
	row := appRow(got.Apps, "sentinel")
	if row == nil {
		t.Fatal("sentinel missing")
	}
	if !row.Comparable {
		t.Fatal("a row carrying a tuple and a hub container reported itself uncomparable")
	}
	joined := strings.Join(row.Drift, " ")
	// The face cannot say "installed, and that is enough": an orb one serial behind shows
	// the same row as one that is current unless the words that disagree come through.
	for _, want := range []string{"serial", "editor"} {
		if !strings.Contains(joined, want) {
			t.Errorf("drift lines %q never name %s — an operator cannot act on \"it differs\"", joined, want)
		}
	}
	if strings.Contains(joined, "slug") {
		t.Errorf("drift reported a slug problem between two rows that match by slug: %q", joined)
	}
}

// A matched pair reports no drift, so the absence of a line means agreement and not
// "nobody looked". The hub's own starter container against a row stamped from it is
// the real shipping case, and starterContainerFor is the production resolver rather
// than a test stub. S4 landed the clock's editor into that container, so the fixture
// carries the region's bytes and the fake derives editor_crc from them the way a real
// dial derives it from the card — the full six-word tuple is under comparison here.
func TestThemeAppsReportsNoDriftForAMatchingStarter(t *testing.T) {
	body, err := starterContainerFor("../..", "clock")
	if err != nil || len(body) == 0 {
		t.Skipf("no starter clock to compare against: %v", err)
	}
	c, err := orbnet.ParseApp(body)
	if err != nil {
		t.Fatalf("the hub's own starter container does not parse: %v", err)
	}
	if !c.HasEditor() {
		t.Fatal("the starter clock ships no editor region; S4 regressed — run make_factory_apps.py")
	}
	region := body[c.EditorOff : c.EditorOff+c.EditorSize]
	orb := orbfake.New().WithCardApps(
		orbfake.CardApp{Stem: "clock", Slug: "clock", Title: "Clock", Caps: capThemes,
			Installed: true, Enabled: true, GeomCRC: c.GeomCRC, CodeCRC: c.CodeCRC,
			CodeSize: c.CodeSize, Serial: c.Serial, Editor: region})
	s, ip, _, _ := fakeOrbHubWith(t, orb)
	s.device.Containers = func(slug string) ([]byte, error) { return starterContainerFor("../..", slug) }
	_, got := getThemeApps(t, s, ip)
	row := appRow(got.Apps, "clock")
	if row == nil {
		t.Fatal("clock missing")
	}
	if !row.Comparable {
		t.Fatal("the starter pair reported uncomparable")
	}
	if len(row.Drift) != 0 {
		t.Errorf("a row built from the hub's own container drifted: %q", row.Drift)
	}
	if !row.HasEditor {
		t.Error("the Themes face hid an editor the starter container plainly carries")
	}
	if row.EditorURL == "" || row.EditorURL == "/composer/" {
		t.Errorf("clock opened on %q; a device-reported region must win the hub-hosted alias", row.EditorURL)
	}
}

// No resolver, no opinion. A hub holding no original must not report a match: the field it
// would compare is a zero the decoder invented.
func TestThemeAppsSaysNothingWithoutAHubsCopy(t *testing.T) {
	orb := orbfake.New().WithCardApps(
		orbfake.CardApp{Stem: "clock", Slug: "clock", Title: "Clock", Caps: capThemes,
			Installed: true, Enabled: true, Serial: 3, GeomCRC: 0x1234})
	s, ip, _, _ := fakeOrbHubWith(t, orb)
	_, got := getThemeApps(t, s, ip)
	row := appRow(got.Apps, "clock")
	if row == nil {
		t.Fatal("clock missing")
	}
	if row.Comparable || len(row.Drift) != 0 {
		t.Errorf("a hub with no copy of the container reported comparable=%v drift=%q", row.Comparable, row.Drift)
	}
}

// appFixtureBytes reads one of the repo's .orbapp fixtures. The hub's own reader parses
// these, and the pair is pinned byte-for-byte by tools/host_app_fixture_test.py — reaching
// them from a package-main test is two directories up instead of four.
func appFixtureBytes(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("..", "..", "tests", "fixtures", name))
	if err != nil {
		t.Fatalf("no fixture %s: %v", name, err)
	}
	return b
}
