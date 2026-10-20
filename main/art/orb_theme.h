/**
 * orb_theme.h — the theme that is actually on the dial, and how a new one gets
 * there.
 *
 * THE PROBLEM THIS SOLVES. A bake has always produced a pair: the pixel bank
 * (assets/clock_assets.bin, into the `assets` partition, mmap'd by art_bank.c)
 * and the geometry that describes it (assets/clock_assets.h, COMPILED INTO THE
 * APP). clock_view.c reads that geometry in 85 places. So a theme arriving from a
 * browser could bring its pixels but not its numbers, and loading an arbitrary
 * theme was impossible without a reflash. The container in tools/orbtheme.py
 * carries the geometry next to the pixels, and the accessor macros below are how
 * clock_view.c stops reading it as compile-time constants — with the compiled-in
 * pair left in place as the fallback, which is what keeps `idf.py flash` and the
 * shipped dial working exactly as they did.
 *
 * INSTALL IS ONE SLOT, INSIDE THE `assets` PARTITION (ruling D13, 2026-10-12).
 * Two slots exist on the partition, each a 4 KB header followed by a
 * 64 KB-aligned payload:
 *
 *     slot A  header @ 0x000000   payload @ 0x00010000   <- the default, always
 *     slot B  header @ 0x00400000 payload @ 0x00410000   <- the only install target
 *
 * Slot A is the raw bank `idf.py flash` writes and the bank use_baked_in() draws
 * the factory dial from. It is NOT an install target any more, and that is the
 * whole of D13: choosing whichever slot is not currently live used to aim the
 * second install at A, and the second pick on a real device erased the factory
 * default and its fallback with it. Payload and geometry are still written and
 * read back verified BEFORE the header, so an install interrupted at any point —
 * power cut, card pulled, crash — leaves B headerless and invalid, and the boot
 * that follows draws the default. A is the reason that is survivable at all.
 *
 * What the dial draws is a CHOICE, stored under THEME_NVS_LIVE as "default" or
 * "card". It is not a pending operation: nothing installs at boot. Boot reads
 * the choice, verifies the slot it names, and falls back to the default with the
 * damage recorded when B's bytes do not match what the install checked — B was
 * verified when it was written, so anything else is corruption, not a state a
 * user is waiting on.
 *
 * A board that has never installed anything reports source "baked-in" and reads
 * its geometry from clock_assets.h.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "clock_assets.h"   /* clk_theme_t, clk_bg_rect_t + the baked-in fallback */

#define ORB_THEME_EXT      ".orbtheme"
#define ORB_THEME_NAME_MAX 56      /* the one name field in a FORMAT-2 header */
#define ORB_THEME_ID_HEX   12      /* sha256(name)[:12] — the card filename stem */

/* Partition offsets inside `assets`, both 64 KB aligned so esp_partition_mmap
 * wastes no MMU entry on rounding. ORB_THEME_SLOT_CAP is what a theme has to fit
 * in: the shipped bank is 3.68 MB of its 4 MB, so check this number before
 * believing any particular theme will install. */
#define ORB_THEME_SLOT_A_HDR  0x000000u
#define ORB_THEME_SLOT_A_ART  0x00010000u
#define ORB_THEME_SLOT_B_HDR  0x00400000u
#define ORB_THEME_SLOT_B_ART  0x00410000u

/* ---- complication records -------------------------------------------------
 *
 * A theme asks for a complication by shipping one of these per aperture, ring or
 * indicator, and the paint path interprets it. Declared here and NOT in the
 * generated header on purpose: that header is the bake's output for the
 * compiled-in fallback, and a theme that ships no complications must keep booting
 * exactly as it does today with a header the bake has not been changed to emit.
 * Every field is one u32 word on the wire, widened so neither side has a padding
 * rule to disagree about. The three lists that must stay in step are
 * tools/theme_schema.py, tools/orbtheme.py and here; tools/host_theme_test.py
 * packs through the Python and reads back through the lifted C.
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
 * that address the bank, so a bucket never reads as a sprite index. */
typedef struct {
    uint16_t tag;              /* manifest comp id (c2 -> 2); 0 = synthetic */
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

/** What clock_view.c reads in place of the generated header's constants. */
typedef struct {
    clk_theme_t   *theme;
    clk_bg_rect_t *rects;
    uint16_t      *rect_first;
    uint16_t      *gif_delays;
    uint32_t      *bg_frames;
    /* The complication/indicator tables. NULL/0 for any theme baked before they
     * existed, which is why the paint path tests the counts and not the pointer.
     * All three live in the one arena the loader allocated, and like the four
     * tables above they are never freed while the dial is running. Non-const for
     * the same reason rects is: orb_theme_geom_parse() fills them in place. */
    clk_comp_t   *comps;
    clk_aux_t    *aux;
    clk_sprite_t *bank;
    uint16_t      n_comps;
    uint16_t      n_aux;
    uint16_t      n_bank;
    size_t         art_size;          /* pixel bytes, not partition bytes */
    uint32_t       blob_off;          /* the theme's bank's offset inside the mapped
                                       *   art region. Zero for FORMAT 2 and for the
                                       *   compiled pair, whose payload starts at the
                                       *   region's first byte; a FORMAT 3 bundle
                                       *   packs one bank per section, so the clock's
                                       *   pixels begin wherever its record said and
                                       *   clock_view's blob base must follow. */
    bool           from_container;    /* false = the compiled-in pair */
    char           name[ORB_THEME_NAME_MAX + 1];  /* the installed theme's owner name */
} orb_theme_geo_t;

/* The five accessors clock_view.c reads through — clk_theme, clk_bg_rects,
 * clk_bg_rect_first, clk_gif_delays, clk_bg_frames, CLK_BLOB_BYTES — live in
 * orb_theme_geo.h, NOT here. They cannot be here: assets/clock_assets.h defines
 * those very names, and a macro would expand inside its own definition line.
 * clock_view.c includes the generated header first and the macro header second. */

/**
 * Resolve the choice the owner made and verify the slot it names. Call BEFORE
 * clock_view_init(), which maps the bank — this reads the same multi-megabyte
 * region the paint path is about to map, and resolving first is what stops the
 * dial drawing half of one theme and half of another.
 *
 * It resolves and verifies ONLY. Nothing installs from the card here (D13): the
 * boot path that opened the card, copied a container and kept a retry counter was
 * a boot that could spend seconds on a card it did not need and could be left
 * half-installed by a power cut. Installing is orb_theme_install()'s job, and the
 * only thing that decides when it runs is a pick.
 */
esp_err_t orb_theme_boot(void);

/** The resolved geometry; valid from orb_theme_boot() onwards, never NULL. */
const orb_theme_geo_t *orb_theme_geo(void);

/** "slot-A", "slot-B" or "baked-in": what the dial is drawing from. */
const char *orb_theme_source(void);
/** The owner name of the theme currently on the wall, "" for the factory dial. */
const char *orb_theme_active_name(void);

/** The owner's choice, as resolved at boot: "default" or "card". */
#define THEME_LIVE_DEFAULT "default"
#define THEME_LIVE_CARD    "card"
const char *orb_theme_live(void);

/**
 * Write the choice. Nothing else: no card, no copy, no reboot. Writing NVS can
 * cost a sector erase, so the caller is a worker task, never the LVGL one, and
 * the caller owns asking for the reboot (orb_jobs_reboot_soon) — this function
 * leaves the dial drawing what it was drawing until the boot that follows.
 */
esp_err_t orb_theme_set_live(const char *which);

/**
 * Install one card theme into slot B: verify the whole container first, copy it,
 * verify the copy out of the partition, then commit the header that makes it
 * live, then set live=card. Runs the same verify-first ladder the old boot path
 * ran, in the same order, for the same reason — a failed copy costs a row message
 * and not a brick. `id` is a device-issued card filename stem (12 hex chars); it is
 * resolved by walking the themes directory and re-verified by hashing that file's
 * header name, so a file swapped under an id cannot install and no browser string
 * ever reaches a path builder. Takes the card lock for the copy and writes flash,
 * so it is a WORKER TASK call: a multi-second SD-to-flash copy on the LVGL task
 * freezes the dial and swallows the knob. `why` receives the sentence the row shows;
 * it may be NULL.
 */
esp_err_t orb_theme_install(const char *id, char *why, size_t whysz);

/* True when `id` is exactly ORB_THEME_ID_HEX chars of [0-9a-f] — a card filename
 * stem this device could have issued. The HTTP handlers call it to reject a
 * malformed id at the edge; install/delete re-check it on the resolve path. */
bool orb_theme_id_ok(const char *id);

/** Why the choice on the wall is not what the owner picked: both are "" when the
 *  slot boot wanted verified, and the id of the install that did not when it
 *  did not. The picker shows the sentence on that theme's row. */
const char *orb_theme_damage_id(void);
const char *orb_theme_damage_why(void);

/** D30's `staged` word (§20.4): the 12-hex id of an install whose pixels reached
 *  the slot and whose header never did, and the sentence for its row. ""/"" when
 *  nothing is staged. Read-only — the word never selects a theme (§20.2's flip
 *  rolls back on its own, because boot draws the slot that is committed). */
const char *orb_theme_staged_id(void);
const char *orb_theme_staged_why(void);

/** The boot arena refill §20.5(2) measured: whole milliseconds of the fused
 *  read-and-CRC pass over the chosen slot, and the bytes it moved. Both 0 on a
 *  boot that drew the default, which maps instead of refilling (D13). */
uint32_t orb_theme_refill_ms(void);
uint32_t orb_theme_refill_bytes(void);

/** The last streaming bundle store (P6.4): milliseconds of the fused card-to-slot
 *  pass and the bytes of art it moved. Both 0 until a bundle installs. The page
 *  reads the pair so the §20.5(1) copy rate needs no serial cable. */
uint32_t orb_theme_bundle_ms(void);
uint32_t orb_theme_bundle_bytes(void);

/** Put a FORMAT 3 app bundle in the target slot: streaming ingest (§22.3, P6.3).
 *  Each section's bank is checksummed as its bytes cross from the card, the region
 *  is verified before the first erase, and the installed bytes are read back and
 *  checksummed from the partition before the header makes them live.
 *
 *  Unlike orb_theme_install() this does NOT choose the theme and does not ask for a
 *  reboot: the firmware that can draw a bundle arrives with the flip page (P6.4) and
 *  the FORMAT 3 compositor, so the row reports the install as stored. Boot says the
 *  same thing from the slot — "an app bundle this dial cannot draw yet" — rather
 *  than calling installed bytes missing (D26).
 *
 *  Returns the same esp_err_t/`why` pair shape as orb_theme_install(). */
esp_err_t orb_theme_install_bundle(const char *id, char *why, size_t whysz);

/**
 * Queue an install on the shell's one worker, and ask for the reboot that draws
 * it if it lands. These are the ONLY way anything below main/ triggers a theme
 * change: the dial's picker and the browser's Load button both post here, so the
 * knob and the page cannot hold two opinions about what picking a theme does.
 *
 * Returns what the queue said — ESP_ERR_INVALID_STATE when another job is
 * outstanding, ESP_ERR_NO_MEM when the queue refused — and never blocks, so a
 * page handler may call it from the httpd task and the shell's poll from the LVGL
 * one. The reason a refused install costs is in orb_theme_try_why(), which the
 * row and the page read as one string.
 */
esp_err_t orb_theme_post_install(const char *id);
esp_err_t orb_theme_post_default(void);

/** P6.4's flip door: post the streaming bundle store (orb_theme_install_bundle)
 *  on the same queue the FORMAT 2 install uses. No reboot follows — a stored
 *  bundle is not drawn, and a boot that shows the owner nothing is theatre. The
 *  refusal lands in orb_theme_try_why() exactly as the FORMAT 2 press's does. */
esp_err_t orb_theme_post_bundle(const char *id);

/** What the last pick could not do: the id it was asked for and the sentence
 *  the ladder answered with. Both "" after a success, and the picker puts them on
 *  the row that was pressed rather than in a status line nothing can reach. */
const char *orb_theme_try_id(void);
const char *orb_theme_try_why(void);

typedef struct {
    char      name[ORB_THEME_NAME_MAX + 1];  /* the owner's name, from the header */
    char      id[ORB_THEME_ID_HEX + 1];      /* sha256(name)[:12] — card filename stem */
    char      why[72];            /* why it is unloadable; empty when err == ESP_OK */
    uint32_t  art_size;
    uint32_t  serial;
    uint32_t  fmt;                /* 2 or 3, from the FORMAT word; 0 = unreadable.
                                   * A fmt 3 row is a BUNDLE: loadable through
                                   * orb_theme_post_bundle(), never through the
                                   * picker, and never active — a FORMAT 3 slot
                                   * is stored, not drawn, until P6.5's reader. */
    uint32_t  n_sections;         /* fmt 3 only: the index's app count; 0 for v2 */
    esp_err_t err;              /* ESP_OK = loadable */
    bool      active;           /* this file is what the dial is drawing */
    bool      changed;          /* the installed copy of this name has different bytes */
} orb_theme_entry_t;

/**
 * List /sd/themes, up to cap entries; -1 when the card is not mounted. A bad
 * entry is RETURNED with its err set rather than hidden — "no themes" and "three
 * themes, all damaged" are different problems (the card's vs the bake's) and the
 * page has to be able to tell the owner which one they have.
 */
int orb_theme_scan(orb_theme_entry_t *out, int cap);

/* ---- uploading a theme FROM the browser TO the card -------------------------
 *
 * The upload is chunked because a theme is ~3.7 MB and the httpd worker has no
 * business holding that in RAM: the page streams the file in, and the device cannot
 * say whether it is a theme until the last byte arrives and the checksums can run.
 * So bytes land in <slug>.orbtheme.part, are verified, and only then rename() into
 * place. A half-uploaded file stays invisible to orb_theme_scan() because it does not
 * carry the .orbtheme extension — writing straight to the final name and deleting on
 * failure would leave the page listing a theme that is two thirds of a file.
 *
 * One upload at a time, holding orb_sd's lock for its duration. While one is in
 * progress orb_theme_scan() reports busy instead of queueing behind a multi-second
 * card write: a page that cannot poll is indistinguishable from a dead device, and
 * this one is in somebody's hand during the wait.
 */
typedef struct orb_theme_up orb_theme_up_t;

/* 4 MB of theme plus room for the header and the alignment gap. A bigger body is
 * refused on Content-Length, before a byte of it reaches the card. */
#define ORB_THEME_UPLOAD_MAX  (0x00400000u + 0x20000u)

esp_err_t orb_theme_upload_begin(orb_theme_up_t **out);
esp_err_t orb_theme_upload_data(orb_theme_up_t *u, const void *buf, size_t n);
/* A distinct code so the page can tell a name collision (offer overwrite, then
 * retry with ?overwrite=1) from any other refusal, which is a plain error the owner
 * fixes by re-baking. Chosen above the ESP common-error range so it cannot collide
 * with an esp_err_t the transport already assigns a meaning to. */
#define ORB_THEME_UPLOAD_E_DUP  ((esp_err_t)0x0101F2)
/* Verifies the whole .part file and renames it into place under the name its own
 * header carries (the filename is theme_id(header.name), never an upload argument).
 * Duplicate policy: same name + same geom_crc/art_crc/serial is accepted
 * idempotently; a different set of bytes under an existing name is refused with
 * ORB_THEME_UPLOAD_E_DUP unless overwrite. On failure nothing is committed and the
 * .part is removed, so a bad upload cannot be loaded by mistake. */
esp_err_t orb_theme_upload_end(orb_theme_up_t **u, bool overwrite, const char **why);
void      orb_theme_upload_cancel(orb_theme_up_t **u);

/* True while an upload owns the card, so the status endpoint can answer instead of
 * blocking the httpd worker behind the write. */
bool orb_theme_upload_busy(void);

/** Delete one theme file from the card by its device-issued id (12 hex chars),
 *  resolved by walking the themes directory and re-verified against the file's
 *  header name. Never builds a path from the caller's string. */
esp_err_t orb_theme_delete(const char *id);

#define ORB_THEME_SLOT_CAP    0x003F0000u   /* 4 MB less the 64 KB header block */
