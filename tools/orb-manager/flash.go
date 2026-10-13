package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
)

// flashRequest is the UI's flash job. Mode selects the image set; Port is the
// serial device to write to. The tool flashes whatever port the caller names —
// a user owns the orb on their own cable. The only automatic guard is a
// per-port lock, so a concurrent operation can't share the wire mid-flash.
type flashRequest struct {
	Port string `json:"port"`
	// Mode is accepted but fixed: an end user always writes the whole manifest.
	// The app-only reflash is bench behaviour and lives on in orbflash -mode app.
	Mode string `json:"mode"`
	Wipe bool   `json:"wipe"` // also ROM-erase the NVS partitions (factory reset)
}

// handleFlash runs one flash, streaming newline-delimited JSON events so the UI
// can render progress. Before touching the wire it checks the plan resolves and
// the port is free — no identity is required: a user flashes the orb in front of
// them. The bundle's own sha256 and descriptor are what /api/build already showed
// the operator, and they are re-read from the same bytes this write sends.
func (s *Server) handleFlash(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req flashRequest
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

	if req.Port == "" {
		send("error", "no serial port selected")
		return
	}

	// Claim the cable. This is the in-process belt to orbbind's cross-process
	// flock: it gives the operation a cancel handle and a state the UI can show,
	// and it lets a second cable be flashed while this one is busy. The request
	// context is the parent, so closing the panel is itself a cancellation.
	job, ctx, oerr := s.ops.start(r.Context(), req.Port, opFlash, 0)
	if oerr != nil {
		send("error", oerr.Error())
		return
	}
	defer s.ops.finish(job)

	b, plan, err := s.planFor(req.Mode)
	if err != nil {
		send("error", err.Error())
		return
	}

	// One exclusive lock per physical port, keyed to the port itself — not to
	// who owns it. A concurrent flash/provision on the same cable would corrupt
	// the write, so it is refused; everything else (identity, ownership) is the
	// user's business, not the tool's. LockPort canonicalizes the by-id vs tty
	// spelling internally so both paths lock onto the same device.
	var opts esprom.Options
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

	opts.Plan = plan
	opts.Ctx = ctx
	// The factory reset resolves its targets from the partition table inside the
	// very bundle being flashed, never from constants and never from a build dir.
	// A stale table could otherwise point the erase at a region the new layout
	// uses for something else. An ordinary flash must still keep the orb's
	// credentials and tz alive, so it erases nothing.
	if req.Wipe {
		regions, werr := esprom.NvsRegions(b.PartitionTable())
		if werr != nil {
			send("error", werr.Error())
			return
		}
		opts.Wipe = regions
	}
	opts.OnEvent = func(m string) { send("log", m) }
	// The chip's eFuse MAC is its station MAC, so it is the join key between
	// the flashed device and the LAN entry /api/watch looks for. Emit it as
	// soon as the ROM hands it over — a later failed write must not lose it.
	var flashedMAC string
	opts.OnMAC = func(m [6]byte) {
		flashedMAC = fmt.Sprintf("%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5])
		send("mac", flashedMAC)
	}
	opts.OnProgress = func(img, n int, off uint32, wr, tot int) {
		send("progress", fmt.Sprintf(`{"image":%d,"of":%d,"offset":"0x%X","written":%d,"total":%d}`, img, n, off, wr, tot))
	}
	// Read the boot console on the port still held across the reset (Session
	// calls this right after its HardReset). It yields the one-shot verdict the
	// AP/provisioning UI needs — a later /api/watch cannot re-reset a booted orb
	// — and is also the console pane the onboarding flow shows.
	var bootAP string
	var bootAssocSeen bool
	var bootIP string
	var bootNetworks []esprom.ScanNet
	var provscanEnd bool
	opts.ObserveBoot = func(line string) bool {
		send("log", "console: "+line)
		if bootAP == "" {
			bootAP = provisioningSSID(line)
		}
		if !bootAssocSeen && stationJoinedLine(line) {
			bootAssocSeen = true
			if ip, ok := esprom.ParseOrbIP(line); ok {
				bootIP = ip // the orb's lease, read off its own console over USB
			}
		}
		if sn, ok := esprom.ParsePROVSCAN(line); ok {
			bootNetworks = append(bootNetworks, sn)
		}
		if strings.Contains(line, "PROVSCAN-END") {
			provscanEnd = true
		}
		// Stop early on a definite outcome. A station that joined needs no
		// further reading; a provisioning orb we wait for its scan to close
		// (PROVSCAN-END) so the wired picker has the list — stopping at the AP
		// banner alone would cut the boot scan off before it printed anything.
		return bootAssocSeen || (bootAP != "" && provscanEnd)
	}

	send("log", fmt.Sprintf("flashing %d image(s) to %s", len(plan.Images), req.Port))
	if err := esprom.Session(pt, opts); err != nil {
		if esprom.IsCancelled(err) {
			// Say what happened, and say what state the orb is in: the chip is
			// still in the ROM, so a reflash is always safe and nothing half-
			// written will boot.
			send("error", "flash cancelled - the orb is still in its bootloader, not running a partial image. Flash it again to finish.")
			return
		}
		send("error", err.Error())
		return
	}
	if flashedMAC != "" {
		// A flash changes what the orb runs, so the identity cached for this
		// port is now stale: a re-probe must re-read it rather than repeat the
		// version from before the flash.
		s.identities.forget(req.Port)
		s.setBoot(macNorm(flashedMAC), bootAP, bootAssocSeen, bootIP, bootNetworks)
		// The eFuse read that named this flash is a cable sighting too: the board
		// that was just written is the board the hub has proof of, whatever a later
		// announcement chooses to say about where it is.
		s.markCable(flashedMAC)
	}
	send("done", "flash complete - orb rebooting into the new firmware")
}

// planFor resolves the flash manifest for a mode out of the selected firmware
// bundle — the bytes the binary shipped with, or the one the user picked. It
// returns the bundle beside the plan so a factory reset erases regions of the
// same artifact being written, even if a new firmware is picked mid-flash.
//
// It never consults an ESP-IDF build dir: offsets and geometry come from the
// manifest that travelled inside the same bundle, so they still cannot drift
// from the images they describe (the serflash.sh lesson), and an end user needs
// nothing installed but this app.
func (s *Server) planFor(mode string) (*firmware.Bundle, esprom.FlashPlan, error) {
	b := s.firmwareBundle()
	if b == nil {
		return nil, esprom.FlashPlan{}, fmt.Errorf("no firmware loaded - restart the hub, or pick a firmware file on the Onboard card")
	}
	// Always the full manifest. A partial write is the thing that leaves an orb
	// bricked-looking, and an end user has no way to know which half they asked
	// for; the bench keeps that rope on purpose, in orbflash -mode app.
	if mode != "" && mode != "full" {
		log.Printf("orb-manager: ignoring flash mode %q - the hub always writes the full manifest", mode)
	}
	return b, b.Plan(), nil
}
