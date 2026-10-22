package esprom

import (
	"context"
	"fmt"
	"testing"
	"time"

	"go.bug.st/serial"
)

// Stranding is the bug these pin. A probe puts the chip into download mode to ask
// it a question; a probe that exits without booting the chip leaves it at
// "waiting for download" — no Wi-Fi, no HTTP, no banner — which on a headless
// bench is indistinguishable from a bricked board. Measured on the bench
// 2026-10-22: a port-thrashing watcher disturbed a free cable probe's Sync
// handshake, the early return shipped no reset, and the board came back looking
// dead after a hub probe nobody had asked for.

// recPort is a serial.Port that records every control-line edge and the close in
// one ordered event log, so a test can assert what the chip was driven into and
// what it was left in. The ReadWriter behind it decides whether the ROM talks.
type recPort struct {
	rw ReadWriter
	ev *[]string
}

func (p *recPort) Read(b []byte) (int, error)  { return p.rw.Read(b) }
func (p *recPort) Write(b []byte) (int, error) { return p.rw.Write(b) }
func (p *recPort) SetDTR(on bool) error {
	*p.ev = append(*p.ev, fmt.Sprintf("dtr:%d", lineBit(on)))
	return nil
}
func (p *recPort) SetRTS(on bool) error {
	*p.ev = append(*p.ev, fmt.Sprintf("rts:%d", lineBit(on)))
	return nil
}
func (p *recPort) Close() error {
	*p.ev = append(*p.ev, "close")
	return nil
}

func (p *recPort) SetMode(*serial.Mode) error         { return nil }
func (p *recPort) Drain() error                       { return nil }
func (p *recPort) ResetInputBuffer() error            { return nil }
func (p *recPort) ResetOutputBuffer() error           { return nil }
func (p *recPort) SetReadTimeout(time.Duration) error { return nil }
func (p *recPort) Break(time.Duration) error          { return nil }
func (p *recPort) GetModemStatusBits() (*serial.ModemStatusBits, error) {
	return &serial.ModemStatusBits{}, nil
}

func lineBit(on bool) int {
	if on {
		return 1
	}
	return 0
}

// deadRW is a chip that answers nothing on the wire.
type deadRW struct{}

func (deadRW) Read([]byte) (int, error)    { return 0, nil }
func (deadRW) Write(b []byte) (int, error) { return len(b), nil }

// fakeProbe wires the probe's two OS seams to a recorded fake port for one test.
func fakeProbe(t *testing.T, cls USBClass, p serial.Port) {
	t.Helper()
	oldC, oldO := probeClassify, probeOpen
	probeClassify = func(string) USBClass { return cls }
	probeOpen = func(string) (*Port, error) { return &Port{p: p, name: "fake-acm"}, nil }
	t.Cleanup(func() { probeClassify, probeOpen = oldC, oldO })
}

func orbCls() USBClass {
	return USBClass{Known: true, Vendor: orbUSBVendor, Product: orbUSBProduct, Serial: "44bd8d782618"}
}

// hardResets counts adjacent rts-high-then-rts-low pairs, the signature of
// HardReset. EnterDownloadMode also raises RTS, but never lowers it on the very
// next edge, so no entry sequence can be miscounted as an exit.
func hardResets(ev []string) int {
	n := 0
	for i := 1; i < len(ev); i++ {
		if ev[i-1] == "rts:1" && ev[i] == "rts:0" {
			n++
		}
	}
	return n
}

// A silent ROM used to be reported as "not an orb" through a return that left the
// chip in download mode. The guarantee: the answer may be wrong, the chip must
// boot.
func TestProbeWithoutBootloaderAnswerStillBoots(t *testing.T) {
	var ev []string
	fakeProbe(t, orbCls(), &recPort{rw: deadRW{}, ev: &ev})
	info, err := ProbeOrb(context.Background(), "/dev/fake")
	if err != nil {
		t.Fatalf("a silent ROM is a verdict, not an error: %v", err)
	}
	if info.Reason != "no bootloader answer - not an orb" {
		t.Errorf("reason = %q", info.Reason)
	}
	if got := hardResets(ev); got != 1 {
		t.Errorf("%d hard resets after a failed sync; the probe must leave the chip bootable (events %v)", got, ev)
	}
	if ev[len(ev)-1] != "close" {
		t.Errorf("the port closed before the reset reached the line; last event %q", ev[len(ev)-1])
	}
}

// A cancel between the download entry and the handshake is the wedged-identify
// case the hub hits on a flapping bench: it must stop honestly as cancelled AND
// still boot the chip.
func TestCancelledProbeStillBoots(t *testing.T) {
	var ev []string
	fakeProbe(t, orbCls(), &recPort{rw: deadRW{}, ev: &ev})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go func() {
		time.Sleep(100 * time.Millisecond)
		cancel()
	}()
	_, err := ProbeOrb(ctx, "/dev/fake")
	if !IsCancelled(err) {
		t.Fatalf("err = %v, want ErrCancelled: a cancelled handshake must not be filed as a chip that failed to answer", err)
	}
	if got := hardResets(ev); got != 1 {
		t.Errorf("%d hard resets after a cancel; stranding the chip is how a probe fakes a brick (events %v)", got, ev)
	}
	if ev[len(ev)-1] != "close" {
		t.Errorf("last event %q, want close after the reset", ev[len(ev)-1])
	}
}

// The guarantee fires at most once: once the probe's own HardReset has run, the
// deferred rescue must not pulse a second reboot into a chip already booting.
func TestProbeBootsExactlyOnce(t *testing.T) {
	var ev []string
	f := newFake(t) // a ROM that answers SYNC and the eFuse reads
	fakeProbe(t, orbCls(), &recPort{rw: f, ev: &ev})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go func() {
		time.Sleep(1500 * time.Millisecond) // after the handshake reset, inside the boot window
		cancel()
	}()
	_, err := ProbeOrb(ctx, "/dev/fake")
	if !IsCancelled(err) {
		t.Fatalf("want the probe cancelled inside its boot window, got %v", err)
	}
	if got := hardResets(ev); got != 1 {
		t.Errorf("%d hard resets on the wire; the rescue must not double-boot a chip already reset (events %v)", got, ev)
	}
}
