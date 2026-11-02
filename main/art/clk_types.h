/* clk_types.h — the analog dial's geometry types.
 *
 * §26 (P6.5) retired the FORMAT 2 dialect and the compiled assets/clock_assets
 * pair with it. These structs were generated into that header by
 * tools/bake_clock.py and are the runtime geometry every clock draws into —
 * FORMAT 3's reader (main/art/orb_clock3.c) projects a bundle's element table
 * straight into a clk_theme_t, and orb_stage.c reads one. The types outlive the
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

/* §31: plate frames in one keyframe bank. orb_sched.c's bgAnim consumer carried
 * this as 8 when the extra planes could only arrive baked into a FORMAT 2 pair;
 * a FORMAT 3 plate row can name more, and the D29 art cap binds first — one
 * full-panel plane is 434,312 B against a 4,128,768 B bank budget, so the
 * longest legal bake is 9 planes and 16 is the array bound that is never the
 * wall. The projection (orb_clock3.c) and the pair mirror (bake_clock.py
 * --from-bundle) refuse a longer bank with this number. */
#define CLK_BG_MAX    16

/* D41: when a driven row moves (docs/hand-motion-plan.md §2.1). D42 retired
 * INHERIT with the old dialects it inherited from: 0 is NONE, the cadence of a still
 * row and of nothing else (docs/element-model-plan.md §4). */
enum {
    CLK_CAD_NONE       = 0,   /* a still row: it never moves                         */
    CLK_CAD_SWEEP      = 1,   /* the whole fraction                                  */
    CLK_CAD_STEP       = 2,   /* the fraction quantised to 1/cad_arg of a unit       */
    CLK_CAD_JUMP       = 3,   /* no fraction: moves on the unit boundary             */
    CLK_CAD_STOP_TO_GO = 4,   /* one turn in cad_arg tenths of a second, then rests  */
    CLK_CAD_DRAG       = 5,   /* holds, then creeps over cad_arg minutes to the boundary */
    CLK_CAD__MAX
};

/* D42 §3: what a row does past the end of its domain. */
enum {
    CLK_END_WRAP    = 0,   /* the next unit starts at P0 unseen: a whole turn, no travel */
    CLK_END_FLYBACK = 1,   /* back to P0 when the source resets, eased by `ease`       */
    CLK_END_HOLD    = 2,   /* rests at P1 until the source resets                      */
    CLK_END__MAX
};

/* D42 §4: which part of the source value a row reads. */
enum {
    CLK_DIGIT_WHOLE = 0,
    CLK_DIGIT_UNITS = 1,   /* v % 10: a double-rotator date's units ring */
    CLK_DIGIT_TENS  = 2,   /* v / 10 % 10: its tens ring                 */
    CLK_DIGIT__MAX
};

/* D42 §2: what a row's art is. The shape says nothing about how it moves. */
enum {
    CLK_SHAPE_ARM  = 0,
    CLK_SHAPE_RING = 1,
    CLK_SHAPE_TEXT = 2,    /* glyphs from the string table: E4, refused until then */
    CLK_SHAPE__MAX
};

/* D42: the clock's moving rows, and the most frames the one animated row carries. */
#define CLK_ROW_MAX   24   /* theme_schema.CLK_ROW_MAX */

/* The dial's section-wide geometry. D42 moved every hand, shadow and carriage into
 * clk_row_t (below); what stays here is the plate, the glass and the background's
 * animation, plus the tempo and carriage figures the reader DERIVES from the rows
 * for the scheduler (orb_sched.c reads second_sweep, tick_rate and the tourbillon
 * pair, and no longer from carried scalars). */
typedef struct {
  uint32_t bg;
  uint8_t  second_sweep, tick_rate;   /* derived: the dial's beat (clock_tempo) */
  uint16_t bg_frames;       /* distinct extra plate frames (0 = still) */
  int16_t  bg_fps; uint8_t bg_loop; uint16_t bg_every;
  uint32_t plate_off;       /* full-screen RGB565 colour plane offset */
  clk_sprite_t overlay;
  uint8_t  bg_mode;         /* 0 = plate + bgAnim frames, 1 = animated delta bank */
  uint32_t bg_key_off;      /* mode 1: full RGB565 keyframe (frame 0) */
  uint16_t gif_frames;      /* mode 1: frames in the loop (incl. keyframe) */
  uint16_t tourbillon_n;    /* derived: the animated row's frames (0 = none) */
  uint16_t tourbillon_fps;  /* derived: 1000 / its frame time */
  clk_sprite_t tourbillon[CLK_TOURB_MAX];  /* the animated row's frames, one geometry */
  uint8_t  car_row;         /* the animated row's index in the row table; 0xFF none */
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
 * this one, so the FORMAT 2 loader and orb_stage.c see the same declarations.
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
 * still parses, but no current bake emits it and orb_stage.c declines to
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
 * is a moon that never reaches the glass. orb_stage.c honours the bucket in
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
    /* C4's delta dialect, appended at the END for the pair rule above: a record
     * baked before them reads 0, and 0 here means NONE. `parent_row` is one more
     * than the row `n` of the drawing row this one hangs on (0 = no parent), so
     * the tag that means plate on the wire — 0 — can never arrive here by
     * accident: the loader refuses ABI-2 rows that name parent 0 outright.
     * `delay_ms` is the row's own per-frame interval, 0 = the tempo the dial
     * keeps for it today (the GIF table, the tick). */
    uint16_t parent_row;       /* parent's element row n + 1; 0 = none */
    uint16_t delay_ms;         /* per-frame interval; 0 = the dial's own default */
} clk_comp_t;

/* D42: one moving row — a hand, a pointer, a ring, a still layer, a shadow twin or
 * the carriage. Every one is an AUX row on the wire (docs/element-model-plan.md §8) and
 * one of these at runtime, drawn by one path in orb_stage.c. Its art sits at P0 (the
 * row's own placement, turned to `zero`); across its domain it turns a further `span`
 * and its pivot travels (dx, dy) to P1, and `end` says what it does past the end. */
typedef struct {
    clk_sprite_t spr;          /* the art at P0: w,h,off,pivot,centre,blend,show */
    uint16_t tag;              /* the AUX slot the row claimed */
    uint8_t  src;              /* CLK_SRC_*; NONE = a still row */
    uint8_t  shape;            /* CLK_SHAPE_* */
    uint8_t  digit;            /* CLK_DIGIT_* */
    uint8_t  end;              /* CLK_END_* */
    uint8_t  cadence;          /* CLK_CAD_* */
    uint8_t  order;            /* depth, 0..63, on the scale the BANK rows share */
    int16_t  zero;             /* P0's angle, tenths of a degree */
    int16_t  span;             /* the turn from P0 to P1, tenths of a degree */
    int16_t  dx, dy;           /* the pivot's travel from P0 to P1, px */
    uint16_t value_min;        /* the value at P0 */
    uint16_t value_max;        /* the value at P1 (a wrap's next P0) */
    uint16_t cad_arg;          /* step N, stop-to-go P (0.1 s), drag window (min) */
    uint16_t ease;             /* settle or flyback ms; 0 = none */
    uint16_t flags;            /* CLK_FLAG_AUX_SHADOW */
    uint16_t parent_row;       /* the row this one rides, its index + 1; 0 = none */
    uint16_t delay_ms;         /* the animated row's frame time */
} clk_row_t;
