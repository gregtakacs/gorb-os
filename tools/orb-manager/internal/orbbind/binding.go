// Package orbbind holds the one device-safety the manager keeps: an exclusive
// advisory lock per physical serial port, so two operations (a flash and a
// provision, or two tools) can never drive one cable at once and corrupt the
// write. It carries no orb identity and reads no bench state — the user owns
// the orb on the port they picked.
package orbbind

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// PortLock is an exclusive advisory lock on a PHYSICAL serial port, keyed to
// its canonical device path — not to any orb identity. A production user owns
// the orb plugged into the port they picked and must never be refused for it;
// this lock's only job is to stop two operations (a flash and a provision, or
// two tools) driving the same cable at once, which corrupts the write.
//
// The lock itself is per-OS (lockPort_unix.go, lockPort_windows.go) because
// nothing is portable about advisory locking. Both implementations release when
// the process dies, so a hub killed mid-flash never leaves a cable locked.
type PortLock struct {
	f    *os.File
	name string
}

// LockPort locks the port at path — a /dev/ttyACMx, a /dev/serial/by-id/...
// link, or a Windows COM port (COM5, \\.\COM5). Spellings of one device are
// resolved to the same canonical path first, so they lock together rather than
// each sneaking past the other. It returns an error only when another operation
// already holds that exact port.
func LockPort(path string) (*PortLock, error) {
	canon := canonicalPortPath(path)
	f, held, err := lockPort(portLockPath(canon))
	if err != nil {
		return nil, fmt.Errorf("orbbind: lock port %s: %w", canon, err)
	}
	if !held {
		return nil, fmt.Errorf("port %s is busy - another flash or provision holds its lock", canon)
	}
	return &PortLock{f: f, name: canon}, nil
}

// CanonicalPortPath exposes the port-spelling collapse to callers that must key
// something on the physical device rather than on the name the OS happened to hand
// out — the hub's operation registry keys on it for exactly the reason LockPort
// canonicalizes: /dev/ttyACM0 and its /dev/serial/by-id/... link are one cable, and
// they must collide rather than each sneak past the other.
func CanonicalPortPath(path string) string { return canonicalPortPath(path) }

// canonicalPortPath collapses the spellings of one device — most importantly a
// /dev/serial/by-id/... link and the /dev/ttyACMx it points at — onto a single
// string, so both claim the same lock.
func canonicalPortPath(path string) string {
	if r, err := filepath.EvalSymlinks(path); err == nil {
		path = r
	}
	// Windows names the same port COM5 and \\.\COM5; they must collide too.
	if len(path) > 4 && strings.EqualFold(path[:4], `\\.\`) {
		path = path[4:]
	}
	return path
}

// portLockPath names the lock file backing a port. It canonicalizes first, so it
// can never hand one device two lock files, which is the failure this whole
// package exists to prevent. Every character that is not safe in a filename on
// BOTH platforms becomes a dash: a COM port arrives as \\.\COM5, and backslashes
// and colons would otherwise be read as path separators or rejected outright by
// CreateFile.
func portLockPath(path string) string {
	canon := canonicalPortPath(path)
	safe := strings.TrimPrefix(canon, "/dev/")
	var b strings.Builder
	for _, r := range safe {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9',
			r == '.', r == '_', r == '-':
			b.WriteRune(r)
		default:
			b.WriteRune('-')
		}
	}
	return filepath.Join(os.TempDir(), "orb-port-"+b.String()+".lock")
}

// Release drops the port lock. Safe to call on a nil receiver.
func (l *PortLock) Release() {
	if l == nil || l.f == nil {
		return
	}
	unlockPort(l.f)
	l.f.Close()
}

// Name returns the canonical port path the lock is held on.
func (l *PortLock) Name() string {
	if l == nil {
		return ""
	}
	return l.name
}
