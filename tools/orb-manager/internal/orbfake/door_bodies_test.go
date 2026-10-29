package orbfake

// This file pins the WORDING of every refusal the double sends, scraped from the firmware
// that sends it. It exists because a hand-copied string rotted the first time through:
// apps.go answered `no installed app "x"` where every real orb answers `no app by that name
// is installed on this dial`, and the suite stayed green all the way up to a live body,
// because the route walk in the root contract test checks only that a route is not 404.
//
// Two consequences for whoever edits this next:
//   - The literals in apps.go are copied by hand and cannot be trusted on their own. This
//     test is what makes them honest.
//   - The firmware's own page (orb_net.c:4477) prints r.d.error to the operator, so this
//     wording is user-visible text. Drift here is a UI bug, not a cosmetic one.

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// firmwareSrc reads a firmware file four levels up: this package sits at
// tools/orb-manager/internal/orbfake, the sources at main/.
func firmwareSrc(t *testing.T, rel ...string) string {
	t.Helper()
	p := filepath.Join(append([]string{"..", "..", "..", "..", "main"}, rel...)...)
	b, err := os.ReadFile(p)
	if err != nil {
		t.Fatalf("read %s: %v", p, err)
	}
	return string(b)
}

// glueC closes adjacent C string literals so `"abc" "def"` reads as "abcdef". The firmware
// splits several door messages across lines, and a scrape that does not glue them reports a
// present message as missing.
var glueC = regexp.MustCompile(`"\s*"`)

func TestDoorRefusalBodiesMatchTheFirmware(t *testing.T) {
	install := glueC.ReplaceAllString(firmwareSrc(t, "app", "orb_app_install.c"), "")
	net := glueC.ReplaceAllString(firmwareSrc(t, "net", "orb_net.c"), "")

	for _, c := range []struct{ body, file, src string }{
		{doorBusyBody, "orb_app_install.c", install},
		{notFoundBody, "orb_app_install.c", install},
		{builtinBody, "orb_app_install.c", install},
		{onDialBody, "orb_app_install.c", install},
		{noIfaceBody, "orb_app_install.c", install},
		{reviveFailBody, "orb_app_install.c", install},
		{notUsableNameBody, "orb_app_install.c", install},
		{uninstallHoldBody, "orb_app_install.c", install},
		{missingIDBody, "orb_net.c", net},
		{badOnBody, "orb_net.c", net},
		{noStemBody, "orb_net.c", net},
		{notEnabledBody, "orb_net.c", net},
		{failedFaceBody, "orb_net.c", net},
	} {
		sentence := strings.TrimSuffix(strings.TrimPrefix(c.body, `{"error":"`), `"}`)
		if sentence == c.body {
			t.Fatalf("%s is not in the shape the doors send", c.body)
		}
		if !strings.Contains(c.src, sentence) {
			t.Errorf("double refuses with %q, which is not a sentence in %s", sentence, c.file)
		}
		if strings.Contains(sentence, "%") {
			t.Errorf("%q interpolates caller text; the firmware puts only its own words in a refusal", sentence)
		}
	}

	// The esp_err to status mapping, scraped rather than assumed. NOT_SUPPORTED is a 400, not
	// the 501 an author would guess, and that is the answer a Manage card shows for the clock.
	for _, arm := range []struct{ code, status string }{
		{"ESP_ERR_NOT_FOUND", `"404 Not Found"`},
		{"ESP_ERR_INVALID_ARG", `"400 Bad Request"`},
		{"ESP_ERR_NOT_SUPPORTED", `"400 Bad Request"`},
		{"ESP_ERR_INVALID_STATE", `"409 Conflict"`},
	} {
		re := regexp.MustCompile(arm.code + `:\s*return\s+` + arm.status)
		if !re.MatchString(net) {
			t.Errorf("door_http_status no longer maps %s to %s; re-check every status the double sends",
				arm.code, arm.status)
		}
	}
}

// TestLedgerSplitMatchesTheDevice pins the two views the firmware gives of one registry.
//
// Both tiers now come from one walk. apps_send_installed (orb_net.c:5054) emits the
// compiled indices 0..ORB_APP_BASE-1 FIRST, each stamped "compiled":true, then the card
// rows from ORB_APP_BASE. The comment this test used to carry claimed /api/v1/apps reported
// "installed":[] beside a three-row ledger, and it cited orb-B on fw 9442432. That was the
// device then. P4a's note inside the emitter says the same plainly — "a built-in that C2
// quarantined at boot is exactly the row an owner must see and clear" — so a double that
// hid the shell could not model the one built-in state with a verb on it.
//
// What the test now pins is the part of the split that survives and matters to the hub: the
// built-in row is PRESENT and FLAGGED, and a caller that counts the owner's inventory must
// filter on the flag rather than assume the tier came pre-filtered.
func TestLedgerSplitMatchesTheDevice(t *testing.T) {
	o := New()
	srv := httptest.NewServer(o)
	defer srv.Close()

	_, body := appDo(t, srv, http.MethodGet, "/api/v1/apps", nil)
	if !strings.Contains(body, `"count":0`) {
		t.Errorf("empty-card listing = %s, want count 0 for a card holding no containers", body)
	}
	if !strings.Contains(body, `"compiled":true`) {
		t.Errorf("the installed tier omitted the built-in shell; the firmware emits it first: %s", body)
	}
	// The flag is the whole contract with the hub's face. A shell row that arrives
	// unflagged is indistinguishable from one the operator installed, and it comes out in
	// the Apps face as an app with an Uninstall button the device will refuse.
	for _, slug := range builtInSlugs {
		row := installedRowOf(body, slug)
		if row == "" {
			t.Errorf("installed tier omitted the built-in %q: %s", slug, body)
			continue
		}
		if !strings.Contains(row, `"compiled":true`) {
			t.Errorf("built-in %q arrived without its flag: %s", slug, row)
		}
		// And no version: a compiled row was never a container, so an empty stamp here
		// would read to the hub as "unstamped card app", which is a different refusal.
		if strings.Contains(row, `"version"`) {
			t.Errorf("built-in %q carries a version key the device omits: %s", slug, row)
		}
	}
	_, sb := appDo(t, srv, http.MethodGet, "/api/v1/status", nil)
	for _, slug := range builtInSlugs {
		if !strings.Contains(sb, `"slug":"`+slug+`"`) {
			t.Errorf("ledger omitted the built-in %q: %s", slug, sb)
		}
	}
	if at := strings.Index(sb, `"apps":[{`+"\"slug\":\"menu\""); at < 0 {
		t.Error("ledger does not start at row zero; h_status walks from the first registered app")
	}
	// The built-in refusal on both doors, at the status the device measured (NOT_SUPPORTED
	// maps to 400, which door_http_status's arms are pinned on above).
	for _, path := range []string{"/api/v1/apps/enable?id=menu&on=0", "/api/v1/apps/uninstall?id=menu"} {
		if code, body := appDo(t, srv, http.MethodPost, path, []byte("{}")); code != 400 ||
			!strings.Contains(body, builtinBody) {
			t.Errorf("POST %s -> %d %s, want 400 %s", path, code, body, builtinBody)
		}
	}
}

// installedRowOf cuts one object out of the installed array by slug. It scans for the
// slug at array depth, so a "slug" key on a card row cannot be mistaken for the one asked
// for, and it returns "" when no such row is present rather than a near miss.
func installedRowOf(body, slug string) string {
	const key = `"installed":[`
	i := strings.Index(body, key)
	if i < 0 {
		return ""
	}
	want := `"slug":"` + slug + `"`
	depth := 0
	start := -1
	for j := i + len(key); j < len(body); j++ {
		switch body[j] {
		case '{':
			if depth == 0 {
				start = j
			}
			depth++
		case '}':
			depth--
			if depth == 0 && start >= 0 {
				row := body[start : j+1]
				if strings.Contains(row, want) {
					return row
				}
				start = -1
			}
		case ']':
			if depth == 0 {
				return ""
			}
		}
	}
	return ""
}
