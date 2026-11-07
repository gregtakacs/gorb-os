package esprom

import (
	"errors"
	"fmt"
	"syscall"
	"time"

	"go.bug.st/serial"
)

// Port is an open serial connection to the orb's native USB-Serial-JTAG
// CDC-ACM endpoint. It satisfies ReadWriter (for Device) and LineCtl (for the
// reset sequences). Baud is nominal only: over CDC-ACM the host link is
// full-speed USB and the ESP ROM ignores it.
type Port struct {
	p    serial.Port
	name string
}

// OpenPort opens name (e.g. /dev/ttyACM1 or a /dev/serial/by-id path) at the
// ROM's 115200 8N1 and arms a short read timeout so Device's deadline loop
// still observes its own bounds while a ROM erase blocks silently.
func OpenPort(name string) (*Port, error) {
	mode := &serial.Mode{
		BaudRate: romBaud,
		DataBits: 8,
		Parity:   serial.NoParity,
		StopBits: serial.OneStopBit,
	}
	p, err := serial.Open(name, mode)
	if err != nil {
		return nil, fmt.Errorf("esprom: open %s: %w", name, err)
	}
	if err := p.SetReadTimeout(500 * time.Millisecond); err != nil {
		p.Close()
		return nil, fmt.Errorf("esprom: read timeout: %w", err)
	}
	return &Port{p: p, name: name}, nil
}

func (pt *Port) Read(b []byte) (int, error) { return pt.p.Read(b) }

// Write sends all of b. The serial library hands the buffer to one write(2) and
// returns its count, and a blocking write to a macOS tty can come back short — a
// signal (Go's runtime preempts with SIGURG) or the tty's output limit ends it part
// way — leaving a flash packet's tail unsent and the ROM waiting for it until the
// hub gives up with "no response from target" (a Mac flash, 2026-11-07, three
// images in). So the rest is sent, and an interrupted call is simply made again.
func (pt *Port) Write(b []byte) (int, error) {
	sent := 0
	for sent < len(b) {
		n, err := pt.p.Write(b[sent:])
		if n > 0 {
			sent += n
		}
		if err != nil {
			if errors.Is(err, syscall.EINTR) || errors.Is(err, syscall.EAGAIN) {
				continue
			}
			return sent, err
		}
		if n == 0 {
			time.Sleep(time.Millisecond) // a full output queue: let it drain
		}
	}
	return sent, nil
}

func (pt *Port) Close() error { return pt.p.Close() }

func (pt *Port) SetDTR(on bool) error { return pt.p.SetDTR(on) }
func (pt *Port) SetRTS(on bool) error { return pt.p.SetRTS(on) }

// Name returns the path the port was opened as, for logs and UI.
func (pt *Port) Name() string { return pt.name }

// ListPorts returns the OS serial device paths, sorted, for the UI's picker.
func ListPorts() ([]string, error) {
	return serial.GetPortsList()
}

const romBaud = 115200
