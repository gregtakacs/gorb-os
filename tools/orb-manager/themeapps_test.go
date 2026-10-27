package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
)

// D37's enumeration tests. The old Themes face derived its app chooser from the
// theme rows already on the card, so an installed, enabled, theme-capable app with
// no theme written yet had no pane and looked theme-blind. Everything here drives
// the route against the double with exactly that app present.

type themeAppsRow struct {
	Slug      string `json:"slug"`
	Title     string `json:"title"`
	Caps      string `json:"caps"`
	Count     int    `json:"count"`
	Active    bool   `json:"active"`
	HasEditor bool   `json:"has_editor"`
	EditorURL string `json:"editor_url"`
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
		orbfake.CardApp{Stem: "hue", Slug: "hue", Title: "Hue", Caps: 0x8, Installed: true, Enabled: true},
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
		orbfake.CardApp{Stem: "hue", Slug: "hue", Title: "Hue", Caps: 0x8, Installed: true, Enabled: true},
		orbfake.CardApp{Stem: "lamp", Slug: "lamp", Title: "Lamp", Caps: 0x1, Installed: true, Enabled: true},
		orbfake.CardApp{Stem: "thermo", Slug: "thermo", Title: "Thermo", Caps: 0xc, Installed: true, Enabled: false},
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
		orbfake.CardApp{Stem: "hue", Slug: "hue", Title: "Hue", Caps: 0x8, Installed: true, Enabled: true},
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

// The clock is the one app the hub can serve an editor for before GEOM_ABI 3 lands,
// and it is the HUB saying so: the web layer must not carry that slug itself.
func TestThemeAppsClockEditorComesFromTheHub(t *testing.T) {
	orb := orbfake.New().WithCardApps(
		orbfake.CardApp{Stem: "clock", Slug: "clock", Title: "Clock", Caps: 0x8, Installed: true, Enabled: true},
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
