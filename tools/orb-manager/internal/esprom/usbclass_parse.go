package esprom

import (
	"bytes"
	"encoding/xml"
	"fmt"
	"io"
	"regexp"
	"strconv"
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

// IOKit property names the macOS classifier reads, taken from shipped code rather than
// guessed at: Apple's serial-driver documentation names IOCalloutDevice as the
// /dev/cu.* node, and pyserial's list_ports_osx.py — what `python -m serial.tools.
// miniterm` runs on every Mac — walks IOSerialBSDClient up to its IOUSBHostDevice
// parent and reads idVendor/idProduct as numbers and "USB Serial Number" as a string.
//
// This parser lives in the portable file, exactly as the Windows instance-path
// parsing above does, so `go test` exercises it on Linux. Only the ioreg invocation is
// darwin-only, and that has nothing to test beyond "returned bytes".
const (
	plistCalloutKey     = "IOCalloutDevice"
	plistConsoleKey     = "IOConsoleDevice"
	plistVendorKey      = "idVendor"
	plistProductKey     = "idProduct"
	plistSerialNumKey   = "USB Serial Number"
	plistVendorNameKey  = "USB Vendor Name"
	plistProductNameKey = "USB Product Name"
	// "_NAME" is the registry entry's own name. It only ever feeds a display string,
	// so the fallbacks in usbInstanceLabel are enough and no decision reads it.
	plistNameKey = "_NAME"
	// A registry tree is deep but not unboundedly so. A malformed document must not be
	// able to grow the stack until the hub dies while serving /api/ports.
	plistMaxDepth = 128
)

// plistNode is one element of an XML property list — the shape Apple's
// PropertyList-1.0 DTD defines, where <dict> holds a <key> immediately followed by the
// value element it names and <array> holds bare value elements.
//
// The raw tree with parent links, rather than a decoded struct, is deliberate: devices
// nest under a container key this code never needs to know the name of, and hard-coding
// it would be one more thing to get wrong about a document nobody here has read.
type plistNode struct {
	name   string
	text   string
	kids   []*plistNode
	parent *plistNode
}

func parsePlist(raw []byte) (*plistNode, error) {
	dec := xml.NewDecoder(bytes.NewReader(raw))
	var root, cur *plistNode
	for {
		tk, err := dec.Token()
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, err
		}
		switch t := tk.(type) {
		case xml.StartElement:
			n := &plistNode{name: t.Name.Local, parent: cur}
			if cur == nil {
				root = n
			} else {
				cur.kids = append(cur.kids, n)
			}
			cur = n
		case xml.CharData:
			// Whitespace between a dict's children lands here too. It is ignored on
			// read, because only value elements are ever asked for their text.
			if cur != nil {
				cur.text += string(t)
			}
		case xml.EndElement:
			if cur != nil {
				cur = cur.parent
			}
		}
	}
	if root == nil {
		return nil, fmt.Errorf("empty property list")
	}
	return root, nil
}

// dictValue returns the value a <key> names, which the DTD guarantees is the element
// immediately after it. A key with nothing after it is a truncated document and reads
// as absent, not as the zero value of whatever type was expected.
func (n *plistNode) dictValue(key string) *plistNode {
	if n == nil {
		return nil
	}
	for i, k := range n.kids {
		if k.name == "key" && strings.TrimSpace(k.text) == key && i+1 < len(n.kids) {
			return n.kids[i+1]
		}
	}
	return nil
}

func (n *plistNode) dictString(key string) string {
	v := n.dictValue(key)
	if v == nil || (v.name != "string" && v.name != "data") {
		return ""
	}
	return strings.TrimSpace(v.text)
}

// dictUint reads an integer property, and only an <integer>: idVendor and idProduct
// are CFNumbers in the registry. A document holding them as strings is not the thing
// this gate was written to trust, and parsing "12346" out of a string would launder an
// unverified value into permission to toggle DTR/RTS on a bench device.
func (n *plistNode) dictUint(key string) (uint64, bool) {
	v := n.dictValue(key)
	if v == nil || v.name != "integer" {
		return 0, false
	}
	u, err := strconv.ParseUint(strings.TrimSpace(v.text), 10, 32)
	if err != nil {
		return 0, false
	}
	return u, true
}

// usbClassFromPlist resolves one serial device path — /dev/cu.usbmodem1101,
// cu.usbmodem1101, or the bare usbmodem1101 — against an ioreg dump, and reports what
// the host says about the USB device behind it.
//
// The answer is deliberately binary. Either the callout node is found AND an ancestor
// states both ids as integers, or the caller gets the zero USBClass and the probe
// refuses the port. Known is never set on a partial read, because identity.go renders
// Known=false as "this host cannot identify the device behind the port, so the hub
// will not reset it" — the true sentence in every one of these cases.
func usbClassFromPlist(raw []byte, dev string) (USBClass, error) {
	base := portBase(dev)
	if base == "" {
		return USBClass{}, nil
	}
	root, err := parsePlist(raw)
	if err != nil {
		return USBClass{}, err
	}
	serial := findCalloutDict(root, base, 0)
	if serial == nil {
		return USBClass{}, nil
	}
	// The ids live on the device, not on the serial client or the interface between
	// them, so walk up to the first ancestor that states both.
	for n := serial.parent; n != nil; n = n.parent {
		v, okV := n.dictUint(plistVendorKey)
		p, okP := n.dictUint(plistProductKey)
		if !okV || !okP {
			continue
		}
		return USBClass{
			Known:    true,
			Vendor:   uint16(v),
			Product:  uint16(p),
			Serial:   normalizeSerialNumber(n.dictString(plistSerialNumKey)),
			Instance: usbInstanceLabel(n),
		}, nil
	}
	// The port was found and nothing above it states its ids. That is not an orb, and
	// it is not something to reset on a guess.
	return USBClass{}, nil
}

// findCalloutDict returns the dict naming this port as its callout, or console, device.
// IOCalloutDevice is the /dev/cu.* node the hub opens; IOConsoleDevice is its
// /dev/tty.* twin with the same basename. Accepting both means a caller that passes
// whichever form it happened to enumerate still gets an answer.
func findCalloutDict(n *plistNode, base string, depth int) *plistNode {
	if n == nil || depth > plistMaxDepth {
		return nil
	}
	if n.name == "dict" {
		for _, key := range []string{plistCalloutKey, plistConsoleKey} {
			if v := n.dictString(key); v != "" && portBase(v) == base {
				return n
			}
		}
	}
	for _, k := range n.kids {
		if hit := findCalloutDict(k, base, depth+1); hit != nil {
			return hit
		}
	}
	return nil
}

// portBase reduces any caller's idea of a port name to the single string ioreg agrees
// on: /dev/cu.usbmodem1101, cu.usbmodem1101 and usbmodem1101 all name one cable. The
// cu./tty. prefix is dropped because IOCalloutDevice and IOConsoleDevice are the same
// hardware under it, so stripping on both sides of the comparison cannot merge two
// different devices — and a caller that only knows the short name still gets an answer.
func portBase(dev string) string {
	dev = strings.TrimSpace(dev)
	if dev == "" {
		return ""
	}
	// A backslash forms no path on darwin, but a stray one must not be able to smuggle
	// a ".." past this comparison either.
	if i := strings.LastIndexAny(dev, `/\`); i >= 0 {
		dev = dev[i+1:]
	}
	for _, prefix := range []string{"cu.", "tty."} {
		if strings.HasPrefix(dev, prefix) {
			return dev[len(prefix):]
		}
	}
	return dev
}

// normalizeSerialNumber lowercases the MAC the S3's USB-JTAG peripheral publishes in
// its serial-number descriptor. The chip holds it bare and the descriptor reads back
// coloned, so both spellings must land on one form — the same rule macFromInstance
// applies for Windows.
func normalizeSerialNumber(s string) string {
	s = strings.ToLower(strings.TrimSpace(s))
	if s == "" {
		return ""
	}
	if macBareRe.MatchString(strings.ToUpper(s)) {
		return colonize(s)
	}
	return s
}

// usbInstanceLabel builds the string shown when a port belongs to something that is
// not an orb, so the user reads "Espressif USB JTAG/serial debug unit" rather than a
// bare pair of hex numbers. Display only: the decision lives in the ids.
func usbInstanceLabel(n *plistNode) string {
	name := n.dictString(plistNameKey)
	product := n.dictString(plistProductNameKey)
	if product == "" {
		product = name
	}
	if label := strings.TrimSpace(n.dictString(plistVendorNameKey) + " " + product); label != "" {
		return label
	}
	return fmt.Sprintf("usb-%04x:%04x", mustUint(n, plistVendorKey), mustUint(n, plistProductKey))
}

// mustUint reads an id for a display string, where a missing or malformed value is
// allowed to print as zero. Decisions never use this — they use dictUint and refuse.
func mustUint(n *plistNode, key string) uint16 {
	v, _ := n.dictUint(key)
	return uint16(v)
}
