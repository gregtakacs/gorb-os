package orbbind

import (
	"context"
	"errors"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// These tests pin the cross-process protocol in press.go. One process is enough to
// exercise it: flock excludes two file descriptions of one file even inside a single
// process, so a second claim here stands for a second hub.

// One background hub may sweep a cable. A second must decline the round outright —
// two probes resetting one board at once is how an identify report goes false.
func TestSweepLeaseIsOneHubPerPort(t *testing.T) {
	port := filepath.Join(t.TempDir(), "ttyACM-lease")

	first, err := SweepPort(context.Background(), port)
	if err != nil {
		t.Fatalf("first SweepPort: %v", err)
	}
	second, err := SweepPort(context.Background(), port)
	if err == nil {
		second.Release()
		first.Release()
		t.Fatal("a second sweep must decline a cable another hub is sweeping")
	}
	if !strings.Contains(err.Error(), "sweep lease") {
		t.Errorf("the decline must name the lease it lost, got: %v", err)
	}

	first.Release()
	third, err := SweepPort(context.Background(), port)
	if err != nil {
		t.Fatalf("SweepPort after the lease released: %v", err)
	}
	third.Release()
}

// A press is the one signal that means a human is using this cable. Every sweep, in
// every process, owes it the port — and gets its cable back the moment it lets go.
func TestPressFlagStopsSweeps(t *testing.T) {
	port := filepath.Join(t.TempDir(), "ttyACM-press")

	lock, err := PressPort(context.Background(), port, 2*time.Second)
	if err != nil {
		t.Fatalf("PressPort: %v", err)
	}
	_, err = SweepPort(context.Background(), port)
	if err == nil {
		lock.Release()
		t.Fatal("a sweep must decline a port an operator press holds")
	}
	if !strings.Contains(err.Error(), "operator write") {
		t.Errorf("the decline must say why, got: %v", err)
	}
	// The flock is still the real guard, so a caller that knows nothing of the press
	// flag (a theme store pass, an older hub) collides on it as it always did.
	if _, err := LockPort(port); err == nil {
		lock.Release()
		t.Fatal("a press must still hold the port flock against a plain claim")
	}

	lock.Release()
	got, err := SweepPort(context.Background(), port)
	if err != nil {
		t.Fatalf("the sweep must return once the press releases: %v", err)
	}
	got.Release()
}

// The bench failure this whole file exists for: a press refused because another
// hub's background round happened to be in flight. The round is bounded, so the
// press waits it out instead of blaming the operator.
func TestPressWaitsOutAnInFlightRound(t *testing.T) {
	port := filepath.Join(t.TempDir(), "ttyACM-inflight")

	round, err := SweepPort(context.Background(), port)
	if err != nil {
		t.Fatalf("SweepPort: %v", err)
	}
	go func() {
		time.Sleep(150 * time.Millisecond)
		round.Release()
	}()

	asked := time.Now()
	lock, err := PressPort(context.Background(), port, 3*time.Second)
	if err != nil {
		t.Fatalf("a press must wait out one background round, got: %v", err)
	}
	waited := time.Since(asked)
	if waited < 100*time.Millisecond {
		t.Errorf("press returned in %v — it did not actually wait for the round", waited)
	}
	if waited > 2*time.Second {
		t.Errorf("press waited %v for one round; the window is the ceiling, not the answer", waited)
	}
	lock.Release()
}

// Presses never queue behind each other. A second writer on one wire corrupts the
// write, and the refusal the operator reads must arrive at once and name the truth.
func TestPressDoesNotQueueBehindAPress(t *testing.T) {
	port := filepath.Join(t.TempDir(), "ttyACM-twopress")

	first, err := PressPort(context.Background(), port, 2*time.Second)
	if err != nil {
		t.Fatalf("first PressPort: %v", err)
	}
	defer first.Release()

	asked := time.Now()
	_, err = PressPort(context.Background(), port, 8*time.Second)
	if err == nil {
		t.Fatal("a second press on a held port must be refused")
	}
	if waited := time.Since(asked); waited > time.Second {
		t.Errorf("a second press refused after %v; it must refuse at once, not queue", waited)
	}
	if !strings.Contains(err.Error(), "another flash or provision holds its lock") {
		t.Errorf("press-vs-press must keep the wording the operator knows, got: %v", err)
	}
}

// A press that gives up must not leave its flag posted, or a cancelled browser would
// mute every sweep on that cable until the hub restarted.
func TestPressCancelDropsItsFlag(t *testing.T) {
	port := filepath.Join(t.TempDir(), "ttyACM-cancel")

	round, err := SweepPort(context.Background(), port)
	if err != nil {
		t.Fatalf("SweepPort: %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		time.Sleep(120 * time.Millisecond)
		cancel()
	}()

	_, err = PressPort(ctx, port, 10*time.Second)
	if !errors.Is(err, context.Canceled) {
		round.Release()
		t.Fatalf("a cancelled wait must report the cancellation, got: %v", err)
	}

	round.Release()
	got, err := SweepPort(context.Background(), port)
	if err != nil {
		t.Fatalf("an abandoned press must not mute the sweep: %v", err)
	}
	got.Release()
}

// The two flag files are derived from the lock name, so they must stay as portable as
// the lock itself and must never share a file with it or each other.
func TestFlagPathsArePortableAndDistinct(t *testing.T) {
	for _, canon := range []string{
		`\\.\COM12`, `COM12`, `/dev/ttyACM0`, `/dev/serial/by-id/usb-Espressif_44:BD:8D`,
	} {
		press, lease, lock := flagPath(canon, ".press"), flagPath(canon, ".sweep"), portLockPath(canon)
		for _, f := range []string{press, lease} {
			base := filepath.Base(f)
			if strings.ContainsAny(base, `/\:`) {
				t.Errorf("flag file %q for %q holds a character Windows rejects in a name", base, canon)
			}
			if !strings.HasPrefix(base, "orb-port-") {
				t.Errorf("flag file %q for %q is not recognisably ours", base, canon)
			}
		}
		if press == lease || press == lock || lease == lock {
			t.Errorf("one cable got the same file twice: lock=%q press=%q sweep=%q", lock, press, lease)
		}
	}
}
