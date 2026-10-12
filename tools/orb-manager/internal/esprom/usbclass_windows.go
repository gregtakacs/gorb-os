//go:build windows

package esprom

import (
	"regexp"
	"strings"
	"syscall"
	"unsafe"
)

// classifyPort asks SetupAPI which device owns the COM port, and reads the ids
// out of the device instance path, which is spelled
//
//	USB\VID_303A&PID_1001\1C9A...  or  USB\VID_303A&PID_1001\44BD8D608088
//
// Raw syscalls, no new module: the same constraint lockPort_windows writes
// against. A port SetupAPI cannot attribute returns Unknown, and Unknown means
// the hub will not reset it.
var (
	vidRe = regexp.MustCompile(`VID_([0-9a-fA-F]{4})`)
	pidRe = regexp.MustCompile(`PID_([0-9a-fA-F]{4})`)
	macRe = regexp.MustCompile(`(?i)[0-9A-F]{12}$`)
	szRe  = regexp.MustCompile(`(?i)\\([0-9A-Za-z]+)$`)
)

func classifyPort(dev string) USBClass {
	name := dev
	// Callers pass either COM7 or \\.\COM7; SetupAPI's friendly-name match wants
	// the bare one.
	if i := strings.LastIndexAny(name, `\/`); i >= 0 {
		name = name[i+1:]
	}
	instance, serial, ok := deviceForComPort(name)
	if !ok {
		return USBClass{}
	}
	out := USBClass{Known: true, Instance: instance, Serial: serial}
	m1, m2 := vidRe.FindStringSubmatch(strings.ToUpper(instance)), pidRe.FindStringSubmatch(strings.ToUpper(instance))
	if m1 == nil || m2 == nil {
		return out
	}
	out.Vendor = uint16(hexOr(m1[1]))
	out.Product = uint16(hexOr(m2[1]))
	// The S3 USB-JTAG serial-number is the MAC without separators; recover it so
	// the picker can label the port before anyone pays for a reboot.
	if m := szRe.FindStringSubmatch(instance); m != nil && macRe.MatchString(m[1]) {
		out.Serial = colonize(m[1])
	}
	return out
}

func hexOr(s string) uint64 {
	var v uint64
	for _, c := range s {
		v <<= 4
		switch {
		case c >= '0' && c <= '9':
			v |= uint64(c - '0')
		case c >= 'a' && c <= 'f':
			v |= uint64(c-'a') + 10
		case c >= 'A' && c <= 'F':
			v |= uint64(c-'A') + 10
		}
	}
	return v
}

func colonize(s string) string {
	var b strings.Builder
	for i := 0; i+2 <= len(s); i += 2 {
		if i > 0 {
			b.WriteByte(':')
		}
		b.WriteString(strings.ToLower(s[i : i+2]))
	}
	return b.String()
}

var (
	setupapi               = syscall.NewLazyDLL("setupapi.dll")
	procGetClassDevs       = setupapi.NewProc("SetupDiGetClassDevsW")
	procEnumDevices        = setupapi.NewProc("SetupDiEnumDeviceInfo")
	procDeviceRegPropertyW = setupapi.NewProc("SetupDiGetDeviceRegistryPropertyW")
)

// digcPresent | digcProfile, the standard "installed devices" filter.
const (
	digcPresent            = 0x00000002
	digcProfile            = 0x00000004
	digcfNoDeviceInterface = 0x00000020

	spdevPropertyDeviceInstance = 1 // DEVPKEY_Device_InstanceId, as a REG_SZ
	spdevPropertyFriendlyName   = 3
)

type devInfoData struct {
	cbSize    uint32
	ClassGuid syscall.GUID
	DevInst   uint32
	_         uint32
	reserved  uintptr
}

// GUID_DEVCLASS_PORTS: 4d36e978-e325-11ce-bfc1-08002be10318 — the class that owns
// COM ports, so enumeration stays small instead of walking every device.
var guidDevClassPorts = syscall.GUID{
	Data1: 0x4d36e978, Data2: 0xe325, Data3: 0x11ce,
	Data4: [8]byte{0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18},
}

// deviceForComPort walks the ports class looking for the device whose friendly
// name is the COM port we were given, and returns its instance path plus the
// trailing serial component.
func deviceForComPort(comName string) (instance, serial string, ok bool) {
	h, _, _ := procGetClassDevs.Call(
		uintptr(unsafe.Pointer(&guidDevClassPorts)), 0, 0,
		digcPresent|digcProfile|digcfNoDeviceInterface)
	if h == uintptr(syscall.InvalidHandle) {
		return "", "", false
	}
	defer syscall.CloseHandle(syscall.Handle(h))

	var data devInfoData
	data.cbSize = uint32(unsafe.Sizeof(data))
	for i := 0; ; i++ {
		r, _, _ := procEnumDevices.Call(h, uintptr(i), uintptr(unsafe.Pointer(&data)))
		if r == 0 {
			return "", "", false
		}
		if inst := readRegString(h, &data, spdevPropertyDeviceInstance); inst != "" {
			if strings.Contains(strings.ToUpper(readRegString(h, &data, spdevPropertyFriendlyName)), strings.ToUpper(comName)) {
				return inst, tailSerial(inst), true
			}
		}
	}
}

func tailSerial(instance string) string {
	if i := strings.LastIndex(instance, `\`); i >= 0 {
		return instance[i+1:]
	}
	return instance
}

func readRegString(h uintptr, data *devInfoData, prop uint32) string {
	var (
		buf         [512]uint16
		typ, needed uint32
	)
	r, _, _ := procDeviceRegPropertyW.Call(h, uintptr(unsafe.Pointer(data)), uintptr(prop),
		uintptr(unsafe.Pointer(&typ)), uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)),
		uintptr(unsafe.Pointer(&needed)), 0)
	if r == 0 || needed < 4 {
		return ""
	}
	return syscall.UTF16ToString(buf[:])
}
