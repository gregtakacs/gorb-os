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
#include <stdlib.h>
bool orb_inflate_stream(const uint8_t *in, size_t n, size_t want, orb_inflate_sink_t sink, void *ctx)
{
    uint8_t *out = malloc(want ? want : 1);
    const bool ok = out && orb_inflate(in, n, out, want);
    if (ok) sink(ctx, out, want);
    free(out);
    return ok;
}
#else
#include "miniz.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

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

bool orb_inflate_stream(const uint8_t *in, size_t n, size_t want, orb_inflate_sink_t sink, void *ctx)
{
    /* miniz's own callback loop (tinfl_decompress_mem_to_callback), with both buffers in
     * PSRAM for the reason above. The window wraps; each pass hands over what it wrote. */
    tinfl_decompressor *d = heap_caps_malloc(sizeof *d, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *dict = heap_caps_malloc(TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = false;
    if (d && dict) {
        tinfl_init(d);
        size_t in_ofs = 0, dict_ofs = 0, total = 0;
        tinfl_status st;
        for (;;) {
            size_t in_n = n - in_ofs, out_n = TINFL_LZ_DICT_SIZE - dict_ofs;
            st = tinfl_decompress(d, in + in_ofs, &in_n, dict, dict + dict_ofs, &out_n,
                                  TINFL_FLAG_PARSE_ZLIB_HEADER);
            in_ofs += in_n;
            if (out_n) {
                total += out_n;
                if (total > want) { st = TINFL_STATUS_FAILED; break; }
                sink(ctx, dict + dict_ofs, out_n);
            }
            dict_ofs = (dict_ofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
            if (st != TINFL_STATUS_HAS_MORE_OUTPUT) break;
        }
        ok = st == TINFL_STATUS_DONE && total == want;
        if (!ok) ESP_LOGW("inflate", "stream ended: status %d, in %u of %u, out %u of %u",
                          (int)st, (unsigned)in_ofs, (unsigned)n, (unsigned)total, (unsigned)want);
    } else {
        ESP_LOGW("inflate", "stream: no memory for the decoder");
    }
    heap_caps_free(dict);
    heap_caps_free(d);
    return ok;
}
#endif
