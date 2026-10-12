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

func TestParseProcArp(t *testing.T) {
	text := `IP address       HW type     Flags       HW address            Mask     Device
192.168.2.1      0x1         0x2         aa:bb:cc:dd:ee:ff     *        eth0
192.168.2.32     0x1         0x2         44:bd:8d:60:80:88     *        eth0
`
	if got := parseProcArp(text, "44:bd:8d:60:80:88"); got != "192.168.2.32" {
		t.Errorf("parseProcArp hit = %q, want 192.168.2.32", got)
	}
	if got := parseProcArp(text, "00:11:22:33:44:55"); got != "" {
		t.Errorf("parseProcArp miss = %q, want empty", got)
	}
}

func TestParseArpA(t *testing.T) {
	macosish := `? (192.168.2.1) at aa:bb:cc:dd:ee:ff on en0 ifscope [ethernet]
? (192.168.2.32) at 44:bd:8d:60:80:88 on en0 ifscope [ethernet]`
	if got := parseArpA(macosish, "44:bd:8d:60:80:88"); got != "192.168.2.32" {
		t.Errorf("parseArpA macos = %q, want 192.168.2.32", got)
	}
	win := `Interface: 192.168.2.14 --- 0xa
  Internet Address      Physical Address      Type
  192.168.2.32          44-bd-8d-60-80-88   dynamic`
	if got := parseArpA(win, "44:bd:8d:60:80:88"); got != "192.168.2.32" {
		t.Errorf("parseArpA windows = %q, want 192.168.2.32", got)
	}
	if got := parseArpA(macosish, "de:ad:be:ef:00:00"); got != "" {
		t.Errorf("parseArpA miss = %q, want empty", got)
	}
}

// TestParseArpingReply pins the stale-ARP guard: the candidate IP must be
// confirmed to still answer for the flashed MAC, in both the iputils and
// busybox reply formats, and a non-reply line must yield nothing so an
// unreachable host is treated as no-evidence (checked=false), not a mismatch.
func TestParseArpingReply(t *testing.T) {
	cases := []struct{ in, want string }{
		{`Unicast reply from 192.168.2.99 [44:BD:8D:60:80:88]  1.4ms`, "44:bd:8d:60:80:88"},
		{`ARP reply from 192.168.2.99 (44-bd-8d-60-80-88)`, "44:bd:8d:60:80:88"},
		{`Requesting 192.168.2.99 while waiting on eth1\nNo reply, 10000 bytes received`, ""},
		{`Some banner mentioning 44:bd:8d:60:80:88 as an address`, ""},
	}
	for _, c := range cases {
		if got := parseArpingReply(c.in); got != c.want {
			t.Errorf("parseArpingReply(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}
