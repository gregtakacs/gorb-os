package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strings"
	"time"

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
//
// The name goes to the DEVICE first and to the hub's file second. That order is the
// whole authority rule in practice: the orb owns its name, so a rename is not a
// success until the board agrees, and a hub that stored the word first would be left
// holding a name the orb refused — which then points every later credential lookup at
// a word this board does not answer to. A board the hub cannot dial is therefore not
// nameable right now, and says so, rather than being half-renamed in a file.
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
	// Cleaned and checked against the DEVICE's rules before the store is touched or
	// a packet is sent, so an accented or over-long name gets one refusal from the
	// hub rather than a 400 attributed to an IP.
	clean, err := orbfleet.CleanName(req.Name)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	if err := orbfleet.CheckNameASCII(clean); err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	row, ok := s.fleet.ByMAC(req.MAC)
	if !ok {
		// The roster knows things the store does not: a board on a cable this sweep
		// read a MAC from is seen, not unknown. Say which plug it is on rather than
		// "never announced", which a cable makes false, and still refuse the write — a
		// descriptor sighting earns a row in the list, not a row in state.json.
		canon, badmac := orbmdns.CanonicalMAC(req.MAC)
		if badmac == "" {
			if port := s.cables.cablePortFor(canon, time.Now()); port != "" {
				s.jsonErr(w, "no orb "+req.MAC+" to name: on a cable at "+port+
					" with no address yet, so there is nothing to write to — identify it or bring it onto the LAN first")
				return
			}
		}
		s.jsonErr(w, "no orb "+req.MAC+" to name: it has never announced and no cable carries it now")
		return
	}
	// The cross-lock BEFORE the dial. Order matters more here than anywhere else in
	// this handler: put the name on the board first and let the fleet refuse it
	// second, and the orb now carries a word the hub will not record — the exact drift
	// this whole arrangement exists to remove, arrived at by a route the operator
	// caused. Refusing first costs them nothing but a keystroke.
	if err := s.fleet.CheckNameFree(req.MAC, req.Name); err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	// Always dial, even when the cleaned value already matches the cache. Naming is
	// a human clicking a button, not a hot path, and comparing against a cache first
	// would skip the request precisely when the cache is the thing that is wrong —
	// most sharply on a clear, where an empty cache and a device carrying a name look
	// identical from here and the board would keep the old word forever.
	if err := s.writeNameToDevice(row, clean); err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	// Both fields are applied or neither reports success: a row left half-renamed
	// reads as the operator's typo rather than the hub's half-measure. The device has
	// said yes and the cross-lock has already been paid, so the only way SetName can
	// still refuse is a rename that landed between the check and the write — rare, and
	// self-correcting: the orb holds the new word and the next status read folds it in.
	if err := s.fleet.SetName(req.MAC, req.Name); err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	if err := s.fleet.SetRoom(req.MAC, req.Room); err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	s.saveState()
	row, _ = s.fleet.ByMAC(req.MAC)
	s.jsonOK(w, map[string]any{"orb": row})
}

// foldDeviceName refreshes a row's cached name from a status body the proxy already
// delivered to the browser. It is the read half of the arrangement: POST
// /api/v1/name puts a name on a board, and this is what pulls it back off the board
// when the OTHER owner of the fact — a second hub, the onboarding flow, a curl on the
// bench — is the one that wrote it.
//
// The row is keyed on the MAC inside the body rather than on the address it was
// fetched from. An address is something an orb has at a moment; the MAC in the same
// payload is something an orb is, and a fold that trusted the address could be aimed
// at whichever board had just taken the lease. The proxy's own grant check is what
// stops an arbitrary address being dialed at all.
//
// A name of "" is a real answer and clears the cache; a body that does not parse, or
// that carries no name key at all, is not an answer. The difference is the whole point:
// a dropped packet, a board still running firmware from before the field existed, and a
// reply truncated by the proxy's own cap are all ordinary events, and a fold that read
// any of them as "no name" would un-name an orb for the crime of answering slowly.
//
// Which is also why this decodes its own probe struct rather than orbnet.Status: a
// string field cannot tell an absent key from a value of "", and the one thing this
// function may not do is guess wrong about which of the two it is looking at.
func (s *Server) foldDeviceName(ip string, body []byte, want bool) {
	if !want || len(body) == 0 || s.fleet == nil {
		return
	}
	var probe struct {
		MAC  string  `json:"mac"`
		Name *string `json:"name"`
	}
	if err := json.Unmarshal(body, &probe); err != nil || probe.MAC == "" || probe.Name == nil {
		return
	}
	name := *probe.Name
	row, changed, err := s.fleet.NoteDeviceName(probe.MAC, name)
	if err != nil || !changed {
		return
	}
	log.Printf("orb-manager: %s reports its own name as %q (%s)", row.DisplayName(), name, ip)
	// Persisted on a change and not otherwise, so a card poll every few seconds does
	// not rewrite the state file on a loop. This is the same rule reconcileFleet
	// follows about not writing on no news.
	s.saveState()
}

// writeNameToDevice puts the name on the board itself. An empty name is a clear and
// still goes over the wire: "unnamed" is a state the device stores, not just the
// absence of one.
//
// It refuses rather than best-effort. The tempting alternative — store the hub's
// string, try the device, log the failure — is what made the old arrangement wrong:
// the card and the orb drifting apart, with the card winning the argument because it
// renders first.
func (s *Server) writeNameToDevice(row orbfleet.Orb, name string) error {
	if row.IP == "" {
		if row.Cable {
			return fmt.Errorf("%s is on a cable but has no address yet: bring it onto the LAN (POST /api/v1/wifi/join) before naming it", row.DisplayName())
		}
		return fmt.Errorf("%s has no address the hub can dial: name it from the Manage card once its address is known", row.DisplayName())
	}
	if ok, why := s.mayDial(row.IP); !ok {
		return fmt.Errorf("%s: %s", row.DisplayName(), why)
	}
	if _, err := s.device.PostName(row.IP, name); err != nil {
		return fmt.Errorf("%s refused the name (%v); nothing was renamed", row.DisplayName(), err)
	}
	return nil
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
