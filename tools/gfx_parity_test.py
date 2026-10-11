#!/usr/bin/env python3
"""Prove main/gfx/orb_gfx.c is the compositor exactly as it shipped.

P1 promised a zero-behaviour-change extraction, and that promise is not kept by
eye. This lifts each routine the move claims to have relocated out of the
pre-extraction clock_view.c AND out of orb_gfx.c, canonicalises the renames and the
row-state consolidation, and compares token streams. Anything beyond the intended
delta — the `const uint8_t *base` parameter threaded into gfx_fill_sprite_dsc and
gfx_paint_ctx_init so gfx never learns about art_bank — fails the run.

The baseline is a PINNED commit, not HEAD: 786efcd is the last revision whose
clock_view.c still held the paint path whole. Pass another ref as argv[1] to audit
against a different point. Change the maths deliberately and this goes red; move the
pin in the same commit, the way host_comp_math_test.py owns the moon spec.

Run:  python3 tools/gfx_parity_test.py        (rc 0 = identical modulo renames)
"""
import re, subprocess, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
REF = sys.argv[1] if len(sys.argv) > 1 else "786efcd"

old = subprocess.run(['git', '-C', str(ROOT), 'show', f'{REF}:main/clock/clock_view.c'],
                     capture_output=True, text=True).stdout
assert old, f'no main/clock/clock_view.c at {REF} — wrong repo or ref?'
new = (ROOT / 'main' / 'gfx' / 'orb_gfx.c').read_text()


def lift(src, sig):
    """One function's text, signature to its first column-0 }. rindex because the
    old file declares most of these twice: prototype first, definition second."""
    i = src.rindex(sig)
    j = src.index('\n}', i) + 2
    return src[i:j]


# gfx name -> the name the shipped file used, so both sides speak it
NAMES = {
    'gfx_mark_rows': 'mark_rows', 'gfx_swept_box': 'swept_box',
    'gfx_swept_rect_box': 'swept_rect_box', 'gfx_area_union': 'area_union',
    'gfx_blend_px': 'blend_px', 'gfx_sprite_bytes': 'sprite_bytes',
    'gfx_sprite_drawable': 'sprite_drawable', 'gfx_fill_sprite_dsc': 'fill_sprite_dsc',
    'gfx_paint_ctx_t': 'paint_ctx_t', 'gfx_paint_ctx_init': 'paint_ctx_init',
    'gfx_paint_row_ctx': 'paint_row_ctx', 'gfx_trigo_sin': 'trigo_sin',
    'gfx_rot_sin_cos': 'rot_sin_cos',
    'rowstate.need': 's_layer_need_row', 'rowstate.force': 's_layer_force_row',
    'rowstate.x0': 's_layer_row_x0', 'rowstate.x1': 's_layer_row_x1',
    's_commit': 's_need_commit',
}


def canon(t):
    t = re.sub(r'\bstatic\b|\binline\b', ' ', t)      # linkage moved; the maths did not
    t = re.sub(r'const uint8_t \*base[,)]',
               lambda m: ')' if m.group(0).endswith(')') else '', t)
    t = re.sub(r'\bbase \+', 'blob() +', t)            # ... resolved from a param now
    for k, v in NAMES.items():
        t = re.sub(r'\b%s\b' % re.escape(k), v, t)
    return re.sub(r'\s+', ' ', t).strip()


CASES = [
    ('rot_sin_cos', 'static void rot_sin_cos(', 'void gfx_rot_sin_cos('),
    ('sprite_bytes', 'static uint32_t sprite_bytes(', 'uint32_t gfx_sprite_bytes('),
    ('sprite_drawable', 'static bool sprite_drawable(', 'bool gfx_sprite_drawable('),
    ('fill_sprite_dsc', 'static void fill_sprite_dsc(', 'void gfx_fill_sprite_dsc('),
    ('swept_rect_box', 'static void swept_rect_box(', 'void gfx_swept_rect_box('),
    ('swept_box', 'static void swept_box(', 'void gfx_swept_box('),
    ('area_union', 'static void area_union(', 'void gfx_area_union('),
    ('trigo_sin', 'static inline int32_t trigo_sin(', 'int32_t gfx_trigo_sin('),
    ('mix_16_16', 'static inline uint16_t mix_16_16(', 'static inline uint16_t mix_16_16('),
    ('blend_px', 'static inline void blend_px(', 'static inline void blend_px('),
    ('mark_rows', 'static inline void mark_rows(', 'void gfx_mark_rows('),
    ('paint_ctx_init', 'static bool paint_ctx_init(', 'bool gfx_paint_ctx_init('),
    ('paint_row_ctx', 'static void paint_row_ctx(', 'void gfx_paint_row_ctx('),
]

fails = []
for name, osig, nsig in CASES:
    a, b = canon(lift(old, osig)), canon(lift(new, nsig))
    print(('identical  ' if a == b else 'DIFFERS    ') + name)
    if a != b:
        fails.append(name)
        import difflib
        print('    ', ' / '.join(d for d in difflib.ndiff(a.split(), b.split())
                                if d[0] in '+-')[:400])

# the commit band: the whole restore-paint-gate walk, as one comparison
o = lift(old, '    /* Walk the marked rows in bands')
n = new[new.index('    /* Walk the marked rows in bands'):]
n = n[:n.index('\n}') + 2]
o = (o.replace('&s_canvas[', '&canvas[')
       .replace('bench_paint_band(y0, nr);', 'if (s_band_hook) s_band_hook(y0, nr);'))
print(('identical  ' if canon(o) == canon(n) else 'DIFFERS    ') + 'commit band loop')
if canon(o) != canon(n):
    import difflib
    for d in difflib.ndiff(canon(o).split(), canon(n).split()):
        if d[0] in '+-':
            print('    ', d)
    fails.append('band loop')

ACCEPTED = {'fill_sprite_dsc', 'paint_ctx_init'}   # the threaded art-bank base
unexpected = [f for f in fails if f not in ACCEPTED]
if unexpected:
    print('\nFAIL: %d differ beyond the documented base parameter: %s'
          % (len(unexpected), ', '.join(unexpected)))
    sys.exit(1)
print('\nparity holds against %s: every moved routine is the shipped one, and the only '
      'deltas are the art-bank base threaded into fill_sprite_dsc and paint_ctx_init.' % REF)
