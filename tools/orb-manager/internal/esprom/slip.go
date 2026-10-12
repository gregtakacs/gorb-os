// Package esprom implements the Espressif ROM bootloader serial protocol:
// SLIP framing, the ESPROM command codec, download-mode entry, and the
// flash-write state machine. It talks to the chip's ROM bootloader directly
// (the equivalent of esptool --no-stub): no stub is loaded, which removes the
// version-2 plugin-stub machinery from the trusted path. Flash throughput is
// not the usual no-stub concern here because every supported board enumerates
// as native USB-Serial-JTAG CDC-ACM, where the link speed is USB-bound.
//
// Protocol facts were verified line-by-line against esptool 4.12
// (loader.py, reset.py, targets/esp32s3.py, cmds.py) on 2026-10-12.
package esprom

import (
	"errors"
	"io"
)

// SLIP specials (esptool loader.py slip_reader / write).
const (
	slipEnd    = 0xC0
	slipEsc    = 0xDB
	slipEscEnd = 0xDC
	slipEscEsc = 0xDD
)

// slipEncode frames one payload between END bytes, escaping END/ESC inside.
func slipEncode(p []byte) []byte {
	out := make([]byte, 0, len(p)+8)
	out = append(out, slipEnd)
	for _, b := range p {
		switch b {
		case slipEnd:
			out = append(out, slipEsc, slipEscEnd)
		case slipEsc:
			out = append(out, slipEsc, slipEscEsc)
		default:
			out = append(out, b)
		}
	}
	return append(out, slipEnd)
}

// slipDecoder turns a byte stream back into SLIP packets. One packet per
// Feed batch boundary: Feed returns (packet, true) when a complete packet
// ended on the END byte that closes this chunk, mirroring esptool's
// slip_reader which never hands out a partial between ENDs.
type slipDecoder struct {
	buf     []byte
	inEsc   bool
	gotByte bool // saw any payload byte since the last packet
}

// Feed pushes raw bytes; ok is true when one full packet is ready.
func (d *slipDecoder) Feed(p []byte) (pkt []byte, ok bool) {
	for _, b := range p {
		if d.inEsc {
			switch {
			case b == slipEscEnd:
				d.buf = append(d.buf, slipEnd)
			case b == slipEscEsc:
				d.buf = append(d.buf, slipEsc)
			default:
				// Invalid escape: esptool dies with "invalid SLIP packet".
				d.buf = nil
				d.inEsc = false
				d.gotByte = false
				return nil, false
			}
			d.inEsc = false
			continue
		}
		switch b {
		case slipEsc:
			d.inEsc = true
			d.gotByte = true
		case slipEnd:
			if d.gotByte {
				pkt, ok = d.buf, true
				d.buf, d.gotByte = nil, false
				return pkt, ok
			}
			// A packet-opening END: start collecting.
			d.buf, d.gotByte = d.buf[:0], false
		default:
			d.buf = append(d.buf, b)
			d.gotByte = true
		}
	}
	return nil, false
}

var (
	ErrTimeout      = errors.New("esprom: no response from target (timeout)")
	ErrNoSync       = errors.New("esprom: chip did not answer SYNC — not in download mode?")
	ErrBadChecksum  = errors.New("esprom: flash write rejected (checksum error on the wire)")
	ErrChipMismatch = errors.New("esprom: attached chip is not an ESP32-S3")
)

// ReadWriter abstracts the serial port read/write pair so tests can feed it a
// scripted device (loopback tests must not need real hardware).
type ReadWriter interface {
	io.ReadWriter
}
