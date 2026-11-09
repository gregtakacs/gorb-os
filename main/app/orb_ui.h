#pragma once
/**
 * orb_ui — the widget and canvas service a wasm app draws through (apps/sdk/orb.h,
 * docs/app-platform-plan.md §43).
 *
 * An app never holds a widget pointer. It holds small integer handles into a per-slot
 * table this file owns, and every call names its slot — the row's stable flash_slot,
 * the same key orb_app_wasm.c uses for the engine — so one app's handle can never name
 * another app's object. Handle 0 is the app's page (a full-screen, black, unscrollable
 * screen built on first use). The page and everything on it is released together.
 *
 * The canvas is an OS-owned RGB565 surface in PSRAM. Its primitives (fill, pixel, ring,
 * disc, line, text) are compiled C here, because §5's rule is that an interpreted pixel
 * is a fatal one; the app only says what to draw. Nothing appears until cv_flush.
 *
 * Threading: LVGL task only, like every other iface hook.
 */
#include <stdbool.h>
#include <stdint.h>

struct _lv_obj_t;

#define ORB_UI_OBJ_MAX 48     /* widgets per app, page excluded */
#define ORB_UI_CV_MAX  2      /* canvases per app */

/* Kinds and properties, numbered exactly as apps/sdk/orb.h numbers them. */
enum { ORB_UIK_BOX = 0, ORB_UIK_LABEL = 1, ORB_UIK_CANVAS = 2 };
enum {
    ORB_UIP_X = 0, ORB_UIP_Y, ORB_UIP_W, ORB_UIP_H,
    ORB_UIP_BG, ORB_UIP_BG_OPA, ORB_UIP_RADIUS, ORB_UIP_BORDER_W, ORB_UIP_BORDER_COLOR,
    ORB_UIP_PAD, ORB_UIP_TEXT_COLOR, ORB_UIP_FONT, ORB_UIP_TEXT_ALIGN, ORB_UIP_HIDDEN,
    ORB_UIP_N
};

/** The app's page, built on first call. NULL only when LVGL cannot allocate. */
struct _lv_obj_t *orb_ui_screen(int slot);
/** True once the app has a page (it built at least one widget, or asked for it). */
bool orb_ui_has_screen(int slot);

int32_t orb_ui_create(int slot, int32_t kind, int32_t parent);
void    orb_ui_set(int slot, int32_t h, int32_t prop, int32_t v);
void    orb_ui_text(int slot, int32_t h, const char *s);
void    orb_ui_align(int slot, int32_t h, int32_t align, int32_t x, int32_t y);
void    orb_ui_refresh(int slot);

int32_t orb_ui_cv_create(int slot, int32_t parent, int32_t w, int32_t h);
/** radar-plan §2.9: a draw target that is not one of the page's canvases (the stage's live
 *  layer): RGB565 pixels at `stride`, and an alpha plane at the same stride, or NULL for an
 *  opaque one. The _tgt_ draws are the canvas draws below, onto it. */
/* radar-plan §7.9 (OS v1.6.1): an indexed target's palette, in the three forms its readers want
 * (the compositor's RGB565 and alpha, LVGL's I8 ARGB8888), and the values written into it. */
typedef struct {
    uint16_t *c565;          /* 256, native RGB565 */
    uint8_t  *a;             /* 256 */
    uint32_t *argb;          /* 256, LVGL's I8 palette */
    uint8_t  *used;          /* 32 B: a bit a value some pixel holds */
} orb_ui_pal_t;
/* An indexed target sets ix (a value a pixel, at stride) and pal; px and a are then unused. A
 * draw onto it writes values: a PNG its own (its palette becomes the target's), a ring or a line
 * the value whose colour it draws with (orb_ui_pal_index). */
typedef struct { uint16_t *px; uint8_t *a; int32_t w, h, stride; uint8_t *ix; const orb_ui_pal_t *pal; } orb_ui_tgt_t;
/* Entry i of an indexed target's palette from argb, in all three forms. */
void orb_ui_pal_set(const orb_ui_pal_t *p, int i, uint32_t argb);
/* The value a colour draws with: a used entry of that colour, else a free one given it, else
 * the nearest used colour. Marks it used. */
int  orb_ui_pal_index(const orb_ui_pal_t *p, uint32_t argb);
/* In a PNG draw's scale: magnify by the low byte (up to 16) and sample the values
 * bilinearly (radar-plan §6.9); without it the scale is 1-4, each source pixel a block. */
#define ORB_UI_PNG_SMOOTH 0x100
/* OS v1.2.38: drawn in 45-degree stripes (5 px on, 3 off), the target's own pixels between:
 * the radar's hatched forecast. */
#define ORB_UI_PNG_HATCH  0x200
/* The glass's disc, row by row: the first pixel of row y inside it (466 - 1 - it the last). */
const uint16_t *orb_ui_disc_left(void);
/* A 1 px ring onto a target, opaque. */
void orb_ui_tgt_ring(const orb_ui_tgt_t *t, int32_t cx, int32_t cy, int32_t r, uint32_t rgb);
/* A 2 px line onto a target, opaque (radar-plan §6.16: the radar's scale bar on its layer). */
void orb_ui_tgt_line(const orb_ui_tgt_t *t, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t rgb);
int32_t orb_ui_tgt_png8(const orb_ui_tgt_t *t, const uint8_t *png, uint32_t len, int32_t sx, int32_t sy,
                        int32_t dx, int32_t dy, int32_t w, int32_t hh, int32_t scale, const uint32_t *argb);
/* The same draw with the caller's scratch (orb_png8_scratch_size() bytes): for a draw off the
 * LVGL task (OS v1.2.38: the live worker, orb_live_work.c). */
int32_t orb_ui_tgt_png8_s(const orb_ui_tgt_t *t, void *scratch, const uint8_t *png, uint32_t len, int32_t sx, int32_t sy,
                          int32_t dx, int32_t dy, int32_t w, int32_t hh, int32_t scale, const uint32_t *argb);
void    orb_ui_tgt_roles(const orb_ui_tgt_t *t, const uint8_t *raster, const uint32_t *argb16,
                         int32_t dx, int32_t dy);
void    orb_ui_cv_fill(int slot, int32_t h, uint32_t rgb);
/** radar-plan §2.3: draw an indexed PNG's window [sx, sy, w/scale, hh/scale] onto the canvas
 *  at (dx, dy), each source pixel scale x scale, through 256 ARGB entries indexed by each
 *  pixel's value, the grey its index names in the file's palette (alpha blends, 0 leaves
 *  the canvas). Answers ORB_PNG8_OK or the reader's error. */
int32_t orb_ui_cv_png8(int slot, int32_t h, const uint8_t *png, uint32_t len, int32_t sx, int32_t sy,
                       int32_t dx, int32_t dy, int32_t w, int32_t hh, int32_t scale, const uint32_t *argb);
/** radar-plan §2.4: blend a map view's role raster onto the canvas through 16 ARGB entries. */
void    orb_ui_cv_roles(int slot, int32_t h, const uint8_t *raster, const uint32_t *argb16, int32_t dx, int32_t dy);
void    orb_ui_cv_pixel(int slot, int32_t h, int32_t x, int32_t y, uint32_t rgb);
void    orb_ui_cv_ring(int slot, int32_t h, int32_t cx, int32_t cy, int32_t r, uint32_t rgb);
void    orb_ui_cv_disc(int slot, int32_t h, int32_t cx, int32_t cy, int32_t r, uint32_t rgb);
void    orb_ui_cv_line(int slot, int32_t h, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                       uint32_t rgb);
void    orb_ui_cv_text(int slot, int32_t h, int32_t x, int32_t y, int32_t w, int32_t font,
                       uint32_t rgb, const char *s);
void    orb_ui_cv_flush(int slot, int32_t h);

/** Delete the page, every widget and every canvas buffer of `slot`. Idempotent. */
void    orb_ui_release(int slot);
