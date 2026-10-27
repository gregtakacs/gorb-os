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
// h_apps walks from ORB_APP_BASE so built-ins never appear in its installed tier, while
// h_status walks from zero and always names them. orb-B (fw 9442432) reports
// "installed":[] beside a three-row ledger, and a double that used one walker for both told
// the hub there were no apps on an orb that has three.
func TestLedgerSplitMatchesTheDevice(t *testing.T) {
	o := New()
	srv := httptest.NewServer(o)
	defer srv.Close()

	_, body := appDo(t, srv, http.MethodGet, "/api/v1/apps", nil)
	if !strings.Contains(body, `"count":0`) || !strings.Contains(body, `"installed":[]`) {
		t.Errorf("empty-card listing = %s, want count 0 and an empty installed tier", body)
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
