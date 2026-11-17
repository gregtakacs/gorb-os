#!/usr/bin/env python3
"""Host check for main/net/orb_geo.c and its generated table, the home location's
arithmetic (flight-tracker-design §6.4).

    python3 tools/host_geo_test.py             # all checks
    python3 tools/host_geo_test.py --mutations # do the checks actually bite?

orb_geo.c and orb_cities.c are compiled as is (no IDF) with a C driver:

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

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "orb_geo.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)


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
    /* ---- search (app-platform-plan §67) ---- */
    {
        int hit[8];
        int n = orb_geo_city_search("lond", hit, 8);
        CHECK(n >= 1 && !strncmp(orb_geo_city_name(hit[0]), "London", 6), "search lond leads with London: %%d %%s", n, n ? orb_geo_city_name(hit[0]) : "-");
        n = orb_geo_city_search("LONDON", hit, 8);
        CHECK(n >= 1 && !strcmp(orb_geo_city_name(hit[0]), "London"), "search is case-insensitive");
        n = orb_geo_city_search("york", hit, 8);
        int ny = 0;
        for (int i = 0; i < n; i++) if (!strcmp(orb_geo_city_name(hit[i]), "New York City")) ny = 1;
        CHECK(ny, "a later word matches: york finds New York");
        n = orb_geo_city_search("ork", hit, 8);
        int mid = 0;
        for (int i = 0; i < n; i++) if (!strcmp(orb_geo_city_name(hit[i]), "New York City")) mid = 1;
        CHECK(!mid, "the middle of a word does not match: ork misses New York");
        CHECK(orb_geo_city_search("", hit, 8) == 0 && orb_geo_city_search("   ", hit, 8) == 0, "an empty query finds nothing");
        CHECK(orb_geo_city_search("a", hit, 3) == 3, "max caps the answer");
        int32_t a = 0, b = 0;
        n = orb_geo_city_search("Phoenix", hit, 1);
        orb_geo_city_ll(hit[0], &a, &b);
        CHECK(n == 1 && abs(a - 33450000) <= 10000 && abs(b + 112070000) <= 10000, "Phoenix's position: %%d %%d", (int)a, (int)b);
        n = orb_geo_city_search("san ", hit, 8);
        int starts = 1;
        for (int i = 0; i < n; i++) if (strncasecmp(orb_geo_city_name(hit[i]), "san ", 4)) starts = 0;
        CHECK(n > 0 && starts, "a trailing space keeps to whole words: every san hit starts San ");
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
    return DRIVER.replace("%%", "%")


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
            ("search case-sensitive", "static int fold(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }", "static int fold(int c) { return c; }"),
            ("search no later words", "if (pass == 0 ? starts : (!starts && word_prefix(nm, q))) out[n++] = c;", "if (pass == 0 ? starts : (!starts && word_prefix(nm, q) && 0)) out[n++] = c;"),
            ("search mid-word", "if (w != name && w[-1] != ' ' && w[-1] != '-' && w[-1] != '\\'') continue;", ""),
            ("search ignores max", "for (int c = 0; c < ORB_GEO_N_CITIES && n < max; c++) {", "for (int c = 0; c < ORB_GEO_N_CITIES; c++) { if (n >= 8) break;"),
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
