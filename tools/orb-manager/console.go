package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
)

// The read-only USB console card: what tools/cap.py does, for an operator who has no
// Python and no clone. It opens the port the app is ALREADY running on, forwards every
// line it prints for a bounded window, and closes. It is registered as a port
// operation, so it refuses to share a cable with a flash and a Cancel reaches it.
//
// Read-only is the whole design. Nothing here asserts into the console, and no reset
// is issued: the port is opened without the download-mode pulse, so reading an orb
// cannot blank its screen or reboot it. The one control line touched is DTR, which
// IDF's USB-Serial-JTAG console requires asserted or it drops the bytes on the floor.

// consoleRequest is one console read: which cable, and for how long.
type consoleRequest struct {
	Port string `json:"port"`
	Secs int    `json:"secs"`
}

// consoleWindowDefault is long enough to catch a face change and a status line, short
// enough that a hub left open is not holding a port indefinitely.
const (
	consoleWindowDefault = 8
	consoleWindowMax     = 30
)

// handleConsole streams the boot/live console of a running orb as NDJSON. It is
// deliberately a stream and not a request/response: console output arrives over
// seconds, and an operator watching a device that is misbehaving wants the lines as
// they come.
func (s *Server) handleConsole(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req consoleRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		s.jsonErr(w, "bad request: "+err.Error())
		return
	}
	if req.Port == "" {
		s.jsonErr(w, "no serial port selected")
		return
	}
	secs := req.Secs
	if secs <= 0 {
		secs = consoleWindowDefault
	}
	if secs > consoleWindowMax {
		secs = consoleWindowMax
	}

	flusher, ok := w.(http.Flusher)
	if !ok {
		s.jsonErr(w, "streaming unsupported on this listener")
		return
	}
	w.Header().Set("Content-Type", "application/x-ndjson")
	w.Header().Set("Cache-Control", "no-store")
	send := func(ev map[string]any) {
		_ = json.NewEncoder(w).Encode(ev)
		flusher.Flush()
	}

	// Same per-cable claim as a flash, so a console read cannot sit on top of a
	// write, and so the Cancel button and the guarded quit cover this too.
	job, ctx, oerr := s.ops.start(r.Context(), req.Port, opConsole, time.Duration(secs+15)*time.Second)
	if oerr != nil {
		s.jsonErr(w, oerr.Error())
		return
	}
	defer s.ops.finish(job)

	lock, lerr := orbbind.PressPort(ctx, req.Port, pressWindow)
	if lerr != nil {
		s.jsonErr(w, lerr.Error())
		return
	}
	defer lock.Release()

	send(map[string]any{"log": fmt.Sprintf("reading %s for %d s — nothing is sent to the orb", req.Port, secs)})
	lines, err := esprom.ReadConsole(ctx, req.Port, secs)
	for _, l := range lines {
		send(map[string]any{"line": l})
	}
	if err != nil {
		send(map[string]any{"error": err.Error()})
		return
	}
	if len(lines) == 0 {
		// Silence is a result. IDF drops console bytes unless a host holds the port
		// with DTR asserted, so an empty window usually means the orb is quiet, not
		// broken — and saying so beats an empty box the operator reads as a fault.
		send(map[string]any{"done": "no output in the window — the orb prints only when something changes"})
		return
	}
	send(map[string]any{"done": fmt.Sprintf("%d line(s) read from %s", len(lines), req.Port)})
}
