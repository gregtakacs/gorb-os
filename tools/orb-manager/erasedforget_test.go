package main

// The erased board on a lead. Forgetting it is the one operation an operator reaches
// for after blanking a chip by hand, and the gate used to refuse it forever: the cable
// check returns first, and the exemption that would let the operator out sits behind
// it. A board with no firmware cannot announce, cannot answer its address, and cannot
// earn the lonely mark — that mark belongs to the hub's own erase door — so the only
// evidence that ever clears it is a paid probe saying the ROM answered and no gorb-os
// booted. These tests pin that evidence all the way through to the button.

import (
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
)

// erasedChip is the verdict ProbeOrb files for a board whose flash holds no gorb-os
// image. ChipAnswered is the whole qualification: the ROM synced and read its eFuse MAC,
// so a following IsOrb=false is a statement about the silicon rather than the wire.
func erasedChip(port, mac string) esprom.OrbInfo {
	return esprom.OrbInfo{
		Port: port, MAC: mac, ChipAnswered: true,
		Reason: "an ESP32-S3 at " + mac + ", but no gorb-os firmware booted",
	}
}

// TestAnErasedBoardOnItsLeadIsForgettable: the disproof and the sighting name the same
// lead. cableSights' contradiction already covered this shape; it is here so the next
// case cannot be mistaken for the only one that works.
func TestAnErasedBoardOnItsLeadIsForgettable(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	quietRow(s, macHall, "192.0.2.31", now)
	const lead = "/dev/ttyACM9"
	stubCables(s, cableSighting{port: lead, isOrb: true, espressif: true, mac: macHall})
	s.identities.done(lead, erasedChip(lead, macHall))

	if port := s.cableOf(macHall, clock()); port != "" {
		t.Fatalf("cableOf still names %s: a disproven chip holds its lead", port)
	}
	if ok, why := s.forgetGate(macHall, clock(), true); !ok {
		t.Errorf("Forget refused a disproven board: %q", why)
	}
}

// TestADisprovenChipClearsALeadItWasNotProvenOn is the case the gate alone could not
// reach. The identity cache is keyed by lead; the fleet and the gate key by MAC. A chip
// disproven on one lead must not keep refusing Forget over another, because
// forgetGate's own exemption sat behind the cable refusal and never ran. This is the
// regression: an orb erased outside the hub, still plugged into the lead it is named by.
func TestADisprovenChipClearsALeadItWasNotProvenOn(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	quietRow(s, macHall, "192.0.2.31", now)
	const live = "/dev/ttyACM1"
	const proven = "/dev/ttyACM4"
	stubCables(s, cableSighting{port: live, isOrb: true, espressif: true, mac: macHall})
	// The paid reading happened over a different socket path on the same bench.
	s.identities.done(proven, erasedChip(proven, macHall))

	if port := s.cableOf(macHall, clock()); port != "" {
		t.Fatalf("cableOf refused on %s from a sighting the hub has disproven by MAC: %s", live, port)
	}
	if ok, why := s.forgetGate(macHall, clock(), true); !ok {
		t.Errorf("Forget refused a board the hub holds a paid not-an-orb verdict for: %q", why)
	}
}

// TestAnOrbStillHoldsItsLead is the guard against the widening above. A board the hub
// has no disproof for keeps its cable refusal and its exact sentence: the exemption
// reads evidence, never the descriptor's claim, or Forget would stop meaning anything.
func TestAnOrbStillHoldsItsLead(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	quietRow(s, macHall, "192.0.2.31", now)
	const lead = "/dev/ttyACM2"
	stubCables(s, cableSighting{port: lead, isOrb: true, espressif: true, mac: macHall})
	s.identities.done(lead, esprom.OrbInfo{Port: lead, MAC: macHall, IsOrb: true,
		Project: "gorb-os", Version: "v1.1.27", ChipAnswered: true, Reason: "identified"})

	if port := s.cableOf(macHall, clock()); port != lead {
		t.Fatalf("cableOf = %q, want the lead %s a real orb still sits on", port, lead)
	}
	ok, why := s.forgetGate(macHall, clock(), true)
	if ok || !strings.Contains(why, "on a cable") {
		t.Errorf("a board running gorb-os on a lead was let go: ok=%v reason=%q", ok, why)
	}
}

// TestTheDeafLeadStillRefuses pins what provenFailed is for: a probe that never reached
// silicon says nothing about the board, and must not hand out a Forget. Without this the
// exemption above would let any flaky cable wipe a healthy orb off the list.
func TestTheDeafLeadStillRefuses(t *testing.T) {
	now := time.Now()
	s, clock := gateClock(now)
	quietRow(s, macHall, "192.0.2.31", now)
	const lead = "/dev/ttyACM3"
	stubCables(s, cableSighting{port: lead, isOrb: true, espressif: true, mac: macHall})
	s.identities.doneFailed(lead, "port busy: held by another job")

	if port := s.cableOf(macHall, clock()); port != lead {
		t.Fatalf("an unread lead cleared the cable: %q", port)
	}
	if ok, why := s.forgetGate(macHall, clock(), true); ok {
		t.Errorf("Forget allowed on an absence of evidence: %q", why)
	}
}
