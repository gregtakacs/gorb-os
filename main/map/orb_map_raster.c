/* The map's vector renderer — contract in orb_map_raster.h. */
#include "orb_map_raster.h"

#include <math.h>
#include <string.h>

#define W   ORB_MAP_W
#define HDR 36

/* ---- the file -------------------------------------------------------------------- */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* zlib's CRC-32 a nibble at a time: a 64 B table in flash rather than 1 KB in RAM. */
static uint32_t crc32_z(const uint8_t *p, size_t n)
{
    static const uint32_t T[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        c = (c >> 4) ^ T[c & 15];
        c = (c >> 4) ^ T[c & 15];
    }
    return ~c;
}

int orb_map_check(const uint8_t *buf, size_t len, int *zoom)
{
    if (len < HDR || memcmp(buf, "ORBM", 4) || (buf[4] | buf[5] << 8) != 1) return ORB_MAP_E_FORMAT;
    const uint32_t hl = buf[6] | buf[7] << 8, blen = rd32(buf + 28);
    if (hl < HDR || hl > len || len - hl < blen) return ORB_MAP_E_TRUNC;
    if (crc32_z(buf + hl, blen) != rd32(buf + 32)) return ORB_MAP_E_CRC;
    if (zoom) *zoom = buf[16];
    return ORB_MAP_OK;
}

typedef struct { const uint8_t *p, *end; int bad; } rd_t;

static uint32_t rv(rd_t *r)
{
    uint32_t v = 0;
    for (int s = 0; s < 35; s += 7) {
        if (r->p >= r->end) { r->bad = 1; return 0; }
        const uint8_t b = *r->p++;
        v |= (uint32_t)(b & 0x7F) << s;
        if (b < 0x80) return v;
    }
    r->bad = 1;
    return 0;
}
static int32_t rz(rd_t *r) { const uint32_t v = rv(r); return (int32_t)(v >> 1) ^ -(int32_t)(v & 1); }
static uint8_t rb(rd_t *r) { if (r->p >= r->end) { r->bad = 1; return 0; } return *r->p++; }

/* ---- the raster ------------------------------------------------------------------ */

static inline void put(uint8_t *ras, int x, int y, uint8_t v)
{
    if ((unsigned)x >= W || (unsigned)y >= W) return;
    const uint32_t i = orb_map_nib(x, y);
    uint8_t *b = &ras[i >> 1];
    *b = (i & 1) ? (uint8_t)((*b & 0xF0) | v) : (uint8_t)((*b & 0x0F) | v << 4);
}

static void span(uint8_t *ras, int y, int x0, int x1, uint8_t v)   /* [x0, x1) */
{
    if (x0 < 0) x0 = 0;
    if (x1 > W) x1 = W;
    if (x0 >= x1) return;
    const uint8_t vv = (uint8_t)(v | v << 4);
    /* One tile row's 8 pixels are 4 bytes: whole ones are written as bytes. */
    uint8_t *row = ras + (((uint32_t)(y >> 3) * ORB_MAP_TW) << 5) + ((y & 7) << 2);
    int x = x0;
    while (x < x1) {
        uint8_t *t = row + ((x >> 3) << 5);
        const int tx0 = x & 7, tx1 = (x1 - (x & ~7)) < 8 ? x1 - (x & ~7) : 8;
        if (tx0 == 0 && tx1 == 8) { t[0] = t[1] = t[2] = t[3] = vv; }
        else for (int k = tx0; k < tx1; k++) {
            uint8_t *b = &t[k >> 1];
            *b = (k & 1) ? (uint8_t)((*b & 0xF0) | v) : (uint8_t)((*b & 0x0F) | v << 4);
        }
        x = (x & ~7) + 8;
    }
}

/* ---- the transform: file units to glass pixels ---------------------------------- */

typedef struct { float a, b, c, d; } xf_t;   /* x' = a*x + b*y, y' = c*x + d*y, then + centre */

static inline void tx(const xf_t *t, int32_t x, int32_t y, float *ox, float *oy)
{
    *ox = W * 0.5f + t->a * (float)x + t->b * (float)y;
    *oy = W * 0.5f + t->c * (float)x + t->d * (float)y;
}

/* ---- fills: even-odd scanline over all of one feature's rings --------------------- */

typedef struct { float x, dxdy; int16_t y0, y1; } edge_t;   /* rows [y0, y1); 12 B */

static int add_edge(edge_t *e, size_t cap, uint32_t *n, float x0, float y0, float x1, float y1)
{
    if (y0 == y1) return 1;
    if (y0 > y1) { float t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    /* A row is sampled at its centre: rows r with y0 <= r + 0.5 < y1. */
    int r0 = (int)ceilf(y0 - 0.5f), r1 = (int)ceilf(y1 - 0.5f);
    if (r0 < 0) r0 = 0;
    if (r1 > W) r1 = W;
    if (r0 >= r1) return 1;
    if (*n >= cap) return 0;
    edge_t *g = &e[(*n)++];
    g->dxdy = (x1 - x0) / (y1 - y0);
    g->x = x0 + ((float)r0 + 0.5f - y0) * g->dxdy;
    g->y0 = (int16_t)r0;
    g->y1 = (int16_t)r1;
    return 1;
}

/* Scratch, per fill: row counts, then the edges, then the edges' order by first row,
 * then the active list. A counting sort keeps the order linear: rings arrive up and
 * down, and an insertion sort of edges living in PSRAM was quadratic on a metro's
 * urban outline (z8: ~3 s on Bronze, 2026-11-07). */
typedef struct { uint32_t *cnt; edge_t *e; uint32_t *order, *act; size_t cap; } fillbuf_t;

static void fill(uint8_t *ras, const fillbuf_t *fb, uint32_t n, uint8_t v)
{
    if (!n) return;
    edge_t *e = fb->e;
    uint32_t *cnt = fb->cnt, *order = fb->order, *act = fb->act;
    memset(cnt, 0, (W + 1) * sizeof *cnt);
    for (uint32_t i = 0; i < n; i++) cnt[e[i].y0 + 1]++;
    for (int r = 0; r < W; r++) cnt[r + 1] += cnt[r];
    for (uint32_t i = 0; i < n; i++) order[cnt[e[i].y0]++] = i;   /* cnt[r] ends at row r+1's start */
    uint32_t next = 0, na = 0;
    float xs[256];
    for (int y = e[order[0]].y0; y < W && (next < n || na); y++) {
        while (next < n && e[order[next]].y0 == y) act[na++] = order[next++];   /* edges starting here */
        int nx = 0;
        for (uint32_t i = 0; i < na;) {
            edge_t *g = &e[act[i]];
            if (g->y1 <= y) { act[i] = act[--na]; continue; }                   /* finished: drop it */
            if (nx < 256) xs[nx++] = g->x;
            g->x += g->dxdy;
            i++;
        }
        for (int i = 1; i < nx; i++) {
            const float k = xs[i];
            int j = i;
            while (j && xs[j - 1] > k) { xs[j] = xs[j - 1]; j--; }
            xs[j] = k;
        }
        for (int i = 0; i + 1 < nx; i += 2)
            span(ras, y, (int)ceilf(xs[i] - 0.5f), (int)ceilf(xs[i + 1] - 0.5f), v);
        if (!na && next < n) y = e[order[next]].y0 - 1;                          /* a gap: jump ahead */
    }
}

/* ---- lines: Bresenham with a square brush, dashes measured along the line ---------- */

static const uint8_t DASH[ORB_MAP_DASH_N][4] = {   /* on, off, on, off in px; 0 ends */
    { 0 }, { 4, 3 }, { 9, 4 }, { 1, 3 }, { 7, 3, 1, 3 },
};

typedef struct { uint8_t *ras; uint8_t v, w; const uint8_t *pat; float plen, phase; } pen_t;

static int dash_on(const pen_t *p)
{
    if (!p->pat || p->plen <= 0) return 1;
    float t = fmodf(p->phase, p->plen);
    for (int i = 0; i < 4 && p->pat[i]; i++) {
        if (t < p->pat[i]) return !(i & 1);
        t -= p->pat[i];
    }
    return 0;
}

static void stamp(pen_t *p, int x, int y)
{
    if (p->w <= 1) { put(p->ras, x, y, p->v); return; }
    if (p->w == 2) {                     /* the common road width: four writes, no span set-up */
        put(p->ras, x, y, p->v); put(p->ras, x + 1, y, p->v);
        put(p->ras, x, y + 1, p->v); put(p->ras, x + 1, y + 1, p->v);
        return;
    }
    const int lo = -(p->w - 1) / 2, hi = p->w / 2;
    for (int dy = lo; dy <= hi; dy++)
        if ((unsigned)(y + dy) < W) span(p->ras, y + dy, x + lo, x + hi + 1, p->v);
}

/* Liang-Barsky to the glass grown by the brush. */
static int clip_seg(float *x0, float *y0, float *x1, float *y1, float m)
{
    float t0 = 0, t1 = 1;
    const float dx = *x1 - *x0, dy = *y1 - *y0;
    const float P[4] = { -dx, dx, -dy, dy };
    const float Q[4] = { *x0 + m, (float)W - 1 + m - *x0, *y0 + m, (float)W - 1 + m - *y0 };
    for (int i = 0; i < 4; i++) {
        if (P[i] == 0) { if (Q[i] < 0) return 0; continue; }
        const float t = Q[i] / P[i];
        if (P[i] < 0) { if (t > t1) return 0; if (t > t0) t0 = t; }
        else          { if (t < t0) return 0; if (t < t1) t1 = t; }
    }
    const float ax = *x0 + t0 * dx, ay = *y0 + t0 * dy;
    *x1 = *x0 + t1 * dx; *y1 = *y0 + t1 * dy; *x0 = ax; *y0 = ay;
    return 1;
}

/* Undashed: no length, no phase. Most lines take this path. */
static void segment_solid(pen_t *p, float fx0, float fy0, float fx1, float fy1)
{
    if (!clip_seg(&fx0, &fy0, &fx1, &fy1, (float)p->w)) return;
    int x = (int)floorf(fx0 + 0.5f), y = (int)floorf(fy0 + 0.5f);
    const int x1 = (int)floorf(fx1 + 0.5f), y1 = (int)floorf(fy1 + 0.5f);
    const int dx = x1 > x ? x1 - x : x - x1, sx = x < x1 ? 1 : -1;
    const int dy = -(y1 > y ? y1 - y : y - y1), sy = y < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        stamp(p, x, y);
        if (x == x1 && y == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
}

static void segment(pen_t *p, float fx0, float fy0, float fx1, float fy1)
{
    if (!p->pat) { segment_solid(p, fx0, fy0, fx1, fy1); return; }
    const float len = sqrtf((fx1 - fx0) * (fx1 - fx0) + (fy1 - fy0) * (fy1 - fy0));
    float cx0 = fx0, cy0 = fy0, cx1 = fx1, cy1 = fy1;
    if (!clip_seg(&cx0, &cy0, &cx1, &cy1, (float)p->w)) { p->phase += len; return; }
    const float lead = sqrtf((cx0 - fx0) * (cx0 - fx0) + (cy0 - fy0) * (cy0 - fy0));
    p->phase += lead;
    int x = (int)floorf(cx0 + 0.5f), y = (int)floorf(cy0 + 0.5f);
    const int x1 = (int)floorf(cx1 + 0.5f), y1 = (int)floorf(cy1 + 0.5f);
    const int dx = x1 > x ? x1 - x : x - x1, sx = x < x1 ? 1 : -1;
    const int dy = -(y1 > y ? y1 - y : y - y1), sy = y < y1 ? 1 : -1;
    const int steps = dx > -dy ? dx : -dy;
    const float stepl = steps ? sqrtf((cx1 - cx0) * (cx1 - cx0) + (cy1 - cy0) * (cy1 - cy0)) / (float)steps : 0;
    int err = dx + dy;
    for (;;) {
        if (dash_on(p)) stamp(p, x, y);
        if (x == x1 && y == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
        p->phase += stepl;
    }
    p->phase += len - lead - stepl * (float)steps;
}

/* ---- the walk ---------------------------------------------------------------------- */

int orb_map_render(const uint8_t *buf, size_t len, const orb_map_style_t *st,
                   uint8_t *raster, void *scratch, size_t scratch_len, orb_map_stats_t *stats)
{
    orb_map_stats_t s;
    memset(&s, 0, sizeof s);
    if (len < HDR || memcmp(buf, "ORBM", 4) || (buf[4] | buf[5] << 8) != 1) return ORB_MAP_E_FORMAT;
    const uint32_t hl = buf[6] | buf[7] << 8, nf = rd32(buf + 20), nm = rd32(buf + 24), blen = rd32(buf + 28);
    if (hl < HDR || hl > len || len - hl < blen) return ORB_MAP_E_TRUNC;
    rd_t r = { buf + hl, buf + hl + blen, 0 };

    memset(raster, ORB_MAP_LAND | ORB_MAP_LAND << 4, ORB_MAP_RASTER_B);
    const float k = st->scale / (float)ORB_MAP_UNIT, cs = cosf(st->rot_rad), sn = sinf(st->rot_rad);
    const xf_t t = { k * cs, -k * sn, k * sn, k * cs };
    fillbuf_t fb;
    const size_t head = (W + 1) * sizeof(uint32_t);
    fb.cap = scratch_len > head ? (scratch_len - head) / (sizeof(edge_t) + 2 * sizeof(uint32_t)) : 0;
    fb.cnt = (uint32_t *)scratch;
    fb.e = (edge_t *)((uint8_t *)scratch + head);
    fb.order = (uint32_t *)(fb.e + fb.cap);
    fb.act = fb.order + fb.cap;
    edge_t *edges = fb.e;
    const size_t ecap = fb.cap;
    int64_t (*const clk)(void) = st->clock_us;
    int64_t t0 = clk ? clk() : 0;

    for (uint32_t f = 0; f < nf && !r.bad; f++) {
        const uint8_t role = rb(&r), kind = rb(&r);
        const uint32_t nr = rv(&r);
        const int on = role < 16 && (st->role_on >> role & 1) &&
                       (kind == 0 || st->width[role & 15]);
        s.features++;
        if (kind == 0) {                                   /* a fill: every ring, then one scan */
            uint32_t ne = 0;
            int fit = 1;
            for (uint32_t q = 0; q < nr && !r.bad; q++) {
                const uint32_t np = rv(&r);
                int32_t x = 0, y = 0;
                float fx0 = 0, fy0 = 0, px = 0, py = 0;
                for (uint32_t i = 0; i < np && !r.bad; i++) {
                    x += rz(&r); y += rz(&r);
                    if (!on) continue;
                    float ox, oy;
                    tx(&t, x, y, &ox, &oy);
                    if (i == 0) { fx0 = ox; fy0 = oy; }
                    else if (fit) fit = add_edge(edges, ecap, &ne, px, py, ox, oy);
                    px = ox; py = oy;
                }
                if (on && np > 2 && fit) fit = add_edge(edges, ecap, &ne, px, py, fx0, fy0);
            }
            if (!on) continue;
            if (!fit) { s.skipped++; continue; }
            fill(raster, &fb, ne, role);
            s.fills++; s.edges += ne;
            if (ne > s.max_fill_edges) s.max_fill_edges = ne;
            if (clk) { const int64_t t1 = clk(); s.fill_us += (uint32_t)(t1 - t0); t0 = t1; }
        } else {                                           /* a line: each ring a polyline */
            pen_t p = { raster, role, st->width[role & 15], NULL, 0, 0 };
            const uint8_t d = st->dash[role & 15] < ORB_MAP_DASH_N ? st->dash[role & 15] : 0;
            if (d) {
                p.pat = DASH[d];
                for (int i = 0; i < 4; i++) p.plen += DASH[d][i];
            }
            for (uint32_t q = 0; q < nr && !r.bad; q++) {
                const uint32_t np = rv(&r);
                int32_t x = 0, y = 0;
                float px = 0, py = 0;
                p.phase = 0;
                for (uint32_t i = 0; i < np && !r.bad; i++) {
                    x += rz(&r); y += rz(&r);
                    if (!on) continue;
                    float ox, oy;
                    tx(&t, x, y, &ox, &oy);
                    if (i) { segment(&p, px, py, ox, oy); s.segments++; }
                    px = ox; py = oy;
                }
            }
            if (on) s.lines++;
            if (clk) { const int64_t t1 = clk(); s.line_us += (uint32_t)(t1 - t0); t0 = t1; }
        }
    }
    for (uint32_t m = 0; m < nm && !r.bad; m++) {
        const uint8_t kind = rb(&r);
        const uint8_t rank = rb(&r);
        const int32_t x = rz(&r), y = rz(&r);
        const uint8_t n = rb(&r);
        if ((size_t)(r.end - r.p) < n) { r.bad = 1; break; }
        const char *name = (const char *)r.p;
        r.p += n;
        float ox, oy;
        tx(&t, x, y, &ox, &oy);
        if (st->mark) st->mark(st->mark_ctx, kind, rank, ox, oy, name, n);
        if (kind > 7 || !(st->mark_on >> kind & 1)) continue;
        const uint8_t role = kind == ORB_MAP_MARK_AIRPORT ? ORB_MAP_AIRPORT : ORB_MAP_CITY;
        if (!(st->role_on >> role & 1)) continue;
        const int cx = (int)floorf(ox + 0.5f), cy = (int)floorf(oy + 0.5f);
        if (role == ORB_MAP_AIRPORT) {
            for (int dy = -2; dy <= 2; dy++) if ((unsigned)(cy + dy) < W) span(raster, cy + dy, cx - 2, cx + 3, role);
        } else {
            static const int8_t HALFW[5] = { 1, 2, 2, 2, 1 };
            for (int dy = -2; dy <= 2; dy++)
                if ((unsigned)(cy + dy) < W) span(raster, cy + dy, cx - HALFW[dy + 2], cx + HALFW[dy + 2] + 1, role);
        }
        s.marks++;
    }
    if (stats) *stats = s;
    return r.bad ? ORB_MAP_E_TRUNC : ORB_MAP_OK;
}
