/* The map's vector renderer — contract in orb_map_raster.h. */
#include "orb_map_raster.h"

#include <math.h>
#include <string.h>

#define W   ORB_MAP_W
#define HDR 36

/* ---- the file -------------------------------------------------------------------- */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* zlib's CRC-32 a nibble at a time: a 64 B table in flash rather than 1 KB in RAM. */
static uint32_t crc32_more(uint32_t c, const uint8_t *p, size_t n)   /* c: ~crc so far */
{
    static const uint32_t T[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    while (n--) {
        c ^= *p++;
        c = (c >> 4) ^ T[c & 15];
        c = (c >> 4) ^ T[c & 15];
    }
    return c;
}
static uint32_t crc32_z(const uint8_t *p, size_t n) { return ~crc32_more(0xFFFFFFFFu, p, n); }

int orb_map_check(const uint8_t *buf, size_t len, int *zoom)
{
    if (len < HDR || memcmp(buf, "ORBM", 4) || (buf[4] | buf[5] << 8) != ORB_MAP_FORMAT) return ORB_MAP_E_FORMAT;
    const uint32_t hl = buf[6] | buf[7] << 8, blen = rd32(buf + 28);
    if (hl < HDR || hl > len || len - hl < blen) return ORB_MAP_E_TRUNC;
    if (crc32_z(buf + hl, blen) != rd32(buf + 32)) return ORB_MAP_E_CRC;
    if (zoom) *zoom = buf[16];
    return ORB_MAP_OK;
}

/* The body's reader: a buffer, or a window refilled from a source (orb_map_render_src),
 * the CRC taken over each byte as it comes. */
typedef struct {
    const orb_map_src_t *src;
    uint8_t *win;
    size_t win_len;
    uint32_t off, end, crc;       /* the next file offset to read, the body's end */
} stream_t;
typedef struct { const uint8_t *p, *end; int bad; stream_t *s; } rd_t;

/* Keep what is left, read on behind it. 0 when the body is spent or the read failed. */
static int refill(rd_t *r)
{
    stream_t *s = r->s;
    if (!s || s->off >= s->end) return 0;
    const size_t keep = (size_t)(r->end - r->p);
    if (keep) memmove(s->win, r->p, keep);
    uint32_t want = (uint32_t)(s->win_len - keep);
    if (want > s->end - s->off) want = s->end - s->off;
    if (!want || s->src->read(s->src->ctx, s->off, s->win + keep, want) != (int)want) return 0;
    s->crc = crc32_more(s->crc, s->win + keep, want);
    s->off += want;
    r->p = s->win;
    r->end = s->win + keep + want;
    return 1;
}

static uint32_t rv(rd_t *r)
{
    uint32_t v = 0;
    for (int s = 0; s < 35; s += 7) {
        if (r->p >= r->end && !refill(r)) { r->bad = 1; return 0; }
        const uint8_t b = *r->p++;
        v |= (uint32_t)(b & 0x7F) << s;
        if (b < 0x80) return v;
    }
    r->bad = 1;
    return 0;
}
static int32_t rz(rd_t *r) { const uint32_t v = rv(r); return (int32_t)(v >> 1) ^ -(int32_t)(v & 1); }
static uint8_t rb(rd_t *r) { if (r->p >= r->end && !refill(r)) { r->bad = 1; return 0; } return *r->p++; }

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

static int render_body(rd_t r, uint32_t nf, uint32_t nm, const orb_map_style_t *st,
                       uint8_t *raster, void *scratch, size_t scratch_len, orb_map_stats_t *stats);

int orb_map_render(const uint8_t *buf, size_t len, const orb_map_style_t *st,
                   uint8_t *raster, void *scratch, size_t scratch_len, orb_map_stats_t *stats)
{
    if (len < HDR || memcmp(buf, "ORBM", 4) || (buf[4] | buf[5] << 8) != ORB_MAP_FORMAT) return ORB_MAP_E_FORMAT;
    const uint32_t hl = buf[6] | buf[7] << 8, nf = rd32(buf + 20), nm = rd32(buf + 24), blen = rd32(buf + 28);
    if (hl < HDR || hl > len || len - hl < blen) return ORB_MAP_E_TRUNC;
    const rd_t r = { buf + hl, buf + hl + blen, 0, NULL };
    return render_body(r, nf, nm, st, raster, scratch, scratch_len, stats);
}

int orb_map_render_src(const orb_map_src_t *src, uint8_t *hdr, uint8_t *win, size_t win_len,
                       const orb_map_style_t *st, uint8_t *raster, void *scratch, size_t scratch_len,
                       orb_map_stats_t *stats)
{
    if (!src || src->len < HDR || win_len < 512 || src->read(src->ctx, 0, hdr, HDR) != HDR) return ORB_MAP_E_TRUNC;
    if (memcmp(hdr, "ORBM", 4) || (hdr[4] | hdr[5] << 8) != ORB_MAP_FORMAT) return ORB_MAP_E_FORMAT;
    const uint32_t hl = hdr[6] | hdr[7] << 8, nf = rd32(hdr + 20), nm = rd32(hdr + 24), blen = rd32(hdr + 28);
    if (hl < HDR || hl > src->len || src->len - hl < blen) return ORB_MAP_E_TRUNC;
    stream_t s = { src, win, win_len, hl, hl + blen, 0xFFFFFFFFu };
    rd_t r = { win, win, 0, &s };
    const int rc = render_body(r, nf, nm, st, raster, scratch, scratch_len, stats);
    if (rc != ORB_MAP_OK) return rc;
    /* the CRC is over the whole body: what the records did not read is read for it */
    rd_t tail = { win, win, 0, &s };
    while (refill(&tail)) tail.p = tail.end;
    if (s.off != s.end) return ORB_MAP_E_TRUNC;
    return ~s.crc == rd32(hdr + 32) ? ORB_MAP_OK : ORB_MAP_E_CRC;
}

/* ---- marks with a look (OS v1.2.38): ORB_MAP_LOOK(shape, r), centred on the landmark --- */
static void mark_look(uint8_t *ras, int cx, int cy, uint8_t look, uint8_t v)
{
    const int shape = look & 7, r = (look >> 3) < 2 ? 2 : look >> 3, th = r >= 5 ? 2 : 1;
    for (int dy = -r - 3; dy <= r + 3; dy++) {
        const int y = cy + dy, ady = dy < 0 ? -dy : dy;
        if ((unsigned)y >= W) continue;
        switch (shape) {
        case ORB_MAP_SHAPE_DOT: case ORB_MAP_SHAPE_RING: case ORB_MAP_SHAPE_TICKED: {
            if (ady <= r) {
                const int o = (int)sqrtf((float)(r * r - dy * dy) + 0.5f);
                const int ri = r - th, i = ady <= ri ? (int)sqrtf((float)(ri * ri - dy * dy) + 0.5f) : -1;
                if (shape == ORB_MAP_SHAPE_DOT || i < 0) span(ras, y, cx - o, cx + o + 1, v);
                else { span(ras, y, cx - o, cx - i, v); span(ras, y, cx + i + 1, cx + o + 1, v); }
            }
            if (shape == ORB_MAP_SHAPE_TICKED) {             /* four ticks out of the ring */
                if (ady > r && ady <= r + 3) span(ras, y, cx - (th >> 1), cx - (th >> 1) + th, v);
                if (dy >= -(th >> 1) && dy < th - (th >> 1)) { span(ras, y, cx - r - 3, cx - r, v); span(ras, y, cx + r + 1, cx + r + 4, v); }
            }
            break;
        }
        case ORB_MAP_SHAPE_DIAMOND: case ORB_MAP_SHAPE_DIAMOND_LINE:
            if (ady <= r) {
                const int o = r - ady, i = o - th - 1;
                if (shape == ORB_MAP_SHAPE_DIAMOND || ady > r - th || i < 0) span(ras, y, cx - o, cx + o + 1, v);
                else { span(ras, y, cx - o, cx - i, v); span(ras, y, cx + i + 1, cx + o + 1, v); }
            }
            break;
        case ORB_MAP_SHAPE_SQUARE: case ORB_MAP_SHAPE_SQUARE_LINE:
            if (ady <= r) {
                if (shape == ORB_MAP_SHAPE_SQUARE || ady > r - th) span(ras, y, cx - r, cx + r + 1, v);
                else { span(ras, y, cx - r, cx - r + th, v); span(ras, y, cx + r + 1 - th, cx + r + 1, v); }
            }
            break;
        default: break;
        }
    }
}

static int render_body(rd_t r, uint32_t nf, uint32_t nm, const orb_map_style_t *st,
                       uint8_t *raster, void *scratch, size_t scratch_len, orb_map_stats_t *stats)
{
    orb_map_stats_t s;
    memset(&s, 0, sizeof s);

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
                       (kind == 0 || (st->width[role & 15] & 7));
        s.features++;
        if (kind == 0) {                                   /* a fill: every ring, then one scan */
            uint32_t ne = 0;
            int fit = 1;
            float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
            for (uint32_t q = 0; q < nr && !r.bad; q++) {
                const uint32_t np = rv(&r);
                int32_t x = 0, y = 0;
                float fx0 = 0, fy0 = 0, px = 0, py = 0;
                for (uint32_t i = 0; i < np && !r.bad; i++) {
                    x += rz(&r); y += rz(&r);
                    if (!on) continue;
                    float ox, oy;
                    tx(&t, x, y, &ox, &oy);
                    if (ox < bx0) bx0 = ox;
                    if (ox > bx1) bx1 = ox;
                    if (oy < by0) by0 = oy;
                    if (oy > by1) by1 = oy;
                    if (i == 0) { fx0 = ox; fy0 = oy; }
                    else if (fit) fit = add_edge(edges, ecap, &ne, px, py, ox, oy);
                    px = ox; py = oy;
                }
                if (on && np > 2 && fit) fit = add_edge(edges, ecap, &ne, px, py, fx0, fy0);
            }
            if (!on) continue;
            const float floor_px = 2.0f * st->dash[role & 15];
            if (floor_px > 0 && bx1 - bx0 < floor_px && by1 - by0 < floor_px) { s.small++; continue; }
            if (!fit) { s.skipped++; continue; }
            fill(raster, &fb, ne, role);
            s.fills++; s.edges += ne;
            if (ne > s.max_fill_edges) s.max_fill_edges = ne;
            if (clk) { const int64_t t1 = clk(); s.fill_us += (uint32_t)(t1 - t0); t0 = t1; }
        } else {                                           /* a line: each ring a polyline */
            pen_t p = { raster, role, (uint8_t)(st->width[role & 15] & 7), NULL, 0, 0 };
            const uint8_t d = st->dash[role & 15] < ORB_MAP_DASH_N ? st->dash[role & 15] : 0;
            if (d) {
                p.pat = DASH[d];
                for (int i = 0; i < 4; i++) p.plen += DASH[d][i];
            }
            /* v1.2.38: the line's floor (width's high bits, 8 px a step): a piece whose box is
             * under it both ways is not drawn (creeks the tiles call rivers). Its points wait
             * in the scratch until its box is known; one longer than the scratch is drawn. */
            const float lfloor = 8.0f * (st->width[role & 15] >> 3);
            float *pts = (float *)scratch;
            const uint32_t pcap = lfloor > 0 ? (uint32_t)(scratch_len / (2 * sizeof(float))) : 0;
            for (uint32_t q = 0; q < nr && !r.bad; q++) {
                const uint32_t np = rv(&r);
                int32_t x = 0, y = 0;
                float px = 0, py = 0, bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
                uint32_t held = 0;
                p.phase = 0;
                for (uint32_t i = 0; i < np && !r.bad; i++) {
                    x += rz(&r); y += rz(&r);
                    if (!on) continue;
                    float ox, oy;
                    tx(&t, x, y, &ox, &oy);
                    if (held < pcap && held == i) {          /* still holding: no segment yet */
                        pts[2 * held] = ox; pts[2 * held + 1] = oy; held++;
                        if (ox < bx0) bx0 = ox;
                        if (ox > bx1) bx1 = ox;
                        if (oy < by0) by0 = oy;
                        if (oy > by1) by1 = oy;
                        continue;
                    }
                    if (held) {                              /* the scratch ran out: draw what it held */
                        for (uint32_t k = 1; k < held; k++) { segment(&p, pts[2 * k - 2], pts[2 * k - 1], pts[2 * k], pts[2 * k + 1]); s.segments++; }
                        px = pts[2 * held - 2]; py = pts[2 * held - 1];
                        held = 0;
                    }
                    if (i) { segment(&p, px, py, ox, oy); s.segments++; }
                    px = ox; py = oy;
                }
                if (held) {
                    if (bx1 - bx0 < lfloor && by1 - by0 < lfloor) { s.small++; continue; }
                    for (uint32_t k = 1; k < held; k++) { segment(&p, pts[2 * k - 2], pts[2 * k - 1], pts[2 * k], pts[2 * k + 1]); s.segments++; }
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
        while ((size_t)(r.end - r.p) < n && refill(&r)) {}
        if ((size_t)(r.end - r.p) < n) { r.bad = 1; break; }
        const char *name = (const char *)r.p;
        r.p += n;
        float ox, oy;
        tx(&t, x, y, &ox, &oy);
        if (st->mark) st->mark(st->mark_ctx, kind, rank, ox, oy, name, n);
        if (kind > 7 || !(st->mark_on >> kind & 1)) continue;
        const uint8_t role = kind == ORB_MAP_MARK_AIRPORT ? ORB_MAP_AIRPORT : ORB_MAP_CITY;
        const int cx = (int)floorf(ox + 0.5f), cy = (int)floorf(oy + 0.5f);
        if (st->role_on >> role & 1) {
            if (st->dash[role] && rank > st->dash[role]) continue;
        } else {
            /* ORB_MAP_MARK_BARE (v1.6.9): an airport the AIRPORT role is off but the RUNWAY role
             * is on keeps its symbol unless a runway lies near: no runways drawn means no runways
             * in the map data, and an airport is never drawn as nothing. */
            if (!(kind == ORB_MAP_MARK_AIRPORT && (st->mark_on & ORB_MAP_MARK_BARE))) continue;
            if (!(st->role_on >> ORB_MAP_RUNWAY & 1)) continue;
            if (st->dash[ORB_MAP_AIRPORT] && rank > st->dash[ORB_MAP_AIRPORT]) continue;
            const int x0 = cx - ORB_MAP_BARE_R < 0 ? 0 : cx - ORB_MAP_BARE_R,
                      x1 = cx + ORB_MAP_BARE_R >= W ? W - 1 : cx + ORB_MAP_BARE_R;
            const int y0 = cy - ORB_MAP_BARE_R < 0 ? 0 : cy - ORB_MAP_BARE_R,
                      y1 = cy + ORB_MAP_BARE_R >= W ? W - 1 : cy + ORB_MAP_BARE_R;
            int near = 0;
            for (int yy = y0; yy <= y1 && !near; yy++)
                for (int xx = x0; xx <= x1 && !near; xx++)
                    if (orb_map_role_at(raster, xx, yy) == ORB_MAP_RUNWAY) near = 1;
            if (near) continue;
        }
        if (st->width[role]) mark_look(raster, cx, cy, st->width[role], role);
        else if (role == ORB_MAP_AIRPORT) {
            for (int dy = -2; dy <= 2; dy++) if ((unsigned)(cy + dy) < W) span(raster, cy + dy, cx - 2, cx + 3, role);
        } else {
            static const int8_t HALFW[5] = { 1, 2, 2, 2, 1 };
            for (int dy = -2; dy <= 2; dy++)
                if ((unsigned)(cy + dy) < W) span(raster, cy + dy, cx - HALFW[dy + 2], cx + HALFW[dy + 2] + 1, role);
        }
        s.marks++;
    }
    if (st->clip) {                                        /* past the clip, the ground */
        const int rr = 2 * st->clip, c = W / 2;
        for (int y = 0; y < W; y++) {
            const int dy = y - c;
            if (dy * dy >= rr * rr) { span(raster, y, 0, W, ORB_MAP_LAND); continue; }
            const int hw = (int)sqrtf((float)(rr * rr - dy * dy));
            span(raster, y, 0, c - hw, ORB_MAP_LAND);
            span(raster, y, c + hw + 1, W, ORB_MAP_LAND);
        }
    }
    if (stats) *stats = s;
    return r.bad ? ORB_MAP_E_TRUNC : ORB_MAP_OK;
}
