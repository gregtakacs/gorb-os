/* Weather: home and up to three places, now, by the hour and five days, from Open-Meteo
 * (docs/weather-plan.md).
 *
 * The data side is wx.c. This file asks for it (one request for every place, every 30
 * minutes and never sooner than 15), keeps the knob, and draws: on the theme's rows when
 * the app's theme is in the pool, else on widgets.
 *
 * The knob (§2.4, app-platform-plan.md §51) is one dial centred on Now, with no arming:
 * clockwise the hours, the cursor on the next one first; anticlockwise the five days,
 * tomorrow first. A fast spin stops on Now, and 20 s idle returns there. A press steps
 * the place and keeps the position; a hold leaves the app. orb_dial.h keeps the dial.
 *
 * The theme's rows (apps/weather/editor/weather-bake.js), by slot: 0 the large icon,
 * 1..5 the small icons, 6..17 the hour bars, 18..22 the day bars, 23 the place dots, and
 * the labels at 24..31. Every row is baked centred on the glass and posed to where it
 * draws, so the geometry below is the whole layout. */
#include "orb.h"
#include "orb_stage.h"
#include "orb_dial.h"
#include "wx.h"
#include "fx.h"

#define CX 233
#define CY 233

/* ---- the theme's rows ------------------------------------------------------------- */

enum {
    T_ICON = 0, T_SICON0 = 1, T_BAR0 = 6, T_DAY0 = 18, T_DOTS = 23,
    T_TITLE = 24, T_BIG, T_LINE, T_DETAIL, T_COLA, T_COLB, T_COLC, T_COLD,
    T_CLOCK = 32, T_PART0 = 33,         /* the weather's life (OS v1.2.14's 64 rows) */
    T_HALO = 49,                        /* the 5 days' cursor: a halo over the day's bar */
    /* §2.10: Hourly's and the 5 days' own words, and Hourly's own icon, at fixed sizes, so
     * the sizes the author gives Now and Places never move them: a title and a line both
     * views share, Hourly's temperature, the 5 days' details. A theme bakes one only where
     * the author resized its font; without it the view draws in Now's row. */
    T_FTITLE = 50, T_FBIG, T_FLINE, T_FDETAIL = 55, T_HICON = 56,
};
#define N_FIX   6                       /* T_FTITLE..T_FDETAIL, by tag - T_FTITLE */
#define N_ROWS  64                      /* the most rows a theme seats (OS v1.2.14) */
#define N_PART  FX_MAX
#define N_SICON 5
#define N_BAR   12
#define N_DAYB  5
#define N_LAB   8

typedef struct {
    int32_t icon, sicon[N_SICON], bar[N_BAR], dayb[N_DAYB], dots, lab[N_LAB];
    int32_t clock, part[N_PART], halo; /* -1 where the theme has none (a theme before them) */
    int32_t fix[N_FIX], hicon;         /* §2.10's rows; -1 in a theme before them */
    int     n_part;                    /* the pool's rows, 0..N_PART, from part[0] on */
    clk_row_t col;              /* the column labels' height (all four share it) */
    int16_t   colw[4];          /* and each one's width */
} rows_t;

static orb_stage_info_t s_stage;
static bool   s_themed;
static rows_t R;
/* Every row's baked centre and box. The editor bakes Now's elements where the author put
 * them (their size's stack and their own Angle and Distance, weather-plan §2.8): on Now a
 * row is posed from its baked spot, so it stands there; every other view poses it from
 * the app's own (s_home), wherever the bake put it. A theme from before §2.8 bakes every
 * row on the centre, and the two are the same. */
static int16_t s_rcx[N_ROWS], s_rcy[N_ROWS], s_rw[N_ROWS], s_rh[N_ROWS];
static bool    s_home;

/* The layout (§2.4), glass coordinates. Now's places are where a theme from before §2.8
 * stands; a newer one carries its own (the editor's). */
#define NOW_TITLE_Y   58
#define NOW_DOTS_Y    88
#define NOW_ICON_Y   150
#define NOW_BIG_Y    236
#define NOW_LINE_Y   292
#define NOW_DETAIL_Y 346       /* three lines: the feel and wind, the high and low, the sun */
#define NOW_CLOCK_Y  404       /* the digital clock, under the details */
#define HR_MID       227       /* Hourly's stack centres here, inside the ring of bars */
#define HR_FIT       200       /* and fits this tall, or takes a small icon */
#define HR_BAR_R     157       /* the bar sprite's centre, from the glass centre */
#define DAY_TITLE_Y   50
#define DAY_Y0        96       /* the list sits high: the day under the cursor is told below it, */
static int day_y0(int pitch) { return DAY_Y0 - 2 * (pitch - 56); }   /* and higher as the font grows */
#define DAY_SPAN     312       /* the list's width, names to rain chance, at most */

/* ---- state ------------------------------------------------------------------------ */

static wx_where_t s_where[WX_PLACES];
static int32_t    s_clock_on = 1;       /* setting "clock": the clock shown on Now; its format is the orb's */
static int32_t    s_force = -1;         /* setting "force", the bench's: a WX_* Now shows whatever the sky
                                          * does (rain, snow, fog, stars on demand); -1 the forecast's own.
                                          * Not on the hub's page: POST /api/v1/apps/cfg sets it */
static int        s_nwhere;
static bool       s_home_set;
static uint32_t   s_cfg_gen, s_home_gen = 0xFFFFFFFFu, s_units_gen = 0xFFFFFFFFu;
static orb_units_t s_units;

static wx_place_t s_pub[WX_PLACES];     /* the last whole answer */
static int        s_npub;
static int64_t    s_pub_at;             /* wall s it landed, 0 = never */
static wx_place_t s_work[WX_PLACES];    /* the answer being read */
static wx_reader_t s_rd;

/* The dial: 0 Now, k > 0 the k-th hour after the current one, -k the k-th day after today. */
static orb_dial_t s_dial = { .idle_ms = ORB_DIAL_IDLE_MS };
static int    s_sel;                    /* the place on the glass */
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
    int32_t v = 1;
    s_clock_on = orb_cfg_get("clock", &v) == ORB_OK ? v != 0 : 1;
    v = -1;
    s_force = orb_cfg_get("force", &v) == ORB_OK && v >= 0 && v < WX_KINDS ? v : -1;
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

/* A place's local time of day for unix time t, in the orb's format. */
static __attribute__((noinline)) void place_time(char *b, const wx_place_t *p, int64_t t)
{
    orb_time_text(b, wx_local_hour(p, t), wx_local_min(p, t), &s_units);
}

/* A day's rain total: inches where distances are miles, millimetres otherwise. Whole
 * numbers only: the guest's printf has no float formatting to spare (a %f ran the
 * interpreter off the lvgl task's stack, v1.2.16). */
static __attribute__((noinline)) void rain_text(char *b, size_t n, float mm)
{
    if (s_units.dist == ORB_UNIT_MI) {
        const int h = wx_round(mm / 0.254f);                         /* hundredths of an inch */
        snprintf(b, n, "%d.%02d in", h / 100, h % 100);
    } else if (mm < 0.05f) {
        snprintf(b, n, "0 mm");
    } else if (mm < 9.95f) {
        const int t = wx_round(mm * 10.0f);                          /* tenths of a mm */
        snprintf(b, n, "%d.%d mm", t / 10, t % 10);
    } else snprintf(b, n, "%d mm", wx_round(mm));
}

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

/* The dial's ends follow the answer on the glass: the hours still ahead, the days after
 * today. With no answer yet the dial is Now alone. */
static void dial_fit(const wx_place_t *p)
{
    if (!p || !p->have_now) { orb_dial_range(&s_dial, 0, 0); return; }
    const int ahead = p->n_hours - hour0(p) - 1;
    int days = p->n_days - 1;
    if (days > WX_DAY_ROWS) days = WX_DAY_ROWS;
    orb_dial_range(&s_dial, -(days > 0 ? days : 0), ahead > 0 ? ahead : 0);
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
    if (row < 0) return;
    if (s_home && row < N_ROWS) orb_stage_pose(row, 0, x - s_rcx[row], y - s_rcy[row]);
    else orb_stage_pose(row, 0, x - CX, y - CY);
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
/* A fixed-size row of §2.10's, or the shared one a theme before it draws in. */
static int32_t fix_or(int k, int shared) { return R.fix[k] >= 0 ? R.fix[k] : R.lab[shared]; }
static void say_row(int32_t row, int x, int y, const char *s, int tone)
{
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
    hide(R.clock);
    hide(R.halo);
    hide(R.hicon);
    for (int i = 0; i < N_FIX; i++) hide(R.fix[i]);
    for (int i = 0; i < N_PART; i++) hide(R.part[i]);
}

/* A column label's box centre for a block whose first line centres on y0. A label's lines
 * stand one font cell apart (orb_label.c), and the bake gives the columns no padding and
 * nine lines (five days and a blank between each): the list's pitch is two cells, so a
 * larger body font spaces the days further apart. */
static int col_fh(void) { return R.col.spr.h / (2 * WX_DAY_ROWS - 1); }
static int day_pitch(void) { const int fh = col_fh(); return fh > 0 ? 2 * fh : 56; }
static int col_cy(int y0)
{
    return y0 - col_fh() / 2 + R.col.spr.h / 2;
}
static int row_h(int32_t row, int dflt) { return row >= 0 && row < N_ROWS && s_rh[row] ? s_rh[row] : dflt; }
static int row_w(int32_t row, int dflt) { return row >= 0 && row < N_ROWS && s_rw[row] ? s_rw[row] : dflt; }

static void draw_message(const char *title, const char *line)
{
    hide_all();
    s_home = true;
    say(T_TITLE - T_TITLE, CX, NOW_TITLE_Y + 80, title, ORB_TONE_NORMAL);
    say(T_LINE - T_TITLE, CX, NOW_LINE_Y - 40, line, ORB_TONE_QUIET);
}

/* ---- the weather's life (§2.7): the clock, the particles, the icon ----------------- */

static fx_t    s_fx = { .kind = -1 };
static int64_t s_life_ms;              /* uptime of the last frame the life drew */

/* The time in the orb's own format (the UNITS page's Time, OS v1.2.16): Home's is the
 * orb's, a place's its own (owner, 2026-11-06: places show their local time where Home
 * shows its time), from the forecast's offset for it. */
static void clock_text(char *b, size_t n, const wx_place_t *p)
{
    orb_tm_t tm;
    b[0] = 0;
    if (n < 9) return;
    if (p && !(s_home_set && s_sel == 0)) {
        orb_time_text(b, wx_local_hour(p, now_s()), wx_local_min(p, now_s()), &s_units);
        return;
    }
    if (orb_localtime(now_s(), &tm) != ORB_OK) return;
    orb_time_text(b, (int)tm.hour, (int)tm.min, &s_units);
}

/* Now's living parts, every frame it shows: the particles for the condition, the icon
 * turning (a clear day's sun) or bobbing (every other), the clock. The frames keep coming
 * while it lives (orb_burst at the panel's ceiling); every other view stands still. */
static void stage_life(int kind, const wx_place_t *p)
{
    const int64_t t = up_ms();
    const float dt = s_life_ms ? (float)(t - s_life_ms) / 1000.0f : 0.0f;
    s_life_ms = t;
    fx_set(&s_fx, kind, R.n_part);
    fx_step(&s_fx, dt);
    for (int i = 0; i < R.n_part; i++) {
        const int o = fx_opa(&s_fx, i);
        if (!o) { hide(R.part[i]); continue; }
        const fx_part_t *q = &s_fx.p[i];
        pick(R.part[i], q->f, (int)lround(q->x + fx_sway(&s_fx, i)), (int)lround(q->y), o);
    }
    if (R.icon >= 0) {
        orb_stage_frame(R.icon, kind);
        orb_stage_opacity(R.icon, 255);
        /* every icon bobs, 3 px over 4 s: a turning one would spin its baked shadow and
         * its light (the editor's Drop shadow and 3D shading) round with it */
        orb_stage_pose(R.icon, 0, 0,
                       NOW_ICON_Y - CY + (int)lround(3.0 * orb_sin((double)(t % 4000) * (2 * M_PI / 4000.0))));
    }
    if (R.clock >= 0 && s_clock_on) {
        char c[16];
        clock_text(c, sizeof c, p);
        orb_stage_text(R.clock, c, ORB_TONE_NORMAL);
        pose(R.clock, CX, NOW_CLOCK_Y);
    }
    orb_burst(400);
}

static void stage_now(const wx_place_t *p)
{
    char b[96], age[32];
    say(0, CX, NOW_TITLE_Y, s_where[s_sel].name, ORB_TONE_NORMAL);
    if (s_nwhere > 1) pick(R.dots, wx_dot_frame(s_nwhere, s_sel), CX, NOW_DOTS_Y, 255);
    const int kind = s_force >= 0 ? (int)s_force : wx_kind(p->now.code, p->now.day);
    stage_life(kind, p);
    snprintf(b, sizeof b, "%d\xC2\xB0", deg(p->now.t));
    say(T_BIG - T_TITLE, CX + 6, NOW_BIG_Y, b, ORB_TONE_NORMAL);
    say(T_LINE - T_TITLE, CX, NOW_LINE_Y, COND[kind], ORB_TONE_NORMAL);
    age_text(age, sizeof age);
    const wx_day_t *d = p->n_days ? &p->day[0] : NULL;
    snprintf(b, sizeof b, "Feels %d\xC2\xB0   Wind %d %s", deg(p->now.feels),
             wx_round(orb_speed_from_kmh(p->now.wind, &s_units)), orb_speed_label(&s_units));
    if (d) {
        const size_t n = strlen(b);
        snprintf(b + n, sizeof b - n, "\nH %d\xC2\xB0  L %d\xC2\xB0", deg(d->hi), deg(d->lo));
    }
    /* the next dawn or dusk, a line of its own under the high and low (owner, 2026-11-06),
     * so no line runs past the details' box at the Titles font's largest; a forecast gone
     * stale says so there instead */
    bool rise = false;
    const int64_t sun = wx_next_sun(p, now_s(), &rise);
    if (age[0]) { const size_t n = strlen(b); snprintf(b + n, sizeof b - n, "\n%s", age); }
    else if (sun) {
        char tt[12];
        place_time(tt, p, sun);
        const size_t n = strlen(b);
        snprintf(b + n, sizeof b - n, "\n%s %s", rise ? "Sunrise" : "Sunset", tt);
    }
    say(T_DETAIL - T_TITLE, CX, NOW_DETAIL_Y, b, ORB_TONE_NORMAL);
}

static void stage_hours(const wx_place_t *p)
{
    const int h0 = hour0(p), avail = p->n_hours - h0, cur = s_dial.pos;
    if (avail <= 1) { draw_message("No hours yet", "the next update brings them"); return; }
    float lo = 1e9f, hi = -1e9f;
    for (int i = h0; i < p->n_hours; i++) {
        if (p->hour[i].t < lo) lo = p->hour[i].t;
        if (p->hour[i].t > hi) hi = p->hour[i].t;
    }
    const int base = (cur / N_BAR) * N_BAR;
    for (int k = 0; k < N_BAR; k++) {
        const int i = h0 + base + k;
        if (R.bar[k] < 0) continue;
        if (base + k >= avail) { hide(R.bar[k]); continue; }
        const wx_hour_t *h = &p->hour[i];
        const int ang = (wx_local_hour(p, h->time) % 12) * 300;      /* a clock face */
        const double a = ang / 10.0 * M_PI / 180.0;
        orb_stage_frame(R.bar[k], wx_bar_frame(h->t, lo, hi, h->pop));
        orb_stage_opacity(R.bar[k], base + k == cur ? 255 : 120);
        orb_stage_pose(R.bar[k], ang, (int)lround(HR_BAR_R * orb_sin(a)), (int)lround(-HR_BAR_R * orb_cos(a)));
    }
    const wx_hour_t *h = &p->hour[h0 + cur];
    char b[48];
    if (s_nwhere > 1) snprintf(b, sizeof b, "%s  %02d:00", s_where[s_sel].name, wx_local_hour(p, h->time));
    else snprintf(b, sizeof b, "%02d:00", wx_local_hour(p, h->time));
    /* the hour under the cursor, stacked inside the ring by the theme's own sizes: the
     * hour, the icon, the temperature, the rain and the condition; a large icon that
     * would push the stack into the bars gives way to a small one */
    const int32_t rt = fix_or(0, 0), rb = fix_or(T_FBIG - T_FTITLE, T_BIG - T_TITLE), rl = fix_or(T_FLINE - T_FTITLE, T_LINE - T_TITLE);
    const int ht = row_h(rt, 20), hb = row_h(rb, 46), hl = row_h(rl, 24);
    int32_t icon = R.hicon >= 0 ? R.hicon : R.icon;
    int hi_ = row_h(icon, 72);
    if (ht + hi_ + hb + hl + 20 > HR_FIT && R.sicon[0] >= 0) { icon = R.sicon[0]; hi_ = row_h(icon, 36); }
    int y = HR_MID - (ht + 4 + hi_ + 1 + hb + 15 + hl) / 2;
    say_row(rt, CX, y + ht / 2, b, ORB_TONE_NORMAL);
    y += ht + 4;
    const int kind = wx_kind(h->code, wx_is_day(p, h->time));
    pick(icon, kind, CX, y + hi_ / 2, 255);
    y += hi_ + 1;
    snprintf(b, sizeof b, "%d\xC2\xB0", deg(h->t));
    say_row(rb, CX + 6, y + hb / 2, b, ORB_TONE_NORMAL);
    y += hb + 15;
    if (h->pop >= 0) snprintf(b, sizeof b, "Rain %d%%   %s", h->pop, COND[kind]);
    else snprintf(b, sizeof b, "%s", COND[kind]);
    say_row(rl, CX, y + hl / 2, b, ORB_TONE_QUIET);
}

/* The 5 days' columns, one string a column, a day a line with a blank between. */
typedef struct { char names[72], lows[56], highs[56], pops[56]; } day_cols_t;

/* One day of the list: its words into the columns, its icon, its bar, and the halo when the
 * cursor is on it. Its own function, never inlined, for days_detail's reason. */
static __attribute__((noinline)) void days_row(const wx_place_t *p, int i, day_cols_t *c, int y,
                                                int icon_x, int bar_x, float wlo, float whi, bool on)
{
    const wx_day_t *d = &p->day[i + 1];
    const char *nl = i ? "\n\n" : "";
    size_t n = strlen(c->names);
    snprintf(c->names + n, sizeof c->names - n, "%s%s", nl, WDAY[wx_local_wday(p, d->time)]);
    n = strlen(c->lows);  snprintf(c->lows + n, sizeof c->lows - n, "%s%d\xC2\xB0", nl, deg(d->lo));
    n = strlen(c->highs); snprintf(c->highs + n, sizeof c->highs - n, "%s%d\xC2\xB0", nl, deg(d->hi));
    n = strlen(c->pops);
    if (d->pop >= 0) snprintf(c->pops + n, sizeof c->pops - n, "%s%d%%", nl, d->pop);
    else snprintf(c->pops + n, sizeof c->pops - n, "%s-", nl);
    if (i < N_SICON) pick(R.sicon[i], wx_kind(d->code, true), icon_x, y, 255);
    if (i < N_DAYB) pick(R.dayb[i], wx_day_frame(d->lo, d->hi, wlo, whi), bar_x, y, 255);
    if (on) pose(R.halo, bar_x, y);
}

/* The day under the cursor, below the list: the condition, then the rain, wind and UV,
 * then the sun's times. Its own function, as stage_days' rows are, and never inlined: the
 * interpreter keeps a native frame for every op of a block until the block returns, and
 * the whole of the 5 days in one ran the guest's 64 KB stack out (v1.2.16, Gold). */
static __attribute__((noinline)) void days_detail(const wx_place_t *p, int sel, int pitch)
{
    /* below the list: the condition, then the rain, wind and UV, then the sun's times */
    const wx_day_t *d = &p->day[sel + 1];
    const int32_t rl = fix_or(T_FLINE - T_FTITLE, T_LINE - T_TITLE), rd = fix_or(T_FDETAIL - T_FTITLE, T_DETAIL - T_TITLE);
    const int hl = row_h(rl, 24), hd = row_h(rd, 53);
    const int line_y = day_y0(pitch) + (WX_DAY_ROWS - 1) * pitch + pitch / 2 + 4;
    say_row(rl, CX, line_y, COND[wx_kind(d->code, true)], ORB_TONE_NORMAL);
    char b[96], v[24];
    b[0] = 0;
    if (d->rain >= 0) { rain_text(v, sizeof v, d->rain); snprintf(b, sizeof b, "%s", v); }
    if (d->wind >= 0) {
        const size_t n = strlen(b);
        snprintf(b + n, sizeof b - n, "%s%d %s", n ? "  " : "",
                 wx_round(orb_speed_from_kmh(d->wind, &s_units)), orb_speed_label(&s_units));
    }
    if (d->uv >= 0) { const size_t n = strlen(b); snprintf(b + n, sizeof b - n, "%sUV %d", n ? "  " : "", wx_round(d->uv)); }
    if (d->sunrise && d->sunset) {
        char r[12], t[12];
        place_time(r, p, d->sunrise);
        place_time(t, p, d->sunset);
        const size_t n = strlen(b);
        snprintf(b + n, sizeof b - n, "%s\xE2\x86\x91 %s  \xE2\x86\x93 %s", n ? "\n" : "", r, t);
    }
    /* two lines of the box's three, centred in it: the box's centre sits a line below the
     * condition's foot */
    say_row(rd, CX, line_y + hl / 2 + (hd - 2) / 3 + 2, b, ORB_TONE_NORMAL);
}

static void stage_days(const wx_place_t *p)
{
    /* The days after today, tomorrow first (§2.4); today is Now's. Every row stands at full
     * strength; the day under the dial's cursor wears a halo over its bar and is told
     * below the list, with what a row has no room for: its condition, rain total,
     * strongest wind, UV and sun (owner, 2026-11-06: a highlight that tells nothing more
     * is no use, and the halo, not the day's name, says which day it is). */
    float wlo = 1e9f, whi = -1e9f;
    for (int i = 1; i < p->n_days; i++) {
        if (p->day[i].lo < wlo) wlo = p->day[i].lo;
        if (p->day[i].hi > whi) whi = p->day[i].hi;
    }
    /* The columns from the theme's own widths, the list centred: names, icon, lows, bar,
     * highs, rain chance. The bake narrows the bar as the font grows (DAY_SPAN). */
    const int pitch = day_pitch(), si = row_w(R.sicon[0], 36), dw = row_w(R.dayb[0], 88);
    const int span = R.colw[0] + si - 2 + R.colw[1] - 8 + 4 + dw + 2 + R.colw[2] + R.colw[3] - 2;
    const int a_l = CX - span / 2, icon_x = a_l + R.colw[0] + si / 2 - 2, b_r = icon_x + si / 2 + R.colw[1] - 8;
    const int bar_x = b_r + 4 + dw / 2, c_l = bar_x + dw / 2 + 2, d_r = c_l + R.colw[2] + R.colw[3] - 2;
    const int sel = -s_dial.pos - 1;
    day_cols_t c = { "", "", "", "" };
    for (int i = 0; i + 1 < p->n_days && i < WX_DAY_ROWS; i++)
        days_row(p, i, &c, day_y0(pitch) + i * pitch, icon_x, bar_x, wlo, whi, i == sel);
    say_row(fix_or(0, 0), CX, DAY_TITLE_Y, s_nwhere > 1 ? s_where[s_sel].name : "5 days", ORB_TONE_NORMAL);
    const int cy = col_cy(day_y0(pitch));
    say(T_COLA - T_TITLE, a_l + R.colw[0] / 2, cy, c.names, ORB_TONE_NORMAL);
    /* The lows and the rain chances are the quiet columns: one font, dimmed by the row. */
    say(T_COLB - T_TITLE, b_r - R.colw[1] / 2, cy, c.lows, ORB_TONE_NORMAL);
    say(T_COLC - T_TITLE, c_l + R.colw[2] / 2, cy, c.highs, ORB_TONE_NORMAL);
    say(T_COLD - T_TITLE, d_r - R.colw[3] / 2, cy, c.pops, ORB_TONE_NORMAL);
    if (R.lab[T_COLB - T_TITLE] >= 0) orb_stage_opacity(R.lab[T_COLB - T_TITLE], 170);
    if (R.lab[T_COLD - T_TITLE] >= 0) orb_stage_opacity(R.lab[T_COLD - T_TITLE], 170);
    if (sel >= 0 && sel + 1 < p->n_days && sel < WX_DAY_ROWS) days_detail(p, sel, pitch);
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
    dial_fit(p);
    s_home = s_dial.pos != 0;            /* Now stands where the theme put it; the rest, where the app says */
    if (s_dial.pos > 0) stage_hours(p);
    else if (s_dial.pos < 0) stage_days(p);
    else stage_now(p);
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
    } else if ((dial_fit(p), s_dial.pos > 0)) {
        const wx_hour_t *h = &p->hour[hour0(p) + s_dial.pos];
        snprintf(t, sizeof t, "%s  %02d:00", s_where[s_sel].name, wx_local_hour(p, h->time));
        snprintf(big, sizeof big, "%d\xC2\xB0", deg(h->t));
        snprintf(line, sizeof line, "%s", COND[wx_kind(h->code, wx_is_day(p, h->time))]);
        if (h->pop >= 0) snprintf(det, sizeof det, "rain %d%%", h->pop);
    } else if (s_dial.pos < 0) {
        snprintf(t, sizeof t, "%s  5 days", s_where[s_sel].name);
        for (int i = 1; i < p->n_days && i <= WX_DAY_ROWS; i++) {
            const wx_day_t *d = &p->day[i];
            const size_t n = strlen(det);
            snprintf(det + n, sizeof det - n, "%s%s  %d\xC2\xB0 / %d\xC2\xB0  %s\n",
                     i == -s_dial.pos ? "> " : "", WDAY[wx_local_wday(p, d->time)], deg(d->lo), deg(d->hi),
                     COND[wx_kind(d->code, true)]);
        }
    } else {
        snprintf(t, sizeof t, "%s", s_where[s_sel].name);
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
        if (i < N_ROWS) { s_rcx[i] = r.spr.centerx; s_rcy[i] = r.spr.centery; s_rw[i] = (int16_t)r.spr.w; s_rh[i] = (int16_t)r.spr.h; }
        if (r.shape == CLK_SHAPE_TEXT) {
            if (t >= T_TITLE && t < T_TITLE + N_LAB) R.lab[t - T_TITLE] = i;
            if (t == T_COLA) R.col = r;
            if (t >= T_COLA && t <= T_COLD) R.colw[t - T_COLA] = (int16_t)r.spr.w;
            if (t == T_CLOCK) R.clock = i;
            if (t >= T_FTITLE && t < T_FTITLE + N_FIX) R.fix[t - T_FTITLE] = i;
            continue;
        }
        if (t == T_ICON) R.icon = i;
        else if (t >= T_SICON0 && t < T_SICON0 + N_SICON) R.sicon[t - T_SICON0] = i;
        else if (t >= T_BAR0 && t < T_BAR0 + N_BAR) R.bar[t - T_BAR0] = i;
        else if (t >= T_DAY0 && t < T_DAY0 + N_DAYB) R.dayb[t - T_DAY0] = i;
        else if (t == T_DOTS) R.dots = i;
        else if (t >= T_PART0 && t < T_PART0 + N_PART) R.part[t - T_PART0] = i;
        else if (t == T_HALO) R.halo = i;
        else if (t == T_HICON) R.hicon = i;
    }
    R.n_part = 0;                       /* the pool runs from part[0]: a gap ends it */
    while (R.n_part < N_PART && R.part[R.n_part] >= 0) R.n_part++;
}

ORB_EXPORT("orb_init") int32_t orb_init(void)
{
    orb_knob_dial();                     /* every turn is the dial's, from the first click */
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

ORB_EXPORT("orb_activate") void orb_activate(void) { orb_dial_home(&s_dial); set_visible(true); redraw(); }
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
    if (orb_dial_idle(&s_dial, up_ms())) s_dirty = true;   /* §51: a glance shows Now */
    const bool was_busy = s_busy;
    feed_step();
    if (was_busy != s_busy) s_dirty = true;
    if (s_dirty) redraw();
}

/* A hold leaves the app; the router's disarm on the way out sends the dial home, so
 * coming back shows Now. orb_knob_dial keeps the turn the app's while it shows. */
ORB_EXPORT("orb_set_armed") int32_t orb_set_armed(int32_t armed)
{
    if (!armed) { orb_dial_home(&s_dial); s_dirty = true; }
    return armed;
}

/* Turn: along the dial. Press: the next place, the position kept, so the same hour or
 * day can be read for each place in turn (§2.4). */
ORB_EXPORT("orb_input") void orb_input(int32_t delta, int32_t pressed)
{
    bool moved = false;
    if (pressed && s_nwhere > 1) {
        s_sel = (s_sel + 1) % s_nwhere;
        moved = true;
    }
    if (delta) {
        dial_fit(place());
        moved |= orb_dial_turn(&s_dial, delta, up_ms());
    }
    if (!moved) return;
    s_dirty = true;
    redraw();
}

/* ---- the options page (the dial's menu draws it): the clock, shown or hidden ------- */

enum { OPT_CLOCK, OPT_N };

ORB_EXPORT("orb_opt_count") int32_t orb_opt_count(void) { return OPT_N; }
ORB_EXPORT("orb_opt_label") void orb_opt_label(int32_t idx) { orb_reply(idx == OPT_CLOCK ? "Clock" : ""); }
ORB_EXPORT("orb_opt_value") void orb_opt_value(int32_t idx)
{
    orb_reply(idx == OPT_CLOCK ? (s_clock_on ? "shown" : "hidden") : "");
}
/* Opening the page re-reads the settings, so a value written from outside is what it shows. */
ORB_EXPORT("orb_opt_arm") void orb_opt_arm(int32_t armed)
{
    if (armed) load_places();
}
/* A press shows or hides the clock and commits it. Its 24- or 12-hour format is the
 * orb's, on the UNITS page. */
ORB_EXPORT("orb_opt_sel") void orb_opt_sel(int32_t idx)
{
    if (idx != OPT_CLOCK) return;
    s_clock_on = !s_clock_on;
    orb_cfg_set("clock", s_clock_on);
    s_cfg_gen = orb_cfg_gen();
    s_dirty = true;
}

