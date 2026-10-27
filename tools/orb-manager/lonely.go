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
		send("error", "erase refused: the request must confirm with the word "+lonelyConfirmWord+
			" — this erases the flash and writes over the orb's firmware, and it takes the fleet row and the name with it")
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
		send("error", "no firmware bundle loaded — the full erase borrows its bootloader and partition table from one")
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
		OnProgress: flashProgressSender(send, plan),
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

	send("log", fmt.Sprintf("erasing the flash of %s completely — it will stop being an orb, keep no settings, and boot a base image that holds its port", req.Port))
	lerr := esprom.Session(pt, opts)
	// The ending runs on the chip read, not on the session's return. The eFuse MAC is
	// read before anything is erased, so a hub that reaches this line knows exactly
	// which board it destroyed even when the write then died, and the stream already
	// tells the operator that board is in its bootloader with a partial image. Filing
	// nothing in that case cost the bench a stuck card (2026-10-26): the stub booted,
	// the fleet row kept its name, address and remembered mark, the grant stayed
	// issued to an address that no longer runs an orb, and the lonely mark — the only
	// thing that keeps a blanked chip's own descriptor from re-filing the row — was
	// never written. An operator who typed the confirm word owns the forgetting
	// whatever the ROM did next.
	if lonelyMAC != "" {
		s.endAsLonely(req.Port, lonelyMAC)
	}
	if lerr != nil {
		if esprom.IsCancelled(lerr) {
			send("error", "erase cancelled - the board is still in its bootloader with a partial image, and this hub has forgotten it as an orb. Flash it, base image or orb, and one of them finishes cleanly.")
			return
		}
		send("error", lerr.Error()+" - the board was already written, and this hub has forgotten it as an orb")
		return
	}

	send("done", "the flash is erased: its settings, name and credentials went with it, and it boots a base image that holds its port until you flash it into something else")
}

// endAsLonely is the pair the lonely door performs on the chip it just blanked: mark
// the chip, then un-make the orb. The order is the point. forgetWipedOrb ends in
// saveState, and
// saveState is what writes the lonely set into the file; marking afterwards recorded
// the forgetting without the memory of why. Proven on the bench 2026-10-15: a real
// lonely write left a state file with the row gone and no lonely field at all, so the
// next launch had nothing to stop the cable sweep from Confirm-ing the descriptor back
// into a fleet row — the board the operator had just ended, listed as an orb again. One
// save must carry both facts. The mark is also what keeps the row gone while the board
// still enumerates the same descriptor MAC: the next orb flash, announcement or serial
// ack clears it.
func (s *Server) endAsLonely(port, mac string) {
	s.lonely.mark(mac)
	s.forgetWipedOrb(port, mac)
}

// flashProgressSender is the progress-event encoder both writers share: the UI parses
// one progress shape for every streaming door.
//
// The shape carries the ROM's per-image counts AND a byte-weighted percentage of the
// whole plan, because the bar tells one story ("58% of this flash") instead of five
// misleading ones: a 4 MB app at 90% is most of the wait and a 12 kB otadata at 100% is
// none of it, which "3/5" cannot say. The weights come from the plan's own sizes, so the
// figure needs no new fact from the chip, and the offset stays in the event for anyone
// who wants the memory depth behind a percentage. Erasing and verifying are not bytes and
// are not in the figure; the bar holds still through them, which is the truth.
func flashProgressSender(send func(kind, msg string), p esprom.FlashPlan) func(img, n int, off uint32, wr, tot int) {
	total := 0
	before := make([]int, len(p.Images))
	for i, img := range p.Images {
		before[i] = total
		n := len(img.Data)
		total += (n + 3) / 4 * 4 // the writer pads to 4, and so do the weights
	}
	return func(img, n int, off uint32, wr, tot int) {
		done := wr
		if img >= 0 && img < len(before) {
			done = before[img] + wr
		}
		pct := 0
		if total > 0 {
			pct = done * 100 / total
			if pct > 100 {
				pct = 100
			}
		}
		send("progress", fmt.Sprintf(`{"image":%d,"of":%d,"offset":"0x%X","written":%d,"total":%d,"pct":%d,"bytes":%d}`,
			img, n, off, wr, tot, pct, total))
	}
}
