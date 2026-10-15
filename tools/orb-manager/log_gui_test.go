package main

import (
	"runtime"
	"testing"
)

// onWindowsTest is the one GOOS check shared by the shims' tests; the windows
// variants live in log_gui_windows_test.go, so this file must not import
// syscall (the SysProcAttr fields differ per platform).
func onWindowsTest() bool { return runtime.GOOS == "windows" }

// The one-window shims are three lines each, but they are the lines the console
// window's absence depends on, and crosscheck only proves they compile. The real
// launch behaviour (PE subsystem, console probe) is measured on Windows by hand;
// here we pin the contract the spawn sites rely on.

func TestHiddenVisibleProcAreNilOffWindows(t *testing.T) {
	if onWindowsTest() {
		t.Skip("field presence differs per platform; windows covers it in TestProcAttrsOnWindows")
	}
	if hiddenProc() != nil || visibleProc() != nil {
		t.Fatal("non-Windows must pass no SysProcAttr: the fields are windows-only and nil is the honest default")
	}
}

func TestRedirectIsInertOffWindows(t *testing.T) {
	if onWindowsTest() {
		t.Skip("the console probe needs a Windows console host")
	}
	redirectLogsForGUI() // must not panic, must not touch the logger
}
