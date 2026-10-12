package esprom

import "time"

// LineCtl is the DTR/RTS control a serial port must offer for download-mode
// entry. go.bug.st/serial satisfies it directly.
type LineCtl interface {
	SetDTR(on bool) error
	SetRTS(on bool) error
}

// EnterDownloadMode runs the USB-Serial/JTAG reset sequence, exactly as
// esptool's USBJTAGSerialReset (reset.py:124) drives it for the native
// ESP32-S3 CDC-ACM peripheral. There are no DTR/RTS wires to GPIO0/EN on this
// board; the ROM's USB-Serial/JTAG auto-download reacts to the control-line
// state transitions, so the timing here is the load-bearing part.
//
// After it returns, the ROM is in download mode and Sync should answer.
func EnterDownloadMode(p LineCtl) error {
	// Faithful transcription of USBJTAGSerialReset.reset():
	if err := p.SetRTS(false); err != nil {
		return err
	}
	if err := p.SetDTR(false); err != nil { // idle
		return err
	}
	time.Sleep(100 * time.Millisecond)
	if err := p.SetDTR(true); err != nil { // set IO0
		return err
	}
	if err := p.SetRTS(false); err != nil {
		return err
	}
	time.Sleep(100 * time.Millisecond)
	// reset: invert RTS so the (dtr,rts) pair passes through (1,1) not (0,0)
	if err := p.SetRTS(true); err != nil {
		return err
	}
	if err := p.SetDTR(false); err != nil {
		return err
	}
	if err := p.SetRTS(true); err != nil { // re-assert DTR alongside RTS for Windows usbser.sys
		return err
	}
	time.Sleep(100 * time.Millisecond)
	if err := p.SetDTR(false); err != nil {
		return err
	}
	if err := p.SetRTS(false); err != nil { // chip out of reset
		return err
	}
	return nil
}

// HardReset runs esptool HardReset(uses_usb=True): assert RTS to hold EN low,
// hold, then release, giving the chip time to re-enumerate its USB port.
func HardReset(p LineCtl) error {
	if err := p.SetRTS(true); err != nil { // EN -> LOW
		return err
	}
	time.Sleep(200 * time.Millisecond)
	if err := p.SetRTS(false); err != nil {
		return err
	}
	time.Sleep(200 * time.Millisecond)
	return nil
}
