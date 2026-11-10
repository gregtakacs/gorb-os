package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// Remembered orb addresses. The proxy will not dial an address the hub has not
// learned, so a hub that forgets on restart makes the operator retype an address they
// already proved works. What is persisted is an address and nothing else.

func TestRememberedAddressesSurviveARestart(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.json")

	first := newHub()
	first.statePath = path
	first.grants.learn("192.168.2.31")
	first.setOrbIP("192.168.2.31")
	first.saveState()
	if _, err := os.Stat(path); err != nil {
		t.Fatalf("saveState wrote nothing: %v", err)
	}

	// A hub that has never seen this orb must refuse it first, or the comparison
	// below proves nothing.
	cold := newHub()
	if cold.grants.allowed("192.168.2.31") {
		t.Fatal("a fresh hub could already dial the address; the test is vacuous")
	}
	cold.statePath = path
	if n := cold.loadState(); n != 1 {
		t.Fatalf("loadState restored %d addresses, want 1", n)
	}
	if !cold.grants.allowed("192.168.2.31") {
		t.Error("the learned address did not come back")
	}
	if cold.currentOrbIP() != "192.168.2.31" {
		t.Errorf("the pre-fill address did not come back: %q", cold.currentOrbIP())
	}
}

// The file holds addresses only. A token, a passphrase, or firmware bytes in it would
// be a leak into the config dir, so the shapes that could cause one are pinned out.
func TestStateFileHoldsNoSecrets(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.json")
	s := newHub()
	s.statePath = path
	s.installID = mintInstallID()
	s.grants.learn("10.1.2.3")
	s.saveState()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	text := string(b)
	for _, leak := range []string{s.token, "passphrase", "ssid", "firmware"} {
		if leak != "" && strings.Contains(strings.ToLower(text), strings.ToLower(leak)) {
			t.Errorf("the state file mentions %q; only addresses belong there:\n%s", leak, text)
		}
	}
	var st hubState
	if err := json.Unmarshal(b, &st); err != nil {
		t.Fatalf("state file is not the documented shape: %v", err)
	}
	if st.Version != stateVersion {
		t.Errorf("version = %d, want %d", st.Version, stateVersion)
	}
	// The install id is the one thing in here that is not an address, and it is an id
	// and not a credential (§7.2): it names this installation to nobody but the site it
	// is eventually sent to, and it grants nothing. So it belongs in this file, and this
	// assertion is what stops a future edit from treating it as one of the secrets above.
	if st.InstallID != s.installID || st.InstallID == "" {
		t.Errorf("installId = %q, want the minted %q to be on disk", st.InstallID, s.installID)
	}
	if st.InstallID == s.token {
		t.Error("the install id and the control token are the same value")
	}
}

// A corrupt or hand-edited file must not become a dial list. The safe reading of
// damaged bytes is "nothing remembered", never "whatever the bytes said", because
// every entry in this set is an address the hub is willing to reach out to.
func TestStateRefusesJunkAndHostnames(t *testing.T) {
	cases := map[string]string{
		"truncated":       `{"version":1,"grants":{"10.0.0.1":`,
		"not json":        "orb=192.168.1.1",
		"hostnames":       `{"version":1,"grants":{"evil.example.com":1,"metadata":1}}`,
		"wrong port form": `{"version":1,"grants":{"10.0.0.1:80":1}}`,
		"ipv6":            `{"version":1,"grants":{"::1":1}}`,
	}
	for name, body := range cases {
		path := filepath.Join(t.TempDir(), "state.json")
		if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
			t.Fatal(err)
		}
		s := newHub()
		s.statePath = path
		s.loadState()
		for _, ip := range []string{"evil.example.com", "metadata", "10.0.0.1:80", "10.0.0.1", "::1"} {
			if s.grants.allowed(ip) {
				t.Errorf("%s: %q entered the dialable set", name, ip)
			}
		}
	}
}

// A save that cannot write must leave the previous file intact rather than truncate
// it: the failure mode it protects against is a hub that loses the operator's whole
// list because one write went wrong.
func TestStateSaveFailureKeepsTheOldFile(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "state.json")
	s := newHub()
	s.statePath = path
	s.grants.learn("10.9.9.9")
	s.saveState()
	before, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(dir, 0o500); err != nil {
		t.Skipf("cannot make the directory read-only on this platform: %v", err)
	}
	defer os.Chmod(dir, 0o700)
	// Chmod returning nil does not mean the directory rejects writes. Windows
	// accepts the call and enforces nothing, so the save below would succeed and
	// this test would report a broken save path that is only unreachable. Probe
	// for the denial instead of trusting the call, and skip where it cannot be had.
	probe := filepath.Join(dir, "write-probe")
	err = os.WriteFile(probe, []byte("x"), 0o600)
	os.Remove(probe)
	if err == nil {
		t.Skip("this platform's file modes do not deny writes to the directory: a save cannot be made to fail here")
	}

	s.grants.learn("10.8.8.8")
	s.saveState()
	after, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("the state file vanished when a save failed: %v", err)
	}
	if string(after) != string(before) {
		t.Errorf("a failed save changed the file:\n%s\nwant\n%s", after, before)
	}
	if s.stateErr == nil {
		t.Error("a save that could not write reported no error")
	}
}

// A missing state file is a normal first run, not a fault to report.
func TestMissingStateFileIsQuiet(t *testing.T) {
	s := newHub()
	s.statePath = filepath.Join(t.TempDir(), "nope", "state.json")
	if n := s.loadState(); n != 0 {
		t.Errorf("loadState restored %d addresses from a file that does not exist", n)
	}
	if s.stateErr != nil {
		t.Errorf("a first run reported a state error: %v", s.stateErr)
	}
}

// The install id (§7.2, ruling 25): one per installation, minted once, read back ever
// after, and sent nowhere. The behaviour that matters is the one a hub cannot repair
// later — an id that changed between launches would be a different Orb Manager on the
// site's Connected list every morning, so the file, not the memory, is the authority.

func TestInstallIDIsMintedOnceAndSurvives(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("the config-dir behaviour under test is the Unix one")
	}
	cfg := t.TempDir()
	t.Setenv("XDG_CONFIG_HOME", cfg)
	t.Setenv("HOME", cfg)
	path := filepath.Join(cfg, "orb-manager", "state.json")

	first := newHub()
	first.armState()
	id := first.installID
	if len(id) != 32 {
		t.Fatalf("the first run minted %q, want 32 hex characters", id)
	}
	if strings.ToLower(id) != id || strings.ContainsAny(id, "ghijklmnopqrstuvwxyz") {
		t.Errorf("the install id is not lower-case hex: %q", id)
	}
	if _, err := os.Stat(path); err != nil {
		t.Fatalf("armState minted an id and wrote no state file: %v", err)
	}
	if st := readHubState(t, path); st.InstallID != id {
		t.Errorf("the file carries %q and the hub holds %q", st.InstallID, id)
	}

	// A save for some other reason mid-session rewrites the id, never a fresh one.
	first.grants.learn("10.3.2.1")
	first.saveState()
	if st := readHubState(t, path); st.InstallID != id {
		t.Errorf("a later save changed the install id from %q to %q", id, st.InstallID)
	}

	// A restart reads the same id back rather than minting a new installation.
	cold := newHub()
	cold.armState()
	if cold.installID != id {
		t.Errorf("a restart minted a second install id: %q, want %q", cold.installID, id)
	}
	if !cold.grants.allowed("10.3.2.1") {
		t.Error("the id came back and the remembered address did not")
	}
}

// An upgrade from a pre-G3 state file (no installId key at all) mints once and persists,
// and a file that already carries an id is left exactly as it was written.
func TestInstallIDSurvivesAnUpgradeAndAHold(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("the config-dir behaviour under test is the Unix one")
	}

	// A v3 file from before the install id existed, with one address in it.
	cfg := t.TempDir()
	t.Setenv("XDG_CONFIG_HOME", cfg)
	t.Setenv("HOME", cfg)
	path := filepath.Join(cfg, "orb-manager", "state.json")
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	old := `{"version":3,"grants":{"192.168.7.7":1730000000},"macs":{"192.168.7.7":"aabbccddeeff"}}`
	if err := os.WriteFile(path, []byte(old), 0o600); err != nil {
		t.Fatal(err)
	}

	up := newHub()
	up.armState()
	if up.installID == "" {
		t.Fatal("a state file with no installId came through G3 without one")
	}
	if !up.grants.allowed("192.168.7.7") {
		t.Error("the upgrade lost the address the old file knew")
	}
	minted := readHubState(t, path).InstallID
	if minted != up.installID {
		t.Errorf("the file carries %q and the hub holds %q", minted, up.installID)
	}

	// A file that already names an id: armState neither replaces it nor clears it, and
	// the next save writes the same word.
	held := newHub()
	held.statePath = path
	held.installID = "0123456789abcdef0123456789abcdef"
	held.saveState()
	again := newHub()
	again.armState()
	if again.installID != "0123456789abcdef0123456789abcdef" {
		t.Errorf("armState overwrote a held install id with %q", again.installID)
	}
}

// readHubState is the file as the next launch will see it.
func readHubState(t *testing.T, path string) hubState {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var st hubState
	if err := json.Unmarshal(b, &st); err != nil {
		t.Fatalf("state file is not the documented shape: %v\n%s", err, b)
	}
	return st
}
