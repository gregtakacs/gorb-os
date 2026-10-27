package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"sort"
	"sync"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
)

// The per-port operation registry. It replaces the process-wide `flashing` bool
// the hub started with, and it exists for three reasons that flag could not serve:
//
//   - One operator with two cables. A process-wide flag refused a second flash
//     while an unrelated port was busy, which is the shared-bench discipline this
//     tool deliberately dropped.
//   - Cancellation. Each operation owns a cancellable context the flash honours,
//     so Cancel and a window close reach the write loop instead of merely being
//     noticed after it.
//   - Shutdown. Quitting with an operation live is how a half-written flash
//     happens, so the window-close path asks this registry before it exits.
//
// The lock in internal/orbbind remains the cross-process guarantee. This is the
// in-process one, and it is what carries the identity, the state string, and the
// cancel handle the UI shows.

// opKind names what holds a port.
type opKind string

const (
	opFlash opKind = "flash"
	// opWipe is the whole-chip erase. It holds the port exactly like a flash:
	// anything sharing the wire during a 16 MB erase corrupts it, and the
	// operation needs its own cancel handle and its own line in /api/ops so the
	// UI can say which cable is being emptied.
	opWipe opKind = "wipe"
	// opLonely is the lonely-image write. It is a flash wearing a different
	// shirt — same port claim, same cancel path, same ROM conversation — and it
	// earns its own kind because /api/ops should say what is ON the wire, not
	// merely that something is.
	opLonely    opKind = "lonely"
	opProvision opKind = "provision"
	opProbe     opKind = "identify"
	// opConsole is a read, but it still HOLDS the port, so it claims the same slot a
	// flash does: reading a console while a write is in flight is how a flash dies.
	opConsole opKind = "console"
	// opCmd is a generic serial command (NAME/SCAN/MODE/RESET/PING) on a configured
	// orb. It writes, so it claims a full port slot exactly like a provision.
	opCmd opKind = "cmd"
	// opTheme is a theme crossing the room to an orb's SD card. It never touches the
	// serial wire — it is HTTP — and it claims the slot anyway, because the thing it
	// must not collide with is this hub's own identity probe: a .part file is deleted the
	// instant the board resets mid-write, and the operator watches a card that never
	// changed. Registering the slot is what makes the probe sweep decline the port until
	// the transfer is over, and it makes /api/ops able to say so.
	opTheme opKind = "theme"
)

// op is one in-flight operation on one physical port.
type op struct {
	Port     string    `json:"port"`  // canonical: the key, and what the lock uses
	Asked    string    `json:"asked"` // the spelling the operator picked in the UI
	Kind     opKind    `json:"kind"`
	Started  time.Time `json:"started"`
	Deadline time.Time `json:"deadline,omitempty"`

	cancel context.CancelFunc
	done   chan struct{} // closed when the operation returns
	closed bool          // guards done against a double finish; set under opRegistry.mu
}

// opRegistry maps canonical port path to the operation holding it.
type opRegistry struct {
	mu     sync.Mutex
	byPort map[string]*op
}

func newOpRegistry() *opRegistry { return &opRegistry{byPort: map[string]*op{}} }

// start claims port for kind. It returns the operation, the context its work must
// run under, and refuses when the port already carries an operation — including
// under the other spelling of the same cable. parent is the request context, so a
// browser that goes away cancels the operation on its own.
func (r *opRegistry) start(parent context.Context, port string, kind opKind, budget time.Duration) (*op, context.Context, error) {
	if port == "" {
		return nil, nil, fmt.Errorf("no serial port named")
	}
	key := orbbind.CanonicalPortPath(port)
	r.mu.Lock()
	defer r.mu.Unlock()
	if live, ok := r.byPort[key]; ok {
		return nil, nil, fmt.Errorf("port %s is busy: %s running since %s - cancel it or wait it out",
			key, live.Kind, live.Started.Format("15:04:05"))
	}
	ctx := parent
	if ctx == nil {
		ctx = context.Background()
	}
	var cancel context.CancelFunc
	if budget > 0 {
		ctx, cancel = context.WithTimeout(ctx, budget)
	} else {
		ctx, cancel = context.WithCancel(ctx)
	}
	o := &op{Port: key, Asked: port, Kind: kind, Started: time.Now(), cancel: cancel, done: make(chan struct{})}
	if budget > 0 {
		o.Deadline = time.Now().Add(budget)
	}
	r.byPort[key] = o
	return o, ctx, nil
}

// finish releases the claim. Safe to call twice and on a nil operation, so a
// handler can `defer` it on every path.
func (r *opRegistry) finish(o *op) {
	if o == nil {
		return
	}
	r.mu.Lock()
	first := false
	if cur, ok := r.byPort[o.Port]; ok && cur == o {
		delete(r.byPort, o.Port)
		first = !o.closed
		o.closed = true
	}
	r.mu.Unlock()
	if first {
		close(o.done)
	}
}

// cancel asks the operation on port to stop. It reports whether one was there. The
// operation ends at its next block boundary; cancelling twice is harmless.
func (r *opRegistry) cancel(port string) (*op, bool) {
	key := orbbind.CanonicalPortPath(port)
	r.mu.Lock()
	o, ok := r.byPort[key]
	r.mu.Unlock()
	if !ok {
		return nil, false
	}
	o.cancel()
	return o, true
}

// cancelAll asks every live operation to stop, which is what a forced quit does:
// the operator said they do not care about the rest of this write.
func (r *opRegistry) cancelAll() []*op {
	r.mu.Lock()
	all := make([]*op, 0, len(r.byPort))
	for _, o := range r.byPort {
		all = append(all, o)
	}
	r.mu.Unlock()
	for _, o := range all {
		o.cancel()
	}
	return all
}

// busy reports the kind of operation holding port, if any.
func (r *opRegistry) busy(port string) (opKind, bool) {
	key := orbbind.CanonicalPortPath(port)
	r.mu.Lock()
	defer r.mu.Unlock()
	o, ok := r.byPort[key]
	if !ok {
		return "", false
	}
	return o.Kind, true
}

// running snapshots the live operations, sorted by port, for /api/ops and for the
// shutdown guard.
func (r *opRegistry) running() []*op {
	r.mu.Lock()
	all := make([]*op, 0, len(r.byPort))
	for _, o := range r.byPort {
		all = append(all, o)
	}
	r.mu.Unlock()
	sort.Slice(all, func(i, j int) bool { return all[i].Port < all[j].Port })
	return all
}

// writesFlash reports whether killing the process mid-operation can leave the
// board holding a partial image. Only these kinds earn the quit guard's long
// wait. A probe, a console read, a serial round-trip and a theme push are safe
// to end at any byte: the chip's flash is untouched, and the hub or the board
// simply re-asks. Reading one while it dies loses a console transcript, not a
// board; a dropped theme transfer leaves a .part file the firmware ignores.
func (k opKind) writesFlash() bool {
	switch k {
	case opFlash, opLonely, opWipe, opProvision:
		return true
	}
	return false
}

// runningWhere snapshots the live operations matching pred, sorted by port.
func (r *opRegistry) runningWhere(pred func(*op) bool) []*op {
	r.mu.Lock()
	all := make([]*op, 0, len(r.byPort))
	for _, o := range r.byPort {
		if pred(o) {
			all = append(all, o)
		}
	}
	r.mu.Unlock()
	sort.Slice(all, func(i, j int) bool { return all[i].Port < all[j].Port })
	return all
}

// cancelWhere cancels every live operation matching pred and returns them.
// Cancelling an already-cancelled operation is harmless, so quit paths can fire
// it twice (window close racing an interrupt) without a second thought.
func (r *opRegistry) cancelWhere(pred func(*op) bool) []*op {
	live := r.runningWhere(pred)
	for _, o := range live {
		o.cancel()
	}
	return live
}

// waitWhere blocks until no live operation matches pred, or until ctx is done.
// The quit path uses it to shelter writers while stepping over a wedged reader.
func (r *opRegistry) waitWhere(ctx context.Context, pred func(*op) bool) error {
	for {
		matches := r.runningWhere(pred)
		if len(matches) == 0 {
			return nil
		}
		r.mu.Lock()
		var first chan struct{}
		for _, o := range r.byPort {
			if pred(o) {
				first = o.done
				break
			}
		}
		r.mu.Unlock()
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-first:
		case <-time.After(250 * time.Millisecond): // belt: a lost done channel must not hang quit
		}
	}
}

// --- HTTP surface -------------------------------------------------------------

// handleOps reports the live port operations, so the UI can show which cable is
// busy with what and offer Cancel for it. A second orb on the bench is the reason
// this is per port: the old process-wide flag could only ever say "a flash is
// running" about nothing in particular.
func (s *Server) handleOps(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	type opView struct {
		Port     string `json:"port"`
		Asked    string `json:"asked"`
		Kind     string `json:"kind"`
		Started  string `json:"started"`
		Seconds  int    `json:"seconds"`
		Deadline string `json:"deadline,omitempty"`
	}
	live := s.ops.running()
	views := make([]opView, 0, len(live))
	for _, o := range live {
		v := opView{Port: o.Port, Asked: o.Asked, Kind: string(o.Kind),
			Started: o.Started.UTC().Format(time.RFC3339), Seconds: int(time.Since(o.Started).Seconds())}
		if !o.Deadline.IsZero() {
			v.Deadline = o.Deadline.UTC().Format(time.RFC3339)
		}
		views = append(views, v)
	}
	s.jsonOK(w, map[string]any{"ops": views})
}

// handleCancel asks the operation on one port to stop. It returns as soon as the
// request is made: the work ends at its next block boundary, which is the whole
// design — stopping mid-block would leave the ROM waiting for data that never
// arrives. The caller learns the outcome from the flash stream's own final event.
func (s *Server) handleCancel(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		Port string `json:"port"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Port == "" {
		s.jsonErr(w, "no serial port named")
		return
	}
	o, ok := s.ops.cancel(req.Port)
	if !ok {
		s.jsonErr(w, fmt.Sprintf("nothing is running on %s", orbbind.CanonicalPortPath(req.Port)))
		return
	}
	log.Printf("orb-manager: cancel requested for %s (%s)", o.Port, o.Kind)
	s.jsonOK(w, map[string]any{"port": o.Port, "kind": string(o.Kind), "cancelling": true})
}
