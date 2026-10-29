package main

import (
	"encoding/json"
	"net/http"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
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

var (
	// The post-flash watch's patience, and its beat. Variables, not constants:
	// the tests that prove a timeout reports honestly cannot spend 45 live
	// seconds per run.
	//
	// The budget widened 45 s to 90 s after the bench's second failed join
	// (2026-10-29): the console HAD read the lease — the board proved it
	// associated — and the LAN confirm still died at 45 s against a −75 dB
	// link, because a board that drops and re-associates on the firmware's
	// escalating ladder spends a minute-plus off the wire, and the mDNS row
	// that could move the dial also cannot burst through a weak link. 90 s
	// covers one full ladder retry, the same reasoning that widened the
	// console's own lease window to 40 s.
	watchLanBudget    = 90 * time.Second
	watchPollInterval = 2 * time.Second
)

// handleWatch resolves the post-flash question the UI cannot ask the orb
// directly: did the just-flashed orb join its stored network, or did it come up
// unprovisioned with the provisioning AP open? The evidence arrives in order of
// trust: first the USB console the flasher owned across its own reset — the
// one-shot boot verdict it recorded, or the lease the serial door read after
// PROV-ACK — and then the orb's own mDNS announcement, admitted only when its
// TXT mac is the very chip MAC the cable read (see Phase B). The hub still
// never hunts the LAN: no sweep, no neighbour table, no row naming no chip or a
// different one. Credentials themselves always travel over HTTP (the orb's
// /api/v1/wifi/scan + /wifi/join through the proxy); serial is a log and
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
			send(map[string]any{"log": "it opened its setup network instead: " + v.apSSID})
			send(map[string]any{"result": map[string]any{
				"mode":     "ap",
				"ap_ssid":  v.apSSID,
				"networks": nets,
			}})
			return
		}
	}

	// Phase B: the orb joined its stored network. The hub's first memory of the
	// address is what the chip itself said down the lead — the lease the serial
	// door captured (req.ip), or the "associated: config page at http://<ip>/"
	// banner the flash logged during its own boot read. A console can miss that
	// banner: the join is retried on an escalating ladder (orb_net.c
	// retry_delay_s) while the hub's lease window is fixed, and the bench
	// router's WPA3/SAE limiter has been stretching joins by a minute or two. A
	// board that joined one breath after the window closed is joined, and the
	// tool must not dead-end the operator with a text box while the chip still
	// has a second voice. So the watch then listens for the orb's own mDNS
	// announcement — the firmware bursts it within seconds of the lease — and
	// admits it on one condition: the TXT mac is the twelve hex digits the eFuse
	// handed the cable a minute ago, and the browser already refused any row
	// whose instance name and MAC disagree (browse.go promote). An announcement
	// naming THAT chip is the board telling the hub where it lives. Naming no
	// chip, or a different one, proves nothing, and the old refusals all stand:
	// no sweep, no neighbour table, no self-declared stranger dialled by the
	// join window (owner ruling, 2026-10-29, on the flow this message used to
	// end: "why can't we just connect using what the orb told us?").
	ip := strings.TrimSpace(req.IP)
	source := "read off the USB console"
	if ip == "" {
		if v := s.bootFor(macNorm(req.MAC)); v != nil && strings.TrimSpace(v.ip) != "" {
			ip = strings.TrimSpace(v.ip)
			source = "read off the boot console during the flash"
		}
	}
	want, _ := orbmdns.CanonicalMAC(req.MAC)
	if ip == "" && want != "" {
		if a, ok := s.announcedFor(want); ok {
			if a.Provisioning {
				sendNoJoin(send)
				return
			}
			if a.IP != "" {
				ip = a.IP
				source = "announced by the orb, and answered as the chip the cable named"
				send(map[string]any{"log": "the orb announced itself at " + ip + " — dialing it"})
			}
		}
		if ip == "" {
			// The banner missed and the air has said nothing yet. The announcement
			// rides the join, so the remaining budget is the join's own clock: wait
			// for the chip to speak rather than guess at it.
			if a, ok := s.waitAnnounced(r, send, want); ok {
				if a.Provisioning {
					sendNoJoin(send)
					return
				}
				ip = a.IP
				source = "announced by the orb, and answered as the chip the cable named"
				send(map[string]any{"log": "the orb announced itself at " + ip + " — dialing it"})
			}
		}
	}
	if ip == "" {
		// Nothing on the wire, nothing on the air. The honest exit: with a cable
		// bound it re-arms the serial door, and the sentence says which failure
		// this is — a board that announces nothing never joined.
		reason := "the orb joined its network but announced no lease on the console - enter its IP to manage it"
		if want != "" {
			reason = "no lease on the console, and no announcement naming " + req.MAC +
				" within the budget — the join did not land or the board is not on this LAN"
		}
		send(map[string]any{"result": map[string]any{
			"mode":   "no_ip",
			"reason": reason,
		}})
		return
	}

	// Confirm that one address really is the orb before declaring success: a
	// just-joined orb can take a beat to bring its httpd up, so poll the single
	// known IP inside the budget. This is a targeted dial to a caller-supplied
	// address, not a discovery sweep, and dialing it here is what authorises the
	// proxy to reach it afterwards — the Manage card's controls are otherwise
	// refused until an address has been learned or entered.
	watchStart := time.Now()
	deadline := watchStart.Add(watchLanBudget)
	// Say so, then say it again on every tick. This loop can run the whole
	// budget, and it runs behind a dialog whose only other content is a spinner
	// and one fixed sentence about rebooting into new firmware. A wait that
	// produces no news for 45 s is indistinguishable from a hung tool, which is
	// what the operator reported (bench, 2026-10-15). Elapsed-and-still-looking
	// is the cheapest proof there is that the hub is alive and still trying.
	send(map[string]any{"log": "looking for the orb at " + ip})
	attempt := 0
	for {
		if st, err := s.orb.Status(ip); err == nil {
			if want != "" {
				if got, why := orbmdns.CanonicalMAC(st.MAC); why == "" && got != want {
					// The address answered — as a different chip than the one the
					// cable held a minute ago. What came off the eFuse and what
					// answers on the LAN cannot both be this board; adopting it
					// would put the wrong face on the wrong watch, so the watch
					// refuses and says which two names disagree.
					send(map[string]any{"result": map[string]any{
						"mode":   "unknown",
						"reason": ip + " (" + source + ") answers as " + st.MAC + ", not the " + req.MAC + " the cable named",
					}})
					return
				}
			}
			// Bind the grant to the MAC the device itself reported at that address.
			// Still a claim, and still HTTP: what it buys is not trust, it is the
			// cross-lock's reference point, so a later announcement naming a
			// different board at this address is a contradiction the hub can see.
			s.grants.learnAs(ip, st.MAC)
			s.setOrbIP(ip)
			s.saveState()
			send(map[string]any{"log": "the orb is back at " + ip})
			send(map[string]any{"result": map[string]any{"mode": "lan", "ip": ip, "status": st}})
			return
		}
		// The miss may mean the number aged out: a banner is a snapshot, and a
		// board that answers on a fresh lease under a new one is found at the new
		// number rather than died on at the old (the same rule waitOrbBack proved
		// for the install ladder). The chip's own announcement is the only thing
		// allowed to move the dial, and the check runs BEFORE the deadline
		// decision: a move learned on the last miss is still the truest address
		// the watch holds, and must be the address the exit names.
		if want != "" {
			if a, ok := s.announcedFor(want); ok && !a.Provisioning && a.IP != "" && a.IP != ip {
				send(map[string]any{"log": "its lease moved: " + ip + " to " + a.IP})
				ip = a.IP
				source = "announced by the orb, and answered as the chip the cable named"
			}
		}
		if !time.Now().Before(deadline) {
			break
		}
		attempt++
		// First tick immediately, then every other one: a line each two seconds
		// turns the console into noise, and the point is proof of life, not a
		// stopwatch.
		if attempt == 1 || attempt%2 == 0 {
			send(map[string]any{"log": "still waiting - " +
				strconv.Itoa(int(time.Since(watchStart).Seconds())) + "s of " +
				strconv.Itoa(int(watchLanBudget.Seconds())) + "s"})
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
	// The honest exit, worded by what actually failed. A lease the hub read off
	// the console's own banner means the board ASSOCIATED — the address is the
	// board's word, not a guess — so "re-check the IP" would blame the operator
	// for the one fact the cable proved. On that source the timeout says the
	// truth: the join landed, the answer never arrived inside the budget, and a
	// weak link with the firmware's retry ladder is the suspect. Only a learned
	// or typed address gets the re-check wording.
	breakReason := ip + " (" + source + ") did not answer as an orb - re-check the IP, or the network it joined"
	if source == "read off the USB console" {
		breakReason = "the board read its lease on the console, so it joined — but it never answered at " +
			ip + " within the budget: a weak link can hold the join on the firmware's retry ladder, and the cable is still a door"
	}
	send(map[string]any{"result": map[string]any{
		"mode":   "unknown",
		"reason": breakReason,
		"ip":     ip,
	}})
}

// --- the orb's second voice: its own announcement, keyed to the cable's fact ---

// announcement is the chip's live mDNS word, as the browse cache holds it: the
// address in the announcement whose TXT mac is this MAC, and whether the board
// says its setup mode is open. The cache only ever holds a row after
// browse.go's promote cross-checked the instance name against that MAC, so an
// answer here is the device's own claim from this minute — never a neighbour's,
// never a stale one.
type announcement struct {
	IP           string
	Provisioning bool
}

// announcedFor reads the browse cache for the one chip want names, live rows
// only: Candidates() ages out anything past the record's own TTL plus grace.
// The empty want answers false — an announcement naming no chip says nothing
// about the chip on the cable, and that is the refusal the cable-only rule got
// right.
func (s *Server) announcedFor(want string) (announcement, bool) {
	if want == "" {
		return announcement{}, false
	}
	s.dmu.Lock()
	b := s.browse
	s.dmu.Unlock()
	if b == nil {
		return announcement{}, false
	}
	for _, c := range b.Candidates() {
		if c.MAC == want {
			return announcement{IP: c.IP, Provisioning: c.Provisioning()}, true
		}
	}
	return announcement{}, false
}

// waitAnnounced waits out the join on the air instead of the console: the
// firmware bursts its announcement the moment DHCP hands the lease, so the
// first packet after a PROV-ACK is the join landing, not a discovery the hub
// provoked — it sends nothing, it only listens to what the board already
// shouts. It speaks every second tick, because a silent budget reads to an
// operator as a hung tool (bench, 2026-10-15). A provisioning-state row ends
// the wait as loudly as a lease does: the board is announcing setup mode again,
// which after an ack has only one meaning. False means the budget ran out with
// the chip silent on every channel the hub is allowed to hear.
func (s *Server) waitAnnounced(r *http.Request, send func(map[string]any), want string) (announcement, bool) {
	start := time.Now()
	deadline := start.Add(watchLanBudget)
	attempt := 0
	for {
		a, ok := s.announcedFor(want)
		if ok && (a.Provisioning || a.IP != "") {
			return a, true
		}
		if !time.Now().Before(deadline) {
			return announcement{}, false
		}
		attempt++
		if attempt == 1 || attempt%2 == 0 {
			send(map[string]any{"log": "waiting for the orb to announce itself - " +
				strconv.Itoa(int(time.Since(start).Seconds())) + "s of " +
				strconv.Itoa(int(watchLanBudget.Seconds())) + "s"})
		}
		select {
		case <-r.Context().Done():
			return announcement{}, false
		case <-time.After(watchPollInterval):
		}
	}
}

// sendNoJoin answers the one outcome a PROV-ACK cannot tell from success: the
// board took the credentials, retried the join on its ladder, and came back
// announcing setup mode. The passphrase is the suspect then — not the hub's
// patience, not the router — and the form under the dialog is the cure, so the
// dialog stays open rather than dead-ending at a text box.
func sendNoJoin(send func(map[string]any)) {
	send(map[string]any{"log": "it announced setup mode, not a lease — the join did not land"})
	send(map[string]any{"result": map[string]any{
		"mode":   "no_ip",
		"reason": "the board says it is still provisioning — the join did not land, and the passphrase is the suspect; the form re-writes it over the cable",
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
