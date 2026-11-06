/* orb_adpcm.h — a theme sound's compressed form: blocked IMA ADPCM, 4 bits a sample.
 *
 * A clip is ceil(n / ORB_ADPCM_BLOCK) blocks of ORB_ADPCM_BLOCK_BYTES each (the last
 * one padded). Each block restarts the decoder, so the mixer can reach any sample by
 * decoding the one block that holds it: an anchored loop re-enters at a phase, and a
 * skip inside silence moves one sample, without decoding from the clip's start.
 *
 *   int16 LE  sample 0 of the block, exact
 *   u8        the step index the next sample decodes from (0..88)
 *   u8        0
 *   128 B     samples 1..255, one nibble each, low nibble first; the last high nibble 0
 *
 * 132 B per 256 samples against PCM16's 512 (3.9:1). The encoder is
 * tools/theme_schema.py adpcm_encode and the baker's adpcmEncode; this decoder is the
 * only one the device runs, and tools/host_adpcm_test.py pins the three together.
 * Pure: no ESP headers, so the host suite compiles it as it stands. */
#pragma once
#include <stdint.h>

/* What a sound clip's bytes are (a sound row's `src`; orb_clock3.h CLK_SND_ENC_* and
 * theme_schema.SOUND_ENC_* are the same numbers). The bakers write ADPCM unless a clip
 * would lose too much to it (theme_schema.ADPCM_MIN_SNR_DB), and then PCM16. */
#define ORB_SOUND_ENC_PCM16 0u
#define ORB_SOUND_ENC_ADPCM 1u

#define ORB_ADPCM_BLOCK       256u
#define ORB_ADPCM_BLOCK_BYTES 132u

static inline uint32_t orb_adpcm_bytes(uint32_t n)
{
    return (n + ORB_ADPCM_BLOCK - 1u) / ORB_ADPCM_BLOCK * ORB_ADPCM_BLOCK_BYTES;
}

static const int16_t ORB_ADPCM_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
    60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t ORB_ADPCM_INDEX[16] = { -1, -1, -1, -1, 2, 4, 6, 8,
                                            -1, -1, -1, -1, 2, 4, 6, 8 };

/* One nibble against the running predictor and step index. */
static inline int16_t orb_adpcm_step(int32_t *pred, int32_t *idx, uint32_t code)
{
    const int32_t step = ORB_ADPCM_STEP[*idx];
    int32_t diff = step >> 3;
    if (code & 4u) diff += step;
    if (code & 2u) diff += step >> 1;
    if (code & 1u) diff += step >> 2;
    int32_t p = (code & 8u) ? *pred - diff : *pred + diff;
    if (p > 32767) p = 32767;
    if (p < -32768) p = -32768;
    *pred = p;
    int32_t i = *idx + ORB_ADPCM_INDEX[code & 15u];
    *idx = i < 0 ? 0 : i > 88 ? 88 : i;
    return (int16_t)p;
}

/* Decode block `b` of a clip into out[0..ORB_ADPCM_BLOCK-1]. */
static inline void orb_adpcm_block(const uint8_t *clip, uint32_t b, int16_t *out)
{
    const uint8_t *h = clip + (uint32_t)b * ORB_ADPCM_BLOCK_BYTES;
    int32_t pred = (int16_t)(uint16_t)(h[0] | (h[1] << 8));
    int32_t idx = h[2] > 88 ? 88 : h[2];
    out[0] = (int16_t)pred;
    const uint8_t *d = h + 4;
    for (uint32_t i = 1; i < ORB_ADPCM_BLOCK; i++) {
        const uint32_t k = i - 1u, code = (d[k >> 1] >> ((k & 1u) * 4u)) & 15u;
        out[i] = orb_adpcm_step(&pred, &idx, code);
    }
}
