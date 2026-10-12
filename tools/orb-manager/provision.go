package main

import (
	"encoding/json"
	"net/http"
	"strings"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
)

// provisionBudget bounds one wired-provisioning round-trip. The three console
// windows inside ProvisionOn (scan 22 s, ack 20 s, lease 25 s) are the work; this is
// the outer ceiling that stops a wedged console from holding a cable forever.
const provisionBudget = 90 * time.Second

// provisionRequest is the wired-provisioning job: the orb's serial port
// plus the network to join. It carries a plaintext passphrase (hex-encoded on
// the wire to the firmware, but plaintext over this local hub request and the
// user's own USB lead) — see the README caveat. It is only for an UNPROVISIONED
// orb: a device that already has credentials boots as a station and never opens
// the serial door; re-configure such an orb through its HTTP page.
type provisionRequest struct {
	Port string `json:"port"`
	SSID string `json:"ssid"`
	Pass string `json:"pass"`
	TZ   string `json:"tz"`
}

// handleProvision drives one wired-provisioning round-trip: it reboots the orb
// on the selected port into its app while holding the console, reads the boot
// scan (PROVSCAN) back as the network list, writes a single PROV line with the
// chosen credentials, and confirms the firmware's PROV-ACK. It provisions
// whatever orb the user picked — no identity gate — and holds only a per-port
// lock so it can't collide with a concurrent flash on the same cable. It
// streams console lines as NDJSON so the UI can show the same live log.
// Credentials never go through the ROM or the flasher; the firmware writes its
// own NVS keys, so the shared storage partition's other namespaces (theme,
// idle) are never touched.
func (s *Server) handleProvision(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req provisionRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "bad request: "+err.Error(), http.StatusBadRequest)
		return
	}
	req.Port = strings.TrimSpace(req.Port)
	req.SSID = strings.TrimSpace(req.SSID)
	if req.Port == "" || req.SSID == "" {
		http.Error(w, "port and ssid are required", http.StatusBadRequest)
		return
	}

	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/x-ndjson")
	w.Header().Set("Cache-Control", "no-store")
	send := func(ev map[string]any) {
		_ = json.NewEncoder(w).Encode(ev)
		flusher.Flush()
	}

	// Same per-cable claim a flash takes, so a provision and a flash can never
	// both hold the port, and so this operation is cancellable and visible too.
	// A different cable stays free: one operator, two orbs.
	job, ctx, oerr := s.ops.start(r.Context(), req.Port, opProvision, provisionBudget)
	if oerr != nil {
		send(map[string]any{"error": oerr.Error()})
		return
	}
	defer s.ops.finish(job)

	// Provision whatever orb is on the port the user picked. A single-orb owner
	// must never be told their own device isn't the "bound" one. The only
	// automatic guard is the per-port lock, so a flash and a provision can't
	// drive the same cable at once and corrupt each other.
	if portLock, perr := orbbind.LockPort(req.Port); perr != nil {
		send(map[string]any{"error": perr.Error()})
		return
	} else {
		defer portLock.Release()
	}

	pt, oerr := esprom.OpenPort(req.Port)
	if oerr != nil {
		send(map[string]any{"error": oerr.Error()})
		return
	}
	defer pt.Close()

	send(map[string]any{"log": "opening the console (this reboots the orb into its app)..."})
	res, err := esprom.ProvisionOn(ctx, pt, req.SSID, req.Pass, req.TZ, 22*time.Second, 20*time.Second, 25*time.Second, func(l string) {
		send(map[string]any{"log": "console: " + l})
	})
	if err != nil {
		if esprom.IsCancelled(err) {
			send(map[string]any{"error": "provisioning cancelled. If the PROV line already reached the orb, its credentials may be saved - check the orb's own screen before re-provisioning."})
			return
		}
		send(map[string]any{"error": err.Error()})
		return
	}
	if res.IP != "" {
		// The lease the orb announced on its own console is a USB-learned address,
		// so the proxy may dial it without anyone typing it. An address the hub
		// never learned over the cable, and was never handed, stays undialable.
		s.grants.learn(res.IP)
		s.setOrbIP(res.IP)
		s.saveState()
	}
	send(map[string]any{"result": map[string]any{
		"ap_seen":  res.APSeen,
		"sent":     res.Sent,
		"ack":      res.Ack,
		"ip":       res.IP,
		"networks": res.Networks,
	}})
}
