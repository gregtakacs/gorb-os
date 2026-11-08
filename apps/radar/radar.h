/* radar.h — the radar app's model (docs/radar-plan.md §2.2, §2.5): LibreWXR's frame list,
 * the point images' URLs and where they sit against the OS's map, the zoom ladder, the
 * words. No imports: tools/host_radar_test.py compiles radar.c as it stands, beside the
 * OS's own tokenizer (main/app/orb_json.h, whose token is the SDK's byte for byte). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __wasm__
#include "orb.h"          /* orb_json_tok_t, ORB_JSON_* */
#else
#include "orb_json.h"
#endif

#define RD_PAST      12           /* LibreWXR's past frames, 600 s apart */
#define RD_NOWCAST    6           /* and its nowcast */
#define RD_FRAMES    (RD_PAST + RD_NOWCAST)
#define RD_STEP_S   600
#define RD_PATH_MAX  40           /* "/v2/radar/1794083400" and room */
#define RD_ZOOMS      3           /* Wide, Region, Local: the map's z6, z7, z8 */
#define RD_MAP_Z0     6
#define RD_IMG        512         /* a point image's side */
#define RD_GLASS      466

typedef struct {
    int64_t t;                    /* the frame's time, unix s */
    char    path[RD_PATH_MAX];
} rd_frame_t;

typedef struct {
    rd_frame_t past[RD_PAST], now[RD_NOWCAST];
    int        n_past, n_now;
    /* the reader */
    int        in;                /* 0 elsewhere, 1 radar.past, 2 radar.nowcast */
    bool       in_radar, started, error;
    rd_frame_t cur;
} rd_list_t;

void rd_list_begin(rd_list_t *l);
/* One token of /public/weather-maps.json. False ends the read (done or broken). */
bool rd_list_token(rd_list_t *l, const orb_json_tok_t *t);
/* A list the radar can show: at least one past frame, in time order, 600 s apart. */
bool rd_list_ok(const rd_list_t *l);

/* `source` (no trailing slash) + the frame's path + "/512/{z}/{lat}/{lon}/255/0_0.png",
 * lat and lon to four decimals, rounded half away from zero. Answers the length, 0 when it
 * does not fit. */
size_t rd_frame_url(char *buf, size_t cap, const char *source, const char *path, int z,
                    int32_t lat_e6, int32_t lon_e6);
size_t rd_list_url(char *buf, size_t cap, const char *source);

/* Python's round (ties to even), the rule LibreWXR's window origin rounds by. */
double rd_round_even(double v);
/* The natural log (the guest's libm has none); for the Mercator y. */
double rd_ln(double x);
/* Ten-thousandths of a degree, as the URL carries them and the server centres on. */
int32_t rd_e4(int32_t e6);

/* The draw window of a point image at image zoom z (512 px tiles), fetched for
 * (img_lat_e6, img_lon_e6) at four decimals, against the OS's map centred on
 * (map_lat_e6, map_lon_e6) at the same scale: the source pixel the glass's left and top
 * edge start at. The server's origin is round(px - 256) (LibreWXR window_origin), its y
 * clamped to the world. */
void rd_window(int z, int32_t img_lat_e6, int32_t img_lon_e6, int32_t map_lat_e6,
               int32_t map_lon_e6, int32_t *sx, int32_t *sy);

/* The zoom ladder: step 0 (Wide) is the map's z6; the image is fetched one zoom lower. */
static inline int rd_map_z(int step) { return RD_MAP_Z0 + step; }
static inline int rd_img_z(int step) { return RD_MAP_Z0 - 1 + step; }
/* The view's radius, centre to the glass's edge, so the map draws at its level's own scale. */
int32_t rd_radius_m(int step, int32_t lat_e6);

/* "Now", "-40 min", "+30 min": minutes from the newest past frame. */
void rd_time_text(char *buf, size_t cap, int minutes);
/* The range, centre to edge, in the orb's distance unit (0 km, 1 mi, 2 nm): "120 km". */
void rd_range_text(char *buf, size_t cap, int32_t radius_m, int unit);

/* ---- the theme's values (§2.11), layout version 1 ------------------------------------ */
#define RD_VALUES_VER 1
typedef struct {
    uint32_t map_pal[16];         /* ARGB per map role (orb_map_view's roles) */
    uint8_t  width[16];           /* line width per role, 0-4 */
    uint8_t  dash[16];            /* dash pattern per line role */
    uint16_t role_on;             /* bit per role drawn */
    uint8_t  mark_on;             /* bit per landmark kind drawn as a dot */
    uint8_t  names;               /* landmark names shown, 0-6 */
    uint8_t  map_order, rain_order;   /* the two live layers' depths, on the rows' scale */
    uint8_t  pad[2];
    uint32_t rain[256];           /* ARGB per raw value; dBZ = value / 2 - 32 */
} rd_values_t;
_Static_assert(sizeof(rd_values_t) == 64 + 16 + 16 + 4 + 4 + 1024, "the editor writes this layout");
/* The defaults a theme without values (or of another layout) draws with. */
void rd_values_default(rd_values_t *v);
