#!/usr/bin/env python3
"""Host bench for the orb's units (main/net/orb_units.c, weather-plan §2.1).

The units are one OS setting every app reads, and the numbering is the app ABI:
orb.h's ORB_UNIT_* must name the same values the OS stores, the route's keys must
round-trip, and a value past a quantity's choices must never be accepted (an app
indexing a label table by it would read past the end). The pure half of the module
is lifted VERBATIM (the tables and the five functions, everything above
orb_units_init) and compiled with host gcc -Werror; orb.h's enum is read from the
SDK header itself, so a drift between the two is a failure here.

Run:  python3 tools/host_units_test.py              (rc 0 = pass)
      python3 tools/host_units_test.py --mutations  (proves the checks bite)
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "main", "net", "orb_units.c")
HDR = os.path.join(ROOT, "main", "net", "orb_units.h")
SDK = os.path.join(ROOT, "apps", "sdk", "orb.h")

MUTATIONS = [
    ("u->temp < ORB_UNIT_TEMP_N", "u->temp <= ORB_UNIT_TEMP_N"),
    ("u->dist < ORB_UNIT_DIST_N", "u->dist < 9"),
    ("if (v < 0 || v >= orb_units_count(kind)) return NULL;\n    return KEYS", "if (v < 0) return NULL;\n    return KEYS"),
    ('{ "kmh", "mph", "kn" }', '{ "kmh", "kn", "mph" }'),
    ("if (!strcmp(KEYS[kind][v], key)) return v;", "if (!strncmp(KEYS[kind][v], key, 1)) return v;"),
    ("case ORB_UNITS_SPEED: return ORB_UNIT_SPEED_N;", "case ORB_UNITS_SPEED: return ORB_UNIT_TEMP_N;"),
]


def lifted(mutation=None):
    src = open(SRC).read()
    start = src.index("static const char *const KEYS")
    end = src.index("void orb_units_init(void)")
    body = src[start:end]
    if mutation:
        old, new = mutation
        if old not in body:
            raise SystemExit("mutation %r does not apply -- rewrite the bench" % old)
        body = body.replace(old, new)
    hdr = open(HDR).read().replace("#pragma once", "")
    return hdr + "\n#include <string.h>\n" + body


HARNESS = r"""
#include <stdio.h>
static int fails;
#define CK(c, label) do { if (c) printf("ok    %s\n", label); else { printf("FAIL  %s\n", label); fails++; } } while (0)
int main(void)
{
    CK(orb_units_count(ORB_UNITS_TEMP) == 2 && orb_units_count(ORB_UNITS_SPEED) == 3 &&
       orb_units_count(ORB_UNITS_DIST) == 3 && orb_units_count(3) == 0 && orb_units_count(-1) == 0,
       "two temperatures, three speeds, three distances, nothing else");
    int rt = 1;
    for (int k = 0; k < ORB_UNITS_KINDS; k++)
        for (int v = 0; v < orb_units_count(k); v++)
            rt &= orb_units_parse(k, orb_units_key(k, v)) == v && orb_units_label(k, v) != NULL;
    CK(rt, "every key parses back to its own value and has a label");
    CK(!strcmp(orb_units_key(ORB_UNITS_TEMP, ORB_UNIT_F), "f") &&
       !strcmp(orb_units_key(ORB_UNITS_SPEED, ORB_UNIT_MPH), "mph") &&
       !strcmp(orb_units_key(ORB_UNITS_SPEED, ORB_UNIT_KN), "kn") &&
       !strcmp(orb_units_key(ORB_UNITS_DIST, ORB_UNIT_NM), "nm"),
       "the route's keys are the ones the hub sends");
    CK(!strcmp(orb_units_label(ORB_UNITS_TEMP, ORB_UNIT_C), "\xC2\xB0" "C") &&
       !strcmp(orb_units_label(ORB_UNITS_SPEED, ORB_UNIT_KMH), "km/h"),
       "the dial's labels");
    CK(orb_units_key(ORB_UNITS_TEMP, 2) == NULL && orb_units_key(ORB_UNITS_DIST, 3) == NULL &&
       orb_units_key(ORB_UNITS_SPEED, -1) == NULL && orb_units_label(ORB_UNITS_TEMP, 2) == NULL &&
       orb_units_key(ORB_UNITS_TEMP, 4) == NULL,   /* in the table, a row on: KEYS[1][1] */
       "no key or label past a quantity's choices");
    CK(orb_units_parse(ORB_UNITS_SPEED, "k") == -1 && orb_units_parse(ORB_UNITS_SPEED, "km") == -1 &&
       orb_units_parse(ORB_UNITS_DIST, "kmh") == -1 && orb_units_parse(ORB_UNITS_TEMP, NULL) == -1 &&
       orb_units_parse(ORB_UNITS_TEMP, "") == -1,
       "a key that is not exactly one of the list is refused");
    orb_units_t u = { ORB_UNIT_F, ORB_UNIT_KN, ORB_UNIT_NM };
    CK(orb_units_valid(&u), "the last choice of each is valid");
    u.temp = 2;  int bad = !orb_units_valid(&u); u.temp = 0;
    u.speed = 3; bad &= !orb_units_valid(&u); u.speed = 0;
    u.dist = 3;  bad &= !orb_units_valid(&u); u.dist = 0;
    CK(bad && !orb_units_valid(NULL), "one past any quantity's choices is refused, and NULL is");
    return fails ? 1 : 0;
}
"""


def run(code):
    with tempfile.TemporaryDirectory() as d:
        c, exe = os.path.join(d, "u.c"), os.path.join(d, "u")
        open(c, "w").write(code)
        r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-o", exe, c],
                           capture_output=True, text=True)
        if r.returncode:
            return 2, r.stderr
        p = subprocess.run([exe], capture_output=True, text=True)
        return p.returncode, p.stdout


def sdk_matches():
    """orb.h's ORB_UNIT_* are the app ABI: the same names, the same values."""
    sdk = open(SDK).read()
    hdr = open(HDR).read()
    names = lambda t: dict((n, int(v)) for n, v in re.findall(r"(ORB_UNIT_[A-Z]+) = (\d+)", t))
    a, b = names(sdk), names(hdr)
    return bool(a) and all(b.get(k) == v for k, v in a.items()) and \
        set(k for k in b if not k.endswith("_N")) == set(a)


def main():
    if "--mutations" in sys.argv:
        caught = 0
        for m in MUTATIONS:
            rc, _ = run(lifted(m) + HARNESS)
            print(("bites " if rc else "MISSED") + "  " + m[0][:60])
            caught += bool(rc)
        print("%d of %d mutants caught" % (caught, len(MUTATIONS)))
        return 0 if caught == len(MUTATIONS) else 1
    rc, out = run(lifted() + HARNESS)
    print(out, end="")
    ok = sdk_matches()
    print(("ok    " if ok else "FAIL  ") + "orb.h's ORB_UNIT_* are the OS's own numbers")
    return 0 if rc == 0 and ok else 1


if __name__ == "__main__":
    sys.exit(main())
