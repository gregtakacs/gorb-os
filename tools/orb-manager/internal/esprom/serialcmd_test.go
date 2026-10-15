package esprom

import "testing"

// The PING answer grew a firmware stamp so a hub with only a lead learns the
// running version without paying a reboot for it. The pin that matters is the
// back-compat edge: a board on older firmware answers with the bare state token
// and must parse exactly as it always did, with no stamp rather than a wrong one.

func TestParsePingState(t *testing.T) {
	cases := []struct {
		line, want string
	}{
		{`I (1234) orb_net: ORB-PING c-sta`, `c-sta`},
		{`I (1234) orb_net: ORB-PING b fw=v1.3.0+deadbee`, `b`},
		{`ORB-PING c-ap fw=094fa22`, `c-ap`},
		{`ORB-PINGX c-sta`, ``}, // the marker is a word, not a prefix
		{`unrelated console line`, ``},
	}
	for _, c := range cases {
		got, ok := ParsePing(c.line)
		if c.want == "" {
			if ok {
				t.Errorf("ParsePing(%q) accepted a non-answer", c.line)
			}
			continue
		}
		if !ok || got != c.want {
			t.Errorf("ParsePing(%q) = %q,%v want %q,true", c.line, got, ok, c.want)
		}
	}
}

func TestParsePingFW(t *testing.T) {
	cases := []struct {
		line, want string
	}{
		{`I (1234) orb_net: ORB-PING c-sta fw=v1.3.0+deadbee`, `v1.3.0+deadbee`},
		{`ORB-PING c-sta fw=094fa22`, `094fa22`},
		{`I (9) orb_net: ORB-PING b`, ``}, // old firmware: no stamp is no news
		{`ORB-PING`, ``},
		{`fw=v1.3.0`, ``}, // an fw= on a line that is not a PING answer is nothing
	}
	for _, c := range cases {
		got, ok := ParsePingFW(c.line)
		if c.want == "" {
			if ok && got != "" {
				t.Errorf("ParsePingFW(%q) = %q, want no stamp", c.line, got)
			}
			continue
		}
		if !ok || got != c.want {
			t.Errorf("ParsePingFW(%q) = %q,%v want %q,true", c.line, got, ok, c.want)
		}
	}
}

// RunCommand needs a real /dev/tty handle, so the driver half of the ack is covered
// by the hub's own proof tests (cableproof_test.go feeds the seam above the I/O).
// What this file owns is the regex contract the firmware moves under, and the two
// table tests above are the whole of it.
