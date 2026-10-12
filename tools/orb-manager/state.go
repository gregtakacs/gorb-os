package main

import (
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// Remembered orb addresses. A hub that forgets the orb it just found makes the
// operator retype an address every launch, and the proxy will not dial an address it
// has not learned — so the learned set has to outlive the process that learned it.
//
// What is persisted is an ADDRESS, nothing secret: the launch token is per-process
// and never written, and no credential the operator typed for a Wi-Fi join is stored
// anywhere either. The file lives under the OS config dir beside the app-window
// profile, and holds one timestamp per address so an old list can expire on its own
// rather than accreting every orb the machine has ever touched.

// stateVersion is written into the file so a future format change can read old ones
// instead of mistaking them for corruption.
const stateVersion = 1

type hubState struct {
	Version int            `json:"version"`
	LastIP  string         `json:"lastIp,omitempty"`
	Grants  map[string]int `json:"grants"` // ip -> unix seconds it was learned
}

// statePath is where the remembered addresses live. It follows the same convention as
// the app-window profile (window.go's browserProfileRel), because an operator who
// clears one expects to have cleared the other.
func statePath() (string, error) {
	dir, err := os.UserConfigDir()
	if err != nil {
		// Windows without %APPDATA%, or a stripped HOME: fall back to the temp dir
		// the port locks already use, so the hub still starts and still remembers for
		// this session rather than refusing to run.
		return filepath.Join(os.TempDir(), "orb-manager-state.json"), nil
	}
	return filepath.Join(dir, "orb-manager", "state.json"), nil
}

// saveState writes the learned set under one lock. It is called at the moments the
// set genuinely changes — an orb was learned, an address was entered or revoked —
// rather than on every request, so a hub idle for a week writes nothing.
func (s *Server) saveState() {
	s.pmu.Lock()
	defer s.pmu.Unlock()
	if s.statePath == "" {
		return
	}
	st := hubState{Version: stateVersion, Grants: map[string]int{}}
	for _, ip := range s.grants.list() {
		st.Grants[ip] = int(time.Now().Unix())
	}
	st.LastIP = s.currentOrbIP()
	b, err := json.MarshalIndent(st, "", "  ")
	if err != nil {
		return
	}
	if err := os.MkdirAll(filepath.Dir(s.statePath), 0o700); err != nil {
		s.stateErr = err
		return
	}
	// Written whole and renamed, so a crash mid-save cannot leave a half file that
	// reads as "no remembered orbs" and silently costs the operator their list.
	tmp := s.statePath + ".tmp"
	if err := os.WriteFile(tmp, b, 0o600); err != nil {
		s.stateErr = err
		return
	}
	if err := os.Rename(tmp, s.statePath); err != nil {
		_ = os.Remove(tmp)
		s.stateErr = err
	}
}

// loadState restores addresses learned by earlier runs. A missing or unreadable file
// is not an error worth reporting: the hub simply starts having learned nothing,
// which is the same state a first run is in. Only IPv4 literals are accepted, so a
// hand-edited file cannot smuggle a hostname into the set the proxy dials.
func (s *Server) loadState() int {
	if s.statePath == "" {
		return 0
	}
	b, err := os.ReadFile(s.statePath)
	if err != nil {
		return 0
	}
	var st hubState
	if err := json.Unmarshal(b, &st); err != nil {
		// Corrupt, or written by a future version. Keep the bytes for a human and
		// start clean rather than dialing whatever a damaged file named.
		s.stateErr = err
		return 0
	}
	n := 0
	for ip := range st.Grants {
		if isV4Literal(ip) {
			s.grants.learn(ip)
			n++
		}
	}
	if isV4Literal(strings.TrimSpace(st.LastIP)) {
		s.setOrbIP(strings.TrimSpace(st.LastIP))
	}
	return n
}

func isV4Literal(s string) bool {
	if s == "" || strings.Contains(s, ":") {
		return false
	}
	if strings.Count(s, ".") != 3 {
		return false
	}
	ip := net.ParseIP(s)
	return ip != nil && ip.To4() != nil
}

// armState points the hub at its state file and loads what is in it. Called once from
// main, after the hub is built and before anything is served, so the first /api/ports
// answer can already carry the orb from last time.
func (s *Server) armState() int {
	p, err := statePath()
	if err != nil {
		s.stateErr = err
		return 0
	}
	s.statePath = p
	return s.loadState()
}

// currentOrbIP is the last address the hub learned or was handed, for the Manage
// card's pre-fill. It is a convenience mirror only: the proxy and the theme calls
// check grants, never this.
func (s *Server) currentOrbIP() string {
	s.imu.Lock()
	defer s.imu.Unlock()
	return s.orbIP
}
