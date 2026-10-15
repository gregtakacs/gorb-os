package esprom

// Device-class gate for the identity probe.
//
// Probing is not a passive read: EnterDownloadMode toggles DTR/RTS, and on a
// bench that also holds an Arduino, an FTDI breakout, or a meter, those edges
// reset devices the user never asked about. So a probe is only allowed on a port
// the OS already says is the Espressif USB-JTAG/serial peripheral an orb
// enumerates as (VID 0x303A, PID 0x1001). Where the OS cannot say, the answer is
// "unknown", and the caller must not reset the device on a guess.
const (
	orbUSBVendor  = 0x303A
	orbUSBProduct = 0x1001
)

// USBClass is what the host knows about the device behind a serial port.
type USBClass struct {
	Known    bool // the host could attribute the port to a device at all
	Vendor   uint16
	Product  uint16
	Instance string // the OS's device id, for the message shown when it is not an orb
	// Serial carries the chip MAC on the ESP32-S3 USB-JTAG peripheral, where the
	// host derives the device path from the serial-number descriptor. It is a
	// free identification the probe would otherwise pay a reboot for.
	Serial string
}

// IsOrbDevice reports whether the port belongs to the Espressif peripheral. An
// unknown device is not an orb: refusing is the safe direction, and the UI says
// so rather than pretending.
func (u USBClass) IsOrbDevice() bool {
	return u.Known && u.Vendor == orbUSBVendor && u.Product == orbUSBProduct
}

// IsEspressif reports the weaker fact: the host could attribute the port to a
// genuine Espressif chip (VID 0x303A), whatever product it enumerates as right
// now. The New Orb gate asks this question — only an Espressif chip can take the
// ROM flash protocol at all — and it must be answerable of a board that is about
// to be blank, mid boot-loop, or sitting in download mode, where a product id is
// a detail of whatever is currently running rather than of the chip.
func (u USBClass) IsEspressif() bool {
	return u.Known && u.Vendor == orbUSBVendor
}

// classifyPort reports what device sits behind a serial port, without opening or
// touching it. The hub uses it to label a port "orb-device" before anyone pays a
// reboot for the full identity.
func ClassifyPort(port string) USBClass { return classifyPort(port) }
