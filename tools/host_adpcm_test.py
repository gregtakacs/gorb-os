#!/usr/bin/env python3
"""Host bench for a theme sound's compressed form (main/audio/orb_adpcm.h).

Three implementations must agree, because the bakers write what the device plays:

- the device's decoder, orb_adpcm.h, compiled here as it stands with host gcc -Werror,
  must turn every block into exactly the samples theme_schema.adpcm_decode does;
- the hub's baker (orbtheme-baker.js adpcmEncode, run in node) must write exactly the
  bytes theme_schema.adpcm_encode does;
- and the round trip must stay close to the signal: a chime-like decaying tone, a
  click train like the tick loop, noise, silence and full-scale steps, each judged by
  its signal-to-noise ratio against what the 2 W speaker can show.

Plus the plane's size rule (orb_adpcm_bytes == theme_schema.sound_bytes) and the
block independence the mixer leans on: any block decodes alone.

Run:  python3 tools/host_adpcm_test.py              (rc 0 = pass)
      python3 tools/host_adpcm_test.py --mutations  (proves the checks bite)
"""
import json, math, os, random, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import theme_schema as ts  # noqa: E402

HDR = os.path.join(ROOT, "main", "audio", "orb_adpcm.h")
BAKER = os.path.join(ROOT, "tools", "orb-manager", "web", "runtime", "baker@1", "orbtheme-baker.js")
NODE = shutil.which("node") or os.path.expanduser("~/.local/share/nodeenv-gorb/bin/node")

MUTATIONS = [
    ("if (code & 2u) diff += step >> 1;", "if (code & 2u) diff += step >> 2;"),
    ("*idx = i < 0 ? 0 : i > 88 ? 88 : i;", "*idx = i < 0 ? 0 : i > 88 ? 87 : i;"),
    ("int32_t idx = h[2] > 88 ? 88 : h[2];", "int32_t idx = 0;"),
    ("(d[k >> 1] >> ((k & 1u) * 4u)) & 15u", "(d[k >> 1] >> ((~k & 1u) * 4u)) & 15u"),
    ("return (n + ORB_ADPCM_BLOCK - 1u) / ORB_ADPCM_BLOCK * ORB_ADPCM_BLOCK_BYTES;",
     "return n / ORB_ADPCM_BLOCK * ORB_ADPCM_BLOCK_BYTES;"),
]

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    /* argv: n, then the clip's bytes on stdin; prints the size rule and every sample */
    const unsigned n = (unsigned)strtoul(argv[1], 0, 10);
    static unsigned char clip[1 << 20];
    const size_t got = fread(clip, 1, sizeof clip, stdin);
    printf("%u %zu\n", orb_adpcm_bytes(n), got);
    int16_t blk[ORB_ADPCM_BLOCK];
    /* blocks decoded last to first: each must stand alone */
    const unsigned nb = (n + ORB_ADPCM_BLOCK - 1) / ORB_ADPCM_BLOCK;
    static int16_t all[1 << 19];
    for (unsigned b = nb; b-- > 0;) {
        orb_adpcm_block(clip, b, blk);
        for (unsigned i = 0; i < ORB_ADPCM_BLOCK; i++) all[b * ORB_ADPCM_BLOCK + i] = blk[i];
    }
    for (unsigned i = 0; i < n; i++) printf("%d\n", all[i]);
    (void)argc;
    return 0;
}
"""

fails = 0


def check(label, ok, why=""):
    global fails
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok or not why else "  " + why))
    fails += 0 if ok else 1


def build(d, mutation=None):
    src = open(HDR).read()
    if mutation:
        if mutation[0] not in src:
            raise SystemExit("mutation %r does not apply -- rewrite the bench" % mutation[0])
        src = src.replace(mutation[0], mutation[1], 1)
    c, exe = os.path.join(d, "a.c"), os.path.join(d, "a")
    open(c, "w").write(src.replace("#pragma once", "") + HARNESS)
    r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-o", exe, c],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("compile failed:\n" + r.stderr)
    return exe


def c_decode(exe, data, n):
    p = subprocess.run([exe, str(n)], input=data, capture_output=True)
    lines = p.stdout.decode().split("\n")
    want, got = (int(x) for x in lines[0].split())
    return want, [int(x) for x in lines[1:1 + n]]


def snr_db(ref, out):
    sig = sum(x * x for x in ref) or 1
    err = sum((a - b) ** 2 for a, b in zip(ref, out)) or 1
    return 10 * math.log10(sig / err)


def signals():
    r = random.Random(7)
    R = ts.SOUND_RATE
    chime = [int(14000 * math.exp(-3 * i / R) * math.sin(2 * math.pi * 880 * i / R)
                 + 6000 * math.exp(-5 * i / R) * math.sin(2 * math.pi * 1320 * i / R))
             for i in range(R)]
    clicks = []
    for i in range(R // 2):
        t = i % (R // 4)
        clicks.append(int(20000 * math.exp(-t / 40) * math.sin(2 * math.pi * 3000 * t / R)))
    noise = [r.randint(-8000, 8000) for _ in range(3001)]
    steps = [32767 if (i // 300) % 2 else -32768 for i in range(1200)]
    return {"chime (1 s, decaying 880+1320 Hz)": (chime, 22),
            "tick train (2 ticks, 3 kHz bursts)": (clicks, 4),
            "noise, 3001 samples (a partial last block)": (noise, 12),
            "silence": ([0] * 700, None),
            "full-scale steps": (steps, 10)}


def js_encode(pcm):
    js = ("const G=require(%s);const pcm=JSON.parse(require('fs').readFileSync(0,'utf8'));"
          "process.stdout.write(Buffer.from(G.adpcmEncode(pcm)).toString('hex'));") % json.dumps(BAKER)
    p = subprocess.run([NODE, "-e", js], input=json.dumps(pcm), capture_output=True, text=True)
    if p.returncode:
        raise SystemExit("node: " + p.stderr[-400:])
    return bytes.fromhex(p.stdout)


def js_choose(pcm):
    js = ("const G=require(%s);const pcm=JSON.parse(require('fs').readFileSync(0,'utf8'));"
          "const r=G.soundEncode(pcm);process.stdout.write(r.enc+' '+Buffer.from(r.bytes).toString('hex'));"
          ) % json.dumps(BAKER)
    p = subprocess.run([NODE, "-e", js], input=json.dumps(pcm), capture_output=True, text=True)
    if p.returncode:
        raise SystemExit("node: " + p.stderr[-400:])
    enc, hexb = p.stdout.split(" ")
    return int(enc), bytes.fromhex(hexb)


def run(exe, quiet=False):
    bad = 0
    for name, (pcm, floor) in signals().items():
        n = len(pcm)
        enc = ts.adpcm_encode(pcm)
        ref = ts.adpcm_decode(enc, n)
        want, got = c_decode(exe, enc, n)
        ok = [len(enc) == ts.sound_bytes(ts.SOUND_ENC_ADPCM, n) == want,
              got == ref]
        if not quiet:
            check("%s: the plane is %d B, by both size rules" % (name, len(enc)), ok[0])
            check("%s: the device decodes what the reference decodes" % name, ok[1])
        if floor is not None:
            s = snr_db(pcm, ref)
            ok.append(s >= floor)
            if not quiet:
                check("%s: round trip %.1f dB SNR (floor %d)" % (name, s, floor), s >= floor)
        else:
            ok.append(max(abs(x) for x in ref) == 0)
            if not quiet:
                check("%s: stays silent" % name, ok[-1])
        bad += not all(ok)
    return bad


def main():
    with tempfile.TemporaryDirectory() as d:
        if "--mutations" in sys.argv:
            caught = 0
            for m in MUTATIONS:
                bit = run(build(d, m), quiet=True) > 0
                print(("bites " if bit else "MISSED") + "  " + m[0][:64])
                caught += bit
            print("%d of %d mutants caught" % (caught, len(MUTATIONS)))
            return 0 if caught == len(MUTATIONS) else 1
        run(build(d))
    print("(the round-trip floors are the codec's; a clip under %d dB is stored as PCM16)" % ts.ADPCM_MIN_SNR_DB)
    for name, (pcm, _f) in signals().items():
        check("%s: the baker writes the schema's bytes" % name,
              js_encode(pcm) == ts.adpcm_encode(pcm))
    # the per-clip choice: a chime compresses, a sharp tick stays PCM16, and both bakers
    # choose alike on the same sums
    chime, ticks = signals()["chime (1 s, decaying 880+1320 Hz)"][0], signals()["tick train (2 ticks, 3 kHz bursts)"][0]
    check("a chime is stored as ADPCM", ts.sound_encode(chime)[0] == ts.SOUND_ENC_ADPCM)
    check("a sharp tick stays PCM16 (it would fall under %d dB)" % ts.ADPCM_MIN_SNR_DB,
          ts.sound_encode(ticks)[0] == ts.SOUND_ENC_PCM16)
    for name, (pcm, _f) in signals().items():
        check("%s: the baker chooses the schema's encoding and bytes" % name,
              js_choose(pcm) == ts.sound_encode(pcm))
    check("3.9:1 against PCM16 for a whole block",
          ts.sound_bytes(ts.SOUND_ENC_ADPCM, 256) * 3.8 < ts.sound_bytes(ts.SOUND_ENC_PCM16, 256))
    print("RESULT: %s" % ("pass" if not fails else "%d failed" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
