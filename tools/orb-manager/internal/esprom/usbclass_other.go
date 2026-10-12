//go:build !linux && !windows && !darwin

package esprom

// classifyPort has no implementation for this platform yet: the host cannot say
// what device a serial port belongs to, so an identity probe is refused rather
// than allowed to toggle DTR/RTS on hardware nobody has checked. BSDs are the
// case this covers now that darwin has its own classifier, and it is deliberately
// the conservative answer.
func classifyPort(dev string) USBClass { return USBClass{} }
