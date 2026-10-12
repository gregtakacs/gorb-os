package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"regexp"
	"runtime"
	"strings"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
)

// watchRequest drives the post-flash onboarding probe. Port is the serial
// device the flash just ran on (console phase); MAC is that chip's eFuse base
// MAC, which equals its station MAC, so it is how the orb is identified on the
// LAN afterwards.
type watchRequest struct {
	Port   string `json:"port"`
	MAC    string `json:"mac"`
	SkipAp bool   `json:"skip_ap"` // after a serial provision, go straight to LAN Phase B
	IP     string `json:"ip"`      // lease the serial door read off the console: probe it first, before the ARP sweep
}

const (
	watchLanBudget    = 45 * time.Second
	watchPollInterval = 2 * time.Second
)

// handleWatch resolves the post-flash question the UI cannot ask the orb
// directly: did the just-flashed orb join its stored network, or did it come up
// unprovisioned with the provisioning AP open? Two evidence sources, both
// already in hand: the USB console the flasher owns, and the neighbour table
// keyed by the ROM-read MAC. Credentials themselves always travel over HTTP
// (the orb's /api/v1/wifi/scan + /wifi/join through the proxy) — serial is a
// log and identity pipe, not a config channel: during a flash the port is the
// ROM bootloader's, and after reset it carries IDF console output only.
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
	// (watch called without a preceding flash), skip straight to LAN discovery.
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

	// Phase B: find it on the LAN by MAC.
	// If the serial door already read the orb's DHCP lease off the console, that
	// address is authoritative — we just booted this very board — so probe it
	// first and skip the ARP sweep entirely. Still confirm it answers as an orb
	// on /api/v1/status before trusting it; a miss (lease not live yet, or DHCP
	// had not landed inside the budget) falls through to the MAC-based sweep.
	if ip := strings.TrimSpace(req.IP); ip != "" {
		if st, err := s.orb.Status(ip); err == nil {
			send(map[string]any{"result": map[string]any{"mode": "lan", "ip": ip, "status": st}})
			return
		}
	}
	req.MAC = macNorm(req.MAC)
	if req.MAC == "" {
		send(map[string]any{"result": map[string]any{"mode": "unknown"}})
		return
	}
	deadline := time.Now().Add(watchLanBudget)
	ticks := 0
	// lanAnswer confirms an ARP hit really is the flashed orb before trusting
	// it: arpByMAC can return a stale row, and /api/v1/status carries no MAC, so
	// a different orb that has since taken that IP would otherwise be reported
	// as this one having joined. Only a positive MAC mismatch rejects; an
	// un-probeable IP (no arping) falls through to the status check as before.
	lanAnswer := func() (map[string]any, bool) {
		ip, found := arpByMAC(req.MAC)
		if !found {
			return nil, false
		}
		if got, checked := macAtIP(ip); checked && got != "" && got != req.MAC {
			send(map[string]any{"log": fmt.Sprintf("arp hit %s answered by %s, not %s — ignoring stale row", ip, got, req.MAC)})
			return nil, false
		}
		st, err := s.orb.Status(ip)
		if err != nil {
			return nil, false
		}
		return map[string]any{"mode": "lan", "ip": ip, "status": st}, true
	}
	for time.Now().Before(deadline) {
		if res, ok := lanAnswer(); ok {
			send(map[string]any{"result": res})
			return
		}
		// A sweep is what puts the entry in the table in the first place: the
		// TCP dials to :80 elicit ARP replies from every live host. Run it on
		// the first tick and every third one after.
		if ticks%3 == 0 {
			if cidr := guessHouseholdCIDR(); cidr != "" {
				if orbs, err := s.orb.Sweep(cidr); err == nil && len(orbs) > 0 {
					send(map[string]any{"log": fmt.Sprintf("sweep: %d orb(s) answering in %s", len(orbs), cidr)})
					if res, ok := lanAnswer(); ok {
						send(map[string]any{"result": res})
						return
					}
				}
			}
		}
		ticks++
		time.Sleep(watchPollInterval)
	}
	send(map[string]any{"result": map[string]any{"mode": "unknown", "reason": "orb did not appear on the LAN"}})
}

// --- pure classifiers and neighbour-table parsers (unit-tested) -------------

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

// arpByMAC returns the LAN IPv4 the neighbour table currently maps to mac.
// On Linux the kernel table is read directly; elsewhere (and on a read
// failure) it shells out to `arp -a`, whose text both macOS and Windows emit.
func arpByMAC(mac string) (string, bool) {
	if runtime.GOOS == "linux" {
		if raw, err := os.ReadFile("/proc/net/arp"); err == nil {
			if ip := parseProcArp(string(raw), mac); ip != "" {
				return ip, true
			}
		}
	}
	out, err := exec.Command("arp", "-a").Output()
	if err != nil && len(out) == 0 {
		return "", false
	}
	if ip := parseArpA(string(out), mac); ip != "" {
		return ip, true
	}
	return "", false
}

// parseProcArp reads /proc/net/arp rows:
// "IP address HW type Flags HW address Mask Device".
func parseProcArp(text, wantMAC string) string {
	for _, line := range strings.Split(text, "\n")[1:] { // skip header
		f := strings.Fields(line)
		if len(f) < 4 {
			continue
		}
		if macNorm(f[3]) == wantMAC {
			return f[0]
		}
	}
	return ""
}

var (
	arpParenIP = regexp.MustCompile(`\((\d{1,3}(?:\.\d{1,3}){3})\)`)
	arpBareIP  = regexp.MustCompile(`^(\d{1,3}(?:\.\d{1,3}){3})`)
	arpHW      = regexp.MustCompile(`(?:[0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}`)
)

// parseArpA matches a MAC in `arp -a` output, which differs per OS:
// macOS/Linux: "? (192.168.2.32) at 44:bd:8d:60:80:88 [ether] on wlan0"
// Windows:     "192.168.2.32 ............ 44-bd-8d-60-80-88 dhcp static"
func parseArpA(text, wantMAC string) string {
	for _, line := range strings.Split(text, "\n") {
		hw := arpHW.FindString(line)
		if hw == "" || macNorm(hw) != wantMAC {
			continue
		}
		if m := arpParenIP.FindStringSubmatch(line); m != nil {
			return m[1]
		}
		if m := arpBareIP.FindStringSubmatch(strings.TrimSpace(line)); m != nil {
			return m[1]
		}
	}
	return ""
}

// macAtIP probes ip for the hardware address that answers ARP for it right now,
// so a stale neighbour row can be caught before its status is trusted. It
// returns the MAC and checked=true only when the probe actually yielded one; a
// missing arping binary or a silent host returns checked=false, which is the
// absence of evidence, not evidence of a mismatch.
func macAtIP(ip string) (string, bool) {
	out, _ := exec.Command("arping", "-c", "1", "-w", "2", ip).CombinedOutput()
	mac := parseArpingReply(string(out))
	return mac, mac != ""
}

// parseArpingReply extracts the hardware address from an arping reply line.
// iputils:  "Unicast reply from 192.168.2.32 [44:bd:8d:60:80:88]  1.2ms"
// busybox:  "ARP reply from 192.168.2.32 (44:bd:8d:60:80:88)"
func parseArpingReply(text string) string {
	for _, line := range strings.Split(text, "\n") {
		if !strings.Contains(strings.ToLower(line), "reply") {
			continue
		}
		if hw := arpHW.FindString(line); hw != "" {
			return macNorm(hw)
		}
	}
	return ""
}
