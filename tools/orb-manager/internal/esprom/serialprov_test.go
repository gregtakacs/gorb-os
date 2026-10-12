package esprom

import (
	"encoding/hex"
	"testing"
)

func TestParsePROVSCAN(t *testing.T) {
	cases := []struct {
		name     string
		line     string
		wantOK   bool
		wantSSID string
		wantRssi int
		wantCh   int
		wantOpen bool
	}{
		{
			name:   "typical encrypted net",
			line:   `I (5123) orb_net: PROVSCAN 0 -55 6 0 MyHomeNet`,
			wantOK: true, wantSSID: "MyHomeNet", wantRssi: -55, wantCh: 6, wantOpen: false,
		},
		{
			name:   "open net",
			line:   `I (5123) orb_net: PROVSCAN 3 -90 11 1 GT-GUEST`,
			wantOK: true, wantSSID: "GT-GUEST", wantRssi: -90, wantCh: 11, wantOpen: true,
		},
		{
			// SSID with spaces must survive as the rest of the line.
			name:   "ssid with spaces",
			line:   `I (5123) orb_net: PROVSCAN 1 -40 1 0 Coffee Shop 5G`,
			wantOK: true, wantSSID: "Coffee Shop 5G", wantRssi: -40, wantCh: 1, wantOpen: false,
		},
		{
			// A '#' or ':' inside the ssid is not a problem: we split from the tag.
			name:   "ssid with punctuation",
			line:   `I (9) orb_net: PROVSCAN 2 -60 4 0 cafe:floor2#net`,
			wantOK: true, wantSSID: "cafe:floor2#net", wantRssi: -60, wantCh: 4, wantOpen: false,
		},
		{name: "begin banner is not a row", line: `I (1) orb_net: PROVSCAN-BEGIN 4`, wantOK: false},
		{name: "end banner is not a row", line: `I (1) orb_net: PROVSCAN-END 4`, wantOK: false},
		{name: "unrelated log", line: `I (1) orb_net: scan done: 4 distinct networks`, wantOK: false},
		{name: "too few fields", line: `I (1) orb_net: PROVSCAN 0 -55 6`, wantOK: false},
		{name: "non-numeric rssi", line: `I (1) orb_net: PROVSCAN 0 x 6 0 bad`, wantOK: false},
		{name: "empty ssid tail", line: `I (1) orb_net: PROVSCAN 0 -55 6 0 `, wantOK: false},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			sn, ok := ParsePROVSCAN(c.line)
			if ok != c.wantOK {
				t.Fatalf("ok = %v, want %v (line %q)", ok, c.wantOK, c.line)
			}
			if !ok {
				return
			}
			if sn.SSID != c.wantSSID || sn.RSSI != c.wantRssi || sn.Ch != c.wantCh || sn.Open != c.wantOpen {
				t.Errorf("got %+v, want ssid=%q rssi=%d ch=%d open=%v", sn, c.wantSSID, c.wantRssi, c.wantCh, c.wantOpen)
			}
		})
	}
}

func TestProvLine(t *testing.T) {
	// Open network (empty pass), empty tz -> both encode as "-" so the firmware
	// reader can distinguish "empty" from absent.
	if got, want := ProvLine("MyNet", "", ""), "PROV "+hex.EncodeToString([]byte("MyNet"))+" - -\r\n"; got != want {
		t.Errorf("open net: got %q want %q", got, want)
	}
	// Passphrase and SSID with spaces/punct round-trip through hex with no
	// delimiter collision — the whole reason for hex on the wire.
	got := ProvLine("a b:c#d", "p@ss word\"x\\y", "UTC0")
	want := "PROV " +
		hex.EncodeToString([]byte("a b:c#d")) + " " +
		hex.EncodeToString([]byte("p@ss word\"x\\y")) + " " +
		hex.EncodeToString([]byte("UTC0")) + "\r\n"
	if got != want {
		t.Errorf("got  %q\nwant %q", got, want)
	}
	// Every field is space-free, so the firmware's strtok split is unambiguous.
	for _, f := range []string{"a b:c#d", "p@ss word", "UTC0"} {
		if h := ProvField(f); containsSpace(h) {
			t.Errorf("ProvField(%q)=%q contains a space", f, h)
		}
	}
	if ProvField("") != "-" {
		t.Errorf("ProvField(\"\") = %q, want \"-\"", ProvField(""))
	}
}

func containsSpace(s string) bool {
	for _, r := range s {
		if r == ' ' {
			return true
		}
	}
	return false
}

func TestParseOrbIP(t *testing.T) {
	cases := []struct {
		name string
		line string
		want string
	}{
		{"real banner", "I (12345) orb_net: associated: config page at http://192.168.2.32/", "192.168.2.32"},
		{"subnet 10", "I (999) orb_net: associated: config page at http://10.0.0.7/", "10.0.0.7"},
		{"https form", "associated: config page at https://192.168.1.5/", "192.168.1.5"},
		// The AP DHCP banner is the fixed 192.168.4.1 and must NOT be taken as
		// the orb's lease — it lacks the "associated: config page at" marker.
		{"ap dhcp line rejected", "I (1673) esp_netif_lwip: DHCP server started on interface WIFI_AP_DEF with IP: 192.168.4.1", ""},
		{"ap boot url line rejected", "I (1981) orb_net: no stored network — provisioning AP orb-8089 (open), then open http://192.168.4.1/", ""},
		{"disassociated rejected", "W (500) orb_net: disassociated: reason 15", ""},
		{"unrelated line rejected", "I (2591) carousel: chip ready (settle 400 ms)", ""},
	}
	for _, c := range cases {
		got, ok := parseOrbIP(c.line)
		if c.want == "" {
			if ok || got != "" {
				t.Errorf("%s: parseOrbIP(%q) = (%q,%v), want (\"\",false)", c.name, c.line, got, ok)
			}
			continue
		}
		if !ok || got != c.want {
			t.Errorf("%s: parseOrbIP(%q) = (%q,%v), want (%q,true)", c.name, c.line, got, ok, c.want)
		}
	}
}
