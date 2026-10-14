package main

// Adopting a board the hub cannot hear.
//
// The fleet is normally built by ears: an mDNS announce lands, a cable sweep
// attributes a MAC, a row appears without anyone pressing anything. Both ears can
// fail at once for reasons that have nothing to do with the orb — a network that
// blocks multicast, a board that took a new lease while its announce stayed
// broken — and the board is sitting right there at an address the operator can
// read off their router. These two routes are the mouth: the operator names an
// address, the hub asks it who it is, and only the orb's own answer decides
// whether a row is created or the dialog says no.
//
//  1. POST /api/orb/add {ip} — ask one address to identify itself. Only an orb
//     answering its own /api/v1 status earns a fleet row and a grant, and the
//     row is written with the identity in the reply, not with whatever was typed.
//  2. POST /api/orb/update {ip, force} — move one orb onto the firmware this hub
//     carries, over the LAN, through the device's own staging endpoint. The page
//     never posts a raw image, so the proxy's standing refusal of /api/v1/ota
//     stays exactly where the security comment wants it: the only route that
//     pushes an image is this one, it dials addresses the hub already grants, it
//     flashes bytes from the hub's own verified bundle, and the device still
//     validates the image before it will boot it.

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
)

// handleOrbAdd is the Add-by-IP dialog's door. The address must be a plain IPv4
// literal, and the addresses this machine could never meaningfully dial are
// refused in words here, so the dialog reports them as its own refusals rather
// than as a mysterious timeout eight seconds later.
func (s *Server) handleOrbAdd(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		IP string `json:"ip"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		s.jsonErr(w, "bad request: "+err.Error())
		return
	}
	ip := strings.TrimSpace(req.IP)
	if !isIPv4(ip) {
		s.jsonErr(w, "that is not an IPv4 address: "+ip)
		return
	}
	// The addresses that can never be a board: the wildcard, multicast, and the
	// broadcast address. Loopback is deliberately absent from this list — the
	// bench fake orb is served from here, and the token-gated dialog was always
	// as trustworthy pointed at 127.0.0.1 as the typed box it replaces.
	if ip == "0.0.0.0" || strings.HasPrefix(ip, "224.") || strings.HasPrefix(ip, "255.") {
		s.jsonErr(w, ip+" is not an address an orb can hold")
		return
	}
	st, err := s.device.Status(ip)
	if err != nil {
		// The one sentence the dialog needs: nothing was learned, nothing was
		// granted, try another address. A wrong typing costs a round-trip and
		// nothing else — the grant set does not move on a failed check.
		s.jsonErr(w, "no orb answering at "+ip+": "+plainDialError(err))
		return
	}
	// A board that answers as an orb but reports no MAC gets a grant and no row:
	// the fleet keys on the chip, and a row with no key is two boards fighting
	// over one address-book entry the day the second one appears.
	mac, badMac := orbmdns.CanonicalMAC(st.MAC)
	known := false
	if badMac == "" {
		_, known = s.fleetKnown(mac)
	}
	if badMac == "" && !known {
		if _, err := s.fleet.Observe(orbfleet.Observation{
			MAC: mac, IP: ip, FW: st.FW, State: st.State,
			Instance: "orb-" + mac[len(mac)-6:], At: time.Now(),
		}); err != nil {
			s.jsonErr(w, "the orb at "+ip+" refused to join the fleet: "+err.Error())
			return
		}
	}
	if badMac == "" && st.Name != "" {
		if _, _, err := s.fleet.NoteDeviceName(mac, st.Name); err != nil {
			s.jsonErr(w, "the orb at "+ip+" is on the wire but its name would not fold: "+err.Error())
			return
		}
	}
	// A grant bound to a MAC is a commitment to THIS board at this address, which
	// is the only kind worth making from a typed one: the cross-lock in mayDial
	// refuses the moment the lease moves to a different board. A board with no
	// MAC still earns an unbound grant — dial-able and honest about being
	// unverified, exactly like the typed address of the old Load box.
	bind := ""
	if badMac == "" {
		bind = mac
	}
	s.grants.learnAs(ip, bind)
	s.setOrbIP(ip)
	s.saveState()
	s.wakeOrbs()
	row := map[string]any{"ip": ip, "mac": bind, "fw": st.FW, "name": st.Name,
		"inFleet": known}
	if badMac != "" {
		row["note"] = "the device answered as an orb but reported no mac, so it is reachable and unverified"
	}
	s.jsonOK(w, row)
}

// plainDialError trims the package prefix off an orbnet error so the dialog's
// sentence is a refusal an operator can read, not a Go string.
func plainDialError(err error) string {
	msg := err.Error()
	if i := strings.Index(msg, ": "); i >= 0 && strings.HasPrefix(msg, "orbnet:") {
		msg = msg[i+2:]
	}
	return msg
}

// handleOrbUpdate stages this hub's app image into the orb's spare slot over the
// LAN, and reboots only after the device has validated it — the same two-request
// shape orb_net.c's h_ota documents, kept as two requests server-side so a bad
// upload never costs a household its clock.
func (s *Server) handleOrbUpdate(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		IP    string `json:"ip"`
		Force bool   `json:"force"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		s.jsonErr(w, "bad request: "+err.Error())
		return
	}
	ip := strings.TrimSpace(req.IP)
	if !isIPv4(ip) {
		s.jsonErr(w, "not an IPv4 address: "+ip)
		return
	}
	// The same gate every other off-box dial passes: granted, and not contradicted
	// by the fleet. /api/orb/update is the one route that may reach /api/v1/ota,
	// and it reaches no address this hub has not already committed to.
	if ok, why := s.mayDial(ip); !ok {
		s.jsonErr(w, why)
		return
	}
	b := s.firmwareBundle()
	if b == nil {
		s.jsonErr(w, "this hub carries no firmware to update with")
		return
	}
	app := b.AppImage()
	if len(app) == 0 {
		s.jsonErr(w, "the hub's firmware carries no app image to push")
		return
	}
	// The device answers for itself before anything is written, and its reply is
	// the input to the version gate: the hub refuses a downgrade or a no-op on
	// the orb's own reported stamp, not the fleet's cached one.
	st, err := s.device.Status(ip)
	if err != nil {
		s.jsonErr(w, "no orb answering at "+ip+": "+plainDialError(err))
		return
	}
	verdict := firmware.Compare(firmware.Release{Version: st.FW}, b.Release())
	if !req.Force {
		switch verdict {
		case firmware.VerdictUpgrade:
		case firmware.VerdictDowngrade:
			s.jsonErr(w, ip+" runs "+st.FW+", newer than this hub's "+b.Version+
				"; the hub will not push it as a downgrade")
			return
		default:
			s.jsonErr(w, ip+" already runs this firmware ("+st.FW+")")
			return
		}
	}

	// h_ota takes Content-Length and erases exactly that much, so the client must
	// be the patient one: the 8-second management budget would cancel every
	// honest update while the flash is still writing. s.ota is that budget.
	ctx, cancel := context.WithTimeout(r.Context(), otaTimeout)
	defer cancel()
	res, err := s.ota.PostImage(ctx, ip, "/api/v1/ota", "application/octet-stream", app)
	if err != nil {
		// A stalled or refused upload never touched the running image: staging is
		// all-or-nothing (esp_ota_end validates, set_boot_partition commits), so
		// the honest report is "nothing moved" plus the device's own reason.
		s.jsonErr(w, "the orb refused the update: "+plainDialError(err)+deviceReason(res))
		return
	}
	// Staged and committed as next-to-boot. The reboot fires here because the
	// operator clicked Update on this very orb, and an acknowledgment that never
	// arrives (a dead radio, a board gone mid-upload) says what is already true:
	// the slot will boot at the next restart whenever the board comes back.
	if _, err := s.device.Reboot(ip); err != nil {
		s.jsonErr(w, "staged, but the orb did not acknowledge the reboot: "+plainDialError(err)+
			" — the new image is already the one it will boot; reboot it when you are ready")
		return
	}
	s.jsonOK(w, map[string]any{
		"ip": ip, "mac": st.MAC, "from": st.FW, "to": b.Version,
		"verdict": string(verdict), "rebooting": true,
		"note": "the orb leaves the wire for one boot and returns with the new stamp",
	})
}

// deviceReason appends the orb's own refusal text when it sent one, because
// h_ota always answers with a reason and the operator deserves the device's
// sentence rather than a bare HTTP status.
func deviceReason(body json.RawMessage) string {
	if len(body) == 0 {
		return ""
	}
	var j struct {
		Error  string `json:"error"`
		Detail string `json:"detail"`
	}
	if json.Unmarshal(body, &j) != nil || j.Error == "" {
		return ""
	}
	if j.Detail != "" {
		return " (" + j.Error + ": " + j.Detail + ")"
	}
	return " (" + j.Error + ")"
}
