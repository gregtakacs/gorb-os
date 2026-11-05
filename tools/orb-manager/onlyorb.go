package main

import (
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
)

// -only-orb is for development, never for a release. Two clones on one bench each
// run a hub, and each clone owns one orb (its .orb-target). The one-hub rule
// (instance.go) exists because two hubs on ONE orb wedge it; two hubs on two orbs
// are fine, as long as neither one reaches for the other's board. So a hub started
// with -only-orb <mac> sees, lists and dials that board alone:
//
//   - the browse drops every other MAC's announcement before it becomes a row;
//   - the cable sweep and the Onboard picker drop every other attributable Espressif
//     lead, so the launch identify never resets the other clone's board and no press
//     can flash it;
//   - mayDial refuses an address committed for another MAC, which is what a
//     remembered grant from state.json would otherwise still allow.
//
// A typed address with no MAC behind it stays dialable: that is the operator's own
// word, and refusing it would leave a hub that cannot reach a board it never heard.
// With the flag unset (every double-click, every release) each answer here is "yes".

// admitsMAC reports whether this hub may treat mac's board as its own. An empty mac
// is not a board's claim, so it is admitted.
func (s *Server) admitsMAC(mac string) bool {
	if s.onlyOrb == "" || mac == "" {
		return true
	}
	c, why := orbmdns.CanonicalMAC(mac)
	return why == "" && c == s.onlyOrb
}

// mdnsAdmit is the browse's filter: nil for a release hub, so its browser is built
// exactly as it always was.
func (s *Server) mdnsAdmit() func(string) bool {
	if s.onlyOrb == "" {
		return nil
	}
	return s.admitsMAC
}

// admitsPort is admitsMAC for a lead. A port that is no Espressif chip is no orb
// either way and stays listed; an Espressif lead is admitted only when the host can
// attribute it to this hub's MAC, because an unattributed one may be the other board.
func (s *Server) admitsPort(p string) bool {
	if s.onlyOrb == "" {
		return true
	}
	u := esprom.ClassifyPort(p)
	if !u.IsOrbDevice() && !u.IsEspressif() {
		return true
	}
	mac, why := orbmdns.CanonicalMAC(u.Serial)
	return why == "" && mac == s.onlyOrb
}

// scopedPorts keeps the ports admitsPort admits.
func (s *Server) scopedPorts(ports []string) []string {
	if s.onlyOrb == "" {
		return ports
	}
	out := ports[:0:0]
	for _, p := range ports {
		if s.admitsPort(p) {
			out = append(out, p)
		}
	}
	return out
}

// scopedSweep wraps the cable sweep the same way.
func (s *Server) scopedSweep(list func() ([]cableSighting, error)) func() ([]cableSighting, error) {
	if s.onlyOrb == "" {
		return list
	}
	return func() ([]cableSighting, error) {
		seen, err := list()
		out := seen[:0:0]
		for _, c := range seen {
			if (!c.isOrb && !c.espressif) || c.mac == s.onlyOrb {
				out = append(out, c)
			}
		}
		return out, err
	}
}
