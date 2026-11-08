#!/usr/bin/env python3
"""Write main/stage/orb_latin.h from tools/latin_marks.py (radar-plan §6.22).

    python3 tools/make_latin_table.py          # write it
    python3 tools/make_latin_table.py --check  # fail when it trails the list"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import latin_marks as L
import theme_schema as ts

OUT = os.path.join(os.path.dirname(HERE), "main", "stage", "orb_latin.h")

def gi(ch):
    return ts.FONT_CHARSET.index(ch)

def text():
    rows = []
    for cp, base, mark, more in L.table():
        rows.append("    { 0x%04X, %3d, %3d, %3d },   /* %s */" % (
            cp, gi(base), -1 if mark < 0 else gi(L.GLYPHS[mark]), gi(more) if more else 255,
            chr(cp) if cp >= 0xC0 else "U+%04X" % cp))
    car = ", ".join("%d" % gi(c) for c in [m[1] for m in L.MARKS] + [m[2] for m in L.MARKS])
    right = ", ".join("1" if m[3] == "r" else "0" for m in L.MARKS * 1) + ", " + \
            ", ".join("1" if m[3] == "r" else "0" for m in L.MARKS)
    return """/* orb_latin.h — written by tools/make_latin_table.py from tools/latin_marks.py; do not edit.
 *
 * Each accented Latin letter the fonts can draw (radar-plan §6.22): its base glyph, its mark
 * glyph (-1 none) and a second plain glyph (255 none: ß is s s), all charset indices. A mark
 * glyph g sits over another base by the difference of their advances from its carrier
 * ORB_MARK_CARRIER[g - ORB_MARK0], centred, or flush right where ORB_MARK_RIGHT says. */
#pragma once
#include <stdint.h>

#define ORB_DOTLESS_I %d
#define ORB_MARK0     %d
#define ORB_MARKS     %d

typedef struct { uint16_t cp; uint8_t base; int16_t mark; uint8_t more; } orb_latin_t;

static const orb_latin_t ORB_LATIN[] = {
%s
};
#define ORB_LATIN_N (sizeof ORB_LATIN / sizeof ORB_LATIN[0])

static const uint8_t ORB_MARK_CARRIER[ORB_MARKS] = { %s };
static const uint8_t ORB_MARK_RIGHT[ORB_MARKS] = { %s };
""" % (gi(L.DOTLESS), gi(L.GLYPHS[0]), len(L.GLYPHS), "\n".join(rows), car, right)

if __name__ == "__main__":
    t = text()
    if "--check" in sys.argv:
        ok = os.path.exists(OUT) and open(OUT).read() == t
        print("orb_latin.h is current" if ok else "orb_latin.h trails tools/latin_marks.py: run tools/make_latin_table.py")
        sys.exit(0 if ok else 1)
    open(OUT, "w").write(t)
    print("wrote %s: %d letters" % (OUT, len(L.table())))
