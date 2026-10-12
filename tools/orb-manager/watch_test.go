package main

import "testing"

func TestProvisioningSSID(t *testing.T) {
	cases := []struct{ line, want string }{
		{`I (2601) orb_net: no stored network — provisioning AP "orb-8088" (open), 900 s`, "orb-8088"},
		// The firmware's own SoftAP-up banner carries no SSID (orb_net.c
		// "SoftAP up — join it and open the URL in the log above"), so it must
		// not produce a verdict on its own — only the "provisioning AP" line,
		// which does name it, ends the console phase early.
		{`W (9001) orb_net: SoftAP up — join it and open the URL in the log above`, ""},
		// Must NOT fire on ordinary lines, least of all the disassociation
		// banner, whose "associated:" substring also threatens stationJoinedLine.
		{`I (1101) orb_net: associated: config page at http://192.168.2.32/`, ""},
		{`W (1234) orb_net: disassociated: beacon-timeout (reason 200, attempt 3)`, ""},
		{`I (300) boot: provisioning of images complete`, ""},
	}
	for _, c := range cases {
		if got := provisioningSSID(c.line); got != c.want {
			t.Errorf("provisioningSSID(%q) = %q, want %q", c.line, got, c.want)
		}
	}
}

func TestStationJoinedLine(t *testing.T) {
	yes := []string{
		`I (2503) orb_net: associated: config page at http://192.168.2.32/`,
	}
	no := []string{
		`W (2600) orb_net: disassociated: beacon-timeout (reason 200, attempt 1)`,
		`I (100) cpu_start: Pro CPU started`,
	}
	for _, l := range yes {
		if !stationJoinedLine(l) {
			t.Errorf("stationJoinedLine(%q) = false, want true", l)
		}
	}
	for _, l := range no {
		if stationJoinedLine(l) {
			t.Errorf("stationJoinedLine(%q) = true, want false", l)
		}
	}
}

func TestMacNorm(t *testing.T) {
	for _, in := range []string{"44:BD:8D:60:80:88", "44-bd-8d-60-80-88", " 44:bd:8d:60:80:88 "} {
		if got := macNorm(in); got != "44:bd:8d:60:80:88" {
			t.Errorf("macNorm(%q) = %q", in, got)
		}
	}
}
