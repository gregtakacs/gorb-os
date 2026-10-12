package orbbind

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestLockPortMutualExclusion is the one safety the production tool keeps: no
// two operations drive the same port at once. A held lock must refuse a second
// claim, and releasing must hand it to the next claimer.
func TestLockPortMutualExclusion(t *testing.T) {
	port := filepath.Join(t.TempDir(), "ttyACM-excl")

	first, err := LockPort(port)
	if err != nil {
		t.Fatalf("first LockPort: %v", err)
	}
	if _, err := LockPort(port); err == nil {
		first.Release()
		t.Fatal("second LockPort on a held port must fail")
	}

	first.Release()
	second, err := LockPort(port)
	if err != nil {
		t.Fatalf("re-LockPort after release: %v", err)
	}
	second.Release()
}

// TestLockPortByIDAndTTY proves a port and its by-id symlink lock together:
// both spellings must resolve to the one canonical device, or a flash on the
// tty could run while a provision thinks the by-id path is free.
func TestLockPortByIDAndTTY(t *testing.T) {
	dir := t.TempDir()
	real := filepath.Join(dir, "ttyACM-canonical")
	if err := os.WriteFile(real, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	byid := filepath.Join(dir, "by-id-link")
	if err := os.Symlink(real, byid); err != nil {
		// Windows makes symlink creation a privileged, developer-mode operation.
		// The collision this test cares about is the canonical path, which
		// TestLockPortDevicePathSpellingsCollide pins without needing a link.
		t.Skipf("no symlink permission on this host: %v", err)
	}

	held, err := LockPort(byid)
	if err != nil {
		t.Fatalf("LockPort(by-id): %v", err)
	}
	defer held.Release()

	if _, err := LockPort(real); err == nil {
		t.Fatal("the tty spelling must collide with the by-id lock on the same device")
	}
}

// A Windows COM port arrives as COM5 or \\.\COM5, and the lock file name is
// built from that string. Neither spelling may smuggle a path separator or a
// colon into the filename — CreateFile rejects those outright, so every Windows
// flash would fail to take its lock — and the two spellings must be one lock.
func TestLockPortDevicePathSpellingsCollide(t *testing.T) {
	if got := canonicalPortPath(`\\.\COM5`); got != "COM5" {
		t.Errorf(`canonicalPortPath of \\.\COM5 = %q, want COM5`, got)
	}
	if a, b := portLockPath(`COM5`), portLockPath(`\\.\COM5`); a != b {
		t.Errorf("the two COM spellings lock different files: %q vs %q", a, b)
	}
	if a, b := portLockPath("/dev/ttyACM0"), portLockPath(`C:\temp\orb`); a == b {
		t.Errorf("different devices must not share a lock file: both name %q", a)
	}

	held, err := LockPort(`\\.\COM7`)
	if err != nil {
		t.Fatalf("LockPort of the device-namespace COM7: %v", err)
	}
	defer held.Release()
	if _, err := LockPort("COM7"); err == nil {
		t.Fatal(`COM7 must collide with the \\.\COM7 lock`)
	}
}

// The sanitizer's job is that the lock filename holds only characters both
// platforms accept in a name.
func TestPortLockNameIsPortable(t *testing.T) {
	for _, canon := range []string{
		`\\.\COM12`, `COM12`, `/dev/ttyACM0`, `/dev/serial/by-id/usb-Espressif_44:BD:8D`,
	} {
		base := filepath.Base(portLockPath(canon))
		if strings.ContainsAny(base, `/\:`) {
			t.Errorf("lock file %q for %q holds a character Windows rejects in a name", base, canon)
		}
		if !strings.HasPrefix(base, "orb-port-") || !strings.HasSuffix(base, ".lock") {
			t.Errorf("lock file %q for %q is not recognisably ours", base, canon)
		}
	}
}
