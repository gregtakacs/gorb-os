#!/usr/bin/env python3
"""Host check for apps/flights/readsb.c, the flight tracker's streaming reader.

    python3 tools/host_readsb_test.py             # all checks
    python3 tools/host_readsb_test.py --mutations # do the checks actually bite?

The reader is compiled as is (it has no IDF include) with a small driver that
prints one line per emitted aircraft. Python's json module reads the same
fixture and says what those lines must be. Every fixture is fed whole, a byte at
a time, and in 7- and 1024-byte pieces, because the socket decides where the
pieces fall, and a reader that only works on a whole body is a reader that works
on the bench and not in the air.

Fixtures are real answers captured 2026-10-31 (tests/fixtures/readsb/): adsb.lol
uses "ac", adsb.fi's v2 used "aircraft" (its v3, which the app now asks, answers
"ac" too: adsbfi-v3-phx-10nm.json, captured 2026-11-04), and both carry nested arrays (mlat, tisb,
nav_modes) that must be skipped, "ground" as an altitude, and null squawks.
"""
import glob, json, math, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "apps", "flights", "readsb.c")
INC = os.path.join(ROOT, "apps", "flights")
# D51: the OS tokenizes; the app's readsb.c only maps tokens. The suite runs both.
JSON_C = os.path.join(ROOT, "main", "app", "orb_json.c")
JSON_INC = os.path.join(ROOT, "main", "app")
FIX = sorted(glob.glob(os.path.join(ROOT, "tests", "fixtures", "readsb", "*.json")))

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "readsb.h"
static void emit(const orb_ac_t *a, void *ctx) {
    (void)ctx;
    printf("AC %s|%s|%s|%s|%.5f|%.5f|%d|%d|%d|%.2f|%.1f|%.0f|%d|%.1f|%d\n", a->hex, a->flight,
           a->type, a->category, a->lat, a->lon, a->alt_ft, a->on_ground, a->has_alt, a->track,
           a->gs, a->baro_rate, a->squawk, a->seen_pos, a->military);
}
int main(int argc, char **argv) {
    size_t chunk = (size_t)atol(argv[1]);
    FILE *f = fopen(argv[2], "rb");
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    if (argc > 3) n = (size_t)atol(argv[3]);           /* truncate */
    (void)chunk;                        /* the OS tokenizer reads the whole body */
    orb_readsb_t r;
    orb_readsb_init(&r, emit, NULL);
    orb_json_t j;
    orb_json_begin(&j, buf, n);
    orb_json_tok_t t;
    for (;;) {
        const int32_t k = orb_json_next(&j, &t);
        if (!orb_readsb_token(&r, &t) || k == ORB_JSON_END || k == ORB_JSON_ERROR) break;
    }
    printf("END objects=%u emitted=%u done=%d error=%d\n",
           r.objects, r.emitted, orb_readsb_done(&r), r.error);
    return 0;
}
"""


def build(src_path, out):
    d = tempfile.mkdtemp()
    drv = os.path.join(d, "drv.c")
    open(drv, "w").write(DRIVER)
    r = subprocess.run(["gcc", "-std=c11", "-O1", "-Wall", "-Werror", "-I", INC, "-I", JSON_INC,
                        drv, src_path, JSON_C, "-lm", "-o", out], capture_output=True, text=True)
    if r.returncode:
        sys.exit("build failed:\n" + r.stderr)


def expect(path):
    d = json.load(open(path))
    rows = d.get("ac") if "ac" in d else d.get("aircraft")
    lines, objects = [], 0
    for a in rows:
        objects += 1
        if not a.get("hex") or "lat" not in a or "lon" not in a:
            continue
        alt = a.get("alt_baro")
        ground = alt == "ground"
        has_alt = ground or isinstance(alt, (int, float))
        trk = a.get("track", a.get("true_heading", float("nan")))
        sq = a.get("squawk")
        sq = int(sq) if isinstance(sq, str) and sq else (-1 if sq is None else int(sq))
        f = lambda v: float("nan") if v is None else float(v)
        lines.append("AC %s|%s|%s|%s|%.5f|%.5f|%d|%d|%d|%.2f|%.1f|%.0f|%d|%.1f|%d" % (
            a["hex"][:7], (a.get("flight") or "")[:8].rstrip(" "), (a.get("t") or "")[:4],
            (a.get("category") or "")[:2],
            a["lat"], a["lon"], 0 if ground or not has_alt else int(alt), int(ground),
            int(has_alt), f(trk), f(a.get("gs")), f(a.get("baro_rate")), sq,
            f(a.get("seen_pos", 0)), int(a.get("dbFlags", 0)) & 1))
    return lines, objects


def run(binary, chunk, path, trunc=None):
    args = [binary, str(chunk), path] + ([str(trunc)] if trunc is not None else [])
    return subprocess.run(args, capture_output=True, text=True).stdout.splitlines()


def checks(binary):
    fails = []
    for path in FIX:
        name = os.path.basename(path)
        want, objects = expect(path)
        for chunk in (1, 7, 1024, 1 << 20):
            out = run(binary, chunk, path)
            got = [l for l in out if l.startswith("AC ")]
            end = out[-1] if out else ""
            if got != want:
                diff = next((i for i, (g, w) in enumerate(zip(got, want)) if g != w), None)
                fails.append(f"{name} chunk {chunk}: {len(got)} rows vs {len(want)}"
                             + (f"; first diff {got[diff]!r} != {want[diff]!r}" if diff is not None else ""))
            if f"objects={objects} emitted={len(want)} done=1 error=0" not in end:
                fails.append(f"{name} chunk {chunk}: {end}")
        # A body cut off mid-flight must never report done: the poll would keep
        # half the sky and call it the whole one.
        size = os.path.getsize(path)
        for cut in (size // 3, size - 2):
            end = run(binary, 64, path, cut)[-1]
            if "done=1" in end:
                fails.append(f"{name} cut at {cut}: reported done")
    return fails


def main():
    binary = os.path.join(tempfile.mkdtemp(), "readsb")
    build(SRC, binary)
    fails = checks(binary)
    n = len(FIX) * 6
    if fails:
        print("\n".join(fails))
        sys.exit(f"FAIL {len(fails)} of {n}")
    print(f"PASS {n} checks over {len(FIX)} fixtures")

    if "--mutations" in sys.argv:
        src = open(SRC).read()
        mutants = [
            ("track loses to true_heading", "if (!r->have_track && num) a->track", "if (num) a->track"),
            ("callsign keeps its padding", "while (n && dst[n - 1] == ' ') n--;", ""),
            ("nested arrays read as fields", "t->depth == r->list_depth + 2", "t->depth >= r->list_depth + 2"),
            ("adsb.fi's key ignored", '|| !strcmp(t->key, "aircraft")', ""),
            ("an aircraft with no position is emitted", "if (r->ac.hex[0] && r->have_lat && r->have_lon)",
             "if (r->ac.hex[0])"),
            ("truncation reads as done", "return !r->error && r->closed && r->list_depth >= 0;",
             "return !r->error && r->list_depth >= 0;"),
        ]
        survived = []
        for label, a, b in mutants:
            if a not in src:
                sys.exit(f"mutation anchor gone: {label}")
            p = os.path.join(tempfile.mkdtemp(), "readsb.c")
            open(p, "w").write(src.replace(a, b, 1))
            mb = p[:-2]
            build(p, mb)
            if not checks(mb):
                survived.append(label)
        if survived:
            sys.exit("mutants survived: " + ", ".join(survived))
        print(f"MUTATIONS {len(mutants)}/{len(mutants)} killed")


main()
