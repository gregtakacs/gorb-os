/* The flight tracker's view — contract in flights_view.h.
 *
 * Drawing: one full-screen canvas in PSRAM. Rings, dots and direction markers
 * are written straight into its pixels; only the callsign labels go through
 * LVGL's draw layer. The LVGL object pool is 22 KB with ~20 KB of measured
 * peak (sdkconfig.defaults, CONFIG_LV_MEM_SIZE), so a widget per aircraft
 * (~75 objects) would overrun it silently; this view adds five objects.
 *
 * Everything here runs on the LVGL task: init on the boot ladder, the redraw
 * timer, the router's input, the menu's options page and the service hook. */
#include "flights_view.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "orb_flights.h"
#include "orb_svc.h"

static const char *TAG = "flights";

#define W        SCREEN_WIDTH
#define H        SCREEN_HEIGHT
#define CX       (W / 2)
#define CY       (H / 2)
#define RING_R   184                 /* full range, px: header and footer sit outside it */
#define MAX_TRK  32                  /* contacts kept between polls */
#define STALE_US (60LL * 1000000)    /* a missed contact dims, then drops */
#define LABELS   6                   /* callsigns drawn, nearest first */
#define VIS_MS   15000               /* poll period while on the glass */

static const uint16_t RANGES_NM[] = { 10, 20, 40, 80 };
static const uint32_t BG_MS[]     = { 0, 60000, 120000, 300000 };
static const char *const BG_TXT[] = { "off", "1 min", "2 min", "5 min" };
static const char *const SRC_TXT[] = { "adsb.lol", "adsb.fi" };
#define N_RANGES (int)(sizeof RANGES_NM / sizeof RANGES_NM[0])
#define N_BG     (int)(sizeof BG_MS / sizeof BG_MS[0])

/* ---- settings (the container declares them; tools/make_factory_apps.py) --- */

typedef struct {
    char    lat[32], lon[32];
    double  latd, lond;
    bool    home_ok;
    int32_t range, source, bg, boost, ground;
} settings_t;

static orb_app_t *s_app;
static settings_t s_set = { .range = 2, .bg = 1, .boost = 1, .ground = 1 };
static uint32_t   s_cfg_gen;
static bool       s_visible;
static bool       s_armed;

static bool parse_deg(const char *s, double lim, double *out)
{
    if (!s[0]) return false;
    char *end;
    const double v = strtod(s, &end);
    if (end == s || *end || !isfinite(v) || fabs(v) > lim) return false;
    *out = v;
    return true;
}

static void load_settings(void)
{
    if (!s_app) return;
    const orb_svc_t *svc = orb_svc_table();
    int32_t v;
    if (svc->cfg_get(s_app, "range", &v) == ESP_OK)  s_set.range = v;
    if (svc->cfg_get(s_app, "source", &v) == ESP_OK) s_set.source = v;
    if (svc->cfg_get(s_app, "bg", &v) == ESP_OK)     s_set.bg = v;
    if (svc->cfg_get(s_app, "boost", &v) == ESP_OK)  s_set.boost = v;
    if (svc->cfg_get(s_app, "ground", &v) == ESP_OK) s_set.ground = v;
    if (svc->cfg_get_str(s_app, "lat", s_set.lat, sizeof s_set.lat) != ESP_OK) s_set.lat[0] = 0;
    if (svc->cfg_get_str(s_app, "lon", s_set.lon, sizeof s_set.lon) != ESP_OK) s_set.lon[0] = 0;
    s_set.home_ok = parse_deg(s_set.lat, 90, &s_set.latd) && parse_deg(s_set.lon, 180, &s_set.lond);
    if (s_set.range < 0 || s_set.range >= N_RANGES) s_set.range = 2;
    if (s_set.bg < 0 || s_set.bg >= N_BG) s_set.bg = 1;
    s_cfg_gen = orb_svc_cfg_gen(s_app);
}

static bool net_ok(void) { return s_app && orb_grant_ok(s_app, CAP_NET); }

/* Hand the service what this app decides: whether to poll at all, where, and
 * at which rate. Called on every change of setting or visibility. */
static void apply_feed(void)
{
    orb_flights_cfg_t c;
    orb_flights_get_cfg(&c);
    c.enabled      = s_set.home_ok && net_ok();
    c.lat          = s_set.latd;
    c.lon          = s_set.lond;
    c.radius_nm    = RANGES_NM[s_set.range];
    c.period_ms    = VIS_MS;
    c.bg_period_ms = BG_MS[s_set.bg];
    c.visible      = s_visible;
    c.boost        = s_set.boost != 0;
    c.hide_ground  = s_set.ground != 0;
    c.source       = (uint8_t)(s_set.source ? ORB_FLIGHTS_ADSB_FI : ORB_FLIGHTS_ADSB_LOL);
    c.keep_alive   = true;
    const esp_err_t e = orb_flights_set_cfg(&c);
    if (e != ESP_OK) ESP_LOGW(TAG, "feed config refused: %s", esp_err_to_name(e));
}

/* ---- the contact table: polls merged, missed contacts aged out ------------ */

typedef struct {
    orb_ac_t ac;
    float    e_km, n_km;     /* east/north of home at t_fix */
    int64_t  t_fix_us;       /* when that position was true */
    int64_t  t_seen_us;      /* last poll that carried it   */
    bool     live;           /* in the last picture         */
    float    x, y, d_km;     /* this frame's projection     */
    bool     drawn;
} trk_t;

static trk_t        *s_trk;          /* PSRAM, MAX_TRK */
static int           s_ntrk;
static orb_flight_t *s_snap;         /* PSRAM, ORB_FLIGHTS_MAX */
static uint32_t      s_seq;
static char         s_sel[8];       /* the selected contact's hex, "" = none */
static int           s_order[MAX_TRK];
static int           s_norder;

static void merge(const orb_flight_t *f, int n)
{
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_ntrk; i++) s_trk[i].live = false;
    for (int i = 0; i < n; i++) {
        int k = -1;
        for (int j = 0; j < s_ntrk && k < 0; j++)
            if (!strcmp(s_trk[j].ac.hex, f[i].ac.hex)) k = j;
        if (k < 0 && s_ntrk < MAX_TRK) k = s_ntrk++;
        if (k < 0) {                     /* full: replace the stalest */
            k = 0;
            for (int j = 1; j < s_ntrk; j++)
                if (s_trk[j].t_seen_us < s_trk[k].t_seen_us) k = j;
        }
        trk_t *t = &s_trk[k];
        t->ac = f[i].ac;
        const float b = f[i].bearing_deg * (float)M_PI / 180.0f;
        t->e_km = f[i].dist_km * sinf(b);
        t->n_km = f[i].dist_km * cosf(b);
        t->t_fix_us = now - (int64_t)(f[i].ac.seen_pos * 1e6f);
        t->t_seen_us = now;
        t->live = true;
    }
    for (int j = 0; j < s_ntrk;) {       /* drop what has been missing too long */
        if (now - s_trk[j].t_seen_us > STALE_US) s_trk[j] = s_trk[--s_ntrk];
        else j++;
    }
}

/* ---- pixels --------------------------------------------------------------- */

static lv_obj_t     *s_scr, *s_canvas, *s_head, *s_foot, *s_card;
static lv_draw_buf_t s_dbuf;
static uint16_t     *s_px;
static uint32_t      s_stride;       /* in pixels */
static lv_timer_t   *s_timer;

static inline uint16_t c16(uint32_t rgb) { return lv_color_to_u16(lv_color_hex(rgb)); }

static uint32_t dim(uint32_t rgb)
{
    return ((rgb >> 1) & 0x7F7F7F);
}

static inline void put(int x, int y, uint16_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) s_px[y * s_stride + x] = c;
}

static void ring(int r, uint16_t c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        put(CX + x, CY + y, c); put(CX + y, CY + x, c);
        put(CX - y, CY + x, c); put(CX - x, CY + y, c);
        put(CX - x, CY - y, c); put(CX - y, CY - x, c);
        put(CX + y, CY - x, c); put(CX + x, CY - y, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

static void ring_at(int cx, int cy, int r, uint16_t c)
{
    for (int a = 0; a < 48; a++) {
        const float t = (float)a * (float)M_PI / 24.0f;
        put(cx + (int)lroundf(r * cosf(t)), cy + (int)lroundf(r * sinf(t)), c);
    }
}

static void disc(int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r + r) put(cx + dx, cy + dy, c);
}

static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put(x0, y0, c); put(x0 + 1, y0, c); put(x0, y0 + 1, c);   /* 2 px pen */
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* orb-os's altitude legend. */
static uint32_t alt_rgb(const orb_ac_t *a)
{
    if (a->on_ground)       return 0x888888;
    if (!a->has_alt)        return 0xC0C0C0;
    if (a->alt_ft < 3000)   return 0xFF5A3C;
    if (a->alt_ft < 10000)  return 0xFFB23C;
    if (a->alt_ft < 20000)  return 0xC8FF3C;
    if (a->alt_ft < 30000)  return 0x39FF14;
    return 0x3CE0FF;
}

static bool emergency(const orb_ac_t *a)
{
    return a->squawk == 7500 || a->squawk == 7600 || a->squawk == 7700;
}

/* ---- one frame ------------------------------------------------------------ */

static char s_lbl[LABELS][32];
static char s_rng_txt[12];

static void project(float range_km)
{
    const int64_t now = esp_timer_get_time();
    s_norder = 0;
    for (int i = 0; i < s_ntrk; i++) {
        trk_t *t = &s_trk[i];
        float e = t->e_km, n = t->n_km;
        if (!isnan(t->ac.track) && !isnan(t->ac.gs) && !t->ac.on_ground) {
            /* Dead reckoning: a 15 s poll is visibly stale without it. Capped,
             * so a contact the feed lost does not sail off across the screen. */
            float dt = (float)(now - t->t_fix_us) / 1e6f;
            if (dt > 90.0f) dt = 90.0f;
            const float km = t->ac.gs * 1.852f / 3600.0f * dt;
            const float tr = t->ac.track * (float)M_PI / 180.0f;
            e += km * sinf(tr);
            n += km * cosf(tr);
        }
        t->d_km = sqrtf(e * e + n * n);
        t->drawn = t->d_km <= range_km;
        if (!t->drawn) continue;
        t->x = CX + e / range_km * RING_R;
        t->y = CY - n / range_km * RING_R;
        s_order[s_norder++] = i;
    }
    /* Nearest first: the selection cycles in this order, labels take the head. */
    for (int i = 1; i < s_norder; i++)
        for (int j = i; j > 0 && s_trk[s_order[j]].d_km < s_trk[s_order[j - 1]].d_km; j--) {
            const int t = s_order[j]; s_order[j] = s_order[j - 1]; s_order[j - 1] = t;
        }
}

/* The selected contact's place in this frame's order, or -1. By hex, because
 * the order re-sorts every second as aircraft move. */
static int sel_index(void)
{
    if (!s_sel[0]) return -1;
    for (int o = 0; o < s_norder; o++)
        if (!strcmp(s_trk[s_order[o]].ac.hex, s_sel)) return o;
    return -1;
}

static void fmt_alt(const orb_ac_t *a, char *b, size_t n)
{
    if (a->on_ground)     snprintf(b, n, "GND");
    else if (!a->has_alt) snprintf(b, n, "---");
    else                  snprintf(b, n, "FL%03d", (int)((a->alt_ft + 50) / 100));
}

static void draw_frame(void)
{
    const float range_km = RANGES_NM[s_set.range] * 1.852f;
    project(range_km);

    memset(s_px, 0, (size_t)s_stride * H * sizeof *s_px);
    const uint16_t ringc = c16(0x2A4A3A);
    ring(RING_R, ringc);
    ring(RING_R / 2, c16(0x1C3026));
    line(CX, CY - RING_R - 6, CX, CY - RING_R + 6, ringc);          /* north tick */
    disc(CX, CY, 3, c16(0xFFFFFF));                                  /* home */

    for (int o = s_norder - 1; o >= 0; o--) {                       /* far first */
        const trk_t *t = &s_trk[s_order[o]];
        const uint32_t rgb = t->live ? alt_rgb(&t->ac) : dim(alt_rgb(&t->ac));
        const uint16_t c = c16(rgb);
        const int x = (int)lroundf(t->x), y = (int)lroundf(t->y);
        if (!isnan(t->ac.track) && !t->ac.on_ground) {
            float len = 6.0f + (isnan(t->ac.gs) ? 0.0f : t->ac.gs / 600.0f * 16.0f);
            if (len > 22.0f) len = 22.0f;
            const float tr = t->ac.track * (float)M_PI / 180.0f;
            line(x, y, x + (int)lroundf(len * sinf(tr)), y - (int)lroundf(len * cosf(tr)), c);
        }
        disc(x, y, 3, c);
        if (emergency(&t->ac)) ring_at(x, y, 9, c16(0xFF2020));
        if (s_sel[0] && !strcmp(t->ac.hex, s_sel)) ring_at(x, y, 8, c16(0xFFFFFF));
    }

    /* Callsigns for the nearest LABELS airborne contacts. Each tries four spots
     * around its dot (outward side first) and takes the first that overlaps no
     * label already placed and stays on the round glass; a contact with no free
     * spot goes unlabelled rather than stacked (the first bench frame showed six
     * labels piled on the airport at the centre). */
    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = &lv_font_montserrat_14;
    lv_area_t placed[LABELS + 1];
    int np = 0;
    placed[np++] = (lv_area_t){ CX - 6, CY - 6, CX + 6, CY + 6 };   /* home */
    int nl = 0;
    for (int o = 0; o < s_norder && nl < LABELS; o++) {
        const trk_t *t = &s_trk[s_order[o]];
        if (t->ac.on_ground) continue;
        char alt[16];
        fmt_alt(&t->ac, alt, sizeof alt);
        snprintf(s_lbl[nl], sizeof s_lbl[nl], "%s %s",
                 t->ac.flight[0] ? t->ac.flight : t->ac.hex, alt);
        lv_point_t sz;
        lv_text_get_size(&sz, s_lbl[nl], d.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int x = (int)lroundf(t->x), y = (int)lroundf(t->y);
        const bool right = x >= CX;
        const int cand[4][2] = {
            { right ? x + 8 : x - 8 - sz.x, y - sz.y / 2 },
            { right ? x - 8 - sz.x : x + 8, y - sz.y / 2 },
            { x - sz.x / 2, y - 10 - sz.y },
            { x - sz.x / 2, y + 10 },
        };
        bool ok = false;
        lv_area_t a;
        for (int k = 0; k < 4 && !ok; k++) {
            a = (lv_area_t){ cand[k][0], cand[k][1], cand[k][0] + sz.x, cand[k][1] + sz.y };
            ok = true;
            /* every corner on the glass, with a margin */
            for (int c = 0; c < 4 && ok; c++) {
                const int px = (c & 1) ? a.x2 : a.x1, py = (c & 2) ? a.y2 : a.y1;
                ok = (px - CX) * (px - CX) + (py - CY) * (py - CY) < 222 * 222;
            }
            for (int q = 0; q < np && ok; q++)
                ok = a.x2 < placed[q].x1 - 2 || a.x1 > placed[q].x2 + 2 ||
                     a.y2 < placed[q].y1 - 1 || a.y1 > placed[q].y2 + 1;
        }
        if (!ok) continue;
        placed[np++] = a;
        d.color = t->live ? lv_color_hex(0xD0D0D0) : lv_color_hex(0x707070);
        d.text = s_lbl[nl];
        lv_draw_label(&layer, &d, &a);
        nl++;
    }
    snprintf(s_rng_txt, sizeof s_rng_txt, "%u nm", (unsigned)RANGES_NM[s_set.range]);
    d.color = lv_color_hex(0x4A7A62);
    d.text = s_rng_txt;
    lv_area_t ra = { CX + 122, CY + 136, CX + 182, CY + 154 };
    lv_draw_label(&layer, &d, &ra);
    lv_canvas_finish_layer(s_canvas, &layer);
    lv_obj_invalidate(s_canvas);
}

static void draw_text(void)
{
    char b[64];
    int live = 0;
    for (int i = 0; i < s_norder; i++) live += s_trk[s_order[i]].live;
    snprintf(b, sizeof b, "%d aircraft", s_norder);
    lv_label_set_text(s_head, b);

    orb_flights_stats_t st;
    orb_flights_stats(&st);
    uint32_t age_ms = UINT32_MAX;
    (void)orb_flights_snapshot(NULL, 0, &age_ms, NULL);
    uint32_t col = 0x909090;
    if (!s_set.home_ok)               { snprintf(b, sizeof b, "set home location"); col = 0xFFB23C; }
    else if (!net_ok())               { snprintf(b, sizeof b, "network permission off"); col = 0xFFB23C; }
    else if (age_ms == UINT32_MAX)    snprintf(b, sizeof b, st.skip_rest ? "feed refused, resting" : "waiting for the feed");
    else if (age_ms < 20000)          snprintf(b, sizeof b, "live  %s", SRC_TXT[st.last_src & 1]);
    else if (age_ms < 3 * VIS_MS)     snprintf(b, sizeof b, "%u s ago", (unsigned)(age_ms / 1000));
    else { snprintf(b, sizeof b, "feed down %u min", (unsigned)(age_ms / 60000 + 1)); col = 0xFFB23C; }
    lv_label_set_text(s_foot, b);
    lv_obj_set_style_text_color(s_foot, lv_color_hex(col), 0);
    (void)live;

    const int si = sel_index();
    if (si < 0) { s_sel[0] = 0; lv_obj_set_hidden(s_card, true); return; }
    const trk_t *t = &s_trk[s_order[si]];
    char alt[16], vs[24], spd[24];
    if (t->ac.on_ground) snprintf(alt, sizeof alt, "on ground");
    else if (t->ac.has_alt) snprintf(alt, sizeof alt, "%d ft", (int)t->ac.alt_ft);
    else snprintf(alt, sizeof alt, "alt ---");
    if (isnan(t->ac.baro_rate) || fabsf(t->ac.baro_rate) < 100) vs[0] = 0;
    else snprintf(vs, sizeof vs, "  %s %d fpm", t->ac.baro_rate > 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
                  abs((int)t->ac.baro_rate));
    if (isnan(t->ac.gs)) snprintf(spd, sizeof spd, "--- kt");
    else snprintf(spd, sizeof spd, "%d kt", (int)t->ac.gs);
    float brg = atan2f(t->x - CX, CY - t->y) * 180.0f / (float)M_PI;
    if (brg < 0) brg += 360.0f;
    char card[160];
    snprintf(card, sizeof card, "%s  %s\n%s%s\n%s  trk %d\n%.1f nm  brg %d",
             t->ac.flight[0] ? t->ac.flight : t->ac.hex, t->ac.type[0] ? t->ac.type : "",
             alt, vs, spd, isnan(t->ac.track) ? 0 : (int)t->ac.track,
             t->d_km / 1.852f, (int)brg);
    lv_label_set_text(s_card, card);
    lv_obj_set_hidden(s_card, false);
}

static void tick(lv_timer_t *tm)
{
    (void)tm;
    if (!s_visible) return;
    uint32_t seq = 0;
    (void)orb_flights_snapshot(NULL, 0, NULL, &seq);
    if (seq != s_seq) {
        const int n = orb_flights_snapshot(s_snap, ORB_FLIGHTS_MAX, NULL, &s_seq);
        merge(s_snap, n);
    }
    draw_frame();
    draw_text();
}

/* ---- the iface's half ----------------------------------------------------- */

static lv_obj_t *text(lv_obj_t *p, const lv_font_t *f, uint32_t rgb, lv_align_t al, int y)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, al, 0, y);
    return l;
}

esp_err_t flights_view_init(orb_app_t *app)
{
    s_app = app;
    s_trk  = heap_caps_calloc(MAX_TRK, sizeof *s_trk, MALLOC_CAP_SPIRAM);
    s_snap = heap_caps_calloc(ORB_FLIGHTS_MAX, sizeof *s_snap, MALLOC_CAP_SPIRAM);
    s_stride = lv_draw_buf_width_to_stride(W, LV_COLOR_FORMAT_RGB565) / 2;
    const size_t bytes = (size_t)s_stride * 2 * H;
    s_px = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, bytes, MALLOC_CAP_SPIRAM);
    if (!s_trk || !s_snap || !s_px) return ESP_ERR_NO_MEM;
    memset(s_px, 0, bytes);
    lv_draw_buf_init(&s_dbuf, W, H, LV_COLOR_FORMAT_RGB565, s_stride * 2, s_px, bytes);

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_scrollable(s_scr, false);
    s_canvas = lv_canvas_create(s_scr);
    lv_canvas_set_draw_buf(s_canvas, &s_dbuf);
    lv_obj_set_pos(s_canvas, 0, 0);
    s_head = text(s_scr, &lv_font_montserrat_16, 0xD0D0D0, LV_ALIGN_TOP_MID, 16);
    s_foot = text(s_scr, &lv_font_montserrat_14, 0x909090, LV_ALIGN_BOTTOM_MID, -14);
    s_card = text(s_scr, &lv_font_montserrat_16, 0xFFFFFF, LV_ALIGN_BOTTOM_MID, -40);
    lv_obj_set_style_text_align(s_card, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(s_card, 8, 0);
    lv_obj_set_style_radius(s_card, 10, 0);
    lv_obj_set_hidden(s_card, true);

    s_timer = lv_timer_create(tick, 1000, NULL);
    lv_timer_pause(s_timer);

    load_settings();
    s_visible = false;
    apply_feed();                    /* background polling starts here, if allowed */
    ESP_LOGI(TAG, "flight tracker ready: home %s, range %u nm, background %s",
             s_set.home_ok ? "set" : "not set", (unsigned)RANGES_NM[s_set.range],
             BG_TXT[s_set.bg]);
    return ESP_OK;
}

lv_obj_t *flights_view_screen(void) { return s_scr; }

void flights_view_set_visible(bool visible)
{
    if (s_visible == visible) return;
    s_visible = visible;
    apply_feed();
    if (!s_timer) return;
    if (visible) { lv_timer_resume(s_timer); lv_timer_ready(s_timer); }
    else lv_timer_pause(s_timer);
}

void flights_view_show(void)
{
    flights_view_set_visible(true);
    tick(NULL);
}

void flights_view_service(void)
{
    /* A setting written from outside (the hub's /api/v1/apps/cfg door) lands
     * here: the row's generation moved, so re-read and re-apply. */
    if (s_app && orb_svc_cfg_gen(s_app) != s_cfg_gen) {
        load_settings();
        apply_feed();
    }
}

bool flights_view_armed(void) { return s_armed; }

void flights_view_set_armed(bool armed)
{
    if (s_armed == armed) return;
    s_armed = armed;
    if (!armed) {
        s_sel[0] = 0;
        if (s_app) {
            orb_svc_table()->cfg_set(s_app, "range", s_set.range);   /* commit edge */
            s_cfg_gen = orb_svc_cfg_gen(s_app);
        }
    }
    if (s_visible) tick(NULL);
}

void flights_view_input(int32_t delta, bool pressed)
{
    if (!s_armed) return;
    if (pressed) {
        /* Cycle the detail card through the drawn aircraft, nearest first,
         * then back to none. */
        const int next = sel_index() + 1;
        if (next < s_norder) memcpy(s_sel, s_trk[s_order[next]].ac.hex, sizeof s_sel);
        else s_sel[0] = 0;
    } else if (delta) {
        const int r = s_set.range + (delta > 0 ? 1 : -1);
        if (r < 0 || r >= N_RANGES) return;
        s_set.range = r;
        apply_feed();                    /* the next poll asks for the new radius */
    } else {
        return;
    }
    tick(NULL);
}

/* ---- options page (the shell renders, these rows answer) ------------------ */

enum { OPT_RANGE, OPT_SOURCE, OPT_BG, OPT_BOOST, OPT_GROUND, OPT_HOME, OPT_N };

int flights_opt_count(orb_app_t *app) { (void)app; return OPT_N; }

const char *flights_opt_label(orb_app_t *app, int idx)
{
    (void)app;
    static const char *const L[OPT_N] = { "Range", "Source", "Background", "Boost",
                                          "Ground traffic", "Home" };
    return (idx >= 0 && idx < OPT_N) ? L[idx] : "";
}

const char *flights_opt_value(orb_app_t *app, int idx)
{
    (void)app;
    static char v[40];
    switch (idx) {
    case OPT_RANGE:  snprintf(v, sizeof v, "%u nm", (unsigned)RANGES_NM[s_set.range]); break;
    case OPT_SOURCE: return SRC_TXT[s_set.source & 1];
    case OPT_BG:     return BG_TXT[s_set.bg];
    case OPT_BOOST:  return s_set.boost ? "on" : "off";
    case OPT_GROUND: return s_set.ground ? "hidden" : "shown";
    case OPT_HOME:
        if (!s_set.home_ok) return "not set (hub)";
        snprintf(v, sizeof v, "%.3f, %.3f", s_set.latd, s_set.lond);
        break;
    default: return "";
    }
    return v;
}

void flights_opt_sel(orb_app_t *app, int idx)
{
    /* A press cycles the row's value and commits it: the page is a deliberate
     * act, unlike a turn on the radar. */
    const orb_svc_t *svc = orb_svc_table();
    switch (idx) {
    case OPT_RANGE:  s_set.range = (s_set.range + 1) % N_RANGES; svc->cfg_set(app, "range", s_set.range); break;
    case OPT_SOURCE: s_set.source = !s_set.source; svc->cfg_set(app, "source", s_set.source); break;
    case OPT_BG:     s_set.bg = (s_set.bg + 1) % N_BG; svc->cfg_set(app, "bg", s_set.bg); break;
    case OPT_BOOST:  s_set.boost = !s_set.boost; svc->cfg_set(app, "boost", s_set.boost); break;
    case OPT_GROUND: s_set.ground = !s_set.ground; svc->cfg_set(app, "ground", s_set.ground); break;
    default: return;
    }
    s_cfg_gen = orb_svc_cfg_gen(app);    /* our own write: no reload needed */
    apply_feed();
}
