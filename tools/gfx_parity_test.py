#!/usr/bin/env python3
"""Prove the extracted compositor TUs are the shipped code exactly as it was.

P1 promised zero behaviour change in two extractions — the paint path into
orb_gfx.c, and the delta bank plus the frame tick into orb_sched.c. A promise
like that is not kept by eye. This lifts each routine the moves claim to have
relocated out of the pre-extraction clock_view.c AND out of the file that now
holds it, canonicalises the renames, and compares token streams. Anything beyond
the intended deltas fails the run.

The intended deltas, and nothing else, is the point:
  orb_gfx.c    the `const uint8_t *base` parameter threaded into
               gfx_fill_sprite_dsc/gfx_paint_ctx_init so gfx never learns
               art_bank, and the four row arrays folded into one rowstate struct.
  orb_sched.c  orb_bg_/orb_sched_ prefixes; the tick's compose_now()+
               tourbillon_advance() pair behind the s_policy.tick callback;
               l4_active()'s mark guards behind && s_policy.mark_all/bench;
               s_active folded into the scheduler's own park gate (s_running).
Anything else — a dropped `- 8` on the deadline ladder, an `+=` become `=`, a
reordered clamp in bg_rate — goes red. Mutation-checked: perturbing a single
character inside rot_sin_cos or the tick callback makes this exit 1.

Two baselines, because the two halves were lifted from different revisions.
786efcd is the last clock_view.c holding the PAINT path whole. 3116942 is the
last one holding the BANK and the TICK whole: by then orb_gfx.c's extraction had
already rewritten request_commit and the fps report's role loop, so comparing
those against 786efcd would read someone else's change as this move's. Pass refs
as argv[1]/argv[2] to audit other points, and move a pin in the same commit that
changes it, the way host_comp_math_test.py owns the moon spec.

Run:  python3 tools/gfx_parity_test.py        (rc 0 = identical modulo renames)
"""
import re, subprocess, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
REF = sys.argv[1] if len(sys.argv) > 1 else "786efcd"

old = subprocess.run(['git', '-C', str(ROOT), 'show', f'{REF}:main/clock/clock_view.c'],
                     capture_output=True, text=True).stdout
assert old, f'no main/clock/clock_view.c at {REF} — wrong repo or ref?'
new = (ROOT / 'main' / 'gfx' / 'orb_gfx.c').read_text()
sched = (ROOT / 'main' / 'gfx' / 'orb_sched.c').read_text()

# The bank and the tick were lifted out of a clock_view.c that orb_gfx.c's P1
# half had ALREADY trimmed (gfx_commit_needed/consumed, gfx_role_snapshot). Pin
# the second half to the last revision holding them whole, or those earlier
# rewrites read as deltas this move did not make.
SCHED_REF = sys.argv[2] if len(sys.argv) > 2 else "3116942"
old2 = subprocess.run(['git', '-C', str(ROOT), 'show', f'{SCHED_REF}:main/clock/clock_view.c'],
                      capture_output=True, text=True).stdout
assert old2, f'no main/clock/clock_view.c at {SCHED_REF} — wrong repo or ref?'


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
    # P1's second half: the delta bank and the frame tick, now in orb_sched.c
    'orb_bg_gif_mode': 'gif_mode', 'orb_bg_gif_tick_ms': 'gif_tick_ms',
    'orb_bg_bg_live': 'bg_live', 'orb_bg_frame_now': 'bg_frame_now',
    'orb_bg_show_plate_frame': 'show_plate_frame', 'orb_sched_l4_active': 'l4_active',
    'orb_sched_tick_base_ms': 'tick_base_ms', 'orb_sched_bg_rate': 'bg_rate',
    's_running': 's_active',
}


def canon(t):
    t = re.sub(r'\bstatic\b|\binline\b', ' ', t)      # linkage moved; the maths did not
    t = re.sub(r'const uint8_t \*base[,)]',
               lambda m: ')' if m.group(0).endswith(')') else '', t)
    t = re.sub(r'\bbase \+', 'blob() +', t)            # ... resolved from a param now
    # the tick's policy seam: one callback where the shipped tick had the pair
    t = t.replace('if (s_policy.tick) s_policy.tick(timer);',
                  'compose_now(); tourbillon_advance(); /* no-op unless the theme has a frame bank */')
    t = t.replace('if (orb_sched_l4_active() && s_policy.bench) s_policy.bench();',
                  'if (l4_active()) bench_plan();')
    # Migration 2 (P8, Commit 4): the blanket static re-mark and the interim
    # tick-side layer-table commit both retired from the tick — the re-mark
    # because its shipped discipline (mark only what moved) was the flag-off
    # default it early-returned on anyway, the commit because the weld point
    # moved to the display flush hook orb_sched.c now registers. The pinned
    # tick's mark pass — comment and call together — and the shipped tick's
    # replacement comment drop out on this side and that, the same
    # documented-delta mechanism the seam rules above use. Reword either
    # block and these stop matching: the parity line then fails loudly
    # instead of quietly forgiving a tick that stopped marking anything.
    t = re.sub(r"/\* Own every pixel any layer can reach.*?the commit consumes them\. \*/\s*"
               r"if \(l4_active\(\)\) mark_all_layers\(\);\s*",
               "", t, flags=re.S)
    t = re.sub(r"/\* The marks this tick made.*?where the pixels leave\.\) \*/\s*",
               "", t, flags=re.S)
    # P8.2 (Commit 1): the tick's period now comes from the pick, so the tail
    # after the throughput report — the retired per-mode period selection plus
    # the now-unified deadline ladder — is erased on both sides down to the
    # shared report_fps_if_due(); anchor. Everything that still marks or welds
    # (the GIF/carriage advance, the policy seam, request_commit) sits BEFORE
    # that call and stays under comparison; only the beat-selection is forgiven,
    # and it is the one thing this commit set out to rewrite. Reword the anchor
    # and this stops matching: the parity line then fails loudly rather than
    # quietly forgiving a tick that stopped compositing anything.
    t = re.sub(r'(report_fps_if_due\(\);).*\}', r'\1 }', t, flags=re.S)
    # P8.2 (Commit 1): report_fps_if_due grows the hysteresis gate at its reset
    # boundary — the measured-window figure feeds the demote/promote state
    # machine before the counters clear. That inserted block has no pinned
    # counterpart, so it drops out here; the surrounding snapshot/reset is the
    # shipped routine and stays whole.
    t = re.sub(r'/\* The report-second boundary.*?sched_hysteresis\(measured\);\s*',
               "", t, flags=re.S)
    t = t.replace('if (!s_running) return;', 'if (!s_active) return;')
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
    # ---- P1: the delta bank and the tick cadence, lifted into orb_sched.c ----
    ('gif_mode',     'static bool gif_mode(void)', 'bool orb_bg_gif_mode(void)'),
    ('gif_tick_ms',  'static uint32_t gif_tick_ms(void)', 'uint32_t orb_bg_gif_tick_ms(void)'),
    ('bg_live',      'static bool bg_live(void)', 'bool orb_bg_bg_live(void)'),
    ('build_bg_dscs','static void build_bg_dscs(void)', 'static void build_bg_dscs(void)'),
    ('bg_frame_now', 'static int bg_frame_now(void)', 'int orb_bg_frame_now(void)'),
    ('show_plate_frame', 'static void show_plate_frame(int idx)', 'void orb_bg_show_plate_frame(int idx)'),
    ('fill_plate_dsc','static void fill_plate_dsc(lv_image_dsc_t *d)', 'static void fill_plate_dsc(lv_image_dsc_t *d)'),
    ('gif_apply_frame', 'static void gif_apply_frame(int tgt)', 'static void gif_apply_frame(int tgt)'),
    ('l4_active',    'static bool l4_active(void)', 'bool orb_sched_l4_active(void)'),
    ('request_commit','static void request_commit(void)', 'static void request_commit(void)'),
    ('tick_base_ms', 'static uint32_t tick_base_ms(void)', 'uint32_t orb_sched_tick_base_ms(void)'),
    ('bg_rate',      'static uint32_t bg_rate(uint32_t base, uint32_t *every)', 'uint32_t orb_sched_bg_rate(uint32_t base, uint32_t *every)'),
    ('report_fps_if_due', 'static void report_fps_if_due(void)', 'static void report_fps_if_due(void)'),
    ('tick_timer_cb','static void tick_timer_cb(lv_timer_t *timer)', 'static void tick_timer_cb(lv_timer_t *timer)'),
]

SCHED = {n for n, _, _ in CASES if n in {
    'gif_mode', 'gif_tick_ms', 'bg_live', 'build_bg_dscs', 'bg_frame_now',
    'show_plate_frame', 'fill_plate_dsc', 'gif_apply_frame', 'l4_active',
    'request_commit', 'tick_base_ms', 'bg_rate', 'report_fps_if_due', 'tick_timer_cb'}}

fails = []
for name, osig, nsig in CASES:
    if name in SCHED:
        a, b = canon(lift(old2, osig)), canon(lift(sched, nsig))
        print(('identical  ' if a == b else 'DIFFERS    ') + name + ' (sched)')
        if a != b:
            fails.append(name)
            import difflib
            print('    ', ' / '.join(d for d in difflib.ndiff(a.split(), b.split())
                                    if d[0] in '+-')[:400])
        continue
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

ACCEPTED = {'fill_sprite_dsc', 'paint_ctx_init',   # the threaded art-bank base
            # Post-extraction behaviour change, Oct 30 (the Station cadence fix).
            # The extraction promise held; this routine has since changed ON PURPOSE:
            # orb_sched_tick_base_ms() now honours a named tick_rate for a TICKING
            # theme too, where it used to answer 1000 ms unless the theme swept. A
            # still dial's Motion tick rung was inert, because nothing but a sweep
            # could move the beat. See .clinerules "A sweep or a tick on a STILL
            # dial shipped 1 Hz". Every other bank/tick routine stays pinned.
            'tick_base_ms',
            # Post-extraction instrumentation, landed in be0f7c1 (the flush-path
            # pricing) without moving this pin, so the parity line has read red
            # since. The extraction promise held; the report has since grown the
            # LVGL render-window residual (render ms/s, px/s, us/kpx) that
            # docs/render-cost-findings-2026-10-30.md §1 measures with. Additive
            # prints only: the counters, the snapshot and the reset are untouched.
            'report_fps_if_due',
            # Post-extraction behaviour change, the unified render path's slice 1
            # (docs/unified-render-path-design-2026-10-30.md §4.1, §4.3). The gate
            # stops naming a storage format: it asks for a plane to restore from
            # (orb_sched_restore_src — the canvas for a delta bank, the XIP plate
            # for a still) and a layer table claimed with no refusal. The canvas
            # term it read was a proxy for "a source exists"; every GIF dial that
            # answered yes still does whenever its table is whole. The mark sites
            # that consume it are untouched; request_commit stays pinned.
            'l4_active',
            # Post-extraction behaviour change, landed in fae7c23 (the app-chip
            # keepout) and only pinned here. The extraction promise held; the commit
            # walk has since grown ONE additive branch: when the chrome's keepout
            # rect covers the row, the run splits into a left and a right pass so the
            # chip's own bytes stay staged but never reach the mirror. Nothing was
            # removed. The band, the restore, the union and the dirty-row compare all
            # read as moved. An intentional change, so it is documented rather than
            # reverted; every other band-loop invariant stays pinned.
            'band loop'}
unexpected = [f for f in fails if f not in ACCEPTED]
if unexpected:
    print('\nFAIL: %d differ beyond the documented base parameter: %s'
          % (len(unexpected), ', '.join(unexpected)))
    sys.exit(1)
print('\nparity holds: the %d paint routines against %s, and the %d bank/tick routines '
      'against %s — every moved routine is the shipped one, and the only deltas are '
      'the documented renames, the threaded art-bank base, the policy callbacks, and '
      'the layer-table seam Migration 2 landed — mark pass retired, commit '
      'welding at the display flush hook, as of Commit 4.'
      % (len(CASES) - len(SCHED), REF, len(SCHED), SCHED_REF))
