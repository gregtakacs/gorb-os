package orbnet

import (
	"io"
	"net/http"
	"sync"
	"sync/atomic"
	"time"
)

// The orb-request trace: one line per request the hub sends an orb, with its
// lane class, answer, bytes and timing. Off unless the hub is started with
// -trace-orb. It exists for the bench's heap hunt (2026-10-31): the orb's
// internal RAM strands to a 1.5 KB largest block at some moment in a session,
// and the serial log alone cannot say which request was on the wire when it
// did. With this on, the hub's log and tools/bench/heap_watch.py's timeline
// share one wall clock.

var traceFn atomic.Pointer[func(format string, args ...any)]

// SetTrace turns the trace on (f non-nil) or off. It takes effect at once for
// every client, including ones already built.
func SetTrace(f func(format string, args ...any)) {
	if f == nil {
		traceFn.Store(nil)
		return
	}
	traceFn.Store(&f)
}

func className(c Class) string {
	switch c {
	case ClassControl:
		return "control"
	case ClassBulk:
		return "bulk"
	case ClassBackground:
		return "background"
	}
	return "?"
}

// traceRT wraps a client's transport. It is always in place and costs one
// atomic load when the trace is off, so turning it on needs no rebuild of the
// clients main already made.
type traceRT struct {
	base    *http.Transport
	class   Class
	preempt bool // a Preempting client: its dials carry WithPreempt
}

func (t *traceRT) RoundTrip(r *http.Request) (*http.Response, error) {
	f := traceFn.Load()
	if f == nil {
		return t.base.RoundTrip(r)
	}
	tag := className(t.class)
	if t.preempt || preempts(r.Context()) {
		tag += "+preempt"
	}
	began := time.Now()
	res, err := t.base.RoundTrip(r)
	if err != nil {
		(*f)("orb> %s %s%s [%s] error after %.2fs: %v",
			r.Method, r.URL.Host, r.URL.RequestURI(), tag, time.Since(began).Seconds(), err)
		return nil, err
	}
	hdr := time.Since(began)
	res.Body = &traceBody{ReadCloser: res.Body, done: func(n int64, rerr error) {
		tail := ""
		if rerr != nil {
			tail = " body error: " + rerr.Error()
		}
		(*f)("orb> %s %s%s [%s] %d %dB headers %.2fs total %.2fs%s",
			r.Method, r.URL.Host, r.URL.RequestURI(), tag, res.StatusCode, n,
			hdr.Seconds(), time.Since(began).Seconds(), tail)
	}}
	return res, nil
}

// traceBody counts what the caller actually read and reports once, at close or
// at the first read error, whichever comes first.
type traceBody struct {
	io.ReadCloser
	n    int64
	once sync.Once
	done func(n int64, err error)
}

func (b *traceBody) Read(p []byte) (int, error) {
	n, err := b.ReadCloser.Read(p)
	b.n += int64(n)
	if err != nil && err != io.EOF {
		b.once.Do(func() { b.done(b.n, err) })
	}
	return n, err
}

func (b *traceBody) Close() error {
	err := b.ReadCloser.Close()
	b.once.Do(func() { b.done(b.n, nil) })
	return err
}
