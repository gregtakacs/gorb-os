//go:build linux

package esprom

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// classifyPort asks sysfs what device sits behind a serial port. It never opens
// the port, so this is the free half of identification: the vendor/product pair
// decides whether a probe may touch it, and the USB serial-number descriptor of
// the ESP32-S3 USB-JTAG peripheral is the chip MAC, spelled
// "44:BD:8D:60:80:88", which labels a cable before anyone pays a reboot.
//
// The path walks up from /sys/class/tty/<name>/device because the ids live on the
// USB device, two levels above the interface node:
//
//	.../usb1/1-1/1-1:1.0   <- device      (no idVendor here)
//	.../usb1/1-1           <- parent      (idVendor, idProduct, serial)
func classifyPort(dev string) USBClass {
	name := filepath.Base(dev)
	if i := strings.LastIndex(name, ":"); i > 0 { // ttyACM0:1 -> ttyACM0
		name = name[:i]
	}
	start, err := filepath.EvalSymlinks(filepath.Join("/sys/class/tty", name, "device"))
	if err != nil {
		return USBClass{}
	}
	for dir := start; ; {
		vendor, okV := readSysfs(dir, "idVendor")
		product, okP := readSysfs(dir, "idProduct")
		if okV && okP {
			serial, _ := readSysfs(dir, "serial")
			manufacturer, _ := readSysfs(dir, "manufacturer")
			productName, _ := readSysfs(dir, "product")
			return USBClass{
				Known:    true,
				Vendor:   uint16(mustHex(vendor)),
				Product:  uint16(mustHex(product)),
				Serial:   strings.ToLower(strings.TrimSpace(serial)),
				Instance: filepath.Base(dir) + "@" + manufacturer + " " + productName,
			}
		}
		parent := filepath.Dir(dir)
		if parent == dir || parent == "/" {
			return USBClass{}
		}
		dir = parent
	}
}

func readSysfs(dir, file string) (string, bool) {
	raw, err := os.ReadFile(filepath.Join(dir, file))
	if err != nil {
		return "", false
	}
	return string(raw), true
}

func mustHex(s string) uint64 {
	v, err := strconv.ParseUint(strings.TrimSpace(s), 16, 16)
	if err != nil {
		return 0
	}
	return v
}
