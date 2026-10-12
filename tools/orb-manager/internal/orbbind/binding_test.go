package orbbind

import (
	"os"
	"path/filepath"
	"testing"
)

// TestMatchesSerial covers the by-id<->tty path canonicalization the UI's
// "(bound orb)" tag relies on. .orb-target stores a /dev/serial/by-id/... path,
// but the port dropdown enumerates /dev/ttyACMx, so the two spellings for the
// SAME board must resolve equal and a different board must not. (It is no longer
// a flash/provision gate — those act on any selected port — but LockPort leans
// on the same canonicalization, so the equality logic still earns its keep.)
func TestMatchesSerial(t *testing.T) {
	dir := t.TempDir()

	// Two distinct "ttys" and their by-id symlinks, as udev lays them out.
	ttyA := filepath.Join(dir, "ttyACM0")
	ttyB := filepath.Join(dir, "ttyACM1")
	for _, f := range []string{ttyA, ttyB} {
		if err := os.WriteFile(f, nil, 0o600); err != nil {
			t.Fatalf("mk %s: %v", f, err)
		}
	}
	byidA := filepath.Join(dir, "by-id-orb-A")
	byidB := filepath.Join(dir, "by-id-orb-B")
	for l, target := range map[string]string{byidA: ttyA, byidB: ttyB} {
		if err := os.Symlink(target, l); err != nil {
			t.Fatalf("link %s->%s: %v", l, target, err)
		}
	}

	bound := &Target{Label: "orb-A", Serial: byidA} // .orb-target holds by-id

	cases := []struct {
		name   string
		serial *Target
		port   string
		want   bool
	}{
		{"by-id target, tty dropdown (the reported bug)", bound, ttyA, true},
		{"by-id target, by-id dropdown", bound, byidA, true},
		{"by-id target, the OTHER orb's tty", bound, ttyB, false},
		{"by-id target, the OTHER orb's by-id", bound, byidB, false},
		{"tty target, by-id dropdown", &Target{Serial: ttyA}, byidA, true},
		{"exact-string match with no symlink", &Target{Serial: "/dev/ttyACM9"}, "/dev/ttyACM9", true},
		{"empty port never matches", bound, "", false},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if got := c.serial.MatchesSerial(c.port); got != c.want {
				t.Fatalf("MatchesSerial(%q)=%v want %v", c.port, got, c.want)
			}
		})
	}

}

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
