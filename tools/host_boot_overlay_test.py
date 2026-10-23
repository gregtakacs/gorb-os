#!/usr/bin/env python3
"""host_boot_overlay_test.py — the boot overlay, mirrored on the host.

Compiles the SHIPPED main/display/orb_boot_overlay.c with host gcc behind small
shims (panel capture, fake clock, heap), then checks the two claims §6.2/§6.4
make about it that a boot log alone cannot show:

  1. it paints the stage it names: the band bytes handed to draw_bitmap must
     decode, with an independent python walk of the same font table, back to
     the exact string passed in — every glyph, centered, at scale 3;
  2. the 4 s rule promotes the frame: at t<4 s the stage band carries only the
     stage word; at t>=4 s it carries the word and its seconds; and a repeat
     of the same (stage, done) under 4 s redraws at most at 10 Hz.

The font table is lifted from the shipped file, not mirrored, so a retyped
glyph fails here rather than on a dial on a wall.

  python3 tools/host_boot_overlay_test.py
"""
import os, re, struct, subprocess, sys, tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SRC  = os.path.join(ROOT, "main", "display", "orb_boot_overlay.c")

def lift_font(s):
    body = s.split("FONT5x7[95][5] = {", 1)[1].split("\n};", 1)[0]
    rows = re.findall(r"\{([^}]*)\}", body)
    assert len(rows) == 95, f"expected 95 glyphs, got {len(rows)}"
    out = []
    for r in rows:
        cols = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", r)]
        assert len(cols) == 5 and all(c <= 0x7F for c in cols), "column bytes malformed"
        out.append(cols)
    return out

PROLOG = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
int g_calls; int g_y[16]; uint16_t *g_b[16]; int64_t g_now_us;
int g_wipe; int g_wipe_y[24];
static int all_black(const uint16_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}
esp_err_t display_band_commit(int y1, int rows, const uint16_t *data)
{
    /* §32's boot wipe arrives as black 24-row strips at y = 0, 24, 48, ...; the
     * content bands ride y=184/252/300, which are not multiples of 24, so an
     * all-black strip is never a stage frame. Wipes are counted on their own so
     * the capture indexes the scenario decodes below do not shift. §32.5: the
     * overlay paints only through this seam, and the seam is exactly where the
     * shipped display.c drains; the capture below is the band as it goes out. */
    if (all_black(data, (size_t)rows * 466)) {
        if (g_wipe < 24) g_wipe_y[g_wipe] = y1;
        g_wipe++;
        return ESP_OK;
    }
    if (g_calls < 16) { g_y[g_calls] = y1;
        g_b[g_calls] = malloc((size_t)rows * 466 * 2);
        memcpy(g_b[g_calls], data, (size_t)rows * 466 * 2); }
    g_calls++;
    return ESP_OK;
}
int64_t esp_timer_get_time(void) { return g_now_us; }
void *display_get_panel(void) { return (void *)1; }
void *heap_caps_malloc(size_t n, int flags) { (void)flags; return malloc(n); }
"""

HDRS = {
    "esp_err.h": "typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERROR_CHECK(x) (void)(x)\n",
    "esp_timer.h": "#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "esp_log.h": "#define ESP_LOGI(t, ...)\n#define ESP_LOGW(t, ...)\n",
    "esp_heap_caps.h": "#include <stddef.h>\nvoid *heap_caps_malloc(size_t, int);\n"
                      "#define MALLOC_CAP_INTERNAL 0\n#define MALLOC_CAP_DMA 0\n",
    "esp_lcd_panel_ops.h": "#include \"esp_err.h\"\ntypedef void *esp_lcd_panel_handle_t;\n"
      "esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t, int, int, int, int, const void *);\n",
    "display.h": "#include \"esp_lcd_panel_ops.h\"\n#include <stdint.h>\n"
      "esp_lcd_panel_handle_t display_get_panel(void);\n"
      "esp_err_t display_band_commit(int, int, const uint16_t *);\n",
    "board_config.h": "#define SCREEN_WIDTH 466\n#define SCREEN_HEIGHT 466\n",
}


DRIVER = r"""
extern int g_calls; extern int g_y[16]; extern uint16_t *g_b[16]; extern int64_t g_now_us;
extern int g_wipe; extern int g_wipe_y[24];
void orb_boot_overlay(const char *, unsigned, unsigned, unsigned);
/* One process, one scenario, in the order the boot lives it: the fake clock
 * steps forward and every band handed to the panel is dumped with its step
 * index, so throttle and promotion are observed as STATE, not as luck. */
#define NSTEP 8
static const struct { int64_t t; const char *stage; unsigned done, total; } STEPS[NSTEP] = {
    {  500000, "mapping", 0, 0 },                /* 0: first stage            */
    { 3000000, "mapping", 0, 0 },                /* 1: same word, no promotion*/
    { 4500000, "mapping", 0, 0 },                /* 2: promoted frame         */
    { 4550000, "loading", 512u*1024u, 1024u*1024u }, /* 3: install frame       */
    { 4560000, "loading", 512u*1024u, 1024u*1024u }, /* 4: 10 Hz throttle      */
    { 5600000, "loading", 512u*1024u, 1024u*1024u }, /* 5: named second ticks  */
    { 7000000, "store\342\200\224cut", 0, 0 },   /* 6: em dash -> ???           */
    { 7100000, "theme load failed 0123456789abcdef0123456789abcdef0123456789abcdef", 0, 0 },
                                                /* 7: over-wide line (§32.3)   */
};
int main(void)
{
    for (int i = 0; i < NSTEP; i++) {
        int before = g_calls;
        g_now_us = STEPS[i].t;
        orb_boot_overlay(STEPS[i].stage, STEPS[i].done, STEPS[i].total, 0);
        printf("step %d calls %d\n", i, g_calls - before);
    }
    for (int i = 0; i < g_calls && i < 16; i++) {
        char path[160];
        snprintf(path, sizeof path, "%s.b%d.y%d", getenv("OV_TAG"), i, g_y[i]);
        FILE *f = fopen(path, "wb");
        fwrite(g_b[i], 1, 466 * 24 * 2, f); fclose(f);
    }
    printf("wipe %d", g_wipe);
    for (int i = 0; i < g_wipe && i < 24; i++) printf(" %d", g_wipe_y[i]);
    printf("\n");
    return 0;
}
"""

W, BANDS = 466, 24

def render(ink_font, s):
    """Glyph-space white cells of band_text(s): 5x7 bits, scale 3, pitch 18."""
    ink = []
    for gi, ch in enumerate(s):
        c = ord(ch)
        if c < 0x20 or c > 0x7E: c = 0x3F
        g = ink_font[c - 0x20]
        for col in range(5):
            for row in range(7):
                if g[col] & (1 << row):
                    ink.append((gi * 18 + col * 3, row * 3))
    return ink


def band_has_string(band, F, s, ncells=None):
    # centering counts the RAW bytes handed to the painter (each non-UTF8 byte
    # still costs one advance cell and prints as '?'), the same as band_text
    n = ncells if ncells is not None else len(s)
    w = n * 18
    x0 = (W - w) // 2
    want = {(x0 + px + sx, py + sy) for (px, py) in render(F, s)
            for sx in range(3) for sy in range(3)}
    got  = {(x, y) for y in range(BANDS) for x in range(W) if band[y * W + x] == 0xFFFF}
    return want == got

fails = checked = 0
def ok(cond, name, extra=""):
    global fails, checked
    checked += 1
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else f"  [{extra}]"))
    if not cond: fails += 1

def main():
    src = open(SRC).read()
    F = lift_font(src)
    with tempfile.TemporaryDirectory() as tmp:
        inc = os.path.join(tmp, "inc")
        os.makedirs(inc)
        for name, body in HDRS.items():
            open(os.path.join(inc, name), "w").write(body)
        one = os.path.join(tmp, "one.c")
        open(one, "w").write(PROLOG + src + DRIVER)
        exe = os.path.join(tmp, "drv")
        cc = subprocess.run(["gcc", "-std=gnu11", "-O0", "-Wall", one, "-I", inc,
                             "-I", os.path.join(ROOT, "main", "display"), "-o", exe],
                            capture_output=True, text=True)
        if cc.returncode != 0:
            print(cc.stderr[:3000]); return 1
        ok(True, "the shipped overlay compiles standalone against the shim")

        env = dict(os.environ, OV_TAG=os.path.join(tmp, "ov"))
        out = subprocess.run([exe], capture_output=True, text=True,
                             check=True, env=env).stdout
        steps = {int(m.group(1)): int(m.group(2))
                 for m in re.finditer(r"step (\d+) calls (\d+)", out)}
        bands = {}
        for i in range(16):
            for f in os.listdir(tmp):
                m = re.fullmatch(r"ov\.b(\d+)\.y(\d+)", f)
                if m and int(m.group(1)) == i:
                    raw = open(os.path.join(tmp, f), "rb").read()
                    bands[i] = (int(m.group(2)),
                                list(struct.unpack("<" + "H" * (W * BANDS), raw)))

        # §32: the first draw of a boot wipes the panel. Every 466 rows must be
        # black before the first stage line — count AND coverage, so a wipe that
        # stopped halfway down the glass cannot pass. The CO530 keeps its frame
        # memory across a warm reset; without the wipe the new boot's three text
        # bands land composited onto the old life's frozen progress bar (orb-B,
        # 2026-10-22: a torn store's 47% bar under the fallback sentences).
        wm = re.search(r"wipe (\d+)([^\n]*)", out)
        wn = int(wm.group(1)) if wm else -1
        wy = [int(x) for x in wm.group(2).split()] if wm else []
        ok(wn == (466 + 23) // 24, f"the first draw blacks the panel: {wn} wipe strips", str(wn))
        ok(wy == list(range(0, 466, 24)), "the wipe strips tile the panel from row 0 past row 456", str(wy))

        # the scenario's draw counts, step by step
        ok(steps.get(0) == 1, "step 0: the first stage draws one band", str(steps))
        ok(steps.get(1) == 1, "step 1: same word at 2.5 s may redraw once (<= 10 Hz)")
        ok(steps.get(2) == 1, "step 2: the 4 s promotion redraws once")
        ok(steps.get(3) == 3, "step 3: the install frame costs three bands")
        ok(steps.get(4) == 0, "step 4: 10 ms later, identical frame is refused", str(steps))
        ok(steps.get(5) == 3, "step 5: a new named second redraws the frame", str(steps))
        ok(steps.get(6) == 1, "step 6: a new stage word always repaints")

        # what those bands actually carry, decoded independently
        drawn = sorted(bands.items())          # global capture order preserved
        ok(band_has_string(drawn[0][1][1], F, "mapping"),
           "the first band decodes back to the word passed in — every glyph, centered")
        ok(band_has_string(drawn[2][1][1], F, "mapping 4s"),
           "the promoted frame names the elapsed seconds beside the stage")
        ok(band_has_string(drawn[3][1][1], F, "loading 4s"),
           "the install frame still names its stage (elapsed suffix rides with it)")
        ok(band_has_string(drawn[4][1][1], F, "50% 512/1024 KB"),
           "the second band does the bytes-in-KB arithmetic §6.2 asks for")
        bar = drawn[5][1][1]
        filled = {(x, y) for y in range(7, 17) for x in range(W) if bar[y * W + x] == 0xF507}
        track  = {(x, y) for y in range(7, 17) for x in range(W) if bar[y * W + x] == 0x0421}
        ok(len(filled) == 160 * 10, "half the track is arc-coloured at done/total = 1/2",
           f"px={len(filled)}")
        ok(len(track) == 160 * 10, "the other half stays on the dark track",
           f"px={len(track)}")

        # §32.5: a progress frame's three lines must LAND on the rows §6.2
        # names. The capture records the row of every committed band, and the
        # install frame of step 3 is captures 3, 4, 5 in global order. The
        # 2026-10-23 report of correct lines sent to incorrect rows was the
        # reused-buffer DMA race, fixed at the seam (display_band_commit
        # returns only drained); these pins are the geometry the glass keeps.
        ys = [bands[i][0] for i in sorted(bands)]
        ok(ys[3:6] == [184, 252, 300],
           "the install frame's three bands ride rows 184, 252, 300", str(ys))
        ok(all(y % 2 == 0 for y in ys),
           "every committed band starts on an even row for the CO5300's row pairing", str(ys))
        ok(band_has_string(drawn[9][1][1], F, "store???cut 6s", 14),
           "an em dash lands as three ? — every byte renderable from the table")

        # §32.3: an over-wide line clips at the RIGHT edge only. Step 7 hands the
        # painter a 66-char failure line — the old code centered it at x=-361 and
        # the guards ate both ends, which is the cut-off "theme load failed
        # xxxxx" the operator read on the glass on 2026-10-23. The head must
        # start at x=0, every column the panel can hold must land, and the first
        # column past the edge must be gone.
        ok(steps.get(7) == 1, "step 7: the over-wide failure line paints once", str(steps))
        LONG = ("theme load failed 0123456789abcdef0123456789abcdef"
                "0123456789abcdef")
        want = set()
        for gi, ch in enumerate(LONG):
            g = F[ord(ch) - 0x20]
            for col in range(5):
                px = gi * 18 + col * 3          # the clamp: x0 = 0, never negative
                if px >= W:
                    break                      # the painter's own column break
                bits = g[col]
                for row in range(7):
                    if bits & (1 << row):
                        for sx in range(3):
                            for sy in range(3):
                                if px + sx < W:
                                    want.add((px + sx, row * 3 + sy))
        got = {(x, y) for y in range(BANDS) for x in range(W)
               if drawn[10][1][1][y * W + x] == 0xFFFF}
        ok(got == want and min(px for px, _ in got) == 0,
           "an over-wide line clips at the right edge, its head whole from x=0",
           f"want {len(want)}px, got {len(got)}px, min_x={min([px for px, _ in got] or [-1])}")
        main_src = open(os.path.join(ROOT, "main", "main.c")).read()
        ok('snprintf(line, sizeof line, "theme load failed %.6s"' in main_src
           and "char line[32];" in main_src,
           "the glass names the failure and six hex; the whole id rides the log")

        ok(F[0] == [0, 0, 0, 0, 0], "codepoint 0x20 is blank")
        ok(F[ord("A") - 0x20][0] == 0x7E and F[ord("Z") - 0x20][0] == 0x61,
           "ASCII extremes keep their bit patterns")
        ok(all(any(c for c in g) for g in F[1:]), "no other codepoint is blank-by-accident")

        print(f"\n== boot overlay mirror: {checked} checks, {fails} failure(s)")
        return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())