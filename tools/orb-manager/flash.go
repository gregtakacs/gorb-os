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
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
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
	// Name is the word the operator chose for this board. The hub writes it onto
	// the chip over the same cable that carried the firmware, before the flash
	// reports done, so the orb is never loose with no name — not even while it
	// is still on the provisioning AP. Empty means leave the name alone, which
	// only a reflash of a board that already has one may do.
	Name string `json:"name"`
	// Fresh marks the New Orb flow's request: a blank board's first flash. The
	// name is mandatory on exactly this request, and this is where that rule is
	// enforced — a client-side gate alone is a gate a second tab walks around.
	Fresh bool `json:"fresh"`
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

	// A blank board's first flash must name it. Enforced here, before any lock
	// is claimed or wire is touched, so the refusal costs nothing and cannot be
	// bypassed by any client — the button on the page is courtesy, this is the
	// rule. A name sent on any flash is checked against the DEVICE's rules
	// first, so an orb never accepts a word the page then cannot display.
	req.Name = strings.TrimSpace(req.Name)
	if req.Fresh && req.Name == "" {
		send("error", "a new orb must be named before it is flashed")
		return
	}
	if req.Name != "" {
		clean, nerr := orbfleet.CleanName(req.Name)
		if nerr == nil {
			nerr = orbfleet.CheckNameASCII(clean)
		}
		if nerr != nil {
			send("error", "the name was refused: "+nerr.Error())
			return
		}
		req.Name = clean
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
		// And an orb flash is the evidence that ends a lonely mark: the chip
		// this hub left minding a display now runs an orb again.
		s.lonely.clear(macNorm(flashedMAC))
	}
	// Name the board over the same lead, now that the app it just wrote is
	// booting. The serial command door (orb_net.c) is armed for the whole life
	// of the app, so the line lands whether the board came up provisioning or
	// station. This happens BEFORE the done event so a client that closes the
	// dialog on done can never race the write out of existence, and a refused
	// name is said in the stream rather than discovered by the fleet list.
	if req.Name != "" {
		nctx, ncancel := context.WithTimeout(context.Background(), 20*time.Second)
		defer ncancel()
		nline := esprom.SerialLine("NAME", esprom.ProvField(req.Name))
		nres, nerr := esprom.RunCommand(nctx, pt, nline, []string{"NAME-ACK"}, func(l string) {
			send("log", "console: "+l)
		})
		if nerr == nil && nres != nil && nres.Ack && nres.Nak == "" {
			send("log", "named \""+req.Name+"\" — the orb acknowledged it over the cable")
			// The chip holds the word; the card must not wait on a LAN poll to say
			// so. A flash-time name that lands only in NVS leaves the store row
			// nameless, and a nameless cable-only row proves nothing to
			// storeProvesOrb — which is how a board the operator has already set up
			// stays offered on the New Orb picker as a blank to adopt. This is the
			// same fold the /api/orb/cmd NAME ack performs; the flash dialog speaks
			// to the chip directly, so it owes the store the write itself.
			if flashedMAC != "" && s.fleet != nil {
				if _, _, werr := s.fleet.NoteDeviceName(macNorm(flashedMAC), req.Name); werr != nil {
					send("log", "note: the orb holds \""+req.Name+"\" but the card kept its old word: "+werr.Error())
				} else {
					s.saveState()
				}
			}
		} else {
			why := "no NAME-ACK inside 20 s"
			if nerr != nil {
				why = nerr.Error()
			} else if nres != nil && nres.Nak != "" {
				why = "the orb refused it: " + nres.Nak
			}
			send("log", "the name \""+req.Name+"\" did not reach the chip ("+why+"); the orb will name it again as soon as it has an address")
		}
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
