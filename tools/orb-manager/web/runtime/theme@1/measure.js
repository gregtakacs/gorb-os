/* theme@1/measure — what a design costs the glass (theme-editor-kit-plan K4).
 *
 * Any app's editor gets, without carrying it:
 *   - OrbTheme.motion(frames): the motion walk. Diff consecutive frames cyclically and
 *     answer the union of moved texels, the moved count and their bounds: the
 *     measurement GorbBake.splitPolicy3 decides a carrier from;
 *   - OrbTheme.estimate(bytes): the baker's own estimate of the rate the device paints a
 *     baked bundle at (GorbBake.estimate3 on its one section), the pace for an app with
 *     no model of its own;
 *   - OrbTheme.fit({rows, cap, want, pace, est}): the verdict an editor shows. Does it fit
 *     (live rows against the cap), and does it run at the rate it was designed for. The
 *     pace is the app's own model when it has one (the clock's _devPace prices a seconds
 *     sweep across its plates), else the bundle's estimate, else unknown.
 *
 * Plain JS, no React.
 */
(function (root) {
  'use strict';
  var OrbTheme = root.OrbTheme = root.OrbTheme || {};

  /* frames: RGBA pixel arrays of one size (w×h, default 466×466), one per frame, in order.
   * The last frame is diffed against the first, as the device loops a bank. */
  OrbTheme.motion = function (frames, w, h) {
    w = w || 466; h = h || w;
    var N = w * h, n = frames.length, mask = new Uint8Array(N);
    var bx = N, by = N, bx2 = -1, by2 = -1, moved = 0, un = 0;
    for (var f = 0; f < n; f++) {
      var a = frames[f], b = frames[(f + 1) % n];
      for (var p = 0, i = 0; p < N; p++, i += 4) {
        if (a[i] !== b[i] || a[i + 1] !== b[i + 1] || a[i + 2] !== b[i + 2]) {
          moved++;
          if (!mask[p]) {
            mask[p] = 1; un++;
            var x = p % w, y = (p / w) | 0;
            if (x < bx) bx = x; if (x > bx2) bx2 = x;
            if (y < by) by = y; if (y > by2) by2 = y;
          }
        }
      }
    }
    return { unionPx: un, movedPx: moved, bboxW: moved ? bx2 - bx + 1 : 0, bboxH: moved ? by2 - by + 1 : 0,
             bbox: moved ? [bx, by, bx2 + 1, by2 + 1] : null, mask: mask };
  };

  // the baker's estimate for a baked bundle, or null when it cannot read one
  OrbTheme.estimate = function (bytes) {
    var G = root.GorbBake;
    if (!G || !G.estimate3 || !G.verifyBundle3) return null;
    try { var v = G.verifyBundle3(bytes); return v && v.sections && v.sections[0] ? G.estimate3(v.sections[0]) : null; }
    catch (e) { return null; }
  };

  OrbTheme.FIT_COLORS = {
    over: { fg: '#f85149', bd: '#5c2326', bg: '#2a1416' },
    full: { fg: '#3fb950', bd: '#23502c', bg: '#12241a' },
    slow: { fg: '#d29922', bd: '#5a4614', bg: '#241d0e' },
  };
  /* rows, cap: the live rows the design ships and the most the orb seats. want: the rate it
   * was designed for. pace: the app's own model of the rate the glass holds; est: a
   * GorbBake.estimate3 answer, used when there is no pace. Neither: the pace is unknown, and
   * the verdict says it fits without claiming a rate. */
  OrbTheme.fit = function (o) {
    var rows = o.rows | 0, cap = o.cap | 0, want = +o.want || 0;
    var pace = o.pace != null ? +o.pace : o.est && o.est.est_fps != null ? +o.est.est_fps : null, from = o.pace != null ? 'model' : pace != null ? 'estimate' : 'none';
    var fits = !(cap && rows > cap), known = pace != null && want > 0;
    var full = !known || pace >= want, near = !known || pace >= want * 0.75;
    var state = !fits ? 'over' : full ? 'full' : 'slow', C = OrbTheme.FIT_COLORS[state];
    return {
      fits: fits, full: full, near: near, state: state, pace: pace, want: want, from: from,
      text: !fits ? '✕ Too many live layers for the orb' : full ? '✓ Fits the orb'
        : near ? '✓ Fits — runs a little slower than designed' : '✓ Fits — the orb paints it slower than designed',
      note: (known ? '~' + pace + ' of ' + want + ' fps · ' : '') + rows + ' of ' + cap + ' live rows',
      short: !fits ? 'Over · ' + rows + ' / ' + cap + ' rows' : (full ? 'Fits' : 'Fits, slower') + (known ? ' · ' + pace + ' fps' : ''),
      color: C.fg, border: C.bd, background: C.bg,
    };
  };
})(typeof window !== 'undefined' ? window : globalThis);
