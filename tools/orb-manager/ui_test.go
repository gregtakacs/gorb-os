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
		"mn-fw-file", "mn-up-btn", "mn-up-state",
		"mn-consolecard", "mn-con-port", "mn-con-go", "mn-con-stop", "mn-con-state", "mn-con-log",
		"ob-cancel", "ob-op",
		// Item 5's LAN list. Pinned by id for the same reason as the rest: a
		// one-sided rename leaves the browse card rendering an empty <ul> forever
		// and nothing else in the suite notices.
		"mn-findcard", "mn-find", "mn-find-state", "mn-find-go", "mn-find-note",
	} {
		if !strings.Contains(page, `id="`+id+`"`) {
			t.Errorf("index.html is missing #%s", id)
		}
	}
	src := readAppJS(t)
	for _, want := range []string{
		"/api/theme/list", "/api/theme/install", "/api/theme/delete", "/api/theme/default",
		"/api/theme/rescan", "/api/theme/upload", "/api/console", "/api/orb/known",
		"/api/orbs", "function readFound",
		// Item 6's naming controls, pinned by the same rule as the rest: a card that
		// stops posting to /api/fleet/set is a card whose name box does nothing, and
		// nothing else in the suite would notice.
		"/api/fleet", "/api/fleet/set", "/api/fleet/forget", "function foundRow",
		"function startPolling", "function loadThemes", "function readConsole",
	} {
		if !strings.Contains(src, want) {
			t.Errorf("app.js never contains %q, so that part of the card is not wired", want)
		}
	}
}

// The theme list arrives from the orb's SD card, so its names are device-supplied bytes.
// They must be built with DOM APIs, never interpolated into markup — the same rule the
// firmware panel's escaping test enforces server-side, and the one a "simplification"
// would quietly undo.
func TestThemeRowsAreBuiltWithoutMarkupInterpolation(t *testing.T) {
	body := blockOf(t, readAppJS(t), "function themeRow")
	for _, bad := range []string{"innerHTML", "insertAdjacentHTML", "${", "outerHTML"} {
		if strings.Contains(body, bad) {
			t.Errorf("themeRow uses %s; theme names are device-supplied bytes and must go through textContent", bad)
		}
	}
	if !strings.Contains(body, "createElement") || !strings.Contains(body, "textContent") {
		t.Error("themeRow must build its row with createElement/textContent")
	}
}

// FORMAT 2 leaves a theme exactly one identifier a caller may address: the 12-hex id the
// orb derived from the name baked inside the container. The device has no route that
// takes a name, so every pin here is the difference between a card that works and one
// where every button answers "id is missing or not a theme id" — the state this client
// shipped in, green, because the only thing it was ever tested against was a fake that
// agreed with it.
func TestThemeCardAddressesThemesByID(t *testing.T) {
	row := blockOf(t, readAppJS(t), "function themeRow")

	for _, want := range []string{"t.id", "/api/theme/install", "/api/theme/delete"} {
		if !strings.Contains(row, want) {
			t.Errorf("themeRow never mentions %s, so the row is not addressing the orb's way", want)
		}
	}
	// The buttons must hand over the id, and must not hand over a name: the hub has no
	// route that would accept one, and the old card did exactly this.
	for _, gone := range []string{"name: t.name", "title: t.title", "?name="} {
		if strings.Contains(row, gone) {
			t.Errorf("themeRow sends %s; a theme is addressed by the id the orb listed, not by a name", gone)
		}
	}
	// FORMAT 1's separate display title is gone: one field is the name and the label.
	if strings.Contains(row, "t.title") {
		t.Error("themeRow reads t.title; FORMAT 2 headers carry one name and no title")
	}
	// The label is the name — the human-facing half of the identity.
	if !strings.Contains(row, "label.textContent = t.name") {
		t.Error("themeRow no longer labels the row with the theme's name, which is the identity an owner reads")
	}

	// An upload is named by its bytes. A box that asks the operator to type one lies to
	// them: the device reads no such argument and files the theme under the name inside
	// the file, so what it shows afterwards would not be what the card did.
	src := readAppJS(t)
	if strings.Contains(src, "mn-up-name") {
		t.Error("app.js still asks the operator to name an upload; the name lives inside the container")
	}
	upload := blockOf(t, src, `$("#mn-fw-file").onchange`)
	if strings.Contains(upload, "&name=") {
		t.Error("the upload posts a name; h_theme_upload reads none")
	}
	// The replace decision is a second send with the firmware's flag on it: the card has
	// to reach it from the refusal, and the flag has to be the one string the device reads.
	if !strings.Contains(upload, "uploadTheme(f, true") {
		t.Error("the upload has no path that retries with the replace flag, so a same-name re-bake dead-ends")
	}
	if !strings.Contains(blockOf(t, src, "async function uploadTheme"), "overwrite=1") {
		t.Error("the retry never sets ?overwrite=1, the only upload argument the firmware reads")
	}
}

// The orb keys the two failure sentences it reports — the install it refused and the copy
// boot could not load — on the theme id. They have to reach the row they belong to, or a
// press that did nothing is indistinguishable from one that worked, which is the C8 lesson
// the firmware's own panel applies the same way.
func TestThemeCardSurfacesTheOrbsOwnRefusals(t *testing.T) {
	row := blockOf(t, readAppJS(t), "function themeRow")
	for _, want := range []string{"try_id", "try_why", "damage_id", "damage_why"} {
		if !strings.Contains(row, want) {
			t.Errorf("themeRow ignores %s, so a refused install stays silent on the card", want)
		}
	}
	// An overwrite keeps the name and so the id: without the changed flag the row claims
	// to be the face on the glass after the card moved on underneath it.
	if !strings.Contains(row, "t.changed") {
		t.Error("themeRow drops changed, so an overwritten theme keeps reporting itself as drawn")
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

// Naming a board is the one part of the fleet the operator types into, and the row is
// built from bytes that arrived over UDP plus a file the hub wrote. So the same two
// rules the theme card applies to device names apply here: build with DOM APIs, and let
// the hub's own refusal be what the card shows.
func TestFleetRowsAreBuiltWithoutMarkupInterpolation(t *testing.T) {
	row := blockOf(t, readAppJS(t), "function foundRow")
	for _, bad := range []string{"innerHTML", "insertAdjacentHTML", "outerHTML"} {
		if strings.Contains(row, bad) {
			t.Errorf("foundRow uses %s; an announced instance name is device-supplied bytes", bad)
		}
	}
	for _, want := range []string{"document.createElement", "textContent"} {
		if !strings.Contains(row, want) {
			t.Errorf("foundRow no longer builds rows with %s", want)
		}
	}
	// The refusal is the hub's sentence, not the card's guess: a row that says "no"
	// without saying why is the row an operator clicks again and blames the orb for.
	if !strings.Contains(row, "o.refusal") {
		t.Error("foundRow drops the hub's refusal, so a row that cannot be dialed has no explanation")
	}
	// The identity is keyed on the MAC, and the only handle the row has to the board is
	// o.mac. A rename that posted the address would rename whatever held the lease.
	if !strings.Contains(row, "mac: o.mac") {
		t.Error("foundRow does not post the MAC as the identity it is renaming")
	}
	if strings.Contains(row, "ip: o.ip") {
		t.Error("foundRow names a board by its address; the fleet keys on the MAC")
	}
	// Forgetting revokes the hub's willingness to dial, so it asks first.
	if !strings.Contains(row, "confirm(") {
		t.Error("Forget no longer asks before dropping a name and revoking an address")
	}
}
