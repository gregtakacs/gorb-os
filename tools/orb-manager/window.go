package main

import (
	"errors"
	"io"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"
)

// App-window mode. The hub is an HTTP server first; this file is the part that
// puts it in a window instead of a browser tab, so the operator sees an app
// titled "Orb Manager" rather than a URL on 127.0.0.1.
//
// Chromium-family only, on purpose. The Theme Editor card is the repo's watch
// composer — a Blink canvas app whose file dialogs and localStorage asset store
// are only ever exercised in Chrome (see .clinerules, "Testing the Watch
// Composer"). Hosting the hub in the same engine the composer is verified on
// keeps one Blink in play and lets /composer/ open inside the same window. A
// WebKit-based wrapper would put an untested engine between the user and those
// 712 KB of canvas code, so this tool does not use one.
//
// The browser is found, never bundled: gorb-os ships a static binary the user
// installs nothing to run, and Chrome or Edge is already on every desktop this
// tool targets. When none is found, launch falls back to the plain
// openBrowser path in api.go and the hub still works as a normal web app.

const (
	// browserProfileRel is where app mode keeps its own Chrome profile, under
	// the OS config dir. A dedicated profile is what makes the process we start
	// the one that owns the window: handed the default profile, Chrome hands
	// the URL to an already-running instance and exits at once, and "the window
	// closed" could never be told apart from "the launcher returned".
	browserProfileRel = "orb-manager/browser-profile"

	// appWindowClass sets the X11/WSLg window class and the Windows taskbar
	// group name, so the hub reads as "Orb Manager" in the taskbar, not Chrome.
	appWindowClass = "Orb Manager"

	// appWindowGeometry is generous for the flash log but small enough to fit a
	// 1080p laptop beside other windows.
	appWindowGeometry = "--window-size=1060,820"
)

// browserSentinel names the --browser value that asks for the old default-tab
// behaviour instead of a window, for an operator who wants the browser's own
// devtools and profile.
const browserSentinel = "browser"

// linuxBrowserNames are the Chromium-family commands to look for on the PATH,
// most-preferred first.
var linuxBrowserNames = []string{
	"google-chrome", "google-chrome-stable",
	"chromium", "chromium-browser",
	"microsoft-edge", "microsoft-edge-stable",
	"brave-browser",
}

// wslWindowsBrowsers are the usual Windows install locations, reached through
// the WSL mount. Under WSL the operator's real Chrome lives on the Windows side
// and a Linux Chromium is often not installed at all, so this is the common
// path here rather than an exotic fallback.
var wslWindowsBrowsers = []string{
	"/mnt/c/Program Files/Google/Chrome/Application/chrome.exe",
	"/mnt/c/Program Files (x86)/Google/Chrome/Application/chrome.exe",
	"/mnt/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
	"/mnt/c/Program Files/Microsoft/Edge/Application/msedge.exe",
}

// macBrowsers are the app-bundle executables tried on macOS, where the CLI
// binary inside the bundle is what accepts the --app flag.
var macBrowsers = []string{
	"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
	"/Applications/Chromium.app/Contents/MacOS/Chromium",
	"/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
	"/Applications/Brave Browser.app/Contents/MacOS/Brave Browser",
}

// winImageNames names the Windows binaries by image name, which the loader
// resolves from the Windows PATH.
var winImageNames = []string{"chrome.exe", "msedge.exe"}

// winBrowserSubpaths are the install layouts under a Windows program root:
// where the setups put the executable, and the layout Windows itself exposes.
var winBrowserSubpaths = []struct{ dir, exe string }{
	{`Google\Chrome\Application`, "chrome.exe"},
	{`Microsoft\Edge\Application`, "msedge.exe"},
}

// winInstallEnvVars are the shell variables that name the roots a browser
// installer writes into: the per-machine Program Files pair, plus LocalAppData
// for the per-user installs Chrome offers.
var winInstallEnvVars = []string{"ProgramFiles", "ProgramFiles (x86)", "ProgramFiles(x86)", "LocalAppData"}

// winBrowserCandidates lists the absolute paths a stock Windows install leaves
// behind. It exists because neither Chrome nor Edge adds its own directory to
// PATH: exec.LookPath("chrome.exe") misses both on a machine where the user
// plainly has Chrome, and the hub would fall back to a plain browser tab and
// lose app-window mode over a PATH technicality. A registry read of
// App Paths\chrome.exe would be more exact, and is not worth the extra
// machinery to answer a question these four roots already answer for
// effectively every install.
func winBrowserCandidates(env func(string) string) []string {
	var out []string
	for _, v := range winInstallEnvVars {
		root := env(v)
		if root == "" {
			continue
		}
		for _, b := range winBrowserSubpaths {
			out = append(out, filepath.Join(root, b.dir, b.exe))
		}
	}
	return out
}

// isWSL reports whether the process runs inside Windows Subsystem for Linux.
// /proc/version carries the microsoft kernel tag on WSL1 and WSL2 alike, and
// WSL_DISTRO_NAME covers a kernel that misreports it.
func isWSL() bool {
	if os.Getenv("WSL_DISTRO_NAME") != "" {
		return true
	}
	raw, err := os.ReadFile("/proc/version")
	if err != nil {
		return false
	}
	return strings.Contains(strings.ToLower(string(raw)), "microsoft")
}

// findBrowser returns the first Chromium-family executable worth launching, or
// "" when the host has none and the caller should fall back to openBrowser.
func findBrowser() string {
	switch runtime.GOOS {
	case "windows":
		for _, n := range winImageNames {
			if p, err := exec.LookPath(n); err == nil {
				return p
			}
		}
		// Neither browser installs onto PATH, so the image-name lookup above
		// misses the common case; the standard install roots catch it.
		for _, p := range winBrowserCandidates(os.Getenv) {
			if fileExists(p) {
				return p
			}
		}
		return ""
	case "darwin":
		for _, p := range macBrowsers {
			if fileExists(p) {
				return p
			}
		}
		return ""
	default:
		for _, n := range linuxBrowserNames {
			if p, err := exec.LookPath(n); err == nil {
				return p
			}
		}
		if isWSL() {
			for _, p := range wslWindowsBrowsers {
				if fileExists(p) {
					return p
				}
			}
		}
		return ""
	}
}

func fileExists(p string) bool {
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}

// appArgs builds the Chromium command line for one app window. --app is what
// strips the tabs, the omnibox, and the button row: the window shows only the
// hub, and its title comes from index.html's <title>.
func appArgs(url, profileDir string) []string {
	args := []string{
		"--app=" + url,
		"--no-first-run",
		"--no-default-browser-check",
		"--disable-sync",
		// Chrome picks an ephemeral port and writes it to DevToolsActivePort in
		// the profile dir; that is how the hub can see its own window even when
		// a pre-existing browser process owns it. Chrome binds it to loopback.
		"--remote-debugging-port=0",
		appWindowGeometry,
	}
	if profileDir != "" {
		args = append(args, "--user-data-dir="+profileDir)
	}
	// --class is an X11 affordance: it is what labels the WSLg window and the
	// Linux taskbar entry. The Windows build ignores it and macOS has no notion
	// of it, so passing either way only adds noise.
	if runtime.GOOS != "windows" {
		args = append(args, "--class="+appWindowClass)
	}
	return args
}

// profileDirFor returns a --user-data-dir the chosen browser can open, or "" to
// let it use its default. Launching chrome.exe from inside WSL needs a
// Windows-shaped path: a Linux /tmp path means nothing to it, and a UNC
// \\wsl.localhost path makes Chrome treat the profile as a network share and
// refuse it. Asking the Windows side for its own %LOCALAPPDATA% is the reliable
// answer, so the profile sits beside the user's real Chrome data.
func profileDirFor(browser string) (string, error) {
	if strings.EqualFold(filepath.Ext(browser), ".exe") {
		return winProfileDir()
	}
	base, err := os.UserConfigDir()
	if err != nil {
		return "", err
	}
	dir := filepath.Join(base, filepath.FromSlash(browserProfileRel))
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return dir, err
	}
	return dir, nil
}

// winProfileDir asks the Windows shell for %LOCALAPPDATA% and appends our
// profile directory, for a chrome.exe started from inside WSL.
func winProfileDir() (string, error) {
	csh, err := exec.LookPath("cmd.exe")
	if err != nil {
		return "", errors.New("cmd.exe not on PATH: cannot locate a Windows profile dir")
	}
	cmd := exec.Command(csh, "/c", "echo", "%LOCALAPPDATA%")
	cmd.SysProcAttr = hiddenProc() // probe: never flash a console window
	out, err := cmd.Output()
	if err != nil {
		return "", err
	}
	base := strings.TrimSpace(string(out))
	// An unexpanded "%LOCALAPPDATA%" means the interop shell had no user env;
	// handing Chrome that literal would mint a junk directory beside the exe.
	if base == "" || strings.Contains(base, "%") {
		return "", errors.New("cmd.exe did not expand %LOCALAPPDATA%")
	}
	return base + "\\orb-manager\\browser-profile", nil
}

// browserLogName is where the launched browser's own chatter is kept.
const browserLogName = "orb-manager-chrome.log"

// chromeLog captures the browser's stderr, returning the writer to attach and
// the path to name in the startup line ("" when nothing could be opened).
//
// Chromium talks constantly about itself: a chrome.exe started from WSL prints
// dozens of "Failed to open named pipe server ... Access is denied" and GCM
// registration lines that have nothing to do with the hub. Inheriting the hub's
// stderr dumped all of that into the terminal one second after "orb-manager
// serving", so a healthy run read like a failing one and the real problem, a
// port picker gone blank, was invisible under it. The browser keeps talking, now
// into a file that says where a window went missing.
func chromeLog() (io.Writer, string) {
	path := filepath.Join(os.TempDir(), browserLogName)
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o600)
	if err != nil {
		log.Printf("orb-manager: browser log %s unavailable (%v); discarding browser output", path, err)
		return nil, ""
	}
	return f, path
}

// The belt on a hub whose window it cannot see.
//
// The watcher keeps looking when it cannot see the window, on purpose: closing
// the hub on a guess would kill a working hub, and the 2026-10-15 bench run
// that taught that lesson is written into the code above. What was never
// written down is the terminus. A hand-off launch (the browser process we
// spawned dies at once and the window lives in a process this hub never
// started) whose DevTools endpoint never publishes is blind for the life of the
// process, and closing that window never tells main — the hub serves forever
// with its window gone, which an operator can only stop by killing it.
//
// The page itself is the witness the blind watcher cannot be. While the window
// is open the page asks the hub for something every 5 s (refreshPorts(),
// app.js:3446) and streams events alongside it, and every one of those timers
// dies with the page; the orb is never the one asking, so only a browser with a
// live page moves this clock. A hub blind to its window that has seen no
// inbound request for blindQuitAfter, confirmed by consecutive polls whose
// silence keeps growing, has no page left to protect — and it quits through the
// same guarded path a window close uses, so a live flash is still sheltered and
// a wedged reader still costs only quietQuitBudget.
//
// The trade, stated plainly: a hidden app window whose tab Chrome freezes goes
// quiet the same way a closed one does, and a hub blind to its window cannot
// tell those apart — it trusts the page or it trusts nothing. Arm on blindness
// only: a hub that CAN see its window never hands this decision to the belt.
//
// pageGoneProbe is the seam: main wires it to the server's own clock and the
// lifetime tests drive it by hand. nil (headless, --browser=browser, and every
// test that never sets one) leaves the belt inactive — those paths make no
// promise about a window, so they have nothing to enforce.
var (
	pageGoneProbe  func() time.Duration
	blindQuitAfter = 5 * time.Minute
	blindQuitPoll  = 5 * time.Second
	// blindQuitStreak is how many consecutive polls must agree the silence is
	// still the SAME silence — a reading that shrank is a beat that arrived —
	// so one transient gap cannot read as a closed window, and the poll right
	// after a wake-from-sleep (which still reports the stale long gap) restarts
	// counting at the first fresh beat instead of ending the hub. With
	// blindQuitPoll this is 20 s of confirmed silence past the budget.
	blindQuitStreak = 4
)

// watchPageGone calls shut when the probe says the page is gone for good. It is
// a no-op when no probe is wired. shut is the caller's guarded close, so the
// belt can never double-close against a watcher that decided at the same moment.
// The probe arrives as an argument, not a package-var read inside this
// goroutine: a `go` statement creates no happens-before edge from the
// goroutine's start to the caller's deferred cleanup, so a belt goroutine that
// starts late can read a var a sibling test has already reset. Snapshotting at
// the call site fixes the ordering — and -race caught it the first night.
func watchPageGone(done <-chan struct{}, blind <-chan struct{}, probe func() time.Duration, shut func()) {
	if probe == nil {
		return
	}
	// Arm on the first report that the window cannot be watched. Until then a
	// watcher owns the decision, and a belt with nothing to overrule has no
	// business making it: a hidden tab throttles its own timers, and a page on
	// a sleeping laptop goes quiet while its window is alive.
	select {
	case <-done:
		return
	case <-blind:
	}
	// Snapshot the tunables at arm time, for the reason window_test's detach
	// tests have always had: a lingering watcher must never read package vars a
	// sibling test is rewriting. (It bit this belt under -race the first night
	// it shipped: one test's cleanup wrote blindQuitAfter while this loop read
	// it, nine times, in a suite that was otherwise green.)
	budget, every, need := blindQuitAfter, blindQuitPoll, blindQuitStreak
	tick := time.NewTicker(every)
	defer tick.Stop()
	streak, last := 0, time.Duration(0)
	for {
		select {
		case <-done:
			return
		case <-tick.C:
		}
		q := probe()
		// Growing (or unmoved) means no beat arrived since the last poll; a
		// smaller reading is a page that just asked for something, and the
		// streak restarts. Requiring growth is what keeps a wake-from-sleep
		// from reading as the silence that was already there.
		if q < budget || q < last {
			if q >= budget {
				streak = 1 // past budget but fresh: start counting from here
			} else {
				streak = 0
			}
			last = q
			continue
		}
		streak++
		last = q
		if streak < need {
			continue
		}
		log.Printf("orb-manager: no page has asked for anything in %s and no window is watchable; shutting down",
			q.Round(time.Second))
		shut()
		return
	}
}

// launchAppWindow starts the hub in a chromeless browser window and returns a
// channel closed when that window goes away, so main can shut the server down
// rather than orphan a listener that still holds the serial port lock. A browser
// that detaches instead of owning the window leaves the channel open and the hub
// keeps serving, exactly as it did before app mode existed — with one exception:
// watchPageGone ends that service once the page stops asking for anything.
func launchAppWindow(url, browser string) (<-chan struct{}, error) {
	if browser == "" {
		browser = findBrowser()
	}
	if browser == "" {
		return nil, errors.New("no Chromium-family browser found")
	}
	profile, perr := profileDirFor(browser)
	if perr != nil {
		log.Printf("orb-manager: browser profile dir unavailable (%v); using the browser default", perr)
	}
	cmd := exec.Command(browser, appArgs(url, profile)...)
	cmd.SysProcAttr = visibleProc() // the app window itself must never be suppressed
	browserLog, logPath := chromeLog()
	cmd.Stderr = browserLog
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	if logPath == "" {
		log.Printf("orb-manager: app window %s via %s", url, browser)
	} else {
		log.Printf("orb-manager: app window %s via %s (browser log: %s)", url, browser, logPath)
	}

	// Two watchers race, whichever learns "gone" first wins. The child process
	// covers the ordinary case where chrome.exe we spawned IS the browser. The
	// DevTools page target covers Chrome's single-instance rule, under which a
	// launcher that finds the profile already open hands the URL to a running
	// browser and dies at once — the normal case for chrome.exe under WSL, where
	// the window then lives in a process this hub never started.
	done := make(chan struct{})
	// Three paths can decide the window is gone — the page watcher, the child
	// watcher, and the page-traffic belt — and only one of them may close it.
	var closeOnce sync.Once
	closeDone := func() { close(done) }
	// blind is closed the first time a watcher reports it cannot see the window.
	// It arms the page-traffic belt; see watchPageGone.
	blind := make(chan struct{})
	var blindOnce sync.Once
	seeBlind := func() { blindOnce.Do(func() { close(blind) }) }
	type event int
	const (
		pageClosed   event = iota // the browser says the window is gone
		endpointDead              // endpoint died mid-watch: only fatal if the child is gone too
		unwatchable               // no endpoint/page ever appeared: only the child is trustworthy
		childExit                 // the child died; its lifetime says window vs hand-off
	)
	events := make(chan event, 2)
	started := time.Now()
	// Snapshot the tunables now: lingering watcher goroutines must never read
	// the package vars, which tests rewrite between cases.
	poll, firstWindow, grace := devtoolsPollEvery, devtoolsFirstWindow, detachGrace
	go func() {
		_ = cmd.Wait()
		events <- childExit
	}()
	go func() {
		// Keep looking for the page for the life of the hub. A first probe that
		// finds nothing is normal — chrome.exe under WSL writes its endpoint late
		// and the window can appear after the launcher is already gone — and a
		// watcher that gave up there left the hub serving a window it could never
		// see close, so closing the window cost the operator a Ctrl-C and a lost
		// prompt (bench, 2026-10-15). Each round waits devtoolsFirstWindow and
		// reports once, so the log says "not yet" exactly as many times as it is
		// true and never claims the window is unwatchable for good.
		for {
			select {
			case <-done:
				return
			default:
			}
			t := awaitOpen(profile, url, firstWindow, poll)
			if t == nil {
				seeBlind()
				select {
				case events <- unwatchable:
				case <-done:
					return
				}
				continue
			}
			if err := waitForClose(t, poll); err != nil {
				seeBlind()
				select {
				case events <- endpointDead:
				case <-done:
					return
				}
				continue
			}
			select {
			case events <- pageClosed:
			case <-done:
			}
			return
		}
	}()

	// The belt on the two watchers below: whichever of them cannot see the
	// window is overruled by the page's own traffic, which stops the moment the
	// window closes. The probe is read here, synchronously, so a later test
	// reset cannot reach into a live belt, and watchPageGone does nothing when
	// main never wired one (headless and plain-tab runs make no window promise).
	pageProbe := pageGoneProbe
	go watchPageGone(done, blind, pageProbe, func() { closeOnce.Do(closeDone) })

	go func() {
		childGone, detachSaid := false, false
		for {
			select {
			case <-done: // already decided
				return
			case ev := <-events:
				switch ev {
				case pageClosed:
					log.Println("orb-manager: window closed; shutting down")
					closeOnce.Do(closeDone)
					return
				case childExit:
					childGone = true
					if time.Since(started) >= grace {
						// The child outlived the hand-off window: it was the
						// browser itself, and it just exited with our window.
						log.Println("orb-manager: browser exited; shutting down")
						closeOnce.Do(closeDone)
						return
					}
					// Fast child exit = URL handed to a running browser. Keep
					// serving until that browser's page target goes away.
					log.Printf("orb-manager: %s handed %s to a running browser instance; watching the window there",
						filepath.Base(browser), url)
				case endpointDead:
					if childGone {
						// Both the process we spawned and the browser that
						// owned the endpoint are gone: nothing holds the window.
						log.Println("orb-manager: browser exited; shutting down")
						closeOnce.Do(closeDone)
						return
					}
					// The child is still up: its window may well be too, and a
					// flaky endpoint must not kill a hub mid-flash. The child's
					// own exit will decide.
				case unwatchable:
					// Detached and blind, so there is nothing to shut down on
					// yet — and NOT a reason to stop looking. The watcher keeps
					// asking, and this line is said once rather than every round,
					// because a hub that prints "still can't see it" forever
					// teaches the operator to ignore the line that matters.
					if childGone && !detachSaid {
						detachSaid = true
						log.Println("orb-manager: browser detached with no watchable window yet; still looking - this hub exits when that window closes, or stop it now with Ctrl-C")
					}
				}
			}
		}
	}()
	return done, nil
}
