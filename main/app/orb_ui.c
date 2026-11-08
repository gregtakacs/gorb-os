/**
 * orb_ui.c — the widget and canvas service behind a wasm app's ui_* and cv_* imports.
 * Contract in orb_ui.h.
 *
 * The per-slot tables live in PSRAM and are allocated on an app's first call: an app
 * that never draws (a service-only module) costs nothing, and the internal heap the
 * framebuffer and httpd share is never asked for a byte here. Every handle is checked
 * against its own slot's table before it is dereferenced; a bad handle is a no-op,
 * never a fault, because a guest bug must not reach the OS's widget tree.
 */
#include "orb_ui.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include "orb_app.h"       /* ORB_APP_CAP: the slot count the table is sized by */
#include "orb_png8.h"      /* orb_ui_cv_png8: radar-plan §2.3 */
#include "orb_map_raster.h" /* orb_ui_cv_roles: radar-plan §2.4 */
#include "orb_disc.h"       /* the glass's disc: draws onto a full glass skip its corners */

static const char *TAG = "appui";

typedef struct {
    lv_draw_buf_t dbuf;
    uint16_t     *px;
    int32_t       w, h, stride;          /* stride in pixels */
} cv_t;

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *obj[ORB_UI_OBJ_MAX + 1];   /* [0] unused: handle 0 is the page */
    uint8_t   kind[ORB_UI_OBJ_MAX + 1];
    int8_t    cv_of[ORB_UI_OBJ_MAX + 1]; /* canvas index for a CANVAS handle, else -1 */
    cv_t      cv[ORB_UI_CV_MAX];
    uint8_t   n_cv;
} ui_slot_t;

static ui_slot_t *s_ui[ORB_APP_CAP];

static ui_slot_t *slot_get(int slot, bool create)
{
    if (slot < 0 || slot >= ORB_APP_CAP) return NULL;
    if (!s_ui[slot] && create) {
        s_ui[slot] = heap_caps_calloc(1, sizeof *s_ui[slot], MALLOC_CAP_SPIRAM);
        if (!s_ui[slot]) ESP_LOGE(TAG, "slot %d: no memory for the widget table", slot);
    }
    return s_ui[slot];
}

lv_obj_t *orb_ui_screen(int slot)
{
    ui_slot_t *u = slot_get(slot, true);
    if (!u) return NULL;
    if (!u->scr) {
        u->scr = lv_obj_create(NULL);
        if (!u->scr) return NULL;
        lv_obj_set_style_bg_color(u->scr, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(u->scr, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(u->scr, 0, 0);
        lv_obj_set_scrollable(u->scr, false);
        lv_obj_set_scrollbar_mode(u->scr, LV_SCROLLBAR_MODE_OFF);
    }
    return u->scr;
}

bool orb_ui_has_screen(int slot)
{
    const ui_slot_t *u = slot_get(slot, false);
    return u && u->scr;
}

/* The object a handle names, or NULL. Handle 0 is the page. */
static lv_obj_t *obj_of(ui_slot_t *u, int32_t h)
{
    if (!u) return NULL;
    if (h == 0) return u->scr;
    if (h < 1 || h > ORB_UI_OBJ_MAX) return NULL;
    return u->obj[h];
}

static int32_t free_handle(const ui_slot_t *u)
{
    for (int32_t h = 1; h <= ORB_UI_OBJ_MAX; h++)
        if (!u->obj[h]) return h;
    return 0;
}

static const lv_font_t *font_of(int32_t f)
{
    switch (f) {
    case 1:  return &lv_font_montserrat_18;   /* ORB_FONT_18 (was 16 before OS v1.2.38) */
    case 2:  return &lv_font_montserrat_48;
    default: return &lv_font_montserrat_14;
    }
}

int32_t orb_ui_create(int slot, int32_t kind, int32_t parent)
{
    if (!orb_ui_screen(slot)) return 0;
    ui_slot_t *u = slot_get(slot, false);
    lv_obj_t *p = obj_of(u, parent);
    const int32_t h = free_handle(u);
    if (!p || !h) return 0;
    lv_obj_t *o;
    switch (kind) {
    case ORB_UIK_BOX:
        o = lv_obj_create(p);
        if (!o) return 0;
        lv_obj_set_style_pad_all(o, 0, 0);
        lv_obj_set_scrollable(o, false);
        lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
        break;
    case ORB_UIK_LABEL:
        o = lv_label_create(p);
        if (!o) return 0;
        lv_label_set_text(o, "");
        break;
    default:
        return 0;
    }
    u->obj[h] = o;
    u->kind[h] = (uint8_t)kind;
    u->cv_of[h] = -1;
    return h;
}

void orb_ui_set(int slot, int32_t h, int32_t prop, int32_t v)
{
    ui_slot_t *u = slot_get(slot, false);
    lv_obj_t *o = obj_of(u, h);
    if (!o) return;
    switch (prop) {
    case ORB_UIP_X:            lv_obj_set_x(o, v); break;
    case ORB_UIP_Y:            lv_obj_set_y(o, v); break;
    case ORB_UIP_W:            lv_obj_set_width(o, v); break;
    case ORB_UIP_H:            lv_obj_set_height(o, v); break;
    case ORB_UIP_BG:
        lv_obj_set_style_bg_color(o, lv_color_hex((uint32_t)v), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        break;
    case ORB_UIP_BG_OPA:       lv_obj_set_style_bg_opa(o, (lv_opa_t)(v < 0 ? 0 : v > 255 ? 255 : v), 0); break;
    case ORB_UIP_RADIUS:       lv_obj_set_style_radius(o, v >= 0x7FFF ? LV_RADIUS_CIRCLE : v, 0); break;
    case ORB_UIP_BORDER_W:     lv_obj_set_style_border_width(o, v, 0); break;
    case ORB_UIP_BORDER_COLOR: lv_obj_set_style_border_color(o, lv_color_hex((uint32_t)v), 0); break;
    case ORB_UIP_PAD:          lv_obj_set_style_pad_all(o, v, 0); break;
    case ORB_UIP_TEXT_COLOR:   lv_obj_set_style_text_color(o, lv_color_hex((uint32_t)v), 0); break;
    case ORB_UIP_FONT:         lv_obj_set_style_text_font(o, font_of(v), 0); break;
    case ORB_UIP_TEXT_ALIGN:
        lv_obj_set_style_text_align(o, v == 1 ? LV_TEXT_ALIGN_CENTER
                                     : v == 2 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
        break;
    case ORB_UIP_HIDDEN:       if (h) lv_obj_set_hidden(o, v != 0); break;
    default: break;
    }
}

void orb_ui_text(int slot, int32_t h, const char *s)
{
    ui_slot_t *u = slot_get(slot, false);
    lv_obj_t *o = obj_of(u, h);
    if (o && h && u->kind[h] == ORB_UIK_LABEL) lv_label_set_text(o, s ? s : "");
}

void orb_ui_align(int slot, int32_t h, int32_t align, int32_t x, int32_t y)
{
    ui_slot_t *u = slot_get(slot, false);
    lv_obj_t *o = obj_of(u, h);
    if (!o || !h) return;
    if (align < LV_ALIGN_TOP_LEFT || align > LV_ALIGN_CENTER) align = LV_ALIGN_TOP_LEFT;
    lv_obj_align(o, (lv_align_t)align, x, y);
}

void orb_ui_refresh(int slot)
{
    ui_slot_t *u = slot_get(slot, false);
    if (u && u->scr) lv_obj_invalidate(u->scr);
}

/* ---- canvas ---------------------------------------------------------------------- */

static cv_t *cv_of(ui_slot_t *u, int32_t h)
{
    if (!u || h < 1 || h > ORB_UI_OBJ_MAX || !u->obj[h] || u->cv_of[h] < 0) return NULL;
    return &u->cv[u->cv_of[h]];
}

int32_t orb_ui_cv_create(int slot, int32_t parent, int32_t w, int32_t h)
{
    if (!orb_ui_screen(slot)) return 0;
    ui_slot_t *u = slot_get(slot, false);
    lv_obj_t *p = obj_of(u, parent);
    const int32_t hd = free_handle(u);
    if (!p || !hd || u->n_cv >= ORB_UI_CV_MAX) return 0;
    if (w < 1 || h < 1 || w > 1024 || h > 1024) return 0;
    cv_t *c = &u->cv[u->n_cv];
    c->stride = (int32_t)lv_draw_buf_width_to_stride((uint32_t)w, LV_COLOR_FORMAT_RGB565) / 2;
    const size_t bytes = (size_t)c->stride * 2u * (size_t)h;
    c->px = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, bytes, MALLOC_CAP_SPIRAM);
    if (!c->px) return 0;
    memset(c->px, 0, bytes);
    c->w = w; c->h = h;
    lv_draw_buf_init(&c->dbuf, (uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_RGB565,
                     (uint32_t)c->stride * 2u, c->px, bytes);
    lv_obj_t *o = lv_canvas_create(p);
    if (!o) { heap_caps_free(c->px); memset(c, 0, sizeof *c); return 0; }
    lv_canvas_set_draw_buf(o, &c->dbuf);
    lv_obj_set_pos(o, 0, 0);
    u->obj[hd] = o;
    u->kind[hd] = ORB_UIK_CANVAS;
    u->cv_of[hd] = (int8_t)u->n_cv++;
    return hd;
}

static inline uint16_t c16(uint32_t rgb) { return lv_color_to_u16(lv_color_hex(rgb)); }

static inline void put(cv_t *c, int32_t x, int32_t y, uint16_t col)
{
    if ((uint32_t)x < (uint32_t)c->w && (uint32_t)y < (uint32_t)c->h)
        c->px[y * c->stride + x] = col;
}

void orb_ui_cv_fill(int slot, int32_t h, uint32_t rgb)
{
    cv_t *c = cv_of(slot_get(slot, false), h);
    if (!c) return;
    const uint16_t col = c16(rgb);
    if (col == 0) { memset(c->px, 0, (size_t)c->stride * 2u * (size_t)c->h); return; }
    for (int32_t y = 0; y < c->h; y++)
        for (int32_t x = 0; x < c->w; x++) c->px[y * c->stride + x] = col;
}

void orb_ui_cv_pixel(int slot, int32_t h, int32_t x, int32_t y, uint32_t rgb)
{
    cv_t *c = cv_of(slot_get(slot, false), h);
    if (c) put(c, x, y, c16(rgb));
}

/* Midpoint circle, one pixel wide. */
void orb_ui_cv_ring(int slot, int32_t h, int32_t cx, int32_t cy, int32_t r, uint32_t rgb)
{
    cv_t *c = cv_of(slot_get(slot, false), h);
    if (!c || r < 0) return;
    const uint16_t col = c16(rgb);
    int32_t x = r, y = 0, err = 1 - r;
    while (x >= y) {
        put(c, cx + x, cy + y, col); put(c, cx + y, cy + x, col);
        put(c, cx - y, cy + x, col); put(c, cx - x, cy + y, col);
        put(c, cx - x, cy - y, col); put(c, cx - y, cy - x, col);
        put(c, cx + y, cy - x, col); put(c, cx + x, cy - y, col);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void orb_ui_cv_disc(int slot, int32_t h, int32_t cx, int32_t cy, int32_t r, uint32_t rgb)
{
    cv_t *c = cv_of(slot_get(slot, false), h);
    if (!c || r < 0 || r > 512) return;
    const uint16_t col = c16(rgb);
    for (int32_t dy = -r; dy <= r; dy++)
        for (int32_t dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r + r) put(c, cx + dx, cy + dy, col);
}

/* Bresenham with a 2 px pen. */
void orb_ui_cv_line(int slot, int32_t h, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                    uint32_t rgb)
{
    cv_t *c = cv_of(slot_get(slot, false), h);
    if (!c) return;
    const uint16_t col = c16(rgb);
    const int32_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int32_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (int guard = 0; guard < 4096; guard++) {
        put(c, x0, y0, col); put(c, x0 + 1, y0, col); put(c, x0, y0 + 1, col);
        if (x0 == x1 && y0 == y1) break;
        const int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void orb_ui_cv_text(int slot, int32_t h, int32_t x, int32_t y, int32_t w, int32_t font,
                    uint32_t rgb, const char *s)
{
    ui_slot_t *u = slot_get(slot, false);
    cv_t *c = cv_of(u, h);
    if (!c || !s || w < 1) return;
    const lv_font_t *f = font_of(font);
    lv_layer_t layer;
    lv_canvas_init_layer(u->obj[h], &layer);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = lv_color_hex(rgb);
    d.text = s;
    d.text_local = 1;               /* the guest's buffer is copied, not kept */
    lv_area_t a = { x, y, x + w - 1, y + lv_font_get_line_height(f) - 1 };
    lv_draw_label(&layer, &d, &a);
    lv_canvas_finish_layer(u->obj[h], &layer);
}

/* ---- radar-plan §2.3/§2.4: indexed pixels through an ARGB palette ------------------ */

/* An ARGB entry as the canvas's RGB565 plus its alpha (0 = leave the canvas). */
typedef struct { uint16_t c; uint8_t a; } pal565_t;

static void pal_prep(pal565_t *out, const uint32_t *argb, int n)
{
    for (int i = 0; i < n; i++) {
        out[i].c = c16(argb[i] & 0xFFFFFF);
        out[i].a = (uint8_t)(argb[i] >> 24);
    }
}

/* src over dst at alpha a, in native RGB565 (lv_color_to_u16's layout). */
static inline uint16_t blend565(uint16_t dst, uint16_t src, uint8_t a)
{
    if (a == 255) return src;
    const uint32_t ia = 255u - a;
    const uint32_t r = (((src >> 11) & 31) * a + ((dst >> 11) & 31) * ia) / 255;
    const uint32_t g = (((src >> 5) & 63) * a + ((dst >> 5) & 63) * ia) / 255;
    const uint32_t b = ((src & 31) * a + (dst & 31) * ia) / 255;
    return (uint16_t)(r << 11 | g << 5 | b);
}

/* src over a target pixel: a canvas is opaque; the live layer keeps an alpha plane, and
 * src over a pixel it only partly covers weights the colours by what each contributes. */
static inline void over(const orb_ui_tgt_t *t, int32_t i, uint16_t c, uint8_t a)
{
    if (!t->a) { t->px[i] = blend565(t->px[i], c, a); return; }
    const uint8_t da = t->a[i];
    if (a == 255 || !da) { t->px[i] = c; t->a[i] = a; return; }
    const uint32_t ao = a + (uint32_t)da * (255u - a) / 255u;
    t->px[i] = blend565(t->px[i], c, (uint8_t)(a * 255u / ao));
    t->a[i] = (uint8_t)ao;
}

static bool tgt_of_cv(int slot, int32_t h, orb_ui_tgt_t *t)
{
    cv_t *c = cv_of(slot_get(slot, false), h);
    if (!c) return false;
    *t = (orb_ui_tgt_t){ c->px, NULL, c->w, c->h, c->stride };
    return true;
}

/* The glass's disc, row by row: the first pixel in it (orb_disc.h's rule; the last is
 * 465 minus it). A draw onto a full-glass target stops there, since the round panel never
 * shows the corners: 21% of every full-glass draw (radar-plan §6.19). */
static uint16_t s_disc_x0[ORB_DISC_SIDE];
static bool s_disc_made;
const uint16_t *orb_ui_disc_left(void)
{
    if (!s_disc_made) {
        s_disc_made = true;
        for (uint32_t y = 0; y < ORB_DISC_SIDE; y++) {
            uint32_t a, b;
            orb_disc_span_n(ORB_DISC_SIDE, y, &a, &b);
            s_disc_x0[y] = (uint16_t)a;
        }
    }
    return s_disc_x0;
}
static const uint16_t *disc_of(const orb_ui_tgt_t *t)
{
    return t && t->w == (int32_t)ORB_DISC_SIDE && t->h == (int32_t)ORB_DISC_SIDE ? orb_ui_disc_left() : NULL;
}

typedef struct {
    const orb_ui_tgt_t *t;
    const pal565_t *pal;
    int32_t sx, sy, dx, dy, w, h, scale;
    const uint16_t *disc;          /* the glass's disc for a full-glass target, else NULL */
} png_ctx_t;

static int png_row(void *ctx, int y, const uint8_t *idx, int w)
{
    png_ctx_t *p = ctx;
    if (y < p->sy) return 0;
    const int32_t oy = (y - p->sy) * p->scale;
    if (oy >= p->h) return 1;                         /* past the window: stop decoding */
    int32_t xlo = 0, xhi = (p->w + p->scale - 1) / p->scale - 1;
    if (p->disc && p->scale == 1) {                   /* the row's disc span, in source x */
        const int32_t Y = p->dy + oy;
        if ((uint32_t)Y >= ORB_DISC_SIDE) return 0;
        const int32_t a = p->disc[Y] - p->dx, b = (int32_t)ORB_DISC_SIDE - 1 - p->disc[Y] - p->dx;
        if (a > xlo) xlo = a;
        if (b < xhi) xhi = b;
    }
    for (int32_t x = xlo; x <= xhi; x++) {
        const int32_t s = p->sx + x;
        if (s < 0 || s >= w) continue;
        const pal565_t e = p->pal[idx[s]];
        if (!e.a) continue;
        for (int32_t ky = 0; ky < p->scale && oy + ky < p->h; ky++)
            for (int32_t kx = 0; kx < p->scale && x * p->scale + kx < p->w; kx++) {
                const int32_t X = p->dx + x * p->scale + kx, Y = p->dy + oy + ky;
                if ((uint32_t)X < (uint32_t)p->t->w && (uint32_t)Y < (uint32_t)p->t->h)
                    over(p->t, Y * p->t->stride + X, e.c, e.a);
            }
    }
    return 0;
}

/* Magnified and smoothed (radar-plan §6.9): each glass pixel samples the source at its own
 * centre, sx + (X - dx + 0.5) / k in source pixels, between the four source pixels around
 * it, and blends their VALUES (a raw radar frame's greys, dBZ) bilinearly before the
 * palette colours the result, as LibreWXR's own smoothing does: colours mixed after the
 * ramp would put orange between green and red. It covers the glass's [0, w) x [0, h); dx
 * and dy place the source on it, to a fraction of a source pixel. Positions are in 1/(2k) of a source pixel,
 * so every weight is exact. The decoder hands rows down the image, and the glass rows
 * between two source rows are drawn when the second arrives. */
typedef struct {
    const orb_ui_tgt_t *t;
    const pal565_t *pal;           /* by value */
    const uint8_t *vmap;           /* index -> value */
    int32_t sx, sy, dx, dy, w, h, k;
    int32_t prev_y, shift;         /* shift: log2(4k^2) when k is a power of two, else -1 */
    const uint16_t *disc;          /* the glass's disc for a full-glass target, else NULL */
    uint8_t prev[ORB_PNG8_MAX_W], cur[ORB_PNG8_MAX_W];
    int16_t x0[ORB_PNG8_MAX_W];    /* per glass column: its left source column, -1 off the image */
    uint8_t fx[ORB_PNG8_MAX_W];    /* and its weight toward the right one, 0 .. 2k-1 */
    int32_t col[ORB_PNG8_MAX_W];   /* the row pair blended down one glass row, per source column */
} smooth_ctx_t;

static int32_t floordiv(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* Each glass column's source column and weight, once a draw, and the divide's shift. */
static void smooth_prep(smooth_ctx_t *p)
{
    const int32_t k2 = 2 * p->k;
    p->shift = -1;
    if (!(p->k & (p->k - 1))) for (p->shift = 0; (1 << p->shift) < k2 * k2; p->shift++) {}
    for (int32_t X = 0; X < p->w && X < ORB_PNG8_MAX_W; X++) {
        const int32_t u = p->sx * k2 + 2 * X - 2 * p->dx + 1 - p->k, x0 = floordiv(u, k2);
        p->x0[X] = (int16_t)(x0 < 0 || x0 >= ORB_PNG8_MAX_W ? -1 : x0);
        p->fx[X] = (uint8_t)(u - x0 * k2);
    }
}

static int smooth_row(void *ctx, int y, const uint8_t *idx, int w)
{
    smooth_ctx_t *p = ctx;
    const int32_t k2 = 2 * p->k;
    for (int x = 0; x < w; x++) p->cur[x] = p->vmap[idx[x]];
    if (p->prev_y == y - 1) {
        /* glass rows whose sample falls in [y-1, y): v = sy*2k + 2(Y - dy) + 1 - k */
        const int32_t c = p->sy * k2 - 2 * p->dy + 1 - p->k;
        int32_t y0 = floordiv((y - 1) * k2 - c + 1, 2), y1 = floordiv(y * k2 - c + 1, 2);
        if (y0 < 0) y0 = 0;
        if (y1 > p->h) y1 = p->h;
        /* The same sum as blending each source row across and then the two down, in the
         * other order: down once per source column, then across per glass pixel. Exact
         * either way; a blank stretch (both values 0 under a clear palette 0) is skipped. */
        const int32_t half = k2 * k2 / 2, sum0 = !p->pal[0].a;
        int32_t xw = p->w < (int32_t)p->t->w ? p->w : (int32_t)p->t->w;
        if (xw > ORB_PNG8_MAX_W) xw = ORB_PNG8_MAX_W;
        for (int32_t Y = y0; Y < y1; Y++) {
            const int32_t fy = 2 * Y + c - (y - 1) * k2;          /* 0 .. 2k-1 */
            if ((uint32_t)Y >= (uint32_t)p->t->h) continue;
            for (int x = 0; x < w; x++) p->col[x] = p->prev[x] * (k2 - fy) + p->cur[x] * fy;
            const int32_t base = Y * p->t->stride;
            int32_t xa = 0, xb = xw - 1;
            if (p->disc) { xa = p->disc[Y]; if ((int32_t)ORB_DISC_SIDE - 1 - xa < xb) xb = (int32_t)ORB_DISC_SIDE - 1 - xa; }
            for (int32_t X = xa; X <= xb; X++) {
                const int32_t x0 = p->x0[X];
                if (x0 < 0 || x0 + 1 >= w) continue;
                const int32_t a = p->col[x0], b = p->col[x0 + 1];
                if (sum0 && !(a | b)) continue;
                const int32_t fx = p->fx[X], n = a * (k2 - fx) + b * fx + half;
                const pal565_t e = p->pal[p->shift >= 0 ? n >> p->shift : n / (k2 * k2)];
                if (e.a) over(p->t, base + X, e.c, e.a);
            }
        }
        if (floordiv(y * k2 - c + 1, 2) >= p->h) return 1;  /* past the window: stop decoding */
    }
    memcpy(p->prev, p->cur, (size_t)w);
    p->prev_y = y;
    return 0;
}

/* A 1 px ring onto a target, opaque (radar-plan §6.19: rings under the rain, on the map). */
void orb_ui_tgt_ring(const orb_ui_tgt_t *t, int32_t cx, int32_t cy, int32_t r, uint32_t rgb)
{
    if (!t || r < 0) return;
    const uint16_t col = c16(rgb);
    int32_t x = r, y = 0, err = 1 - r;
    while (x >= y) {
        const int32_t px[8] = { cx + x, cx + y, cx - y, cx - x, cx - x, cx - y, cx + y, cx + x };
        const int32_t py[8] = { cy + y, cy + x, cy + x, cy + y, cy - y, cy - x, cy - x, cy - y };
        for (int k = 0; k < 8; k++)
            if ((uint32_t)px[k] < (uint32_t)t->w && (uint32_t)py[k] < (uint32_t)t->h)
                over(t, py[k] * t->stride + px[k], col, 255);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void orb_ui_tgt_line(const orb_ui_tgt_t *t, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t rgb)
{
    if (!t) return;
    const uint16_t col = c16(rgb);
    const int32_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int32_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (int guard = 0; guard < 4096; guard++) {
        for (int k = 0; k < 3; k++) {          /* the canvas line's 2 px pen */
            const int32_t X = x0 + (k == 1), Y = y0 + (k == 2);
            if ((uint32_t)X < (uint32_t)t->w && (uint32_t)Y < (uint32_t)t->h) over(t, Y * t->stride + X, col, 255);
        }
        if (x0 == x1 && y0 == y1) break;
        const int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

int32_t orb_ui_tgt_png8(const orb_ui_tgt_t *t, const uint8_t *png, uint32_t len, int32_t sx, int32_t sy,
                        int32_t dx, int32_t dy, int32_t w, int32_t hh, int32_t scale, const uint32_t *argb)
{
    const bool smooth = (scale & ORB_UI_PNG_SMOOTH) != 0;
    scale &= 0xFF;
    if (!t || !png || !argb || scale < 1 || scale > (smooth ? 16 : 4)) return ORB_PNG8_E_FORMAT;
    /* One scratch for every draw: they all run on the LVGL task, one at a time. */
    static void *scratch;
    if (!scratch) scratch = heap_caps_malloc(orb_png8_scratch_size(), MALLOC_CAP_SPIRAM);
    if (!scratch) return ORB_PNG8_E_SCRATCH;
    /* The app colours values, not indices: a grey PNG's index is its encoder's choice
     * (LibreWXR's raw frames number their greys afresh in every frame), so each index
     * takes the colour of its palette entry's grey. A file without a palette is its own. */
    pal565_t *pal = heap_caps_malloc(512 * sizeof *pal, MALLOC_CAP_SPIRAM);
    if (!pal) return ORB_PNG8_E_SCRATCH;
    pal_prep(pal + 256, argb, 256);
    uint8_t value[256];
    const int np = orb_png8_plte(png, len, value);
    if (np < 0) { heap_caps_free(pal); return np; }
    for (int i = 0; i < 256; i++) pal[i] = pal[256 + (np ? value[i] : i)];
    int r;
    if (smooth) {
        smooth_ctx_t *sc = heap_caps_malloc(sizeof *sc, MALLOC_CAP_SPIRAM);
        uint8_t *vmap = heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
        if (!sc || !vmap) { heap_caps_free(sc); heap_caps_free(vmap); heap_caps_free(pal); return ORB_PNG8_E_SCRATCH; }
        for (int i = 0; i < 256; i++) vmap[i] = np ? value[i] : (uint8_t)i;
        *sc = (smooth_ctx_t){ .t = t, .pal = pal + 256, .vmap = vmap, .sx = sx, .sy = sy, .dx = dx, .dy = dy,
                              .w = w, .h = hh, .k = scale, .prev_y = -2, .disc = disc_of(t) };
        smooth_prep(sc);
        r = orb_png8_decode(png, len, scratch, orb_png8_scratch_size(), smooth_row, sc);
        heap_caps_free(sc);
        heap_caps_free(vmap);
    } else {
        png_ctx_t ctx = { t, pal, sx, sy, dx, dy, w, hh, scale, disc_of(t) };
        r = orb_png8_decode(png, len, scratch, orb_png8_scratch_size(), png_row, &ctx);
    }
    if (r == ORB_PNG8_E_STOP) r = ORB_PNG8_OK;        /* the window ended before the image */
    heap_caps_free(pal);
    return r;
}

int32_t orb_ui_cv_png8(int slot, int32_t h, const uint8_t *png, uint32_t len, int32_t sx, int32_t sy,
                       int32_t dx, int32_t dy, int32_t w, int32_t hh, int32_t scale, const uint32_t *argb)
{
    orb_ui_tgt_t t;
    if (!tgt_of_cv(slot, h, &t)) return ORB_PNG8_E_FORMAT;
    return orb_ui_tgt_png8(&t, png, len, sx, sy, dx, dy, w, hh, scale, argb);
}

void orb_ui_tgt_roles(const orb_ui_tgt_t *t, const uint8_t *raster, const uint32_t *argb16, int32_t dx, int32_t dy)
{
    if (!t || !raster || !argb16) return;
    pal565_t pal[16];
    pal_prep(pal, argb16, 16);
    const uint16_t *disc = disc_of(t);
    for (int y = 0; y < ORB_MAP_W; y++) {
        const int32_t Y = dy + y;
        if ((uint32_t)Y >= (uint32_t)t->h) continue;
        int xa = 0, xb = ORB_MAP_W - 1;
        if (disc) {
            if (disc[Y] - dx > xa) xa = disc[Y] - dx;
            if ((int32_t)ORB_DISC_SIDE - 1 - disc[Y] - dx < xb) xb = (int32_t)ORB_DISC_SIDE - 1 - disc[Y] - dx;
        }
        for (int x = xa; x <= xb; x++) {
            const int32_t X = dx + x;
            if ((uint32_t)X >= (uint32_t)t->w) continue;
            const pal565_t e = pal[orb_map_role_at(raster, x, y)];
            if (e.a) over(t, Y * t->stride + X, e.c, e.a);
        }
    }
}

void orb_ui_cv_roles(int slot, int32_t h, const uint8_t *raster, const uint32_t *argb16, int32_t dx, int32_t dy)
{
    orb_ui_tgt_t t;
    if (tgt_of_cv(slot, h, &t)) orb_ui_tgt_roles(&t, raster, argb16, dx, dy);
}

void orb_ui_cv_flush(int slot, int32_t h)
{
    ui_slot_t *u = slot_get(slot, false);
    if (cv_of(u, h)) lv_obj_invalidate(u->obj[h]);
}

void orb_ui_release(int slot)
{
    ui_slot_t *u = slot_get(slot, false);
    if (!u) return;
    /* A page still on the glass is left standing: the second strike of a trapping
     * app lands while it is showing, and deleting LVGL's active screen leaves the
     * display with nothing to draw. The dial moves off it on the next switch, and the
     * next boot never builds it (C2), so what stays is one page of orphaned widgets
     * and its canvases, not a leak that grows. */
    if (u->scr && u->scr == lv_screen_active()) {
        ESP_LOGW(TAG, "slot %d: page is on the glass — kept until the dial moves", slot);
        s_ui[slot] = NULL;
        return;
    }
    if (u->scr) lv_obj_delete(u->scr);           /* takes every child with it */
    for (int i = 0; i < u->n_cv; i++)
        if (u->cv[i].px) heap_caps_free(u->cv[i].px);
    heap_caps_free(u);
    s_ui[slot] = NULL;
}
