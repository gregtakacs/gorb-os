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
// the form it replaces, and the Later door that must stay live through both.
// Pin the ones a partial rewrite once left unhooked.
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
