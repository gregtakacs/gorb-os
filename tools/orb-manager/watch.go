package main

import (
	"encoding/json"
	"net/http"
	"regexp"
	"strings"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
)

// watchRequest drives the post-flash onboarding probe. Port is the serial
// device the flash just ran on (console phase); MAC is that chip's eFuse base
// MAC, which equals its station MAC, so it is the key back to the boot verdict
// the flash captured over USB.
type watchRequest struct {
	Port   string `json:"port"`
	MAC    string `json:"mac"`
	SkipAp bool   `json:"skip_ap"` // after a serial provision, go straight to LAN Phase B
	IP     string `json:"ip"`      // lease the serial door read off the USB console: confirm it directly
}

const (
	watchLanBudget    = 45 * time.Second
	watchPollInterval = 2 * time.Second
)

// handleWatch resolves the post-flash question the UI cannot ask the orb
// directly: did the just-flashed orb join its stored network, or did it come up
// unprovisioned with the provisioning AP open? The single evidence source is
// the USB console the flasher owned across its own reset — the one-shot boot
// verdict it recorded, or the lease the serial door read after PROV-ACK. The
// hub never hunts the LAN. Credentials themselves always travel over HTTP (the
// orb's /api/v1/wifi/scan + /wifi/join through the proxy); serial is a log and
// identity pipe, not a config channel: during a flash the port is the ROM
// bootloader's, and after reset it carries IDF console output only.
func (s *Server) handleWatch(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req watchRequest
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
	send := func(ev map[string]any) {
		_ = json.NewEncoder(w).Encode(ev)
		flusher.Flush()
	}

	// Phase A: consume the boot verdict the flash captured while it held the
	// port across its own HardReset. /api/watch deliberately does NOT reset or
	// read the serial itself — USB-Serial-JTAG has no EN wire, so a chip whose
	// app is already running ignores an RTS pulse (the flash's own reset is the
	// only one that lands), and reading a booted orb yields live telemetry, not
	// the one-shot provisioning/association banner. If the flash left no verdict
	// (watch called without a preceding flash), fall through to the USB-learned
	// LAN address in Phase B.
	if !req.SkipAp {
		if v := s.bootFor(macNorm(req.MAC)); v != nil && v.apSSID != "" {
			// Fresh/erased NVS: the firmware opens the provisioning AP itself. On the
			// wired bench the hub cannot reach that AP's 192.168.4.1, so the network
			// list comes from the PROVSCAN lines captured off the boot console during
			// the flash, and the join is driven over the serial console by
			// /api/provision. The phone/HTTP door (join from the AP page) is unchanged.
			nets := v.networks
			if nets == nil {
				nets = []esprom.ScanNet{}
			}
			send(map[string]any{"log": "console: provisioning AP " + v.apSSID + " observed at boot"})
			send(map[string]any{"result": map[string]any{
				"mode":     "ap",
				"ap_ssid":  v.apSSID,
				"networks": nets,
			}})
			return
		}
	}

	// Phase B: the orb joined its stored network. Its address is known only from
	// what the hub already read over USB — the lease the serial door captured
	// (req.ip), or the "associated: config page at http://<ip>/" banner the flash
	// logged during its own boot read (the boot verdict's ip). The hub does NOT
	// hunt for it: no neighbour-table lookup, no /24 sweep, no mDNS. With no
	// USB-learned address (e.g. a full flash whose boot banner was missed inside
	// the window), it reports "no_ip" and the operator enters the address to
	// manage the orb — rather than the tool guessing.
	ip := strings.TrimSpace(req.IP)
	source := "read off the USB console"
	if ip == "" {
		if v := s.bootFor(macNorm(req.MAC)); v != nil && strings.TrimSpace(v.ip) != "" {
			ip = strings.TrimSpace(v.ip)
			source = "read off the boot console during the flash"
		}
	}
	if ip == "" {
		send(map[string]any{"result": map[string]any{
			"mode":   "no_ip",
			"reason": "the orb joined its network but announced no lease on the console - enter its IP to manage it",
		}})
		return
	}

	// Confirm that one address really is the orb before declaring success: a
	// just-joined orb can take a beat to bring its httpd up, so poll the single
	// known IP inside the budget. This is a targeted dial to a caller-supplied
	// address, not a discovery sweep.
	deadline := time.Now().Add(watchLanBudget)
	for {
		if st, err := s.orb.Status(ip); err == nil {
			send(map[string]any{"log": "orb joined its network as a station at " + ip + " (" + source + ")"})
			send(map[string]any{"result": map[string]any{"mode": "lan", "ip": ip, "status": st}})
			return
		}
		if !time.Now().Before(deadline) {
			break
		}
		// Abandon the poll the moment the panel goes away. This loop holds no port
		// lock — it dials one known IP over the LAN — so it is not dangerous, just
		// wasteful: a closed panel used to keep it dialing for the rest of the
		// budget with nobody left to read the answer.
		select {
		case <-r.Context().Done():
			return
		case <-time.After(watchPollInterval):
		}
	}
	send(map[string]any{"result": map[string]any{
		"mode":   "unknown",
		"reason": ip + " (" + source + ") did not answer as an orb - re-check the IP, or the network it joined",
		"ip":     ip,
	}})
}

// --- pure console classifiers (unit-tested) ---------------------------------

// provisioningSSID extracts the provisioning AP name the firmware logs when
// NVS holds no network ("no stored network — provisioning AP \"orb-8088\"
// (open)"). The SSID is orb-%02X%02X of the two low MAC bytes (orb_net.c).
var apSSIDRe = regexp.MustCompile(`orb-[0-9A-Fa-f]{4}`)

func provisioningSSID(line string) string {
	if !strings.Contains(line, "provisioning AP") && !strings.Contains(line, "SoftAP up") {
		return ""
	}
	m := apSSIDRe.FindString(line)
	if m == "" {
		return ""
	}
	return strings.ToLower(m)
}

// stationJoinedLine recognizes the firmware's association banner
// ("associated: config page at http://..."). "disassociated:" must not match.
func stationJoinedLine(line string) bool {
	return strings.Contains(line, "associated:") && !strings.Contains(line, "disassociated:")
}

func macNorm(s string) string {
	s = strings.ToLower(strings.TrimSpace(s))
	return strings.ReplaceAll(s, "-", ":")
}
