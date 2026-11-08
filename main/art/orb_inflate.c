/**
 * orb_inflate.c — see the header (§60).
 */
#include "orb_inflate.h"

#ifdef ORB_INFLATE_HOST
#include <zlib.h>
bool orb_inflate(const uint8_t *in, size_t n, uint8_t *out, size_t want)
{
    uLongf got = (uLongf)want;
    return uncompress(out, &got, in, (uLong)n) == Z_OK && got == want;
}
#else
#include "miniz.h"
#include "esp_heap_caps.h"

bool orb_inflate(const uint8_t *in, size_t n, uint8_t *out, size_t want)
{
    /* PSRAM only. Internal RAM measured 6 ms faster, but it is the pool the display's DMA
     * ring is re-funded from after every card read: 11 KB taken there mid-scroll left the
     * ring short ("re-funded short: 1 of 2"), or unfunded, and the glass froze on its last
     * frame (Gold, 2026-11-07, the Themes page scrolled fast). */
    tinfl_decompressor *d = heap_caps_malloc(sizeof *d, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) return false;
    tinfl_init(d);
    size_t isz = n, osz = want;
    const tinfl_status st = tinfl_decompress(d, in, &isz, out, out, &osz,
                                             TINFL_FLAG_PARSE_ZLIB_HEADER |
                                             TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    heap_caps_free(d);
    return st == TINFL_STATUS_DONE && osz == want;
}
#endif
