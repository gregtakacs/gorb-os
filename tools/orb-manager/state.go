package main

import (
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
)

// Remembered orbs. A hub that forgets the orb it just found makes the operator retype
// an address every launch, and the proxy will not dial an address it has not learned —
// so the learned set has to outlive the process that learned it.
//
// What is persisted is an ADDRESS, the MAC it was committed for, and the last name the
// hub read off that board. None of that is a credential. The launch token stays
// per-process and is never written, and the Wi-Fi passphrase a join used is not stored
// either, because the orb owns that secret (orb_net.c keeps it in NVS) and a second
// copy in a config dir is a second place it leaks. The stored name is a CACHE and not
// an authority: the device owns the word, every status read corrects the copy, and a
// board that answers with an empty name empties it here too. Item 9 rewrites this
// paragraph on purpose: display pairing hands the hub a PERMANENT device token, and the
// first real credential in this file must be written with the same 0600, never logged,
// never echoed to a page, and never copied into a firmware bundle.
//
// The file lives under the OS config dir beside the app-window profile, and holds one
// timestamp per address so an old list can expire on its own rather than accreting
// every orb the machine has ever touched.

// stateVersion is written into the file so a future format change can read old ones
// instead of mistaking them for corruption.
//
// Version 3 is the address book: MAC, IP and Port, plus the last name the hub read off
// the board (a cache, restored as such and corrected by the next device read). Version
// 2 files carry name, room, tty, cable, firstSeen and announces as well; the name is
// read back as the cache it always eventually was, and the rest are dropped rather than
// rejected — a hub downgrading keeps every address it knew, loses no grant, and simply
// asks the boards for their own words again. Nothing is migrated on disk: the next save
// writes the smaller shape, because the file is a cache of what the hub can re-learn and
// not the owner of any of it. Entries with no address are dropped at load: a cable-only
// board is a fact about this launch's bench, not a remembered destination.
//
// Version 1 and 2 files still load. A v1 file has no MAC per grant, so those grants come
// back unbound.
const stateVersion = 3

type hubState struct {
	Version int               `json:"version"`
	LastIP  string            `json:"lastIp,omitempty"`
	Grants  map[string]int    `json:"grants"` // ip -> unix seconds it was learned
	MACs    map[string]string `json:"macs,omitempty"`
	Fleet   []orbfleet.Orb    `json:"fleet,omitempty"`
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
	st := hubState{Version: stateVersion, Grants: map[string]int{}, MACs: map[string]string{}}
	for _, ip := range s.grants.list() {
		st.Grants[ip] = int(time.Now().Unix())
		if m := s.grants.bound(ip); m != "" {
			st.MACs[ip] = m
		}
	}
	if s.fleet != nil {
		st.Fleet = s.fleet.Snapshot()
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
			// The MAC this address was committed for, when the file carries one. A
			// version 1 file has none, and the grant comes back unbound: dialable,
			// because the operator did commit to it, but the card has to say the hub
			// cannot prove which board is there.
			s.grants.learnAs(ip, st.MACs[ip])
			n++
		}
	}
	if s.fleet != nil && len(st.Fleet) > 0 {
		s.fleet.Restore(st.Fleet)
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
