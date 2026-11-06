/* Weather: home and up to three places, now, by the hour and five days, from Open-Meteo
 * (docs/weather-plan.md).
 *
 * The data side is wx.c. This file asks for it (one request for every place, every 30
 * minutes and never sooner than 15), keeps the knob, and draws: on the theme's rows when
 * the app's theme is in the pool, else on widgets.
 *
 * The knob (§2.4, flights' convention): unarmed, Home's Now is on the glass and a turn
 * moves between apps. A press arms: turning steps Home and the places, each showing its
 * Now. Press again for its hours (turning moves the cursor), again for its five days, and
 * a press there, or a hold anywhere, hands the knob back, the place chosen staying on the glass.
 *
 * The theme's rows (apps/weather/editor/weather-bake.js), by slot: 0 the large icon,
 * 1..5 the small icons, 6..17 the hour bars, 18..22 the day bars, 23 the place dots, and
 * the labels at 24..31. Every row is baked centred on the glass and posed to where it
 * draws, so the geometry below is the whole layout. */
#include "orb.h"
#include "orb_stage.h"
#include "wx.h"

#define CX 233
#define CY 233

/* ---- the theme's rows ------------------------------------------------------------- */

enum {
    T_ICON = 0, T_SICON0 = 1, T_BAR0 = 6, T_DAY0 = 18, T_DOTS = 23,
    T_TITLE = 24, T_BIG, T_LINE, T_DETAIL, T_COLA, T_COLB, T_COLC, T_COLD,
};
#define N_SICON 5
#define N_BAR   12
#define N_DAYB  5
#define N_LAB   8

typedef struct {
    int32_t icon, sicon[N_SICON], bar[N_BAR], dayb[N_DAYB], dots, lab[N_LAB];
    clk_row_t col;              /* the column labels' height (all four share it) */
    int16_t   colw[4];          /* and each one's width */
} rows_t;

static orb_stage_info_t s_stage;
static bool   s_themed;
static rows_t R;

/* The layout (§2.4), glass coordinates. */
#define NOW_TITLE_Y   58
#define NOW_DOTS_Y    88
#define NOW_ICON_Y   150
#define NOW_BIG_Y    236
#define NOW_LINE_Y   292
#define NOW_DETAIL_Y 336
#define HR_TITLE_Y   146
#define HR_ICON_Y    196
#define HR_BIG_Y     256
#define HR_LINE_Y    306
#define HR_BAR_R     157       /* the bar sprite's centre, from the glass centre */
#define DAY_TITLE_Y   62
#define DAY_Y0       120
#define DAY_PITCH     56
#define DAY_ICON_X   158
#define DAY_BAR_X    258
#define DAY_COLA_L    78       /* names, left-aligned from here */
#define DAY_COLB_R   210       /* lows, right-aligned to here */
#define DAY_COLC_L   304       /* highs */
#define DAY_COLD_R   390       /* rain chance */

/* ---- state ------------------------------------------------------------------------ */

typedef enum { V_NOW, V_PLACES, V_HOURS, V_DAYS } view_t;

static wx_where_t s_where[WX_PLACES];
static int        s_nwhere;
static bool       s_home_set;
static uint32_t   s_cfg_gen, s_home_gen = 0xFFFFFFFFu, s_units_gen = 0xFFFFFFFFu;
static orb_units_t s_units;

static wx_place_t s_pub[WX_PLACES];     /* the last whole answer */
static int        s_npub;
static int64_t    s_pub_at;             /* wall s it landed, 0 = never */
static wx_place_t s_work[WX_PLACES];    /* the answer being read */
static wx_reader_t s_rd;

static view_t s_view = V_NOW;
static int    s_sel;                    /* the place on the glass */
static int    s_hcur;                   /* the hour cursor */
static bool   s_visible, s_dirty = true;

static int64_t now_s(void) { int64_t s; int32_t f; orb_now(&s, &f); return s; }
static int64_t up_ms(void) { return orb_uptime_us() / 1000; }

/* ---- places and settings ---------------------------------------------------------- */

static void load_places(void)
{
    s_nwhere = 0;
    orb_home_t h;
    memset(&h, 0, sizeof h);
    s_home_set = orb_home(&h) == ORB_OK;
    s_home_gen = orb_home_gen();
    if (s_home_set) {
        wx_where_t *w = &s_where[s_nwhere++];
        snprintf(w->name, sizeof w->name, "Home");
        w->lat = h.lat_e6 / 1e6;
        w->lon = h.lon_e6 / 1e6;
    }
    static const char *const ID[3] = { "place1", "place2", "place3" };
    for (int i = 0; i < 3 && s_nwhere < WX_PLACES; i++) {
        char v[32];
        if (orb_cfg_get_str(ID[i], v, sizeof v) == ORB_OK && wx_where_parse(v, &s_where[s_nwhere]))
            s_nwhere++;
    }
    s_cfg_gen = orb_cfg_gen();
    if (s_sel >= s_nwhere) s_sel = 0;
}

/* ---- the feed --------------------------------------------------------------------- */

#define EVERY_MS     1800000u   /* §2.3: every 30 minutes */
#define FLOOR_MS      900000u   /* and never sooner than 15, for any reason */
#define BACKOFF0_MS   300000u   /* failures: 5, 10, 20, 40, then 60 minutes */
#define BACKOFF_MAX  3600000u
#define PARSE_SLICE      120    /* tokens per service pass (flights' bound, D51) */
#define STALE_S         5400    /* past 90 minutes the age is shown */

static bool     s_busy, s_parsing, s_kick;
static int64_t  s_next_ms, s_last_req_ms = -(int64_t)FLOOR_MS;
static uint32_t s_backoff;
static uint32_t s_req_n;        /* the places the request asked for */

static void schedule(uint32_t wait)
{
    s_next_ms = up_ms() + wait;
}

static void start_fetch(void)
{
    char url[480];
    if (!s_nwhere || !wx_url(url, sizeof url, s_where, s_nwhere)) { schedule(EVERY_MS); return; }
    if (orb_net_get(url, NULL) != ORB_OK) { schedule(BACKOFF0_MS); return; }
    s_busy = true;
    s_parsing = false;
    s_req_n = (uint32_t)s_nwhere;
    s_last_req_ms = up_ms();
    wx_begin(&s_rd, s_work, s_nwhere);
}

static void finish(const orb_net_info_t *ni, bool good)
{
    if (good) {
        memcpy(s_pub, s_work, sizeof s_pub);
        s_npub = (int)s_req_n;
        s_pub_at = now_s();
        s_backoff = 0;
        s_dirty = true;
        schedule(EVERY_MS);
    } else {
        s_backoff = s_backoff ? (s_backoff * 2 > BACKOFF_MAX ? BACKOFF_MAX : s_backoff * 2) : BACKOFF0_MS;
        uint32_t w = s_backoff;
        if (ni->retry_after_s && (uint64_t)ni->retry_after_s * 1000u > w)
            w = ni->retry_after_s > 86400u ? 86400000u : ni->retry_after_s * 1000u;
        schedule(w);
    }
    orb_logf("WX %s %d places=%u B=%u total=%u ms", good ? "ok" : "FAIL", (int)ni->status,
             (unsigned)s_req_n, (unsigned)ni->len, (unsigned)ni->total_ms);
    orb_net_free();
    s_busy = false;
}

static void feed_step(void)
{
    orb_net_info_t ni;
    if (!s_busy) {
        const int64_t now = up_ms();
        const bool floor_ok = now - s_last_req_ms >= (int64_t)FLOOR_MS || !s_npub;
        if ((s_kick && floor_ok) || now >= s_next_ms) {
            if (!floor_ok) { schedule((uint32_t)(s_last_req_ms + FLOOR_MS - now)); return; }
            s_kick = false;
            start_fetch();
        }
        return;
    }
    if (orb_net_info(&ni) != ORB_OK || ni.state == ORB_NET_BUSY) return;
    if (ni.state != ORB_NET_DONE || ni.status != 200) { finish(&ni, false); return; }
    if (!s_parsing) {
        if (orb_json_begin(NULL, 0) != ORB_OK) { finish(&ni, false); return; }
        s_parsing = true;
    }
    for (int n = 0; n < PARSE_SLICE; n++) {
        orb_json_tok_t t;
        const int32_t k = orb_json_next(&t);
        if (!wx_token(&s_rd, &t) || k == ORB_JSON_END || k == ORB_JSON_ERROR) {
            s_parsing = false;
            finish(&ni, wx_done(&s_rd) && s_rd.places == (int)s_req_n);
            return;
        }
    }
}

/* ---- words ------------------------------------------------------------------------ */

static const char *const COND[WX_KINDS] = {
    "Clear", "Clear", "Partly cloudy", "Partly cloudy", "Cloudy", "Fog",
    "Drizzle", "Rain", "Snow", "Thunder",
};
static const char *const WDAY[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

static int deg(float c) { return wx_round(orb_temp_from_c(c, &s_units)); }

static const wx_place_t *place(void) { return s_sel < s_npub ? &s_pub[s_sel] : NULL; }

/* The hours still ahead: the answer starts at the hour it was asked, and an hour that has
 * passed since is dropped from the front. */
static int hour0(const wx_place_t *p)
{
    const int64_t now = now_s();
    int i = 0;
    while (i < p->n_hours - 1 && p->hour[i].time + 3600 <= now) i++;
    return i;
}

static void age_text(char *b, size_t n)
{
    b[0] = 0;
    if (!s_pub_at) return;
    const int64_t a = now_s() - s_pub_at;
    if (a < STALE_S) return;
    if (a < 86400) snprintf(b, n, "updated %d h ago", (int)(a / 3600));
    else snprintf(b, n, "updated %d d ago", (int)(a / 86400));
}

/* ---- drawing on the theme --------------------------------------------------------- */

static void pose(int32_t row, int x, int y)
{
    if (row >= 0) orb_stage_pose(row, 0, x - CX, y - CY);
}
static void hide(int32_t row)
{
    if (row >= 0) orb_stage_pose(row, ORB_STAGE_HIDDEN, 0, 0);
}
static void say(int which, int x, int y, const char *s, int tone)
{
    const int32_t row = R.lab[which];
    if (row < 0) return;
    orb_stage_text(row, s, tone);
    pose(row, x, y);
}
static void pick(int32_t row, int frame, int x, int y, int opa)
{
    if (row < 0) return;
    orb_stage_frame(row, frame);
    orb_stage_opacity(row, opa);
    pose(row, x, y);
}

static void hide_all(void)
{
    hide(R.icon);
    hide(R.dots);
    for (int i = 0; i < N_SICON; i++) hide(R.sicon[i]);
    for (int i = 0; i < N_BAR; i++) hide(R.bar[i]);
    for (int i = 0; i < N_DAYB; i++) hide(R.dayb[i]);
    for (int i = 0; i < N_LAB; i++) hide(R.lab[i]);
}

/* A column label's box centre for a block whose first line centres on y0. A label's lines
 * stand one font cell apart (orb_label.c), and the bake gives the columns no padding, nine
 * lines (five days and a blank between each) and a cell of half DAY_PITCH. */
static int col_cy(int y0)
{
    const int h = R.col.spr.h, fh = h / (2 * WX_DAYS - 1);
    return y0 - fh / 2 + h / 2;
}

static void draw_message(const char *title, const char *line)
{
    hide_all();
    say(T_TITLE - T_TITLE, CX, NOW_TITLE_Y + 80, title, ORB_TONE_NORMAL);
    say(T_LINE - T_TITLE, CX, NOW_LINE_Y - 40, line, ORB_TONE_QUIET);
}

static void stage_now(const wx_place_t *p, bool armed)
{
    char b[96], age[32];
    say(0, CX, NOW_TITLE_Y, s_where[s_sel].name, armed ? ORB_TONE_ACCENT : ORB_TONE_NORMAL);
    if (s_nwhere > 1) pick(R.dots, wx_dot_frame(s_nwhere, s_sel), CX, NOW_DOTS_Y, 255);
    const int kind = wx_kind(p->now.code, p->now.day);
    pick(R.icon, kind, CX, NOW_ICON_Y, 255);
    snprintf(b, sizeof b, "%d\xC2\xB0", deg(p->now.t));
    say(T_BIG - T_TITLE, CX + 6, NOW_BIG_Y, b, ORB_TONE_NORMAL);
    say(T_LINE - T_TITLE, CX, NOW_LINE_Y, COND[kind], ORB_TONE_NORMAL);
    age_text(age, sizeof age);
    const wx_day_t *d = p->n_days ? &p->day[0] : NULL;
    snprintf(b, sizeof b, "Feels %d\xC2\xB0   Wind %d %s", deg(p->now.feels),
             wx_round(orb_speed_from_kmh(p->now.wind, &s_units)), orb_speed_label(&s_units));
    if (d) {
        const size_t n = strlen(b);
        snprintf(b + n, sizeof b - n, "\nH %d\xC2\xB0   L %d\xC2\xB0", deg(d->hi), deg(d->lo));
    }
    if (age[0]) { const size_t n = strlen(b); snprintf(b + n, sizeof b - n, "\n%s", age); }
    say(T_DETAIL - T_TITLE, CX, NOW_DETAIL_Y, b, ORB_TONE_NORMAL);
}

static void stage_hours(const wx_place_t *p)
{
    const int h0 = hour0(p), avail = p->n_hours - h0;
    if (avail <= 0) { draw_message("No hours yet", "the next update brings them"); return; }
    if (s_hcur >= avail) s_hcur = avail - 1;
    float lo = 1e9f, hi = -1e9f;
    for (int i = h0; i < p->n_hours; i++) {
        if (p->hour[i].t < lo) lo = p->hour[i].t;
        if (p->hour[i].t > hi) hi = p->hour[i].t;
    }
    const int base = (s_hcur / N_BAR) * N_BAR;
    for (int k = 0; k < N_BAR; k++) {
        const int i = h0 + base + k;
        if (R.bar[k] < 0) continue;
        if (base + k >= avail) { hide(R.bar[k]); continue; }
        const wx_hour_t *h = &p->hour[i];
        const int ang = (wx_local_hour(p, h->time) % 12) * 300;      /* a clock face */
        const double a = ang / 10.0 * M_PI / 180.0;
        orb_stage_frame(R.bar[k], wx_bar_frame(h->t, lo, hi, h->pop));
        orb_stage_opacity(R.bar[k], base + k == s_hcur ? 255 : 120);
        orb_stage_pose(R.bar[k], ang, (int)lround(HR_BAR_R * orb_sin(a)), (int)lround(-HR_BAR_R * orb_cos(a)));
    }
    const wx_hour_t *h = &p->hour[h0 + s_hcur];
    char b[48];
    snprintf(b, sizeof b, "%02d:00", wx_local_hour(p, h->time));
    say(0, CX, HR_TITLE_Y, b, ORB_TONE_NORMAL);
    const int kind = wx_kind(h->code, wx_is_day(p, h->time));
    pick(R.icon, kind, CX, HR_ICON_Y, 255);
    snprintf(b, sizeof b, "%d\xC2\xB0", deg(h->t));
    say(T_BIG - T_TITLE, CX + 6, HR_BIG_Y, b, ORB_TONE_NORMAL);
    if (h->pop >= 0) snprintf(b, sizeof b, "Rain %d%%   %s", h->pop, COND[kind]);
    else snprintf(b, sizeof b, "%s", COND[kind]);
    say(T_LINE - T_TITLE, CX, HR_LINE_Y, b, ORB_TONE_QUIET);
}

static void stage_days(const wx_place_t *p)
{
    char names[72] = "", lows[56] = "", highs[56] = "", pops[56] = "";
    float wlo = 1e9f, whi = -1e9f;
    for (int i = 0; i < p->n_days; i++) {
        if (p->day[i].lo < wlo) wlo = p->day[i].lo;
        if (p->day[i].hi > whi) whi = p->day[i].hi;
    }
    for (int i = 0; i < p->n_days && i < WX_DAYS; i++) {
        const wx_day_t *d = &p->day[i];
        const int y = DAY_Y0 + i * DAY_PITCH;
        const char *nl = i ? "\n\n" : "";
        size_t n = strlen(names);
        snprintf(names + n, sizeof names - n, "%s%s", nl, i ? WDAY[wx_local_wday(p, d->time)] : "Today");
        n = strlen(lows);  snprintf(lows + n, sizeof lows - n, "%s%d\xC2\xB0", nl, deg(d->lo));
        n = strlen(highs); snprintf(highs + n, sizeof highs - n, "%s%d\xC2\xB0", nl, deg(d->hi));
        n = strlen(pops);
        if (d->pop >= 0) snprintf(pops + n, sizeof pops - n, "%s%d%%", nl, d->pop);
        else snprintf(pops + n, sizeof pops - n, "%s-", nl);
        if (i < N_SICON) pick(R.sicon[i], wx_kind(d->code, true), DAY_ICON_X, y, 255);
        if (i < N_DAYB) pick(R.dayb[i], wx_day_frame(d->lo, d->hi, wlo, whi), DAY_BAR_X, y, 255);
    }
    say(0, CX, DAY_TITLE_Y, "5 days", ORB_TONE_NORMAL);
    const int cy = col_cy(DAY_Y0);
    say(T_COLA - T_TITLE, DAY_COLA_L + R.colw[0] / 2, cy, names, ORB_TONE_NORMAL);
    /* The lows and the rain chances are the quiet columns: one font, dimmed by the row. */
    say(T_COLB - T_TITLE, DAY_COLB_R - R.colw[1] / 2, cy, lows, ORB_TONE_NORMAL);
    say(T_COLC - T_TITLE, DAY_COLC_L + R.colw[2] / 2, cy, highs, ORB_TONE_NORMAL);
    say(T_COLD - T_TITLE, DAY_COLD_R - R.colw[3] / 2, cy, pops, ORB_TONE_NORMAL);
    if (R.lab[T_COLB - T_TITLE] >= 0) orb_stage_opacity(R.lab[T_COLB - T_TITLE], 170);
    if (R.lab[T_COLD - T_TITLE] >= 0) orb_stage_opacity(R.lab[T_COLD - T_TITLE], 170);
}

static void draw_stage(void)
{
    if (!s_nwhere) { draw_message("No place yet", "set home in Orb Manager"); return; }
    const wx_place_t *p = place();
    if (!p || !p->have_now) {
        draw_message(s_where[s_sel].name, s_busy ? "fetching the forecast" : "no forecast yet");
        return;
    }
    hide_all();
    switch (s_view) {
    case V_NOW:    stage_now(p, false); break;
    case V_PLACES: stage_now(p, true); break;
    case V_HOURS:  stage_hours(p); break;
    case V_DAYS:   stage_days(p); break;
    }
}

/* ---- drawing on widgets (no theme in the pool) ------------------------------------ */

static int32_t W_title, W_big, W_line, W_detail;

static int32_t text(int32_t font, uint32_t rgb, int32_t align, int32_t y)
{
    const int32_t h = orb_ui_create(ORB_UI_LABEL, 0);
    orb_ui_set(h, ORB_P_FONT, font);
    orb_ui_set(h, ORB_P_TEXT_COLOR, (int32_t)rgb);
    orb_ui_set(h, ORB_P_TEXT_ALIGN, 1);
    orb_ui_align(h, align, 0, y);
    return h;
}

static void draw_widgets(void)
{
    char t[48] = "", big[16] = "", line[64] = "", det[160] = "";
    const wx_place_t *p = place();
    if (!s_nwhere) { snprintf(t, sizeof t, "No place yet"); snprintf(line, sizeof line, "set home in Orb Manager"); }
    else if (!p || !p->have_now) {
        snprintf(t, sizeof t, "%s", s_where[s_sel].name);
        snprintf(line, sizeof line, "%s", s_busy ? "fetching the forecast" : "no forecast yet");
    } else if (s_view == V_HOURS) {
        const int h0 = hour0(p);
        if (s_hcur >= p->n_hours - h0) s_hcur = p->n_hours - h0 - 1;
        if (s_hcur < 0) s_hcur = 0;
        const wx_hour_t *h = &p->hour[h0 + s_hcur];
        snprintf(t, sizeof t, "%s  %02d:00", s_where[s_sel].name, wx_local_hour(p, h->time));
        snprintf(big, sizeof big, "%d\xC2\xB0", deg(h->t));
        snprintf(line, sizeof line, "%s", COND[wx_kind(h->code, wx_is_day(p, h->time))]);
        if (h->pop >= 0) snprintf(det, sizeof det, "rain %d%%", h->pop);
    } else if (s_view == V_DAYS) {
        snprintf(t, sizeof t, "%s  5 days", s_where[s_sel].name);
        for (int i = 0; i < p->n_days; i++) {
            const wx_day_t *d = &p->day[i];
            const size_t n = strlen(det);
            snprintf(det + n, sizeof det - n, "%s%s  %d\xC2\xB0 / %d\xC2\xB0  %s\n",
                     i ? WDAY[wx_local_wday(p, d->time)] : "Today", "", deg(d->lo), deg(d->hi),
                     COND[wx_kind(d->code, true)]);
        }
    } else {
        snprintf(t, sizeof t, "%s%s", s_where[s_sel].name, s_view == V_PLACES ? "  <>" : "");
        snprintf(big, sizeof big, "%d\xC2\xB0", deg(p->now.t));
        snprintf(line, sizeof line, "%s", COND[wx_kind(p->now.code, p->now.day)]);
        snprintf(det, sizeof det, "feels %d\xC2\xB0   wind %d %s", deg(p->now.feels),
                 wx_round(orb_speed_from_kmh(p->now.wind, &s_units)), orb_speed_label(&s_units));
    }
    orb_ui_text(W_title, t);
    orb_ui_text(W_big, big);
    orb_ui_text(W_line, line);
    orb_ui_text(W_detail, det);
}

static void redraw(void)
{
    s_dirty = false;
    if (!s_visible) return;
    if (s_themed) draw_stage(); else draw_widgets();
}

/* ---- entry points ----------------------------------------------------------------- */

static void find_rows(void)
{
    memset(&R, 0xFF, sizeof R);         /* every row -1: absent */
    memset(&R.col, 0, sizeof R.col);
    for (int32_t i = 0; i < (int32_t)s_stage.n_rows; i++) {
        clk_row_t r;
        if (orb_stage_row(i, &r) != ORB_OK) continue;
        const int t = r.tag;
        if (r.shape == CLK_SHAPE_TEXT) {
            if (t >= T_TITLE && t < T_TITLE + N_LAB) R.lab[t - T_TITLE] = i;
            if (t == T_COLA) R.col = r;
            if (t >= T_COLA && t <= T_COLD) R.colw[t - T_COLA] = (int16_t)r.spr.w;
            continue;
        }
        if (t == T_ICON) R.icon = i;
        else if (t >= T_SICON0 && t < T_SICON0 + N_SICON) R.sicon[t - T_SICON0] = i;
        else if (t >= T_BAR0 && t < T_BAR0 + N_BAR) R.bar[t - T_BAR0] = i;
        else if (t >= T_DAY0 && t < T_DAY0 + N_DAYB) R.dayb[t - T_DAY0] = i;
        else if (t == T_DOTS) R.dots = i;
    }
}

ORB_EXPORT("orb_init") int32_t orb_init(void)
{
    s_themed = orb_stage_open(&s_stage) == ORB_OK && !s_stage.base;
    if (s_themed) {
        find_rows();
        /* A theme without the large icon or the title label is not this app's layout:
         * draw on widgets rather than half a screen. */
        if (R.icon < 0 || R.lab[0] < 0) s_themed = false;
        orb_logf("weather on its theme: %s", s_themed ? "rows found" : "rows missing, widgets");
        if (s_themed) orb_set_beat(0);   /* the stage's frames are the beat */
    }
    if (!s_themed) orb_set_beat(60000);  /* widgets: once a minute, for the age line */
    if (!s_themed) {
        W_title  = text(ORB_FONT_16, 0xB0B0B0, ORB_AL_TOP_MID, 60);
        W_big    = text(ORB_FONT_48, 0xFFFFFF, ORB_AL_CENTER, -30);
        W_line   = text(ORB_FONT_16, 0xE0E0E0, ORB_AL_CENTER, 30);
        W_detail = text(ORB_FONT_14, 0x909090, ORB_AL_CENTER, 90);
    }
    load_places();
    orb_units(&s_units);
    s_units_gen = orb_units_gen();
    s_next_ms = 0;                       /* the first fetch rides the first pass */
    orb_logf("weather ready: %d places", s_nwhere);
    return ORB_OK;
}

static void set_visible(bool v)
{
    s_visible = v;
    if (v) s_dirty = true;
}

ORB_EXPORT("orb_activate") void orb_activate(void) { set_visible(true); redraw(); }
ORB_EXPORT("orb_resume") void orb_resume(void) { set_visible(true); redraw(); }
ORB_EXPORT("orb_show") void orb_show(void) { set_visible(true); redraw(); }
ORB_EXPORT("orb_suspend") void orb_suspend(void) { s_visible = false; }

/* With a theme the stage ticks the app every frame and the app poses every row
 * (orb_stage.h), so the themed screen is drawn on each tick; a label recomposes only
 * when its string changes. Widgets keep what they were given. */
ORB_EXPORT("orb_tick") void orb_tick(uint32_t now)
{
    (void)now;
    if (s_dirty || s_themed) redraw();
}

ORB_EXPORT("orb_service") void orb_service(void)
{
    if (orb_cfg_gen() != s_cfg_gen || orb_home_gen() != s_home_gen) {
        load_places();
        s_kick = true;                   /* the places moved: ask, inside the floor */
        s_npub = 0;                      /* the old answer is for other places */
        s_dirty = true;
    }
    if (orb_units_gen() != s_units_gen) {
        orb_units(&s_units);
        s_units_gen = orb_units_gen();
        s_dirty = true;
    }
    const bool was_busy = s_busy;
    feed_step();
    if (was_busy != s_busy) s_dirty = true;
    if (s_dirty) redraw();
}

ORB_EXPORT("orb_set_armed") int32_t orb_set_armed(int32_t armed)
{
    if (armed) {
        if (s_view == V_NOW) s_view = V_PLACES;
    } else {
        /* Back to Now on the place last chosen: a full cycle, or a hold, keeps it
         * (owner, 2026-11-06); only the hour cursor starts over. */
        s_view = V_NOW;
        s_hcur = 0;
    }
    s_dirty = true;
    redraw();
    return s_view != V_NOW;
}

ORB_EXPORT("orb_input") void orb_input(int32_t delta, int32_t pressed)
{
    if (s_view == V_NOW) return;
    if (pressed) {
        if (s_view == V_PLACES) { s_view = V_HOURS; s_hcur = 0; }
        else if (s_view == V_HOURS) s_view = V_DAYS;
        else {
            orb_set_armed(0);
            orb_armed(0);                /* hand the knob back */
            return;
        }
    } else if (delta && s_view == V_PLACES && s_nwhere > 1) {
        s_sel = (s_sel + (delta > 0 ? 1 : s_nwhere - 1)) % s_nwhere;
    } else if (delta && s_view == V_HOURS) {
        s_hcur += delta > 0 ? 1 : -1;
        if (s_hcur < 0) s_hcur = 0;
        if (s_hcur > WX_HOURS - 1) s_hcur = WX_HOURS - 1;
    } else {
        return;
    }
    s_dirty = true;
    redraw();
}
