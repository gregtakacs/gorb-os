package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// The seed (D36) is the only way apps reach a fresh dial, so these tests carry
// the weight of that claim: the run is idempotent, the gates refuse before any
// byte crosses the room, and the committed seed files are the generators' own
// bytes rather than whatever a hand left in the tree.

// seedHub wires the hub to a bare fake orb — no themes on the card, no apps
// installed, firmware at the seed's own floor — on the same ip:port trick
// fakeOrbHub uses, with the status client pointed at the fake too because
// seedWaitBack dials it after the store's reboot.
func seedHub(t *testing.T, fw string) (*Server, string, *orbfake.Orb) {
	t.Helper()
	f := orbfake.New()
	f.SetFirmware(fw)
	orb := httptest.NewServer(f)
	t.Cleanup(orb.Close)
	hp := strings.TrimPrefix(orb.URL, "http://")
	host, port := hp[:strings.LastIndex(hp, ":")], hp[strings.LastIndex(hp, ":")+1:]
	s := newHub()
	s.device = orbnet.NewClient(2 * time.Second)
	s.device.DevicePort = port
	s.ota = orbnet.NewClient(30 * time.Second)
	s.ota.DevicePort = port
	s.preview = orbnet.NewClient(2 * time.Second)
	s.preview.DevicePort = port
	s.orb = orbnet.NewClient(2 * time.Second)
	s.orb.DevicePort = port
	s.grants.learn(host)
	return s, host, f
}

// postSeedThroughMux drives the real route, so the control gate and the
// registration are exercised next to the handler — the postTheme discipline.
func postSeedThroughMux(t *testing.T, s *Server, body string) *httptest.ResponseRecorder {
	t.Helper()
	mux := http.NewServeMux()
	s.routes(mux)
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/orb/seed", strings.NewReader(body))
	req.Header.Set(tokenHeader, s.token)
	mux.ServeHTTP(rec, req)
	return rec
}

func seedSteps(t *testing.T, rec *httptest.ResponseRecorder) []seedStep {
	t.Helper()
	var out []seedStep
	for _, line := range strings.Split(rec.Body.String(), "\n") {
		if strings.TrimSpace(line) == "" {
			continue
		}
		var st seedStep
		if err := json.Unmarshal([]byte(line), &st); err != nil {
			t.Fatalf("seed stream line is not a step: %q", line)
		}
		out = append(out, st)
	}
	return out
}

func actionOf(steps []seedStep, step, action string) bool {
	for _, s := range steps {
		if s.Step == step && s.Action == action {
			return true
		}
	}
	return false
}

func lastStep(steps []seedStep) seedStep { return steps[len(steps)-1] }

// A fresh board walks the whole ladder: each starter app uploads, installs and
// enables; the theme lands only because the clock now runs and the card was
// empty; the run closes on the done step. The ledger and the card afterwards
// are the device's own testimony that the seed spoke its doors.
func TestSeedLandsTheStarterPairAndTheBuiltInTheme(t *testing.T) {
	seedInstallPoll = 10 * time.Millisecond
	seedInstallWait = 2 * time.Second
	seedInstallHard = 5 * time.Second
	seedRebootWait = 2 * time.Second

	s, ip, f := seedHub(t, "v1.1.20")
	rec := postSeedThroughMux(t, s, `{"ip":"`+ip+`"}`)
	steps := seedSteps(t, rec)
	if len(steps) < 8 {
		t.Fatalf("stream too short: %v", steps)
	}
	for _, want := range [][2]string{
		{"clock", "uploaded"}, {"clock", "installing"}, {"clock", "installed"}, {"clock", "enabled"},
		{"color_picker", "uploaded"}, {"color_picker", "installing"}, {"color_picker", "installed"}, {"color_picker", "enabled"},
		{"theme", "uploaded-theme"}, {"theme", "stored"},
		{"seed", "done"},
	} {
		if !actionOf(steps, want[0], want[1]) {
			t.Errorf("no %s/%s step in the stream: %v", want[0], want[1], steps)
		}
	}
	if lastStep(steps).Action != "done" {
		t.Errorf("the run ended on %v, not on done", steps[len(steps)-1])
	}
	// The device's testimony: both rows installed, the theme filed under its own
	// name, stored, and active — the dial's first face came from the hub's seed.
	list, err := s.device.AppsList(ip)
	if err != nil {
		t.Fatal(err)
	}
	var clock, picker bool
	for _, r := range list.Installed {
		switch r.Slug {
		case "clock":
			clock = r.Enabled
		case "color_picker":
			picker = r.Enabled
		}
	}
	if !clock || !picker {
		t.Errorf("starter rows after the seed: clock=%v picker=%v, want both installed and enabled", clock, picker)
	}
	tl, err := s.device.ThemeList(ip)
	if err != nil {
		t.Fatal(err)
	}
	if tl.Active != "av-kit" || len(tl.Themes) != 1 || !tl.Themes[0].Stored {
		t.Errorf("theme after the seed: active=%q rows=%d, want one stored av-kit row", tl.Active, len(tl.Themes))
	}
	_ = f
}

// The same press twice: the second run reads the ledger, finds every row it is
// asked to place already standing, and writes nothing. A seed that re-uploaded
// on a healthy dial would reboot an owner onto a face they already had.
func TestSeedIsIdempotentOnABoardThatAlreadyCarriesThePair(t *testing.T) {
	seedInstallPoll = 10 * time.Millisecond
	seedInstallWait = 2 * time.Second
	seedInstallHard = 5 * time.Second
	seedRebootWait = 2 * time.Second
	s, ip, _ := seedHub(t, "v1.1.20")
	// The first press walks the whole ladder — the previous test's claim.
	if lastStep(seedSteps(t, postSeedThroughMux(t, s, `{"ip":"`+ip+`"}`))).Action != "done" {
		t.Fatal("the first seed did not finish; idempotence has nothing to stand on")
	}
	// The second press must READ, not write: every fact the seed checks for now
	// stands on the device — both rows installed, one theme on the card and in
	// the slot. A re-press that re-uploaded would reboot an owner onto a face
	// they already had, which is the failure this test exists to catch.
	rec := postSeedThroughMux(t, s, `{"ip":"`+ip+`"}`)
	steps := seedSteps(t, rec)
	for _, want := range [][2]string{{"clock", "skipped"}, {"color_picker", "skipped"}, {"theme", "skipped"}, {"seed", "done"}} {
		if !actionOf(steps, want[0], want[1]) {
			t.Errorf("no %s/%s step: %v", want[0], want[1], steps)
		}
	}
	for _, st := range steps {
		if st.Action == "uploaded" || st.Action == "uploaded-theme" || st.Action == "installed" {
			t.Fatalf("second seed wrote (%s/%s) onto a board that already carried the pair", st.Step, st.Action)
		}
	}
}

// The version gate: the fake's stock fw is not a versioned stamp, and an older
// device must be refused the seed rather than half-seeded — the same sentence
// the /apps update row speaks for the Flash button.
func TestSeedRefusesAFirmwareOlderThanTheSeed(t *testing.T) {
	s, ip, _ := seedHub(t, "v1.1.19")
	rec := postSeedThroughMux(t, s, `{"ip":"`+ip+`"}`)
	steps := seedSteps(t, rec)
	if len(steps) != 1 || steps[0].Action != "refused" || !strings.Contains(steps[0].Note, "v1.1.20") {
		t.Fatalf("old-firmware seed = %v, want one refused step naming v1.1.20", steps)
	}
}

// Every door the seed walks lives on the card: with none mounted, the honest
// answer is the card sentence, not three silent failures.
func TestSeedRefusesABoardWithNoCard(t *testing.T) {
	s, ip, f := seedHub(t, "v1.1.20")
	f.Mounted = false
	rec := postSeedThroughMux(t, s, `{"ip":"`+ip+`"}`)
	steps := seedSteps(t, rec)
	if len(steps) != 1 || steps[0].Action != "refused" || !strings.Contains(steps[0].Note, "card") {
		t.Fatalf("no-card seed = %v, want one refused step naming the card", steps)
	}
}

// The route is a write: an address the hub never learned is refused before the
// first dial, exactly as every other device-touching route refuses it.
func TestSeedRefusesAnAddressTheHubNeverLearned(t *testing.T) {
	s, _, _ := seedHub(t, "v1.1.20")
	rec := postSeedThroughMux(t, s, `{"ip":"203.0.113.9"}`)
	if rec.Code == http.StatusOK && strings.Contains(rec.Body.String(), `"done"`) {
		t.Fatal("an unlearned address reached the seed")
	}
}

// The committed seed files are what the generators emit, byte for byte — the
// same freshness rule tools/host_orbapp_install_test.py holds the factory
// containers to. A stale seed fails here, on the binary that ships it, and not
// on a bench orb that cannot say where the bytes came from.
func TestSeedFilesAreFresh(t *testing.T) {
	py, err := exec.LookPath("python3")
	if err != nil {
		t.Skip("no python3 on PATH; the generator lint runs on the host suite")
	}
	for _, tool := range []string{"make_factory_apps.py", "make_seed_theme.py"} {
		cmd := exec.Command(py, filepath.Join("..", "..", "tools", tool), "--check")
		out, err := cmd.CombinedOutput()
		if err != nil || cmd.ProcessState.ExitCode() != 0 {
			t.Errorf("%s --check failed: %v (%s)", tool, err, strings.TrimSpace(string(out)))
		}
	}
}

// The embedded seed is byte-identical to the files on disk, so the shipped
// binary cannot drift from the committed tree it was built from.
func TestEmbeddedSeedMatchesDisk(t *testing.T) {
	hashes, err := seedHashes()
	if err != nil {
		t.Fatal(err)
	}
	for file, want := range hashes {
		disk, err := os.ReadFile(filepath.Join("seed", filepath.Base(file)))
		if err != nil {
			t.Fatalf("read %s: %v", file, err)
		}
		if got := sha256hex(disk); got != want {
			t.Errorf("%s: embedded %s != disk %s", file, want[:12], got[:12])
		}
	}
}

func sha256hex(b []byte) string {
	sum := sha256.Sum256(b)
	return hex.EncodeToString(sum[:])
}
