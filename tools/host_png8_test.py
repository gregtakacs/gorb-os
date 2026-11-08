#!/usr/bin/env python3
"""Host bench for the indexed-PNG reader (main/gfx/orb_png8.c, radar-plan.md §2.3).

The reader is compiled VERBATIM with host gcc -Werror against a miniz.h stand-in whose
tinfl_decompress is zlib's inflate: the S3 runs the ROM's tinfl, whose inflate is not
in question here; the chunk walk, the CRCs, the five row filters, the bit unpacking and
the refusals are. The PNGs are written by this file, so every filter type, every bit
depth, an IDAT split across chunks and a single-byte IDAT are all forced, and each
decode is compared to the indices that went in.

Run:  python3 tools/host_png8_test.py              (rc 0 = pass)
      python3 tools/host_png8_test.py --mutations  (proves the checks bite)
"""
import os, random, struct, subprocess, sys, tempfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "main", "gfx", "orb_png8.c")
HDR = os.path.join(ROOT, "main", "gfx", "orb_png8.h")

MINIZ_SHIM = r"""
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>
typedef unsigned char mz_uint8;
typedef unsigned int mz_uint32;
#define TINFL_LZ_DICT_SIZE 32768
enum { TINFL_FLAG_PARSE_ZLIB_HEADER = 1, TINFL_FLAG_HAS_MORE_INPUT = 2,
       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 4, TINFL_FLAG_COMPUTE_ADLER32 = 8 };
typedef enum { TINFL_STATUS_BAD_PARAM = -3, TINFL_STATUS_ADLER32_MISMATCH = -2, TINFL_STATUS_FAILED = -1,
               TINFL_STATUS_DONE = 0, TINFL_STATUS_NEEDS_MORE_INPUT = 1, TINFL_STATUS_HAS_MORE_OUTPUT = 2 } tinfl_status;
typedef struct { int m_state; z_stream z; size_t total; } tinfl_decompressor;
#define tinfl_init(r) do { (r)->m_state = 0; } while (0)
static inline tinfl_status tinfl_decompress(tinfl_decompressor *r, const mz_uint8 *in, size_t *in_n,
                                            mz_uint8 *out_start, mz_uint8 *out_next, size_t *out_n, mz_uint32 flags)
{
    if (!r->m_state) {
        memset(&r->z, 0, sizeof r->z);
        r->total = 0;
        if (inflateInit2(&r->z, (flags & TINFL_FLAG_PARSE_ZLIB_HEADER) ? 15 : -15) != Z_OK) return TINFL_STATUS_FAILED;
        r->m_state = 1;
    }
    /* The ROM's tinfl keeps its history in the caller's 32 KB window: output must go to
     * the next byte of it, wrapping, and never past its end. zlib does not care, so check. */
    if ((size_t)(out_next - out_start) != r->total % TINFL_LZ_DICT_SIZE ||
        (size_t)(out_next - out_start) + *out_n > TINFL_LZ_DICT_SIZE) return TINFL_STATUS_BAD_PARAM;
    r->z.next_in = (Bytef *)in; r->z.avail_in = (uInt)*in_n;
    r->z.next_out = out_next; r->z.avail_out = (uInt)*out_n;
    const int z = inflate(&r->z, Z_NO_FLUSH);
    *in_n -= r->z.avail_in; *out_n -= r->z.avail_out;
    r->total += *out_n;
    if (z == Z_STREAM_END) { inflateEnd(&r->z); return TINFL_STATUS_DONE; }
    if (z != Z_OK && z != Z_BUF_ERROR) { inflateEnd(&r->z); return TINFL_STATUS_FAILED; }
    if (r->z.avail_out == 0) return TINFL_STATUS_HAS_MORE_OUTPUT;
    return (flags & TINFL_FLAG_HAS_MORE_INPUT) ? TINFL_STATUS_NEEDS_MORE_INPUT : TINFL_STATUS_FAILED;
}
"""

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
static unsigned char *g_out; static int g_w, g_rows;
static int row(void *ctx, int y, const uint8_t *idx, int w) { (void)ctx; memcpy(g_out + (size_t)y * w, idx, (size_t)w); g_w = w; g_rows++; return 0; }
static int stop_at_3(void *ctx, int y, const uint8_t *idx, int w) { (void)ctx; (void)idx; (void)w; return y == 3; }
int main(int argc, char **argv)
{
    /* argv: file, mode ("decode" | "stop" | "small" | "plte") */
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n); if (fread(buf, 1, (size_t)n, f) != (size_t)n) return 9; fclose(f);
    size_t ss = orb_png8_scratch_size(); void *s = malloc(ss);
    memset(s, 0xA5, ss);   /* never zeroed for it: the reader must clear what it relies on */
    int w = 0, h = 0, rc;
    g_out = calloc(8192 * 1024, 1);
    if (argc > 2 && !strcmp(argv[2], "plte")) {
        uint8_t v[256];
        memset(v, 0xA5, sizeof v);       /* the reader clears what the palette leaves */
        rc = orb_png8_plte(buf, (size_t)n, v);
        printf("%d 0 0 0\n", rc);
        fwrite(v, 1, 256, stdout);
        return 0;
    }
    if (argc > 2 && !strcmp(argv[2], "stop")) rc = orb_png8_decode(buf, (size_t)n, s, ss, stop_at_3, NULL);
    else if (argc > 2 && !strcmp(argv[2], "small")) rc = orb_png8_decode(buf, (size_t)n, s, ss - 1, row, NULL);
    else rc = orb_png8_decode(buf, (size_t)n, s, ss, row, NULL);
    orb_png8_info(buf, (size_t)n, &w, &h);
    printf("%d %d %d %d\n", rc, w, h, g_rows);
    if (rc == 0) fwrite(g_out, 1, (size_t)w * h, stdout);
    return 0;
}
"""


def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)


def pack_row(idx, depth):
    if depth == 8:
        return bytes(idx)
    per = 8 // depth; out = bytearray((len(idx) * depth + 7) // 8)
    for x, v in enumerate(idx):
        out[x // per] |= v << (8 - depth * (x % per + 1))
    return bytes(out)


def paeth(a, b, c):
    p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)


def filt(raw, prev, f):
    out = bytearray(len(raw))
    for i in range(len(raw)):
        a = raw[i - 1] if i else 0; b = prev[i]; c = prev[i - 1] if i else 0
        pred = (0, a, b, (a + b) >> 1, paeth(a, b, c))[f]
        out[i] = (raw[i] - pred) & 0xFF
    return bytes(out)


def png(rows, w, depth, filters=None, split=None, ctype=3, lace=0, palette=True, plte=None):
    raw = b""; prev = bytes((w * depth + 7) // 8)
    for y, r in enumerate(rows):
        pr = pack_row(r, depth); f = (filters or [0])[y % len(filters or [0])]
        raw += bytes([f]) + filt(pr, prev, f); prev = pr
    z = zlib.compress(raw, 9)
    ihdr = struct.pack(">IIBBBBB", w, len(rows), depth, ctype, 0, 0, lace)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
    if palette:
        out += chunk(b"PLTE", plte if plte is not None else bytes(range(48)))
    parts = [z] if not split else [z[i:i + split] for i in range(0, len(z), split)]
    for p in parts:
        out += chunk(b"IDAT", p)
    return out + chunk(b"IEND", b"")


def build(src, d):
    open(os.path.join(d, "miniz.h"), "w").write(MINIZ_SHIM)
    c = os.path.join(d, "t.c")
    open(c, "w").write(src + "\n" + HARNESS)
    exe = os.path.join(d, "t")
    r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", d, "-I", os.path.dirname(HDR),
                        "-o", exe, c, "-lz"], capture_output=True, text=True)
    return exe if r.returncode == 0 else (None, r.stderr)


def decode(exe, d, data, mode="decode"):
    p = os.path.join(d, "in.png"); open(p, "wb").write(data)
    r = subprocess.run([exe, p, mode], capture_output=True)
    head, _, body = r.stdout.partition(b"\n")
    rc, w, h, rows = map(int, head.split())
    return rc, w, h, rows, body


def suite(src, quiet=False):
    fails = []
    def ck(ok, what):
        if not quiet: print(("ok    " if ok else "FAIL  ") + what)
        if not ok: fails.append(what)
    rng = random.Random(7)
    with tempfile.TemporaryDirectory() as d:
        exe = build(src, d)
        if isinstance(exe, tuple):
            if not quiet: print(exe[1])
            return ["compile"]
        for depth in (1, 2, 4, 8):
            for w in (1, 7, 13, 466):
                h = 9
                rows = [[rng.randrange(1 << depth) for _ in range(w)] for _ in range(h)]
                data = png(rows, w, depth, filters=[0, 1, 2, 3, 4])
                rc, gw, gh, nr, body = decode(exe, d, data)
                ck(rc == 0 and (gw, gh, nr) == (w, h, h) and body == bytes(sum(rows, [])),
                   "depth %d, width %d: every filter type, every index back" % (depth, w))
        rows = [[rng.randrange(256) for _ in range(512)] for _ in range(512)]
        data = png(rows, 512, 8, filters=[4, 1, 3, 2, 0], split=997)
        rc, gw, gh, nr, body = decode(exe, d, data)
        ck(rc == 0 and body == bytes(sum(rows, [])), "a radar-sized 512x512 frame, its IDAT in 997-byte chunks, past the 32 KB window")
        data = png(rows[:20], 512, 8, filters=[1], split=1)
        rc, *_, body = decode(exe, d, data)
        ck(rc == 0 and body == bytes(sum(rows[:20], [])), "an IDAT a byte at a time")
        flat = [[0] * 466 for _ in range(466)]
        data = png(flat, 466, 8, filters=[2])
        rc, *_, body = decode(exe, d, data)
        ck(rc == 0 and body == bytes(466 * 466), "an empty sky (every index 0)")
        good = png([[1, 2, 3]] * 4, 3, 8)
        ck(decode(exe, d, b"GIF89a" + good[6:])[0] == -1, "not a PNG is refused")
        ck(decode(exe, d, png([[1, 2, 3]] * 4, 3, 8, ctype=6, palette=False))[0] == -2, "RGBA is refused")
        ck(decode(exe, d, png([[1, 2, 3]] * 4, 3, 8, lace=1))[0] == -2, "interlaced is refused")
        ck(decode(exe, d, png([[0] * 1025], 1025, 8))[0] == -2, "wider than 1024 is refused")
        bad = bytearray(good); i = bad.index(b"IDAT") + 6; bad[i] ^= 0x55
        ck(decode(exe, d, bytes(bad))[0] == -3, "a flipped IDAT byte fails its CRC")
        bad = bytearray(good); bad[20] ^= 1
        ck(decode(exe, d, bytes(bad))[0] == -3, "a flipped IHDR byte fails its CRC")
        ck(decode(exe, d, good[:good.index(b"IEND") - 4 - 6])[0] == -4, "a file cut inside its IDAT is truncated")
        cut = good[:good.index(b"IEND") - 4] + chunk(b"IEND", b"")
        short = png([[1, 2, 3]] * 2, 3, 8)
        hdr4 = struct.pack(">IIBBBBB", 3, 4, 8, 3, 0, 0, 0)
        lie = short.replace(chunk(b"IHDR", struct.pack(">IIBBBBB", 3, 2, 8, 3, 0, 0, 0)), chunk(b"IHDR", hdr4))
        ck(decode(exe, d, lie)[0] == -4, "an image with fewer rows than its header says is truncated")
        raw = bytes([5, 1, 2, 3]) * 2
        badf = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 3, 2, 8, 3, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
        ck(decode(exe, d, badf)[0] == -6, "filter type 5 is refused")
        ck(decode(exe, d, good, "stop")[0] == -8, "the row callback can stop the decode")
        ck(decode(exe, d, good, "small")[0] == -7, "scratch one byte short is refused")
        ck(cut == good, "the fixtures are what they claim")

        # orb_png8_plte: LibreWXR numbers its greys afresh every frame (Seattle: index 1 is
        # grey 89, New York's index 1 was 84), so the draw colours the palette's value
        grey = [0, 89, 95, 101, 255]
        pl = b"".join(bytes([g, 255 - g, 7]) for g in grey)     # not grey: the red channel is read
        rc, *_, body = decode(exe, d, png([[1, 2, 3]] * 2, 3, 8, plte=pl), "plte")
        ck(rc == 5 and list(body[:5]) == grey and body[5:] == bytes(251),
           "the palette reads as its entries' first channel, and indices past it as 0")
        rc, *_, body = decode(exe, d, png([[1, 2, 3]] * 2, 3, 8, palette=False), "plte")
        ck(rc == 0 and body == bytes(256), "no palette: 0 entries, the caller's indices stand")
        full = bytes(sum(([i, 0, 0] for i in range(256)), []))
        rc, *_, body = decode(exe, d, png([[1]], 1, 8, plte=full), "plte")
        ck(rc == 256 and body == bytes(range(256)), "a full 256-entry palette")
        bad = bytearray(png([[1]], 1, 8, plte=pl)); i = bad.index(b"PLTE") + 5; bad[i] ^= 1
        ck(decode(exe, d, bytes(bad), "plte")[0] == -3, "a flipped palette byte fails its CRC")
        ck(decode(exe, d, png([[1]], 1, 8, plte=pl + b"\x01"), "plte")[0] == -2,
           "a palette that is not whole entries is refused")
        ck(decode(exe, d, png([[1]], 1, 8, plte=full + b"\x00\x00\x00"), "plte")[0] == -2,
           "a palette of 257 entries is refused")
    return fails


MUTATIONS = [
    ("case 1: for (int i = 1; i < n; i++)", "case 1: for (int i = 2; i < n; i++)"),
    ("(((i ? x[i - 1] : 0) + u[i]) >> 1)", "((i ? x[i - 1] : 0) + u[i])"),
    ("(pb <= pc ? b : c)", "(pb < pc ? b : c)"),
    ("const int sh = 8 - depth * (x % per + 1);", "const int sh = depth * (x % per);"),
    ("if (!chunk_ok(type, dlen)) return ORB_PNG8_E_CRC;\n        if (!memcmp(type, \"IEND\"", "if (!memcmp(type, \"IEND\""),
    ("if (type != 3 || comp || filt || lace) return ORB_PNG8_E_FORMAT;", "if (type != 3 || comp || filt) return ORB_PNG8_E_FORMAT;"),
    ("return y == hd.h ? ORB_PNG8_OK : ORB_PNG8_E_TRUNC;", "return ORB_PNG8_OK;"),
    ("wpos = (wpos + out_n) & (WIN - 1);", "wpos = 0;"),
    ("memset(s->rows[1], 0, (size_t)hd.rowbytes + 1);", ";"),
    ("default: return ORB_PNG8_E_FILTER;", "default: break;"),
    ("if (!scratch || scratch_len < sizeof(scratch_t))", "if (!scratch)"),
    ("value[i] = type[4 + 3 * i];", "value[i] = type[5 + 3 * i];"),
    ("    memset(value, 0, 256);\n", ""),
    ("if (!chunk_ok(type, dlen)) return ORB_PNG8_E_CRC;\n            if (!dlen", "if (!dlen"),
    ("if (!dlen || dlen % 3 || dlen > 768)", "if (!dlen || dlen > 768)"),
    ("if (!dlen || dlen % 3 || dlen > 768)", "if (!dlen || dlen % 3)"),
    ("return (int)(dlen / 3);", "return 256;"),
]


def main():
    src = open(SRC).read()
    fails = suite(src)
    ok = not fails
    if "--mutations" in sys.argv:
        caught = 0
        for old, new in MUTATIONS:
            if old not in src:
                print("mutation does not apply: %r" % old); ok = False; continue
            hit = bool(suite(src.replace(old, new, 1), quiet=True))
            caught += hit
            print("%s mutant: %s" % ("caught" if hit else "MISSED", new[:60]))
        print("%d of %d mutants caught" % (caught, len(MUTATIONS)))
        ok = ok and caught == len(MUTATIONS)
    print("all png8 checks pass" if ok else "png8 checks FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
