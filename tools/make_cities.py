#!/usr/bin/env python3
"""make_cities.py — bake the home-location city list (flight-tracker-design §6.4).

The orb names a home it was given as bare coordinates ("near Phoenix") and
proposes its time zone from the nearest big city, with no geocoder and no
network. This tool is the one author of that table, main/net/orb_cities.c.

Two steps, so the committed table can be re-derived without the network:

    # 1. refresh the committed source rows from GeoNames (rarely):
    python3 tools/make_cities.py --import cities15000.txt
    # 2. bake the C table from them (and whenever this file changes):
    python3 tools/make_cities.py
    python3 tools/make_cities.py --check     # the freshness gate

Step 1 keeps places of 500k people or more, drops sections of a city (GeoNames
feature PPLX: Brooklyn would otherwise name Manhattan), and folds any place
within 15 km of a bigger kept one into it. Then it adds the correction rows:
every place of 100k-500k, biggest first, whose zone rule differs from the rule
of the nearest place already kept. The 500k list alone has whole regions where
the nearest big city sits across a zone line (Boise was "555 km E of Portland"
in Pacific time on the bench); about 200 such rows fix the zone and give the
place its own name, for ~4 KB. Rules are compared, not zone names: Detroit and
New York are one clock. The rows land in tools/data/cities500k.tsv. The GeoNames data is CC-BY 4.0
(https://www.geonames.org/), credited in the generated file.

Step 2 turns each IANA zone the cities use into a POSIX rule, because the orb's
newlib has no zoneinfo. The derivation is the clock page's (tabs.js tzSynth,
host_tz_test.py _trans): bisect each month for the transitions, encode them as
M-rules in the local wall time before the change. Every rule is then checked
against this machine's zoneinfo through glibc (TZ=<rule>, which reads POSIX
rules the way newlib does) at every 2025-2028 transition and daily through
2026. A zone whose clock change follows no fixed rule (Asia/Jerusalem's
"Friday before the last Sunday", Africa/Casablanca's Ramadan pause) fails that
check; it ships as its standard offset with the approximate flag, and the
orb says so when it proposes it. Identical rules are stored once.

The same bake writes the hub's copy, tools/orb-manager/web/cities.json: the
same rows, biggest first, as [name, country, lat, lon] for the Orb face's
place search. The hub sends a picked city's point and the orb names it and
zones it from its own table, so the two lists agreeing is what makes a
search for "Boise" come back as "Boise" in Mountain time. --check gates both.

Coordinates pack to 0.01 degree in int16 (about 1 km: enough to say which city
is nearest, and the home itself is stored at full precision elsewhere).
"""
import argparse
import datetime as dt
import json
import os
import sys
import time
import zoneinfo
from zoneinfo import ZoneInfo

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC_TSV = os.path.join(HERE, "data", "cities500k.tsv")
OUT_C = os.path.join(ROOT, "main", "net", "orb_cities.c")
OUT_JSON = os.path.join(HERE, "orb-manager", "web", "cities.json")

MIN_POP = 500_000
FIX_POP = 100_000       # correction rows: smaller places whose zone the big ones get wrong
FOLD_KM = 15.0
NAME_MAX = 23           # orb_geo.h ORB_GEO_NAME_MAX - 1
YEARS = (2026, 2027, 2028)
# A home is set today, so the rules describe today onward: derived from 2027
# (the first whole year ahead) and checked from this date. History is not the
# orb's business, and tzdata 2026e put BC, Alberta and Manitoba on permanent
# daylight time from March 2026, which a 2025 probe would call a broken rule.
DERIVE_YEAR = 2027

# The rules are derived from the tz database, so the table is only as reproducible as
# the database it was baked from: a host on an older release bakes different bytes and
# --check calls the committed table stale (CI, 2026-11-03: the IDF container's tzdata).
# One release is pinned. The pip package (`pip install tzdata==2026.5`) is used when it
# is that release, the system database when it is, and anything else refuses.
TZDATA = "2026e"


def pin_tzdata():
    try:
        import tzdata
        if tzdata.IANA_VERSION == TZDATA:
            zoneinfo.reset_tzpath(to=[])          # read the package, not /usr/share
            return "tzdata package " + TZDATA
    except ImportError:
        pass
    for d in zoneinfo.TZPATH:
        try:
            with open(os.path.join(d, "tzdata.zi")) as f:
                if f.readline().strip() == "# version " + TZDATA:
                    return "system " + d + " " + TZDATA
        except OSError:
            continue
    sys.exit("make_cities: the zone rules are baked from tz database %s, and this host has "
             "no copy of it: pip install tzdata==2026.5 (or move TZDATA and re-bake)" % TZDATA)
VERIFY_FROM = int(dt.datetime(2026, 11, 1, tzinfo=dt.timezone.utc).timestamp())


def km(a, b):
    import math
    la1, lo1, la2, lo2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = (math.sin((la2 - la1) / 2) ** 2
         + math.cos(la1) * math.cos(la2) * math.sin((lo2 - lo1) / 2) ** 2)
    return 6371.0 * 2 * math.asin(math.sqrt(h))


def _flat_nearest(rows, la, lo):
    """The orb's own scan (orb_geo_nearest): cos(lat)-scaled degrees, wrapped."""
    import math
    c = max(math.cos(math.radians(la)), 0.05)
    return min(rows, key=lambda r: (r[2] - la) ** 2 + ((((r[3] - lo) + 180) % 360 - 180) * c) ** 2)


def do_import(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            c = line.rstrip("\n").split("\t")
            if len(c) < 18 or not c[14].isdigit() or int(c[14]) < FIX_POP:
                continue
            if c[7] == "PPLX":
                continue
            name = c[2].strip() or c[1].strip()
            if not name or not name.isascii():
                continue
            rows.append((int(c[14]), name[:NAME_MAX], float(c[4]), float(c[5]), c[8], c[17]))
    rows.sort(key=lambda r: (-r[0], r[1]))
    kept = []
    for r in rows:
        if r[0] < MIN_POP:
            continue
        if any(km((r[2], r[3]), (k[2], k[3])) < FOLD_KM for k in kept):
            continue
        kept.append(r)
    rule = {}
    def rule_of(z):
        if z not in rule:
            rule[z] = derive(z)[0]
        return rule[z]
    big = len(kept)
    for r in rows:
        if r[0] >= MIN_POP:
            continue
        if rule_of(_flat_nearest(kept, r[2], r[3])[5]) != rule_of(r[5]):
            kept.append(r)
    os.makedirs(os.path.dirname(SRC_TSV), exist_ok=True)
    with open(SRC_TSV, "w", encoding="ascii") as f:
        f.write("# population\tname\tlat\tlon\tcountry\tzone"
                "  (GeoNames cities15000, CC-BY 4.0; tools/make_cities.py --import;"
                " 500k+ first, then the 100k+ zone corrections)\n")
        for r in kept:
            f.write("%d\t%s\t%.4f\t%.4f\t%s\t%s\n" % r)
    print("%d cities (%d of 500k+, %d zone corrections) -> %s"
          % (len(kept), big, len(kept) - big, SRC_TSV))


def load_rows():
    out = []
    with open(SRC_TSV, encoding="ascii") as f:
        for line in f:
            if line.startswith("#"):
                continue
            p, n, la, lo, cc, z = line.rstrip("\n").split("\t")
            out.append((int(p), n, float(la), float(lo), cc, z))
    return out


# ---- zone rules -------------------------------------------------------------

def _off(z, ts):
    d = dt.datetime.fromtimestamp(ts, dt.timezone.utc).astimezone(z)
    return int(d.utcoffset().total_seconds() // 60)


def _trans(z, year):
    out = []
    for m in range(1, 13):
        a0 = int(dt.datetime(year, m, 1, tzinfo=dt.timezone.utc).timestamp()) // 60
        a1 = int(dt.datetime(year + (m == 12), 1 if m == 12 else m + 1, 1,
                             tzinfo=dt.timezone.utc).timestamp()) // 60
        o0, o1 = _off(z, a0 * 60), _off(z, a1 * 60)
        if o0 == o1:
            continue
        lo, hi = a0, a1
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if _off(z, mid * 60) == o0:
                lo = mid
            else:
                hi = mid
        out.append((hi * 60, o0, o1))
    return out


def _foff(east_min):
    w = -east_min
    s = "-" if w < 0 else ""
    a = abs(w)
    return s + str(a // 60) + (":%02d" % (a % 60) if a % 60 else "")


def _mrule(t, pre_east):
    d = dt.datetime.fromtimestamp(t + pre_east * 60, dt.timezone.utc)
    nxt = (d.replace(day=28) + dt.timedelta(days=4)).replace(day=1)
    dim = (nxt - dt.timedelta(days=1)).day
    wk = 5 if d.day + 7 > dim else (d.day - 1) // 7 + 1
    wd = (d.weekday() + 1) % 7                      # POSIX: 0 = Sunday
    hh, mm = d.hour, d.minute
    tt = "" if (hh, mm) == (2, 0) else "/%d" % hh + (":%02d" % mm if mm else "")
    return "M%d.%d.%d%s" % (d.month, wk, wd, tt)


def derive(zname):
    """(rule, exact) for one IANA zone."""
    z = ZoneInfo(zname)
    ts = _trans(z, DERIVE_YEAR)
    if not ts:
        mid = int(dt.datetime(DERIVE_YEAR, 7, 1, tzinfo=dt.timezone.utc).timestamp())
        return "STD" + _foff(_off(z, mid)), True
    if len(ts) == 2:
        a, b = ts
        std, dst = min(a[1], b[1]), max(a[1], b[1])
        on = a if a[2] == dst else b
        off = b if on is a else a
        rule = "STD%sDST%s,%s,%s" % (_foff(std), _foff(dst),
                                     _mrule(on[0], on[1]), _mrule(off[0], off[1]))
        return rule, True
    # an odd number of changes in the year: no static rule; the standard time
    # (the smaller offset in force that year) is the honest approximation
    offs = sorted({o for t in ts for o in t[1:]})
    return "STD" + _foff(offs[0]), False


def glibc_ok(zname, rule):
    """Does glibc, reading `rule`, agree with zoneinfo everywhere we probe?"""
    z = ZoneInfo(zname)
    probes = []
    for y in YEARS:
        for t, lo, hi in _trans(z, y):
            if t - 60 >= VERIFY_FROM:
                probes += [(t - 60, lo), (t, hi), (t + 3540, hi)]
    base = VERIFY_FROM + 12 * 3600
    probes += [(base + k * 86400, _off(z, base + k * 86400)) for k in range(800)]
    old = os.environ.get("TZ")
    os.environ["TZ"] = rule
    time.tzset()
    try:
        return all(time.localtime(t).tm_gmtoff // 60 == want for t, want in probes)
    finally:
        if old is None:
            del os.environ["TZ"]
        else:
            os.environ["TZ"] = old
        time.tzset()


# ---- the C file ---------------------------------------------------------------

def bake():
    rows = load_rows()
    zones = sorted({r[5] for r in rows})
    rules, rule_ix, zone_rule, inexact = [], {}, [], []
    for zn in zones:
        rule, exact = derive(zn)
        if exact and not glibc_ok(zn, rule):
            exact = False
            rule = "STD" + _foff(min(_off(ZoneInfo(zn), t) for t in
                                     (int(dt.datetime(DERIVE_YEAR, m, 1, tzinfo=dt.timezone.utc).timestamp())
                                      for m in (1, 7))))
        if len(rule) > 64:
            sys.exit("rule for %s is %d chars, over NET_TZ_MAX" % (zn, len(rule)))
        if not exact:
            inexact.append(zn)
        if rule not in rule_ix:
            rule_ix[rule] = len(rules)
            rules.append(rule)
        zone_rule.append((rule_ix[rule], exact))
    if len(zones) > 255 or len(rules) > 127:
        sys.exit("zone index no longer fits a byte (%d zones, %d rules)" % (len(zones), len(rules)))

    names, name_off = [], []
    off = 0
    for r in rows:
        name_off.append(off)
        names.append(r[1])
        off += len(r[1]) + 1
    if off > 0xFFFF:
        sys.exit("name blob %d B no longer fits a u16 offset" % off)

    L = []
    w = L.append
    w("/* GENERATED by tools/make_cities.py from tools/data/cities500k.tsv -- do not edit.")
    w(" * City data: GeoNames (https://www.geonames.org/), CC-BY 4.0.")
    w(" * %d cities, %d zones, %d distinct rules; %d zone(s) approximate: %s */"
      % (len(rows), len(zones), len(rules), len(inexact), ", ".join(inexact) or "none"))
    w('#include "orb_geo.h"')
    w("")
    w("const char *const ORB_GEO_RULES[] = {")
    for r in rules:
        w('    "%s",' % r)
    w("};")
    w("")
    w("/* rule index in the low 7 bits; bit 7 = the rule only approximates the zone */")
    w("const orb_geo_zone_t ORB_GEO_ZONES[] = {")
    for zn, (ri, ex) in zip(zones, zone_rule):
        w('    { "%s", %d },' % (zn, ri | (0 if ex else 0x80)))
    w("};")
    w("const int ORB_GEO_N_ZONES = %d;" % len(zones))
    w("")
    w("const char ORB_GEO_NAMES[] =")
    for i in range(0, len(names), 6):
        w('    "' + "".join(n.replace('"', "") + "\\0" for n in names[i:i + 6]) + '"')
    w("    ;")
    w("")
    w("/* lat, lon in 0.01 deg; name offset; zone index -- largest population first */")
    w("const orb_geo_city_t ORB_GEO_CITIES[] = {")
    zix = {z: i for i, z in enumerate(zones)}
    for r, no in zip(rows, name_off):
        w("    { %d, %d, %d, %d }," % (round(r[2] * 100), round(r[3] * 100), no, zix[r[5]]))
    w("};")
    w("const int ORB_GEO_N_CITIES = %d;" % len(rows))
    return "\n".join(L) + "\n", inexact, len(rules)


def bake_json():
    rows = sorted(load_rows(), key=lambda r: -r[0])
    return ('{"source":"GeoNames (https://www.geonames.org/), CC-BY 4.0; tools/make_cities.py",'
            '"cities":[\n' + ",\n".join(json.dumps([r[1], r[4], r[2], r[3]]) for r in rows) + "\n]}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--import", dest="imp", metavar="cities15000.txt")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    pin_tzdata()
    if a.imp:
        do_import(a.imp)
    text, inexact, nrules = bake()
    hub = bake_json()
    if a.check:
        for path, want in ((OUT_C, text), (OUT_JSON, hub)):
            cur = open(path).read() if os.path.exists(path) else ""
            if cur != want:
                sys.exit("%s is stale: re-run tools/make_cities.py" % os.path.basename(path))
        print("orb_cities.c and cities.json are fresh")
        return
    with open(OUT_C, "w") as f:
        f.write(text)
    with open(OUT_JSON, "w") as f:
        f.write(hub)
    print("wrote %s (%d B source, %d rules; approximate: %s)"
          % (OUT_C, len(text), nrules, ", ".join(inexact) or "none"))


if __name__ == "__main__":
    main()
