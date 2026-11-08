/* main.c — the radar (docs/radar-plan.md §2.5).
 *
 * LibreWXR's radar over the OS's map of home or a place, on the app's theme:
 *  - live layer 0 holds the map, drawn once a view (orb_map_open/draw); live layer 1 the
 *    rain, cleared and drawn alone each step (orb_cv_png8 through the theme's ramp), so a
 *    step costs one frame's decode, not the map's blend again (§6.5);
 *  - the theme's rows stand among and over them: the place's dot, the time on the rim,
 *    the time, the place, the range, a quiet status line and the landmarks' names;
 *  - the dial is time, centred on Now: anticlockwise back through the past, clockwise
 *    into the nowcast; untouched, the past loops at 4 a second and holds Now 2 s; a press
 *    is the next place, a press-and-turn the zoom (Wide, Region, Local), a hold the OS's.
 * Every colour, width, dash and depth is the theme's values (§2.11); the app's own
 * settings are the zoom it starts at and the LibreWXR source. */
#include <string.h>
#include "orb.h"
#include "orb_stage.h"
#include "orb_dial.h"
#include "radar.h"

#define CX 233
#define CY 233
#define DEFAULT_SOURCE "https://api.librewxr.net"

/* the theme's slots (apps/radar/editor/radar-bake.js) */
enum { T_DOT = 0, T_HAND = 1, T_TIME = 8, T_PLACE = 9, T_RANGE = 10, T_STATUS = 11, T_NAME0 = 12, T_ZOOM = 18 };
#define N_NAMES 6
#define N_ROWS 64

static orb_stage_info_t s_stage;
static bool  s_themed;
static int32_t R_dot = -1, R_hand = -1, R_time = -1, R_place = -1, R_range = -1, R_status = -1, R_zoom = -1;
static int32_t R_name[N_NAMES];
static rd_values_t V;

static int64_t up_ms(void) { return orb_uptime_us() / 1000; }
static int64_t now_s(void) { int64_t s; int32_t f; orb_now(&s, &f); return s; }

/* ---- the places: home, then the OS's three (radar-plan §2.1) -------------------------- */

typedef struct { int os;  int32_t lat_e6, lon_e6; char name[40]; } where_t;
static where_t  s_where[4];
static int      s_nwhere, s_sel;
static uint32_t s_home_gen = 0xFFFFFFFFu, s_places_gen = 0xFFFFFFFFu, s_cfg_gen, s_maps_gen, s_units_gen = 0xFFFFFFFFu;
static orb_units_t s_units;
static int      s_zoom = 1;                 /* 0 Wide, 1 Region, 2 Local */
static char     s_source[96] = DEFAULT_SOURCE;

static void load_places(void)
{
    const int keep = s_nwhere ? s_where[s_sel].os : 0;
    s_nwhere = 0;
    orb_home_t h;
    if (orb_home(&h) == ORB_OK && (h.lat_e6 || h.lon_e6)) {
        where_t *w = &s_where[s_nwhere++];
        w->os = 0; w->lat_e6 = h.lat_e6; w->lon_e6 = h.lon_e6;
        memcpy(w->name, "Home", 5);
    }
    for (uint32_t i = 0; i < 3; i++) {
        orb_place_t p;
        if (orb_place(i, &p) != ORB_OK) continue;
        where_t *w = &s_where[s_nwhere++];
        w->os = (int)i + 1; w->lat_e6 = p.lat_e6; w->lon_e6 = p.lon_e6;
        memcpy(w->name, p.place, sizeof w->name);
        w->name[sizeof w->name - 1] = 0;
    }
    s_sel = 0;
    for (int i = 0; i < s_nwhere; i++) if (s_where[i].os == keep) s_sel = i;
    s_home_gen = orb_home_gen();
    s_places_gen = orb_places_gen();
}

static void load_settings(void)
{
    int32_t z = 1;
    if (orb_cfg_get("zoom", &z) == ORB_OK && z >= 0 && z < RD_ZOOMS) s_zoom = (int)z;
    char src[96] = "";
    if (orb_cfg_get_str("source", src, sizeof src) >= 0 && src[0]) {
        size_t n = strlen(src);
        while (n && src[n - 1] == '/') src[--n] = 0;     /* the paths start with '/' */
        memcpy(s_source, src, n + 1);
    } else {
        memcpy(s_source, DEFAULT_SOURCE, sizeof DEFAULT_SOURCE);
    }
    s_cfg_gen = orb_cfg_gen();
}

static const where_t *here(void) { return s_nwhere ? &s_where[s_sel] : NULL; }

/* ---- the frames (§2.2) --------------------------------------------------------------- */

#define LIST_EVERY_MS  600000u      /* the list moves every 10 minutes */
#define STALE_S          1800       /* past 30 minutes the age is shown */
#define N_SLOTS            24       /* the OS keeps 24 bodies an app */

typedef struct {
    int64_t t;
    char    path[RD_PATH_MAX];
    char    etag[24];
    int8_t  slot;                   /* -1 = none held */
    bool    now;                    /* nowcast */
    bool    recheck;                /* a nowcast frame held from the last list: ask again */
    bool    failed;
} frame_t;

static frame_t  F[RD_FRAMES];
static int      s_npast, s_nnow;
static int64_t  s_list_at;                      /* wall s of the last good list */
static rd_list_t s_rd;
enum { NET_IDLE, NET_LIST, NET_FRAME };
static int      s_net = NET_IDLE, s_net_frame = -1;
static bool     s_parsing;
static int64_t  s_next_list_ms;
static uint32_t s_backoff_ms;
static int64_t  s_retry_ms;                     /* frames: not before this after a failure */

static bool slot_used(int s)
{
    for (int i = 0; i < RD_FRAMES; i++) if (F[i].slot == s) return true;
    return false;
}
static int slot_free(void)
{
    for (int s = 0; s < N_SLOTS; s++) if (!slot_used(s)) return s;
    return -1;
}
static void frames_drop(void)                    /* a new place or zoom: none of these fit it */
{
    for (int i = 0; i < RD_FRAMES; i++) {
        if (F[i].slot >= 0) orb_blob_free((uint32_t)F[i].slot);
        F[i].slot = -1;
        F[i].etag[0] = 0;
        F[i].recheck = F[i].failed = false;
    }
}
static int idx_newest(void) { return s_npast - 1; }

/* A new list: a frame keeps its body when the same time and kind come back; a nowcast
 * frame kept is asked again (it may have been re-forecast); one that turned from nowcast
 * to past is fetched afresh (the same path is now the observation, §2.2). */
static void list_adopt(const rd_list_t *l)
{
    frame_t old[RD_FRAMES];
    memcpy(old, F, sizeof F);
    memset(F, 0, sizeof F);
    for (int i = 0; i < RD_FRAMES; i++) F[i].slot = -1;
    s_npast = l->n_past;
    s_nnow = l->n_now;
    for (int i = 0; i < RD_FRAMES; i++) {
        const bool nc = i >= s_npast;
        if (i >= s_npast + s_nnow) break;
        const rd_frame_t *src = nc ? &l->now[i - s_npast] : &l->past[i];
        F[i].t = src->t;
        memcpy(F[i].path, src->path, sizeof F[i].path);
        F[i].now = nc;
        for (int k = 0; k < RD_FRAMES; k++) {
            if (old[k].slot < 0 || old[k].t != F[i].t || old[k].now != nc) continue;
            F[i].slot = old[k].slot;
            memcpy(F[i].etag, old[k].etag, sizeof F[i].etag);
            F[i].recheck = nc;
            old[k].slot = -1;
            break;
        }
    }
    for (int k = 0; k < RD_FRAMES; k++) if (old[k].slot >= 0) orb_blob_free((uint32_t)old[k].slot);
}

/* The next frame to ask for: the newest past one, the nowcast, then the older past ones
 * newest first (§2.2). -1 when every frame is held. */
static int next_frame(void)
{
    if (s_npast && F[idx_newest()].slot < 0 && !F[idx_newest()].failed) return idx_newest();
    for (int i = s_npast; i < s_npast + s_nnow; i++)
        if ((F[i].slot < 0 || F[i].recheck) && !F[i].failed) return i;
    for (int i = s_npast - 2; i >= 0; i--) if (F[i].slot < 0 && !F[i].failed) return i;
    return -1;
}

static bool s_rain_dirty, s_ui_dirty;

static void net_done(void)
{
    orb_net_free();
    s_net = NET_IDLE;
    s_net_frame = -1;
}

static void net_failed(const orb_net_info_t *ni)
{
    s_backoff_ms = s_backoff_ms ? (s_backoff_ms >= 300000u ? 600000u : s_backoff_ms >= 120000u ? 300000u : 120000u) : 60000u;
    uint32_t w = s_backoff_ms;
    if (ni && ni->retry_after_s && ni->retry_after_s * 1000u > w)
        w = ni->retry_after_s > 3600u ? 3600000u : ni->retry_after_s * 1000u;
    if (s_net == NET_LIST) s_next_list_ms = up_ms() + w;
    else s_retry_ms = up_ms() + w;
    orb_logf("radar: %s failed (%d), again in %u s", s_net == NET_LIST ? "list" : "frame",
             ni ? (int)ni->status : -1, (unsigned)(w / 1000));
    net_done();
}

static void net_step(bool visible)
{
    orb_net_info_t ni;
    if (s_net == NET_IDLE) {
        if (!visible || !here()) return;
        const int64_t t = up_ms();
        if (t >= s_next_list_ms) {
            char url[160];
            if (!rd_list_url(url, sizeof url, s_source) || orb_net_get(url, NULL) != ORB_OK) { s_next_list_ms = t + 60000; return; }
            s_net = NET_LIST;
            s_parsing = false;
            rd_list_begin(&s_rd);
            return;
        }
        if (t < s_retry_ms) return;
        const int i = next_frame();
        if (i < 0) return;
        const where_t *w = here();
        char url[200], hdr[48] = "";
        if (!rd_frame_url(url, sizeof url, s_source, F[i].path, rd_img_z(s_zoom), w->lat_e6, w->lon_e6,
                          false)) { F[i].failed = true; return; }
        if (F[i].recheck && F[i].slot >= 0 && F[i].etag[0]) {
            memcpy(hdr, "If-None-Match: ", 15);
            memcpy(hdr + 15, F[i].etag, strlen(F[i].etag) + 1);
        }
        if (orb_net_request(ORB_GET, url, hdr[0] ? hdr : NULL, NULL, 0, NULL) != ORB_OK) return;
        s_net = NET_FRAME;
        s_net_frame = i;
        return;
    }
    if (orb_net_info(&ni) != ORB_OK || ni.state == ORB_NET_BUSY) return;
    if (s_net == NET_LIST) {
        if (ni.state != ORB_NET_DONE || ni.status != 200) { net_failed(&ni); return; }
        if (!s_parsing) {
            if (orb_json_begin(NULL, 0) != ORB_OK) { net_failed(&ni); return; }
            s_parsing = true;
        }
        for (int n = 0; n < 120; n++) {                  /* tokens a service pass (D51) */
            orb_json_tok_t tk;
            const int32_t k = orb_json_next(&tk);
            if (!rd_list_token(&s_rd, &tk) || k == ORB_JSON_END || k == ORB_JSON_ERROR) {
                s_parsing = false;
                if (!rd_list_ok(&s_rd)) { net_failed(&ni); return; }
                list_adopt(&s_rd);
                s_list_at = now_s();
                s_backoff_ms = 0;
                s_next_list_ms = up_ms() + LIST_EVERY_MS;
                s_ui_dirty = s_rain_dirty = true;
                orb_logf("radar: list %d past + %d nowcast, newest %d", s_npast, s_nnow, (int)(F[idx_newest()].t % 100000));
                net_done();
                return;
            }
        }
        return;
    }
    /* a frame */
    frame_t *f = s_net_frame >= 0 ? &F[s_net_frame] : NULL;
    if (!f) { net_done(); return; }
    if (ni.state == ORB_NET_DONE && ni.status == 304 && f->slot >= 0) {
        f->recheck = false;                              /* the forecast stands */
        net_done();
        return;
    }
    if (ni.state == ORB_NET_DONE && ni.status == 404) {
        /* the server no longer keeps it (the list is up to 10 min old): not asked again, and
         * nothing backs off, since the rest are there */
        f->failed = true;
        net_done();
        return;
    }
    if (ni.state != ORB_NET_DONE || ni.status != 200 || ni.len < 40) { net_failed(&ni); return; }
    const int s = f->slot >= 0 ? f->slot : slot_free();
    const int32_t kept = s < 0 ? ORB_ERR_NO_MEM : orb_net_keep((uint32_t)s);
    if (kept == ORB_ERR_NO_MEM && !f->now && s_net_frame < idx_newest()) {
        /* The OS keeps 768 KB of bodies an app, and a stormy day's frames are 50-70 KB:
         * the older past frames go unfetched for this list, and the loop starts later.
         * Not a failure: nothing backs off. */
        for (int k = 0; k <= s_net_frame; k++) F[k].failed = true;
        orb_logf("radar: the frames fill the OS's 768 KB; the loop starts at %d of %d", s_net_frame + 1, s_npast);
        net_done();
        return;
    }
    if (kept != ORB_OK) { net_failed(&ni); return; }
    f->slot = (int8_t)s;
    f->recheck = false;
    char tag[24];
    const int32_t tn = orb_net_header("ETag", tag, sizeof tag);
    if (tn > 0 && tn < (int32_t)sizeof tag) memcpy(f->etag, tag, (size_t)tn + 1); else f->etag[0] = 0;
    s_backoff_ms = 0;
    s_rain_dirty = s_ui_dirty = true;
    net_done();
}

/* ---- the map view (§2.4) --------------------------------------------------------------- */

static int32_t s_view = 0, s_view_next = 0;     /* handles; 0 = none */
static bool    s_map_dirty, s_map_absent;
static int64_t s_map_retry_ms;                  /* a map that failed for want of memory: again */
static int     s_shown = -2;                   /* the frame on the glass, -1 none */

/* No map on the card (or no card) is "No map yet" until the hub makes one; anything else
 * (memory, a read) is tried again in 3 s, the old view kept up meanwhile. */
static void map_failed(int32_t why)
{
    s_view_next = 0;
    if (why == ORB_MAP_ABSENT || why == ORB_MAP_NOCARD) { s_map_absent = true; s_map_dirty = true; }
    else s_map_retry_ms = up_ms() + 3000;
    orb_logf("radar: the map did not open (%d)%s", (int)why, s_map_absent ? "" : ", again in 3 s");
}

static void view_open(void)
{
    const where_t *w = here();
    if (!w) return;
    orb_map_view_t v;
    memset(&v, 0, sizeof v);
    v.radius_m = rd_radius_m(s_zoom, w->lat_e6);
    v.role_on = rd_roles_at(&V, s_zoom);              /* the roads thin out as it widens */
    v.mark_on = V.mark_on;
    memcpy(v.width, V.width, sizeof v.width);
    memcpy(v.dash, V.dash, sizeof v.dash);
    if (s_view_next > 0) orb_map_close(s_view_next);
    s_view_next = orb_map_open((uint32_t)w->os, &v);
    if (s_view_next < 0) map_failed(s_view_next);
    s_maps_gen = orb_maps_gen();
}

static void view_step(void)
{
    if (s_view_next <= 0) return;
    const int32_t r = orb_map_ready(s_view_next);
    if (r == 0) return;
    if (r < 0) {
        orb_map_close(s_view_next);
        map_failed(r);
        if (!s_map_absent) return;
    } else {
        if (s_view > 0) orb_map_close(s_view);
        s_view = s_view_next;
        s_view_next = 0;
        s_map_absent = false;
    }
    s_map_dirty = s_ui_dirty = s_rain_dirty = true;
    s_shown = -2;                      /* the rain again, at the new view's magnification */
}

/* ---- the glass ----------------------------------------------------------------------- */

static orb_dial_t s_dial = { .idle_ms = ORB_DIAL_IDLE_MS };
static const char *const ZOOM[RD_ZOOMS] = { "Wide", "Region", "Local", "Area", "City", "Close" };
/* Choosing a zoom (owner, 2026-11-07): from a press-and-turn's first click until the knob
 * is let go (orb_push(0)), only the pill moves: the loop holds, nothing is fetched, no view
 * opens. The choice lands on the let-go. ZOOM_LOST_MS is the safety for an end that never
 * comes (an OS before v1.2.27 tells none). */
#define ZOOM_LOST_MS 60000
static int     s_zoom_want = -1;
static int64_t s_zoom_click_ms;
static void zoom_step(void);
static bool    s_visible, s_layers;
static bool    s_play = true;                  /* untouched: the loop plays */
static int     s_loop = 0;                     /* the loop's frame, an index into F */
static int64_t s_loop_ms, s_zoom_said_ms;
static int32_t s_sx, s_sy, s_dx, s_dy;         /* the frames' draw window */

static void pose(int32_t row, int x, int y)
{
    if (row >= 0) orb_stage_pose(row, 0, x - CX, y - CY);
}
static void hide(int32_t row)
{
    if (row >= 0) orb_stage_pose(row, ORB_STAGE_HIDDEN, 0, 0);
}
static void say(int32_t row, int x, int y, const char *s, int tone)
{
    if (row < 0) return;
    if (!s || !s[0]) { hide(row); return; }
    orb_stage_text(row, s, tone);
    pose(row, x, y);
}

/* The frame the glass should show: the loop's while it plays, else the dial's. */
static int frame_wanted(void)
{
    if (!s_npast) return -1;
    if (s_play) return s_loop;
    const int i = idx_newest() + s_dial.pos;
    return i >= 0 && i < s_npast + s_nnow ? i : idx_newest();
}

static void layers_open(void)
{
    if (!s_themed) return;
    const int32_t a = orb_stage_live(0, V.map_order, 0), b = orb_stage_live(1, V.rain_order, 0);
    s_layers = a == ORB_OK && b == ORB_OK;
    if (!s_layers) orb_logf("radar: the live layers refused (%d, %d)", (int)a, (int)b);
    s_map_dirty = s_rain_dirty = s_ui_dirty = true;
    s_shown = -2;
}

static void draw_map(void)
{
    s_map_dirty = false;
    if (!s_layers) return;
    orb_stage_live(0, V.map_order, 0);                   /* clear: the plate shows */
    if (s_view > 0) orb_map_draw(ORB_CV_LIVE0, s_view, V.map_pal, 0, 0);
}

static void draw_rain(void)
{
    if (s_view_next > 0) return;       /* a new view is rendering: the old map and rain stay up */
    s_rain_dirty = false;
    if (!s_layers) return;
    const int i = frame_wanted();
    const frame_t *f = i >= 0 ? &F[i] : NULL;
    if (s_shown == i && f && f->slot >= 0) return;
    orb_stage_live(1, V.rain_order, 0);
    s_shown = -1;
    if (!f || f->slot < 0 || !orb_blob_len((uint32_t)f->slot)) return;
    const int k = rd_img_k(s_zoom);
    orb_png_draw_t d = { .slot = (uint32_t)f->slot, .sx = s_sx, .sy = s_sy, .dx = s_dx, .dy = s_dy,
                         .w = RD_GLASS, .h = RD_GLASS, .scale = k > 1 ? (k | ORB_PNG_SMOOTH) : 1, .pal = V.rain };
    const int32_t rc = orb_cv_png8(ORB_CV_LIVE1, &d);
    if (rc == ORB_OK) s_shown = i;
    else orb_logf("radar: frame %d (slot %d, %u B) did not draw: %d", i, (int)f->slot, (unsigned)orb_blob_len((uint32_t)f->slot), (int)rc);
}

/* The landmarks' names, best rank first, kept off the place's dot, off each other and
 * inside the glass. */
static void draw_names(void)
{
    int n = 0;
    int16_t bx[N_NAMES], by[N_NAMES], bw[N_NAMES];
    for (uint32_t k = 0; s_view > 0 && n < V.names && n < N_NAMES && k < 32; k++) {
        orb_map_mark_t m;
        if (orb_map_mark(s_view, k, &m) != 1) break;          /* 1: ready (orb_map_ready's word) */
        if (R_name[n] < 0) break;
        const int w = 7 * (int)strlen(m.name) + 10, x = m.x, y = m.y + 14;
        const int dx = x - CX, dy = y - CY;
        if (dx * dx + dy * dy > 185 * 185) continue;               /* the rim's, not the map's */
        if (dx * dx + dy * dy < 40 * 40) continue;                 /* the place's dot */
        if (y < 110 || y > 360) continue;                          /* the place, time, status and range */
        bool hit = false;
        for (int j = 0; j < n && !hit; j++)
            hit = (x - bx[j] < (w + bw[j]) / 2 && bx[j] - x < (w + bw[j]) / 2) && (y - by[j] < 18 && by[j] - y < 18);
        if (hit) continue;
        bx[n] = (int16_t)x; by[n] = (int16_t)y; bw[n] = (int16_t)w;
        say(R_name[n], x, y, m.name, 0);
        n++;
    }
    for (; n < N_NAMES; n++) hide(R_name[n]);
}

static void draw_ui(void)
{
    s_ui_dirty = false;
    const where_t *w = here();
    char b[48];
    pose(R_dot, CX, CY);
    say(R_place, CX, 64, w ? w->name : "", 0);
    if (w) rd_range_text(b, sizeof b, rd_radius_m(s_zoom_want >= 0 ? s_zoom_want : s_zoom, w->lat_e6), s_units.dist);
    else b[0] = 0;
    say(R_range, CX, 404, b, 0);
    const int i = frame_wanted();
    if (i >= 0) {
        const int min = (int)((F[i].t - F[idx_newest()].t) / 60);
        rd_time_text(b, sizeof b, min);
        say(R_time, CX, 92, b, F[i].now ? 1 : 0);                  /* a forecast reads as one */
        if (R_hand >= 0) orb_stage_pose(R_hand, min * 15, 0, 0);   /* 1.5 degrees a minute */
    } else {
        hide(R_time);
        hide(R_hand);
    }
    const char *st = "";
    /* the zoom, large, while it changes (owner, 2026-11-07: the small word was lost in the
     * map); a theme without the large row says it on the status line */
    const bool zooming = s_zoom_want >= 0 || up_ms() - s_zoom_said_ms < 2000;
    const int zs = s_zoom_want >= 0 ? s_zoom_want : s_zoom;
    if (R_zoom >= 0 && zooming && w) {
        char zb[40];
        rd_range_text(b, sizeof b, rd_radius_m(zs, w->lat_e6), s_units.dist);
        snprintf(zb, sizeof zb, "%s  %s", ZOOM[zs], b);
        say(R_zoom, CX, CY + 56, zb, 0);
    } else {
        hide(R_zoom);
    }
    const int want = frame_wanted();
    if (!w) st = "Set home on the hub";
    else if (zooming && R_zoom < 0) st = ZOOM[zs];
    else if (s_view_next > 0 || s_map_retry_ms) st = "Loading map";
    else if (s_map_absent) st = "No map yet";
    else if (!s_npast) st = s_net != NET_IDLE ? "Loading radar" : "No radar yet";
    else if (want >= 0 && F[want].slot < 0 && !F[want].failed) st = "Loading radar";
    else if (s_list_at && now_s() - s_list_at > STALE_S) {
        snprintf(b, sizeof b, "updated %d min ago", (int)((now_s() - s_list_at) / 60));
        st = b;
    }
    say(R_status, CX, 376, st, 0);
    draw_names();
}

/* A new place or zoom. The frames go with a place, or with a zoom that fetches another
 * image zoom; Local to Close share Local's frames, magnified (RD_IMG_ZMAX). */
static void window_set(void)
{
    const where_t *w = here();
    if (!w) return;
    const int k = rd_img_k(s_zoom);
    s_dx = s_dy = 0;
    if (k > 1) rd_window_k(rd_img_z(s_zoom), k, w->lat_e6, w->lon_e6, w->lat_e6, w->lon_e6, &s_sx, &s_sy, &s_dx, &s_dy);
    else rd_window(rd_img_z(s_zoom), w->lat_e6, w->lon_e6, w->lat_e6, w->lon_e6, &s_sx, &s_sy);
}

static void view_changed(bool frames)
{
    if (frames) {
        frames_drop();
        s_play = true;
        s_loop = 0;
        orb_dial_home(&s_dial);
    }
    s_shown = -2;
    window_set();
    s_map_absent = false;
    view_open();
    s_rain_dirty = s_ui_dirty = true;
}

static void place_changed(void)
{
    view_changed(true);
}

/* The loop (§2.5): oldest held past frame to Now, 250 ms a step, Now held 2 s. */
static void loop_step(void)
{
    if (!s_play || !s_npast || s_zoom_want >= 0) return;   /* choosing a zoom: it holds */
    orb_burst(1000);               /* a theme with no moving rows ticks once a second: keep 4 */
    const int64_t t = up_ms();
    const int hold = s_loop == idx_newest() ? 2000 : 250;
    if (t - s_loop_ms < hold) return;
    s_loop_ms = t;
    int next = s_loop >= idx_newest() ? 0 : s_loop + 1;
    while (next < idx_newest() && F[next].slot < 0) next++;    /* skip what has not landed */
    if (next != s_loop) { s_loop = next; s_rain_dirty = s_ui_dirty = true; }
}

static void redraw(void)
{
    if (!s_visible || !s_themed) return;
    if (!s_layers) layers_open();
    if (s_map_dirty) draw_map();
    if (s_rain_dirty) draw_rain();
    if (s_ui_dirty) draw_ui();
}

/* ---- entry points -------------------------------------------------------------------- */

static void find_rows(void)
{
    R_dot = R_hand = R_time = R_place = R_range = R_status = R_zoom = -1;
    for (int k = 0; k < N_NAMES; k++) R_name[k] = -1;
    for (int32_t i = 0; i < (int32_t)s_stage.n_rows && i < N_ROWS; i++) {
        clk_row_t r;
        if (orb_stage_row(i, &r) != ORB_OK) continue;
        const int t = r.tag;
        if (t == T_DOT) R_dot = i;
        else if (t == T_HAND) R_hand = i;
        else if (t == T_TIME) R_time = i;
        else if (t == T_PLACE) R_place = i;
        else if (t == T_RANGE) R_range = i;
        else if (t == T_STATUS) R_status = i;
        else if (t == T_ZOOM) R_zoom = i;
        else if (t >= T_NAME0 && t < T_NAME0 + N_NAMES) R_name[t - T_NAME0] = i;
    }
}

static void read_values(void)
{
    uint32_t ver = 0;
    rd_values_default(&V);
    rd_values_t got;
    const uint32_t n = s_themed ? orb_stage_values(&got, sizeof got, &ver) : 0;
    if (n == sizeof got && ver == RD_VALUES_VER) memcpy(&V, &got, sizeof V);
    else if (n) orb_logf("radar: the theme's values are layout %u, %u B: the defaults draw", (unsigned)ver, (unsigned)n);
    if (V.names > N_NAMES) V.names = N_NAMES;
}

/* The stage, read afresh: at a boot the theme may not be bound while the app builds, and
 * a theme may be swapped or fail to bind (PSRAM ran short: Bronze, 2026-11-07), so every
 * arrival asks, and the service asks again every 2 s while it has none. No widget stands in
 * for it: a widget on the page, hidden or not, took the dial off its fast path for good
 * (orb_stage.c's veto), and the hub installs the shelf's theme with the app. */
static int64_t s_stage_retry_ms;
static void stage_read(void)
{
    const bool was = s_themed;
    s_themed = orb_stage_open(&s_stage) == ORB_OK && !s_stage.base;
    if (s_themed) find_rows();
    if (s_themed && R_time < 0) s_themed = false;      /* not this app's layout */
    read_values();
    orb_set_beat(s_themed ? 0 : 2000);
    if (was != s_themed) orb_logf("radar: %s", s_themed ? "on its theme" : "no theme bound yet: asking again");
    s_stage_retry_ms = up_ms() + 2000;
}

ORB_EXPORT("orb_init") int32_t orb_init(void)
{
    orb_knob_dial();
    for (int i = 0; i < RD_FRAMES; i++) F[i].slot = -1;
    stage_read();
    load_settings();
    load_places();
    orb_units(&s_units);
    s_units_gen = orb_units_gen();
    s_dial.lo = 0; s_dial.hi = 0;
    orb_logf("radar ready: %s, %d places, zoom %d", s_themed ? "themed" : "no theme", s_nwhere, s_zoom);
    return ORB_OK;
}

static void on_glass(void)
{
    s_visible = true;
    s_layers = false;                  /* the stage frees the layers when another app binds */
    stage_read();
    /* the OS closed the map's views when the app left (no display buffers off the glass,
     * OS v1.2.27): a new one, the frames kept unless nothing has been shown yet */
    const bool first = !s_view && !s_view_next && !s_npast;
    s_view = s_view_next = 0;
    if (first) place_changed();
    else view_changed(false);
    orb_logf("radar on the glass: %s, rows %d, time %d, %d places, zoom %d", s_themed ? "themed" : "no theme",
             (int)s_stage.n_rows, (int)R_time, s_nwhere, s_zoom);
    redraw();
}

ORB_EXPORT("orb_activate") void orb_activate(void) { orb_dial_home(&s_dial); s_play = true; on_glass(); }
ORB_EXPORT("orb_resume") void orb_resume(void) { on_glass(); }
ORB_EXPORT("orb_show") void orb_show(void) { on_glass(); }
ORB_EXPORT("orb_suspend") void orb_suspend(void) { s_visible = false; s_layers = false; }

ORB_EXPORT("orb_tick") void orb_tick(uint32_t now)
{
    (void)now;
    zoom_step();
    loop_step();
    redraw();
}

ORB_EXPORT("orb_service") void orb_service(void)
{
    bool moved = false;
    if (orb_cfg_gen() != s_cfg_gen) {
        const int z = s_zoom;
        char src[96];
        memcpy(src, s_source, sizeof src);
        load_settings();
        if (strcmp(src, s_source)) { s_next_list_ms = 0; moved = true; }
        if (z != s_zoom && !s_visible) moved = true;   /* on the glass the push owns the zoom */
        else s_zoom = z;
    }
    if (orb_home_gen() != s_home_gen || orb_places_gen() != s_places_gen) {
        const int keep = here() ? here()->os : -1;
        load_places();
        moved = moved || !here() || here()->os != keep;
        if (!moved && here()) moved = true;             /* the place may have moved */
    }
    if (orb_maps_gen() != s_maps_gen && here()) { view_open(); }
    if (orb_units_gen() != s_units_gen) { orb_units(&s_units); s_units_gen = orb_units_gen(); s_ui_dirty = true; }
    if (moved) place_changed();
    orb_dial_range(&s_dial, -(s_npast > 0 ? s_npast - 1 : 0), s_nnow);
    if (orb_dial_idle(&s_dial, up_ms()) || (!s_play && !s_dial.pos && up_ms() - s_dial.last_ms > ORB_DIAL_IDLE_MS)) {
        s_play = true;                                   /* §51: a glance plays the loop */
        s_ui_dirty = s_rain_dirty = true;
    }
    zoom_step();
    if (s_visible && !s_themed && up_ms() >= s_stage_retry_ms) { stage_read(); s_layers = false; s_map_dirty = s_rain_dirty = s_ui_dirty = true; }
    if (s_map_retry_ms && up_ms() >= s_map_retry_ms) { s_map_retry_ms = 0; view_open(); }
    view_step();
    net_step(s_visible && s_zoom_want < 0);             /* nothing fetched while choosing */
    if (s_visible) redraw();
}

ORB_EXPORT("orb_set_armed") int32_t orb_set_armed(int32_t armed)
{
    if (!armed) { orb_dial_home(&s_dial); s_play = true; s_rain_dirty = s_ui_dirty = true; }
    return armed;
}

/* Turn: through time, the loop stopped. Press: the next place, at the same step. */
ORB_EXPORT("orb_input") void orb_input(int32_t delta, int32_t pressed)
{
    if (pressed && s_nwhere > 1) {
        s_sel = (s_sel + 1) % s_nwhere;
        place_changed();
    }
    if (delta) {
        if (s_play) { s_play = false; orb_dial_home(&s_dial); }
        orb_dial_turn(&s_dial, delta, up_ms());
        s_rain_dirty = s_ui_dirty = true;
    }
    redraw();
}

/* Press-and-turn (§2.10): clockwise closer, anticlockwise wider; the ends stop. */
ORB_EXPORT("orb_push") void orb_push(int32_t delta)
{
    if (!delta) {                                        /* let go: the choice lands */
        orb_logf("radar: press-and-turn let go (choosing %d)", s_zoom_want);
        if (s_zoom_want >= 0) s_zoom_click_ms = 0;
        zoom_step();
        redraw();
        return;
    }
    int z = (s_zoom_want >= 0 ? s_zoom_want : s_zoom) + (int)delta;   /* each click a step */
    if (z < 0) z = 0;
    if (z > RD_ZOOMS - 1) z = RD_ZOOMS - 1;
    s_zoom_want = z;
    s_zoom_click_ms = s_zoom_said_ms = up_ms();
    s_ui_dirty = true;
    redraw();
}

/* The gathered zoom lands once the turn has rested. */
static void zoom_step(void)
{
    if (s_zoom_want < 0 || (s_zoom_click_ms && up_ms() - s_zoom_click_ms < ZOOM_LOST_MS)) return;
    const int z = s_zoom_want;
    orb_logf("radar: zoom lands at %d (%s)", z, s_zoom_click_ms ? "no end came" : "let go");
    s_zoom_want = -1;
    s_zoom_said_ms = up_ms();
    s_ui_dirty = true;
    if (z != s_zoom) {
        const bool frames = rd_img_z(z) != rd_img_z(s_zoom);
        s_zoom = z;
        view_changed(frames);
    }
}

/* ---- the options page: the zoom it starts at ----------------------------------------- */

ORB_EXPORT("orb_opt_count") int32_t orb_opt_count(void) { return 1; }
ORB_EXPORT("orb_opt_label") void orb_opt_label(int32_t idx) { orb_reply(idx == 0 ? "Zoom" : ""); }
ORB_EXPORT("orb_opt_value") void orb_opt_value(int32_t idx)
{
    int32_t z = 1;
    orb_cfg_get("zoom", &z);
    orb_reply(idx == 0 && z >= 0 && z < RD_ZOOMS ? ZOOM[z] : "");
}
ORB_EXPORT("orb_opt_arm") void orb_opt_arm(int32_t armed) { (void)armed; }
ORB_EXPORT("orb_opt_sel") void orb_opt_sel(int32_t idx)
{
    if (idx != 0) return;
    int32_t z = 1;
    orb_cfg_get("zoom", &z);
    orb_cfg_set("zoom", (z + 1) % RD_ZOOMS);
}
