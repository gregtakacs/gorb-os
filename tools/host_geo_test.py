#!/usr/bin/env python3
"""Host check for main/net/orb_geo.c and its generated table, the home location's
arithmetic (flight-tracker-design §6.4).

    python3 tools/host_geo_test.py             # all checks
    python3 tools/host_geo_test.py --mutations # do the checks actually bite?

orb_geo.c and orb_cities.c are compiled as is (no IDF) with a C driver:

  olc       full plus codes decode to the official library's cell centres
            (the vectors below came from openlocationcode 1.0, seeded, and
            include grid digits, padding, both poles and lower case); short
            codes, misplaced '+', bad padding and off-the-globe first pairs
            are refused with a reason
  coords    "lat, lon" in its spellings parses to exact millionths; a missing
            separator, past-the-pole values and trailing junk are refused
  nearest   picks the right city, wraps the antimeridian, names it in the
            three distances' words with the compass from the city; a
            correction row (Boise) wins over a big city across a zone line
  zones     every generated rule fits the orb's tz guard (printable, no quote,
            <= 64), and the table is fresh against tools/make_cities.py, which
            re-verifies every rule against zoneinfo through glibc
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "main", "net", "orb_geo.c")
TABLE = os.path.join(ROOT, "main", "net", "orb_cities.c")
INC = [os.path.join(ROOT, "main", "net")]

OLC = [
    ('8559CWXG+9C', 33448437, -112073938),
    ('8FVC9G8F+6X', 47365562, 8524938),
    ('4RRH46J5+FPM', -33868787, 151209297),
    ('4VMP5Q26+2222', -36849998, 174760004),
    ('CVXXXXXX+XX', 89999938, 179999938),
    ('22222222+22', -89999938, -179999938),
    ('6FG22222+22', 62, 62),
    ('9C3XGV4C+XV834', 51507400, -127800),
    ('8Q7XMMG2+F4M4343', 35676200, 139650300),
    ('99X6R269+65X35', 69810600, -35982017),
    ('7Q7XJFG3+2G5F9FV', 15625012, 139453858),
    ('6GJGR4GG+9F', 2825938, 30126188),
    ('2P73HV7H+MRQGH2V', -84435788, 101879615),
    ('9QC5FF28+J3', 58451562, 123465188),
    ('67W8Q924+3P3', 8750138, -73643203),
    ('CRPR48HR+42', 84127812, 156340062),
    ('5JHMMR3Q+R92', -18345488, 73838391),
    ('3CC95J9G+G9', -61831188, -12374062),
    ('6J4GPMMP+9R78P', -7266592, 70687052),
    ('26X3QG8V+PHW68', -70233144, -98456058),
    ('5JXVWRFV+74', -10076812, 77842812),
    ('28X97PHM+JX', -70720938, -52265062),
    ('7CF7JPFX+38X4', 19622728, -14251637),
    ('37MC4MP7+WH', -56862688, -71336062),
    ('9GFF5X78+CMCPG', 59163568, 29966646),
    ('8JCMXR3G+738', 38953163, 73825203),
    ('27H4P775+7V', -78286812, -77740312),
    ('5587CWFM+GQ5Q3', -23576234, -114065505),
    ('8GQ65MCM+2Q6P3', 45170040, 24684394),
    ('9JFXP5Q9+RF', 59739562, 79168688),
    ('23HRW8MG+JWF', -78065938, -143672703),
    ('32H2CFG7+F65335F', -58573875, -179536895),
    ('9QXJM43J+6M2R', 69653023, 132131629),
    ('7FG49Q00+', 20375000, 2775000),
    ('47000000+', -40000000, -70000000),
    ('8FVC0000+', 47500000, 8500000),
    ('8FVC9G00+', 47375000, 8525000),
    ('8m48fwx3+qq', 32499438, 86904438),
]

OLC_BAD = [
    ("FWX3+QQ", "short code"),
    ("FWX3+QQ Phoenix", "short code"),
    ("8559CWXG9C", "no +"),
    ("8559CWXG9+C", "8th"),
    ("8559CWXGA+9C", "plus-code character"),
    ("8559CWXG+9", "one digit"),
    ("8559CWXG+9C+", "more than one"),
    ("8F000000+00", "padding"),
    ("8F0C0000+", "after the 0"),
    ("8F00000+", "short code"),
    ("X2222222+22", "pole"),
    ("2X222222+22", "180"),
]

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_geo.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct olc { const char *c; int la, lo; };
static const struct olc OK[] = { %(olc)s };
struct bad { const char *c, *why; };
static const struct bad BAD[] = { %(bad)s };

static void place_is(double la, double lo, const char *want, const char *what)
{
    char p[ORB_GEO_PLACE_MAX];
    orb_geo_place((int)(la * 1e6), (int)(lo * 1e6), p, sizeof p);
    CHECK(!strcmp(p, want), "place %%s: got \"%%s\", want \"%%s\"", what, p, want);
}

static void coords_ok(const char *s, int la, int lo)
{
    int32_t a = 0, b = 0;
    const char *e = orb_geo_parse_coords(s, &a, &b);
    CHECK(!e && a == la && b == lo, "coords \"%%s\" -> %%s %%d %%d", s, e ? e : "ok", (int)a, (int)b);
}

static void coords_bad(const char *s, const char *why)
{
    int32_t a = 7, b = 7;
    const char *e = orb_geo_parse_coords(s, &a, &b);
    CHECK(e && strstr(e, why) && a == 7 && b == 7, "coords \"%%s\" refused for \"%%s\": got %%s", s, why, e ? e : "accepted");
}

int main(void)
{
    /* ---- olc ---- */
    for (size_t i = 0; i < sizeof OK / sizeof OK[0]; i++) {
        int32_t a = 0, b = 0;
        const char *e = orb_geo_olc_decode(OK[i].c, &a, &b);
        CHECK(!e && abs(a - OK[i].la) <= 1 && abs(b - OK[i].lo) <= 1,
              "olc %%s -> %%s %%d %%d, want %%d %%d", OK[i].c, e ? e : "ok", (int)a, (int)b, OK[i].la, OK[i].lo);
    }
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        int32_t a = 0, b = 0;
        const char *e = orb_geo_olc_decode(BAD[i].c, &a, &b);
        CHECK(e && strstr(e, BAD[i].why), "olc %%s refused for \"%%s\": got %%s", BAD[i].c, BAD[i].why, e ? e : "accepted");
    }

    /* ---- coords ---- */
    coords_ok("33.4484, -112.074", 33448400, -112074000);
    coords_ok("33.4484,-112.074", 33448400, -112074000);
    coords_ok("  33.4484 -112.074 ", 33448400, -112074000);
    coords_ok("-0.5, +179.999999", -500000, 179999999);
    coords_ok("90,-180", 90000000, -180000000);
    coords_ok(".5,.25", 500000, 250000);
    coords_bad("33.4484", "comma");
    coords_bad("33.4484 x", "longitude");
    coords_bad("90.000001, 0", "90");
    coords_bad("0, 180.5", "180");
    coords_bad("1.1234567, 0", "latitude");
    coords_bad("1, 2, 3", "extra");
    coords_bad("abc, 2", "latitude");
    coords_bad("1234, 2", "latitude");

    /* ---- nearest + place ---- */
    place_is(33.4484, -112.0740, "Phoenix", "on Phoenix");
    place_is(33.60, -112.40, "near Phoenix", "30 km out");
    place_is(41.54, -104.98, "200 km N of Denver", "north of Denver");
    place_is(39.74, -102.40, "221 km E of Denver", "east of Denver");
    /* The correction rows (make_cities.py): Boise is under 500k, and without
     * its own row the nearest big city was Portland, in Pacific time. */
    place_is(43.6150, -116.2023, "Boise", "Boise, a correction row");
    {
        const int b = orb_geo_city_zone(orb_geo_nearest(43615000, -116202300, NULL));
        CHECK(!strcmp(orb_geo_zone_posix(b), "STD7DST6,M3.2.0,M11.1.0"),
              "Boise keeps Mountain time: %%s %%s", orb_geo_zone_iana(b), orb_geo_zone_posix(b));
    }
    /* -179.9 is 5 deg east of Auckland across the antimeridian; a scan that
     * does not wrap finds somewhere in the Americas */
    {
        uint32_t d = 0;
        const int c = orb_geo_nearest(-36850000, -179900000, &d);
        CHECK(!strcmp(orb_geo_city_name(c), "Auckland") && d > 400000 && d < 500000,
              "antimeridian: got %%s at %%u m", orb_geo_city_name(c), (unsigned)d);
    }
    /* At 60.5N a degree of longitude is half a degree of latitude: Moscow is
     * nearer in raw degrees, Saint Petersburg (293 km vs 542 km) on the ground */
    CHECK(!strcmp(orb_geo_city_name(orb_geo_nearest(60500000, 35500000, NULL)), "Saint Petersburg"),
          "high latitude: got %%s", orb_geo_city_name(orb_geo_nearest(60500000, 35500000, NULL)));
    CHECK(orb_geo_dist_m(0, 0, 0, 1000000) > 111000 && orb_geo_dist_m(0, 0, 0, 1000000) < 111400,
          "a degree of longitude at the equator: %%u m", (unsigned)orb_geo_dist_m(0, 0, 0, 1000000));

    /* ---- zones ---- */
    const int phx = orb_geo_city_zone(orb_geo_nearest(33448400, -112074000, NULL));
    CHECK(!strcmp(orb_geo_zone_iana(phx), "America/Phoenix") && !strcmp(orb_geo_zone_posix(phx), "STD7"),
          "Phoenix's zone is %%s %%s", orb_geo_zone_iana(phx), orb_geo_zone_posix(phx));
    const int den = orb_geo_zone_find("America/Denver");
    CHECK(den >= 0 && !strcmp(orb_geo_zone_posix(den), "STD7DST6,M3.2.0,M11.1.0") && orb_geo_zone_exact(den),
          "Denver's rule: %%s", orb_geo_zone_posix(den));
    /* 2026-11-08: tzdata's own rules (changes past 24 h) made Jerusalem and Santiago exact;
     * Gaza follows Ramadan, which no fixed rule can, and is flagged */
    const int jer = orb_geo_zone_find("Asia/Jerusalem");
    CHECK(jer >= 0 && orb_geo_zone_exact(jer) && !strcmp(orb_geo_zone_posix(jer), "STD-2DST-3,M3.4.4/26,M10.5.0"),
          "Jerusalem's rule is tzdata's own and exact: %%s", orb_geo_zone_posix(jer));
    const int scl = orb_geo_zone_find("America/Santiago");
    CHECK(scl >= 0 && orb_geo_zone_exact(scl) && !strcmp(orb_geo_zone_posix(scl), "STD4DST3,M9.1.6/24,M4.1.6/24"),
          "Santiago changes on the Sunday on or after the 2nd: %%s", orb_geo_zone_posix(scl));
    const int gza = orb_geo_zone_find("Asia/Gaza");
    CHECK(gza < 0 || !orb_geo_zone_exact(gza), "Gaza is flagged approximate");
    CHECK(orb_geo_zone_find("Mars/Olympus") < 0 && !strcmp(orb_geo_zone_iana(-1), "") &&
          !strcmp(orb_geo_zone_posix(ORB_GEO_N_ZONES), ""), "bad zone indices answer empty");
    for (int z = 0; z < ORB_GEO_N_ZONES; z++) {
        const char *r = orb_geo_zone_posix(z);
        int ok = r[0] && strlen(r) <= 64;
        for (const char *p = r; *p; p++)
            if (*p < 0x20 || *p > 0x7e || *p == '"' || *p == '\\') ok = 0;
        CHECK(ok, "zone %%s rule \"%%s\" would fail the tz guard", orb_geo_zone_iana(z), r);
    }
    for (int c = 0; c < ORB_GEO_N_CITIES; c++) {
        const size_t n = strlen(orb_geo_city_name(c));
        CHECK(n > 0 && n < ORB_GEO_NAME_MAX && orb_geo_city_zone(c) < ORB_GEO_N_ZONES,
              "city %%d is malformed", c);
    }

    char f[24];
    orb_geo_fmt(-112074000, f, sizeof f);
    CHECK(!strcmp(f, "-112.074000"), "fmt: %%s", f);
    orb_geo_fmt(-500000, f, sizeof f);
    CHECK(!strcmp(f, "-0.500000"), "fmt keeps the sign below one degree: %%s", f);

    printf("%%s %%d/%%d geo checks\n", fails ? "FAIL" : "ok", checks - fails, checks);
    return fails ? 1 : 0;
}
"""


def driver():
    olc = ", ".join('{"%s", %d, %d}' % v for v in OLC)
    bad = ", ".join('{"%s", "%s"}' % v for v in OLC_BAD)
    return DRIVER % {"olc": olc, "bad": bad}


def build(src, out):
    drv = out + "_drv.c"
    open(drv, "w").write(driver())
    args = ["gcc", "-std=c11", "-O1", "-Wall", "-Werror", "-Wno-unused-but-set-variable", "-Wno-unused-variable", "-D_GNU_SOURCE"]
    for i in INC:
        args += ["-I", i]
    r = subprocess.run(args + [drv, src, TABLE, "-lm", "-o", out], capture_output=True, text=True)
    if r.returncode:
        sys.exit("build failed:\n" + r.stderr)


def run(binary):
    r = subprocess.run([binary], capture_output=True, text=True)
    return r.returncode, r.stdout


def main():
    b = os.path.join(tempfile.mkdtemp(), "geo")
    build(SRC, b)
    rc, out = run(b)
    print(out.strip())
    fr = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "make_cities.py"), "--check"],
                        capture_output=True, text=True)
    print(("ok    " if fr.returncode == 0 else "FAIL  ") + (fr.stdout.strip() or fr.stderr.strip()))
    if rc or fr.returncode:
        sys.exit(1)
    if "--mutations" in sys.argv:
        src = open(SRC).read()
        mutants = [
            ("olc lat/lon swapped", "lat += olc_val(d[i]) * PAIR[i / 2];", "lon += olc_val(d[i]) * PAIR[i / 2];"),
            ("olc no half-cell centre", "(double)(2 * lat + lat_cell)", "(double)(2 * lat)"),
            ("olc grid rows/cols swapped", "lat += (v / 4) * lat_cell;", "lat += (v % 4) * lat_cell;"),
            ("olc short codes accepted", 'if (plus + pad < 8) return "short code: needs the full 8 before the +";', ""),
            ("olc case-sensitive", "if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');", ""),
            ("olc pole not checked", 'if (olc_val(d[0]) > 8) return "first character is past the pole";', ""),
            ("coords float rounding", "for (int k = nf; k < 6; k++) frac *= 10;", "for (int k = nf; k < 5; k++) frac *= 10;"),
            ("coords lose the sign", "*out = (int32_t)(neg ? -v : v);", "*out = (int32_t)v;"),
            ("coords no range check", 'if (la < -90000000 || la > 90000000) return "latitude is past +-90";', ""),
            ("nearest no antimeridian wrap", "if (dx > 18000) dx -= 36000;", ""),
            ("nearest ignores cos(lat)", "const float cdx = dx * (c < 0.05f ? 0.05f : c);", "const float cdx = dx;"),
            ("place compass from home, not city", "float b = atan2f(", "float b = (float)M_PI + atan2f("),
            ("place no 'near' band", "if (d < 80000) {", "if (d < 15000) {"),
            ("approximate flag ignored", "!(ORB_GEO_ZONES[zone].rule & 0x80)", "1"),
        ]
        survived = []
        for label, a, z in mutants:
            if a not in src:
                sys.exit("mutation anchor gone: " + label)
            p = os.path.join(tempfile.mkdtemp(), "orb_geo.c")
            open(p, "w").write(src.replace(a, z, 1))
            mb = p[:-2]
            build(p, mb)
            if run(mb)[0] == 0:
                survived.append(label)
        if survived:
            sys.exit("mutants survived: " + ", ".join(survived))
        print("MUTATIONS %d/%d killed" % (len(mutants), len(mutants)))


main()
