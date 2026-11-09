#pragma once
/**
 * orb_voice — an app's voice bank (docs/theme-cache-plan.md §2.6): its theme's sounds, kept
 * in the pool as their own entry so they play whatever is on the glass, and stay when the
 * cache drops the theme's display entry.
 *
 * A bank is a head, a table of sounds and their bytes, all addressed from the bank's start:
 *
 *   head   32 B   magic "VBNK", version, count, the hour pattern, where the bytes start and
 *                 how many there are, the theme's file CRC (the choice the bank was made
 *                 for), and the CRC of the head and table
 *   table  24 B a sound: where its bytes start (from the data start, even), its samples,
 *                 first beat, bytes, beat in centihertz, slot, flags, encoding
 *   data         each sound's PCM16 or blocked IMA ADPCM, as the theme carried it
 *
 * Pure C: no partition, no allocator. orb_voice_parse is the one gate between a bank's bytes
 * and the mixer; every offset it answers lies inside the bank it was given.
 */

#include <stddef.h>
#include <stdint.h>

#include "orb_clock3.h"   /* clk_sound_t, CLK_SND_ENC_*, CLK_PAT_* */

#define ORB_VOICE_MAGIC       0x4B4E4256u   /* "VBNK", little-endian */
#define ORB_VOICE_VERSION     1u
#define ORB_VOICE_HEAD_BYTES  32u
#define ORB_VOICE_REC_BYTES   24u
#define ORB_VOICE_MAX         16u           /* sounds a bank holds */
#define ORB_VOICE_CAP         (1024u * 1024u)   /* a bank's bytes at most (theme-cache-plan ruling 6) */

/* One sound as the builder hands it in: its record and its byte count. */
typedef struct {
    clk_sound_t s;          /* pcm_off is ignored on the way in, set on the way out */
    uint32_t    bytes;
} orb_voice_in_t;

typedef struct {
    uint32_t    n;
    uint8_t     pattern;
    uint32_t    src_crc;
    uint32_t    data_off, data_bytes;
    clk_sound_t s[ORB_VOICE_MAX];   /* pcm_off: from the BANK's start, even */
    uint32_t    bytes[ORB_VOICE_MAX];
} orb_voice_t;

/* The head and table's size for n sounds, and where the data starts (both even). */
uint32_t orb_voice_head_bytes(uint32_t n);

/* Lay out a bank for `in`: write its head and table into `out` (orb_voice_head_bytes(n)
 * bytes), each sound's data offset in data order, and answer the bank's whole size, or 0
 * when it cannot be one (no sounds, too many, a sound whose bytes are not its samples', or
 * more than ORB_VOICE_CAP). The caller then copies sound k's bytes to the offset
 * orb_voice_parse answers for it. */
uint32_t orb_voice_build(const orb_voice_in_t *in, uint32_t n, uint8_t pattern, uint32_t src_crc,
                         uint8_t *out, uint32_t cap);

/* Check a bank's head and table against `len`, the bytes there are: NULL and *out filled,
 * or the refusal. The data's own bytes are the caller's to CRC. */
const char *orb_voice_parse(const uint8_t *bank, uint32_t len, orb_voice_t *out);
