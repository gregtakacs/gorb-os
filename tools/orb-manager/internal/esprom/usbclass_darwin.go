//go:build darwin

package esprom

import (
	"bytes"
	"context"
	"errors"
	"os/exec"
	"sync"
	"time"
)

// classifyPort asks IOKit what device sits behind a serial port, without opening the
// port, so the probe gate has the same free half it has on Linux and Windows: the
// vendor/product pair decides whether a probe may touch the cable at all.
//
// IOKit is the source of truth and pyserial reads it through the framework directly.
// This cannot: the hub is built CGO_ENABLED=0 on every platform so that one binary
// runs on a Mac with no toolchain, which rules out calling IOServiceGetMatchingServices
// itself. ioreg(8) is the supported way to ask the same question from a pure-Go
// process, and -x asks it for XML rather than the ASCII tree, whose indentation,
// glyphs and decimal-versus-hex integer rendering would all be guesses.
//
// The IOUSB plane is the one that holds both halves of the answer in one document: the
// IOUSBHostDevice node carries idVendor/idProduct/"USB Serial Number", and the
// IOSerialBSDClient leaf beneath its interface carries IOCalloutDevice — the
// /dev/cu.usbmodem… name the hub was handed. Walking from the leaf up to the device is
// usbClassFromPlist's job and is unit-tested here on Linux; this function only fetches
// the document.
//
// Failure is always the zero USBClass, which is not an orb and never gets reset. A
// missing ioreg, a non-zero exit, an empty or unparsable dump and a port that simply
// is not in the registry all take that same path, so the worst this file can do on a
// Mac it has never been run on is restore the behaviour macOS had before it existed:
// "this host cannot identify the device behind the port, so the hub will not reset it".
func classifyPort(dev string) USBClass {
	raw, err := ioregUSBPlane()
	if err != nil {
		return USBClass{}
	}
	out, err := usbClassFromPlist(raw, dev)
	if err != nil {
		return USBClass{}
	}
	return out
}

const (
	// ioreg walks the whole registry on a machine with a dock, a keyboard, a hub and a
	// phone cable in it. Two seconds is generous for that and short enough that a wedged
	// IOKit cannot stall the ports request into looking like a hung hub.
	ioregTimeout = 2 * time.Second
	// One dump serves a whole /api/ports sweep: classifyPort is called once per port,
	// and paying a fresh registry walk per port turns a list request into N of them.
	// Short enough that unplug-and-replug is still visible on the next poll.
	ioregTTL = 250 * time.Millisecond
)

var (
	ioregMu   sync.Mutex
	ioregAt   time.Time
	ioregDump []byte
)

// ioregUSBPlane returns the XML dump of the IOUSB plane, reusing the last one while it
// is still fresh. The cached value is the raw bytes only: no decision is memoised, so a
// dump that turns out to be unusable cannot become permanently unusable.
func ioregUSBPlane() ([]byte, error) {
	ioregMu.Lock()
	defer ioregMu.Unlock()
	if ioregDump != nil && time.Since(ioregAt) < ioregTTL {
		return ioregDump, nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), ioregTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, "ioreg", "-p", "IOUSB", "-l", "-x")
	var out bytes.Buffer
	cmd.Stdout = &out
	// Stderr stays nil: ioreg's complaints would be surfaced by the caller as a
	// refused port, which is the whole response the hub has for them anyway.
	if err := cmd.Run(); err != nil {
		return nil, err
	}
	if out.Len() == 0 {
		return nil, errEmptyIoreg
	}
	ioregDump, ioregAt = out.Bytes(), time.Now()
	return ioregDump, nil
}

// errEmptyIoreg distinguishes "ioreg ran and said nothing" from "ioreg failed". Both
// mean the same thing to the caller — refuse the port — and the distinction exists only
// so the exit status is not mistaken for a registry with no orb in it.
var errEmptyIoreg = errors.New("ioreg returned no property list")
