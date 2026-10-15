package main

import (
	"encoding/json"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// --- window lifetime through the browser's own view of it --------------------
//
// Waiting on the child process is not enough. Chrome is single-instance per
// profile: a launcher that finds the profile already open hands the URL to that
// browser process and exits at once, so the window we are supposed to follow
// lives in a process this hub never started — the normal case for chrome.exe
// under WSL, where the interop launcher detaches on every run after the first.
//
// The browser itself knows. With --remote-debugging-port=0 it publishes the
// chosen port in DevToolsActivePort under the profile dir and serves a target
// list over HTTP. One target per open window is ground truth: our URL is in the
// list while the window is up and absent once it is closed, whichever process
// owns that window.

// devtoolsPortFile is the file Chrome writes inside --user-data-dir holding the
// ephemeral debugging port on its first line.
const devtoolsPortFile = "DevToolsActivePort"

var (
	// Tunables are vars so the lifetime tests can drive them without sleeping
	// for seconds at a time.
	devtoolsPollEvery   = 500 * time.Millisecond
	devtoolsFirstWindow = 20 * time.Second
	detachGrace         = 3 * time.Second
)

// windowTracker answers "is our page still open?" from the browser's target
// list. A nil tracker means the endpoint is unavailable and the caller must
// fall back to watching the child process alone.
type windowTracker struct {
	c    *http.Client
	base string // http://127.0.0.1:<port>
	url  string // the hub URL the app window was opened at
}

// newWindowTracker reads the port Chrome wrote for this profile dir, returning
// nil when no usable DevTools endpoint is published there.
func newWindowTracker(profileDir, url string) *windowTracker {
	if profileDir == "" {
		return nil
	}
	raw, err := os.ReadFile(filepath.Join(profileDir, devtoolsPortFile))
	if err != nil {
		return nil
	}
	// First line is the port, second the browser websocket path.
	line, _, _ := strings.Cut(strings.TrimSpace(string(raw)), "\n")
	port, err := strconv.Atoi(strings.TrimSpace(line))
	if err != nil || port <= 0 || port > 65535 {
		return nil
	}
	return &windowTracker{
		c:    &http.Client{Timeout: 2 * time.Second},
		base: "http://127.0.0.1:" + strconv.Itoa(port),
		url:  url,
	}
}

// windowOpen reports whether the browser still lists a page target for our URL.
// The bool is only meaningful when err is nil; an error means the endpoint is
// not answering, which is the absence of evidence, not a closed window.
func (w *windowTracker) windowOpen() (bool, error) {
	resp, err := w.c.Get(w.base + "/json/list")
	if err != nil {
		return false, err
	}
	defer resp.Body.Close()
	var targets []struct {
		Type string `json:"type"`
		URL  string `json:"url"`
	}
	if err := json.NewDecoder(io.LimitReader(resp.Body, 1<<20)).Decode(&targets); err != nil {
		return false, err
	}
	for _, t := range targets {
		if t.Type == "page" && strings.HasPrefix(t.URL, w.url) {
			return true, nil
		}
	}
	return false, nil
}

// alive reports whether this endpoint belongs to a browser that is actually
// running right now.
func (w *windowTracker) alive() bool {
	resp, err := w.c.Get(w.base + "/json/version")
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	return resp.StatusCode == http.StatusOK
}

// awaitOpen returns a tracker whose endpoint answers AND whose page target for
// url has appeared, polling every `every` up to budget d for Chrome to start. A
// stale file from a browser that has since died is discarded; a live one is
// kept, because in the hand-off case it belongs to the already-running browser
// our URL was passed to, and that port is the only record of the process
// holding our window.
func awaitOpen(profileDir, url string, d, every time.Duration) *windowTracker {
	deadline := time.Now().Add(d)
	for {
		t := newWindowTracker(profileDir, url)
		if t != nil && t.alive() {
			if open, err := t.windowOpen(); err == nil && open {
				return t
			}
		}
		if t != nil && !t.alive() {
			clearDevToolsPort(profileDir) // dead browser: let the live one republish
		}
		if !time.Now().Before(deadline) {
			return nil
		}
		time.Sleep(every)
	}
}

// waitForClose polls every `every` until the page target is gone. It returns
// nil when the window closed, or an error when the endpoint stopped answering —
// the browser exiting outright, not a transient miss, since one dropped request
// while a flash is in flight must not read as a closed window.
func waitForClose(t *windowTracker, every time.Duration) error {
	misses := 0
	for {
		open, err := t.windowOpen()
		if err == nil {
			misses = 0
			if !open {
				return nil
			}
		} else {
			misses++
			if misses >= 6 {
				return err
			}
		}
		time.Sleep(every)
	}
}

// clearDevToolsPort removes the file a previous hub run left behind, so a stale
// port from a dead browser can never be mistaken for a live one.
func clearDevToolsPort(profileDir string) {
	if profileDir == "" {
		return
	}
	_ = os.Remove(filepath.Join(profileDir, devtoolsPortFile))
}
