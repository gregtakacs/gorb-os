"""Watch Composer export-fps loop check.

The composer picks the export fps from the rows the animated geartrain changes
per tick (docs/rows-capacity-2026-10-10.md). That measurement is taken inside
the movement build, and the build is gated on mvKey() -- a key that carries
frames: this.F through opts(). Pre-patch, budgetStats was keyed on that same
_mk, so the measurement invalidated itself on every pass: build publishes stats
for _mk, budget() then caps fps, opts().frames moves, mvKey() != _mk,
componentDidUpdate rebuilds, _mk is republished for the new frame count, the
published stats key no longer matches, budget() reports the uncapped want, and
the frame count snaps back. Every pass re-renders all frames, and while a pass
is in flight the per-frame setState repaints the readout at the uncapped rate
even though the movement is about to be published capped. That is the flicker
and the walking fps number, and it leaves the frame bank a different length from
the fps the manifest exports (prep_watch_kit.py requires fps == frames).

The batch-7 occlusion cull (--bg-occlude) adds a second failure mode this file
guards: the rows now depend on the DIAL as well, because the texels an opaque
static covers at alpha 255 never reach the panel. The frames themselves do not
depend on the dial, so a skeleton-slider move must re-PRICE the rows against the
cached frames (remeasureRows) instead of re-rendering the train -- which is what
the original flicker was. That puts the measurement validity on the movement
signature (stats['mv']), never on a key the build itself can move, and makes a
dial edit that does not change the tempo a zero-build event.

A third failure mode lives in the JS source rather than in the key semantics:
`get F()` returns budget().fps, and budget() keys itself on mvKey(1) -- which
runs through opts(), where `frames: this.F` lives. Building the full opts() and
deleting the field afterwards still EVALUATES the getter, so budget -> mvKey ->
opts -> F -> budget folded into each other and the page died with "maximum call
stack size exceeded" before the first render. opts() therefore takes the
noframes flag and never reads the getter on that path, `get F()` throws if a
budget is already on the stack, and source_cycle_check() asserts both shapes.

Run: python3 tools/bench/loopcheck.py   (exit 1 on regression)"""
import os

CAPS = {False: {15: 96, 12: 160, 8: 304}, True: {15: 64, 12: 96, 8: 240}}
# The baker holds the carriage bank at CLK_TOURB_MAX frames (bake_clock.py), so a
# carriage kit may not sit at a tempo whose bank is deeper than that. Only the
# 12 fps tier (BANKS[12]=24) busts it; the carriage is otherwise priced by rows
# like every other mover (baseImgs()/rowPrice()), so an exposed carriage still
# costs rows while one behind an opaque dial seals to ~0 and earns the top tier.
# The JS ladder mirrors this with MAXBANK.
CLK_TOURB_MAX = 16
# Bank depth per tempo, read from dial-renderer.js (BANKS) so the model cannot
# drift from the component. The bank may be deeper than the tempo: the device
# advances exactly one bank frame per tick, so a deeper bank plays the same
# authored loop more slowly. Everything that counts FRAMES uses this; everything
# that PACEs them, and the rows/tick the tier ladder compares, uses the tempo.
_bank_src = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         '..', 'watch-composer', 'dial-renderer.js')
def _load_banks():
    import re
    try:
        dr = open(_bank_src, encoding='utf-8').read()
    except OSError:
        return {4: 8, 8: 16, 12: 24, 15: 15}
    m = re.search(r"const BANKS = \{([^}]*)\}", dr)
    if not m:
        return {4: 8, 8: 16, 12: 24, 15: 15}
    return {int(k): int(v) for k, v in re.findall(r"(\d+):\s*(\d+)", m.group(1))}
BANKS = _load_banks()
def bank_frames(fps):
    return BANKS.get(fps, fps)
# rows/tick measured at each candidate tempo, keyed by the frames rendered.
MONO = {15: 60, 12: 75, 8: 110, 4: 190}          # narrow skeleton openings
WIDE = {15: 300, 12: 340, 8: 380, 4: 420}        # wide openings
NEEDLE = {15: 130, 12: 150, 8: 175, 4: 230}      # running seconds + geartrain
JITTER = {15: 200, 12: 170, 8: 150, 4: 200}      # anti-aliasing picks a worse pair
FROZEN = {15: 0, 12: 0, 8: 0, 4: 0}
TICKY = {15: 150, 12: 160, 8: 200, 4: 250}
# What fraction of the train the dial leaves uncovered, most open first. The
# cull is per texel and one layer must cover alone, so this is the honest shape
# of the number rather than a smooth knob: closed dial -> only the openings.
DIALS = {'openwork': 1.0, 'sector': 0.40, 'crescent': 0.20, 'closed': 0.05}


class Base:
    def __init__(s, needle=False, tick='sweep', rows=MONO, dial='openwork', car=False):
        s.needle, s.tick, s.rows_map, s.dial, s.car = needle, tick, rows, dial, car
        s._mk, s.stats, s.builds, s.seq = None, None, 0, []
        # `rendered` is how many PNGs the bank holds; `rendered_fps` is the TEMPO
        # those frames were authored for. They differ by design, and the rows table
        # is keyed by tempo, so a check that confuses them reads the wrong price.
        s.rendered, s.rendered_fps = None, None
        s.passes = []
        s._bG = False                     # JS _bG: set while budget() runs
        s.occl = 0                        # JS _occKey: bump when the occluder mask is retaken
        s._hbFps = None                     # JS: the last tempo budget() delivered
        s._bi = None                       # cached gear frames, keyed (mv, F)

    def mvart(s):
        # The MOVEMENT art only: the dial must never appear here, or a dial edit
        # would invalidate the cached frames it cannot change.
        return ('art', s.needle, s.tick, tuple(sorted(s.rows_map.items())))

    def art(s):
        return (s.mvart(), s.dial)

    def artKey(s):
        return repr(s.art())

    def mvKey(s, noframes=0):
        # frames: this.B in opts() -- the bank, not the tempo
        return repr(s.mvart() if noframes else (s.mvart(), ('frames', s.B())))

    def B(s):
        # Mirrors `get B()` in the component: the bank depth for the delivered tempo.
        return bank_frames(s.F())

    def measured(s, f):
        return int(round(s.rows_map[f] * DIALS[s.dial]))

    def tier(s, r, want):
        K = CAPS[s.needle]
        for t in (15, 12, 8):
            if t > want:
                continue
            if getattr(s, 'car', False) and bank_frames(t) > CLK_TOURB_MAX:
                continue            # carriage bank too deep for the baker (the 12 fps tier)
            if r <= K[t]:
                return t
        return 4

    def budget(s, forceWant=None):
        raise NotImplementedError

    def F(s):
        # Mirrors the component's `get F()`, which throws for the same reason:
        # budget() builds the movement key through opts(), and opts() carries
        # frames: this.F. A budget that reads the tempo it is deciding on folds
        # the two calls into each other -- in the browser that is "maximum call
        # stack size exceeded" on load, so the model refuses it here instead.
        if s._bG:
            raise AssertionError('budget() read this.F: the key it builds must not depend on the export frame count')
        return s.budget()['fps']

    def budget(s, forceWant=None):
        s._bG = True
        try:
            return s._budget(forceWant)
        finally:
            s._bG = False

    def render(s, f):
        # JS buildMovement reads the tempo F and the bank B from one budget(), then
        # renders B frames and publishes the price under f: F. `f` is the TEMPO.
        s.rendered_fps = f
        s.rendered = bank_frames(f)         # JS: this.gt / _bi hold B frames
        s.stats = dict(key=s.stats_key(), mv=s.mvKey(1), ak=s.artKey(), f=f, rows=s.measured(f), occ=s.occl)
        s._bi = (s.mvKey(1), s.rendered)    # JS: this._bi / _biKey cache of gear frames

    def build(s):
        raise NotImplementedError

    def upd(s):
        raise NotImplementedError


class PrePatch(Base):
    """budgetStats keyed on _mk; unbounded rebuilds from componentDidUpdate."""

    def stats_key(s):
        return s._mk

    def measured(s, f):
        return s.rows_map[f]             # no cull: the dial hides nothing here

    def _budget(s, forceWant=None):
        want = 15 if s.tick == 'sweep' else 12
        st = s.stats if (s.stats and s.stats['key'] == s._mk) else None
        if st is None:
            return {'fps': want, 'rows': -1}
        return {'fps': s.tier(st['rows'], want), 'rows': st['rows']}

    def build(s):
        s.builds += 1
        before = s.F()                      # the rate the readout still shows
        s._mk = s.mvKey()                   # JS: this._mk = this.mvKey()
        s.render(s.F())                     # JS: const F = this.F, read after _mk moved
        s.passes.append((before, s.rendered_fps, s.F()))

    def upd(s):
        if s._mk != s.mvKey():
            s.build()
        s.seq.append(s.F())


class PostPatch(Base):
    """budgetStats keyed on the movement signature, _ftier clamp, three-pass cap,
    and a dial-only change that re-prices rows instead of re-rendering gears."""

    def __init__(s, *a, **k):
        pm = k.pop('price_ms', 1)           # 1 = every commit may price (no throttle)
        Base.__init__(s, *a, **k)
        s._artKey, s._ftier, s._ftierKey, s._bk, s._bp = None, 15, None, None, 0
        s._ftierOcc = None               # the occluder mask the current _ftier was priced against
        # virtual commit clock; PRICE_MS is the JS throttle window in commits
        s.clock, s._rmT, s._rmAt, s.prices, s.PRICE_MS = 0, None, None, 0, pm

    def stats_key(s):
        return s._artKey

    def _budget(s, forceWant=None):
        want = forceWant or (15 if s.tick == 'sweep' else 12)
        # Valid while the MOVEMENT signature holds (JS budgetStats.mv).
        mvk, ak = s.mvKey(1), s.artKey()
        st = s.stats if (s.stats and s.stats['mv'] == mvk) else None
        if st is not None and st['ak'] != ak:
            # Priced through the dial before the last edit: hold the delivered
            # tempo. Re-tiering these rows under the new art key is the trap --
            # _ftier is monotone downward, so the open dial's cap becomes the
            # closed dial's ceiling and the tempo is stuck until a gear edit.
            return {'fps': getattr(s, '_hbFps', None) or want, 'rows': st['rows']}
        if st is None:
            return {'fps': want, 'rows': -1}
        fps = s.tier(st['rows'], want)
        if not forceWant:
            fk = (ak, want)
            if s._ftierKey != fk:
                s._ftierKey, s._ftier, s._ftierOcc = fk, 15, None
            # The pin may only hold against the measurement that set it, so it is
            # released when the price is taken against a DIFFERENT occluder mask:
            # that is the one input that can silently land late (dial canvas still
            # being built, image upload arriving), and without a release one bad
            # first price holds a 0-row closed dial at 8 fps for the session, since
            # min(15, 8) is 8 forever. Rows alone cannot be the trigger -- a lower
            # count is often just fewer frames to differ over (JITTER) -- and a
            # lower tempo is not new information about the art.
            if s._ftierOcc is not None and st.get('occ') != s._ftierOcc:
                s._ftier = 15
            fps = min(fps, s._ftier)
            s._ftier = fps
            s._ftierOcc = st.get('occ')    # the mask THIS measurement was taken against
            s._hbFps = fps                 # the rate the frames on screen are at
        return {'fps': fps, 'rows': st['rows']}


    def remeasure(s):
        """JS remeasureRows(): re-price rows on the cached frames, no re-render.
        Advances _artKey itself, so the caller cannot quote an unpriced art key.
        The cache is keyed on the BANK (that is what _bi holds), the price is
        looked up by TEMPO (that is what the rows table and the ladder use)."""
        F = s.F()
        if s._bi != (s.mvKey(1), bank_frames(F)):
            return False
        ak = s.artKey()
        s.stats = dict(key=s.stats_key(), mv=s.mvKey(1), ak=ak, f=F, rows=s.measured(F), occ=s.occl)
        s._artKey = ak
        s.prices += 1
        return True

    def remeasureSoon(s):
        """JS remeasureSoon(): at most one price per PRICE_MS commits, with a
        trailing price so the LAST slider position always gets priced. 'pending'
        means the trailing price is in flight and budget() is holding the tempo."""
        if s._rmT is not None:
            return 'pending'
        if s.clock - (s._rmAt or 0) < s.PRICE_MS:
            s._rmT = s.PRICE_MS - (s.clock - (s._rmAt or 0))
            return 'pending'
        s._rmAt = s.clock
        return 'priced' if s.remeasure() else 'stale'

    def build(s):
        s.builds += 1
        before = s.F()
        s._mk = s.mvKey()
        s.render(s.F())
        s._artKey = s.stats['ak']          # JS: published only when the price landed
        s.passes.append((before, s.rendered_fps, s.F()))

    def _seatbelt_build(s):
        # JS componentDidUpdate's build gate: a fresh art key resets the pass
        # counter, and _bp stops a pathological dial from rebuilding forever.
        ak = s.artKey()
        if ak != s._bk:
            s._bk, s._bp = ak, 0
        if s._bp < 3:
            s._bp += 1
            s.build()

    def _advance(s):
        """One React commit: move the virtual clock, fire a due trailing price."""
        s.clock += 1
        if s._rmT is None:
            return
        s._rmT -= 1
        if s._rmT > 0:
            return
        s._rmT, s._rmAt = None, s.clock
        if not s.remeasure():
            s._seatbelt_build()
        elif s._mk != s.mvKey():
            s._seatbelt_build()               # trailing price bought the tempo back

    def upd(s):
        s._advance()
        if s._mk != s.mvKey():
            s._seatbelt_build()
        elif s._artKey != s.artKey():
            st = s.remeasureSoon()
            if st == 'priced' and s._mk != s.mvKey():
                # Closing the dial buys the tempo back, so the matching bank must
                # re-render in this SAME commit: leaving it shows one rate while the
                # frames on screen were authored for another.
                s._seatbelt_build()
            elif st == 'stale':
                s._seatbelt_build()                       # frames must be re-rendered
        s.seq.append(s.F())


def source_cycle_check(html=None):
    """Assert the shapes that keep budget() and `get F()` apart, straight off the
    shipped HTML.

    The key model above cannot see this bug: which opts() call is allowed to read
    the tempo is a property of the source text, not of the key semantics, and the
    failure it prevents is a load-time stack overflow rather than a wrong number.
    Returns the names of the checks that did not hold.
    """
    p = html or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             '..', 'watch-composer', 'Watch Composer.dc.html')
    src = open(p, encoding='utf-8').read()
    body = src.split('_budget(forceWant) {')[-1].split('\n  preset(')[0]
    checks = [
        ('get F() throws on a re-entrant budget', 'if (this._bG) throw' in src),
        ('budget() raises the flag around _budget',
         'this._bG = 1;' in src and 'return this._budget(forceWant);' in src
         and 'finally { this._bG = 0; }' in src),
        ('opts() reads the bank only behind noframes',
         'frames: noframes ? 0 : this.B' in src),
        ('the bank is a tempo, never a literal frame count',
         'get B() { return window.DialRenderer.bankFrames(this.F); }' in src),
        ('mvKey threads its noframes flag into opts()',
         'this.opts(null, noframes)' in src),
        ('nothing rebuilds the full opts() to key on it', 'this.opts();' not in src),
        ('_budget asks for the movement key, not the tempo',
         'this.mvKey(1)' in body and 'this.F' not in body and 'this._bG' not in body),
    ]
    print('=== source: this.F / budget() separation ===')
    for n, ok in checks:
        print('  %-48s %s' % (n, 'ok' if ok else 'FAIL'))
    return [n for n, ok in checks if not ok]


def source_syntax_check(html=None):
    """Parse every script the composer ships with a real JavaScript parser.

    The model above cannot see a broken edit because it never reads the shipped
    file. A splice that lands a `/*` at the end of a line already carrying a `//`
    comment leaves the block unopened, so its prose parses as code -- a load-time
    syntax error the browser reports while every check here stays green. The two
    mutants below are what stop this gate from passing vacuously.
    """
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'watch-composer')
    try:
        from tree_sitter import Language, Parser
        import tree_sitter_javascript as tsj
    except ImportError:
        print('=== source: shipped scripts parse ===')
        print('  SKIP: pip install tree-sitter tree-sitter-javascript')
        return []
    parser = Parser(Language(tsj.language()))

    def broken(code):
        return parser.parse(code.encode('utf-8')).root_node.has_error

    p = html or os.path.join(d, 'Watch Composer.dc.html')
    src = open(p, encoding='utf-8').read()
    tag = '<script type="text/x-dc" data-dc-script>'
    i = src.index(tag) + len(tag)
    inline = src[i:src.index('</script>', i)]
    units = [('composer inline script', inline)]
    for f in ('support.js', 'watch-engine.js', 'dial-renderer.js'):
        units.append((f, open(os.path.join(d, f), encoding='utf-8').read()))
    print('=== source: shipped scripts parse ===')
    out = [n for n, code in units if broken(code)]
    for n, code in units:
        print('  %-48s %s' % (n, 'FAIL: does not parse' if n in out else 'ok'))
    for n, m in (('mutant: /* glued onto a // line',
                  inline + '\n// note     /* x\n * prose that is now code\n'),
                 ('mutant: unclosed function header',
                  inline + '\nfunction __broken__( {\n')):
        held = broken(m)
        print('  %-48s %s' % (n, 'ok' if held else 'FAIL: gate cannot fail'))
        if not held:
            out.append(n)
    return out


def gear_marking_check(renderer=None, engine=None):
    """Assert every toothed wheel both CHANGES PIXELS and reads as turning FORWARD.

    Two distinct faults, and the first K = 7 build tested only the first:

      FROZEN -- a step of a whole number of pitches lands the rim back on itself,
        so a bank resolves fewer distinct images than it has frames. K = 4 at
        4 fps stepped exactly one tooth and the rim sat still while only the
        spokes crawled. K = 7, coprime with every bank length, fixes that, and
        it is what lets the teeth be as fine as they look best: tooth size
        follows the module and the module follows the pitch radius m * N / 2.

      REVERSED -- distinct frames are not unambiguous frames. An unmarked rim
        repeats every single pitch, so a tick turning s pitches with s > 0.5 is
        pixel-identical to one turning s - 1 the OTHER way, and the eye always
        takes the slower reading. Doubling the bank exposed this at 4 fps: seven
        teeth over an eight-frame bank is 0.875 of a tooth, which reads as
        -0.125, so the rim crawled backwards while the spokes (period K teeth)
        crawled correctly forward. That is the fault this gate was missing.

    So each wheel, feature and tier is judged on the period the renderer ACTUALLY
    drew -- one pitch unmarked, mp pitches with the index tooth in -- and the
    reading the eye takes is the one folded into +-period/2, which must be
    forward. Where an unmarked rim would read backward the mark must be engaged,
    and it must measurably break the one-pitch symmetry: a mark too faint to make
    the reverse image differ is decoration, not an index, and the wheel reads as
    running backwards whatever the arithmetic says.
    """
    import re
    from fractions import Fraction
    from math import ceil, cos, pi, sin, floor
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'watch-composer')
    dr = open(renderer or os.path.join(d, 'dial-renderer.js'), encoding='utf-8').read()
    en = open(engine or os.path.join(d, 'watch-engine.js'), encoding='utf-8').read()
    print('=== gear teeth: a bank tick must change pixels AND read forward ===')
    grab = lambda pat: float(re.search(pat, dr).group(1))
    try:
        K = int(re.search(r"\bK = (\d+),", dr).group(1))
        mk = [grab(r"%s = (-?[\d.]+)" % n) for n in ('MK_DEEP', 'MK_TIP', 'MK_BASE')]
        import numpy as np
    except (AttributeError, ImportError):
        print('  SKIP: needs numpy, and MK_DEEP/MK_TIP/MK_BASE in dial-renderer.js')
        return []
    TAU = 2 * pi
    TEMPOS = (4, 8, 12, 15)
    NEUTRAL = (0.0, 0.13, 0.3)

    def fold(x, period):
        # the representative the eye settles on: x reduced to (-period/2, period/2]
        x, period = float(x), float(period)
        return x - period * floor(x / period + 0.5)

    def mark_period(loop, copies, frames):
        # mirrors markPeriod() in dial-renderer.js: engaged only when one tick
        # turns more than half of the rim's own repeat period, and never on a
        # fractional loop (no whole-number mark can sit on a non-symmetry)
        if abs(Fraction(loop) / frames) <= Fraction(1, 2):
            return 1
        if Fraction(loop).denominator != 1:
            return 1
        L = int(loop)
        for div in (K, 2):
            if L % div == 0 and int(copies) % div == 0:
                return div
        return 1

    def distinct(step, period):
        # how many different images the bank produces for a feature that repeats
        # every `period` pitches and steps `step` pitches a frame
        k, st, pe = 1, Fraction(step), Fraction(period)
        while (k * st) % pe != 0:
            k += 1
            if k > 8192:
                break
        return k


    def outline(n, rr, ra, th, mp, deep, tipw, basew):
        pitch, lo, pts = TAU / n, ra - deep * (ra - rr), []
        marked = lambda i: mp > 1 and (i % mp) == (mp - 1)
        for i in range(n):
            a, mki, nx = th + i * pitch, marked(i), marked(i + 1)
            r0 = lo if mki else ra
            l, b = (tipw if mki else 0.13), (basew if mki else 0.3)
            nb = basew if nx else 0.3
            for rad, ang in ((rr, a - b * pitch), (r0, a - l * pitch),
                             (r0, a + l * pitch), (rr, a + b * pitch)):
                pts.append((cos(ang) * rad, sin(ang) * rad))
            s0, s1 = a + b * pitch, a + pitch - nb * pitch
            for k in range(1, 7):
                a2 = s0 + (s1 - s0) * k / 6
                pts.append((cos(a2) * rr, sin(a2) * rr))
        return np.array(pts)

    def fill(pts, R):
        n = 2 * R + 1
        gy, gx = np.mgrid[0:n, 0:n].astype(float)
        px, py = gx - R, gy - R
        inside = np.zeros(n * n, dtype=bool)
        X, Y = pts[:, 0], pts[:, 1]
        for i in range(len(pts)):
            j = (i - 1) % len(pts)
            if Y[i] == Y[j]:
                continue
            hit = (Y[i] > py.ravel()) != (Y[j] > py.ravel())
            xint = X[i] + (py.ravel() - Y[i]) * (X[j] - X[i]) / (Y[j] - Y[i])
            inside ^= hit & (px.ravel() < xint)
        return inside.reshape(n, n)

    def rim(n, m, mp, th):
        rp = m * n / 2.0
        ra, rr = rp + m, rp - 1.25 * m
        return fill(outline(n, rr, ra, float(th) * (TAU / n), mp,
                            *(mk if mp > 1 else NEUTRAL)), int(ceil(ra + 3)))

    def tick_pixels(n, m, mp, step, against=None):
        # pixels the rim changes when it turns `step` pitches. `against` names the
        # rotation the FIRST image was drawn at, so the caller can ask how far the
        # aliased reading is from the true one rather than from zero.
        a = rim(n, m, mp, against if against is not None else 0.0)
        return int((a ^ rim(n, m, mp, step)).sum())

    pairs = sorted({(int(n), float(m))
                    for side, arr, m in re.findall(r"\b(front|rear): \[(.*?)\],\s*[fr]m:\s*([\d.]+)", en, re.S)
                    for n in re.findall(r"\bN:\s*(\d+)\b", arr)})
    bad, worst, dup = [], (1 << 30, None), {}
    if mk[0] < 0 or mk[1] > 0.13 or mk[2] > 0.3:
        bad.append('mark grows outward or widens (deep=%g tip=%g base=%g): a meshing pair has '
                   'only 0.25 * m of tip-to-root clearance, so the marked tooth would '
                   'overlap the wheel it drives' % tuple(mk))
    marked_tiers = set()

    for n, m in pairs:
        if n % K:
            bad.append('N=%d m=%g: %d teeth is not a multiple of K=%d, so the %d-tooth loop '
                       'is not a symmetry of this wheel and the seam will pop' % (n, m, n, K, K))
            continue
        spokes = n // K
        pin = spokes * (2 if spokes < 6 else 1)
        for feat, cnt, loop in (('teeth', n, K), ('pinion', pin, Fraction(K * pin, n))):
            if loop.denominator != 1:
                bad.append('N=%d m=%g %s: a %d-tooth loop turns %s pinion pitches, so the '
                           'pinion is not a symmetry of the loop' % (n, m, feat, K, loop))
                continue
            for tier in TEMPOS:
                bank = bank_frames(tier)
                step = Fraction(loop, bank)
                mp = mark_period(loop, cnt, bank)
                # A mark that draws nothing leaves every pitch identical to its
                # neighbour, so the period that counts is 1 however mp reads.
                live = mp > 1 and tuple(mk) != NEUTRAL
                period = mp if live else 1
                if live:
                    marked_tiers.add(tier)
                # A bank deeper than the feature's own repeat leaves some frames
                # equal to earlier ones. That is NOT a freeze -- every tick still
                # turns, and an unchanged ring costs the panel no rows -- so it is
                # reported, not failed. The freeze is the CONSECUTIVE pair being
                # equal, which is what the reading below catches as `frozen`.
                nd = distinct(step, period)
                if nd != bank:
                    dup[feat] = max(dup.get(feat, (0, '')),
                                    (bank - nd, 'N=%d m=%g @%d fps' % (n, m, tier)))
                # ---- the direction gate -------------------------------------
                # what the eye settles on, given the symmetry the renderer drew
                chosen = fold(step, period)
                if abs(abs(chosen) - period / 2.0) < 1e-9:
                    bad.append('N=%d m=%g %s @%d fps (%d-frame bank): one tick turns exactly '
                               'half of a %d-pitch repeat, so forward and backward are equally '
                               'good readings and the wheel will look like it wobbles'
                               % (n, m, feat, tier, bank, period))
                elif chosen <= 0:
                    bad.append('N=%d m=%g %s @%d fps (%d-frame bank): one tick turns %s '
                               'pitches, which on a %d-pitch-period rim reads as %s' %
                               (n, m, feat, tier, bank, float(step), period,
                                'frozen' if abs(chosen) < 1e-9
                                else 'BACKWARD (%.3f/pitch per tick)' % chosen))
                if feat != 'teeth':
                    continue
                # Where the rim's own one-pitch symmetry would hand the eye a
                # slower REVERSE reading, the index tooth has to be present and
                # has to break that symmetry in pixels, not just in name.
                naive = fold(step, 1)
                if naive < -1e-9:
                    if not live:
                        bad.append('N=%d m=%g teeth @%d fps (%d-frame bank): a tick turns %s '
                                   'teeth, past half a tooth, and no index tooth is engaged -- '
                                   'the unmarked rim is identical to a %.3f-tooth step the other '
                                   'way, so the rim crawls backward while the spokes go forward'
                                   % (n, m, tier, bank, float(step), -naive))
                    else:
                        px = tick_pixels(n, m, mp, step, against=step - 1)
                        if px < max(8, n // 4):
                            bad.append('N=%d m=%g teeth @%d fps: the index tooth is engaged but '
                                       'the reverse image differs by only %d px -- too faint to '
                                       'rule the backward reading out' % (n, m, tier, px))
                # A tooth under ~3 px tall at 466 px is sub-pixel decoration, not a
                # tooth: it cannot be seen to move, whatever the arithmetic says.
                if 2.25 * m < 3.0:
                    bad.append('N=%d m=%g: teeth are only %.2f px tall, too fine to read as '
                               'moving at any tempo' % (n, m, 2.25 * m))
                if tier in (4, 15):
                    px = tick_pixels(n, m, mp, step)
                    # every tooth edge must actually cross a pixel: N teeth moving a
                    # fraction of a pitch cannot change fewer than N pixels. This is
                    # what catches a wheel shrunk to a blob, where the lattice is
                    # sound but nothing is left to see.
                    if px < n:
                        bad.append('N=%d m=%g teeth: only %d px change per %d fps tick, below '
                                   'one per tooth' % (n, m, px, tier))
                    if px < worst[0]:
                        worst = (px, 'N=%d m=%g @%d fps' % (n, m, tier))
                # Frozen-rim control: a whole-tooth step on an unmarked rim must be
                # invisible. If it is not, the rasteriser cannot see the fault and
                # every assertion above is worthless.
                if tick_pixels(n, m, 1, 1):
                    bad.append('N=%d m=%g: a whole-tooth step on an unmarked rim changed pixels '
                               '-- the control is broken, so this gate cannot detect a frozen rim'
                               % (n, m))
    print('  %-46s %d (N x module combinations)' % ('wheels checked:', len(pairs)))
    print('  %-46s %s' % ('bank depth per tempo:',
                          ' '.join('%d fps:%d' % (t, bank_frames(t)) for t in TEMPOS)))
    print('  %-46s %s' % ('tiers with the index tooth engaged:',
                          ', '.join('%d fps' % t for t in sorted(marked_tiers)) or 'none'))
    print('  %-46s %s' % ('whole-tooth control is frozen (as K=4 was):',
                          'FAIL' if any('control is broken' in b for b in bad)
                          else 'ok (0 px at every wheel)'))
    # ---- mutants: the arithmetic must bite on the historical faults -----------
    # (a) the freeze bug: K = 4 unmarked lands back on the rim every tick
    alias4 = [distinct(Fraction(4, bank_frames(f)), 1) for f in TEMPOS]
    print('  %-46s %s' % ('mutant: K=4 unmarked resolves:', ' '.join(map(str, alias4))))
    if alias4 == [bank_frames(f) for f in TEMPOS]:
        bad.append('mutant: K=4 resolved one image per frame -- the positions check cannot fail')
    # (b) at a whole-tooth step an unmarked rim must be invisible and a marked
    # rim must not be: that difference IS the index tooth, and if either half is
    # wrong every pixel assertion above is measuring nothing
    mmax = max(mm for _, mm in pairs)
    plain, index = tick_pixels(32, mmax, 1, 1), tick_pixels(32, mmax, 4, 1)
    print('  %-46s unmarked %d px / index tooth engaged %d px'
          % ('mutant: whole-tooth step, mark off / on:', plain, index))
    if plain:
        bad.append('mutant: an unmarked rim changed %d px over a whole tooth -- the rasteriser '
                   'cannot see a frozen rim, so no assertion here means anything' % plain)
    if not index:
        bad.append('mutant: the index tooth changed 0 px over a whole tooth -- it is invisible, '
                   'so nothing rules the backward reading out')
    # (c) the fault this build actually shipped with: strip the index tooth from
    # every tier and the direction gate must name the tier that reads backward
    strip = [fold(Fraction(K, bank_frames(t)), 1) for t in TEMPOS]
    needs = [t for t, r in zip(TEMPOS, strip) if r < 0]
    print('  %-46s %s' % ('mutant: no mark, reading/pitch per tick:',
                          ' '.join('%d:%+.3f' % (t, r) for t, r in zip(TEMPOS, strip))))
    if not needs:
        bad.append('mutant: no tier reads backward with the mark removed -- the direction '
                   'gate can never fire')
    elif not all(t in marked_tiers for t in needs):
        bad.append('tier(s) %s need the index tooth to read forward and do not have one' % needs)
    print('  %-46s %s px  (%s)' % ('smallest tick at any tier:', worst[0], worst[1] or '-'))
    print('  %-46s %s' % ('frames a ring repeats inside the bank:',
                          ', '.join('%s: up to %d extra (%s)' % (f, d[0], d[1])
                                    for f, d in sorted(dup.items())) or 'none'))
    for b in bad[:6]:
        print('  FAIL ' + b)
    return bad



def settle(cls, iters=60, **kw):
    o = cls(**kw)
    o.build()
    for _ in range(iters):
        o.upd()
    b = o.budget()
    return dict(builds=o.builds, fps=b['fps'], rows=b['rows'], rendered=o.rendered,
                rendered_fps=o.rendered_fps,
                seq=o.seq, passes=o.passes, stable=len(set(o.seq[-8:])) == 1)


def settle_dial(cls, dials, iters=6, **kw):
    """Sweep the dial after settling, recording what each edit costs.

    iters is deliberately small: one upd() is one React commit, and a dial edit
    that needs more than a pass or two to settle is the regression being hunted.
    """
    o = cls(**kw)
    o.build()
    for _ in range(30):
        o.upd()
    out = []
    for d in dials:
        before_builds, before_bp = o.builds, o._bp
        o.dial = d
        for _ in range(iters):
            o.upd()
        b = o.budget()
        out.append(dict(dial=d, fps=b['fps'], rows=b['rows'], bank=o.rendered,
                        builds=o.builds, builds_added=o.builds - before_builds,
                        bp_delta=o._bp - before_bp,
                        stable=len(set(o.seq[-6:])) == 1))
    return out


def seal_cull_check(engine=None, html=None, bake=None):
    """Assert the date window and the moonphase are sealed -- not priced as viewports.

    A dial-hosted window or moon is an opaque data disc the device composites over
    its own aperture once a day; the geartrain under it never reaches the panel, so
    those rows must not count toward the export tempo. The fix has one mechanism
    covering both types (the ``seal`` flag on the aperture descriptor), and THREE
    coupled halves that must all hold or the upper-bound invariant breaks:

      ENGINE   buildDial takes a ``seal`` mode and, in it, never punches an aperture
               the ``seal`` flag marks -- only genuine skeleton openings stay cut. The
               opening branch returns BEFORE any seal is set, so a real cutout is
               charged. face() builds the sealed canvas once (only when a dial-hosted
               disc exists) and exposes it as F.dialSealed.

      COMPOSER occlMask() ORs fc.dialSealed (not the holed fc.dial) so rowPrice stops
               charging an aperture the opaque disc plainly hides, AND exportKit ships
               layers/dial.png from that same sealed canvas. Both sides use ONE source
               so the priced mask and the shipped art cannot drift apart.

      BAKE     bake_clock.py's bg_occluder still ORs any non-rotating, NORMAL-blend,
               UNFOLDED static layer that paints a texel ALONE at alpha 255. Because the
               shipped dial (static1) is now opaque over the aperture, the bake culls
               exactly the rows the composer stopped charging -- so rowPrice stays an
               UPPER bound on what the bank DMA's, never below it.

    A one-sided change breaks that: seal in the composer but ship a holed dial and the
    bake still pushes the train the disc hides (flush misses the tempo the composer
    picked); seal the shipped dial but price the holed one and the readout over-charges.
    The two mutants below are those one-sided edits; both MUST be caught here.
    """
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'watch-composer')
    en = open(engine or os.path.join(d, 'watch-engine.js'), encoding='utf-8').read()
    src = open(html or os.path.join(d, 'Watch Composer.dc.html'), encoding='utf-8').read()
    bk = open(bake or os.path.join(d, '..', 'bake_clock.py'), encoding='utf-8').read()
    print('=== date/moon seal: the aperture is an occluder, not a viewport ===')

    # predicates: engine, composer-inline, bake source -> bool
    preds = [
        ('engine: buildDial takes a seal mode',
         lambda e, s, b: 'function buildDial(D, lay, baseEn, pal, glow, seal)' in e),
        ('engine: seal mode drops the sealed apertures',
         lambda e, s, b: 'seal ? lay.dialHoles.filter((h) => !h.seal) : lay.dialHoles' in e),
        ('engine: the window/moon branch is the only seal',
         lambda e, s, b: e.count('hole.seal = true') == 1),
        ('engine: seal is set only on a dial-hosted disc',
         lambda e, s, b: 'if (!onSub) hole.seal = true;' in e),
        ('engine: an opening returns before it can be sealed',
         lambda e, s, b: (e.split("if (c.type === 'opening') {", 1)[-1]
                          .split('hole.seal', 1)[0].count('return;') >= 1)),
        ('engine: face exposes F.dialSealed',
         lambda e, s, b: 'F.dialSealed = lay.dialHoles.some((h) => h.seal)' in e),
        ('composer: ships layers/dial.png from the sealed canvas',
         lambda e, s, b: 'await this.png(bake(lf.dialSealed))' in s),
        ('composer: occlMask prices the sealed dial',
         lambda e, s, b: '[fc.dialSealed, fc.glass, fc.outer]' in s),
        ('bake: the occluder still demands one opaque layer alone at 255',
         lambda e, s, b: 'full = a >= 255' in b and 'occ | full' in b
         and 'if L.rotating or L.blend != 0 or L in folded:' in b),
    ]
    out = [n for n, f in preds if not f(en, src, bk)]
    for n, f in preds:
        print('  %-48s %s' % (n, 'FAIL' if n in out else 'ok'))

    # mutants: the two one-sided edits must be caught, or this gate passes vacuously.
    ships_holed = src.replace('await this.png(bake(lf.dialSealed))',
                              'await this.png(bake(lf.dial))')
    prices_holed = src.replace('[fc.dialSealed, fc.glass, fc.outer]',
                               '[fc.dial, fc.glass, fc.outer]')
    for n, msrc in (('mutant: ships a holed dial (bake still pushes the train)', ships_holed),
                    ('mutant: prices the holed dial (over-counts the sealed disc)', prices_holed)):
        caught = ('await this.png(bake(lf.dialSealed))' not in msrc) or \
                 ('[fc.dialSealed, fc.glass, fc.outer]' not in msrc)
        print('  %-48s %s' % (n, 'ok' if caught else 'FAIL: gate cannot fail'))
        if not caught:
            out.append(n)
    return out


def main():
    fails = []
    # ---- the load-time stack overflow: asserted off the shipped HTML itself ----
    for n in source_cycle_check():
        fails.append('source separation: ' + n)
    for n in source_syntax_check():
        fails.append('source syntax: ' + n)
    for n in gear_marking_check():
        fails.append('gear marking: ' + n)
    for n in seal_cull_check():
        fails.append('date/moon seal: ' + n)
    # ---- the occlusion cull: a dial edit re-prices rows, it does not re-render ----
    print('=== dial-only change (batch-7 occlusion cull) ===')
    rows = settle_dial(PostPatch, ['openwork', 'sector', 'crescent', 'closed',
                                   'openwork'], rows=MONO)
    for r in rows:
        print('  %-9s fps=%2d rows=%3d bank=%s builds+=%d _bp+=%d stable=%s %s'
              % (r['dial'], r['fps'], r['rows'], r['bank'], r['builds_added'],
                 r['bp_delta'], r['stable'], 'ok' if r['stable'] else 'FAIL'))
        if not r['stable']:
            fails.append('dial sweep unstable ' + r['dial'])
    if any(r['builds_added'] or r['bp_delta'] for r in rows):
        print('  FAIL: a dial-only edit re-rendered the train (builds/_bp moved)')
        fails.append('dial edit re-renders gears')
    if rows[0]['rows'] is not None:
        seq = [r['rows'] for r in rows[:4]]
        if seq != sorted(seq, reverse=True):
            print('  FAIL: rows do not fall as the dial closes: %s' % seq)
            fails.append('rows not monotone under dial closing')
        else:
            print('  rows fall monotonically as the dial closes: %s' % seq)

    # a dial edit that crosses a cap must re-render exactly once (new bank length).
    # The bank is BANKS[fps], not fps: below the top tier it is deliberately deeper.
    print('=== dial edit that crosses the cap ===')
    cross = settle_dial(PostPatch, ['openwork', 'closed', 'openwork'], rows=WIDE)
    for r in cross:
        ok = r['stable'] and r['bank'] == bank_frames(r['fps'])
        print('  %-9s fps=%2d rows=%3d bank=%s builds+=%d %s'
              % (r['dial'], r['fps'], r['rows'], r['bank'], r['builds_added'],
                 'ok' if ok else 'FAIL'))
        if not ok:
            fails.append('cap-crossing dial edit ' + r['dial'])
    if cross[1]['builds_added'] > 1:
        print('  FAIL: cap-crossing rebuild did not settle in one pass (%d)'
              % cross[1]['builds_added'])
        fails.append('cap-crossing dial edit unstable')

    # ---- the hold belongs to DIAL edits only, never to a gear-art edit ---------
    # Two cases have to stay distinguishable, and the mv gate in budget() is what
    # separates them. A dial edit leaves the frames on screen untouched, so
    # quoting the delivered tempo is the truth and re-tiering the stale rows is
    # the trap. A GEAR edit invalidates stats['mv'] instead, and must fall to case
    # 1 (quote want): if the hold leaked across it, an art that fits 15 fps would
    # stay pinned at the 4 fps its predecessor earned, because _ftier only walks
    # down. Each probe below is safe to make: the branch it exercises returns
    # before the _ftier/_hbFps bookkeeping, so reading it does not change it.
    print('=== the hold: dial edit keeps it, gear edit drops it ===')
    # (first commit, settled rate). The dial case ends FAST on purpose: holding is
    # about not lying mid-flight, and once the honest rows land the closed dial
    # earns its tempo back. A hold that leaked across the gear edit would end
    # pinned at 4 instead, since _ftier cannot walk back up.
    for edit, first_exp, end_exp in (('dial', 4, 15), ('gear', 15, 15)):
        g = PostPatch(rows=WIDE)
        g.build()
        for _ in range(30):
            g.upd()
        low = g.budget()['fps']
        if edit == 'dial':
            g.dial = 'closed'                     # frames unchanged: hold the tempo
        else:
            g.rows_map = MONO                     # movement art: re-price from want
        first = g.budget()['fps']                 # the rate the readout shows at once
        for _ in range(30):
            g.upd()
        end = g.budget()
        ok = (low == 4 and first == first_exp and end['fps'] == end_exp
              and g.rendered == bank_frames(end['fps'])
              and len(set(g.seq[-6:])) == 1)
        print('  %-4s edit: prior fps=%d -> first commit quotes %2d, settles at '
              '%2d fps bank=%s %s'
              % (edit, low, first, end['fps'], g.rendered, 'ok' if ok else 'FAIL'))
        if not ok:
            fails.append('hold leaks on ' + edit + ' edit')

    # ---- a stale first price must release, not pin the session ------------------
    # The clamp exists to stop two prices of ONE measurement picking two tiers. It
    # must not outlive that measurement: a closed dial behind a needle is a 0-row
    # art, so if the session's first price saw the whole train live (occluder mask
    # cold, dial canvas mid-rebuild) an unbreakable pin would hold that kit at 8 fps
    # -- min(15, 8) is 8 every time -- while the re-priced rows plainly say 15.
    print('=== a stale first price releases when better evidence lands ===')

    class StaleFirst(PostPatch):
        bad = True

        def measured(s, f):
            return 130 if s.bad else 0      # 130 rows behind a needle caps at 8 fps

    

    g = StaleFirst(rows=MONO, needle=True, tick='sweep', dial='closed')
    g.build()
    for _ in range(20):
        g.upd()
    pinned = g.budget()['fps']
    g.bad = False
    g.occl += 1                             # the mask finally landed; the art did not move
    g.remeasure()                           # SAME art key: only the occlusion changed
    for _ in range(20):
        g.upd()
    end = g.budget()
    ok = (pinned == 8 and end['fps'] == 15 and end['rows'] == 0
          and g.rendered == 15 and len(set(g.seq[-6:])) == 1)
    print('  needle + closed dial: stale price caps at %d fps, honest 0-row '
          're-price settles at %2d fps bank=%s %s'
          % (pinned, end['fps'], g.rendered, 'ok' if ok else 'FAIL'))
    if not ok:
        fails.append('stale first price pins the tempo')

    # ---- sweep is always selectable; it clamps to the top tier the art fits -----
    # Sweep is a look, not a budget decision: the Hands tab no longer vetoes it.
    # Whichever art is in hand, picking sweep asks for 15 fps and budget() silently
    # delivers the highest tier the rows actually fit. The two cases below prove
    # the choice is never refused -- a light art sweeps at its own tier, a heavy
    # one still sweeps, just slower -- and that the settled rate is the tier.
    print('=== sweep always selectable, clamped to the fitted tier ===')
    # openwork dials scale by 1.0, so the rows priced are the rows written here.
    # The needle 15-fps cap is the sweeping-needle frontier (CAPS[15]=64), NOT a
    # binary 0: a closed Aviator/railway dial prices a ~48-row geartrain sliver
    # that rowPrice()'s upper bound cannot fold away, and it must earn 15 fps,
    # not fall to 12. The three cases below pin both edges of that cap.
    FLAT48 = {15: 48, 12: 48, 8: 48, 4: 48}        # closed needle dial -> 15
    FLAT80 = {15: 80, 12: 80, 8: 80, 4: 80}        # past the frontier -> still 12
    FLAT300 = {15: 300, 12: 300, 8: 300, 4: 300}   # genuinely too heavy
    for name, rows_map, exp_fps in (('48 rows', FLAT48, 15), ('80 rows', FLAT80, 12),
                                    ('300 rows', FLAT300, 4)):
        g = PostPatch(rows=rows_map, needle=True, tick='sweep', dial='openwork')
        g.build()
        for _ in range(30):
            g.upd()
        # want is 15 (sweep). 48 rows <= CAPS[15]=64 -> 15; 80 clears only
        # CAPS[12]=96 -> 12; 300 clears only CAPS[8]=240 -> 4.
        b = g.budget()
        ok = (b['fps'] == exp_fps and g.tick == 'sweep'
              and g.rendered == bank_frames(b['fps'])
              and len(set(g.seq[-6:])) == 1)
        print('  needle + sweep, %-9s -> settled %2d fps (want 15), bank=%s %s'
              % (name, b['fps'], g.rendered, 'ok' if ok else 'FAIL (want %d)' % exp_fps))
        if not ok:
            fails.append('sweep clamp wrong at ' + name)

    # ---- the shipped regression: a closed needle dial must reach 15, not 12 ------
    # Aviator (matte black, comps: []) and the railway prints seal the whole
    # train behind an opaque disc, so the only rows left are the needle's own
    # swept sliver (~48 through rowPrice's upper bound). With the old binary
    # CAPS[15]=0 gate that sliver stranded them on the 12 tier; the measured
    # sweeping-needle frontier (CAPS[15]=64) lets them earn 15 fps on sweep.
    print('=== closed dial + centre-second needle sweeps at 15 fps ===')
    for name, rows_map, dial in (('aviator (48)', FLAT48, 'openwork'),
                                 ('railway (closed)', MONO, 'closed'),
                                 ('pilot (closed)', NEEDLE, 'closed')):
        g = PostPatch(rows=rows_map, needle=True, tick='sweep', dial=dial)
        g.build()
        for _ in range(30):
            g.upd()
        b = g.budget()
        ok = (b['fps'] == 15 and g.tick == 'sweep'
              and g.rendered == bank_frames(15) and len(set(g.seq[-6:])) == 1)
        print('  needle, %-16s -> settled %2d fps rows=%3d bank=%s %s'
              % (name, b['fps'], b['rows'], g.rendered, 'ok' if ok else 'FAIL'))
        if not ok:
            fails.append('closed needle dial stranded off 15 fps: ' + name)

    # ---- the carriage is priced, not locked --------------------------------------
    # Removing the hard 4 fps tourbillon pin is the point: an exposed carriage
    # still pays for its swept rows (it lands at 4 fps on a wide dial), but one
    # sealed behind an opaque dial prices to ~0 rows and earns the top tier. The
    # only extra rule is the baker's CLK_TOURB_MAX bank: a carriage may never sit
    # at 12 fps (bank 24), so that tier is skipped rather than refused outright.
    print('=== carriage priced by opening area, capped by the bank ===')
    # A carriage on a fully closed dial: the cull drops its whole swept box, so a
    # sweep hand there is a 0-row art and tiers to the top rate the baker can hold.
    gc = PostPatch(rows=MONO, tick='sweep', dial='closed', car=True)
    gc.build()
    for _ in range(30):
        gc.upd()
    bc = gc.budget()
    ok = (bc['fps'] == 15 and gc.rendered == bank_frames(15) <= CLK_TOURB_MAX
          and len(set(gc.seq[-6:])) == 1)
    print('  carriage, closed dial, sweep -> %2d fps bank=%s %s'
          % (bc['fps'], gc.rendered, 'ok' if ok else 'FAIL'))
    if not ok:
        fails.append('carriage locked on a closed dial')
    # A carriage on a wide open dial priced at the 12 fps band: it must fall past
    # 12 (bank 24 > CLK_TOURB_MAX) to the next affordable tier, not export 12 fps
    # and trip the baker. NEEDLE rows sit inside CAPS[12]=160 so the guard is the
    # only thing keeping it off 12; it lands at 8 (bank 16, fits).
    gw = PostPatch(rows={15: 130, 12: 130, 8: 130, 4: 130}, needle=True, tick='sweep',
                   dial='openwork', car=True)
    gw.build()
    for _ in range(30):
        gw.upd()
    bw = gw.budget()
    ok = (bw['fps'] == 8 and gw.rendered == bank_frames(8) and bank_frames(8) <= CLK_TOURB_MAX
          and len(set(gw.seq[-6:])) == 1)
    print('  carriage, 130 rows (12 fps band) -> %2d fps bank=%s (skips 12) %s'
          % (bw['fps'], gw.rendered, 'ok' if ok else 'FAIL'))
    if not ok:
        fails.append('carriage sat on the 12 fps tier the baker cannot bake')

    # ---- the slider drag: throttled prices, monotone tempo, bank always agrees ----
    # 21 pointer moves must NOT cost 21 row-prices (one getImageData + a full diff
    # each). The tempo IS allowed to step while the user opens the dial -- a wider
    # opening really does cost rows -- but only downward (the _ftier clamp), and the
    # rate quoted at any moment must be the rate the bank actually holds, which is
    # the disagreement the original flicker was made of. The trailing price is what
    # makes the LAST position honest, so it must land once the pointer stops.
    print('=== skeleton-slider drag (throttled re-price) ===')
    DIALS.update({'p%d' % i: i / 20.0 for i in range(21)})
    o = PostPatch(rows=WIDE, price_ms=4)
    o.build()
    for _ in range(20):
        o.upd()
    p0 = o.prices
    mid = []
    for i in range(21):
        o.dial = 'p%d' % i
        o.upd()
        mid.append((o.budget()['fps'], o.rendered, o.rendered_fps))
    drag = o.prices - p0
    for _ in range(12):
        o.upd()
    b = o.budget()
    want_rows = o.measured(o.rendered_fps)      # the rows table is keyed by TEMPO
    settled = len(set(o.seq[-6:])) == 1
    # The readout's tempo must be the tempo the frames on screen were made for,
    # and the bank on screen must be that tempo's bank depth. Both halves matter:
    # frames are counted in bank units, prices are quoted per tick at the tempo.
    agreed = all(rf == f and r == bank_frames(rf) for f, r, rf in mid)
    stepped = [t[0] for t in mid]
    no_rise = all(a >= c for a, c in zip(stepped, stepped[1:]))
    print('  21 moves -> %d prices (throttled); fps steps %s; bank agrees=%s; '
          'never rises=%s; final fps=%d bank=%s rows=%d/%d stable=%s %s'
          % (drag, sorted(set(stepped), reverse=True), agreed, no_rise, b['fps'],
             o.rendered, b['rows'], want_rows, settled,
             'ok' if (1 <= drag < 21 and agreed and no_rise and settled
                      and b['fps'] == o.rendered_fps
                      and o.rendered == bank_frames(b['fps'])
                      and b['rows'] == want_rows) else 'FAIL'))
    if not (1 <= drag < 21 and agreed and no_rise and settled
            and b['fps'] == o.rendered_fps
            and o.rendered == bank_frames(b['fps'])
            and b['rows'] == want_rows):
        print('   FAIL: drag history=%s rendered=%s rows=%s want=%s'
              % (mid[-6:], o.rendered, b['rows'], want_rows))
        fails.append('slider drag throttle')

    capped = [('wide openings', dict(rows=WIDE), 4, True),
              ('jittery boundary', dict(rows=JITTER), 8, True),
              ('needle + geartrain', dict(rows=NEEDLE, needle=True), 8, True),
              ('tick 4, mid band', dict(rows=TICKY, tick='4'), 12, False)]
    print('=== pre-patch: does the build render the rate it publishes? ===')
    for name, kw, _, needs_cap in capped:
        r = settle(PrePatch, **kw)
        swings = [p for p in r['passes'] if p[1] != p[2]]
        bad = (r['rendered_fps'] != r['fps']) != needs_cap
        print('%-18s builds=%2d fps=%2d bank=%2s passes(before,render,publish)=%s'
              % (name, r['builds'], r['fps'], r['rendered'], r['passes'][:3]))
        if bad:
            print('   FAIL: bank/export agreement not as expected for this art')
            fails.append('pre-patch ' + name)
        elif needs_cap:
            print('   defect reproduced: the last pass rendered at %s fps but publishes %s fps;'
                  ' the readout walks %s mid-render'
                  % (r['rendered_fps'], r['fps'], [p[1] for p in swings]))
        else:
            print('   control: art fits the first tempo tried, so no rebuild was needed')
    r = settle(PrePatch, rows=MONO)
    print('%-18s builds=%2d fps=%2d bank=%2s (control: art already inside the 15 fps cap)'
          % ('narrow openings', r['builds'], r['fps'], r['rendered']))

    print('\n=== post-patch: settle, no swings, bank == BANKS[fps] ===')
    for name, kw, expect, _ in capped:
        r = settle(PostPatch, **kw)
        # after the cold pass the readout must never jump: a pass that renders a
        # different rate from the one already on screen is the flicker symptom
        swings = [p for p in r['passes'][1:] if p[0] != p[1]]
        rises = [b for a, b in zip([p[2] for p in r['passes']], [p[2] for p in r['passes']][1:]) if b > a]
        ok = (r['fps'] == expect and r['builds'] <= 3 and r['stable']
              and r['rendered_fps'] == r['fps']
              and r['rendered'] == bank_frames(r['fps'])
              and not swings and not rises)
        if not ok:
            fails.append(name)
        print('%-18s builds=%d fps=%2d(want %2d) rows=%3d bank=%s stable=%s jumps=%d rises=%d %s'
              % (name, r['builds'], r['fps'], expect, r['rows'], r['rendered'],
                 r['stable'], len(swings), len(rises), 'ok' if ok else 'FAIL'))
    for name, kw, expect in [('narrow band, sweep', dict(rows=MONO), 15),
                             ('frozen dial, sweep', dict(rows=FROZEN), 15)]:
        r = settle(PostPatch, **kw)
        ok = (r['fps'] == expect and r['builds'] == 1
              and r['rendered_fps'] == r['fps']
              and r['rendered'] == bank_frames(r['fps']))
        if not ok:
            fails.append(name)
        print('%-18s builds=%d fps=%2d(want %2d) rows=%3d bank=%s %s'
              % (name, r['builds'], r['fps'], expect, r['rows'], r['rendered'],
                 'ok' if ok else 'FAIL'))
    print('\n%d failing case(s)' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    raise SystemExit(main())



