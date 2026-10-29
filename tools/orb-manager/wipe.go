package main

import (
	"log"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
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
// wired back. It is the wipe's pair: the chip's answer may or may not have
// reached the hub, so the port's cached identity — which now describes bytes that
// are gone — goes first, and the shared ending un-makes the rest. Dropping the
// identity is also what re-arms the next sweep to re-probe the port: without it
// a lonely board's own descriptor keeps an "orb" claim the hub paid for on a
// firmware that no longer exists, and never gets re-polled.
func (s *Server) forgetWipedOrb(port, mac string) {
	s.identities.forget(port)
	s.unMakeOrb(mac)
}

// unMakeOrb drops every fact this hub held about the CHIP behind a MAC: the
// identity is per-port and belongs to whoever holds the cable (forgetWipedOrb
// drops it; a probe that just filed a fresh contradiction keeps its stamp — it is
// the evidence holding the descriptor down). The boot verdict describes a firmware
// that no longer exists; the fleet row, its name and its address were earned by a
// firmware that is not there to re-earn them. Grants go too, unless another live
// board still claims the address — revoking a shared address over one erased
// board would cut off an orb that did nothing wrong. And with each revoked
// address the liveness cache loses its verdict: an orb the hub just ended must
// read neither alive nor "the hub is asking", because both would keep a
// half-forgotten board on the operator's card for a sweep's worth of minutes.
//
// Two doors call it: the lonely write (through forgetWipedOrb) and a paid probe
// that answered this chip as running no gorb-os (endProvenNotOrb). Forgetting is
// the whole contract of both — a flash erase wipes the data that this board ever
// was an orb, chip and hub both, no excuses.
func (s *Server) unMakeOrb(mac string) {
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
	// The record the deletion must not read back. The lonely mark already refuses a
	// packet older than the act on the fold path, and an orb flash that clears the mark
	// is the honest way for the row to come back; dropping the cache here keeps the
	// board off the card for the two minutes it would otherwise take the ghost to age
	// out, which is the window an operator experiences as "Forget did nothing".
	s.forgetGhost(mac)
	// The lonely mark belongs to the caller that ends a board as lonely; this
	// helper only un-makes the orb, and a caller that re-purposes it (a headless
	// erase, a future door) must not inherit the mark by accident.
	for _, ip := range held {
		if len(s.fleet.ClaimedBy(ip)) == 0 {
			s.grants.revoke(ip)
			s.live.forget(ip)
		}
	}
	s.saveState()
}

// endProvenNotOrb is the second half of the rule a paid probe completes: a chip
// that answered for itself as running no gorb-os has ended its own orb life, and
// the hub's storage must not keep the sentence "this was an orb" a moment longer.
// A board erased or written over on a bench — by the CLI, by a lonely write whose
// MAC never streamed back, by any hand this hub did not hold the cable of — was
// otherwise stuck on the card as a disconnected orb that Forget refused, because
// every free fact (descriptor, grant, announce clock) is an eFuse MAC's claim or
// a memory of a firmware that is gone. The probe is the one question that cannot
// be inherited, so its negative is treated exactly as the lonely door's ending
// is: the row, the name, the grant and the verdict go at once.
//
// Only a ChipAnswered negative counts. An answer that never got past the wire —
// a port busy, a ROM that would not SYNC after a disturbed entry — says nothing
// about the firmware and must not cost a healthy orb its row.
func (s *Server) endProvenNotOrb(info esprom.OrbInfo) {
	if !info.ChipAnswered || info.MAC == "" || s.fleet == nil {
		return
	}
	if _, known := s.fleet.Look(info.MAC); !known {
		return
	}
	log.Printf("orb-manager: ending %s at %s: a paid probe proved the chip runs no gorb-os (%s)",
		info.MAC, info.Port, info.Reason)
	s.unMakeOrb(info.MAC)
	s.wakeOrbs()
}

// macKnownForPort answers "which chip was this port an orb on, as far as this hub
// personally knows" for the doors whose own read of the chip failed. It trusts
// two records and nothing else: the live identity cache (fresh, cable-swap
// checked) and the current sweep's sighting for this port. It exists so a lonely
// session that reached the ROM but never streamed a MAC can still end the board
// it was asked to end; a port neither record knows gives back "".
func (s *Server) macKnownForPort(port string) string {
	if port == "" {
		return ""
	}
	if s.identities != nil {
		if info, ok := s.identities.get(port); ok && info.MAC != "" {
			if c, why := orbmdns.CanonicalMAC(info.MAC); why == "" {
				return c
			}
		}
	}
	for _, c := range s.cableSights(time.Now()) {
		if c.port == port && c.mac != "" {
			if canon, why := orbmdns.CanonicalMAC(c.mac); why == "" {
				return canon
			}
		}
	}
	return ""
}

// _ keeps the orbfleet import honest against a future edit that drops the loop;
// the Forget/ClaimedBy pair is the whole reason this file speaks the store.
var _ = orbfleet.Orb{}
