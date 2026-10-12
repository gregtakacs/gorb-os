// Package main is the orb-manager server: a single static binary that serves
// the hub UI, reverse-proxies the orb's on-device API to kill CORS, and drives
// the native Go flasher. The orb's LAN address is learned from its own console
// over USB (the provisioning lease, or the boot banner the flash captures) or
// typed in by the operator — the hub never sweeps a subnet or sniffs ARP. End
// users install nothing: the UI is embedded and the serial library is compiled
// in. By default it opens in a chromeless Chromium app window (see window.go);
// --no-window serves the same UI as a plain web app instead.
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
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
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
)

// version is stamped at build time via -ldflags "-X main.version=..." (see the
// Makefile), carrying `git describe --always --dirty` for the tree it was built
// from. It stays "dev" for an untagged `go build`.
var version = "dev"

// Server holds the shared clients and the in-flight flash guard.
type Server struct {
	mu       sync.Mutex
	flashing bool           // one flash at a time, process-wide (belt to the flock)
	orb      *orbnet.Client // status-probe tuned: short timeout for a single known IP
	device   *orbnet.Client // proxy/management-tuned: a busy orb's httpd can idle >0.4 s before first byte
	repoDir  string         // the gorb-os clone root, for the Theme Editor only
	// fw is the firmware to flash: the bundle compiled in, replaced only by a
	// file the user picks on the Onboard card. There is no build-dir fallback —
	// an end user has no ESP-IDF and never will.
	fmu sync.RWMutex
	fw  *firmware.Bundle

	// lastBoot holds the verdict the flash session read off the boot console
	// while it held the port across its own HardReset. /api/watch consumes it:
	// it is the only trustworthy record of whether the orb came up on its
	// provisioning AP or joined the LAN, since a later probe cannot reset a chip
	// whose app already owns the (wire-less) USB-Serial-JTAG port.
	bmu      sync.Mutex
	lastBoot *bootVerdict
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

// setBoot records the newest boot verdict, overwriting any prior one.
func (s *Server) setBoot(mac, apSSID string, associated bool, ip string, networks []esprom.ScanNet) {
	s.bmu.Lock()
	s.lastBoot = &bootVerdict{mac: mac, apSSID: apSSID, associated: associated, ip: ip, networks: networks, at: time.Now()}
	s.bmu.Unlock()
}

// bootFor returns the recorded verdict if it names mac and is recent enough to
// still describe the boot this watch call is following.
func (s *Server) bootFor(mac string) *bootVerdict {
	s.bmu.Lock()
	defer s.bmu.Unlock()
	if s.lastBoot == nil || s.lastBoot.mac != mac {
		return nil
	}
	if time.Since(s.lastBoot.at) > 90*time.Second {
		return nil
	}
	v := *s.lastBoot
	return &v
}

func main() {
	var addr, browser, repo, fwPath string
	var noWindow bool
	flag.StringVar(&addr, "addr", fmt.Sprintf("127.0.0.1:%d", listenPort), "host:port to bind")
	flag.StringVar(&repo, "repo", "", "path to the gorb-os clone root, for the Theme Editor; empty detects it")
	flag.StringVar(&fwPath, "firmware", "", "a firmware bundle (.zip) to flash instead of the one built into this binary")
	flag.StringVar(&browser, "browser", "", "path to a Chromium/Edge binary; empty autodetects, \"browser\" opens a normal tab")
	flag.BoolVar(&noWindow, "no-window", false, "serve only and print the URL (bench, SSH, CI)")
	flag.Parse()

	repoDir := strings.TrimSpace(repo)
	if repoDir == "" {
		repoDir = detectRepoDir()
	}

	s := &Server{orb: orbnet.NewClient(scanTimeout), device: orbnet.NewClient(deviceTimeout), repoDir: repoDir}

	// Firmware is settled before anything is served: an Onboard card that would
	// flash nothing is worse than a hub that refuses to start.
	if err := s.startFirmware(strings.TrimSpace(fwPath)); err != nil {
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
		log.Printf("orb-manager: headless — open %s in a browser", url)
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
				// launchAppWindow already said why the window is gone; all
				// that is left is releasing the listener it was guarding.
				<-done
				ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
				defer cancel()
				_ = srv.Shutdown(ctx)
			}()
		}
	}

	if err := srv.Serve(ln); err != nil && err != http.ErrServerClosed {
		log.Fatalf("listen: %v", err)
	}
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
	mux.Handle("/web/", http.StripPrefix("/web/", http.FileServer(http.FS(sub))))
	mux.HandleFunc("/", s.handleIndex)

	mux.HandleFunc("/api/status", s.json(s.hello))
	mux.HandleFunc("/api/ports", s.json(s.listPorts))
	mux.HandleFunc("/api/build", s.json(s.describeFirmware))
	// Firmware the user picks, uploaded rather than named: the hub never walks
	// the filesystem looking for a build, so there is no traversal to get wrong.
	mux.HandleFunc("/api/firmware", s.handleFirmwareUpload)
	mux.HandleFunc("/api/firmware/reset", s.handleFirmwareReset)
	mux.HandleFunc("/api/flash", s.handleFlash)         // streaming, not s.json
	mux.HandleFunc("/api/watch", s.handleWatch)         // streaming post-flash probe
	mux.HandleFunc("/api/provision", s.handleProvision) // streaming wired-provisioning round-trip

	// Same-origin proxy for the orb's device API. The browser only ever talks
	// to us; we reach the orb. This is what the missing CORS headers force.
	mux.HandleFunc("/orb/", s.proxyOrb)

	// Theme Editor: the watch-composer, served from the repo.
	s.routesComposer(mux)
}

func (s *Server) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	// /web/ serves index.html via FileServer; pointing at it directly avoids the
	// extra hop FileServer adds when handed .../index.html outright.
	http.Redirect(w, r, "/web/", http.StatusFound)
}
