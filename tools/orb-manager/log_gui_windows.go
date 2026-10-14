//go:build windows

package main

import (
	"log"
	"os"
	"path/filepath"
	"syscall"
)

// One-window launch. The shipped Windows build is linked -H windowsgui (make
// windows-gui, and release), so no console appears beside the app window: a
// console-subsystem exe (PE subsystem 3, what plain `make windows` emits) makes
// Windows allocate the black terminal a desktop user double-clicking an app has
// no reason to ever see. This file is the other half of that change — where the
// log goes once the console is gone.
//
// A GUI-subsystem process launched by Explorer has no console at all, and the
// startup line carries the log paths an operator needs when no window shows up.
// So such a launch redirects log, stdout and stderr to one file beside the
// browser log (chromeLog in window.go). A console-launched run keeps printing
// where it was started from: cmd.exe attaches its console to GUI binaries too,
// so the bench and the cmd test flow are unaffected by the linker flag.
//
// main calls this only in windowed mode: --no-window's contract is "print the
// URL", and a scheduled task capturing stderr must still see it.
// guiLogName is the hub's own log file for console-less launches.
const guiLogName = "orb-manager.log"

// AttachConsole/FreeConsole are kernel32 calls, and Go's stdlib syscall exports
// neither (nor does this project carry x/sys — vendor-check forbids a third-party
// import for two calls). LazyDLL resolves them at first use off the known-good
// system DLL, the same mechanism the stdlib's own windows syscalls are built on.
var (
	kernel32                           = syscall.NewLazyDLL("kernel32.dll")
	procAttachConsole, procFreeConsole = kernel32.NewProc("AttachConsole"), kernel32.NewProc("FreeConsole")
)

const (
	// attachParentProcess asks the kernel for the console of our spawner.
	attachParentProcess = 0xffffffff // ATTACH_PARENT_PROCESS
)

// attachConsole reports the Win32 bool return and, on a false, the reason: the
// proc call's error IS GetLastError, the same contract syscall.Syscall uses.
func attachConsole(console uint32) error {
	if r, _, e := procAttachConsole.Call(uintptr(console)); r == 0 {
		return e
	}
	return nil
}

func freeConsole() { _, _, _ = procFreeConsole.Call() }

// redirectLogsForGUI moves all log output to the per-user temp log when this
// process was launched with no console of its own. Call it in main as soon as
// flags are parsed, before anything logs.
//
// The probe is measured, not assumed: a real double-click (verified through
// wscript, a console-less spawner) fails AttachConsole with ERROR_INVALID_HANDLE,
// while cmd.exe and a bash session under WSL interop succeed, because the
// spawner has a console to hand. Any failure is therefore treated as
// console-less — after the FreeConsole above there is no stream left that could
// still carry output, so the file is the only destination that loses nothing.
// On a console launch the probe re-detaches and prints go to the terminal that
// started us, exactly as before.
func redirectLogsForGUI() {
	freeConsole()
	if attachConsole(attachParentProcess) == nil {
		freeConsole() // probe attached it; the default streams already print there
		return
	}
	path := filepath.Join(os.TempDir(), guiLogName)
	f, ferr := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o600)
	if ferr != nil {
		return // nowhere to redirect, nowhere to complain: keep defaults
	}
	log.SetOutput(f)
	os.Stdout, os.Stderr = f, f
	log.Printf("orb-manager: console-less launch; this log and all future output append to %s", path)
}

// hiddenProc stops a helper child (cmd.exe probing %LOCALAPPDATA%, rundll32 for
// the fallback browser) from flashing its own console window. The field is
// windows-only, so it lives here rather than at the spawn sites.
func hiddenProc() *syscall.SysProcAttr { return &syscall.SysProcAttr{HideWindow: true} }

// visibleProc is the same field spelled false for the app window's own browser
// child: the window must reach the desktop, and under WSL interop a hidden flag
// on that spawn path has bitten this flow before.
func visibleProc() *syscall.SysProcAttr { return &syscall.SysProcAttr{HideWindow: false} }
