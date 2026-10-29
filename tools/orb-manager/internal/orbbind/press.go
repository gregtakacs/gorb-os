package orbbind

import (
	"context"
	"fmt"
	"os"
	"strings"
	"time"
)

// This file is the cross-process half of preemption.
//
// The hub's operation registry cancels THIS hub's background sweep when a press
// arrives, and it cannot see a second hub. The flock is the only claim that crosses
// processes, so a press in one hub lost to a sweep in the other and blamed the
// operator: "port /dev/ttyACM2 is busy - another flash or provision holds its lock".
// Live on the bench 2026-10-29 with two hubs and one blank board on ACM2 — five
// refusals in the log, every one caused by a background round nobody asked for.
//
// Two more advisory files carry the protocol, both keyed to the canonical port:
//
//	<port>.press  held for one whole operator write. A sweep checks it FIRST and
//	              declines, in any process, so no background round starts on a
//	              cable a human is using.
//	<port>.sweep  the sweep lease. One hub at a time runs background work on a
//	              cable; the loser skips the round and its ladder re-arms later.
//
// Lock order is press, then sweep, then the port flock. A press holds press and
// waits only for the flock; a sweep releases press the instant it checks it, so the
// two can never hold-and-wait on each other.
//
// The residual is honest. A round already past its checks keeps the flock until its
// own budget expires, so a press waits up to window for it. Refusing after that
// window still beats stealing a wire nobody released.

// pressPoll is how often a waiting press re-reads the flock. A background round
// releases within milliseconds of its work ending; this only has to be finer than
// that and cheap enough to spin for seconds.
const pressPoll = 40 * time.Millisecond

// PressWindowDefault is the wait a command-line tool gives a background round already
// in flight on the cable it wants. The hub passes its own, smaller, test-shrinkable
// value instead.
const PressWindowDefault = 12 * time.Second

// flagPath names one protocol flag file for one port. It derives from portLockPath,
// so the flock and both flags of one cable share one canonical stem and can never
// disagree about which device they mean.
func flagPath(path, suffix string) string {
	return strings.TrimSuffix(portLockPath(path), ".lock") + suffix
}

// PressPort locks the port for an operator write — a flash, a lonely-image write, a
// provision, a command, a console read. It holds the press flag for the whole
// operation, so the background sweep of every hub on this box declines the cable
// rather than racing it, and it waits up to window for a round that another process
// started before the flag went up.
//
// It refuses immediately when another press owns the flag. Presses never queue:
// two writers on one wire corrupts the write, and a press that silently waited out
// another press is a press whose result the operator cannot trust.
func PressPort(ctx context.Context, path string, window time.Duration) (*PortLock, error) {
	canon := canonicalPortPath(path)
	press, held, err := lockPort(flagPath(canon, ".press"))
	if err != nil {
		return nil, fmt.Errorf("orbbind: press claim %s: %w", canon, err)
	}
	if !held {
		return nil, fmt.Errorf("port %s is busy - another flash or provision holds its lock", canon)
	}
	drop := func() {
		unlockPort(press)
		_ = press.Close()
	}
	deadline := time.Now().Add(window)
	for {
		lock, free, lerr := tryLockPort(canon)
		if lerr != nil {
			drop()
			return nil, fmt.Errorf("orbbind: lock port %s: %w", canon, lerr)
		}
		if free {
			lock.press = press
			return lock, nil
		}
		left := time.Until(deadline)
		if left <= 0 {
			drop()
			return nil, fmt.Errorf("port %s is busy - another flash or provision holds its lock", canon)
		}
		select {
		case <-ctx.Done():
			drop()
			return nil, fmt.Errorf("press on %s abandoned: %w", canon, ctx.Err())
		case <-time.After(min(pressPoll, left)):
		}
	}
}

// SweepPort locks the cable for ONE background round: a cable proof, a launch or
// cadence probe, a command-line identify. Any error means "do not touch this wire
// right now", and every caller already treats it as a round skipped rather than a
// job failed — the ladder re-arms, and the board answers a later ask.
func SweepPort(ctx context.Context, path string) (*PortLock, error) {
	canon := canonicalPortPath(path)
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	// Press first, and release it at once. Holding it across the round would make a
	// sweep a press, and the round would then be uncancellable by a real one.
	press, held, err := lockPort(flagPath(canon, ".press"))
	if err != nil {
		return nil, fmt.Errorf("orbbind: press check %s: %w", canon, err)
	}
	if !held {
		return nil, fmt.Errorf("port %s is held by an operator write", canon)
	}
	unlockPort(press)
	_ = press.Close()

	lease, held, err := lockPort(flagPath(canon, ".sweep"))
	if err != nil {
		return nil, fmt.Errorf("orbbind: sweep lease %s: %w", canon, err)
	}
	if !held {
		return nil, fmt.Errorf("port %s already carries another hub's sweep lease", canon)
	}
	lock, free, lerr := tryLockPort(canon)
	if lerr != nil {
		releaseFlag(lease)
		return nil, fmt.Errorf("orbbind: lock port %s: %w", canon, lerr)
	}
	if !free {
		releaseFlag(lease)
		return nil, fmt.Errorf("port %s is busy - another flash or provision holds its lock", canon)
	}
	lock.lease = lease
	return lock, nil
}

// releaseFlag drops one protocol flag. Safe on nil. A failed flag never outlives its
// holder: both lock implementations revoke it when the process dies.
func releaseFlag(f *os.File) {
	if f == nil {
		return
	}
	unlockPort(f)
	_ = f.Close()
}
