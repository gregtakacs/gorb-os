package esprom

import (
	"regexp"
	"strings"
)

// A USB device instance path is three backslash-separated fields:
//
//	USB\VID_303A&PID_1001\44:BD:8D:60:80:88          the composite parent
//	USB\VID_303A&PID_1001&MI_00\6&891293c&0&0000      one interface of it
//
// The middle field carries the ids and the interface number. The last field is
// the device's USB serial number on a real device, and a Windows-assigned slot
// id on an interface — which is why the MAC has to be read off the parent.
//
// Measured on the S3 built-in USB-Serial-JTAG: the serial number is the MAC in
// colon form, not the bare twelve hex digits the descriptor itself holds.
var (
	vidRe = regexp.MustCompile(`(?i)VID_([0-9A-F]{4})`)
	pidRe = regexp.MustCompile(`(?i)PID_([0-9A-F]{4})`)
	// either 44:BD:8D:60:80:88 or 44BD8D608088, and nothing else in that field
	macFieldRe = regexp.MustCompile(`(?i)^([0-9A-F]{2}(?::[0-9A-F]{2}){5})$`)
	macBareRe  = regexp.MustCompile(`(?i)^([0-9A-F]{12})$`)
	// an interface instance is ...&MI_00\6&891293c&0&0000: the number is followed
	// by a backslash and the slot id, not by the end of the string
	interfaceRe   = regexp.MustCompile(`(?i)&MI_([0-9A-F]{2})(?:\\|$)`)
	instanceSplit = regexp.MustCompile(`\\+`)
)

// usbIDsFrom reads the vendor and product out of any instance path or hardware
// id that contains them.
func usbIDsFrom(instance string) (vendor, product uint16, ok bool) {
	m1, m2 := vidRe.FindStringSubmatch(instance), pidRe.FindStringSubmatch(instance)
	if m1 == nil || m2 == nil {
		return 0, 0, false
	}
	v, ok1 := hexOr(m1[1])
	p, ok2 := hexOr(m2[1])
	if !ok1 || !ok2 {
		return 0, 0, false
	}
	return uint16(v), uint16(p), true
}

// macFromInstance returns the MAC encoded in an instance path's serial field, or
// "" when that field is a Windows slot id rather than a serial number.
func macFromInstance(instance string) string {
	fields := instanceSplit.Split(strings.Trim(instance, `\`), -1)
	if len(fields) == 0 {
		return ""
	}
	tail := fields[len(fields)-1]
	if m := macFieldRe.FindStringSubmatch(tail); m != nil {
		return strings.ToLower(m[1])
	}
	if m := macBareRe.FindStringSubmatch(tail); m != nil {
		return colonize(m[1])
	}
	return ""
}

// usbInterfaceFrom returns the MI_xx number an interface instance path names.
func usbInterfaceFrom(instance string) (int, bool) {
	m := interfaceRe.FindStringSubmatch(instance)
	if m == nil {
		return 0, false
	}
	v, ok := hexOr(m[1])
	if !ok {
		return 0, false
	}
	return int(v), true
}

// hexOr parses a run of hex digits. It refuses the whole run rather than
// returning the prefix it managed to read: "nothex" starting with a valid 'e' is
// how a lenient parser handed back 0xe0 as if it were a product id.
func hexOr(s string) (uint64, bool) {
	if s == "" {
		return 0, false
	}
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
		default:
			return 0, false
		}
	}
	return v, true
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
