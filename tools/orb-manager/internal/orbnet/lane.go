package orbnet

// Dial scheduler (the "orb bus"). It lives here, beside the transport that
// calls it, rather than in the hub, so no caller can forget it: the gate is
// wired into Client's DialContext, below every method, so a dial that does not
// want to be scheduled still is.

import (
	"context"
	"net"
	"sync"
	"time"
)

// Lane hands out the right to open one connection to one host at a time. The
// hub builds a single Lane and gives it to all four of its clients, so the
// status sweep, the proxy relay, a preview and a firmware push all contend on
// one grant per orb instead of each transport capping only itself.
//
// bulk says what kind of work is asking. A bulk dial is a long stream — a
// preview plane, an app image — that will hold the lane for seconds. A control
// dial is a short call — status, a scan, a name write. The Lane lets a control
// dial pass the bulk dials already queued behind the holder, so one slow
// transfer cannot make the ten-second liveness tick look like a dead board.
// It cannot preempt the dial already in flight: the orb's single httpd task is
// mid-response and cannot be interrupted from here, only queued behind.
type Lane interface {
	Acquire(ctx context.Context, host string, bulk bool) (release func(), err error)
}

// DialGate is the Lane the hub uses: a per-host ticket counter with a priority
// queue. Its zero value is not usable; build one with NewDialGate.
type DialGate struct {
	mu    sync.Mutex
	lanes map[string]*laneState
}

type laneState struct {
	held    bool
	seq     uint64
	waiters []*laneWaiter
}

type laneWaiter struct {
	bulk     bool
	seq      uint64
	ready    chan struct{} // closed when this ticket owns the lane
	canceled bool          // ctx died; release must skip it
	granted  bool          // released to it; a racy ctx-cancel must not strand it
}

// NewDialGate returns an empty gate. Lanes are created on first use for a host
// and dropped when they fall idle, so the map is bounded by the orbs in play,
// not by every address ever dialed.
func NewDialGate() *DialGate {
	return &DialGate{lanes: map[string]*laneState{}}
}

// state returns the lane for host, creating it. Caller holds g.mu.
func (g *DialGate) state(host string) *laneState {
	st := g.lanes[host]
	if st == nil {
		st = &laneState{}
		g.lanes[host] = st
	}
	return st
}

// Acquire takes the lane for host, or waits for it, or returns ctx.Err. The
// release it hands back frees the lane for the next ticket and is idempotent.
func (g *DialGate) Acquire(ctx context.Context, host string, bulk bool) (func(), error) {
	g.mu.Lock()
	st := g.state(host)
	// The lane is free and nothing is ahead: take it without queueing.
	if !st.held && len(st.waiters) == 0 {
		st.held = true
		g.mu.Unlock()
		return g.releaser(host), nil
	}
	// Queue. Control inserts ahead of the bulk block, FIFO within its class.
	st.seq++
	w := &laneWaiter{bulk: bulk, seq: st.seq, ready: make(chan struct{})}
	pos := len(st.waiters)
	if !bulk {
		for i, other := range st.waiters {
			if other.bulk {
				pos = i
				break
			}
		}
	}
	st.waiters = append(st.waiters, nil)
	copy(st.waiters[pos+1:], st.waiters[pos:])
	st.waiters[pos] = w
	g.mu.Unlock()

	select {
	case <-w.ready:
		return g.releaser(host), nil
	case <-ctx.Done():
		g.mu.Lock()
		if w.granted {
			// We were handed the lane the same instant ctx died. Take it and
			// release it now rather than strand the lane held with no owner.
			g.mu.Unlock()
			g.release(host)
			return nil, ctx.Err()
		}
		w.canceled = true
		g.dropWaiter(st, w)
		g.mu.Unlock()
		return nil, ctx.Err()
	}
}

// dropWaiter removes a canceled ticket that has not been granted. Caller holds
// g.mu and owns st.
func (g *DialGate) dropWaiter(st *laneState, w *laneWaiter) {
	for i, other := range st.waiters {
		if other == w {
			st.waiters = append(st.waiters[:i], st.waiters[i+1:]...)
			return
		}
	}
}

// release frees the lane, handing it to the highest-priority waiting ticket
// (control first, then FIFO) or marking it free when none remain.
func (g *DialGate) release(host string) {
	g.mu.Lock()
	defer g.mu.Unlock()
	st := g.lanes[host]
	if st == nil {
		return
	}
	for len(st.waiters) > 0 {
		w := st.waiters[0]
		st.waiters = st.waiters[1:]
		if w.canceled {
			continue
		}
		// Hand off without freeing: the lane never idles while a waiter waits,
		// so a burst of control dials cannot leapfrog the ticket already next.
		w.granted = true
		close(w.ready)
		return
	}
	st.held = false
	if len(st.waiters) == 0 {
		delete(g.lanes, host)
	}
}

func (g *DialGate) releaser(host string) func() {
	var once sync.Once
	return func() { once.Do(func() { g.release(host) }) }
}

// laneConn is a net.Conn that frees its lane when the transport closes it. With
// DisableKeepAlives the transport closes the connection once the response body
// is drained, so the lane is held for exactly one request and no longer.
type laneConn struct {
	net.Conn
	once    sync.Once
	release func()
}

func (c *laneConn) Close() error {
	err := c.Conn.Close()
	c.once.Do(c.release)
	return err
}

// gatedDialer returns a DialContext that takes a ticket on lane for host before
// connecting and ties its release to the connection's life. A nil lane dials
// straight through, which is what every test and the plain NewClient get.
func gatedDialer(lane Lane, bulk bool) func(context.Context, string, string) (net.Conn, error) {
	base := (&net.Dialer{Timeout: 800 * time.Millisecond}).DialContext
	if lane == nil {
		return base
	}
	return func(ctx context.Context, network, addr string) (net.Conn, error) {
		release, err := lane.Acquire(ctx, addr, bulk)
		if err != nil {
			return nil, err
		}
		conn, derr := base(ctx, network, addr)
		if derr != nil {
			release()
			return nil, derr
		}
		return &laneConn{Conn: conn, release: release}, nil
	}
}
