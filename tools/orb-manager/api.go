package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"path"
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
	return map[string]any{"platform": runtime.GOOS, "version": version, "repoDir": s.repoDir, "buildDir": s.buildDir}, nil
}

func (s *Server) listPorts(*http.Request) (any, error) {
	return esprom.ListPorts()
}

func (s *Server) describeBuild(*http.Request) (any, error) {
	if s.buildDir == "" {
		return map[string]any{"present": false}, nil
	}
	if _, err := os.Stat(path.Join(s.buildDir, "flasher_args.json")); err != nil {
		return map[string]any{"present": false, "buildDir": s.buildDir}, nil
	}
	plan, err := esprom.PlanFromBuildDir(s.buildDir)
	if err != nil {
		return nil, err
	}
	imgs := make([]map[string]any, 0, len(plan.Images))
	for _, im := range plan.Images {
		size := int64(0)
		if st, err := os.Stat(im.Path); err == nil {
			size = st.Size()
		}
		imgs = append(imgs, map[string]any{"name": path.Base(im.Path), "offset": fmt.Sprintf("0x%X", im.Offset), "bytes": size})
	}
	return map[string]any{"present": true, "buildDir": s.buildDir, "images": imgs}, nil
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
