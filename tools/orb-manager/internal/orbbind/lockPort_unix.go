//go:build linux || darwin || freebsd || netbsd || openbsd || dragonfly

package orbbind

import (
	"errors"
	"os"
	"syscall"
)

// lockPort takes the kernel's exclusive advisory lock on path, creating the
// file if it is absent. flock drops automatically when the process dies, so a
// hub killed mid-flash cannot leave the cable locked against the next run.
func lockPort(path string) (*os.File, bool, error) {
	f, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0o644)
	if err != nil {
		return nil, false, err
	}
	if err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		f.Close()
		// EWOULDBLOCK (EAGAIN on Linux) is the contention answer: someone else
		// holds it. Any other errno is a real failure, not a busy port, and
		// saying so keeps a broken filesystem from reading as "port in use".
		if errors.Is(err, syscall.EWOULDBLOCK) {
			return nil, false, nil
		}
		return nil, false, err
	}
	return f, true, nil
}

func unlockPort(f *os.File) {
	_ = syscall.Flock(int(f.Fd()), syscall.LOCK_UN)
}
