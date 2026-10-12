//go:build windows

package orbbind

import (
	"errors"
	"os"
	"syscall"
)

// lockPort is the Windows half of the port lock. Windows has no flock, and Go's
// syscall package exposes neither LockFile nor LockFileEx, so the handle itself
// carries the exclusion: CreateFile with dwShareMode 0 asks for the file
// exclusively, and the kernel revokes that as soon as the process dies. A hub
// killed mid-flash therefore cannot leave a cable locked against the next run,
// which is the property the Unix flock implementation has and the reason a
// "does this file exist?" check would be the wrong substitute.
func lockPort(path string) (*os.File, bool, error) {
	h, err := syscall.CreateFile(
		syscall.StringToUTF16Ptr(path),
		syscall.GENERIC_READ|syscall.GENERIC_WRITE,
		0, // no sharing: the second claimer is told the port is busy
		nil,
		syscall.OPEN_ALWAYS,
		syscall.FILE_ATTRIBUTE_NORMAL,
		0,
	)
	if err != nil {
		var errno syscall.Errno
		// 32 is ERROR_SHARING_VIOLATION: another handle owns the file
		// exclusively. The name lives in golang.org/x/sys/windows, which this
		// tool deliberately does not depend on.
		if errors.As(err, &errno) && errno == 32 {
			return nil, false, nil
		}
		return nil, false, err
	}
	return os.NewFile(uintptr(h), path), true, nil
}

// unlockPort drops the exclusion. Closing the handle is the whole of it: the
// kernel releases a share-mode lock with the last handle to the file.
func unlockPort(f *os.File) {
	// Nothing to unwind before Close; kept as a mirror of the Unix path so
	// PortLock.Release has one shape.
}
