package main

import (
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
)

// The bench report of 2026-10-26: a board ended with the lonely write came back on the
// lead answering `runs different firmware: lonely`, and the card could not forget it.
// The row's line read "announced itself 22s ago" — a chip that announces NOTHING — and
// the dialog opened on the wait body with no button at all: cableOf refuses to call this
// lead live (the paid contradiction clears the orb claim, and the lonely mark suppresses
// it besides), so the dialog had no lead to name its endings after, and forgettable was
// false because the presence clock was fresh. The clock was fresh because the hub had
// wound it itself, in foldProbeIdentity, one line before the verdict saying this chip is
// not an orb. A gate that refuses on a clock the hub winds for the refusal is a dead end.

// lonelyAnswer is what a reboot-paid probe reports for a board running the hub's own
// blanking stub: a MAC read from the eFuse, a project that is not gorb-os, and no orb.
func lonelyAnswer(port, mac string) esprom.OrbInfo {
	return esprom.OrbInfo{Port: port, MAC: mac, IsOrb: false, Project: "lonely",
		Version: "premerge-A-20261015-36-gf7086df", Reason: "runs different firmware: lonely"}
}

// onboardRow loads a board up the way a normal session would have left it: an address,
// a name, and the announcement the fleet row was built from.
func onboardRow(t *testing.T, s *Server, mac, ip string, at time.Time) {
	t.Helper()
	if _, err := s.fleet.Observe(orbfleet.Observation{MAC: mac, Instance: instanceOf(mac),
		IP: ip, Port: 80, FW: "v1.1.20", State: "ready", At: at}); err != nil {
		t.Fatalf("seed fleet row: %v", err)
	}
}

// onLead puts this MAC on a lead the host reads as an orb, and files answer as the
// hub's verdict about what runs there. proven is what the gate is allowed to believe.
func onLead(s *Server, mac, port string, answer esprom.OrbInfo, proven string) {
	s.cables.list = func() ([]cableSighting, error) {
		return []cableSighting{{port: port, isOrb: true, espressif: true, mac: mac}}, nil
	}
	s.identities.record(port, answer, proven)
}

// TestNotOrbChipReadFilesNoRow is the source fix: the fold that learns a chip is not an
// orb must not raise a fleet row for it, because that row is what the Forget gate then
// refuses on. endAsLonely already deleted the row; this pins that nothing puts it back.
func TestNotOrbChipReadFilesNoRow(t *testing.T) {
	s := newHub()
	const mac, port = macHall, "/dev/ttyACM9"

	onboardRow(t, s, mac, "192.0.2.31", time.Now())
	onLead(s, mac, port, esprom.OrbInfo{Port: port, MAC: mac, IsOrb: true,
		Project: "gorb-os", Version: "v1.1.20"}, provenProbe)
	if _, known := s.fleet.Look(mac); !known {
		t.Fatal("setup: the fleet row is not there to be ended")
	}

	s.endAsLonely(port, mac)
	if _, known := s.fleet.Look(mac); known {
		t.Fatal("setup: the lonely write left the fleet row in place")
	}

	// The next paid probe answers the stub. This is the line the report died on.
	s.foldProbeIdentity(lonelyAnswer(port, mac))

	if _, known := s.fleet.Look(mac); known {
		t.Error("a NOT-an-orb chip read rebuilt the fleet row the operator just ended")
	}
	if !s.lonely.has(mac) {
		t.Error("a NOT-an-orb chip read cleared the lonely mark that ended the board")
	}
}


// TestContradictedRowIsForgettable is the gate fix, and the one the operator needed
// within the minute: the row exists, its presence clock is fresh, and nothing on the
// chip will ever announce again. The gate must read the contradiction it already holds
// and say forgettable, rather than hand the card a reason whose only advice is the
// dialog the card is already showing.
func TestContradictedRowIsForgettable(t *testing.T) {
	s := newHub()
	const mac, port = macHall, "/dev/ttyACM9"

	now := time.Now()
	onboardRow(t, s, mac, "192.0.2.31", now)
	onLead(s, mac, port, lonelyAnswer(port, mac), provenProbe)

	ok, why := s.forgetGate(mac, now, false)
	if !ok {
		t.Errorf("Forget refused on a chip the hub paid to learn is not an orb: %s", why)
	}

	// The same verdict the card renders, through the annotation the roster applies.
	rows := []orbRow{{MAC: mac}}
	s.annotateForget(rows, now)
	if !rows[0].Forgettable {
		t.Errorf("the card shows no Remove for a board the hub ended: why=%q liveForget=%v",
			rows[0].ForgetWhy, rows[0].LiveForget)
	}
	if rows[0].LiveForget {
		t.Error("a plain-forgettable row must not also be flagged for the dialog")
	}
}

// TestLonelyMarkedRowIsForgettable covers the same dead end one step earlier: a hub
// restarted from a state file carrying the lonely set has no paid probe to point at,
// only its own memory. That memory is the same kind of fact and opens the same gate.
func TestLonelyMarkedRowIsForgettable(t *testing.T) {
	s := newHub()
	const mac, port = macHall, "/dev/ttyACM9"

	now := time.Now()
	onboardRow(t, s, mac, "192.0.2.31", now)
	s.cables.list = func() ([]cableSighting, error) {
		return []cableSighting{{port: port, isOrb: true, espressif: true, mac: mac}}, nil
	}
	s.lonely.mark(mac)

	if ok, why := s.forgetGate(mac, now, false); !ok {
		t.Errorf("Forget refused on a board this hub marked lonely: %s", why)
	}
}

// TestOrbOnALeadIsNotForgettable is the guard against over-reaching: a board the probe
// found RUNNING gorb-os on a lead is exactly the case the cable refusal exists for.
func TestOrbOnALeadIsNotForgettable(t *testing.T) {
	s := newHub()
	const mac, port = macHall, "/dev/ttyACM9"

	now := time.Now()
	onboardRow(t, s, mac, "192.0.2.31", now)
	onLead(s, mac, port, esprom.OrbInfo{Port: port, MAC: mac, IsOrb: true,
		Project: "gorb-os", Version: "v1.1.20"}, provenProbe)

	if ok, why := s.forgetGate(mac, now, false); ok {
		t.Errorf("a board the hub found running gorb-os on a lead was marked forgettable (why=%q)", why)
	} else if !strings.Contains(why, "Forget") {
		t.Errorf("the cable refusal stopped naming the endings that need it: %q", why)
	}
}

// TestQuietRowStillRefusesNothing pins that a contradiction does not create permission
// where there is no row to forget: an unknown MAC gets the no-op answer it always had.
func TestQuietUnknownRowIsForgotten(t *testing.T) {
	s := newHub()
	if ok, why := s.forgetGate(macHall, time.Now(), false); !ok || why != "" {
		t.Errorf("a row the hub never kept must read as a no-op: ok=%v why=%q", ok, why)
	}
}
