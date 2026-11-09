#!/usr/bin/env python3
"""Host bench for main/net/orb_tzrule.c: a POSIX TZ rule's offset at any instant, the
OS's answer for a place's zone (radar-plan §2.1, owner 2026-11-08).

Every rule in the zone table (main/net/orb_cities.c) is compiled into one gcc program
with orb_tzrule.c and asked for its offset every 3 hours from 2025 through 2028, and at
each minute either side of every changeover zoneinfo knows in those years. Each zone
the table calls exact is held to Python's zoneinfo at all of those instants: the offset
and whether daylight time is in force. A rule the table marks approximate (bit 7) is
left out, as the table says it does not match. Malformed rules must be refused.

Run:  python3 tools/host_tzrule_test.py              (rc 0 = pass)
      python3 tools/host_tzrule_test.py --mutations  (proves the checks bite)
"""
import datetime as dt
import os
import re
import subprocess
import sys
import tempfile
from zoneinfo import ZoneInfo

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "main", "net", "orb_tzrule.c")
HDR = os.path.join(ROOT, "main", "net", "orb_tzrule.h")
CITIES = os.path.join(ROOT, "main", "net", "orb_cities.c")

# Zones whose stored rule the table should mark approximate but does not, left out of the
# hold with the reason. Empty since 2026-11-08: tools/make_cities.py holds a decade and
# reaches for tzdata's own rule, and the zones no fixed rule follows (Gaza, Hebron) are
# flagged. A zone listed here is a finding for that tool.
TABLE_APPROX = {}

# POSIX forms the table does not use, each held to a zone it describes: a daylight offset
# left unsaid (an hour ahead of standard), and a <quoted> name with minutes.
EXTRA = [("EST5EDT,M3.2.0,M11.1.0", "America/New_York"), ("<+0330>-3:30", "Asia/Tehran"),
         # tzdata's own rules with change times past 24 h (2026e's TZif footers)
         ("<-04>4<-03>,M9.1.6/24,M4.1.6/24", "America/Santiago"), ("IST-2IDT,M3.4.4/26,M10.5.0", "Asia/Jerusalem"),
         # a negative change time: the hour before midnight
         ("<-02>2<-01>,M3.5.0/-1,M10.5.0/0", "America/Nuuk")]

MUTATIONS = [
    ("dst_w = std_w - 3600;", "dst_w = std_w;"),
    ("c->at_s = 2 * 3600;", "c->at_s = 0;"),
    ("const int64_t end = change_day(y, &off) * 86400 + off.at_s + dst_w;", "const int64_t end = change_day(y, &off) * 86400 + off.at_s + std_w;"),
    ("while (day > month_days(y, c->mon)) day -= 7;", ";"),
    ("*off_min = -(in ? dst_w : std_w) / 60;", "*off_min = (in ? dst_w : std_w) / 60;"),
    ("(utc >= start || utc < end)", "(utc >= start && utc < end)"),
    ("const int wd1 = (int)(((first % 7) + 7 + 4) % 7);", "const int wd1 = (int)(((first % 7) + 7 + 3) % 7);"),
    ("sign = *p == '-' ? -1 : 1;", "sign = 1;"),
    ("p = rule_hms(p + 1, &t, 167);", "p = rule_hms(p + 1, &t, 24);"),
]


def table():
    src = open(CITIES).read()
    rules = re.findall(r'"([^"]*)"', src[src.index("ORB_GEO_RULES[] = {"):src.index("};", src.index("ORB_GEO_RULES[] = {"))])
    zones = re.findall(r'\{ "([^"]+)", (\d+) \}', src[src.index("ORB_GEO_ZONES[] = {"):])
    return rules, [(z, int(r)) for z, r in zones]


# The table holds each zone's rule as it stands now (tools/make_cities.py), so it is held
# to zoneinfo from 2026-11-08 to the end of 2036, the decade make_cities checks, not over
# years another rule governed.
T0 = int(dt.datetime(2026, 11, 8, tzinfo=dt.timezone.utc).timestamp())
T1 = int(dt.datetime(2037, 1, 1, tzinfo=dt.timezone.utc).timestamp())


def instants(zones):
    t0, t1 = T0, T1
    ts = set(range(t0, t1, 3 * 3600))
    # the minute either side of every changeover zoneinfo knows, hour by hour
    for z, _ in zones:
        zi = ZoneInfo(z)
        prev = None
        for t in range(t0, t1, 3600):
            o = dt.datetime.fromtimestamp(t, zi).utcoffset()
            if prev is not None and o != prev:
                lo, hi = t - 3600, t
                while hi - lo > 1:                                # the second it changes
                    mid = (lo + hi) // 2
                    if dt.datetime.fromtimestamp(mid, zi).utcoffset() == prev: lo = mid
                    else: hi = mid
                ts.update((hi - 60, hi - 1, hi, hi + 60))
            prev = o
    return sorted(ts)


PROGRAM = r'''
#include <stdio.h>
#include <stdlib.h>
#include "orb_tzrule.h"
static const char *R[] = { %s };
int main(void)
{
    long long t;
    while (scanf("%%lld", &t) == 1) {
        for (unsigned i = 0; i < sizeof R / sizeof *R; i++) {
            int32_t m = 0; bool d = false;
            if (!orb_tzrule_offset(R[i], t, &m, &d)) { printf("x "); continue; }
            printf("%%d:%%d ", (int)m, d ? 1 : 0);
        }
        printf("\n");
    }
    return 0;
}
'''
BAD = ["", "STD", "ST5", "STD5DST4,M3.2.0", "STD5DST4,M13.2.0,M11.1.0", "STD5DST4,M3.6.0,M11.1.0",
       "STD5DST4,M3.2.7,M11.1.0", "STD5DST4,M3.2.0,M11.1.0x", "STD25", "STD5:6", "STD5DST4,M3.2.0/168,M11.1.0"]


def run(src_text, rules, ts):
    d = tempfile.mkdtemp(prefix="tzrule.")
    open(os.path.join(d, "orb_tzrule.c"), "w").write(src_text)
    open(os.path.join(d, "orb_tzrule.h"), "w").write(open(HDR).read())
    lits = ", ".join('"%s"' % r for r in rules + BAD)
    open(os.path.join(d, "main.c"), "w").write(PROGRAM % lits)
    exe = os.path.join(d, "t")
    r = subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-o", exe,
                        os.path.join(d, "main.c"), os.path.join(d, "orb_tzrule.c")], capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr
    out = subprocess.run([exe], input="\n".join(map(str, ts)) + "\n", capture_output=True, text=True).stdout.split("\n")
    return [line.split() for line in out if line.strip()], ""


def check(rows, rules, zones, ts):
    fails = []
    n = len(rules)
    for row in rows:
        if len(row) != n + len(BAD):
            return ["the program answered %d fields, not %d" % (len(row), n + len(BAD))]
        bad = [i for i, v in enumerate(row[n:]) if v != "x"]
        if bad:
            fails.append("a malformed rule was read: %r" % BAD[bad[0]])
            break
    seen = 0
    for z, r in zones:
        if r & 0x80 or z in TABLE_APPROX:
            continue
        zi, k = ZoneInfo(z), r & 0x7F
        # daylight is the larger of the zone's offsets in the window: Europe/Dublin's law calls
        # summer standard and winter the exception, which zoneinfo's dst() follows
        lo = min(dt.datetime.fromtimestamp(t, zi).utcoffset() for t in ts[::40])
        for i, t in enumerate(ts):
            # the instant in the zone: tzinfo.utcoffset() reads a datetime as that zone's wall time
            u = dt.datetime.fromtimestamp(t, zi)
            want = int(u.utcoffset().total_seconds() // 60)
            wdst = 1 if u.utcoffset() > lo else 0
            got = rows[i][k]
            seen += 1
            if got == "x" or got != "%d:%d" % (want, wdst):
                fails.append("%s (%s) at %s: %s, zoneinfo %d:%d" % (z, rules[k], u.isoformat(), got, want, wdst))
                break
    return fails, seen


def main():
    # held to the release the table was baked from, not whatever /usr/share carries
    # (CI, 2026-11-09: the IDF image's older zoneinfo read BC as still changing clocks)
    sys.path.insert(0, HERE)
    from make_cities import pin_tzdata
    print("tzrule: zone rules from", pin_tzdata())
    rules, zones = table()
    for rule, z in EXTRA:                       # extra rules, each with a zone to be held to
        zones.append((z, len(rules)))
        rules.append(rule)
    ts = instants([z for z in zones if not z[1] & 0x80])
    src = open(SRC).read()
    rows, err = run(src, rules, ts)
    if rows is None:
        print("FAIL compile: " + err)
        return 1
    fails, seen = check(rows, rules, zones, ts)
    exact = sum(1 for z, r in zones if not r & 0x80 and z not in TABLE_APPROX)
    for f in fails[:20]:
        print("FAIL " + f)
    print("tzrule: %d zones (%d held: the table's approximate ones and %d it does not mark are left out), %d rules, %d instants, %d zone-instants held to zoneinfo, %d failures"
          % (len(zones), exact, len(TABLE_APPROX), len(rules), len(ts), seen, len(fails)))
    bad = bool(fails)
    if "--mutations" in sys.argv:
        caught = 0
        for a, b in MUTATIONS:
            if src.count(a) != 1:
                print("FAIL mutant anchor %r found %d times" % (a, src.count(a)))
                bad = True
                continue
            mrows, _ = run(src.replace(a, b), rules, ts)
            got = ["compile"] if mrows is None else check(mrows, rules, zones, ts)[0]
            if got:
                caught += 1
            else:
                print("FAIL mutant survived: %r" % b)
                bad = True
        print("mutants: %d of %d caught" % (caught, len(MUTATIONS)))
    print("ALL PASS" if not bad else "FAILED")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
