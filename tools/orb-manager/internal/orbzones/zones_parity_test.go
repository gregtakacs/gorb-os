package orbzones

import (
	"os"
	"regexp"
	"testing"
)

// The picker is only honest while it is the device's list. This test re-parses
// main/net/orb_net.c at check time and refuses any drift, so editing the
// firmware table without re-running gen_zones.py fails the build instead of
// shipping an orb-manager that offers zones the device will reject. A checkout
// without the firmware (the hub alone) skips rather than lies.

func TestZonesMatchTheFirmwareTable(t *testing.T) {
	path := os.Getenv("ORB_NET_C")
	if path == "" {
		path = "../../../../../main/net/orb_net.c"
	}
	src, err := os.ReadFile(path)
	if err != nil {
		t.Skip("firmware source not present here:", err)
	}
	body := firmwareTable(string(src), t)
	rows := regexp.MustCompile(`\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\}`).
		FindAllStringSubmatch(body, -1)
	if len(rows) < 10 {
		t.Fatalf("s_tz_tbl parsed %d rows — the table moved or the pattern rotted", len(rows))
	}
	if len(rows) != len(table) {
		t.Fatalf("firmware lists %d zones, the hub serves %d — re-run gen_zones.py",
			len(rows), len(table))
	}
	for i, r := range rows {
		z := table[i]
		if z.Name != r[1] || z.POSIX != r[2] || z.IANA != r[3] || z.Label != r[4] {
			t.Fatalf("row %d drifted: firmware %v, hub %+v — re-run gen_zones.py", i, r[1:], z)
		}
	}
}

func firmwareTable(src string, t *testing.T) string {
	t.Helper()
	m := regexp.MustCompile(`(?s)static const tz_entry_t s_tz_tbl\[\] = \{(.*?)\n\};`).FindStringSubmatch(src)
	if m == nil {
		t.Fatal("s_tz_tbl not found in the firmware source")
	}
	return m[1]
}
