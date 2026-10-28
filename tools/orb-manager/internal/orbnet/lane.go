package orbnet

// Dial scheduler (the "orb bus"). It lives here, beside the transport that
// calls it, rather than in the hub, so no caller can forget it: the gate is
// wired into Client's DialContext, below every method, so a dial that does not
// want to be scheduled still is.

import (
	"context"
	"errors"
	"io"
	"net"
	"sync"
	"time"
)

// ErrLaneBusy is what a control dial gets when the orb's lane is held by a hub
// transfer and does not free within the control queue budget. It is the
// opposite of a verdict: the hub itself has an in-flight request to that orb,
// so the orb answered the door minutes ago at worst. Every consumer must treat
// it as neutral — never a silence verdict, never grounds for a reset pulse,
// and for the UI a fast 503 that names the cause instead of an 8-second
// timeout that names nothing. This error is what makes the bus honest: waiting
// is reported as waiting, not as death.
var ErrLaneBusy = errors.New("orbnet: orb busy: another transfer is in flight")

// ErrLaneCut classifies the error a page interrupt leaves behind: the hub
// closed the socket of its own background dial so a page read could take the
// lane (DialGate.Interrupt). It is never a card's silence and never grounds
// for a strike, a debounce stamp, or a retry: the page that cut it owns the
// lane now. It matches the family a closed or torn connection leaves in the
// transport — use-of-closed, unexpected EOF, cancelled context, and any
// net.Error timeout, which for a background dial is the same insurance spent.
var ErrLaneCut = errors.New("orbnet: dial cut by the hub for a page read")

// LaneCut classifies an error from a background dial as "this dial was cut or
// died like one", which is the answer's whole meaning for the caller: do not
// retry, do not strike, the page won.
func LaneCut(err error) bool {
	if err == nil {
		return false
	}
	if errors.Is(err, ErrLaneCut) || errors.Is(err, net.ErrClosed) ||
		errors.Is(err, io.ErrUnexpectedEOF) || errors.Is(err, context.Canceled) {
		return true
	}
	var ne net.Error
	return errors.As(err, &ne) && ne.Timeout()
}

// controlQueueWait is how long a control dial waits for a busy lane before it
// reports ErrLaneBusy instead. 1.5 s sits under a healthy preview's ~2.3 s
// hold on purpose: a status poll queued behind a WORKING stream usually waits
// out a slice of it and still gets served (the lane passes to it when the
// stream closes), and a poll queued behind a STALLED stream finds out in 1.5 s
// that it is stuck behind something, not in 8 s when its whole budget has
// burned — at which point the failure was indistinguishable from a dead orb.
const controlQueueWait = 1500 * time.Millisecond

// Class says what kind of work is asking for the lane.
//
// A control dial is a short call — status, a scan, a name write. A bulk dial is
// a long stream — a preview plane, an app image, an OTA push — that will hold
// the lane for seconds. A background dial is the hub's OWN speculative work:
// the read-ahead pass, the card walk behind a warm kick. The three differ in
// what they owe the others. The Lane lets a control dial pass the bulk dials
// already queued behind the holder, so one slow transfer cannot make the
// ten-second liveness tick look like a dead board. Background work owes more:
// it is insurance, and a page that wants the lane takes it FROM a background
// holder — the gate interrupts the in-flight dial by closing its socket (see
// DialGate.Interrupt), which is the only preemption the orb's single-task httpd
// allows from this side. It cannot interrupt a page's own dial: a page's stream
// that gets cut is a page that failed for no reason, so those holders are left
// whole and the fast queue decline stands for them.
type Class int

const (
	ClassControl Class = iota
	ClassBulk
	ClassBackground
)

// Lane hands out the right to open one connection to one host at a time. The
// hub builds a single Lane and gives it to all its clients, so the status
// sweep, the proxy relay, a preview and a firmware push all contend on one
// grant per orb instead of each transport capping only itself.
type Lane interface {
	Acquire(ctx context.Context, host string, class Class) (release func(), err error)
}

// DialGate is the Lane the hub uses: a per-host ticket counter with a priority
// queue. Its zero value is not usable; build one with NewDialGate.
type DialGate struct {
	mu sync.Mutex
	// controlWait is the control queue budget; see controlQueueWait. It is a
	// field only so tests can compress time; SetControlWait is the writer and
	// zero means the default.
	controlWait time.Duration
	lanes       map[string]*laneState
	// abortedAt files the last Interrupt per host, so a caller that just cut a
	// background dial can ask, after its own fetch returns, whether the failure
	// it is holding was the cut it paid for rather than the card refusing to
	// answer. It is a stamp, not a queue: the hub has one worker per host here,
	// and one timestamp cannot misattribute between two of them.
	abortedAt map[string]time.Time
}

// CutSince answers whether Interrupt closed a background socket at host after
// the moment the caller names. The pass's strike logic reads it: a dial the hub
// cut for a page is not a card that failed, and pricing it as one would retire
// the read-ahead over the page's own success.
func (g *DialGate) CutSince(host string, since time.Time) bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	t, ok := g.abortedAt[host]
	return ok && t.After(since)
}

// Interrupt closes the socket of an in-flight BACKGROUND holder so the page
// dial that just asked for the lane can go. Closing the hub's own side of the
// connection ends the hub's request immediately and the device's httpd task
// finishes its abandoned write into a dead socket and returns to listening —
// which is the whole difference between background work (interruptible, its
// answer is refetchable) and page work (not). It returns whether a holder was
// cut, so a caller can price what it just bought. host is the host:port the
// dial was addressed to, exactly as Acquire takes it.
func (g *DialGate) Interrupt(host string) bool {
	g.mu.Lock()
	st := g.lanes[host]
	var cut bool
	if st != nil && st.held && st.bg && st.conn != nil {
		st.conn.Close() // the release path runs through the closed conn
		if g.abortedAt == nil {
			g.abortedAt = map[string]time.Time{}
		}
		g.abortedAt[host] = time.Now()
		cut = true
	}
	g.mu.Unlock()
	return cut
}

// SetControlWait overrides the control queue budget. Call it before the gate
// serves traffic; it exists for tests, which cannot sleep 1.5 s per case.
func (g *DialGate) SetControlWait(d time.Duration) {
	g.mu.Lock()
	g.controlWait = d
	g.mu.Unlock()
}

type laneState struct {
	held    bool
	seq     uint64
	waiters []*laneWaiter
	// The current holder, so Interrupt can name it and close it. conn is
	// filed by the gated dialer through noteConn the instant the socket opens;
	// bg says whether cutting it is allowed.
	bg   bool
	conn net.Conn
	// freedAt is the last moment this lane went idle — no holder, no waiters.
	// IdleSince answers its quiet window.
	freedAt time.Time
}

type laneWaiter struct {
	class    Class
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
func (g *DialGate) Acquire(ctx context.Context, host string, class Class) (func(), error) {
	g.mu.Lock()
	st := g.state(host)
	// The lane is free and nothing is ahead: take it without queueing.
	if !st.held && len(st.waiters) == 0 {
		st.held = true
		st.bg = class == ClassBackground
		g.mu.Unlock()
		return g.releaser(host), nil
	}
	// (holder identity is stamped at every grant, below and in release.)
	// Queue. Control and background insert ahead of the bulk block, FIFO
	// within a class, control ahead of background. The class belongs to the
	// TICKET; whether the lane may be interrupted belongs to the HOLDER and
	// gets stamped at the grant, below.
	st.seq++
	w := &laneWaiter{class: class, seq: st.seq, ready: make(chan struct{})}
	pos := len(st.waiters)
	if class != ClassBulk {
		for i, other := range st.waiters {
			if other.class == ClassBulk || (class == ClassControl && other.class == ClassBackground) {
				pos = i
				break
			}
		}
	}
	st.waiters = append(st.waiters, nil)
	copy(st.waiters[pos+1:], st.waiters[pos:])
	st.waiters[pos] = w
	queueBudget := g.controlWait
	if queueBudget == 0 {
		queueBudget = controlQueueWait
	}
	g.mu.Unlock()

	// A control dial waits for the lane only as long as its queue budget; past
	// that it reports ErrLaneBusy rather than burn its caller's whole timeout
	// sitting in a queue — a queue stall that dies at the caller's deadline is
	// indistinguishable from an orb that never answered. A bulk dial waits its
	// full ctx: a preview or a push that eventually gets the lane still works,
	// and its own client timeout already bounds the wait.
	// The fast decline is priced for a lane a PAGE dial holds: the holder is
	// someone's answer, it will not be cut from here, and a wait that dies at
	// the caller's deadline is the silence lie ErrLaneBusy exists to prevent.
	// A queued ticket behind a BACKGROUND holder uses the SAME budget, and that
	// is deliberate: the page dial that outranks the holder does not wait in
	// this queue at all — the hub interrupts the holder first (see Interrupt),
	// so by the time a page ticket queues, the only thing left to wait behind
	// is other page work. A background ticket never spends a busy report: it
	// yields to everything and waits its own ctx.
	var overBudget <-chan time.Time
	switch class {
	case ClassControl:
		timer := time.NewTimer(queueBudget)
		defer timer.Stop()
		overBudget = timer.C
	case ClassBackground:
		// waits its full ctx like bulk; its client timeout is the bound
	}

	select {
	case <-w.ready:
		return g.releaser(host), nil
	case <-overBudget:
		g.mu.Lock()
		if w.granted {
			// Granted the same instant the budget ran out: the lane is ours.
			// Take it — we are through the queue, so the busy report is moot.
			g.mu.Unlock()
			return g.releaser(host), nil
		}
		w.canceled = true
		g.dropWaiter(st, w)
		g.mu.Unlock()
		return nil, ErrLaneBusy
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
		// The grant stamps the new holder's class: from this instant
		// Interrupt may close this ticket's socket once the dialer files it.
		st.bg = w.class == ClassBackground
		w.granted = true
		close(w.ready)
		return
	}
	st.held = false
	st.bg = false
	st.conn = nil
	st.freedAt = time.Now()
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

// noteConn files a background holder's socket so Interrupt can close it. It
// runs only while the ticket still owns the lane and nothing is filed: a
// socket that arrives after its own release, or onto a lane already noted,
// is closed rather than registered, because closing it would then cut a dial
// the gate does not own.
func (g *DialGate) noteConn(host string, c net.Conn) {
	g.mu.Lock()
	ok := false
	if st := g.lanes[host]; st != nil && st.held && st.bg && st.conn == nil {
		st.conn = c
		ok = true
	}
	g.mu.Unlock()
	if !ok {
		c.Close()
	}
}

// IdleSince answers the pass's yield question for one host: has the lane held
// no one and waited for no one for at least d? A lane with a page ticket in it
// is not idle, and a lane released a heartbeat ago may re-dial at any instant,
// so the window is the answer's whole meaning: "quiet long enough that the next
// dial is unlikely to meet a page at the door."
func (g *DialGate) IdleSince(host string, d time.Duration) bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	st := g.lanes[host]
	if st == nil {
		return true // a host the gate has never scheduled has never been busy
	}
	if st.held || len(st.waiters) > 0 {
		return false
	}
	return time.Since(st.freedAt) >= d
}

// gatedDialer returns a DialContext that takes a ticket on lane for host before
// connecting and ties its release to the connection's life. A nil lane dials
// straight through, which is what every test and the plain NewClient get.
// Background tickets file their socket with the gate, which is what makes them
// interruptible; every other class holds its connection whole until it closes
// its own response, because cutting a page's or an OTA's dial mid-stream is a
// failure this side caused and no one asked for.
func gatedDialer(lane Lane, class Class) func(context.Context, string, string) (net.Conn, error) {
	base := (&net.Dialer{Timeout: 800 * time.Millisecond}).DialContext
	if lane == nil {
		return base
	}
	return func(ctx context.Context, network, addr string) (net.Conn, error) {
		release, err := lane.Acquire(ctx, addr, class)
		if err != nil {
			return nil, err
		}
		conn, derr := base(ctx, network, addr)
		if derr != nil {
			release()
			return nil, derr
		}
		if g, ok := lane.(*DialGate); ok && class == ClassBackground {
			g.noteConn(addr, conn)
		}
		return &laneConn{Conn: conn, release: release}, nil
	}
}
