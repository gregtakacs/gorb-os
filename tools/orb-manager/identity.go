package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"sync"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
)

// Identity is opt-in and expensive: a probe reboots the orb to catch the
// bootloader printing its own descriptor. So the hub never probes on its own —
// the Onboard card offers one button per port, and the answer is cached here
// against the port path so a reload of the page does not cost another reboot.
type identityCache struct {
	mu     sync.Mutex
	byPort map[string]esprom.OrbInfo
	// inFlight guards one probe per port, so a double click cannot queue two
	// resets of the same chip.
	inFlight map[string]bool
}

// newHub builds a Server in the state main() leaves it in, so a test cannot
// accidentally exercise a half-wired hub with a nil cache.
func newHub() *Server {
	return &Server{identities: newIdentityCache()}
}

func newIdentityCache() *identityCache {
	return &identityCache{byPort: map[string]esprom.OrbInfo{}, inFlight: map[string]bool{}}
}

func (c *identityCache) get(port string) (esprom.OrbInfo, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	v, ok := c.byPort[port]
	return v, ok
}

// begin claims the port for probing, reporting false if one is already running.
func (c *identityCache) begin(port string) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.inFlight[port] {
		return false
	}
	c.inFlight[port] = true
	return true
}

func (c *identityCache) done(port string, info esprom.OrbInfo) {
	c.mu.Lock()
	defer c.mu.Unlock()
	delete(c.inFlight, port)
	c.byPort[port] = info
}

func (c *identityCache) all() map[string]esprom.OrbInfo {
	c.mu.Lock()
	defer c.mu.Unlock()
	out := make(map[string]esprom.OrbInfo, len(c.byPort))
	for k, v := range c.byPort {
		out[k] = v
	}
	return out
}

// forget drops a port's cached answer, which is what makes a re-probe after a
// flash meaningful instead of returning the pre-flash version forever.
func (c *identityCache) forget(port string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	delete(c.byPort, port)
}

// handlePorts answers the Onboard picker. Each entry carries the cached identity
// when there is one, so the card renders the MAC beside the port without having
// to ask the chip again.
func (s *Server) handlePorts(w http.ResponseWriter, r *http.Request) {
	ports, err := esprom.ListPorts()
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	if ports == nil {
		ports = []string{}
	}
	cached := s.identities.all()
	entries := make([]map[string]any, 0, len(ports))
	for _, p := range ports {
		// The hint costs nothing — sysfs, no port open, no reset — so it is on
		// every entry. It is what labels an unprobed cable as an orb at all, and
		// an identity left behind the cache would make the picker look empty.
		e := map[string]any{"port": p, "known": false, "hint": usbHint(p)}
		if info, ok := cached[p]; ok {
			e["known"] = true
			e["isOrb"] = info.IsOrb
			e["mac"] = info.MAC
			e["project"] = info.Project
			e["version"] = info.Version
			e["compileTime"] = info.CompileTime
			e["compiledAt"] = info.CompiledAt
			e["elfSha256"] = info.ElfSHA256
			e["idf"] = info.IDF
			e["ip"] = info.IP
			e["reason"] = info.Reason
			e["probedAt"] = info.ProbedAt
			e["verdict"] = string(s.verdictFor(info))
		}
		entries = append(entries, e)
	}
	// Same object wrapper the bare list used: app.js destructures { ports }, and
	// a bare array once left the picker showing an error instead of a cable.
	s.jsonOK(w, map[string]any{"ports": entries, "count": len(entries)})
}

// usbHint is what the host already knows about the device behind a port, without
// touching it: the Espressif peripheral names itself, and its serial is the MAC.
func usbHint(port string) string {
	u := esprom.ClassifyPort(port)
	switch {
	case u.IsOrbDevice():
		if u.Serial != "" {
			return "orb-device:" + u.Serial
		}
		return "orb-device"
	case !u.Known:
		return ""
	default:
		return fmt.Sprintf("usb-%04x:%04x", u.Vendor, u.Product)
	}
}

// handleProbe identifies one port on explicit request. It takes the same per-port
// lock a flash does, because an identity probe resets the chip and a concurrent
// write on that wire would be corrupted by it.
func (s *Server) handleProbe(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		Port string `json:"port"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Port == "" {
		s.jsonErr(w, "no serial port named")
		return
	}
	// Read the flag under the mutex, do not probe the mutex: a flash holds s.mu
	// only long enough to set s.flashing, so a TryLock here would succeed in the
	// middle of a flash and the probe would reset a chip mid-write.
	if s.flashRunning() {
		s.jsonErr(w, "a flash is running; try again when it finishes")
		return
	}

	if !s.identities.begin(req.Port) {
		s.jsonErr(w, "already identifying that port")
		return
	}
	lock, lerr := orbbind.LockPort(req.Port)
	if lerr != nil {
		s.identities.done(req.Port, esprom.OrbInfo{Port: req.Port, Reason: "port busy: " + lerr.Error()})
		s.jsonErr(w, "port busy: "+lerr.Error())
		return
	}
	info, err := esprom.ProbeOrb(req.Port)
	lock.Release()
	if err != nil {
		s.identities.done(req.Port, esprom.OrbInfo{Port: req.Port, Reason: err.Error()})
		s.jsonErr(w, err.Error())
		return
	}
	s.identities.done(req.Port, info)
	info.Verdict = string(s.verdictFor(info))
	s.jsonOK(w, info)
}

// verdictFor compares the running firmware a probe found against the firmware the
// hub would flash right now — the built-in bundle, or whatever the user picked.
// A port that is not an orb gets no verdict: the question does not apply.
func (s *Server) verdictFor(info esprom.OrbInfo) firmware.Verdict {
	if !info.IsOrb {
		return firmware.VerdictUnknown
	}
	b := s.firmwareBundle()
	if b == nil {
		return firmware.VerdictUnknown
	}
	return firmware.Compare(orbRelease(info), b.Release())
}

func orbRelease(info esprom.OrbInfo) firmware.Release {
	r := firmware.Release{Version: info.Version, ElfSHA256: info.ElfSHA256}
	if info.CompiledAt != "" {
		if t, err := time.Parse(time.RFC3339, info.CompiledAt); err == nil {
			r.CompiledAt = t
		}
	}
	return r
}
