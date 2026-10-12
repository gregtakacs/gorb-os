// Package orbbind is the Go twin of tools/orb_bind.py and orb_bind.sh: it
// reads a clone's untracked .orb-target, and holds a per-orb advisory lock so
// two flashers can never write one board at once. Device selection is never
// positional — a bound orb that is not attached is refused by name.
package orbbind

import (
	"encoding/binary"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
)

// MAC is a 6-octet hardware address with a printable form.
type MAC [6]byte

func (m MAC) String() string {
	return fmt.Sprintf("%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5])
}

// Target is one clone's bound orb.
type Target struct {
	Label   string
	BaseMAC MAC
	Serial  string // a /dev/serial/by-id path or a tty
	IP      string
	Path    string // the .orb-target file it came from
}

// ParseMAC accepts aa:bb:cc:dd:ee:ff or aa-bb-... and returns the bytes.
func ParseMAC(s string) (MAC, error) {
	var mac MAC
	s = strings.TrimSpace(strings.ToLower(s))
	parts := strings.FieldsFunc(s, func(r rune) bool { return r == ':' || r == '-' })
	if len(parts) != 6 {
		return mac, fmt.Errorf("orbbind: MAC %q is not 6 octets", s)
	}
	for i, p := range parts {
		v, err := strconv.ParseUint(p, 16, 8)
		if err != nil {
			return mac, fmt.Errorf("orbbind: MAC %q octet %q", s, p)
		}
		mac[i] = byte(v)
	}
	return mac, nil
}

// Load reads .orb-target. An ORB_TARGET env var overrides the path; otherwise
// it walks up from startDir. Missing keys are fatal — the same refusal the
// Python twin enforces before any port is opened.
func Load(startDir string) (*Target, error) {
	path := os.Getenv("ORB_TARGET")
	if path == "" {
		path = find(startDir)
	}
	if path == "" {
		return nil, fmt.Errorf("orbbind: no .orb-target found from %s — write ORB_LABEL, ORB_BASE_MAC, ORB_SERIAL and ORB_IP, or set ORB_TARGET", startDir)
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("orbbind: read %s: %w", path, err)
	}
	t := &Target{Path: path}
	for _, line := range strings.Split(string(raw), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		k, v, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		k, v = strings.TrimSpace(k), strings.Trim(strings.TrimSpace(v), `"`)
		switch k {
		case "ORB_LABEL":
			t.Label = v
		case "ORB_BASE_MAC":
			if t.BaseMAC, err = ParseMAC(v); err != nil {
				return nil, err
			}
		case "ORB_SERIAL":
			t.Serial = v
		case "ORB_IP":
			t.IP = v
		}
	}
	if t.Label == "" || t.Serial == "" {
		return nil, fmt.Errorf("orbbind: %s missing ORB_LABEL/ORB_BASE_MAC/ORB_SERIAL", path)
	}
	return t, nil
}

// find walks up from dir to the filesystem root looking for .orb-target.
func find(dir string) string {
	for {
		cand := filepath.Join(dir, ".orb-target")
		if st, err := os.Stat(cand); err == nil && !st.IsDir() {
			return cand
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return ""
		}
		dir = parent
	}
}

// MatchesSerial reports whether the given serial path names this target's orb.
// It tolerates the two spellings the hub sees: .orb-target stores a
// /dev/serial/by-id/... path, while the port dropdown (esprom.ListPorts)
// enumerates plain /dev/ttyACMx. The strings differ for the same board, so a
// literal compare would refuse the bound orb; resolving both through
// EvalSymlinks makes the by-id path and the tty it points at compare equal.
// The raw strings are still compared first so an unattached-but-named path
// matches itself, exactly as the old EqualFold gate did.
func (t *Target) MatchesSerial(port string) bool {
	if strings.EqualFold(t.Serial, port) {
		return true
	}
	a, aerr := filepath.EvalSymlinks(t.Serial)
	b, berr := filepath.EvalSymlinks(port)
	if aerr != nil || berr != nil {
		return false
	}
	return a == b
}

// Port resolves the bound orb's serial path, refusing if it is not attached —
// naming the orb it wanted rather than grabbing whichever ttyACM enumerated.
func (t *Target) Port() (string, error) {
	if _, err := os.Stat(t.Serial); err != nil {
		return "", fmt.Errorf("orbbind: orb %s is not attached: no %s (unplugged? usbipd not attached?). No other port was touched", t.Label, t.Serial)
	}
	return t.Serial, nil
}

// Lock is a held per-orb advisory lock (the /tmp sidecar of orb_bind.py,
// keyed to the bound MAC — NOT the tty, which our own Port flocks).
type Lock struct {
	f *os.File
	t *Target
}

// Acquire takes the flock for this target, or refuses if another tool holds
// it — the loud double-write refusal, not a silent queue.
func (t *Target) Acquire() (*Lock, error) {
	norm := strings.ReplaceAll(strings.ToLower(t.BaseMAC.String()), ":", "")
	path := fmt.Sprintf("/tmp/orb-%s-%s.lock", t.Label, norm)
	f, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0o644)
	if err != nil {
		return nil, fmt.Errorf("orbbind: cannot open lock %s: %w", path, err)
	}
	if err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		f.Close()
		return nil, fmt.Errorf("orbbind: another orb tool holds %s (%s) — refusing to double-write", t.Label, t.BaseMAC)
	}
	return &Lock{f: f, t: t}, nil
}

// Release drops the flock and closes the sidecar.
func (l *Lock) Release() {
	if l == nil || l.f == nil {
		return
	}
	syscall.Flock(int(l.f.Fd()), syscall.LOCK_UN)
	l.f.Close()
}

// PortLock is an exclusive advisory lock on a PHYSICAL serial port, keyed to
// its canonical device path — not to any orb identity. A production user owns
// the orb plugged into the port they picked and must never be refused for it;
// this lock's only job is to stop two operations (a flash and a provision, or
// two tools) driving the same cable at once, which corrupts the write.
type PortLock struct {
	f    *os.File
	name string
}

// LockPort locks the port at path — a /dev/ttyACMx or a /dev/serial/by-id/...
// link. Both are resolved to the same canonical device first, so the two
// spellings lock together rather than letting each sneak past the other. It
// returns an error only when another operation already holds that exact port.
func LockPort(path string) (*PortLock, error) {
	canon := path
	if r, err := filepath.EvalSymlinks(path); err == nil {
		canon = r
	}
	safe := strings.ReplaceAll(strings.TrimPrefix(canon, "/dev/"), "/", "-")
	lockPath := filepath.Join(os.TempDir(), "orb-port-"+safe+".lock")
	f, err := os.OpenFile(lockPath, os.O_CREATE|os.O_RDWR, 0o644)
	if err != nil {
		return nil, fmt.Errorf("orbbind: open port lock %s: %w", lockPath, err)
	}
	if err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		f.Close()
		return nil, fmt.Errorf("port %s is busy — another flash or provision holds its lock", canon)
	}
	return &PortLock{f: f, name: canon}, nil
}

// Release drops the port lock. Safe to call on a nil receiver.
func (l *PortLock) Release() {
	if l == nil || l.f == nil {
		return
	}
	syscall.Flock(int(l.f.Fd()), syscall.LOCK_UN)
	l.f.Close()
}

// Name returns the canonical port path the lock is held on.
func (l *PortLock) Name() string {
	if l == nil {
		return ""
	}
	return l.name
}

// ARPMatches reports whether have is the same nic as want, tolerating the +1
// low octet the STA/AP/BT MAC variants use (first five octets must match).
func ARPMatches(have, want MAC) bool {
	for i := 0; i < 5; i++ {
		if have[i] != want[i] {
			return false
		}
	}
	d := (int(have[5]) - int(want[5]) + 256) % 256
	return d <= 1
}

// HostIPTo4 packs a dotted-quad IPv4 to a uint32 (scan guard helper).
func HostIPTo4(ip net.IP) uint32 {
	v4 := ip.To4()
	if v4 == nil {
		return 0
	}
	return binary.BigEndian.Uint32(v4)
}

// ipNeighMAC reads the neighbour entry for ip via `ip neigh`, the same trusted
// source orb_bind uses (a flat bench LAN, not hostile-network safe).
func ipNeighMAC(ip string) (MAC, bool) {
	var mac MAC
	out, err := exec.Command("ip", "neigh", "show", ip).Output()
	if err != nil {
		return mac, false
	}
	fields := strings.Fields(string(out))
	for i, fl := range fields {
		if fl == "lladdr" && i+1 < len(fields) {
			if m, err := ParseMAC(fields[i+1]); err == nil {
				return m, true
			}
		}
	}
	return mac, false
}

// GateIP proves ip still belongs to the bound orb via ip neigh, refreshing a
// stale entry with one ping. It refuses when the answer is a different nic so
// the caller never benches the wrong orb.
func (t *Target) GateIP(ip string) (string, error) {
	if ip == "" {
		ip = t.IP
	}
	if ip == "" {
		return "", fmt.Errorf("orbbind: %s has no ORB_IP — provision it onto the LAN first", t.Label)
	}
	mac, ok := ipNeighMAC(ip)
	if !ok {
		_ = exec.Command("ping", "-c1", "-W1", ip).Run()
		mac, ok = ipNeighMAC(ip)
	}
	if !ok {
		return "", fmt.Errorf("orbbind: no ARP answer from %s (%s)", ip, t.Label)
	}
	if !ARPMatches(mac, t.BaseMAC) {
		return "", fmt.Errorf("orbbind: %s answers as %s but %s is %s — likely the OTHER orb", ip, mac, t.Label, t.BaseMAC)
	}
	return ip, nil
}

// StampAudit replicates serflash.sh's second guard. /api/v1/status's fw comes
// from esp_app_desc_t, which CMake stamps at CONFIGURE time and caches, so a
// stale image can carry an old version string even after source edits. The
// image's own truth is the 32-byte version field at app-image byte 48 (the
// esp_app_desc_t layout); compare it to `git describe --always --dirty` for
// the tree the build came from. A mismatch means the bin predates the working
// tree and flashing it would bench code that is not what you are looking at.
func StampAudit(appBin, repoDir string) (imageStamp, treeStamp string, err error) {
	raw, err := os.ReadFile(appBin)
	if err != nil {
		return "", "", fmt.Errorf("orbbind: read app image %s: %w", appBin, err)
	}
	if len(raw) < 80 {
		return "", "", fmt.Errorf("orbbind: %s too short to carry an app descriptor", appBin)
	}
	stamp := strings.TrimRight(string(raw[48:80]), "\x00")
	out, gerr := exec.Command("git", "-C", repoDir, "describe", "--always", "--dirty").Output()
	if gerr != nil {
		return stamp, "", fmt.Errorf("orbbind: git describe in %s: %w", repoDir, gerr)
	}
	want := strings.TrimSpace(string(out))
	if stamp != want {
		return stamp, want, fmt.Errorf("orbbind: image stamp [%s] does not match the tree [%s] — rebuild before flashing", stamp, want)
	}
	return stamp, want, nil
}
