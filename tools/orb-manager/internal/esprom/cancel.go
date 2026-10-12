package esprom

import (
	"context"
	"errors"
	"time"
)

// ErrCancelled reports that the operator stopped an operation before it finished:
// the browser closed the panel, the hub was asked to cancel, or the process was
// told to quit. It is a distinct error rather than a timeout because the UI must
// be able to say "cancelled" honestly instead of blaming the orb for not answering,
// and because a cancelled write leaves the chip in download mode with a partial
// image — a state the operator, not the chip, caused.
var ErrCancelled = errors.New("esprom: cancelled by the operator")

// IsCancelled reports whether err is (or wraps) an operator cancellation.
func IsCancelled(err error) bool { return errors.Is(err, ErrCancelled) }

// ctxErr reports ErrCancelled once ctx is done, and nil otherwise. A nil ctx means
// "no cancellation wanted" and is what every bench caller gets by leaving
// Options.Ctx unset, so the headless tools keep behaving exactly as before.
//
// Cancellation is only ever observed BETWEEN wire operations: the checks sit in the
// read loop's iterations and between flash blocks, never inside a FLASH_DATA write.
// The ROM accepts or rejects a whole block, so stopping between blocks leaves the
// flash in a state the next full flash overwrites cleanly, while stopping mid-block
// would leave the ROM waiting for a block that never arrives.
func ctxErr(ctx context.Context) error {
	if ctx == nil {
		return nil
	}
	select {
	case <-ctx.Done():
		return ErrCancelled
	default:
		return nil
	}
}

// sleepCtx waits d, or until ctx is done, whichever comes first. The bare
// time.Sleep in the sync-retry loop would otherwise hold the port for its full
// window after the operator has already walked away.
func sleepCtx(ctx context.Context, d time.Duration) error {
	if ctx == nil {
		time.Sleep(d)
		return nil
	}
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-t.C:
		return nil
	case <-ctx.Done():
		return ErrCancelled
	}
}
