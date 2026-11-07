/* An indexed-PNG reader (radar-plan.md §2.3): colour type 3 at bit depths 1, 2, 4 and 8,
 * non-interlaced, streamed through the S3 ROM's tinfl into a 32 KB window and handed out a
 * row at a time as 8-bit indices. No whole-image buffer: the radar's 512x512 frame costs
 * the scratch below, not 256 KB. The palette in the file is ignored; the caller colours
 * the indices with its own.
 *
 * Plain C over miniz's tinfl API, so the host suite (tools/host_png8_test.py) compiles this
 * same file against a zlib stand-in. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define ORB_PNG8_MAX_W 1024

/* Scratch the reader needs: the inflater's state, its 32 KB window and two rows. */
size_t orb_png8_scratch_size(void);

enum {
    ORB_PNG8_OK = 0,
    ORB_PNG8_E_SIG = -1,       /* not a PNG */
    ORB_PNG8_E_FORMAT = -2,    /* not indexed, interlaced, an unknown method, or too wide */
    ORB_PNG8_E_CRC = -3,       /* a chunk's CRC does not match */
    ORB_PNG8_E_TRUNC = -4,     /* the file ends inside a chunk, or before every row */
    ORB_PNG8_E_INFLATE = -5,   /* the deflate stream is bad */
    ORB_PNG8_E_FILTER = -6,    /* a row's filter type is not 0-4 */
    ORB_PNG8_E_SCRATCH = -7,   /* scratch too small */
    ORB_PNG8_E_STOP = -8,      /* the row callback asked to stop */
};

/* Called once per row, top to bottom, with w indices (0-255). Answers nonzero to stop. */
typedef int (*orb_png8_row_fn)(void *ctx, int y, const uint8_t *idx, int w);

/* Read the header only: width and height of an indexed PNG the reader accepts. */
int orb_png8_info(const uint8_t *png, size_t len, int *w, int *h);

/* Decode, calling row() for each row. Every chunk's CRC is checked before its data is used. */
int orb_png8_decode(const uint8_t *png, size_t len, void *scratch, size_t scratch_len,
                    orb_png8_row_fn row, void *ctx);
