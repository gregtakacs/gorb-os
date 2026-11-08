/**
 * flights — the flight tracker, as an app (apps/sdk/orb.h; docs/flight-tracker-design.md).
 *
 * Everything that is the tracker's own lives here: which provider to ask and how
 * often, resting a provider that refused, adsb.lol's hourly budget, the streaming
 * parse (readsb.c), the contact table and its dead reckoning (model.c), route
 * lookups, the radar and the knob's dial (§51). What it borrows from the OS is the
 * HTTPS fetch (orb.net_*: one request at a time, gated, run off the UI task, with the
 * owner's adsb.lol key attached by the OS and never seen here), the home location,
 * its declared settings, a canvas with drawn primitives, and the beat.
 *
 * Moved out of the firmware (main/flights/ + main/app/orb_flights.c) by §43 with its
 * behaviour unchanged. One shape changed: the feed used to be its own task, and is now
 * a state machine stepped from orb_service, which the OS calls every pass whether or
 * not the tracker is on the glass — so background polling still runs off the glass.
 * A body is parsed a slice per pass, so a 70 KB answer never holds the dial.
 */
#include "orb.h"
#include "orb_stage.h"
#include "orb_dial.h"

#include "model.h"
#include "readsb.h"

#define W        ORB_SCREEN_W
#define H        ORB_SCREEN_H
#define CX       (W / 2)
#define CY       (H / 2)
#define RING_R   184                 /* full range, px: header and footer sit outside it */
#define VIS_MS   15000               /* poll period while on the glass */
#define BOOST_MS 20000               /* background poll while something is worth watching */
#define TICK_MS  1000                /* redraw while on the glass (the manifest floor) */

/* The arrows: LVGL's symbol font for the widgets, Unicode for a theme's fonts, which carry
 * the arrows themselves (theme-text-plan §2.1). Picked at init. */
static const char *SYM_UP = "\xEF\x81\xB7", *SYM_DOWN = "\xEF\x81\xB8", *SYM_RIGHT = "\xEF\x81\x94";

/* The range steps in the orb's distance unit (weather-plan §2.2): round numbers in each
 * unit, never a converted nautical mile, and no outer step wider than 80 nm (148 km, 92 mi),
 * the radius the parse budget below was measured on. The stored `range` stays an index, so
 * a unit change keeps the step. Rows by ORB_UNIT_KM, _MI, _NM. */
static const uint16_t RANGES[3][4] = { { 20, 40, 80, 140 }, { 10, 25, 50, 90 }, { 10, 20, 40, 80 } };
static const double   UNIT_KM[3]   = { 1.0, 1.609344, 1.852 };
static const uint32_t BG_MS[]     = { 0, 60000, 120000, 300000 };
static const char *const BG_TXT[] = { "off", "1 min", "2 min", "5 min" };
static const char *const SRC_TXT[] = { "adsb.lol", "adsb.fi" };
#define N_RANGES (int)(sizeof RANGES[0] / sizeof RANGES[0][0])
#define N_BG     (int)(sizeof BG_MS / sizeof BG_MS[0])

enum { SRC_LOL = 0, SRC_FI = 1 };

static int64_t now_ms(void) { return orb_uptime_us() / 1000; }

/* ---- settings (declared in the container; app.json) ------------------------------ */

typedef struct {
    double  latd, lond;          /* the OS home (cap location), not a setting */
    bool    home_ok;
    char    place[40];
    int32_t range, source, bg, boost, ground;
} settings_t;

static settings_t s_set = { .range = 2, .source = 1, .bg = 2, .boost = 0, .ground = 1 };
static uint32_t   s_cfg_gen, s_home_gen = 0xFFFFFFFFu, s_units_gen;
static orb_units_t s_units;          /* the orb's units (OS v1.2.10): distance only here */

static int dist_unit(void) { return s_units.dist <= ORB_UNIT_NM ? s_units.dist : ORB_UNIT_KM; }
static float range_km(void) { return (float)(RANGES[dist_unit()][s_set.range] * UNIT_KM[dist_unit()]); }
/* The feeds take a whole number of nautical miles: the step, rounded up so the scope's
 * edge is always inside what was asked for. */
static unsigned range_req_nm(void) { return (unsigned)ceil(range_km() / 1.852 - 1e-9); }
static void range_text(char *b, size_t n)
{
    snprintf(b, n, "%u %s", (unsigned)RANGES[dist_unit()][s_set.range], orb_dist_label(&s_units));
}
static bool       s_visible;

/* The knob is one dial centred on the plain scope (app-platform-plan.md §51, flight-
 * tracker-design "Knob"): clockwise the aircraft nearest first, anticlockwise farthest
 * first, each with its card; a press steps the range; a hold leaves. The dial's position
 * is re-read from the selected aircraft before every turn, because the order re-sorts
 * by distance on every poll and the card follows its aircraft, not a slot. */
static orb_dial_t s_dial = { .idle_ms = ORB_DIAL_IDLE_MS };
static int8_t     s_side = 1;            /* which way the selection was reached */
static int64_t    s_rng_until;           /* the header names the range until then */

/* Home is the OS's: one owner setting, read through the location cap. A revoked cap
 * reads as no home, and the footer says why. */
static void load_home(void)
{
    orb_home_t h;
    memset(&h, 0, sizeof h);
    const int32_t e = orb_home(&h);
    s_home_gen = orb_home_gen();
    s_set.home_ok = e == ORB_OK;
    s_set.latd = h.lat_e6 / 1e6;
    s_set.lond = h.lon_e6 / 1e6;
    snprintf(s_set.place, sizeof s_set.place, "%s", e == ORB_OK ? h.place :
             e == ORB_ERR_NOT_FOUND ? "" : "location not allowed");
}

static void load_settings(void)
{
    int32_t v;
    if (orb_cfg_get("range", &v) == ORB_OK)  s_set.range = v;
    if (orb_cfg_get("source", &v) == ORB_OK) s_set.source = v;
    if (orb_cfg_get("bg", &v) == ORB_OK)     s_set.bg = v;
    if (orb_cfg_get("boost", &v) == ORB_OK)  s_set.boost = v;
    if (orb_cfg_get("ground", &v) == ORB_OK) s_set.ground = v;
    load_home();
    orb_units(&s_units);
    s_units_gen = orb_units_gen();
    if (s_set.range < 0 || s_set.range >= N_RANGES) s_set.range = 2;
    if (s_set.bg < 0 || s_set.bg >= N_BG) s_set.bg = 1;
    s_cfg_gen = orb_cfg_gen();
}

/* ---- the feed ------------------------------------------------------------------- */

#define FLIGHTS_MAX 24               /* nearest kept per poll: a 466 px scope's worth */

typedef struct {
    orb_ac_t ac;
    float    dist_km, bearing_deg;
} flight_t;

/* A 4xx is the provider saying no: rest it, starting at a minute and doubling to a
 * quarter hour; a 403 is a ban and rests an hour, doubling to twelve. */
#define REST_MIN_MS   60000u
#define REST_MAX_MS   900000u
#define BAN_MIN_MS    3600000u
#define BAN_MAX_MS    43200000u
/* adsb.lol's budget (the owner's free key, 2026-11-01): 150 requests an hour, so one
 * request every ten minutes at most, whatever the settings say. */
#define LOL_GAP_MS    600000u

static uint32_t next_rest_ms(uint32_t last, int status)
{
    const bool ban = status == 403;
    const uint32_t lo = ban ? BAN_MIN_MS : REST_MIN_MS, hi = ban ? BAN_MAX_MS : REST_MAX_MS;
    uint32_t r = last ? last * 2 : lo;
    if (r < lo) r = lo;
    return r > hi ? hi : r;
}

static struct {
    int64_t  rest_until[2];
    uint32_t rest_ms[2];
    int64_t  lol_next;
    int64_t  next_poll;          /* 0 = poll now; INT64_MAX = no poll scheduled */
    bool     kick;               /* a setting changed: poll at the next chance */
    /* the picture */
    flight_t work[FLIGHTS_MAX], pub[FLIGHTS_MAX];
    int      work_n, pub_n;
    int64_t  pub_ms;             /* 0 = never */
    uint32_t seq;
    bool     interesting;
    int      last_src;
    uint32_t polls, ok, fail, skip_rest;
} F;

static bool resting(int src)
{
    const int64_t t = now_ms();
    return t < F.rest_until[src] || (src == SRC_LOL && t < F.lol_next);
}

/* A 429's Retry-After is a floor: the rest is never shorter than asked. */
static void rest(int src, int status, uint32_t retry_after_s)
{
    F.rest_ms[src] = next_rest_ms(F.rest_ms[src], status);
    if (retry_after_s && (uint64_t)retry_after_s * 1000 > F.rest_ms[src])
        F.rest_ms[src] = retry_after_s > 86400u ? 86400000u : retry_after_s * 1000u;
    F.rest_until[src] = now_ms() + F.rest_ms[src];
    orb_logf("FLT provider %d refused (%d); resting it %u s", src, status,
             (unsigned)(F.rest_ms[src] / 1000));
}

static bool feed_enabled(void)
{
    return s_set.home_ok && (s_visible || BG_MS[s_set.bg]);
}

static uint32_t next_wait_ms(void)
{
    if (s_visible) return VIS_MS;
    const uint32_t bg = BG_MS[s_set.bg];
    if (!bg) return 0;
    if (s_set.boost && F.interesting && BOOST_MS < bg) return BOOST_MS;
    return bg;
}

/* The sink: nearest-N as the body streams past. */
typedef struct {
    orb_readsb_t p;
    double lat0, lon0, coslat0;
    bool   hide_ground;
} poll_ctx_t;
static poll_ctx_t s_pc;

static void emit(const orb_ac_t *a, void *ctx)
{
    const poll_ctx_t *c = ctx;
    if (c->hide_ground && a->on_ground) return;
    /* Equirectangular is exact enough inside a few hundred km, and the dial cannot
     * draw the difference. */
    const double dy = (a->lat - c->lat0) * 111.195;
    const double dx = (a->lon - c->lon0) * 111.195 * c->coslat0;
    const float d = (float)sqrt(dx * dx + dy * dy);
    int slot;
    if (F.work_n < FLIGHTS_MAX) {
        slot = F.work_n++;
    } else {
        int far = 0;
        for (int i = 1; i < F.work_n; i++)
            if (F.work[i].dist_km > F.work[far].dist_km) far = i;
        if (d >= F.work[far].dist_km) return;
        slot = far;
    }
    F.work[slot].ac = *a;
    F.work[slot].dist_km = d;
    float b = (float)(atan2(dx, dy) * 180.0 / M_PI);
    F.work[slot].bearing_deg = b < 0 ? b + 360.0f : b;
}

/* ---- routes (§6.6): adsbdb.com, one GET per callsign, ~700 B ---------------------- */

#define ROUTE_ORIGIN  "https://api.adsbdb.com"
#define ROUTE_TRIES   2
#define ROUTES_N      16
enum { ROUTE_NONE, ROUTE_PENDING, ROUTE_FOUND };

typedef struct {
    char       cs[9];
    uint8_t    state, tries;
    fm_route_t r;
} route_slot_t;

static route_slot_t s_routes[ROUTES_N];
static int          s_route_next;
static int64_t      s_route_rest_until;
static uint32_t     s_route_rest_ms;
static char         s_route_cs[9];        /* the callsign in flight */

static int route_state(const char *cs, fm_route_t *out)
{
    if (!cs || !fm_route_callsign_ok(cs)) return ROUTE_NONE;
    for (int k = 0; k < ROUTES_N; k++)
        if (!strcmp(s_routes[k].cs, cs)) {
            if (s_routes[k].state == ROUTE_FOUND && out) *out = s_routes[k].r;
            return s_routes[k].state;
        }
    route_slot_t *e = &s_routes[s_route_next];          /* the oldest goes */
    s_route_next = (s_route_next + 1) % ROUTES_N;
    memset(e, 0, sizeof *e);
    snprintf(e->cs, sizeof e->cs, "%s", cs);
    e->state = ROUTE_PENDING;
    return ROUTE_PENDING;
}

static bool route_pending(char cs[9])
{
    for (int k = 0; k < ROUTES_N; k++) {
        const route_slot_t *e = &s_routes[(s_route_next + k) % ROUTES_N];
        if (e->cs[0] && e->state == ROUTE_PENDING) { memcpy(cs, e->cs, 9); return true; }
    }
    return false;
}

static void route_answer(const char *cs, int st, const fm_route_t *r, bool failed)
{
    for (int k = 0; k < ROUTES_N; k++) {
        route_slot_t *e = &s_routes[k];
        if (strcmp(e->cs, cs)) continue;
        if (failed && ++e->tries < ROUTE_TRIES) break;  /* stays pending: try again */
        e->state = (uint8_t)st;
        if (r) e->r = *r;
        break;
    }
}

/* ---- the request state machine ---------------------------------------------------- */

typedef enum { REQ_NONE, REQ_POLL, REQ_ROUTE } req_t;
static req_t    s_req;
static int      s_req_src;
static bool     s_parsing;           /* the body is landed and being tokenized */
#define PARSE_SLICE 100              /* JSON events taken per service pass (D51: the OS
                                      * lexes natively; this bounds the app's own work).
                                      * 400 held one pass for 34 ms over a 21 KB body at
                                      * 80 nm, a dropped frame per poll (2026-11-04); 100
                                      * keeps a pass near a quarter of that */

static void schedule_next(void)
{
    const uint32_t w = next_wait_ms();
    F.next_poll = w ? now_ms() + w : INT64_MAX;
}

static bool start_poll(void)
{
    F.polls++;
    int src = s_set.source ? SRC_FI : SRC_LOL;
    if (resting(src)) {
        if (resting(!src)) {
            F.skip_rest++;
            orb_log("FLT skip: both providers resting");
            schedule_next();
            return false;
        }
        src = !src;
    }
    char url[128];
    const unsigned nm = range_req_nm();
    if (src == SRC_FI)
        snprintf(url, sizeof url, "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%u",
                 s_set.latd, s_set.lond, nm);
    else
        snprintf(url, sizeof url, "https://api.adsb.lol/v2/point/%.4f/%.4f/%u",
                 s_set.latd, s_set.lond, nm);
    /* The owner's adsb.lol key is this app's `key` secret: the OS attaches it, and only
     * to api.adsb.lol (the manifest says so), so asking for it costs nothing elsewhere. */
    const char *cred = src == SRC_LOL && orb_net_cred("key") ? "key" : NULL;
    if (orb_net_get(url, cred) != ORB_OK) return false;
    /* Spent before the answer, so a request that hangs still counts. */
    if (src == SRC_LOL) F.lol_next = now_ms() + LOL_GAP_MS;
    s_req = REQ_POLL;
    s_req_src = src;
    s_parsing = false;
    memset(&s_pc, 0, sizeof s_pc);
    s_pc.lat0 = s_set.latd;
    s_pc.lon0 = s_set.lond;
    s_pc.coslat0 = cos(s_set.latd * M_PI / 180.0);
    s_pc.hide_ground = s_set.ground != 0;
    orb_readsb_init(&s_pc.p, emit, &s_pc);
    F.work_n = 0;
    return true;
}

static void finish_poll(const orb_net_info_t *ni, bool good)
{
    F.last_src = s_req_src;
    if (ni->status >= 400 && ni->status < 500) rest(s_req_src, ni->status, ni->retry_after_s);
    if (good) {
        F.rest_ms[s_req_src] = 0;            /* forgiven: the next refusal starts at a minute */
        F.ok++;
        memcpy(F.pub, F.work, sizeof F.pub);
        F.pub_n = F.work_n;
        F.pub_ms = now_ms();
        F.seq++;
        F.interesting = false;
        for (int i = 0; i < F.pub_n; i++)
            if (fm_interesting(&F.pub[i].ac, F.pub[i].dist_km, F.pub[i].bearing_deg))
                F.interesting = true;
    } else {
        F.fail++;
    }
    orb_logf("FLT %s %d %s total=%u B=%u ac=%u kept=%d", good ? "ok" : "FAIL", (int)ni->status,
             SRC_TXT[s_req_src], (unsigned)ni->total_ms, (unsigned)ni->len,
             (unsigned)s_pc.p.objects, F.work_n);
    orb_net_free();
    s_req = REQ_NONE;
    schedule_next();
}

static bool start_route(void)
{
    char cs[9];
    if (!route_pending(cs) || now_ms() < s_route_rest_until) return false;
    char path[32];
    snprintf(path, sizeof path, "/v0/callsign/%s", cs);
    char url[64];
    snprintf(url, sizeof url, "%s%s", ROUTE_ORIGIN, path);
    if (orb_net_get(url, NULL) != ORB_OK) return false;
    memcpy(s_route_cs, cs, sizeof s_route_cs);
    s_req = REQ_ROUTE;
    return true;
}

/* A 404 is adsbdb not knowing the callsign; another 4xx rests route lookups the way a
 * refusal rests a feed provider, and the callsign waits. */
static void finish_route(const orb_net_info_t *ni)
{
    const char *cs = s_route_cs;
    /* The ~700 B answer is a few dozen events: read in one pass, through the OS's
     * reader, straight off the landed body. */
    fm_route_t rt;
    bool found = false;
    if (ni->state == ORB_NET_DONE && ni->status == 200 && orb_json_begin(NULL, 0) == ORB_OK) {
        fm_route_rd_t rd;
        fm_route_begin(&rd);
        orb_json_tok_t t;
        for (int guard = 0; guard < 4096; guard++) {
            const int32_t k = orb_json_next(&t);
            if (k == ORB_JSON_END || k == ORB_JSON_ERROR) break;
            fm_route_take(&rd, &t);
        }
        found = fm_route_end(&rd, &rt);
    }
    if (found) {
        s_route_rest_ms = 0;
        route_answer(cs, ROUTE_FOUND, &rt, false);
        orb_logf("RTE %s %s-%s total=%u", cs, rt.from, rt.to, (unsigned)ni->total_ms);
    } else if (ni->state == ORB_NET_DONE && (ni->status == 200 || ni->status == 404)) {
        route_answer(cs, ROUTE_NONE, NULL, false);
        orb_logf("RTE %s none (%d)", cs, (int)ni->status);
    } else if (ni->status >= 400 && ni->status < 500) {
        s_route_rest_ms = next_rest_ms(s_route_rest_ms, ni->status);
        if (ni->retry_after_s && (uint64_t)ni->retry_after_s * 1000 > s_route_rest_ms)
            s_route_rest_ms = ni->retry_after_s > 86400u ? 86400000u : ni->retry_after_s * 1000u;
        s_route_rest_until = now_ms() + s_route_rest_ms;
        orb_logf("RTE refused (%d); resting %u s", (int)ni->status, (unsigned)(s_route_rest_ms / 1000));
    } else {
        route_answer(cs, ROUTE_NONE, NULL, true);
        orb_logf("RTE %s failed: err %d status %d", cs, (int)ni->err, (int)ni->status);
    }
    orb_net_free();
    s_req = REQ_NONE;
}

static void feed_step(void)
{
    orb_net_info_t ni;
    switch (s_req) {
    case REQ_NONE:
        if (!feed_enabled()) { F.next_poll = 0; return; }
        if (F.kick || (F.next_poll != INT64_MAX && now_ms() >= F.next_poll)) {
            F.kick = false;
            if (start_poll()) return;
        }
        /* Routes ride the time between polls, one lookup per pass, and only the
         * tracker on the glass asks. */
        if (s_visible) (void)start_route();
        return;
    case REQ_POLL:
        if (orb_net_info(&ni) != ORB_OK || ni.state == ORB_NET_BUSY) return;
        if (ni.state != ORB_NET_DONE || ni.status != 200) { finish_poll(&ni, false); return; }
        if (!s_parsing) {
            if (orb_json_begin(NULL, 0) != ORB_OK) { finish_poll(&ni, false); return; }
            s_parsing = true;
        }
        for (int n = 0; n < PARSE_SLICE; n++) {
            orb_json_tok_t t;
            const int32_t k = orb_json_next(&t);
            if (!orb_readsb_token(&s_pc.p, &t) || k == ORB_JSON_END || k == ORB_JSON_ERROR) {
                s_parsing = false;
                finish_poll(&ni, orb_readsb_done(&s_pc.p));
                return;
            }
        }
        return;                                    /* the next slice, next pass */
        return;
    case REQ_ROUTE:
        if (orb_net_info(&ni) != ORB_OK || ni.state == ORB_NET_BUSY) return;
        finish_route(&ni);
        return;
    }
}

/* ---- the contact table (model.c: merge, aging, projection) ------------------------ */

static fm_t     s_m;
static uint32_t s_seq_seen;
static char     s_sel[8];          /* the selected contact's hex, "" = none */

static void merge(void)
{
    const int64_t now = orb_uptime_us();
    fm_merge_begin(&s_m);
    for (int i = 0; i < F.pub_n; i++)
        fm_upsert(&s_m, &F.pub[i].ac, F.pub[i].dist_km, F.pub[i].bearing_deg, now);
    fm_merge_end(&s_m, now);
}

/* ---- pixels --------------------------------------------------------------------- */

static int32_t s_cv, s_head, s_foot, s_card;
static float   s_ring_cos[48], s_ring_sin[48];

/* flight-tracker-design §11, theme-pool-plan P4: a theme of this app's in the pool puts the
 * scope on the stage. Its rows (apps/flights/art/bake_theme.py): slot 0 the arm, slots 1..15
 * the plane pool, each naming one bank of 7 shapes x 7 bands, and the selection and emergency
 * rings at SEL_SLOT and EM_SLOT. The range rings and home dot are the plate's
 * (theme-text-plan §0). With no theme, the canvas. */
/* One turn of the arm: the arm row's vmax, in seconds (flight-tracker-design §12.5). A bake
 * from before the arm had a speed says 60, the seconds domain, and sweeps at the old 4 s. */
#define SWEEP_MS    4000
#define SWEEP_S_MIN 2
#define SWEEP_S_MAX 30
#define PLANE_SLOTS 15
#define STALE_OPA   128              /* a contact past its fresh window, as dim() halves it */
/* The theme's labels (theme-text-plan §2.2), by slot: the header, the footer, the info card
 * and the range figure. With all four, the app makes no widget and the stage draws every
 * pixel; without them (an older bake), the widgets. */
enum { LAB_HEAD, LAB_FOOT, LAB_CARD, LAB_RNG, LAB_N };
#define LAB_SLOT0   20
#define CARD_CLEAR  24                   /* the card at this slot keeps clear of its contact */
#define CARD_MARGIN 20                   /* px around the card a contact under it may not enter */
#define SEL_SLOT    16
#define EM_SLOT     17
static orb_stage_info_t s_stage;
static bool    s_themed;
static int32_t s_arm = -1;
static int32_t s_planes[PLANE_SLOTS];
/* Far contacts as dots, the theme's choice: the tracked aircraft past the PLANE_SLOTS
 * nearest each take a dot row of their own. A theme with no dot rows shows only the nearest
 * PLANE_SLOTS; dot rows carrying 7 frames colour a dot by its altitude band (band_of), one
 * frame draws every dot alike. The slots are the nine an app's rows leave free under 32
 * (CLK_ROW_MAX): with FLIGHTS_MAX tracked, every one of them is on the glass. */
static const uint8_t DOT_SLOTS[] = { 18, 19, 25, 26, 27, 28, 29, 30, 31 };
#define DOT_MAX ((int)(sizeof DOT_SLOTS / sizeof DOT_SLOTS[0]))
static int32_t s_dots[DOT_MAX];
static int     s_ndots;
static bool    s_dot_bands;
static int     s_nplanes;
static int32_t s_sel_row = -1, s_em_row = -1, s_rng;
static int32_t s_lab[LAB_N] = { -1, -1, -1, -1 };
static bool    s_labels;                 /* all four labels: the theme writes the words */
static int32_t s_sweep_ms = SWEEP_MS;
static bool    s_card_clear;             /* the card moves off the contact it describes */
static int     s_card_dy;                /* where it stands: 0 home, else its mirror's offset */

/* Set a label's text and colour only when they change: with a theme the stage ticks the
 * app every frame, and a re-set label is a repaint. */
static void put_label(int32_t h, int slot, const char *s, uint32_t col);
static void set_hidden(int32_t h, bool hide, bool *was);
static int16_t s_card_cy, s_card_hw, s_card_hh, s_card_cx;   /* the card's box at home: centre, half size */

/* Say `s` on label `which`: the theme's label in `tone`, or the widget in `col`. */
static void say(int which, const char *s, int32_t tone, uint32_t col)
{
    static const int32_t *const widget[LAB_N] = { &s_head, &s_foot, &s_card, &s_rng };
    if (s_labels) {
        orb_stage_text(s_lab[which], s, tone);
        orb_stage_pose(s_lab[which], 0, 0, which == LAB_CARD ? s_card_dy : 0);
    } else {
        put_label(*widget[which], which, s, col);
    }
}

/* Show or hide label `which`. */
static void show(int which, bool on, bool *was)
{
    static const int32_t *const widget[LAB_N] = { &s_head, &s_foot, &s_card, &s_rng };
    if (s_labels) orb_stage_pose(s_lab[which], on ? 0 : ORB_STAGE_HIDDEN, 0, which == LAB_CARD ? s_card_dy : 0);
    else set_hidden(*widget[which], !on, was);
}

static void put_label(int32_t h, int slot, const char *s, uint32_t col)
{
    static char     last[4][192];
    static uint32_t lastc[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    if (strcmp(last[slot], s) != 0) {
        snprintf(last[slot], sizeof last[slot], "%s", s);
        orb_ui_text(h, s);
    }
    if (lastc[slot] != col) { lastc[slot] = col; orb_ui_set(h, ORB_P_TEXT_COLOR, (int32_t)col); }
}

/* Put a marker row on a contact, or hide it. */
static void marker(int32_t row, bool on, int x, int y)
{
    if (row >= 0) orb_stage_pose(row, on ? 0 : ORB_STAGE_HIDDEN, x - CX, y - CY);
}

static void set_hidden(int32_t h, bool hide, bool *was)
{
    if (*was == hide) return;
    *was = hide;
    orb_ui_set(h, ORB_P_HIDDEN, hide);
}

static uint32_t dim(uint32_t rgb) { return (rgb >> 1) & 0x7F7F7F; }

static void ring_at(int cx, int cy, int r, uint32_t c)
{
    for (int a = 0; a < 48; a++)
        orb_cv_pixel(s_cv, cx + (int)lroundf(r * s_ring_cos[a]), cy + (int)lroundf(r * s_ring_sin[a]), c);
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

/* The bank's band index, in alt_rgb's order (make_art.BANDS). */
static int band_of(const orb_ac_t *a)
{
    if (a->on_ground)       return 0;
    if (!a->has_alt)        return 1;
    if (a->alt_ft < 3000)   return 2;
    if (a->alt_ft < 10000)  return 3;
    if (a->alt_ft < 20000)  return 4;
    if (a->alt_ft < 30000)  return 5;
    return 6;
}

/* Turboprops by ICAO type prefix: the emitter category calls them and business jets alike
 * "small" (A2), so the type tells them apart. */
static bool turboprop(const char *t)
{
    static const char *const P[] = { "AT4", "AT7", "DH8", "SF34", "E120", "B190", "C208", "PC12",
                                     "BE20", "BE9", "JS3", "D328", "F50", "SW4", "C212", "DHC6",
                                     "PC6", "AN2", "BE30", "C441" };
    for (unsigned i = 0; t[0] && i < sizeof P / sizeof P[0]; i++)
        if (!strncmp(t, P[i], strlen(P[i]))) return true;
    return false;
}

/* The bank's shape index, in bake_theme.SHAPES order: light, turboprop, bizjet, airliner,
 * heavy, helicopter, generic. From the ADS-B emitter category, the type where it must. */
static int shape_of(const orb_ac_t *a)
{
    const char *c = a->category;
    if (c[0] == 'A') switch (c[1]) {
        case '1': return 0;
        case '2': return turboprop(a->type) ? 1 : 2;
        case '3': case '4': return 3;
        case '5': return 4;
        case '6': return 2;
        case '7': return 5;
        default:  break;
    }
    if (c[0] == 'B' && (c[1] == '1' || c[1] == '4')) return 0;   /* glider, ultralight */
    if (!c[0] && turboprop(a->type)) return 1;
    return 6;
}

static void draw_frame(void)
{
    const float rkm = range_km();
    fm_project(&s_m, rkm, orb_uptime_us(), CX, CY, RING_R);

    orb_cv_fill(s_cv, 0);
    orb_cv_ring(s_cv, CX, CY, RING_R, 0x2A4A3A);
    orb_cv_ring(s_cv, CX, CY, RING_R / 2, 0x1C3026);
    orb_cv_line(s_cv, CX, CY - RING_R - 6, CX, CY - RING_R + 6, 0x2A4A3A);   /* north tick */
    orb_cv_disc(s_cv, CX, CY, 3, 0xFFFFFF);                                  /* home */

    for (int o = s_m.norder - 1; o >= 0; o--) {                              /* far first */
        const fm_trk_t *t = &s_m.t[s_m.order[o]];
        const uint32_t c = t->live ? alt_rgb(&t->ac) : dim(alt_rgb(&t->ac));
        const int x = (int)lroundf(t->x), y = (int)lroundf(t->y);
        if (!isnan(t->ac.track) && !t->ac.on_ground) {
            float len = 6.0f + (isnan(t->ac.gs) ? 0.0f : t->ac.gs / 600.0f * 16.0f);
            if (len > 22.0f) len = 22.0f;
            const float tr = t->ac.track * (float)M_PI / 180.0f;
            orb_cv_line(s_cv, x, y, x + (int)lroundf(len * sinf(tr)), y - (int)lroundf(len * cosf(tr)), c);
        }
        orb_cv_disc(s_cv, x, y, 3, c);
        if (emergency(&t->ac)) ring_at(x, y, 9, 0xFF2020);
        if (s_sel[0] && !strcmp(t->ac.hex, s_sel)) ring_at(x, y, 8, 0xFFFFFF);
    }
    char rng[12];
    range_text(rng, sizeof rng);
    orb_cv_text(s_cv, CX + 122, CY + 136, 61, ORB_FONT_14, 0x4A7A62, rng);
    orb_cv_flush(s_cv);
}

/* The themed frame: the arm turned, every drawn contact on its row — the frame its shape
 * and band pick, turned to its track, moved to where it is — and the rows past the drawn
 * ones hidden. The nearest contacts are the ones drawn; among them the higher takes the
 * later row, and rows at one depth draw in table order, so a higher aircraft passes over a
 * lower one (owner, §12.5). The selection and an emergency ride as rings over their
 * contacts, and a card that keeps clear moves off the contact it describes. */
static void draw_stage(void)
{
    const float rkm = range_km();
    fm_project(&s_m, rkm, orb_uptime_us(), CX, CY, RING_R);
    if (s_arm >= 0)
        orb_stage_pose(s_arm, (int32_t)((now_ms() % s_sweep_ms) * 3600 / s_sweep_ms), 0, 0);
    int idx[PLANE_SLOTS];
    const int n = fm_stack(&s_m, s_nplanes, idx);
    bool sel_on = false, em_on = false;
    /* The far ones: the tracked order past the n the plane rows took, nearest first. */
    int d = 0;
    for (int o = n; o < s_m.norder && d < s_ndots; o++, d++) {
        const fm_trk_t *t = &s_m.t[s_m.order[o]];
        const int32_t row = s_dots[d];
        const int x = (int)lroundf(t->x), y = (int)lroundf(t->y);
        if (s_dot_bands) orb_stage_frame(row, band_of(&t->ac));
        orb_stage_pose(row, 0, x - CX, y - CY);
        orb_stage_opacity(row, t->live ? 255 : STALE_OPA);
        if (!sel_on && s_sel[0] && !strcmp(t->ac.hex, s_sel)) {
            marker(s_sel_row, true, x, y);
            sel_on = true;
            if (s_card_clear)
                s_card_dy = fm_card_dy(s_card_cx, s_card_cy, s_card_hw, s_card_hh, CY, x, y, s_card_dy, CARD_MARGIN);
        }
        if (!em_on && emergency(&t->ac)) {
            marker(s_em_row, true, x, y);
            em_on = true;
        }
    }
    for (; d < s_ndots; d++) orb_stage_pose(s_dots[d], ORB_STAGE_HIDDEN, 0, 0);
    int k = 0;
    for (; k < n; k++) {
        const fm_trk_t *t = &s_m.t[idx[k]];
        const int32_t row = s_planes[k];
        const int x = (int)lroundf(t->x), y = (int)lroundf(t->y);
        orb_stage_frame(row, shape_of(&t->ac) * 7 + band_of(&t->ac));
        const int32_t ang = isnan(t->ac.track) || t->ac.on_ground ? 0 : (int32_t)lroundf(t->ac.track * 10.0f);
        orb_stage_pose(row, ang, x - CX, y - CY);
        orb_stage_opacity(row, t->live ? 255 : STALE_OPA);
        if (!sel_on && s_sel[0] && !strcmp(t->ac.hex, s_sel)) {
            marker(s_sel_row, true, x, y);
            sel_on = true;
            if (s_card_clear)
                s_card_dy = fm_card_dy(s_card_cx, s_card_cy, s_card_hw, s_card_hh, CY, x, y, s_card_dy, CARD_MARGIN);
        }
        if (!em_on && emergency(&t->ac)) {
            marker(s_em_row, true, x, y);
            em_on = true;
        }
    }
    for (; k < s_nplanes; k++) orb_stage_pose(s_planes[k], ORB_STAGE_HIDDEN, 0, 0);
    if (!sel_on) marker(s_sel_row, false, CX, CY);
    if (!em_on) marker(s_em_row, false, CX, CY);
    char rng[12];
    range_text(rng, sizeof rng);
    say(LAB_RNG, rng, ORB_TONE_NORMAL, 0x8A9A90);
}

/* How many aircraft the glass draws: on a theme, the plane rows and then the dots take
 * the nearest-first order (draw_stage), and the rest are tracked but not shown. The dial
 * stops at the drawn ones, so a card never describes an aircraft nobody can see. */
static int drawn_n(void)
{
    const int cap = s_themed ? s_nplanes + s_ndots : s_m.norder;
    return s_m.norder < cap ? s_m.norder : cap;
}

static void draw_text(void)
{
    char b[64];
    /* The selected aircraft left the range or aged out: its card closes and the dial
     * is home, rather than a different aircraft's card taking its place unasked. */
    if (s_sel[0] && (fm_find(&s_m, s_sel) < 0 || fm_find(&s_m, s_sel) >= drawn_n())) {
        s_sel[0] = 0;
        orb_dial_home(&s_dial);
    }
    /* The header says what the knob last did: the range a press chose, for a moment, or
     * which aircraft the card is, so a turn is never a guess. */
    const bool rng_note = now_ms() < s_rng_until, accent = rng_note || s_sel[0];
    if (rng_note)
        { char r[12]; range_text(r, sizeof r); snprintf(b, sizeof b, "range  %s", r); }
    else if (s_sel[0])
        snprintf(b, sizeof b, "%d of %d", fm_find(&s_m, s_sel) + 1, drawn_n());
    else {
        /* What the glass can show: the plane rows, and the dots when the theme has them. A
         * count past it says how many are drawn, so the header never promises more. */
        const int shown = s_themed ? s_nplanes + s_ndots : s_m.norder;
        if (s_m.norder > shown) snprintf(b, sizeof b, "%d aircraft, %d shown", s_m.norder, shown);
        else snprintf(b, sizeof b, "%d aircraft", s_m.norder);
    }
    say(LAB_HEAD, b, accent ? ORB_TONE_ACCENT : ORB_TONE_NORMAL, accent ? 0x39FF14 : 0xD0D0D0);

    const int64_t age = F.pub_ms ? now_ms() - F.pub_ms : -1;
    uint32_t col = 0x909090;
    if (!s_set.home_ok)         { snprintf(b, sizeof b, "%s", s_set.place[0] ? s_set.place : "set home in the hub (Orb tab)"); col = 0xFFB23C; }
    else if (age < 0)           snprintf(b, sizeof b, F.skip_rest ? "feed refused, resting" : "waiting for the feed");
    else if (age < 20000)       snprintf(b, sizeof b, "live  %s", SRC_TXT[F.last_src & 1]);
    else if (age < 3 * VIS_MS)  snprintf(b, sizeof b, "%u s ago", (unsigned)(age / 1000));
    else { snprintf(b, sizeof b, "feed down %u min", (unsigned)(age / 60000 + 1)); col = 0xFFB23C; }
    say(LAB_FOOT, b, col == 0xFFB23C ? ORB_TONE_WARNING : ORB_TONE_NORMAL, col);

    static bool card_hidden = true;
    const int si = fm_find(&s_m, s_sel);
    if (si < 0) { s_sel[0] = 0; show(LAB_CARD, false, &card_hidden); return; }
    const fm_trk_t *t = &s_m.t[s_m.order[si]];
    char alt[16], vs[24], spd[24];
    if (t->ac.on_ground) snprintf(alt, sizeof alt, "on ground");
    else if (t->ac.has_alt) snprintf(alt, sizeof alt, "%d ft", (int)t->ac.alt_ft);
    else snprintf(alt, sizeof alt, "alt ---");
    if (isnan(t->ac.baro_rate) || fabsf(t->ac.baro_rate) < 100) vs[0] = 0;
    else snprintf(vs, sizeof vs, "  %s %d fpm", t->ac.baro_rate > 0 ? SYM_UP : SYM_DOWN,
                  abs((int)t->ac.baro_rate));
    if (isnan(t->ac.gs)) snprintf(spd, sizeof spd, "--- kt");
    else snprintf(spd, sizeof spd, "%d kt", (int)t->ac.gs);
    /* The route line, when adsbdb knows the callsign and the aircraft is plausibly
     * flying it (§6.6). Asking queues the lookup; the line appears on a later tick,
     * and a stale schedule never does. */
    char route[24] = "";
    fm_route_t rt;
    if (route_state(t->ac.flight, &rt) == ROUTE_FOUND &&
        fm_route_plausible(&rt, t->ac.lat, t->ac.lon, t->ac.on_ground ? NAN : t->ac.track, t->ac.baro_rate))
        snprintf(route, sizeof route, "%s %s %s\n", rt.from, SYM_RIGHT, rt.to);
    /* Distance and bearing are not on the card: the scope already shows both (owner,
     * flight-tracker-design §11.5). */
    char card[192];
    snprintf(card, sizeof card, "%s  %s\n%s%s%s\n%s  trk %d",
             t->ac.flight[0] ? t->ac.flight : t->ac.hex, t->ac.type[0] ? t->ac.type : "",
             route, alt, vs, spd, isnan(t->ac.track) ? 0 : (int)t->ac.track);
    say(LAB_CARD, card, ORB_TONE_NORMAL, 0xFFFFFF);
    show(LAB_CARD, true, &card_hidden);
}

/* ---- the map under the scope (flights-map-plan §2.2, OS v1.2.38) ------------------- *
 * Home's map, made by Orb Manager, drawn by the OS into live layer 0 at the theme's depth:
 * over the ground, under the arm and the aircraft, and cut at the outer ring so the bezel
 * stays the theme's. One view a range, rendered once and blended through the theme's
 * colours; a range step clears the layer until its own view is ready, so the map never
 * stands at the wrong scale under the aircraft. The theme says what is drawn at each range
 * (roads and runways close, the airports' symbols wide) and which landmarks are named. */
#define FL_VALUES_VER 1
#define N_NAMES       6
#define NAME_SLOT0    32
typedef struct {
    uint32_t map_pal[16];          /* ARGB per map role */
    uint8_t  width[16];            /* line widths; the marks' looks (ORB_MAP_LOOK) */
    uint8_t  dash[16];             /* line dashes; the fills' floors; the marks' rank caps */
    uint16_t role_on[N_RANGES];    /* per range step, nearest first: 0 everywhere is no map */
    uint8_t  mark_on[N_RANGES];
    uint8_t  names[N_RANGES];      /* landmarks named, 0-6 */
    uint8_t  name_kinds;           /* bit per ORB_MARK_* that may be named */
    uint8_t  name_rank;            /* the worst rank named; 0 any */
    uint8_t  map_order;            /* the layer's depth on the rows' scale */
    uint8_t  clip;                 /* half the radius, px, the map stops at */
    uint8_t  name_px;              /* the names' font size, for their boxes */
    uint8_t  pad[3];
} fl_values_t;
_Static_assert(sizeof(fl_values_t) == 120, "flights-bake.js writes this layout");
static fl_values_t V;
static bool    s_map;                    /* the theme draws a map */
static bool    s_layer, s_map_drawn;
static int32_t s_view, s_view_next;
static int32_t s_view_key = -1, s_next_key = -1;
static int64_t s_map_retry_ms;
static int32_t s_name[N_NAMES] = { -1, -1, -1, -1, -1, -1 };
static bool    s_reread;                 /* the theme is read again at the next frame */
static void    stage_reread(void);

static void read_values(void)
{
    uint32_t ver = 0;
    memset(&V, 0, sizeof V);
    const uint32_t n = s_themed ? orb_stage_values(&V, sizeof V, &ver) : 0;
    if (n && (n != sizeof V || ver != FL_VALUES_VER)) {
        orb_logf("flights: the theme's values are layout %u, %u B: no map", (unsigned)ver, (unsigned)n);
        memset(&V, 0, sizeof V);
    }
    s_map = false;
    for (int k = 0; k < N_RANGES; k++) {
        if (V.names[k] > N_NAMES) V.names[k] = N_NAMES;
        s_map |= V.role_on[k] != 0;
    }
    if (V.name_px < 14 || V.name_px > 40) V.name_px = 18;
}
static void names_hide(void);

static void names_hide(void)
{
    for (int k = 0; k < N_NAMES; k++) if (s_name[k] >= 0) orb_stage_pose(s_name[k], ORB_STAGE_HIDDEN, 0, 0);
}

/* The landmarks' names, best rank first, kept inside the inner rings, off home's dot, off
 * the range figure and off each other: under the name's own mark, as a chart prints it. */
static void names_draw(void)
{
    const int want = V.names[s_set.range], cw = V.name_px / 2 + 1, lh = V.name_px + 6;
    int n = 0;
    int16_t bx[N_NAMES], by[N_NAMES], bw[N_NAMES];
    for (uint32_t k = 0; s_view > 0 && n < want && k < 32; k++) {
        orb_map_mark_t m;
        if (orb_map_mark(s_view, k, &m) != 1) break;
        if (s_name[n] < 0) break;
        if (!(V.name_kinds >> m.kind & 1) || (V.name_rank && m.rank > V.name_rank)) continue;
        const int w = cw * (int)strlen(m.name) + 8, x = m.x, y = m.y + lh / 2 + 6;
        const int dx = x - CX, dy = y - CY;
        if (dx * dx + dy * dy > (RING_R - 24) * (RING_R - 24)) continue;
        if (dx * dx + dy * dy < 30 * 30) continue;                       /* home's dot */
        if (x + w / 2 > CX + 70 && y + lh / 2 > CY + 116) continue;      /* the range figure */
        bool hit = false;
        for (int j = 0; j < n && !hit; j++)
            hit = abs(x - bx[j]) < (w + bw[j]) / 2 && abs(y - by[j]) < lh;
        if (hit) continue;
        bx[n] = (int16_t)x; by[n] = (int16_t)y; bw[n] = (int16_t)w;
        orb_stage_text(s_name[n], m.name, ORB_TONE_NORMAL);
        orb_stage_pose(s_name[n], 0, x - CX, y - CY);
        n++;
    }
    for (; n < N_NAMES; n++) if (s_name[n] >= 0) orb_stage_pose(s_name[n], ORB_STAGE_HIDDEN, 0, 0);
}

/* What a view is drawn for: the range step, home and the maps' generations. */
static int32_t map_key(void)
{
    return (int32_t)((s_set.range & 3) | (s_home_gen & 0xFFF) << 2 | (orb_maps_gen() & 0xFFF) << 14 | (uint32_t)dist_unit() << 26);
}

static void map_clear(void)
{
    if (s_layer && s_map_drawn) orb_stage_live(0, V.map_order, 0);
    s_map_drawn = false;
    names_hide();
}

static void map_open(void)
{
    orb_map_view_t v;
    memset(&v, 0, sizeof v);
    /* the scope's outer ring is the range; the view's radius is the glass's edge */
    v.radius_m = (int32_t)lroundf(range_km() * 1000.0f * (W / 2) / RING_R);
    v.role_on = V.role_on[s_set.range];
    v.mark_on = V.mark_on[s_set.range];
    v.clip = V.clip;
    memcpy(v.width, V.width, sizeof v.width);
    memcpy(v.dash, V.dash, sizeof v.dash);
    if (s_view_next > 0) orb_map_close(s_view_next);
    s_view_next = orb_map_open(0, &v);
    s_next_key = map_key();
    orb_logf("flights: map view %d for range %d, %d m", (int)s_view_next, (int)s_set.range, (int)v.radius_m);
    if (s_view_next < 0) {
        const bool absent = s_view_next == ORB_MAP_ABSENT || s_view_next == ORB_MAP_NOCARD;
        orb_logf("flights: the map did not open (%d), again in %d s", (int)s_view_next, absent ? 20 : 3);
        s_map_retry_ms = now_ms() + (absent ? 20000 : 3000);
        s_view_next = 0;
    }
}

static void map_step(void)
{
    if (!s_map || !s_set.home_ok || !s_visible) return;
    if (!s_layer) {                          /* refused before the stage binds: the next frame asks */
        s_layer = orb_stage_live(0, V.map_order, 0) == ORB_OK;
        s_map_drawn = false;
        if (!s_layer) return;
    }
    const int32_t key = map_key();
    if (key != s_view_key && key != s_next_key) {
        if (s_view_key >= 0 && (s_view_key & 3) != (key & 3)) map_clear();   /* another scale */
        if (now_ms() >= s_map_retry_ms) map_open();
    }
    if (s_view_next > 0) {
        const int32_t r = orb_map_ready(s_view_next);
        if (r < 0) {
            orb_map_close(s_view_next);
            s_view_next = 0; s_next_key = -1;
            s_map_retry_ms = now_ms() + (r == ORB_MAP_ABSENT || r == ORB_MAP_NOCARD ? 20000 : 3000);
            if (r != ORB_MAP_ABSENT) orb_logf("flights: the map did not draw (%d)", (int)r);
            map_clear();                          /* the scope stands alone, and asks again */
        } else if (r > 0) {
            if (s_view > 0) orb_map_close(s_view);
            s_view = s_view_next; s_view_key = s_next_key;
            s_view_next = 0; s_next_key = -1;
            s_map_drawn = false;
        }
    }
    if (s_view > 0 && !s_map_drawn && s_view_key == key) {
        orb_stage_live(0, V.map_order, 0);
        orb_map_draw(ORB_CV_LIVE0, s_view, V.map_pal, 0, 0);
        s_map_drawn = true;
        names_draw();
    }
}

static void redraw(void)
{
    if (!s_visible) return;
    if (s_reread) stage_reread();
    if (F.seq != s_seq_seen) { s_seq_seen = F.seq; merge(); }
    if (s_themed) { map_step(); draw_stage(); } else draw_frame();
    draw_text();
}

static int32_t text(int32_t font, uint32_t rgb, int32_t align, int32_t y)
{
    const int32_t h = orb_ui_create(ORB_UI_LABEL, 0);
    orb_ui_set(h, ORB_P_FONT, font);
    orb_ui_set(h, ORB_P_TEXT_COLOR, (int32_t)rgb);
    orb_ui_align(h, align, 0, y);
    return h;
}

/* ---- entry points ---------------------------------------------------------------- */

static bool is_dot_slot(uint16_t tag)
{
    for (int k = 0; k < DOT_MAX; k++) if (DOT_SLOTS[k] == tag) return true;
    return false;
}

/* The theme's rows, by slot: the arm at 0, the planes after it, the marker rings, and the
 * far-contact dots when the theme carries them. */
static void stage_rows(void)
{
    s_nplanes = 0;
    s_ndots = 0;
    s_dot_bands = false;
    s_sel_row = s_em_row = -1;
    s_card_clear = false; s_card_dy = 0;
    for (int k = 0; k < LAB_N; k++) s_lab[k] = -1;
    for (int k = 0; k < N_NAMES; k++) s_name[k] = -1;
    for (int32_t i = 0; i < (int32_t)s_stage.n_rows; i++) {
        clk_row_t r;
        if (orb_stage_row(i, &r) != ORB_OK) continue;
        if (r.tag == 0) {
            s_arm = i;
            s_sweep_ms = r.value_max >= SWEEP_S_MIN && r.value_max <= SWEEP_S_MAX ? r.value_max * 1000 : SWEEP_MS;
        }
        else if (r.tag == SEL_SLOT) s_sel_row = i;
        else if (r.tag == EM_SLOT) s_em_row = i;
        else if (r.shape != CLK_SHAPE_TEXT && is_dot_slot(r.tag) && s_ndots < DOT_MAX) {
            if (!s_ndots) s_dot_bands = orb_stage_frame(i, -1) >= 7;
            s_dots[s_ndots++] = i;
        }
        else if (r.shape == CLK_SHAPE_TEXT && r.tag >= LAB_SLOT0 && r.tag < LAB_SLOT0 + LAB_N)
            s_lab[r.tag - LAB_SLOT0] = i;
        else if (r.shape == CLK_SHAPE_TEXT && r.tag >= NAME_SLOT0 && r.tag < NAME_SLOT0 + N_NAMES)
            s_name[r.tag - NAME_SLOT0] = i;
        else if (r.shape == CLK_SHAPE_TEXT && r.tag == CARD_CLEAR) {
            s_lab[LAB_CARD] = i;
            s_card_clear = true;
            s_card_cx = r.spr.centerx; s_card_cy = r.spr.centery;
            s_card_hw = (int16_t)(r.spr.w / 2); s_card_hh = (int16_t)(r.spr.h / 2);
        }
        else if (r.tag <= PLANE_SLOTS && s_nplanes < PLANE_SLOTS && orb_stage_frame(i, -1) >= 49)
            s_planes[s_nplanes++] = i;
    }
}

ORB_EXPORT("orb_init") int32_t orb_init(void)
{
    orb_knob_dial();                     /* every turn is the dial's, from the first click */
    s_themed = orb_stage_open(&s_stage) == ORB_OK && !s_stage.base;
    if (s_themed) {
        stage_rows();
        read_values();
        names_hide();
        s_labels = s_lab[LAB_HEAD] >= 0 && s_lab[LAB_FOOT] >= 0 && s_lab[LAB_CARD] >= 0 &&
                   s_lab[LAB_RNG] >= 0;
        if (s_labels) {
            SYM_UP = "\xE2\x86\x91"; SYM_DOWN = "\xE2\x86\x93"; SYM_RIGHT = "\xE2\x86\x92";
        } else {
            s_rng = orb_ui_create(ORB_UI_LABEL, 0);
            orb_ui_set(s_rng, ORB_P_FONT, ORB_FONT_14);
            orb_ui_align(s_rng, ORB_AL_CENTER, 140, 140);
        }
        orb_logf("flight tracker on its theme: arm row %d, %d plane rows, %d dot rows%s, markers %d/%d, %s%s",
                 (int)s_arm, s_nplanes, s_ndots, s_ndots ? (s_dot_bands ? " by altitude" : " plain") : "",
                 (int)s_sel_row, (int)s_em_row,
                 s_labels ? "the theme's labels" : "widgets for the words", s_map ? ", a map" : "");
    } else {
        s_cv = orb_cv_create(0, W, H);
        if (!s_cv) return ORB_ERR_NO_MEM;
    }
    if (!s_labels) {                 /* a theme's labels draw the words: no widget at all */
        s_head = text(ORB_FONT_16, 0xD0D0D0, ORB_AL_TOP_MID, 16);
        s_foot = text(ORB_FONT_14, 0x909090, ORB_AL_BOTTOM_MID, -14);
        s_card = text(ORB_FONT_16, 0xFFFFFF, ORB_AL_BOTTOM_MID, -40);
        orb_ui_set(s_card, ORB_P_TEXT_ALIGN, 1);
        orb_ui_set(s_card, ORB_P_BG, 0x101820);
        orb_ui_set(s_card, ORB_P_BG_OPA, 204);
        orb_ui_set(s_card, ORB_P_PAD, 8);
        orb_ui_set(s_card, ORB_P_RADIUS, 10);
        orb_ui_set(s_card, ORB_P_HIDDEN, 1);
    }
    for (int a = 0; a < 48; a++) {
        const double t = a * M_PI / 24.0;
        s_ring_cos[a] = (float)cos(t);
        s_ring_sin[a] = (float)sin(t);
    }
    load_settings();
    s_visible = false;
    F.next_poll = 0;                 /* background polling starts at the first pass */
    char rng[12];
    range_text(rng, sizeof rng);
    orb_logf("flight tracker ready: home %s, range %s, background %s",
             s_set.home_ok ? "set" : "not set", rng, BG_TXT[s_set.bg]);
    return ORB_OK;
}

/* The theme, read again at the first frame after every arrival: at a boot the app builds
 * before its theme is bound, and a theme sent from the editor binds while the app is away,
 * so the rows and values orb_init saw may not be the ones on the glass now. The OS binds the
 * stage after orb_activate and orb_resume, so the read waits for the stage's first tick. */
static void stage_reread(void)
{
    clk_row_t r0;
    if (!s_themed) { s_reread = false; return; }
    if (orb_stage_row(0, &r0) != ORB_OK) return;          /* not bound yet (orb_show comes first) */
    s_reread = false;
    /* never orb_stage_open here: it binds the stage again, and from inside the stage's own
     * frame that stops it (Bronze, 2026-11-08: the glass held its plate). The rows are read
     * up to the most a theme carries; a slot past the theme's last answers an error. */
    s_stage.n_rows = CLK_ROW_MAX;
    const bool had = s_map;
    stage_rows();
    read_values();
    names_hide();
    s_view_key = s_next_key = -1;           /* the map is drawn again, in the values read now */
    if (had != s_map) orb_logf("flights: the theme %s", s_map ? "draws a map" : "draws no map");
}

static void set_visible(bool v)
{
    if (s_visible == v) return;
    s_visible = v;
    orb_set_beat(v && !s_themed ? TICK_MS : 0);   /* a theme's stage ticks it every frame */
    /* On the glass: a fresh picture now, unless the one in hand (or the one being
     * fetched) is already fresh — a background poll that just ran is not repeated. */
    if (v) F.kick = s_req != REQ_POLL && (!F.pub_ms || now_ms() - F.pub_ms > 5000);
    else schedule_next();
    s_layer = s_map_drawn = false;   /* the OS frees the live layers off the glass */
    if (!v) { s_view = s_view_next = 0; s_view_key = s_next_key = -1; }   /* and closes the map's views */
}

ORB_EXPORT("orb_activate") void orb_activate(void) { s_sel[0] = 0; orb_dial_home(&s_dial); s_reread = true; set_visible(true); }
ORB_EXPORT("orb_resume")   void orb_resume(void)   { s_reread = true; set_visible(true); }
ORB_EXPORT("orb_suspend")  void orb_suspend(void)  { set_visible(false); }

ORB_EXPORT("orb_show") void orb_show(void)
{
    set_visible(true);
    redraw();
}

ORB_EXPORT("orb_tick") void orb_tick(uint32_t now)
{
    (void)now;
    /* §51: 20 s without a turn closes the card, so a glance shows the scope. */
    if (orb_dial_idle(&s_dial, now_ms())) s_sel[0] = 0;
    redraw();
}

ORB_EXPORT("orb_service") void orb_service(void)
{
    /* A setting written from outside (the hub's /api/v1/apps/cfg door, the options
     * page) or a new home lands here: a generation moved, so re-read and re-apply. */
    if (orb_cfg_gen() != s_cfg_gen || orb_home_gen() != s_home_gen || orb_units_gen() != s_units_gen) {
        /* A new home invalidates every kept contact: their east/north offsets are from
         * the old one. */
        if (orb_home_gen() != s_home_gen) {
            s_m.n = s_m.norder = 0;
            s_sel[0] = 0;
        }
        load_settings();
        F.kick = true;
        redraw();
    }
    feed_step();
}

/* A hold leaves the app; the router's disarm on the way out sends the dial home, so
 * coming back shows the plain scope. orb_knob_dial keeps the turn the app's. */
ORB_EXPORT("orb_set_armed") int32_t orb_set_armed(int32_t armed)
{
    if (!armed && s_sel[0]) { s_sel[0] = 0; orb_dial_home(&s_dial); redraw(); }
    return armed;
}

/* The selected aircraft's place on the dial: +k the k-th nearest when reached
 * clockwise, -k the k-th farthest drawn when reached the other way, 0 none. */
static void dial_sync(void)
{
    const int n = drawn_n(), i = s_sel[0] ? fm_find(&s_m, s_sel) : -1;
    s_dial.lo = -n;
    s_dial.hi = n;
    s_dial.pos = i < 0 || i >= n ? 0 : s_side > 0 ? i + 1 : -(n - i);
}

ORB_EXPORT("orb_input") void orb_input(int32_t delta, int32_t pressed)
{
    if (pressed) {
        /* The next range, wrapping: the next poll asks for the new radius, the rings
         * rescale at once, and the header names it for a moment. */
        s_set.range = (s_set.range + 1) % N_RANGES;
        orb_cfg_set("range", s_set.range);
        s_cfg_gen = orb_cfg_gen();
        F.kick = true;
        s_rng_until = now_ms() + 2000;
    }
    if (delta) {
        dial_sync();
        if (orb_dial_turn(&s_dial, delta, now_ms())) {
            const int p = s_dial.pos;
            if (!p) s_sel[0] = 0;
            else {
                s_side = p > 0 ? 1 : -1;
                const fm_trk_t *t = &s_m.t[s_m.order[p > 0 ? p - 1 : drawn_n() + p]];
                snprintf(s_sel, sizeof s_sel, "%s", t->ac.hex);
            }
        }
    }
    redraw();
}

/* The options page is the OS's, drawn from app.json's settings (app-platform-plan §57):
 * a row's write moves orb_cfg_gen, and orb_service re-reads the settings. */
