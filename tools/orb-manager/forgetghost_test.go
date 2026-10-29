package main

// The plain Forget that un-forgets itself.
//
// lonelyghost_test.go and docs/architecture.md pin the LONELY erase against one
// mechanism: the browse cache serves a board's LAST announcement for TTL plus grace
// (120 s by the device's MDNS_TTL, main/net/orb_mdns.c), so the fold right after a
// deletion meets a ghost packet the chip sent while it was still an orb. The lonely path
// survived because an erase leaves a mark and lonelyMark.outranked refuses a packet older
// than that mark.
//
// The plain door has no mark to leave. POST /api/fleet/forget deletes the row for a board
// that is off the network rather than blanked on a lead, and the very next fold read the
// ghost and re-filed the row through fleet.Observe. The card showed the board again named
// by its MAC-derived instance (orb-<mac>), and that same ghost refreshed the row's
// presence clock, so Forget refused it for another minute and the button stayed shut.
// (bench, 2026-10-29: an operator deleted an orb and it stayed, named orb-<mac>.)
//
// The lonely fix reached for the mark; the rule underneath it is the one liveness.go
// already states for the presence clock — a hub may not wind its own facts out of its own
// cache. A plain forget is an act with the same status an erase has, and a record the hub
// is only still holding because the board stopped talking is not evidence that survives
// it. So both forget doors now drop the board's own browse record as they delete the row,
// and these tests pin that the ghost has nowhere left to re-file from.

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
)

// plainForgetHub is a hub ninety seconds after its operator forgot a board with the
// plain door: no fleet row, no grant, and a browser still holding the announcement the
// board sent while it was an orb. filled() pins the browser's clock at its Now, so the
// record's LastSeen is the frozen instant and the erase must come after it.
func plainForgetHub(t *testing.T, heard, forgot time.Time) (*Server, *orbmdns.Browser) {
	t.Helper()
	s := newHub()
	s.fleet = orbfleet.New(orbfleet.Config{Now: func() time.Time { return forgot }})
	s.lonely.now = func() time.Time { return forgot }
	if _, err := s.fleet.Observe(orbfleet.Observation{MAC: macHall, Instance: inst(macHall),
		IP: "192.168.2.31", Port: 80, FW: "v1.1.29", State: "ready", At: heard}); err != nil {
		t.Fatalf("seed fleet row: %v", err)
	}
	if _, known := s.fleet.Look(macHall); !known {
		t.Fatal("setup: the row a forget is meant to delete was never filed")
	}
	s.grants.learnAs("192.168.2.31", macHall)
	// The door's own two moves, run for real rather than simulated: the gate must let a
	// board whose only presence evidence is a ghost through, and the delete must stick.
	if ok, why := s.forgetGate(macHall, forgot, true); !ok {
		t.Fatalf("setup: Forget refused a board the hub had already dialled away: %q", why)
	}
	held, err := s.fleet.Forget(macHall)
	if err != nil {
		t.Fatalf("the forget door: %v", err)
	}
	for _, ip := range held {
		if len(s.fleet.ClaimedBy(ip)) == 0 {
			s.grants.revoke(ip)
		}
	}
	b := filled(t, orbmdns.Candidate{
		MAC: macHall, Instance: inst(macHall), IP: "192.168.2.31", Port: 80,
		State: "associated", From: "192.168.2.31", Expires: heard.Add(2 * time.Minute),
	})
	s.browse = b
	return s, b
}

// TestAPlainForgetSurvivesTheBrowseCache is the bench case whole: an orb forgotten from a
// hub that still holds its last announcement must stay forgotten through the next fold.
func TestAPlainForgetSurvivesTheBrowseCache(t *testing.T) {
	heard := time.Unix(1760000000, 0)     // the board's last word while it was an orb
	forgot := heard.Add(90 * time.Second) // the operator pressed Forget ninety seconds later
	s, _ := plainForgetHub(t, heard, forgot)

	s.forgetGhost(macHall) // the door's purge, beside the row delete
	s.foldCandidates(true) // the upkeep tick, running minutes after the forget

	if _, known := s.fleet.Look(macHall); known {
		t.Error("a pre-forget announcement re-filed the fleet row the plain Forget deleted")
	}
	for _, c := range s.browse.Candidates() {
		if c.MAC == macHall {
			t.Error("the hub still holds an announcement for a board it just forgot")
		}
	}
	if ok, why := s.forgetGate(macHall, forgot.Add(time.Minute), false); !ok {
		t.Errorf("Forget refused a board this hub already forgot: %q", why)
	}
}

// TestTheForgetDoorSweepsItsOwnCache is the bench case at the door rather than one call
// below it: the operator pressed Forget, the hub said ok, and the card brought the board
// back as orb-<mac>. Everything past this handler — the fold, the presence clock, the
// relabelled card — is the symptom. The door is where the deletion has to be complete.
func TestTheForgetDoorSweepsItsOwnCache(t *testing.T) {
	heard := time.Unix(1760000000, 0)
	s, b := plainForgetHub(t, heard, heard.Add(90*time.Second))

	rec := httptest.NewRecorder()
	s.handleFleetForget(rec, httptest.NewRequest(http.MethodPost, "/api/fleet/forget",
		strings.NewReader(`{"mac":"44:bd:8d:78:26:18","confirm":true}`)))
	if rec.Code != http.StatusOK {
		t.Fatalf("the door refused its own forget: %d %s", rec.Code, rec.Body.String())
	}
	if len(b.Candidates()) != 0 {
		t.Error("POST /api/fleet/forget deleted the row and left the announcement to rebuild it")
	}
}

// TestAFreshAnnouncementStillFilesAForgottenBoardBack is the guard against over-reaching.
// The purge removes a stale claim, not the board's standing: a chip that announces again
// AFTER the forget is making a present-tense claim, and a hub that refused it would be
// hiding a live orb its operator can plainly still reach. Same hub, one newer packet.
func TestAFreshAnnouncementStillFilesAForgottenBoardBack(t *testing.T) {
	heard := time.Unix(1760000000, 0)
	forgot := heard.Add(90 * time.Second)
	s, b := plainForgetHub(t, heard, forgot)
	s.forgetGhost(macHall) // the door's purge, run before the board speaks again

	checkIn(t, b, macHall, "192.168.2.31", "normal") // an orb, announcing now
	s.foldCandidates(true)

	row, known := s.fleet.Look(macHall)
	if !known {
		t.Fatal("an announcement heard after the forget did not re-file the row — the purge became a blacklist")
	}
	if row.IP != "192.168.2.31" {
		t.Errorf("the re-filed row answers at %q, not the address it announced", row.IP)
	}
}

// TestForgetMACIsQuietAboutStrangers pins the browser's own half: the purge is keyed on
// the chip, so forgetting one board leaves every other cached orb alone, and a MAC that
// will not parse forgets nothing rather than guessing at a match.
func TestForgetMACIsQuietAboutStrangers(t *testing.T) {
	s, _ := plainForgetHub(t, time.Unix(1760000000, 0), time.Unix(1760000000, 0).Add(90*time.Second))
	b := filled(t,
		orbmdns.Candidate{MAC: macHall, Instance: inst(macHall), IP: "192.168.2.31", Port: 80,
			State: "associated", From: "192.168.2.31", Expires: time.Unix(1760000999, 0).Add(2 * time.Minute)},
		orbmdns.Candidate{MAC: macBed, Instance: inst(macBed), IP: "192.168.2.32", Port: 80,
			State: "associated", From: "192.168.2.32", Expires: time.Unix(1760000999, 0).Add(2 * time.Minute)},
	)
	s.browse = b
	if !b.ForgetMAC("44:bd:8d:78:26:18") {
		t.Error("the coloned spelling of a cached MAC forgot nothing")
	}
	if b.ForgetMAC("not-a-mac") {
		t.Error("an unparseable MAC reported a purge it cannot have performed")
	}
	left := b.Candidates()
	if len(left) != 1 || left[0].MAC != macBed {
		t.Errorf("purging one board left %d rows (%s), want the other orb intact", len(left),
			func() string {
				out := ""
				for _, c := range left {
					out += c.MAC + " "
				}
				return out
			}())
	}
}
