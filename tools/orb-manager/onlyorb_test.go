package main

import (
	"os"
	"strings"
	"testing"
)

// A release hub (no -only-orb) admits everything and builds its browse unfiltered.
func TestOnlyOrbUnsetAdmitsEveryOrb(t *testing.T) {
	s := newHub()
	if s.mdnsAdmit() != nil {
		t.Error("a hub without -only-orb filters its browse")
	}
	if !s.admitsMAC("44bd8d608088") || !s.admitsMAC("44bd8d782618") {
		t.Error("a hub without -only-orb refused a board")
	}
	in := []cableSighting{{port: "/dev/ttyACM0", isOrb: true, mac: "44bd8d608088"}, {port: "/dev/ttyACM1", isOrb: true}}
	got, _ := s.scopedSweep(func() ([]cableSighting, error) { return in, nil })()
	if len(got) != 2 {
		t.Errorf("a hub without -only-orb dropped leads: %+v", got)
	}
}

func TestOnlyOrbKeepsItsOwnBoard(t *testing.T) {
	s := newHub()
	s.onlyOrb = "44bd8d782618"
	if !s.admitsMAC("44:BD:8D:78:26:18") || s.admitsMAC("44bd8d608088") {
		t.Error("admitsMAC does not match the -only-orb board by canonical MAC alone")
	}
	if !s.admitsMAC("") {
		t.Error("an address with no MAC behind it (the operator's own) was refused")
	}
	in := []cableSighting{
		{port: "/dev/ttyACM0", isOrb: true, mac: "44bd8d608088"}, // the other clone's board
		{port: "/dev/ttyACM1", isOrb: true, mac: "44bd8d782618"}, // this one
		{port: "/dev/ttyACM2", espressif: true},                  // unattributed: may be the other
		{port: "/dev/ttyUSB0"},                                   // not an Espressif chip
	}
	got, _ := s.scopedSweep(func() ([]cableSighting, error) { return in, nil })()
	var ports []string
	for _, c := range got {
		ports = append(ports, c.port)
	}
	if strings.Join(ports, " ") != "/dev/ttyACM1 /dev/ttyUSB0" {
		t.Errorf("sweep kept %v, want this board's lead and the non-Espressif port", ports)
	}
}

// A grant remembered from state.json for the other board must not dial.
func TestOnlyOrbRefusesAGrantForAnotherBoard(t *testing.T) {
	s := newHub()
	s.onlyOrb = "44bd8d782618"
	s.grants.learnAs("192.168.2.32", "44bd8d608088")
	s.grants.learnAs("192.168.2.34", "44bd8d782618")
	if ok, why := s.mayDial("192.168.2.32"); ok || !strings.Contains(why, "-only-orb") {
		t.Errorf("mayDial(other board) = %v %q, want a refusal naming -only-orb", ok, why)
	}
	if ok, why := s.mayDial("192.168.2.34"); !ok {
		t.Errorf("mayDial(this board) refused: %q", why)
	}
}

// A shared state file from before the split (v1-style unbound grants) names both orbs.
// A hub for one of them restores only its own card and grant, and saving gives the
// other's back unchanged, so a release hub on the same config dir still knows it.
func TestOnlyOrbHoldsTheOtherOrbsState(t *testing.T) {
	dir := t.TempDir()
	path := dir + "/state.json"
	file := `{"version":3,"lastIp":"192.168.2.32",
	  "grants":{"192.168.2.32":1,"192.168.2.34":1},
	  "fleet":[{"mac":"44bd8d608088","ip":"192.168.2.32","port":80,"remembered":true,"name":"Bronze"},
	           {"mac":"44bd8d782618","ip":"192.168.2.34","port":80,"remembered":true,"name":"Gold"}]}`
	if err := os.WriteFile(path, []byte(file), 0o600); err != nil {
		t.Fatal(err)
	}
	s := newHub()
	s.onlyOrb = "44bd8d608088"
	s.statePath = path
	s.loadState()
	for _, r := range s.fleet.All() {
		if r.MAC != "44bd8d608088" {
			t.Errorf("the other clone's orb came back from the file: %+v", r)
		}
	}
	if ok, _ := s.mayDial("192.168.2.34"); ok {
		t.Error("the other orb's remembered address is still dialable")
	}
	if ok, why := s.mayDial("192.168.2.32"); !ok {
		t.Errorf("this hub's own orb is refused: %s", why)
	}
	s.saveState()
	b, _ := os.ReadFile(path)
	if !strings.Contains(string(b), `"name": "Gold"`) || !strings.Contains(string(b), `"192.168.2.34": "44bd8d782618"`) {
		t.Errorf("saving dropped the other orb's share:\n%s", b)
	}
}
