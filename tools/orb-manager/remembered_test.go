package main

import (
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
)

// The list shows what the hub can reach: a board on a cable right now, or a board
// with an address to dial. The operator stated the rule on the bench, 2026-10-15:
// "the only reason to keep an orb when it's not actively connected is to be able to
// ping its IP address. One that is not pingable does not matter, we can't manage it
// ever unless it connects to the serial port at which point it will self discover
// anyway." Every orb flash used to stamp the fleet row Remembered, and that mark is
// what kept an address-less board on the card across restarts, unplugs, and its own
// ending. These two tests are the rule and the other half of the rule; the ending
// that marks a chip lives in endaslonely_test.go.

// The unplugged case: a board that never had an address and is no longer on a lead
// produces nothing at all. It must not reach the card from the store, and it must
// not survive a save. The MAC is synthetic on purpose — roster enumerates the real
// leads of the machine running this test, and a bench board's row proves nothing here.
func TestAddresslessBoardDiesWithItsCable(t *testing.T) {
	s := newHub()
	const mac = "aabbcc112233"
	if err := s.fleet.MarkRemembered(mac); err != nil {
		t.Fatal(err)
	}
	if _, _, err := s.fleet.NoteDeviceName(mac, "Gold"); err != nil {
		t.Fatal(err)
	}
	rows, _ := s.roster(nil, time.Now())
	for _, r := range rows {
		if r.MAC == mac {
			t.Fatalf("an address-less, cable-less board is on the card: %+v", r)
		}
	}
	if got := s.fleet.Snapshot(); len(got) != 0 {
		t.Fatalf("the save still writes an unreachable row: %+v", got)
	}
}

// The other half of the same rule: an address keeps a row. The point of the file is
// the board that is asleep on the shelf at a known address, and nothing here may
// cost the operator that.
func TestBoardWithAnAddressStillHoldsItsRow(t *testing.T) {
	s := newHub()
	const mac = "aabbcc445566"
	if _, err := s.fleet.Observe(orbfleet.Observation{
		MAC: mac, Instance: "orb-445566", IP: "192.0.2.50", Port: 80, At: time.Now(),
	}); err != nil {
		t.Fatal(err)
	}
	rows, _ := s.roster(nil, time.Now())
	var got *orbRow
	for i := range rows {
		if rows[i].MAC == mac {
			got = &rows[i]
		}
	}
	if got == nil {
		t.Fatal("a remembered address did not reach the card")
	}
	if got.IP != "192.0.2.50" {
		t.Errorf("the row lost the address that keeps it: %+v", *got)
	}
	if got.Reachable != "unknown" {
		t.Errorf("the row claims a present the hub has not checked: %+v", *got)
	}
}
