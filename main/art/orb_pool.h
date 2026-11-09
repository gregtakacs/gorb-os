#pragma once
/**
 * orb_pool — the theme pool's table (docs/theme-pool-plan.md §2, theme-cache-plan.md §4.1).
 *
 * The `assets` partition holds several apps' themes and, in its last two sectors, two
 * copies of a table saying what is where. Format 3 (C7, the owner's 2026-11-09 choice):
 * the pool is its pages (ORB_POOL_PAGES_IN its partition) of 64 KB (the S3's MMU page), and every entry is a
 * set of them, wherever they were free. An entry's pages in ascending flash order are its
 * bytes in order, so its page i is the i-th page it owns. Nothing ever moves: a new entry
 * is written into free pages no table names, then one table write names them; an eviction
 * or a removal is one table write that frees pages. A cut at any step leaves the last
 * table whole and nothing torn (the half-written pages are free space), so there is
 * nothing to repair.
 *
 * It knows nothing of a theme's insides: an entry is a head (the slot header's sector and
 * the bundle's index) and a run of banks after it, each with a CRC. Every flash access
 * goes through orb_pool_io_t, so tools/host_pool_test.py drives this exact file over a
 * simulated flash that loses power at every step, and the device drives it over
 * esp_partition.
 *
 * A table write goes to the copy that is not live, so it is atomic. Formats 1 and 2 (a
 * packed pool on the 4 KB grid) still decode, so boot can read what they said, but their
 * entries are not carried: they do not sit on pages (§4.1's carry).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ORB_POOL_SECTOR      0x1000u                       /* the erase unit */
#define ORB_POOL_PAGE        0x10000u                      /* the MMU page: an entry's unit */
#define ORB_POOL_FIRST       0x0u                          /* the one slot is entry 0 */
/* The pool is the `assets` partition as the device's own table sizes it, so one image runs
 * on both layouts: 8 MB (127 pages, an orb only ever updated over Wi-Fi keeps its table)
 * and 9.5 MB (151 pages, partitions.csv since 2026-11-09). The tables are its last two
 * sectors and the pages everything before them. A table of another page count is not this
 * pool's: it is refused, and the pool starts empty. */
#define ORB_POOL_PAGES_MAX   160u                          /* the arrays' bound: a 10 MB partition */
#define ORB_POOL_TABLE_A_IN(part) ((part) - 2u * ORB_POOL_SECTOR)
#define ORB_POOL_TABLE_B_IN(part) ((part) - 1u * ORB_POOL_SECTOR)
#define ORB_POOL_PAGES_IN(part)   (((part) - 2u * ORB_POOL_SECTOR) / ORB_POOL_PAGE)
#define ORB_POOL_ENTRIES     16u                           /* theme-cache-plan §2.6: a voice bank and a display entry for each of B5's eight apps */
#define ORB_POOL_ENTRIES_F1  8u                            /* a format 1 table's */
#define ORB_POOL_SLUG_MAX    24u                           /* the 23-byte slug rule + NUL */
#define ORB_POOL_NAME_MAX    56u                           /* the slot header's name */
#define ORB_POOL_MAGIC       0x4C4F4F50u                   /* "POOL", little-endian */
#define ORB_POOL_FORMAT      3u                            /* formats 1 and 2 decode, not carried */
#define ORB_POOL_HEAD_BYTES  32u
#define ORB_POOL_ENTRY_BYTES 136u                          /* format 2's 132 and the page id */
#define ORB_POOL_ENTRY_BYTES_F2 132u
#define ORB_POOL_ENTRY_BYTES_F1 128u
#define ORB_POOL_MAP_AT      (ORB_POOL_HEAD_BYTES + ORB_POOL_ENTRIES * ORB_POOL_ENTRY_BYTES)

enum { ORB_POOL_LIVE = 1, ORB_POOL_MOVING = 2 };           /* MOVING: formats 1 and 2 only */
/* An entry's kind (theme-cache-plan §2.6): an app's voice bank, resident while its theme is
 * chosen, or its display entry, the cache's to drop. A format 1 table's entries are display
 * entries. */
enum { ORB_POOL_DISPLAY = 1, ORB_POOL_VOICE = 2,
       /* owner, 2026-11-09: an app's preview, the theme's full-glass picture as the bundle
        * deflated it, so the carousel reads it from flash. Stored only into free room and the
        * first thing evicted; without one the carousel reads it off the card. */
       ORB_POOL_PREVIEW = 3 };

/* One app's theme (or voice bank) in the pool. Offsets inside it are the entry's own: its
 * head is [0, geom_size), its banks [art_off, art_off + art_size), and byte x of it is at
 * page x / ORB_POOL_PAGE of its pages (orb_pool_pages). `off` is its first page, kept for
 * the log and the hub; `size` is whole pages. The index records bank offsets as they were
 * when the entry stood at `base` (the slot's own layout). */
typedef struct {
    char     slug[ORB_POOL_SLUG_MAX];
    char     name[ORB_POOL_NAME_MAX];
    uint32_t off, size, base;
    uint32_t geom_size, geom_crc;
    uint32_t art_off, art_size, art_crc;
    uint32_t file_crc, serial, state, abi;
    uint32_t kind;                          /* ORB_POOL_DISPLAY or ORB_POOL_VOICE */
    uint32_t pid;                           /* format 3: its pages' owner, 1..255 */
} orb_pool_entry_t;

typedef struct {
    uint32_t         serial;
    uint32_t         n;
    uint32_t         format;                /* as decoded: 3, or 1/2 (not carried) */
    uint32_t         pages;                 /* the pool's page count (ORB_POOL_PAGES_IN) */
    orb_pool_entry_t e[ORB_POOL_ENTRIES];
    uint8_t          owner[ORB_POOL_PAGES_MAX]; /* each page's entry pid, 0 = free */
} orb_pool_t;

/* An entry's pages as flash offsets, in its byte order. */
typedef struct {
    uint32_t n;
    uint32_t off[ORB_POOL_PAGES_MAX];
} orb_pool_pages_t;

/* Flash access. Offsets are partition-relative; erase is whole sectors. Each answers 0
 * or a negative failure. `buf` is the caller's scratch, at least one sector. */
typedef struct {
    void    *ctx;
    int    (*read)(void *ctx, uint32_t off, void *dst, uint32_t n);
    int    (*erase)(void *ctx, uint32_t off, uint32_t n);
    int    (*write)(void *ctx, uint32_t off, const void *src, uint32_t n);
    uint8_t *buf;
    uint32_t buf_bytes;
    /* Unused since format 3 (nothing moves); kept so a caller's progress hook compiles. */
    void   (*progress)(void *ctx, uint32_t done, uint32_t total);
    uint32_t part_bytes;                    /* the partition's size: the pool's pages and tables */
} orb_pool_io_t;

/* Failures, beside the io's own negatives. */
#define ORB_POOL_ERR_FULL    (-1001)   /* the theme does not fit, nothing touched */
#define ORB_POOL_ERR_TABLE   (-1002)   /* the table is full or would be invalid */
#define ORB_POOL_ERR_VERIFY  (-1003)   /* bytes read back wrong */
#define ORB_POOL_ERR_ARG     (-1004)

/* ---- the table, as bytes (pure) ---- */

/* Encode into one sector's worth of bytes (the rest zero). Always format 3. */
void orb_pool_encode(const orb_pool_t *p, uint8_t out[ORB_POOL_SECTOR]);

/* Decode and check. Format 3: magic, count, CRC, every entry well formed (a known kind and
 * state LIVE, one entry of a kind an app, a unique page id, whole pages, its index and
 * banks inside it), and the page map naming exactly each entry's pages. Formats 1 and 2
 * decode as they always did (out->format says which), for boot to read, never to use.
 * NULL on success, else the refusal. */
const char *orb_pool_decode(const uint8_t *in, size_t len, orb_pool_t *out);

/* `slug`'s display entry (its theme as the stage binds it), or -1. */
int orb_pool_find(const orb_pool_t *p, const char *slug);
/* `slug`'s entry of `kind`, or -1. */
int orb_pool_find_kind(const orb_pool_t *p, const char *slug, uint32_t kind);

/* Entry k's pages, in its byte order. */
void orb_pool_pages(const orb_pool_t *p, uint32_t k, orb_pool_pages_t *out);
/* Pages no entry owns. */
uint32_t orb_pool_free_pages(const orb_pool_t *p);
/* Bytes the entries hold (whole pages). */
uint32_t orb_pool_used(const orb_pool_t *p);

/* Whether `slug`'s display entry of `size` bytes fits once `slug`'s old one is gone: 0, or
 * ORB_POOL_ERR_FULL (no room), or ORB_POOL_ERR_TABLE (the table's slots). Pure. */
int orb_pool_fits(const orb_pool_t *p, const char *slug, uint32_t size);

/* The bytes the pool's pages hold. */
uint32_t orb_pool_cap(const orb_pool_t *p);

/* A size rounded up to whole pages. */
uint32_t orb_pool_round(uint32_t n);

/* ---- an entry's bytes, through its pages (pure but for io) ---- */

/* Read, write, erase or checksum [off, off + n) of the entry whose pages are `pg`, split at
 * page boundaries. Erase wants a sector-aligned range. */
int orb_pool_pread(const orb_pool_io_t *io, const orb_pool_pages_t *pg, uint32_t off,
                   void *dst, uint32_t n);
int orb_pool_pwrite(const orb_pool_io_t *io, const orb_pool_pages_t *pg, uint32_t off,
                    const void *src, uint32_t n);
int orb_pool_perase(const orb_pool_io_t *io, const orb_pool_pages_t *pg, uint32_t off,
                    uint32_t n);
int orb_pool_pcrc(const orb_pool_io_t *io, const orb_pool_pages_t *pg, uint32_t off,
                  uint32_t n, uint32_t *crc);

/* ---- the pool on flash (through io) ---- */

/* Read both copies and take the valid one with the higher serial. *which is 0 for none
 * (an empty pool, serial 0), 1 for A, 2 for B. A table of format 1 or 2 is answered as
 * read (p->format says so): the caller reads what it needs from it and calls
 * orb_pool_reset. */
int orb_pool_load(const orb_pool_io_t *io, orb_pool_t *p, int *which);

/* Bump the serial and write the table to the copy that is not live. */
int orb_pool_write(const orb_pool_io_t *io, orb_pool_t *p, int *which);

/* An empty format 3 pool over whatever was there, its serial past the old one's, written. */
int orb_pool_reset(const orb_pool_io_t *io, orb_pool_t *p, int *which);

/* Remove `slug`'s display entry: one table write, its pages free. Absent is not a failure. */
int orb_pool_remove(const orb_pool_io_t *io, orb_pool_t *p, int *which, const char *slug);
/* Remove `slug`'s entry of `kind`. Absent is not a failure. */
int orb_pool_remove_kind(const orb_pool_io_t *io, orb_pool_t *p, int *which, const char *slug,
                         uint32_t kind);

/* The theme cache (docs/theme-cache-plan.md §2.2): the entries a new entry of `kind` and
 * `size` bytes for `slug` evicts, least recently on the glass first. `order` lists slugs
 * from least to most recent; an entry it does not name counts as older than any it does,
 * in table order. `keep`'s display entry (may be NULL) is never evicted; `slug`'s own old
 * entry of `kind` always goes. Previews go first, then display entries, a voice bank never;
 * a new preview evicts nothing (owner, 2026-11-09: a slower carousel before a reloading
 * app). Pure: sets bit k of *mask for each
 * entry k to drop, and answers 0, or ORB_POOL_ERR_FULL when even every entry it may evict
 * leaves too few pages (or no table slot). */
int orb_pool_evictions_kind(const orb_pool_t *p, const char *slug, uint32_t kind, uint32_t size,
                            const char *const *order, uint32_t n_order, const char *keep,
                            uint32_t *mask);
int orb_pool_evictions(const orb_pool_t *p, const char *slug, uint32_t size,
                       const char *const *order, uint32_t n_order, const char *keep,
                       uint32_t *mask);

/* Room for a new entry of `kind`: refuse with ORB_POOL_ERR_FULL, touching nothing; else drop
 * every evicted entry (and `slug`'s own old one of `kind`) in one table write, and answer in
 * *pg the free pages the new bytes go to (the lowest free ones). Nothing names those pages
 * until orb_pool_commit. *evicted (may be NULL) gets the evicted slugs, `slug`'s own old
 * entry not among them; *n_evicted their count. */
int orb_pool_reserve(const orb_pool_io_t *io, orb_pool_t *p, int *which,
                     const char *slug, uint32_t kind, uint32_t size,
                     const char *const *order, uint32_t n_order, const char *keep,
                     orb_pool_pages_t *pg, char (*evicted)[ORB_POOL_SLUG_MAX], uint32_t *n_evicted);

/* Name entry `e` (its size whole pages) on pages `pg`, all free, in one table write: it gets
 * a page id, its `off` is its first page. Refuses ORB_POOL_ERR_ARG for a full table, a page
 * already owned, a page count that is not its size, or an app that already has one of its
 * kind. On a refusal or a failed write the table in RAM is as it was. */
int orb_pool_commit(const orb_pool_io_t *io, orb_pool_t *p, int *which,
                    const orb_pool_entry_t *e, const orb_pool_pages_t *pg);
