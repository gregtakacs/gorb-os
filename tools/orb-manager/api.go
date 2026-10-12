package main

import (
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path"
	"runtime"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
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
	ports, err := esprom.ListPorts()
	if err != nil {
		return nil, err
	}
	bound := ""
	if t, err := orbbind.Load(mustGetwd()); err == nil {
		// .orb-target stores a by-id path but ListPorts returns /dev/ttyACMx,
		// so t.Serial is not itself a dropdown value. Report whichever listed
		// port canonicalizes to the bound orb, so the UI's "(bound orb)" tag
		// and its bound-port check both match what the user can actually pick.
		for _, p := range ports {
			if t.MatchesSerial(p) {
				bound = p
				break
			}
		}
	}
	return map[string]any{"ports": ports, "bound": bound}, nil
}

func mustGetwd() string {
	d, _ := os.Getwd()
	return d
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

func (s *Server) scan(r *http.Request) (any, error) {
	cidr := r.URL.Query().Get("cidr")
	if cidr == "" {
		cidr = guessHouseholdCIDR()
	}
	if cidr == "" {
		return nil, fmt.Errorf("could not infer a household /24; pass ?cidr=192.168.1.0/24")
	}
	return s.orb.Sweep(cidr)
}

// guessHouseholdCIDR derives the local /24 from the first non-loopback
// interface address, so the default scan stays inside the house by being
// derived from the host's own subnet.
func guessHouseholdCIDR() string {
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return ""
	}
	for _, a := range addrs {
		ipn, ok := a.(*net.IPNet)
		if !ok {
			continue
		}
		v4 := ipn.IP.To4()
		if v4 == nil || v4.IsLoopback() {
			continue
		}
		ones, bits := ipn.Mask.Size()
		if bits == 32 && ones >= 24 {
			return fmt.Sprintf("%d.%d.%d.0/24", v4[0], v4[1], v4[2])
		}
	}
	return ""
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
