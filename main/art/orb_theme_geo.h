/**
 * orb_theme_geo.h — the six names clock_view.c reads theme geometry through.
 *
 * Include this AFTER assets/clock_assets.h. The generated header declares
 * `static const clk_theme_t clk_theme = {…}` and `#define CLK_BLOB_BYTES …`;
 * defining these macros before it would expand them inside its own definitions,
 * which is a compile error that reads like a broken generator.
 *
 * Why macros and not a struct of pointers at every call site: clock_view.c
 * references clk_theme in 85 places and subscript-uses the four tables, and the
 * bake chose their lengths. This header is therefore the whole of the firmware
 * change for theme-at-runtime — clock_view.c adds one #include and stops treating
 * these as compile-time constants.
 *
 * What this forbids, and clock_view.c honours: no sizeof() of any of the four
 * tables, and no array declarator sized by them. Both would silently become
 * pointer arithmetic (or fail to compile), losing the length the bake picked.
 */
#pragma once

#include "orb_theme.h"

#undef clk_theme
#undef clk_bg_rects
#undef clk_bg_rect_first
#undef clk_gif_delays
#undef clk_bg_frames
#undef CLK_BLOB_BYTES

/* The dereference here is the whole trick: `clk_theme.tick_rate` in clock_view.c
 * expands to `(*orb_theme_geo()->theme).tick_rate`, so the 85 read sites are
 * untouched and the value still comes from one cached pointer — orb_theme_geo()
 * returns a file static, which keeps this exactly as cheap on the per-frame paint
 * path as the compile-time const struct was. Note theme is a POINTER inside
 * orb_theme_geo_t (it addresses either the parsed struct or the baked one), so
 * the macro must dereference it rather than member-select through it. */
#define clk_theme         (*orb_theme_geo()->theme)
#define clk_bg_rects      (orb_theme_geo()->rects)
#define clk_bg_rect_first (orb_theme_geo()->rect_first)
#define clk_gif_delays    (orb_theme_geo()->gif_delays)
#define clk_bg_frames     (orb_theme_geo()->bg_frames)
#define CLK_BLOB_BYTES    (orb_theme_geo()->art_size)
