#pragma once
/**
 * orb_inflate — a zlib stream into a buffer, by the ROM's own tinfl (§60: a theme's preview
 * is the whole glass, stored as its disc and deflated). The decoder's ~11 KB of state lives on
 * the heap in PSRAM — never internal RAM, which is the display's DMA ring's pool (see the .c) —
 * and never on the caller's stack: no task in the tree has room for it there.
 *
 * Host suites link a zlib twin (tools/host_*: -DORB_INFLATE_HOST), so a reader that compiles
 * there reads the same bytes.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Inflate `in` (a whole zlib stream, header and Adler-32) into `out`. True only when the
 *  stream ended cleanly, its checksum held, and it filled exactly `want` bytes. */
bool orb_inflate(const uint8_t *in, size_t n, uint8_t *out, size_t want);

/** §61: the same stream handed to `sink` as it inflates, through a 32 KB window rather than
 *  a buffer the size of the output: a full-glass preview needs 434 KB contiguous to inflate
 *  whole, and the theme carousel cannot count on that beside a running clock. `sink` sees
 *  every output byte once, in order. True as orb_inflate is: a clean end, a good checksum,
 *  exactly `want` bytes. */
typedef void (*orb_inflate_sink_t)(void *ctx, const uint8_t *p, size_t n);
bool orb_inflate_stream(const uint8_t *in, size_t n, size_t want, orb_inflate_sink_t sink, void *ctx);

/** A zlib stream's two-byte header, as orb_theme tells a deflated preview by (with its size). */
static inline bool orb_inflate_head_ok(const uint8_t *p, size_t n)
{
    return n >= 6 && (p[0] & 0x0F) == 8 && (p[0] >> 4) <= 7 && ((p[0] << 8) | p[1]) % 31 == 0 &&
           !(p[1] & 0x20);
}
