package orbbind

import (
	"os"
	"path/filepath"
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
		t.Fatal(err)
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
