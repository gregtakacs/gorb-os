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
		"mn-themecard", "mn-themes", "mn-themestate", "mn-rescan",
		"mn-fw-file", "mn-up-btn", "mn-up-state",
		"mn-consolecard", "mn-con-port", "mn-con-go", "mn-con-stop", "mn-con-state", "mn-con-log",
		"ob-cancel", "ob-op",
		// The flash panel's two status lines, pinned separately on purpose. The
		// percentage and the hub's "flash running (12s)" report are written by two
		// different timers, and sharing one element made them alternate on screen
		// twice a second; the bar jumped because the row grew. If either id goes
		// away the panel is back to flickering, so this is a contract and not
		// decoration.
		"ob-liveop", "ob-bar", "nf-liveop", "nf-op", "nf-bar",
		// Item 5's LAN list. Pinned by id for the same reason as the rest: a
		// one-sided rename leaves the browse card rendering an empty <ul> forever
		// and nothing else in the suite notices. There is no mn-find-go any more: the
		// card is pushed, and TestTheDiscoveryCardIsPushedNotPolled fails the build if
		// the button or the timer that replaced it come back.
		"mn-findcard", "mn-find", "mn-find-state", "mn-find-note",
	} {
		if !strings.Contains(page, `id="`+id+`"`) {
			t.Errorf("index.html is missing #%s", id)
		}
	}
	src := readAppJS(t)
	for _, want := range []string{
		"/api/theme/list", "/api/theme/install", "/api/theme/delete",
		"/api/theme/rescan", "/api/theme/upload", "/api/theme/upload-status", "/api/console", "/api/orb/known",
		"/api/orbs", "function applyFound", "function watchOrbs", "function paintFound",
		// The fleet controls, pinned by the same rule as the rest. /api/fleet/set
		// is deliberately NOT one of them: naming on this page is display-only,
		// so a card that posts a rename is the bug now, not the feature. The live
		// row's ending goes through the forget dialog instead.
		"/api/fleet", "/api/fleet/forget", "function foundRow", "openForgetDialog",
		"function startPolling", "function loadThemes", "function readConsole",
	} {
		if !strings.Contains(src, want) {
			t.Errorf("app.js never contains %q, so that part of the card is not wired", want)
		}
	}
}

// The Wi-Fi join card is a face of its own, not a guest on the Orb face. It was
// originally placed there because the card looked small; the reason it moves is
// the request underneath it. The scan/join pair is the only part of the page
// that drives a radio, and dormancy means the operator's press, not the page's
// load, is what asks for it — so the card needs a boundary the page can be read
// against. Pinned in both directions: the card carries the wifi tab, and nothing
// on the orb tab still looks like a join form.
func TestWifiIsItsOwnFace(t *testing.T) {
	page := readWebFile(t, "index.html")
	tabs := readWebFile(t, "tabs.js")
	// The join controls belong to THIS card, and to exactly one card: a second
	// copy of the button somewhere else is the bug this face split could leave
	// behind, and two doors writing one join is a real hazard.
	card := cardMarkup(page, "mn-wificard")
	if section(card, "data-tab") != "wifi" {
		t.Errorf(`the Wi-Fi card is on face %q, not its own: it drives the radio and must open on a press`, section(card, "data-tab"))
	}
	for _, id := range []string{"j-go", "j-ssid", "j-rescan"} {
		if !strings.Contains(card, `id="`+id+`"`) {
			t.Errorf("the join control #%s is no longer on the Wi-Fi card", id)
		}
		if n := strings.Count(page, `id="`+id+`"`); n != 1 {
			t.Errorf("join control #%s appears %d times on the page; the join form must exist once", id, n)
		}
	}
	if !strings.Contains(page, `data-tab="wifi" role="tab"`) {
		t.Error("index.html lost the Wi-Fi tab button")
	}
	// The write-in is the face's standing answer, same sentinel the New Orb
	// flow uses, and the join reads it: an empty scan must never dead-end.
	if (!strings.Contains(card, `id="j-manual"`)) {
		t.Error("the Wi-Fi card lost its write-in input; a hidden network has no other door")
	}
	if !strings.Contains(readAppJS(t), `"__manual__"`) {
		t.Error(`app.js no longer handles the "__manual__" sentinel on this card's picker`)
	}
	// The zone field is gone and stays gone: the Orb face's picker is the only
	// door that sets it, and a second one is how drift starts.
	for _, gone := range []string{"j-tz"} {
		if strings.Contains(page, gone) || strings.Contains(readAppJS(t), gone) {
			t.Errorf("%s is back — the time zone has one door (the Orb face's picker)", gone)
		}
	}
	// The face is withdrawn, not left standing, for a board with no address.
	if !strings.Contains(tabs, `name === "wifi"`) || !strings.Contains(tabs, "current.ip") {
		t.Error("tabs.js no longer gates the Wi-Fi face on the board having an address")
	}
	// And opening it asks for the cached table, not a fresh sweep of the air.
	if !strings.Contains(tabs, "loadNetworks(false)") {
		t.Error("tabs.js opens the Wi-Fi face with a forced rescan; the first read must take the board's cache")
	}
}

// Two owner-visible bugs this test keeps out of the code, both found on the
// live board 2026-10-26.
//
// First: GET /api/v1/apps answers with two arrays. `installed` is the ledger —
// what stands on the dial, built-in shell rows included. `apps` is only the
// /sd/apps drop zone, empty precisely when the dial is full. The first cut of
// the Apps face rendered `apps` as the ledger, so a board running clock, HSB
// and menu told its owner "0 installed apps". The face must read d.installed.
//
// Second: theming is a face of its own. It was written as the clock sub-tab's
// guest, and the split that gave Wi-Fi its tab left it there; the owner's word
// is a Themes tab that sets and deletes themes, one sub-tab per themable app.
// The card, the tab button and the lazy load must all say "themes".
func TestAppsFaceReadsTheLedgerAndThemesIsItsOwnFace(t *testing.T) {
	page := readWebFile(t, "index.html")
	tabs := readWebFile(t, "tabs.js")
	if !strings.Contains(tabs, "d.installed") {
		t.Error("the apps face no longer reads d.installed as the ledger; reading d.apps shows the drop zone and reports a full dial as empty")
	}
	if !strings.Contains(tabs, "Array.isArray(d.apps)") {
		t.Error("the apps face lost its drop-zone tier; card candidates with their refusal sentences must still be listed")
	}
	for _, door := range []string{"/api/v1/apps/enable", "/api/v1/apps/install", "/api/v1/apps/uninstall", "/api/v1/apps/clear", "/api/v1/apps/factory"} {
		if !strings.Contains(tabs, door) {
			t.Errorf("the apps face lost its %s door", door)
		}
	}
	if card := cardMarkup(page, "mn-themecard"); section(card, "data-tab") != "themes" {
		t.Errorf("the theme card is on face %q, not its own: theming is a press of its own", section(card, "data-tab"))
	}
	if !strings.Contains(page, `data-tab="themes" role="tab"`) {
		t.Error("index.html has no Themes tab button")
	}
	if !strings.Contains(page, `id="mn-themes-tabs"`) {
		t.Error("the Themes face lost its per-app chooser; a themable app other than the clock has no way to be shown")
	}
	if !strings.Contains(tabs, `lazyTab("themes", themeOpen)`) {
		t.Error(`the Themes face never lazy-loads: opening it must fetch the listing on a press, like every other face`)
	}
	if strings.Contains(tabs, "themesVisible") {
		t.Error("the clock sub-tab's theme reveal is back; the Themes face owns that card now")
	}
	if !strings.Contains(readAppJS(t), "function themeFaceReset") {
		t.Error("app.js lost themeFaceReset, so switching boards would keep the last board's theme app selection")
	}
}

// cardMarkup returns the article element whose id is wanted, or "" — enough of
// the page to read one card's own attributes and controls without the whole
// document's contents answering a question about one card.
func cardMarkup(page, want string) string {
	const open = `<article`
	at := strings.Index(page, `id="`+want+`"`)
	if at < 0 {
		return ""
	}
	start := strings.LastIndex(page[:at], open)
	end := strings.Index(page[at:], "</article>")
	if start < 0 || end < 0 {
		return ""
	}
	return page[start : at+end+len("</article>")]
}

// section reads one attribute's quoted value out of a fragment of markup.
func section(fragment, attr string) string {
	at := strings.Index(fragment, attr+`="`)
	if at < 0 {
		return ""
	}
	rest := fragment[at+len(attr)+2:]
	end := strings.Index(rest, `"`)
	if end < 0 {
		return ""
	}
	return rest[:end]
}

// The chip erase is gone from this page and stays gone: a blank S3 boot-loops,
// flaps its port, and reads as a broken cable to everyone but a bench script.
// The lonely write is the only end of orb life the manager page offers. The
// name half of this pin changed 2026-10-26 by owner request: the manager can
// rename, and the save goes to the board's own /api/v1/name (which still holds
// the veto) — never to /api/fleet/set, which would make the hub's cache row the
// authority. What stays forbidden is a control that cannot save: a readonly box
// beside a live button is the bug this pins shut from both directions.
func TestPageNeverOffersChipEraseAndTheNameIsEditable(t *testing.T) {
	page := readWebFile(t, "index.html")
	src := readAppJS(t)
	tabs := readWebFile(t, "tabs.js")
	for _, gone := range []string{"ob-chipwipe", "/api/wipe", "wipeArm"} {
		if strings.Contains(page, gone) || strings.Contains(src, gone) || strings.Contains(tabs, gone) {
			t.Errorf("%s is back on the manager page — the hub offers lonely, never a chip erase", gone)
		}
	}
	if !strings.Contains(page, `id="mn-name" size="18" maxlength="32"`) {
		t.Error("the manage page's name field is not the editable one (no readonly, maxlength 32)")
	}
	if strings.Contains(page, `id="mn-name" size="18" maxlength="32" readonly`) {
		t.Error("the name field went readonly again — naming is editable from the manager now")
	}
	if !strings.Contains(page, `id="mn-name-go"`) {
		t.Error("index.html lost the save-name button")
	}
	if !strings.Contains(tabs, "/api/v1/name") {
		t.Error("tabs.js never posts /api/v1/name — the save button has no door")
	}
	if strings.Contains(src, `"/api/fleet/set"`) || strings.Contains(tabs, `"/api/fleet/set"`) {
		t.Error("the page posts /api/fleet/set again; renames go to the board, not the hub's cache")
	}
}

// The discovery card is pushed, not polled, and has no button. Both halves are load-bearing:
// a hub that pushes nothing needs a timer, and a page with a timer and a button is the
// previous design wearing this one's markup. A JavaScript engine is not on this machine, so
// the check is over the source, in the direction that a later edit could realistically break.
func TestTheDiscoveryCardIsPushedNotPolled(t *testing.T) {
	page := readWebFile(t, "index.html")
	src := readAppJS(t)
	for _, gone := range []string{"mn-find-go", "function readFound"} {
		if strings.Contains(page, gone) || strings.Contains(src, gone) {
			t.Errorf("%s is back: the card is pushed, so it has no refresh control and no reader of that name", gone)
		}
	}
	for _, want := range []string{"/api/orbs/stream", "foundRows.get(", "if (li._shown !== shown)", "dataset.mac", "foundRows.clear()"} {
		if !strings.Contains(src, want) {
			t.Errorf("app.js no longer contains %q: the card stopped being live, patched in place, keyed by mac, or cleared when the listener died", want)
		}
	}
	// No timer may re-read the card. The watchdog interval is allowed to exist and must not
	// fetch: a card that re-reads on a clock is the design this replaced, and it fails closed
	// as a stale card the moment the hub stops answering rather than as a dead connection.
	for _, body := range setIntervalBodies(src) {
		for _, banned := range []string{"pullFound", "applyFound", "watchOrbs"} {
			if strings.Contains(body, banned) {
				t.Errorf("a setInterval re-reads the card through %s: the discovery stream is the only thing that moves this card\n%s", banned, body)
			}
		}
	}
}

// setIntervalBodies returns the text of every setInterval(...) call in app.js. Hand-rolled and
// bracket-counting, because there is no parser here: the point is to see what a timer does, not
// to parse JavaScript.
func setIntervalBodies(src string) []string {
	var out []string
	for i := 0; ; {
		j := strings.Index(src[i:], "setInterval(")
		if j < 0 {
			return out
		}
		start := i + j + len("setInterval(")
		depth, k := 1, start
		for ; k < len(src) && depth > 0; k++ {
			switch src[k] {
			case '(':
				depth++
			case ')':
				depth--
			}
		}
		if depth != 0 {
			return out
		}
		out = append(out, src[start:k-1])
		i = k
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
	// to reach it from the refusal, and the flag has to be the one string the device
	// reads. §32 moved the second press from a browser dialog inside onchange onto the
	// button itself — the precheck stops a duplicate before bytes move, the button's
	// next press ships the same file with the flag. Same rule, steadier shape.
	if !strings.Contains(blockOf(t, src, `$("#mn-up-btn").onclick`), "uploadTheme(f, true") {
		t.Error("the upload has no path that retries with the replace flag, so a same-name re-bake dead-ends")
	}
	// The dialog-era confirm() on an upload refusal is gone by rule, not by taste: it
	// arrived after the whole file had crossed the room and re-sent it in full.
	if strings.Contains(upload, "confirm(") {
		t.Error("the upload path still calls confirm(); the replace prompt is inline since §32")
	}
	if !strings.Contains(blockOf(t, src, "async function uploadTheme"), "overwrite=1") {
		t.Error("the retry never sets ?overwrite=1, the only upload argument the firmware reads")
	}
}

// The orb keys the two failure sentences it reports — the install it refused and the
// store that tore before its header — on the theme id. They have to reach the row they
// belong to, or a press that did nothing is indistinguishable from one that worked,
// which is the C8 lesson the firmware's own panel applies the same way.
func TestThemeCardSurfacesTheOrbsOwnRefusals(t *testing.T) {
	row := blockOf(t, readAppJS(t), "function themeRow")
	for _, want := range []string{"try_id", "try_why", "staged_id", "staged_why"} {
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

// blockOfLine returns the one source line that mentions marker. An HTML attribute
// has no brace block to lift, and pinning against the whole page lets a claim about
// one element pass on text that lives in another.
func blockOfLine(t *testing.T, src, marker string) string {
	t.Helper()
	for _, line := range strings.Split(src, "\n") {
		if strings.Contains(line, marker) {
			return line
		}
	}
	t.Fatalf("no line mentions %q — the control was renamed or removed and this pin is now meaningless", marker)
	return ""
}

// Naming a board is the one part of the fleet the operator types into, and the row is
// built from bytes that arrived over UDP plus a file the hub wrote. So the same two
// rules the theme card applies to device names apply here: build with DOM APIs, and let
// the hub's own refusal be what the card shows.
func TestFleetRowsAreBuiltWithoutMarkupInterpolation(t *testing.T) {
	// The row is built in one function and written to in the other, because the card is pushed:
	// foundRow makes the controls once and paintFound updates them on every frame. The pins below
	// are about the row a person ends up looking at, so they read across both.
	row := blockOf(t, readAppJS(t), "function foundRow") + blockOf(t, readAppJS(t), "function paintFound")
	// The Forget press lives in the dialog's drop body now, not in the row: the row is the
	// question and the dialog is the asking. The pins that follow read the two together.
	asked := row + blockOf(t, readAppJS(t), `$("#fj-forget").onclick`)
	for _, bad := range []string{"innerHTML", "insertAdjacentHTML", "outerHTML"} {
		if strings.Contains(row, bad) {
			t.Errorf("the found row uses %s; an announced instance name is device-supplied bytes", bad)
		}
	}
	for _, want := range []string{"document.createElement", "textContent"} {
		if !strings.Contains(row, want) {
			t.Errorf("the found row no longer builds with %s", want)
		}
	}
	// The refusal is the hub's sentence, not the card's guess: a row that says "no"
	// without saying why is the row an operator clicks again and blames the orb for.
	if !strings.Contains(row, "o.refusal") {
		t.Error("the found row drops the hub's refusal, so a row that cannot be dialed has no explanation")
	}
	// The identity is keyed on the MAC, and the row carries it as its own key rather than as
	// whatever the last frame happened to say. A rename that posted the address would rename
	// whatever held the lease tomorrow.
	if !strings.Contains(row, "li.dataset.mac = o.mac") || !strings.Contains(asked, "mac: li.dataset.mac") {
		t.Error("the found row no longer posts the MAC as the identity it is renaming")
	}
	if strings.Contains(row, "ip: o.ip") {
		t.Error("the found row names a board by its address; the fleet keys on the MAC")
	}
	// Forgetting revokes the hub's willingness to dial, so it asks first — and the asking is
	// the page's own dialog, never a browser popup. A popup cannot show which of the hub's
	// three verdicts it is answering, and it kept offering a dead board's delete beside a
	// live board's cable endings as if the page, not the hub, had decided the difference.
	if !strings.Contains(row, "openForgetDialog(li)") {
		t.Error("Forget no longer asks through the page's own dialog before dropping a name and revoking an address")
	}
	if strings.Contains(row, "confirm(") {
		t.Error("the fleet row raises a browser popup; the forget dialog is the only ask this card has")
	}
	// Forget is the hub's verdict, not the card's guess: the row carries whether the board
	// can be reached, and the button is enabled or opened from that field. A card that
	// decided this locally would offer a plain delete to an announcing orb, whose next
	// check-in puts the row straight back — the failure this gate exists to prevent. A
	// liveForget verdict opens the dialog instead of the delete.
	if !strings.Contains(row, "o.forgettable === true") || !strings.Contains(row, "o.liveForget === true") {
		t.Error("the found row no longer requires the hub's forget and live-forget verdicts before it lights Forget")
	}
	// The exact shape of those comparisons is the bug this pin keeps out. Written "=== false"
	// it reads an ABSENT field as permission, and an absent field is what a hub that refuses
	// sends if the bool carries an omitempty: every refusal, cable included, becomes a live
	// button. The page must be able to tell "no" from "not told", and treat both as no.
	if strings.Contains(row, "o.forgettable === false") || strings.Contains(row, "o.forgettable != true") {
		t.Error("the Forget button treats a missing verdict as permission; only a literal true may enable it")
	}
	if !strings.Contains(row, "forget.disabled") {
		t.Error("the Forget button is never disabled, so the verdict is read and ignored")
	}
	if !strings.Contains(row, "o.forgetWhy") {
		t.Error("a disabled Forget carries no reason, so the button is a mystery to the operator")
	}
	// The reason is text on the row, not only a hover. A greyed button whose explanation is
	// reachable only by pointing at it reads as a broken button, and an operator who cannot
	// see which of the three facts refused cannot act on it.
	if !strings.Contains(row, "_fwhy") {
		t.Error("the Forget refusal is a tooltip only, so the row cannot explain itself")
	}
	// The door can still refuse a button the card drew a frame too early, and the press must
	// show that rather than redraw as if it worked. It answers in the dialog the operator is
	// looking at, and it re-reads the row the refusal belongs to.
	if !strings.Contains(asked, "refusal") || !strings.Contains(asked, "pullFound") {
		t.Error("a refused forget is silent: the dialog leaves no word and the row is never re-read")
	}
}

// The tile is the door on the fleet page. The redesign moved the press, the hover and the
// cursor onto the row itself and deleted the read-only name box that only repeated the
// title, so three things now carry the design and none of them is visible to a Go test
// unless it is pinned: the container around the grid must not be a card, the hover must
// belong to the row rather than to a box that opens nothing, and a row the hub will not
// dial must neither point nor light. A later edit that re-frames the list or re-uses
// .card:hover restores the page where every box looked clickable and two of them were not.
func TestTheFleetTileIsTheDoorAndNothingAroundItIs(t *testing.T) {
	page := readWebFile(t, "index.html")
	src := readAppJS(t)
	css := readWebFile(t, "app.css")

	if strings.Contains(page, `id="mn-findcard" class="card"`) || strings.Contains(page, `class="card" id="mn-findcard"`) {
		t.Error("the fleet list is framed as a card again: the grid is a wall of doors, not one door")
	}
	// The only hovers in the stylesheet are a link-card's and a fleet row's. A bare
	// .card:hover puts the highlight back onto containers that open nothing.
	if strings.Contains(css, ".card:hover") && !strings.Contains(css, "a.card:hover") {
		t.Error("app.css highlights a plain .card on hover again, and most .card elements are containers")
	}
	for _, want := range []string{
		".orbgrid li.found:not(.refused):not(.noclick):hover",
		".orbgrid li.found:not(.refused):not(.noclick){cursor:pointer}",
		`li.classList.toggle("noclick", !!o.contended)`,
		`if (e.target.closest("button")) return;`,
		`if (sel && String(sel).length) return;`,
	} {
		if !strings.Contains(css, want) && !strings.Contains(src, want) {
			t.Errorf("the fleet page no longer contains %q: a press anywhere on a tile, or its refusal, is no longer wired", want)
		}
	}
	// The title button is the keyboard's way to the same door the tile opens, so it must
	// inherit the tile's cursor rather than set its own, and must not draw a box inside
	// the tile. A press on it goes through the row's own door, which is the same action.
	pick := css[strings.Index(css, ".orbgrid .found-pick{"):strings.Index(css, ".orbgrid .found-pick:disabled")]
	for _, banned := range []string{"background:var", "border:1px", "cursor:pointer"} {
		if strings.Contains(pick, banned) {
			t.Errorf("the row's title button carries %q: the tile is the button's frame, not the other way round", banned)
		}
	}
	// The tile has no text field, so the row cannot be left showing one name while the
	// operator is typing another.
	if strings.Contains(blockOf(t, src, "function foundRow"), `document.createElement("input")`) {
		t.Error("foundRow builds a text field again: a name box on a tile repeats the tile's own title")
	}
}

var joinCallRe = regexp.MustCompile(`openJoinDialog\(([^)]*)\)`)

// The third parameter of openJoinDialog is the /api/watch skip_ap flag, and a
// truthy value there drops Phase A: the boot verdict, the provisioning AP, and
// the boot scan. On 2026-10-15 the New Orb path passed the orb's NAME into that
// slot, so every fresh flash came back "the scan produced nothing — it came up
// with nothing": a name is a truthy string, Phase A was skipped, and the picker
// was never filled. A call site cannot be trusted to remember which positional
// slot is which, so the call sites are pinned instead.
func TestJoinDialogIsNeverOpenedWithANameInTheSkipFlag(t *testing.T) {
	src := readWebFile(t, "app.js")
	allowed := map[string]bool{`""`: true, "true": true, "false": true, "skipAp": true}
	calls := 0
	for _, line := range strings.Split(src, "\n") {
		trimmed := strings.TrimSpace(line)
		if strings.HasPrefix(trimmed, "function openJoinDialog") {
			continue
		}
		m := joinCallRe.FindStringSubmatch(trimmed)
		if m == nil {
			continue
		}
		calls++
		args := strings.Split(m[1], ",")
		if len(args) != 3 {
			t.Errorf("openJoinDialog wants three arguments, skip flag last: %q", trimmed)
			continue
		}
		if flag := strings.TrimSpace(args[2]); !allowed[flag] {
			t.Errorf("the skip_ap slot got %q, which is not a flag; a name here silences the boot scan: %q",
				flag, trimmed)
		}
	}
	if calls < 2 {
		t.Errorf("only %d join-dialog call sites found; the pin stopped seeing them", calls)
	}
}

// Phase B dials one address for the whole watchLanBudget and used to emit
// nothing at all while it did, behind a dialog whose only text is a fixed
// sentence about rebooting. A wait that sends no news looks exactly like a hung
// tool, and that is what got reported. The loop must speak before it can be
// silent for a budget.
func TestWatchReportsWhileItWaitsForTheOrb(t *testing.T) {
	src := readWebFile(t, "../watch.go")
	loop := src[strings.Index(src, "watchStart := time.Now()"):]
	body := loop[:strings.Index(loop, "unknown\"")]
	if !strings.Contains(body, `"log"`) {
		t.Error("the Phase B poll loop emits no progress event; the join dialog sits on a static sentence for up to 45 s")
	}
	if !strings.Contains(body, "watchStart") {
		t.Error("the Phase B progress event does not say how long it has been waiting")
	}
}

// The hub takes two firmware shapes, and the picker is the only place an operator
// finds that out. A file input that lists .zip alone hides the .bin sitting on the
// desktop, and a panel that reports "1 image(s) @0x0" without saying what that write
// costs is a warning nobody reads.
// The hub takes two firmware shapes, and the modal is the only place an operator
// finds that out. A file input that lists .zip alone hides the .bin sitting on the
// desktop, and a row that reports a filename without naming the project inside it
// leaves the wrong-file catch to happen after the write.
func TestFirmwarePickerAcceptsBothShapes(t *testing.T) {
	page := readWebFile(t, "index.html")
	input := blockOfLine(t, page, `id="ob-fwfile"`)
	for _, want := range []string{".zip", ".bin"} {
		if !strings.Contains(input, want) {
			t.Errorf("the firmware input does not accept %s, so the dialog hides half of what the hub takes", want)
		}
	}
	// Both shapes live behind one choice. The hub decides which it got by reading
	// the file, so the modal must not ask the operator to declare it up front.
	for _, want := range []string{`id="ff-pick"`, `id="ff-file"`, `id="ff-go"`} {
		if !strings.Contains(page, want) {
			t.Errorf("the flash modal is missing %s", want)
		}
	}

	src := readAppJS(t)
	// The probe reply drives the file row, so the project name the hub read is what
	// the operator sees. A row that showed only the filename would let a mislabelled
	// vendor download look exactly like the release it was saved next to.
	picked := blockContaining(t, src, `$("#ob-fwfile").onchange`)
	for _, want := range []string{"r.foreign", "r.project", "r.version"} {
		if !strings.Contains(picked, want) {
			t.Errorf("the file row ignores %s, so a picked file is not identified before it is flashed", want)
		}
	}
	// The build panel still says what the hub carries, in two tags rather than two
	// paragraphs, and still guards the array it takes .map on.
	panel := blockOf(t, src, "async function refreshBuild")
	for _, want := range []string{"b.merged", "b.foreign", "whole-flash", "not gorb-os", "Array.isArray(b.images)"} {
		if !strings.Contains(panel, want) {
			t.Errorf("the firmware panel ignores %s", want)
		}
	}
}

// A foreign blob is a cable-only choice. The gate that used to refuse it existed to
// stop a silent wireless push of a blob nobody owned; over a lead an operator picked
// up, choosing the file and pressing Flash is the confirmation, so the modal sends
// the flag on the first request instead of refusing and offering a retry. These pins
// hold that arrangement: it stays cable-only, and it stays explicit.
func TestForeignBlobIsACableOnlyChoice(t *testing.T) {
	src := readAppJS(t)
	modal := blockOf(t, src, "function openFlashModal")
	// The disk row is hidden unless a cable is in. This is the whole gate now.
	if !strings.Contains(modal, `$("#ff-disk-row").hidden = !port`) {
		t.Error("the disk choice is not gated on a cable, so a foreign blob could reach the wireless path")
	}
	probe := blockOf(t, src, "async function ffProbe")
	if !strings.Contains(probe, `form.append("allow_foreign", "1")`) {
		t.Error("the cable probe does not carry the flag, so a vendor .bin would be refused before it could be shown")
	}
	// The hub still owns the decision: the page never inspects the bytes itself.
	if !strings.Contains(probe, "/api/firmware") {
		t.Error("the file is not handed to the hub, so its shape would be guessed from the suffix")
	}
	commit := blockContaining(t, src, `$("#ff-go").onclick`)
	if !strings.Contains(commit, `form.append("allow_foreign", "1")`) {
		t.Error("the committing upload drops the flag, so the flash the modal promised would be refused")
	}
	// A blob the hub could not identify stays refused: no project name means no
	// label, and an unlabelled confirmation asks for trust rather than attention.
	hub := readFile(t, "firmware.go")
	if !strings.Contains(hub, "func foreignProjectName") {
		t.Error("the hub lost the typed refusal that names a foreign project")
	}
}
func TestFlashDonePanelCanDismissItself(t *testing.T) {
	page := readWebFile(t, "index.html")
	start := strings.Index(page, `id="ob-done"`)
	if start < 0 {
		t.Fatal("the flash-done panel is gone, and with it every exit from a finished flash")
	}
	// The panel spans several lines, so read its element rather than one line.
	end := strings.Index(page[start:], "</div>")
	if end < 0 {
		t.Fatal("the flash-done panel does not close")
	}
	done := page[start : start+end]
	if !strings.Contains(done, `id="ob-close"`) {
		t.Fatal("the flash-done panel offers no way to acknowledge a finished flash and stay on the hub")
	}
	if !strings.Contains(done, "Done") {
		t.Error("the done control is not labelled as finishing the job")
	}
	src := readAppJS(t)
	handler := blockOfLine(t, src, `$("#ob-close").onclick`)
	// It must actually close the dialog, and must not navigate anywhere.
	if !strings.Contains(handler, "closeJoinDialog()") {
		t.Error("Done does not close the join dialog")
	}
	if strings.Contains(handler, "openOrbByIP") {
		t.Error("Done navigates to the orb, which is what the other button already does")
	}
}

// A control whose class matches no rule renders as unstyled text: it is present, it
// works, and it reads as broken. Nothing else in this suite looks at the stylesheet for
// a class the markup invented, which is how the flash modal's two shape tags and the
// card's update badge nearly shipped invisible.
func TestFlashModalClassesAreStyled(t *testing.T) {
	page := readWebFile(t, "index.html")
	css := readWebFile(t, "app.css")
	// Only the two dialogs this change added, and the card row that opens them.
	var markup strings.Builder
	// Only the two dialogs. Each closes at column zero, which is what makes the
	// boundary below exact; an indented element would sweep the region past itself
	// into whichever dialog happens to close next, and accuse this change of classes
	// it never introduced.
	for _, id := range []string{`id="ff-mask"`, `id="rt-mask"`} {
		start := strings.Index(page, id)
		if start < 0 {
			t.Fatalf("%s is gone from the page", id)
		}
		// The dialog's own closing tag is the boundary: the outer one sits alone on its
		// line, so an inner div cannot end the region early and the scan cannot run on
		// into the next dialog's controls, which this change did not add.
		end := strings.Index(page[start:], "\n</div>")
		if end < 0 {
			t.Fatalf("%s does not close", id)
		}
		markup.WriteString(page[start : start+end])
		markup.WriteString("\n")
	}
	used := map[string]bool{}
	for _, m := range regexp.MustCompile(`class="([^"]+)"`).FindAllStringSubmatch(markup.String(), -1) {
		for _, c := range strings.Fields(m[1]) {
			used[c] = true
		}
	}
	if len(used) == 0 {
		t.Fatal("no classes found in the new dialogs, so this test asserts nothing")
	}
	for class := range used {
		// .mask and .dialog are the page's existing shell; .hint and .check the
		// existing form idioms. Each must still have a rule somewhere.
		if !strings.Contains(css, "."+class) {
			t.Errorf("class %q is used by the new dialogs and matched by no rule in app.css", class)
		}
	}
	// The card row's badge and the build panel's shape tags are carried by id
	// selectors, named rather than left to the loop, because an id is not a class.
	for _, sel := range []string{"#ob-build .tag", "#mn-updatebadge"} {
		if !strings.Contains(css, sel) {
			t.Errorf("app.css has no %s rule, so that control renders unstyled", sel)
		}
	}
}
