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
 * INSTALL IS A/B, INSIDE THE `assets` PARTITION. Two slots, each a 4 KB header
 * followed by a 64 KB-aligned payload:
 *
 *     slot A  header @ 0x000000   payload @ 0x00010000
 *     slot B  header @ 0x00400000 payload @ 0x00410000
 *
 * The live slot is the one whose header magic, format, geometry ABI, sizes,
 * header CRC32 and payload CRC32 ALL check, with the higher serial. Payload and
 * geometry are written and read back verified BEFORE the header, so a load that
 * is interrupted — by a power cut, by a card pulled, by a crash — leaves the
 * OTHER slot's header intact and the device boots the theme it had. That is the
 * whole reason it is two slots: one slot plus an erase is a theme feature that
 * can brick the dial of a device in a sealed case, and the dial is the product.
 *
 * A board flashed the traditional way (raw bank at partition offset 0, no header
 * anywhere) reports source "baked-in", reads its geometry from clock_assets.h,
 * and installs its first theme into slot B so that bank is never clobbered while
 * it is still the only thing that works.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "clock_assets.h"   /* clk_theme_t, clk_bg_rect_t + the baked-in fallback */

#define ORB_THEME_EXT      ".orbtheme"
#define ORB_THEME_SLUG_MAX 31      /* == ORB_SD_NAME_MAX: one path segment */

/* Partition offsets inside `assets`, both 64 KB aligned so esp_partition_mmap
 * wastes no MMU entry on rounding. ORB_THEME_SLOT_CAP is what a theme has to fit
 * in: the shipped bank is 3.68 MB of its 4 MB, so check this number before
 * believing any particular theme will install. */
#define ORB_THEME_SLOT_A_HDR  0x000000u
#define ORB_THEME_SLOT_A_ART  0x00010000u
#define ORB_THEME_SLOT_B_HDR  0x00400000u
#define ORB_THEME_SLOT_B_ART  0x00410000u

/** What clock_view.c reads in place of the generated header's constants. */
typedef struct {
    clk_theme_t   *theme;
    clk_bg_rect_t *rects;
    uint16_t      *rect_first;
    uint16_t      *gif_delays;
    uint32_t      *bg_frames;
    size_t         art_size;          /* pixel bytes, not partition bytes */
    bool           from_container;    /* false = the compiled-in pair */
    char           slug[ORB_THEME_SLUG_MAX + 1];
} orb_theme_geo_t;

/* The five accessors clock_view.c reads through — clk_theme, clk_bg_rects,
 * clk_bg_rect_first, clk_gif_delays, clk_bg_frames, CLK_BLOB_BYTES — live in
 * orb_theme_geo.h, NOT here. They cannot be here: assets/clock_assets.h defines
 * those very names, and a macro would expand inside its own definition line.
 * clock_view.c includes the generated header first and the macro header second. */

/**
 * Resolve the geometry and locate the bank to boot with; carries out any load
 * the page staged. Call BEFORE clock_view_init(), which maps the bank — a theme
 * swap is an erase-and-rewrite of a multi-megabyte region, and doing it while the
 * LVGL task is reading pixels out of that same region is how a dial ends up
 * showing half of one theme and half of another.
 */
esp_err_t orb_theme_boot(void);

/** The resolved geometry; valid from orb_theme_boot() onwards, never NULL. */
const orb_theme_geo_t *orb_theme_geo(void);

/** "slot-A", "slot-B" or "baked-in": what the dial is drawing from. */
const char *orb_theme_source(void);
const char *orb_theme_active_slug(void);
const char *orb_theme_pending_slug(void);

/**
 * Stage a card theme for install at the next boot. The whole container is
 * streamed and both checksums verified FIRST, so a damaged file answers with an
 * error here instead of costing a reboot into a slot that will not validate.
 */
esp_err_t orb_theme_stage_load(const char *slug);
esp_err_t orb_theme_unstage(void);

typedef struct {
    char      slug[ORB_THEME_SLUG_MAX + 1];
    char      title[40];
    char      why[56];            /* why it is unloadable; empty when err == ESP_OK */
    uint32_t  art_size;
    uint32_t  serial;
    esp_err_t err;              /* ESP_OK = loadable */
    bool      active;
    bool      pending;
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

esp_err_t orb_theme_upload_begin(const char *slug, orb_theme_up_t **out);
esp_err_t orb_theme_upload_data(orb_theme_up_t *u, const void *buf, size_t n);
/* Verifies the whole .part file and renames it into place. On failure nothing is
 * committed and the .part is removed, so a bad upload cannot be loaded by mistake. */
esp_err_t orb_theme_upload_end(orb_theme_up_t **u, const char **why);
void      orb_theme_upload_cancel(orb_theme_up_t **u);

/* True while an upload owns the card, so the status endpoint can answer instead of
 * blocking the httpd worker behind the write. */
bool orb_theme_upload_busy(void);

/** Delete one theme file from the card. Only ever /sd/themes/<slug>.orbtheme. */
esp_err_t orb_theme_delete(const char *slug);

#define ORB_THEME_SLOT_CAP    0x003F0000u   /* 4 MB less the 64 KB header block */
