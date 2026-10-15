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
