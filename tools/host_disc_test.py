#!/usr/bin/env python3
"""Host bench for a plate stored as the panel's disc (main/art/orb_disc.h, OS v1.2.12).

The spans are never stored: the baker, the schema and the device each derive them from
one integer rule, so the three are held to the same answer here, row for row.

- orb_disc.h, compiled as it stands with host gcc -Werror: its spans and byte count
  equal theme_schema.disc_span / disc_bytes, and orb_disc_expand of a packed plane is
  the plane with its corners black;
- orbtheme-baker.js (node): discSpan, discBytes and discPack agree with the schema;
- the rule keeps the panel: every pixel whose centre lies within the 233 px panel radius
  is in, the disc is symmetric, and the saving is about a fifth.

Run:  python3 tools/host_disc_test.py              (rc 0 = pass)
      python3 tools/host_disc_test.py --mutations  (proves the checks bite)
"""
import json, os, random, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import theme_schema as ts  # noqa: E402

HDR = os.path.join(ROOT, "main", "art", "orb_disc.h")
BAKER = os.path.join(ROOT, "tools", "orb-manager", "web", "runtime", "baker@1", "orbtheme-baker.js")
NODE = shutil.which("node") or os.path.expanduser("~/.local/share/nodeenv-gorb/bin/node")
S = ts.DISC_SIDE

MUTATIONS = [
    ("const int32_t dy = 2 * (int32_t)y + 1 - (int32_t)side;",
     "const int32_t dy = 2 * (int32_t)y - (int32_t)side;"),
    ("const int64_t r = (int64_t)side / 2 + 1;", "const int64_t r = (int64_t)side / 2;"),
    ("*x1 = side - 1u - x;", "*x1 = side - x;"),
    ("memset(row, 0, x0 * 2u);\n        memcpy", ";\n        memcpy"),
    ("disc += n * 2u;", "disc += n;"),
    ("const uint8_t *a = disc + orb_disc_px(side) * 2u;", "const uint8_t *a = disc + orb_disc_px(side);"),
    ("for (uint32_t y = side; y-- > 0;) {", "for (uint32_t y = 0; y < side; y++) {"),
    ("memmove(row + x0 * 2u, buf + at, n);\n        memset(row, 0, x0 * 2u);",
     "memset(row, 0, x0 * 2u);\n        memmove(row + x0 * 2u, buf + at, n);"),
]

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
static uint16_t sq[ORB_DISC_SIDE * ORB_DISC_SIDE];
int main(void)
{
    uint32_t x0, x1;
    for (uint32_t y = 0; y < ORB_DISC_SIDE; y++) { orb_disc_span(y, &x0, &x1); printf("%u %u\n", x0, x1); }
    printf("%u\n", orb_disc_bytes());
    static uint8_t disc[ORB_DISC_SIDE * ORB_DISC_SIDE * 2];
    const size_t n = fread(disc, 1, sizeof disc, stdin);
    for (uint32_t i = 0; i < ORB_DISC_SIDE * ORB_DISC_SIDE; i++) sq[i] = 0xABCD;   /* stale */
    orb_disc_expand(disc, sq);
    fwrite(sq, 2, ORB_DISC_SIDE * ORB_DISC_SIDE, stderr);
    /* a 232 px preview: its spans, and the disc expanded in place at the buffer's head */
    for (uint32_t y = 0; y < 232; y++) { orb_disc_span_n(232, y, &x0, &x1); printf("p %u %u\n", x0, x1); }
    static uint8_t pv[232 * 232 * 2];
    const uint32_t pn = orb_disc_px(232) * 2u;
    for (uint32_t i = 0; i < sizeof pv; i++) pv[i] = 0x5A;                 /* stale */
    for (uint32_t i = 0; i < pn; i++) pv[i] = (uint8_t)(i * 7u + 3u);
    orb_disc_expand_inplace(pv, 232);
    fwrite(pv, 1, sizeof pv, stderr);
    /* the glass: a masked disc into an RGB565A8 square */
    static uint8_t gm[ORB_DISC_SIDE * ORB_DISC_SIDE * 3];
    const uint32_t gp = orb_disc_px(ORB_DISC_SIDE);
    static uint8_t gd[ORB_DISC_SIDE * ORB_DISC_SIDE * 3];
    for (uint32_t i = 0; i < gp * 3u; i++) gd[i] = (uint8_t)(i * 13u + 1u);
    memset(gm, 0x77, sizeof gm);
    orb_disc_expand_masked(gd, gm, ORB_DISC_SIDE);
    fwrite(gm, 1, sizeof gm, stderr);
    (void)n;
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
    c, exe = os.path.join(d, "d.c"), os.path.join(d, "d")
    open(c, "w").write(src.replace("#pragma once", "") + HARNESS)
    r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-o", exe, c],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("compile failed:\n" + r.stderr)
    return exe


def square():
    r = random.Random(3)
    return bytes(r.getrandbits(8) for _ in range(S * S * 2))


def c_run(exe, disc):
    p = subprocess.run([exe], input=disc, capture_output=True)
    lines = p.stdout.decode().split("\n")
    spans = [tuple(int(v) for v in lines[y].split()) for y in range(S)]
    pspans = [tuple(int(v) for v in lines[S + 1 + y].split()[1:]) for y in range(232)]
    err = p.stderr
    return spans, int(lines[S]), err[:S * S * 2], pspans, err[S * S * 2:S * S * 2 + 232 * 232 * 2], \
        err[S * S * 2 + 232 * 232 * 2:]


def device(exe, quiet=False):
    sq = square()
    disc = ts.disc_pack(sq)
    spans, nbytes, out, pspans, pv, gm = c_run(exe, disc)
    want = [ts.disc_span(y) for y in range(S)]
    pn = ts.disc_px(232) * 2
    pdisc = bytes((i * 7 + 3) & 0xFF for i in range(pn))
    gp = ts.disc_px(S)
    gd = bytes((i * 13 + 1) & 0xFF for i in range(gp * 3))
    gm_want = ts.disc_expand(gd[:2 * gp]) + _expand_a8(gd[2 * gp:])
    corners_black = all(out[(y * S + x) * 2:(y * S + x) * 2 + 2] == b"\0\0"
                        for y in range(S) for x in (0, S - 1) if x < want[y][0] or x > want[y][1])
    inside = all(out[(y * S + x) * 2:(y * S + x) * 2 + 2] == sq[(y * S + x) * 2:(y * S + x) * 2 + 2]
                 for y in range(0, S, 7) for x in range(want[y][0], want[y][1] + 1, 3))
    res = [spans == want, nbytes == ts.disc_bytes() == len(disc), inside and corners_black,
           ts.disc_expand(disc) == out,
           pspans == [ts.disc_span(y, 232) for y in range(232)] and pv == ts.disc_expand(pdisc, 232),
           gm == gm_want]
    if not quiet:
        check("the device's spans are the schema's, row for row", res[0])
        check("the device's byte count is the schema's and the packed plane's (%d B)" % len(disc), res[1])
        check("expand restores every kept pixel and blacks the corners", res[2])
        check("the device's expand is the schema's, byte for byte", res[3])
        check("a 232 px preview: the device's spans are the schema's, and its in-place expand "
              "is the schema's square", res[4])
        check("the glass: a masked disc expands to its colour and its alpha, the corners clear",
              res[5])
    return 0 if all(res) else 1


def _expand_a8(a):
    out, at = bytearray(S * S), 0
    for y in range(S):
        x0, x1 = ts.disc_span(y)
        n = x1 - x0 + 1
        out[y * S + x0:y * S + x0 + n] = a[at:at + n]
        at += n
    return bytes(out)


def js():
    sq = square()[:S * S * 2]
    code = ("const G=require(%s);const fs=require('fs');const sq=new Uint8Array(fs.readFileSync(0));"
            "const sp=[];for(let y=0;y<%d;y++)sp.push(G.discSpan(y));"
            "process.stdout.write(JSON.stringify({sp:sp,n:G.discBytes(),"
            "d:Buffer.from(G.discPack(sq)).toString('hex')}));") % (json.dumps(BAKER), S)
    p = subprocess.run([NODE, "-e", code], input=sq, capture_output=True)
    if p.returncode:
        raise SystemExit("node: " + p.stderr.decode()[-400:])
    o = json.loads(p.stdout)
    check("the baker's spans are the schema's", [tuple(x) for x in o["sp"]] == [ts.disc_span(y) for y in range(S)])
    check("the baker's byte count is the schema's", o["n"] == ts.disc_bytes())
    check("the baker packs the bytes the schema packs", bytes.fromhex(o["d"]) == ts.disc_pack(sq))
    m = bytes(random.Random(5).getrandbits(8) for _ in range(S * S * 3))
    p2 = bytes(random.Random(6).getrandbits(8) for _ in range(128 * 128 * 2))
    code = ("const G=require(%s);const fs=require('fs');const b=new Uint8Array(fs.readFileSync(0));"
            "const m=b.subarray(0,%d),p=b.subarray(%d);process.stdout.write(JSON.stringify({"
            "m:Buffer.from(G.discPackMasked(m,%d)).toString('hex'),p:Buffer.from(G.discPack(p,128)).toString('hex')}));"
            ) % (json.dumps(BAKER), S * S * 3, S * S * 3, S)
    q = subprocess.run([NODE, "-e", code], input=m + p2, capture_output=True)
    o2 = json.loads(q.stdout)
    check("the baker packs a masked glass as the schema does", bytes.fromhex(o2["m"]) == ts.disc_pack_masked(m))
    check("the baker packs a 128 px preview as the schema does", bytes.fromhex(o2["p"]) == ts.disc_pack(p2, 128))


def rule():
    spans = [ts.disc_span(y) for y in range(S)]
    covers = all(x0 <= x <= x1 for y, (x0, x1) in enumerate(spans) for x in range(S)
                 if (2 * x + 1 - S) ** 2 + (2 * y + 1 - S) ** 2 <= (2 * 233) ** 2)
    check("every pixel within the panel's 233 px radius is kept", covers)
    check("the disc is symmetric top to bottom", all(spans[y] == spans[S - 1 - y] for y in range(S)))
    saving = 1 - ts.disc_bytes() / (S * S * 2)
    check("a disc saves about a fifth of a square plate (%.1f%%)" % (100 * saving), 0.19 < saving < 0.22)


def main():
    with tempfile.TemporaryDirectory() as d:
        if "--mutations" in sys.argv:
            caught = 0
            for m in MUTATIONS:
                try:
                    bit = device(build(d, m), quiet=True) != 0
                except (IndexError, ValueError):
                    bit = True            # the mutant broke the harness's own reading
                print(("bites " if bit else "MISSED") + "  " + m[0][:64])
                caught += bit
            print("%d of %d mutants caught" % (caught, len(MUTATIONS)))
            return 0 if caught == len(MUTATIONS) else 1
        device(build(d))
    js()
    rule()
    print("RESULT: %s" % ("pass" if not fails else "%d failed" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
