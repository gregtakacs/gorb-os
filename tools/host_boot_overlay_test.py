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
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x1, int y1,
                                    int x2, int y2, const void *data)
{
    (void)p; (void)x1; (void)x2;
    if (g_calls < 16) { g_y[g_calls] = y1;
        g_b[g_calls] = malloc((size_t)(y2 - y1) * 466 * 2);
        memcpy(g_b[g_calls], data, (size_t)(y2 - y1) * 466 * 2); }
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
    "display.h": "#include \"esp_lcd_panel_ops.h\"\nesp_lcd_panel_handle_t display_get_panel(void);\n",
    "board_config.h": "#define SCREEN_WIDTH 466\n#define SCREEN_HEIGHT 466\n",
}


DRIVER = r"""
extern int g_calls; extern int g_y[16]; extern uint16_t *g_b[16]; extern int64_t g_now_us;
void orb_boot_overlay(const char *, unsigned, unsigned, unsigned);
/* One process, one scenario, in the order the boot lives it: the fake clock
 * steps forward and every band handed to the panel is dumped with its step
 * index, so throttle and promotion are observed as STATE, not as luck. */
#define NSTEP 7
static const struct { int64_t t; const char *stage; unsigned done, total; } STEPS[NSTEP] = {
    {  500000, "mapping", 0, 0 },                /* 0: first stage            */
    { 3000000, "mapping", 0, 0 },                /* 1: same word, no promotion*/
    { 4500000, "mapping", 0, 0 },                /* 2: promoted frame         */
    { 4550000, "loading", 512u*1024u, 1024u*1024u }, /* 3: install frame       */
    { 4560000, "loading", 512u*1024u, 1024u*1024u }, /* 4: 10 Hz throttle      */
    { 5600000, "loading", 512u*1024u, 1024u*1024u }, /* 5: named second ticks  */
    { 7000000, "store\342\200\224cut", 0, 0 },   /* 6: em dash -> ???           */
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
        ok(band_has_string(drawn[9][1][1], F, "store???cut 6s", 14),
           "an em dash lands as three ? — every byte renderable from the table")

        ok(F[0] == [0, 0, 0, 0, 0], "codepoint 0x20 is blank")
        ok(F[ord("A") - 0x20][0] == 0x7E and F[ord("Z") - 0x20][0] == 0x61,
           "ASCII extremes keep their bit patterns")
        ok(all(any(c for c in g) for g in F[1:]), "no other codepoint is blank-by-accident")

        print(f"\n== boot overlay mirror: {checked} checks, {fails} failure(s)")
        return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())