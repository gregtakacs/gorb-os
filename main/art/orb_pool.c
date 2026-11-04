/*
 * orb_pool.c — the theme pool's table and its compaction (docs/theme-pool-plan.md §2).
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
enum { H_MAGIC, H_FORMAT, H_SERIAL, H_N, H_ENTRY_BYTES, H_CRC };
#define E_WORDS_AT (ORB_POOL_SLUG_MAX + ORB_POOL_NAME_MAX)

static uint32_t *entry_word(orb_pool_entry_t *e, int k)
{
    uint32_t *w[] = { &e->off, &e->size, &e->base, &e->geom_size, &e->geom_crc,
                      &e->art_off, &e->art_size, &e->art_crc, &e->file_crc, &e->serial,
                      &e->state, &e->abi };
    return w[k];
}
#define E_WORDS 12

/* The CRC over a table's used bytes with its own CRC word read as zero. */
static uint32_t table_crc(const uint8_t *t, uint32_t n)
{
    static const uint8_t zero[4] = { 0 };
    const uint32_t used = ORB_POOL_HEAD_BYTES + n * ORB_POOL_ENTRY_BYTES;
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
    for (uint32_t i = 0; i < p->n && i < ORB_POOL_ENTRIES; i++) {
        uint8_t *r = out + ORB_POOL_HEAD_BYTES + i * ORB_POOL_ENTRY_BYTES;
        orb_pool_entry_t e = p->e[i];
        memcpy(r, e.slug, ORB_POOL_SLUG_MAX);
        memcpy(r + ORB_POOL_SLUG_MAX, e.name, ORB_POOL_NAME_MAX);
        for (int k = 0; k < E_WORDS; k++) put32(r + E_WORDS_AT + 4 * k, *entry_word(&e, k));
    }
    put32(out + H_CRC * 4, table_crc(out, p->n < ORB_POOL_ENTRIES ? p->n : ORB_POOL_ENTRIES));
}

static bool str_ok(const char *s, size_t cap, bool need)
{
    const char *z = memchr(s, 0, cap);
    return z && (!need || z != s);
}

const char *orb_pool_decode(const uint8_t *in, size_t len, orb_pool_t *out)
{
    memset(out, 0, sizeof *out);
    if (len < ORB_POOL_SECTOR) return "a table is one sector";
    if (get32(in + H_MAGIC * 4) != ORB_POOL_MAGIC) return "no pool table here";
    if (get32(in + H_FORMAT * 4) != ORB_POOL_FORMAT) return "a pool table of another format";
    const uint32_t n = get32(in + H_N * 4);
    if (n > ORB_POOL_ENTRIES) return "more entries than the pool holds";
    if (get32(in + H_ENTRY_BYTES * 4) != ORB_POOL_ENTRY_BYTES) return "entries of another width";
    if (get32(in + H_CRC * 4) != table_crc(in, n)) return "the table's CRC is wrong";
    out->serial = get32(in + H_SERIAL * 4);
    out->n = n;
    uint32_t end = ORB_POOL_FIRST;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *r = in + ORB_POOL_HEAD_BYTES + i * ORB_POOL_ENTRY_BYTES;
        orb_pool_entry_t *e = &out->e[i];
        memcpy(e->slug, r, ORB_POOL_SLUG_MAX);
        memcpy(e->name, r + ORB_POOL_SLUG_MAX, ORB_POOL_NAME_MAX);
        for (int k = 0; k < E_WORDS; k++) *entry_word(e, k) = get32(r + E_WORDS_AT + 4 * k);
        if (!str_ok(e->slug, ORB_POOL_SLUG_MAX, true)) return "an entry with no app";
        if (!str_ok(e->name, ORB_POOL_NAME_MAX, false)) return "an entry's name runs on";
        for (uint32_t j = 0; j < i; j++)
            if (!strcmp(out->e[j].slug, e->slug)) return "two entries for one app";
        if (e->state != ORB_POOL_LIVE && e->state != ORB_POOL_MOVING)
            return "an entry in no known state";
        if (e->off % ORB_POOL_SECTOR || e->size % ORB_POOL_SECTOR || !e->size)
            return "an entry off the sector grid";
        if (e->off < end) return "entries out of order or overlapping";
        if (e->size > ORB_POOL_END - e->off) return "an entry past the pool's end";
        if (e->geom_size > e->art_off || e->art_off > e->size ||
            e->art_size > e->size - e->art_off)
            return "an entry's index or banks outside it";
        end = e->off + e->size;
    }
    return NULL;
}

bool orb_pool_packed(const orb_pool_t *p)
{
    uint32_t at = ORB_POOL_FIRST;
    for (uint32_t i = 0; i < p->n; i++) {
        if (p->e[i].off != at) return false;
        at += p->e[i].size;
    }
    return true;
}

uint32_t orb_pool_used_end(const orb_pool_t *p)
{
    return p->n ? p->e[p->n - 1].off + p->e[p->n - 1].size : ORB_POOL_FIRST;
}

int orb_pool_find(const orb_pool_t *p, const char *slug)
{
    for (uint32_t i = 0; i < p->n; i++)
        if (!strncmp(p->e[i].slug, slug, ORB_POOL_SLUG_MAX)) return (int)i;
    return -1;
}

uint32_t orb_pool_round(uint32_t n)
{
    return (n + ORB_POOL_SECTOR - 1u) & ~(ORB_POOL_SECTOR - 1u);
}

int orb_pool_crc(const orb_pool_io_t *io, uint32_t off, uint32_t n, uint32_t *crc)
{
    uint32_t c = 0;
    while (n) {
        const uint32_t k = n < io->buf_bytes ? n : io->buf_bytes;
        const int r = io->read(io->ctx, off, io->buf, k);
        if (r) return r;
        c = esp_rom_crc32_le(c, io->buf, k);
        off += k;
        n -= k;
    }
    *crc = c;
    return 0;
}

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
        if (!a_ok) { memset(p, 0, sizeof *p); return 0; }
        r = io->read(io->ctx, ORB_POOL_TABLE_A, io->buf, ORB_POOL_SECTOR);
        if (r) return r;
        (void)orb_pool_decode(io->buf, ORB_POOL_SECTOR, p);
    }
    if (a_ok) *which = 1;
    else memset(p, 0, sizeof *p);
    return 0;
}

int orb_pool_write(const orb_pool_io_t *io, orb_pool_t *p, int *which)
{
    /* Never write a table this file would refuse to read. Static, not on the stack:
     * a pool is a kilobyte, and pool work is one worker's, never two at once. */
    static orb_pool_t check;
    p->serial++;
    orb_pool_encode(p, io->buf);
    if (orb_pool_decode(io->buf, ORB_POOL_SECTOR, &check)) return ORB_POOL_ERR_TABLE;
    const uint32_t at = *which == 1 ? ORB_POOL_TABLE_B : ORB_POOL_TABLE_A;
    int r = io->erase(io->ctx, at, ORB_POOL_SECTOR);
    if (!r) r = io->write(io->ctx, at, io->buf, ORB_POOL_SECTOR);
    if (r) return r;
    *which = at == ORB_POOL_TABLE_A ? 1 : 2;
    return 0;
}

/* Entry k down to dst: MOVING, the bytes sector by sector in ascending order (so an
 * overlapping move downward never erases a sector it has not copied), read back
 * against its CRCs, then LIVE at dst. */
static int move_entry(const orb_pool_io_t *io, orb_pool_t *p, int *which, uint32_t k,
                      uint32_t dst)
{
    orb_pool_entry_t *e = &p->e[k];
    const uint32_t src = e->off;
    e->state = ORB_POOL_MOVING;
    int r = orb_pool_write(io, p, which);
    if (r) return r;
    for (uint32_t o = 0; o < e->size; o += ORB_POOL_SECTOR) {
        if ((r = io->read(io->ctx, src + o, io->buf, ORB_POOL_SECTOR)) ||
            (r = io->erase(io->ctx, dst + o, ORB_POOL_SECTOR)) ||
            (r = io->write(io->ctx, dst + o, io->buf, ORB_POOL_SECTOR)))
            return r;
    }
    uint32_t gc, ac;
    if ((r = orb_pool_crc(io, dst, e->geom_size, &gc)) ||
        (r = orb_pool_crc(io, dst + e->art_off, e->art_size, &ac)))
        return r;
    if (gc != e->geom_crc || ac != e->art_crc) return ORB_POOL_ERR_VERIFY;
    e->off = dst;
    e->state = ORB_POOL_LIVE;
    return orb_pool_write(io, p, which);
}

int orb_pool_compact(const orb_pool_io_t *io, orb_pool_t *p, int *which)
{
    uint32_t at = ORB_POOL_FIRST;
    for (uint32_t k = 0; k < p->n; k++) {
        if (p->e[k].off > at) {
            const int r = move_entry(io, p, which, k, at);
            if (r) return r;
        }
        at = p->e[k].off + p->e[k].size;
    }
    return 0;
}

static void drop(orb_pool_t *p, uint32_t k)
{
    memmove(&p->e[k], &p->e[k + 1], (p->n - k - 1u) * sizeof p->e[0]);
    p->n--;
    memset(&p->e[p->n], 0, sizeof p->e[0]);
}

/* An entry the table calls MOVING still stands where the table says it was unless the
 * move overwrote its own first sectors (only a move by less than its size can), so it
 * is read back there: whole, it is LIVE again and the compaction moves it anew; torn,
 * it is dropped and its app draws its base face. */
int orb_pool_repair(const orb_pool_io_t *io, orb_pool_t *p, int *which)
{
    bool dropped = false;
    for (uint32_t k = p->n; k-- > 0;) {
        orb_pool_entry_t *e = &p->e[k];
        if (e->state == ORB_POOL_LIVE) continue;
        uint32_t gc, ac;
        int r = orb_pool_crc(io, e->off, e->geom_size, &gc);
        if (!r) r = orb_pool_crc(io, e->off + e->art_off, e->art_size, &ac);
        if (r) return r;
        if (gc == e->geom_crc && ac == e->art_crc) e->state = ORB_POOL_LIVE;
        else drop(p, k);
        dropped = true;
    }
    if (dropped) {
        const int r = orb_pool_write(io, p, which);
        if (r) return r;
    }
    return orb_pool_compact(io, p, which);
}

int orb_pool_remove(const orb_pool_io_t *io, orb_pool_t *p, int *which, const char *slug)
{
    const int k = orb_pool_find(p, slug);
    if (k >= 0) {
        drop(p, (uint32_t)k);
        const int r = orb_pool_write(io, p, which);
        if (r) return r;
    }
    return orb_pool_compact(io, p, which);
}

int orb_pool_reserve(const orb_pool_io_t *io, orb_pool_t *p, int *which,
                     const char *slug, uint32_t size, uint32_t *at)
{
    if (!size) return ORB_POOL_ERR_ARG;
    const int k = orb_pool_find(p, slug);
    uint64_t total = orb_pool_round(size);
    for (uint32_t i = 0; i < p->n; i++)
        if ((int)i != k) total += p->e[i].size;
    if (total > ORB_POOL_END - ORB_POOL_FIRST) return ORB_POOL_ERR_FULL;
    if (k < 0 && p->n >= ORB_POOL_ENTRIES) return ORB_POOL_ERR_TABLE;
    const int r = orb_pool_remove(io, p, which, slug);
    if (r) return r;
    *at = orb_pool_used_end(p);
    return 0;
}

int orb_pool_commit(const orb_pool_io_t *io, orb_pool_t *p, int *which,
                    const orb_pool_entry_t *e)
{
    if (p->n >= ORB_POOL_ENTRIES || orb_pool_find(p, e->slug) >= 0 ||
        e->off != orb_pool_used_end(p))
        return ORB_POOL_ERR_ARG;
    p->e[p->n] = *e;
    p->e[p->n].state = ORB_POOL_LIVE;
    p->n++;
    const int r = orb_pool_write(io, p, which);
    if (r) { p->n--; memset(&p->e[p->n], 0, sizeof p->e[0]); }
    return r;
}
