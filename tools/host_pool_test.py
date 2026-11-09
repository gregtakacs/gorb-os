#!/usr/bin/env python3
"""Host suite for main/art/orb_pool.c, the theme pool (theme-cache-plan.md §4.1, format 3).

Compiles the shipped file with host gcc and drives it over a simulated 8 MB NOR flash:
a write can only clear bits (so a missing erase shows), and power can be cut at any erase
or write (the op lands half done and every later access fails). Each run then "reboots":
load, and check what is left.

    python3 tools/host_pool_test.py [--mutations]

What it proves:
  - decode refuses every malformed format 3 table, by the sentence that names it, and reads
    a format 2 table for what it says (format 2), which a reset replaces with an empty
    format 3 pool at a higher serial;
  - an entry's bytes go through its pages: a read, write, erase and checksum across page
    boundaries land on the right pages, and on no other;
  - stores and removals never move a byte of any other entry: after a removal leaves a hole,
    the next store takes the hole's pages and free pages past the others (scattered), and
    every other entry's flash is byte-for-byte as it was;
  - the eviction: least recently on the glass first, an app the order does not name older
    than any it does, `keep` never, a voice bank never, a refusal touching nothing, the
    table's slots;
  - a cut at every single step of a store that evicts two themes leaves a valid table, every
    entry's bytes matching its CRCs, every surviving entry's flash untouched, and either the
    old theme set less the evicted or the new one;
  - commit refuses a page already owned, a page count that is not the size, pages out of
    order, and an app's second entry of a kind.
--mutations breaks each guard in turn and checks a test notices.
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
POOL_C = os.path.join(ROOT, "main", "art", "orb_pool.c")
POOL_H = os.path.join(ROOT, "main", "art", "orb_pool.h")

SHIM_CRC = """#pragma once
#include <stdint.h>
#include <stddef.h>
#include <zlib.h>
static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, size_t len)
{ return (uint32_t)crc32((uLong)crc, buf, (uInt)len); }
"""

HARNESS_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_rom_crc.h"
#include "orb_pool.h"

static int g_fail, g_quick;
#define CK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the simulated flash ---- */
static uint8_t *fl;
static long g_ops, g_cut = -1;
static int g_dead;

static int f_read(void *c, uint32_t off, void *dst, uint32_t n)
{
    (void)c;
    if (g_dead) return -1;
    if (off + n > ORB_POOL_PART_BYTES) return -2;
    memcpy(dst, fl + off, n);
    return 0;
}
static int cut_now(void) { return g_cut >= 0 && ++g_ops == g_cut; }
static int f_erase(void *c, uint32_t off, uint32_t n)
{
    (void)c;
    if (g_dead) return -1;
    if (off % ORB_POOL_SECTOR || n % ORB_POOL_SECTOR || off + n > ORB_POOL_PART_BYTES) return -3;
    if (cut_now()) { memset(fl + off, 0xFF, n / 2); g_dead = 1; return -1; }
    memset(fl + off, 0xFF, n);
    return 0;
}
static int f_write(void *c, uint32_t off, const void *src, uint32_t n)
{
    (void)c;
    if (g_dead) return -1;
    if (off + n > ORB_POOL_PART_BYTES) return -4;
    const uint8_t *s = src;
    const int cut = cut_now();
    const uint32_t m = cut ? n / 2 : n;
    for (uint32_t i = 0; i < m; i++) fl[off + i] &= s[i];       /* NOR: bits only clear */
    if (cut) { g_dead = 1; return -1; }
    return 0;
}

static uint8_t g_buf[ORB_POOL_SECTOR];
static const orb_pool_io_t IO = { NULL, f_read, f_erase, f_write, g_buf, sizeof g_buf, NULL };

static void power(long cut) { g_ops = 0; g_cut = cut; g_dead = 0; }

/* ---- themes: an index and banks from a seed, through the entry's pages ---- */
typedef struct { const char *slug; uint32_t kind, geom, art_off, art; uint8_t seed; } th_t;

static uint8_t pat(uint8_t seed, uint32_t i) { return (uint8_t)(seed * 31u + i * 7u + (i >> 9)); }

static int write_theme(const th_t *t, const orb_pool_pages_t *pg, orb_pool_entry_t *e)
{
    memset(e, 0, sizeof *e);
    snprintf(e->slug, sizeof e->slug, "%s", t->slug);
    snprintf(e->name, sizeof e->name, "%s theme %u", t->slug, t->seed);
    e->geom_size = t->geom; e->art_off = t->art_off; e->art_size = t->art;
    e->size = orb_pool_round(t->art_off + t->art);
    e->kind = t->kind;
    static uint8_t sec[ORB_POOL_SECTOR];
    for (uint32_t o = 0; o < e->size; o += ORB_POOL_SECTOR) {
        for (uint32_t i = 0; i < ORB_POOL_SECTOR; i++) {
            const uint32_t b = o + i;
            sec[i] = (b < t->geom || (b >= t->art_off && b < t->art_off + t->art)) ? pat(t->seed, b) : 0;
        }
        int r = orb_pool_perase(&IO, pg, o, ORB_POOL_SECTOR);
        if (!r) r = orb_pool_pwrite(&IO, pg, o, sec, ORB_POOL_SECTOR);
        if (r) return r;
    }
    int r = orb_pool_pcrc(&IO, pg, 0, t->geom, &e->geom_crc);
    if (!r) r = orb_pool_pcrc(&IO, pg, t->art_off, t->art, &e->art_crc);
    e->serial = t->seed;
    e->state = ORB_POOL_LIVE;
    return r;
}

static const char *ORDER0[] = { NULL };
static int store_o(orb_pool_t *p, int *w, const th_t *t, const char *const *order, uint32_t n_order,
                   const char *keep, uint32_t *n_ev)
{
    orb_pool_pages_t pg;
    uint32_t nev = 0;
    int r = orb_pool_reserve(&IO, p, w, t->slug, t->kind, t->art_off + t->art, order, n_order, keep,
                             &pg, NULL, &nev);
    if (n_ev) *n_ev = nev;
    if (r) return r;
    orb_pool_entry_t e;
    if ((r = write_theme(t, &pg, &e))) return r;
    return orb_pool_commit(&IO, p, w, &e, &pg);
}
static int store(orb_pool_t *p, int *w, const th_t *t) { return store_o(p, w, t, ORDER0, 0, NULL, NULL); }

/* Every entry LIVE, its bytes its CRCs and its own seed's pattern. */
static int sound(const orb_pool_t *p, const char *ctx)
{
    int bad = 0;
    for (uint32_t k = 0; k < p->n; k++) {
        const orb_pool_entry_t *e = &p->e[k];
        orb_pool_pages_t pg;
        orb_pool_pages(p, k, &pg);
        uint32_t gc = 0, ac = 0;
        uint8_t b5 = 0;
        orb_pool_pcrc(&IO, &pg, 0, e->geom_size, &gc);
        orb_pool_pcrc(&IO, &pg, e->art_off, e->art_size, &ac);
        orb_pool_pread(&IO, &pg, 5, &b5, 1);
        if (e->state != ORB_POOL_LIVE || gc != e->geom_crc || ac != e->art_crc ||
            b5 != pat((uint8_t)e->serial, 5) || pg.n * ORB_POOL_PAGE != e->size) {
            printf("FAIL %s: %s (%u pages) crc %s\n", ctx, e->slug, pg.n,
                   gc == e->geom_crc && ac == e->art_crc ? "good" : "BAD");
            bad++;
        }
    }
    g_fail += bad;
    return bad;
}

static int reboot(orb_pool_t *p, int *w)
{
    power(-1);
    return orb_pool_load(&IO, p, w);
}

/* sizes in whole pages, as a theme's are rounded: 5, 2, 1, 4 and a voice bank of 3 */
static const th_t CLOCK  = { "clock",   ORB_POOL_DISPLAY, 0x3000, 0x10000, 0x40000, 11 };
static const th_t FLIGHT = { "flights", ORB_POOL_DISPLAY, 0x2000, 0x10000, 0x10000, 22 };
static const th_t PICKER = { "color",   ORB_POOL_DISPLAY, 0x1000, 0x1000,  0xC000,  33 };
static const th_t WX     = { "weather", ORB_POOL_DISPLAY, 0x2000, 0x10000, 0x30000, 44 };
static const th_t CVOICE = { "clock",   ORB_POOL_VOICE,   0x200,  0x200,   0x2FE00, 55 };

static void fresh(orb_pool_t *p, int *w)
{
    memset(fl, 0xFF, ORB_POOL_PART_BYTES);
    power(-1);
    CK(orb_pool_load(&IO, p, w) == 0 && *w == 0 && p->n == 0, "an erased pool loads empty");
    CK(store(p, w, &CLOCK) == 0, "store clock");
    CK(store(p, w, &FLIGHT) == 0, "store flights");
    CK(store(p, w, &PICKER) == 0, "store color");
}

/* The flash outside `skip`'s pages (and the tables) unchanged against `was`. */
static int only_touched(const uint8_t *was, const orb_pool_t *before, const char *skip_slug,
                        uint32_t skip_kind)
{
    int changed = 0;
    for (uint32_t k = 0; k < before->n; k++) {
        if (skip_slug && !strcmp(before->e[k].slug, skip_slug) && before->e[k].kind == skip_kind) continue;
        orb_pool_pages_t pg;
        orb_pool_pages(before, k, &pg);
        for (uint32_t i = 0; i < pg.n; i++)
            if (memcmp(was + pg.off[i], fl + pg.off[i], ORB_POOL_PAGE)) changed++;
    }
    return changed;
}

/* ---- decode refusals ---- */
static void t_decode(void)
{
    orb_pool_t p, q;
    int w;
    fresh(&p, &w);
    static uint8_t good[ORB_POOL_SECTOR], bad[ORB_POOL_SECTOR];
    orb_pool_encode(&p, good);
    CK(orb_pool_decode(good, sizeof good, &q) == NULL && q.n == 3 && q.format == 3, "a good table decodes");
    #define E0 (ORB_POOL_HEAD_BYTES)
    #define EW(k) (E0 + ORB_POOL_SLUG_MAX + ORB_POOL_NAME_MAX + 4 * (k))
    struct { const char *why; uint32_t at; uint32_t val; int width; } cases[] = {
        { "no pool table here", 0, 0x12345678u, 4 },
        { "a pool table of another format", 4, 4, 4 },
        { "more entries than the pool holds", 12, 17, 4 },
        { "entries of another width", 16, 132, 4 },
        { "a pool of another page count", 24, 128, 4 },
        { "the table's CRC is wrong", ORB_POOL_MAP_AT + 100, 0x01, 1 },
        { "an entry with no app", E0, 0, 1 },
        { "an entry of no known kind", EW(12), 3, 4 },
        { "an entry in no known state", EW(10), 2, 4 },
        { "an entry with no page id", EW(13), 0, 4 },
        { "an entry not in whole pages", EW(1), 0x51000, 4 },
        { "an entry's index or banks outside it", EW(6), 0x7FFFFF, 4 },
        { "a page owned by no entry", ORB_POOL_MAP_AT + 120, 77, 1 },
        { "an entry's first page is not its offset", EW(0), 0x20000, 4 },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        memcpy(bad, good, sizeof bad);
        if (cases[i].width == 4) {
            bad[cases[i].at] = (uint8_t)cases[i].val; bad[cases[i].at + 1] = (uint8_t)(cases[i].val >> 8);
            bad[cases[i].at + 2] = (uint8_t)(cases[i].val >> 16); bad[cases[i].at + 3] = (uint8_t)(cases[i].val >> 24);
        } else bad[cases[i].at] = (uint8_t)cases[i].val;
        /* re-seal the CRC except where the CRC is the point */
        if (strcmp(cases[i].why, "the table's CRC is wrong")) {
            orb_pool_t tmp;
            memset(&tmp, 0, sizeof tmp);
            /* recompute: the CRC word with itself zeroed, over the format 3 extent */
            uint32_t saved; memcpy(&saved, bad + 20, 4); (void)saved;
            uint8_t z[4] = { 0 };
            memcpy(bad + 20, z, 4);
            uint32_t c = esp_rom_crc32_le(0, bad, ORB_POOL_MAP_AT + ORB_POOL_PAGES);
            bad[20] = (uint8_t)c; bad[21] = (uint8_t)(c >> 8); bad[22] = (uint8_t)(c >> 16); bad[23] = (uint8_t)(c >> 24);
        }
        const char *got = orb_pool_decode(bad, sizeof bad, &q);
        CK(got && !strcmp(got, cases[i].why), "decode %s: got %s", cases[i].why, got ? got : "(accepted)");
    }
    /* two entries of a kind, and one page id twice */
    orb_pool_t d = p;
    memcpy(&d.e[1].slug, "clock", 6);
    orb_pool_encode(&d, bad);
    CK(orb_pool_decode(bad, sizeof bad, &q) && !strcmp(orb_pool_decode(bad, sizeof bad, &q), "two entries of a kind for one app"),
       "two display entries for one app refused");
    d = p; d.e[2].pid = d.e[1].pid;
    orb_pool_encode(&d, bad);
    const char *g2 = orb_pool_decode(bad, sizeof bad, &q);
    CK(g2 && !strcmp(g2, "two entries with one page id"), "one page id twice refused: %s", g2 ? g2 : "(accepted)");
    d = p; d.owner[30] = d.e[0].pid;          /* one page more than its size */
    orb_pool_encode(&d, bad);
    g2 = orb_pool_decode(bad, sizeof bad, &q);
    CK(g2 && !strcmp(g2, "an entry's pages are not its size"), "a page too many refused: %s", g2 ? g2 : "(accepted)");
}

/* ---- a format 2 table: read for what it says, then reset ---- */
static void t_format2(void)
{
    memset(fl, 0xFF, ORB_POOL_PART_BYTES);
    static uint8_t t2[ORB_POOL_SECTOR];
    memset(t2, 0, sizeof t2);
    #define P32(at, v) do { uint32_t _v = (v); t2[(at)] = (uint8_t)_v; t2[(at)+1] = (uint8_t)(_v >> 8); t2[(at)+2] = (uint8_t)(_v >> 16); t2[(at)+3] = (uint8_t)(_v >> 24); } while (0)
    P32(0, ORB_POOL_MAGIC); P32(4, 2); P32(8, 40); P32(12, 1); P32(16, 132);
    memcpy(t2 + 32, "clock", 6); memcpy(t2 + 32 + 24, "Neon moon", 10);
    const uint32_t ew = 32 + 24 + 56;
    P32(ew + 0, 0); P32(ew + 4, 0x2A3000); P32(ew + 12, 0x2000); P32(ew + 20, 0x10000); P32(ew + 24, 0x293000);
    P32(ew + 32, 0xABCDEF01u); P32(ew + 40, 1); P32(ew + 48, ORB_POOL_DISPLAY);
    uint32_t c = esp_rom_crc32_le(0, t2, 20);
    const uint8_t z[4] = { 0 };
    c = esp_rom_crc32_le(c, z, 4);
    c = esp_rom_crc32_le(c, t2 + 24, 32 + 132 - 24);
    P32(20, c);
    memcpy(fl + ORB_POOL_TABLE_A, t2, sizeof t2);
    orb_pool_t p;
    int w;
    power(-1);
    CK(orb_pool_load(&IO, &p, &w) == 0 && w == 1 && p.format == 2 && p.n == 1 &&
       !strcmp(p.e[0].slug, "clock") && p.e[0].file_crc == 0xABCDEF01u, "a format 2 table is read for what it says");
    CK(orb_pool_reset(&IO, &p, &w) == 0 && p.n == 0 && p.format == 3, "a reset starts an empty format 3 pool");
    orb_pool_t q;
    CK(reboot(&q, &w) == 0 && q.format == 3 && q.n == 0 && q.serial > 40, "the reset reads back, newer than the old");
}

/* ---- an entry's bytes across pages ---- */
static void t_pages(void)
{
    memset(fl, 0xFF, ORB_POOL_PART_BYTES);
    power(-1);
    orb_pool_pages_t pg = { 3, { 0x50000, 0x10000, 0x70000 } };    /* deliberately out of order */
    static uint8_t a[0x20000], b[0x20000];
    for (uint32_t i = 0; i < sizeof a; i++) a[i] = (uint8_t)(i * 13u + 1u);
    CK(orb_pool_perase(&IO, &pg, 0x8000, 0x20000) == 0, "erase across pages");
    CK(orb_pool_pwrite(&IO, &pg, 0x8000, a, 0x20000) == 0, "write across pages");
    CK(memcmp(fl + 0x58000, a, 0x8000) == 0 && memcmp(fl + 0x10000, a + 0x8000, 0x10000) == 0 &&
       memcmp(fl + 0x70000, a + 0x18000, 0x8000) == 0, "each part lands on its page");
    CK(fl[0x57FFF] == 0xFF && fl[0x20000] == 0xFF && fl[0x78000] == 0xFF, "and on no other byte");
    CK(orb_pool_pread(&IO, &pg, 0x8000, b, 0x20000) == 0 && !memcmp(a, b, 0x20000), "read back across pages");
    uint32_t c1 = 0;
    CK(orb_pool_pcrc(&IO, &pg, 0x8000, 0x20000, &c1) == 0 && c1 == esp_rom_crc32_le(0, a, 0x20000), "checksum across pages");
    CK(orb_pool_pread(&IO, &pg, 0x2F000, b, 0x2000) == ORB_POOL_ERR_ARG, "a read past its pages is refused");
}

/* ---- nothing moves: a hole's pages are taken, and nothing else is touched ---- */
static void t_scatter(void)
{
    orb_pool_t p;
    int w;
    fresh(&p, &w);                                     /* clock 5 pages, flights 2, color 1 */
    static uint8_t was[ORB_POOL_PART_BYTES];
    CK(orb_pool_remove(&IO, &p, &w, "flights") == 0 && p.n == 2, "remove flights");
    memcpy(was, fl, ORB_POOL_PART_BYTES);
    orb_pool_t before = p;
    CK(store(&p, &w, &WX) == 0, "store weather (4 pages) into a 2-page hole and beyond");
    const int k = orb_pool_find(&p, "weather");
    orb_pool_pages_t pg;
    orb_pool_pages(&p, (uint32_t)k, &pg);
    CK(pg.n == 4 && pg.off[0] == 5 * ORB_POOL_PAGE && pg.off[1] == 6 * ORB_POOL_PAGE &&
       pg.off[2] == 8 * ORB_POOL_PAGE && pg.off[3] == 9 * ORB_POOL_PAGE,
       "weather takes the hole (pages 5, 6) and the free pages after color (8, 9)");
    CK(only_touched(was, &before, NULL, 0) == 0, "no other theme's flash changed");
    sound(&p, "after the scattered store");
    orb_pool_t q;
    CK(reboot(&q, &w) == 0 && q.n == 3, "the scattered pool reloads");
    sound(&q, "the scattered pool, reloaded");
    CK(orb_pool_used(&q) == 10 * ORB_POOL_PAGE && orb_pool_free_pages(&q) == ORB_POOL_PAGES - 10, "ten pages used");
    /* a voice bank is just pages too */
    memcpy(was, fl, ORB_POOL_PART_BYTES);
    before = p;
    CK(store(&p, &w, &CVOICE) == 0, "a voice bank stored beside the display entries");
    CK(only_touched(was, &before, NULL, 0) == 0, "and it moved nothing");
    sound(&p, "with a voice bank");
}

/* ---- the eviction ---- */
static void t_evict(void)
{
    orb_pool_t p;
    int w;
    memset(fl, 0xFF, ORB_POOL_PART_BYTES);
    power(-1);
    (void)orb_pool_load(&IO, &p, &w);
    /* nine 14-page themes and a voice bank fill 127 pages to 126 + 1 */
    char slugs[9][8];
    th_t big[9];
    for (int i = 0; i < 9; i++) {
        snprintf(slugs[i], sizeof slugs[i], "app%d", i);
        big[i] = (th_t){ slugs[i], ORB_POOL_DISPLAY, 0x1000, 0x10000, 13 * ORB_POOL_PAGE, (uint8_t)(60 + i) };
    }
    for (int i = 0; i < 8; i++) CK(store(&p, &w, &big[i]) == 0, "store app%d", i);
    CK(store(&p, &w, &CVOICE) == 0, "store a voice bank");
    CK(orb_pool_free_pages(&p) == ORB_POOL_PAGES - 8 * 14 - 3, "the pages add up");
    uint32_t mask;
    const char *order[] = { "app3", "app0", "app1", "app2", "app4", "app5", "app6", "app7" };
    CK(orb_pool_evictions(&p, "app8", 14 * ORB_POOL_PAGE, order, 8, NULL, &mask) == 0 &&
       mask == (1u << orb_pool_find(&p, "app3")), "the least recent goes first");
    CK(orb_pool_evictions(&p, "app8", 14 * ORB_POOL_PAGE, order, 8, "app3", &mask) == 0 &&
       mask == (1u << orb_pool_find(&p, "app0")), "keep is never evicted");
    const char *partial[] = { "app0", "app1" };
    CK(orb_pool_evictions(&p, "app8", 14 * ORB_POOL_PAGE, partial, 2, NULL, &mask) == 0 &&
       mask == (1u << orb_pool_find(&p, "app2")), "an app the order does not name is older (table order)");
    static uint8_t was[ORB_POOL_PART_BYTES];
    memcpy(was, fl, ORB_POOL_PART_BYTES);
    orb_pool_t before = p;
    orb_pool_pages_t pg;
    CK(orb_pool_reserve(&IO, &p, &w, "huge", ORB_POOL_DISPLAY, ORB_POOL_CAP - 2 * ORB_POOL_PAGE,
                        order, 8, NULL, &pg, NULL, NULL) == ORB_POOL_ERR_FULL, "too big for the voice banks' room");
    CK(!memcmp(was, fl, ORB_POOL_PART_BYTES) && p.n == before.n && pg.n == 0, "and the refusal touched nothing");
    uint32_t nev = 0;
    CK(store_o(&p, &w, &big[8], order, 8, NULL, &nev) == 0 && nev == 1 && orb_pool_find(&p, "app3") < 0,
       "a store evicts one and lands");
    CK(orb_pool_find_kind(&p, "clock", ORB_POOL_VOICE) >= 0, "the voice bank stays");
    CK(only_touched(was, &before, "app3", ORB_POOL_DISPLAY) == 0, "no survivor's flash changed");
    sound(&p, "after the eviction");
}

/* ---- commit's refusals ---- */
static void t_commit(void)
{
    orb_pool_t p;
    int w;
    fresh(&p, &w);
    orb_pool_entry_t e;
    memset(&e, 0, sizeof e);
    strcpy(e.slug, "radar"); e.kind = ORB_POOL_DISPLAY; e.size = 2 * ORB_POOL_PAGE; e.art_off = 0x10000; e.art_size = 0x100;
    orb_pool_pages_t owned = { 2, { 0, 20 * ORB_POOL_PAGE } };
    CK(orb_pool_commit(&IO, &p, &w, &e, &owned) == ORB_POOL_ERR_ARG, "a page already owned is refused");
    orb_pool_pages_t three = { 3, { 20 * ORB_POOL_PAGE, 21 * ORB_POOL_PAGE, 22 * ORB_POOL_PAGE } };
    CK(orb_pool_commit(&IO, &p, &w, &e, &three) == ORB_POOL_ERR_ARG, "a page count that is not the size is refused");
    orb_pool_pages_t back = { 2, { 21 * ORB_POOL_PAGE, 20 * ORB_POOL_PAGE } };
    CK(orb_pool_commit(&IO, &p, &w, &e, &back) == ORB_POOL_ERR_ARG, "pages out of order are refused");
    strcpy(e.slug, "clock");
    orb_pool_pages_t two = { 2, { 20 * ORB_POOL_PAGE, 21 * ORB_POOL_PAGE } };
    CK(orb_pool_commit(&IO, &p, &w, &e, &two) == ORB_POOL_ERR_ARG, "an app's second display entry is refused");
    CK(p.n == 3, "and nothing was added");
    /* an entry the table could not read back (its banks past its pages) is never written */
    static uint8_t was[ORB_POOL_PART_BYTES];
    memcpy(was, fl, ORB_POOL_PART_BYTES);
    strcpy(e.slug, "radar"); e.art_size = 0x20000;
    CK(orb_pool_commit(&IO, &p, &w, &e, &two) == ORB_POOL_ERR_TABLE, "a table that would not read back is refused");
    CK(!memcmp(was, fl, ORB_POOL_PART_BYTES) && p.n == 3, "and the flash and the table are as they were");
}

/* ---- a cut at every step of a store that evicts two ---- */
static void t_cuts(void)
{
    static uint8_t base_flash[ORB_POOL_PART_BYTES], was[ORB_POOL_PART_BYTES];
    orb_pool_t p;
    int w;
    memset(fl, 0xFF, ORB_POOL_PART_BYTES);
    power(-1);
    (void)orb_pool_load(&IO, &p, &w);
    th_t t[4] = {
        { "a", ORB_POOL_DISPLAY, 0x1000, 0x10000, 39 * ORB_POOL_PAGE, 71 },
        { "b", ORB_POOL_DISPLAY, 0x1000, 0x10000, 39 * ORB_POOL_PAGE, 72 },
        { "c", ORB_POOL_DISPLAY, 0x1000, 0x10000, 39 * ORB_POOL_PAGE, 73 },
        { "d", ORB_POOL_DISPLAY, 0x1000, 0x10000, 69 * ORB_POOL_PAGE, 74 },
    };
    for (int i = 0; i < 3; i++) CK(store(&p, &w, &t[i]) == 0, "store %s", t[i].slug);
    CK(orb_pool_remove(&IO, &p, &w, "b") == 0, "a hole in the middle");
    CK(store(&p, &w, &t[1]) == 0, "b again, into its hole");
    memcpy(base_flash, fl, ORB_POOL_PART_BYTES);
    const orb_pool_t base = p;
    const int base_w = w;
    const char *order[] = { "a", "b", "c" };
    /* how many ops a clean store takes */
    power(-1);
    g_cut = 1L << 40;
    CK(store_o(&p, &w, &t[3], order, 3, NULL, NULL) == 0, "the clean store evicting a and b");
    const long total = g_ops;
    sound(&p, "the clean store");
    CK(orb_pool_find(&p, "a") < 0 && orb_pool_find(&p, "b") < 0 && orb_pool_find(&p, "c") >= 0, "a and b went");
    long step = g_quick ? 37 : 1;
    int checked = 0;
    for (long cut = 1; cut <= total; cut += step) {
        memcpy(fl, base_flash, ORB_POOL_PART_BYTES);
        p = base; w = base_w;
        power(cut);
        (void)store_o(&p, &w, &t[3], order, 3, NULL, NULL);
        memcpy(was, fl, ORB_POOL_PART_BYTES);
        orb_pool_t q;
        int qw;
        if (reboot(&q, &qw)) { g_fail++; printf("FAIL cut %ld: no table loads\n", cut); continue; }
        char ctx[48];
        snprintf(ctx, sizeof ctx, "cut %ld of %ld", cut, total);
        sound(&q, ctx);
        const bool old = orb_pool_find(&q, "a") >= 0;
        const bool neu = orb_pool_find(&q, "d") >= 0;
        CK(orb_pool_find(&q, "c") >= 0, "%s: c survives", ctx);
        CK(old ? (orb_pool_find(&q, "b") >= 0 && !neu) : true, "%s: the old set whole, or not the old set", ctx);
        CK(neu ? (!old && orb_pool_find(&q, "b") < 0) : true, "%s: the new set", ctx);
        /* c's flash is c's, whatever the cut */
        const int kc = orb_pool_find(&base, "c");
        orb_pool_pages_t pc;
        orb_pool_pages(&base, (uint32_t)kc, &pc);
        int moved = 0;
        for (uint32_t i = 0; i < pc.n; i++) moved += memcmp(base_flash + pc.off[i], fl + pc.off[i], ORB_POOL_PAGE) != 0;
        CK(!moved, "%s: c's pages untouched", ctx);
        /* and the store goes through after the cut */
        power(-1);
        CK(store_o(&q, &qw, &t[3], order, 3, NULL, NULL) == 0 || orb_pool_find(&q, "d") >= 0,
           "%s: the store lands after the cut", ctx);
        sound(&q, ctx);
        checked++;
    }
    printf("ok    %d cuts across a %ld-step store that evicts two, every one whole\n", checked, total);
}

int main(int argc, char **argv)
{
    g_quick = argc > 1 && !strcmp(argv[1], "--quick");
    fl = malloc(ORB_POOL_PART_BYTES);
    if (!fl) return 2;
    t_decode();   printf("ok    decode\n");
    t_format2();  printf("ok    a format 2 table, read and reset\n");
    t_pages();    printf("ok    an entry's bytes through its pages\n");
    t_scatter();  printf("ok    a hole's pages taken, nothing moved\n");
    t_evict();    printf("ok    the eviction\n");
    t_commit();   printf("ok    commit's refusals\n");
    t_cuts();
    printf("%d failure%s\n", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
"""

MUTANTS = (
    ("a page owned by no entry", "if (k == n) return \"a page owned by no entry\";", ""),
    ("an entry's page count", "if (cnt * ORB_POOL_PAGE != out->e[i].size) return \"an entry's pages are not its size\";", ""),
    ("an entry's first page", "if (first != out->e[i].off) return \"an entry's first page is not its offset\";", ""),
    ("one page id twice", "if (out->e[j].pid == e->pid) return \"two entries with one page id\";", ""),
    ("whole pages", "if (!e->size || e->size % ORB_POOL_PAGE || e->size > ORB_POOL_CAP) return \"an entry not in whole pages\";", ""),
    ("commit: an owned page", "page >= ORB_POOL_PAGES || p->owner[page]) return ORB_POOL_ERR_ARG;", "page >= ORB_POOL_PAGES) return ORB_POOL_ERR_ARG;"),
    ("commit: ascending pages", "if (i && pg->off[i] <= pg->off[i - 1]) return ORB_POOL_ERR_ARG;", ""),
    ("commit: the page count", "if (!pg->n || pg->n * ORB_POOL_PAGE != e->size) return ORB_POOL_ERR_ARG;", "if (!pg->n) return ORB_POOL_ERR_ARG;"),
    ("reserve: free pages only", "if (!p->owner[i]) pg->off[pg->n++] = i * ORB_POOL_PAGE;", "pg->off[pg->n++] = i * ORB_POOL_PAGE;"),
    ("a voice bank never evicted", "if (p->e[k].kind != ORB_POOL_DISPLAY) continue;", ""),
    ("keep never evicted", "if (keep && !strcmp(p->e[k].slug, keep)) continue;", ""),
    ("least recent first", "if (pick < 0 || rank[k] < rank[pick]) pick = (int)k;", "if (pick < 0 || rank[k] > rank[pick]) pick = (int)k;"),
    ("a part's page bound", "if (page >= pg->n) return ORB_POOL_ERR_ARG;", ""),
    ("the page split", "const uint32_t k = n < ORB_POOL_PAGE - in ? n : ORB_POOL_PAGE - in;", "const uint32_t k = n;"),
    ("drop frees its pages", "if (p->owner[pg] == p->e[k].pid) p->owner[pg] = 0;", ""),
    ("the write's own check", "if (orb_pool_decode(io->buf, ORB_POOL_SECTOR, &check)) { p->serial--; return ORB_POOL_ERR_TABLE; }", ""),
    ("the map in the CRC", "put32(out + H_CRC * 4, table_crc(out, T3_BYTES));", "put32(out + H_CRC * 4, table_crc(out, ORB_POOL_MAP_AT));"),
)


def build(d, src, strict=True):
    open(os.path.join(d, "esp_rom_crc.h"), "w").write(SHIM_CRC)
    open(os.path.join(d, "harness.c"), "w").write(HARNESS_C)
    open(os.path.join(d, "orb_pool.c"), "w").write(src)
    exe = os.path.join(d, "t")
    warn = ["-Wall", "-Wextra", "-Werror"] if strict else []   # a mutant need only compile
    cc = subprocess.run(["gcc", "-std=gnu11", "-O2", *warn,
                         "-Wno-unused-function", "-I", d, "-I", os.path.dirname(POOL_H),
                         os.path.join(d, "harness.c"), os.path.join(d, "orb_pool.c"),
                         "-lz", "-o", exe], capture_output=True, text=True)
    if cc.returncode:
        raise SystemExit("gcc failed:\n" + cc.stderr)
    return exe


def main():
    src = open(POOL_C).read()
    with tempfile.TemporaryDirectory() as d:
        exe = build(d, src)
        r = subprocess.run([exe], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        ok = r.returncode == 0
        if "--mutations" in sys.argv:
            caught = 0
            for what, old, new in MUTANTS:
                if src.count(old) != 1:
                    print("FAIL mutant %r: its guard is not in orb_pool.c once" % what)
                    ok = False
                    continue
                mexe = build(d, src.replace(old, new), strict=False)
                m = subprocess.run([mexe, "--quick"], capture_output=True, text=True, timeout=600)
                if m.returncode == 0:
                    print("FAIL uncaught mutant: %s" % what)
                    ok = False
                else:
                    caught += 1
            print("  mutants: %d of %d guards caught when broken" % (caught, len(MUTANTS)))
        print("all pool checks pass" if ok else "POOL FAILURES")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
