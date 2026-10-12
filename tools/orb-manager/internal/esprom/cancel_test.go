package esprom

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"testing"
	"time"
)

// Cancellation is the reason a hub can be closed without leaving an orb half
// written. The contract these tests pin is specific: after a cancel, NOTHING goes
// back out on the wire, and what already went out stopped at a block boundary
// rather than mid-block.

// A cancellation that arrives before the first write must put nothing on the wire
// at all. This is the case a Ctrl-C during an earlier image hits.
func TestCancelledSessionWritesNothing(t *testing.T) {
	f := newFake(t)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	d := New(f, nil).WithContext(ctx)

	err := d.WriteImage(0x20000, bytes.Repeat([]byte{0x33}, 3*1024), nil)
	if !IsCancelled(err) {
		t.Fatalf("err = %v, want ErrCancelled", err)
	}
	if len(f.beginBodies) != 0 || len(f.seenData) != 0 {
		t.Errorf("a cancelled device wrote to the wire: %d FLASH_BEGIN, %d FLASH_DATA",
			len(f.beginBodies), len(f.seenData))
	}
}

// A cancellation that arrives mid-image must stop at the NEXT block boundary: the
// block in flight completes, because the ROM takes a whole block or rejects it, and
// no further block is ever sent.
func TestCancelStopsAtBlockBoundary(t *testing.T) {
	f := newFake(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	d := New(f, nil).WithContext(ctx)

	data := bytes.Repeat([]byte{0x33}, 8*1024)
	calls := 0
	err := d.WriteImage(0x20000, data, func(written, total int) {
		calls++
		if calls == 2 {
			cancel() // the operator hits Cancel after the second block lands
		}
	})
	if !IsCancelled(err) {
		t.Fatalf("err = %v, want ErrCancelled", err)
	}
	if len(f.seenData) != 2 {
		t.Errorf("%d FLASH_DATA blocks reached the wire after cancel at 2; only a boundary stop is safe", len(f.seenData))
	}
}

// A read against a chip that has gone silent must still observe the cancel: the
// deadline is minutes out, so without the ctx check the hub would sit there until
// the ROM timeout expired.
func TestCancelInterruptsAWaitingRead(t *testing.T) {
	f := newFake(t) // Read always returns 0,nil: a chip that answers nothing
	ctx, cancel := context.WithCancel(context.Background())
	d := New(f, nil).WithContext(ctx)
	go func() {
		time.Sleep(50 * time.Millisecond)
		cancel()
	}()

	start := time.Now()
	_, err := d.readPacket(time.Now().Add(10 * time.Minute))
	if !errors.Is(err, ErrCancelled) {
		t.Fatalf("err = %v, want ErrCancelled", err)
	}
	if d := time.Since(start); d > 3*time.Second {
		t.Errorf("cancel took %v to land; the read loop must observe it promptly", d)
	}
}

// The sync retry loop's own sleeps are cancellable too, or a hub that has been told
// to quit still holds the port through every retry.
func TestSessionHonoursCancelBeforeSyncRetrySleep(t *testing.T) {
	pt := &Port{} // never used: Session must stop at its first check
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	err := Session(pt, Options{Ctx: ctx, Plan: FlashPlan{}})
	if !IsCancelled(err) {
		t.Fatalf("err = %v, want ErrCancelled from the entry check", err)
	}
}

// IsCancelled must see through the wrapping the session layers on top of a bare
// block write, so the UI can tell "you stopped it" from "the orb never answered".
func TestIsCancelledSeesThroughWrapping(t *testing.T) {
	if !IsCancelled(errors.Join(ErrCancelled, errors.New("other"))) {
		t.Error("IsCancelled missed a joined cancellation")
	}
	if !IsCancelled(fmt.Errorf("flash app @0x20000: %w", ErrCancelled)) {
		t.Error("IsCancelled missed a wrapped cancellation")
	}
	if IsCancelled(ErrTimeout) {
		t.Error("a timeout must never be reported as a cancellation")
	}
}
