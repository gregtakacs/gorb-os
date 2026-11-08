/* The map's vector renderer (radar-plan.md §2.4): one .orbmap level drawn into a
 * 466x466 4-bit role raster at any radius and rotation, with a width and a dash
 * pattern per line role. Plain C with no ESP-IDF dependency, so the host suite
 * (tools/host_map_raster_test.py) compiles this same file.
 *
 * The file format is tools/orbmap.py's FORMAT 2 (FORMAT 1 read only the old roads). */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define ORB_MAP_W        466
/* The raster is 8x8-pixel tiles, row-major by tile, each tile 32 B: one S3 data-cache
 * line. Row-major pixels put every vertical step of a line on a new cache line, and the
 * 108 KB raster cannot stay in the 32 KB cache (R0, Bronze 2026-11-07). Inside a tile,
 * pixels are row-major, two a byte, high nibble first. */
#define ORB_MAP_TW       ((ORB_MAP_W + 7) / 8)                 /* 59 tiles a side */
#define ORB_MAP_RASTER_B (ORB_MAP_TW * ORB_MAP_TW * 32)        /* 111,392 B */
#define ORB_MAP_UNIT     16                                    /* file units per file pixel */

enum {
    ORB_MAP_WATER, ORB_MAP_LAND, ORB_MAP_URBAN, ORB_MAP_LAKE, ORB_MAP_RIVER,
    ORB_MAP_ROAD2, ORB_MAP_ROAD1, ORB_MAP_STATE, ORB_MAP_BORDER, ORB_MAP_COAST,
    ORB_MAP_CITY, ORB_MAP_AIRPORT, ORB_MAP_RUNWAY,
    ORB_MAP_ROAD3, ORB_MAP_ROAD4,     /* FORMAT 2: ROAD1 motorway and trunk, ROAD2 primary, these
                                       * secondary and tertiary (radar-plan §6.9) */
};
#define ORB_MAP_FORMAT 2
enum { ORB_MAP_DASH_SOLID, ORB_MAP_DASH_SHORT, ORB_MAP_DASH_LONG, ORB_MAP_DASH_DOT,
       ORB_MAP_DASH_DASHDOT, ORB_MAP_DASH_N };
enum { ORB_MAP_MARK_CITY, ORB_MAP_MARK_TOWN, ORB_MAP_MARK_VILLAGE, ORB_MAP_MARK_AIRPORT };
/* OS v1.2.38 (flights-map-plan §2.1), every one 0 for the old draw:
 *  - width[CITY], width[AIRPORT]: the mark's look, ORB_MAP_LOOK(shape, radius px 2-15);
 *    0 is the old 5 px dot and square;
 *  - dash[CITY], dash[AIRPORT]: the rank cap, the marks of rank above it not drawn (an
 *    airport with an IATA code ranks 1); 0 draws every rank;
 *  - dash[WATER], dash[URBAN], dash[LAKE]: the floor, a piece whose box is under 2 x that many
 *    px both ways not drawn (ponds); 0 draws every piece.
 *  - a line role's width, bits 3-7: its floor in 8 px steps, a piece under it both ways not
 *    drawn (the creeks the tiles call rivers); the width itself is bits 0-2. */
enum { ORB_MAP_SHAPE_DOT = 1, ORB_MAP_SHAPE_RING, ORB_MAP_SHAPE_DIAMOND, ORB_MAP_SHAPE_DIAMOND_LINE,
       ORB_MAP_SHAPE_SQUARE, ORB_MAP_SHAPE_SQUARE_LINE, ORB_MAP_SHAPE_TICKED };
#define ORB_MAP_LOOK(shape, r) ((uint8_t)((shape) | (r) << 3))

typedef struct {
    float    scale;           /* glass px per file px: 1 at the file's own zoom */
    float    rot_rad;         /* clockwise */
    uint16_t role_on;         /* bit per role; a role whose bit is clear is not drawn */
    uint8_t  mark_on;         /* bit per mark kind */
    uint8_t  width[16];       /* per line role, 1-4 px (0 = not drawn) */
    uint8_t  dash[16];        /* per line role, ORB_MAP_DASH_*; a fill role's is its floor and a
                               * mark role's its rank cap (below) */
    uint8_t  clip;            /* half the radius, px, past which nothing is drawn; 0 = none */
    int64_t (*clock_us)(void);/* optional: splits the stats' time into fills and lines */
    /* Optional: each landmark, whatever mark_on says, in glass pixels at this style's
     * transform (x, y may be off the glass). name is not NUL-terminated. */
    void   (*mark)(void *ctx, uint8_t kind, uint8_t rank, float x, float y, const char *name, uint8_t n);
    void    *mark_ctx;
} orb_map_style_t;

typedef struct {
    uint32_t features, fills, lines, edges, segments, marks, skipped;
    uint32_t fill_us, line_us;   /* with a clock: time in fills (parse included) and in lines */
    uint32_t max_fill_edges;     /* the largest single fill: what the scratch must hold */
    uint32_t small;              /* fills under their role's floor, not drawn (v1.2.38) */
} orb_map_stats_t;

enum {
    ORB_MAP_OK = 0,
    ORB_MAP_E_FORMAT = -1,    /* not an ORBM FORMAT 2 header */
    ORB_MAP_E_TRUNC = -2,     /* the body ends inside a record */
    ORB_MAP_E_CRC = -3,
};

/** Header and CRC check. Answers ORB_MAP_OK or an error; *zoom gets the file's zoom. */
int orb_map_check(const uint8_t *buf, size_t len, int *zoom);

/** Draw the file into raster (ORB_MAP_RASTER_B bytes). scratch holds one fill's edges,
 *  their order and active list (20 B an edge, after 1.9 KB of row counts); a fill with more edges than fit is skipped and counted. The CRC is
 *  not checked here (orb_map_check). */
int orb_map_render(const uint8_t *buf, size_t len, const orb_map_style_t *st,
                   uint8_t *raster, void *scratch, size_t scratch_len, orb_map_stats_t *stats);

/** A file read in pieces: read(ctx, off, dst, n) answers the bytes read (n, or the call
 *  failed). len is the file's length. */
typedef struct {
    int (*read)(void *ctx, uint32_t off, uint8_t *dst, uint32_t n);
    void *ctx;
    uint32_t len;
} orb_map_src_t;

/** The same draw from a source, through a window of win_len bytes (>= 512), with no file in
 *  memory (radar-plan §6.20: a level file larger than PSRAM's largest block). hdr (36 B)
 *  gets the header, for the caller's centre. The CRC is checked over the body as it streams:
 *  ORB_MAP_E_CRC means the raster drawn is not to be shown. */
int orb_map_render_src(const orb_map_src_t *src, uint8_t *hdr, uint8_t *win, size_t win_len,
                       const orb_map_style_t *st, uint8_t *raster, void *scratch, size_t scratch_len,
                       orb_map_stats_t *stats);

/** One raster pixel's role. */
static inline uint32_t orb_map_nib(int x, int y)   /* the pixel's nibble index */
{
    return (((uint32_t)(y >> 3) * ORB_MAP_TW + (uint32_t)(x >> 3)) << 6) | (uint32_t)((y & 7) << 3 | (x & 7));
}
static inline int orb_map_role_at(const uint8_t *raster, int x, int y)
{
    const uint32_t i = orb_map_nib(x, y);
    return (i & 1) ? raster[i >> 1] & 0x0F : raster[i >> 1] >> 4;
}
