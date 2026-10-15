package main

import (
	"log"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
)

// The whole-chip erase has no door on this hub. The manager page ends a board's
// orb life with the lonely write, which wipes the settings the erase would have
// reached by destruction and leaves the board enumerable, bootable and legible
// instead of a boot-looping flapping port. The ROM erase itself stays in
// internal/esprom (internal/esprom/wipe_test.go) and in the headless bench CLI
// (cmd/orbflash -mode wipe) — one for a bricked-looking board a bench script is
// recovering, deliberately unreachable from the page and the hub's HTTP surface.

// forgetWipedOrb drops everything the hub believed about a board whose firmware
// has just been ended — the lonely write today, a headless erase if one is ever
// wired back. The identity cache is per-port and now describes bytes that are
// gone; the boot verdict describes a firmware that no longer exists; the fleet
// row, its name and its address were earned by a firmware that is not there to
// re-earn them. Grants go too, unless another live board still claims the
// address — revoking a shared address over one erased board would cut off an orb
// that did nothing wrong.
func (s *Server) forgetWipedOrb(port, mac string) {
	s.identities.forget(port)
	s.forgetBoot(mac)
	// The chip's word went with the firmware, so the read goes with the chip's
	// word. Without this the board keeps an in-memory "confirmed" identity that
	// hides it from the New Orb door while its row shows nothing on the fleet —
	// the one state an operator cannot click out of.
	s.clearChipRead(mac)
	if mac == "" || s.fleet == nil {
		s.saveState()
		return
	}
	held, err := s.fleet.Forget(mac)
	if err != nil {
		log.Printf("orb-manager: ending %s left its fleet row in place: %v", mac, err)
		return
	}
	// The lonely mark belongs to the caller that ends a board as lonely; this
	// helper only un-makes the orb, and a caller that re-purposes it (a headless
	// erase, a future door) must not inherit the mark by accident.
	for _, ip := range held {
		if len(s.fleet.ClaimedBy(ip)) == 0 {
			s.grants.revoke(ip)
		}
	}
	s.saveState()
}

// _ keeps the orbfleet import honest against a future edit that drops the loop;
// the Forget/ClaimedBy pair is the whole reason this file speaks the store.
var _ = orbfleet.Orb{}
