package main

import (
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// deviceHub points a fresh hub's device client at a fake that answers the two doors verifySeed
// reads: /api/v1/apps (the ledger) and /api/v1/sd (the theme card count). appsJSON and sdJSON are
// the exact bodies the fake serves, so a test says what the dial said and nothing else.
func deviceHub(t *testing.T, appsJSON, sdJSON string) (*Server, *[]string) {
	t.Helper()
	var paths []string
	h := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		paths = append(paths, r.URL.Path)
		w.Header().Set("Content-Type", "application/json")
		switch r.URL.Path {
		case "/api/v1/apps":
			w.Write([]byte(appsJSON))
		case "/api/v1/sd":
			w.Write([]byte(sdJSON))
		default:
			w.Write([]byte(`{}`))
		}
	})
	srv := httptest.NewServer(h)
	t.Cleanup(srv.Close)
	s := newHub()
	s.device = orbnet.NewClient(deviceTimeout)
	s.device.DevicePort = portOf(srv.URL)
	return s, &paths
}

const (
	goodApps = `{"mounted":true,"apps":[{"stem":"clock","ok":true}],` +
		`"installed":[{"slug":"clock","enabled":true}]}`
	goodSD = `{"mounted":true,"card_count":1,"active":"skeleton"}`
)

// A ledger that claims a clock row but has no card theme behind it is exactly the board the owner
// reported: Apps says installed, the glass shows no face. verifySeed must refuse that, not bless it.
func TestVerifySeedRefusesTheFacelessRow(t *testing.T) {
	s, _ := deviceHub(t, goodApps, `{"mounted":true,"card_count":0,"active":"Aviator"}`)
	err := s.verifySeedRun(context.Background(), "127.0.0.1", func(installStep) {})
	if err == nil {
		t.Fatal("a clock with no theme on the card verified, which is the false-green this gate exists to stop")
	}
}

// An installed-and-disabled row is symptom 2: the row stands, the enable bit is off, nothing draws.
func TestVerifySeedRefusesTheDisabledRow(t *testing.T) {
	s, _ := deviceHub(t,
		`{"mounted":true,"apps":[{"stem":"clock","ok":true}],`+
			`"installed":[{"slug":"clock","enabled":false}]}`, goodSD)
	err := s.verifySeedRun(context.Background(), "127.0.0.1", func(installStep) {})
	if err == nil {
		t.Fatal("a disabled clock row verified; it cannot be on the glass while switched off")
	}
}

// The whole point of the fix: a clean board with an enabled clock and a card theme verifies.
func TestVerifySeedAcceptsAGreenBoard(t *testing.T) {
	s, _ := deviceHub(t, goodApps, goodSD)
	seen := ""
	err := s.verifySeedRun(context.Background(), "127.0.0.1", func(st installStep) { seen = st.Action })
	if err != nil {
		t.Fatalf("a seeded board was refused: %v", err)
	}
	if seen != "verified" {
		t.Errorf("the run reported %q, not a verified step", seen)
	}
}

// liveIPforMAC is the answer "Open this orb" asks after the theme reboot moved the lease. The MAC
// is stable, so a fleet row that re-announced at a new address must win over what the dialog holds.
func TestWhereResolvesTheChipToItsLiveAddress(t *testing.T) {
	s := newHub()
	if _, err := s.fleet.Observe(orbfleet.Observation{
		MAC: "44bd8d608088", Instance: "orb-608088", IP: "192.168.2.32",
		FW: installMinFW, State: "connected", At: time.Now(),
	}); err != nil {
		t.Fatalf("observing the fleet row: %v", err)
	}
	if got := s.liveIPforMAC("44:BD:8D:60:80:88"); got != "192.168.2.32" {
		t.Errorf("a dashed MAC resolved to %q, not the dashed-free fleet row", got)
	}
	if got := s.liveIPforMAC("44bd8d608088"); got != "192.168.2.32" {
		t.Errorf("the bare MAC resolved to %q", got)
	}
	if got := s.liveIPforMAC("aabbccddee00"); got != "" {
		t.Errorf("an unknown chip resolved to %q; no row must answer empty", got)
	}
}

// hollowApps is the live Bronze's own /api/v1/apps body from 2026-10-29, verbatim in shape: a
// compiled menu, a clock row switched off with zero card files behind it. The old shelf gate
// counted that hollow row as "what the owner put there", the automatic arm skipped to done,
// and the page headline claimed the glass wore a clock. A hollow, disabled row is leftover,
// not curatorship: the gate must answer false so the ladder refills the promise.
const hollowApps = `{"mounted":true,"apps":[],"installed":[` +
	`{"slug":"menu","title":"Menu","enabled":true,"failed":false,"walkable":true,"on_glass":true,"compiled":true},` +
	`{"slug":"clock","title":"Clock","enabled":false,"failed":false,"walkable":true,"on_glass":false,"code_size":0,"serial":10129,"version":"v1.1.29"}]}`

func TestShelfGateRefusesToCountTheHollowRow(t *testing.T) {
	s, _ := deviceHub(t, hollowApps, `{"mounted":true,"card_count":0,"active":"Skeleton","themes":[]}`)
	held, err := s.shelfHoldsOwnersContent("127.0.0.1")
	if err != nil {
		t.Fatalf("asking the shelf: %v", err)
	}
	if held {
		t.Fatal("a disabled row over an empty card counted as the owner's shelf; that is the exact body the skip-lie was fed")
	}
}

// The rule the hollow-row test must not break: a clock row that is enabled and has its card
// copy present IS the owner's doing (or a finished seed's), and the automatic arm still
// stands down before it — earning its done by verify rather than by the row's bare existence.
func TestShelfGateHonoursARealShelf(t *testing.T) {
	s, _ := deviceHub(t, goodApps, goodSD)
	held, err := s.shelfHoldsOwnersContent("127.0.0.1")
	if err != nil {
		t.Fatalf("asking the shelf: %v", err)
	}
	if !held {
		t.Fatal("an enabled clock row with a readable card copy did not count as owner content; D38's shelf rule is dead")
	}
}

// installTheme must keep its reboot-retry against the door's conflated refusal, and it
// must keep it CONDITIONED on the mounted re-read — a card that is truly gone has to keep
// the honest error, not spin the board. Text gate, the convention this file's siblings use:
// the bench death (2026-10-29, Bronze: join-time themes died at 503 over a mounted card,
// and three months of empty themes folders traced back to one NULL registry entry).
func TestThemeDoorRetryStaysArmedAndConditional(t *testing.T) {
	src, err := os.ReadFile("installflow.go")
	if err != nil {
		t.Fatalf("installflow.go: %v", err)
	}
	fn := string(src)
	at := strings.Index(fn, "func (s *Server) installTheme")
	if at < 0 {
		t.Fatal("installTheme is gone")
	}
	fn = fn[at:]
	if n := strings.Index(fn, "\n}\n"); n > 0 {
		fn = fn[:n]
	}
	for _, want := range []string{"no card in the socket", "Reboot(", "installWaitBack(", "Mounted"} {
		if !strings.Contains(fn, want) {
			t.Errorf("installTheme lost %q: the join-time theme step is back to eating the door's lie", want)
		}
	}
}
