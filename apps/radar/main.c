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
enum { T_DOT = 0, T_HAND = 1, T_SWEEP = 2, T_TIME = 8, T_PLACE = 9, T_RANGE = 10, T_STATUS = 11, T_NAME0 = 12, T_ZOOM = 18 };
#define N_NAMES 6
#define N_ROWS 64

static orb_stage_info_t s_stage;
static bool  s_themed;
static int32_t R_sweep = -1;
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

static int s_floor;                /* "Lightest rain shown", 0-3 (rd_floor_dbz) */
static void load_settings(void)
{
    int32_t z = 1, f = 0;
    if (orb_cfg_get("zoom", &z) == ORB_OK && z >= 0 && z < RD_ZOOMS) s_zoom = (int)z;
    s_floor = orb_cfg_get("floor", &f) == ORB_OK && f >= 0 && f <= 3 ? (int)f : 0;
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
static uint32_t s_rain_gen;     /* bumped when the rain's pictures are void (RAIN_AGAIN) */
static int      s_npast, s_nnow;
static int64_t  s_list_at;                      /* wall s of the last good list */
static rd_list_t s_rd;
enum { NET_IDLE, NET_LIST, NET_FRAME };
static int      s_net = NET_IDLE, s_net_frame = -1;
static bool     s_parsing;
static int64_t  s_next_list_ms;
static uint32_t s_backoff_ms;
static int64_t  s_retry_ms;                     /* frames: not before this after a failure */

/* radar-plan §7 (OS v1.6.1): frames wait on the card. In PSRAM stay the frame on the glass, the
 * next two the loop or the dial will show, and the newest; the rest are parked as they land,
 * and loaded back two steps ahead. With no usable card (or an OS that cannot park) the old
 * rule stands: the oldest past frames go and the loop starts later. */
#define FRAME_MAX      (256u * 1024u)    /* the OS's body cap: the room a landing needs */
#define BLOB_BUDGET    (768u * 1024u)    /* the OS's kept bodies an app (PSRAM) */
static bool     s_card_ok = true;        /* parks are answered; false: the old rule */
static int64_t  s_card_retry_ms;         /* a refused park: asked again after this */

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
    s_net_frame = -1;                            /* a frame in flight was the old view's */
    for (int i = 0; i < RD_FRAMES; i++) {
        if (F[i].slot >= 0) orb_blob_free((uint32_t)F[i].slot);
        F[i].slot = -1;
        F[i].etag[0] = 0;
        F[i].recheck = F[i].failed = false;
    }
}
static int idx_newest(void) { return s_npast - 1; }

static int fstate(const frame_t *f) { return f && f->slot >= 0 ? (int)orb_blob_state((uint32_t)f->slot) : ORB_BLOB_EMPTY; }
static bool fready(const frame_t *f) { return fstate(f) == ORB_BLOB_RAM; }

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
    s_rain_gen++;                       /* the indices name other frames now */
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
static bool s_merged;              /* the two layers composed as one (§6.19) */

/* §7.4: the PSRAM the frames hold (a load counts from its ask, a park until it is written),
 * the biggest frame yet, and whether every frame in PSRAM is one the window keeps. */
static uint32_t s_biggest;
static uint32_t ram_used(void)
{
    uint32_t u = 0;
    for (int k = 0; k < s_npast + s_nnow; k++) {
        const int st = fstate(&F[k]);
        if (st == ORB_BLOB_RAM || st == ORB_BLOB_MOVING) u += orb_blob_len((uint32_t)F[k].slot);
    }
    return u;
}
static bool s_parkable;            /* frames_tend: a frame in PSRAM the window does not keep */

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
        /* §7.4: room for the landing first; the parks frames_tend asked for free it */
        const uint32_t need = s_biggest ? s_biggest + s_biggest / 4 : FRAME_MAX;
        if (s_card_ok && s_parkable && ram_used() + need > BLOB_BUDGET) return;
        const where_t *w = here();
        char url[200], hdr[48] = "";
        if (!rd_frame_url(url, sizeof url, s_source, F[i].path, rd_img_side(s_zoom), rd_img_zurl(s_zoom), w->lat_e6, w->lon_e6,
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
    int32_t kept = s < 0 ? ORB_ERR_NO_MEM : orb_net_keep((uint32_t)s);
    if (kept == ORB_ERR_NO_MEM && s >= 0 && s_card_ok) {
        /* §7.4: with the card, room comes from parking, never from giving frames up: the
         * frame is asked again once the parks have freed it */
        s_retry_ms = up_ms() + 2000;
        net_done();
        return;
    }
    /* The newest past frame and the nowcast make room: the oldest past frames held go,
     * and the loop starts later. Refused here, a new list's newest frame backed off for
     * up to 10 minutes, and a place flipped meanwhile sat at "Loading radar" (owner,
     * Bronze, 2026-11-08). */
    for (int k = 0; kept == ORB_ERR_NO_MEM && s >= 0 && (f->now || s_net_frame == idx_newest()) && k < idx_newest(); k++) {
        if (F[k].slot < 0) continue;
        orb_blob_free((uint32_t)F[k].slot);
        F[k].slot = -1;
        F[k].failed = true;
        kept = orb_net_keep((uint32_t)s);
    }
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
    if (ni.len > s_biggest) s_biggest = ni.len;
    char tag[24];
    const int32_t tn = orb_net_header("ETag", tag, sizeof tag);
    if (tn > 0 && tn < (int32_t)sizeof tag) memcpy(f->etag, tag, (size_t)tn + 1); else f->etag[0] = 0;
    s_backoff_ms = 0;
    s_rain_dirty = s_ui_dirty = true;
    s_rain_gen++;                                        /* a picture drawn ahead of it is void */
    net_done();
}

/* ---- the map view (§2.4) --------------------------------------------------------------- */

static int32_t s_view = 0, s_view_next = 0;     /* handles; 0 = none */
static bool    s_map_dirty, s_map_absent;
static int64_t s_map_retry_ms;                  /* a map that failed for want of memory: again */
static int     s_shown = -2;                   /* the frame on the glass, -1 none */
static bool s_visible;                 /* between on_glass and orb_suspend */
/* What drew the rain moved (a view, a zoom, the palette, the units, the place, the list, a
 * frame's body): the glass's rain is drawn again, and a picture drawn ahead is void. */
#define RAIN_AGAIN(v) (s_shown = (v), s_rain_gen++)

/* No map on the card (or no card) is "No map yet", asked again every 20 s (a card busy
 * under a hub store reads as absent too); anything else (memory, a read) is tried again
 * in 3 s, the old view kept up meanwhile. */
static void map_failed(int32_t why)
{
    s_view_next = 0;
    if (why == ORB_MAP_OFF_GLASS) return;               /* not now: on_glass opens it */
    const bool absent = why == ORB_MAP_ABSENT || why == ORB_MAP_NOCARD;
    if (absent) { s_map_absent = true; s_map_dirty = true; }
    s_map_retry_ms = up_ms() + (absent ? 20000 : 3000);
    orb_logf("radar: the map did not open (%d), again in %d s", (int)why, absent ? 20 : 3);
}

static void view_open(void)
{
    const where_t *w = here();
    /* off the glass no map is rendered (the card is the glass's): on_glass opens it */
    if (!w || !s_visible) return;
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
    RAIN_AGAIN(-2);                    /* the rain again, at the new view's magnification */
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
static bool    s_layers;
static bool    s_play = true;                  /* untouched: the loop plays */
static int     s_loop = 0;                     /* the loop's frame, an index into F */
static int64_t s_loop_ms, s_zoom_said_ms;
/* A press's new place, said at once in the large pill until its map is up (owner,
 * 2026-11-08: seconds with no sign the press took, so a second press went one further). */
static int64_t s_flip_ms;
static bool    s_pill_up;          /* the large pill is on the glass: its end must redraw */
#define FLIP_SAY_MS 1200
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

static bool s_back_open;
/* §6.28: the back's picture is drawn ahead (the next frame the dial turns toward), shown only
 * if asked for; the frame and the rain generation it was drawn for, and what it drew (-1 none) */
static bool     s_back_spec;
static int      s_back_frame = -1, s_back_drawn = -1;
static uint32_t s_back_gen;
static int      s_scrub_dir = -1;                /* the way the dial last turned: -1 back in time */
static int32_t  s_scrub_pos;                     /* the dial's position the last frame (0 playing) */
static void back_close(void) { s_back_open = s_back_spec = false; s_back_frame = -1; }
static void layers_open(void)
{
    if (!s_themed) return;
    back_close();                      /* a merge reallocates the layers and drops their backs */
    const int32_t a = orb_stage_live(0, V.map_order, 0), b = orb_stage_live(1, V.rain_order, 0);
    s_layers = a == ORB_OK && b == ORB_OK;
    if (!s_layers) orb_logf("radar: the live layers refused (%d, %d)", (int)a, (int)b);
    /* §6.19: one covering layer, the map's copy with the rain on it, is a copy for the
     * compositor where two were a blend. Only a map that covers (its water and land
     * opaque) can stand alone; else the two layers stay as they were. */
    s_merged = s_layers && (V.map_pal[0] >> 24) == 0xFF && (V.map_pal[1] >> 24) == 0xFF &&
               orb_stage_live_merge(1) == ORB_OK;
    s_map_dirty = s_rain_dirty = s_ui_dirty = true;
    RAIN_AGAIN(-2);
}

/* What the theme wants under the rain, drawn onto the map once a view (§6.19). */
static void draw_under(void)
{
    if (!(V.under_argb >> 24)) return;
    const uint32_t c = V.under_argb & 0xFFFFFF;
    for (int k = 1; k <= V.under_rings; k++) {
        const int r = 232 * k / V.under_rings;
        for (int w = 0; w < V.under_w; w++) orb_cv_ring(ORB_CV_LIVE0, CX, CY, r - w, c);
    }
    if (V.under_cross)
        for (int w = 0; w < V.under_w; w += 2) {
            orb_cv_line(ORB_CV_LIVE0, CX - 232, CY + w, CX + 232, CY + w, c);
            orb_cv_line(ORB_CV_LIVE0, CX + w, CY - 232, CX + w, CY + 232, c);
        }
}

static void draw_map(void)
{
    s_map_dirty = false;
    if (!s_layers) return;
    orb_stage_live(0, V.map_order, 0);                   /* clear: the plate shows */
    if (s_view > 0) orb_map_draw(ORB_CV_LIVE0, s_view, V.map_pal, 0, 0);
    draw_under();
    if (s_merged) { RAIN_AGAIN(-2); s_rain_dirty = true; }  /* the rain's layer is the map's copy */
}

/* The scale bar under its length (owner, 2026-11-08), on the rain's layer so no rain
 * covers it: ink on a dark edge, end ticks up. */
#define SCALE_Y   440               /* the bar's top */
static int32_t s_tgt = ORB_CV_LIVE1;  /* where the bar goes: the rain's layer, or the bare canvas */
static void band(int x0, int x1, int y0, int y1, uint32_t argb)   /* rows y0..y1, the 2 px pen */
{
    if (!(argb >> 24)) return;
    for (int y = y0; y <= y1; y += 2) orb_cv_line(s_tgt, x0, y > y1 - 1 ? y1 - 1 : y, x1, y > y1 - 1 ? y1 - 1 : y, argb & 0xFFFFFF);
}
static void draw_scale(void)
{
    const where_t *w = here();
    if (!w) return;
    int px;
    if (V.scale_ring) {                  /* the mock-up's: a ring at the round length, labelled by draw_ui */
        (void)rd_scale(rd_radius_m(s_zoom, w->lat_e6), s_units.dist, 200, &px);
        orb_cv_ring(s_tgt, CX, CY, px, V.scale_ink & 0xFFFFFF);
        return;
    }
    (void)rd_scale(rd_radius_m(s_zoom, w->lat_e6), s_units.dist, V.scale_max, &px);
    const int x0 = CX - px / 2, x1 = x0 + px, t = V.scale_tick, h = V.scale_w;
    /* the edge a pixel round the bar and the ticks, then the ink over it */
    band(x0 - 2, x1 + 1, SCALE_Y - 1, SCALE_Y + h, V.scale_edge);
    if (t) {
        band(x0 - 2, x0 + 1, SCALE_Y - t - 1, SCALE_Y, V.scale_edge);
        band(x1 - 2, x1 + 1, SCALE_Y - t - 1, SCALE_Y, V.scale_edge);
    }
    band(x0, x1 - 1, SCALE_Y, SCALE_Y + h - 1, V.scale_ink);
    if (t) {
        orb_cv_line(s_tgt, x0, SCALE_Y - t, x0, SCALE_Y, V.scale_ink & 0xFFFFFF);
        orb_cv_line(s_tgt, x1 - 2, SCALE_Y - t, x1 - 2, SCALE_Y, V.scale_ink & 0xFFFFFF);
    }
}

/* The theme's rain colours with the owner's floor: values under it draw nothing. */
static uint32_t s_pal[256], s_pal_fc[256];      /* observed, and a nowcast frame's (V.forecast) */
static void pal_build(void)
{
    const int v0 = rd_floor_value(s_floor);
    for (int i = 0; i < 256; i++) {
        const uint32_t c = i < v0 ? 0 : V.rain[i];
        s_pal[i] = c;
        if (V.forecast == RD_FC_DIM) s_pal_fc[i] = ((c >> 25) << 24) | (c & 0xFFFFFF);   /* half the opacity */
        else if (V.forecast == RD_FC_DESAT) {                                             /* seven parts grey */
            const uint32_t r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, b = c & 0xFF, l = (r * 77 + g * 150 + b * 29) >> 8;
            s_pal_fc[i] = (c & 0xFF000000u) | ((r + 7 * l) >> 3) << 16 | ((g + 7 * l) >> 3) << 8 | ((b + 7 * l) >> 3);
        } else s_pal_fc[i] = c;
    }
}

/* radar-plan §6.27 (OS v1.2.38): the next rain picture is drawn into the layer's back copy
 * by the OS's worker, off the frame, and presented whole when it is done, so the sweep keeps
 * its pace (Bronze: a decode in the call held the tick ~117 ms, the sweep fell to 9 fps). */
/* s_back_open: a back picture is being drawn (declared by layers_open). One begins with the
 * map copied in and the frame drawn over it, queued on the OS's worker. */
static bool back_begin(int i, bool spec)
{
    if (orb_stage_live_back(1, s_merged ? 0 : -1, 0) != ORB_OK) return false;   /* the worker is busy */
    s_back_open = true;
    s_back_spec = spec;
    s_back_frame = i;
    s_back_gen = s_rain_gen;
    s_back_drawn = -1;
    s_tgt = ORB_CV_BACK1;
    const frame_t *f = i >= 0 ? &F[i] : NULL;
    if (!f || f->slot < 0 || !fready(f)) { draw_scale(); return true; }
    const int k = rd_img_k(s_zoom);
    orb_png_draw_t d = { .slot = (uint32_t)f->slot, .sx = s_sx, .sy = s_sy, .dx = s_dx, .dy = s_dy,
                         .w = RD_GLASS, .h = RD_GLASS, .scale = k > 1 ? (k | ORB_PNG_SMOOTH) : 1, .pal = f->now ? s_pal_fc : s_pal };
    if (f->now && V.forecast == RD_FC_HATCH) d.scale |= ORB_PNG_HATCH;   /* a forecast never looks observed */
    const int32_t rc = orb_cv_png8(ORB_CV_BACK1, &d);
    draw_scale();
    if (rc == ORB_OK) s_back_drawn = i;
    else orb_logf("radar: frame %d (slot %d, %u B) did not draw: %d", i, (int)f->slot, (unsigned)orb_blob_len((uint32_t)f->slot), (int)rc);
    return true;
}

static void back_step(void)
{
    if (!s_back_open) return;
    if (s_back_spec) {                                   /* drawn ahead: compared, never shown unasked */
        (void)orb_stage_live_present(1 | ORB_LIVE_PREPARE);
        return;
    }
    /* §6.28: a scrub's step is the owner's, and goes at once; the loop is paced by its blips */
    const int32_t r = orb_stage_live_present(1 | (s_play ? 0 : ORB_LIVE_NOW));
    if (r == 0) return;                                  /* still being drawn, compared or paced */
    s_back_open = false;
    if (r < 0) { orb_logf("radar: the rain's back picture did not present (%d)", (int)r); RAIN_AGAIN(-2); s_rain_dirty = true; return; }
    s_shown = s_back_drawn;
    /* §6.28: scrubbing, the next frame the way the dial turns is drawn ahead into the idle back,
     * so the next detent that way is one blip (the decode and the compare already made) */
    const int n = s_shown + s_scrub_dir;
    if (!s_play && s_shown >= 0 && n >= 0 && n < s_npast + s_nnow && fready(&F[n]))
        (void)back_begin(n, true);
}

static void draw_rain(void)
{
    if (s_view_next > 0) return;       /* a new view is rendering: the old map and rain stay up */
    if (!s_layers) { s_rain_dirty = false; return; }
    const int i = frame_wanted();
    const frame_t *f = i >= 0 ? &F[i] : NULL;
    /* the way the dial turns, from the dial itself: out of the loop, the jump from the loop's
     * frame to the dial's says nothing about the direction */
    if (s_play) s_scrub_pos = 0;
    else if (s_dial.pos != s_scrub_pos) { s_scrub_dir = s_dial.pos > s_scrub_pos ? 1 : -1; s_scrub_pos = s_dial.pos; }
    if (s_back_open && s_back_frame == i && s_back_gen == s_rain_gen) {
        s_back_spec = false;           /* drawn ahead, or being drawn: back_step shows it */
        s_rain_dirty = false;
        return;
    }
    if (s_shown == i && f && f->slot >= 0) {
        s_rain_dirty = false;
        if (s_back_open) s_back_spec = true;   /* the dial came back: what is in flight is not asked for */
        return;
    }
    /* §7: on the card, being loaded: the rain on the glass stays until it is in PSRAM */
    if (f && f->slot >= 0 && !fready(f)) {
        if (fstate(f) == ORB_BLOB_CARD) (void)orb_blob_load((uint32_t)f->slot);
        s_rain_dirty = true;
        return;
    }
    /* the map, then the rain over it (merged), or a clear layer: begun in the back copy, over
     * whatever it held; refused while the worker still draws, so asked again next frame */
    if (!back_begin(i, false)) { s_rain_dirty = true; return; }
    s_rain_dirty = false;
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
        const int w = 13 * (int)strlen(m.name) + 12, x = m.x, y = m.y + 22;   /* 26 px words */
        const int dx = x - CX, dy = y - CY;
        if (dx * dx + dy * dy > 185 * 185) continue;               /* the rim's, not the map's */
        if (dx * dx + dy * dy < 40 * 40) continue;                 /* the place's dot */
        if (y < 130 || y > 330) continue;                          /* the place, time, status and range */
        bool hit = false;
        for (int j = 0; j < n && !hit; j++)
            hit = (x - bx[j] < (w + bw[j]) / 2 && bx[j] - x < (w + bw[j]) / 2) && (y - by[j] < 32 && by[j] - y < 32);
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
    char pl[40];
    snprintf(pl, sizeof pl, "%s", w ? w->name : "");
    if (V.caps) for (char *c = pl; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);   /* ASCII alone */
    say(R_place, CX, V.place_y, pl, 0);
    int rpx = 0;
    if (w) {                                              /* the scale's round length */
        const int len = rd_scale(rd_radius_m(s_zoom, w->lat_e6), s_units.dist, V.scale_ring ? 200 : V.scale_max, &rpx);
        snprintf(b, sizeof b, "%d %s", len, s_units.dist == 1 ? "mi" : s_units.dist == 2 ? "nm" : "km");
    } else b[0] = 0;
    /* the bar's length under it; the ring's on the ring, up and to the right */
    if (V.scale_ring) say(R_range, CX + rpx * 7071 / 10000 + 30, CY - rpx * 7071 / 10000 - 6, b, 0);
    else say(R_range, CX, 414, b, 0);
    const int i = frame_wanted();
    if (i >= 0) {
        const int min = (int)((F[i].t - F[idx_newest()].t) / 60);
        rd_time_text(b, sizeof b, min);
        say(R_time, CX, V.time_y, b, F[i].now ? 1 : 0);           /* a forecast reads as one */
        /* the cursor: round, Now at the top and 1.5 degrees a minute; bottom (the mock-up's),
         * Now at the bottom and half a degree a minute, the past to its left */
        if (R_hand >= 0) orb_stage_pose(R_hand, V.track_bottom ? 1800 - min * 5 : min * 15, 0, 0);
    } else {
        hide(R_time);
        hide(R_hand);
    }
    const char *st = "";
    /* the zoom, large, while it changes (owner, 2026-11-07: the small word was lost in the
     * map); a theme without the large row says it on the status line */
    const bool zooming = s_zoom_want >= 0 || up_ms() - s_zoom_said_ms < 2000;
    const int zs = s_zoom_want >= 0 ? s_zoom_want : s_zoom;
    s_pill_up = R_zoom >= 0 && w && (zooming || s_flip_ms);
    if (R_zoom >= 0 && s_flip_ms && w && !zooming) {
        say(R_zoom, CX, CY + 62, w->name, 0);
    } else if (R_zoom >= 0 && zooming && w) {
        char zb[40];
        int px;                                         /* the scale bar's round length there */
        const int len = rd_scale(rd_radius_m(zs, w->lat_e6), s_units.dist, V.scale_max, &px);
        snprintf(zb, sizeof zb, "%s  %d %s", ZOOM[zs], len, s_units.dist == 1 ? "mi" : s_units.dist == 2 ? "nm" : "km");
        say(R_zoom, CX, CY + 62, zb, 0);
    } else {
        hide(R_zoom);
    }
    const int want = frame_wanted();
    if (!w) st = "Set home on the hub";
    else if (zooming && R_zoom < 0) st = ZOOM[zs];
    else if (s_map_absent) st = "No map yet";
    else if (s_view_next > 0 || s_map_retry_ms) st = "Loading map";
    else if (!s_npast) st = s_net != NET_IDLE ? "Loading radar" : "No radar yet";
    else if (want >= 0 && F[want].slot < 0 && !F[want].failed) st = "Loading radar";
    else if (s_list_at && now_s() - s_list_at > STALE_S) {
        snprintf(b, sizeof b, "updated %d min ago", (int)((now_s() - s_list_at) / 60));
        st = b;
    }
    else if (want >= 0 && F[want].now && V.track_bottom) st = "forecast";   /* the mock-up's word under the time */
    say(R_status, CX, V.status_y, st, 0);
    if (s_flip_ms) for (int k = 0; k < N_NAMES; k++) hide(R_name[k]);   /* the old map's */
    else draw_names();
}

/* A new place or zoom. The frames go with a place, or with a zoom that fetches another
 * image zoom; Local to Close share Local's frames, magnified (RD_IMG_ZMAX). */
static void window_set(void)
{
    const where_t *w = here();
    if (!w) return;
    const int k = rd_img_k(s_zoom);
    s_dx = s_dy = 0;
    if (k > 1) rd_window_k(rd_img_z(s_zoom), k, rd_img_side(s_zoom), w->lat_e6, w->lon_e6, w->lat_e6, w->lon_e6, &s_sx, &s_sy, &s_dx, &s_dy);
    else rd_window(rd_img_z(s_zoom), w->lat_e6, w->lon_e6, w->lat_e6, w->lon_e6, &s_sx, &s_sy);
}

static void view_changed(bool frames)
{
    if (frames) {
        frames_drop();
        s_retry_ms = 0;                          /* a new place asks at once */
        s_backoff_ms = 0;
        s_play = true;
        s_loop = 0;
        orb_dial_home(&s_dial);
    }
    RAIN_AGAIN(-2);
    window_set();
    s_map_absent = false;
    s_map_retry_ms = 0;
    view_open();
    s_rain_dirty = s_ui_dirty = true;
}

static void place_changed(void)
{
    view_changed(true);
    if (s_visible && s_merged) orb_stage_live_copy(1, 0);   /* the old place's rain goes at once */
    else if (s_visible && s_layers) orb_stage_live(1, V.rain_order, 0);   /* no layers off the glass */
    RAIN_AGAIN(-1);
}

/* The loop (§2.5): oldest held past frame to Now, 333 ms a step (§7.1: 3 a second, a card
 * read's time with headroom; was 250), Now held 2 s. */
static void loop_step(void)
{
    if (!s_play || !s_npast || s_zoom_want >= 0) return;   /* choosing a zoom: it holds */
    /* the last step's rain is not on the glass yet: the loop waits for it, so a storm's heavy
     * frames step it more slowly rather than skip (the OS paces each swap by its blip) */
    if (s_back_open && !s_back_spec) return;
    orb_burst(1000);               /* a theme with no moving rows ticks once a second: keep 4 */
    const int64_t t = up_ms();
    const int hold = s_loop == idx_newest() ? 2000 : 333;
    if (t - s_loop_ms < hold) return;
    int next = s_loop >= idx_newest() ? 0 : s_loop + 1;
    while (next < idx_newest() && F[next].slot < 0) next++;    /* skip what has not landed */
    /* §7.4: a frame on the card waits for its load (asked two steps ahead) rather than skip */
    if (next != s_loop && F[next].slot >= 0 && !fready(&F[next])) {
        if (fstate(&F[next]) == ORB_BLOB_CARD) (void)orb_blob_load((uint32_t)F[next].slot);
        return;
    }
    s_loop_ms = t;
    if (next != s_loop) { s_loop = next; s_rain_dirty = s_ui_dirty = true; }
}

/* §7.4: the frames to keep in PSRAM: the one wanted, the next `ahead` the loop (or the dial,
 * the way it turns) will show, and the newest. */
static bool in_window(int k, int cur, int ahead)
{
    if (k == cur || (ahead > 1 && k == idx_newest())) return true;
    const int n = s_npast + s_nnow;
    for (int a = 1, j = cur; a <= ahead && j >= 0; a++) {
        j = s_play ? (j >= idx_newest() ? 0 : j + 1) : j + s_scrub_dir;
        if (j < 0 || j >= n) break;
        if (j == k) return true;
    }
    return false;
}

/* §7.4, once a service pass: what the window keeps is loaded, the rest parked. A window too big
 * for the 768 KB with room for a landing keeps the wanted frame and the next alone. */
static void frames_tend(void)
{
    s_parkable = false;
    if (!s_npast) return;
    if (!s_card_ok) {
        if (up_ms() < s_card_retry_ms) return;
        s_card_ok = true;
    }
    const int cur = frame_wanted();
    uint32_t win = 0;
    for (int k = 0; k < s_npast + s_nnow; k++)
        if (F[k].slot >= 0 && in_window(k, cur, 2)) win += orb_blob_len((uint32_t)F[k].slot);
    const int ahead = win + (s_biggest ? s_biggest : FRAME_MAX) > BLOB_BUDGET ? 1 : 2;
    int calls = 0;
    for (int k = 0; k < s_npast + s_nnow && calls < 4; k++) {
        const int st = fstate(&F[k]);
        if (st != ORB_BLOB_RAM && st != ORB_BLOB_CARD) continue;
        const bool keep = in_window(k, cur, ahead);
        if (keep && st == ORB_BLOB_CARD) {
            calls++;
            (void)orb_blob_load((uint32_t)F[k].slot);   /* NO_MEM: the parks below free it */
        } else if (!keep && st == ORB_BLOB_RAM) {
            s_parkable = true;
            calls++;
            if (orb_blob_park((uint32_t)F[k].slot) == ORB_ERR_NOT_SUPPORTED) {
                s_card_ok = false;
                s_card_retry_ms = up_ms() + 60000;
                orb_logf("radar: no card to keep frames on: the oldest go for room (asked again in 60 s)");
                return;
            }
        }
    }
}

/* ---- the bare form: no theme bound (app-platform-plan §26: every app draws its base
 * form from the OS's primitives) ---------------------------------------------------------
 * One canvas, composed whole whenever anything moved: the ground, the OS's map in the
 * radar's own colours (rd_values_default), the rain frame through the same ramp and floor,
 * a ring and the rim, home's dot, the scale bar, and the words in the system fonts (18 px,
 * 48 px for the time and the zoom), names folded to ASCII (the system fonts carry no
 * accents). Same knob, same loop, same fetches as on a theme. */
#define BARE_GROUND 0x06140C            /* Scope's phosphor, as the default values' map is */
#define BARE_INK    0xB4FFD2
#define BARE_QUIET  0x5FBF8A
#define BARE_NEXT   0xE0B040            /* a forecast frame's time */
#define BARE_RING   0x299057
static int32_t s_cv;                    /* the bare canvas; 0 on a theme */
static void bare_text(int y, int w, int font, uint32_t rgb, const char *s)
{
    char a[64];
    rd_ascii(a, sizeof a, s);
    orb_cv_text(s_cv, CX - w / 2, y, w, font | ORB_CV_CENTER, rgb, a);
}

static void bare_names(void)
{
    int n = 0;
    int16_t bx[N_NAMES], by[N_NAMES], bw[N_NAMES];
    for (uint32_t k = 0; s_view > 0 && n < V.names && n < N_NAMES && k < 32; k++) {
        orb_map_mark_t m;
        if (orb_map_mark(s_view, k, &m) != 1) break;
        char a[32];
        const int len = (int)rd_ascii(a, sizeof a, m.name);
        const int w = 11 * len + 12, x = m.x, y = m.y + 6;      /* 18 px words, under the mark */
        const int dx = x - CX, dy = y - CY;
        if (dx * dx + dy * dy > 185 * 185 || dx * dx + dy * dy < 40 * 40) continue;
        if (y < 130 || y > 330) continue;                         /* the time, the status, the scale */
        bool hit = false;
        for (int j = 0; j < n && !hit; j++)
            hit = (x - bx[j] < (w + bw[j]) / 2 && bx[j] - x < (w + bw[j]) / 2) && (y - by[j] < 24 && by[j] - y < 24);
        if (hit) continue;
        bx[n] = (int16_t)x; by[n] = (int16_t)y; bw[n] = (int16_t)w;
        orb_cv_text(s_cv, x - w / 2, y, w, ORB_FONT_18 | ORB_CV_CENTER, BARE_QUIET, a);
        n++;
    }
}

static void bare_draw(void)
{
    s_map_dirty = s_rain_dirty = s_ui_dirty = false;
    const where_t *w = here();
    char b[48];
    orb_cv_fill(s_cv, BARE_GROUND);
    if (s_view > 0) orb_map_draw(s_cv, s_view, V.map_pal, 0, 0);
    const int i = frame_wanted();
    const frame_t *f = i >= 0 ? &F[i] : NULL;
    s_shown = -1;
    if (fready(f)) {
        const int k = rd_img_k(s_zoom);
        orb_png_draw_t d = { .slot = (uint32_t)f->slot, .sx = s_sx, .sy = s_sy, .dx = s_dx, .dy = s_dy,
                             .w = RD_GLASS, .h = RD_GLASS, .scale = k > 1 ? (k | ORB_PNG_SMOOTH) : 1, .pal = s_pal };
        if (orb_cv_png8(s_cv, &d) == ORB_OK) s_shown = i;
    }
    orb_cv_ring(s_cv, CX, CY, 116, BARE_RING);
    orb_cv_ring(s_cv, CX, CY, 231, BARE_RING);
    orb_cv_ring(s_cv, CX, CY, 230, BARE_RING);
    if (!s_flip_ms) bare_names();
    orb_cv_disc(s_cv, CX, CY, 6, BARE_NEXT);
    orb_cv_disc(s_cv, CX, CY, 4, 0xFFFFFF);
    s_tgt = s_cv;
    draw_scale();
    if (w) {
        int px;
        const int len = rd_scale(rd_radius_m(s_zoom, w->lat_e6), s_units.dist, V.scale_max, &px);
        snprintf(b, sizeof b, "%d %s", len, s_units.dist == 1 ? "mi" : s_units.dist == 2 ? "nm" : "km");
        bare_text(410, 160, ORB_FONT_18, BARE_INK, b);
        bare_text(36, 300, ORB_FONT_18, BARE_QUIET, w->name);
    }
    if (i >= 0) {
        rd_time_text(b, sizeof b, (int)((F[i].t - F[idx_newest()].t) / 60));
        bare_text(62, 320, ORB_FONT_48, F[i].now ? BARE_NEXT : BARE_INK, b);
    }
    /* the zoom while it changes, or a press's new place: large, on a dark band */
    const bool zooming = s_zoom_want >= 0 || up_ms() - s_zoom_said_ms < 2000;
    const int zs = s_zoom_want >= 0 ? s_zoom_want : s_zoom;
    s_pill_up = w && (zooming || s_flip_ms);
    if (s_pill_up) {
        if (s_flip_ms && !zooming) snprintf(b, sizeof b, "%s", w->name);
        else snprintf(b, sizeof b, "%s", ZOOM[zs]);
        for (int y = CY + 36; y < CY + 92; y += 2) orb_cv_line(s_cv, CX - 190, y, CX + 190, y, 0x05090D);
        bare_text(CY + 40, 370, ORB_FONT_48, BARE_INK, b);
    }
    const char *st = "";
    if (!w) st = "Set home on the hub";
    else if (s_map_absent) st = "No map yet";
    else if (s_view_next > 0 || s_map_retry_ms) st = "Loading map";
    else if (!s_npast) st = s_net != NET_IDLE ? "Loading radar" : "No radar yet";
    else if (i >= 0 && F[i].slot < 0 && !F[i].failed) st = "Loading radar";
    else if (s_list_at && now_s() - s_list_at > STALE_S) {
        snprintf(b, sizeof b, "updated %d min ago", (int)((now_s() - s_list_at) / 60));
        st = b;
    }
    if (st[0]) bare_text(352, 300, ORB_FONT_18, BARE_QUIET, st);
    orb_cv_flush(s_cv);
}

static void redraw(void)
{
    if (!s_visible) return;
    if (!s_themed) {
        const int i = frame_wanted();
        const frame_t *f = i >= 0 ? &F[i] : NULL;
        if (f && f->slot >= 0 && !fready(f)) {            /* §7: the picture stays while it loads */
            if (fstate(f) == ORB_BLOB_CARD) (void)orb_blob_load((uint32_t)f->slot);
            return;
        }
        if (s_cv && (s_map_dirty || s_rain_dirty || s_ui_dirty)) bare_draw();
        return;
    }
    if (!s_layers) layers_open();
    back_step();
    if (s_map_dirty) draw_map();
    if (s_rain_dirty) draw_rain();
    if (s_back_open && !s_back_spec) back_step();   /* §6.28: a picture drawn ahead goes this frame */
    /* the words and the track's cursor change with the rain they name, in the swap's own
     * frame, never ahead of it (owner, 2026-11-08: the time read half -40, half -50) */
    if (s_ui_dirty && !(s_back_open && !s_back_spec)) draw_ui();
}

/* ---- entry points -------------------------------------------------------------------- */

static void find_rows(void)
{
    R_dot = R_hand = R_time = R_place = R_range = R_status = R_zoom = R_sweep = -1;
    for (int k = 0; k < N_NAMES; k++) R_name[k] = -1;
    for (int32_t i = 0; i < (int32_t)s_stage.n_rows && i < N_ROWS; i++) {
        clk_row_t r;
        if (orb_stage_row(i, &r) != ORB_OK) continue;
        const int t = r.tag;
        if (t == T_DOT) R_dot = i;
        else if (t == T_HAND) R_hand = i;
        else if (t == T_SWEEP) R_sweep = i;
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
    else if (n == RD_VALUES_V4_B && ver == 4) memcpy(&V, &got, RD_VALUES_V4_B);
    else if (n == RD_VALUES_V3_B && ver == 3) memcpy(&V, &got, RD_VALUES_V3_B);   /* nothing under the rain */
    else if (n == RD_VALUES_V2_B && ver == 2) memcpy(&V, &got, RD_VALUES_V2_B);   /* the scale's defaults stay */
    else if (n) orb_logf("radar: the theme's values are layout %u, %u B: the defaults draw", (unsigned)ver, (unsigned)n);
    if (n && ver < 5 && (n == RD_VALUES_V4_B || n == RD_VALUES_V3_B || n == RD_VALUES_V2_B)) {
        /* a theme from before the mock-up's look: its glass has the round track and no ring */
        V.track_bottom = 0; V.forecast = RD_FC_NONE; V.scale_ring = 0; V.caps = 0;
        V.place_y = 58; V.time_y = 98; V.status_y = 362; V.sweep_s = 0;
    }
    if (V.place_y > 440 || V.time_y > 440 || V.status_y > 440) { V.place_y = 58; V.time_y = 98; V.status_y = 362; }
    if (V.sweep_s > 30) V.sweep_s = 30;
    if (V.names > N_NAMES) V.names = N_NAMES;
    if (V.scale_w < 2) V.scale_w = 2;
    if (V.scale_w > 8) V.scale_w = 8;
    if (V.scale_tick > 24) V.scale_tick = 24;
    if (V.scale_max < 60) V.scale_max = 60;
    if (V.scale_max > 200) V.scale_max = 200;
    if (V.under_rings > 6) V.under_rings = 6;
    if (V.under_w < 1) V.under_w = 1;
    if (V.under_w > 3) V.under_w = 3;
    pal_build();
}

/* The stage, read afresh: at a boot the theme may not be bound while the app builds, and
 * a theme may be swapped or fail to bind (PSRAM ran short: Bronze, 2026-11-07), so every
 * arrival asks, and the service asks again every 2 s while it has none. No widget stands in
 * for it: a widget on the page, hidden or not, took the dial off its fast path for good
 * (orb_stage.c's veto), and the hub installs the shelf's theme with the app. */
static int64_t s_stage_retry_ms;
static void stage_read(bool may_bare)
{
    const bool was = s_themed;
    s_themed = orb_stage_open(&s_stage) == ORB_OK && !s_stage.base;
    if (s_themed) find_rows();
    if (s_themed && R_time < 0) s_themed = false;      /* not this app's layout */
    read_values();
    /* no theme: the bare form, for good (a theme's store reboots the orb, which starts the
     * app again on it); its loop steps every 250 ms. Never at orb_init or an arrival: the OS
     * binds the stage after them, and the canvas, a widget, keeps the stage off for good
     * (Bronze, 2026-11-08: after a reboot the radar stood bare over its Scope theme). The
     * service's next ask decides. */
    if (!s_themed && !s_cv && may_bare) {
        s_cv = orb_cv_create(0, CX * 2, CY * 2);
        orb_logf("radar: no theme: the bare form%s", s_cv ? "" : ", but its canvas was refused");
    }
    /* Bare, the loop steps from orb_service, which runs many times a second: a beat is
     * never under the manifest's 1000 ms floor (D16), and a 250 ms one, refused, left the
     * loop standing on its first frame (owner, 2026-11-08). The beat keeps the age line. */
    const uint32_t beat = s_themed ? 0 : s_cv ? 1000 : 2000;
    if (orb_set_beat(beat) != ORB_OK) orb_logf("radar: the OS refused a %u ms beat", (unsigned)beat);
    if (was != s_themed) orb_logf("radar: %s", s_themed ? "on its theme" : "no theme bound yet: asking again");
    s_stage_retry_ms = up_ms() + (may_bare ? 2000 : 300);
}

ORB_EXPORT("orb_init") int32_t orb_init(void)
{
    orb_knob_dial();
    for (int i = 0; i < RD_FRAMES; i++) F[i].slot = -1;
    stage_read(false);
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
    back_close();                      /* and their back copies with them */
    if (!s_cv) stage_read(false);      /* the bare form stays bare */
    s_map_dirty = s_rain_dirty = s_ui_dirty = true;
    /* the OS closed the map's views when the app left (no display buffers off the glass,
     * OS v1.2.27): a new one, the frames kept unless nothing has been shown yet */
    for (int i = 0; i < RD_FRAMES; i++)              /* the OS frees kept bodies 15 min off the glass */
        if (F[i].slot >= 0 && !orb_blob_len((uint32_t)F[i].slot)) { F[i].slot = -1; F[i].etag[0] = 0; }
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
ORB_EXPORT("orb_suspend") void orb_suspend(void) { s_visible = false; s_layers = false; back_close(); }

ORB_EXPORT("orb_tick") void orb_tick(uint32_t now)
{
    (void)now;
    if (R_sweep >= 0 && V.sweep_s && s_themed && s_visible) {      /* Scope's sweep: one turn a sweep_s */
        /* radar-plan §7.8 (owner, 2026-11-09): the arm turns slower while the OS paces its
         * frames slower (a storm's steps), by the theme's frame over the frame now, so it moves
         * as far a frame as at the theme's pace: smoother, and less of the glass to draw. The
         * angle is integrated, so a change of speed never jumps it. */
        static int64_t s_sw_ms;
        static int64_t s_sw_a;                                       /* milli-tenths of a degree */
        const int64_t t = up_ms(), per = (int64_t)V.sweep_s * 1000;
        int64_t dt = s_sw_ms ? t - s_sw_ms : 0;
        if (dt < 0 || dt > 500) dt = 0;                              /* back on the glass: as it was */
        s_sw_ms = t;
        const int64_t now_ms = orb_frame_ms(0), own_ms = orb_frame_ms(1);
        const int64_t k = now_ms > own_ms && own_ms > 0 ? own_ms * 1000 / now_ms : 1000;   /* permille */
        s_sw_a = (s_sw_a + dt * 3600 * 1000 / per * k / 1000) % 3600000;
        orb_stage_pose(R_sweep, (int32_t)(s_sw_a / 1000), 0, 0);
    }
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
        const int fl = s_floor;
        load_settings();
        if (fl != s_floor) { pal_build(); s_rain_dirty = true; RAIN_AGAIN(-2); }
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
    if (orb_units_gen() != s_units_gen) {               /* the scale's unit: the bar too */
        orb_units(&s_units); s_units_gen = orb_units_gen();
        s_ui_dirty = s_rain_dirty = true; RAIN_AGAIN(-2);
    }
    if (moved) place_changed();
    /* The scrub reaches back only as far as frames the loop can show: past ones given up
     * for room are no stop on the dial (owner: blank frames behind the loop's start). */
    /* Held, not merely asked for: a zoom out drops the frames, and a reach kept from the
     * zoom before let the dial back to -110 min before they came, or where they never
     * would (owner, 2026-11-08). The reach grows as frames land. */
    int oldest = idx_newest();
    if (s_npast && F[oldest].slot >= 0) while (oldest > 0 && F[oldest - 1].slot >= 0) oldest--;
    int ahead = 0;
    while (ahead < s_nnow && F[s_npast + ahead].slot >= 0) ahead++;
    orb_dial_range(&s_dial, -(s_npast > 0 ? idx_newest() - oldest : 0), ahead);
    if (orb_dial_idle(&s_dial, up_ms()) || (!s_play && !s_dial.pos && up_ms() - s_dial.last_ms > ORB_DIAL_IDLE_MS)) {
        s_play = true;                                   /* §51: a glance plays the loop */
        s_ui_dirty = s_rain_dirty = true;
    }
    zoom_step();
    if (s_visible && s_cv) loop_step();                  /* bare: the loop's clock is the service */
    if (s_visible && !s_themed && !s_cv && up_ms() >= s_stage_retry_ms) { stage_read(true); s_layers = false; s_map_dirty = s_rain_dirty = s_ui_dirty = true; }
    if (s_map_retry_ms && up_ms() >= s_map_retry_ms) { s_map_retry_ms = 0; view_open(); }
    view_step();
    /* The zoom's pill goes 2 s after it lands. Playing, the loop's next frame redrew it away;
     * scrubbed, nothing did, and it stayed until the loop played again (owner, 2026-11-08). */
    if (s_pill_up && s_zoom_want < 0 && !s_flip_ms && up_ms() - s_zoom_said_ms >= 2000) s_ui_dirty = true;
    if (s_flip_ms && ((!s_view_next && up_ms() - s_flip_ms > FLIP_SAY_MS) || up_ms() - s_flip_ms > 10000)) {
        s_flip_ms = 0;                                   /* the new map is up: the pill goes */
        s_ui_dirty = true;
    }
    if (s_visible) frames_tend();                        /* §7.4: off the glass the OS frees them */
    net_step(s_visible && s_zoom_want < 0);             /* nothing fetched while choosing */
    if (s_visible) redraw();
}

ORB_EXPORT("orb_set_armed") int32_t orb_set_armed(int32_t armed)
{
    if (!armed) { orb_dial_home(&s_dial); s_play = true; s_rain_dirty = s_ui_dirty = true; }
    return armed;
}

/* Turn: through time, the loop stopped. Press: the next place, at the same step. Either
 * while a zoom is still being chosen means its end went missing: the zoom lands, and the
 * press is that gesture's let-go, not the next place. */
ORB_EXPORT("orb_input") void orb_input(int32_t delta, int32_t pressed)
{
    if (s_zoom_want >= 0) {
        orb_logf("radar: %s while choosing: the zoom lands", pressed ? "a press" : "a turn");
        s_zoom_click_ms = 0;
        zoom_step();
        pressed = 0;
    }
    if (pressed && s_nwhere > 1) {
        s_sel = (s_sel + 1) % s_nwhere;
        s_flip_ms = up_ms();
        s_ui_dirty = true;
        place_changed();
        orb_burst(1500);
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
        const bool frames = rd_img_zurl(z) != rd_img_zurl(s_zoom);
        s_zoom = z;
        view_changed(frames);
    }
}
