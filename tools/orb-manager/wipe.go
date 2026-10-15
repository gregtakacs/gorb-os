package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
)

// wipeRequest is the UI's whole-chip erase. Confirm must read exactly WIPE, in
// caps: the request destroys the Wi-Fi credentials along with everything else,
// and a checkbox that rides along to the wrong button click is not what stops
// that. The hub checks the word before it opens the port.
type wipeRequest struct {
	Port    string `json:"port"`
	Confirm string `json:"confirm"`
	Verify  *bool  `json:"verify,omitempty"` // nil = verify; false skips the blank check
}

// wipeConfirmWord is the typed confirmation /api/wipe demands. Documented on the
// button so the operator is never guessing at the magic string.
const wipeConfirmWord = "WIPE"

// handleWipe erases one port's whole flash back to blank, streaming NDJSON events
// the way /api/flash does. It is the opposite of the flash handler: nothing is
// written afterwards, and the orb is left unable to boot, announce itself, or
// answer the hub until it is flashed.
//
// The hub's own memory of that board goes with it. A wiped orb keeps its eFuse
// MAC — that is burned, not stored — so the fleet row, its name, and any address
// the hub had granted for it are all forgotten on success. Keeping them would be
// the ghost-in-the-list bug: the card would show an orb that provably no longer
// runs anything, and the proxy would still be willing to dial an address nothing
// answers on.
func (s *Server) handleWipe(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req wipeRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "bad request: "+err.Error(), http.StatusBadRequest)
		return
	}
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/x-ndjson")
	w.Header().Set("Cache-Control", "no-store")
	send := func(kind, msg string) {
		_ = json.NewEncoder(w).Encode(map[string]string{kind: msg})
		flusher.Flush()
	}

	if strings.TrimSpace(req.Port) == "" {
		send("error", "no serial port selected")
		return
	}
	if strings.TrimSpace(req.Confirm) != wipeConfirmWord {
		send("error", "wipe refused: the request must confirm with the word "+wipeConfirmWord+
			" — a chip erase takes the firmware, the Wi-Fi credentials and the name off the same orb")
		return
	}

	// Same per-port claim as a flash, under its own kind, so a second operation
	// cannot share the wire mid-erase and the UI's Cancel can reach this one.
	job, ctx, oerr := s.ops.start(r.Context(), req.Port, opWipe, 0)
	if oerr != nil {
		send("error", oerr.Error())
		return
	}
	defer s.ops.finish(job)

	portLock, perr := orbbind.LockPort(req.Port)
	if perr != nil {
		send("error", perr.Error())
		return
	}
	defer portLock.Release()

	pt, oerr := esprom.OpenPort(req.Port)
	if oerr != nil {
		send("error", oerr.Error())
		return
	}
	defer pt.Close()

	// Erase exactly the span the firmware this hub carries declares, so the
	// erase covers the whole chip on a board whose geometry the bundle states.
	// With no bundle loaded the erase falls back to the orb's 16 MB default.
	var opts esprom.WipeOptions
	opts.Ctx = ctx
	opts.OnEvent = func(m string) { send("log", m) }
	if b := s.firmwareBundle(); b != nil {
		opts.FlashSize = b.Plan().FlashSize
	}
	if req.Verify != nil && !*req.Verify {
		opts.SkipVerify = true
	}
	var wipedMAC string
	opts.OnMAC = func(m [6]byte) {
		wipedMAC = fmt.Sprintf("%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5])
		// Named before the erase, not after: the MAC survives it, and a caller
		// that lost the stream still wants to know which board went blank.
		send("mac", wipedMAC)
	}

	send("log", fmt.Sprintf("wiping %s — the orb will not boot until it is flashed again", req.Port))
	if err := esprom.Wipe(pt, opts); err != nil {
		if esprom.IsCancelled(err) {
			send("error", "wipe cancelled - the chip is still in its bootloader with part of the flash erased. "+
				"It will not boot. Flash it again, or wipe it again, and either one finishes cleanly.")
			return
		}
		send("error", err.Error())
		return
	}

	s.forgetWipedOrb(req.Port, wipedMAC)
	send("done", "chip erased — this orb is blank, with no firmware, no credentials and no name. Flash it to put one back.")
}

// forgetWipedOrb drops everything the hub believed about a board that has just
// been erased. The identity cache is per-port and now describes bytes that are
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
		log.Printf("orb-manager: wipe of %s left its fleet row in place: %v", mac, err)
		return
	}
	for _, ip := range held {
		if len(s.fleet.ClaimedBy(ip)) == 0 {
			s.grants.revoke(ip)
		}
	}
	s.saveState()
}
