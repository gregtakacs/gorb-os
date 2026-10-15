package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strings"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
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
			continue
		}
		// An orb announcing itself is evidence of orb life no lonely mark can
		// outweigh: the chip runs orb firmware again, whatever this hub wrote
		// to it last.
		s.lonely.clear(c.MAC)
		if renew && c.IP != "" {
			s.grants.renew(c.IP, c.MAC)
		}
	}
}

// orbmdnsToObservation projects a browse record into a store observation. At carries
// the time the browser actually HEARD the packet, not the time of this fold: the card
// rebuild re-projects the whole cache every wake, and stamping fold-time would let an
// unplugged board refresh its presence clock from the hub's own memory for the two
// minutes the browser holds the record. The Forget gate reads that clock (liveness.go);
// a clock the hub winds itself is a gate that never opens.
func orbmdnsToObservation(c orbmdns.Candidate) orbfleet.Observation {
	return orbfleet.Observation{
		MAC: c.MAC, Instance: c.Instance, IP: c.IP, Port: c.Port, FW: c.FW, State: c.State,
		At: c.LastSeen,
	}
}

// seeCable records a descriptor sighting — the host enumerated this MAC on this port —
// without claiming the chip was read over that lead. It is what makes remembering
// automatic: every board the sweep can attribute earns a row, and the row is what lets a
// later packet fold an address onto the same MAC. Chip reads (a flash's eFuse, a console
// verdict, an identify) still go through markCable, which outranks packets.
func (s *Server) seeCable(mac, port string) {
	if s.fleet == nil {
		return
	}
	if _, err := s.fleet.Confirm(mac); err != nil {
		log.Printf("orb-manager: cable sighting not recorded (%s): %v", mac, err)
		return
	}
	log.Printf("orb-manager: %s seen on %s", mac, port)
}

// markCable records that the hub read this MAC off the chip itself. Every cable path
// calls it — a flash's eFuse read, a console boot verdict — because that is the one
// sighting an announcement cannot argue with.
func (s *Server) markCable(mac string) {
	if s.fleet == nil {
		return
	}
	row, err := s.fleet.Confirm(mac)
	if err != nil {
		log.Printf("orb-manager: cable sighting not recorded (%s): %v", mac, err)
		return
	}
	s.markChipRead(row.MAC)
	log.Printf("orb-manager: %s confirmed over USB", row.MAC)
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
		s.jsonOK(w, map[string]any{"orbs": []map[string]any{}, "refused": []orbfleet.Refusal{}})
		return
	}
	// Fold the cache before answering, with no renewal: see foldCandidates.
	s.foldCandidates(false)
	s.jsonOK(w, map[string]any{
		"orbs":    fleetView(s.fleet.All()),
		"refused": s.fleet.Refusals(),
		"learned": s.knownGrants(),
	})
}

// fleetView is the read shape for /api/fleet. The persisted store now carries only the
// address book (mac, ip, port); the display fields and the derived clash flags live on
// the row only for the current frame, so the endpoint maps them out explicitly rather
// than marshaling the store struct — which no longer serializes them by design.
func fleetView(rows []orbfleet.Orb) []map[string]any {
	out := make([]map[string]any, 0, len(rows))
	for _, r := range rows {
		out = append(out, map[string]any{
			"mac": r.MAC, "ip": r.IP, "port": r.Port,
			"name": r.Name, "instance": r.Instance, "fw": r.FW, "state": r.State,
			"note": r.Note, "shared": r.Shared, "contended": r.Contended,
			"displayName": r.DisplayName(),
		})
	}
	return out
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

// handleFleetSet is the operator's side of the identity: give a board a name.
// Guarded, because it writes to disk and because the name is what later points
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
		// descriptor sighting earns a row in the list, not a name the hub can write with
		// no address to dial. Writing over the lead is the Onboard card's job, where the
		// port is already the thing under test.
		canon, badmac := orbmdns.CanonicalMAC(req.MAC)
		if badmac == "" {
			if port := s.cables.cablePortFor(canon, time.Now()); port != "" {
				// The cable sighting is a fact independent of the rename: remember the
				// board is on a lead even as this write is refused, so a failed rename
				// does not un-see it. What it does not do is put a name on the file —
				// the write goes to the board, and there is no board address to reach.
				s.seeCable(canon, port)
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
	// The device accepted it, so the hub's copy is now the board's word: fold it in and
	// persist it, so the row reads by name at the next launch even if the board stays in
	// a drawer until then. There is no room field and there is no write on no change: the
	// board owns the name, the file caches it, and a refused device write never reaches
	// this line.
	_, changed, err := s.fleet.NoteDeviceName(req.MAC, clean)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	if changed {
		s.saveState()
	}
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
	if err != nil {
		return
	}
	// A board that answered a status poll with its own MAC and a name key is running
	// orb firmware, and that outranks any memory of this hub leaving it lonely — the
	// same rule the announcement fold applies. It sits after the store accepted the
	// fold on purpose: the payload is whatever the far end chose to say, so the right
	// to clear a mark is earned by landing on a row the fleet already holds, exactly
	// as the rename above is. A lonely board could not produce this body at all — it
	// opens no HTTP door — so the poll would have failed rather than answered.
	s.lonely.clear(probe.MAC)
	if !changed {
		return
	}
	log.Printf("orb-manager: %s reports its own name as %q (%s)", row.DisplayName(), name, ip)
	// Persisted on a change and not otherwise, so a card poll every few seconds does
	// not rewrite the state file on a loop. This is the same rule reconcileFleet
	// follows about not writing on no news.
	s.saveState()
}

// foldProbeStatus files what one liveness dial learned about a board into the
// fleet row, keyed by the MAC the body itself named. This is the fast path for
// fw and state on a LAN board: the sweep asks every remembered address every ten
// seconds and once at launch, and until now it kept only the yes/no out of a
// body that carried the running version, the state token and the board's name.
// A dial does not renew presence — NoteAnswer is the store call that refuses to
// touch LastSeen, for the reasons orbmdnsToObservation states — it renews facts.
//
// The cross-lock still applies: the fold lands only when the body's MAC names a
// row the store holds AND that row's own address is the one the hub dialed. A
// board whose lease moved answers at the old address with a stranger's face or
// not at all, and an answer from a board at an address its row does not carry is
// the announce's job — the next check-in moves the row's IP and the next dial
// after that folds its facts. A stranger's answer at a known address folds onto
// no row: introducing boards is what Adopt and the browse are for.
//
// The name folds only when the body carried a word. orbnet.Status is a typed read
// and cannot tell an absent key from an empty one, and the rule that only a
// successful status read may CLEAR a cached name belongs to the proxy fold, which
// reads the raw body and can tell. Here absence is old firmware, which is no news.
func (s *Server) foldProbeStatus(dialIP string, st *orbnet.Status) {
	if s.fleet == nil || st == nil || st.MAC == "" {
		return
	}
	row, known := s.fleet.Look(st.MAC)
	if !known || (row.IP != "" && row.IP != dialIP) {
		return
	}
	changed := false
	if _, ch, err := s.fleet.NoteAnswer(st.MAC, st.FW, st.State); err == nil && ch {
		changed = true
	}
	if st.Name != "" {
		if _, ch, err := s.fleet.NoteDeviceName(st.MAC, st.Name); err == nil && ch {
			changed = true
		}
	}
	if !changed {
		return
	}
	log.Printf("orb-manager: %s answers %s as fw %q, state %q", st.MAC, dialIP, st.FW, st.State)
	s.saveState()
	s.wakeOrbs()
}

// foldBootName caches a name the board announced about itself on its boot console,
// which is the only read path for a board with no address: the serial door writes
// NAME and never reads it back, and the status fold needs an address to dial. So a
// board named on another bench, by the CLI, or on its own page before its lease
// moved, still arrives on the card by name the next time its console is open. The
// word goes through the same gate every other name passes, and NoteDeviceName
// updates a row the hub already holds — a boot banner is not how a new orb appears.
// Returns whether the store changed, so the caller knows whether to persist.
func (s *Server) foldBootName(mac, raw string) bool {
	if s.fleet == nil || raw == "" {
		return false
	}
	clean, err := orbfleet.CleanName(raw)
	if err != nil {
		log.Printf("orb-manager: boot banner name ignored (%s): %v", mac, err)
		return false
	}
	if err := orbfleet.CheckNameASCII(clean); err != nil {
		log.Printf("orb-manager: boot banner name ignored (%s): %v", mac, err)
		return false
	}
	if clean == "" {
		return false
	}
	_, changed, err := s.fleet.NoteDeviceName(mac, clean)
	if err != nil {
		log.Printf("orb-manager: boot banner name not stored (%s): %v", mac, err)
		return false
	}
	return changed
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
		// A board on a cable with no address used to be unnamedable at all. The
		// firmware's always-on serial door (orb_net.c NAME) closes that gap: the live
		// sweep says which port carries this MAC right now, so the name goes down the
		// wire the board is actually on. That is the only source, and it is the right
		// one — a remembered socket is a claim about a previous plug-in.
		if port := s.cables.cablePortFor(row.MAC, time.Now()); port != "" {
			return s.serialName(port, row.MAC, name)
		}
		if s.chipRead(row.MAC) {
			return fmt.Errorf("%s is on a cable but the hub cannot say which port: identify it first", row.DisplayName())
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

// serialName writes a NAME command over the cable already carrying this board and
// waits for the firmware's ack, reusing the one serial-command driver the /api/orb/cmd
// door uses. It refuses rather than half-writing: an orb that NAK'd the value keeps its
// old name and the store is not touched.
func (s *Server) serialName(port, mac, name string) error {
	pt, oerr := esprom.OpenPort(port)
	if oerr != nil {
		return fmt.Errorf("%s: %s unreachable for a serial write: %v", mac, port, oerr)
	}
	defer pt.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	line := esprom.SerialLine("NAME", esprom.ProvField(name))
	res, err := esprom.RunCommand(ctx, pt, line, []string{"NAME-ACK"}, nil)
	if res != nil && res.Nak != "" {
		return fmt.Errorf("%s refused the name (%s); nothing was renamed", mac, res.Nak)
	}
	if err != nil {
		return fmt.Errorf("%s: serial name on %s failed: %v", mac, port, err)
	}
	if res == nil || !res.Ack {
		return fmt.Errorf("%s did not acknowledge the name over %s; nothing was renamed", mac, port)
	}
	return nil
}

// storeProvesOrb is the address book's half of orbRow.provesOrb: of everything a
// stored row can carry, these fields are only ever there because a firmware reported
// them, an operator named the board, or this hub wrote firmware into the chip. A row
// created by Confirm — a MAC read off a USB descriptor on a cable sweep — carries none
// of them, so a bare sighting is not evidence the chip has ever been an orb. The port
// entry writes orbSeen from this, and the page's New Orb door reads orbSeen, so the two
// gates the page depends on come from one rule stated once, here and in provesOrb.
func storeProvesOrb(o orbfleet.Orb) bool {
	return o.Name != "" || o.IP != "" || o.FW != "" || o.State != "" || o.Remembered
}

// fleetKnown reports whether the address book already holds this chip, and the
// row it holds. Two doors read it: the port list, to tell a known orb on its
// cable from a blank to adopt, and the by-IP add, to refuse adopting a board the
// operator can already see. It sits here rather than inline because both doors
// must weigh the same fact the same way, and a nil store is the same answer as
// an empty one.
func (s *Server) fleetKnown(mac string) (orbfleet.Orb, bool) {
	if s.fleet == nil || mac == "" {
		return orbfleet.Orb{}, false
	}
	c, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		return orbfleet.Orb{}, false
	}
	// A board this hub left lonely is not a known orb even while its row is
	// being rebuilt from a cable descriptor: the lonely flash forgot the row,
	// and the mark says the sighting describes a chip running no orb.
	if s.lonely.has(c) {
		return orbfleet.Orb{}, false
	}
	// Every, not ByMAC: the question is whether the hub has ever met this chip,
	// not whether its last sighting is fresh. A board in a drawer is still the
	// board the operator named, and a gate that blinked with the TTL would
	// advertise a known orb as blank to adopt once an hour.
	for _, o := range s.fleet.Every() {
		if o.MAC == c {
			return o, true
		}
	}
	return orbfleet.Orb{}, false
}

// handleFleetForget takes a board out of the fleet and revokes the addresses it held.
// Forgetting the identity while the hub still dials the address would leave the exact
// thing the operator was trying to get rid of.
//
// The gate is the same forgetGate the card reads, called with confirm=true, so an answer
// the sweep has not priced yet is priced here: one dial, once per button press, at the
// only point where deleting something is about to happen. A button the card should not
// have drawn is refused here, and the row comes back on the next frame.
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
	if ok, why := s.forgetGate(req.MAC, time.Now(), true); !ok {
		s.jsonErr(w, why)
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
