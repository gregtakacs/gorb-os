"""Accented Latin letters drawn as a base letter and a mark (radar-plan §6.22).

A theme font carries printable ASCII and a few symbols, so a name like Győr or Tatabánya
drew '?' where its accents were. Baking every accented letter costs ~220 glyphs a font;
instead a font carries a dotless i and fourteen marks, each in a lower and an upper case
form, and the OS draws an accented letter as its base and its mark. A mark's cell is its
carrier letter accented, less the carrier: the accent alone, where it sits over (or under)
that letter. The OS places it over another base by the difference of their advances,
centred, or flush right for the ogonek, which hangs from a letter's right foot.

This file is the one list: theme_schema.FONT_CHARSET takes the dotless i and the marks
from it, tools/make_latin_table.py writes main/stage/orb_latin.h from it, and the bakers'
GorbBake.FONT_MORE is checked against it (tools/host_label_test.py)."""
import unicodedata

DOTLESS = "ı"
# combining mark, lower carrier, upper carrier, 'c'entred or 'r'ight
MARKS = (
    ("́", "o", "O", "c"),   # acute
    ("̀", "o", "O", "c"),   # grave
    ("̂", "o", "O", "c"),   # circumflex
    ("̃", "o", "O", "c"),   # tilde
    ("̈", "o", "O", "c"),   # diaeresis
    ("̊", "a", "A", "c"),   # ring
    ("̌", "c", "C", "c"),   # caron
    ("̆", "a", "A", "c"),   # breve
    ("̄", "o", "O", "c"),   # macron
    ("̋", "o", "O", "c"),   # double acute
    ("̇", "z", "Z", "c"),   # dot above
    ("̧", "c", "C", "c"),   # cedilla
    ("̨", "a", "A", "r"),   # ogonek
    ("̦", "s", "S", "c"),   # comma below
)
N = len(MARKS)
# The mark glyphs, lower case forms then upper: each its carrier accented
LOWER = tuple(unicodedata.normalize("NFC", car + m) for m, car, _, _ in MARKS)
UPPER = tuple(unicodedata.normalize("NFC", car + m) for m, _, car, _ in MARKS)
GLYPHS = LOWER + UPPER

# letters with no mark of these: the nearest plain letters
SPELL = {
    "ß": "ss", "æ": "ae", "Æ": "AE", "ø": "o", "Ø": "O", "đ": "d",
    "Đ": "D", "ł": "l", "Ł": "L", "ħ": "h", "Ħ": "H", "ŋ": "n",
    "Ŋ": "N", "œ": "oe", "Œ": "OE", "þ": "th", "Þ": "Th", "ð": "d",
    "Ð": "D", "ĸ": "k", "ŀ": "l", "Ŀ": "L", "ŉ": "n", "ſ": "s",
    "Ĳ": "IJ", "ĳ": "ij", " ": " ",
}

def table():
    """(codepoint, base, mark index or -1, second plain letter or '') for every letter of
    Latin-1, Latin Extended-A and the Romanian comma letters this list can draw."""
    out = []
    cps = list(range(0xA0, 0x180)) + [0x218, 0x219, 0x21A, 0x21B]
    combining = [m for m, _, _, _ in MARKS]
    for cp in cps:
        ch = chr(cp)
        if ch in SPELL:
            s = SPELL[ch]
            out.append((cp, s[0], -1, s[1:]))
            continue
        d = unicodedata.normalize("NFD", ch)
        if len(d) != 2 or not (0x20 <= ord(d[0]) < 0x7F) or d[1] not in combining:
            continue
        base, mark = d[0], combining.index(d[1])
        upper = base.isupper()
        if base == "i" and mark not in (11, 12, 13):   # a mark above takes the i's dot
            base = DOTLESS
        out.append((cp, base, mark + (N if upper else 0), ""))
    if "ı" not in [chr(c) for c, *_ in out]:
        out.append((0x131, DOTLESS, -1, ""))
    return sorted(out)
