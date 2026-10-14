package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// The operation registry is what replaced the process-wide `flashing` bool. These
// pin the three things that change buys: two cables work at once, one cable cannot
// be claimed twice under two different spellings, and every claim can be cancelled
// by the thing that made it.

func TestOperationRegistryIsPerPort(t *testing.T) {
	r := newOpRegistry()
	a, ctxA, err := r.start(context.Background(), "/dev/ttyACM0", opFlash, 0)
	if err != nil {
		t.Fatalf("first claim: %v", err)
	}
	if _, _, err := r.start(context.Background(), "/dev/ttyACM1", opFlash, 0); err != nil {
		t.Errorf("a second cable was refused while the first was busy: %v", err)
	}
	if _, _, err := r.start(context.Background(), "/dev/ttyACM0", opProvision, 0); err == nil {
		t.Error("the same port was claimed twice")
	} else if !strings.Contains(err.Error(), "busy") {
		t.Errorf("double-claim error = %q, want it to name the busy port", err)
	}

	// Cancel reaches the right cable only.
	if _, ok := r.cancel("/dev/ttyACM1"); !ok {
		t.Error("cancel found nothing on the second port")
	}
	if ctxA.Err() != nil {
		t.Error("cancelling ttyACM1 cancelled ttyACM0's operation")
	}
	if _, ok := r.cancel("/dev/ttyACM9"); ok {
		t.Error("cancel reported an operation that was never started")
	}
	r.finish(a)
	r.finish(a) // idempotent: a handler's defer must survive being called twice
	if _, ok := r.busy("/dev/ttyACM0"); ok {
		t.Error("port still busy after finish")
	}
}

// The registry keys on the canonical path exactly as the flock does: a
// /dev/serial/by-id link and the tty it resolves to are ONE cable. If they were two
// slots, two flashes could share one wire and corrupt each other, which is the
// single failure this tool exists to prevent.
func TestOperationRegistryCollapsesPortSpellings(t *testing.T) {
	dir := t.TempDir()
	target := filepath.Join(dir, "ttyACM77")
	if err := os.WriteFile(target, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(dir, "by-id-link")
	if err := os.Symlink(target, link); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	r := newOpRegistry()
	if _, _, err := r.start(context.Background(), link, opFlash, 0); err != nil {
		t.Fatalf("claiming by symlink: %v", err)
	}
	if _, _, err := r.start(context.Background(), target, opFlash, 0); err == nil {
		t.Error("the tty spelling of an already-claimed by-id cable was accepted as a second cable")
	}
	if kind, ok := r.busy(target); !ok || kind != opFlash {
		t.Errorf("busy(%s) = %q,%v; the registry must see the two spellings as one port", target, kind, ok)
	}
}

// An empty port name must be refused rather than becoming a claim on "".
func TestOperationRegistryRefusesEmptyPort(t *testing.T) {
	r := newOpRegistry()
	if _, _, err := r.start(context.Background(), "", opFlash, 0); err == nil {
		t.Fatal("an empty port name was accepted as a claim")
	}
	if live := r.running(); len(live) != 0 {
		t.Errorf("the refused claim was still registered: %+v", live)
	}
}

// A budget-bearing operation cancels itself at the deadline, so a wedged console
// cannot hold a cable past the timeout the handler promised.
func TestOperationBudgetCancelsItsContext(t *testing.T) {
	r := newOpRegistry()
	_, ctx, err := r.start(context.Background(), "/dev/ttyACM0", opProvision, 40*time.Millisecond)
	if err != nil {
		t.Fatal(err)
	}
	select {
	case <-ctx.Done():
	case <-time.After(2 * time.Second):
		t.Fatal("the operation budget never cancelled its context")
	}
}

// A quit with a live operation must wait for it instead of dropping the listener
// out from under the write — that is how an orb is left half-flashed — and a forced
// quit must still be able to land.
func TestQuitWaitsForLiveOperations(t *testing.T) {
	s := newHub()
	job, _, err := s.ops.start(context.Background(), "/dev/ttyACM0", opFlash, 0)
	if err != nil {
		t.Fatalf("claiming the port: %v", err)
	}
	srv := &http.Server{Handler: http.NotFoundHandler()}
	released := make(chan struct{})
	go func() {
		s.quit(srv)
		close(released)
	}()

	select {
	case <-released:
		t.Fatal("quit released the listener while a flash held the port")
	case <-time.After(250 * time.Millisecond):
	}

	// The operator insists, which is the second-interrupt path: cancel, then the
	// operation lands and quit completes.
	if live := s.ops.cancelAll(); len(live) != 1 {
		t.Fatalf("cancelAll cancelled %d operations, want 1", len(live))
	}
	s.ops.finish(job)
	select {
	case <-released:
	case <-time.After(5 * time.Second):
		t.Fatal("quit never completed after the operation was cancelled and finished")
	}
}

// A quit with nothing running must not wait, or Ctrl-C on an idle hub would sit out
// the whole quit budget.
func TestQuitIsImmediateWhenIdle(t *testing.T) {
	s := newHub()
	done := make(chan time.Duration, 1)
	start := time.Now()
	go func() {
		s.quit(&http.Server{Handler: http.NotFoundHandler()})
		done <- time.Since(start)
	}()
	select {
	case d := <-done:
		if d > 2*time.Second {
			t.Errorf("an idle quit took %v; it must not wait for anything", d)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("an idle quit never returned")
	}
}

// /api/ops says which cable is busy with what; /api/flash/cancel asks that work to
// stop. Both are pinned here, including refusing to cancel a cable at rest.
func TestOpsAndCancelEndpoints(t *testing.T) {
	s := newHub()
	job, ctx, err := s.ops.start(context.Background(), "/dev/ttyACM0", opFlash, 0)
	if err != nil {
		t.Fatal(err)
	}

	rec := httptest.NewRecorder()
	s.handleOps(rec, httptest.NewRequest(http.MethodGet, "/api/ops", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("GET /api/ops status = %d", rec.Code)
	}
	var payload struct {
		Data struct {
			Ops []struct {
				Port string `json:"port"`
				Kind string `json:"kind"`
			} `json:"ops"`
		} `json:"data"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &payload); err != nil {
		t.Fatalf("decode /api/ops: %v (body %s)", err, rec.Body)
	}
	if len(payload.Data.Ops) != 1 || payload.Data.Ops[0].Kind != "flash" {
		t.Fatalf("/api/ops = %s, want one live flash", rec.Body)
	}

	// Cancelling an idle cable is refused, and must not disturb the live one.
	recIdle := httptest.NewRecorder()
	s.handleCancel(recIdle, httptest.NewRequest(http.MethodPost, "/api/flash/cancel",
		strings.NewReader(`{"port":"/dev/ttyACM5"}`)))
	if recIdle.Code != http.StatusBadRequest {
		t.Errorf("cancelling an idle port returned %d, want 400", recIdle.Code)
	}
	if ctx.Err() != nil {
		t.Error("cancelling an idle port cancelled a live operation")
	}

	recOK := httptest.NewRecorder()
	s.handleCancel(recOK, httptest.NewRequest(http.MethodPost, "/api/flash/cancel",
		strings.NewReader(`{"port":"/dev/ttyACM0"}`)))
	if recOK.Code != http.StatusOK {
		t.Fatalf("POST /api/flash/cancel status = %d body %s", recOK.Code, recOK.Body)
	}
	select {
	case <-ctx.Done():
	case <-time.After(time.Second):
		t.Fatal("cancel never reached the operation's context")
	}
	s.ops.finish(job)

	recPost := httptest.NewRecorder()
	s.handleOps(recPost, httptest.NewRequest(http.MethodPost, "/api/ops", nil))
	if recPost.Code != http.StatusMethodNotAllowed {
		t.Error("GET-only /api/ops accepted a POST")
	}
}

// A cancel nobody can reach is the same as none: an interrupted flash still has to
// wait out the ROM deadline, which is the failure mode this whole iteration exists
// to remove. Pin both ends, because a dropped route and a dropped button look
// identical to the operator.
func TestUICanCancelAndQuitSafely(t *testing.T) {
	src := readAppJS(t)
	for _, want := range []string{"/api/flash/cancel", "ob-cancel", "showLiveOp", "/api/ops"} {
		if !strings.Contains(src, want) {
			t.Errorf("app.js does not reference %q, so the operator cannot reach that half of the cancel flow", want)
		}
	}
	page, err := os.ReadFile(filepath.Join("web", "index.html"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(page), `id="ob-cancel"`) {
		t.Error(`index.html has no #ob-cancel button for streamFlash to reveal`)
	}
	// The cancel must be the hub-side one, and the ban is scoped to the flasher on purpose.
	// Dropping a fetch mid-flash leaves the ROM mid-write with nobody watching, which is the bug
	// this pin exists to catch. A read-only stream is the opposite case: the hub is holding
	// nothing the page could corrupt by going away, so the discovery watchdog is allowed — and
	// required — to drop its own connection in order to reopen one. Hence: exactly one abort in
	// the file, it belongs to the orb stream, and none belongs to a flash.
	if flash := blockOf(t, src, "async function streamFlash"); strings.Contains(flash, ".abort(") {
		t.Error("cancelling a flash must go through /api/flash/cancel, not by dropping the fetch")
	}
	if n := strings.Count(src, ".abort("); n != 1 || !strings.Contains(src, "orbAbort.abort()") {
		t.Errorf("app.js contains %d client-side aborts: only the discovery stream's own watchdog may drop a fetch", n)
	}
}
