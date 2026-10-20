package esprom

import "testing"

// The boot banner is the only channel that reaches a board with no lease, so the
// parse is what stands between a named board and a blank card. These pins are the
// marker's own shape: the firmware prints the name raw to end of line, the way
// PROVSCAN prints an SSID, because a name may hold a space and a \S+ class drops it.

func TestParseOrbNameReadsTheBanner(t *testing.T) {
	cases := []struct {
		line string
		want string
	}{
		{`I (1234) orb_net: GorbOS-STATE c-sta`, ``},
		{`I (1235) orb_net: GorbOS-NAME The Orb`, `The Orb`},
		{`GorbOS-NAME Kitchen`, `Kitchen`},
		{`I (9) orb_net: GorbOS-NAME   Padded Pier  `, `Padded Pier`},
		{`GorbOS-NAME`, ``},        // marker with no operand is not a name
		{`GorbOS-NAMEx bogus`, ``}, // the marker is a word, not a prefix
		{`not a banner at all`, ``},
	}
	for _, c := range cases {
		got, ok := ParseOrbName(c.line)
		if c.want == "" {
			if ok && got != "" {
				t.Errorf("ParseOrbName(%q) = %q, want no name", c.line, got)
			}
			continue
		}
		if !ok || got != c.want {
			t.Errorf("ParseOrbName(%q) = %q,%v want %q,true", c.line, got, ok, c.want)
		}
	}
}

// A probe reads the whole boot window through ParseAppInfo, so the name has to ride
// the same OrbInfo the version rides on — otherwise a paid probe answers "orb,
// unnamed" for a board that said its name out loud two lines earlier.
func TestParseAppInfoCarriesTheBannerName(t *testing.T) {
	lines := []string{
		"I (700) main: === gorb-os booting ===",
		"I (710) app_init: Project name: gorb-os",
		"I (712) app_init: App version: v1.4.3",
		"I (1230) orb_net: GorbOS-STATE c-sta",
		"I (1231) orb_net: GorbOS-NAME Reading Room",
	}
	info, found := ParseAppInfo(lines)
	if !found {
		t.Fatal("a gorb-os boot window was not found")
	}
	if info.Name != "Reading Room" {
		t.Errorf("info.Name = %q, want the name the board announced", info.Name)
	}
	if info.Version != "v1.4.3" {
		t.Errorf("the descriptor stopped parsing: %+v", info)
	}

	// An unnamed board prints nothing, and nothing must not read as cleared: a hub
	// that guessed would wipe the cache of every board running older firmware.
	quiet, _ := ParseAppInfo(lines[:4])
	if quiet.Name != "" {
		t.Errorf("an unnamed boot window produced %q, want empty", quiet.Name)
	}
}
