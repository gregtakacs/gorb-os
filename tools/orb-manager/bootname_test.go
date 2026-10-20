package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
)

// The boot banner is the hub's only way to READ a name off a board it cannot dial.
// Two files on two sides of a USB lead have to agree on one marker, and nothing links
// them but this test and the strings inside orb_net.c. That is the whole reason it
// exists: the last cross-language assumption in this area (the state banner) was
// checked only from one side, and it drifted for a release before anyone noticed.

const bootNameMarker = "GorbOS-NAME"

// TestBootBannerNameRoundTripsThroughTheHubParser pins the pair. It reads the format
// string out of the firmware, renders the line the way ESP_LOGI renders it, and hands
// the result to the parse that consumes it. Change the marker on one side and this
// fails; change the spacing or the argument order and it fails too.
func TestBootBannerNameRoundTripsThroughTheHubParser(t *testing.T) {
	src, err := os.ReadFile(filepath.Join("..", "..", "main", "net", "orb_net.c"))
	if err != nil {
		t.Skipf("no firmware source: %v", err)
	}
	c := string(src)

	want := `ESP_LOGI(TAG, "` + bootNameMarker + ` %s", s_name);`
	if !strings.Contains(c, want) {
		t.Fatalf("orb_net.c does not print the banner as %q — the hub's parser still reads the old shape", want)
	}
	// Only a named board speaks. An unnamed one prints nothing, which the hub must
	// read as no news: guessing "cleared" here would wipe the cached name of every
	// board still running firmware that prints no such line.
	if !strings.Contains(c, `if (s_name[0] != '\0') {`) {
		t.Error("the banner is not gated on the name being set, so an unnamed board announces an empty name")
	}

	// The line as the console emits it, ESP_LOGI prefix included.
	line := "I (1231) orb_net: " + bootNameMarker + " Reading Room"
	got, ok := esprom.ParseOrbName(line)
	if !ok || got != "Reading Room" {
		t.Errorf("the hub cannot read the line the firmware prints (%q,%v)", got, ok)
	}
	if _, ok := esprom.ParseOrbName("I (1231) orb_net: GorbOS-STATE c-sta"); ok {
		t.Error("the state banner parsed as a name; the two markers are not distinct")
	}
}

// A banner name updates a row the hub already holds and never creates one. A board
// that has never been seen belongs to the picker, not the fleet, and a boot line is
// not evidence of a device the operator has adopted.
func TestBootBannerNameFoldsOntoARowAndNotIntoOne(t *testing.T) {
	s := newHub()
	const mac = "44bd8d782618"
	if _, err := s.fleet.Confirm(mac); err != nil {
		t.Fatalf("seeding the row: %v", err)
	}
	if !s.foldBootName(mac, "Reading Room") {
		t.Fatal("the announced name did not reach the store")
	}
	row, ok := s.fleet.Look(mac)
	if !ok {
		t.Fatal("the row the test seeded is gone")
	}
	if row.Name != "Reading Room" {
		t.Errorf("row.Name = %q, want the board's own word", row.Name)
	}
	if s.foldBootName(mac, "Reading Room") {
		t.Error("folding the same name again reported a change")
	}

	// Unknown board: refused, and no row invented for it.
	if s.foldBootName("44bd8d000000", "Ghost") {
		t.Error("a boot banner created a fleet row")
	}
	// Hostile or unusable words are refused by the same gate every name passes.
	for _, bad := range []string{"", "   ", "<script>", "bell\u0007name", strings.Repeat("x", orbfleet.MaxName+1)} {
		if s.foldBootName(mac, bad) {
			t.Errorf("foldBootName accepted %q", bad)
		}
	}
	if row2, _ := s.fleet.Look(mac); row2.Name != "Reading Room" {
		t.Errorf("a refused banner overwrote the stored name: %q", row2.Name)
	}
}

// The two call sites are the feature. A parse nobody calls is a string in a file, so
// both folds are pinned to the branch that has to reach them.
func TestBootBannerNameIsFoldedAtProbeAndFlash(t *testing.T) {
	id, err := os.ReadFile("identity.go")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(id), "s.foldBootName(info.MAC, info.Name)") {
		t.Error("a paid probe no longer folds the banner name; a cable-only card stays blank")
	}

	fl, err := os.ReadFile("flash.go")
	if err != nil {
		t.Fatal(err)
	}
	f := string(fl)
	if !strings.Contains(f, "s.foldBootName(flashedMAC, bootName)") {
		t.Error("handleFlash no longer folds the boot banner; a re-flash of a named board loses its name")
	}
	// A name the operator typed in this dialog was already written to the chip and
	// folded. The banner must not get a second, later word over their keystroke.
	if !strings.Contains(f, `req.Name == "" && bootName != ""`) {
		t.Error("the flash fold is not gated on the dialog having said nothing")
	}
}
