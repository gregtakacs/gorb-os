//go:build !linux && !windows

package esprom

// classifyPort has no implementation for this platform yet: the host cannot say
// what device a serial port belongs to, so an identity probe is refused rather
// than allowed to toggle DTR/RTS on hardware nobody has checked. macOS is the
// case this covers, and it is deliberately the conservative answer.
func classifyPort(dev string) USBClass { return USBClass{} }
