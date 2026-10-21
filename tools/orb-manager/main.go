// Package main is the orb-manager server: a single static binary that serves
// the hub UI, reverse-proxies the orb's on-device API to kill CORS, and drives
// the native Go flasher. The orb's LAN address comes from its own console over
// USB (the provisioning lease, or the boot banner the flash captures), from the
// orb's own mDNS announcement (discover.go: listed, never dialled), or from the
// operator. The hub never sweeps a subnet and never sniffs the neighbour table.
// End users install nothing: the UI is embedded and the serial library is
// compiled in. By default it opens in a chromeless Chromium app window (see
// window.go); --no-window serves the same UI as a plain web app instead.
package main

import (
	"context"
	"embed"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

//go:embed web
var webFS embed.FS

const (
	listenPort  = 3779
	scanTimeout = 400 * time.Millisecond
	// deviceTimeout is the budget for one management call (status, tz, ap,
	// reboot). The orb's lwIP httpd is a single poll thread behind the render
	// loop, so first byte can exceed the scan's aggressive 400 ms; the proxy
	// must not fail a deliberate user click the way a sweep tolerates a miss.
	deviceTimeout = 8 * time.Second
	// otaTimeout is the budget for one whole app-image upload. The device
	// erases and writes flash with the cache disabled (orb_net.c h_ota), and
	// a ~2 MB image over LAN is minutes of patient streaming, not a call. The
	// 8 s management budget would kill every honest update at the first flush.
	otaTimeout = 180 * time.Second
)

// version is stamped at build time via -ldflags "-X main.version=..." (see the
// Makefile), carrying `git describe --always --dirty` for the tree it was built
// from. It stays "dev" for an untagged `go build`.
var version = "dev"

// Server holds the shared clients and the in-flight operation registry.
type Server struct {
	device *orbnet.Client // proxy/management-tuned: a busy orb's httpd can idle >0.4 s before first byte
	orb    *orbnet.Client // status-probe tuned: short timeout for a single known IP
	// ota is the upload-tuned client: app images to /api/v1/ota and .orbtheme
	// containers to /api/v1/sd/theme are minutes of flash and card writes, and
	// reusing the 8 s management client would cancel every honest transfer
	// mid-body.
	ota     *orbnet.Client
	repoDir string // the gorb-os clone root, for the Theme Editor only
	// ops is the per-port operation registry: who holds which cable, doing what,
	// and how to ask that work to stop. It replaces the process-wide flashing
	// flag, which could not tell two cables apart or cancel anything.
	ops *opRegistry
	// fw is the firmware to flash: the bundle compiled in, replaced only by a
	// file the user picks on the Onboard card. There is no build-dir fallback —
	// an end user has no ESP-IDF and never will.
	fmu sync.RWMutex
	fw  *firmware.Bundle

	// identities caches what each port turned out to be, because a probe reboots
	// the orb and the answer must not be paid for twice.
	identities *identityCache
	// probeOrb is the reboot itself, held as a field so the launch sweep (launchprobe.go)
	// is testable without a chip on the wire. main leaves it at esprom.ProbeOrb; a test
	// scripts the answers the bench would give.
	probeOrb func(ctx context.Context, port string) (esprom.OrbInfo, error)

	// token is this launch's control secret: minted once in main, handed to the
	// page it serves, and required on every request that changes something. See
	// security.go for why the loopback bind is not the thing that makes this safe.
	token string
	// grants are the orb addresses the proxy is willing to dial. Empty means the
	// hub reaches nothing off the box.
	grants *ipGrants
	// fleet is the hub's address book: {mac, ip, port, cached name},
	// keyed on the chip and never on the lease. It is consulted before a dial, not
	// instead of one — the grant still decides whether an address may be reached at
	// all, and the fleet only ever takes a dial away (see mayDial).
	fleet *orbfleet.Store
	// orbIP mirrors the address the onboarding flow last learned, so the status
	// probe has a default target before the browser supplies one. It is a UI
	// convenience: the proxy decides with grants, never with this field.
	imu sync.Mutex
	// cmu guards read: the MACs this run has confirmed off a chip. See chipRead.
	cmu  sync.Mutex
	read map[string]bool
	// kickMu guards lastPlug: the orb MACs the previous cable sweep attributed, which
	// is what lets kickLostCables (liveness.go) tell an unplug from a steady bench and
	// re-dial the unplugged board's remembered address the second the lead goes quiet.
	kickMu   sync.Mutex
	lastPlug map[string]bool
	// suspicious guards the MACs whose wired dial went quiet, pending one paid probe. See
	// noteWiredSilence (launchprobe.go): silence is grounds to ask, never grounds to claim.
	suspMu     sync.Mutex
	suspicious map[string]bool
	// owed is the silence escalation for the identity pass currently running: MACs whose
	// wired dial went quiet and whose question is therefore due regardless of how fresh the
	// cached answer looks. Set and cleared inside one identifyCables pass, so it needs no
	// lock and lives only as long as that pass.
	owed map[string]bool
	// lonely remembers the boards this hub personally left lonely. The cable
	// sweep re-attributes a lonely board's descriptor MAC like any orb's, so
	// without this mark the forgotten fleet row would rebuild itself from the
	// sighting and the board would read as an orb again seconds after its
	// lonely flash. Evidence of orb life — an orb flash, an announcement, a
	// serial ack — clears the mark. See lonelyset.go.
	lonely *lonelyMark
	orbIP  string
	// statePath is where learned orb addresses are remembered across launches
	// (state.go). stateErr holds the last failure writing or reading it, so a hub
	// that cannot persist says so once in the log instead of silently forgetting.
	statePath string
	stateErr  error
	pmu       sync.Mutex

	// browse is the _orb._tcp discovery cache, started by main and not by newHub:
	// the suite builds servers by the dozen, and one of them holding UDP 5353 makes
	// the next one's bind a coin flip. dmu guards it and nothing else, because the
	// browse runs on its own goroutine against a Server that is otherwise single-
	// gowned-per-field.
	dmu          sync.Mutex
	browse       discovery
	browseErr    string
	browseIfaces []string

	// cables is the USB half of the one list (roster.go). Like the browse, its source is
	// wired by main and not by newHub, so a test hub does not pay for a sysfs read it did
	// not ask for, and a test that wants cable rows injects the sweep rather than lying
	// about the machine it runs on.
	cables *cableCache

	// prover is the throttle and the seam behind the free cable read
	// (cableproof.go): the PING the hub sends itself down a lead it already owns, so
	// an orb on a cable becomes a card with no reboot, no packet and no dial. Its
	// round trip is wired by main beside enableCables, for the cables reason: the
	// suite must never open a serial device it did not ask to open.
	prover *cableProof

	// live is the answer to "does the address this row remembers still answer as an
	// orb", cached so the discovery card never dials on the path that draws it. It is
	// what makes Forget an offer only for a board that cannot come straight back; see
	// liveness.go. Its probe is wired by main, not by newHub, for the cables reason.
	live *liveCache

	// orbSubs are the open /api/orbs/stream connections, each with a one-slot wake-up
	// channel. omu guards this map and nothing else: the browser fires the wake on its own
	// reader goroutine while the handler that owns an entry is parked in a select, and the
	// lock order across the two has to stay one-way — a handler may take dmu through
	// orbCard, so the wake path must never take dmu to reach a subscriber.
	omu     sync.Mutex
	orbSubs map[chan struct{}]struct{}

	// boots holds the verdicts the flash session read off the boot console while
	// it held the port across its own HardReset, keyed by the chip MAC. /api/watch
	// consumes them: they are the only trustworthy record of whether the orb came
	// up on its provisioning AP or joined the LAN, since a later probe cannot reset
	// a chip whose app already owns the (wire-less) USB-Serial-JTAG port. Keyed by
	// MAC and not one slot, because flashing two orbs in a row is a normal bench
	// afternoon and the first verdict must survive the second.
	bmu   sync.Mutex
	boots map[string]*bootVerdict
}

// bootVerdict is the one-shot console conclusion a flash captured for a MAC.
// networks carries the PROVSCAN rows seen on the boot console while the orb sat
// in its first-boot provisioning AP, so the wired door can show a picker without
// the hub ever reaching the AP's 192.168.4.1. ip carries the DHCP lease the orb
// announced on that same console ("associated: config page at http://<ip>/"),
// so a station that self-joined at boot is known by address without any LAN
// discovery.
type bootVerdict struct {
	mac        string
	apSSID     string
	associated bool
	ip         string
	networks   []esprom.ScanNet
	at         time.Time
}

// bootFreshWindow bounds how long a recorded verdict still describes the boot
// /api/watch is following. Past it the orb has had time to do anything, and a
// stale verdict presented as a current one would send the UI down the wrong
// branch of the onboarding flow.
const bootFreshWindow = 90 * time.Second

// setBoot records this orb's boot verdict, leaving every other orb's verdict
// alone. The key is the chip MAC the flash read from eFuse, so a second flash on a
// second cable cannot overwrite the first one's answer — which is what the old
// single slot did.
func (s *Server) setBoot(mac, apSSID string, associated bool, ip string, networks []esprom.ScanNet) {
	mac = macNorm(mac)
	if mac == "" {
		return // a verdict with no MAC to answer for is not addressable later
	}
	s.bmu.Lock()
	defer s.bmu.Unlock()
	if s.boots == nil {
		s.boots = map[string]*bootVerdict{}
	}
	s.boots[mac] = &bootVerdict{mac: mac, apSSID: apSSID, associated: associated, ip: ip, networks: networks, at: time.Now()}
	for k, v := range s.boots {
		if time.Since(v.at) > bootFreshWindow {
			delete(s.boots, k) // the map is written on every flash; it must not grow forever
		}
	}
}

// forgetBoot drops a recorded boot verdict. A wipe leaves nothing to have a
// verdict about: the one-shot answer a flash captured describes a firmware that
// is no longer on the chip, and /api/watch serving it would send onboarding down
// a branch for an orb that just went blank.
func (s *Server) forgetBoot(mac string) {
	mac = macNorm(mac)
	if mac == "" {
		return
	}
	s.bmu.Lock()
	defer s.bmu.Unlock()
	delete(s.boots, mac)
}

// bootFor returns the recorded verdict for mac if it is recent enough to still
// describe the boot this watch call is following.
func (s *Server) bootFor(mac string) *bootVerdict {
	mac = macNorm(mac)
	s.bmu.Lock()
	defer s.bmu.Unlock()
	v, ok := s.boots[mac]
	if !ok {
		return nil
	}
	if time.Since(v.at) > bootFreshWindow {
		delete(s.boots, mac)
		return nil
	}
	copied := *v
	return &copied
}

func main() {
	var addr, browser, repo, fwPath, orbPort string
	var noWindow, allowRemote, allowForeignFirmware, probeCablesOnLaunch bool

	flag.StringVar(&addr, "addr", fmt.Sprintf("127.0.0.1:%d", listenPort), "host:port to bind")
	flag.StringVar(&repo, "repo", "", "path to the gorb-os clone root, for the Theme Editor; empty detects it")
	flag.StringVar(&fwPath, "firmware", "", "a firmware bundle (.zip) or whole-flash image (.bin) to flash instead of the one built into this binary")
	flag.BoolVar(&allowForeignFirmware, "allow-foreign", false, "accept a firmware whose image descriptor names a project other than gorb-os; with -firmware it applies to that file, and picked files still need the confirm on the card")
	flag.StringVar(&browser, "browser", "", "path to a Chromium/Edge binary; empty autodetects, \"browser\" opens a normal tab")
	flag.BoolVar(&noWindow, "no-window", false, "serve only and print the URL (bench, SSH, CI)")
	flag.BoolVar(&allowRemote, "allow-remote", false, "permit -addr to bind somewhere other than loopback; every control route still needs this launch's token")
	flag.StringVar(&orbPort, "orb-port", "80", "TCP port the hub dials on an orb; the device's httpd listens on 80, and only a bench run against cmd/orbfake has a reason to change it")
	flag.BoolVar(&probeCablesOnLaunch, "probe-cables", true, "at launch, identify every board on an attributable USB lead by resetting it and reading its bootloader, so the hub never decides from a MAC which firmware a cable carries; costs each wired board one reboot per hub start, and a board with no orb on it is otherwise drawn as one forever")
	flag.Parse()

	// Windowed mode on Windows can start with no console (double-click on a
	// -H windowsgui build), and log output must not die with it; a console
	// launch prints on unchanged. See log_gui_windows.go. --no-window keeps
	// stderr even console-less: its contract is the printed URL.
	if !noWindow {
		redirectLogsForGUI()
	}

	// Loopback is the whole of the hub's exposure story, so binding off it has to
	// be a decision someone typed. See security.go: a page on the browser's own
	// machine can already reach 127.0.0.1, so the address is not what makes the
	// control surface safe — the token and the origin check are. Binding a
	// flasher to a LAN or to 0.0.0.0 without saying so is the part worth refusing.
	if !isLoopbackHost(addr) && !allowRemote {
		host := addr
		if h, _, err := net.SplitHostPort(addr); err == nil {
			host = h
		}
		log.Fatalf("orb-manager: refusing to bind %s: that is not a loopback address, and this tool holds a flasher."+
			"\n  Use 127.0.0.1, or pass -allow-remote if you really mean to expose the hub on %s.", addr, host)
	}
	if !isLoopbackHost(addr) {
		log.Printf("orb-manager: exposed on %s at operator request - mutating routes still require the per-launch token, but anyone on that network can reach the login-free UI", addr)
	}

	repoDir := strings.TrimSpace(repo)
	if repoDir == "" {
		repoDir = detectRepoDir()
	}

	// One construction site, shared with the tests through newHub(), so a field added
	// to Server cannot be wired in one and forgotten in the other — which is how the
	// boot-verdict map nearly shipped nil in the real binary.
	s := newHub()
	s.orb = orbnet.NewClient(scanTimeout)
	s.device = orbnet.NewClient(deviceTimeout)
	s.ota = orbnet.NewClient(otaTimeout)
	s.repoDir = repoDir

	// The dial port is validated here rather than at first use, so a typo surfaces as
	// a startup message instead of as every orb timing out.
	if orbPort == "" {
		orbPort = "80"
	}
	if _, err := net.LookupPort("tcp", orbPort); err != nil {
		log.Fatalf("orb-manager: -orb-port %q is not a TCP port: %v", orbPort, err)
	}
	s.orb.DevicePort = orbPort
	s.device.DevicePort = orbPort
	s.ota.DevicePort = orbPort
	if orbPort != "80" {
		log.Printf("orb-manager: dialing orbs on port %s (the device's own httpd uses 80; this is a bench setting)", orbPort)
	}

	// Remembered before the first request is served: an operator who found their orb
	// last session should not have to type its address into this one, and the proxy
	// will not dial an address the hub has not learned.
	if n := s.armState(); n > 0 {
		log.Printf("orb-manager: remembered %d orb address(es) from a previous run", n)
	} else if s.stateErr != nil {
		log.Printf("orb-manager: orb address history unavailable (%v); starting with none remembered", s.stateErr)
	}

	// Discovery starts before the first request is served, so the Manage card has
	// something to show on the open-orb path and the operator never types an
	// address the LAN already announced. A failure here is a line in the log and an
	// empty list with a reason: the hub still flashes, still themes, still manages.
	listenCtx, stopListen := context.WithCancel(context.Background())
	defer stopListen()
	s.startListening(listenCtx)

	// The cable half of the one list (roster.go). Enabled here rather than in newHub so
	// the suite never pays for a sysfs read, and watched on the same shutdown as
	// discovery: a plug-in reaches an open stream inside half the sweep TTL instead of
	// waiting for an mDNS packet that a cable-only board will never send.
	s.enableCables(hostCableSweep, s.wakeOrbs)
	// The free cable read rides the same sweep and the same shutdown: a board on a
	// lead becomes a card here rather than by waiting for a packet it will never send
	// or paying for a reboot it does not need. cableproof.go is the whole argument.
	s.enableCableProof()
	cableCtx, stopCables := context.WithCancel(context.Background())
	defer stopCables()
	go s.watchCables(cableCtx)
	// Once before the first tick, so a hub started with a cable already in it is not
	// three minutes late to its own bench.
	go s.proveCables(cableCtx)
	// And the paid question, once, before the card is believed (launchprobe.go). The
	// free read above can only ever say a board IS an orb: it is a PING, and a PING
	// that says nothing is indistinguishable from an asleep orb. Only a reboot reads
	// the bootloader and can say a board on this lead is running something else, which
	// is the fact the descriptor cannot supply and the one a reflash invalidates.
	if probeCablesOnLaunch {
		go s.probeCablesAtLaunch(cableCtx)
	}

	// The liveness verdicts Forget is gated on (liveness.go). The probe is the same
	// status fetch the post-flash watch uses — one GET, orb-shaped answer or nothing —
	// and it is wired here rather than in newHub so the suite never dials a LAN. The
	// sweeper shares the cable context's shutdown for the same reason the cable watcher
	// does: both exist to keep an open card current, and both are pointless without one.
	//
	// The body is not discarded. Every ten seconds and once at launch this hub already
	// pays for a full /api/v1/status from every remembered address; fw, state and the
	// board's own name come back in it, and the tile used to wait up to a full announce
	// interval for facts it was already holding. Throwing them away was the bug;
	// foldProbeStatus is the ten lines that stopped it. A dial renews facts, never the
	// presence clock — the Forget gate still reads announces only.
	s.live.probe = func(ip string) bool {
		st, err := s.orb.Status(ip)
		if err != nil {
			// A board on a lead that stops answering the door it used to answer is the
			// only free sign that something was written to it without the hub's
			// knowledge. Silence is not a verdict — a boot-loop and a loose cable look
			// identical to a reflash — so it buys no claim about the board. It buys one
			// question, and that question is what makes the button unnecessary for a
			// plugged-in board reflashed by ESP-IDF. See noteWiredSilence.
			s.noteWiredSilence(ip)
			return false
		}
		s.foldProbeStatus(ip, st)
		return true
	}
	go s.probeLiveness(cableCtx)
	// Once at startup, before the first card is worth looking at: the file already says
	// which boards the hub knows, and this turns those addresses into answers.
	go s.probeNow(cableCtx)

	// Firmware is settled before anything is served: an Onboard card that would
	// flash nothing is worse than a hub that refuses to start.
	if err := s.startFirmware(strings.TrimSpace(fwPath), allowForeignFirmware); err != nil {
		log.Fatalf("orb-manager: %v", err)
	}

	mux := http.NewServeMux()
	s.routes(mux)

	// Bind before opening anything, so the window is handed the address that is
	// really listening — the default port can be taken by a stale hub, and a
	// window pointed at a port nobody holds is just an error page.
	ln, url := bindHub(addr)
	log.Printf("orb-manager %s on %s (firmware: %s)", version, url, s.firmwareName())

	srv := &http.Server{Handler: s.log(mux)}
	browser = strings.TrimSpace(browser)
	switch {
	case noWindow:
		log.Printf("orb-manager: headless - open %s in a browser", url)
	case strings.EqualFold(browser, browserSentinel):
		// --browser=browser: the pre-app-window behaviour, a plain tab in the
		// default browser, kept for the devtools and profile it brings along.
		go openBrowser(url)
	default:
		done, err := launchAppWindow(url, browser)
		if err != nil {
			// No Chromium, an unreadable profile dir, or an exec failure: the
			// hub is still a web app, so degrade to the old default-browser
			// path rather than refusing to serve.
			log.Printf("orb-manager: app window unavailable (%v); falling back to the default browser", err)
			go openBrowser(url)
		} else {
			go func() {
				// launchAppWindow already said why the window is gone. Closing
				// it must not pull the wire out from under a live flash, so this
				// goes through the same guarded quit the interrupt path uses.
				<-done
				s.quit(srv)
			}()
		}
	}
	s.watchSignals(srv)

	if err := srv.Serve(ln); err != nil && err != http.ErrServerClosed {
		log.Fatalf("listen: %v", err)
	}
}

// opQuitBudget bounds how long a quit waits for a live port operation to land on
// its feet. A full-manifest flash writes the 1.85 MB art partition too, so minutes
// is the right order; past it a wedged operation must not make the hub unquittable.
const opQuitBudget = 5 * time.Minute

// quit releases the listener, giving any live port operation the chance to stop at
// a block boundary first. Window-close and Ctrl-C both land here. The order is the
// point: srv.Shutdown waits a bounded 3 s and then returns, and main returning ends
// the process — which mid-write is exactly how an orb is left half-flashed.
func (s *Server) quit(srv *http.Server) {
	if live := s.ops.running(); len(live) > 0 {
		log.Printf("orb-manager: %d port operation(s) live (%s); waiting up to %s for them to finish - interrupt again to cancel and quit",
			len(live), opSummary(live), opQuitBudget)
		ctx, cancel := context.WithTimeout(context.Background(), opQuitBudget)
		defer cancel()
		if err := s.ops.waitIdle(ctx); err != nil {
			log.Printf("orb-manager: %d operation(s) still live after %s; quitting anyway",
				len(s.ops.running()), opQuitBudget)
		}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_ = srv.Shutdown(ctx)
}

// watchSignals makes Ctrl-C mean "quit when nothing is writing", and a second
// interrupt mean "cancel what is writing and quit now". The first interrupt does
// not kill a flash; the second one cancels it between blocks and lets the process
// end with the flasher's own cleanup run, not with the process yanked mid-write.
func (s *Server) watchSignals(srv *http.Server) {
	ch := make(chan os.Signal, 2)
	signal.Notify(ch, os.Interrupt)
	go func() {
		<-ch
		s.quit(srv)
		<-ch
		if live := s.ops.cancelAll(); len(live) > 0 {
			log.Printf("orb-manager: forced quit - cancelled %d operation(s): %s", len(live), opSummary(live))
		}
		os.Exit(1)
	}()
}

// opSummary names the live operations for a log line.
func opSummary(ops []*op) string {
	parts := make([]string, 0, len(ops))
	for _, o := range ops {
		parts = append(parts, fmt.Sprintf("%s:%s", o.Kind, o.Port))
	}
	return strings.Join(parts, ", ")
}

// bindHub listens on addr, falling back to an ephemeral port when it is taken so
// a second hub instance opens its own window instead of dying on listen. It
// returns the listener and the URL to hand a browser.
func bindHub(addr string) (net.Listener, string) {
	ln, err := net.Listen("tcp", addr)
	if err == nil {
		return ln, "http://" + ln.Addr().String()
	}
	host, _, _ := strings.Cut(addr, ":")
	ln2, err2 := net.Listen("tcp", host+":0")
	if err2 != nil {
		log.Fatalf("listen %s: %v", addr, err)
	}
	log.Printf("orb-manager: %s is busy (%v); using %s instead", addr, err, ln2.Addr())
	return ln2, "http://" + ln2.Addr().String()
}

// detectRepoDir locates the gorb-os clone root so the manager finds build/ and
// the embedded Theme Editor's source without the user pointing at them.
//
// git answers this exactly, and cheaply, wherever git exists. It is not
// installed on every machine that will run this: a Windows box that only ever
// flashed through WSL has no git on PATH, and the old fallback of "just use the
// working directory" then handed the hub tools/orb-manager as the clone root, so
// /composer/ 404'd and the build plan came back missing. Falling back to a walk
// up from a real marker is what makes the same binary work there.
func detectRepoDir() string {
	if out, err := exec.Command("git", "rev-parse", "--show-toplevel").Output(); err == nil {
		if top := strings.TrimSpace(string(out)); top != "" {
			return top
		}
	}
	var starts []string
	if cwd, err := os.Getwd(); err == nil {
		starts = append(starts, cwd)
	}
	// The binary itself is a second vantage: double-clicking orb-manager.exe out
	// of the clone is a normal way to start it, and then cwd is wherever Windows
	// decided to launch it from.
	if exe, err := os.Executable(); err == nil {
		starts = append(starts, filepath.Dir(exe))
	}
	for _, s := range starts {
		if dir := repoFrom(s); dir != "" {
			return dir
		}
	}
	return ""
}

// repoMarkerRel is the file that says "this directory is the gorb-os clone": the
// Theme Editor's entry point, which the hub serves straight out of the tree and
// so cannot do without.
func repoMarkerRel() string { return filepath.Join(composerDirRel, composerEntry) }

// repoFrom walks up from start until it finds the clone marker, returning "" if
// the whole chain has none.
func repoFrom(start string) string {
	marker := repoMarkerRel()
	for dir := filepath.Clean(start); ; {
		if fileExists(filepath.Join(dir, marker)) {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return ""
		}
		dir = parent
	}
}

func orElse(s, alt string) string {
	if s == "" {
		return alt
	}
	return s
}

func (s *Server) log(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		h.ServeHTTP(w, r)
		if !strings.HasPrefix(r.URL.Path, "/web/") {
			log.Printf("%s %s -> %s", r.Method, r.URL.Path, time.Since(start).Round(time.Millisecond))
		}
	})
}

func (s *Server) routes(mux *http.ServeMux) {
	sub, err := fs.Sub(webFS, "web")
	if err != nil {
		log.Fatalf("embed web: %v", err)
	}
	// Static assets only. The document itself comes from handleIndex, which is the
	// one place that knows this launch's token.
	mux.Handle("/web/", http.StripPrefix("/web/", http.FileServer(http.FS(sub))))
	mux.HandleFunc("/", s.handleIndex)

	mux.HandleFunc("/api/status", s.json(s.hello))
	mux.HandleFunc("/api/ports", s.handlePorts) // cached identity rides along, never a probe
	mux.HandleFunc("/api/tz", s.handleZones)    // the device's zone table, for the join dialog's picker
	mux.HandleFunc("/api/build", s.json(s.describeFirmware))
	mux.HandleFunc("/api/ops", s.handleOps)
	mux.Handle("/api/orb/allow", s.requireControl(http.HandlerFunc(s.handleAllowIP)))
	mux.Handle("/api/orb/forget", s.requireControl(http.HandlerFunc(s.handleForgetIP)))
	// The typed-address door and the OTA door (adopt.go). Both mutate the hub's
	// willingness to dial, so both stand behind the launch token like every
	// other hand on the grant set.
	mux.Handle("/api/orb/add", s.requireControl(http.HandlerFunc(s.handleOrbAdd)))
	mux.Handle("/api/orb/update", s.requireControl(http.HandlerFunc(s.handleOrbUpdate)))
	mux.HandleFunc("/api/orb/known", s.handleRememberedIP)

	// Discovery. The list is a read of what the hub overheard, so it is unguarded
	// like /api/ports; asking the LAN again is not an action this hub takes. The
	// candidate list is a record of what arrived unasked, and the proxy still
	// decides with grants: nothing here is a dial queue.
	mux.HandleFunc("/api/orbs", s.handleDiscovered)

	// The same list, pushed: opened once by the page, held until the page goes, and
	// answered again whenever a check-in changes what the card says. Unguarded like the
	// read, and for the same reason — every frame is a byte for byte copy of what that
	// read answers, so a token here would protect nothing the poll left open while
	// costing the reconnect path its header. It costs a held goroutine per connection,
	// which is why the bind stays loopback by default and the remote bind is opt-in
	// behind a warning; a page that stops reading is dropped by the write failing, not
	// by a queue growing.
	mux.HandleFunc("/api/orbs/stream", s.handleOrbsStream)

	// The fleet: who the hub thinks is behind the addresses it will dial. The read is
	// unguarded like the candidate list; the writes change what a later credential is
	// keyed to, so they carry the token and the origin check.
	mux.HandleFunc("/api/fleet", s.handleFleet)
	mux.Handle("/api/fleet/set", s.requireControl(http.HandlerFunc(s.handleFleetSet)))
	mux.Handle("/api/fleet/forget", s.requireControl(http.HandlerFunc(s.handleFleetForget)))

	// Theme management for the Manage card. Each of these dials the orb itself, so
	// each passes the same learned-address gate the proxy uses, and each names a
	// route main/net/orb_net.c actually registers. They are separate endpoints rather
	// than /orb/ forwards so the theme name is checked before a byte leaves the box,
	// and so the orb `id` that docs/theme-identity-plan.md adds later has exactly one
	// module to land in (internal/orbnet/themes.go).
	mux.Handle("/api/theme/list", s.requireControl(http.HandlerFunc(s.handleThemeList)))
	mux.Handle("/api/theme/install", s.requireControl(http.HandlerFunc(s.handleThemeInstall)))
	mux.Handle("/api/theme/flip", s.requireControl(http.HandlerFunc(s.handleThemeFlip)))
	mux.Handle("/api/theme/default", s.requireControl(http.HandlerFunc(s.handleThemeDefault)))
	mux.Handle("/api/theme/delete", s.requireControl(http.HandlerFunc(s.handleThemeDelete)))
	mux.Handle("/api/theme/rescan", s.requireControl(http.HandlerFunc(s.handleThemeRescan)))
	mux.Handle("/api/theme/upload", s.requireControl(http.HandlerFunc(s.handleThemeUpload)))

	// Everything below changes something: it resets a chip, writes a flash, drives
	// the device, or puts a file in the repo. requireControl is the whole reason a
	// page the operator never opened cannot do those things from a browser that
	// can already reach this port. See security.go.
	mux.Handle("/api/firmware", s.requireControl(http.HandlerFunc(s.handleFirmwareUpload)))
	mux.Handle("/api/firmware/reset", s.requireControl(http.HandlerFunc(s.handleFirmwareReset)))
	mux.Handle("/api/flash", s.requireControl(http.HandlerFunc(s.handleFlash))) // streaming, not s.json
	// There is no /api/wipe on this hub. The page's one end-of-orb-life door is
	// the lonely write below: it wipes the same settings a chip erase wiped, and
	// leaves a board that boots and explains itself instead of a blank chip
	// boot-looping its port. The ROM erase lives in the headless CLI.
	// The lonely image — the one destructive write the page offers, and the
	// same class of write: it ends a board's orb life and takes the fleet row,
	// the name and every credential with it. Same guard, same streaming
	// contract, its own typed word.
	mux.Handle("/api/lonely", s.requireControl(http.HandlerFunc(s.handleLonely)))
	// Cancellation and live-operation state. Cancel stops the flash at its next
	// block boundary; /api/ops says which cable is busy with what, which a
	// process-wide flag could never answer once two orbs shared a bench.
	mux.Handle("/api/flash/cancel", s.requireControl(http.HandlerFunc(s.handleCancel)))
	mux.Handle("/api/watch", s.requireControl(http.HandlerFunc(s.handleWatch)))         // streaming post-flash probe
	mux.Handle("/api/provision", s.requireControl(http.HandlerFunc(s.handleProvision))) // streaming wired-provisioning round-trip
	// Generic serial command door (name/scan/mode/reset/ping) on a configured orb:
	// the wired twin of the fleet card's write, for a board with no reachable address.
	mux.Handle("/api/orb/cmd", s.requireControl(http.HandlerFunc(s.handleOrbCmd)))
	// Read-only USB console: a port operation, because holding the cable to read it
	// must exclude a flash for the same reason writing one does.
	mux.Handle("/api/console", s.requireControl(http.HandlerFunc(s.handleConsole)))

	// Same-origin proxy for the orb's device API. The browser only ever talks
	// to us; we reach the orb. This is what the missing CORS headers force.
	// Guarded like the rest: half of the device API it exposes writes to the orb
	// (/wifi/join, /config, /reboot), and the page driving it is served by us.
	mux.Handle("/orb/", s.requireControl(http.HandlerFunc(s.proxyOrb)))

	// Theme Editor: the watch-composer, served from the repo.
	s.routesComposer(mux)
}

// indexMarker is the QUOTED placeholder in web/index.html that receives the launch
// token. The quotes are part of the match on purpose: replacing the bare identifier
// would rewrite `window.__ORB_TOKEN__` itself, leaving the client reading a property
// that no longer exists and sending no token at all. Replaced at serve time rather
// than baked into the embed, so the token stays per-launch and the committed HTML
// never carries a secret.
const indexMarker = `"__ORB_TOKEN__"`

func (s *Server) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	page, err := webFS.ReadFile("web/index.html")
	if err != nil {
		http.Error(w, "embedded UI missing: "+err.Error(), http.StatusInternalServerError)
		return
	}
	// Two channels, same secret: the cookie is what the browser attaches to every
	// request it makes from this origin — including the composer page, whose JS is
	// vendor bytes this repo must not edit — and the inline assignment is what the
	// hub's own client sends explicitly. A cross-site page can neither read the
	// HttpOnly cookie nor forge the header on a simple form POST.
	setLaunchToken(w, s.token)
	body := strings.ReplaceAll(string(page), indexMarker, strconv.Quote(s.token))
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	_, _ = w.Write([]byte(body))
}
