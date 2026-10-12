//go:build windows

package esprom

import (
	"strings"
	"syscall"
	"unsafe"
)

// classifyPort resolves a COM port to the USB device behind it, and reads the ids
// the probe gate needs without ever opening the port.
//
// Windows keeps the mapping in the device registry, which is what the serial
// driver itself reads back:
//
//	Enum\USB\VID_303A&PID_1001&MI_00\6&891293c&0&0000\Device Parameters  PortName=COM3
//	Enum\USB\VID_303A&PID_1001\44:BD:8D:60:80:88
//
// so the walk is: find the interface whose PortName is this COM port, then step
// up to its composite parent, where the serial number — and on this chip the MAC
// — actually lives. The interface instance's own tail is a Windows slot id
// (6&891293c&0&0000), so reading the MAC there, which an earlier version of this
// file did, quietly produced nothing.
//
// Raw syscalls against advapi32 and cfgmgr32, no new module: the same constraint
// lockPort_windows writes against. A port that cannot be attributed returns the
// zero USBClass, and an unattributed port never gets reset.
func classifyPort(dev string) USBClass {
	name := dev
	// Callers pass either COM7 or \\.\COM7; PortName is stored bare.
	if i := strings.LastIndexAny(name, `\/`); i >= 0 {
		name = name[i+1:]
	}
	child := comPortInstance(name)
	if child == "" {
		return USBClass{}
	}
	out := USBClass{Known: true, Instance: child}
	if v, p, ok := usbIDsFrom(child); ok {
		out.Vendor, out.Product = v, p
	}
	// Prefer the parent: it holds the real serial number. A device that is not a
	// composite has no parent above us that is still this USB function, so keep
	// whatever the child itself gave us.
	if parent := parentInstance(child); parent != "" {
		if v, p, ok := usbIDsFrom(parent); ok && (out.Vendor == 0 || out.Product == 0) {
			out.Vendor, out.Product = v, p
		}
		if mac := macFromInstance(parent); mac != "" {
			out.Serial, out.Instance = mac, parent
		}
	}
	return out
}

const (
	sysRead = 0x20019 // KEY_READ | KEY_WOW64_64KEY: the registry is 64-bit here
	enumKey = `SYSTEM\CurrentControlSet\Enum\USB`
)

// comPortInstance returns the device instance path that owns comName, or "".
//
// It scans every hardware-id directory under Enum\USB, then every instance in
// it, and reads that instance's Device Parameters\PortName. Only usbser-backed
// ports carry the value, so everything else falls through; a machine with a
// thousand USB devices in its registry pays a few milliseconds for it.
func comPortInstance(comName string) string {
	root, err := regOpen(enumKey)
	if err != nil {
		return ""
	}
	defer regClose(root)
	hwids, err := regKeys(root)
	if err != nil {
		return ""
	}
	for _, hwid := range hwids {
		k, err := regOpen(enumKey + `\` + hwid)
		if err != nil {
			continue
		}
		instances, err := regKeys(k)
		regClose(k)
		if err != nil {
			continue
		}
		for _, serial := range instances {
			instance := hwid + `\` + serial
			p, err := regOpen(enumKey + `\` + instance + `\Device Parameters`)
			if err != nil {
				continue
			}
			port, err := regValue(p, "PortName")
			regClose(p)
			if err == nil && strings.EqualFold(strings.TrimSpace(port), comName) {
				return `USB\` + instance
			}
		}
	}
	return ""
}

// parentInstance steps one level up the devnode tree, which is where the
// composite parent that carries the serial number sits.
func parentInstance(instance string) string {
	var node uint32
	// The out-param comes first and the id second. Passed the other way round the
	// call still reports CR_SUCCESS, hands back devnode 0, and the parent lookup
	// fails with CR_INVALID_DEVNODE — which reads as "no MAC" rather than "bug".
	r, _, _ := procLocateDevNode.Call(uintptr(unsafe.Pointer(&node)),
		uintptr(unsafe.Pointer(syscall.StringToUTF16Ptr(instance))), 0)
	if r != 0 || node == 0 {
		return ""
	}
	var parent uint32
	if r, _, _ := procGetParent.Call(uintptr(unsafe.Pointer(&parent)), uintptr(node), 0); r != 0 || parent == 0 {
		return ""
	}
	return deviceID(parent)
}

func deviceID(node uint32) string {
	var buf [512]uint16
	if r, _, _ := procGetDeviceID.Call(uintptr(node), uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)), 0); r != 0 {
		return ""
	}
	return syscall.UTF16ToString(buf[:])
}

var (
	advapi            = syscall.NewLazyDLL("advapi32.dll")
	cfgmgr            = syscall.NewLazyDLL("cfgmgr32.dll")
	procOpenKey       = advapi.NewProc("RegOpenKeyExW")
	procCloseKey      = advapi.NewProc("RegCloseKey")
	procEnumKey       = advapi.NewProc("RegEnumKeyW")
	procQueryInfoKey  = advapi.NewProc("RegQueryInfoKeyW")
	procQueryValue    = advapi.NewProc("RegQueryValueExW")
	procLocateDevNode = cfgmgr.NewProc("CM_Locate_DevNodeW")
	procGetParent     = cfgmgr.NewProc("CM_Get_Parent")
	procGetDeviceID   = cfgmgr.NewProc("CM_Get_Device_IDW")
)

const hklm = uintptr(0x80000002)

func regOpen(path string) (uintptr, error) {
	p, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return 0, err
	}
	var k uintptr
	r, _, e := procOpenKey.Call(hklm, uintptr(unsafe.Pointer(p)), 0, sysRead, uintptr(unsafe.Pointer(&k)))
	if r != 0 {
		return 0, e
	}
	return k, nil
}

func regClose(k uintptr) { procCloseKey.Call(k) }

// regKeys lists the subkey names of k. RegQueryInfoKeyW must be asked for the
// maximum name length first, so the buffer handed to RegEnumKeyW is big enough.
func regKeys(k uintptr) ([]string, error) {
	var count, maxLen uint32
	// args: key, class, pcchClass, reserved, cSubKeys, cchMaxSubKeyLen, then the
	// six we do not need. Passing cSubKeys one slot early reads back zero.
	procQueryInfoKey.Call(k, 0, 0, 0, uintptr(unsafe.Pointer(&count)), uintptr(unsafe.Pointer(&maxLen)),
		0, 0, 0, 0, 0, 0)
	if count == 0 {
		return nil, nil
	}
	out := make([]string, 0, count)
	name := make([]uint16, maxLen+2)
	for i := uint32(0); i < count; i++ {
		n := uint32(len(name))
		r, _, _ := procEnumKey.Call(k, uintptr(i), uintptr(unsafe.Pointer(&name[0])), uintptr(unsafe.Pointer(&n)))
		if r == 0 {
			out = append(out, syscall.UTF16ToString(name[:n]))
		}
	}
	return out, nil
}

func regValue(k uintptr, name string) (string, error) {
	p, err := syscall.UTF16PtrFromString(name)
	if err != nil {
		return "", err
	}
	buf := make([]uint16, 260)
	var typ uint32
	// RegQueryValueExW takes the buffer size in bytes on input and the length it
	// wrote on output; a zero here fails every read with ERROR_SUCCESS never set.
	n := uint32(len(buf) * 2)
	r, _, e := procQueryValue.Call(k, uintptr(unsafe.Pointer(p)), 0, uintptr(unsafe.Pointer(&typ)),
		uintptr(unsafe.Pointer(&buf[0])), uintptr(unsafe.Pointer(&n)))
	if r != 0 || n < 2 {
		return "", e
	}
	return syscall.UTF16ToString(buf[:n/2]), nil
}
