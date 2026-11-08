#!/usr/bin/env python3
"""Host bench for the compositor's chrome keep-out (main/gfx/orb_gfx.c, radar-plan §2.4).

Chrome stands on LVGL's top layer over the dial: the carousel chip, and since R5's
first half the map credit. The direct-push compositor must leave a chrome's pixels
alone while it stands, and only those: a row that crosses a rect still repaints every
pixel outside it. The cut before this one took a crossing row's whole segment out (its
side runs had min and max swapped), which no one saw while the one chrome also parked
the beat, and which a live map under the credit would have shown.

Lifted VERBATIM into gcc: the slot functions (gfx_keepout_set/clear, ko_hit, ko_row)
and the cut itself (the block inside the commit's row loop), around a fake row.

Run:  python3 tools/host_keepout_test.py              (rc 0 = pass)
      python3 tools/host_keepout_test.py --mutations  (proves the checks bite)
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "main", "gfx", "orb_gfx.c")
HDR = os.path.join(ROOT, "main", "gfx", "orb_gfx.h")

MUTATIONS = [
    ("if (ko_hit(x, y)) continue;", ";"),
    ("x >= s_ko[k][0] && x <= s_ko[k][2]", "x >= s_ko[k][0]"),
    ("    s_ko_up[slot] = false;\n    s_ko_on = false;", "    s_ko_up[slot] = false;\n    s_ko_on = s_ko_on;"),
    ("    for (int k = 0; k < GFX_KO_N; k++) s_ko_on = s_ko_on || s_ko_up[k];\n", "    s_ko_on = false;\n"),
    ("    gfx_mark_rows(s_ko[slot][1], s_ko[slot][3], s_ko[slot][0], s_ko[slot][2], false);", ""),
    ("if (s_ko_up[k] && y >= s_ko[k][1] && y <= s_ko[k][3]) return true;", "if (s_ko_up[k] && y == s_ko[k][1]) return true;"),
]

def lifted(mut=None):
    src = open(SRC).read()
    if mut:
        if mut[0] not in src:
            raise SystemExit("mutation %r does not apply -- rewrite the bench" % mut[0][:50])
        src = src.replace(mut[0], mut[1], 1)
    a = src.index("static int32_t s_ko[GFX_KO_N][4];")
    b = src.index("void gfx_hook_phases(")
    fns = src[a:b]
    c = src.index("                if (!ko_row(y)) {")
    d = src.index("                display_commit_row(y, send);")
    cut = src[c:d]
    cut = cut[:cut.rstrip().rindex("}")]          # the row loop's own closing brace stays out
    hdr = open(HDR).read()
    e = hdr.index("enum { GFX_KO_CHIP")
    enum = hdr[e:hdr.index("\n", e) + 1]
    return enum, fns, cut

HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int marks, mk[4];
static void gfx_mark_rows(int32_t y0, int32_t y1, int32_t x0, int32_t x1, bool f)
{ (void)f; marks++; mk[0] = y0; mk[1] = y1; mk[2] = x0; mk[3] = x1; }
ENUM
FNS
static uint16_t s_band[1][466];
static bool cut_row(int32_t y, int32_t xa, int32_t xb, uint16_t *fbrow)
{
    const int r = 0;
    bool send = false;
CUT
    return send;
}
static int fails;
#define CK(c, l) do { if (c) printf("ok    %s\n", l); else { printf("FAIL  %s\n", l); fails++; } } while (0)
int main(void)
{
    uint16_t fb[466];
    for (int x = 0; x < 466; x++) s_band[0][x] = 0xAAAA;
    memset(fb, 0, sizeof fb);
    cut_row(10, 0, 465, fb);
    int all = 1; for (int x = 0; x < 466; x++) all &= fb[x] == 0xAAAA;
    CK(all, "no chrome: the whole segment is painted");

    gfx_keepout_set(GFX_KO_CREDIT, 133, 408, 332, 431);
    memset(fb, 0, sizeof fb);
    const bool sent = cut_row(420, 0, 465, fb);
    int left = 1, mid = 1, right = 1;
    for (int x = 0; x < 133; x++) left &= fb[x] == 0xAAAA;
    for (int x = 133; x <= 332; x++) mid &= fb[x] == 0;
    for (int x = 333; x < 466; x++) right &= fb[x] == 0xAAAA;
    CK(left && right && sent, "a row crossing the credit still paints both sides of it");
    CK(mid, "and leaves the credit's own pixels alone");
    memset(fb, 0, sizeof fb);
    cut_row(200, 0, 465, fb);
    all = 1; for (int x = 0; x < 466; x++) all &= fb[x] == 0xAAAA;
    CK(all, "a row above the credit is untouched by it");
    memset(fb, 0, sizeof fb);
    cut_row(431, 300, 400, fb);
    CK(fb[332] == 0 && fb[333] == 0xAAAA && fb[300] == 0, "a segment that starts inside the rect paints only past its edge");

    gfx_keepout_set(GFX_KO_CHIP, 80, 306, 399, 369);
    memset(fb, 0, sizeof fb);
    cut_row(330, 0, 465, fb);
    CK(fb[79] == 0xAAAA && fb[80] == 0 && fb[399] == 0 && fb[400] == 0xAAAA, "two chromes at once: the chip's row cut at the chip");
    marks = 0;
    gfx_keepout_clear(GFX_KO_CHIP);
    CK(marks == 1 && mk[0] == 306 && mk[1] == 369 && mk[2] == 80 && mk[3] == 399, "clearing the chip re-marks the chip's rect");
    memset(fb, 0, sizeof fb);
    cut_row(330, 0, 465, fb);
    all = 1; for (int x = 0; x < 466; x++) all &= fb[x] == 0xAAAA;
    CK(all, "and its rows paint whole again");
    memset(fb, 0, sizeof fb);
    cut_row(420, 0, 465, fb);
    CK(fb[200] == 0 && fb[100] == 0xAAAA, "while the credit's slot still stands");
    gfx_keepout_clear(GFX_KO_CREDIT);
    memset(fb, 0, sizeof fb);
    cut_row(420, 0, 465, fb);
    all = 1; for (int x = 0; x < 466; x++) all &= fb[x] == 0xAAAA;
    CK(all && !s_ko_on, "both cleared: nothing kept out, and the any-slot flag is down");
    marks = 0;
    gfx_keepout_clear(GFX_KO_CREDIT);
    gfx_keepout_set(7, 0, 0, 10, 10);
    CK(marks == 0 && !s_ko_on, "a second clear and a slot out of range do nothing");
    return fails ? 1 : 0;
}
"""

def run(mut=None, quiet=False):
    enum, fns, cut = lifted(mut)
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, "t.c")
        open(c, "w").write(HARNESS.replace("ENUM", enum).replace("FNS", fns).replace("CUT", cut))
        exe = os.path.join(d, "t")
        r = subprocess.run(["gcc", "-std=c11", "-Wall", "-Werror", "-Wno-unused-function", "-o", exe, c],
                           capture_output=True, text=True)
        if r.returncode:
            if not quiet: print(r.stderr)
            return None
        r = subprocess.run([exe], capture_output=True, text=True)
        if not quiet: print(r.stdout, end="")
        return r.returncode

def main():
    if "--mutations" in sys.argv:
        caught = 0
        for m in MUTATIONS:
            rc = run(m, quiet=True)
            hit = rc != 0
            caught += hit
            print("%s  %r" % ("caught" if hit else "MISSED", m[1][:60]))
        print("%d/%d mutants caught" % (caught, len(MUTATIONS)))
        return 0 if caught == len(MUTATIONS) else 1
    rc = run()
    return 1 if rc is None else rc

if __name__ == "__main__":
    sys.exit(main())
