//go:build !windows

package main

import "syscall"

// The one-window machinery is Windows-only: on other platforms nothing links
// -H windowsgui, no console ever appears beside the app window, and log output
// already goes wherever the process was started. The spawn attributes are no-ops
// there too (see log_gui_windows.go for why the fields themselves are Windows-only).

func redirectLogsForGUI()               {}
func hiddenProc() *syscall.SysProcAttr  { return nil }
func visibleProc() *syscall.SysProcAttr { return nil }
