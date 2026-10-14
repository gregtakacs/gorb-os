package main

import (
	"encoding/json"
	"log"
	"net/http"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
)

// Who the hub believes is behind the addresses it is willing to dial.
//
// The grant set answers "may this address be reached". This file answers the other
// question, which the grant set cannot: "is the thing at that address still the board
// the operator committed to". Item 5 gave the hub a new way to learn an address —
// somebody else's packet — and a MAC that only ever arrives over UDP is a claim, not
// a credential. Pairing (item 9) is what eventually turns the second kind into the
// first. Until then the fleet is used only to take dials away, never to add one:
// mayDial can refuse a granted address, and nothing here can grant an address that
// grants.allowed would refuse.
//
// Three sources meet here, and they are not equal:
//
//   - the cable: the flasher's eFuse read and the USB serial descriptor, both the BASE
//     MAC (orb_net.c uses ESP_MAC_BASE, hardware-verified against esptool on orb-B);
//   - the wire: the TXT mac= of an mDNS announcement, checked against the instance
//     name the same packet carries;
//   - the operator's word: the address they typed, or the row they clicked.
//
// When the cable has spoken about a board, the wire cannot contradict it.

// mayDial is the one answer to "what happens if the page asks for /orb/<ip>/...".
// The grant still decides whether the address is reachable at all; the fleet can only
// make a granted address unreachable, and only when staying silent would mean dialing
// a board other than the one the operator named.
func (s *Server) mayDial(ip string) (bool, string) {
	if !s.grants.allowed(ip) {
		return false, "the hub will not dial " + ip +
			": it is not an orb address this hub learned over USB or was given. Enter the orb's address in the Manage card first."
	}
	if s.fleet == nil {
		return true, ""
	}
	claims := s.fleet.ClaimedBy(ip)
	switch len(claims) {
	case 0:
		// Nothing announced this address, which is the normal case for a typed
		// address or an orb whose announcements never reached us. Absence of
		// contradiction is not proof of identity, so this path stays available —
		// refusing it would make the hub unusable on a LAN it cannot browse —
		// but the card marks the grant unbound rather than pretending otherwise.
		return true, ""
	case 1:
		bound := s.grants.bound(ip)
		if bound != "" && claims[0].MAC != bound {
			// The commitment named a board and the address now answers to a
			// different one. Revoking rather than merely refusing is the point:
			// the next click has to be a fresh decision by a human, not a retry.
			s.grants.revoke(ip)
			return false, ip + " now announces itself as " + claims[0].MAC +
				", but this address was allowed for " + bound +
				". The grant is revoked; browse again and load the orb you want."
		}
		return true, ""
	default:
		macs := make([]string, 0, len(claims))
		here := false
		for _, c := range claims {
			macs = append(macs, c.MAC)
			if c.MAC == s.grants.bound(ip) {
				here = true
			}
		}
		bound := s.grants.bound(ip)
		if bound != "" && !here {
			// Two other boards are arguing over an address the operator committed to
			// for a board that is not among them. That is not a transient: the board
			// they meant is gone and somebody else holds its lease.
			s.grants.revoke(ip)
			return false, ip + " is announced by " + lenWord(len(claims)) + " boards (" +
				strings.Join(macs, ", ") + "), none of them " + bound +
				", the board this address was allowed for. The grant is revoked."
		}
		// The committed board is one of the claimants, so this is a contention that may
		// resolve by itself: two rows appearing for one address is what a DHCP
		// handover looks like from here, and the old lease stops being announced
		// within a TTL. Refusing to dial is the safe answer for now; destroying a
		// commitment the operator made for the contention to blow over is not, because
		// it would mean re-clicking the orb every time the router moved a lease.
		return false, ip + " is announced by " + lenWord(len(claims)) + " boards (" +
			strings.Join(macs, ", ") + "); the hub cannot tell which one the page meant, so it dials neither"
	}
}

func lenWord(n int) string {
	if n == 2 {
		return "two"
	}
	if n == 3 {
		return "three"
	}
	return "several"
}

// reconcileFleet folds the current browse cache into the fleet and lets a board that
// is still announcing keep the commitment it was given. main calls it on the upkeep
// tick, and no GET route does: replaying what the hub already heard must not be able
// to extend a dial's life, so only the timer renews — and that timer renews from the
// cache, which only a packet the orb sent first can fill. The tick sends nothing;
// what renews a grant is the orb's own check-in landing in the cache before the tick
// reads it.
func (s *Server) reconcileFleet() {
	s.foldCandidates(true)
}

// foldCandidates puts what the browse heard into the fleet.
//
// renew is the parameter that matters. Only the timer may renew a grant's life, so a
// page that polls the card cannot extend a dial by asking for it; the LAN has to keep
// producing the board. Everything else here is a projection of the cache the hub
// already holds, which is why the read handlers may call it: observing an announcement
// grants nothing, because reachability is still decided by grants.allowed alone.
func (s *Server) foldCandidates(renew bool) {
	if s.fleet == nil {
		return
	}
	s.dmu.Lock()
	b := s.browse
	s.dmu.Unlock()
	if b == nil {
		return
	}
	for _, c := range b.Candidates() {
		if _, err := s.fleet.Observe(orbmdnsToObservation(c)); err != nil {
			// A candidate the browser promoted and the fleet will not fold is a
			// disagreement between two of our own checks. It stays on the refused
			// list rather than becoming a row nobody explained.
			log.Printf("orb-manager: fleet refused %s: %v", c.Instance, err)
		}
		if renew && c.IP != "" {
			s.grants.renew(c.IP, c.MAC)
		}
	}
}

func orbmdnsToObservation(c orbmdns.Candidate) orbfleet.Observation {
	return orbfleet.Observation{
		MAC: c.MAC, Instance: c.Instance, IP: c.IP, Port: c.Port, FW: c.FW, State: c.State,
	}
}

// markCable records that the hub read this MAC off the chip itself. Every cable path
// calls it — a flash's eFuse read, a console boot verdict — because that is the one
// sighting an announcement cannot argue with.
func (s *Server) markCable(mac string) {
	if s.fleet == nil {
		return
	}
	row, err := s.fleet.MarkCable(mac)
	if err != nil {
		log.Printf("orb-manager: cable sighting not recorded (%s): %v", mac, err)
		return
	}
	log.Printf("orb-manager: %s confirmed over USB as %s", row.DisplayName(), row.MAC)
}

// --- the fleet endpoints ----------------------------------------------------------

// handleFleet is the read side: every board the hub has met, named or not. Unguarded
// like /api/orbs and /api/ports: it is local state the operator already owns, and the
// dangerous surface is the one that sends a packet.
func (s *Server) handleFleet(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	if s.fleet == nil {
		s.jsonOK(w, map[string]any{"orbs": []orbfleet.Orb{}, "refused": []orbfleet.Refusal{}})
		return
	}
	// Fold the cache before answering, with no renewal: see foldCandidates.
	s.foldCandidates(false)
	s.jsonOK(w, map[string]any{
		"orbs":    s.fleet.All(),
		"refused": s.fleet.Refusals(),
		"learned": s.knownGrants(),
	})
}

// knownGrants pairs each remembered address with the board it was committed for, so
// the card can tell an allowed-and-cross-locked row from an allowed-and-hoping row.
func (s *Server) knownGrants() []map[string]any {
	out := []map[string]any{}
	for _, ip := range s.grants.list() {
		m := s.grants.bound(ip)
		out = append(out, map[string]any{"ip": ip, "mac": m, "bound": m != ""})
	}
	return out
}

// handleFleetSet is the operator's side of the identity: give a board a name and a
// room. Guarded, because it writes to disk and because the name is what later points
// a credential: a page that could rename an orb could aim whatever the hub stores for
// it at a different board.
func (s *Server) handleFleetSet(w http.ResponseWriter, r *http.Request) {
	var req struct {
		MAC  string `json:"mac"`
		Name string `json:"name"`
		Room string `json:"room"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		s.jsonErr(w, "bad request: "+err.Error())
		return
	}
	if s.fleet == nil {
		s.jsonErr(w, "no fleet is configured on this hub")
		return
	}
	// Both fields are applied or neither reports success: a row left half-renamed
	// reads as the operator's typo rather than the hub's half-measure.
	if err := s.fleet.SetName(req.MAC, req.Name); err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	if err := s.fleet.SetRoom(req.MAC, req.Room); err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	s.saveState()
	row, _ := s.fleet.ByMAC(req.MAC)
	s.jsonOK(w, map[string]any{"orb": row})
}

// handleFleetForget takes a board out of the fleet and revokes the addresses it held.
// Forgetting the identity while the hub still dials the address would leave the exact
// thing the operator was trying to get rid of.
func (s *Server) handleFleetForget(w http.ResponseWriter, r *http.Request) {
	var req struct {
		MAC string `json:"mac"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		s.jsonErr(w, "bad request: "+err.Error())
		return
	}
	if s.fleet == nil {
		s.jsonErr(w, "no fleet is configured on this hub")
		return
	}
	held, err := s.fleet.Forget(req.MAC)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	// An address another live board still claims is not this one's to revoke.
	for _, ip := range held {
		if len(s.fleet.ClaimedBy(ip)) == 0 {
			s.grants.revoke(ip)
		}
	}
	s.saveState()
	s.jsonOK(w, map[string]any{"forgotten": req.MAC, "revoked": held})
}
