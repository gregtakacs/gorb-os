package main

import (
	"encoding/json"
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
)

// One hub per operator, ever. Two hubs are two dial gates: each serializes only
// its own dials, so one orb gets two big streams at once, its single-task httpd
// wedges for ~30 s, and the other hub reads that silence as a dead board and
// resets it over the cable (bench, 2026-10-31 — a stale -no-window hub on :3779
// beside a fresh launch that had quietly fallen back to an ephemeral port). The
// lane, the warm pass and every probe verdict assume they are the only hub, so
// the rule is enforced at the door rather than patched behind it.
//
// The claim is a kernel lock (orbbind.TryLockFile) beside state.json, taken
// before anything dials an orb or touches a cable, and held for the life of the
// process. The address a second launch needs lives in a separate file, because
// on Windows the locked file cannot be read by anyone else.

const (
	instanceLockName = "hub.lock"
	instanceInfoName = "hub.json"
)

// hubClaim holds main's claim for the life of the process. It is a package
// variable on purpose: an *os.File nothing references is closed by its
// finalizer, and closing the lock file is releasing the lock.
var hubClaim *hubInstance

// hubInstance is this process's claim. Its file must stay open: closing it is
// releasing the lock.
type hubInstance struct {
	lock *os.File
	info string
}

// runningHub is what the holder announced: where its page is, and who it is.
type runningHub struct {
	URL string `json:"url"`
	PID int    `json:"pid"`
}

// instanceDir is where the claim lives: state.json's own directory, so the one
// hub and its remembered fleet are found in the same place.
func instanceDir() (string, error) {
	p, err := statePath()
	if err != nil {
		return "", err
	}
	return filepath.Dir(p), nil
}

// claimInstance takes the one-hub lock in dir. held=false means another hub
// holds it, and other is whatever that hub announced — empty URL while it is
// still starting up.
func claimInstance(dir string) (inst *hubInstance, other runningHub, held bool, err error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, other, false, err
	}
	info := filepath.Join(dir, instanceInfoName)
	f, held, err := orbbind.TryLockFile(filepath.Join(dir, instanceLockName))
	if err != nil {
		return nil, other, false, err
	}
	if !held {
		if b, rerr := os.ReadFile(info); rerr == nil {
			_ = json.Unmarshal(b, &other)
		}
		return nil, other, false, nil
	}
	// Whatever a previous holder announced died with it; a launch that loses the
	// race to this one must not be sent to a page nobody serves.
	_ = os.Remove(info)
	return &hubInstance{lock: f, info: info}, other, true, nil
}

// announce publishes this hub's page address for the next launch to find.
func (h *hubInstance) announce(url string) {
	b, _ := json.Marshal(runningHub{URL: url, PID: os.Getpid()})
	tmp := h.info + ".tmp"
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		log.Printf("orb-manager: could not record this hub's address for a second launch: %v", err)
		return
	}
	if err := os.Rename(tmp, h.info); err != nil {
		log.Printf("orb-manager: could not record this hub's address for a second launch: %v", err)
	}
}

// release drops the claim. Process exit does the same; this is for tests.
func (h *hubInstance) release() {
	_ = os.Remove(h.info)
	_ = h.lock.Close()
}

// describe names the running hub for a refusal line.
func (r runningHub) describe() string {
	switch {
	case r.URL != "" && r.PID > 0:
		return fmt.Sprintf("at %s (pid %d)", r.URL, r.PID)
	case r.URL != "":
		return "at " + r.URL
	case r.PID > 0:
		return fmt.Sprintf("(pid %d, still starting)", r.PID)
	}
	return "(still starting)"
}

// showRunningHub is a second launch's whole job: point the operator at the hub
// that already runs, then leave. A windowed launch opens that hub's page — a
// double-click should land somewhere useful, not on an error — and a headless one
// says where it is. Either way this process serves nothing and dials nothing.
func showRunningHub(other runningHub, noWindow bool, browser string) {
	if noWindow || other.URL == "" {
		log.Fatalf("orb-manager: another hub is already running %s; only one may run at a time. "+
			"Use that one, or stop it first.", other.describe())
	}
	log.Printf("orb-manager: a hub is already running %s; opening its page instead of starting a second", other.describe())
	browser = strings.TrimSpace(browser)
	if strings.EqualFold(browser, browserSentinel) {
		openBrowser(other.URL)
		os.Exit(0)
	}
	if browser == "" {
		browser = findBrowser()
	}
	if browser != "" {
		profile, _ := profileDirFor(browser)
		cmd := exec.Command(browser, appArgs(other.URL, profile)...)
		cmd.SysProcAttr = visibleProc()
		if cmd.Start() == nil {
			os.Exit(0)
		}
	}
	openBrowser(other.URL)
	os.Exit(0)
}
