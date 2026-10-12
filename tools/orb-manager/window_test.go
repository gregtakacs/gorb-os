package main

import (
	"bytes"
	"log"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestAppArgsCarriesAppModeFlags(t *testing.T) {
	args := appArgs("http://127.0.0.1:3779", "/cfg/orb-manager/browser-profile")
	want := []string{
		"--app=http://127.0.0.1:3779",
		"--user-data-dir=/cfg/orb-manager/browser-profile",
		"--no-first-run",
		"--no-default-browser-check",
		"--remote-debugging-port=0",
		appWindowGeometry,
	}
	for _, w := range want {
		if !containsString(args, w) {
			t.Errorf("appArgs missing %q in %v", w, args)
		}
	}
	// --class is the Linux/WSLg window label; it must not reach the Windows build.
	if runtime.GOOS == "windows" {
		if containsString(args, "--class="+appWindowClass) {
			t.Error("Windows build must not be passed --class")
		}
		return
	}
	if !containsString(args, "--class="+appWindowClass) {
		t.Errorf("appArgs missing --class in %v", args)
	}
}

func TestAppArgsOmitsEmptyProfile(t *testing.T) {
	if got := appArgs("http://x", ""); hasPrefixIn(got, "--user-data-dir") {
		t.Errorf("empty profile must omit the flag, got %v", got)
	}
}

func TestProfileDirForNonExeCreatesLinuxDir(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("the config-dir behaviour under test is the Unix one")
	}
	cfg := t.TempDir()
	t.Setenv("XDG_CONFIG_HOME", cfg)
	got, err := profileDirFor("/usr/bin/chromium")
	if err != nil {
		t.Fatalf("profileDirFor: %v", err)
	}
	if want := filepath.Join(cfg, "orb-manager", "browser-profile"); got != want {
		t.Errorf("profile dir = %q, want %q", got, want)
	}
	if st, err := os.Stat(got); err != nil || !st.IsDir() {
		t.Errorf("profile dir was not created: %v", err)
	}
}

func TestProfileDirForExeUsesWindowsShell(t *testing.T) {
	// chrome.exe from WSL cannot open a Linux path, so the profile must come
	// from the Windows shell's %LOCALAPPDATA%, not from os.UserConfigDir().
	if _, err := exec.LookPath("cmd.exe"); err != nil {
		t.Skip("no cmd.exe: not a WSL host")
	}
	got, err := profileDirFor("/mnt/c/Program Files/Google/Chrome/Application/chrome.exe")
	if err != nil {
		t.Fatalf("profileDirFor(exe): %v", err)
	}
	if !strings.HasSuffix(got, `\orb-manager\browser-profile`) {
		t.Errorf("windows profile dir = %q, want the \\orb-manager\\browser-profile suffix", got)
	}
	if len(got) < 3 || got[1] != ':' {
		t.Errorf("windows profile dir = %q, want a drive-letter path from %%LOCALAPPDATA%%", got)
	}
	if strings.HasPrefix(got, "/") || strings.Contains(got, "wsl.localhost") {
		t.Errorf("windows profile dir = %q, must be neither a Linux path nor a WSL UNC share", got)
	}
}

func TestFindBrowserPrefersPathThenWSLMount(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("discovery order under test is the Linux one")
	}
	dir := t.TempDir()
	t.Setenv("PATH", dir) // nothing installed on PATH
	if got := findBrowser(); got != "" && !fileExists(got) {
		t.Errorf("findBrowser returned a path that does not exist: %q", got)
	}
	chrome := filepath.Join(dir, "google-chrome")
	if err := os.WriteFile(chrome, []byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	if got := findBrowser(); got != chrome {
		t.Errorf("findBrowser = %q, want the google-chrome just installed at %q", got, chrome)
	}
}

func TestLaunchAppWindowRejectsMissingBrowser(t *testing.T) {
	missing := filepath.Join(t.TempDir(), "not-a-browser")
	if _, err := launchAppWindow("http://127.0.0.1:1", missing); err == nil {
		t.Fatal("expected an error for a browser path that does not exist")
	}
	if _, err := launchAppWindow("http://127.0.0.1:1", "/nonexistent/chromium-does-not-exist"); err == nil {
		t.Fatal("expected an error when no browser can be found")
	}
}

// The lifetime rule is the subtle part: closing the window must shut the hub
// down, but a launcher that hands the URL to an already-running instance and
// returns at once must not. Both look like an exited child, so only the
// duration it stayed alive tells them apart.
func TestLaunchAppWindowSignalsCloseWhenWindowOutlivesGrace(t *testing.T) {
	done, err := launchAppWindow("http://127.0.0.1:9", writeStubBrowser(t, "sleep 4"))
	if err != nil {
		t.Fatalf("launchAppWindow: %v", err)
	}
	select {
	case <-done:
	case <-time.After(9 * time.Second):
		t.Fatal("window close was never signalled")
	}
}

func TestLaunchAppWindowKeepsServingWhenLauncherDetaches(t *testing.T) {
	done, err := launchAppWindow("http://127.0.0.1:9", writeStubBrowser(t, "true"))
	if err != nil { // returns at once: a hand-off, not a close
		t.Fatalf("launchAppWindow: %v", err)
	}
	select {
	case <-done:
		t.Fatal("a detaching launcher must not close the hub")
	case <-time.After(500 * time.Millisecond):
	}
}

func TestLaunchAppWindowPassesTheURLToTheBrowser(t *testing.T) {
	requireShStub(t)
	isolateBrowserEnv(t)
	dir := t.TempDir()
	argsFile := filepath.Join(dir, "args")
	stub := filepath.Join(dir, "browser")
	body := "#!/bin/sh\nprintf '%s\\n' \"$@\" > " + argsFile + "\nsleep 3.4\n"
	if err := os.WriteFile(stub, []byte(body), 0o755); err != nil {
		t.Fatal(err)
	}
	if _, err := launchAppWindow("http://127.0.0.1:4321", stub); err != nil {
		t.Fatalf("launchAppWindow: %v", err)
	}
	time.Sleep(700 * time.Millisecond)
	raw, err := os.ReadFile(argsFile)
	if err != nil {
		t.Fatalf("stub browser never recorded its args: %v", err)
	}
	got := string(raw)
	for _, want := range []string{"--app=http://127.0.0.1:4321", "--no-first-run", "--disable-sync"} {
		if !strings.Contains(got, want) {
			t.Errorf("browser args missing %q, got:\n%s", want, got)
		}
	}
}

// A stock Windows machine has Chrome in Program Files and nothing on PATH
// pointing at it, which is exactly the case the image-name LookPath misses.
// This runs on every OS: winBrowserCandidates is a pure function over the env
// table, so its Windows answer is checkable from Linux.
func TestWinBrowserCandidatesCoverTheStandardInstallRoots(t *testing.T) {
	env := map[string]string{
		"ProgramFiles": `C:\Program Files`,
		"LocalAppData": `C:\Users\greg\AppData\Local`,
	}
	got := winBrowserCandidates(func(k string) string { return env[k] })

	if len(got) != 4 {
		t.Fatalf("candidates = %v, want one Chrome and one Edge under each of the two roots", got)
	}
	// Each root contributes chrome.exe then msedge.exe, in the order the tables
	// are walked. The roots are matched as substrings because the separator the
	// joiner uses is the host's, and this check runs on Linux too.
	checks := []struct{ path, exe, root string }{
		{got[0], "chrome.exe", `C:\Program Files`},
		{got[1], "msedge.exe", `C:\Program Files`},
		{got[2], "chrome.exe", `AppData\Local`},
		{got[3], "msedge.exe", `AppData\Local`},
	}
	for _, c := range checks {
		if b := filepath.Base(c.path); b != c.exe {
			t.Errorf("candidate %q, want an executable named %s", c.path, c.exe)
		}
		if !strings.Contains(c.path, c.root) {
			t.Errorf("candidate %q does not sit under the %s install root", c.path, c.root)
		}
	}
	if c := winBrowserCandidates(func(string) string { return "" }); len(c) != 0 {
		t.Errorf("no install roots must yield no candidates, got %v", c)
	}
}

func TestIsWSLReadsItsEnv(t *testing.T) {
	t.Setenv("WSL_DISTRO_NAME", "Ubuntu")
	if !isWSL() {
		t.Error("WSL_DISTRO_NAME set must report WSL")
	}
}

// --- helpers ----------------------------------------------------------------

// requireShStub skips a test whose stand-in browser is a POSIX shell script.
// CreateProcess cannot exec a #!/bin/sh file, so on Windows the launch and
// lifetime behaviour these tests pin has no stub to drive. It is verified on the
// Linux/WSL side, which is where the hub launches Chrome from in practice; the
// Windows-only halves (install-dir discovery, the profile path, port locking)
// have their own tests that do run there.
func requireShStub(t *testing.T) {
	t.Helper()
	if runtime.GOOS == "windows" {
		t.Skip("stub browser is a /bin/sh script; Windows cannot exec it")
	}
}

// setTempDir points os.TempDir at a fresh dir so a test can inspect a file the
// hub writes there. Unix consults TMPDIR; Windows consults TMP then TEMP.
func setTempDir(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	t.Setenv("TMPDIR", dir)
	t.Setenv("TMP", dir)
	t.Setenv("TEMP", dir)
	return dir
}

func writeStubBrowser(t *testing.T, scriptlet string) string {
	t.Helper()
	requireShStub(t)
	isolateBrowserEnv(t)
	p := filepath.Join(t.TempDir(), "stub-browser")
	if err := os.WriteFile(p, []byte("#!/bin/sh\n"+scriptlet+"\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	return p
}

// isolateBrowserEnv keeps a launch test out of the operator's real config dir
// (a non-.exe stub gets a profile path under UserConfigDir) and collapses the
// DevTools wait so the fallback path is exercised in milliseconds, not seconds.
func isolateBrowserEnv(t *testing.T) {
	t.Helper()
	t.Setenv("XDG_CONFIG_HOME", t.TempDir())
	d := time.Duration(2 * time.Millisecond)
	oldPoll, oldFirst := devtoolsPollEvery, devtoolsFirstWindow
	devtoolsPollEvery, devtoolsFirstWindow = d, 20*time.Millisecond
	t.Cleanup(func() { devtoolsPollEvery, devtoolsFirstWindow = oldPoll, oldFirst })
}

func containsString(hay []string, needle string) bool {
	for _, h := range hay {
		if h == needle {
			return true
		}
	}
	return false
}

func hasPrefixIn(hay []string, prefix string) bool {
	for _, h := range hay {
		if strings.HasPrefix(h, prefix) {
			return true
		}
	}
	return false
}

// --- DevTools window tracking -------------------------------------------------

func stubDevTools(t *testing.T, open func() bool) *httptest.Server {
	t.Helper()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/json/version":
			_, _ = w.Write([]byte(`{"Browser":"Stub"}`))
		case "/json/list":
			if !open() {
				_, _ = w.Write([]byte(`[{"type":"page","url":"http://elsewhere/"}]`))
				return
			}
			_, _ = w.Write([]byte(`[{"type":"page","url":"http://127.0.0.1:3779/web/"},` +
				`{"type":"service_worker","url":"http://127.0.0.1:3779/sw.js"}]`))
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(srv.Close)
	return srv
}

// The Theme Editor card navigates the SAME window to /composer/. That page
// target must still count as our window being open, or the hub would shut down
// while the operator is mid-design and the editor's pushes would have nothing
// to talk to.
func TestWindowTrackerCountsTheComposerPageAsOurWindow(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/json/list" {
			_, _ = w.Write([]byte(`[{"type":"page","url":"http://127.0.0.1:3779/composer/"}]`))
			return
		}
		_, _ = w.Write([]byte(`{"Browser":"Stub"}`))
	}))
	t.Cleanup(srv.Close)
	w := &windowTracker{c: srv.Client(), base: srv.URL, url: "http://127.0.0.1:3779"}
	open, err := w.windowOpen()
	if err != nil {
		t.Fatalf("windowOpen: %v", err)
	}
	if !open {
		t.Error("the editor page on the hub's own origin must keep the window alive")
	}
}

func TestWindowTrackerMatchesOnlyOurPageTarget(t *testing.T) {
	srv := stubDevTools(t, func() bool { return true })
	w := &windowTracker{c: srv.Client(), base: srv.URL, url: "http://127.0.0.1:3779"}
	open, err := w.windowOpen()
	if err != nil {
		t.Fatalf("windowOpen: %v", err)
	}
	if !open {
		t.Error("our page target must read as an open window")
	}
	// A different hub URL must not match, or one hub would watch another's window.
	other := &windowTracker{c: srv.Client(), base: srv.URL, url: "http://127.0.0.1:9999"}
	if open, _ := other.windowOpen(); open {
		t.Error("a target for another URL must not read as our window")
	}
}

func TestWaitForCloseReturnsWhenThePageTargetVanishes(t *testing.T) {
	var open atomic.Bool
	open.Store(true)
	srv := stubDevTools(t, open.Load)
	w := &windowTracker{c: srv.Client(), base: srv.URL, url: "http://127.0.0.1:3779"}
	done := make(chan error, 1)
	go func() { done <- waitForClose(w, 2*time.Millisecond) }()
	time.Sleep(30 * time.Millisecond)
	open.Store(false) // the operator closes the window
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("waitForClose = %v, want nil (a closed window)", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("waitForClose never returned after the target disappeared")
	}
}

func TestNewWindowTrackerReadsChromesPortFile(t *testing.T) {
	srv := stubDevTools(t, func() bool { return true })
	dir := t.TempDir()
	port := strings.TrimPrefix(srv.URL, "http://127.0.0.1:")
	if err := os.WriteFile(filepath.Join(dir, devtoolsPortFile), []byte(port+"\n/devtools/browser/abc\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	w := newWindowTracker(dir, "http://127.0.0.1:3779")
	if w == nil {
		t.Fatal("newWindowTracker = nil, want a tracker for the published port")
	}
	if !w.alive() {
		t.Error("an answering endpoint must report alive")
	}
	if open, err := w.windowOpen(); err != nil || !open {
		t.Errorf("windowOpen = %v, %v; want true, nil", open, err)
	}
}

func TestNewWindowTrackerRejectsUnusablePortFiles(t *testing.T) {
	for _, body := range []string{"", "not-a-port\n", "0\n", "99999\n"} {
		dir := t.TempDir()
		if err := os.WriteFile(filepath.Join(dir, devtoolsPortFile), []byte(body), 0o600); err != nil {
			t.Fatal(err)
		}
		if w := newWindowTracker(dir, "http://x"); w != nil {
			t.Errorf("newWindowTracker for %q = non-nil, want nil", body)
		}
	}
	if w := newWindowTracker("", "http://x"); w != nil {
		t.Error("no profile dir must yield no tracker")
	}
}

func TestAwaitOpenReadsChromesPortFile(t *testing.T) {
	srv := stubDevTools(t, func() bool { return true })
	dir := t.TempDir()
	port := strings.TrimPrefix(srv.URL, "http://127.0.0.1:")
	if err := os.WriteFile(filepath.Join(dir, devtoolsPortFile), []byte(port+"\n/devtools/browser/abc\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	w := awaitOpen(dir, "http://127.0.0.1:3779", 2*time.Second, 2*time.Millisecond)
	if w == nil {
		t.Fatal("awaitOpen = nil, want a tracker for the published port and page")
	}
	if !w.alive() {
		t.Error("an answering endpoint must report alive")
	}
	if open, err := w.windowOpen(); err != nil || !open {
		t.Errorf("windowOpen = %v, %v; want true, nil", open, err)
	}
}

func TestAwaitOpenDiscardsAStalePortFile(t *testing.T) {
	dir := t.TempDir()
	// A port nothing listens on: the browser that wrote it is long gone.
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := strconv.Itoa(ln.Addr().(*net.TCPAddr).Port)
	_ = ln.Close()
	file := filepath.Join(dir, devtoolsPortFile)
	if err := os.WriteFile(file, []byte(port+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if w := awaitOpen(dir, "http://127.0.0.1:3779", 30*time.Millisecond, 2*time.Millisecond); w != nil {
		t.Fatal("a dead browser's port must not be trusted")
	}
	if _, err := os.Stat(file); err == nil {
		t.Error("the stale port file must be removed so a live browser can republish")
	}
}

func TestAwaitOpenTimesOutWhenThePageNeverAppears(t *testing.T) {
	dir := t.TempDir()
	srv := stubDevTools(t, func() bool { return false }) // endpoint answers, no page for us
	port := strings.TrimPrefix(srv.URL, "http://127.0.0.1:")
	if err := os.WriteFile(filepath.Join(dir, devtoolsPortFile), []byte(port+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if w := awaitOpen(dir, "http://127.0.0.1:3779", 30*time.Millisecond, 2*time.Millisecond); w != nil {
		t.Fatal("awaitOpen must not return a tracker for a URL with no page target")
	}
}

// The WSL reality: chrome.exe hands the URL to a running browser, the launcher
// dies at once, and the window lives in a process we never started. The DevTools
// page target is the only handle on it — and closing that window must still
// shut the hub down, which is exactly what the old child-only design missed.
func TestLaunchAppWindowWatchesTheHandedOffWindow(t *testing.T) {
	isolateBrowserEnv(t)
	var open atomic.Bool
	open.Store(true)
	srv := stubDevTools(t, open.Load)

	// Chrome would publish into the profile dir; plant the port file there
	// BEFORE the launch so awaitOpen finds the endpoint the moment it starts.
	cfg, _ := os.UserConfigDir()
	profile := filepath.Join(cfg, filepath.FromSlash(browserProfileRel))
	if err := os.MkdirAll(profile, 0o700); err != nil {
		t.Fatal(err)
	}
	port := strings.TrimPrefix(srv.URL, "http://127.0.0.1:")
	if err := os.WriteFile(filepath.Join(profile, devtoolsPortFile), []byte(port+"\n"), 0o600); err != nil {
		t.Fatalf("plant port file: %v", err)
	}

	// Child exits immediately (a hand-off), so only the page target can decide.
	done, err := launchAppWindow("http://127.0.0.1:3779", writeStubBrowserNoIsolate(t, "true"))
	if err != nil {
		t.Fatalf("launchAppWindow: %v", err)
	}
	select {
	case <-done:
		t.Fatal("hub closed while the page target was still open")
	case <-time.After(150 * time.Millisecond):
	}

	open.Store(false) // the operator closes the handed-off window
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("hub never shut down after the watched page target vanished")
	}
}

// A detached launcher with NOTHING to watch (no endpoint, no page) must keep
// the hub serving — shutting down on a guess would kill a working hub.
func TestLaunchAppWindowDetachedAndUnwatchableKeepsServing(t *testing.T) {
	isolateBrowserEnv(t)
	done, err := launchAppWindow("http://127.0.0.1:3779", writeStubBrowserNoIsolate(t, "true"))
	if err != nil {
		t.Fatalf("launchAppWindow: %v", err)
	}
	select {
	case <-done:
		t.Fatal("an unwatchable detach must not close the hub")
	case <-time.After(2 * time.Second):
	}
}

// A Chromium-family browser narrates its own startup: under WSL a chrome.exe
// prints dozens of named-pipe "Access is denied" and GCM lines unrelated to the
// hub. Inheriting the hub's stderr buried the hub's own one-line story under
// them, so a healthy run looked broken. The narration belongs in a file.
func TestAppWindowKeepsBrowserNoiseOutOfTheHubLog(t *testing.T) {
	dir := setTempDir(t)

	var hubLog bytes.Buffer
	old := log.Writer()
	log.SetOutput(&hubLog)
	t.Cleanup(func() { log.SetOutput(old) })

	const noise = "Failed to open named pipe server process 20688: Access is denied. (0x5)"
	// sleep 4 so the child outlives detachGrace: an early exit would read as a
	// hand-off to a running browser and the hub would keep serving forever.
	stub := writeStubBrowser(t, "echo '"+noise+"' >&2; sleep 4")
	done, err := launchAppWindow("http://127.0.0.1:3779", stub)
	if err != nil {
		t.Fatalf("launchAppWindow: %v", err)
	}
	select {
	case <-done:
	case <-time.After(9 * time.Second):
		t.Fatal("window close was never signalled")
	}

	raw, err := os.ReadFile(filepath.Join(dir, browserLogName))
	if err != nil {
		t.Fatalf("browser log: %v", err)
	}
	if !strings.Contains(string(raw), noise) {
		t.Errorf("browser stderr missing from %s: %q", browserLogName, raw)
	}
	if strings.Contains(hubLog.String(), noise) {
		t.Errorf("browser chatter leaked into the hub log:\n%s", hubLog.String())
	}
	if !strings.Contains(hubLog.String(), browserLogName) {
		t.Errorf("startup line does not tell the operator where the browser log is:\n%s", hubLog.String())
	}
}

func writeStubBrowserNoIsolate(t *testing.T, scriptlet string) string {
	t.Helper()
	requireShStub(t)
	p := filepath.Join(t.TempDir(), "stub-browser")
	if err := os.WriteFile(p, []byte("#!/bin/sh\n"+scriptlet+"\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	return p
}
