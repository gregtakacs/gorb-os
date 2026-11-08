#!/usr/bin/env python3
"""Host bench for the orb's three places beside home (main/net/orb_places.c,
radar-plan.md §2.1).

The places are one OS setting every app reads, so the refusals are the contract: a
slot past the three, a position off the globe, a name that would break the route's
JSON echo or overrun its 40 bytes. The pure half of the module (everything above its
"end of the pure half" marker) is lifted VERBATIM and compiled with host gcc -Werror,
against orb_geo.c's own orb_geo_valid. The SDK's record is checked too: orb_place_t
must stay home's 52 B shape, since the import copies exactly that.

Run:  python3 tools/host_places_test.py              (rc 0 = pass)
      python3 tools/host_places_test.py --mutations  (proves the checks bite)
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "main", "net", "orb_places.c")
HDR = os.path.join(ROOT, "main", "net", "orb_places.h")
GEO = os.path.join(ROOT, "main", "net", "orb_geo.c")
SDK = os.path.join(ROOT, "apps", "sdk", "orb.h")
WASM = os.path.join(ROOT, "main", "app", "orb_app_wasm.c")

MUTATIONS = [
    ("if (i < 0 || i >= ORB_PLACES_N)", "if (i < 0 || i > ORB_PLACES_N)"),
    ("if (i < 0 || i >= ORB_PLACES_N)", "if (i >= ORB_PLACES_N)"),
    ("if (!orb_geo_valid(lat_e6, lon_e6))", "if (0)"),
    ("strlen(name) >= ORB_GEO_PLACE_MAX", "strlen(name) > ORB_GEO_PLACE_MAX"),
    ("*u == '\"' || *u == '\\\\'", "*u == '\"'"),
    ("if (*u < 0x20 || *u == 0x7f ||", "if (*u < 0x20 ||"),
    ("for (int k = 1; k <= n; k++) if ((u[k] & 0xC0) != 0x80) return false;", ";"),
    ("if (n < 0 || *u < 0xC2) return false;", "if (n < 0) return false;"),
    ("if (name && (!places_safe_text(name)", "if ((!places_safe_text(name)"),
]


def lifted(mutation=None):
    src = open(SRC).read()
    body = src[src.index("/* ---- pure: lifted"):src.index("/* ---- end of the pure half")]
    if mutation:
        old, new = mutation
        if old not in body:
            raise SystemExit("mutation %r does not apply -- rewrite the bench" % (old,))
        body = body.replace(old, new, 1)
    geo = open(GEO).read()
    valid = geo[geo.index("bool orb_geo_valid("):]
    valid = valid[:valid.index("\n}\n") + 3]
    hdr = open(HDR).read().replace("#pragma once", "").replace('#include "orb_geo.h"', "")
    return ("#include <stdbool.h>\n#include <stdint.h>\n#include <string.h>\n#include <stddef.h>\n"
            "#define ORB_GEO_PLACE_MAX 40\nbool orb_geo_valid(int32_t lat_e6, int32_t lon_e6);\n"
            + hdr + "\n" + valid + "\n" + body)


HARNESS = r"""
#include <stdio.h>
static int fails;
#define CK(c, label) do { if (c) printf("ok    %s\n", label); else { printf("FAIL  %s\n", label); fails++; } } while (0)
int main(void)
{
    CK(!orb_places_check(0, 38722300, -9139300, "Lisbon") && !orb_places_check(2, 0, 0, NULL) &&
       !orb_places_check(1, 0, 0, ""), "three slots, 0 to 2, take a place; no name or an empty one is fine");
    CK(orb_places_check(3, 0, 0, NULL) && orb_places_check(-1, 0, 0, NULL), "slot 3 and slot -1 are refused");
    CK(!orb_places_check(0, 90000000, 180000000, NULL) && !orb_places_check(0, -90000000, -180000000, NULL),
       "the poles and the date line are on the globe");
    CK(orb_places_check(0, 90000001, 0, NULL) && orb_places_check(0, 0, -180000001, NULL),
       "a millionth of a degree past them is not");
    char n39[40], n40[41];
    memset(n39, 'a', 39); n39[39] = 0;
    memset(n40, 'a', 40); n40[40] = 0;
    CK(!orb_places_check(0, 0, 0, n39) && orb_places_check(0, 0, 0, n40),
       "a 39-character name fits the 40-byte record, a 40-character one does not");
    CK(orb_places_check(0, 0, 0, "Say \"hi\"") && orb_places_check(0, 0, 0, "a\\b") &&
       orb_places_check(0, 0, 0, "tab\there") && orb_places_check(0, 0, 0, "bad\xc3") &&
       orb_places_check(0, 0, 0, "lone\xa9") && orb_places_check(0, 0, 0, "x\xc3" "Ay") && orb_places_check(0, 0, 0, "over\xc0\xaf") &&
       orb_places_check(0, 0, 0, "del\x7f"),
       "a quote, a backslash, a control character, DEL or broken UTF-8 would break the JSON echo");
    CK(!orb_places_check(0, 0, 0, "Fort Worth, TX (home-ish) 76102"), "ordinary punctuation is fine");
    CK(!orb_places_check(0, 0, 0, "Gy\xc5\x91r") && !orb_places_check(0, 0, 0, "Tatab\xc3\xa1nya") &&
       !orb_places_check(0, 0, 0, "Z\xc3\xbcrich"), "a name in its own letters is fine (radar-plan 6.22)");
    return fails ? 1 : 0;
}
"""


def run(code):
    with tempfile.TemporaryDirectory() as d:
        c, exe = os.path.join(d, "p.c"), os.path.join(d, "p")
        open(c, "w").write(code)
        r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-o", exe, c],
                           capture_output=True, text=True)
        if r.returncode:
            return 2, r.stderr
        r = subprocess.run([exe], capture_output=True, text=True)
        return r.returncode, r.stdout


def abi_checks():
    """The SDK record and the import agree: orb_place_t is orb_home_t, which is 52 B, and
    the firmware's import copies 52 B with the name at +12."""
    ok = True
    sdk = open(SDK).read()
    if "typedef orb_home_t orb_place_t;" not in sdk:
        print("FAIL  orb.h: orb_place_t is no longer home's record"); ok = False
    m = re.search(r"typedef struct \{\s*int32_t\s+lat_e6, lon_e6;\s*uint32_t gen;[^}]*char\s+place\[40\];\s*\} orb_home_t;", sdk)
    if not m:
        print("FAIL  orb.h: orb_home_t is not lat, lon, gen, place[40] (52 B)"); ok = False
    w = open(WASM).read()
    imp = w[w.index("m3ApiRawFunction(imp_place)"):]
    imp = imp[:imp.index("\n}\n")]
    if "m3ApiCheckMem(out, 52)" not in imp or "memcpy(out + 12, h.place, 40)" not in imp:
        print("FAIL  orb_app_wasm.c: imp_place does not copy home's 52 B record"); ok = False
    if '{ "orb", "place",         "i(ii)",     imp_place }' not in w:
        print("FAIL  orb_app_wasm.c: the place import is not registered as i(ii)"); ok = False
    print("ok    orb_place_t is home's 52 B record, and imp_place copies exactly that" if ok else "")
    return ok


def main():
    rc, out = run(lifted() + HARNESS)
    print(out, end="")
    ok = rc == 0 and abi_checks()
    if "--mutations" in sys.argv:
        caught = 0
        for m in MUTATIONS:
            r, _ = run(lifted(m) + HARNESS)
            hit = r != 0
            caught += hit
            print("%s mutant: %s -> %s" % ("caught" if hit else "MISSED", m[0][:48], m[1][:48]))
        print("%d of %d mutants caught" % (caught, len(MUTATIONS)))
        ok = ok and caught == len(MUTATIONS)
    print("all places checks pass" if ok else "places checks FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
