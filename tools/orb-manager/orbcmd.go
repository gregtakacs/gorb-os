package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
)

// cmdBudget bounds one serial-command round-trip. RunCommand re-pushes for 15 s and a
// SCAN must wait out a full radio scan, so this ceiling sits above both while still
// stopping a wedged console from holding a cable forever.
const cmdBudget = 40 * time.Second

// orbCmdRequest names one serial command against one wired orb. It is the wired twin of
// the fleet card's name field and the config page's wifi/mode endpoints, for the case a
// LAN door cannot serve: the board is on a cable but has no address the hub can reach,
// so the only way to rename, rescan, flip mode, or factory-reset it is the USB lead.
// operand is plain text (the hub hex-encodes it), except RESET, whose guard "ALL" is
// sent literally.
type orbCmdRequest struct {
	Port    string `json:"port"`
	MAC     string `json:"mac"`     // for the fleet write-back on a name ack
	Command string `json:"command"` // name|scan|mode|reset|ping|prov
	SSID    string `json:"ssid,omitempty"`
	Pass    string `json:"pass,omitempty"`
	TZ      string `json:"tz,omitempty"`
	Operand string `json:"operand,omitempty"` // name text, or mode "ap"/"sta"
}

// errorString is a tiny error type so serialCmd can return reasons without importing
// fmt just for errors.New at each branch.
type errorString string

func (e errorString) Error() string { return string(e) }

// serialCmd builds the wire line + expected ack token for one request, or an error for
// an unknown verb or a missing operand. Hex encoding matches the firmware parser exactly
// (esprom.ProvField: "-" is the empty operand).
func serialCmd(req orbCmdRequest) (line string, ack string, err error) {
	switch strings.ToLower(strings.TrimSpace(req.Command)) {
	case "name":
		clean, cerr := orbfleet.CleanName(req.Operand)
		if cerr != nil {
			return "", "", cerr
		}
		return esprom.SerialLine("NAME", esprom.ProvField(clean)), "NAME-ACK", nil
	case "scan":
		return esprom.SerialLine("SCAN", ""), "PROVSCAN-END", nil
	case "mode":
		m := strings.ToLower(strings.TrimSpace(req.Operand))
		if m != "ap" && m != "sta" {
			return "", "", errorString("mode must be ap or sta")
		}
		return esprom.SerialLine("MODE", esprom.ProvField(m)), "MODE-ACK", nil
	case "reset":
		if strings.TrimSpace(req.Operand) != "ALL" {
			return "", "", errorString("reset requires the guard operand ALL")
		}
		// Two acks. The firmware reboots even when only one of its two NVS partitions
		// erased — a half-wiped board is neither configured nor clean — and says
		// RESET-PART for that. Waiting on RESET-ACK alone would time out on a board
		// that did exactly what it could, and report a clean reset it did not do.
		return esprom.SerialLine("RESET", "ALL"), "RESET-ACK,RESET-PART", nil
	case "ping":
		return esprom.SerialLine("PING", ""), "ORB-PING", nil
	case "prov":
		if strings.TrimSpace(req.SSID) == "" {
			return "", "", errorString("prov requires an ssid")
		}
		return esprom.ProvLine(req.SSID, req.Pass, req.TZ), "PROV-ACK", nil
	default:
		return "", "", errorString("unknown serial command")
	}
}

// handleOrbCmd drives one serial command over the console of the orb on req.Port and
// streams the exchange as NDJSON, exactly as handleProvision streams a PROV line. It
// opens no download mode and forces no reboot first: orb_net.c runs the serial reader
// for the whole life of the app, so a configured orb answers a plain console line. A
// name the orb accepted is written back into the fleet store under the MAC the card
// already keys on; a lease read after a PROV is learned so the proxy may dial it.
func (s *Server) handleOrbCmd(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req orbCmdRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "bad request: "+err.Error(), http.StatusBadRequest)
		return
	}
	req.Port = strings.TrimSpace(req.Port)
	if req.Port == "" {
		http.Error(w, "port is required", http.StatusBadRequest)
		return
	}
	line, ack, cerr := serialCmd(req)
	if cerr != nil {
		http.Error(w, cerr.Error(), http.StatusBadRequest)
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

	// Same per-cable claim a flash or provision takes, so a command can never collide
	// with a concurrent flash on the same wire, and so it is cancellable and visible.
	job, ctx, oerr := s.ops.start(r.Context(), req.Port, opCmd, cmdBudget)
	if oerr != nil {
		send(map[string]any{"error": oerr.Error()})
		return
	}
	defer s.ops.finish(job)
	if portLock, perr := orbbind.PressPort(ctx, req.Port, pressWindow); perr != nil {
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

	// serialCmd packs a multi-ack verb as a comma list; RESET is the only one today.
	acks := strings.Split(ack, ",")
	res, err := esprom.RunCommand(ctx, pt, line, acks, func(l string) {
		if strings.Contains(l, "RESET-PART") {
			send(map[string]any{"log": "orb reset was PARTIAL — one NVS partition survived; " +
				"credentials may remain on the board. Re-run RESET ALL, and reflash with wipe if it repeats."})
		}
		send(map[string]any{"log": "console: " + l})
	})
	// A cancelled or timed-out operation reports through the op registry, so say so.
	// A read error AFTER the ack is the orb vanishing on reboot — that is a success.
	if err != nil && (res == nil || !res.Ack) {
		if res != nil && res.Crash != "" {
			// The budget died on a panic voice from the console: say crashed,
			// not merely unanswered — the difference is retype vs report.
			err = fmt.Errorf("%w — the board crashed on the command: %s", err, res.Crash)
		}
		send(map[string]any{"error": err.Error()})
		return
	}
	if res == nil {
		send(map[string]any{"error": "no verdict from " + req.Port})
		return
	}

	// A name the orb accepted is the same write the fleet card would have made over
	// LAN, so it lands in the store under the MAC already on the card. A reset or mode
	// change alters what the store can claim about the board, so it is recorded honestly.
	if strings.EqualFold(req.Command, "name") && res.Ack && req.MAC != "" && s.fleet != nil {
		// The board took the word into NVS; the hub's copy is the cache of it and rides
		// in state.json so the row still reads by name at the next launch. A status
		// read re-checks the copy the moment the board is reachable again.
		if _, changed, werr := s.fleet.NoteDeviceName(req.MAC, req.Operand); werr != nil {
			send(map[string]any{"log": "note: name set on the orb but not yet on the card: " + werr.Error()})
		} else if changed {
			s.saveState()
		}
	}
	// Any ack proves the chip answers the orb serial door, which an orb-only
	// door: whatever this hub remembered about the board being lonely is now
	// contradicted by the board itself.
	if res.Ack {
		s.lonely.clear(macNorm(req.MAC))
		// A PING the operator sent is the same evidence as the one the hub sends
		// itself, and it belongs in the same place: the chip answered the orb door,
		// so its row is a card and its port is no longer a blank to flash. The MAC
		// the caller named wins; the descriptor's is the fallback, because the
		// forget dialog sends a port and not a chip.
		if strings.EqualFold(req.Command, "ping") {
			mac := macNorm(req.MAC)
			if mac == "" {
				mac = macNorm(hostUSBSerial(req.Port))
			}
			changed := s.recordCableProof(req.Port, mac, res.State, res.FW, "")
			if changed {
				s.saveState()
			}
			s.wakeOrbs()
		}
	}
	// A board this hub just factory-reset over the lead has lost its own word
	// — its NVS name went with the credentials — so the card takes the default
	// name the reset implies. It is a store-cache write only: the next name the
	// device gives (the next owner's word) corrects it on the next fold.
	if strings.EqualFold(req.Command, "reset") && res.Ack && req.MAC != "" && s.fleet != nil {
		if _, _, werr := s.fleet.NoteDeviceName(macNorm(req.MAC), defaultOrbName); werr != nil {
			send(map[string]any{"log": "note: reset landed but the card kept its word: " + werr.Error()})
		} else {
			s.saveState()
			s.wakeOrbs()
		}
	}
	if strings.EqualFold(req.Command, "prov") && res.IP != "" {
		s.grants.learn(res.IP)
		s.setOrbIP(res.IP)
		s.saveState()
	}

	send(map[string]any{"result": map[string]any{
		"ack":      res.Ack,
		"nak":      res.Nak,
		"ip":       res.IP,
		"state":    res.State,
		"networks": res.Networks,
		// The no-ack verdict's receipts: when a panic line crossed the console
		// between the push and the budget, "didn't accept" is a lie — it accepted
		// and died. The UI quotes it.
		"crash": res.Crash,
	}})
}
