#pragma once
/**
 * orb_pool — the theme pool's table and its compaction (docs/theme-pool-plan.md §2).
 *
 * The `assets` partition holds several apps' themes, packed from ORB_POOL_FIRST with no
 * holes, and two copies of a table saying what is where in its last two sectors. This
 * file is the table's format and the only code that moves an entry. It knows nothing of
 * a theme's insides: an entry is an extent with a head at its start (the slot header's
 * sector and the bundle's index, as the one slot laid them out) and a run of banks
 * after it, each with a CRC. Every flash access goes through orb_pool_io_t, so
 * tools/host_pool_test.py drives this exact file over a simulated flash that loses
 * power at every step, and the device drives it over esp_partition.
 *
 * A table write goes to the copy that is not live, so it is atomic: a cut leaves the
 * other copy. An entry is marked MOVING before its bytes move and LIVE at its new
 * offset after they are read back, so a cut costs at most that one entry.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ORB_POOL_SECTOR      0x1000u                       /* the erase unit */
#define ORB_POOL_PART_BYTES  0x800000u                     /* partitions.csv `assets` */
#define ORB_POOL_FIRST       0x0u                          /* the one slot is entry 0 */
#define ORB_POOL_TABLE_A     (ORB_POOL_PART_BYTES - 2u * ORB_POOL_SECTOR)
#define ORB_POOL_TABLE_B     (ORB_POOL_PART_BYTES - 1u * ORB_POOL_SECTOR)
#define ORB_POOL_END         ORB_POOL_TABLE_A              /* entries stop here */
#define ORB_POOL_ENTRIES     8u                            /* B5: one per app */
#define ORB_POOL_SLUG_MAX    24u                           /* the 23-byte slug rule + NUL */
#define ORB_POOL_NAME_MAX    56u                           /* the slot header's name */
#define ORB_POOL_MAGIC       0x4C4F4F50u                   /* "POOL", little-endian */
#define ORB_POOL_FORMAT      1u
#define ORB_POOL_HEAD_BYTES  32u
#define ORB_POOL_ENTRY_BYTES 128u

enum { ORB_POOL_LIVE = 1, ORB_POOL_MOVING = 2 };

/* One app's theme in the pool. Its head (header sector and index) is [off, off +
 * geom_size); its banks are [off + art_off, off + art_off + art_size). The index records
 * bank offsets as they were when the entry stood at `base`, so a bank is at record -
 * base + off. */
typedef struct {
    char     slug[ORB_POOL_SLUG_MAX];
    char     name[ORB_POOL_NAME_MAX];
    uint32_t off, size, base;
    uint32_t geom_size, geom_crc;
    uint32_t art_off, art_size, art_crc;
    uint32_t file_crc, serial, state, abi;
} orb_pool_entry_t;

typedef struct {
    uint32_t         serial;
    uint32_t         n;
    orb_pool_entry_t e[ORB_POOL_ENTRIES];   /* in pool order */
} orb_pool_t;

/* Flash access. Offsets are partition-relative; erase is whole sectors. Each answers 0
 * or a negative failure. `buf` is the caller's scratch, at least one sector. */
typedef struct {
    void    *ctx;
    int    (*read)(void *ctx, uint32_t off, void *dst, uint32_t n);
    int    (*erase)(void *ctx, uint32_t off, uint32_t n);
    int    (*write)(void *ctx, uint32_t off, const void *src, uint32_t n);
    uint8_t *buf;
    uint32_t buf_bytes;
    /* Optional: called after each sector a move copies, with the bytes of the entry
     * moved so far and its size, so a long compaction can show it is moving. */
    void   (*progress)(void *ctx, uint32_t done, uint32_t total);
} orb_pool_io_t;

/* Failures, beside the io's own negatives. */
#define ORB_POOL_ERR_FULL    (-1001)   /* the theme does not fit, nothing touched */
#define ORB_POOL_ERR_TABLE   (-1002)   /* the table is full or would be invalid */
#define ORB_POOL_ERR_VERIFY  (-1003)   /* a moved entry read back wrong */
#define ORB_POOL_ERR_ARG     (-1004)

/* ---- the table, as bytes (pure) ---- */

/* Encode into one sector's worth of bytes (the rest zero). */
void orb_pool_encode(const orb_pool_t *p, uint8_t out[ORB_POOL_SECTOR]);

/* Decode and check: magic, format, count, CRC, and every entry well formed, in pool
 * order, inside [ORB_POOL_FIRST, ORB_POOL_END), not overlapping, slugs unique. Gaps are
 * allowed (a compaction cut between moves leaves them); orb_pool_packed() asks for
 * none. NULL on success, else the refusal. */
const char *orb_pool_decode(const uint8_t *in, size_t len, orb_pool_t *out);

/* No holes: the entries run from ORB_POOL_FIRST, each where the last one ended. */
bool orb_pool_packed(const orb_pool_t *p);

/* Where the packed run ends (ORB_POOL_FIRST for an empty pool). */
uint32_t orb_pool_used_end(const orb_pool_t *p);

/* The entry for `slug`, or -1. */
int orb_pool_find(const orb_pool_t *p, const char *slug);

/* Whether `slug`'s theme of `size` bytes fits once `slug`'s old entry is gone: 0, or
 * ORB_POOL_ERR_FULL (no room), or ORB_POOL_ERR_TABLE (a ninth app). Pure: a store asks
 * it before it parks the dial, and orb_pool_reserve asks it again. */
int orb_pool_fits(const orb_pool_t *p, const char *slug, uint32_t size);

/* A sector-rounded size. */
uint32_t orb_pool_round(uint32_t n);

/* ---- the pool on flash (through io) ---- */

/* Read both copies and take the valid one with the higher serial. *which is 0 for
 * none (an empty pool, serial 0), 1 for A, 2 for B. */
int orb_pool_load(const orb_pool_io_t *io, orb_pool_t *p, int *which);

/* Bump the serial and write the table to the copy that is not live. */
int orb_pool_write(const orb_pool_io_t *io, orb_pool_t *p, int *which);

/* After a cut: drop any MOVING entry (its bytes are torn), then compact. */
int orb_pool_repair(const orb_pool_io_t *io, orb_pool_t *p, int *which);

/* Move every entry down until the pool is packed. */
int orb_pool_compact(const orb_pool_io_t *io, orb_pool_t *p, int *which);

/* Remove `slug`'s entry, then compact. Absent is not a failure. */
int orb_pool_remove(const orb_pool_io_t *io, orb_pool_t *p, int *which, const char *slug);

/* Make room for `slug`'s new theme of `size` bytes: refuse with ORB_POOL_ERR_FULL,
 * touching nothing, if it cannot fit once `slug`'s old entry is gone; else remove that
 * entry, compact, and answer in *at where the new bytes go (the packed run's end). The
 * caller writes them and calls orb_pool_commit. */
int orb_pool_reserve(const orb_pool_io_t *io, orb_pool_t *p, int *which,
                     const char *slug, uint32_t size, uint32_t *at);

/* The theme cache (docs/theme-cache-plan.md §2.2): the entries a theme of `size` bytes for
 * `slug` evicts, least recently on the glass first. `order` lists slugs from least to most
 * recent; an entry it does not name counts as older than any it does, in pool order. `keep`
 * (may be NULL) is never evicted; `slug`'s own old entry always goes. Pure: sets bit k of
 * *mask for each entry k to drop, and answers 0, or ORB_POOL_ERR_FULL when even every
 * entry it may evict leaves no room (or no table slot). */
int orb_pool_evictions(const orb_pool_t *p, const char *slug, uint32_t size,
                       const char *const *order, uint32_t n_order, const char *keep,
                       uint32_t *mask);

/* orb_pool_reserve, evicting as orb_pool_evictions says: refuse with ORB_POOL_ERR_FULL,
 * touching nothing; else drop every evicted entry in one table write, compact, and answer
 * in *at where the new bytes go. The compaction may MOVE `keep`: the caller makes sure
 * nothing reads the pool through a mapping while this runs. *evicted (may be NULL) gets
 * the evicted slugs, `slug`'s own old entry not among them; *n_evicted their count. */
int orb_pool_reserve_lru(const orb_pool_io_t *io, orb_pool_t *p, int *which,
                         const char *slug, uint32_t size,
                         const char *const *order, uint32_t n_order, const char *keep,
                         uint32_t *at, char (*evicted)[ORB_POOL_SLUG_MAX], uint32_t *n_evicted);

/* Append the entry the caller wrote at orb_pool_reserve's *at, LIVE, and write the
 * table. e->off must be that offset and e->slug absent from the table. */
int orb_pool_commit(const orb_pool_io_t *io, orb_pool_t *p, int *which,
                    const orb_pool_entry_t *e);

/* CRC of [off, off + n) on flash, read through io->buf. */
int orb_pool_crc(const orb_pool_io_t *io, uint32_t off, uint32_t n, uint32_t *crc);
