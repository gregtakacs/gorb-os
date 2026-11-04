/* theme@1/preview — how an editor shows the glass (theme-editor-kit-plan K3).
 *
 * Any app's editor gets, without carrying it:
 *   - OrbTheme.screen: the panel's physical facts (466 px across 1.75 in) and the true-size
 *     calibration, one number per browser (owner ruling, 2026-11-03: per browser, local
 *     data) shared by every app, set with a ruler or a bank card;
 *   - OrbTheme.view: the stage's views (Fit, 1:1, True size, Magnify), one function from
 *     the view to the stage box, the ruler and the captions, so they cannot disagree;
 *   - OrbTheme.panel: Panel and Split, the device's RGB565 blend of the app's layers
 *     through GorbBake's panel mirror (held to orb_gfx.c by headless-panel);
 *   - OrbTheme.ticker and OrbTheme.previewTime: the frame clock (with the slow mode) and
 *     the time the preview shows (live, a fixed pose, or an app's own mode).
 *
 * Plain JS, no React. The app draws its own layers; this decides how big, how they blend,
 * and when the next frame is due.
 */
(function (root) {
  'use strict';
  var OrbTheme = root.OrbTheme = root.OrbTheme || {};
  var W = 466;

  /* ---- the screen. A page cannot measure its own monitor: it knows css px, and a css
   * inch is 96 px only on a 96 dpi screen at 100 %. The calibration is the factor that
   * corrects it, and it describes the monitor, so it lives with the browser, never with
   * a design, a kit or the artwork. Storage can throw (file://, private mode): every
   * touch is guarded, and "calibrated" is what this page set or read, never assumed. */
  var screen = OrbTheme.screen = {
    PANEL: { px: W, inch: 1.75, cssInch: 96 },
    RULER_MM: 40,
    CARD_MM: [85.6, 53.98, 3.18],          // ISO/IEC 7810 ID-1: width, height, corner radius
    MIN: 0.5, MAX: 3,
    KEY: 'orb.screen.cal',
    LEGACY: ['wc.viewCal'],                 // the clock editor's key before K3: read once, carried over
  };
  screen.MM_PER_PX = (screen.PANEL.inch * 25.4) / screen.PANEL.px;
  screen.REAL_CSS = Math.round(screen.PANEL.inch * screen.PANEL.cssInch);
  screen.REAL_Z = screen.REAL_CSS / screen.PANEL.px;
  var ok = function (v) { return v >= screen.MIN && v <= screen.MAX; };
  screen.clamp = function (v) { return +Math.min(screen.MAX, Math.max(screen.MIN, +v || 1)).toFixed(4); };
  // {cal, set}: the stored factor, or 1 and set=false when this browser holds none
  screen.load = function () {
    try {
      var v = +root.localStorage.getItem(screen.KEY);
      if (ok(v)) return { cal: v, set: true };
      for (var i = 0; i < screen.LEGACY.length; i++) {
        var o = +root.localStorage.getItem(screen.LEGACY[i]);
        if (ok(o)) { try { root.localStorage.setItem(screen.KEY, String(o)); } catch (e) { /* kept for this page */ } return { cal: o, set: true }; }
      }
    } catch (e) { /* no storage: calibrate per page */ }
    return { cal: 1, set: false };
  };
  // the clamped factor, written to this browser when it can be
  screen.save = function (v) {
    var c = screen.clamp(v);
    try { root.localStorage.setItem(screen.KEY, String(c)); } catch (e) { /* this page only */ }
    return c;
  };
  // past 2× a 0.005 step moves the true-size face about 0.8 css px, finer than the stage shows
  screen.step = function (cal) { return cal > 2 ? 0.01 : 0.005; };
  screen.realCss = function (cal) { return Math.max(60, Math.round(screen.REAL_CSS * (cal > 0 ? cal : 1))); };
  screen.rulerPx = function (cal) { return Math.round((screen.RULER_MM / 25.4) * screen.PANEL.cssInch * (cal > 0 ? cal : 1)); };
  screen.screenInch = function (css, cal) { return css / (screen.PANEL.cssInch * (cal > 0 ? cal : 1)); };
  // the bank card's outline at true size, in css px
  screen.cardCss = function (cal) {
    var k = (screen.PANEL.cssInch * (cal > 0 ? cal : 1)) / 25.4;
    return { w: Math.round(screen.CARD_MM[0] * k), h: Math.round(screen.CARD_MM[1] * k), r: Math.round(screen.CARD_MM[2] * k) };
  };
  /* The two instruments. The ruler is drawn RULER_MM long and the user types what a real
   * ruler reads against it; the card outline is drawn at the card's size and the user
   * types how wide it really measures (or 0 when the card matches). Either solves the
   * factor outright. null when the reading is out of range. */
  screen.fromRuler = function (cal, mm) {
    var m = +mm;
    if (!(m >= 5 && m <= 300)) return null;
    return screen.clamp(cal * (screen.RULER_MM / m));
  };
  screen.fromCard = function (cal, measuredMm) {
    var m = +measuredMm;
    if (!(m >= 20 && m <= 400)) return null;
    return screen.clamp(cal * (screen.CARD_MM[0] / m));
  };

  /* ---- the stage's views. One number, the scale k, drives the box the canvas sits in,
   * the ruler and the captions, so none can disagree with another. Past 466 css px the box
   * stops growing and the frame scrolls through it: every layer is drawn at 466 px, so a
   * bigger view enlarges the artwork without inventing detail. */
  OrbTheme.VIEWS = [
    ['fit', 'Fit', 'The whole face as large as the stage holds it.'],
    ['full', '1:1', 'The artwork at its own 466 px: one canvas pixel per screen pixel.'],
    ['true', 'True size', 'The face at the size the panel really is: the only preview that tells you how print will read. Calibrate this screen once.'],
    ['zoom', 'Magnify', 'Scroll a bigger frame through the stage to inspect the print. The layers are drawn at 466 px, so this enlarges them; it does not add detail.'],
  ];
  OrbTheme.ZOOMS = [1.5, 2, 3, 4];
  OrbTheme.viewK = function (s) {
    if (s.view === 'fit') return (+s.fitPx || W) / W;
    if (s.view === 'full') return 1;
    if (s.view === 'zoom') return Math.min(4, Math.max(1, +s.zoom || 2));
    return screen.REAL_Z * (s.cal > 0 ? s.cal : 1);
  };
  // s: {view, zoom, fitPx, cal, calibrated}
  OrbTheme.view = function (s) {
    var cal = s.cal > 0 ? s.cal : 1, k = OrbTheme.viewK(s), v = s.view;
    var css = v === 'true' ? screen.realCss(cal) : Math.max(60, Math.round(W * k)), inch = screen.screenInch(css, cal);
    var zoomed = v === 'zoom' || css > W;
    var ring = Math.max(3, Math.min(12, Math.round((12 * css) / W)));
    var mm = screen.RULER_MM, mmPanel = (screen.PANEL.inch * 25.4).toFixed(1), ticks = [];
    for (var i = 0; i <= mm; i++) ticks.push({ left: (i / mm) * 100, h: i % 10 === 0 ? 14 : i % 5 === 0 ? 10 : 6, op: i % 5 === 0 ? '0.9' : '0.45', lab: i % 10 === 0 ? String(i) : '' });
    return {
      k: k, css: css, win: zoomed ? W : css, zoomed: zoomed, ring: ring, flow: zoomed ? 'auto' : 'hidden', inch: inch,
      tag: v === 'fit' ? 'Fit · ' + k.toFixed(2) + '×' : v === 'full' ? '1:1 · 466 px' : v === 'true' ? 'True size · ' + inch.toFixed(2) + ' in' : k + '× · scroll to pan',
      // the ruler belongs to true size only: the others magnify the artwork, and a physical
      // claim drawn there would be a claim about the zoom, not the orb
      showRuler: v === 'true', rulerPx: screen.rulerPx(cal), rulerMm: mm, rulerTicks: ticks, card: screen.cardCss(cal),
      calText: cal.toFixed(3) + '× · ' + Math.round(screen.PANEL.cssInch * cal) + ' css px per screen in',
      text: (v === 'fit'
        ? 'Fitted to the stage: ' + k.toFixed(2) + '× the artwork, ' + inch.toFixed(2) + ' in across on this screen'
        : v === 'full'
        ? 'Shown at the artwork’s own scale: ' + inch.toFixed(2) + ' in across on this screen, ' + (inch / screen.PANEL.inch).toFixed(1) + '× the 1.75 in panel'
        : v === 'zoom'
          ? 'Magnified ' + k.toFixed(2) + '× — the frame is drawn at 466 px, so this enlarges it without adding detail'
          : s.calibrated
            ? 'True size: ' + inch.toFixed(2) + ' in on this screen, the 1.75 in panel at ' + mmPanel + ' mm'
            // uncalibrated, 168 px is only an assumption: say so, and point at the fix
            : 'True size drawn at the 96 px/in standard, so the 1.75 in panel is ' + inch.toFixed(2) + ' in on this screen — on a denser monitor that is smaller than the orb; match the ruler or a bank card once and it is exact')
        + ' · 1 px = ' + screen.MM_PER_PX.toFixed(4) + ' mm on the glass',
    };
  };

  /* ---- Panel and Split. layers: the app's stage split the way the device stacks it, as
   * RGBA pixel arrays at 466×466: {under, live: [{px, still}], top, keep}. under is the
   * plate (stored RGB565, dithered), live the moving rows bottom first (truncated sprites;
   * a still row dithered, as the bake ships it), top the glass and case (dithered), keep
   * an optional per-texel mask of plate texels the bake leaves undithered. In Split the
   * design pixels already on the canvas keep the right of the wipe. */
  OrbTheme.panel = function (ctx, layers, opts) {
    var G = root.GorbBake;
    if (!G || !G.panelPlate) return false;
    opts = opts || {};
    var codes = G.panelPlate(layers.under, W, W, true);
    if (layers.keep) {
      var tr = G.panelPlate(layers.under, W, W, false);
      for (var p = 0; p < layers.keep.length; p++) if (layers.keep[p]) codes[p] = tr[p];
    }
    (layers.live || []).forEach(function (l) { G.panelLayer(codes, W, l.px, W, W, { dither: !!l.still }); });
    if (layers.top) G.panelLayer(codes, W, layers.top, W, W, { dither: true });
    var img = ctx.createImageData(W, W);
    G.panelRGBA(codes, img.data);
    if (opts.mode === 'compare') {
      var w = Math.max(0, Math.min(W, opts.wipe | 0));
      if (w > 0) ctx.putImageData(img, 0, 0, 0, 0, w, W);
      ctx.save(); ctx.strokeStyle = 'rgba(88,166,255,.9)'; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(w + 0.5, 0); ctx.lineTo(w + 0.5, W); ctx.stroke(); ctx.restore();
    } else ctx.putImageData(img, 0, 0);
    return true;
  };
  OrbTheme.RENDERS = [
    ['design', 'Design', 'The artwork as the editor draws it: 8 bits a channel, smooth blending.'],
    ['panel', 'Panel', 'RGB565, what the orb will show: the plate as the bake stores it (RGB565, dithered), the moving rows and the glass blended the way the device blends them. Banding and the slight darkening the glass adds show here.'],
    ['compare', 'Split', 'Panel on the left of the split, Design on the right. Move the split with the slider.'],
  ];

  /* ---- the frame clock. fps() is read at each start, so an editor that re-prices its
   * pace calls start() again; slow plays at a fifth of it. Returns the running ticker. */
  OrbTheme.ticker = function (fps, slow, onTick) {
    var F = Math.max(1, +fps || 1), id = setInterval(onTick, slow ? 1000 / Math.max(1, F / 5) : 1000 / F);
    return { stop: function () { clearInterval(id); }, fps: F, slow: !!slow };
  };
  /* The time the preview shows: 'live' is now, 'pose' the picture pose (10:09:36, the
   * hands' most legible spread), and any other mode is the app's own (modes[mode](now)). */
  OrbTheme.previewTime = function (mode, modes) {
    var d = new Date();
    if (mode === 'pose') { d.setHours(10, 9, 36, 0); return d; }
    if (mode && mode !== 'live' && modes && typeof modes[mode] === 'function') return modes[mode](d);
    return d;
  };
})(typeof window !== 'undefined' ? window : globalThis);
