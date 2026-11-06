/* orb_disc.h — a square picture stored as its disc alone (OS v1.2.12).
 *
 * The glass is round, so a 466x466 plate's four corners are never seen: 21% of every
 * square plate is bytes nobody looks at. A disc keeps, row by row, only the span of
 * pixels whose centres fall within side/2 + 1 px of the square's centre, rows back to
 * back. The spans are not stored: every reader derives them from the one integer rule
 * below, so the bakers (orbtheme-baker.js discSpan, theme_schema.disc_span) and the
 * device cannot disagree about a row; tools/host_disc_test.py pins the three.
 *
 * Three pictures take the disc: the plate (or a delta bank's keyframe), the masked glass
 * overlay (its colour disc, then its alpha disc), and a theme's preview at its own side.
 * The radius is one pixel past the half side, so a picture's own anti-aliased rim
 * survives whole. The device expands a disc into its square before anything reads it
 * (orb_sched.c and orb_stage.c at bind, orb_theme_preview_read in place), so every
 * reader still addresses a square; the corners read black, or clear under an alpha.
 * Pure: tools/host_disc_test.py compiles it as it stands. */
#pragma once
#include <stdint.h>
#include <string.h>

#define ORB_DISC_SIDE 466u
#define ORB_DISC_R    234u      /* the panel's: ORB_DISC_SIDE / 2 + 1 */

/* Row y's span for a square of `side` (even), x0..x1 inclusive, symmetric about the
 * centre. A pixel is in when its centre's distance from the centre, in half pixels, is
 * at most 2R with R = side / 2 + 1. */
static inline void orb_disc_span_n(uint32_t side, uint32_t y, uint32_t *x0, uint32_t *x1)
{
    const int64_t r = (int64_t)side / 2 + 1;
    const int32_t dy = 2 * (int32_t)y + 1 - (int32_t)side;
    const int64_t room = 4ll * r * r - (int64_t)dy * dy;
    uint32_t x = 0;
    while (x < side / 2u) {
        const int32_t dx = 2 * (int32_t)x + 1 - (int32_t)side;
        if ((int64_t)dx * dx <= room) break;
        x++;
    }
    *x0 = x;
    *x1 = side - 1u - x;
}

/* The pixels a disc of `side` keeps. */
static inline uint32_t orb_disc_px(uint32_t side)
{
    uint32_t n = 0, x0, x1;
    for (uint32_t y = 0; y < side; y++) {
        orb_disc_span_n(side, y, &x0, &x1);
        n += x1 - x0 + 1u;
    }
    return n;
}

/* An RGB565 disc into its square (stride `side`), the corners black. */
static inline void orb_disc_expand_n(const uint8_t *disc, uint16_t *square, uint32_t side)
{
    uint32_t x0, x1;
    for (uint32_t y = 0; y < side; y++) {
        orb_disc_span_n(side, y, &x0, &x1);
        uint16_t *row = square + y * side;
        const uint32_t n = x1 - x0 + 1u;
        memset(row, 0, x0 * 2u);
        memcpy(row + x0, disc, n * 2u);
        memset(row + x1 + 1u, 0, (side - 1u - x1) * 2u);
        disc += n * 2u;
    }
}

/* A masked disc (its RGB565 disc, then its alpha disc) into an RGB565A8 square (colour
 * plane, then alpha plane), the corners black and clear. */
static inline void orb_disc_expand_masked(const uint8_t *disc, uint8_t *square, uint32_t side)
{
    orb_disc_expand_n(disc, (uint16_t *)square, side);
    const uint8_t *a = disc + orb_disc_px(side) * 2u;
    uint8_t *alpha = square + side * side * 2u;
    uint32_t x0, x1;
    for (uint32_t y = 0; y < side; y++) {
        orb_disc_span_n(side, y, &x0, &x1);
        uint8_t *row = alpha + y * side;
        const uint32_t n = x1 - x0 + 1u;
        memset(row, 0, x0);
        memcpy(row + x0, a, n);
        memset(row + x1 + 1u, 0, side - 1u - x1);
        a += n;
    }
}

/* An RGB565 disc read into the head of its own square's buffer, expanded where it lies:
 * rows bottom up, each moved before its corners are cleared. Row y's square starts at
 * y*side*2, never before the end of the disc rows above it, so nothing unread is
 * overwritten. */
static inline void orb_disc_expand_inplace(uint8_t *buf, uint32_t side)
{
    uint32_t at = orb_disc_px(side) * 2u, x0, x1;
    for (uint32_t y = side; y-- > 0;) {
        orb_disc_span_n(side, y, &x0, &x1);
        const uint32_t n = (x1 - x0 + 1u) * 2u;
        at -= n;
        uint8_t *row = buf + y * side * 2u;
        memmove(row + x0 * 2u, buf + at, n);
        memset(row, 0, x0 * 2u);
        memset(row + (x1 + 1u) * 2u, 0, (side - 1u - x1) * 2u);
    }
}

/* The panel's own: a 466 px plate. */
static inline void orb_disc_span(uint32_t y, uint32_t *x0, uint32_t *x1)
{
    orb_disc_span_n(ORB_DISC_SIDE, y, x0, x1);
}
static inline uint32_t orb_disc_bytes(void) { return orb_disc_px(ORB_DISC_SIDE) * 2u; }
static inline void orb_disc_expand(const uint8_t *disc, uint16_t *square)
{
    orb_disc_expand_n(disc, square, ORB_DISC_SIDE);
}
