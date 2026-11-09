/*
 * orb_voice.c — an app's voice bank (orb_voice.h, docs/theme-cache-plan.md §2.6).
 *
 * Pure C: tools/host_voice_test.py builds it on gcc and throws malformed banks at it.
 */
#include <string.h>

#include "esp_rom_crc.h"
#include "orb_adpcm.h"
#include "orb_voice.h"

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The head's words. */
enum { H_MAGIC, H_VERSION, H_N, H_PATTERN, H_DATA_OFF, H_DATA_BYTES, H_SRC_CRC, H_CRC };

/* A sound's bytes, from its encoding and samples; 0 for an encoding this does not know. */
static uint32_t sound_bytes(uint8_t enc, uint32_t n)
{
    if (enc == CLK_SND_ENC_PCM16) return 2u * n;
    if (enc == CLK_SND_ENC_ADPCM) return orb_adpcm_bytes(n);
    return 0;
}

uint32_t orb_voice_head_bytes(uint32_t n)
{
    return ORB_VOICE_HEAD_BYTES + n * ORB_VOICE_REC_BYTES;
}

static uint32_t head_crc(const uint8_t *b, uint32_t n)
{
    static const uint8_t zero[4] = { 0 };
    uint32_t c = esp_rom_crc32_le(0, b, H_CRC * 4u);
    c = esp_rom_crc32_le(c, zero, 4u);
    return esp_rom_crc32_le(c, b + ORB_VOICE_HEAD_BYTES, n * ORB_VOICE_REC_BYTES);
}

uint32_t orb_voice_build(const orb_voice_in_t *in, uint32_t n, uint8_t pattern, uint32_t src_crc,
                         uint8_t *out, uint32_t cap)
{
    const uint32_t head = orb_voice_head_bytes(n);
    if (!n || n > ORB_VOICE_MAX || cap < head) return 0;
    memset(out, 0, head);
    uint32_t at = 0;                                     /* data-relative */
    for (uint32_t k = 0; k < n; k++) {
        const clk_sound_t *s = &in[k].s;
        if (!s->n || in[k].bytes != sound_bytes(s->enc, s->n)) return 0;
        if (in[k].bytes > ORB_VOICE_CAP - head - at) return 0;
        uint8_t *r = out + ORB_VOICE_HEAD_BYTES + k * ORB_VOICE_REC_BYTES;
        put32(r, at);
        put32(r + 4, s->n);
        put32(r + 8, s->first_beat);
        put32(r + 12, in[k].bytes);
        r[16] = (uint8_t)s->beat_chz; r[17] = (uint8_t)(s->beat_chz >> 8);
        r[18] = s->slot; r[19] = s->flags; r[20] = s->enc;
        at += (in[k].bytes + 1u) & ~1u;                  /* the next one even */
    }
    put32(out + H_MAGIC * 4, ORB_VOICE_MAGIC);
    put32(out + H_VERSION * 4, ORB_VOICE_VERSION);
    put32(out + H_N * 4, n);
    put32(out + H_PATTERN * 4, pattern);
    put32(out + H_DATA_OFF * 4, head);
    put32(out + H_DATA_BYTES * 4, at);
    put32(out + H_SRC_CRC * 4, src_crc);
    put32(out + H_CRC * 4, head_crc(out, n));
    return head + at;
}

const char *orb_voice_parse(const uint8_t *b, uint32_t len, orb_voice_t *out)
{
    memset(out, 0, sizeof *out);
    if (len < ORB_VOICE_HEAD_BYTES) return "a voice bank shorter than its head";
    if (get32(b + H_MAGIC * 4) != ORB_VOICE_MAGIC) return "no voice bank here";
    if (get32(b + H_VERSION * 4) != ORB_VOICE_VERSION) return "a voice bank of another version";
    const uint32_t n = get32(b + H_N * 4);
    if (!n || n > ORB_VOICE_MAX) return "a voice bank with no sounds, or more than it may hold";
    const uint32_t head = orb_voice_head_bytes(n);
    if (len < head) return "a voice bank shorter than its table";
    if (get32(b + H_CRC * 4) != head_crc(b, n)) return "the voice bank's CRC is wrong";
    const uint32_t data_off = get32(b + H_DATA_OFF * 4), data_bytes = get32(b + H_DATA_BYTES * 4);
    if (data_off != head) return "a voice bank whose data does not follow its table";
    if (data_bytes > len - head || data_bytes > ORB_VOICE_CAP - head) return "a voice bank's data past its end";
    out->n = n;
    out->pattern = (uint8_t)get32(b + H_PATTERN * 4);
    out->src_crc = get32(b + H_SRC_CRC * 4);
    out->data_off = data_off;
    out->data_bytes = data_bytes;
    for (uint32_t k = 0; k < n; k++) {
        const uint8_t *r = b + ORB_VOICE_HEAD_BYTES + k * ORB_VOICE_REC_BYTES;
        clk_sound_t *s = &out->s[k];
        const uint32_t at = get32(r), bytes = get32(r + 12);
        s->n = get32(r + 4);
        s->first_beat = get32(r + 8);
        s->beat_chz = (uint16_t)(r[16] | r[17] << 8);
        s->slot = r[18]; s->flags = r[19]; s->enc = r[20];
        if (at & 1u) return "a sound at an odd offset";
        if (!s->n || bytes != sound_bytes(s->enc, s->n)) return "a sound whose bytes are not its samples'";
        if (at > data_bytes || bytes > data_bytes - at) return "a sound past the bank's data";
        s->pcm_off = data_off + at;                      /* from the bank's start */
        out->bytes[k] = bytes;
    }
    return NULL;
}
