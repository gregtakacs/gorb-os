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

Run: python3 tools/bench/loopcheck.py   (exit 1 on regression)"""
CAPS = {False: {15: 96, 12: 160, 8: 304}, True: {15: 0, 12: 96, 8: 240}}
# rows/tick measured at each candidate tempo, keyed by the frames rendered.
MONO = {15: 60, 12: 75, 8: 110, 4: 190}          # narrow skeleton openings
WIDE = {15: 300, 12: 340, 8: 380, 4: 420}        # wide openings
NEEDLE = {15: 130, 12: 150, 8: 175, 4: 230}      # running seconds + geartrain
JITTER = {15: 200, 12: 170, 8: 150, 4: 200}      # anti-aliasing picks a worse pair
FROZEN = {15: 0, 12: 0, 8: 0, 4: 0}
TICKY = {15: 150, 12: 160, 8: 200, 4: 250}


class Base:
    def __init__(s, needle=False, tick='sweep', rows=MONO):
        s.needle, s.tick, s.rows_map = needle, tick, rows
        s._mk, s.stats, s.builds, s.seq, s.rendered = None, None, 0, [], None
        s.passes = []

    def art(s):
        return ('art', s.needle, s.tick, tuple(sorted(s.rows_map.items())))

    def mvKey(s, noframes=0):
        return repr(s.art() if noframes else (s.art(), ('frames', s.F())))

    def tier(s, r, want):
        K = CAPS[s.needle]
        for t in (15, 12, 8):
            if t > want:
                continue
            if r <= K[t]:
                return t
        return 4

    def budget(s, forceWant=None):
        raise NotImplementedError

    def F(s):
        return s.budget()['fps']

    def render(s, f):
        s.rendered = f
        s.stats = dict(key=s.stats_key(), f=f, rows=s.rows_map[f])

    def build(s):
        raise NotImplementedError

    def upd(s):
        raise NotImplementedError


class PrePatch(Base):
    """budgetStats keyed on _mk; unbounded rebuilds from componentDidUpdate."""

    def stats_key(s):
        return s._mk

    def budget(s, forceWant=None):
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
        s.passes.append((before, s.rendered, s.F()))

    def upd(s):
        if s._mk != s.mvKey():
            s.build()
        s.seq.append(s.F())


class PostPatch(Base):
    """budgetStats keyed on artKey() (no frames), _ftier clamp, three-pass cap."""

    def __init__(s, *a, **k):
        Base.__init__(s, *a, **k)
        s._artKey, s._ftier, s._ftierKey, s._bk, s._bp = None, 15, None, None, 0

    def stats_key(s):
        return s._artKey

    def budget(s, forceWant=None):
        want = forceWant or (15 if s.tick == 'sweep' else 12)
        st = s.stats if (s.stats and s.stats['key'] == s._artKey) else None
        if st is None:
            return {'fps': want, 'rows': -1}
        fps = s.tier(st['rows'], want)
        if not forceWant:
            fk = (s._artKey, want)
            if s._ftierKey != fk:
                s._ftierKey, s._ftier = fk, 15
            fps = min(fps, s._ftier)
            s._ftier = fps
        return {'fps': fps, 'rows': st['rows']}

    def build(s):
        s.builds += 1
        before = s.F()
        s._mk, s._artKey = s.mvKey(), s.mvKey(1)
        s.render(s.F())
        s.passes.append((before, s.rendered, s.F()))

    def upd(s):
        if s._mk != s.mvKey():
            ak = s.mvKey(1)
            if ak != s._bk:
                s._bk, s._bp = ak, 0
            if s._bp < 3:
                s._bp += 1
                s.build()
        s.seq.append(s.F())


def settle(cls, iters=60, **kw):
    o = cls(**kw)
    o.build()
    for _ in range(iters):
        o.upd()
    b = o.budget()
    return dict(builds=o.builds, fps=b['fps'], rows=b['rows'], rendered=o.rendered,
                seq=o.seq, passes=o.passes, stable=len(set(o.seq[-8:])) == 1)


def main():
    fails = []
    capped = [('wide openings', dict(rows=WIDE), 4, True),
              ('jittery boundary', dict(rows=JITTER), 8, True),
              ('needle + geartrain', dict(rows=NEEDLE, needle=True), 8, True),
              ('tick 4, mid band', dict(rows=TICKY, tick='4'), 12, False)]
    print('=== pre-patch: does the build render the rate it publishes? ===')
    for name, kw, _, needs_cap in capped:
        r = settle(PrePatch, **kw)
        swings = [p for p in r['passes'] if p[1] != p[2]]
        bad = (r['rendered'] != r['fps']) != needs_cap
        print('%-18s builds=%2d fps=%2d bank=%2s passes(before,render,publish)=%s'
              % (name, r['builds'], r['fps'], r['rendered'], r['passes'][:3]))
        if bad:
            print('   FAIL: bank/export agreement not as expected for this art')
            fails.append('pre-patch ' + name)
        elif needs_cap:
            print('   defect reproduced: the last pass rendered %s frames but publishes %s fps;'
                  ' the readout walks %s mid-render'
                  % (r['rendered'], r['fps'], [p[1] for p in swings]))
        else:
            print('   control: art fits the first tempo tried, so no rebuild was needed')
    r = settle(PrePatch, rows=MONO)
    print('%-18s builds=%2d fps=%2d bank=%2s (control: art already inside the 15 fps cap)'
          % ('narrow openings', r['builds'], r['fps'], r['rendered']))

    print('\n=== post-patch: settle, no swings, bank == export fps ===')
    for name, kw, expect, _ in capped:
        r = settle(PostPatch, **kw)
        # after the cold pass the readout must never jump: a pass that renders a
        # different rate from the one already on screen is the flicker symptom
        swings = [p for p in r['passes'][1:] if p[0] != p[1]]
        rises = [b for a, b in zip([p[2] for p in r['passes']], [p[2] for p in r['passes']][1:]) if b > a]
        ok = (r['fps'] == expect and r['builds'] <= 3 and r['stable']
              and r['rendered'] == r['fps'] and not swings and not rises)
        if not ok:
            fails.append(name)
        print('%-18s builds=%d fps=%2d(want %2d) rows=%3d bank=%s stable=%s jumps=%d rises=%d %s'
              % (name, r['builds'], r['fps'], expect, r['rows'], r['rendered'],
                 r['stable'], len(swings), len(rises), 'ok' if ok else 'FAIL'))
    for name, kw, expect in [('narrow band, sweep', dict(rows=MONO), 15),
                             ('frozen dial, sweep', dict(rows=FROZEN), 15)]:
        r = settle(PostPatch, **kw)
        ok = r['fps'] == expect and r['builds'] == 1 and r['rendered'] == r['fps']
        if not ok:
            fails.append(name)
        print('%-18s builds=%d fps=%2d(want %2d) rows=%3d bank=%s %s'
              % (name, r['builds'], r['fps'], expect, r['rows'], r['rendered'],
                 'ok' if ok else 'FAIL'))
    print('\n%d failing case(s)' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    raise SystemExit(main())



