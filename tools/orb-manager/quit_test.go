package main

import (
	"context"
	"net/http"
	"testing"
	"time"
)

// The quit guard, and the bench failure it answers (2026-10-26): closing the hub
// while a plain PING was live printed "1 port operation(s) live (cmd:/dev/ttyACM0);
// waiting up to 5m0s" and sat there — almost always. The old guard sheltered every
// kind for the full flash budget, but a read or a round-trip interrupted at any byte
// cannot half-write a board, and its stall lives below the context (a serial open or
// close that ignores cancellation), so waiting out its budget waits on nothing. The
// guard now shelters only kinds that write flash, and steps over a wedged reader
// after a short grace. These drive quit itself, with the grace shrunk so the wedge
// costs milliseconds and the pin still measures what it guards.

func TestQuitCancelsALiveRead(t *testing.T) {
	s := newHub()
	job, ctx, err := s.ops.start(context.Background(), "/dev/ttyACM0", opCmd, cmdBudget)
	if err != nil {
		t.Fatal(err)
	}
	go func() { <-ctx.Done(); s.ops.finish(job) }() // well-behaved: leaves on the cancel

	s.quit(&http.Server{})

	if ctx.Err() == nil {
		t.Error("quit left the cmd uncancelled")
	}
	if _, ok := s.ops.busy("/dev/ttyACM0"); ok {
		t.Error("quit returned while the cancelled cmd still held its port")
	}
}

// TestQuitWalksOverWedgedRead is the reported failure, shrunk to test scale: an op
// that never finishes — cancel honoured or not — must not hold quit anywhere near
// opQuitBudget. With the grace at 250 ms, quit returns on the grace's clock.
func TestQuitWalksOverWedgedRead(t *testing.T) {
	old := quietQuitBudget
	quietQuitBudget = 250 * time.Millisecond
	defer func() { quietQuitBudget = old }()

	s := newHub()
	_, ctx, err := s.ops.start(context.Background(), "/dev/ttyACM0", opCmd, cmdBudget)
	if err != nil {
		t.Fatal(err)
	} // deliberately never finished: this is the wedge

	start := time.Now()
	s.quit(&http.Server{})
	if d := time.Since(start); d > 3*time.Second {
		t.Errorf("quit spent %v on one wedged read; the grace is %v and the old 5-minute shelter is gone", d, quietQuitBudget)
	}
	if ctx.Err() == nil {
		t.Error("quit stepped over the wedged read without even cancelling it")
	}
}

// TestQuitStillSheltersFlashWrites holds the other hand of the fix: a live writer is
// never cancelled by quit and never stepped over. The finisher below doubles as the
// tripwire — if quit ever cancels the flash's context, the test fails outright.
func TestQuitStillSheltersFlashWrites(t *testing.T) {
	old := quietQuitBudget
	quietQuitBudget = 100 * time.Millisecond
	defer func() { quietQuitBudget = old }()

	s := newHub()
	job, ctx, err := s.ops.start(context.Background(), "/dev/ttyACM1", opFlash, 0)
	if err != nil {
		t.Fatal(err)
	}
	landed := make(chan error, 1)
	go func() {
		select {
		case <-ctx.Done():
			landed <- ctx.Err() // quit cancelled a writer: the one thing it must not do
		case <-time.After(300 * time.Millisecond):
			s.ops.finish(job)
			landed <- nil
		}
	}()

	start := time.Now()
	s.quit(&http.Server{})
	if err := <-landed; err != nil {
		t.Fatalf("quit cancelled the live flash: %v", err)
	}
	if d := time.Since(start); d < 300*time.Millisecond {
		t.Errorf("quit returned after %v, before the 300ms flash write could land", d)
	}
}

// TestWritesFlashClassification pins the split itself, so a new op kind cannot
// silently join the sheltered list — or leave it — without failing here.
func TestWritesFlashClassification(t *testing.T) {
	writers := []opKind{opFlash, opLonely, opWipe, opProvision}
	readers := []opKind{opProbe, opCmd, opConsole, opTheme}
	for _, k := range writers {
		if !k.writesFlash() {
			t.Errorf("%s stopped writing flash: quit would now kill it mid-block", k)
		}
	}
	for _, k := range readers {
		if k.writesFlash() {
			t.Errorf("%s joined the sheltered list: one wedged %s would hold quit for the full flash budget again", k, k)
		}
	}
}
