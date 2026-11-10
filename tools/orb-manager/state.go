package main

import (
	"crypto/rand"
	"encoding/hex"
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
// not the owner of any of it. Entries with no address are dropped at load, with no
// exception: the book holds addresses, and a board with nothing to dial re-learns itself
// the moment it sits on a lead (the rule, stated by the operator, bench 2026-10-15). The
// lonely set rides in a v3 file as an added field, and so does the remembered mark: a hub
// that never saw either starts without them and loses a memory, never a credential.
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
	// Lonely carries the MACs this hub left lonely (lonelyset.go). It is a
	// memory of an act the hub itself performed, so it is the one piece of hub
	// state a cable descriptor must not overrule: without it a restart would
	// let the next sweep rebuild from the MAC the lonely board still enumerates,
	// and the row the operator ended would rise quietly on the first frame.
	Lonely []string `json:"lonely,omitempty"`
	// MapTiles is the map tool's TileJSON (maps.go), absent for OpenFreeMap's default. A
	// hub that never set one writes nothing, and an older hub drops it harmlessly.
	MapTiles string `json:"mapTiles,omitempty"`
	// InstallID is this installation's id (§7.2, ruling 25): minted once on the first
	// G3 run and never regenerated. It is an id and not a credential — it names the
	// install without authenticating anything — and this step sends it nowhere: it
	// reaches the network only when H2 links this Orb Manager to an account.
	InstallID string `json:"installId,omitempty"`
	// UpdateChoices is what the operator refused on the Updates list (§7.6, ruling 26):
	// one entry per item, keyed Kind + ":" + Key. A skipped version is a memory of one
	// answer to one version — the next newer one is offered again — and a keep is a
	// memory of an intent whose bytes (the theme's `.origin.json`) are already gone from
	// the shelf, so the entry is the only trace left that the theme was ever tracked.
	// Nothing here is a credential and nothing here grants anything.
	UpdateChoices map[string]UpdateChoice `json:"updateChoices,omitempty"`
}

// UpdateChoice is one refusal on the Updates list. Kind names the row's kind ("theme",
// "app", "firmware", "manager") and Key the row's own key: a theme's origin design id, an
// app's slug, "hub" for firmware. SkippedVersion is the version the operator passed on
// this time; Keep marks a theme retired from tracking for good.
type UpdateChoice struct {
	Kind           string `json:"kind"`
	Key            string `json:"key"`
	SkippedVersion int    `json:"skippedVersion,omitempty"`
	Keep           bool   `json:"keep,omitempty"`
}

// choiceKey is how a refusal is filed. It is a function so the write side and the read
// side can never disagree about the shape of the map's keys.
func choiceKey(kind, key string) string { return kind + ":" + key }

// mintInstallID is the one install id this hub will ever mint: 16 bytes of
// crypto/rand, lower-case hex, 32 characters. A rand failure is answered with the
// empty string rather than a panic or a clock-derived fallback, because a hub with no
// id still serves everything it always served and H2's link retries; an id guessed
// from a timestamp would be an id two installs could share.
func mintInstallID() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		return ""
	}
	return hex.EncodeToString(b[:])
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
	s.held.writeBack(&st)
	if s.lonely != nil {
		st.Lonely = s.lonely.list()
	}
	st.LastIP = s.currentOrbIP()
	st.MapTiles = s.maps.stored()
	st.InstallID = s.installID
	st.UpdateChoices = s.choicesLocked()
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
	// The install id comes back exactly as it was written (state.go's mintInstallID
	// is the only thing that ever mints one, and armState is the only thing that
	// calls it), so a restart recognises its own installation.
	s.installID = st.InstallID
	// Refusals come back as they were filed, and a file that named none leaves the hub
	// with none: the reading of a missing key is always "no choice", never a guessed one.
	s.updateChoices = map[string]UpdateChoice{}
	for k, c := range st.UpdateChoices {
		if c.Kind == "" || c.Key == "" || choiceKey(c.Kind, c.Key) != k {
			// A hand-edited entry filed under a key that disagrees with its own body is
			// dropped rather than honoured: the key is what the list looks up.
			continue
		}
		s.updateChoices[k] = c
	}
	s.holdOthers(&st)
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
	if len(st.Lonely) > 0 {
		s.lonely.restore(st.Lonely)
	}
	if _, err := mapsURL(st.MapTiles); err == nil {
		s.maps.set(st.MapTiles)
	}
	if isV4Literal(strings.TrimSpace(st.LastIP)) {
		s.setOrbIP(strings.TrimSpace(st.LastIP))
	}
	return n
}

// choicesLocked is the snapshot saveState writes. Called with pmu already held, so it
// copies: the marshal that follows is outside any lock the caller still holds, and a map
// written by a refusal landing during that marshal would be a race the file's bytes
// would show as a torn answer.
func (s *Server) choicesLocked() map[string]UpdateChoice {
	if len(s.updateChoices) == 0 {
		return nil
	}
	out := make(map[string]UpdateChoice, len(s.updateChoices))
	for k, c := range s.updateChoices {
		out[k] = c
	}
	return out
}

// choiceFor reads one refusal. No entry is the same answer as a hub that was never told
// anything: "no choice".
func (s *Server) choiceFor(kind, key string) (UpdateChoice, bool) {
	s.pmu.Lock()
	defer s.pmu.Unlock()
	c, ok := s.updateChoices[choiceKey(kind, key)]
	return c, ok
}

// mutateChoice records one refusal under pmu and reports the merged entry. It does not
// save: the caller saves after any side effect it owes (the keep's sidecar removal), so
// a state.json never claims a keep for a theme whose origin is still on the shelf.
func (s *Server) mutateChoice(kind, key string, merge func(*UpdateChoice)) UpdateChoice {
	s.pmu.Lock()
	if s.updateChoices == nil {
		s.updateChoices = map[string]UpdateChoice{}
	}
	k := choiceKey(kind, key)
	c := s.updateChoices[k]
	c.Kind, c.Key = kind, key
	merge(&c)
	s.updateChoices[k] = c
	s.pmu.Unlock()
	return c
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
//
// It is also where the install id is born (§7.2, ruling 25): a file that named none —
// a first run, or any state.json from before G3, or a damaged one whose reading already
// reported itself in stateErr — gets one minted and written here, before anything is
// served, so the id on disk is never younger than the first answer that could have been
// asked about it. A file that carries one leaves it untouched.
func (s *Server) armState() int {
	p, err := statePath()
	if err != nil {
		s.stateErr = err
		return 0
	}
	s.statePath = p
	n := s.loadState()
	if s.installID == "" {
		s.installID = mintInstallID()
		if s.installID != "" {
			s.saveState()
		}
	}
	return n
}

// currentOrbIP is the last address the hub learned or was handed, for the Manage// card's pre-fill. It is a convenience mirror only: the proxy and the theme calls
// check grants, never this.
func (s *Server) currentOrbIP() string {
	s.imu.Lock()
	defer s.imu.Unlock()
	return s.orbIP
}
