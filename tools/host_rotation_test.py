#!/usr/bin/env python3
"""Host bench for the carousel's order and rotation (main/app/orb_rotation.c, plan §59).

The shipped file compiles here with host gcc (ORB_ROTATION_HOST leaves NVS out) and its pure
decisions are held to what the owner asked for (2026-11-07):

- a slug list parses trimmed, without empties or repeats, and never past its table;
- an app the order names ranks by its place; one it does not name ranks after every named
  one, in slot order;
- the rotation moves only when it is on, only from an app that takes part, only once the
  knob has been left alone a whole interval, and to the next app taking part, wrapping;
  an app that is the only one taking part stays;
- an interval outside 10 s to a day is refused, and the refusal changes nothing.

Run:  python3 tools/host_rotation_test.py              (rc 0 = pass)
      python3 tools/host_rotation_test.py --mutations  (proves the checks bite)
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "main", "app", "orb_rotation.c")

MUTATIONS = [
    ("if (!in_set(r, walk[cur])) return -1;", ";"),
    ("if (quiet_us < (int64_t)r->every_s * 1000000) return -1;", "if (quiet_us < (int64_t)r->every_s * 1000) return -1;"),
    ("const int i = (cur + k) % n;", "const int i = cur + k < n ? cur + k : n - 1;"),
    ("return ORB_ROTATION_MAX + fallback;", "return fallback;"),
    ("if (!dup) { memcpy(out[n], slug, len + 1); n++; }", "{ memcpy(out[n], slug, len + 1); n++; }"),
    ("if (every_s < ORB_ROTATION_MIN_S || every_s > ORB_ROTATION_MAX_S)", "if (every_s > ORB_ROTATION_MAX_S)"),
    ("if (!r || !r->on || !walk || cur < 0 || cur >= n) return -1;", "if (!r || !walk || cur < 0 || cur >= n) return -1;"),
]

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "orb_rotation.h"
static int fails, checks;
#define CK(c, m) do { checks++; if (!(c)) { fails++; printf("FAIL %s\n", m); } else printf("ok   %s\n", m); } while (0)
int main(void)
{
    char out[ORB_ROTATION_MAX][ORB_ROTATION_SLUG];
    int n = orb_rotation_csv(" weather, flights,,clock ,weather", out, ORB_ROTATION_MAX);
    CK(n == 3 && !strcmp(out[0], "weather") && !strcmp(out[1], "flights") && !strcmp(out[2], "clock"),
       "a slug list parses trimmed, without empties or repeats");
    char big[ORB_ROTATION_MAX + 4][ORB_ROTATION_SLUG];
    n = orb_rotation_csv("a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s", big, ORB_ROTATION_MAX);
    CK(n == ORB_ROTATION_MAX, "and never past its table");

    CK(orb_rotation_set_order("flights,weather") == NULL, "an order is taken");
    CK(orb_rotation_rank("flights", 3) == 0 && orb_rotation_rank("weather", 0) == 1,
       "a named app ranks by its place in the order");
    CK(orb_rotation_rank("clock", 0) > orb_rotation_rank("weather", 9) &&
       orb_rotation_rank("clock", 0) < orb_rotation_rank("hsb", 1),
       "an unnamed app ranks after every named one, in slot order");

    CK(orb_rotation_set(true, 5, "weather") != NULL, "an interval under 10 s is refused");
    orb_rotation_t r; orb_rotation_get(&r);
    CK(!r.on && r.n == 0, "and the refusal changes nothing");
    CK(orb_rotation_set(true, 86401, NULL) != NULL, "so is one over a day");
    CK(orb_rotation_set(true, 60, "weather,flights,clock") == NULL, "a minute over three apps is taken");
    orb_rotation_get(&r);
    CK(r.on && r.every_s == 60 && r.n == 3 && orb_rotation_takes_part("flights") && !orb_rotation_takes_part("hsb"),
       "the rotation reads back, and says who takes part");

    const char *walk[] = { "flights", "weather", "hsb", "clock" };
    const long long MIN = 60LL * 1000000;
    CK(orb_rotation_pick(&r, walk, 4, 0, MIN - 1) == -1, "not before the knob has been left alone a whole interval");
    CK(orb_rotation_pick(&r, walk, 4, 0, MIN) == 1, "then to the next app along");
    CK(orb_rotation_pick(&r, walk, 4, 1, MIN) == 3, "skipping an app that does not take part");
    CK(orb_rotation_pick(&r, walk, 4, 3, MIN) == 0, "and wrapping from the last to the first");
    CK(orb_rotation_pick(&r, walk, 4, 2, 10 * MIN) == -1, "never away from an app that does not take part");
    CK(orb_rotation_pick(&r, walk, 4, -1, 10 * MIN) == -1, "nor from something that is not an app");
    orb_rotation_set(false, 60, NULL); orb_rotation_get(&r);
    CK(orb_rotation_pick(&r, walk, 4, 0, 10 * MIN) == -1 && r.n == 3, "off, it stays put and keeps its apps");
    orb_rotation_set(true, 60, "clock"); orb_rotation_get(&r);
    CK(orb_rotation_pick(&r, walk, 4, 3, 10 * MIN) == -1, "the only app taking part stays");
    orb_rotation_set(true, 60, ""); orb_rotation_get(&r);
    CK(r.n == 0, "an empty list leaves no app taking part");
    printf(fails ? "%d of %d FAILED\n" : "all %d checks pass\n", fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
"""


def run(src_text):
    with tempfile.TemporaryDirectory() as td:
        c = os.path.join(td, "orb_rotation.c")
        open(c, "w").write(src_text)
        h = os.path.join(td, "h.c")
        open(h, "w").write(HARNESS)
        exe = os.path.join(td, "t")
        r = subprocess.run(["cc", "-std=gnu17", "-Wall", "-Werror", "-DORB_ROTATION_HOST",
                            "-I", os.path.join(ROOT, "main", "app"), c, h, "-o", exe],
                           capture_output=True, text=True)
        if r.returncode:
            return False, "COMPILE FAILED\n" + r.stderr
        p = subprocess.run([exe], capture_output=True, text=True)
        return p.returncode == 0, p.stdout


def main():
    src = open(SRC).read()
    ok, out = run(src)
    print(out.strip())
    if not ok:
        return 1
    if "--mutations" in sys.argv:
        bad = 0
        for old, new in MUTATIONS:
            if src.count(old) != 1:
                print("STALE mutant: %r" % old)
                bad += 1
                continue
            mok, _ = run(src.replace(old, new))
            print(("bites   " if not mok else "ESCAPES ") + old[:70])
            bad += mok
        print("%d of %d mutants caught" % (len(MUTATIONS) - bad, len(MUTATIONS)))
        return 1 if bad else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
