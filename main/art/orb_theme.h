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
 * ONE 8 MB SLOT AT THE HEAD OF THE `assets` PARTITION (Migration 1).
 * A single slot holds one theme: a 4 KB header at offset 0, geometry tables at
 * 0x1000, pixel banks at 0x10000, an 8 MB ceiling. There is no second slot and no
 * A/B switch. The `assets` partition row in partitions.csv does not change — the
 * retired A/B pair was two regions inside the same 8 MB partition.
 *
 * A store erases the slot, writes the payload and geometry, reads them back and
 * checksums them, and writes the header LAST. A store interrupted at any point —
 * power cut, card pulled, a killed upload — leaves the slot headerless and invalid,
 * so the next boot falls to the base digital face and one re-store recovers it. The
 * loading page during a store is what protects the live pixels: there are no live
 * pixels to protect, because a store IS the theme change and the only answer to a
 * torn store is to press it again.
 *
 * The choice of which theme the dial draws is gone: the one slot holds the one
 * theme. `orb_theme_source()` answers "slot" when that theme is a valid drawable
 * bundle and "none" otherwise. The `theme_live` NVS key and the damage-reporting
 * pair are retired; `theme_staged` survives as a witness that a store was armed and
 * never completed. A board carrying old slot-B-only data boots themeless once and
 * re-stores.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "clk_types.h"   /* clk_theme_t, clk_bg_rect_t + the baked-in fallback */
#include "orb_bundle.h"  /* orb_bundle_sec_t — orb_theme_live_sec() hands one back */
#include "orb_assets.h"  /* orb_asset_ans_t, ORB_ASSET_PREVIEW — orb_theme_preview_read answers */

#define ORB_THEME_EXT      ".orbtheme"
#define ORB_THEME_NAME_MAX 56      /* the one name field in a FORMAT-2 header */
#define ORB_THEME_ID_HEX   12      /* sha256("<slug>/<name>")[:12] — the stem's identity tail */
/* The card filename stem: "<slug>-<token>-<ID_HEX>", the app that owns the theme,
 * a legible cut of the theme name, and the app-scoped hash that makes the pair a
 * handle. ORB_THEME_HANDLE_MAX is the stem's length ceiling, and it equals
 * ORB_SD_NAME_MAX by rule — a theme name the validator would refuse is a file
 * the card cannot hold — pinned by an assert in orb_theme.c. */
#define ORB_THEME_HANDLE_MAX 55
#define ORB_THEME_APP_MAX    23      /* ORB_BUNDLE_SLUG_MAX: the stem's leading token */

/* Partition offsets inside `assets`. One slot at the head of the partition: a 4 KB
 * header at offset 0, 64 KB-aligned geometry at 0x1000 and pixels at 0x10000. See
 * orb_theme.c's SLOT_* defines for the authoritative layout. ORB_THEME_SLOT_CAP is
 * what a theme has to fit in; the 8 MB ceiling is the whole `assets` partition.
 * (Migration 1 retired the old A/B slot pair — two 4 MB regions inside this same 8 MB
 * partition — so the header no longer names a slot letter.) */

/* The complication/aux record types and the CLK_KIND_/CLK_SRC_/CLK_FLAG_
 * vocabularies moved to clk_types.h with D32 slice 2: the FORMAT 3 projection
 * fills them from a bundle's BANK rows, and that file compiles on a host with
 * nothing but clk_types.h and orb_bundle.h. This header includes it. */

/* What clock_view.c reads in place of the generated header's constants. */
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
     * the same reason rects is: orb_clock3_parse() fills them in place. */
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
 * Read the one slot's header, verify its bytes, and hand back the geometry the
 * dial draws. Call BEFORE clock_view_init(), which maps the bank — this reads the
 * same multi-megabyte region the paint path is about to map, and resolving first is
 * what stops the dial drawing half of one theme and half of another.
 *
 * It reads and verifies ONLY. Nothing installs from the card here (D13): the boot
 * path that opened the card, copied a container and kept a retry counter was a boot
 * that could spend seconds on a card it did not need and could be left half-installed
 * by a power cut. Storing is orb_theme_install_bundle()'s job, and the only thing
 * that decides when it runs is a press.
 */
esp_err_t orb_theme_boot(void);

/** The resolved geometry; valid from orb_theme_boot() onwards, never NULL. */
const orb_theme_geo_t *orb_theme_geo(void);

/** The residency verdict: true only when boot adopted a valid, drawable theme and
 *  published its geometry. Distinct from art_bank_ready(), which answers only
 *  whether a pixel span is mapped — a mapped span whose clock projection refused
 *  is bytes with no dial, and a fallback that tests only the mapping walks the
 *  themed leg over them. A themed leg needs both: see clock_view_init()'s gate. */
bool orb_theme_resident(void);

/** The adopted bundle's section for `slug`, with the offset at which that
 *  section's own art bank begins inside the payload art_bank_ptr() maps. True
 *  fills both; false (a base-face dial, or a bundle carrying no such section)
 *  leaves them untouched. The returned section is the reader's live view of the
 *  adopted bytes: walk it with orb_bundle_elem_at() and orb_elems_at(), read
 *  only, and expect it to stand for the life of the adoption — a flip reboots,
 *  so it stands for the life of the boot. This is the door an app resolves its
 *  elements through; the geometry adapter is what the clock's legacy path
 *  reads until Migration 2 deletes it. */
bool orb_theme_live_sec(const char *slug, const orb_bundle_sec_t **sec,
                        uint32_t *bank_off);


/** The theme's own preview plane, read transiently out of its `.orbtheme` card
 *  file. The OS's per-app picker calls it on the worker when the cursor lands on
 *  a row (never the LVGL task: a card read while another task holds the mutex
 *  freezes the dial for the hold's length). The plane is class `ORB_ASSET_PREVIEW`
 *  (9) at slot 0, one frame, RGB565 (2 B/px), and lives in the theme's own section
 *  bank. Any app's picker may resolve its themes through this call: the handle's
 *  leading token names the section the reader finds, and the class is fixed at 9
 *  so a bundle that files the same handle under another class refuses with
 *  ORB_ASSET_CLASS rather than painting the wrong layer at the right address.
 *
 *  The caller supplies `buf` sized for the preview it will draw. `cap` is that
 *  size in bytes. `*w` and `*h` come back as the plane's own extent on OK; the
 *  reader refuses with ORB_ASSET_ROOM if `cap` is smaller than `w * h * 2` so the
 *  caller can log the actual figure rather than draw a torn preview. When the
 *  theme carries no preview row (the common case: `preview` is optional by design),
 *  the answer is ORB_ASSET_NO_CONTENT and the row stays text-only — an absent
 *  preview is never an error. When the handle's file is missing, corrupt, or filed
 *  under a different stem than it names, the answer is ORB_ASSET_NO_SLOT and
 *  `why` carries the sentence (bounded by `whysz`; a NULL `why` asks for the answer
 *  alone). Any other answer means the file is on the card and the row is not
 *  drawable as a preview: a caller that shows status lines prints `why`, and a
 *  caller that does not still falls back to the text row it would have shown
 *  before the plane existed.
 *
 *  If `buf`/`*w`/`*h` are NULL the reader still verifies the row's presence and
 *  the caller's cap fits it, and returns ORB_ASSET_OK with nothing copied: the
 *  picker can use the shape (the plane's own extent in `*w`/`*h`) to size a PSRAM
 *  buffer before the real read, in one call and one card open.
 *
 *  A transient read: opens the card, mmaps the region into a PSRAM scratch,
 *  parses, copies the plane into `buf`, and returns. Nothing here stays resident
 *  between cursor moves, and no lock outlives the call. */
orb_asset_ans_t orb_theme_preview_read(const char *handle,
                                       uint8_t *buf, size_t cap,
                                       uint16_t *w, uint16_t *h,
                                       char *why, size_t whysz);


/** "slot" when the dial draws the slot's bundle, "none" for the base face. */
const char *orb_theme_source(void);
/** The owner name of the theme currently on the wall, "" for the base face. */
const char *orb_theme_active_name(void);

/* True when `id` is a card filename stem this device could have issued: a legal
 * app slug, a '-', a legible token carrying no dot, and a tail of exactly
 * ORB_THEME_ID_HEX chars of [0-9a-f]. The token between the pair is NOT checked
 * — it is decoration, and a hand rename that keeps the slug and the hex keeps
 * the theme addressable. The HTTP handlers call it to reject a malformed id at
 * the edge; install/delete re-check it on the resolve path. */
bool orb_theme_id_ok(const char *id);

/** D30's `staged` word (§20.4): the card stem of a store whose pixels reached
 *  the slot and whose header never did, and the sentence for its row. ""/"" when
 *  nothing is staged. Read-only — under the single slot the word is only a witness
 *  that a store was armed and never completed; boot draws the base face and one
 *  re-store clears the word. */
const char *orb_theme_staged_id(void);
const char *orb_theme_staged_why(void);

/** The boot arena refill §20.5(2) measured: whole milliseconds of the fused
 *  read-and-CRC pass over the chosen slot, and the bytes it moved. Both 0 on a
 *  boot that drew the default, which maps instead of refilling (D13). */
uint32_t orb_theme_refill_ms(void);
uint32_t orb_theme_refill_bytes(void);

/** C3 (§5.3): true from the store's unmap of the primary bank until the reboot
 *  that re-establishes it. Between those two points the theme's pixels are gone
 *  for this boot, so the dial must not be re-posed from cached descriptors —
 *  clock_view's bounce refuses to wake on it. False until a store parks, and
 *  false again only on the next boot. */
bool orb_theme_store_torn(void);

/** The last streaming bundle store (P6.4): milliseconds of the fused card-to-slot
 *  pass and the bytes of art it moved. Both 0 until a bundle installs. The page
 *  reads the pair so the §20.5(1) copy rate needs no serial cable. */
uint32_t orb_theme_bundle_ms(void);
uint32_t orb_theme_bundle_bytes(void);

/** Put a FORMAT 3 app bundle in the single slot: streaming ingest (§22.3, P6.3).
 *  Each section's bank is checksummed as its bytes cross from the card, the region
 *  is verified before the first erase, and the installed bytes are read back and
 *  checksummed from the partition before the header makes them live.
 *
 *  This is the only theme-store door, and it writes the ONE slot at offset 0. It
 *  makes no choice and reboots on its own; the caller that posts it asks for the
 *  reboot on success (orb_jobs_reboot_soon), because under the single slot the
 *  stored bundle IS the theme the next boot draws. A store that returns an error
 *  left the slot headerless — the base face on the next boot, recoverable by
 *  pressing again.
 *
 *  Returns the esp_err_t/`why` pair the picker and page show. */
esp_err_t orb_theme_install_bundle(const char *id, char *why, size_t whysz);

/**
 * Queue the streaming bundle store (orb_theme_install_bundle) on the shell's one
 * worker, then reboot on success. This is the ONLY way anything below main/ triggers
 * a theme change: the dial's picker and the browser's Store and upload doors all post
 * here, so the knob and the page cannot hold two opinions about what picking a theme
 * does. A stored bundle is the theme the next boot draws, so a successful store asks
 * for the reboot that draws it.
 *
 * Returns what the queue said — ESP_ERR_INVALID_STATE when another job is
 * outstanding, ESP_ERR_NO_MEM when the queue refused — and never blocks, so a
 * page handler may call it from the httpd task and the shell's poll from the LVGL
 * one. The reason a refused store costs is in orb_theme_try_why(), which the
 * row and the page read as one string.
 */
esp_err_t orb_theme_post_bundle(const char *id);

/** What the last pick could not do: the id it was asked for and the sentence
 *  the ladder answered with. Both "" after a success, and the picker puts them on
 *  the row that was pressed rather than in a status line nothing can reach. */
const char *orb_theme_try_id(void);
const char *orb_theme_try_why(void);

typedef struct {
    char      name[ORB_THEME_NAME_MAX + 1];  /* the owner's name, from the header */
    char      id[ORB_THEME_HANDLE_MAX + 1];  /* "<slug>-<token>-<hex12>": the card stem,
                                              * the handle every later request carries */
    char      app[ORB_THEME_APP_MAX + 1];    /* the stem's leading token: which app owns
                                              * this theme; "" when the name did not
                                              * parse and the row is only a refusal */
    char      why[72];            /* why it is unloadable; empty when err == ESP_OK */
    uint32_t  art_size;
    uint32_t  serial;
    uint32_t  fmt;                /* 2 or 3, from the FORMAT word; 0 = unreadable.
                                   * A fmt 3 row is a BUNDLE: loadable through
                                   * orb_theme_post_bundle(), never through the
                                   * picker, and never active until it is the one
                                   * theme in the slot. */
    uint32_t  n_sections;         /* fmt 3 only: the index's app count; 0 for v2 */
    esp_err_t err;              /* ESP_OK = loadable */
    bool      active;           /* this file is what the dial is drawing */
    bool      changed;          /* the installed copy of this name has different bytes */
    bool      stored;           /* the single slot holds this file's bytes.
                                 * Migration 1: repurposed from the A/B 'A'/'B' slot
                                 * designator to a plain "is this the stored theme".
                                 * With one slot it is the stem's app-scoped id
                                 * re-derived from the slot's stored name. */
} orb_theme_entry_t;

/**
 * List /sd/themes, up to cap entries; -1 when the card is not mounted. A bad
 * entry is RETURNED with its err set rather than hidden — "no themes" and "three
 * themes, all damaged" are different problems (the card's vs the bake's) and the
 * page has to be able to tell the owner which one they have.
 */
int orb_theme_scan(orb_theme_entry_t *out, int cap);

/** How many files the last scan's walk saw carrying the theme extension at all,
 *  the listing cap and other apps' files included: the "shown of on the card"
 *  pair. 0 before the first successful scan. */
int orb_theme_scan_counted(void);

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

/* The whole `assets` partition (8 MB) plus room for the header and the alignment
 * gap. A bigger body is refused on Content-Length, before a byte of it reaches the
 * card. The single slot's payload ceiling is 8 MB less the 64 KB header block. */
#define ORB_THEME_UPLOAD_MAX  (0x00800000u + 0x20000u)

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
 * .part is removed, so a bad upload cannot be loaded by mistake. On success the
 * committed card stem is copied into `committed_id` (buffer ORB_THEME_HANDLE_MAX
 * + 1, may be NULL); orb_theme_upload_begin
 * already armed the slot invalid, so the caller re-stores this id (post_bundle) to land
 * the theme and reboot. */
esp_err_t orb_theme_upload_end(orb_theme_up_t **u, bool overwrite, const char **why,
                               char *committed_id);
void      orb_theme_upload_cancel(orb_theme_up_t **u);

/* The confirmed answer to a duplicate refusal, with no second body. upload_end keeps
 * the verified scratch (upload.part) when it answers ORB_THEME_UPLOAD_E_DUP, and one
 * one-shot token names it: orb_theme_staged_commit() is that token, empty when no
 * scratch waits. recommit() accepts an upload only carrying that token, re-verifies
 * the scratch from the card, and files it exactly as a completed upload would — so a
 * replaced theme crosses the radio once, not twice. Any error (ESP_ERR_NOT_FOUND for
 * a scratch that is gone or no longer tokened) means the caller must upload for real;
 * the bytes of the answer are never a client's guess: the token came from this
 * module's own RNG. */
esp_err_t orb_theme_upload_recommit(const char *token, const char **why,
                                    char *committed_id, uint32_t *bytes);
const char *orb_theme_staged_commit(void);   /* "" when nothing is staged */

/* True while an upload owns the card, so the status endpoint can answer instead of
 * blocking the httpd worker behind the write. */
bool orb_theme_upload_busy(void);

/* Put the copy-to-card progress on the glass. The upload is the longest wait an
 * owner sees with the dial frozen, and it used to name nothing: these park the
 * compositor the way the store's park does, black the panel, and draw the
 * overlay's stage frames until the body has landed. park() returns false if
 * the park could not take the glass (a resume raced it), in which case the
 * upload must proceed without the frames rather than fight the compositor; it
 * has already then returned the dial. Call park() after upload_begin succeeds
 * (the slot arms the moment the file opens, so the base-face promise is already
 * made), and unpark() on every refusal after it. A completed upload does NOT
 * unpark: the store the door queues takes the glass from here and reboots.
 *
 * `stage` names the wait in the one word the band shows: "copying" for the
 * card uploads this was built for, "updating" for the OTA push that borrowed
 * the same discipline, "rebooting" for the gap a POST /api/v1/reboot opens.
 * The overlay draws it as-is from its compiled font, so lowercase and short. */
bool orb_theme_upload_park(uint32_t total, const char *stage);
void orb_theme_upload_progress(uint32_t done, uint32_t total);
void orb_theme_upload_unpark(void);

/** Delete one theme file from the card by its device-issued id (12 hex chars),
 *  resolved by walking the themes directory and re-verified against the file's
 *  header name. Never builds a path from the caller's string. */
esp_err_t orb_theme_delete(const char *id);

/* ORB_THEME_SLOT_CAP is defined exactly once, in orb_bundle.h — the header the
 * format's readers and tools share — and reaches this file through the include
 * above. It names the ceiling a bundle's total art must fit within (Migration 1
 * made the whole 8 MB `assets` partition one slot). Do not re-declare it here
 * or in any tool: §7.4 is the record of what four restatements cost. */
