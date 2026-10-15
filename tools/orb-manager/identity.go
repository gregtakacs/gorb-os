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
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// Identity is opt-in and expensive: a probe reboots the orb to catch the
// bootloader printing its own descriptor. So the hub never probes on its own —
// the Onboard card offers one button per port, and the answer is cached here so a
// reload of the page does not cost another reboot.
//
// The cache key is the CABLE, not the tty name: the entry remembers the USB serial
// the host could see when the probe ran, and an answer is only handed back while
// that serial is still what is behind the port. Keying on the path alone was wrong
// in the way that matters most on a bench — orb A is identified on /dev/ttyACM0,
// orb A is unplugged, orb B is plugged into the same socket, and the picker keeps
// asserting B is running the version that belongs to A. A tty name is a lease from
// the OS, not an identity.
type identityEntry struct {
	info esprom.OrbInfo
	who  string    // the USB serial seen when this was recorded, "" if the host could not attribute the cable
	at   time.Time // when it was recorded, for the TTL
}

type identityCache struct {
	mu       sync.Mutex
	byPort   map[string]identityEntry
	ttl      time.Duration
	inFlight map[string]bool
	// usbMAC is the host's free view of what cable is in a port. It is a field so
	// a test can replay a re-plug without a cable; production uses ClassifyPort.
	usbMAC func(port string) string
}

// identityTTL bounds how long an answer may be trusted without being paid for
// again. It is a reboot behind the user, so an answer from last hour is not a
// description of the orb now: an orb that has since OTA'd or been reflashed from
// another tool would still be reported with the old version.
const identityTTL = 30 * time.Minute

// newHub builds a Server in the state main() leaves it in, so a test cannot
// accidentally exercise a half-wired hub with a nil cache or a nil registry.
func newHub() *Server {
	return &Server{identities: newIdentityCache(), ops: newOpRegistry(),
		token: newLaunchToken(), grants: newIPGrants(ipGrantTTL),
		cables: newCableCache(), live: newLiveCache(),
		lonely: newLonelyMark(),
		orb:    orbnet.NewClient(scanTimeout),
		device: orbnet.NewClient(deviceTimeout),
		ota:    orbnet.NewClient(otaTimeout),
		fleet:  orbfleet.New(orbfleet.Config{}), boots: map[string]*bootVerdict{}}
}

func newIdentityCache() *identityCache {
	return &identityCache{
		byPort:   map[string]identityEntry{},
		ttl:      identityTTL,
		inFlight: map[string]bool{},
		usbMAC:   hostUSBSerial,
	}
}

// hostUSBSerial is the serial number the OS reports for the USB device behind a
// port, which for the ESP32-S3 native USB peripheral is the chip's base MAC. An
// empty string means the host cannot attribute the cable at all — the case on
// Windows without a friendly-name read, and on macOS whenever ioreg has nothing to
// say about the port. Refusing to name a MAC is always safe here; guessing one is
// what the gate in esprom/identity.go exists to prevent.
func hostUSBSerial(port string) string {
	u := esprom.ClassifyPort(port)
	if !u.IsOrbDevice() {
		return ""
	}
	return u.Serial
}

// get returns the cached answer for port, and nothing when the cable behind it has
// turned out to be a different orb, when the answer has aged out, or when the port
// has disappeared. Reporting "no answer" is always the right answer to a question
// about a device the hub can no longer prove is the one it measured.
func (c *identityCache) get(port string) (esprom.OrbInfo, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	e, ok := c.byPort[port]
	if !ok {
		return esprom.OrbInfo{}, false
	}
	if !c.freshLocked(port, e) {
		delete(c.byPort, port)
		return esprom.OrbInfo{}, false
	}
	return e.info, true
}

// freshLocked is the staleness rule, stated once for get and for the listing path.
func (c *identityCache) freshLocked(port string, e identityEntry) bool {
	if c.ttl > 0 && time.Since(e.at) > c.ttl {
		return false
	}
	return e.who == c.serialOf(port)
}

func (c *identityCache) serialOf(port string) string {
	if c.usbMAC == nil {
		return ""
	}
	return c.usbMAC(port)
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

// done records an answer, tagged with who the host believes is behind the port at
// the moment the probe finished — the same instant that produced the answer, so a
// cable swapped between those two events is caught rather than inherited.
func (c *identityCache) done(port string, info esprom.OrbInfo) {
	c.mu.Lock()
	defer c.mu.Unlock()
	delete(c.inFlight, port)
	c.byPort[port] = identityEntry{info: info, who: c.serialOf(port), at: time.Now()}
}

// all returns the still-true answers, keyed by port, and drops what is no longer
// true: entries whose port vanished from the enumeration (unplugged, so the next
// device to take that name starts clean) and entries whose cable is a different orb.
func (c *identityCache) all(present ...string) map[string]esprom.OrbInfo {
	c.mu.Lock()
	defer c.mu.Unlock()
	seen := make(map[string]bool, len(present))
	for _, p := range present {
		seen[p] = true
	}
	out := make(map[string]esprom.OrbInfo, len(c.byPort))
	for port, e := range c.byPort {
		if len(present) > 0 && !seen[port] {
			delete(c.byPort, port)
			continue
		}
		if !c.freshLocked(port, e) {
			delete(c.byPort, port)
			continue
		}
		out[port] = e.info
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
	cached := s.identities.all(ports...)
	entries := make([]map[string]any, 0, len(ports))
	for _, p := range ports {
		// Two separate facts, because the client needs them for different things:
		// "attributed" is what the host can say about the cable for free (sysfs or
		// the registry, no port open, no reset), and "probed" is whether someone has
		// paid a reboot to ask the chip. Collapsing them once hid the free MAC from
		// the picker label, which is the only thing that tells two cables apart.
		u := esprom.ClassifyPort(p)
		e := map[string]any{
			"port":       p,
			"hint":       usbLabel(u),
			"attributed": u.Known,
			"probed":     false,
			// "espressif" is the New Orb gate: a genuine Espressif chip (VID
			// 303A) whatever it currently enumerates as. The page asks it of a
			// board that may be blank, boot-looping, or in download mode, and
			// only the host can answer without touching the chip. The product
			// id decides nothing here: blank and flashed boards report
			// differently, and a gate keyed on the product id would hide a
			// board precisely when the operator most needs the door open.
			"espressif": u.IsEspressif(),
			// "orbSeen" is the other half of that gate, and the mirror of the
			// row field of the same meaning: the hub has evidence this chip has
			// been an orb, so the board belongs on a card and not in the New
			// Orb picker. It is NOT row existence — the roster writes a row for
			// a bare cable sighting, so existence would make every board that
			// has ever been plugged in un-adoptable forever, which is the bug
			// this field replaces. Always written, so a hub that cannot answer
			// the question reads as a no rather than a hole.
			"orbSeen": false,
		}
		// The MAC the host read off the descriptor, as opposed to the one a probe
		// heard on the console. Kept separate so the client never has to unpick a
		// display string to label a cable.
		if u.IsOrbDevice() && u.Serial != "" {
			e["usbMac"] = u.Serial
			// A board this hub left lonely is a blank Espressif chip with a
			// memory, not an orb: the entry says so, and the picker labels it
			// "left lonely" instead of letting the descriptor silently re-orb it.
			if s.lonely.has(macNorm(u.Serial)) {
				e["lonely"] = true
			} else {
				// Two facts the page must not conflate, so they stay two fields:
				// the address book holds this chip at all ("inFleet", what the
				// add-by-IP door refuses on), and the book proves it has been an
				// orb ("orbSeen", what hides it from the New Orb door). A row that
				// only ever recorded a cable gives the first without the second.
				if row, known := s.fleetKnown(macNorm(u.Serial)); known {
					e["inFleet"] = true
					if storeProvesOrb(row) {
						e["orbSeen"] = true
					}
				}
			}
		}

		if info, ok := cached[p]; ok {
			e["probed"] = true
			e["isOrb"] = info.IsOrb
			// A chip that has answered for itself as an orb is the strongest
			// proof the hub keeps, and it outranks the address book: a board
			// identified after a wipe is known even before it has a name.
			if info.IsOrb {
				e["orbSeen"] = true
			}
			e["mac"] = info.MAC

			e["project"] = info.Project
			e["version"] = info.Version
			// The same stamp as structure, so a card can print v1.0.0 without
			// parsing the string. Legacy boards come back as legacy: true.
			e["semver"] = semverPayload(info.Version)
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

// usbLabel is what the host already knows about the device behind a port, without
// touching it: the Espressif peripheral names itself, and its serial is the MAC.
// It is deliberately a display string, not something to parse decisions out of —
// the machine-readable form goes out as usbMac.
func usbLabel(u esprom.USBClass) string {
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
	if !s.identities.begin(req.Port) {
		s.jsonErr(w, "already identifying that port")
		return
	}
	// A probe resets the chip, so it may not share one cable with an operation
	// that is writing it. The claim is PER PORT, which is the whole change from
	// the old process-wide flag: a flash on /dev/ttyACM0 no longer blocks
	// Identify on /dev/ttyACM1, and the registry entry is held for the probe's
	// whole run rather than being a mutex that is only borrowed to set a bool.
	if kind, busy := s.ops.busy(req.Port); busy {
		reason := fmt.Sprintf("a %s is running on that port - an identity probe resets the chip, so it will not share the wire with it", kind)
		s.identities.done(req.Port, esprom.OrbInfo{Port: req.Port, Reason: reason})
		s.jsonErr(w, reason)
		return
	}
	job, ctx, oerr := s.ops.start(r.Context(), req.Port, opProbe, probeBudget)
	if oerr != nil {
		s.identities.done(req.Port, esprom.OrbInfo{Port: req.Port, Reason: oerr.Error()})
		s.jsonErr(w, oerr.Error())
		return
	}
	defer s.ops.finish(job)

	lock, lerr := orbbind.LockPort(req.Port)
	if lerr != nil {
		s.identities.done(req.Port, esprom.OrbInfo{Port: req.Port, Reason: "port busy: " + lerr.Error()})
		s.jsonErr(w, "port busy: "+lerr.Error())
		return
	}
	info, err := esprom.ProbeOrb(ctx, req.Port)
	lock.Release()
	if err != nil {
		s.identities.done(req.Port, esprom.OrbInfo{Port: req.Port, Reason: err.Error()})
		s.jsonErr(w, err.Error())
		return
	}
	s.identities.done(req.Port, info)
	if info.MAC != "" {
		// A bootloader descriptor read down the USB lead is the strongest thing the
		// hub can say about a board: it is the chip answering for itself, not a name
		// claimed on the LAN. The fleet holds that sighting above every announcement.
		s.markCable(info.MAC)
	}
	info.Verdict = string(s.verdictFor(info))
	s.jsonOK(w, info)
}

// probeBudget bounds one identity probe. The probe itself reads a 9 s boot window
// plus the ROM handshake; this is the outer ceiling a Cancel or a forced quit stops
// against, so a wedged console cannot hold a cable indefinitely.
const probeBudget = 40 * time.Second

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
