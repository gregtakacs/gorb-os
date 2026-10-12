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
	"syscall"
)

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
