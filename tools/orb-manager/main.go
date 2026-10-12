// Package main is the orb-manager server: a single static binary that serves
// the hub UI, reverse-proxies the orb's on-device API to kill CORS, sweeps the
// household /24 for orbs, and drives the native Go flasher. End users install
// nothing — the UI is embedded and the serial library is compiled in.
package main

import (
	"embed"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
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
	orb      *orbnet.Client // scan-tuned: short timeout so a /24 sweep stays fast
	device   *orbnet.Client // proxy/management-tuned: a busy orb's httpd can idle >0.4 s before first byte
	repoDir  string         // the gorb-os clone root, for the stamp audit
	buildDir string         // <repoDir>/build, where flasher_args.json lives

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
// the hub ever reaching the AP's 192.168.4.1.
type bootVerdict struct {
	mac        string
	apSSID     string
	associated bool
	networks   []esprom.ScanNet
	at         time.Time
}

// setBoot records the newest boot verdict, overwriting any prior one.
func (s *Server) setBoot(mac, apSSID string, associated bool, networks []esprom.ScanNet) {
	s.bmu.Lock()
	s.lastBoot = &bootVerdict{mac: mac, apSSID: apSSID, associated: associated, networks: networks, at: time.Now()}
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
	var addr, buildDir string
	flag.StringVar(&addr, "addr", fmt.Sprintf("127.0.0.1:%d", listenPort), "host:port to bind")
	flag.StringVar(&buildDir, "build", "", "path to a gorb-os build dir (defaults to <repo>/build)")
	flag.Parse()

	repoDir := detectRepoDir()
	if buildDir == "" && repoDir != "" {
		buildDir = filepath.Join(repoDir, "build")
	}

	s := &Server{orb: orbnet.NewClient(scanTimeout), device: orbnet.NewClient(deviceTimeout), repoDir: repoDir, buildDir: buildDir}

	mux := http.NewServeMux()
	s.routes(mux)

	url := "http://" + addr
	log.Printf("orb-manager %s on %s (build dir: %s)", version, url, orElse(buildDir, "not detected"))
	go openBrowser(url)
	if err := http.ListenAndServe(addr, s.log(mux)); err != nil {
		log.Fatalf("listen: %v", err)
	}
}

// detectRepoDir asks git for the toplevel of the working directory so the
// manager finds .orb-target and build/ without the user pointing at them.
func detectRepoDir() string {
	cwd, err := os.Getwd()
	if err != nil {
		return ""
	}
	if out, err := exec.Command("git", "rev-parse", "--show-toplevel").Output(); err == nil {
		return strings.TrimSpace(string(out))
	}
	return cwd
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
	mux.HandleFunc("/api/build", s.json(s.describeBuild))
	mux.HandleFunc("/api/scan", s.json(s.scan))
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
