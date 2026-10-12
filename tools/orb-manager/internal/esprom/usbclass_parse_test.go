package esprom

import "testing"

// Every string here was read off a live Windows host with two S3 orbs plugged in
// (HKLM\SYSTEM\CurrentControlSet\Enum\USB, 2026-10-12). The first version of the
// Windows classifier guessed at this shape instead of measuring it, read the
// serial off the interface node, and matched a twelve-hex-digit MAC that this
// chip does not store that way; the identity card came back blank and the probe
// refused to run. These cases are that bug, pinned.

const (
	// the composite parent: this is the node that carries the serial number
	winParent = `USB\VID_303A&PID_1001\44:BD:8D:60:80:88`
	// one interface of it: the serial field here is a Windows slot id
	winComInterface  = `USB\VID_303A&PID_1001&MI_00\6&891293c&0&0000`
	winJtagInterface = `USB\VID_303A&PID_1001&MI_02\6&d5791ba&0&0002`
)

func TestUSBIDsFromMeasuredInstancePaths(t *testing.T) {
	for _, tc := range []struct{ name, in string }{
		{"parent", winParent},
		{"com interface", winComInterface},
		{"jtag interface", winJtagInterface},
		{"hardware id form", `USB\VID_303A&PID_1001&MI_00`},
		{"lowercase", `usb\vid_303a&pid_1001\44:bd:8d:60:80:88`},
	} {
		v, p, ok := usbIDsFrom(tc.in)
		if !ok || v != 0x303A || p != 0x1001 {
			t.Errorf("%s: usbIDsFrom(%q) = (%#04x,%#04x,%v), want 0x303a/0x1001/true", tc.name, tc.in, v, p, ok)
		}
	}
	for _, bad := range []string{`PCI\VEN_8086&DEV_A36D`, `USB\VID_303A`, ``, `Razer Device`} {
		if _, _, ok := usbIDsFrom(bad); ok {
			t.Errorf("usbIDsFrom(%q) claimed ids it does not have", bad)
		}
	}
}

func TestMacFromInstanceReadsTheParentNotTheInterface(t *testing.T) {
	if got := macFromInstance(winParent); got != "44:bd:8d:60:80:88" {
		t.Errorf("parent mac = %q, want 44:bd:8d:60:80:88", got)
	}
	// The interface tail is a slot id. Reading it as a MAC is the exact bug: it
	// looks like an answer and is not one.
	for _, in := range []string{winComInterface, winJtagInterface, `USB\VID_1532&PID_0277\7&203261&0&3`} {
		if got := macFromInstance(in); got != "" {
			t.Errorf("macFromInstance(%q) = %q, want \"\" for a Windows slot id", in, got)
		}
	}
	// Some builds report the same MAC without separators; both forms must land.
	if got := macFromInstance(`USB\VID_303A&PID_1001\44BD8D608088`); got != "44:bd:8d:60:80:88" {
		t.Errorf("bare mac = %q, want colonized 44:bd:8d:60:80:88", got)
	}
	for _, in := range []string{``, `\`, `USB\VID_303A&PID_1001\`, `USB\VID_303A&PID_1001\ZZ:BD:8D:60:80:88`} {
		if got := macFromInstance(in); got != "" {
			t.Errorf("macFromInstance(%q) = %q, want \"\"", in, got)
		}
	}
}

func TestUSBInterfaceFromMeasuredPaths(t *testing.T) {
	for _, tc := range []struct {
		in   string
		want int
		ok   bool
	}{
		{winComInterface, 0x00, true},
		{winJtagInterface, 0x02, true},
		{winParent, 0, false},
		{`/dev/ttyACM0`, 0, false},
	} {
		got, ok := usbInterfaceFrom(tc.in)
		if got != tc.want || ok != tc.ok {
			t.Errorf("usbInterfaceFrom(%q) = (%d,%v), want (%d,%v)", tc.in, got, ok, tc.want, tc.ok)
		}
	}
}

func TestHexOrAndColonize(t *testing.T) {
	if v, ok := hexOr("303a"); !ok || v != 0x303A {
		t.Errorf("hexOr(303a) = (%#x,%v)", v, ok)
	}
	// Refusing beats returning the prefix it managed to read.
	if v, ok := hexOr("nothex"); ok || v != 0 {
		t.Errorf("hexOr(nothex) = (%#x,%v), want (0,false) rather than a half-parsed id", v, ok)
	}
	if v, ok := hexOr(""); ok || v != 0 {
		t.Errorf(`hexOr("") = (%#x,%v), want (0,false)`, v, ok)
	}
	if got := colonize("44BD8D608088"); got != "44:bd:8d:60:80:88" {
		t.Errorf("colonize = %q", got)
	}
	if got := colonize("44BD8D"); got != "44:bd:8d" {
		t.Errorf("colonize(44BD8D) = %q", got)
	}
}
