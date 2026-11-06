package esprom

import (
	"strings"
	"testing"
)

// The macOS classifier can only ever run on a Mac, so everything that decides whether
// a port may be reset is decided here, in a parser that runs under `go test` on this
// box.
//
// PROVENANCE, stated plainly: the property *names* below are the ones pyserial's
// list_ports_osx.py reads off IOKit (IOSerialBSDClient -> IOCalloutDevice, parent
// IOUSBHostDevice -> idVendor/idProduct as numbers, "USB Serial Number" as a string),
// cross-checked against Apple's serial-driver documentation. The element grammar is
// Apple's PropertyList-1.0 DTD. The bytes themselves are hand-authored here, NOT
// captured from a Mac — no macOS host was available. So the negative cases carry the
// real weight: they pin that a document this code does not recognise produces a
// refusal. The positive case proves the parser reads the keys it was written to read.
//
// The first live run on a Mac (owner's, 2026-11-06) found two faults the hand-authored
// documents could not: `-x` is ioreg's hexadecimal flag, not XML (`-a` is), and the
// IOUSB plane lists devices without their serial clients (0 IOCalloutDevice there, 1 in
// the IOService subtree `-r -c IOUSBHostDevice` prints). With both fixed the hub tied
// /dev/cu.usbmodem21101 to the orb's chip, 44:bd:8d:60:80:88, behind two hubs.
// macServiceDump below is that layout.

const (
	macOrbCallout = "/dev/cu.usbmodem1101"
	macOrbMAC     = "44:bd:8d:60:80:88"
	macFTDICallup = "/dev/cu.usbserial-A50285BI"
)

// macOrbDump is the S3 on its built-in USB-Serial-JTAG: a device dict stating the
// Espressif ids and the chip MAC as its serial number, with the serial client that
// owns /dev/cu.usbmodem1101 nested beneath its interface.
const macOrbDump = `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>IORegistryEntryChildren</key>
  <array>
    <dict>
      <key>_NAME</key><string>USB JTAG_serial debug unit</string>
      <key>idVendor</key><integer>12346</integer>
      <key>idProduct</key><integer>4097</integer>
      <key>USB Vendor Name</key><string>Espressif</string>
      <key>USB Product Name</key><string>USB JTAG_serial debug unit</string>
      <key>USB Serial Number</key><string>44:BD:8D:60:80:88</string>
      <key>IORegistryEntryChildren</key>
      <array>
        <dict>
          <key>IORegistryEntryChildren</key>
          <array>
            <dict>
              <key>IOCalloutDevice</key><string>/dev/cu.usbmodem1101</string>
              <key>IOConsoleDevice</key><string>/dev/tty.usbmodem1101</string>
            </dict>
          </array>
        </dict>
      </array>
    </dict>
  </array>
</dict>
</plist>`

// macTwoDeviceDump holds two unrelated devices side by side. The port under test
// belongs to the second, so a classifier that answered "the first device in the dump"
// would report the FTDI's ids here and pass unnoticed.
const macTwoDeviceDump = `<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
  <key>IORegistryEntryChildren</key>
  <array>
    <dict>
      <key>idVendor</key><integer>1027</integer>
      <key>idProduct</key><integer>24577</integer>
      <key>USB Serial Number</key><string>A50285BI</string>
      <key>IORegistryEntryChildren</key>
      <array>
        <dict>
          <key>IOCalloutDevice</key><string>/dev/cu.usbserial-A50285BI</string>
        </dict>
      </array>
    </dict>
    <dict>
      <key>idVendor</key><integer>12346</integer>
      <key>idProduct</key><integer>4097</integer>
      <key>USB Serial Number</key><string>44:BD:8D:60:80:88</string>
      <key>IORegistryEntryChildren</key>
      <array>
        <dict>
          <key>IOCalloutDevice</key><string>/dev/cu.usbmodem1101</string>
        </dict>
      </array>
    </dict>
  </array>
</dict>
</plist>`

// macNestedHubDump is an orb behind a USB hub: the nearest id-stating ancestor is the
// orb's own device, not the hub above it. Reading the outer dict would attribute the
// hub and refuse the port, which reads to the user as "the Mac says this is not an
// orb".
const macNestedHubDump = `<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
  <key>idVendor</key><integer>8457</integer>
  <key>idProduct</key><integer>10263</integer>
  <key>USB Vendor Name</key><string>Generic</string>
  <key>USB Product Name</key><string>USB2.0 Hub</string>
  <key>IORegistryEntryChildren</key>
  <array>
    <dict>
      <key>idVendor</key><integer>12346</integer>
      <key>idProduct</key><integer>4097</integer>
      <key>USB Serial Number</key><string>44:BD:8D:60:80:88</string>
      <key>IORegistryEntryChildren</key>
      <array>
        <dict>
          <key>IOCalloutDevice</key><string>/dev/cu.usbmodem1101</string>
        </dict>
      </array>
    </dict>
  </array>
</dict>
</plist>`

func mustClass(t *testing.T, raw []byte, dev string) USBClass {
	t.Helper()
	c, err := usbClassFromPlist(raw, dev)
	if err != nil {
		t.Fatalf("usbClassFromPlist(%q): %v", dev, err)
	}
	return c
}

func TestUSBClassFromPlistAttributesTheOrb(t *testing.T) {
	// Every spelling of the same cable has to land on the same device, because the
	// picker hands over whatever go.bug.st/serial enumerated.
	for _, dev := range []string{macOrbCallout, "cu.usbmodem1101", "usbmodem1101", "  /dev/cu.usbmodem1101  "} {
		c := mustClass(t, []byte(macOrbDump), dev)
		if !c.Known {
			t.Fatalf("%q: Known = false, want the port attributed", dev)
		}
		if !c.IsOrbDevice() {
			t.Errorf("%q: IsOrbDevice() = false for %#04x:%#04x, want the S3 peripheral recognised", dev, c.Vendor, c.Product)
		}
		if c.Serial != macOrbMAC {
			t.Errorf("%q: Serial = %q, want %q", dev, c.Serial, macOrbMAC)
		}
		if !strings.Contains(c.Instance, "Espressif") {
			t.Errorf("%q: Instance = %q, want the vendor name in the label", dev, c.Instance)
		}
	}
	// The /dev/tty.* twin names the same hardware and must classify identically.
	if c := mustClass(t, []byte(macOrbDump), "/dev/tty.usbmodem1101"); !c.IsOrbDevice() {
		t.Errorf("console device form not recognised: %#v", c)
	}
}

func TestUSBClassFromPlistPicksTheOwningDeviceNotTheFirst(t *testing.T) {
	// The fixture's two devices differ on purpose. If that stopped being true this
	// test would prove nothing, so assert it out loud first.
	a := mustClass(t, []byte(macTwoDeviceDump), macFTDICallup)
	b := mustClass(t, []byte(macTwoDeviceDump), macOrbCallout)
	if a.Vendor == b.Vendor {
		t.Fatalf("both ports resolved to %#04x: the fixture no longer distinguishes them", a.Vendor)
	}
	if a.IsOrbDevice() {
		t.Errorf("the FTDI cable classified as an orb: %#04x:%#04x", a.Vendor, a.Product)
	} else if a.Vendor != 0x0403 || a.Product != 0x6001 {
		t.Errorf("non-orb ids = %#04x:%#04x, want the FTDI 0403:6001 the fixture gives it", a.Vendor, a.Product)
	}
	if !b.IsOrbDevice() {
		t.Errorf("the orb listed after the FTDI entry was misattributed: %#04x:%#04x known=%v", b.Vendor, b.Product, b.Known)
	}
	// An orb behind a hub reports the orb's ids, not the hub's.
	if c := mustClass(t, []byte(macNestedHubDump), macOrbCallout); !c.IsOrbDevice() {
		t.Errorf("orb behind a hub classified as %#04x:%#04x, want 303a:1001 not the hub", c.Vendor, c.Product)
	}
}

// TestUSBClassFromPlistRefusesWhatItCannotProve is the safety net. Every case must
// produce the zero USBClass: a Known=true here is what licenses toggling DTR/RTS on a
// bench that may be holding somebody's Arduino.
func TestUSBClassFromPlistRefusesWhatItCannotProve(t *testing.T) {
	// A serial client with no device above it stating its ids.
	const orphaned = `<plist><dict><key>IOCalloutDevice</key><string>/dev/cu.usbmodem1101</string></dict></plist>`
	// ids as strings: plausible-looking, and not the CFNumbers the gate reads.
	const stringIDs = `<plist><dict><key>idVendor</key><string>12346</string><key>idProduct</key><string>4097</string>` +
		`<key>IORegistryEntryChildren</key><array><dict><key>IOCalloutDevice</key><string>/dev/cu.usbmodem1101</string></dict></array></dict></plist>`
	// One id and not the other.
	const halfIDs = `<plist><dict><key>idVendor</key><integer>12346</integer>` +
		`<key>IORegistryEntryChildren</key><array><dict><key>IOCalloutDevice</key><string>/dev/cu.usbmodem1101</string></dict></array></dict></plist>`
	// An id that is not a number at all.
	const garbageID = `<plist><dict><key>idVendor</key><integer>12346</integer><key>idProduct</key><integer>n/a</integer>` +
		`<key>IORegistryEntryChildren</key><array><dict><key>IOCalloutDevice</key><string>/dev/cu.usbmodem1101</string></dict></array></dict></plist>`
	// A key naming a value that is not there: a truncated document.
	const danglingKey = `<plist><dict><key>idVendor</key><integer>12346</integer><key>idProduct</key></dict></plist>`
	// The port sits past the nesting ceiling, where the walk stops on purpose.
	deep := "<plist>" + strings.Repeat("<dict><key>k</key><array>", plistMaxDepth+8) +
		`<dict><key>idVendor</key><integer>12346</integer><key>idProduct</key><integer>4097</integer>` +
		`<key>IOCalloutDevice</key><string>/dev/cu.usbmodem1101</string></dict>` +
		strings.Repeat("</array></dict>", plistMaxDepth+8) + "</plist>"

	for _, tc := range []struct{ name, doc string }{
		{"empty document", ``},
		{"not xml at all", `ioreg: unknown plane`},
		{"truncated plist", macOrbDump[:len(macOrbDump)-len("</plist>")]},
		{"orphaned serial client", orphaned},
		{"ids as strings", stringIDs},
		{"only one id", halfIDs},
		{"unparseable id", garbageID},
		{"truncated value", danglingKey},
		{"past the depth ceiling", deep},
	} {
		c, err := usbClassFromPlist([]byte(tc.doc), macOrbCallout)
		if err != nil && c != (USBClass{}) {
			t.Errorf("%s: returned an error and a non-zero class anyway: %#v / %v", tc.name, c, err)
			continue
		}
		if c.Known {
			t.Errorf("%s: Known = true on an unprovable device, which would let the probe reset it: %#04x:%#04x", tc.name, c.Vendor, c.Product)
		}
		if c.IsOrbDevice() {
			t.Errorf("%s: classified as an orb with no basis: %#v", tc.name, c)
		}
	}
}

func TestUSBClassFromPlistIgnoresForeignPorts(t *testing.T) {
	for _, dev := range []string{"", "   ", "/dev/ttyACM0", "COM7", `/../../etc/passwd`, "usbmodem1101x"} {
		c, err := usbClassFromPlist([]byte(macOrbDump), dev)
		if err != nil {
			t.Fatalf("%q: %v", dev, err)
		}
		if c != (USBClass{}) {
			t.Errorf("%q: matched a port it does not name: %#v", dev, c)
		}
	}
}

func TestNormalizeSerialNumber(t *testing.T) {
	for _, tc := range []struct{ in, want string }{
		{"44:BD:8D:60:80:88", "44:bd:8d:60:80:88"},
		{"44BD8D608088", "44:bd:8d:60:80:88"},
		{"  44:bd:8d:60:80:88  ", "44:bd:8d:60:80:88"},
		{"", ""},
		// Not a MAC at all: passed through lowercased rather than colonised into
		// something that only looks like one.
		{"SerialPort", "serialport"},
		{"44:BD:8D:60:80", "44:bd:8d:60:80"},
	} {
		if got := normalizeSerialNumber(tc.in); got != tc.want {
			t.Errorf("normalizeSerialNumber(%q) = %q, want %q", tc.in, got, tc.want)
		}
	}
}

// The IOService plane, as `ioreg -r -c IOUSBHostDevice -l -x` prints it (owner's Mac,
// 2026-11-06: the IOUSB plane held 0 IOCalloutDevice, this one 1): a top-level array of
// device subtrees, the interface between device and serial client stating the same ids
// with no serial number, and a hub's subtree holding the devices behind it.
const macServiceDump = `<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<array>
  <dict>
    <key>USB Product Name</key><string>USB2.0 Hub</string>
    <key>idVendor</key><integer>8457</integer>
    <key>idProduct</key><integer>10263</integer>
    <key>USB Serial Number</key><string>000000000</string>
    <key>IORegistryEntryChildren</key>
    <array>
      <dict>
        <key>USB Product Name</key><string>USB JTAG_serial debug unit</string>
        <key>idVendor</key><integer>12346</integer>
        <key>idProduct</key><integer>4097</integer>
        <key>IORegistryEntryChildren</key>
        <array>
          <dict>
            <key>bInterfaceNumber</key><integer>0</integer>
            <key>idVendor</key><integer>12346</integer>
            <key>idProduct</key><integer>4097</integer>
            <key>IORegistryEntryChildren</key>
            <array>
              <dict>
                <key>IOCalloutDevice</key><string>/dev/cu.usbmodem21101</string>
                <key>IOConsoleDevice</key><string>/dev/tty.usbmodem21101</string>
              </dict>
            </array>
          </dict>
        </array>
      </dict>
    </array>
  </dict>
  <dict>
    <key>USB Vendor Name</key><string>Espressif</string>
    <key>USB Product Name</key><string>USB JTAG_serial debug unit</string>
    <key>idVendor</key><integer>12346</integer>
    <key>idProduct</key><integer>4097</integer>
    <key>USB Serial Number</key><string>44:BD:8D:60:80:88</string>
    <key>IORegistryEntryChildren</key>
    <array>
      <dict>
        <key>bInterfaceNumber</key><integer>0</integer>
        <key>idVendor</key><integer>12346</integer>
        <key>idProduct</key><integer>4097</integer>
        <key>IORegistryEntryChildren</key>
        <array>
          <dict>
            <key>IOCalloutDevice</key><string>/dev/cu.usbmodem1101</string>
            <key>IOConsoleDevice</key><string>/dev/tty.usbmodem1101</string>
          </dict>
        </array>
      </dict>
    </array>
  </dict>
</array>
</plist>`

func TestUSBClassFromPlistReadsTheServicePlane(t *testing.T) {
	// the interface states the ids too; the MAC is the device's, one level up
	c := mustClass(t, []byte(macServiceDump), "/dev/cu.usbmodem1101")
	if !c.Known || !c.IsOrbDevice() {
		t.Fatalf("an orb in the IOService plane not attributed: %#v", c)
	}
	if c.Serial != macOrbMAC {
		t.Errorf("Serial = %q, want the device's %q past the interface that has none", c.Serial, macOrbMAC)
	}
	if !strings.Contains(c.Instance, "Espressif") {
		t.Errorf("Instance = %q, want the device's label, not the interface's", c.Instance)
	}
}

func TestUSBClassFromPlistNeverTakesTheHubsIdentity(t *testing.T) {
	// an orb behind a hub, its device stating no serial: the walk stops at the orb
	c := mustClass(t, []byte(macServiceDump), "/dev/cu.usbmodem21101")
	if !c.Known || c.Vendor != 0x303a || c.Product != 0x1001 {
		t.Fatalf("orb behind a hub = %#04x:%#04x known=%v, want 303a:1001", c.Vendor, c.Product, c.Known)
	}
	if c.Serial != "" {
		t.Errorf("Serial = %q: the hub's serial number was taken for the orb's", c.Serial)
	}
}
