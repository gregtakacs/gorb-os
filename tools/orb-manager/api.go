package main

import (
	"encoding/json"
	"net/http"
	"os/exec"
	"runtime"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
)

// jsonHandler returns (payload, error); s.json wraps it into a uniform
// {ok,data} / {ok,error} envelope so the UI has one shape to parse.
type jsonHandler func(r *http.Request) (any, error)

func (s *Server) json(h jsonHandler) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		payload, err := h(r)
		if err != nil {
			_ = json.NewEncoder(w).Encode(map[string]any{"ok": false, "error": err.Error()})
			return
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"ok": true, "data": payload})
	}
}

func (s *Server) hello(*http.Request) (any, error) {
	b := s.firmwareBundle()
	fwVersion, fwSource := "", ""
	if b != nil {
		fwVersion, fwSource = b.Version, b.Source
	}
	return map[string]any{"platform": runtime.GOOS, "version": version, "repoDir": s.repoDir,
		"firmware": fwVersion, "firmwareSource": fwSource}, nil
}

// listPorts feeds the Onboard port picker. The payload is an object, not the
// bare slice esprom returns: app.js reads `const { ports } = ...`, so a bare
// array left ports undefined and the picker showed an error instead of the
// cable the operator plugged in. The slice is never nil for the same reason —
// the UI takes .length on it directly, and null has no length either.
func (s *Server) listPorts(*http.Request) (any, error) {
	ports, err := esprom.ListPorts()
	if err != nil {
		return nil, err
	}
	return portsPayload(ports), nil
}

func portsPayload(ports []string) map[string]any {
	if ports == nil {
		ports = []string{}
	}
	return map[string]any{"ports": ports}
}

// openBrowser launches the default browser once, best-effort, after bind.
func openBrowser(url string) {
	var cmd string
	var args []string
	switch runtime.GOOS {
	case "darwin":
		cmd = "open"
	case "windows":
		cmd, args = "rundll32", []string{"url.dll,FileProtocolHandler"}
	default:
		cmd = "xdg-open"
	}
	args = append(args, url)
	_ = exec.Command(cmd, args...).Start()
}
