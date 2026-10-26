package orbnet

import (
	"context"
	"errors"
	"net/http"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// The dial scheduler's contract, pinned without a network so the ordering and
// cancellation races are forced rather than hoped for. The e2e test at the bottom
// then proves the wiring actually serializes two clients over one gate.

// acquireUntil blocks until the gate hands out host's lane or ctx expires.
func TestDialGateSerializesPerHost(t *testing.T) {
	g := NewDialGate()
	var inflight, maxInflight int32
	var wg sync.WaitGroup
	for i := 0; i < 12; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			release, err := g.Acquire(context.Background(), "10.0.0.1:80", false)
			if err != nil {
				t.Errorf("Acquire: %v", err)
				return
			}
			defer release()
			n := atomic.AddInt32(&inflight, 1)
			for {
				m := atomic.LoadInt32(&maxInflight)
				if n <= m || atomic.CompareAndSwapInt32(&maxInflight, m, n) {
					break
				}
			}
			time.Sleep(3 * time.Millisecond)
			atomic.AddInt32(&inflight, -1)
		}()
	}
	wg.Wait()
	if maxInflight > 1 {
		t.Fatalf("gate let %d dials to one host at once; wanted 1", maxInflight)
	}
}

func TestDialGateControlPassesQueuedBulk(t *testing.T) {
	g := NewDialGate()
	const host = "10.0.0.2:80"
	hold, err := g.Acquire(context.Background(), host, false)
	if err != nil {
		t.Fatal(err)
	}
	order := make(chan string, 8)

	// Queue one bulk holder's-wait first, then a control, while host is held.
	go func() {
		r, _ := g.Acquire(context.Background(), host, true)
		order <- "bulk"
		time.Sleep(2 * time.Millisecond)
		r()
	}()
	time.Sleep(20 * time.Millisecond) // let the bulk ticket enqueue
	go func() {
		r, _ := g.Acquire(context.Background(), host, false)
		order <- "control"
		time.Sleep(2 * time.Millisecond)
		r()
	}()
	time.Sleep(20 * time.Millisecond) // let the control ticket enqueue

	hold() // release the holder; the control ticket must win over the bulk one
	first := <-order
	if first != "control" {
		t.Fatalf("after the holder released, %s got the lane first; control must pass bulk", first)
	}
}

func TestDialGateCancelFreesQueue(t *testing.T) {
	g := NewDialGate()
	const host = "10.0.0.3:80"
	hold, err := g.Acquire(context.Background(), host, false)
	if err != nil {
		t.Fatal(err)
	}
	// A queued control dial that is canceled must return the cancellation and
	// must NOT strand the lane: the next asker still gets served after release.
	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		time.Sleep(15 * time.Millisecond)
		cancel()
	}()
	if _, err := g.Acquire(ctx, host, false); !errors.Is(err, context.Canceled) {
		t.Fatalf("canceled queue wait returned %v, want context.Canceled", err)
	}
	hold() // release the original holder
	done := make(chan error, 1)
	go func() {
		r, err := g.Acquire(context.Background(), host, false)
		if err == nil {
			r()
		}
		done <- err
	}()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("lane stranded after a canceled waiter: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("lane never came free after a canceled waiter; queue stranded")
	}
}

func TestDialGateHostsAreIndependent(t *testing.T) {
	g := NewDialGate()
	a, err := g.Acquire(context.Background(), "10.0.0.4:80", false)
	if err != nil {
		t.Fatal(err)
	}
	defer a()
	// A different orb must not wait on the first one's lane.
	b, err := g.Acquire(context.Background(), "10.0.0.5:80", false)
	if err != nil {
		t.Fatalf("a second host was blocked by the first: %v", err)
	}
	b()
}

func TestDialGateReleaseIsIdempotent(t *testing.T) {
	g := NewDialGate()
	const host = "10.0.0.6:80"
	r, err := g.Acquire(context.Background(), host, false)
	if err != nil {
		t.Fatal(err)
	}
	r()
	r() // a double release must not over-promote into a two-holder lane
	n, err := g.Acquire(context.Background(), host, false)
	if err != nil {
		t.Fatalf("lane unusable after a double release: %v", err)
	}
	n()
}

// TestGatedClientsShareOneLane is the point of B over A: A caps each transport to
// one connection, but the hub has four, so one orb could still meet four at once.
// Two GATED clients — one control, one bulk, the same shape as the hub's status and
// preview clients — sharing one DialGate must show a single in-flight request even
// when both hammer the same server. Under A alone this would reach two.
func TestGatedClientsShareOneLane(t *testing.T) {
	var inflight, maxInflight int32
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		n := atomic.AddInt32(&inflight, 1)
		defer atomic.AddInt32(&inflight, -1)
		for {
			m := atomic.LoadInt32(&maxInflight)
			if n <= m || atomic.CompareAndSwapInt32(&maxInflight, m, n) {
				break
			}
		}
		time.Sleep(10 * time.Millisecond) // long enough that concurrent dials overlap
		_, _ = w.Write([]byte(`{}`))
	})
	host := fakeIP(srv)

	gate := NewDialGate()
	control := NewGatedClient(2*time.Second, gate, false)
	bulk := NewGatedClient(2*time.Second, gate, true)

	var wg sync.WaitGroup
	for i := 0; i < 6; i++ {
		wg.Add(2)
		go func() { defer wg.Done(); _, _ = control.ThemeList(host) }()
		go func() { defer wg.Done(); _, _ = bulk.ThemeList(host) }()
	}
	wg.Wait()

	if maxInflight != 1 {
		t.Fatalf("one gate, one host: saw %d concurrent requests, wanted exactly 1 "+
			"(the two clients did not share the lane)", maxInflight)
	}
}
