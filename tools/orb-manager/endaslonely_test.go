package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
)

// The lonely case, and the reason Forget looked broken on the bench (2026-10-15):
// the door marked the chip AFTER forgetWipedOrb had already saved, so the file kept
// the forgetting and lost the reason for it. A restart restored no mark, the cable
// sweep read the descriptor the lonely board still enumerates, Confirm re-created
// the row, and the board the operator had just ended was an orb again in the list.
// One save must carry both facts; this drives the pair the door actually calls.
func TestEndAsLonelyPersistsTheMarkThatKeepsTheRowGone(t *testing.T) {
	s := newHub()
	const mac = "aabbcc778899"
	const port = "/dev/ttyACM9"
	if _, err := s.fleet.Observe(orbfleet.Observation{
		MAC: mac, Instance: "orb-778899", IP: "192.0.2.31", Port: 80, At: time.Now(),
	}); err != nil {
		t.Fatal(err)
	}
	s.statePath = filepath.Join(t.TempDir(), "state.json")

	s.endAsLonely(port, mac)

	b, err := os.ReadFile(s.statePath)
	if err != nil {
		t.Fatalf("the ending wrote no state file: %v", err)
	}
	file := string(b)
	if strings.Contains(file, "192.0.2.31") {
		t.Fatalf("the ended board is still in the file: %s", file)
	}
	if !strings.Contains(file, `"aabbcc778899"`) {
		t.Fatalf("the save recorded the forgetting but not the mark that keeps it: %s", file)
	}

	// The launch that follows has to hold the line on its own, with nothing but the
	// file. Two sources could still resurrect the row the operator ended: the sweep
	// attributing the descriptor the lonely board still enumerates, and a packet that
	// folds the old address back onto the MAC. The mark, not the absence of an
	// address, is what has to stop both.
	s2 := newHub()
	s2.statePath = s.statePath
	s2.loadState()
	if !s2.lonely.has(mac) {
		t.Fatal("the lonely mark did not come back from the file the ending wrote")
	}
	s2.seeCable(mac, port)
	if _, err := s2.fleet.Observe(orbfleet.Observation{
		MAC: mac, Instance: "orb-778899", IP: "192.0.2.31", Port: 80, At: time.Now(),
	}); err != nil {
		t.Fatal(err)
	}
	rows, _ := s2.roster(nil, time.Now())
	for _, r := range rows {
		if r.MAC == mac {
			t.Errorf("the sweep or the address rebuilt the row the operator ended: %+v", r)
		}
	}
}
