/*
 * orb_pool.c — the theme pool's table (docs/theme-pool-plan.md §2, theme-cache-plan.md §4.1).
 *
 * Pure C over orb_pool_io_t: no partition, no allocator, no logging. The device wraps
 * esp_partition in the io; tools/host_pool_test.py wraps a simulated flash that loses
 * power at every erase and write, and checks what each cut leaves.
 */
#include <string.h>

#include "esp_rom_crc.h"
#include "orb_pool.h"

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The head's words, and an entry's after its two strings. */
enum { H_MAGIC, H_FORMAT, H_SERIAL, H_N, H_ENTRY_BYTES, H_CRC, H_PAGES };
#define E_WORDS_AT (ORB_POOL_SLUG_MAX + ORB_POOL_NAME_MAX)
#define E_WORDS    14                       /* format 3: format 2's thirteen and the page id */
#define E_WORDS_F2 13
#define E_WORDS_F1 12
#define T3_BYTES   (ORB_POOL_MAP_AT + ORB_POOL_PAGES)   /* format 3's checked bytes */
_Static_assert(E_WORDS_AT + E_WORDS * 4 <= ORB_POOL_ENTRY_BYTES, "an entry's words must fit its record");
_Static_assert(E_WORDS_AT + E_WORDS_F2 * 4 <= ORB_POOL_ENTRY_BYTES_F2, "format 2's words fit its record");
_Static_assert(E_WORDS_AT + E_WORDS_F1 * 4 <= ORB_POOL_ENTRY_BYTES_F1, "format 1's words fit its record");
_Static_assert(T3_BYTES <= ORB_POOL_SECTOR, "the table and its page map are one sector");
_Static_assert(ORB_POOL_PAGES <= 255u, "a page's owner is a byte");

static uint32_t *entry_word(orb_pool_entry_t *e, int k)
{
    uint32_t *w[] = { &e->off, &e->size, &e->base, &e->geom_size, &e->geom_crc,
                      &e->art_off, &e->art_size, &e->art_crc, &e->file_crc, &e->serial,
                      &e->state, &e->abi, &e->kind, &e->pid };
    return w[k];
}

/* The CRC over `used` bytes of a table with its own CRC word read as zero. */
static uint32_t table_crc(const uint8_t *t, uint32_t used)
{
    static const uint8_t zero[4] = { 0 };
    uint32_t c = esp_rom_crc32_le(0, t, H_CRC * 4u);
    c = esp_rom_crc32_le(c, zero, 4u);
    return esp_rom_crc32_le(c, t + (H_CRC + 1) * 4u, used - (H_CRC + 1) * 4u);
}

void orb_pool_encode(const orb_pool_t *p, uint8_t out[ORB_POOL_SECTOR])
{
    memset(out, 0, ORB_POOL_SECTOR);
    put32(out + H_MAGIC * 4, ORB_POOL_MAGIC);
    put32(out + H_FORMAT * 4, ORB_POOL_FORMAT);
    put32(out + H_SERIAL * 4, p->serial);
    put32(out + H_N * 4, p->n);
    put32(out + H_ENTRY_BYTES * 4, ORB_POOL_ENTRY_BYTES);
    put32(out + H_PAGES * 4, ORB_POOL_PAGES);
    for (uint32_t i = 0; i < p->n && i < ORB_POOL_ENTRIES; i++) {
        uint8_t *r = out + ORB_POOL_HEAD_BYTES + i * ORB_POOL_ENTRY_BYTES;
        orb_pool_entry_t e = p->e[i];
        memcpy(r, e.slug, ORB_POOL_SLUG_MAX);
        memcpy(r + ORB_POOL_SLUG_MAX, e.name, ORB_POOL_NAME_MAX);
        for (int k = 0; k < E_WORDS; k++) put32(r + E_WORDS_AT + 4 * k, *entry_word(&e, k));
    }
    memcpy(out + ORB_POOL_MAP_AT, p->owner, ORB_POOL_PAGES);
    put32(out + H_CRC * 4, table_crc(out, T3_BYTES));
}

static bool str_ok(const char *s, size_t cap, bool need)
{
    const char *z = memchr(s, 0, cap);
    return z && (!need || z != s);
}

/* Formats 1 and 2, as they were checked: read for what they say, never used as a pool. */
static const char *decode_old(const uint8_t *in, uint32_t fmt, orb_pool_t *out)
{
    const bool f1 = fmt == 1u;
    const uint32_t width = f1 ? ORB_POOL_ENTRY_BYTES_F1 : ORB_POOL_ENTRY_BYTES_F2;
    const uint32_t n = get32(in + H_N * 4);
    if (n > (f1 ? ORB_POOL_ENTRIES_F1 : ORB_POOL_ENTRIES)) return "more entries than the pool holds";
    if (get32(in + H_ENTRY_BYTES * 4) != width) return "entries of another width";
    if (get32(in + H_CRC * 4) != table_crc(in, ORB_POOL_HEAD_BYTES + n * width)) return "the table's CRC is wrong";
    out->serial = get32(in + H_SERIAL * 4);
    out->n = n;
    out->format = fmt;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *r = in + ORB_POOL_HEAD_BYTES + i * width;
        orb_pool_entry_t *e = &out->e[i];
        memcpy(e->slug, r, ORB_POOL_SLUG_MAX);
        memcpy(e->name, r + ORB_POOL_SLUG_MAX, ORB_POOL_NAME_MAX);
        for (int k = 0; k < (f1 ? E_WORDS_F1 : E_WORDS_F2); k++) *entry_word(e, k) = get32(r + E_WORDS_AT + 4 * k);
        if (f1) e->kind = ORB_POOL_DISPLAY;
        if (!str_ok(e->slug, ORB_POOL_SLUG_MAX, true)) return "an entry with no app";
        if (!str_ok(e->name, ORB_POOL_NAME_MAX, false)) return "an entry's name runs on";
        if (e->kind != ORB_POOL_DISPLAY && e->kind != ORB_POOL_VOICE) return "an entry of no known kind";
    }
    return NULL;
}

const char *orb_pool_decode(const uint8_t *in, size_t len, orb_pool_t *out)
{
    memset(out, 0, sizeof *out);
    if (len < ORB_POOL_SECTOR) return "a table is one sector";
    if (get32(in + H_MAGIC * 4) != ORB_POOL_MAGIC) return "no pool table here";
    const uint32_t fmt = get32(in + H_FORMAT * 4);
    if (fmt == 1u || fmt == 2u) {
        const char *w = decode_old(in, fmt, out);
        if (w) memset(out, 0, sizeof *out);
        return w;
    }
    if (fmt != ORB_POOL_FORMAT) return "a pool table of another format";
    const uint32_t n = get32(in + H_N * 4);
    if (n > ORB_POOL_ENTRIES) return "more entries than the pool holds";
    if (get32(in + H_ENTRY_BYTES * 4) != ORB_POOL_ENTRY_BYTES) return "entries of another width";
    if (get32(in + H_PAGES * 4) != ORB_POOL_PAGES) return "a pool of another page count";
    if (get32(in + H_CRC * 4) != table_crc(in, T3_BYTES)) return "the table's CRC is wrong";
    out->serial = get32(in + H_SERIAL * 4);
    out->n = n;
    out->format = fmt;
    memcpy(out->owner, in + ORB_POOL_MAP_AT, ORB_POOL_PAGES);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *r = in + ORB_POOL_HEAD_BYTES + i * ORB_POOL_ENTRY_BYTES;
        orb_pool_entry_t *e = &out->e[i];
        memcpy(e->slug, r, ORB_POOL_SLUG_MAX);
        memcpy(e->name, r + ORB_POOL_SLUG_MAX, ORB_POOL_NAME_MAX);
        for (int k = 0; k < E_WORDS; k++) *entry_word(e, k) = get32(r + E_WORDS_AT + 4 * k);
        if (!str_ok(e->slug, ORB_POOL_SLUG_MAX, true)) return "an entry with no app";
        if (!str_ok(e->name, ORB_POOL_NAME_MAX, false)) return "an entry's name runs on";
        if (e->kind != ORB_POOL_DISPLAY && e->kind != ORB_POOL_VOICE && e->kind != ORB_POOL_PREVIEW)
            return "an entry of no known kind";
        if (e->state != ORB_POOL_LIVE) return "an entry in no known state";
        if (!e->pid || e->pid > 255u) return "an entry with no page id";
        for (uint32_t j = 0; j < i; j++) {
            if (!strcmp(out->e[j].slug, e->slug) && out->e[j].kind == e->kind) return "two entries of a kind for one app";
            if (out->e[j].pid == e->pid) return "two entries with one page id";
        }
        if (!e->size || e->size % ORB_POOL_PAGE || e->size > ORB_POOL_CAP) return "an entry not in whole pages";
        if (e->geom_size > e->art_off || e->art_off > e->size ||
            e->art_size > e->size - e->art_off)
            return "an entry's index or banks outside it";
    }
    /* the map names exactly each entry's pages, and its first is its `off` */
    for (uint32_t pg = 0; pg < ORB_POOL_PAGES; pg++) {
        if (!out->owner[pg]) continue;
        uint32_t k = 0;
        while (k < n && out->e[k].pid != out->owner[pg]) k++;
        if (k == n) return "a page owned by no entry";
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cnt = 0, first = UINT32_MAX;
        for (uint32_t pg = 0; pg < ORB_POOL_PAGES; pg++)
            if (out->owner[pg] == out->e[i].pid) { if (!cnt) first = pg * ORB_POOL_PAGE; cnt++; }
        if (cnt * ORB_POOL_PAGE != out->e[i].size) return "an entry's pages are not its size";
        if (first != out->e[i].off) return "an entry's first page is not its offset";
    }
    return NULL;
}

int orb_pool_find_kind(const orb_pool_t *p, const char *slug, uint32_t kind)
{
    for (uint32_t i = 0; i < p->n; i++)
        if (p->e[i].kind == kind && !strncmp(p->e[i].slug, slug, ORB_POOL_SLUG_MAX)) return (int)i;
    return -1;
}

int orb_pool_find(const orb_pool_t *p, const char *slug)
{
    return orb_pool_find_kind(p, slug, ORB_POOL_DISPLAY);
}

void orb_pool_pages(const orb_pool_t *p, uint32_t k, orb_pool_pages_t *out)
{
    out->n = 0;
    if (k >= p->n) return;
    for (uint32_t pg = 0; pg < ORB_POOL_PAGES; pg++)
        if (p->owner[pg] == p->e[k].pid) out->off[out->n++] = pg * ORB_POOL_PAGE;
}

uint32_t orb_pool_free_pages(const orb_pool_t *p)
{
    uint32_t f = 0;
    for (uint32_t pg = 0; pg < ORB_POOL_PAGES; pg++) f += !p->owner[pg];
    return f;
}

uint32_t orb_pool_used(const orb_pool_t *p)
{
    return (ORB_POOL_PAGES - orb_pool_free_pages(p)) * ORB_POOL_PAGE;
}

uint32_t orb_pool_round(uint32_t n)
{
    return (n + ORB_POOL_PAGE - 1u) & ~(ORB_POOL_PAGE - 1u);
}

/* ---- an entry's bytes ---- */

/* [off, off + n) of the entry, one page's part at a time: fn gets the flash offset. */
typedef int (*part_fn)(const orb_pool_io_t *io, uint32_t at, uint8_t *b, uint32_t n, void *arg);
static int each_part(const orb_pool_io_t *io, const orb_pool_pages_t *pg, uint32_t off,
                     uint8_t *b, uint32_t n, part_fn fn, void *arg)
{
    while (n) {
        const uint32_t page = off / ORB_POOL_PAGE, in = off % ORB_POOL_PAGE;
        if (page >= pg->n) return ORB_POOL_ERR_ARG;
        const uint32_t k = n < ORB_POOL_PAGE - in ? n : ORB_POOL_PAGE - in;
        const int r = fn(io, pg->off[page] + in, b, k, arg);
        if (r) return r;
        off += k;
        n -= k;
        if (b) b += k;
    }
    return 0;
}

static int part_read(const orb_pool_io_t *io, uint32_t at, uint8_t *b, uint32_t n, void *a)
{ (void)a; return io->read(io->ctx, at, b, n); }
static int part_write(const orb_pool_io_t *io, uint32_t at, uint8_t *b, uint32_t n, void *a)
{ (void)a; return io->write(io->ctx, at, b, n); }
static int part_erase(const orb_pool_io_t *io, uint32_t at, uint8_t *b, uint32_t n, void *a)
{ (void)a; (void)b; return io->erase(io->ctx, at, n); }
static int part_crc(const orb_pool_io_t *io, uint32_t at, uint8_t *b, uint32_t n, void *a)
{
    (void)b;
    uint32_t *c = a;
    while (n) {
        const uint32_t k = n < io->buf_bytes ? n : io->buf_bytes;
        const int r = io->read(io->ctx, at, io->buf, k);
        if (r) return r;
        *c = esp_rom_crc32_le(*c, io->buf, k);
        at += k;
        n -= k;
    }
    return 0;
}

int orb_pool_pread(const orb_pool_io_t *io, const orb_pool_pages_t *pg, uint32_t off,
                   void *dst, uint32_t n)
{
    return each_part(io, pg, off, dst, n, part_read, NULL);
}

int orb_pool_pwrite(const orb_pool_io_t *io, const orb_pool_pages_t *pg, uint32_t off,
                    const void *src, uint32_t n)
{
    return each_part(io, pg, off, (uint8_t *)(uintptr_t)src, n, part_write, NULL);
}

int orb_pool_perase(const orb_pool_io_t *io, const orb_pool_pages_t *pg, uint32_t off, uint32_t n)
{
    if (off % ORB_POOL_SECTOR || n % ORB_POOL_SECTOR) return ORB_POOL_ERR_ARG;
    return each_part(io, pg, off, NULL, n, part_erase, NULL);
}

int orb_pool_pcrc(const orb_pool_io_t *io, const orb_pool_pages_t *pg, uint32_t off,
                  uint32_t n, uint32_t *crc)
{
    *crc = 0;
    return each_part(io, pg, off, NULL, n, part_crc, crc);
}

/* ---- the pool on flash ---- */

int orb_pool_load(const orb_pool_io_t *io, orb_pool_t *p, int *which)
{
    if (!io->buf || io->buf_bytes < ORB_POOL_SECTOR) return ORB_POOL_ERR_ARG;
    *which = 0;
    int r = io->read(io->ctx, ORB_POOL_TABLE_A, io->buf, ORB_POOL_SECTOR);
    if (r) return r;
    const bool a_ok = orb_pool_decode(io->buf, ORB_POOL_SECTOR, p) == NULL;
    const uint32_t a_serial = a_ok ? p->serial : 0;
    r = io->read(io->ctx, ORB_POOL_TABLE_B, io->buf, ORB_POOL_SECTOR);
    if (r) return r;
    /* B only where it is newer: decode it over p, and fall back to A if it lies. */
    if (get32(io->buf + H_MAGIC * 4) == ORB_POOL_MAGIC &&
        (!a_ok || get32(io->buf + H_SERIAL * 4) > a_serial)) {
        if (orb_pool_decode(io->buf, ORB_POOL_SECTOR, p) == NULL) { *which = 2; return 0; }
        if (!a_ok) { memset(p, 0, sizeof *p); p->format = ORB_POOL_FORMAT; return 0; }
        r = io->read(io->ctx, ORB_POOL_TABLE_A, io->buf, ORB_POOL_SECTOR);
        if (r) return r;
        (void)orb_pool_decode(io->buf, ORB_POOL_SECTOR, p);
    }
    if (a_ok) *which = 1;
    else { memset(p, 0, sizeof *p); p->format = ORB_POOL_FORMAT; }
    return 0;
}

int orb_pool_write(const orb_pool_io_t *io, orb_pool_t *p, int *which)
{
    /* Never write a table this file would refuse to read. Static, not on the stack:
     * a pool is a few kilobytes, and pool work is one worker's, never two at once. */
    static orb_pool_t check;
    p->serial++;
    orb_pool_encode(p, io->buf);
    if (orb_pool_decode(io->buf, ORB_POOL_SECTOR, &check)) { p->serial--; return ORB_POOL_ERR_TABLE; }
    const uint32_t at = *which == 1 ? ORB_POOL_TABLE_B : ORB_POOL_TABLE_A;
    int r = io->erase(io->ctx, at, ORB_POOL_SECTOR);
    if (!r) r = io->write(io->ctx, at, io->buf, ORB_POOL_SECTOR);
    if (r) return r;
    *which = at == ORB_POOL_TABLE_A ? 1 : 2;
    p->format = ORB_POOL_FORMAT;
    return 0;
}

int orb_pool_reset(const orb_pool_io_t *io, orb_pool_t *p, int *which)
{
    const uint32_t serial = p->serial;
    memset(p, 0, sizeof *p);
    p->serial = serial;
    p->format = ORB_POOL_FORMAT;
    return orb_pool_write(io, p, which);
}

static void drop(orb_pool_t *p, uint32_t k)
{
    for (uint32_t pg = 0; pg < ORB_POOL_PAGES; pg++)
        if (p->owner[pg] == p->e[k].pid) p->owner[pg] = 0;
    memmove(&p->e[k], &p->e[k + 1], (p->n - k - 1u) * sizeof p->e[0]);
    p->n--;
    memset(&p->e[p->n], 0, sizeof p->e[0]);
}

int orb_pool_remove(const orb_pool_io_t *io, orb_pool_t *p, int *which, const char *slug)
{
    return orb_pool_remove_kind(io, p, which, slug, ORB_POOL_DISPLAY);
}

int orb_pool_remove_kind(const orb_pool_io_t *io, orb_pool_t *p, int *which, const char *slug,
                         uint32_t kind)
{
    const int k = orb_pool_find_kind(p, slug, kind);
    if (k < 0) return 0;
    drop(p, (uint32_t)k);
    return orb_pool_write(io, p, which);
}

int orb_pool_fits(const orb_pool_t *p, const char *slug, uint32_t size)
{
    const int k = orb_pool_find(p, slug);
    uint64_t total = orb_pool_round(size);
    for (uint32_t i = 0; i < p->n; i++)
        if ((int)i != k) total += p->e[i].size;
    if (total > ORB_POOL_CAP) return ORB_POOL_ERR_FULL;
    if (k < 0 && p->n >= ORB_POOL_ENTRIES) return ORB_POOL_ERR_TABLE;
    return 0;
}

int orb_pool_evictions(const orb_pool_t *p, const char *slug, uint32_t size,
                       const char *const *order, uint32_t n_order, const char *keep,
                       uint32_t *mask)
{
    return orb_pool_evictions_kind(p, slug, ORB_POOL_DISPLAY, size, order, n_order, keep, mask);
}

int orb_pool_evictions_kind(const orb_pool_t *p, const char *slug, uint32_t kind, uint32_t size,
                            const char *const *order, uint32_t n_order, const char *keep,
                            uint32_t *mask)
{
    *mask = 0;
    if (!size || !slug) return ORB_POOL_ERR_ARG;
    uint64_t total = orb_pool_round(size);
    uint32_t count = 1;                              /* the new entry */
    int rank[ORB_POOL_ENTRIES];
    for (uint32_t k = 0; k < p->n; k++) {
        if (p->e[k].kind == kind && !strcmp(p->e[k].slug, slug)) { *mask |= 1u << k; rank[k] = -2; continue; }   /* its own old */
        total += p->e[k].size;
        count++;
        rank[k] = -1;                                /* not named: older than any named */
        for (uint32_t r = 0; order && r < n_order; r++)
            if (order[r] && !strcmp(order[r], p->e[k].slug)) { rank[k] = (int)r; break; }
    }
    /* least recent first, ties in table order; never keep, never a voice bank */
    while (total > ORB_POOL_CAP || count > ORB_POOL_ENTRIES) {
        int pick = -1;
        for (uint32_t k = 0; k < p->n; k++) {
            if ((*mask >> k) & 1u) continue;
            if (p->e[k].kind != ORB_POOL_DISPLAY) continue;
            if (keep && !strcmp(p->e[k].slug, keep)) continue;
            if (pick < 0 || rank[k] < rank[pick]) pick = (int)k;
        }
        if (pick < 0) { *mask = 0; return ORB_POOL_ERR_FULL; }
        *mask |= 1u << pick;
        total -= p->e[pick].size;
        count--;
    }
    return 0;
}

int orb_pool_reserve(const orb_pool_io_t *io, orb_pool_t *p, int *which,
                     const char *slug, uint32_t kind, uint32_t size,
                     const char *const *order, uint32_t n_order, const char *keep,
                     orb_pool_pages_t *pg, char (*evicted)[ORB_POOL_SLUG_MAX], uint32_t *n_evicted)
{
    uint32_t mask;
    if (n_evicted) *n_evicted = 0;
    pg->n = 0;
    const int f = orb_pool_evictions_kind(p, slug, kind, size, order, n_order, keep, &mask);
    if (f) return f;
    if (mask) {
        for (uint32_t k = p->n; k-- > 0;) {
            if (!((mask >> k) & 1u)) continue;
            if (n_evicted && (p->e[k].kind != kind || strcmp(p->e[k].slug, slug))) {
                if (evicted) memcpy(evicted[*n_evicted], p->e[k].slug, ORB_POOL_SLUG_MAX);
                (*n_evicted)++;
            }
            drop(p, k);
        }
        const int r = orb_pool_write(io, p, which);  /* every eviction in one table write */
        if (r) return r;
    }
    const uint32_t need = orb_pool_round(size) / ORB_POOL_PAGE;
    for (uint32_t i = 0; i < ORB_POOL_PAGES && pg->n < need; i++)
        if (!p->owner[i]) pg->off[pg->n++] = i * ORB_POOL_PAGE;
    if (pg->n < need) { pg->n = 0; return ORB_POOL_ERR_FULL; }   /* the evictions said it would fit */
    return 0;
}

int orb_pool_commit(const orb_pool_io_t *io, orb_pool_t *p, int *which,
                    const orb_pool_entry_t *e, const orb_pool_pages_t *pg)
{
    if (p->n >= ORB_POOL_ENTRIES || orb_pool_find_kind(p, e->slug, e->kind) >= 0) return ORB_POOL_ERR_ARG;
    if (e->kind != ORB_POOL_DISPLAY && e->kind != ORB_POOL_VOICE && e->kind != ORB_POOL_PREVIEW)
        return ORB_POOL_ERR_ARG;
    if (!pg->n || pg->n * ORB_POOL_PAGE != e->size) return ORB_POOL_ERR_ARG;
    for (uint32_t i = 0; i < pg->n; i++) {
        const uint32_t page = pg->off[i] / ORB_POOL_PAGE;
        if (pg->off[i] % ORB_POOL_PAGE || page >= ORB_POOL_PAGES || p->owner[page]) return ORB_POOL_ERR_ARG;
        if (i && pg->off[i] <= pg->off[i - 1]) return ORB_POOL_ERR_ARG;   /* ascending: byte order */
    }
    uint32_t pid = 1;
    for (bool taken = true; taken && pid <= 255u; ) {
        taken = false;
        for (uint32_t k = 0; k < p->n; k++) if (p->e[k].pid == pid) { taken = true; pid++; break; }
    }
    if (pid > 255u) return ORB_POOL_ERR_TABLE;
    const uint32_t at = p->n;
    p->e[at] = *e;
    p->e[at].state = ORB_POOL_LIVE;
    p->e[at].pid = pid;
    p->e[at].off = pg->off[0];
    for (uint32_t i = 0; i < pg->n; i++) p->owner[pg->off[i] / ORB_POOL_PAGE] = (uint8_t)pid;
    p->n++;
    const int r = orb_pool_write(io, p, which);
    if (r) {
        for (uint32_t i = 0; i < pg->n; i++) p->owner[pg->off[i] / ORB_POOL_PAGE] = 0;
        p->n--;
        memset(&p->e[p->n], 0, sizeof p->e[0]);
    }
    return r;
}
