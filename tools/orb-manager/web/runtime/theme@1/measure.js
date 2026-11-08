/* theme@1/measure — what a design costs the glass (theme-editor-kit-plan K4).
 *
 * Any app's editor gets, without carrying it:
 *   - OrbTheme.motion(frames): the motion walk. Diff consecutive frames cyclically and
 *     answer the union of moved texels, the moved count and their bounds: the
 *     measurement GorbBake.splitPolicy3 decides a carrier from;
 *   - OrbTheme.estimate(bytes): the baker's own estimate of the rate the device paints a
 *     baked bundle at (GorbBake.estimate3 on its one section), the pace for an app with
 *     no model of its own;
 *   - OrbTheme.pace(frames, {base, want}): the rate a view holds on the orb, for ANY app
 *     (theme-editor-kit-plan §8, E6), from what its preview draws: consecutive frames sampled
 *     at `base` fps are diffed, and each candidate rate on the panel's grid is priced on the
 *     frames that far apart with estimate4's compositor costs (GorbBake.EST4);
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

  /* One frame's price on the ink-span compositor, from the pixels that changed between two
   * frames (RGBA arrays, w×h). The orb restores, per screen row, the union of its movers' old
   * and new spans: one span from the first changed pixel to the last, less the widest inkless
   * gap inside when it is at least GAP_MIN (orb_gfx.c INK_GAP_MIN), and walks the ink it
   * repaints (the changed pixels). frame_ms = R restored_kpx + W walk_kpx + FIXED (§49.15). */
  OrbTheme.frameCost = function (a, b, w, h) {
    var E = root.GorbBake.EST4, A = new Uint32Array(a.buffer, a.byteOffset, w * h), B = new Uint32Array(b.buffer, b.byteOffset, w * h);
    var restored = 0, walk = 0;
    for (var y = 0; y < h; y++) {
      var o = y * w, lo = -1, hi = -1, gap = 0, run = 0;
      for (var x = 0; x < w; x++) {
        if (A[o + x] !== B[o + x]) {
          walk++;
          if (lo < 0) lo = x; else if (run > gap) gap = run;
          hi = x; run = 0;
        } else if (lo >= 0) run++;
      }
      if (lo >= 0) restored += hi - lo + 1 - (gap >= E.GAP_MIN ? gap : 0);
    }
    return { ms: (E.R * restored + E.W * walk) / 1000 + E.FIXED, restored: restored, walk: walk };
  };
  /* frames: RGBA arrays sampled at o.base fps, in order; o.want: the rate the view is
   * designed for (the grid's top caps it). Each grid rate H at or under it is priced on
   * frames base/H apart, its worst frame against its shortest period (7.5 alternates 7 and 8
   * scans): it holds when cost × (1 + SLOP) + MARGIN fits. Answers {est, want, plans: [{fps,
   * worst_ms, mean_ms, fits}], restored_kpx, walk_kpx} at the estimate's rate; est is the
   * fastest rate that holds, else the grid's slowest. */
  OrbTheme.pace = function (frames, o) {
    var G = root.GorbBake, E = G.EST4, grid = G.GRID4, w = o.w || 466, h = o.h || w, base = o.base;
    var want = Math.min(grid[0], +o.want || grid[0]), plans = [], at = null;
    for (var g = 0; g < grid.length; g++) {
      var H = grid[g]; if (H > want + 1e-6) continue;
      var k = Math.max(1, Math.round(base / H)), worst = null, sum = 0, n = 0;
      for (var i = 0; i + k < frames.length; i += k) {
        var c = OrbTheme.frameCost(frames[i], frames[i + k], w, h);
        sum += c.ms; n++;
        if (!worst || c.ms > worst.ms) worst = c;
      }
      if (!n) continue;
      var period = Math.floor(60 / H) * E.SCAN, fits = worst.ms * (1 + E.SLOP) + E.MARGIN <= period;
      var p = { fps: H, worst_ms: +worst.ms.toFixed(1), mean_ms: +(sum / n).toFixed(1), fits: fits,
                restored_kpx: +(worst.restored / 1000).toFixed(1), walk_kpx: +(worst.walk / 1000).toFixed(1) };
      plans.push(p);
      if (fits && !at) at = p;
    }
    if (!at) at = plans[plans.length - 1] || null;
    return { est: at ? at.fps : null, want: want, plans: plans, restored_kpx: at ? at.restored_kpx : 0, walk_kpx: at ? at.walk_kpx : 0,
             worst_ms: at ? at.worst_ms : 0 };
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
