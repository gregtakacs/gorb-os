package main

// The erase that un-erases itself.
//
// A lonely erase marks the chip and drops the row. The browse cache then hands the next
// fold an announcement the board sent BEFORE the erase: Candidates() serves a record until
// its TTL plus grace ages out, and the device's MDNS_TTL is 120 s (main/net/orb_mdns.c).
// Folding that ghost called fleet.Observe, which files a row out of nothing, and then
// cleared the lonely mark that existed to keep the row gone. The board — blank, speaking
// nothing — came back on the card named by its MAC-derived instance, and Forget refused it
// forever on the strength of the cable it was born on (bench, 2026-10-28).
//
// The rule that stops this is the one liveness.go already states for the presence clock: a
// hub must not wind its own facts out of its own cache. A packet is evidence only if it was
// heard after the mark, so lonelyMark.outranked compares the two instants and
// foldCandidates skips a record that cannot speak to the present.

import (
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
)

// lonelyGhost builds the hub ninety seconds after it blanked a board: the mark is on file
// at markAt, and the browser still holds the pre-erase announcement. filled() pins the
// browser clock at its Now, so heardAt is that fixed instant and markAt must be later.
func lonelyGhost(t *testing.T, heardAt, markAt time.Time) *Server {
	t.Helper()
	s := newHub()
	s.fleet = orbfleet.New(orbfleet.Config{Now: func() time.Time { return markAt }})
	s.lonely.now = func() time.Time { return markAt }
	s.lonely.mark(macHall)
	s.browse = filled(t, orbmdns.Candidate{
		MAC: macHall, Instance: inst(macHall), IP: "192.168.2.31", Port: 80,
		State: "associated", From: "192.168.2.31", Expires: heardAt.Add(2 * time.Minute),
	})
	return s
}

// TestAGhostAnnouncementDoesNotUndoAnErase is the bench case whole: the fold that ran after
// an erase must file no row, keep the mark, and leave Forget open.
func TestAGhostAnnouncementDoesNotUndoAnErase(t *testing.T) {
	heard := time.Unix(1760000000, 0)   // the board's last word while it was an orb
	mark := heard.Add(90 * time.Second) // the hub blanked it ninety seconds later
	s := lonelyGhost(t, heard, mark)

	s.foldCandidates(true) // the upkeep tick, running minutes after the erase

	if _, known := s.fleet.Look(macHall); known {
		t.Error("a pre-erase announcement re-filed the fleet row the erase deleted")
	}
	if !s.lonely.has(macHall) {
		t.Fatal("the fold cleared the lonely mark on a packet older than that mark")
	}
	if ok, why := s.forgetGate(macHall, mark.Add(time.Minute), false); !ok {
		t.Errorf("Forget refused a board this hub blanked: %q", why)
	}
}

// TestAGhostFoldDoesNotRenewTheGrant is the same ghost at the door the fold is not supposed
// to operate: renew=true is the timer's power to extend a dial's life, and a packet the
// board stopped sending must not spend it.
func TestAGhostFoldDoesNotRenewTheGrant(t *testing.T) {
	heard := time.Unix(1760000000, 0)
	mark := heard.Add(90 * time.Second)
	s := lonelyGhost(t, heard, mark)
	s.grants.learnAs("192.168.2.31", macBed) // a grant bound to somebody else

	s.foldCandidates(true)

	if got := s.grants.bound("192.168.2.31"); got == macHall {
		t.Error("a pre-erase ghost took over the grant binding for a board the hub blanked")
	}
}

// TestAnAnnouncementHeardAfterTheEraseEndsTheMark is the guard against over-reaching. A
// board reflashed back into gorb-os announces again, and that packet is newer than the
// mark: it must land in the store and clear the mark. The fix may not make a lonely mark
// unfalsifiable, which is the failure launchprobe.go names for the same shape on the probe
// path.
func TestAnAnnouncementHeardAfterTheEraseEndsTheMark(t *testing.T) {
	spoke := time.Unix(1760000000, 0)   // what the browser clock stamps the packet
	mark := spoke.Add(-4 * time.Minute) // the erase happened four minutes before it
	s := newHub()
	s.fleet = orbfleet.New(orbfleet.Config{Now: func() time.Time { return spoke }})
	s.lonely.now = func() time.Time { return mark }
	s.lonely.mark(macHall)
	s.browse = filled(t, orbmdns.Candidate{
		MAC: macHall, Instance: inst(macHall), IP: "192.168.2.31", Port: 80,
		State: "associated", From: "192.168.2.31", Expires: spoke.Add(2 * time.Minute),
	})

	s.foldCandidates(true)

	if s.lonely.has(macHall) {
		t.Error("an orb that announced after the erase is still held down by the mark")
	}
	row, known := s.fleet.Look(macHall)
	if !known || row.IP != "192.168.2.31" {
		t.Fatalf("the fresh announcement did not rebuild its row: %+v", row)
	}
}

// TestLonelyMarkOutrankedByPacketTimeOnly pins the rule at the unit level, so the fold test
// cannot pass by accident. Equal is not newer: a mark and a packet sharing an instant means
// the fold saw no evidence after the erase, and the mark stands.
func TestLonelyMarkOutrankedByPacketTimeOnly(t *testing.T) {
	mark := time.Unix(1760000000, 0)
	l := newLonelyMark()
	l.now = func() time.Time { return mark }
	l.mark(macHall)

	if !l.outranked(macHall, mark.Add(-time.Second)) {
		t.Error("a packet heard before the mark was allowed to outrank it")
	}
	if !l.outranked(macHall, mark) {
		t.Error("a packet heard at the mark's own instant was read as new evidence")
	}
	if l.outranked(macHall, mark.Add(time.Second)) {
		t.Error("a packet heard after the mark must end it")
	}
	if l.outranked(macHall, time.Time{}) {
		t.Error("no packet time is no evidence")
	}
	if l.outranked(macBed, mark.Add(-time.Hour)) {
		t.Error("one board's mark must not silence another's announcement")
	}
	// Colon and bare spellings are one chip, and the mark answers whoever asks.
	if !l.outranked("44:bd:8d:78:26:18", mark.Add(-time.Second)) {
		t.Error("a mark keyed by the bare MAC did not answer the coloned spelling")
	}
	l.clear(macHall)
	if l.outranked(macHall, mark.Add(-time.Hour)) {
		t.Error("a cleared mark still refused to fold")
	}
}
