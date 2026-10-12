package main

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// There is no JavaScript engine on the machine this suite runs on — no node, deno,
// bun, or d8 — so app.js cannot be handed to a real parser. That gap is stated in the
// README rather than hidden. What CAN be checked here is what has actually bitten this
// client before: a handler wired to a control that is not in the page, which fails as
// silently in a browser as the payload-shape bug that once blanked the port picker.

func readWebFile(t *testing.T, name string) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("web", name))
	if err != nil {
		t.Fatalf("read web/%s: %v", name, err)
	}
	return string(b)
}

var (
	selRe     = regexp.MustCompile(`\$\("#([A-Za-z0-9_-]+)"`)
	idRe      = regexp.MustCompile(`id="([A-Za-z0-9_-]+)"`)
	templateS = regexp.MustCompile(`(?s)<template[^>]*>(.*?)</template>`)
)

// Every element the client reaches for must exist in the page, including the ids the
// runtime templates declare. A one-sided rename is invisible in a browser: the button
// is simply never wired.
func TestAppJSOnlyReferencesElementsThePageHas(t *testing.T) {
	page := readWebFile(t, "index.html")
	ids := map[string]bool{}
	for _, m := range idRe.FindAllStringSubmatch(page, -1) {
		ids[m[1]] = true
	}
	if len(ids) == 0 {
		t.Fatal("index.html has no ids; this test's regex or the page changed underneath it")
	}
	for _, m := range selRe.FindAllStringSubmatch(readAppJS(t), -1) {
		if !ids[m[1]] {
			t.Errorf(`app.js reaches for $("#%s") but index.html has no such id`, m[1])
		}
	}
}

// The buttons iteration 5 promised. Pinning them by id keeps a refactor from deleting
// a card and its handler one at a time, which is how a feature quietly becomes half a
// feature.
func TestManageViewHasThemeAndConsoleControls(t *testing.T) {
	page := readWebFile(t, "index.html")
	for _, id := range []string{
		"mn-themecard", "mn-themes", "mn-themestate", "mn-default", "mn-rescan",
		"mn-fw-file", "mn-up-btn", "mn-up-name", "mn-up-state",
		"mn-consolecard", "mn-con-port", "mn-con-go", "mn-con-stop", "mn-con-state", "mn-con-log",
		"ob-cancel", "ob-op",
	} {
		if !strings.Contains(page, `id="`+id+`"`) {
			t.Errorf("index.html is missing #%s", id)
		}
	}
	src := readAppJS(t)
	for _, want := range []string{
		"/api/theme/list", "/api/theme/install", "/api/theme/delete", "/api/theme/default",
		"/api/theme/rescan", "/api/theme/upload", "/api/console", "/api/orb/known",
		"function startPolling", "function loadThemes", "function readConsole",
	} {
		if !strings.Contains(src, want) {
			t.Errorf("app.js never contains %q, so that part of the card is not wired", want)
		}
	}
}

// The theme list arrives from the orb's SD card, so its names and titles are
// device-supplied bytes. They must be built with DOM APIs, never interpolated into
// markup — the same rule the firmware panel's escaping test enforces server-side, and
// the one a "simplification" would quietly undo.
func TestThemeRowsAreBuiltWithoutMarkupInterpolation(t *testing.T) {
	body := blockOf(t, readAppJS(t), "function themeRow")
	for _, bad := range []string{"innerHTML", "insertAdjacentHTML", "${", "outerHTML"} {
		if strings.Contains(body, bad) {
			t.Errorf("themeRow uses %s; theme names and titles are device-supplied and must go through textContent", bad)
		}
	}
	if !strings.Contains(body, "createElement") || !strings.Contains(body, "textContent") {
		t.Error("themeRow must build its row with createElement/textContent")
	}
}

// Deleting a theme is the one card action with no undo available from the hub, so the
// card asks first. A confirm() that goes missing turns an accidental click into a
// deleted theme.
func TestThemeDeleteAsksBeforeRemoving(t *testing.T) {
	body := blockOf(t, readAppJS(t), "function themeRow")
	if !strings.Contains(body, "confirm(") {
		t.Error("the delete button must confirm before calling /api/theme/delete")
	}
	if !strings.Contains(body, "/api/theme/delete") {
		t.Error("themeRow is expected to be the only caller of /api/theme/delete")
	}
}

// blockOf returns one function's source, for the targeted textual pins above.
func blockOf(t *testing.T, src, marker string) string {
	t.Helper()
	i := strings.Index(src, marker)
	if i < 0 {
		t.Fatalf("app.js has no %q — the control was renamed or removed and this pin is now meaningless", marker)
	}
	open := strings.Index(src[i:], "{")
	if open < 0 {
		t.Fatalf("unterminated block at %q", marker)
	}
	depth := 0
	for k := i + open; k < len(src); k++ {
		switch src[k] {
		case '{':
			depth++
		case '}':
			depth--
			if depth == 0 {
				return src[i : k+1]
			}
		}
	}
	t.Fatalf("%q never closes", marker)
	return ""
}
