package main

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
)

//go:embed lonely/lonely.bin
var lonelyImage []byte

// lonelyRequest is the UI's "Leave it lonely" — a whole-chip erase's kinder
// twin: the board stops being an orb, but it boots, shows its piece, and
// enumerates steadily instead of boot-looping. Confirm must read exactly
// LONELY: it destroys the running firmware and every credential alongside it,
// so it earns the same typed word a WIPE does.
type lonelyRequest struct {
	Port    string `json:"port"`
	Confirm string `json:"confirm"`
}

const lonelyConfirmWord = "LONELY"

// handleLonely writes the lonely image — the bundle's bootloader, partition
// table and initial otadata, with the lonely app in ota_0, and the NVS-region
// erase the same session applies afterwards — and streams the same NDJSON
// contract as /api/flash and /api/wipe. The erase is the point of the design:
// the lonely firmware that comes up owns no name, no time zone and no
// credential, and a later reflash finds a blank config, the way a factory
// board does.
//
// It replaces the post-wipe boot-loop instead of deepening it: a blank S3
// retries its nonexistent image every boot and its USB port flaps with each
// retry (measured, both wiped boards), which reads as a bad cable and needs a
// BOOT-button press to stop. A lonely board boots once, holds its enumeration,
// and animates the one message that explains itself. The hub forgets the board
// afterwards exactly as a wipe does — the fleet row, the name, the grant —
// and marks it lonely, because nothing answering to "orb" lives at that
// address any more and the cable's MAC-shaped descriptor must not rebuild a
// row the operator just ended (lonelyset.go).
//
// A lonely board reads as a blank one to every other door: it does not speak
// the serial command protocol, announces nothing, and New Orb flashes over it
// in the same geometry this wrote, byte for byte.
func (s *Server) handleLonely(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req lonelyRequest
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
	if strings.TrimSpace(req.Confirm) != lonelyConfirmWord {
		send("error", "lonely refused: the request must confirm with the word "+lonelyConfirmWord+
			" — this writes over the orb's firmware, and it takes the fleet row and the name with it")
		return
	}
	job, ctx, oerr := s.ops.start(r.Context(), req.Port, opLonely, 0)
	if oerr != nil {
		send("error", oerr.Error())
		return
	}
	defer s.ops.finish(job)

	// The claim happens before the bundle is consulted on purpose: a cable
	// mid-flash is the truer refusal than a missing bundle, and a hub with no
	// bundle still frees the port it took the moment this returns.
	bundle := s.firmwareBundle()
	if bundle == nil {
		send("error", "no firmware bundle loaded — the lonely image borrows its bootloader and partition table from one")
		return
	}
	plan, err := bundle.LonelyPlan(lonelyImage)
	if err != nil {
		send("error", err.Error())
		return
	}
	// The lonely write is the wipe that replaced the wipe: the board's settings
	// — time zone, name, every credential — are erased by the same session, so
	// nothing the owner configured survives the firmware that ends the owner.
	// The regions are read out of the very bundle whose table this plan writes,
	// so the erase can only ever target what is actually there.
	nvs, werr := esprom.NvsRegions(bundle.PartitionTable())
	if werr != nil {
		send("error", werr.Error())
		return
	}

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

	var lonelyMAC string
	opts := esprom.Options{
		Plan:       plan,
		Ctx:        ctx,
		OnEvent:    func(m string) { send("log", m) },
		OnProgress: flashProgressSender(send),
		// A lonely board keeps nothing of its owner: the same ROM session that
		// writes the image erases the NVS and storage regions the bundle's own
		// partition table declares, so the time zone, the name and every Wi-Fi
		// credential go with the orb life. The targets come from the table being
		// written, never from constants — the factory-reset flash rule.
		Wipe: nvs,
		OnMAC: func(m [6]byte) {
			lonelyMAC = fmt.Sprintf("%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5])
			send("mac", lonelyMAC)
		},
	}

	send("log", fmt.Sprintf("leaving %s lonely — it will stop being an orb, keep no settings, and start minding the display", req.Port))
	if err := esprom.Session(pt, opts); err != nil {
		if esprom.IsCancelled(err) {
			send("error", "lonely cancelled - the board is still in its bootloader with a partial image. Flash it, lonely or orb, and one of them finishes cleanly.")
			return
		}
		send("error", err.Error())
		return
	}

	s.forgetWipedOrb(req.Port, lonelyMAC)
	// The mark is what keeps the row gone: the cable sweep re-attributes the
	// same descriptor MAC a moment later, and a hub with no memory of leaving
	// this board lonely would rebuild the fleet row from that sighting. The
	// next orb flash, announcement or serial ack clears it.
	s.lonely.mark(lonelyMAC)
	send("done", "the orb is lonely now: its settings, name and credentials went with it, and it bounces its message until you flash it into something else")
}

// flashProgressSender is the progress-event encoder both writers share: the UI
// parses one progress shape for every streaming door.
func flashProgressSender(send func(kind, msg string)) func(img, n int, off uint32, wr, tot int) {
	return func(img, n int, off uint32, wr, tot int) {
		send("progress", fmt.Sprintf(`{"image":%d,"of":%d,"offset":"0x%X","written":%d,"total":%d}`, img, n, off, wr, tot))
	}
}
