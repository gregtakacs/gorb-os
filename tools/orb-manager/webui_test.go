package main

import (
	"os"
	"regexp"
	"strings"
	"testing"
)

// The page script and the shipped markup are one program with two files, and
// nothing compiled them together: an id renamed in one and not the other fails
// at click time, in the dialog, with the hub nowhere near the error. The join
// dialog's boot lock learned this the hard way — a missing #wj-lock makes
// joinLock() throw, and the dialog then opens locked open forever.

func TestPageScriptIdsAllExistInTheMarkup(t *testing.T) {
	js, err := os.ReadFile("web/app.js")
	if err != nil {
		t.Fatalf("app.js: %v", err)
	}
	html, err := os.ReadFile("web/index.html")
	if err != nil {
		t.Fatalf("index.html: %v", err)
	}
	page := string(html)
	seen := map[string]bool{}
	for _, m := range regexp.MustCompile(`\$\("#([a-z0-9-]+)"\)`).FindAllStringSubmatch(string(js), -1) {
		id := m[1]
		if seen[id] {
			continue
		}
		seen[id] = true
		if !strings.Contains(page, `id="`+id+`"`) {
			t.Errorf(`app.js drives $("#%s") but index.html has no id="%s"`, id, id)
		}
	}
	if len(seen) < 40 {
		t.Errorf("only %d ids scraped — the pattern stopped matching the file", len(seen))
	}
}

// The join dialog's boot phase is a lock with three parts: the spinner block,
// the form it replaces, and the exits (Cancel, backdrop, Escape) that must
// stay live through both. Pin the ones a partial rewrite once left unhooked.
func TestJoinDialogBootPhaseIsWired(t *testing.T) {
	js, err := os.ReadFile("web/app.js")
	if err != nil {
		t.Fatalf("app.js: %v", err)
	}
	src := string(js)
	for _, want := range []string{
		`function joinLock(`,
		`$("#wj-lock")`,
		`$("#sp-rescan").onclick`,
		`command: "scan"`,
		`async function serialRescan(`,
		`#sp-pass-show`,
		`#j-pass-show`,
		`d.data.zones`, // the {ok,data} envelope, not a bare .zones
	} {
		if !strings.Contains(src, want) {
			t.Errorf("app.js lost %q — the boot-locked join form depends on it", want)
		}
	}
}

// The forget dialog is one mask with three bodies, and the body it shows is chosen from
// the row's CURRENT frame — that is what keeps an open dialog honest when the board it
// names leaves the bench. Pin the three bodies, the repaint hook, and the two things that
// must stay gone: the browser popup, and the frozen port a stale dialog once flashed on.
func TestForgetDialogFollowsTheRowItCameFrom(t *testing.T) {
	js, err := os.ReadFile("web/app.js")
	if err != nil {
		t.Fatalf("app.js: %v", err)
	}
	src := string(js)
	page, err := os.ReadFile("web/index.html")
	if err != nil {
		t.Fatalf("index.html: %v", err)
	}
	markup := string(page)
	for _, id := range []string{"fj-mask", "fj-title", "fj-who", "fj-cable", "fj-gone", "fj-gone-what",
		"fj-wait", "fj-note", "fj-log", "fj-cancel", "fj-lonely", "fj-reset", "fj-forget"} {
		if !strings.Contains(markup, `id="`+id+`"`) {
			t.Errorf("index.html lost the forget dialog's #%s", id)
		}
	}
	for _, want := range []string{
		`function forgetMode(`,                              // the one place the body is chosen
		`function paintForgetDialog(`,                       // the one place the body is written
		`if (fjRow === li && !fjBusy) paintForgetDialog();`, // an open dialog follows its row
		`if (entry[1] === fjRow) closeForgetDialog();`,      // a row that leaves closes the dialog
		`$("#fj-mask").hidden = false;`,
	} {
		if !strings.Contains(src, want) {
			t.Errorf("app.js lost %q — the forget dialog is no longer driven by the row it opened on", want)
		}
	}
	// The two ways this dialog used to lie about a board that is no longer plugged in.
	// The port it names may only come from livePort, so a frame that carries no lead can
	// never leave an old one on screen; and the cable endings may only be sent to the port
	// the hub reports at press time, which the frozen dataset.port made impossible.
	if strings.Contains(src, "_livePort || (li._port") || strings.Contains(src, "dataset.port") {
		t.Error("the forget dialog can name a port the hub no longer sees")
	}
	// Forget is the hub's verdict and the page's dialog. Across the whole forget section —
	// the dialog, its three bodies and the row's press — no native popup may appear. The
	// theme card keeps its own confirm for a file on an SD card, which is a different
	// question with no hub verdict behind it, so the pin is scoped to this section.
	start := strings.Index(src, "// ---- the forget dialog")
	finish := strings.Index(src, "// ---- post-flash onboarding")
	if start < 0 || finish < 0 || finish <= start {
		t.Fatal("the forget dialog section cannot be located — its marker moved")
	}
	if section := src[start:finish]; strings.Contains(section, "confirm(") {
		t.Error("the forget section raises a browser popup; the dialog is the only ask it has")
	}
}

// The hub asks a board on a lead the free question, and the page is the half that has to
// say the result honestly in both directions: an answer is a card, and a silence is a
// named silence rather than the old "none on a cable" that sent an operator to check a
// wire that was fine.
func TestTheFleetHeaderSaysWhatHappenedOnTheCable(t *testing.T) {
	src := readAppJS(t)
	for _, want := range []string{
		// The count of rows the hub could not prove, taken before the filter drops them.
		`const unproven = raw.filter((o) => isBlankRow(o) && o && o.onCable).length;`,
		`silent when asked`, // a lead with a silent chip on it
		`"no orb seen: none announced, and none on a cable"`, // and a bench with no lead at all
	} {
		if !strings.Contains(src, want) {
			t.Errorf("app.js lost %q — the empty fleet list can lie about the cable again", want)
		}
	}
	// The two sentences must stay in the order that chooses them: the silent-board line is
	// picked on `unproven`, ahead of the no-cable-at-all fallback.
	if strings.Index(src, "? unproven +") > strings.Index(src, `"no orb seen: none announced`) {
		t.Error("the silent-cable sentence is no longer chosen before the no-cable fallback")
	}
	// The promise the card makes in prose has to match the code above it.
	page, err := os.ReadFile("web/index.html")
	if err != nil {
		t.Fatalf("index.html: %v", err)
	}
	if !strings.Contains(string(page), "asked what it is the moment the") {
		t.Error("index.html no longer tells the operator the hub asks a board on a lead")
	}
}

// The lonely door says goodbye once, in the dialog's own log, and never again on the
// card. The sentence removed here (bench, 2026-10-15: "it is stupid and doesn't mean
// anything") narrated a screen the hub cannot see, and the same words existed to say it:
// the row left the list, and the lead went blank on the picker. Pin the endings clear the
// note rather than leaving last action's sentence on screen.
func TestALonelyEndingLeavesNoStoryOnTheFleetCard(t *testing.T) {
	src := readAppJS(t)
	for _, gone := range []string{"that board is lonely now", "wiped its own settings, and waits",
		"New Orb takes it back"} {
		if strings.Contains(src, gone) {
			t.Errorf("app.js is telling the lonely story on the card again: %q", gone)
		}
	}
	for _, marker := range []string{`$("#ob-lonely-go").onclick`, `$("#fj-lonely").onclick`} {
		i := strings.Index(src, marker)
		if i < 0 {
			t.Fatalf("app.js has no %s — the lonely door's press handler was renamed", marker)
		}
		// The handler, not the handler and everything after it: these are assigned
		// arrow functions ending in "};" rather than a block-closing "}", so the
		// block helper other pins use would carry the segment into the next door.
		body := src[i:]
		if j := strings.Index(body[1:], "\n$(\""); j >= 0 {
			body = body[:j+1]
		}
		if !strings.Contains(body, `$("#mn-find-note").textContent = "";`) {
			t.Errorf("%s leaves the note holding whatever the last action wrote", marker)
		}
	}
	// The hub's own line, read once in the log the operator watched the write through, is
	// the other half of this and stays.
	hub, err := os.ReadFile("lonely.go")
	if err != nil {
		t.Fatalf("lonely.go: %v", err)
	}
	if !strings.Contains(string(hub), `"the orb is lonely now: its settings, name and credentials went with it`) {
		t.Error("the lonely door no longer reports what it did in the log it streams")
	}
}

// A cable answer is not a probe. The port label may say the chip is an orb, may not say
// anything about the firmware it never read, and may not claim a reboot.
func TestACableAnswerIsLabelledAsWhatItCost(t *testing.T) {
	src := readAppJS(t)
	label := blockContaining(t, src, "function portLabel")
	atProbed := strings.Index(label, "entry.probed")
	atProof := strings.Index(label, "entry.cableProof")
	if atProbed < 0 || atProof < 0 {
		t.Fatal("portLabel lost one of its two identification branches")
	}
	if atProof < atProbed {
		t.Error("portLabel reads cableProof ahead of probed, so a probe's version string loses to a PING")
	}
	if !strings.Contains(label, "an orb, answered over the cable") {
		t.Error("portLabel has no words for a console answer, so the picker calls it unidentified")
	}
	if proof := label[atProof:]; strings.Contains(proof, "entry.version") || strings.Contains(proof, "entry.verdict") {
		t.Error("portLabel prints a firmware stamp for a chip that was only ever pinged")
	}
}
