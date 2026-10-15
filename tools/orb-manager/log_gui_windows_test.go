//go:build windows

package main

import "testing"

func TestProcAttrsOnWindows(t *testing.T) {
	h, v := hiddenProc(), visibleProc()
	if h == nil || !h.HideWindow {
		t.Fatal("hiddenProc must set HideWindow: a flashing cmd.exe is the second window this change removes")
	}
	if v == nil || v.HideWindow {
		t.Fatal("visibleProc must not hide: the app window itself must reach the desktop")
	}
}

func TestGuiLogNameIsWhatTheReadmeNames(t *testing.T) {
	// The operator-facing promise: the hub log beside the browser log, under this
	// exact name (README "Testing on Windows").
	if want := "orb-manager.log"; guiLogName != want {
		t.Fatalf("guiLogName = %q, want %q: the README names this file", guiLogName, want)
	}
}
