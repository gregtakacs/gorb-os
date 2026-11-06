/* orb_label.h — a label's pixels: the app's string in a theme's font over its panel
 * (docs/theme-text-plan.md §2.3).
 *
 * Pure C over byte planes, no LVGL and no theme reader, so tools/host_label_test.py
 * compiles it on a host and checks the pixels. The stage (orb_stage.c) finds the fonts,
 * holds the strings and owns the planes; this file only draws. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "clk_types.h"   /* CLK_FONT_ASCII, CLK_FONT_GLYPHS */

/* One font: glyph cells w x h, consecutive from `glyphs` in the charset's order. A cell
 * is its coverage alone (`bpp` 1, A8), coloured as it draws: `c0` at the cell's top
 * running to `c1` at its foot (equal for a flat colour), both RGB565, from the font row's
 * vmin and vmax. A theme from before OS v1.2.12 carries RGB565A8 cells (`bpp` 3, colour
 * plane then alpha) and draws their own colours. orb_clock3.c proved the catalogue
 * contiguous and every glyph one size, so glyph g is `glyphs + g * w * h * bpp`. */
typedef struct {
    const uint8_t *glyphs;
    uint16_t w, h, n;
    uint8_t  bpp;                    /* 1 (A8, coloured live) or 3 (RGB565A8) */
    uint16_t c0, c1;                 /* bpp 1: the top and foot colours, RGB565 */
    uint8_t  space, track;
    bool     measured;
    uint8_t  adv[CLK_FONT_GLYPHS];   /* each glyph's advance, after orb_font_measure */
} orb_font_t;

/** Each glyph's advance: one past its ink's right edge, plus the tracking; a glyph with
 *  no ink advances the space. */
void orb_font_measure(orb_font_t *f);

/** The next character of a UTF-8 string as an index into the charset (ASCII 0x20..0x7E,
 *  then up, down, right and left arrows and the degree sign). A tab is a space; anything
 *  the font does not carry is '?'. Advances *p past the character. */
int orb_glyph_next(const char **p, uint16_t n);

/** Draw `text` into `plane` (w x h, RGB565A8): `panel` first (the same shape; NULL for a
 *  clear plane), then the string in `f`, lines split at '\n', each glyph over what is
 *  there in straight alpha, clipped to the box inset by `pad`. `halign` places each line
 *  across (0 start, 1 centre, 2 end), `valign` the block down. Measures `f` if it has not
 *  been. */
void orb_label_compose(uint8_t *plane, uint32_t w, uint32_t h, const uint8_t *panel,
                       orb_font_t *f, const char *text, int pad, int halign, int valign);
