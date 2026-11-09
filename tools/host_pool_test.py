#!/usr/bin/env python3
"""Host suite for main/art/orb_pool.c, the theme pool (docs/theme-pool-plan.md §2).

Compiles the shipped file with host gcc and drives it over a simulated 8 MB NOR flash:
a write can only clear bits (so a missing erase shows), power can be cut at any erase
or write (the op lands half done and every later access fails), and a write can be told
to flip a byte silently. Each run then "reboots": load, repair, and check what is left.

    python3 tools/host_pool_test.py [--mutations]

What it proves:
  - decode refuses every malformed table, by the sentence that names it;
  - stores, replacements and removals keep the pool packed and every other theme
    byte-for-byte intact; a theme that cannot fit is refused with flash untouched;
  - a cut at every single step of a replacement and of a removal leaves a valid table,
    a packed pool, every entry's bytes matching its CRCs, and at most one theme lost
    besides the one being stored or removed; and a second cut during the repair too;
  - a move that reads back wrong is refused rather than recorded;
  - the theme cache's eviction (docs/theme-cache-plan.md §2.2): least recently on the glass
    first, an app the order does not name older than any it does, `keep` never, a refusal
    touching nothing, the table's eight slots, and a cut at every step of a fill that
    evicts two themes, and of one whose eviction slides the next theme over itself.
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

static int g_fail;
#define CK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the simulated flash ---- */
static uint8_t *fl, *snap;
static long g_ops, g_cut = -1, g_flip = -1;
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
    if (g_flip >= 0 && !cut && ++g_ops == g_flip) fl[off + n / 3] ^= 0x10;
    if (cut) { g_dead = 1; return -1; }
    return 0;
}

static uint8_t g_buf[ORB_POOL_SECTOR];
static long g_prog_calls;
static uint32_t g_prog_done, g_prog_total;
static void f_progress(void *c, uint32_t done, uint32_t total)
{
    (void)c;
    g_prog_calls++; g_prog_done = done; g_prog_total = total;
}
static const orb_pool_io_t IO = { NULL, f_read, f_erase, f_write, g_buf, sizeof g_buf, f_progress };

static void power(long cut) { g_ops = 0; g_cut = cut; g_dead = 0; g_flip = -1; }

/* ---- themes: an index region and banks filled from a seed, their CRCs taken ---- */
typedef struct { const char *slug; uint32_t geom, art_off, art; uint8_t seed; } th_t;

static uint8_t pat(uint8_t seed, uint32_t i) { return (uint8_t)(seed * 31u + i * 7u + (i >> 9)); }

static int write_theme(const th_t *t, uint32_t at, orb_pool_entry_t *e)
{
    memset(e, 0, sizeof *e);
    snprintf(e->slug, sizeof e->slug, "%s", t->slug);
    snprintf(e->name, sizeof e->name, "%s theme %u", t->slug, t->seed);
    e->off = at; e->base = at;
    e->geom_size = t->geom; e->art_off = t->art_off; e->art_size = t->art;
    e->size = orb_pool_round(t->art_off + t->art);
    static uint8_t sec[ORB_POOL_SECTOR];
    for (uint32_t o = 0; o < e->size; o += ORB_POOL_SECTOR) {
        for (uint32_t i = 0; i < ORB_POOL_SECTOR; i++) {
            const uint32_t b = o + i;
            sec[i] = (b < t->geom || (b >= t->art_off && b < t->art_off + t->art)) ? pat(t->seed, b) : 0;
        }
        int r = f_erase(NULL, at + o, ORB_POOL_SECTOR);
        if (!r) r = f_write(NULL, at + o, sec, ORB_POOL_SECTOR);
        if (r) return r;
    }
    int r = orb_pool_crc(&IO, at, t->geom, &e->geom_crc);
    if (!r) r = orb_pool_crc(&IO, at + t->art_off, t->art, &e->art_crc);
    e->serial = t->seed;
    e->state = ORB_POOL_LIVE;
    return r;
}

static int store(orb_pool_t *p, int *w, const th_t *t)
{
    uint32_t at;
    int r = orb_pool_reserve(&IO, p, w, t->slug, t->art_off + t->art, &at);
    if (r) return r;
    orb_pool_entry_t e;
    if ((r = write_theme(t, at, &e))) return r;
    return orb_pool_commit(&IO, p, w, &e);
}

/* Every entry LIVE, packed, its bytes its CRCs and its own seed's pattern. */
static int sound(const orb_pool_t *p, const char *ctx)
{
    int bad = 0;
    if (!orb_pool_packed(p)) { printf("FAIL %s: not packed\n", ctx); bad++; }
    for (uint32_t k = 0; k < p->n; k++) {
        const orb_pool_entry_t *e = &p->e[k];
        uint32_t gc = 0, ac = 0;
        orb_pool_crc(&IO, e->off, e->geom_size, &gc);
        orb_pool_crc(&IO, e->off + e->art_off, e->art_size, &ac);
        if (e->state != ORB_POOL_LIVE || gc != e->geom_crc || ac != e->art_crc ||
            fl[e->off + 5] != pat((uint8_t)e->serial, 5)) {
            printf("FAIL %s: %s at 0x%x is %s, crc %s\n", ctx, e->slug, e->off,
                   e->state == ORB_POOL_LIVE ? "live" : "not live",
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
    int r = orb_pool_load(&IO, p, w);
    if (!r) r = orb_pool_repair(&IO, p, w);
    return r;
}

static const th_t CLOCK  = { "clock",   0x3000, 0x4000, 0x280000, 11 };
static const th_t FLIGHT = { "flights", 0x2000, 0x2000, 0x70000,  22 };
static const th_t PICKER = { "color",   0x1000, 0x1000, 0xC000,   33 };
static const th_t FLIGHT2= { "flights", 0x2000, 0x3000, 0xE8000,  44 };

static void fresh(orb_pool_t *p, int *w)
{
    memset(fl, 0xFF, ORB_POOL_PART_BYTES);
    power(-1);
    CK(orb_pool_load(&IO, p, w) == 0 && *w == 0 && p->n == 0, "an erased pool loads empty");
    CK(store(p, w, &CLOCK) == 0, "store clock");
    CK(store(p, w, &FLIGHT) == 0, "store flights");
    CK(store(p, w, &PICKER) == 0, "store color");
}

/* A small theme first and a big one after it: removing the small one moves the big
 * one down by less than its own size, so source and destination overlap. */
static void small_first(orb_pool_t *p, int *w)
{
    memset(fl, 0xFF, ORB_POOL_PART_BYTES);
    power(-1);
    CK(orb_pool_load(&IO, p, w) == 0 && p->n == 0, "an erased pool loads empty");
    CK(store(p, w, &PICKER) == 0, "store color");
    CK(store(p, w, &CLOCK) == 0, "store clock");
    CK(store(p, w, &FLIGHT) == 0, "store flights");
}

/* ---- decode refusals ---- */
static void decode_vectors(void)
{
    orb_pool_t p, q;
    memset(&p, 0, sizeof p);
    p.serial = 7; p.n = 2;
    orb_pool_entry_t a = { "clock", "a", ORB_POOL_FIRST, 0x10000, ORB_POOL_FIRST, 0x1000, 1, 0x2000, 0x8000, 2, 0, 1, ORB_POOL_LIVE, 6 };
    orb_pool_entry_t b = a;
    snprintf(b.slug, sizeof b.slug, "flights");
    b.off = b.base = ORB_POOL_FIRST + 0x10000;
    p.e[0] = a; p.e[1] = b;
    static uint8_t t[ORB_POOL_SECTOR];
    orb_pool_encode(&p, t);
    CK(orb_pool_decode(t, sizeof t, &q) == NULL && q.n == 2 && q.serial == 7 &&
       !strcmp(q.e[1].slug, "flights") && q.e[1].off == b.off, "a good table round-trips");
    struct { const char *why; void (*f)(orb_pool_t *); } V[] = {
#define VEC(why, body) { why, ({ void f_(orb_pool_t *x) { body; } f_; }) }
        VEC("out of order or overlapping", x->e[1].off = ORB_POOL_FIRST + 0x8000),
        VEC("out of order or overlapping", { orb_pool_entry_t s = x->e[0]; x->e[0] = x->e[1]; x->e[1] = s; }),
        VEC("two entries for one app", snprintf(x->e[1].slug, 16, "clock")),
        VEC("no app", x->e[1].slug[0] = 0),
        VEC("sector grid", x->e[1].off += 0x800),
        VEC("sector grid", x->e[1].size = 0),
        VEC("past the pool's end", x->e[1].size = ORB_POOL_END),
        VEC("outside it", x->e[1].geom_size = 0x3000),
        VEC("outside it", x->e[1].art_size = 0x10000),
        VEC("no known state", x->e[1].state = 9),
        VEC("more entries", x->n = 9),
    };
    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++) {
        orb_pool_t m = p;
        V[i].f(&m);
        orb_pool_encode(&m, t);
        const char *why = orb_pool_decode(t, sizeof t, &q);
        CK(why && strstr(why, V[i].why), "decode vector %zu: %s, not \"%s\"", i, why ? why : "accepted", V[i].why);
    }
    orb_pool_encode(&p, t);
    t[40] ^= 1;
    CK(orb_pool_decode(t, sizeof t, &q) && strstr(orb_pool_decode(t, sizeof t, &q), "CRC"), "a flipped bit is the CRC's");
    orb_pool_encode(&p, t);
    t[0] ^= 1;
    CK(orb_pool_decode(t, sizeof t, &q) != NULL, "no magic, no table");
    printf("  decode: a good table round-trips, %zu malformed ones refused by name\n", sizeof V / sizeof V[0] + 2);
}

/* ---- the plain road ---- */
static void basic(void)
{
    orb_pool_t p;
    int w;
    fresh(&p, &w);
    CK(p.n == 3 && !sound(&p, "three stored"), "three themes stored and sound");
    const uint32_t clock_off = p.e[0].off;

    CK(store(&p, &w, &FLIGHT2) == 0, "replace flights with a bigger theme");
    CK(p.n == 3 && !strcmp(p.e[2].slug, "flights") && p.e[2].serial == 44 && p.e[0].off == clock_off,
       "the replacement goes last, the clock stays put");
    sound(&p, "after the replacement");

    orb_pool_t r; int rw;
    CK(orb_pool_load(&IO, &r, &rw) == 0 && r.n == 3 && r.serial == p.serial, "what is on flash is what was written");

    g_prog_calls = 0;
    CK(orb_pool_remove(&IO, &p, &w, "clock") == 0 && p.n == 2 && p.e[0].off == ORB_POOL_FIRST,
       "removing the clock moves the rest down to the start");
    CK(g_prog_calls == (long)((p.e[0].size + p.e[1].size) / ORB_POOL_SECTOR) &&
       g_prog_done == g_prog_total && g_prog_total == p.e[1].size,
       "a move reports every sector, and its last report is the whole entry (%ld calls)", g_prog_calls);
    sound(&p, "after removing the clock");
    CK(orb_pool_remove(&IO, &p, &w, "nobody") == 0 && p.n == 2, "removing an absent app is no failure");

    memcpy(snap, fl, ORB_POOL_PART_BYTES);
    const th_t huge = { "huge", 0x1000, 0x1000, ORB_POOL_END, 55 };
    CK(store(&p, &w, &huge) == ORB_POOL_ERR_FULL, "a theme bigger than the pool is refused");
    CK(!memcmp(snap, fl, ORB_POOL_PART_BYTES), "the refusal touched nothing");
    const th_t fills = { "flights", 0x1000, 0x1000, ORB_POOL_END - ORB_POOL_FIRST - p.e[0].size - 0x1000, 66 };
    CK(store(&p, &w, &fills) == 0 && orb_pool_used_end(&p) == ORB_POOL_END, "a theme that fills the pool exactly fits");
    sound(&p, "a full pool");

    fresh(&p, &w);
    char names[8][16];
    int r9 = 0;
    for (int i = 0; i < 5 && !r9; i++) {
        snprintf(names[i], 16, "app%d", i);
        th_t t = { names[i], 0x1000, 0x1000, 0x1000, (uint8_t)(70 + i) };
        r9 = store(&p, &w, &t);
    }
    th_t nine = { "ninth", 0x1000, 0x1000, 0x1000, 99 };
    CK(!r9 && p.n == 8 && store(&p, &w, &nine) == ORB_POOL_ERR_TABLE, "a ninth app is refused: the table holds eight");
    sound(&p, "eight apps");

    /* an overlapping move: the clock slides down by the small theme's size */
    small_first(&p, &w);
    CK(orb_pool_remove(&IO, &p, &w, "color") == 0 && p.n == 2 && p.e[0].off == ORB_POOL_FIRST &&
       !strcmp(p.e[0].slug, "clock"), "removing the small first theme slides the clock over itself");
    sound(&p, "after the overlapping move");

    /* the newer copy wins; a torn newer copy loses to the older */
    fresh(&p, &w);
    const uint32_t newer = w == 1 ? ORB_POOL_TABLE_A : ORB_POOL_TABLE_B;
    CK(orb_pool_load(&IO, &r, &rw) == 0 && rw == w && r.serial == p.serial, "the newer copy is the live one");
    fl[newer + 50] ^= 0x01;
    CK(orb_pool_load(&IO, &r, &rw) == 0 && rw != w && r.serial == p.serial - 1, "a torn newer copy falls back to the older");

    /* a move that reads back wrong is refused, not recorded */
    fresh(&p, &w);
    power(-1);
    g_ops = 0; g_flip = 3;
    const int vr = orb_pool_remove(&IO, &p, &w, "clock");
    g_flip = -1;
    CK(vr == ORB_POOL_ERR_VERIFY, "a move whose bytes read back wrong is refused (%d)", vr);
    CK(reboot(&p, &w) == 0 && orb_pool_find(&p, "clock") < 0 && p.n <= 2, "after it, the reboot drops what it must");
    sound(&p, "after the refused move");
    printf("  basic: store, replace, remove, a full pool, eight apps, the newer table, a bad move\n");
}

/* ---- the theme cache: evictions (theme-cache-plan §2.2) ---- */
static int store_lru(orb_pool_t *p, int *w, const th_t *t, const char *const *order, uint32_t n,
                     const char *keep, char (*ev)[ORB_POOL_SLUG_MAX], uint32_t *nev)
{
    uint32_t at;
    int r = orb_pool_reserve_lru(&IO, p, w, t->slug, t->art_off + t->art, order, n, keep, &at, ev, nev);
    if (r) return r;
    orb_pool_entry_t e;
    if ((r = write_theme(t, at, &e))) return r;
    return orb_pool_commit(&IO, p, w, &e);
}
static int has(char (*ev)[ORB_POOL_SLUG_MAX], uint32_t n, const char *s)
{
    for (uint32_t i = 0; i < n; i++) if (!strcmp(ev[i], s)) return 1;
    return 0;
}
static uint32_t room(const orb_pool_t *p, const char *a, const char *b)   /* the pool less a and b */
{
    uint32_t t = ORB_POOL_END - ORB_POOL_FIRST;
    for (uint32_t k = 0; k < p->n; k++)
        if ((a && !strcmp(p->e[k].slug, a)) || (b && !strcmp(p->e[k].slug, b))) t -= p->e[k].size;
    return t;
}
static const char *const ORDER[] = { "color", "clock", "flights" };   /* least recent first */

static void lru(void)
{
    orb_pool_t p; int w;
    char ev[8][ORB_POOL_SLUG_MAX]; uint32_t nev;
    uint32_t mask;

    /* a theme that needs two out: color (least recent) and then the clock, never flights */
    fresh(&p, &w);
    th_t radar = { "radar", 0x1000, 0x1000, room(&p, "flights", NULL) - 0x1000, 77 };
    memcpy(snap, fl, ORB_POOL_PART_BYTES);
    CK(orb_pool_evictions(&p, "radar", radar.art_off + radar.art, ORDER, 3, NULL, &mask) == 0 &&
       mask == ((1u << 0) | (1u << 2)), "the evictions are clock and color (mask 0x%x)", mask);
    CK(!memcmp(snap, fl, ORB_POOL_PART_BYTES), "asking the evictions touches nothing");
    CK(store_lru(&p, &w, &radar, ORDER, 3, NULL, ev, &nev) == 0 && nev == 2 && has(ev, nev, "color") &&
       has(ev, nev, "clock") && p.n == 2 && orb_pool_find(&p, "flights") == 0, "a fill evicts color and the clock, keeps flights");
    sound(&p, "after evicting two");

    /* least recent first: one out is enough, and it is color, not the clock */
    fresh(&p, &w);
    const th_t small = { "radar", 0x1000, 0x1000, room(&p, "flights", "clock") - 0x2000, 78 };
    CK(store_lru(&p, &w, &small, ORDER, 3, NULL, ev, &nev) == 0 && nev == 1 && has(ev, nev, "color") &&
       orb_pool_find(&p, "clock") >= 0, "one eviction is enough, and it is the least recent");
    sound(&p, "after evicting one");

    /* an app the order does not name is older than any it names, in pool order */
    fresh(&p, &w);
    const char *const named[] = { "flights" };
    const th_t mid = { "radar", 0x1000, 0x1000, room(&p, "flights", "color") - 0x1000, 79 };
    CK(store_lru(&p, &w, &mid, named, 1, NULL, ev, &nev) == 0 && nev == 1 && has(ev, nev, "clock"),
       "unnamed apps go first, the first in the pool first");
    sound(&p, "after evicting an unnamed one");

    /* keep is never evicted: a refusal, flash untouched */
    fresh(&p, &w);
    memcpy(snap, fl, ORB_POOL_PART_BYTES);
    CK(store_lru(&p, &w, &radar, ORDER, 3, "clock", ev, &nev) == ORB_POOL_ERR_FULL && p.n == 3,
       "a fill that needs the kept theme out is refused");
    CK(!memcmp(snap, fl, ORB_POOL_PART_BYTES), "the refusal touched nothing");
    CK(store_lru(&p, &w, &small, ORDER, 3, "color", ev, &nev) == 0 && nev == 1 && has(ev, nev, "clock") &&
       orb_pool_find(&p, "color") >= 0, "keep passes over the least recent to the next");
    sound(&p, "after keeping color");

    /* the app's own old entry goes, uncounted */
    fresh(&p, &w);
    CK(store_lru(&p, &w, &FLIGHT2, ORDER, 3, NULL, ev, &nev) == 0 && nev == 0 && p.n == 3 &&
       p.e[2].serial == 44, "a replacement that fits evicts nobody");
    sound(&p, "after a replacement");

    /* eight apps: a ninth evicts the least recent one */
    fresh(&p, &w);
    char names[8][16];
    for (int i = 0; i < 5; i++) {
        snprintf(names[i], 16, "app%d", i);
        th_t t = { names[i], 0x1000, 0x1000, 0x1000, (uint8_t)(80 + i) };
        CK(store(&p, &w, &t) == 0, "store %s", names[i]);
    }
    const char *const by[] = { "app3", "clock", "flights", "color", "app0", "app1", "app2", "app4" };
    th_t nine = { "ninth", 0x1000, 0x1000, 0x1000, 99 };
    CK(store_lru(&p, &w, &nine, by, 8, NULL, ev, &nev) == 0 && nev == 1 && has(ev, nev, "app3") && p.n == 8,
       "a ninth app evicts the least recent of eight");
    sound(&p, "nine apps through eight slots");

    /* nothing it may evict leaves room: refused */
    fresh(&p, &w);
    const th_t huge = { "huge", 0x1000, 0x1000, ORB_POOL_END, 55 };
    CK(store_lru(&p, &w, &huge, ORDER, 3, NULL, ev, &nev) == ORB_POOL_ERR_FULL && p.n == 3,
       "a theme bigger than the pool is refused, evicting nobody");
    printf("  lru: least recent first, unnamed oldest, keep never, a ninth app, refusals untouched\n");
}

/* ---- a cut at every step ---- */
typedef int (*op_t)(orb_pool_t *, int *);
static int op_replace(orb_pool_t *p, int *w) { return store(p, w, &FLIGHT2); }
static int op_remove(orb_pool_t *p, int *w) { return orb_pool_remove(&IO, p, w, "clock"); }
static int op_remove_small(orb_pool_t *p, int *w) { return orb_pool_remove(&IO, p, w, "color"); }
/* a fill that evicts the clock and color in a small-first pool: flights moves 2.5 MB down */
static int op_fill(orb_pool_t *p, int *w)
{
    const th_t t = { "radar", 0x1000, 0x1000, room(p, "flights", NULL) - 0x1000, 77 };
    return store_lru(p, w, &t, ORDER, 3, NULL, NULL, NULL);
}
/* a fill that evicts color alone, first in the pool: the clock slides over itself */
static int op_fill_small(orb_pool_t *p, int *w)
{
    static const char *const o[] = { "color", "flights", "clock" };
    const th_t t = { "radar", 0x1000, 0x1000, room(p, "clock", "flights") - 0x1000, 77 };
    return store_lru(p, w, &t, o, 3, NULL, NULL, NULL);
}
static int targeted(const char *targets, const char *slug)   /* targets: comma separated */
{
    const size_t n = strlen(slug);
    for (const char *s = targets; s && *s;) {
        const char *e = strchr(s, ',');
        const size_t m = e ? (size_t)(e - s) : strlen(s);
        if (m == n && !strncmp(s, slug, n)) return 1;
        s = e ? e + 1 : NULL;
    }
    return 0;
}

static void cuts(const char *what, void (*setup)(orb_pool_t *, int *), op_t op,
                 const char *target, int nested)
{
    orb_pool_t p, base;
    int w, bw;
    setup(&base, &bw);
    static uint8_t *before, *cutimg;
    if (!before) { before = malloc(ORB_POOL_PART_BYTES); cutimg = malloc(ORB_POOL_PART_BYTES); }
    memcpy(before, fl, ORB_POOL_PART_BYTES);
    long n = 1, runs = 0, worst = 0;
    for (;; n++) {
        memcpy(fl, before, ORB_POOL_PART_BYTES);
        p = base; w = bw;
        power(n);
        const int r = op(&p, &w);
        if (!g_dead) { CK(r == 0, "%s: finished uncut but answered %d", what, r); break; }
        memcpy(cutimg, fl, ORB_POOL_PART_BYTES);
        for (long m = 0; m <= (nested ? 1 : 0); m++) {
            memcpy(fl, cutimg, ORB_POOL_PART_BYTES);
            long second = m ? 1 + (n * 7919) % 400 : -1;
            orb_pool_t q; int qw;
            power(-1);
            if (second > 0) {
                orb_pool_load(&IO, &q, &qw);
                power(second);
                orb_pool_repair(&IO, &q, &qw);
            }
            if (reboot(&q, &qw)) { printf("FAIL %s: cut at %ld: the repair failed\n", what, n); g_fail++; continue; }
            char ctx[96];
            snprintf(ctx, sizeof ctx, "%s cut at %ld%s", what, n, second > 0 ? " and in the repair" : "");
            sound(&q, ctx);
            int lost = 0;
            for (uint32_t k = 0; k < base.n; k++)
                if (!targeted(target, base.e[k].slug) && orb_pool_find(&q, base.e[k].slug) < 0) lost++;
            if (lost > 1) { printf("FAIL %s: %d other themes lost\n", ctx, lost); g_fail++; }
            if (lost > worst) worst = lost;
            runs++;
        }
    }
    printf("  cuts: %s, a cut at each of %ld steps%s, %ld reboots, at most %ld other theme lost\n",
           what, n - 1, nested ? " and again in each repair" : "", runs, worst);
}

int main(int argc, char **argv)
{
    fl = malloc(ORB_POOL_PART_BYTES);
    snap = malloc(ORB_POOL_PART_BYTES);
    const int quick = argc > 1 && !strcmp(argv[1], "--quick");
    decode_vectors();
    basic();
    cuts("replacing flights", fresh, op_replace, "flights", !quick);
    cuts("removing the clock", fresh, op_remove, "clock", !quick);
    cuts("removing a small first theme (an overlapping move)", small_first, op_remove_small,
         "color", !quick);
    lru();
    cuts("a fill evicting two themes", small_first, op_fill, "radar,clock,color", !quick);
    cuts("a fill evicting the small first theme (the clock slides over itself)", small_first,
         op_fill_small, "radar,color", !quick);
    printf("%d failures\n", g_fail);
    return g_fail ? 1 : 0;
}
"""

# (what, the guard as written, its break, the test that must notice)
MUTANTS = (
    ("the MOVING mark", "    e->state = ORB_POOL_MOVING;\n    int r = orb_pool_write(io, p, which);",
     "    int r = 0;"),
    ("the copy alternation", "*which == 1 ? ORB_POOL_TABLE_B : ORB_POOL_TABLE_A", "ORB_POOL_TABLE_A"),
    ("the repair's read-back",
     "if (gc == e->geom_crc && ac == e->art_crc) e->state = ORB_POOL_LIVE;\n        else drop(p, k);",
     "e->state = ORB_POOL_LIVE;"),
    ("the read-back", "if (gc != e->geom_crc || ac != e->art_crc) return ORB_POOL_ERR_VERIFY;", ""),
    ("the fit", "if (total > ORB_POOL_END - ORB_POOL_FIRST) return ORB_POOL_ERR_FULL;", ""),
    ("the ninth app", "if (k < 0 && p->n >= ORB_POOL_ENTRIES) return ORB_POOL_ERR_TABLE;", ""),
    ("the order check", "if (e->off < end) return", "if (0) return"),
    ("the newer copy", "get32(io->buf + H_SERIAL * 4) > a_serial", "get32(io->buf + H_SERIAL * 4) < a_serial"),
    ("the cache's keep", "if (keep && !strcmp(p->e[k].slug, keep)) continue;", ""),
    ("least recent first", "if (pick < 0 || rank[k] < rank[pick]) pick = (int)k;",
     "if (pick < 0 || rank[k] > rank[pick]) pick = (int)k;"),
    ("unnamed is oldest", "rank[k] = -1;                                /* not named: older than any named */",
     "rank[k] = 99;"),
    ("the cache's table slots", "while (total > ORB_POOL_END - ORB_POOL_FIRST || count > ORB_POOL_ENTRIES) {",
     "while (total > ORB_POOL_END - ORB_POOL_FIRST) {"),
    ("the ascending copy", "for (uint32_t o = 0; o < e->size; o += ORB_POOL_SECTOR) {",
     "for (uint32_t o = e->size - ORB_POOL_SECTOR; o < e->size; o -= ORB_POOL_SECTOR) {"),
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
