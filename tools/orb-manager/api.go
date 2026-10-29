package main

import (
	"encoding/json"
	"net/http"
	"os/exec"
	"runtime"
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
	// The library the hub resolved to, and which rung won (S6, plan §1). It is here rather
	// than only on /api/library because the two answers belong together when something is
	// wrong: "the hub is seeding the clone's copy" is the sentence that explains a hub on
	// a second machine behaving like the development one.
	appsDir, themesDir, appsFrom, themesFrom := "", "", "none", "none"
	if s.lib != nil {
		appsDir, themesDir, appsFrom, themesFrom =
			s.lib.AppsDir, s.lib.ThemesDir, s.lib.AppsSource, s.lib.ThemesSource
	}
	return map[string]any{"platform": runtime.GOOS, "version": version, "repoDir": s.repoDir,
		"firmware": fwVersion, "firmwareSource": fwSource,
		"appsDir": appsDir, "appsSource": appsFrom,
		"themesDir": themesDir, "themesSource": themesFrom}, nil
}

// portsPayload is the Onboard port list's envelope. It stays an object with a
// named ports field rather than a bare array because app.js destructures
// { ports }: when list discovery shed the wrapper the picker read undefined,
// took .length on it, and blamed the browser for a payload-shape bug. The slice
// is never nil for the same reason — null has no length.
//
// TestPortPickerContractMatchesTheHandler pins it against app.js. handlePorts
// sends richer entries through the same wrapper, so that invariant is one rule
// with one place of truth, not two that can disagree.
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
	c := exec.Command(cmd, args...)
	c.SysProcAttr = hiddenProc() // fallback tab: no console window beside it
	_ = c.Start()
}
