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
		// Bug 6's shelf preview, pinned the same way as the card paths: the shelf
		// row's own endpoint and the drawer that reads it. If either goes away the
		// Themes face is back to showing a library theme with no face (docs
		// bugfix-batch-2026-10-29.md #6), and nothing else in the suite notices.
		"/api/theme/shelf-preview", "function shelfPreviewPlane",
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
	if !strings.Contains(card, `id="j-manual"`) {
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
	app := readWebFile(t, "app.js")
	if !strings.Contains(tabs, "d.installed") {
		t.Error("the apps face no longer reads d.installed as the ledger; reading d.apps shows the drop zone and reports a full dial as empty")
	}
	if !strings.Contains(tabs, "Array.isArray(d.apps)") {
		t.Error("the apps face lost its drop-zone tier; card candidates with their refusal sentences must still be listed")
	}
	// D38's install door is the hub's STREAMED ladder, not a raw POST at the
	// device: every Install press — a card-resident one included — runs through
	// /api/library/install so the progress panel gets its steps. The route
	// literal lives in app.js's streamInstall, which the row's press calls.
	for _, door := range []string{"/api/v1/apps/uninstall", "/api/v1/apps/clear"} {
		if !strings.Contains(tabs, door) {
			t.Errorf("the apps face lost its %s door", door)
		}
	}
	if !strings.Contains(app, "/api/library/install") {
		t.Error("app.js names no /api/library/install door — the apps face has no install path")
	}
	if !strings.Contains(tabs, "streamInstall(orbIP") {
		t.Error("tabs.js never dials the streamed ladder, so a press would run the raw door again")
	}
	if strings.Contains(tabs, "appPost(\"/api/v1/apps/install") {
		t.Error("a press POSTs the raw device install door again: the silent shortcut that " +
			"leaves an install with no steps and no enable word")
	}
	// Three states, by ruling. The device still answers /apps/enable and the hub still
	// proxies whatever a board offers, but this face must never ASK: an app is installed
	// (in flash, in a slot, on the carousel) or it is not, and a toggle that hid an
	// installed row off the carousel re-introduced the fourth state the operator rejected.
	// Pinning the absence is the point — a future "convenience" switch restored the bug
	// once already by looking like the device's own page.
	if strings.Contains(tabs, "/api/v1/apps/enable") {
		t.Error("the apps face offers an enable toggle again; a row is installed or it is not")
	}
	// The wording is checked where it would actually appear: as a button label. The face's
	// own comment names the removed control in prose, and a gate that could not tell those
	// apart would fail on the very fix it exists to protect.
	if strings.Contains(tabs, "On the carousel") || strings.Contains(tabs, "Turned off") {
		t.Error("the apps face grew a carousel-toggle button again; a row is installed or it is not")
	}
	// The erase that finishes a removal walks BOTH device doors, in this order: free the
	// slot, then delete the container. An uninstall-only button was half the verb and
	// silently left a card file the operator believed gone.
	for _, leg := range []string{"/api/v1/apps/uninstall", "/api/v1/sd/rm"} {
		if !strings.Contains(tabs, leg) {
			t.Errorf("Remove from card no longer walks %s, so it erases less than its label says", leg)
		}
	}
	// S5 deleted the embedded seed and pinned its own deletion; S6 restores the seed from
	// the library under new names, so the assertions below pin the SHAPE the ruling left
	// behind rather than the absence. The old route stays gone (the hub has no /api/orb/seed
	// any more and never re-adds one), and what replaced it must be the library's own route
	// backed by a real pane — a seed that silently reverted to shipped bytes would be the
	// failure this block exists to catch, and it would look exactly like a working feature.
	if strings.Contains(tabs, "/api/orb/seed") || strings.Contains(app, "/api/orb/seed") {
		t.Error("the hub has no /api/orb/seed route; the install is /api/library/install (S6)")
	}
	if strings.Contains(app, "joinSeedApps") || strings.Contains(app, "seedFS") {
		t.Error("app.js references an embedded seed; S6 seeds from the resolved library")
	}
	if !strings.Contains(app, "/api/library/install") {
		t.Error("app.js never dials /api/library/install, so no Library press and no join-time run")
	}
	if !strings.Contains(app, "function autoInstall(") || !strings.Contains(app, "autoInstall(ip)") {
		t.Error("the join no longer arms the seed; a fresh orb boots to an empty shell (S6 §2)")
	}
	if !strings.Contains(app, "installTried") {
		t.Error("the join seed has no once-per-board guard, so a renewing DHCP lease re-seeds a used card")
	}
	if !strings.Contains(tabs, "function renderLibrary(") {
		t.Error("the apps face has no library pane; the seed press has nothing to be pressed from")
	}
	// The invariant //go:embed broke, pinned on the client side: the refusal tier is drawn
	// as its own list, so a file that will not load can never simply be absent.
	if !strings.Contains(tabs, "libCache.refused") {
		t.Error("the library pane hides refusals; an unparseable file must be named on the card")
	}
	// The owner read the resolved-directory line exactly once and called it
	// noise (2026-10-28): a path no button dials and a count that restates the
	// list above. Pinning the ABSENCE is the point — provenance lives in the
	// hub's log, not on an action face. Scoped to renderLibrary: the shelf
	// delete's confirm box NAMING the directory it wipes is not decoration.
	if strings.Contains(tabs, "resolved from") {
		t.Error("the apps face printed the library's resolved directories again")
	}
	rl := tabs[strings.Index(tabs, "async function renderLibrary("):]
	if n := strings.Index(rl, "\n}\n"); n > 0 {
		rl = rl[:n]
	}
	if strings.Contains(rl, "libCache.appsDir") || strings.Contains(rl, "libCache.themesDir") {
		t.Error("renderLibrary prints library directory paths again — the face shows refusals, not paths")
	}
	if strings.Contains(rl, "libCache.apps) || []).length") {
		t.Error("renderLibrary counts apps again; the list above already is the count")
	}
	if !strings.Contains(tabs, "\"version\"") && !strings.Contains(tabs, "a.version") {
		t.Error("the library rows dropped the version stamp, which is the whole point of FORMAT 3")
	}
	// D38: the drop-zone zone and the library section are folded into one row per
	// app. The unified list is the third-party path's home now — the row that
	// names a card file is the row that installs and erases it.
	if !strings.Contains(tabs, "function appRow(") {
		t.Error("the apps face lost its unified row; the card tier's verbs need a home on the list")
	}
	if strings.Contains(tabs, "/api/v1/apps/factory") || strings.Contains(tabs, "factory_missing") {
		t.Error("the apps face speaks a factory tier that exists neither in firmware nor in the hub")
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
	// D37: the chooser's membership is the LEDGER's answer. Deriving it from the
	// theme rows is the bug this slice removes — it made a theme-aware app with no
	// themes written yet look theme-blind, to the person trying to author the first.
	if !strings.Contains(app, "function loadThemeApps(") || !strings.Contains(app, `"/api/theme/apps`) {
		t.Error("the Themes face never dials /api/theme/apps, so its app chooser is back to guessing from installed themes")
	}
	if strings.Contains(app, "for (const t of themeRowsAll)") {
		t.Error("renderThemeTabs derives its apps from themeRowsAll again; the enumeration is the source now")
	}
	if !strings.Contains(app, "function themeEmptyPane(") {
		t.Error("the Themes face lost its per-app empty state, so an app with no themes has nothing to draw in its pane")
	}
	if !strings.Contains(app, "has_editor") {
		t.Error("the Themes face shows an editor button without asking the hub whether one exists")
	}
	if strings.Contains(app, `"color_picker": "HSB"`) {
		t.Error("app.js hard-codes app labels again; titles come off the device's ledger row")
	}
}

// The editor is app-owned (D37): it rides in the container, opens INSIDE the
// Themes pane, and belongs to whichever app its region names. The header's old
// global "Theme Editor" link is the shape this retires — it advertised a global
// editor the system no longer has, and the first bench pass found the real per-app
// door doubly broken: it only painted on an empty pane, and its URL named no board
// so the loader route 400'd. Three pins, one per broken thing.
func TestThemeEditorIsAppOwnedNotAGlobalLink(t *testing.T) {
	page := readWebFile(t, "index.html")
	app := readAppJS(t)
	if strings.Contains(page, `id="composer-link"`) {
		t.Error("the header carries a global Theme Editor link again; the editor belongs to an app and opens in the Themes pane")
	}
	if !strings.Contains(page, `id="mn-theme-editrow"`) {
		t.Error("the Themes pane lost its toolbar Edit slot, so an app that already has themes has no door to its editor")
	}
	if !strings.Contains(app, "function themeEditButton(") || !strings.Contains(app, "function renderThemeEditRow(") {
		t.Error("the Themes pane's Edit door is back to living only inside the empty-pane branch")
	}
	// The argument is the resolved scope, not the raw pressed tab: a one-app dial has no
	// pressed tab and still has one right app to open the editor for.
	if !strings.Contains(app, "renderThemeEditRow(scope, shown.length > 0)") {
		t.Error("loadThemes never fills the toolbar Edit slot, so the door is only real on an empty card")
	}
}

// A board rebooting mid-afternoon is the hub's own doing most of the time, and
// the page has to survive it in the open. Live on 2026-10-26: the operator ran
// a firmware update, the Orb face's zone fetch died against a dial that was
// busy booting, and the card sat reading "reading the board's zone table..."
// forever — the exact frozen-page costume the update overlay used to wear. Two
// sentences keep that class out of the code:
//
//   - loadZonesTable names its failure in the card's own line instead of
//     leaving the fetch's silence painted as progress, re-throws so the
//     lazy ledger drops the key and a later press reloads, and re-dials the
//     read itself on a bounded boot-clock ladder: a board that answers
//     status and starves config has no other mechanism that owns the
//     reload (owner, 2026-10-28, onto an orb mid-boot after a theme store).
//   - The page's return-from-silence path — a wake retry that answers, an
//     update's boot wait, an mDNS frame saying the board is back — calls
//     tabRearm, so the faces the board was showing reload on their own.
func TestZoneFaceTellsTheTruthAndFacesRearmOnReturn(t *testing.T) {
	tabs := readWebFile(t, "tabs.js")
	app := readAppJS(t)
	if !strings.Contains(tabs, "the zone table did not load: ") {
		t.Error("loadZonesTable is back to leaving the card parked on \"reading...\" when the fetch dies")
	}
	if !strings.Contains(tabs, "throw e;") {
		t.Error("loadZonesTable swallows its failure; lazyTab can no longer re-arm the key for a retry")
	}
	// The old line promised "it reloads when the board answers again" while no
	// mechanism owned that reload; the sentence may only return with a ladder
	// behind it, and this pair is the ladder.
	if !strings.Contains(tabs, "zoneWakeTmr = setTimeout(") || !strings.Contains(tabs, "ZONE_WAKE_MAX") {
		t.Error("loadZonesTable's failure line is back to promising a reload it never schedules")
	}
	if !strings.Contains(tabs, "stopZoneWake()") {
		t.Error("the zone wake ladder is no longer retired when a different board opens")
	}
	if strings.Contains(tabs, "it reloads when the board answers again") {
		t.Error("the false reload promise is back in the zone card's failure line")
	}
	if !strings.Contains(tabs, "function tabRearm()") {
		t.Error("tabs.js lost tabRearm, so a board that rebooted could not have its faces re-armed")
	}
	if !strings.Contains(app, "if (wasBack) tabRearm();") {
		t.Error("app.js no longer re-arms the faces when a silent board answers again")
	}
	if !strings.Contains(app, "loadOrb(true); pullFound()") {
		t.Error("the update's boot wait stopped declaring itself a return; faces would reload stale")
	}
}

// Found live on 2026-10-26: the operator pressed Flash while the bench board sat
// on a lead, watched it succeed, and reported two bugs — the dial never showed
// the updating overlay, and nobody could say whether the write went over the
// cable or the air. Neither is the firmware's fault. A board on a cable is
// always flashed by the cable (app.js's press handler checks current.port
// first), and a cable write runs esptool INSTEAD of the app: the firmware that
// draws the overlay is the thing being overwritten, so the glass is dark by
// physics, not by bug. The dialog's own note said "over the cable" in small
// type and the button said only "Flash" — the channel must be named where the
// hand goes, on the button, on the card's headline, and in the note about the
// dark dial.
func TestFlashNamesItsChannelBeforeThePress(t *testing.T) {
	app := readAppJS(t)
	for _, want := range []string{
		`current.port ? "Flash over cable" : "Update over Wi-Fi"`, // the button, before the press
		`"Flash anyway over Wi-Fi"`,                               // the forced second press keeps the channel
		`flashed over the cable on `,                              // the card's headline after a cable write
		`staged and rebooting over Wi-Fi: `,                       // and after a wireless one
		`The dial shows nothing through a cable write`,            // why the glass stays dark on the lead
	} {
		if !strings.Contains(app, want) {
			t.Errorf("app.js no longer says %q; the operator cannot tell cable from Wi-Fi before pressing", want)
		}
	}
	if strings.Contains(app, `go.textContent = ffForced ? "Flash anyway" : "Flash";`) {
		t.Error("ffGate is back to the channel-blind label")
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
// card asks first. An ask that goes missing turns an accidental click into a deleted
// theme, and an ask the browser owns is an ask that cannot name what it costs.
func TestThemeDeleteAsksBeforeRemoving(t *testing.T) {
	body := blockOf(t, readAppJS(t), "function themeRow")
	if !strings.Contains(body, "askConfirm(") {
		t.Error("the delete button must ask through the hub's modal before calling /api/theme/delete")
	}
	// The modal exists to retire the browser dialog: it cannot be styled, cannot say
	// what the press costs, and lands focus on OK so a stray Enter approves a deletion.
	if n := strings.Count(body, " confirm(") + strings.Count(body, "(!confirm("); n != 0 {
		t.Errorf("the theme row reaches for a native confirm() %d times; the hub's own modal "+
			"is the one ask this face may make", n)
	}
	if !strings.Contains(body, "/api/theme/delete") {
		t.Error("themeRow is expected to be the only caller of /api/theme/delete")
	}
	// D38: deleting the ACTIVE theme now clears the wall too — the slot is erased
	// and the dial reboots to its no-theme face. The confirm has to say so BEFORE
	// the press (the rule lives in the firmware; the face must not be surprised),
	// and the answer's deactivated word drives the reboot wait after it.
	if !strings.Contains(body, "no-theme face") {
		t.Error("the remove confirm does not name the deactivation the active theme's press causes")
	}
	if !strings.Contains(body, "d.deactivated") {
		t.Error("the row ignores the device's deactivated answer, so the reboot wait never runs")
	}
}

// Three of this face's captions described the wrong thing, and a button's label is the
// whole of its documentation. "Edit Clock themes…" sat over a window that composes a
// theme, and on an orb with none it had nothing to edit. "Add a theme…" put a file on
// the hub's shelf — an import — while the install that follows it is a different press
// on a different row. "Re-read card" described a reread of something the hub was
// already holding, when the press dials the orb and asks it to re-scan its own card.
func TestThemeCardCaptionsSayWhatTheyDo(t *testing.T) {
	app := readAppJS(t)
	html := readWebFile(t, "index.html")
	edit := blockOf(t, app, "function themeEditButton(")
	if !strings.Contains(edit, `btn.textContent = "Compose new theme"`) {
		t.Error(`the authoring door is not labelled "Compose new theme"`)
	}
	if !strings.Contains(edit, "themeAppTitle(") {
		t.Error("the authoring door no longer says which app's editor it opens")
	}
	if !strings.Contains(app, "Use Compose new theme below") {
		t.Error("the empty pane tells the operator to use a door with a different name")
	}
	for _, c := range []struct{ id, want, why string }{
		{"mn-rescan", "Refresh from Orb",
			"the press dials the orb and asks it to re-scan its card"},
		{"mn-up-btn", "Import a theme",
			"the press puts a file the hub did not have onto the hub's shelf"},
	} {
		at := strings.Index(html, `id="`+c.id+`"`)
		if at < 0 {
			t.Fatalf("index.html lost #%s", c.id)
		}
		end := strings.Index(html[at:], "</button>")
		if end < 0 || end > 600 {
			t.Fatalf("#%s is no longer a captioned button", c.id)
		}
		if !strings.Contains(html[at:at+end], c.want) {
			t.Errorf("#%s does not read %q: %s", c.id, c.want, c.why)
		}
	}
	for _, gone := range []string{"Add a theme", "Re-read card", "Upload theme"} {
		if strings.Contains(html, gone) || strings.Contains(app, "press "+gone) {
			t.Errorf("%q is back: a prompt and the caption it names must be the same words", gone)
		}
	}
	if n := strings.Count(app, "press Import a theme again"); n != 2 {
		t.Errorf("%d second-press prompts name the import door; both refusal paths must", n)
	}
}

// D38: the Themes face follows the Apps face's pattern — one list of everything
// the hub can see (the orb's card AND the hub's folder), and three verbs:
// activate a card file, install a shelf file, remove a card file from the card.
// Themes are gone from the Apps page entirely: the tabs check is the same one
// the shelf row used to be reachable by, now pinned against tabs.js.
func TestThemesFaceFollowsTheAppsPattern(t *testing.T) {
	app := readAppJS(t)
	tabs := readWebFile(t, "tabs.js")
	row := blockOf(t, app, "function themeRow")
	for _, want := range []string{`use.textContent = "Activate"`, `del.textContent = "Remove from card"`} {
		if !strings.Contains(row, want) {
			t.Errorf("the orb theme row has no %s; the row's verbs are activate and remove-from-card", want)
		}
	}
	shelf := blockOf(t, app, "function themeShelfRows")
	for _, want := range []string{`inst.textContent = "Install"`, "Remove from hub library", "/api/library/push?kind=theme"} {
		if !strings.Contains(shelf, want) {
			t.Errorf("the shelf theme row has no %s; a hub-folder theme must be installable from this face", want)
		}
	}
	if strings.Contains(tabs, "libThemeRow") || strings.Contains(tabs, "Install theme") {
		t.Error("theme rows are back on the Apps page; the Themes face owns themes")
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

// The library card's two Install presses are the only mutating controls on the face, and a
// press that reaches no route is indistinguishable from a card that did nothing. This is the
// same class of bug the selector check above exists for: it fails silently in a browser.
func TestLibraryPressesReachRoutesTheHubServes(t *testing.T) {
	// The routes are posted by app.js's helper and tabs.js's picker; both sides are named so a
	// rename on either one fails here rather than silently in a browser.
	app := readWebFile(t, "app.js")
	tabs := readWebFile(t, "tabs.js")
	if !strings.Contains(app, "/api/library/install?ip=") {
		t.Error("app.js never posts to /api/library/install, so the Install presses answer nothing")
	}
	// Import files the shelf and stops: it always keeps, and it never installs. The install
	// flag leaving this call site is the point — the row that results carries the Install
	// button, so an imported app travels the same deliberate path as one already on the hub.
	if !strings.Contains(tabs, "/api/apps/import?keep=1") {
		t.Error("the import press no longer asks the hub to keep the file")
	}
	if strings.Contains(tabs, "install=1&ip=") {
		t.Error("the import still installs onto the orb in hand; that is the row's job now")
	}
	// The delete button reaches the hub's one library-removal door.
	if !strings.Contains(tabs, "/api/library/rm?") {
		t.Error("the library row offers Remove from hub library and posts to no route")
	}

	if !strings.Contains(tabs, "streamInstall(orbIP, e.slug") {
		t.Error("tabs.js never calls streamInstall(orbIP, e.slug, so the row's Install/Update presses are wired to nothing")
	}
	// D38: a theme has no card-only half-step. The shelf's theme reaches the dial
	// through /api/library/push, whose upload door stores on the device's own
	// promise — the Themes face's Install press dials exactly that route.
	if !strings.Contains(app, "/api/library/push?kind=theme") {
		t.Error("app.js never posts kind=theme to /api/library/push, so a shelf theme has no Install door")
	}
	if !strings.Contains(tabs, "/api/library/push?kind=app&name=") {
		t.Error("the unified row's Add to card button reaches no route; the filing verb is decoration")
	}
	if strings.Contains(tabs, "/api/library/seed") || strings.Contains(app, "/api/library/seed") {
		t.Error("the client still posts to the renamed /api/library/seed route")
	}
	if !strings.Contains(app, "async function streamInstall(") {
		t.Error("no streamInstall reader in app.js")
	}
}

// The complaint this mandate pins: a press wrote one line into a <pre> beneath the row and
// then said nothing until the whole run finished, which for a FORMAT 3 container over WiFi
// reads as a hang. The press now opens the shared flash panel before the first byte leaves
// and streams the hub's steps into it.
func TestLibraryInstallOpensTheSharedProgressPanel(t *testing.T) {
	tabs := readWebFile(t, "tabs.js")
	app := readWebFile(t, "app.js")
	// Each press handler is checked inside its own body: counting fpOpen calls file-wide would
	// pass on the manage face's unrelated flash panel and miss a Library press that stayed
	// silent, which is the exact bug this mandate exists for.
	for _, probe := range []struct {
		label string
		mark  string
	}{
		// The app press moved into installLibButton, which serves Install and Upgrade from
		// one body: the label arrives as the `name` parameter, so the probe is the parameter
		// assignment rather than a literal that no longer appears.
		{"the app row's Install press", "btn.textContent = name;"},
		// D38: a theme's Install press is the shelf row in app.js's themeShelfRows,
		// and its progress is one bar that fills while the push streams — the push
		// route speaks the install ladder's own NDJSON dialect, so the bar moves
		// with real bytes.
		{"the theme row's Install press", `inst.textContent = "Install";`},
	} {
		src := tabs
		if probe.label == "the theme row's Install press" {
			src = app
		}
		at := strings.Index(src, probe.mark)
		if at < 0 {
			t.Errorf("%s is not in its file at all", probe.label)
			continue
		}
		// The body is the handler alone: from its own `btn.onclick` to the two-space-indented
		// brace that closes it. A looser cut ran past the app press into the theme press and
		// let a silent app button pass on the theme's fpOpen.
		onslice := "btn.onclick = async function"
		verdict := "if (ok) fpClose();"
		wants := []string{"fpOpen();", "fpUI()", "streamInstall(orbIP"}
		if probe.label == "the theme row's Install press" {
			onslice = "inst.onclick = async () =>"
			verdict = "fpClose();"
			wants = []string{"fpOpen();", "fpUI()", "/api/library/push?kind=theme"}
		}
		onclick := strings.Index(src[at:], onslice)
		if onclick < 0 {
			t.Errorf("%s has no click handler", probe.label)
			continue
		}
		body := src[at+onclick:]
		if n := strings.Index(body, "\n  };"); n > 0 {
			body = body[:n]
		} else if n := strings.Index(body, "\n    };"); n > 0 {
			body = body[:n]
		}
		for _, want := range wants {
			if !strings.Contains(body, want) {
				t.Errorf("%s does no %s, so the press has no window while the container crosses the room",
					probe.label, want)
			}
		}
		// Success may close the panel; a refusal may not, because the refusal IS the panel's
		// last word and Close is the operator's decision.
		if !strings.Contains(body, verdict) {
			t.Errorf("%s does not close the panel on success, or the close is not scoped to it",
				probe.label)
		}
	}
	// The bar is fed by the hub's byte-progress steps, and the headline by every step.
	for _, want := range []string{`action === "uploading"`, "frame.doing.textContent", "frame.bar.style.width"} {
		if !strings.Contains(app, want) {
			t.Errorf("app.js has no %s, so a mid-upload step cannot move the panel", want)
		}
	}
	if !strings.Contains(app, "frame.log.textContent") {
		t.Error("the install steps no longer reach a visible frame")
	}
}

// The post-flash wait is one line per tick, and two sentences was the reported verbosity.
func TestPostFlashWaitLinesAreOneSentence(t *testing.T) {
	app := readWebFile(t, "app.js")
	if !strings.Contains(app, "of ") || !strings.Contains(app, "waiting for the orb to restart") {
		t.Error("the post-flash wait lost its elapsed-of-total or its next-action half")
	}
	for _, gone := range []string{"and will announce itself when it is back", "if it goes quiet for too long"} {
		if strings.Contains(app, gone) {
			t.Errorf("the post-flash wait carries the removed tail %q", gone)
		}
	}
}

// D36 §7: the shell is not an app the face offers. Two separate things kept naming it —
// APP_TITLES carried a label for it, and loadApps drew every row the ledger returned,
// compiled ones included, so the operator got a fourth sub-tab whose whole body was the
// sentence "built into the firmware — always on". A row with no verb on it is not an app.
func TestAppsFaceNeverListsTheBuiltInShell(t *testing.T) {
	tabs := readWebFile(t, "tabs.js")
	at := strings.Index(tabs, "const APP_TITLES")
	if at < 0 {
		t.Fatal("APP_TITLES is gone from tabs.js")
	}
	line := tabs[at:]
	if n := strings.Index(line, "\n"); n > 0 {
		line = line[:n]
	}
	if strings.Contains(line, "menu") {
		t.Errorf("the shell is back in the face's label table: %s", strings.TrimSpace(line))
	}
	// The filter lives in the load itself, not in one caller: a later face that reads
	// appList inherits the decision rather than re-deriving it.
	load := tabs[strings.Index(tabs, "async function loadApps("):]
	if n := strings.Index(load, "\n}\n"); n > 0 {
		load = load[:n]
	}
	if !strings.Contains(load, ".compiled") {
		t.Error("loadApps no longer filters on compiled, so the shell is listed again")
	}
	// A quarantined built-in is the one compiled state with a verb on it — Clear fault —
	// and dropping it would hide the only row explaining a half-drawing dial.
	if !strings.Contains(load, "a.failed") {
		t.Error("loadApps drops failed built-ins, so a quarantined shell has no Clear fault door")
	}
	// And the header stays quiet about what it filtered. The one compiled row
	// every board carries is the shell, and a permanent "1 built-in not listed"
	// on every load is the noise an owner called unnecessary (2026-10-28): a
	// row with no verb and no question has no business in a count.
	if strings.Contains(load, "built-in") {
		t.Error("the Apps state line counts built-ins again; a filtered row with no verb is not a fact to print")
	}
}

// The report this mandate exists for: Install said "installed on the orb in hand" and the
// list beside it still had no such app — no sub-tab, no On/Off toggle, no Uninstall door
// for the thing the operator had just spent sixty seconds putting on the dial. The press
// owns the re-read; nothing else on this face refreshes the ledger by itself.
func TestLibraryInstallRefetchesTheLedgerItJustChanged(t *testing.T) {
	tabs := readWebFile(t, "tabs.js")
	for _, probe := range []struct {
		label  string
		file   string // "tabs" or "app"
		mark   string
		want   string
		surviv string
	}{
		{"the app row's Install press", "tabs", "btn.textContent = name;", "loadApps()", "libVerdict"},
		// D38: the shelf theme's Install re-reads through loadThemes, and its own
		// refusal line is the survivor sentence — the push has no row verdict to keep.
		{"the theme row's Install press", "app", `inst.textContent = "Install";`, "loadThemes(themeIP)", "install refused"},
	} {
		src := tabs
		if probe.file == "app" {
			src = readWebFile(t, "app.js")
		}
		at := strings.Index(src, probe.mark)
		if at < 0 {
			t.Errorf("%s is not in %s.js at all", probe.label, probe.file)
			continue
		}
		body := src[at:]
		if n := strings.Index(body, "\n  };"); n > 0 {
			body = body[:n]
		} else if n := strings.Index(body, "\n    };"); n > 0 {
			body = body[:n]
		}
		if !strings.Contains(body, probe.want) {
			t.Errorf("%s never re-reads %s, so the dial's new row waits for a tab press to appear",
				probe.label, probe.want)
		}
		// The verdict the press earned has to survive that re-render, or the re-read
		// deletes the only sentence answering the press.
		if !strings.Contains(body, probe.surviv) {
			t.Errorf("%s re-renders straight over its own answer", probe.label)
		}
	}
	// A row the ledger lists says so wherever it is drawn, not only after a press.
	if !strings.Contains(tabs, "on the dial\"") {
		t.Error("library rows no longer mark the containers the ledger says are installed")
	}
}

// Uninstall leaves the container on the card by the device's own design, so the drop zone
// is the only place an operator can finish the job, reclaim the space, or clear a file the
// dial refuses. The verb is the device's file-manager door, the same one Files drives.
// Uninstall leaves the container on the card by the device's own design, so the
// unified row is the only place an operator can finish the job, reclaim the space,
// or clear a file the dial refuses. The verb is the device's file-manager door,
// the same one Files drives — and since D38 it hangs off the same row that owns
// the card chip, not off a separate drop-zone pane.
func TestCardDropZoneCanEraseAContainer(t *testing.T) {
	tabs := readWebFile(t, "tabs.js")
	at := strings.Index(tabs, "function eraseCardButton(")
	if at < 0 {
		t.Fatal("eraseCardButton is gone from tabs.js")
	}
	zone := tabs[at:]
	if n := strings.Index(zone, "\nfunction "); n > 0 {
		zone = zone[:n]
	}
	for _, want := range []string{"Remove from card", "/api/v1/sd/rm?path=", "askConfirm("} {
		if !strings.Contains(zone, want) {
			t.Errorf("the card row has no %s, so a card container cannot be erased from this face", want)
		}
	}
	// The path is the device's own filed name: apps/, .orbapp, and the listing's
	// OWN stem — a file that arrived under a stem other than its slug is erased
	// under the name the card actually holds, not the one the slug hopes for.
	if !strings.Contains(zone, `"apps/" + stem + ".orbapp"`) {
		t.Error("the delete does not name the card path the listing filed the file at")
	}
	// The door hangs off the bare card fact, not the ok-only branch: a refused
	// row is the likeliest one to want erasing.
	rw := tabs[strings.Index(tabs, "function appRow("):]
	if n := strings.Index(rw, "\nfunction "); n > 0 {
		rw = rw[:n]
	}
	if strings.Contains(rw, "if (c.ok)") && strings.Index(rw, "if (c.ok)") < strings.Index(rw, "eraseCardButton(") {
		t.Error("the remove door sits behind an ok-only guard, so a refused container cannot be cleared")
	}
	if !strings.Contains(rw, "if (c)") || strings.Index(rw, "if (c)") > strings.Index(rw, "eraseCardButton(") {
		t.Error("the row no longer offers Remove from card on the bare fact that the card holds it")
	}
}

// The Themes face drew its chooser only above one app, so a dial with a single
// theme-capable app — every dial in the fleet today — showed a list of themes belonging to
// nobody. The chooser names the pane; one app still has exactly one right name.
func TestThemesFaceNamesItsAppEvenWithOnlyOne(t *testing.T) {
	app := readWebFile(t, "app.js")
	at := strings.Index(app, "function renderThemeTabs(")
	if at < 0 {
		t.Fatal("renderThemeTabs is gone from app.js")
	}
	tabs := app[at:]
	if n := strings.Index(tabs, "\n}\n"); n > 0 {
		tabs = tabs[:n]
	}
	if strings.Contains(tabs, "apps.length < 2") {
		t.Error("renderThemeTabs still bails below two apps, so a one-app orb names nothing")
	}
	if !strings.Contains(tabs, "apps.length === 1") {
		t.Error("renderThemeTabs has no single-app branch")
	}
	if !strings.Contains(tabs, `" themes"`) {
		t.Error("the single-app tab does not say themes, so the pane wears no label")
	}
	// The single-app branch drew a BUTTON whose click re-read the list already on
	// screen, which is a second door into the room the operator is standing in and a
	// card read priced as a no-op. One theme-capable app has nothing to choose, so the
	// pane wears a heading.
	if strings.Contains(tabs, "mk(apps[0].slug") {
		t.Error("the one-app pane is back to a button whose only act is to re-read what it shows")
	}
	if !strings.Contains(tabs, `"tablabel"`) {
		t.Error("the one-app pane has no static label to name itself with")
	}
	// One answer, shared by the chooser, the state line, the empty pane and the Edit row.
	// Four private guesses is the shape the bug had.
	if !strings.Contains(app, "function themeFaceScope(") {
		t.Error("no themeFaceScope: the face is back to inferring its app in four places")
	}
	if n := strings.Count(app, "themeAppsRows.length === 1 ? themeAppsRows[0].slug"); n > 1 {
		t.Errorf("%d sites still re-derive the one-app answer beside themeFaceScope", n)
	}
	if !strings.Contains(app, "themeAppTitle(scope)") {
		t.Error("the Themes state line does not name the app whose themes it counted")
	}
}

// The join-time seed is supposed to leave a shelf the owner emptied alone. It read the
// count off /api/fleet, and the store became an address book that carries no such field,
// so the guard became `if (undefined) return` and every rejoin refilled the board. The
// mandate is the read itself and the built-in exclusion: h_apps_list's installed tier
// carries the shell, so a count that includes it never reads empty and the seed dies.
//
// The gate moved hubs on 2026-10-29. Held by the page, it sat ahead of the install route and
// could not see the adoption mark, so a lonely-erased board — whose SD card keeps every
// container the last orb life wrote, because a ROM chip erase does not touch the card — read
// as an owner's shelf and was never seeded. The rule is the same one, in shelfHoldsOwnersContent,
// now behind the slate check instead of in front of the run.
func TestAutoInstallGatesOnTheLedgerNotOnAVanishedField(t *testing.T) {
	app := readWebFile(t, "app.js")
	at := strings.Index(app, "async function autoInstall(")
	if at < 0 {
		t.Fatal("autoInstall is gone from app.js")
	}
	body := app[at:]
	if n := strings.Index(body, "\n}\n"); n > 0 {
		body = body[:n]
	}
	if strings.Contains(body, `getJSON("/api/fleet")`) {
		t.Error("the join gate still reads /api/fleet, whose rows carry no app count")
	}
	if !strings.Contains(body, "installTried.has(ip)") {
		t.Error("the join gate lost its one-attempt-per-address memory")
	}

	// The gate is a hub rule now, and it still reads a real ledger with the built-in
	// excluded. A hub-side gate that counted the shell would never see an empty board.
	src, err := os.ReadFile("installflow.go")
	if err != nil {
		t.Fatalf("installflow.go: %v", err)
	}
	fn := string(src)
	at = strings.Index(fn, "func (s *Server) shelfHoldsOwnersContent")
	if at < 0 {
		t.Fatal("the join-time shelf gate is gone from the hub, so nothing protects an owner's shelf")
	}
	fn = fn[at:]
	if n := strings.Index(fn, "\n}\n"); n > 0 {
		fn = fn[:n]
	}
	if !strings.Contains(fn, "AppsList") {
		t.Error("the hub's shelf gate reads no ledger, so it cannot know whether the shelf is empty")
	}
	if !strings.Contains(fn, "Compiled") {
		t.Error("the hub's shelf gate counts built-ins, so no dial ever answers as empty")
	}
}

// §1–§3 of the library-install ruling: three app states, a version-aware library row, and
// an import that imports. Every one of these is a text gate on the shipped face, because
// the failure mode the operator named was the face offering a verb the product decided not
// to have, and no amount of Go-side correctness shows up in a browser.
func TestAppsFaceOffersThreeStatesAndNoToggle(t *testing.T) {
	tabs := readWebFile(t, "tabs.js")

	// The erase that finishes a removal walks the device's two doors in order: free the
	// slot, then delete the container. Reversed, the sd rm erases the only copy the
	// uninstall still points at and the row cannot be reinstalled from the card.
	free := strings.Index(tabs, `/api/v1/apps/uninstall?id=`)
	rm := strings.Index(tabs, `/api/v1/sd/rm?path=`+`"+enc`)
	if free < 0 {
		free = strings.Index(tabs, "/api/v1/apps/uninstall?id=")
	}
	if rm < 0 {
		rm = strings.Index(tabs, "/api/v1/sd/rm?path=")
	}
	if free < 0 || rm < 0 {
		t.Fatalf("the erase button lost a leg (uninstall=%d sd/rm=%d)", free, rm)
	}
	// The order test runs inside eraseCardButton only, so the whole file matching is not
	// enough — scope the window to that function.
	start := strings.Index(tabs, "function eraseCardButton")
	if start < 0 {
		t.Fatal("eraseCardButton is gone; the one button that finishes a removal is the point of §1")
	}
	end := strings.Index(tabs[start:], "\nfunction ")
	body := tabs[start:]
	if end > 0 {
		body = tabs[start : start+end]
	}
	u := strings.Index(body, "/api/v1/apps/uninstall?id=")
	r := strings.Index(body, "/api/v1/sd/rm?path=")
	if u < 0 || r < 0 || u > r {
		t.Errorf("eraseCardButton must uninstall before it erases the container (uninstall at %d, sd/rm at %d)", u, r)
	}
	// The state the button exists for: installed AND still on the card, which is what an
	// ordinary Uninstall leaves behind. The button has to be offered on an installed row.
	face := tabs[strings.Index(tabs, "function appRow("):]
	if next := strings.Index(face, "\nfunction "); next > 0 {
		face = face[:next]
	}
	if !strings.Contains(face, "eraseCardButton(") {
		t.Error("the installed row offers no Remove from card, so no button finishes a removal")
	}
	if strings.Contains(face, "/api/v1/apps/enable") {
		t.Error("the installed row grew an enable toggle again")
	}
}

// The unified row (D38) is the face's whole verb table, and the ruling it has
// to carry is mutual exclusion: the dial holds a slot or it does not, and one
// row never shows Install and Uninstall at once. An owner pressed Install on a
// row the hub had marked "installed, switched off" and the row answered
// "installed" while nothing moved — the state between the two verbs is not a
// state this face keeps. Update appears only on the ordinals' say-so, and a
// shelf copy can always be deleted.
func TestLibraryRowYieldsToWhatTheDialAlreadyRuns(t *testing.T) {
	tabs := readWebFile(t, "tabs.js")
	start := strings.Index(tabs, "function appRow(")
	if start < 0 {
		t.Fatal("appRow is gone")
	}
	end := strings.Index(tabs[start:], "\nfunction ")
	body := tabs[start:]
	if end > 0 {
		body = tabs[start : start+end]
	}

	// Install requires the ABSENCE of a slot (not a) — that is what makes the
	// two doors mutually exclusive by construction, and it is also why a
	// current app never offers to reinstall: a slot's presence hides the door.
	if !strings.Contains(body, "if (!a && (f || (c && c.ok)) && !updateGate(e))") {
		t.Error("appRow's Install door is not gated on the absence of a slot, so one row " +
			"can offer Install and Uninstall at once")
	}
	// Uninstall is the exact complement: a slot, not built in.
	if !strings.Contains(body, "if (a && !a.compiled)") {
		t.Error("appRow's Uninstall door no longer keys on the slot alone")
	}
	// Update is the same door with newer bytes behind it, and only when the ordinals
	// say so. The gate lives in updateGate, and it must refuse both a same-release
	// card (>= 0 comparison) and a row where nothing was ever present.
	ug := tabs[strings.Index(tabs, "function updateGate("):]
	if n := strings.Index(ug, "\nfunction "); n > 0 {
		ug = ug[:n]
	}
	if !strings.Contains(ug, "verCompare(e.card.version, e.lib.version) >= 0") ||
		!strings.Contains(ug, "verCompare(e.installed.version, e.lib.version) >= 0") {
		t.Error("updateGate does not refuse same-or-newer stamps; an unstamped or current pair would read as an update")
	}
	if !strings.Contains(ug, "if (!e.card && !e.installed) return false") {
		t.Error("updateGate offers an Update for an app that is nowhere — that row's verb is Install")
	}
	if !strings.Contains(body, "removeFromLibraryButton") {
		t.Error("appRow no longer offers to delete the hub's own copy")
	}
	if !strings.Contains(tabs, "/api/library/rm?kind=app") {
		t.Error("the library delete never dials /api/library/rm, so the button is decoration")
	}
}

func TestImportImportsAndNeverInstalls(t *testing.T) {
	page := readWebFile(t, "index.html")
	tabs := readWebFile(t, "tabs.js")

	if strings.Contains(page, `id="lib-keep"`) || strings.Contains(page, `id="lib-install"`) {
		t.Error("the import row grew its checkboxes back; import files the library and the row's Install moves it onto a board")
	}
	if !strings.Contains(page, "Import an app") {
		t.Error("the button still says \"Add an app\"; the verb that matters is that nothing installs")
	}
	// The press must send keep and must NOT send install, in either direction: a face that
	// quietly installed would do the thing the operator just asked it to stop doing.
	start := strings.Index(tabs, "function wireLibraryImport")
	if start < 0 {
		t.Fatal("wireLibraryImport is gone")
	}
	end := strings.Index(tabs[start:], "\nfunction ")
	body := tabs[start:]
	if end > 0 {
		body = tabs[start : start+end]
	}
	if !strings.Contains(body, "/api/apps/import?keep=1") {
		t.Error("the import no longer asks the hub to keep the file")
	}
	if strings.Contains(body, "install=1") {
		t.Error("the import sends install=1; it files into the library and stops there")
	}
}
