/**
 * orb.h — the whole surface a gorb-os app may call (docs/app-platform-plan.md D15,
 * D20; the ABI is §43 of that file).
 *
 * An app is one wasm32 module carried in its .orbapp's code region. It links against
 * NOTHING but this header and apps/sdk/orb_rt.c: every OS service arrives as an import
 * from module "orb", bound by main/app/orb_app_wasm.c to the broker table, and every
 * import is gated by the caps the manifest declared — a call the owner did not grant
 * answers a refusal, exactly as it would for any other app. There is no libc, no LVGL,
 * no pointer into the OS: strings cross as NUL-terminated pointers into the app's own
 * linear memory, and the OS copies them.
 *
 * The OS calls the app back through the exports named under "Entry points" below.
 * Every one is optional except orb_init. They all run on the dial's one UI task, so an
 * entry point that loops is a frozen dial: do bounded work and return.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <math.h>        /* apps/sdk/libc: the C an app may use */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ORB_IMPORT(name) __attribute__((import_module("orb"), import_name(name)))
#define ORB_EXPORT(name) __attribute__((export_name(name)))

/* esp_err_t values the imports answer with (the OS's own numbers). */
#define ORB_OK              0
#define ORB_FAIL           -1
#define ORB_ERR_NO_MEM      0x101
#define ORB_ERR_INVALID_ARG 0x102
#define ORB_ERR_INVALID_STATE 0x103
#define ORB_ERR_INVALID_SIZE 0x104
#define ORB_ERR_NOT_FOUND   0x105
#define ORB_ERR_NOT_SUPPORTED 0x106
#define ORB_ERR_NOT_ALLOWED 0x10A

#define ORB_SCREEN_W 466
#define ORB_SCREEN_H 466

/* ---- time (free) -------------------------------------------------------------- */

/** Wall time: whole seconds into *sec, the fraction in nanoseconds into *frac_ns.
 *  Answers 1 when the clock has synced at least once, 0 while it is still a guess. */
ORB_IMPORT("now")        int32_t orb_now(int64_t *sec, int32_t *frac_ns);
/** Monotonic time since boot. */
ORB_IMPORT("uptime_ms")  uint32_t orb_uptime_ms(void);
ORB_IMPORT("uptime_us")  int64_t  orb_uptime_us(void);

/** Local broken-down time for `t` in the owner's zone. */
typedef struct {
    int32_t sec, min, hour, mday, mon, year, wday, yday, isdst;
} orb_tm_t;
ORB_IMPORT("localtime")  int32_t orb_localtime(int64_t t, orb_tm_t *out);
/** The zone's UTC offset at `t`, in minutes. */
ORB_IMPORT("tz_offset")  int32_t orb_tz_offset(int64_t t);

/* ---- the app's icon (free, §61) ------------------------------------------------- *
 * The picture the app carousel shows for an app with no theme: app.json's "icon" names a
 * square PNG in the app's directory, and orbsdk compiles it in as a deflated disc (232 px).
 * Hand it over from orb_init with ORB_ICON(); the OS keeps a copy. -1 means the OS would
 * not take the bytes. An app that names no icon must not use ORB_ICON (it will not link). */
ORB_IMPORT("icon")       int32_t orb_icon(const void *z, uint32_t n, uint32_t side);
extern const unsigned char orb_app_icon[];
extern const unsigned int  orb_app_icon_n, orb_app_icon_side;
#define ORB_ICON() orb_icon(orb_app_icon, orb_app_icon_n, orb_app_icon_side)

/* ---- logging (free) ----------------------------------------------------------- */

/** One line on the console, prefixed with the app's slug. */
ORB_IMPORT("log")        void orb_log(const char *line);

/* ---- settings (free: only the settings the manifest declared) ----------------- *
 * app.json's "settings" declares each one's type, label, range and choices; the OS
 * draws the dial's page and Orb Manager's from that and stores the values. An int,
 * number (scaled by 10^decimals), enum (the choice's index), multi (a bitmask) or
 * toggle reads with orb_cfg_get; a text or place with orb_cfg_get_str. A secret is
 * never readable: name it as orb_net_request's `cred` and the OS attaches it. */

ORB_IMPORT("cfg_get")     int32_t orb_cfg_get(const char *id, int32_t *out);
ORB_IMPORT("cfg_set")     int32_t orb_cfg_set(const char *id, int32_t v);
ORB_IMPORT("cfg_get_str") int32_t orb_cfg_get_str(const char *id, char *buf, uint32_t cap);
ORB_IMPORT("cfg_set_str") int32_t orb_cfg_set_str(const char *id, const char *v);
/** Moves every time any of this app's settings change, from the dial or the LAN. */
ORB_IMPORT("cfg_gen")     uint32_t orb_cfg_gen(void);

/* ---- the beat (free; the manifest's period_ms is the floor) ------------------- */

/** Ask for orb_tick every `period_ms` while the app is on the glass; 0 stops it. */
ORB_IMPORT("set_beat")   int32_t orb_set_beat(uint32_t period_ms);
/** Ask the OS to keep the dial's frame rate up for `ms` (an animation in flight). */
ORB_IMPORT("burst")      void orb_burst(uint32_t ms);

/* ---- widgets (free) ----------------------------------------------------------- *
 * Each app owns one full-screen page. Objects are handles > 0; parent 0 is the page.
 * A refused call answers 0 (create) or does nothing (set). */

enum { ORB_UI_BOX = 0, ORB_UI_LABEL = 1 };

/* Alignment, numbered as the OS's widget library numbers them. */
enum {
    ORB_AL_TOP_LEFT = 1, ORB_AL_TOP_MID, ORB_AL_TOP_RIGHT,
    ORB_AL_BOTTOM_LEFT, ORB_AL_BOTTOM_MID, ORB_AL_BOTTOM_RIGHT,
    ORB_AL_LEFT_MID, ORB_AL_RIGHT_MID, ORB_AL_CENTER
};

/* Properties for orb_ui_set. Colours are 0xRRGGBB. */
enum {
    ORB_P_X = 0, ORB_P_Y, ORB_P_W, ORB_P_H,
    ORB_P_BG,             /* background colour (also makes the background opaque) */
    ORB_P_BG_OPA,         /* 0..255 */
    ORB_P_RADIUS,         /* px; ORB_RADIUS_CIRCLE for a disc */
    ORB_P_BORDER_W, ORB_P_BORDER_COLOR,
    ORB_P_PAD,
    ORB_P_TEXT_COLOR,
    ORB_P_FONT,           /* ORB_FONT_* */
    ORB_P_TEXT_ALIGN,     /* 0 left, 1 centre, 2 right */
    ORB_P_HIDDEN,         /* 0/1 */
};
#define ORB_RADIUS_CIRCLE 0x7FFF

/* The system fonts (Montserrat, ASCII and LVGL's symbols): an app's text is 18 px, 14 the
 * small print, 48 the one big figure. OS v1.2.38: the 16 px font became 18 in its slot, so an
 * app built for 16 draws at 18. */
enum { ORB_FONT_14 = 0, ORB_FONT_18 = 1, ORB_FONT_48 = 2 };

ORB_IMPORT("ui_create")  int32_t orb_ui_create(int32_t kind, int32_t parent);
ORB_IMPORT("ui_set")     void    orb_ui_set(int32_t h, int32_t prop, int32_t v);
ORB_IMPORT("ui_text")    void    orb_ui_text(int32_t h, const char *s);
ORB_IMPORT("ui_align")   void    orb_ui_align(int32_t h, int32_t align, int32_t x, int32_t y);
/** Repaint the whole page on the next frame. */
ORB_IMPORT("ui_refresh") void    orb_ui_refresh(void);

/* ---- canvas (free): an OS-owned RGB565 surface with OS-drawn primitives -------- *
 * The pixel loops are the OS's (an interpreted pixel is a slow pixel); the app says
 * what to draw. Coordinates are canvas-local. */

ORB_IMPORT("cv_create")  int32_t orb_cv_create(int32_t parent, int32_t w, int32_t h);
ORB_IMPORT("cv_fill")    void orb_cv_fill(int32_t h, uint32_t rgb);
ORB_IMPORT("cv_pixel")   void orb_cv_pixel(int32_t h, int32_t x, int32_t y, uint32_t rgb);
/* h may be ORB_CV_LIVE0/1 for a ring (OS v1.2.33) as for a line (v1.2.32): opaque, onto the layer. */
ORB_IMPORT("cv_ring")    void orb_cv_ring(int32_t h, int32_t cx, int32_t cy, int32_t r, uint32_t rgb);
ORB_IMPORT("cv_disc")    void orb_cv_disc(int32_t h, int32_t cx, int32_t cy, int32_t r, uint32_t rgb);
/* h may be ORB_CV_LIVE0/1 (OS v1.2.32): a 2 px opaque line onto the live layer. */
ORB_IMPORT("cv_line")    void orb_cv_line(int32_t h, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                          uint32_t rgb);
/* The words in the box x .. x + w - 1 from y down, left-aligned; font | ORB_CV_CENTER or
 * | ORB_CV_RIGHT (OS v1.2.38) places them centred or flush right in it. The system fonts
 * carry ASCII alone. */
#define ORB_CV_CENTER 0x100
#define ORB_CV_RIGHT  0x200
ORB_IMPORT("cv_text")    void orb_cv_text(int32_t h, int32_t x, int32_t y, int32_t w, int32_t font,
                                          uint32_t rgb, const char *s);
/** Show what was drawn since the last flush. */
ORB_IMPORT("cv_flush")   void orb_cv_flush(int32_t h);

/** Tell the OS the app left (or entered) its armed state on its own — a third push
 *  that hands the knob back, say. The router reads it on its next poll. */
ORB_IMPORT("armed")      void orb_armed(int32_t armed);

/** Take every turn and press while on the glass, from the first click: no arming, and
 *  a hold still leaves the app (plan §51). Turns arrive accelerated, so |delta| > 1
 *  means a fast spin. Call once from orb_init. apps/sdk/orb_dial.h keeps the dial. */
ORB_IMPORT("knob_dial")  void orb_knob_dial(void);

/* ---- sound (cap "sound") ------------------------------------------------------- */

#define ORB_SOUND_LOOP   0x01u
#define ORB_SOUND_SECOND 0x02u
/** Play one of this app's own theme sounds by slot. Answers a handle > 0, or a
 *  negative error (not granted, slot empty, quiet hours). */
ORB_IMPORT("sound_play")   int32_t orb_sound_play(uint32_t slot, uint32_t flags, uint32_t delay_ms);
ORB_IMPORT("sound_stop")   void    orb_sound_stop(int32_t handle);
ORB_IMPORT("sound_active") int32_t orb_sound_active(int32_t handle);

/* ---- location (cap "location") ------------------------------------------------- */

typedef struct {
    int32_t  lat_e6, lon_e6;
    uint32_t gen;            /* moves on every change: re-read when it does */
    char     place[40];
} orb_home_t;
ORB_IMPORT("home")       int32_t orb_home(orb_home_t *out);
ORB_IMPORT("home_gen")   uint32_t orb_home_gen(void);

/* The owner's three places beside home (radar-plan §2.1), set on the hub's Orb tab: one
 * OS setting, so every app shows the same places. i is 0..2 and a slot keeps its number
 * when another is cleared. Answers ORB_OK, or an error for an empty slot or an i past 2.
 * The record is home's shape; `gen` is orb_places_gen(). Available from OS v1.2.19. */
typedef orb_home_t orb_place_t;
ORB_IMPORT("place")      int32_t orb_place(uint32_t i, orb_place_t *out);
ORB_IMPORT("places_gen") uint32_t orb_places_gen(void);

/* ---- units (free) -------------------------------------------------------------- *
 * The orb's units, one OS setting for every app (weather-plan §2.1): the owner sets
 * them on the dial's UNITS page or from the hub. Work in metric and convert when you
 * draw; re-read when orb_units_gen moves. Available from OS v1.2.10. */

enum { ORB_UNIT_C = 0, ORB_UNIT_F = 1 };                        /* temp  */
enum { ORB_UNIT_KMH = 0, ORB_UNIT_MPH = 1, ORB_UNIT_KN = 2 };   /* speed */
enum { ORB_UNIT_KM = 0, ORB_UNIT_MI = 1, ORB_UNIT_NM = 2 };     /* dist  */
enum { ORB_UNIT_24H = 0, ORB_UNIT_12H = 1 };                    /* time: OS v1.2.16 (0 before) */
typedef struct {
    uint8_t  temp, speed, dist, time;
    uint32_t gen;            /* moves on every change: re-read when it does */
} orb_units_t;
_Static_assert(sizeof(orb_units_t) == 8, "the OS writes this record verbatim");
ORB_IMPORT("units")      int32_t orb_units(orb_units_t *out);
ORB_IMPORT("units_gen")  uint32_t orb_units_gen(void);
/* Pressure's unit (OS v1.2.17): its own import, the record above having no byte to spare.
 * It moves orb_units_gen like the others. */
enum { ORB_UNIT_HPA = 0, ORB_UNIT_INHG = 1, ORB_UNIT_MMHG = 2 };   /* pressure */
ORB_IMPORT("units_pressure") int32_t orb_units_pressure(void);

static inline double orb_temp_from_c(double c, const orb_units_t *u)
{
    return u->temp == ORB_UNIT_F ? c * 9.0 / 5.0 + 32.0 : c;
}
static inline double orb_speed_from_kmh(double kmh, const orb_units_t *u)
{
    return u->speed == ORB_UNIT_MPH ? kmh / 1.609344 : u->speed == ORB_UNIT_KN ? kmh / 1.852 : kmh;
}
static inline double orb_dist_from_km(double km, const orb_units_t *u)
{
    return u->dist == ORB_UNIT_MI ? km / 1.609344 : u->dist == ORB_UNIT_NM ? km / 1.852 : km;
}
static inline const char *orb_speed_label(const orb_units_t *u)
{
    return u->speed == ORB_UNIT_MPH ? "mph" : u->speed == ORB_UNIT_KN ? "kn" : "km/h";
}
/* A pressure in hPa as the owner reads it, into `out` (at least 12 bytes): "1016 hPa",
 * "30.01 inHg" or "762 mmHg". Whole numbers only (no float formatting in a guest). */
static inline void orb_pressure_text(char *out, double hpa, int unit)
{
    char d[12];
    int i = 0, n = 0;
    long v = unit == ORB_UNIT_INHG ? (long)(hpa * 2.9529983 + 0.5)          /* hundredths */
           : unit == ORB_UNIT_MMHG ? (long)(hpa * 0.75006158 + 0.5) : (long)(hpa + 0.5);
    if (v < 0) v = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    if (unit == ORB_UNIT_INHG) while (n < 3) d[n++] = '0';
    while (n) { out[i++] = d[--n]; if (unit == ORB_UNIT_INHG && n == 2) out[i++] = '.'; }
    const char *l = unit == ORB_UNIT_INHG ? " inHg" : unit == ORB_UNIT_MMHG ? " mmHg" : " hPa";
    while (*l) out[i++] = *l++;
    out[i] = 0;
}
static inline const char *orb_dist_label(const orb_units_t *u)
{
    return u->dist == ORB_UNIT_MI ? "mi" : u->dist == ORB_UNIT_NM ? "nm" : "km";
}
/* A time of day in the orb's format: "14:05" (24-hour) or "2:05 PM" (12-hour). `out` holds
 * at least 9 bytes. The format is the owner's, set once for the orb (OS v1.2.16). */
static inline void orb_time_text(char *out, int hour, int min, const orb_units_t *u)
{
    int i = 0, h = hour;
    const int twelve = u->time == ORB_UNIT_12H;
    if (twelve) h = hour % 12 ? hour % 12 : 12;
    if (h >= 10 || !twelve) out[i++] = (char)('0' + h / 10);
    out[i++] = (char)('0' + h % 10);
    out[i++] = ':';
    out[i++] = (char)('0' + min / 10);
    out[i++] = (char)('0' + min % 10);
    if (twelve) { out[i++] = ' '; out[i++] = hour < 12 ? 'A' : 'P'; out[i++] = 'M'; }
    out[i] = 0;
}

/* ---- power (cap "power") ------------------------------------------------------- */

ORB_IMPORT("power_request") int32_t orb_power_request(uint32_t timeout_s, int32_t hold);

/* ---- network (cap "net") ------------------------------------------------------- *
 * The OS's HTTP and HTTPS client (D51): one request at a time per app, run off the UI
 * task behind the network gate. The body (any status's) lands in an OS buffer the app
 * reads in pieces — or tokenizes in place with orb_json_begin(NULL, 0). */

enum { ORB_GET = 0, ORB_POST, ORB_PUT, ORB_DELETE, ORB_HEAD };
enum { ORB_NET_IDLE = 0, ORB_NET_BUSY = 1, ORB_NET_DONE = 2, ORB_NET_FAILED = 3 };
typedef struct {
    int32_t  state;          /* ORB_NET_* */
    int32_t  status;         /* HTTP status, 0 on transport failure */
    uint32_t len;            /* body bytes held */
    uint32_t retry_after_s;  /* the response's Retry-After, 0 = none */
    uint32_t total_ms;
    int32_t  err;            /* the transport's error, ORB_OK when a status arrived */
} orb_net_info_t;

/** Start a request to a full http:// or https:// URL. `headers` is "Name: value" lines
 *  separated by \n (NULL for none; up to 255 B); `body` up to 16 KB (NULL for none).
 *  `cred` names one of the app's declared secret settings for the OS to attach (NULL
 *  for none): the app never sees its value, and the OS refuses the request unless the
 *  URL is https to the one host the manifest declared for it. Refused while a request
 *  is in flight. */
ORB_IMPORT("net_request") int32_t orb_net_request(uint32_t method, const char *url,
                                                  const char *headers, const void *body,
                                                  uint32_t body_len, const char *cred);
ORB_IMPORT("net_info")    int32_t orb_net_info(orb_net_info_t *out);
ORB_IMPORT("net_read")    int32_t orb_net_read(uint32_t off, void *buf, uint32_t cap);
/** One response header's value; answers its full length, or -1 when absent. */
ORB_IMPORT("net_header")  int32_t orb_net_header(const char *name, char *buf, uint32_t cap);
ORB_IMPORT("net_free")    void    orb_net_free(void);
/** 1 when the owner has set the app's secret setting `id`. */
ORB_IMPORT("net_cred")    int32_t orb_net_cred(const char *id);

static inline int32_t orb_net_get(const char *url, const char *cred)
{
    return orb_net_request(ORB_GET, url, NULL, NULL, 0, cred);
}

/* ---- kept bodies (cap "net"; radar-plan §2.3, OS v1.2.20) ------------------------- *
 * A landed response kept as an OS buffer in PSRAM, named by a slot 0-23: an app's own
 * memory cannot hold a radar's frames. 768 KB an app; the OS frees them 15 min after the
 * app leaves the glass. */
ORB_IMPORT("net_keep")  int32_t  orb_net_keep(uint32_t slot);   /* the landed body -> slot; ORB_OK */
ORB_IMPORT("blob_len")  uint32_t orb_blob_len(uint32_t slot);   /* 0 = empty or reclaimed */
ORB_IMPORT("blob_free") void     orb_blob_free(uint32_t slot);

/* An indexed PNG (colour type 3, 1-8 bits, not interlaced) from a kept body, drawn onto a
 * canvas (or ORB_CV_LIVE0/1) through the app's 256 ARGB words: alpha blends, 0 leaves the
 * canvas. The words are indexed by each pixel's VALUE, the grey its index names in the
 * file's palette, never by the index, which is the encoder's (LibreWXR's raw frames number
 * their greys afresh every frame; pal[v] colours dBZ v/2 - 32). The source window [sx, sy]
 * goes to (dx, dy), w x h glass pixels, each source pixel scale x scale. */
typedef struct {
    uint32_t slot;
    int32_t  sx, sy, dx, dy, w, h, scale;
    const uint32_t *pal;           /* 256 entries, 0xAARRGGBB */
} orb_png_draw_t;
/* scale | ORB_PNG_SMOOTH (OS v1.2.26): magnify by scale (up to 16), each glass pixel
 * sampled at its centre, sx + (x - dx + 0.5) / scale source pixels, from the four around
 * it, their values blended bilinearly before the palette colours them. dx may be negative
 * (the window's first source pixel can start left of the glass) for exact alignment. */
#define ORB_PNG_SMOOTH 0x100
ORB_IMPORT("cv_png8") int32_t orb_cv_png8(int32_t cv, const orb_png_draw_t *d);

/* ---- maps (cap "location"; radar-plan §2.4, OS v1.2.20) ---------------------------- *
 * The map of home (place 0) or a place (1-3), made by Orb Manager and drawn on the orb.
 * A view renders once in the background (orb_map_ready says when) into what each pixel
 * IS; orb_map_draw then colours it through the theme's 16 ARGB words every frame, which
 * costs no render. Two views an app: open the next, draw the old one until it is ready,
 * then close the old. Re-open when orb_maps_gen moves. */
enum {
    ORB_MAP_WATER, ORB_MAP_LAND, ORB_MAP_URBAN, ORB_MAP_LAKE, ORB_MAP_RIVER, ORB_MAP_ROAD2,
    ORB_MAP_ROAD1, ORB_MAP_STATE, ORB_MAP_BORDER, ORB_MAP_COAST, ORB_MAP_CITY, ORB_MAP_AIRPORT,
    ORB_MAP_RUNWAY,
    ORB_MAP_ROAD3, ORB_MAP_ROAD4,          /* the roles, 0-14. OS v1.2.26's maps (FORMAT 2): ROAD1
                                            * is motorway and trunk, ROAD2 primary, ROAD3
                                            * secondary, ROAD4 tertiary */
};
enum { ORB_MAP_DASH_SOLID, ORB_MAP_DASH_SHORT, ORB_MAP_DASH_LONG, ORB_MAP_DASH_DOT, ORB_MAP_DASH_DASHDOT };
enum { ORB_MARK_CITY, ORB_MARK_TOWN, ORB_MARK_VILLAGE, ORB_MARK_AIRPORT };
enum { ORB_MAP_ABSENT = -1, ORB_MAP_NOCARD = -2, ORB_MAP_BAD = -3, ORB_MAP_NOMEM = -4 };

typedef struct {
    int32_t  radius_m;             /* centre to the glass's edge */
    int32_t  rot_cdeg;             /* clockwise, hundredths of a degree */
    uint16_t role_on;              /* bit per role */
    uint8_t  mark_on;              /* bit per ORB_MARK_* drawn as a dot or a square */
    uint8_t  clip;                 /* OS v1.2.38: half the radius, px, past which nothing is drawn; 0 none */
    uint8_t  width[16];            /* per line role, 1-4 px; 0 = not drawn */
    uint8_t  dash[16];             /* per line role, ORB_MAP_DASH_* */
} orb_map_view_t;
/* OS v1.2.38 (flights-map-plan §2.1), each 0 for the old draw (older OSes ignore them):
 *  - width[ORB_MAP_CITY], width[ORB_MAP_AIRPORT]: the mark's look, ORB_MAP_LOOK(shape, radius
 *    2-15 px); 0 is the old 5 px dot and square;
 *  - dash[ORB_MAP_CITY], dash[ORB_MAP_AIRPORT]: the rank cap, marks ranked past it not drawn
 *    (an airport with an IATA code ranks 1); 0 draws every rank;
 *  - dash[ORB_MAP_WATER/URBAN/LAKE]: the floor, a piece under 2 x that many px both ways not
 *    drawn (ponds); 0 draws every piece.
 *  - a line role's width, bits 3-7: its floor in 8 px steps, a piece under it both ways not
 *    drawn (the creeks the tiles call rivers); the width itself is bits 0-2. */
enum { ORB_MAP_SHAPE_DOT = 1, ORB_MAP_SHAPE_RING, ORB_MAP_SHAPE_DIAMOND, ORB_MAP_SHAPE_DIAMOND_LINE,
       ORB_MAP_SHAPE_SQUARE, ORB_MAP_SHAPE_SQUARE_LINE, ORB_MAP_SHAPE_TICKED };
#define ORB_MAP_LOOK(shape, r) ((uint8_t)((shape) | (r) << 3))
_Static_assert(sizeof(orb_map_view_t) == 44, "the OS copies this view verbatim");

typedef struct {
    int16_t x, y;                  /* glass pixel; may be off the glass */
    uint8_t kind, rank;            /* ORB_MARK_*; lower rank is more important */
    char    name[24];
} orb_map_mark_t;
_Static_assert(sizeof(orb_map_mark_t) == 30, "the OS writes this mark verbatim");

ORB_IMPORT("map_open")    int32_t  orb_map_open(uint32_t place, const orb_map_view_t *v);  /* a handle, or <0 */
ORB_IMPORT("map_ready")   int32_t  orb_map_ready(int32_t h);      /* 1 ready, 0 rendering, <0 */
ORB_IMPORT("map_draw")    int32_t  orb_map_draw(int32_t cv, int32_t h, const uint32_t pal[16], int32_t dx, int32_t dy);
ORB_IMPORT("map_mark")    int32_t  orb_map_mark(int32_t h, uint32_t i, orb_map_mark_t *out);  /* best rank first */
ORB_IMPORT("map_project") int32_t  orb_map_project(int32_t h, int32_t lat_e6, int32_t lon_e6, int32_t *x, int32_t *y);
ORB_IMPORT("map_close")   void     orb_map_close(int32_t h);
ORB_IMPORT("maps_gen")    uint32_t orb_maps_gen(void);

/* ---- JSON (free): the OS's streaming tokenizer (D51) ----------------------------- *
 * Begin over the landed network body (buf NULL) or a buffer of the app's own (copied,
 * up to 16 KB); each orb_json_next answers one event with the member key it sits
 * under ("" inside an array), its depth and its value. orb_json_skip jumps past the
 * container the last OBJ/ARR event opened, natively. */

enum {
    ORB_JSON_END = 0, ORB_JSON_OBJ, ORB_JSON_OBJ_END, ORB_JSON_ARR, ORB_JSON_ARR_END,
    ORB_JSON_STR, ORB_JSON_NUM, ORB_JSON_TRUE, ORB_JSON_FALSE, ORB_JSON_NULL,
    ORB_JSON_ERROR = -1
};
typedef struct {
    int32_t type, depth;
    double  num;
    uint8_t key_trunc, str_trunc;
    char    key[32];
    char    str[96];
} orb_json_tok_t;
_Static_assert(sizeof(orb_json_tok_t) == 152, "the OS copies this token verbatim");

ORB_IMPORT("json_begin") int32_t orb_json_begin(const void *buf, uint32_t len);
ORB_IMPORT("json_next")  int32_t orb_json_next(orb_json_tok_t *t);
ORB_IMPORT("json_skip")  int32_t orb_json_skip(void);

/* ---- logging helper (apps/sdk/orb_rt.c) ------------------------------------- */

/** printf-style orb_log through a 160-byte line. */
void orb_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- entry points the OS calls (all optional except orb_init) ------------------ *
 *
 *   int32_t orb_init(void)              build the page; nonzero refuses the app
 *   void    orb_activate(void)          on the glass (after a switch to it)
 *   void    orb_suspend(void)           off the glass
 *   void    orb_resume(void)            back on the glass without a rebuild
 *   void    orb_show(void)              the page was just loaded: re-push state
 *   void    orb_tick(uint32_t now_ms)   the beat, while on the glass
 *   void    orb_service(void)           every service pass, on the glass or not
 *   void    orb_input(int32_t delta, int32_t pressed)   the knob, while armed
 *   void    orb_push(int32_t delta)     a press-and-turn: clicks turned with the knob held
 *                                       down (OS v1.2.19; radar-plan §2.10). Exporting it is
 *                                       the opt-in: without it the clicks arrive in orb_input
 *                                       as ordinary turns, and the press they began is spent.
 *                                       From OS v1.2.27, orb_push(0) once the knob is let go
 *                                       (400 ms on, so a slipping switch is not an end): act
 *                                       on the choice then, and let the clicks only show it
 *   int32_t orb_set_armed(int32_t armed) a press arms, a hold disarms; answer the
 *                                       armed state the app actually took
 *
 * An app has no options hooks: its settings page, on the dial and in Orb Manager, is
 * the OS's, drawn from the "settings" the manifest declares (apps/README.md, plan
 * §57). Read them with orb_cfg_get/orb_cfg_get_str, and re-read whenever orb_cfg_gen
 * moves — the owner changes them from either place while the app runs.
 */
