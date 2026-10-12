package main

import (
	"encoding/json"
	"os"
	"path/filepath"
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
