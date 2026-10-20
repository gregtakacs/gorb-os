/* clk_types.h — the analog dial's geometry types.
 *
 * §26 (P6.5) retired the FORMAT 2 dialect and the compiled assets/clock_assets
 * pair with it. These structs were generated into that header by
 * tools/bake_clock.py and are the runtime geometry every clock draws into —
 * FORMAT 3's reader (main/art/orb_clock3.c) projects a bundle's element table
 * straight into a clk_theme_t, and clock_view.c reads one. The types outlive the
 * FORMAT 2 pair that first defined them, so they now live here, hand-authored.
 *
 * No generated data lives here: no clk_theme instance, no pixel-plane arrays, no
 * CLK_BLOB_BYTES. The dial's geometry comes from a resident container — the
 * flashed slot-A default bundle or an installed one — never from a compiled blob.
 * CLK_SCR_W/H are the panel's own resolution, fixed for the device.
 */
#pragma once

#include <stdint.h>

#define CLK_SCR_W 466
#define CLK_SCR_H 466

typedef struct {
  uint16_t w, h;            /* cropped ink size */
  int16_t  offx, offy;      /* crop origin in the original canvas */
  int16_t  pivotx, pivoty;  /* rotation pivot in CROPPED coords */
  int16_t  centerx, centery;/* screen point the pivot maps to */
  uint8_t  blend;           /* 0 normal 1 multiply 2 screen */
  uint8_t  show;            /* draw this layer */
  uint32_t color_off;       /* blob offset of the colour plane (w*h*2);
                             *   the alpha plane follows at +w*h*2, stride w */
  int16_t  diffx, diffy;    /* tourbillon frames only: rect that differs
                             *   from the PREVIOUS frame, CROPPED coords;
                             *   0,0 = identical to its predecessor */
  uint16_t diffw, diffh;
} clk_sprite_t;

#define CLK_TOURB_MAX 16

typedef struct {
  uint32_t bg;
  uint8_t  plate_follow;    /* 0 none 1 hour 2 minute 3 second */
  uint8_t  shadow_on;
  int8_t   shadow_dx, shadow_dy;
  uint8_t  second_sweep, second_railway, text_over_hands, tick_rate;
  uint16_t bg_frames;       /* distinct extra plate frames (0 = still) */
  int16_t  bg_fps; uint8_t bg_loop; uint16_t bg_every;
  uint32_t plate_off;       /* full-screen RGB565 colour plane offset */
  uint8_t  order[5]; uint8_t order_n;
  uint8_t  matte_mask;      /* bit i = hand slot i folded into the
                             *   background at bake time, never
                             *   composited at runtime; bit 5 = the
                             *   glass overlay was folded too */
  clk_sprite_t hand[5];
  clk_sprite_t shadow[5];
  clk_sprite_t overlay;
  uint8_t  bg_mode;         /* 0 = plate + bgAnim frames, 1 = animated delta bank */
  uint32_t bg_key_off;      /* mode 1: full RGB565 keyframe (frame 0) */
  uint16_t gif_frames;      /* mode 1: frames in the loop (incl. keyframe) */
  uint16_t tourbillon_n;    /* second-hand frame bank size (0 = plain hand) */
  uint16_t tourbillon_fps;  /* bank playback tempo */
  clk_sprite_t tourbillon[CLK_TOURB_MAX];  /* shown frame's pixels replace hand[2]'s */
  /* D32 ruling C, appended at the END so a positional initializer list
   * (tools/bake_clock.py, and every .h it has ever printed) still lands field
   * for field: a pair baked before this line carries 0, which is what it means
   * — no start orientation, a plain bank at the seconds angle. */
  uint16_t tourbillon_start;/* the carriage row's start orientation, fixed-point
                             *   100ths of a degree, added to the seconds angle */
} clk_theme_t;

typedef struct {
  uint16_t x, y, w, h;  /* screen rect */
  uint32_t off;         /* blob offset of the RGB565 payload */
} clk_bg_rect_t;

/* ---- complication records -------------------------------------------------
 *
 * Moved here from orb_theme.h by D32 slice 2: the FORMAT 3 projection
 * (main/art/orb_clock3.c) fills these from a bundle's BANK rows, and that file
 * compiles on a host with nothing but this header and orb_bundle.h. The types
 * are the dial's geometry like every struct above them; orb_theme.h includes
 * this one, so the FORMAT 2 loader and clock_view.c see the same declarations.
 *
 * A theme asks for a complication by shipping one of these per aperture, ring or
 * indicator, and the paint path interprets it. Every FORMAT 2 field is one u32
 * word on the wire; the FORMAT 3 projection fills a subset, named per field.
 * The lists that must stay in step are tools/theme_schema.py, tools/orbtheme.py
 * and here; tools/host_theme_test.py packs through the Python and reads back
 * through the lifted C.
 */
enum {
    CLK_KIND_WINDOW = 0,   /* one whole-window tile per value (day/month/date/moon) */
    CLK_KIND_STRIP  = 1,   /* value composed from per-glyph sprites + edge pieces    */
    CLK_KIND_RING   = 2,   /* count sprites on a circle, ring rotated by value      */
    CLK_KIND_FLARE  = 3,   /* one sprite picked by value, drawn with a blend        */
    CLK_KIND__MAX
};

/* What drives a comp's value. Every one derives from the wall clock and the
 * battery state the view already has, so a new theme feature needs no new input
 * path and no new firmware build. */
enum {
    CLK_SRC_NONE = 0, CLK_SRC_WEEKDAY, CLK_SRC_MONTH, CLK_SRC_DATE,
    CLK_SRC_HOUR12, CLK_SRC_HOUR24, CLK_SRC_MINUTE, CLK_SRC_SECOND,
    CLK_SRC_MOON_AGE, CLK_SRC_BATTERY, CLK_SRC_POWER_RESERVE, CLK_SRC_TEXT,
    CLK_SRC_AMPM, CLK_SRC_WEEK, CLK_SRC_DAY_OF_YEAR, CLK_SRC_QUARTER,
    CLK_SRC_VALUE,
    CLK_SRC__MAX
};

enum {
    CLK_FLAG_UNDER_DIAL = 1 << 0,   /* tile pixels already include the dial art */
    CLK_FLAG_ZERO_PAD   = 1 << 1,   /* strip: pad the number with the zero glyph */
    CLK_FLAG_REVERSE    = 1 << 2,   /* ring/strip: walk frames the other way */
    CLK_FLAG_JUMP       = 1 << 3,   /* aux hand: snap to whole units */
    CLK_FLAG_24H        = 1 << 4,   /* aux hand: hour value is 0..23 */
    CLK_FLAG_WRAP       = 1 << 5,   /* aux hand: keep sweeping past value_max */
    CLK_FLAG_BLINK      = 1 << 6,   /* comp: draw every other second */
    CLK_FLAG_DISABLED   = 1 << 7,   /* parsed and validated, deliberately not drawn */
    CLK_FLAG_AUX_SHADOW = 1 << 8    /* aux row: the blur/offset TWIN of the row
                                     * before it, same tag+src. It owns pixels and
                                     * a centre (the hand's, moved by the light
                                     * vector) and no input of its own: the driver
                                     * rotates it by the previous row's angle, so
                                     * a pointer over its plate gets a shadow
                                     * without a second record's worth of state.
                                     * Mirrors theme_schema.FLAG_AUX_SHADOW. */
};

#define CLK_STRIP_GLYPHS_MAX 8   /* glyph image objects per strip, and the cap a
                                  * strip's `count` may reach. Mirrors
                                  * theme_schema.CLK_STRIP_GLYPHS, which is what
                                  * the bake packed against. */
#define CLK_COMP_MAX  16   /* complication records      — theme_schema.CLK_COMP_MAX  */
#define CLK_AUX_MAX    8   /* extra hands beyond the 5  — theme_schema.CLK_AUX_MAX   */
#define CLK_BANK_MAX 256   /* shared bank sprites       — theme_schema.CLK_BANK_MAX  */
_Static_assert(CLK_SRC__MAX == 17, "SOURCES tuple of theme_schema.py grew; the "
                                   "bake and this reader must agree on its length");

/* One window tile is w*h*3 bytes: the raw colour plane (w*h*2, stride w*2)
 * with its planar alpha behind it (w*h, stride w). The bake packs tiles RAW —
 * never composited against the dial — and the paint path lays the selected
 * tile UNDER the live dial layer, so the dial's own window frame composites
 * over the tile's edge the way a real bezel covers the disc beneath it. That
 * runtime composite is the shipped policy. CLK_FLAG_UNDER_DIAL is the wire's
 * historical other case (an opaque tile the bake had composited against the
 * dial, w*h*2, no alpha plane): accepted by the reader so an older container
 * still parses, but no current bake emits it and clock_view.c declines to
 * draw one — an opaque square would bury the bezel it is supposed to sit
 * under. Must agree with theme_schema.window_bytes(), which is what the bake
 * checked on the way out. */
#define CLK_WINDOW_BYTES(c) \
    ((uint32_t)(c)->w * (uint32_t)(c)->h * \
     (((c)->flags & CLK_FLAG_UNDER_DIAL) ? 2u : 3u))

/* A WINDOW's `first` is a z bucket, not a bank index: the theme layer slot
 * (tools/bake_clock.py KINDS — 0 hour, 1 minute, 2 second, 3 static1/dial,
 * 4 static2/sub-dial plates) whose live layer this tile must stay UNDER. 0
 * means "below every live layer", which is every bake from before a sub-dial
 * ever hosted a window, so an old container keeps drawing exactly as it did.
 * The kit's own drawOrder is what decides it — the perpetual-calendar kit lists
 * its moon between the dial and the c4 plate, and a moon drawn under the dial
 * is a moon that never reaches the glass. clock_view.c honours the bucket in
 * comp_add_jobs, and the orb_theme.c bank-range guard covers only the kinds
 * that address the bank, so a bucket never reads as a sprite index.
 *
 * A FORMAT 3 BANK row fills one of these too: the projection takes the box, the
 * kind, the src and the z bucket (the row's `order`) from the table, the tile
 * base and stride from the sprite catalogue, and the domain from the clock —
 * never the row. The ring, strip and reserve fields belong to the FORMAT 2
 * manifest dialect and a FORMAT 3 projection leaves them zero. */
typedef struct {
    uint16_t tag;              /* manifest comp id (c2 -> 2); FORMAT 3: the slot */
    uint8_t  kind;             /* CLK_KIND_*  */
    uint8_t  src;              /* CLK_SRC_*   */
    int16_t  x, y;             /* window/strip box origin */
    uint16_t w, h;             /* box, one glyph, or one ring slot */
    uint16_t count;            /* frames across the value domain */
    uint16_t first;            /* bank index for strip/ring/flare; for a
                                * WINDOW the z bucket, see above  */
    uint32_t off;              /* window: blob offset of tile 0 */
    uint32_t frame_bytes;      /* window: stride between tiles */

    int16_t  cx, cy;           /* ring centre */
    int16_t  radius;           /* ring radius, centre to sprite centre */
    int16_t  angle0;           /* tenths of a degree, 0 = twelve o'clock */
    int16_t  angle_step;       /* tenths of a degree between slots */
    int16_t  value_min;        /* the value frame 0 stands for */
    uint16_t scale;            /* frames per unit of value, x16 */
    int16_t  glyph_step;       /* strip: px between glyph origins */
    int16_t  glyph_lead;       /* strip: px from box edge to first glyph */
    uint16_t edge_left;        /* strip: left window piece, bank index, 0xFFFF none */
    uint16_t edge_right;       /* strip: right piece, 0xFFFF none */
    uint16_t flags;            /* CLK_FLAG_*  */
    uint8_t  blend;            /* 0 normal 1 multiply 2 screen */
    uint8_t  show;             /* draw it */
    uint32_t reserve;          /* must be 0: checked at load, so it stays free */
} clk_comp_t;

/* One extra hand: a baked sprite plus how it is driven. The sub-dial hour /
 * hour+min / minute / second / 24h hands, the battery reserve and a retrograde
 * sweep are all one record here, so none of them costs one of the five
 * compiled-in hand slots or a new field in the generated header. */
typedef struct {
    clk_sprite_t spr;          /* w,h,off,pivot,centre,blend,show,color_off */
    uint16_t tag;              /* manifest hand index, or the comp tag it belongs to */
    uint8_t  src;              /* CLK_SRC_*   */
    uint8_t  kind;             /* 0 rotating hand, 1 retrograde, 2 indexed pointer */
    uint16_t host;             /* 0 = main dial centre, else a comp tag */
    int16_t  span;             /* tenths of a degree swept across the value range */
    int16_t  zero;             /* tenths of a degree at value_min */
    uint16_t value_min;
    uint16_t value_max;
    uint16_t flags;            /* CLK_FLAG_*  */
} clk_aux_t;
