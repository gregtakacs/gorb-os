#!/usr/bin/env python3
"""Host suite for main/art/orb_voice.c, an app's voice bank (docs/theme-cache-plan.md §2.6).

Compiles the shipped file with host gcc and checks:
  - a bank built from PCM16 and ADPCM sounds parses back to the same sounds, every offset
    even and inside the bank, the data packed after the table;
  - every malformed bank is refused by the sentence that names it;
  - the builder refuses what the parser would (bytes that are not a sound's samples', too
    many sounds, more than the 1 MB a bank may hold);
  - 20,000 random corruptions of a good bank never parse into a sound that reaches past
    the bytes the parser was given.

    python3 tools/host_voice_test.py [--mutations]

--mutations breaks each guard in turn and checks a test notices.
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VOICE_C = os.path.join(ROOT, "main", "art", "orb_voice.c")

SHIM_CRC = """#pragma once
#include <stdint.h>
#include <stddef.h>
#include <zlib.h>
static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, size_t len)
{ return (uint32_t)crc32((uLong)crc, buf, (uInt)len); }
"""

HARNESS_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_rom_crc.h"
#include "orb_adpcm.h"
#include "orb_voice.h"

static int g_fail;
#define CK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t bank[ORB_VOICE_CAP + 64];

static orb_voice_in_t snd(uint8_t slot, uint8_t enc, uint32_t n, uint8_t flags)
{
    orb_voice_in_t v;
    memset(&v, 0, sizeof v);
    v.s.slot = slot; v.s.enc = enc; v.s.n = n; v.s.flags = flags;
    v.s.first_beat = n / 3; v.s.beat_chz = flags ? 100 : 0;
    v.bytes = enc == CLK_SND_ENC_PCM16 ? 2 * n : orb_adpcm_bytes(n);
    return v;
}

/* A good bank: a strike in PCM16, a chime in ADPCM, an odd-sample loop. */
static uint32_t good(orb_voice_in_t *in)
{
    in[0] = snd(1, CLK_SND_ENC_PCM16, 2205, 0);
    in[1] = snd(4, CLK_SND_ENC_ADPCM, 44100, 0);
    in[2] = snd(7, CLK_SND_ENC_PCM16, 1001, CLK_SND_F_LOOP);
    const uint32_t size = orb_voice_build(in, 3, 2, 0xC0FFEE, bank, sizeof bank);
    for (uint32_t i = orb_voice_head_bytes(3); i < size; i++) bank[i] = (uint8_t)(i * 13);
    return size;
}

/* Rewrite the head's CRC after a hand edit, so the edit is what is refused. */
static void recrc(uint32_t n)
{
    static const uint8_t zero[4] = { 0 };
    uint32_t c = esp_rom_crc32_le(0, bank, 28);
    c = esp_rom_crc32_le(c, zero, 4);
    c = esp_rom_crc32_le(c, bank + ORB_VOICE_HEAD_BYTES, n * ORB_VOICE_REC_BYTES);
    memcpy(bank + 28, &c, 4);
}
static void put(uint32_t at, uint32_t v) { memcpy(bank + at, &v, 4); }

int main(void)
{
    orb_voice_in_t in[ORB_VOICE_MAX + 1];
    orb_voice_t v;

    /* the plain road */
    const uint32_t size = good(in);
    CK(size == orb_voice_head_bytes(3) + 4410 + orb_adpcm_bytes(44100) + 2002, "the bank's size (%u)", size);
    const char *why = orb_voice_parse(bank, size, &v);
    CK(!why && v.n == 3 && v.pattern == 2 && v.src_crc == 0xC0FFEE, "a good bank parses (%s)", why ? why : "");
    for (uint32_t k = 0; k < v.n; k++) {
        CK(v.s[k].slot == in[k].s.slot && v.s[k].n == in[k].s.n && v.s[k].enc == in[k].s.enc &&
           v.s[k].flags == in[k].s.flags && v.s[k].first_beat == in[k].s.first_beat &&
           v.s[k].beat_chz == in[k].s.beat_chz && v.bytes[k] == in[k].bytes, "sound %u round-trips", k);
        CK(!(v.s[k].pcm_off & 1) && v.s[k].pcm_off >= v.data_off && v.s[k].pcm_off + v.bytes[k] <= size,
           "sound %u is even and inside the bank", k);
    }
    CK(v.s[1].pcm_off == v.s[0].pcm_off + 4410 && v.s[2].pcm_off == v.s[1].pcm_off + ((in[1].bytes + 1) & ~1u),
       "the sounds are packed in order, each even");
    printf("  build: three sounds round-trip, packed, even, inside the bank\n");

    /* refusals, each by name */
    struct { const char *why; int (*f)(uint32_t *); } V[] = {
#define VEC(why, body) { why, ({ int f_(uint32_t *len) { (void)len; body; return 0; } f_; }) }
        VEC("shorter than its head", *len = 20),
        VEC("no voice bank here", put(0, 0)),
        VEC("another version", put(4, 2); recrc(3)),
        VEC("no sounds", put(8, 0); recrc(0)),
        VEC("no sounds", put(8, ORB_VOICE_MAX + 1)),
        VEC("shorter than its table", *len = ORB_VOICE_HEAD_BYTES + 30),
        VEC("CRC is wrong", bank[40] ^= 1),
        VEC("does not follow its table", put(16, orb_voice_head_bytes(3) + 2); recrc(3)),
        VEC("data past its end", *len -= 2),
        VEC("odd offset", put(ORB_VOICE_HEAD_BYTES + ORB_VOICE_REC_BYTES, 4411); recrc(3)),
        VEC("not its samples'", put(ORB_VOICE_HEAD_BYTES + 12, 4412); recrc(3)),
        VEC("not its samples'", bank[ORB_VOICE_HEAD_BYTES + 20] = 9; recrc(3)),
        VEC("not its samples'", put(ORB_VOICE_HEAD_BYTES + 4, 0); put(ORB_VOICE_HEAD_BYTES + 12, 0); recrc(3)),
        VEC("past the bank's data", put(ORB_VOICE_HEAD_BYTES + 2 * ORB_VOICE_REC_BYTES, 4410 + 30000); recrc(3)),
    };
    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++) {
        uint32_t len = good(in);
        V[i].f(&len);
        const char *w = orb_voice_parse(bank, len, &v);
        CK(w && strstr(w, V[i].why), "vector %zu: %s, not \"%s\"", i, w ? w : "accepted", V[i].why);
    }
    printf("  parse: %zu malformed banks refused by name\n", sizeof V / sizeof V[0]);

    /* the builder refuses what the parser would */
    good(in);
    in[1].bytes += 2;
    CK(!orb_voice_build(in, 3, 0, 0, bank, sizeof bank), "a sound whose bytes are not its samples' is not built");
    good(in);
    CK(!orb_voice_build(in, 0, 0, 0, bank, sizeof bank), "no sounds, no bank");
    for (int k = 0; k <= (int)ORB_VOICE_MAX; k++) in[k] = snd((uint8_t)k, CLK_SND_ENC_PCM16, 10, 0);
    CK(!orb_voice_build(in, ORB_VOICE_MAX + 1, 0, 0, bank, sizeof bank), "more sounds than a bank holds");
    CK(orb_voice_build(in, ORB_VOICE_MAX, 0, 0, bank, sizeof bank), "as many as it holds");
    in[0] = snd(1, CLK_SND_ENC_PCM16, ORB_VOICE_CAP / 2, 0);
    CK(!orb_voice_build(in, 1, 0, 0, bank, sizeof bank), "a bank over 1 MB is not built");
    in[0] = snd(1, CLK_SND_ENC_PCM16, (ORB_VOICE_CAP - orb_voice_head_bytes(1)) / 2, 0);
    CK(orb_voice_build(in, 1, 0, 0, bank, sizeof bank) == ORB_VOICE_CAP, "one that fills 1 MB exactly is");
    printf("  build: refuses bad bytes, none, too many, over 1 MB\n");

    /* fuzz: no corruption parses into a sound past the bytes given */
    srand(7);
    int parsed = 0;
    for (int t = 0; t < 20000; t++) {
        uint32_t len = good(in);
        const int flips = 1 + rand() % 4;
        for (int f = 0; f < flips; f++) bank[rand() % orb_voice_head_bytes(3)] ^= (uint8_t)(1u << (rand() % 8));
        if (rand() % 2) recrc(3);
        if (rand() % 4 == 0) len -= (uint32_t)(rand() % 64);
        if (orb_voice_parse(bank, len, &v)) continue;
        parsed++;
        for (uint32_t k = 0; k < v.n; k++)
            CK(v.s[k].pcm_off + v.bytes[k] <= len && !(v.s[k].pcm_off & 1), "fuzz %d: sound %u reaches past the bank", t, k);
    }
    printf("  fuzz: 20000 corruptions, %d parsed, none reached past its bytes\n", parsed);
    printf("%d failures\n", g_fail);
    return g_fail ? 1 : 0;
}
"""

MUTANTS = (
    ("the magic", 'if (get32(b + H_MAGIC * 4) != ORB_VOICE_MAGIC) return "no voice bank here";', ""),
    ("the count", "if (!n || n > ORB_VOICE_MAX) return", "if (0) return"),
    ("the table's length", 'if (len < head) return "a voice bank shorter than its table";', ""),
    ("the CRC", 'if (get32(b + H_CRC * 4) != head_crc(b, n)) return "the voice bank\'s CRC is wrong";', ""),
    ("the data's end", "if (data_bytes > len - head || data_bytes > ORB_VOICE_CAP - head)", "if (0)"),
    ("the odd offset", 'if (at & 1u) return "a sound at an odd offset";', ""),
    ("a sound's bytes", "if (!s->n || bytes != sound_bytes(s->enc, s->n))", "if (!s->n)"),
    ("a sound past the data", "if (at > data_bytes || bytes > data_bytes - at)", "if (0)"),
    ("the builder's bytes", "if (!s->n || in[k].bytes != sound_bytes(s->enc, s->n)) return 0;", ""),
    ("the builder's cap", "if (in[k].bytes > ORB_VOICE_CAP - head - at) return 0;", ""),
)


def build(d, src, strict=True):
    open(os.path.join(d, "esp_rom_crc.h"), "w").write(SHIM_CRC)
    open(os.path.join(d, "harness.c"), "w").write(HARNESS_C)
    open(os.path.join(d, "orb_voice.c"), "w").write(src)
    exe = os.path.join(d, "t")
    warn = ["-Wall", "-Wextra", "-Werror"] if strict else []
    cc = subprocess.run(["gcc", "-std=gnu11", "-O1", *warn, "-Wno-unused-function",
                         "-I", d, "-I", os.path.join(ROOT, "main", "art"),
                         "-I", os.path.join(ROOT, "main", "audio"),
                         os.path.join(d, "harness.c"), os.path.join(d, "orb_voice.c"),
                         "-lz", "-o", exe], capture_output=True, text=True)
    if cc.returncode:
        raise SystemExit("gcc failed:\n" + cc.stderr)
    return exe


def main():
    src = open(VOICE_C).read()
    with tempfile.TemporaryDirectory() as d:
        exe = build(d, src)
        r = subprocess.run([exe], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        ok = r.returncode == 0
        if "--mutations" in sys.argv:
            caught = 0
            for what, old, new in MUTANTS:
                if src.count(old) != 1:
                    print("FAIL mutant %r: its guard is not in orb_voice.c once" % what)
                    ok = False
                    continue
                m = subprocess.run([build(d, src.replace(old, new), strict=False)],
                                   capture_output=True, text=True, timeout=300)
                if m.returncode == 0:
                    print("FAIL uncaught mutant: %s" % what)
                    ok = False
                else:
                    caught += 1
            print("  mutants: %d of %d guards caught when broken" % (caught, len(MUTANTS)))
        print("all voice checks pass" if ok else "VOICE FAILURES")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
