/**
 * orb_theme.c — see orb_theme.h for the model. An installed theme is a container
 * from the card copied into one of two A/B slots in the `assets` partition, and
 * the geometry it carries is what clock_view.c reads instead of the compiled-in
 * one.
 *
 * The wire format is DEFINED in tools/orbtheme.py and mirrored here. That mirror
 * is the risk in this file: a field added on one side only produces a dial whose
 * hand pivots read as overlay offsets, which looks like bad art and not like a
 * bug. Three things hold it together — the static asserts on the header layout,
 * the bounds checks in orb_theme_geom_parse() that refuse a theme rather than
 * render it wrong, and the sentinel test in tools/host_tz_test.py that packs
 * through the Python and reads back through this C lifted verbatim.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"              // the listing timing in orb_theme_scan()
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "orb_theme.h"
#include "orb_sd.h"
#include "art_bank.h"

static const char *TAG = "orb_theme";

/* Settings live in their OWN namespace in the `storage` partition, as
 * partitions.csv demands: esp_wifi owns "nvs.net80211" and orb_net owns
 * "orb.net". A chosen theme is device config the owner picked, so it has to
 * survive the same things the timezone does — including main.c's recovery
 * nvs_flash_erase() of the default partition. */
#define THEME_NVS_PARTITION "storage"
#define THEME_NVS_NAMESPACE "orb.settings"
#define THEME_NVS_PENDING   "theme_load"   /* slug staged by the page */

/* Container header words. Mirrors tools/orbtheme.py's W_* map; the sentinel test
 * is what keeps the two lists in step. */
enum {
    W_MAGIC = 0, W_FORMAT, W_GEOM_ABI, W_HDR_SIZE,
    W_GEOM_OFF, W_GEOM_SIZE, W_GEOM_CRC,
    W_ART_SIZE, W_ART_CRC,
    W_SCR_W, W_SCR_H, W_SERIAL, W_BAKED_MS, W_ART_OFF,
    W_SLUG = 14,        /* 24 bytes -> words 14..19 */
    W_TITLE = 20,       /* 32 bytes -> words 20..27 */
    W_HDR_CRC = 28,     /* crc32 of all 128 B with THIS word zeroed */
    W_WORDS = 32
};
_Static_assert(W_TITLE == W_SLUG + (24 + 3) / 4,
               "title must start where the slug ends — they may not overlap");
_Static_assert(W_HDR_CRC >= W_TITLE + (32 + 3) / 4,
               "title runs into the header checksum word");
_Static_assert(W_WORDS == 32, "container header must be 128 bytes");

#define THEME_MAGIC    0x474F5254u   /* 'T','R','O','G' — the same constant twice */
#define THEME_FORMAT   1
#define THEME_GEOM_ABI 1
#define THEME_SECTOR   4096

/* Slot layout, private to this file. The two within-slot offsets are the only
 * numbers here; a slot's absolute addresses are its base plus those. Both are
 * 64 KB aligned because 64 KB is esp_partition_mmap's page, and an unaligned start
 * wastes an MMU entry on rounding at each end. The geometry sits in its own sector
 * at +0x1000 rather than tight against the 128-byte header so that making a slot
 * live — an erase plus a rewrite of its header block — cannot touch anything
 * underneath it. */
#define SLOT_A_HDR      0x000000u
#define SLOT_B_HDR      0x00400000u
#define SLOT_GEOM_OFF   0x00001000u                 /* within a slot */
#define SLOT_ART_OFF    0x00010000u                 /* within a slot */
#define SLOT_A_ART      (SLOT_A_HDR + SLOT_ART_OFF)
#define SLOT_B_ART      (SLOT_B_HDR + SLOT_ART_OFF)
#define SLOT_GEOM_CAP   (SLOT_ART_OFF - SLOT_GEOM_OFF)   /* 60 KB of tables */
#define SLOT_CAP        0x00400000u                      /* 4 MB per slot    */

/* Wire counts. THEME_SCALARS_N must equal len(orbtheme.THEME_FIELDS) and
 * SPRITE_WORDS_N len(orbtheme.SPRITE_FIELDS): a mismatch shifts every field after
 * it, and the sentinel test is what notices. */
#define THEME_SCALARS_N  26
#define SPRITE_WORDS_N   15
#define TOURB_MAX        16                        /* CLK_TOURB_MAX */
#define SPRITES_TOTAL    (5 + 5 + 1 + TOURB_MAX)   /* hands, shadows, overlay, tb */
#define RECT_WORDS       5                         /* x, y, w, h, off */
#define GEOM_FIXED_WORDS (4u + THEME_SCALARS_N + (uint32_t)SPRITES_TOTAL * SPRITE_WORDS_N)

/* Bounds the loader accepts, from clock_view.c's own array sizes and from what
 * the shipped bake measures (128 rects, 8 GIF frames). A theme wanting more than
 * the dial holds is refused as unloadable rather than installed and then clamped
 * at paint time into a half-drawn frame. */
#define RECTS_MAX       2048
#define GIFS_MAX        32                         /* CLK_GIF_MAX */
#define BG_FRAMES_MAX   8                          /* CLK_BG_MAX  */

/* A sprite occupies colour w*h*2 then alpha w*h. Must match sprite_bytes() in
 * clock_view.c, which is the code these offsets are checked against. */
#define SPRITE_BYTES(s) ((uint64_t)(s)->w * (uint64_t)(s)->h * 3u)
#define PLATE_BYTES     ((uint64_t)CLK_SCR_W * (uint64_t)CLK_SCR_H * 2u)


/* ------------------------------------------------------------------------- *
 * Header + geometry blob -> clk_theme_t. Pure: no allocator, no card, no
 * partition. tools/host_tz_test.py calls these with static buffers against a blob
 * packed by tools/orbtheme.py with a distinct sentinel in every field, so "both
 * sides agree on the field order" is a green line and not a hope.
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t geom_off, geom_size;
    uint32_t art_off,  art_size;
    uint32_t geom_crc, art_crc;
    uint32_t serial;
    uint32_t n_rects, n_first, n_delays, n_bgf;
    char     slug[24 + 1];
    char     title[32 + 1];
} orb_thdr_t;

typedef struct {
    const uint32_t *w;
    size_t          n, i;
    const char     *why;                 /* first reason this is not a theme */
} rd_t;

static uint32_t rdt(rd_t *r, const char *what)
{
    if (r->why) return 0;                /* already refused; stay in range */
    if (r->i >= r->n) { r->why = what; return 0; }
    return r->w[r->i++];
}

static uint16_t rd_u16(rd_t *r, const char *w) { return (uint16_t)rdt(r, w); }
static int16_t  rd_i16(rd_t *r, const char *w) { return (int16_t)rdt(r, w); }
static uint8_t  rd_u8 (rd_t *r, const char *w) { return (uint8_t)rdt(r, w); }
static int8_t   rd_i8 (rd_t *r, const char *w) { return (int8_t)rdt(r, w); }

/* One clk_sprite_t, in tools/orbtheme.py's SPRITE_FIELDS order. Each read names the
 * field it wanted, so the first out-of-range word says WHICH one: "blob too short"
 * is the useless version of that message, and it is the one somebody reads over
 * USB serial with the case still screwed shut. */
static void rd_sprite(rd_t *r, clk_sprite_t *s, const char *tag)
{
    s->w         = rd_u16(r, tag);
    s->h         = rd_u16(r, tag);
    s->offx      = rd_i16(r, tag);
    s->offy      = rd_i16(r, tag);
    s->pivotx    = rd_i16(r, tag);
    s->pivoty    = rd_i16(r, tag);
    s->centerx   = rd_i16(r, tag);
    s->centery   = rd_i16(r, tag);
    s->blend     = rd_u8 (r, tag);
    s->show      = rd_u8 (r, tag);
    s->color_off = rdt   (r, tag);
    s->diffx     = rd_i16(r, tag);
    s->diffy     = rd_i16(r, tag);
    s->diffw     = rd_u16(r, tag);
    s->diffh     = rd_u16(r, tag);
}

static void rd_wordstr(const uint32_t *w, int base, char *dst, int cap)
{
    int i = 0;
    for (; i < cap - 1; i++) {
        const char c = (char)((w[base + i / 4] >> (8 * (i % 4))) & 0xFF);
        if (!c) break;
        dst[i] = c;
    }
    dst[i] = '\0';
}

/* NULL on success, otherwise the reason this file is not a theme this device can
 * load — in words, because every one of them is what the page shows next to the
 * file the owner just uploaded. */
static const char *hdr_parse(const uint32_t *w, orb_thdr_t *o)
{
    if (w[W_MAGIC]    != THEME_MAGIC)     return "not an .orbtheme file";
    if (w[W_FORMAT]   != THEME_FORMAT)    return "theme format this firmware cannot read";
    if (w[W_GEOM_ABI] != THEME_GEOM_ABI)  return "baked for a different dial — re-bake it";

    /* The self-checksum first: it covers 128 bytes, so it costs nothing, and a
     * torn header that claims a 4 MB payload would otherwise be streamed, hashed
     * and written to flash before anything noticed. */
    uint32_t zeroed[W_WORDS];
    memcpy(zeroed, w, sizeof(zeroed));
    zeroed[W_HDR_CRC] = 0;
    if (w[W_HDR_CRC] != esp_rom_crc32_le(0, (const uint8_t *)zeroed, sizeof(zeroed)))
        return "container header is damaged — re-copy the file onto the card";

    o->geom_off  = w[W_GEOM_OFF];   o->geom_size = w[W_GEOM_SIZE];
    o->art_off   = w[W_ART_OFF];    o->art_size  = w[W_ART_SIZE];
    o->geom_crc  = w[W_GEOM_CRC];   o->art_crc   = w[W_ART_CRC];
    o->serial    = w[W_SERIAL];
    if (w[W_SCR_W] != CLK_SCR_W || w[W_SCR_H] != CLK_SCR_H)
        return "baked for a different panel size";
    /* Where the two parts sit is the CALLER's rule, not this one: a container FILE
     * puts its blob right after the header, an INSTALLED slot puts it at
     * SLOT_GEOM_OFF. Both check against their own layout. Sizes are checked here
     * because they decide how many bytes get streamed and how much RAM the
     * geometry lands in. */
    if (o->geom_size < GEOM_FIXED_WORDS * 4u || o->geom_size > SLOT_GEOM_CAP)
        return "geometry blob size is not credible for this dial";
    if (o->art_size < PLATE_BYTES || o->art_size > SLOT_CAP)
        return "pixel payload is bigger than a theme slot — re-bake it";
    rd_wordstr(w, W_SLUG,  o->slug,  sizeof(o->slug));
    rd_wordstr(w, W_TITLE, o->title, sizeof(o->title));
    o->n_rects = o->n_first = o->n_delays = o->n_bgf = 0;
    return NULL;
}

/* The blob's length is fully determined by its four counts, so a header whose
 * numbers do not add up is refused before a single pixel is read. */
static bool geom_sizes_ok(uint32_t geom_size, uint32_t n_rects, uint32_t n_first,
                          uint32_t n_delays, uint32_t n_bgf)
{
    const uint64_t need = (GEOM_FIXED_WORDS + (uint64_t)n_rects * RECT_WORDS +
                           n_first + n_delays + n_bgf) * 4u;
    return need == geom_size;
}

/* Fill `out` from the blob; the caller allocates, which keeps this free of side
 * effects for the host test. Returns NULL or the reason to refuse the theme.
 *
 * `art_size` is the payload length the container declared, and every sprite, rect
 * and full-screen plane is checked against it here. That is the memory-safety half
 * of the feature: the pixels are mmap'd at exactly art_size bytes, so an extent
 * past the end of the mapping is a crash on the first paint after a reboot nobody
 * is watching. An unusable theme beats a wall clock that stops telling the time. */
const char *orb_theme_geom_parse(const uint32_t *words, size_t nwords,
                                 uint32_t art_size, orb_theme_geo_t *out)
{
    rd_t r = { .w = words, .n = nwords };
    clk_theme_t *t = out->theme;

    const uint32_t n_rects  = rdt(&r, "n_rects");
    const uint32_t n_first  = rdt(&r, "n_first");
    const uint32_t n_delays = rdt(&r, "n_delays");
    const uint32_t n_bgf    = rdt(&r, "n_bg_frames");
    if (r.why) return r.why;
    if (n_rects  > RECTS_MAX)      return "too many delta rects for this dial";
    if (n_first  > GIFS_MAX + 1)   return "too many GIF frame indices";
    if (n_delays > GIFS_MAX)       return "too many GIF delays";
    if (n_bgf    > BG_FRAMES_MAX)  return "too many background frames";

    /* 26 scalars, tools/orbtheme.py's THEME_FIELDS order. */
    t->bg              = rdt(&r, "bg");
    t->plate_follow    = rd_u8 (&r, "plate_follow");
    t->shadow_on       = rd_u8 (&r, "shadow_on");
    t->shadow_dx       = rd_i8 (&r, "shadow_dx");
    t->shadow_dy       = rd_i8 (&r, "shadow_dy");
    t->second_sweep    = rd_u8 (&r, "second_sweep");
    t->second_railway  = rd_u8 (&r, "second_railway");
    t->text_over_hands = rd_u8 (&r, "text_over_hands");
    t->tick_rate       = rd_u8 (&r, "tick_rate");
    t->bg_frames       = rd_u16(&r, "bg_frames");
    t->bg_fps          = rd_i16(&r, "bg_fps");
    t->bg_loop         = rd_u8 (&r, "bg_loop");
    t->bg_every        = rd_u16(&r, "bg_every");
    t->plate_off       = rdt(&r, "plate_off");
    for (int i = 0; i < 5; i++) t->order[i] = rd_u8(&r, "order");
    t->order_n         = rd_u8 (&r, "order_n");
    t->matte_mask      = rd_u8 (&r, "matte_mask");
    t->bg_mode         = rd_u8 (&r, "bg_mode");
    t->bg_key_off      = rdt(&r, "bg_key_off");
    t->gif_frames      = rd_u16(&r, "gif_frames");
    t->tourbillon_n    = rd_u16(&r, "tourbillon_n");
    t->tourbillon_fps  = rd_u16(&r, "tourbillon_fps");
    if (r.why) return r.why;

    if (t->order_n > 5)                return "layer order longer than 5";
    if (t->tourbillon_n > TOURB_MAX)   return "more tourbillon frames than the dial holds";
    if (t->gif_frames > GIFS_MAX)      return "more GIF frames than the dial holds";
    if (t->bg_frames  > BG_FRAMES_MAX) return "more background frames than the dial holds";
    /* bg_mode 1 is the delta-rect bank: two frames minimum to be a loop, and the
     * index table is one longer than the count because clock_view walks rects as
     * [first[i], first[i+1]) — see clock_view.c:461. */
    if (t->bg_mode == 1 && t->gif_frames < 2)
        return "delta-rect background with fewer than two frames";
    if (n_first  && n_first  < (uint32_t)t->gif_frames + 1u)
        return "GIF index table too short for gif_frames";
    if (n_delays && n_delays < (uint32_t)t->gif_frames)
        return "GIF delay table too short for gif_frames";

    for (int i = 0; i < 5; i++)         rd_sprite(&r, &t->hand[i],       "hand");
    for (int i = 0; i < 5; i++)         rd_sprite(&r, &t->shadow[i],     "shadow");
    rd_sprite(&r, &t->overlay,                                             "overlay");
    for (int i = 0; i < TOURB_MAX; i++) rd_sprite(&r, &t->tourbillon[i], "tourbillon");
    if (r.why) return r.why;

    /* Extents, layer by layer. A layer the dial never looks at (show == 0, or a
     * zero w/h) is not checked: clock_view.c will not read it, and refusing a bake
     * for the geometry of a layer it does not draw would only block the theme. */
    if (t->overlay.w && t->overlay.h &&
        (uint64_t)t->overlay.color_off + SPRITE_BYTES(&t->overlay) > art_size)
        return "overlay pixels lie outside the declared payload";
    for (int i = 0; i < 5; i++) {
        const clk_sprite_t *h = &t->hand[i];
        if (h->show && h->w && h->h &&
            (uint64_t)h->color_off + SPRITE_BYTES(h) > art_size)
            return "hand pixels lie outside the declared payload";
        const clk_sprite_t *d = &t->shadow[i];
        if (t->shadow_on && d->show && d->w && d->h &&
            (uint64_t)d->color_off + SPRITE_BYTES(d) > art_size)
            return "shadow pixels lie outside the declared payload";
    }
    for (int i = 0; i < (int)t->tourbillon_n; i++) {
        const clk_sprite_t *s = &t->tourbillon[i];
        if (s->w && s->h && (uint64_t)s->color_off + SPRITE_BYTES(s) > art_size)
            return "tourbillon pixels lie outside the declared payload";
    }
    /* The plate and the GIF keyframe are full-screen and read unconditionally in
     * their modes, so they are checked although they are not sprites. */
    if ((uint64_t)t->plate_off + PLATE_BYTES > art_size)
        return "plate pixels lie outside the declared payload";
    if (t->bg_mode == 1 && (uint64_t)t->bg_key_off + PLATE_BYTES > art_size)
        return "GIF keyframe lies outside the declared payload";


    /* The variable tables, sized by the counts validated above. */
    for (uint32_t i = 0; i < n_rects; i++) {
        clk_bg_rect_t *rc = &out->rects[i];
        rc->x   = rd_u16(&r, "rect.x");
        rc->y   = rd_u16(&r, "rect.y");
        rc->w   = rd_u16(&r, "rect.w");
        rc->h   = rd_u16(&r, "rect.h");
        rc->off = rdt(&r, "rect.off");
        if (r.why) return r.why;
        /* clock_view.c:469 paints a rect as the byte run off+x .. off+x+w*2 and
         * mirrors it into the canvas at x,y, so BOTH extents have to hold. This is
         * the check a half-copied or hand-edited file fails, and the only one that
         * catches it before the dial reads off the end of the mapping. */
        if ((uint64_t)rc->off + (uint64_t)rc->x + (uint64_t)rc->w * 2u > art_size)
            return "delta rect lies outside the declared payload";
        if ((int)rc->x + (int)rc->w > CLK_SCR_W || (int)rc->y + (int)rc->h > CLK_SCR_H)
            return "delta rect lies outside the panel";
    }
    for (uint32_t i = 0; i < n_first; i++) {
        out->rect_first[i] = rd_u16(&r, "rect_first");
        if (r.why) return r.why;
        /* first[] indexes the rect table, and clock_view reads first[tgt+1] — the
         * +1 is covered because the baker emits gif_frames+1 entries and the count
         * check above demanded it. */
        if (out->rect_first[i] > n_rects) return "frame index runs past the rect table";
    }
    for (uint32_t i = 0; i < n_delays; i++) {
        out->gif_delays[i] = rd_u16(&r, "gif_delay");
        if (r.why) return r.why;
    }
    for (uint32_t i = 0; i < n_bgf; i++) {
        out->bg_frames[i] = rdt(&r, "bg_frame_off");
        if (r.why) return r.why;
        if ((uint64_t)out->bg_frames[i] + PLATE_BYTES > art_size)
            return "background frame lies outside the declared payload";
    }
    /* Trailing words mean the baker and this reader disagree on how much to
     * consume. Refusing is the only safe answer: reading on drops the tail of the
     * tables, and accepting them quietly is how a theme installs with a
     * half-populated rect table and shows a frame with a piece missing. */
    if (r.i != r.n) return "geometry blob has words this build does not recognise";
    return NULL;
}

/* ------------------------------------------------------------------------- *
 * The resolved geometry the dial draws with.
 * ------------------------------------------------------------------------- */

static clk_theme_t   s_theme;                    /* parsed container geometry */
static clk_bg_rect_t *s_rects;
static uint16_t     *s_first, *s_delays;
static uint32_t     *s_bgframes;
static orb_theme_geo_t s_geo;
static char          s_active_slug[ORB_THEME_SLUG_MAX + 1];
static char          s_pending_slug[ORB_THEME_SLUG_MAX + 1];
static const char   *s_source = "baked-in";

/* The fallback pair, straight out of the generated header. Naming those objects
 * works in THIS file only, and only because orb_theme_geo.h is not included here:
 * its macros shadow exactly these names, and they are what clock_view.c reads. */
static void use_baked_in(void)
{
    /* Geometry from clock_assets.h, pixels from partition offset 0 — the
     * arrangement `idf.py flash` puts there, and the reason a board with no
     * installed theme still boots the dial it always showed. */
    s_geo.theme      = (clk_theme_t *)&clk_theme;
    s_geo.rects      = (clk_bg_rect_t *)clk_bg_rects;
    s_geo.rect_first = (uint16_t *)clk_bg_rect_first;
    s_geo.gif_delays = (uint16_t *)clk_gif_delays;
    s_geo.bg_frames  = (uint32_t *)clk_bg_frames;
    s_geo.art_size   = (size_t)CLK_BLOB_BYTES;
    s_geo.from_container = false;
    s_geo.slug[0] = '\0';
    s_active_slug[0] = '\0';
    s_source = "baked-in";
}

const orb_theme_geo_t *orb_theme_geo(void) { return &s_geo; }
const char *orb_theme_source(void)         { return s_source; }
const char *orb_theme_active_slug(void)    { return s_active_slug; }
const char *orb_theme_pending_slug(void)   { return s_pending_slug; }


/* ------------------------------------------------------------------------- *
 * Reading a container off the card.
 *
 * One 4 KB chunk buffer, shared by every card operation, from internal RAM. Not on
 * the stack: esp_http_server's task is sized at 4096 bytes by
 * HTTPD_DEFAULT_CONFIG() (esp_http_server.h:57 — whatever the note beside the OTA
 * chunk buffer once claimed about 8 KB), and 4 KB on top of FATFS's own frame use
 * is a stack overflow on the first upload rather than a slow day.
 * ------------------------------------------------------------------------- */

static uint8_t *s_chunk;
#define CHUNK_BYTES 4096

static esp_err_t ensure_chunk(void)
{
    if (s_chunk) return ESP_OK;
    /* INTERNAL, not PSRAM: this buffer is the source or sink of the SPI DMA the
     * sdmmc driver runs underneath these reads. 4 KB against an internal budget the
     * network stack already spends most of is the cost of the feature. */
    s_chunk = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return s_chunk ? ESP_OK : ESP_ERR_NO_MEM;
}

/* "/sd/themes/<slug>.orbtheme", or false. The extension is appended HERE, not by
 * the caller: the page sends a slug, and letting the client spell the suffix is how
 * ".orbtheme/../../x" or a doubled extension reaches the path. The directory and the
 * name rule come from the SD layer, so an endpoint added later cannot reach the card
 * without going through the one validator the host test exercises. */
static bool theme_file(const char *slug, char *out, size_t outsz)
{
    char base[ORB_SD_PATH_MAX];
    if (!orb_sd_theme_path(base, sizeof base, slug)) { out[0] = '\0'; return false; }
    const size_t blen = strlen(base);
    /* sizeof, not strlen: the tail carries the NUL, so the comparison is exact and a
     * slug that only just fits comes back refused instead of as a truncated name --
     * and a truncated name is a different file on the card. */
    if (blen + sizeof(ORB_THEME_EXT) > outsz) { out[0] = '\0'; return false; }
    memcpy(out, base, blen);
    memcpy(out + blen, ORB_THEME_EXT, sizeof(ORB_THEME_EXT));
    return true;
}

/* Read n bytes at *io_pos. pread, not read: verify_container() and the installer
 * walk one fd at two cursors (header, blob, payload), and a shared file offset
 * across those walks is a bug that shows up only on a card whose sector size makes
 * the two cursors meet. */
static ssize_t rd_at(int fd, size_t *io_pos, uint8_t *dst, size_t n)
{
    size_t done = 0;
    while (done < n) {
        const ssize_t got = pread(fd, dst + done, n - done, (off_t)(*io_pos + done));
        if (got < 0) return -1;
        if (got == 0) break;                      /* short file */
        done += (size_t)got;
    }
    *io_pos += done;
    return (ssize_t)done;
}

/* NULL if the container is intact, otherwise the reason it is not. Called before
 * anything is staged and before the partition is touched, so a kit copied onto the
 * card with a flaky reader is caught on a page that still says "Load", not as a
 * boot that quietly fell back to the previous slot with nothing to blame.
 *
 * Each checksum covers exactly its own bytes, the way tools/orbtheme.py computes
 * them (zlib.crc32 over the blob, then over the payload). Hashing the alignment gap
 * between the two because it is simpler would make the device disagree with every
 * file the baker has ever written. */
static const char *verify_container(int fd, const orb_thdr_t *h)
{
    /* The function that touches s_chunk reserves it, the way slot_verify_pixels()
     * already does — idempotent, one branch once the buffer exists. Reserve it HERE
     * and not in each caller: orb_theme_boot() and orb_theme_stage_load() reach this
     * through card_open(), which reserves, but orb_theme_upload_end() opens its .part
     * with a bare open() and never passes through that door. Measured on the bench:
     * the whole 3.87 MB arrived, this function pread() into a NULL s_chunk on a boot
     * that had never staged a theme, FatFs stored through the NULL, and the dial died
     * with a StoreProhibited at the end of a successful upload. */
    if (ensure_chunk() != ESP_OK) return "out of memory for the card read buffer";

    uint32_t crc = 0;
    size_t pos = h->geom_off, left = h->geom_size;
    while (left) {
        const size_t want = left > CHUNK_BYTES ? CHUNK_BYTES : left;
        if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want)
            return "file is shorter than its header claims — re-copy it";
        crc = esp_rom_crc32_le(crc, s_chunk, (uint32_t)want);
        left -= want;
    }
    if (crc != h->geom_crc)
        return "geometry does not match its checksum — re-copy the file onto the card";

    crc = 0;
    pos = h->art_off; left = h->art_size;
    while (left) {
        const size_t want = left > CHUNK_BYTES ? CHUNK_BYTES : left;
        if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want)
            return "pixel data is shorter than its header claims — re-copy it";
        crc = esp_rom_crc32_le(crc, s_chunk, (uint32_t)want);
        left -= want;
    }
    if (crc != h->art_crc)
        return "pixels do not match their checksum — re-copy the file onto the card";
    return NULL;
}


/* Opens and validates everything short of the pixels. On success returns an open
 * fd and the CALLER OWNS BOTH the fd and the card lock — release them together
 * with card_close(), never separately. FATFS keeps one window per volume and this
 * board has one CS line, so two tasks holding files open at once is how a directory
 * entry gets written by one and cached as something else by the other. */
/* The card-lock-free core: the caller must already hold orb_sd_lock(), and on
 * success keeps holding it until card_close_locked(). Taking the mutex in here is
 * what deadlocked the device for real: orb_theme_scan() holds the lock across its
 * whole directory walk and validated each entry through card_open(), which asked for
 * the same non-recursive mutex again. With an empty themes directory the loop body
 * never runs, so the bench saw nothing; the first real theme on a real card left the
 * one httpd worker blocked in xSemaphoreTake forever, and every route stopped
 * answering while ping kept replying. card_open() below is the wrapper for callers
 * that do not hold the lock. */
static int card_open_locked(const char *slug, orb_thdr_t *o, const char **why)
{
    char path[ORB_SD_PATH_MAX];
    *why = NULL;
    if (!theme_file(slug, path, sizeof path)) { *why = "not a usable theme name"; return -1; }
    /* orb_sd_mounted(), not orb_sd_ensure_mounted(): the caller checked the card is
     * there before it took the lock, and ensure_mounted takes that same non-recursive
     * mutex itself. Measured here: this call from inside the locked core left the httpd
     * worker waiting on a lock it already owned — "waiting for the card: held by
     * \"httpd\"" — which is every web route dead and the dial still ticking. This
     * version only asks whether the card is still in, and refuses if it is not. */
    if (!orb_sd_mounted())                        { *why = "no card in the socket"; return -1; }
    if (ensure_chunk() != ESP_OK)                 { *why = "out of memory"; return -1; }

    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        *why = errno == ENOENT ? "no such theme on the card" : "cannot open the theme file";
        return -1;
    }

    uint32_t hdr[W_WORDS];
    size_t pos = 0;
    if (rd_at(fd, &pos, (uint8_t *)hdr, sizeof hdr) != (ssize_t)sizeof hdr) {
        close(fd);
        *why = "file is too short to be a theme";
        return -1;
    }
    const char *r = hdr_parse(hdr, o);
    if (r) { close(fd); *why = r; return -1; }

    /* The file's own layout rule, which hdr_parse leaves open deliberately: an
     * installed slot puts its blob somewhere else and is checked separately. */
    if (o->geom_off != W_WORDS * 4u || o->art_off < o->geom_off + o->geom_size ||
        o->art_off % THEME_SECTOR) {
        close(fd);
        *why = "container parts are not where this format puts them";
        return -1;
    }

    /* The four counts decide the blob's whole length, so a header whose numbers do
     * not add up is refused here — before 4 MB is streamed off the card. */
    uint32_t c[4];
    pos = o->geom_off;
    if (rd_at(fd, &pos, (uint8_t *)c, 16) != 16) {
        close(fd); *why = "geometry blob is missing"; return -1;
    }
    o->n_rects = c[0]; o->n_first = c[1]; o->n_delays = c[2]; o->n_bgf = c[3];
    if (!geom_sizes_ok(o->geom_size, o->n_rects, o->n_first, o->n_delays, o->n_bgf)) {
        close(fd);
        *why = "geometry blob size does not match its own tables — re-bake it";
        return -1;
    }
    return fd;
}

static void card_close_locked(int fd)
{
    if (fd >= 0) close(fd);
}

/* For callers that do not hold the card lock: the mutex is taken here and released by
 * card_close(), which is why the pair is documented as owning both. A failure inside
 * the core never leaves the mutex held.
 *
 * The mount check belongs HERE, before the take, and not in the core. orb_theme_boot()
 * runs from main.c:72, while orb_sd_init() only runs at main.c:138 — deliberately,
 * because an install rewrites the region the dial reads its pixels from and must
 * finish before the first paint, and because the SD init ladder can burn a second per
 * failed rung with no card-detect line. So the ONLY thing that mounts the card in time
 * for a staged install is this call. Measured on hardware: with the check moved into
 * the core (which cannot mount without taking the mutex its caller already holds), the
 * staged install at t=712 ms found no card — the mount happened at t=2136 ms — and the
 * dial burned attempt 1 of 3 and drew the baked-in bank for a file that was on the
 * card the whole time. */
static int card_open(const char *slug, orb_thdr_t *o, const char **why)
{
    if (orb_sd_ensure_mounted() != ESP_OK) { *why = "no card in the socket"; return -1; }
    orb_sd_lock();
    const int fd = card_open_locked(slug, o, why);
    if (fd < 0) orb_sd_unlock();
    return fd;
}

static void card_close(int fd)
{
    if (fd >= 0) close(fd);
    orb_sd_unlock();
}

/* ------------------------------------------------------------------------- *
 * The A/B slots.
 * ------------------------------------------------------------------------- */

typedef struct {
    bool       valid;
    uint32_t   hdr_base;                 /* partition offset of its header block */
    uint32_t   art_base;                 /* partition offset of its payload      */
    orb_thdr_t h;
} slot_t;

static void slot_init(slot_t *s, uint32_t hdr_base, uint32_t art_base)
{
    s->valid = false; s->hdr_base = hdr_base; s->art_base = art_base;
    memset(&s->h, 0, sizeof(s->h));
}

static void slot_read(slot_t *s, const esp_partition_t *p)
{
    uint32_t hdr[W_WORDS];
    /* esp_partition_read, not a pointer: the header block is deliberately NOT
     * mmap'd (art_bank.h on the DROM window), and 128 bytes do not justify an MMU
     * entry every boot. */
    if (esp_partition_read(p, s->hdr_base, hdr, sizeof hdr) != ESP_OK) return;
    if (hdr_parse(hdr, &s->h)) return;
    /* The slot's own layout rule, against the SLOT offsets — not the file's. */
    if (s->h.geom_off != SLOT_GEOM_OFF || s->h.art_off != SLOT_ART_OFF) return;
    uint32_t c[4];
    if (esp_partition_read(p, s->hdr_base + SLOT_GEOM_OFF, c, 16) != ESP_OK) return;
    if (!geom_sizes_ok(s->h.geom_size, c[0], c[1], c[2], c[3])) return;
    s->h.n_rects = c[0]; s->h.n_first = c[1]; s->h.n_delays = c[2]; s->h.n_bgf = c[3];
    s->valid = true;
}


/* Payload checksum, re-read from the partition in chunks rather than through the
 * map: this runs before clock_view_init() has mapped anything, and a checksum is
 * exactly the place not to depend on a mapping that may not exist. Re-reading
 * 3.68 MB costs a fraction of a second against the seconds an erase-and-write
 * takes, and it is the only thing between a half-written slot and a dial rendering
 * whatever bytes happened to land. */
static esp_err_t slot_verify_pixels(const slot_t *s, const esp_partition_t *p)
{
    if (ensure_chunk() != ESP_OK) return ESP_ERR_NO_MEM;
    uint32_t crc = 0;
    for (uint32_t done = 0; done < s->h.art_size; ) {
        const uint32_t want = (s->h.art_size - done > CHUNK_BYTES) ? CHUNK_BYTES
                                                                   : (s->h.art_size - done);
        const esp_err_t e = esp_partition_read(p, s->art_base + done, s_chunk, want);
        if (e != ESP_OK) return e;
        crc = esp_rom_crc32_le(crc, s_chunk, want);
        done += want;
    }
    return crc == s->h.art_crc ? ESP_OK : ESP_ERR_INVALID_CRC;
}

/* Unpack a slot's geometry blob into the statics. RAM is PSRAM first: the blob is a
 * few KB read once at boot, and internal RAM is the resource this board is out of.
 * Returns ESP_OK only if orb_theme_geom_parse() accepted every number in it. */
static esp_err_t slot_load_geom(const slot_t *s, const esp_partition_t *p)
{
    uint32_t *blob = heap_caps_malloc(s->h.geom_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blob) blob = heap_caps_malloc(s->h.geom_size, MALLOC_CAP_8BIT);
    if (!blob) return ESP_ERR_NO_MEM;
    for (uint32_t off = 0; off < s->h.geom_size; off += CHUNK_BYTES) {
        const uint32_t want = (s->h.geom_size - off > CHUNK_BYTES) ? CHUNK_BYTES
                                                                    : (s->h.geom_size - off);
        const esp_err_t e = esp_partition_read(p, s->hdr_base + SLOT_GEOM_OFF + off,
                                               (uint8_t *)blob + off, want);
        if (e != ESP_OK) { free(blob); return e; }
    }

    /* One arena for the four tables, sized from the counts the slot declared —
     * geom_sizes_ok() already proved those counts and this length agree. */
    const size_t bytes = sizeof(clk_bg_rect_t) * (size_t)s->h.n_rects
                       + sizeof(uint16_t) * (size_t)(s->h.n_first + s->h.n_delays)
                       + sizeof(uint32_t) * (size_t)s->h.n_bgf + 16;
    uint8_t *arena = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!arena) arena = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (!arena) { free(blob); return ESP_ERR_NO_MEM; }
    /* Zeroed before the parse fills it: clock_view.c bounds some loops by clk_theme
     * numbers rather than by these lengths (it reads clk_bg_rect_first[tgt+1]), so a
     * byte the validator never saw must never be an uninitialised one. */
    memset(arena, 0, bytes);
    uint8_t *q = arena;
    s_rects    = (clk_bg_rect_t *)q; q += sizeof(clk_bg_rect_t) * (size_t)s->h.n_rects;
    s_first    = (uint16_t *)q;      q += sizeof(uint16_t) * (size_t)s->h.n_first;
    s_delays   = (uint16_t *)q;      q += sizeof(uint16_t) * (size_t)s->h.n_delays;
    s_bgframes = (uint32_t *)q;

    orb_theme_geo_t tmp = { .theme = &s_theme, .rects = s_rects, .rect_first = s_first,
                            .gif_delays = s_delays, .bg_frames = s_bgframes };
    const char *why = orb_theme_geom_parse(blob, s->h.geom_size / 4u, s->h.art_size, &tmp);
    free(blob);
    if (why) {
        free(arena);
        /* All four, not just rects: the arena is gone, and a static still pointing
         * into it is a trap for whoever reads these tables next. Nothing reads them
         * on this path — orb_theme_boot() falls back to use_baked_in(), which
         * repopulates every field of s_geo — but the dangling three were the only
         * ones that could have. */
        s_rects = NULL; s_first = NULL; s_delays = NULL; s_bgframes = NULL;
        ESP_LOGE(TAG, "%s: %s — refusing to draw it", s->h.slug, why);
        return ESP_FAIL;
    }
    /* The arena is never freed: clock_view.c caches pointers into these tables in
     * its LVGL descriptors, the same lifetime rule art_bank.c applies to the map. */
    s_geo.theme      = &s_theme;
    s_geo.rects      = s_rects;
    s_geo.rect_first = s_first;
    s_geo.gif_delays = s_delays;
    s_geo.bg_frames  = s_bgframes;
    s_geo.art_size   = s->h.art_size;
    s_geo.from_container = true;
    strlcpy(s_geo.slug, s->h.slug, sizeof(s_geo.slug));
    strlcpy(s_active_slug, s->h.slug, sizeof(s_active_slug));
    return ESP_OK;
}


/* ------------------------------------------------------------------------- *
 * The staged load, and the install that carries it out.
 * ------------------------------------------------------------------------- */

static nvs_handle_t s_h;              /* one open handle, for the boot's life */

static nvs_handle_t theme_nvs(void)
{
    if (!s_h) {
        /* The dedicated `storage` partition first, per partitions.csv's retention
         * rule. An orb still running an older table has no partition by that name,
         * and a theme feature that cannot start there is worse than settings sharing
         * esp_wifi's namespace the way they used to. */
        if (nvs_open_from_partition(THEME_NVS_PARTITION, THEME_NVS_NAMESPACE,
                                    NVS_READWRITE, &s_h) != ESP_OK) {
            if (nvs_open(THEME_NVS_NAMESPACE, NVS_READWRITE, &s_h) != ESP_OK) {
                s_h = 0;
                ESP_LOGE(TAG, "no NVS for theme settings — loads will not survive a reboot");
            }
        }
    }
    return s_h;
}

static esp_err_t nvs_put(const char *key, const char *val)
{
    const nvs_handle_t h = theme_nvs();
    if (!h) return ESP_ERR_INVALID_STATE;
    esp_err_t e = val ? nvs_set_str(h, key, val) : nvs_erase_key(h, key);
    /* Erasing a key that was never written is the normal case for "nothing staged",
     * not a failure to report. */
    if (e == ESP_ERR_NVS_NOT_FOUND && !val) e = ESP_OK;
    if (e != ESP_OK) return e;
    /* The commit is what makes a staged load survive the reboot that follows. Skip
     * it and the page says "rebooting into <theme>" and comes back with the theme it
     * already had — the worst failure this feature can have: quiet, and
     * indistinguishable from a theme that simply looked wrong. */
    return nvs_commit(h);
}

static bool nvs_get(const char *key, char *out, size_t outsz)
{
    const nvs_handle_t h = theme_nvs();
    if (!h) return false;
    out[0] = '\0';
    size_t sz = outsz;
    return nvs_get_str(h, key, out, &sz) == ESP_OK;
}

/* Build the header that makes a slot live. Built here rather than copied from the
 * file: same magic and the same two checksums (the bytes they cover are copied
 * verbatim), but the offsets describe the SLOT layout and the serial is a boot-time
 * counter. Rebuilding it is also what stops a container that lied about its own
 * offsets from surviving an install — those fields get overwritten with the values
 * this function actually wrote to. */
static void build_slot_hdr(uint32_t hdr[W_WORDS], const orb_thdr_t *h, uint32_t serial)
{
    memset(hdr, 0, W_WORDS * 4);
    hdr[W_MAGIC] = THEME_MAGIC;   hdr[W_FORMAT] = THEME_FORMAT;
    hdr[W_GEOM_ABI] = THEME_GEOM_ABI;
    hdr[W_HDR_SIZE] = W_WORDS * 4;
    hdr[W_GEOM_OFF] = SLOT_GEOM_OFF;  hdr[W_GEOM_SIZE] = h->geom_size;
    hdr[W_GEOM_CRC] = h->geom_crc;
    hdr[W_ART_SIZE] = h->art_size;    hdr[W_ART_CRC] = h->art_crc;
    hdr[W_SCR_W] = CLK_SCR_W;     hdr[W_SCR_H] = CLK_SCR_H;
    hdr[W_SERIAL] = serial;       hdr[W_ART_OFF] = SLOT_ART_OFF;
    /* The slug/title byte arrays are bounded by the struct's own field sizes, so a
     * field widened in orb_thdr_t cannot overflow the header's word range here. */
    for (int i = 0; h->slug[i] && i < (int)sizeof(h->slug) - 1; i++)
        hdr[W_SLUG  + i / 4] |= (uint8_t)h->slug[i]  << (8 * (i % 4));
    for (int i = 0; h->title[i] && i < (int)sizeof(h->title) - 1; i++)
        hdr[W_TITLE + i / 4] |= (uint8_t)h->title[i] << (8 * (i % 4));
    /* Computed last, over the whole block with the checksum field still zero. */
    hdr[W_HDR_CRC] = esp_rom_crc32_le(0, (const uint8_t *)hdr, W_WORDS * 4);
}


/* Copy the container's two parts into one slot. `fd` is open on the file and the
 * caller holds the card lock.
 *
 * ORDER IS THE WHOLE POINT. Erase, write geometry, write pixels, verify — and let
 * the CALLER write the header last, because the header is what makes a slot live. An
 * install interrupted at any point in here (power cut, card pulled, watchdog) leaves
 * the target slot headerless and therefore invalid, and the device boots the theme it
 * already had. The other slot is never touched at all.
 *
 * Erase is bounded to what is written, aligned up to the sector, rather than the
 * whole 4 MB slot: esp_partition_erase_range() runs with the cache disabled, and
 * although CONFIG_SPI_FLASH_YIELD_DURING_ERASE=y (which this build has) makes the
 * driver yield between sectors, bounding the erase is still what keeps the stall
 * proportionate to the theme being loaded instead of to the partition size. */
static esp_err_t copy_into(int fd, const orb_thdr_t *h, const esp_partition_t *p,
                           uint32_t hdr_base, const char **why)
{
    /* Same rule as verify_container(): the function that reads through s_chunk
     * reserves it, so a future caller cannot reach this loop with a NULL buffer the
     * way the upload path reached verify_container(). */
    if (ensure_chunk() != ESP_OK) { *why = "out of memory for the card read buffer"; return ESP_ERR_NO_MEM; }

    const uint32_t geom_len = (h->geom_size + THEME_SECTOR - 1) & ~(THEME_SECTOR - 1);
    const uint32_t art_len  = (h->art_size  + THEME_SECTOR - 1) & ~(THEME_SECTOR - 1);
    if (SLOT_GEOM_OFF + geom_len > SLOT_ART_OFF) { *why = "geometry would overrun the pixels"; return ESP_ERR_INVALID_SIZE; }
    if (SLOT_ART_OFF + art_len > SLOT_CAP)       { *why = "theme is larger than a slot";      return ESP_ERR_INVALID_SIZE; }

    esp_err_t e = esp_partition_erase_range(p, hdr_base + SLOT_GEOM_OFF, geom_len);
    if (e == ESP_OK) e = esp_partition_erase_range(p, hdr_base + SLOT_ART_OFF, art_len);
    if (e != ESP_OK) { *why = "flash erase failed"; return e; }

    size_t pos = h->geom_off;
    for (uint32_t done = 0; done < h->geom_size; ) {
        const uint32_t want = (h->geom_size - done > CHUNK_BYTES) ? CHUNK_BYTES
                                                                   : (h->geom_size - done);
        if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want) {
            *why = "theme file ended before its declared geometry"; return ESP_ERR_INVALID_SIZE;
        }
        e = esp_partition_write(p, hdr_base + SLOT_GEOM_OFF + done, s_chunk, want);
        if (e != ESP_OK) { *why = "flash write failed on the geometry"; return e; }
        done += want;
    }
    pos = h->art_off;
    for (uint32_t done = 0; done < h->art_size; ) {
        const uint32_t want = (h->art_size - done > CHUNK_BYTES) ? CHUNK_BYTES
                                                                  : (h->art_size - done);
        if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want) {
            *why = "theme file ended before its declared pixels"; return ESP_ERR_INVALID_SIZE;
        }
        e = esp_partition_write(p, hdr_base + SLOT_ART_OFF + done, s_chunk, want);
        if (e != ESP_OK) { *why = "flash write failed on the pixels"; return e; }
        done += want;
    }
    return ESP_OK;
}

/* The retry bound for a staged install. Without it a card that has gone missing — or
 * a theme corrupted on the card after the page staged it — is rebooted back into the
 * same failing install forever, and a device that cannot finish booting is exactly
 * the brick this feature must not become. Three attempts, then the pending key is
 * dropped and the dial boots the theme it had. */
#define THEME_MAX_FAILS 3
#define THEME_NVS_FAILS "theme_fails"

/* Pick the slot to install INTO: the one that is NOT the live theme, so the theme the
 * dial is about to fall back to is never the one being erased. With no valid slot at
 * all, B is chosen so the raw bank at partition offset 0 — what `idf.py flash` wrote
 * — survives as the fallback until something better exists. */
static void target_slot(const slot_t *a, const slot_t *b, uint32_t *hdr_base,
                        uint32_t *serial_out)
{
    uint32_t max_serial = 0;
    if (a->valid && a->h.serial > max_serial) max_serial = a->h.serial;
    if (b->valid && b->h.serial > max_serial) max_serial = b->h.serial;
    *serial_out = max_serial + 1;

    if (!a->valid && !b->valid) { *hdr_base = SLOT_B_HDR; return; }
    if (a->valid && b->valid) {
        /* Both live: replace the older one, keep the current theme installable-to. */
        *hdr_base = (a->h.serial >= b->h.serial) ? SLOT_B_HDR : SLOT_A_HDR;
        return;
    }
    *hdr_base = a->valid ? SLOT_B_HDR : SLOT_A_HDR;
}


/* Read both slot headers. */
static void slots_read(const esp_partition_t *p, slot_t *a, slot_t *b)
{
    slot_init(a, SLOT_A_HDR, SLOT_A_ART);
    slot_init(b, SLOT_B_HDR, SLOT_B_ART);
    slot_read(a, p);
    slot_read(b, p);
}

/* Make the resolved theme the one the dial draws: geometry into s_geo, pixels
 * mapped by art_bank. On any failure the caller gets the baked-in pair, which is
 * the only state in which the clock is guaranteed to come up. */
static esp_err_t adopt(const esp_partition_t *p, slot_t *s, const char *src_name)
{
    if (slot_verify_pixels(s, p) != ESP_OK) return ESP_FAIL;
    if (slot_load_geom(s, p) != ESP_OK)     return ESP_FAIL;
    s_source = src_name;
    return art_bank_init(s->art_base, s->h.art_size);
}

/* Carries out a staged install, then resolves the theme to draw. Called from
 * app_main BEFORE display_init(), so nothing has read the bank yet — a theme swap
 * therefore happens when the dial is not looking, which is what keeps the LVGL
 * task's cached sprite pointers honest. */
esp_err_t orb_theme_boot(void)
{
    const esp_partition_t *p = art_bank_partition();
    if (!p) {
        ESP_LOGE(TAG, "no \"%s\" partition — theme storage is off", ART_BANK_PARTITION_NAME);
        use_baked_in();
        return ESP_ERR_NOT_FOUND;
    }

    s_pending_slug[0] = '\0';
    nvs_get(THEME_NVS_PENDING, s_pending_slug, sizeof(s_pending_slug));

    if (s_pending_slug[0]) {
        const nvs_handle_t h = theme_nvs();
        uint8_t fails = 0;
        if (h) nvs_get_u8(h, THEME_NVS_FAILS, &fails);

        const char *why = NULL;
        orb_thdr_t h2;
        int fd = card_open(s_pending_slug, &h2, &why);
        esp_err_t e = ESP_FAIL;
        if (fd >= 0) {
            /* Checksum the copy on the card BEFORE it is written anywhere, so a
             * damaged file costs a message instead of a slot. */
            const char *vv = verify_container(fd, &h2);
            if (vv) {
                why = vv;
            } else {
                slot_t a, b; slots_read(p, &a, &b);
                uint32_t hb, serial;
                target_slot(&a, &b, &hb, &serial);
                e = copy_into(fd, &h2, p, hb, &why);
                if (e == ESP_OK) {
                    /* Verify from the PARTITION, then make the slot live. Reversing
                     * those two would let a slot go live on bytes nobody read back. */
                    slot_t probe;
                    slot_init(&probe, hb, hb + SLOT_ART_OFF);
                    memcpy(&probe.h, &h2, sizeof(h2));
                    probe.h.geom_off = SLOT_GEOM_OFF;
                    probe.h.art_off  = SLOT_ART_OFF;
                    if (slot_verify_pixels(&probe, p) != ESP_OK) {
                        why = "pixels did not read back intact — try another card";
                        e = ESP_FAIL;
                    } else {
                        /* The header block is erased HERE, not in copy_into(): erasing
                         * it earlier would invalidate the target slot for the whole
                         * multi-second copy, and erasing it later is impossible — a NOR
                         * write over already-programmed bytes ANDs the bits and yields
                         * a header whose checksum can never match. Between these two
                         * facts sits the safety property: the target slot's old header
                         * is valid right up until the new one is ready to replace it. */
                        const esp_err_t ee = esp_partition_erase_range(p, hb, THEME_SECTOR);
                        if (ee != ESP_OK) {
                            why = "could not clear the old slot header";
                            e = ee;
                        } else {
                            uint32_t hdr[W_WORDS];
                            build_slot_hdr(hdr, &h2, serial);
                            e = esp_partition_write(p, hb, hdr, sizeof(hdr));
                            if (e != ESP_OK) why = "could not make the new slot live";
                        }
                    }
                }
                ESP_LOGI(TAG, "install of \"%s\" from the card: %s",
                         s_pending_slug, e == ESP_OK ? "ok" : (why ? why : "failed"));
            }
            card_close(fd);
        }

        if (e == ESP_OK) {
            nvs_put(THEME_NVS_PENDING, NULL);
            if (h) { nvs_set_u8(h, THEME_NVS_FAILS, 0); nvs_commit(h); }
            s_pending_slug[0] = '\0';
        } else {
            if (h) { nvs_set_u8(h, THEME_NVS_FAILS, (uint8_t)(fails + 1)); nvs_commit(h); }
            ESP_LOGE(TAG, "theme install attempt %u of %d failed", (unsigned)fails + 1,
                     THEME_MAX_FAILS);
            if (fails + 1 >= THEME_MAX_FAILS) {
                nvs_put(THEME_NVS_PENDING, NULL);
                if (h) { nvs_set_u8(h, THEME_NVS_FAILS, 0); nvs_commit(h); }
                s_pending_slug[0] = '\0';
                ESP_LOGE(TAG, "giving up on that install — the dial keeps its theme");
            }
        }
    }

    slot_t a, b;
    slots_read(p, &a, &b);
    slot_t *live = NULL;
    if (a.valid && b.valid) live = (a.h.serial >= b.h.serial) ? &a : &b;
    else if (a.valid)       live = &a;
    else if (b.valid)       live = &b;

    if (!live) {
        /* Logged at INFO, not silently: after a failed install this is the line that
         * says which theme is actually on the wall. */
        ESP_LOGI(TAG, "no installed theme — drawing the baked-in bank");
        use_baked_in();
        return art_bank_init(0, s_geo.art_size);
    }
    if (adopt(p, live, live == &a ? "slot-A" : "slot-B") == ESP_OK) {
        ESP_LOGI(TAG, "theme \"%s\" from %s, %u B of pixels",
                 s_active_slug, s_source, (unsigned)live->h.art_size);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "\"%s\" would not load — falling back", live->h.slug);
    use_baked_in();
    return art_bank_init(0, s_geo.art_size);
}


/* ------------------------------------------------------------------------- *
 * The page-facing API.
 * ------------------------------------------------------------------------- */

esp_err_t orb_theme_stage_load(const char *slug)
{
    if (!orb_sd_name_ok(slug)) return ESP_ERR_INVALID_ARG;

    /* Validated in full — both checksums, over the whole file — before anything is
     * written down. The alternative is a page that says "rebooting" and comes back
     * on the theme it had, because the file was damaged and the install refused it
     * three boots later. */
    const char *why = NULL;
    orb_thdr_t h;
    const int64_t t0 = esp_timer_get_time();
    const int fd = card_open(slug, &h, &why);
    if (fd < 0) {
        ESP_LOGW(TAG, "stage \"%s\": %s", slug, why ? why : "unreadable");
        return ESP_FAIL;
    }
    const char *vv = verify_container(fd, &h);
    card_close(fd);
    /* After the close, and timed. Both ends of the wait matter on the serial line: this
     * is the request that holds the one httpd worker for seconds (a 3.87 MB container
     * reads off the card at ~1.34 MB/s at SPI 20 MHz), so the number says how long
     * /api/v1/sd can take to answer behind it, and the line only appearing after the
     * give is what proves the mutex came back. */
    ESP_LOGI(TAG, "stage \"%s\": container verified in %lld ms", slug,
             (long long)((esp_timer_get_time() - t0) / 1000));
    if (vv) {
        ESP_LOGW(TAG, "stage \"%s\": %s", slug, vv);
        return ESP_ERR_INVALID_CRC;
    }

    /* The file's own slug must match the name it is filed under: two entries claiming
     * to be the same theme is a card somebody has copied by hand, and the page lists
     * one while flash holds the other. */
    if (strcmp(h.slug, slug) != 0) {
        ESP_LOGW(TAG, "stage \"%s\": the file calls itself \"%s\" — rename one of them",
                 slug, h.slug);
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t e = nvs_put(THEME_NVS_PENDING, slug);
    if (e != ESP_OK) return e;
    const nvs_handle_t hnd = theme_nvs();
    if (hnd) { nvs_set_u8(hnd, THEME_NVS_FAILS, 0); nvs_commit(hnd); }   /* fresh attempt count */
    strlcpy(s_pending_slug, slug, sizeof(s_pending_slug));
    ESP_LOGI(TAG, "staged \"%s\" — reboot to install it", slug);
    return ESP_OK;
}

esp_err_t orb_theme_unstage(void)
{
    const esp_err_t e = nvs_put(THEME_NVS_PENDING, NULL);
    const nvs_handle_t h = theme_nvs();
    if (h) { nvs_set_u8(h, THEME_NVS_FAILS, 0); nvs_commit(h); }
    s_pending_slug[0] = '\0';
    return e;
}

esp_err_t orb_theme_delete(const char *slug)
{
    char path[ORB_SD_PATH_MAX];
    if (!theme_file(slug, path, sizeof path)) return ESP_ERR_INVALID_ARG;
    if (orb_sd_ensure_mounted() != ESP_OK) return ESP_ERR_NOT_FOUND;

    /* Deleting the theme that is on the wall is allowed and must be: the installed
     * copy is already in flash and does not need the card. But it is the case where
     * "delete" looks like it changed nothing, because the dial keeps drawing that
     * theme until another is loaded — so it is logged, and the page says so too. */
    orb_sd_lock();
    const int r = unlink(path);
    const int err = errno;
    orb_sd_unlock();
    if (r != 0) {
        ESP_LOGW(TAG, "delete \"%s\": %s", slug, strerror(err));
        return err == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    ESP_LOGI(TAG, "deleted \"%s\" from the card%s", slug,
             strcmp(s_active_slug, slug) == 0 ? " (still installed on the dial)" : "");
    return ESP_OK;
}

int orb_theme_scan(orb_theme_entry_t *out, int cap)
{
    /* While an upload owns the card, do not open a second file on it: FatFs is
     * reentrant per volume, but this walk plus a 3.7 MB write on the same
     * directory would serialise the httpd worker behind the upload for seconds, and
     * a page that cannot poll its own list looks exactly like a device that has
     * stopped answering. The caller reports "uploading" instead. */
    if (orb_theme_upload_busy()) return -1;
    if (orb_sd_ensure_mounted() != ESP_OK) return -1;

    orb_sd_lock();
    /* Both ends logged, with the wait: a scan that never comes back is the one card
     * failure that looks like a dead web server from outside (the dial keeps drawing
     * and ping keeps replying), and which of the two lines is missing says whether the
     * worker is waiting on the mutex or inside FatFs. */
    const int64_t t_scan = esp_timer_get_time();
    ESP_LOGI(TAG, "listing %s", ORB_SD_THEME_DIR);
    DIR *d = opendir(ORB_SD_THEME_DIR);
    if (!d) {
        orb_sd_unlock();
        /* The directory not existing is the no-card-ever-used state, not an error:
         * orb_sd_init() creates it at mount, so an absent one means the volume came
         * up read-only or somebody formatted it on a laptop. */
        ESP_LOGW(TAG, "%s is not a directory (%s)", ORB_SD_THEME_DIR, strerror(errno));
        return -1;
    }

    int n = 0;
    struct dirent *de;
    while (n < cap && (de = readdir(d)) != NULL) {
        /* Only <slug>.orbtheme, and the slug is taken from BEFORE the extension so a
         * file named from the page can be deleted and loaded by what it is listed as. */
        const size_t len = strlen(de->d_name);
        const size_t ext = sizeof(ORB_THEME_EXT) - 1;
        if (len <= ext || strcmp(de->d_name + (len - ext), ORB_THEME_EXT) != 0) continue;
        char slug[ORB_THEME_SLUG_MAX + 1];
        if (len - ext > sizeof(slug) - 1) continue;
        memcpy(slug, de->d_name, len - ext);
        slug[len - ext] = '\0';
        if (!orb_sd_name_ok(slug)) continue;                 // hides junk, not themes

        orb_thdr_t h;
        const char *why = NULL;
        ESP_LOGI(TAG, "checking \"%s\" on the card", slug);
        const int fd = card_open_locked(slug, &h, &why);   /* this walk already holds the lock */
        if (fd < 0) {
            /* Listed with its reason rather than skipped. "no themes on the card" and
             * "three themes, all damaged" are different problems — the card's or the
             * bake's — and hiding the entries is what makes that undiagnosable. The
             * reason goes in .why, NOT in .title: the page shows the title as a label
             * and the reason as a warning, and overloading one field with the other
             * means a damaged theme is listed under the name "geometry checksum". */
            if (!why) why = "unreadable";
            strlcpy(out[n].slug, slug, sizeof(out[n].slug));
            out[n].title[0] = '\0';
            strlcpy(out[n].why, why, sizeof(out[n].why));
            out[n].art_size = 0; out[n].serial = 0;
            out[n].err = ESP_ERR_INVALID_ARG;
            out[n].active = false; out[n].pending = false;
            n++;
            continue;
        }
        card_close_locked(fd);
        strlcpy(out[n].slug, slug, sizeof(out[n].slug));
        strlcpy(out[n].title, h.title[0] ? h.title : slug, sizeof(out[n].title));
        out[n].why[0] = '\0';
        out[n].art_size = h.art_size;
        out[n].serial   = h.serial;
        out[n].err      = ESP_OK;
        out[n].active   = strcmp(slug, s_active_slug) == 0;
        out[n].pending  = strcmp(slug, s_pending_slug) == 0;
        n++;
    }
    closedir(d);
    orb_sd_unlock();
    ESP_LOGI(TAG, "%d entr%slisted in %lld ms", n, n == 1 ? "y " : "ies ",
             (long long)((esp_timer_get_time() - t_scan) / 1000));
    return n;
}

/* ------------------------------------------------------------------------- *
 * Uploading a theme from the browser.
 * ------------------------------------------------------------------------- */

struct orb_theme_up {
    int    fd;
    char   tmp[ORB_SD_PATH_MAX];
    char   final[ORB_SD_PATH_MAX];
    uint32_t got;
};

static orb_theme_up_t *s_up;                  /* the one upload in progress */

bool orb_theme_upload_busy(void) { return s_up != NULL; }

esp_err_t orb_theme_upload_begin(const char *slug, orb_theme_up_t **out)
{
    *out = NULL;
    if (s_up) return ESP_ERR_INVALID_STATE;                /* one at a time */
    if (orb_sd_ensure_mounted() != ESP_OK) return ESP_ERR_NOT_FOUND;
    /* Reserve the card read buffer NOW, before the radio carries 3.87 MB for a
     * transfer this device cannot finish. verify_container() reserves it again
     * defensively; this is the early, cheap no. */
    if (ensure_chunk() != ESP_OK) return ESP_ERR_NO_MEM;

    orb_theme_up_t *u = calloc(1, sizeof *u);
    if (!u) return ESP_ERR_NO_MEM;
    /* The .part name is built from the slug here, so neither half of it can come
     * from the client: the page sends ?name=, and the destination is whatever this
     * function decides. */
    if (!theme_file(slug, u->final, sizeof u->final)) { free(u); return ESP_ERR_INVALID_ARG; }
    /* Built by hand rather than with "%s.part" on u->final: gcc is right that the
     * result can outgrow the buffer, and the honest fix is to measure it and refuse
     * the slug, not to enlarge the array and truncate quietly. orb_sd_name_ok already
     * caps the slug at 31 bytes, so this cannot trigger in practice — which is exactly
     * why it must still be checked, since the alternative on overflow is a .part
     * landing on SOME file's path. */
    const size_t flen = strlen(u->final);
    if (flen + 6 >= sizeof u->tmp) { free(u); return ESP_ERR_INVALID_SIZE; }
    memcpy(u->tmp, u->final, flen);
    memcpy(u->tmp + flen, ".part", 6);

    orb_sd_lock();
    u->fd = open(u->tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (u->fd < 0) {
        const int e = errno;
        orb_sd_unlock(); free(u);
        ESP_LOGW(TAG, "upload %s: create failed (%s)", slug, strerror(e));
        return ESP_FAIL;
    }
    s_up = u;
    *out = u;
    ESP_LOGI(TAG, "uploading a theme to the card as \"%s\"", slug);
    return ESP_OK;
}

esp_err_t orb_theme_upload_data(orb_theme_up_t *u, const void *buf, size_t n)
{
    if (!u || u != s_up) return ESP_ERR_INVALID_STATE;
    if (u->got + n > ORB_THEME_UPLOAD_MAX) {
        /* Refused before the write, not after: a card has finite free space and an
         * attacker with a browser has an infinite body, and the one that costs
         * nothing to stop is the one stopped on a length. */
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *p = buf;
    while (n) {
        const ssize_t w = write(u->fd, p, n);
        if (w <= 0) return ESP_FAIL;          /* ENOSPC and friends: caller reports it */
        p += w; n -= (size_t)w; u->got += (uint32_t)w;
    }
    return ESP_OK;
}

esp_err_t orb_theme_upload_end(orb_theme_up_t **u, const char **why)
{
    orb_theme_up_t *const up = *u;
    *why = NULL;
    if (!up || up != s_up) { *why = "no upload is in progress"; return ESP_ERR_INVALID_STATE; }

    close(up->fd);
    s_up = NULL;                     /* the card is still locked; released below */

    /* Verify the file that is NOW on the card — not the bytes that passed through
     * this handler. A card that dropped a write, or a client that lied about the
     * length, is caught here rather than by the install three boots later. */
    const int fd = open(up->tmp, O_RDONLY);
    if (fd < 0) {
        *why = "the uploaded file is not on the card";
        unlink(up->tmp); orb_sd_unlock(); free(up);
        return ESP_FAIL;
    }
    orb_thdr_t h;
    size_t pos = 0;
    uint32_t hdr[W_WORDS];
    const char *r = NULL;
    if (rd_at(fd, &pos, (uint8_t *)hdr, sizeof hdr) != (ssize_t)sizeof hdr) r = "too short to be a theme";
    if (!r) r = hdr_parse(hdr, &h);
    if (!r && (h.geom_off != W_WORDS * 4u || h.art_off < h.geom_off + h.geom_size ||
               h.art_off % THEME_SECTOR))
        r = "container parts are not where this format puts them";
    uint32_t c[4];
    if (!r) {
        pos = h.geom_off;
        if (rd_at(fd, &pos, (uint8_t *)c, 16) != 16) r = "geometry blob is missing";
        else {
            h.n_rects = c[0]; h.n_first = c[1]; h.n_delays = c[2]; h.n_bgf = c[3];
            if (!geom_sizes_ok(h.geom_size, c[0], c[1], c[2], c[3]))
                r = "geometry blob size does not match its own tables";
        }
    }
    if (!r) r = verify_container(fd, &h);
    close(fd);

    if (r) {
        /* Deleted, not left: a rejected upload that survives on the card shows up in
         * the list as an unloadable entry, and the owner has to guess whether they
         * uploaded it or somebody else did. */
        unlink(up->tmp);
        orb_sd_unlock();
        *why = r;
        ESP_LOGW(TAG, "upload rejected: %s", r);
        free(up);
        return ESP_ERR_INVALID_CRC;
    }

    /* f_rename() fails on an existing destination (FR_EXIST), so a re-upload of the
     * same theme name is an unlink plus a rename rather than a replace. Not atomic
     * together: the window between them is the one moment this slug does not exist
     * on the card, and it is a fraction of a millisecond on a device nobody is
     * pulling the card from mid-upload — the alternative, renaming over a live
     * theme and discovering the checksum mismatch afterwards, destroys the theme
     * that was working. */
    unlink(up->final);
    const int rr = rename(up->tmp, up->final);
    const int re = errno;
    orb_sd_unlock();
    free(up);
    if (rr != 0) {
        *why = "could not put the uploaded theme in place";
        ESP_LOGE(TAG, "rename: %s", strerror(re));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "theme uploaded to the card");
    return ESP_OK;
}

void orb_theme_upload_cancel(orb_theme_up_t **u)
{
    orb_theme_up_t *const up = *u;
    if (!up) return;
    *u = NULL;
    if (up == s_up) {
        close(up->fd);
        s_up = NULL;
        /* The partial file is always removed: the next attempt truncates it anyway,
         * but a .part left here is a file with the theme's name and none of its
         * bytes, which is the kind of thing that gets found months later and
         * investigated as corruption. */
        unlink(up->tmp);
        orb_sd_unlock();
    }
    free(up);
}
