#!/usr/bin/env python3
"""Host suite for main/stage/orb_label.c: a label's pixels (docs/theme-text-plan.md §2.3).

    python3 tools/host_label_test.py [--mutations]

The shipped orb_label.c compiles with gcc against a synthetic font: glyph g is a block of
ink (g % 5) + 1 columns wide in colour g, the space has none. The checks read the plane
back: advances, the UTF-8 map, alignment across and down, clipping to the padded box, and
the glyph laid over the panel in straight alpha. --mutations breaks each guarded line in a
private copy and expects a check to fail.
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "main", "stage", "orb_label.c")
INC = ["-I", os.path.join(ROOT, "main", "stage"), "-I", os.path.join(ROOT, "main", "art")]

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_label.h"

#define CW 6
#define CH 8
#define PW 60
#define PH 30
static uint8_t glyphs[CLK_FONT_GLYPHS * CW * CH * 3];
static uint8_t cover[CLK_FONT_GLYPHS * CW * CH];   /* the same font as coverage alone */
static uint8_t plane[PW * PH * 3], panel[PW * PH * 3];
static int fails, checks;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint16_t col(int x, int y) { return ((uint16_t *)plane)[y * PW + x]; }
static uint8_t alp(int x, int y) { return plane[PW * PH * 2 + y * PW + x]; }

static void font(orb_font_t *f, int n)
{
    memset(f, 0, sizeof *f);
    f->glyphs = glyphs; f->w = CW; f->h = CH; f->n = (uint16_t)n; f->space = 3; f->track = 1;
    for (int g = 0; g < CLK_FONT_GLYPHS; g++) {
        uint8_t *p = glyphs + g * CW * CH * 3;
        uint16_t *c = (uint16_t *)p;
        uint8_t *a = p + CW * CH * 2;
        if (g == 0) continue;                       /* the space: no ink */
        for (int y = 1; y < CH - 1; y++)
            for (int x = 0; x <= g % 5; x++) { c[y * CW + x] = (uint16_t)(0x0800 + g); a[y * CW + x] = 255; }
    }
}

/* The same shapes as font(), as OS v1.2.12 stores them: one coverage byte a pixel, the
 * colour the font row's, top c0 to foot c1. */
static void cover_font(orb_font_t *f, int n, uint16_t c0, uint16_t c1)
{
    memset(f, 0, sizeof *f);
    f->glyphs = cover; f->w = CW; f->h = CH; f->n = (uint16_t)n; f->space = 3; f->track = 1;
    f->bpp = 1; f->c0 = c0; f->c1 = c1;
    memset(cover, 0, sizeof cover);
    for (int g = 1; g < CLK_FONT_GLYPHS; g++)
        for (int y = 1; y < CH - 1; y++)
            for (int x = 0; x <= g % 5; x++) cover[g * CW * CH + y * CW + x] = 255;
}

/* the first column, left to right, where row y holds glyph colour */
static int ink_at(int y, int from) { for (int x = from; x < PW; x++) if (alp(x, y) == 255 && col(x, y) >= 0x0800) return x; return -1; }

int main(void)
{
    orb_font_t f;
    font(&f, CLK_FONT_GLYPHS);
    orb_font_measure(&f);
    CK(f.adv[0] == 3, "the space advances the space: %d", f.adv[0]);
    CK(f.adv['A' - 0x20] == ('A' - 0x20) % 5 + 1 + 1, "A advances its ink + 1 + tracking: %d", f.adv['A' - 0x20]);
    CK(f.adv['E' - 0x20] == ('E' - 0x20) % 5 + 2, "E advances its ink + 1 + tracking: %d", f.adv['E' - 0x20]);

    const char *s;
    s = "A"; CK(orb_glyph_next(&s, 100) == 'A' - 0x20 && !*s, "A maps to its ASCII cell");
    s = "\xe2\x86\x91"; CK(orb_glyph_next(&s, 100) == 95 && !*s, "the up arrow maps to 95");
    s = "\xe2\x86\x92"; CK(orb_glyph_next(&s, 100) == 97, "the right arrow maps to 97");
    s = "\xc2\xb0"; CK(orb_glyph_next(&s, 100) == 99 && !*s, "the degree sign maps to 99");
    s = "\xc3\xa9x"; CK(orb_glyph_next(&s, 100) == '?' - 0x20 && *s == 'x', "an unmapped letter is '?', and only it is eaten");
    s = "\xe2\x86\x91"; CK(orb_glyph_next(&s, 95) == '?' - 0x20, "an arrow a font of 95 lacks is '?'");
    s = "\t"; CK(orb_glyph_next(&s, 100) == 0, "a tab is a space");
    s = "\x7f"; CK(orb_glyph_next(&s, 100) == '?' - 0x20, "DEL is '?'");

    /* left, top, pad 2: 'A' at (2, 2), 'B' one advance on */
    memset(panel, 0, sizeof panel);
    orb_label_compose(plane, PW, PH, panel, &f, "AB", 2, 0, 0);
    const int ax = ink_at(3, 0);
    CK(ax == 2 && col(2, 3) == 0x0800 + ('A' - 0x20), "left: A starts at the padding (%d)", ax);
    const int bx = 2 + f.adv['A' - 0x20];
    CK(col(bx, 3) == 0x0800 + ('B' - 0x20) && alp(bx - 1, 3) == 0, "left: B one advance on (%d)", bx);
    CK(alp(2, 2) == 0 && alp(2, 3) == 255, "top: the glyph's own first row is blank, its ink at y 3");

    /* centre: (inner - width) / 2 from the padding */
    orb_label_compose(plane, PW, PH, panel, &f, "AB", 2, 1, 0);
    const int lw = f.adv['A' - 0x20] + f.adv['B' - 0x20], cx = 2 + ((PW - 4) - lw) / 2;
    CK(ink_at(3, 0) == cx, "centre: A at %d, want %d", ink_at(3, 0), cx);
    /* end: flush to the far padding */
    orb_label_compose(plane, PW, PH, panel, &f, "AB", 2, 2, 0);
    CK(ink_at(3, 0) == PW - 2 - lw, "end: A at %d, want %d", ink_at(3, 0), PW - 2 - lw);

    /* down: two lines in the middle, then at the bottom */
    orb_label_compose(plane, PW, PH, panel, &f, "A\nB", 2, 0, 1);
    const int my = 2 + ((PH - 4) - 2 * CH) / 2;
    CK(alp(2, my) == 0 && col(2, my + 1) == 0x0800 + ('A' - 0x20) && col(2, my + CH + 1) == 0x0800 + ('B' - 0x20),
       "middle: two lines from row %d", my);
    orb_label_compose(plane, PW, PH, panel, &f, "A", 2, 0, 2);
    CK(alp(2, PH - 2 - CH + 1) == 255 && alp(2, PH - 2 - CH) == 0, "bottom: the line ends at the padding");

    /* clipped to the padded box: a run of the widest glyph (G, 5 px of ink) never inks the
     * last two columns, nor a stack of lines the last two rows */
    orb_label_compose(plane, PW, PH, panel, &f, "GGGGGGGGGGGGGGGGGGGGGGGGGGGGGG\nG\nG\nG\nG", 2, 0, 0);
    int outside = 0;
    for (int y = 0; y < PH; y++) for (int x = 0; x < PW; x++)
        if ((x < 2 || x > PW - 3 || y < 2 || y > PH - 3) && alp(x, y)) outside++;
    CK(outside == 0, "clip: %d px inked outside the padded box", outside);

    /* over the panel: an opaque panel under a half glyph mixes; a clear one takes it */
    for (int i = 0; i < PW * PH; i++) { ((uint16_t *)panel)[i] = 0xF800; panel[PW * PH * 2 + i] = 255; }
    uint8_t *ga = glyphs + ('E' - 0x20) * CW * CH * 3 + CW * CH * 2;
    for (int i = 0; i < CW * CH; i++) if (ga[i]) ga[i] = 128;
    orb_label_compose(plane, PW, PH, panel, &f, "E", 0, 0, 0);
    const uint16_t m = col(0, 1);
    CK(alp(0, 1) == 255 && (m >> 11) > 0 && (m >> 11) < 31, "over: a half glyph on red mixes (%04x)", m);
    CK(col(20, 20) == 0xF800 && alp(20, 20) == 255, "over: the panel stands where no glyph is");
    memset(panel, 0, sizeof panel);
    orb_label_compose(plane, PW, PH, panel, &f, "E", 0, 0, 0);
    CK(alp(0, 1) == 128 && col(0, 1) == 0x0800 + ('E' - 0x20), "over: a clear panel takes the glyph whole (%d)", alp(0, 1));
    orb_label_compose(plane, PW, PH, NULL, NULL, "E", 0, 0, 0);
    CK(alp(0, 1) == 0, "no font: the panel alone");

    /* ---- coverage fonts (OS v1.2.12): the shapes the colour font has, coloured live ---- */
    orb_font_t c;
    cover_font(&c, CLK_FONT_GLYPHS, 0x07E0, 0x07E0);
    orb_font_measure(&c);
    CK(c.adv[0] == f.adv[0] && c.adv['A' - 0x20] == f.adv['A' - 0x20] && c.adv['E' - 0x20] == f.adv['E' - 0x20],
       "coverage: the advances are the colour font's (%d %d)", c.adv['A' - 0x20], c.adv['E' - 0x20]);
    memset(panel, 0, sizeof panel);
    orb_label_compose(plane, PW, PH, panel, &c, "AE", 0, 0, 0);
    const int ex = c.adv['A' - 0x20];
    CK(col(0, 1) == 0x07E0 && alp(0, 1) == 255 && col(ex, 6) == 0x07E0 && alp(ex, 6) == 255,
       "coverage: a flat font inks its colour at full cover (%04x %04x)", col(0, 1), col(ex, 6));
    CK(alp(0, 0) == 0 && alp(ex + ('E' - 0x20) % 5 + 1, 3) == 0,
       "coverage: no ink where the cell has none");
    CK(alp(('A' - 0x20) % 5, 1) == 255 && alp(ex + ('E' - 0x20) % 5, CH - 2) == 255,
       "coverage: each glyph inks to its last column, at its top and foot rows");
    cover_font(&c, CLK_FONT_GLYPHS, 0xF800, 0x001F);
    orb_label_compose(plane, PW, PH, panel, &c, "E", 0, 0, 0);
    const uint16_t top = col(0, 1), foot = col(0, CH - 2);
    CK((top >> 11) > (foot >> 11) && (top & 0x1F) < (foot & 0x1F),
       "coverage: a gradient runs from the top colour to the foot's (%04x -> %04x)", top, foot);
    CK((top >> 11) >= 24 && (foot & 0x1F) >= 24, "coverage: near the ends it is near the end colours");
    uint8_t *ca = cover + ('E' - 0x20) * CW * CH;
    for (int i = 0; i < CW * CH; i++) if (ca[i]) ca[i] = 128;
    cover_font(&c, CLK_FONT_GLYPHS, 0x07E0, 0x07E0);
    for (int i = 0; i < CW * CH; i++) if (cover[('E' - 0x20) * CW * CH + i]) cover[('E' - 0x20) * CW * CH + i] = 128;
    orb_label_compose(plane, PW, PH, panel, &c, "E", 0, 0, 0);
    CK(alp(0, 1) == 128 && col(0, 1) == 0x07E0, "coverage: half cover is half alpha in the font's colour (%d)", alp(0, 1));
    (void)ca;

    /* ---- sparse fonts (OS v1.2.17): the cells a font carries, in charset order ---- */
    {
        static uint8_t sp[3 * CW * CH];
        cover_font(&c, CLK_FONT_GLYPHS, 0x07E0, 0x07E0);   /* the shapes to copy from */
        const int keep[3] = { '1' - 0x20, '7' - 0x20, '?' - 0x20 };
        for (int k = 0; k < 3; k++) memcpy(sp + k * CW * CH, cover + keep[k] * CW * CH, CW * CH);
        orb_font_t s1;
        memset(&s1, 0, sizeof s1);
        s1.glyphs = sp; s1.w = CW; s1.h = CH; s1.n = CLK_FONT_GLYPHS; s1.space = 3; s1.track = 1;
        s1.bpp = 1; s1.c0 = s1.c1 = 0x07E0; s1.sparse = true;
        memset(s1.rank, ORB_GLYPH_NONE, sizeof s1.rank);
        for (int k = 0; k < 2; k++) s1.rank[keep[k]] = (uint8_t)k;
        orb_font_measure(&s1);
        CK(s1.adv[keep[0]] == keep[0] % 5 + 2 && s1.adv[keep[1]] == keep[1] % 5 + 2,
           "sparse: the advances of the glyphs it carries (%d %d)", s1.adv[keep[0]], s1.adv[keep[1]]);
        CK(orb_font_glyph(&s1, 'A' - 0x20) == -1 && orb_font_glyph(&s1, keep[1]) == keep[1],
           "sparse: a glyph it lacks, with no '?', draws nothing");
        memset(panel, 0, sizeof panel);
        orb_label_compose(plane, PW, PH, panel, &s1, "17A7", 0, 0, 0);
        const int x7 = s1.adv[keep[0]], xa = x7 + s1.adv[keep[1]], x72 = xa + s1.space;
        CK(alp(keep[0] % 5, 1) == 255 && alp(keep[0] % 5 + 1, 1) == 0, "sparse: '1' from its own cell");
        CK(alp(x7 + keep[1] % 5, 1) == 255 && alp(x7 + keep[1] % 5 + 1, 1) == 0, "sparse: '7' from the next cell");
        int gap = 0;
        for (int x = xa; x < x72; x++) gap += alp(x, 1) != 0;
        CK(gap == 0 && alp(x72, 1) == 255, "sparse: the lacking 'A' advances a space, blank, and '7' follows");
        s1.rank[keep[2]] = 2;                      /* now it carries '?' */
        s1.measured = false;
        orb_font_measure(&s1);
        CK(orb_font_glyph(&s1, 'A' - 0x20) == keep[2], "sparse: a glyph it lacks draws as '?' when it carries one");
        orb_label_compose(plane, PW, PH, panel, &s1, "A", 0, 0, 0);
        CK(alp(keep[2] % 5, 1) == 255 && alp(keep[2] % 5 + 1, 1) == 0, "sparse: the '?' from its third cell");
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
"""

MUTANTS = (
    ("a sparse font's rank", "const uint32_t cell = f->sparse ? f->rank[gi] : (uint32_t)gi;", "const uint32_t cell = (uint32_t)gi;"),
    ("a lacking glyph's '?'", "return carries(f, '?' - 0x20) ? '?' - 0x20 : -1;", "return -1;"),
    ("a lacking glyph's space", "if (gi < 0) { x += f->space; continue; }", "if (gi < 0) { continue; }"),
    ("the advance's +1", "right + 1 + f->track", "right + f->track"),
    ("the space's advance", "right < 0 ? f->space :", "right < 0 ? 0 :"),
    ("the up arrow", "case 0x2191: gi = CLK_FONT_ASCII + 0;", "case 0x2191: gi = CLK_FONT_ASCII + 1;"),
    ("the font's own count", "return gi < n ? gi : '?' - 0x20;", "return gi;"),
    ("the tab", "if (cp == '\\t') cp = ' ';", "if (0) cp = ' ';"),
    ("the centre", "halign == 1 ? x0 + ((x1 - x0 + 1) - lw) / 2", "halign == 1 ? x0 + ((x1 - x0 + 1) - lw) / 3"),
    ("the end", "halign == 2 ? x1 + 1 - lw : x0", "halign == 2 ? x1 - lw : x0"),
    ("the middle", "valign == 1 ? y0 + ((y1 - y0 + 1) - text_h) / 2", "valign == 1 ? y0"),
    ("the clip across", "if (px < x0 || px > x1) continue;", "if (px < x0) continue;"),
    ("the clip down", "if (py < y0 || py > y1) continue;", "if (py < y0) continue;"),
    ("the line split", "lines += *p == '\\n';", "lines += 0;"),
    ("the mix", "pa[di] = (uint8_t)oa;", "pa[di] = (uint8_t)a;"),
    ("a coverage glyph's stride", "(f->bpp == 1 ? 1u : 3u)", "3u"),
    ("a coverage glyph's alpha", "const uint8_t *ga = cov ? g :", "const uint8_t *ga = cov ? g + 1 :"),
    ("the gradient's direction", "const uint32_t t = (uint32_t)y * 256u / (uint32_t)(f->h - 1), u = 256u - t;",
     "const uint32_t u = (uint32_t)y * 256u / (uint32_t)(f->h - 1), t = 256u - u;"),
    ("the live colour", "const uint16_t c = cov ? rc : gc[si];", "const uint16_t c = gc[si];"),
)


def build_and_run(src_text):
    d = tempfile.mkdtemp(prefix="label.")
    src = os.path.join(d, "orb_label.c")
    with open(src, "w") as f:
        f.write(src_text)
    h = os.path.join(d, "harness.c")
    with open(h, "w") as f:
        f.write(HARNESS)
    exe = os.path.join(d, "t")
    c = subprocess.run(["gcc", "-std=gnu11", "-O1", "-Wall", "-Werror"] + INC + [src, h, "-o", exe],
                       capture_output=True, text=True)
    if c.returncode:
        return None, c.stderr
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout


def main():
    text = open(SRC).read()
    rc, out = build_and_run(text)
    if rc is None:
        print("COMPILE FAILED\n" + out)
        return 1
    print(out.strip())
    bad = rc != 0
    if "--mutations" in sys.argv:
        caught = 0
        for name, a, b in MUTANTS:
            if text.count(a) != 1:
                print("FAIL mutant anchor %r: found %d times" % (name, text.count(a)))
                bad = True
                continue
            mrc, mout = build_and_run(text.replace(a, b))
            if mrc == 0:
                print("FAIL uncaught mutant: %s" % name)
                bad = True
            else:
                caught += 1
        print("mutants: %d of %d caught" % (caught, len(MUTANTS)))
    print("ALL PASS" if not bad else "FAILED")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
