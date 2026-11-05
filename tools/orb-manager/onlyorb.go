package main

import (
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
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

// heldState is the other orbs' share of the state file. A development hub on :3779
// shares its config dir with every release launch, so it must not forget a board
// just because it was told not to look at it.
type heldState struct {
	fleet  []orbfleet.Orb
	grants map[string]string // ip -> the MAC it belongs to
}

// holdOthers takes every other orb's remembered row and grant out of st before the
// hub loads it. A grant goes with its MAC, and an unbound one (a v1 file's) goes when
// its address is a held row's: otherwise the card comes back from the file and the
// address stays dialable, which is how the other clone's orb reached this hub's page.
func (s *Server) holdOthers(st *hubState) {
	if s.onlyOrb == "" {
		return
	}
	heldIP := map[string]string{}
	keep := st.Fleet[:0:0]
	for _, r := range st.Fleet {
		if s.admitsMAC(r.MAC) {
			keep = append(keep, r)
			continue
		}
		s.held.fleet = append(s.held.fleet, r)
		if r.IP != "" {
			heldIP[r.IP] = r.MAC
		}
	}
	st.Fleet = keep
	for ip := range st.Grants {
		mac := st.MACs[ip]
		if mac == "" {
			mac = heldIP[ip]
		}
		if mac == "" || s.admitsMAC(mac) {
			continue
		}
		if s.held.grants == nil {
			s.held.grants = map[string]string{}
		}
		s.held.grants[ip] = mac
		delete(st.Grants, ip)
		delete(st.MACs, ip)
	}
}

// writeBack returns the held share to a file about to be saved. This hub's own rows
// win an address both claim.
func (h heldState) writeBack(st *hubState) {
	ips := map[string]bool{}
	for _, r := range st.Fleet {
		ips[r.IP] = true
	}
	for _, r := range h.fleet {
		if !ips[r.IP] {
			st.Fleet = append(st.Fleet, r)
		}
	}
	for ip, mac := range h.grants {
		if _, ok := st.Grants[ip]; !ok {
			st.Grants[ip] = int(time.Now().Unix())
			st.MACs[ip] = mac
		}
	}
}
