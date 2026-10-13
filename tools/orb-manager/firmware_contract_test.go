package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// The hub's theme surface, pinned to the firmware source rather than to anybody's
// reading of it.
//
// This file exists because of how the last break survived. The client, its unit test and
// the device double each recorded what the theme API looked like, all three were written
// from the same mistaken idea, and the suite stayed green while every real orb 400-ed an
// install. Three copies of one assumption agree with each other and disagree with the
// device. Reading the assertions out of main/net/orb_net.c is the only arrangement where
// the firmware gets a vote.
//
// It skips when the firmware source is not reachable, which is the shipped-app case: a
// user's machine has the hub, not the tree.

func firmwareFile(t *testing.T, rel string) string {
	t.Helper()
	p := filepath.Join("..", "..", rel)
	b, err := os.ReadFile(p)
	if err != nil {
		t.Skipf("no firmware source at %s: %v", p, err)
	}
	return string(b)
}

// cFunction lifts one C function's text by name, brace-matched, so a claim about a
// handler is checked against that handler and not against a comment describing it.
func cFunction(t *testing.T, src, name string) string {
	t.Helper()
	mark := "static esp_err_t " + name + "("
	i := strings.Index(src, mark)
	if i < 0 {
		t.Fatalf("main/net/orb_net.c has no %s — the handler was renamed and this pin is now meaningless", name)
	}
	open := strings.Index(src[i:], "{")
	if open < 0 {
		t.Fatalf("%s has no body", name)
	}
	depth, j := 0, i+open
	for ; j < len(src); j++ {
		switch src[j] {
		case '{':
			depth++
		case '}':
			depth--
			if depth == 0 {
				return src[i : j+1]
			}
		}
	}
	t.Fatalf("unbalanced braces in %s", name)
	return ""
}

// The device route allowlist must be the firmware's uri table for the theme surface:
// an entry the orb does not register is a route the hub will dial into a 404, and one
// the orb registers but the hub omits is a button that cannot reach the device.
func TestDeviceRouteAllowlistMatchesTheFirmwaresUriTable(t *testing.T) {
	src := firmwareFile(t, filepath.Join("main", "net", "orb_net.c"))
	rows := regexp.MustCompile(`\{\s*\.uri\s*=\s*"([^"]+)",\s*\.method\s*=\s*HTTP_(GET|POST),\s*\.handler\s*=\s*(\w+)\s*\}`)
	found := rows.FindAllStringSubmatch(src, -1)
	if len(found) == 0 {
		t.Fatal("no uri table parsed out of orb_net.c — the table's spelling changed and this test is blind")
	}
	seen := 0
	for _, m := range found {
		uri, method := m[1], strings.ToUpper(m[2])
		if !strings.HasPrefix(uri, "/api/v1/sd") {
			continue
		}
		seen++
		methods, ok := deviceRoutes[uri]
		if !ok {
			t.Errorf("the orb registers %s %s but the hub's allowlist omits it, so the Manage card cannot reach it", method, uri)
			continue
		}
		if !contains(methods, method) {
			t.Errorf("the orb registers %s %s but the hub allows %v", method, uri, methods)
		}
	}
	if seen == 0 {
		t.Fatal("no /api/v1/sd routes found in the firmware's uri table")
	}
	// The other direction: an allowlisted theme route the firmware no longer registers
	// would let a browser page dial a route that answers 404, or one that was deleted.
	for uri := range deviceRoutes {
		if !strings.HasPrefix(uri, "/api/v1/sd") {
			continue
		}
		hit := false
		for _, m := range found {
			if m[1] == uri {
				hit = true
			}
		}
		if !hit {
			t.Errorf("the hub allows %s, which the firmware does not register", uri)
		}
	}
}

// install and delete are addressed by id, in the firmware's own words, and the client
// must match that rather than the description of it in a plan.
func TestFirmwareAddressesThemeVerbsByID(t *testing.T) {
	src := firmwareFile(t, filepath.Join("main", "net", "orb_net.c"))
	for _, h := range []string{"h_theme_install", "h_theme_del"} {
		body := cFunction(t, src, h)
		if !strings.Contains(body, `q_param(req, "id"`) {
			t.Errorf("%s does not read ?id=; check what the client is expected to send", h)
		}
		if !strings.Contains(body, "orb_theme_id_ok") {
			t.Errorf("%s does not gate on orb_theme_id_ok, so the client's own 12-hex check has no counterpart", h)
		}
		if strings.Contains(body, `q_param(req, "name"`) {
			t.Errorf("%s reads ?name=; FORMAT 2 addresses a card file by id only", h)
		}
	}
}

// h_theme_upload reads exactly one argument. Anything else the hub sends is decoration
// the device ignores — the condition that let a typed-in theme name look meaningful.
func TestFirmwareUploadTakesNoName(t *testing.T) {
	src := firmwareFile(t, filepath.Join("main", "net", "orb_net.c"))
	body := cFunction(t, src, "h_theme_upload")
	if strings.Contains(body, `q_param(req, "name"`) {
		t.Error("h_theme_upload reads ?name=; the theme's name comes from inside the container")
	}
	for _, want := range []string{`q_param(req, "overwrite"`, "Content-Length required"} {
		if !strings.Contains(body, want) {
			t.Errorf("h_theme_upload no longer mentions %q, and the hub relies on it", want)
		}
	}
}

// The row the card renders is the row h_sd formats. Field names are checked against the
// firmware's own snprintf, so a rename on either side is a failing test here rather than
// a blank column on the page.
func TestThemeRowFieldsMatchHsd(t *testing.T) {
	src := firmwareFile(t, filepath.Join("main", "net", "orb_net.c"))
	body := cFunction(t, src, "h_sd")

	// The rows, and the card state beside them: both structs decode out of this one
	// handler, so both are pinned here. Checking only ThemeEntry once let a revert of
	// try_id to try_slug through unnoticed, because the field lives on ThemeList.
	tags := themeRowJSONTags(t)
	tags = append(tags, themeListJSONTags(t)...)
	for _, tag := range tags {
		if !strings.Contains(body, "\\\""+tag+"\\\"") {
			t.Errorf("h_sd never emits %q, but the hub decodes a field by that name", tag)
		}
	}
	// FORMAT 1's separate display title, and the slug-keyed failure strings, must stay
	// gone in both directions: the client must not decode them, and the firmware must not
	// start emitting them again.
	for _, gone := range []string{"title", "try_slug", "damage_slug", "slug"} {
		if strings.Contains(body, "\\\""+gone+"\\\"") {
			t.Errorf("h_sd emits %q, which FORMAT 2 retired; the hub no longer decodes it", gone)
		}
		for _, tag := range tags {
			if tag == gone {
				t.Errorf("the hub still decodes %q, a field FORMAT 2 removed from the device", gone)
			}
		}
	}
	for _, want := range []string{"try_id", "damage_id", "active", "changed"} {
		if !strings.Contains(body, "\\\""+want+"\\\"") {
			t.Errorf("h_sd stopped reporting %q; the card keys its failure notes and its stale-row hint on it", want)
		}
	}
}

// The id rule lives in three languages. This checks the Go copy against the C one on
// values the firmware itself would issue, which is the pair a theme install depends on.
func TestThemeIdRuleMatchesTheFirmware(t *testing.T) {
	src := firmwareFile(t, filepath.Join("main", "art", "orb_theme.c"))
	if !strings.Contains(src, "mbedtls_sha256") || !strings.Contains(src, "THEME_ID_HEX") {
		t.Fatal("theme_id() is no longer recognisable in orb_theme.c; update the Go mirror in internal/orbnet")
	}
	for _, tc := range []struct{ name, id string }{
		{"aurora", "9b89025ce7a6"},
		{"Aurora", "e9de098c7733"},
		{"skeleton", "2ac11a92104c"},
	} {
		got, err := orbnet.ThemeIDOf(tc.name)
		if err != nil {
			t.Fatalf("ThemeIDOf(%q): %v", tc.name, err)
		}
		if got != tc.id {
			t.Errorf("ThemeIDOf(%q) = %s, want %s", tc.name, got, tc.id)
		}
	}
	// The 12-hex gate, spelled the firmware's way and the client's way.
	h := firmwareFile(t, filepath.Join("main", "net", "orb_net.c"))
	if !strings.Contains(h, "ORB_THEME_ID_HEX + 1") {
		t.Error("h_theme_install no longer sizes an id buffer from ORB_THEME_ID_HEX; re-check CheckThemeID")
	}
	for _, bad := range []string{"", "A", "9B89025CE7A6", "9b89025ce7a", "9b89025ce7a6f"} {
		if err := orbnet.CheckThemeID(bad); err == nil {
			t.Errorf("CheckThemeID(%q) accepted a handle orb_theme_id_ok refuses", bad)
		}
	}
}

// The upload ceiling is a device constant, restated in two hub-side places. Read it from
// the header rather than trusting either copy.
func TestUploadCeilingMatchesTheHeader(t *testing.T) {
	h := firmwareFile(t, filepath.Join("main", "art", "orb_theme.h"))
	m := regexp.MustCompile(`#define\s+ORB_THEME_UPLOAD_MAX\s+\(?\s*(0x[0-9A-Fa-f]+)[uUlL]*\s*\+\s*(0x[0-9A-Fa-f]+)[uUlL]*`).FindStringSubmatch(h)
	if m == nil {
		t.Fatal("ORB_THEME_UPLOAD_MAX is not in a form this test can parse; do not let this check go quiet — read the value off main/art/orb_theme.h and compare it to orbnet.MaxThemeUpload by hand")
	}
	var a, b uint64
	// Strip the 0x and parse by base: fmt's %x verb does NOT accept a base prefix, so
	// Sscanf("%x") on "0x00400000" matches nothing and leaves the operand at zero — a
	// guard that reads zero, compares it against zero, and calls the constant verified.
	var perr, perr2 error
	if a, perr = strconv.ParseUint(strings.TrimPrefix(m[1], "0x"), 16, 64); perr != nil {
		t.Fatalf("parsing %q: %v", m[1], perr)
	}
	if b, perr2 = strconv.ParseUint(strings.TrimPrefix(m[2], "0x"), 16, 64); perr2 != nil {
		t.Fatalf("parsing %q: %v", m[2], perr2)
	}
	if a+b == 0 {
		t.Fatal("the header's upload ceiling parsed to zero; this check is not measuring anything")
	}
	if uint64(orbnet.MaxThemeUpload) != a+b {
		t.Errorf("orbnet.MaxThemeUpload = %d, header says %d", orbnet.MaxThemeUpload, a+b)
	}
}

func themeRowJSONTags(t *testing.T) []string {
	t.Helper()
	tags := jsonTagsOf(t, orbnet.ThemeEntry{})
	if len(tags) != 8 {
		t.Fatalf("ThemeEntry has %d JSON fields (%v); h_sd emits 8, and this test only checks the ones the struct declares", len(tags), tags)
	}
	return tags
}

// themeListJSONTags reads the card-state struct the same way. The failure strings the
// card points at a row live here, not on the row, so a rename is invisible to a test
// that looks only at ThemeEntry.
func themeListJSONTags(t *testing.T) []string {
	t.Helper()
	tags := jsonTagsOf(t, orbnet.ThemeList{})
	for _, want := range []string{"try_id", "try_why", "damage_id", "damage_why", "active", "live", "mounted", "card", "themes"} {
		if !contains(tags, want) {
			t.Errorf("ThemeList does not decode %q, so the card loses that part of h_sd's reply", want)
		}
	}
	return tags
}

func jsonTagsOf(t *testing.T, v any) []string {
	t.Helper()
	b, err := json.Marshal(v)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(b, &m); err != nil {
		t.Fatal(err)
	}
	tags := make([]string, 0, len(m))
	for k := range m {
		tags = append(tags, k)
	}
	return tags
}

func contains(xs []string, want string) bool {
	for _, x := range xs {
		if x == want {
			return true
		}
	}
	return false
}
