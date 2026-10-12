package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
)

// flashRequest is the UI's flash job. Mode selects the image set; Port is the
// serial device to write to. The tool flashes whatever port the caller names —
// a user owns the orb on their own cable. The only automatic guard is a
// per-port lock, so a concurrent operation can't share the wire mid-flash.
type flashRequest struct {
	Port  string `json:"port"`
	Mode  string `json:"mode"`  // "full" (build manifest) | "app" (app-only)
	Force bool   `json:"force"` // skip the stamp audit (explicit user override)
	Wipe  bool   `json:"wipe"`  // also ROM-erase the NVS partitions (factory reset)
}

// handleFlash runs one flash, streaming newline-delimited JSON events so the UI
// can render progress. Before touching the wire it checks the plan resolves, the
// stamp audit passes (unless overridden), and the port is free — no identity is
// required: a user flashes the orb in front of them.
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

	s.mu.Lock()
	if s.flashing {
		s.mu.Unlock()
		send("error", "another flash is already running in this manager")
		return
	}
	s.flashing = true
	s.mu.Unlock()
	defer func() { s.mu.Lock(); s.flashing = false; s.mu.Unlock() }()

	if req.Port == "" {
		send("error", "no serial port selected")
		return
	}

	plan, err := s.planFor(req.Mode)
	if err != nil {
		send("error", err.Error())
		return
	}
	if req.Mode == "app" && !req.Force && s.buildDir != "" {
		app := s.buildDir + "/gorb-os.bin"
		if stamp, want, serr := orbbind.StampAudit(app, s.repoDir); serr != nil {
			send("error", fmt.Sprintf("stamp audit refused: %v (image [%s] vs tree [%s]). Rebuild, or check Force to override.", serr, stamp, want))
			return
		} else {
			send("log", fmt.Sprintf("stamp audit ok: image stamped [%s]", stamp))
		}
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
	// The factory reset resolves its targets from the built partition table
	// (the same authoritative bytes flashed at 0x8000), never from constants,
	// and only on the explicit request — an ordinary flash must keep the
	// orb's credentials and tz alive.
	if req.Wipe {
		regions, werr := esprom.NvsRegions(s.buildDir)
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
	var bootNetworks []esprom.ScanNet
	var provscanEnd bool
	opts.ObserveBoot = func(line string) bool {
		send("log", "console: "+line)
		if bootAP == "" {
			bootAP = provisioningSSID(line)
		}
		if !bootAssocSeen && stationJoinedLine(line) {
			bootAssocSeen = true
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
		send("error", err.Error())
		return
	}
	if flashedMAC != "" {
		s.setBoot(macNorm(flashedMAC), bootAP, bootAssocSeen, bootNetworks)
	}
	send("done", "flash complete — orb rebooting into the new firmware")
}

// planFor resolves the flash manifest for a mode, always from the build so
// offsets/mode are never hardcoded (the serflash.sh discipline).
func (s *Server) planFor(mode string) (esprom.FlashPlan, error) {
	if s.buildDir == "" {
		return esprom.FlashPlan{}, fmt.Errorf("no build dir — start the manager with --build")
	}
	switch mode {
	case "app":
		app := s.buildDir + "/gorb-os.bin"
		ota := s.buildDir + "/ota_data_initial.bin"
		return esprom.AppOnlyPlan(app, ota), nil
	case "full", "":
		return esprom.PlanFromBuildDir(s.buildDir)
	default:
		return esprom.FlashPlan{}, fmt.Errorf("unknown flash mode %q", mode)
	}
}
