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
		// Two different reasons, and the operator can only act on the right one:
		// a bundle whose manifest names no app image is a broken file, while a
		// whole-flash image is exactly what it claims and simply has no separable
		// slot to push. Say which, so the fix is in the message.
		if b.Merged {
			s.jsonErr(w, "the hub's firmware is a whole-flash image written at 0x0, which has no app slot to push over WiFi — flash this orb over the cable, or load a .zip bundle to update it wirelessly")
			return
		}
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
			// The one refusal on this route that has a yes behind it. The verdict
			// travels as a field rather than only as prose so the page can recognise
			// a downgrade without parsing a sentence, and so a scripted caller sees the
			// same structure. Rollback is enabled in the bootloader, which is what makes
			// retrying with force a recoverable act rather than a leap of faith.
			s.jsonErrFields(w, ip+" runs "+st.FW+", newer than this hub's "+b.Version+
				"; forcing it pushes a downgrade over Wi-Fi", map[string]any{
				"verdict": string(verdict), "overridable": true,
				"from": st.FW, "to": b.Version,
			})
			return
		default:
			s.jsonErrFields(w, ip+" already runs this firmware ("+st.FW+")", map[string]any{
				"verdict": string(verdict),
			})
			return
		}
	}

	// h_ota takes Content-Length and erases exactly that much, so the client must
	// be the patient one: the 8-second management budget would cancel every
	// honest update while the flash is still writing. s.ota is that budget.
	//
	// The tracker is this push's shadow: every phase transition and every byte
	// the transport consumes lands here, and the page reads it back over
	// /api/orb/update/progress while this POST is still open. The orb itself
	// cannot carry that report — h_ota holds the one httpd task for the whole
	// upload, so the board answers no status request while its flash is written
	// — which leaves the writer's byte count as the only live figure on the wire.
	ctx, cancel := context.WithTimeout(r.Context(), otaTimeout)
	defer cancel()
	s.updSet(ip, "uploading", st.FW, b.Version, 0, int64(len(app)))
	res, err := s.ota.PostImageProgress(ctx, ip, "/api/v1/ota", "application/octet-stream", app,
		func(sent, total int64) { s.updSent(ip, sent, total) })
	if err != nil {
		// A stalled or refused upload never touched the running image: staging is
		// all-or-nothing (esp_ota_end validates, set_boot_partition commits), so
		// the honest report is "nothing moved" plus the device's own reason.
		s.updSet(ip, "failed", st.FW, b.Version, 0, int64(len(app)))
		s.jsonErr(w, "the orb refused the update: "+plainDialError(err)+deviceReason(res))
		return
	}
	s.updSet(ip, "staged", st.FW, b.Version, int64(len(app)), int64(len(app)))
	// Staged and committed as next-to-boot. The reboot fires here because the
	// operator clicked Update on this very orb, and an acknowledgment that never
	// arrives (a dead radio, a board gone mid-upload) says what is already true:
	// the slot will boot at the next restart whenever the board comes back.
	if _, err := s.device.Reboot(ip); err != nil {
		s.updSet(ip, "staged, no reboot ack", st.FW, b.Version, int64(len(app)), int64(len(app)))
		s.jsonErr(w, "staged, but the orb did not acknowledge the reboot: "+plainDialError(err)+
			" — the new image is already the one it will boot; reboot it when you are ready")
		return
	}
	s.updSet(ip, "rebooting", st.FW, b.Version, int64(len(app)), int64(len(app)))
	// Fold the stamp this hub just wrote into the slot and committed as
	// next-to-boot. The row's amber badge is a comparison of the cached stamp
	// against the bundle, and a board mid-reboot cannot answer for itself yet:
	// without the fold the card keeps promising the update it just took until
	// the next announce or the ten-second sweep re-dials it. The board's next
	// real answer overwrites this one through NoteAnswer either way, so a
	// rollback that reverts the boot rewrites the row back within a sweep.
	if s.fleet != nil && st.MAC != "" {
		if _, ch, ferr := s.fleet.NoteAnswer(st.MAC, b.Version, ""); ferr == nil && ch {
			s.saveState()
			s.wakeOrbs()
		}
	}
	// The update just made every serial answer about this chip wrong, and the
	// sweep that first read it will never read it again on its own — that is
	// the rule this call exists to lift: an orb updated under this hub is
	// asked over its lead once more, and the running stamp comes back off the
	// wire while the radio is still rejoining. Async and silent by design;
	// a board with no cable, or one still booting past the budget, loses
	// nothing the Wi-Fi probe will not fold moments later.
	go s.rereadCableAfterUpdate(st.MAC)
	s.jsonOK(w, map[string]any{
		"ip": ip, "mac": st.MAC, "from": st.FW, "to": b.Version,
		"verdict": string(verdict), "rebooting": true,
		"note": "the orb leaves the wire for one boot and returns with the new stamp",
	})
}

// ---- the upload tracker -------------------------------------------------------
//
// upd is the live state of the hub's Wi-Fi pushes, keyed by address. One update
// per board at a time is already the law (h_ota answers 409 to a second upload,
// and the route dials one address per request), so one entry per ip is all the
// map ever holds for a board. Entries are kept until an hour after the push
// ended — a page opened on the progress route an hour later is asking about a
// done push, and "done" is the answer, not a 404 about a bookkeeping detail.

type uploadState struct {
	phase       string
	sent, total int64
	from, to    string
	at          time.Time
}

func (s *Server) updSet(ip, phase, from, to string, sent, total int64) {
	s.updMu.Lock()
	defer s.updMu.Unlock()
	if s.uploads == nil {
		s.uploads = map[string]*uploadState{}
	}
	u := s.uploads[ip]
	if u == nil {
		u = &uploadState{}
		s.uploads[ip] = u
	}
	u.phase, u.from, u.to, u.at = phase, from, to, time.Now()
	if total > 0 {
		u.total = total
	}
	if phase == "uploading" {
		u.sent = sent
	} else if sent > 0 {
		u.sent = sent
	}
}

func (s *Server) updSent(ip string, sent, total int64) {
	s.updMu.Lock()
	defer s.updMu.Unlock()
	if u := s.uploads[ip]; u != nil {
		u.sent, u.total, u.at = sent, total, time.Now()
	}
}

// handleOrbUpdateProgress answers the one question the flash modal cannot ask
// the orb while the hub is mid-upload: how far in is it. Read of hub state
// alone — no dial, no token past the control gate, no address permission
// beyond naming a well-formed ip — because during an upload the board answers
// nothing at all, which is the whole reason this route exists.
func (s *Server) handleOrbUpdateProgress(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		s.jsonErr(w, "GET only")
		return
	}
	ip := strings.TrimSpace(r.URL.Query().Get("ip"))
	if !isIPv4(ip) {
		s.jsonErr(w, "not an IPv4 address: "+ip)
		return
	}
	s.updMu.Lock()
	u := s.uploads[ip]
	var out any
	if u != nil {
		pct := 0
		if u.total > 0 {
			pct = int(u.sent * 100 / u.total)
			if pct > 100 {
				pct = 100
			}
		}
		out = map[string]any{
			"phase": u.phase, "sent": u.sent, "total": u.total, "pct": pct,
			"from": u.from, "to": u.to, "ago": int(time.Since(u.at).Seconds()),
		}
	}
	s.updMu.Unlock()
	if u == nil {
		out = map[string]any{"phase": "idle", "sent": 0, "total": 0, "pct": 0}
	}
	s.jsonOK(w, out)
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
