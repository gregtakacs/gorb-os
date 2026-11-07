/* An indexed-PNG reader — contract in orb_png8.h. */
#include "orb_png8.h"

#include <string.h>

#include "miniz.h"   /* the ROM's tinfl on the S3; a zlib stand-in on the host */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* zlib's CRC-32 a nibble at a time: a 64 B table in flash rather than 1 KB in RAM. */
static uint32_t crc_update(uint32_t c, const uint8_t *p, size_t n)
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

static int chunk_ok(const uint8_t *chunk, uint32_t dlen)   /* chunk -> its type; CRC over type+data */
{
    const uint32_t c = ~crc_update(0xFFFFFFFFu, chunk, 4 + (size_t)dlen);
    return c == be32(chunk + 4 + dlen);
}

static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

typedef struct { int w, h, depth, rowbytes; } hdr_t;

static int read_ihdr(const uint8_t *png, size_t len, hdr_t *h)
{
    if (len < 8 + 25 || memcmp(png, SIG, 8)) return ORB_PNG8_E_SIG;
    const uint8_t *c = png + 8;
    if (be32(c) != 13 || memcmp(c + 4, "IHDR", 4)) return ORB_PNG8_E_SIG;
    if (!chunk_ok(c + 4, 13)) return ORB_PNG8_E_CRC;
    const uint8_t *d = c + 8;
    const uint32_t w = be32(d), hh = be32(d + 4);
    const int depth = d[8], type = d[9], comp = d[10], filt = d[11], lace = d[12];
    if (type != 3 || comp || filt || lace) return ORB_PNG8_E_FORMAT;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8) return ORB_PNG8_E_FORMAT;
    if (!w || !hh || w > ORB_PNG8_MAX_W || hh > 8192) return ORB_PNG8_E_FORMAT;
    h->w = (int)w;
    h->h = (int)hh;
    h->depth = depth;
    h->rowbytes = (int)((w * (uint32_t)depth + 7) / 8);
    return ORB_PNG8_OK;
}

int orb_png8_info(const uint8_t *png, size_t len, int *w, int *h)
{
    hdr_t hd;
    const int e = read_ihdr(png, len, &hd);
    if (e) return e;
    if (w) *w = hd.w;
    if (h) *h = hd.h;
    return ORB_PNG8_OK;
}

#define WIN TINFL_LZ_DICT_SIZE

typedef struct {
    tinfl_decompressor inf;
    uint8_t  win[WIN];
    uint8_t  rows[2][1 + ORB_PNG8_MAX_W];   /* filter byte + the row, 8-bit at most */
    uint8_t  idx[ORB_PNG8_MAX_W];
} scratch_t;

size_t orb_png8_scratch_size(void) { return sizeof(scratch_t); }

static int paeth(int a, int b, int c)
{
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

/* Undo one row's filter in place (bpp is 1: an indexed pixel is at most a byte). */
static int unfilter(uint8_t *cur, const uint8_t *prev, int n)
{
    const int f = cur[0];
    uint8_t *x = cur + 1;
    const uint8_t *u = prev + 1;
    switch (f) {
    case 0: break;
    case 1: for (int i = 1; i < n; i++) x[i] = (uint8_t)(x[i] + x[i - 1]); break;
    case 2: for (int i = 0; i < n; i++) x[i] = (uint8_t)(x[i] + u[i]); break;
    case 3:
        for (int i = 0; i < n; i++) x[i] = (uint8_t)(x[i] + (((i ? x[i - 1] : 0) + u[i]) >> 1));
        break;
    case 4:
        for (int i = 0; i < n; i++) x[i] = (uint8_t)(x[i] + paeth(i ? x[i - 1] : 0, u[i], i ? u[i - 1] : 0));
        break;
    default: return ORB_PNG8_E_FILTER;
    }
    return ORB_PNG8_OK;
}

static void unpack(const uint8_t *src, uint8_t *dst, int w, int depth)
{
    if (depth == 8) { memcpy(dst, src, (size_t)w); return; }
    const int per = 8 / depth, mask = (1 << depth) - 1;
    for (int x = 0; x < w; x++) {
        const int sh = 8 - depth * (x % per + 1);
        dst[x] = (uint8_t)(src[x / per] >> sh & mask);
    }
}

int orb_png8_decode(const uint8_t *png, size_t len, void *scratch, size_t scratch_len,
                    orb_png8_row_fn row, void *ctx)
{
    hdr_t hd;
    int e = read_ihdr(png, len, &hd);
    if (e) return e;
    if (!scratch || scratch_len < sizeof(scratch_t)) return ORB_PNG8_E_SCRATCH;
    scratch_t *s = (scratch_t *)scratch;
    tinfl_init(&s->inf);
    memset(s->rows[1], 0, (size_t)hd.rowbytes + 1);    /* the row above the first is zeros */

    const int need = hd.rowbytes + 1;
    int y = 0, have = 0, cur = 0;
    size_t wpos = 0;                                    /* the window's write position */
    size_t off = 8;
    int done = 0;
    while (!done) {
        if (off + 12 > len) return ORB_PNG8_E_TRUNC;
        const uint32_t dlen = be32(png + off);
        const uint8_t *type = png + off + 4;
        if (dlen > len - off - 12) return ORB_PNG8_E_TRUNC;
        if (!chunk_ok(type, dlen)) return ORB_PNG8_E_CRC;
        if (!memcmp(type, "IEND", 4)) break;
        if (!memcmp(type, "IDAT", 4)) {
            const uint8_t *in = type + 4;
            size_t left = dlen;
            for (;;) {
                size_t in_n = left, out_n = WIN - wpos;
                const tinfl_status st = tinfl_decompress(&s->inf, in, &in_n, s->win, s->win + wpos, &out_n,
                                                         TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
                in += in_n;
                left -= in_n;
                /* Hand what came out to the rows, a row at a time. */
                for (size_t k = 0; k < out_n && y < hd.h; ) {
                    const size_t take = (size_t)(need - have) < out_n - k ? (size_t)(need - have) : out_n - k;
                    memcpy(s->rows[cur] + have, s->win + wpos + k, take);
                    have += (int)take;
                    k += take;
                    if (have == need) {
                        e = unfilter(s->rows[cur], s->rows[cur ^ 1], hd.rowbytes);
                        if (e) return e;
                        unpack(s->rows[cur] + 1, s->idx, hd.w, hd.depth);
                        if (row && row(ctx, y, s->idx, hd.w)) return ORB_PNG8_E_STOP;
                        y++;
                        have = 0;
                        cur ^= 1;
                    }
                }
                wpos = (wpos + out_n) & (WIN - 1);
                if (st == TINFL_STATUS_DONE) { done = 1; break; }
                if (st < 0) return ORB_PNG8_E_INFLATE;
                if (st == TINFL_STATUS_NEEDS_MORE_INPUT && !left) break;   /* the next IDAT */
                if (st == TINFL_STATUS_HAS_MORE_OUTPUT || left) continue;
                break;
            }
        }
        off += 12 + dlen;
    }
    return y == hd.h ? ORB_PNG8_OK : ORB_PNG8_E_TRUNC;
}
