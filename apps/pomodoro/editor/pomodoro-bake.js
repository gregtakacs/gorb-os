/* pomodoro-bake.js — the pomodoro app's theme, baked in the browser
 * (docs/specs/pomodoro-editor-design.md §5, docs/specs/pomodoro-looks.md).
 *
 * A design (schema 1) drawn on canvases and assembled by the OS's stage bake
 * (GorbBake.stage3) into what apps/pomodoro/main.c reads:
 *   - plate 0: the ground with the look's instrument folded into it — one plane, every
 *             part hideable and every look drawing its own (plan §64.4: two looks sharing
 *             a part is the failure this file exists to avoid). Nothing rides plate 1: the
 *             OS paints that slot over every row, so an instrument baked there buries the
 *             look's own words (see plateCanvas);
 *   - fonts 0 (the figures, d.px), 1 (the words, 30 px) and 2 (the small words, 24 px);
 *   - the labels: 8 the time `mm:ss`, 9 the phase, 10 the count, 11 the state,
 *             12 the choice (on a backing that hides the look's baked caption);
 *   - the dots at 13 and 14: a hollow track per cell count (twelve frames of N cells, N =
 *             1..12) and a fill per count over it (thirteen frames of n filled cells, n =
 *             0..12, frame 0 empty), both left-aligned and drawn at the one place — the app
 *             picks the pair with pm_dots and centres both with one pose offset;
 *   - the chimes at sound slots 1, 2 and 3 (never 0: §3 — slot 0 is the clock's tick
 *             loop and the projector refuses a one-shot there);
 *   - the values (`pm_values_t`, 16 B, layout 1): the ring's three colours and the slots;
 *   - the preview: the whole glass, from picture().
 *
 * Four rules from pomodoro-looks.md every look here obeys, and two are measured by
 * _selftest/bake_check.py: the digits of one width (rule 3) and the furniture inside
 * radius 200 (rule 4). Rule 3 needs one thing more than a wide cell: a glyph's advance on
 * the device is its ink's right edge + 1 + the track (docs/theme-text-plan.md §2.3,
 * main/stage/orb_label.c orb_font_measure), so a figure centred in a wide cell still
 * advances by its own width, and a timer that changes every second walks sideways — and
 * Station's tiles would come unglued from the digits. The figures' cells therefore carry
 * their ink centred AND a column of alpha 2 at each cell edge, so the ink's bbox is the
 * cell for all ten and every figure advances by exactly cell + track. Two alpha bytes in
 * 255 is a tenth of one per cent of the ink over the ground: invisible, and the reason the
 * time stands still. bake_check.py measures both readings of "ink": any alpha, and alpha
 * a eye would call ink.
 *
 * Needs a document (canvas, document.fonts) and GorbBake. The tables and valuesBytes are
 * DOM-free, so node can read the module for what it says about itself. */
(function (root) {
  'use strict';

  var SCHEMA = 1, G = 466, CX = 233, NONE = 0xF;
  var VALUES_VER = 1;                        // pm_values_t, apps/pomodoro/pomodoro.h
  var SLOT = { time: 8, phase: 9, count: 10, state: 11, head: 12, dots: 13, fill: 14 };
  var WORD_PX = 30, SMALL_PX = 24;           // fonts 1 and 2 (design §5)
  var PX_MIN = 64, PX_MAX = 144, PX_DFLT = 96;
  var DOT_N = 12, DOT_SZ = 11, DOT_GAP = 6;  // twelve seats: the app's ceiling (PM_N_MAX)
  var N_TRACK = DOT_N, N_FILL = DOT_N + 1;   // the dots as two picked rows (alerts-0 amendment 2):
                                             // row 13, twelve hollow tracks; row 14, n filled
                                             // cells over them, n = 0..12, frame 0 empty
  var UPLOAD_MAX_S = 4.0;                    // owner question 3: an upload to 4.0 s
  // the kit's own sample rate, asked of the baker at the moment it is needed
  function rate() { return (root.GorbBake && root.GorbBake.SOUND_RATE) || 22050; }
  var R_MIN = 200;                           // rule 4: no furniture mark outside this radius
  var R_CLIP = 193;                          // ... and the bake clips inside this one
  var PAD_ALPHA = 2;                         // the cell-edge column, see the header

  // theme_schema.FONT_CHARSET's printable ASCII and the five symbols, in the order the
  // format numbers cells by; the 29 marks GorbBake.FONT_MORE adds ride after them
  var CHARSET = (function () { var c = []; for (var i = 0x20; i < 0x7F; i++) c.push(String.fromCharCode(i)); return c.concat(['↑', '↓', '→', '←', '°']); })();

  // the nine looks: grounds, faces and inks are the same-named look's in radar, weather and
  // flights, taken by value from docs/specs/pomodoro-looks.md's table
  var LOOKS = {
    scope:     { name: 'Scope',     inst: 'a phosphor countdown readout', top: '#0a1f14', foot: '#020805',
                 face: 'IBM Plex Mono', bodyFace: 'IBM Plex Mono', quietFace: 'IBM Plex Mono',
                 ink: '#b4ffd2', quiet: '#5fbf8a', accent: '#e0b040', dots: 'square', cap: 'T−',
                 ring: ['#4BE38A', '#E0B040', '#5AD1E6'], chime: 'sonar' },
    paper:     { name: 'Paper',     inst: 'a printed timer ticket',
                 top: '#f2ead8', foot: '#e3d7bd',
                 face: 'Playfair Display', bodyFace: 'IBM Plex Sans', quietFace: 'IBM Plex Sans',
                 ink: '#2a2218', quiet: '#5a4a36', accent: '#a04a18', dots: 'round', cap: 'TIMER TICKET',
                 ring: ['#C8401E', '#5E8C4A', '#2F5B8C'], chime: 'bell' },
    brass:     { name: 'Brass',     inst: 'a marine stopwatch',
                 top: '#2e332e', foot: '#151815',
                 face: 'Cormorant Garamond', bodyFace: 'Cormorant Garamond', quietFace: 'Cormorant Garamond',
                 ink: '#f2e6c8', quiet: '#a8946a', accent: '#ff9a3c', dots: 'stud', cap: 'CHRONOMETER',
                 ring: ['#E0A43C', '#4FA38A', '#3A6EA5'], chime: 'shipbell' },
    blueprint: { name: 'Blueprint', inst: 'a drawing of a timer',
                 top: '#174a82', foot: '#0c2c58',
                 face: 'IBM Plex Mono', bodyFace: 'IBM Plex Mono', quietFace: 'IBM Plex Mono',
                 ink: '#eaf4ff', quiet: '#a8cdf0', accent: '#ffe07a', dots: 'square2', cap: 'DWG 25-MIN',
                 ring: ['#FFE07A', '#EAF4FF', '#8EC5FF'], chime: 'beep' },
    station:   { name: 'Station',   inst: 'a departure board countdown',
                 top: '#ffffff', foot: '#e6e6e3',
                 face: 'Jost', bodyFace: 'Jost', quietFace: 'Jost',
                 ink: '#111111', quiet: '#555555', accent: '#e3000f', dots: 'disc', cap: 'DEPARTS IN', fig: '#f4f4f2',
                 ring: ['#E3000F', '#1E8E3E', '#1F4E8C'], chime: 'platform' },
    celestial: { name: 'Celestial', inst: 'an hourglass of stars',
                 top: '#251a55', foot: '#0b0820',
                 face: 'Space Grotesk', bodyFace: 'Space Grotesk', quietFace: 'Space Grotesk',
                 ink: '#edebff', quiet: '#9c95c9', accent: '#ffd27a', dots: 'star', cap: 'ANNO HORAE',
                 ring: ['#FFD27A', '#B9A8FF', '#62D2C6'], chime: 'bowl' },
    silver:    { name: 'Silver',    inst: 'a chronograph stopwatch',
                 top: '#3a3f46', foot: '#1c1f24',
                 face: 'Jost', bodyFace: 'Jost', quietFace: 'Jost',
                 ink: '#f2f4f7', quiet: '#aab2bd', accent: '#7fc8ff', dots: 'ring', cap: 'SPLIT',
                 ring: ['#7FC8FF', '#D6DCE4', '#5B7DA8'], chime: 'click' },
    sectional: { name: 'Sectional', inst: 'a flight log',
                 top: '#eef1e4', foot: '#dfe5cf',
                 face: 'Barlow Condensed', bodyFace: 'Barlow Condensed', quietFace: 'Barlow Condensed',
                 ink: '#1f2a44', quiet: '#3e5a80', accent: '#c2187a', dots: 'chart', cap: 'ETE',
                 ring: ['#C2187A', '#2E7D32', '#2A5DB0'], chime: 'cabin' },
    blank:     { name: 'New',       inst: 'a blank face to start from',
                 top: '#1c1c1c', foot: '#101010',
                 face: 'IBM Plex Sans', bodyFace: 'IBM Plex Sans', quietFace: 'IBM Plex Sans',
                 ink: '#e8e8e8', quiet: '#a8a8a8', accent: '#c8c8c8', dots: 'round', cap: 'POMODORO',
                 ring: ['#D62828', '#0FA36B', '#2E93E4'], chime: 'tone' }
  };
  var LOOK_KEYS = ['scope', 'paper', 'brass', 'blueprint', 'station', 'celestial', 'silver', 'sectional', 'blank'];
  var STARTERS = ['scope', 'paper', 'brass', 'blueprint', 'station', 'celestial', 'silver', 'sectional'];
  var STARTER_TITLES = { scope: 'Scope', paper: 'Paper', brass: 'Brass', blueprint: 'Blueprint', station: 'Station',
                         celestial: 'Celestial', silver: 'Silver', sectional: 'Sectional', blank: 'New' };
  var STARTER_NOTES = {
    scope: 'A phosphor countdown: graticule, scan lines, T\u2212',
    paper: 'A printed timer ticket, perforated and stamped',
    brass: 'A marine stopwatch: engraved plate, crown, ship\u2019s bell',
    blueprint: 'A drawing of a timer: construction circle, title block',
    station: 'A departure board: split-flap tiles, red platform bar',
    celestial: 'An hourglass of stars over a star field',
    silver: 'A chronograph: brushed disc, two pushers, split',
    sectional: 'A flight log: chart grid, magenta checkpoint',
    blank: 'Nothing drawn: every row present, your own look from here'
  };
  // every face named here is one the runtime ships (vendor/fonts.css)
  var FACES = ['IBM Plex Sans', 'IBM Plex Mono', 'Space Grotesk', 'Space Mono', 'Jost', 'Barlow Condensed',
               'Oswald', 'Cormorant Garamond', 'Playfair Display', 'Bodoni Moda'];
  var DOTS = ['round', 'square', 'stud', 'square2', 'disc', 'star', 'ring', 'chart'];
  var DOT_TITLES = { round: 'Inked rounds', square: 'Square blips', stud: 'Brass studs', square2: 'Drafted squares',
                     disc: 'Black discs', star: 'Stars', ring: 'Machined rings', chart: 'Chart squares' };
  // the instrument's parts, by look: the Furniture step hides any one of them (design §7).
  // No two looks share one: each is that look's instrument, translated from the sketch.
  var PARTS = {
    scope: ['graticule', 'scanline', 'caption'],
    paper: ['ticket', 'perforation', 'stamp', 'caption'],
    brass: ['plate', 'crown', 'pips', 'caption'],
    blueprint: ['construction', 'dimension', 'titleblock', 'caption'],
    station: ['tiles', 'platform', 'destination', 'caption'],
    celestial: ['hourglass', 'field', 'caption'],
    silver: ['disc', 'pushers', 'bars', 'caption'],
    sectional: ['grid', 'leg', 'checkpoint', 'caption'],
    blank: ['caption']
  };
  // where each look's rows stand on the glass: the sketch's places, by value
  var LAYOUT = {
    scope: { head: 104, phase: 152, time: 228, count: 326, state: 362, dots: 292 },
    paper: { head: 104, phase: 152, time: 228, count: 330, state: 362, dots: 292, countX: 329 },
    brass: { head: 104, phase: 152, time: 228, count: 326, state: 362, dots: 292 },
    blueprint: { head: 104, phase: 152, time: 228, count: 326, state: 322, dots: 292 },
    station: { head: 104, phase: 150, time: 228, count: 350, state: 380, dots: 318 },
    celestial: { head: 104, phase: 152, time: 228, count: 326, state: 362, dots: 292 },
    silver: { head: 104, phase: 152, time: 228, count: 326, state: 362, dots: 292 },
    sectional: { head: 104, phase: 152, time: 226, count: 326, state: 362, dots: 292 },
    blank: { head: 104, phase: 152, time: 228, count: 326, state: 362, dots: 292 }
  };

  /* ---- the chimes: the recipes of pomodoro-looks.md, as data for GorbBake.synth ----
   * Each look has one chime, one event table, and the brighter variant the break's end
   * strikes (owner question 2). The kit renders them deterministically at 22050 Hz,
   * high-passed at 350 Hz for the 2 W speaker and peak-normalised. `f_mul` is how each
   * variant is written: one multiplier over the whole recipe, which is what the kit's
   * variant mechanism is (Station's and Sectional's sketches vary their notes note by note;
   * one multiplier is the closest the recipe table comes, and the Chimes step can name a
   * different chime outright). */
  var CHIMES = {
    sonar: { label: 'Sonar ping', dur: 0.95, events: [
      { kind: 'tone', t: 0, dur: 0.9, f: 1200, gain: 0.5, attack: 0.004 },
      { kind: 'tone', t: 0.18, dur: 0.6, f: 1200, gain: 0.18, attack: 0.004 }],
      brighter: { f_mul: 1.25 } },
    bell: { label: 'Kitchen bell', dur: 1.25, events: [
      { kind: 'tone', t: 0, dur: 1.2, f: 2000, gain: 0.45, attack: 0.002, decay: 0.133 },
      { kind: 'tone', t: 0, dur: 1.2, f: 4800, gain: 0.18, attack: 0.002, decay: 0.11 },
      { kind: 'tone', t: 0, dur: 1.2, f: 7100, gain: 0.08, attack: 0.002, decay: 0.09 }],
      brighter: { f_mul: 1.12 } },
    shipbell: { label: 'Ship\u2019s bell', dur: 2.4, events: [
      { kind: 'bell', t: 0, dur: 1.8, f: 620, gain: 0.4, ratios: [1, 2, 2.4, 3], attack: 0.002 },
      { kind: 'bell', t: 0.55, dur: 1.8, f: 620, gain: 0.4, ratios: [1, 2, 2.4, 3], attack: 0.002 }],
      brighter: { f_mul: 1.194 } },
    beep: { label: 'Drafting beep', dur: 0.32, events: [
      { kind: 'beep', t: 0, dur: 0.11, f: 1600, gain: 0.22 },
      { kind: 'beep', t: 0.16, dur: 0.11, f: 1600, gain: 0.22 }],
      brighter: { f_mul: 1.1875 } },
    platform: { label: 'Platform chime', dur: 1.95, events: [
      { kind: 'tone', t: 0, dur: 1.1, f: 784, gain: 0.35 }, { kind: 'tone', t: 0, dur: 0.6, f: 1568, gain: 0.08 },
      { kind: 'tone', t: 0.38, dur: 1.1, f: 659, gain: 0.35 }, { kind: 'tone', t: 0.38, dur: 0.6, f: 1318, gain: 0.08 },
      { kind: 'tone', t: 0.76, dur: 1.1, f: 1047, gain: 0.35 }, { kind: 'tone', t: 0.76, dur: 0.6, f: 2094, gain: 0.08 }],
      brighter: { f_mul: 1.122 } },
    bowl: { label: 'Glass bowl', dur: 2.45, events: [
      { kind: 'tone', t: 0, dur: 2.4, f: 880, gain: 0.3, attack: 0.03 },
      { kind: 'tone', t: 0, dur: 2.4, f: 883, gain: 0.3, attack: 0.03 },
      { kind: 'tone', t: 0, dur: 1.4, f: 2600, gain: 0.06, attack: 0.05 }],
      brighter: { f_mul: 1.1886 } },
    click: { label: 'Stopwatch click and ping', dur: 1.0, events: [
      { kind: 'click', t: 0, dur: 0.012, f: 3000, q: 2, gain: 0.9 },
      { kind: 'click', t: 0.09, dur: 0.012, f: 3000, q: 2, gain: 0.7 },
      { kind: 'tone', t: 0.2, dur: 0.7, f: 2400, gain: 0.3 }],
      brighter: { f_mul: 1.1667 } },
    cabin: { label: 'Cabin chime', dur: 1.75, events: [
      { kind: 'tone', t: 0, dur: 0.9, f: 1050, gain: 0.4 }, { kind: 'tone', t: 0, dur: 0.5, f: 2100, gain: 0.05 },
      { kind: 'tone', t: 0.55, dur: 1.1, f: 830, gain: 0.4 }, { kind: 'tone', t: 0.55, dur: 0.6, f: 1660, gain: 0.05 }],
      brighter: { f_mul: 1.19 } },
    tone: { label: 'Simple tone', dur: 0.45, events: [
      { kind: 'tone', t: 0, dur: 0.4, f: 1000, gain: 0.4 }],
      brighter: { f_mul: 1.25 } }
  };
  var CHIME_KEYS = ['sonar', 'bell', 'shipbell', 'beep', 'platform', 'bowl', 'click', 'cabin', 'tone'];

  /* ---- drawing helpers ---- */
  function canvas(w, h) { var c = document.createElement('canvas'); c.width = w; c.height = h; return c; }
  function plane(c) {
    var x = c.getContext('2d').getImageData(0, 0, c.width, c.height);
    return { w: c.width, h: c.height, rgba: new Uint8Array(x.data.buffer) };
  }
  function hex(c) { return [parseInt(c.slice(1, 3), 16), parseInt(c.slice(3, 5), 16), parseInt(c.slice(5, 7), 16)]; }
  function rgba(c, a) { var h = hex(c); return 'rgba(' + h[0] + ',' + h[1] + ',' + h[2] + ',' + a + ')'; }
  function argb(c, a) { var h = hex(c); return ((Math.round((a === undefined ? 1 : a) * 255) << 24) | (h[0] << 16) | (h[1] << 8) | h[2]) >>> 0; }
  // the ground's own colour at one row of the panel: the gradient's answer, for a backing
  function groundAt(d, y) {
    var a = hex(d.groundTop), b = hex(d.groundFoot), k = Math.max(0, Math.min(1, y / G));
    return 'rgb(' + a.map(function (v, i) { return Math.round(v + (b[i] - v) * k); }).join(',') + ')';
  }
  async function loadFace(face, px, text) {
    try { await document.fonts.load(px + 'px "' + face + '"', text || 'N0123456789'); } catch (e) { /* the fallback face draws */ }
  }
  // every face and size a bake or a picture draws with, loaded before anything is drawn
  async function loadFaces(d) {
    await Promise.all([loadFace(d.face, d.px, '0123456789:%'), loadFace(d.bodyFace, WORD_PX),
                       loadFace(d.quietFace, SMALL_PX), loadFace(d.bodyFace, 15)]);
  }

  /* ---- the fonts: theme_schema.FONT_CHARSET's order, one cell, ink centred ---- */
  function fontSet() { return CHARSET.concat(root.GorbBake.FONT_MORE); }
  function fontText(set) { return set.map(function (e) { return typeof e === 'string' ? e : e.acc + e.car; }).join(''); }
  function boxOf(m, e) {
    var t = m.measureText(typeof e === 'string' ? e : e.car), v = typeof e === 'string' ? t : m.measureText(e.acc);
    return { l: Math.floor(-t.actualBoundingBoxLeft), r: Math.ceil(t.actualBoundingBoxRight),
      t: Math.ceil(v.actualBoundingBoxAscent), b: Math.ceil(v.actualBoundingBoxDescent) };
  }
  // the alpha box of what was actually drawn: the centring is measured off the pixels, not
  // off the face's metrics, because rule 3 and the device's advance are both about pixels
  function inkBox(p) {
    var a = p.rgba, l = -1, r = -1, t = -1, b = -1;
    for (var y = 0; y < p.h; y++) for (var x = 0; x < p.w; x++) {
      if (!a[(y * p.w + x) * 4 + 3]) continue;
      if (l < 0 || x < l) l = x;
      if (x > r) r = x;
      if (t < 0 || y < t) t = y;
      if (y > b) b = y;
    }
    return l < 0 ? null : { l: l, r: r, t: t, b: b };
  }
  // o = { keep: [chars] , figures: true }: `keep` leaves every other cell an empty frame
  // (the format allows it, orbtheme-bake §2.1), and `figures` makes the kept cells tabular:
  // the cell is the widest figure, the ink centred in it, and a column of PAD_ALPHA at each
  // edge so the ink's box is the cell and the advance is one width for all ten (see header)
  async function font(face, px, colour, o) {
    o = o || {};
    var spec = (o.weight ? o.weight + ' ' : '') + px + 'px "' + face + '"', CS = fontSet();
    await loadFace(face, px, fontText(CS));
    var m = canvas(4, 4).getContext('2d');
    m.font = spec;
    var top = 0, bottom = 0, w = 1, boxes = CS.map(function (ch) { return boxOf(m, ch); });
    var want = function (i) { return !o.keep || o.keep.indexOf(CS[i]) >= 0; };
    boxes.forEach(function (b, i) {
      if (!b || b.r <= b.l || !want(i)) return;
      top = Math.max(top, b.t); bottom = Math.max(bottom, b.b); w = Math.max(w, b.r - b.l);
    });
    if (!o.figures) {                                  // words need the whole line box, figures do not
      var fm = m.measureText('Hg');
      top = Math.max(top, Math.ceil(fm.fontBoundingBoxAscent || 0));
      bottom = Math.max(bottom, Math.ceil(fm.fontBoundingBoxDescent || 0));
    }
    top += 1; bottom += 1;
    if (o.figures) { w += 2; top = Math.max(top, Math.round(px * 0.72)); }   // slack for the edges, and a cell that does not hug the ink
    var h = top + bottom, glyphs = boxes.map(function (b, i) {
      if (!want(i)) return null;
      var c = canvas(w, h), x = c.getContext('2d');
      x.font = spec; x.textBaseline = 'alphabetic'; x.fillStyle = colour;
      if (b && b.r > b.l) {
        var dx = Math.round((w - (b.r - b.l)) / 2) - b.l;             // the metric box, centred
        var draw = function (at) { x.clearRect(0, 0, w, h); root.GorbBake.drawGlyph(x, CS[i], at, top); };
        draw(dx);
        if (o.figures) {
          var ib = inkBox(plane(c));                                   // then corrected on the pixels
          if (ib) draw(dx + Math.round(((w - 1) - (ib.l + ib.r)) / 2));
          padEdges(c);
        }
      }
      return c;
    });
    var space = Math.max(1, Math.min(w, Math.round(m.measureText(' ').width) || Math.round(px / 3)));
    return { w: w, h: h, glyphs: glyphs, space: space, track: 1, adv: o.figures ? w + 1 : 0 };
  }
  // the tabular edge columns: PAD_ALPHA out of 255 at x 0 and x w-1, every row of the cell
  function padEdges(c) {
    var x = c.getContext('2d'), im = x.getImageData(0, 0, c.width, c.height), a = im.data;
    for (var y = 0; y < c.height; y++) {
      a[(y * c.width) * 4 + 3] = PAD_ALPHA;
      a[((y * c.width) + c.width - 1) * 4 + 3] = PAD_ALPHA;
    }
    x.putImageData(im, 0, 0);
  }
  /* ---- the designs ---- */
  // a starter: the look's palette, the look's parts all shown, its own chime for all three
  // phase ends (the breaks brighter), and its accent as the overlay colour (§64.9)
  function starter(k) {
    var L = LOOKS[k], parts = {};
    PARTS[k].forEach(function (p) { parts[p] = true; });
    return {
      name: L.name, look: k,
      groundTop: L.top, groundFoot: L.foot, figInk: L.fig || L.ink,
      px: PX_DFLT, face: L.face, bodyFace: L.bodyFace, quietFace: L.quietFace, cap: L.cap,
      ink: L.ink, accent: L.accent, quiet: L.quiet,
      parts: parts, dots: L.dots,
      ring: { focus: L.ring[0], short: L.ring[1], long: L.ring[2] },
      overlay: L.ring[0],
      chimes: { focus: { rec: L.chime }, short: { rec: L.chime, variant: 'brighter' }, long: { rec: L.chime, variant: 'brighter' } },
      uploads: []
    };
  }
  var PRESETS = {};
  LOOK_KEYS.forEach(function (k) { PRESETS[k] = starter(k); });

  function preset(k) { return JSON.parse(JSON.stringify(PRESETS[k] || PRESETS.scope)); }
  function blank() { return starter('blank'); }
  function scheme(d, k) { return starter(k === undefined ? (d && d.look) || 'scope' : k); }

  // a design saved by an older editor, or hand-written: every field this bake reads, filled
  function complete(d) {
    var s = starter((d && LOOKS[d.look]) ? d.look : 'scope');
    d = d || {};
    var out = Object.assign({}, s, d);
    out.parts = {};
    PARTS[out.look].forEach(function (p) {
      var v = d.parts && d.parts[p];
      out.parts[p] = v === undefined || v === null ? true : !!v;
    });
    out.ring = Object.assign({}, s.ring, d.ring || {});
    out.chimes = {};
    ['focus', 'short', 'long'].forEach(function (ph) {
      var c = (d.chimes && d.chimes[ph]) || s.chimes[ph];
      out.chimes[ph] = typeof c === 'string' ? { rec: c, variant: ph === 'focus' ? '' : 'brighter' }
                                             : Object.assign({}, s.chimes[ph], c);
    });
    out.uploads = (d.uploads || []).slice();
    if (!(out.px > 0)) out.px = PX_DFLT;              // filled when missing, never clamped: refuse() says by how much
    if (!out.cap && out.cap !== '') out.cap = LOOKS[out.look].cap;
    return out;
  }

  /* ---- the plates ---- */
  // the ground alone, under the instrument plateCanvas folds into it. Nothing drawn here:
  // rule 1 keeps a timer's ground free of numerals, hands and any twelve-hour ring.
  function groundCanvas(d) {
    var c = canvas(G, G), x = c.getContext('2d'), g = x.createLinearGradient(0, 0, 0, G);
    g.addColorStop(0, d.groundTop); g.addColorStop(1, d.groundFoot);
    x.fillStyle = g; x.fillRect(0, 0, G, G);
    return c;
  }

  // rule 4, enforced where every mark is drawn: the furniture is clipped to R_CLIP, so a
  // part written for a small cell cannot spill toward the ring's band at 224..231
  function clipDisc(x) { x.beginPath(); x.arc(CX, CX, R_CLIP, 0, 2 * Math.PI); x.clip(); }
  function polar(a, r) { return [CX + r * Math.sin(a), CX - r * Math.cos(a)]; }
  function ring(x, r, col, w, dash) {
    x.strokeStyle = col; x.lineWidth = w; x.setLineDash(dash || []);
    x.beginPath(); x.arc(CX, CX, r, 0, 2 * Math.PI); x.stroke(); x.setLineDash([]);
  }
  function seg(x, x1, y1, x2, y2, col, w, dash) {
    x.strokeStyle = col; x.lineWidth = w; x.setLineDash(dash || []);
    x.beginPath(); x.moveTo(x1, y1); x.lineTo(x2, y2); x.stroke(); x.setLineDash([]);
  }
  function roundRect(x, px, py, pw, ph, r) {
    x.beginPath(); x.moveTo(px + r, py); x.arcTo(px + pw, py, px + pw, py + ph, r);
    x.arcTo(px + pw, py + ph, px, py + ph, r); x.arcTo(px, py + ph, px, py, r); x.arcTo(px, py, px + pw, py, r); x.closePath();
  }
  function seeded(seed) { var s = seed; return function () { s = (s * 9301 + 49297) % 233280; return s / 233280; }; }
  function text(x, s, px, py, face, size, col, weight) {
    x.font = (weight || 600) + ' ' + size + 'px "' + face + '"'; x.textAlign = 'center'; x.textBaseline = 'middle';
    x.fillStyle = col; x.fillText(s, px, py);
  }
  // the time's standing place: centred on the glass, at the look's own height, five cells wide
  function timeBox(d, ctx) {
    var adv = (ctx && ctx.cell ? ctx.cell : { adv: 0, h: 0 }).adv, L = LAYOUT[d.look];
    return { x: CX, y: L.time, w: 5 * (adv || Math.round(d.px * 0.62)), h: ctx && ctx.cell ? ctx.cell.h : Math.round(d.px), adv: adv || Math.round(d.px * 0.62) };
  }

  /* ---- the furniture: one look's instrument, part by part (plan §64.4: none shared) ----
   * Translated from furniture(id, L) in docs/specs/pomodoro-looks-preview.html, split into
   * the parts the Furniture step hides, and clipped to the disc. Only the header caption is
   * drawn the same way everywhere, because the header row is the one thing the looks design
   * says every look has (pomodoro-looks.md's rows); its words, face and colour are the look's. */
  function CAP(d, x) { text(x, d.cap, CX, LAYOUT[d.look].head, d.bodyFace, 15, rgba(d.quiet, 0.95)); }
  var DRAW = {};
  DRAW.scope = {
    graticule: function (d, x) {
      ring(x, 120, rgba(d.quiet, 0.22), 1); ring(x, 190, rgba(d.quiet, 0.22), 1);
      seg(x, CX, 40, CX, 426, rgba(d.quiet, 0.16), 1); seg(x, 40, CX, 426, CX, rgba(d.quiet, 0.16), 1);
    },
    scanline: function (d, x) {
      for (var y = 0; y < G; y += 3) { x.fillStyle = 'rgba(0,0,0,.18)'; x.fillRect(0, y, G, 1); }
    },
    caption: CAP
  };
  DRAW.paper = {
    ticket: function (d, x) { x.fillStyle = 'rgba(255,255,255,.35)'; x.fillRect(96, 128, 274, 230); },
    perforation: function (d, x) {
      seg(x, 96, 128, 370, 128, d.quiet, 1.2, [3, 4]); seg(x, 96, 358, 370, 358, d.quiet, 1.2, [3, 4]);
    },
    stamp: function (d, x) {                                       // the No. box the count lands in
      x.strokeStyle = d.accent; x.lineWidth = 1.5; x.strokeRect(279, 308, 100, 44);
      text(x, 'No.', 299, 319, d.bodyFace, 10, d.accent);
    },
    caption: CAP
  };
  DRAW.brass = {
    plate: function (d, x) {
      var g = x.createRadialGradient(CX, CX - 40, 20, CX, CX, 150);
      g.addColorStop(0, '#3a3f38'); g.addColorStop(1, '#1d211d');
      x.fillStyle = g; x.beginPath(); x.arc(CX, CX, 150, 0, 2 * Math.PI); x.fill();
      ring(x, 150, '#b08d4a', 3); ring(x, 143, '#6e5a2e', 1);
    },
    crown: function (d, x) {
      x.fillStyle = '#b08d4a'; x.fillRect(CX - 10, 60, 20, 18); x.fillRect(CX - 5, 52, 10, 10);
    },
    pips: function (d, x) {
      x.fillStyle = '#b08d4a';
      for (var i = 0; i < 4; i++) { var p = polar(i * Math.PI / 2, 132); x.beginPath(); x.arc(p[0], p[1], 3, 0, 2 * Math.PI); x.fill(); }
    },
    caption: CAP
  };
  DRAW.blueprint = {
    construction: function (d, x) { ring(x, 185, rgba(d.quiet, 0.45), 1, [6, 5]); },
    dimension: function (d, x, ctx) {                                 // the bar under the time, arrowed
      var b = timeBox(d, ctx), y = b.y + Math.round(b.h / 2) + 22, half = Math.min(110, Math.max(103, b.w / 2 + 10));
      seg(x, CX - half, y, CX + half, y, d.quiet, 1);
      seg(x, CX - half, y - 10, CX - half, y + 10, d.quiet, 1); seg(x, CX + half, y - 10, CX + half, y + 10, d.quiet, 1);
      [[CX - half, 1], [CX + half, -1]].forEach(function (a) {
        x.fillStyle = d.quiet; x.beginPath(); x.moveTo(a[0], y); x.lineTo(a[0] + 9 * a[1], y - 4); x.lineTo(a[0] + 9 * a[1], y + 4); x.closePath(); x.fill();
      });
    },
    titleblock: function (d, x) {
      x.strokeStyle = d.quiet; x.lineWidth = 1; x.strokeRect(128, 346, 210, 36); seg(x, 233, 346, 233, 382, d.quiet, 1);
      x.font = '400 10px "' + d.quietFace + '"'; x.textAlign = 'left'; x.textBaseline = 'middle'; x.fillStyle = d.quiet;
      x.fillText('POMODORO', 134, 364); x.fillText('SHEET n OF N', 239, 364);
    },
    caption: CAP
  };
  DRAW.station = {
    tiles: function (d, x, ctx) {                                     // the split-flap time, hinged
      var b = timeBox(d, ctx), x0 = CX - b.w / 2, top = b.y - 56, h = 112;
      for (var i = 0; i < 5; i++) {
        var tx = x0 + i * b.adv + 2, tw = b.adv - 4;
        x.fillStyle = '#151515'; roundRect(x, tx, top, tw, h, 6); x.fill();
        seg(x, tx, b.y, tx + tw, b.y, '#000000', 2);
      }
    },
    platform: function (d, x) { x.fillStyle = d.accent; x.fillRect(96, 300, 274, 6); },
    destination: function (d, x) {                                    // the rules the destination stands between
      var y = LAYOUT[d.look].phase + 22;
      seg(x, CX - 150, y, CX + 150, y, rgba(d.ink, 0.5), 1);
      seg(x, CX - 150, y + 3, CX + 150, y + 3, rgba(d.ink, 0.25), 1);
      x.fillStyle = rgba(d.ink, 0.5); x.fillRect(CX - 152, y - 2, 4, 4); x.fillRect(CX + 148, y - 2, 4, 4);
    },
    caption: CAP
  };
  DRAW.celestial = {
    hourglass: function (d, x) {
      var hg = [[150, 70], [316, 70], [233, 233], [316, 396], [150, 396], [233, 233]];
      x.strokeStyle = rgba(d.accent, 0.35); x.lineWidth = 1; x.beginPath();
      hg.forEach(function (p, i) { i ? x.lineTo(p[0], p[1]) : x.moveTo(p[0], p[1]); });
      x.closePath(); x.stroke();
      x.fillStyle = rgba(d.accent, 0.7);
      hg.slice(0, 5).forEach(function (p) { x.beginPath(); x.arc(p[0], p[1], 2.4, 0, 2 * Math.PI); x.fill(); });
    },
    field: function (d, x) {                                          // the stars, seeded, inside r 200
      var rnd = seeded(7);
      for (var i = 0; i < 80; i++) {
        var px = rnd() * G, py = rnd() * G, a = 0.2 + rnd() * 0.6;
        if (Math.hypot(px - CX, py - CX) > R_CLIP - 2) continue;
        x.fillStyle = 'rgba(255,255,255,' + a.toFixed(2) + ')'; x.fillRect(px, py, 1.4, 1.4);
      }
    },
    caption: CAP
  };
  DRAW.silver = {
    disc: function (d, x) {
      var g = x.createRadialGradient(CX, CX, 10, CX, CX, 170);
      g.addColorStop(0, '#4a5058'); g.addColorStop(1, '#262a30');
      x.fillStyle = g; x.beginPath(); x.arc(CX, CX, 170, 0, 2 * Math.PI); x.fill();
      for (var i = 0; i < 90; i++) { var a = i / 90 * Math.PI * 2, p1 = polar(a, 40), p2 = polar(a, 168); seg(x, p1[0], p1[1], p2[0], p2[1], 'rgba(255,255,255,.025)', 1); }
      ring(x, 170, '#8d96a3', 1.5);
    },
    pushers: function (d, x) {
      [-0.5, 0.5].forEach(function (a) {
        var p = polar(a, 178);
        x.save(); x.translate(p[0], p[1]); x.rotate(a); x.fillStyle = '#aab2bd'; x.fillRect(-7, -9, 14, 14); x.restore();
      });
    },
    bars: function (d, x, ctx) {                                     // four cardinals on the disc's edge;
      for (var i = 0; i < 4; i++) {                                  // east and west stand off the time's box,
        var a = i * Math.PI / 2, b = timeBox(d, ctx), r0 = 150, r1 = 162;   // and are not drawn under it at all
        if (i % 2) {
          var need = b.w / 2 + 8;
          if (need > 192) continue;                                  // rule 2: at most four, fewer when there is no room
          r0 = Math.max(r0, need); r1 = Math.min(R_CLIP - 2, r0 + 12);
        }
        var p1 = polar(a, r0), p2 = polar(a, r1);
        seg(x, p1[0], p1[1], p2[0], p2[1], '#f2f4f7', 3);
      }
    },
    caption: CAP
  };
  DRAW.sectional = {
    grid: function (d, x) {                                           // the chart's own squares
      for (var px = 40; px < G; px += 46) seg(x, px, 0, px, G, rgba(d.quiet, 0.28), 1);
      for (var py = 40; py < G; py += 46) seg(x, 0, py, G, py, rgba(d.quiet, 0.28), 1);
    },
    leg: function (d, x) { seg(x, 60, 400, 406, 66, rgba(d.accent, 0.45), 2, [10, 6]); },
    checkpoint: function (d, x, ctx) {                                // the box the time is logged in
      var b = timeBox(d, ctx), w = Math.min(2 * (R_CLIP - 6), b.w + 34), hgt = b.h + 34;
      x.fillStyle = rgba(d.groundTop, 0.92); x.fillRect(CX - w / 2, b.y - hgt / 2, w, hgt);
      x.strokeStyle = d.accent; x.lineWidth = 2.5; x.strokeRect(CX - w / 2, b.y - hgt / 2, w, hgt);
    },
    caption: CAP
  };
  DRAW.blank = {                                                      // New: every row, nothing drawn
    caption: function (d, x) { text(x, d.cap, CX, LAYOUT[d.look].head, d.bodyFace, 15, rgba(d.quiet, 0.7)); }
  };

  /* ---- the dots: two picked rows drawn one over the other at the same place (row 13 the
   * track, row 14 the fill; docs/specs/alerts-0-dots.md amendment 2). The track's frame N-1
   * draws N hollow cells, the fill's frame n draws n filled cells fully covering the hollow
   * ones under them, frame 0 empty; both left-aligned on this canvas — the width of twelve
   * cells, the app's ceiling — at this cell size and gap, in the look's own style. The app
   * centres the pair with one shared pose offset off pm_dots' answer. dotFrame stays the
   * editor's preview: one centred row, its own cells first filled. */
  var DOT_W = DOT_N * DOT_SZ + (DOT_N - 1) * DOT_GAP, DOT_H = 14;
  function dotCell(x, d, px, cy, sz, f) {
    var cx = px + sz / 2, on = d.ink;
    x.fillStyle = d.dots === 'stud' ? '#d9b26a' : (d.dots === 'star' || d.dots === 'chart') ? d.accent : on;
    if (d.dots === 'square') { f ? x.fillRect(px, cy - sz / 2, sz, sz) : x.strokeRect(px + .75, cy - sz / 2 + .75, sz - 1.5, sz - 1.5); }
    else if (d.dots === 'square2') {                                    // drafted: a box, filled from its centre
      x.strokeRect(px + .75, cy - sz / 2 + .75, sz - 1.5, sz - 1.5);
      if (f) x.fillRect(cx - 2.5, cy - 2.5, 5, 5);
    }
    else if (d.dots === 'star') {
      x.beginPath();
      for (var p = 0; p < 10; p++) {
        var r = p % 2 ? sz * .22 : sz * .55, a = p / 10 * 2 * Math.PI;
        x.lineTo(cx + r * Math.sin(a), cy - r * Math.cos(a));
      }
      x.closePath(); f ? x.fill() : x.stroke();
    }
    else if (d.dots === 'ring') {                                        // machined: a ring, filled to the bore
      x.beginPath(); x.arc(cx, cy, sz / 2 - 1, 0, 2 * Math.PI); x.stroke();
      if (f) { x.beginPath(); x.arc(cx, cy, sz / 2 - 3.5, 0, 2 * Math.PI); x.fill(); }
    }
    else if (d.dots === 'stud') {
      x.strokeStyle = '#8a6f3a'; x.beginPath(); x.arc(cx, cy, sz / 2 - .75, 0, 2 * Math.PI); f ? x.fill() : x.stroke();
    }
    else if (d.dots === 'disc') {                                        // a board's snoot: dark when spent
      if (f) { x.fillStyle = d.ink; x.beginPath(); x.arc(cx, cy, sz / 2 - .75, 0, 2 * Math.PI); x.fill(); }
      else { x.beginPath(); x.arc(cx, cy, sz / 2 - .75, 0, 2 * Math.PI); x.stroke(); }
    }
    else if (d.dots === 'chart') {                                       // a chart square, ticked when logged
      x.strokeRect(px + .75, cy - sz / 2 + .75, sz - 1.5, sz - 1.5);
      if (f) { x.beginPath(); x.moveTo(px + 1.5, cy + sz / 2 - 1.5); x.lineTo(cx, cy - sz / 2 + 1.5); x.lineTo(px + sz - 1.5, cy + sz / 2 - 1.5); x.closePath(); x.fill(); }
    }
    else { x.beginPath(); x.arc(cx, cy, sz / 2 - .75, 0, 2 * Math.PI); f ? x.fill() : x.stroke(); }
  }
  function dotRowCanvas(d, count, filled, left) {                   // count cells, the first `filled` lit
    var n = Math.max(0, Math.min(DOT_N, Math.round(count)));
    var f0 = Math.max(0, Math.min(n, Math.round(filled)));
    var c = canvas(DOT_W, DOT_H), x = c.getContext('2d'), sz = DOT_SZ;
    x.lineWidth = 1.5; x.strokeStyle = d.quiet;
    var row = n * DOT_SZ + (n - 1) * DOT_GAP;                       // what this frame draws
    for (var i = 0, px = left ? 0 : Math.round((DOT_W - row) / 2); i < n; i++, px += sz + DOT_GAP) dotCell(x, d, px, DOT_H / 2, sz, i < f0);
    return c;
  }
  function dotFrame(d, filled, cells) {                             // the editor's preview: one centred row
    var n = cells === undefined || cells === null ? DOT_N : Math.round(cells);
    n = n < 1 ? 1 : n > DOT_N ? DOT_N : n;                          // never fewer than one cell
    return dotRowCanvas(d, n, filled, false);
  }
  function dotTrack(d) {                                            // row 13: N hollow cells, left-aligned
    var out = [];
    for (var n = 1; n <= DOT_N; n++) out.push(dotRowCanvas(d, n, 0, true));
    if (out.length !== N_TRACK) throw new Error('the track row bakes ' + out.length + ' frames, not ' + N_TRACK);
    return out;
  }
  function dotFill(d) {                                             // row 14: n filled cells, frame 0 empty
    var out = [];
    for (var n = 0; n <= DOT_N; n++) out.push(dotRowCanvas(d, n, n, true));
    if (out.length !== N_FILL) throw new Error('the fill row bakes ' + out.length + ' frames, not ' + N_FILL);
    return out;
  }
  var FIGURES = ['0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ' ', '%'];

  // one part of the look's instrument, hidden or not, clipped to rule 4's disc
  function frame(d, ctx, part) {
    var f = DRAW[d.look] && DRAW[d.look][part];
    if (!f) return false;
    if (d.parts && d.parts[part] === false) return false;
    var x = ctx.ctx || ctx;
    x.save(); clipDisc(x); f(d, x, ctx); x.restore();
    return true;
  }
  // the whole instrument as one transparent plane, all its parts that are showing
  function furnitureCanvas(d, ctx) {
    var c = canvas(G, G), x = c.getContext('2d'), fctx = Object.assign({ ctx: x }, ctx);
    PARTS[d.look].forEach(function (p) { frame(d, fctx, p); });
    return c;
  }
  /* The ground with its instrument folded into it, which is what plate 0 carries. The
   * stacking rule: plate 1 is the OS's glass overlay and rides above EVERY row — main/stage/
   * orb_stage.c sorts a plate at slot 1 to the top of the stack and tags its widget above
   * the glass' own rows — so furniture baked there paints over the look's own words and the
   * digits vanish under the flaps (Station), the bars under the disc (Silver), the engraving
   * under the figures (Brass). Statics belong in the plate the words stand on. */
  function plateCanvas(d, ctx) {
    var c = groundCanvas(d);
    c.getContext('2d').drawImage(furnitureCanvas(d, ctx), 0, 0);
    return c;
  }

  /* ---- the picture: the picker's preview, the start sheet's tile and the PNG evidence all
   * from here (design §5: a second renderer for the evidence is the drift this rule prevents) ---- */
  var VIEWS = {
    focus: { time: '25:00', phase: 'Focus', count: '3 of 4', state: '', cells: 4, filled: 3 },
    short: { time: '05:00', phase: 'Short break', count: 'break', state: '', cells: 4, filled: 3 },
    long: { time: '15:00', phase: 'Long break', count: 'break', state: '', cells: 4, filled: 4 }
  };
  // the three fonts measured once: `cell` is what the furniture stands under
  async function fontContext(d) {
    var fig = await font(d.face, d.px, figInk(d), { keep: FIGURES, figures: true });
    return { cell: { w: fig.w, h: fig.h, adv: fig.adv, space: fig.space }, fig: fig };
  }
  async function picture(d, size, o) {
    o = o || {};
    d = complete(d);
    var v = VIEWS[o.view] || VIEWS.focus, ctx = await fontContext(d), L = LAYOUT[d.look];
    // pomodoro-editor §7's `count` view: which pomodoro this is, at the look's own row. Only a
    // focus counts; a break's row keeps the word the bake's own table gives it.
    if (o.count && v === VIEWS.focus) v = Object.assign({}, v, { count: o.count + ' of 4', filled: o.count });
    var c = canvas(G, G), x = c.getContext('2d');
    x.drawImage(plateCanvas(d, ctx), 0, 0);
    if (o.choosing) {                                   // the choice, on the backing the bake gives it
      var bw = 380, bh = WORD_PX * 2;
      x.fillStyle = groundAt(d, L.head); roundRect(x, CX - bw / 2, L.head - bh / 2, bw, bh, bh / 2); x.fill();
      text(x, o.choice || 'Focus 3', CX, L.head, d.bodyFace, 22, d.accent);
    }
    text(x, v.phase, CX, L.phase, d.bodyFace, WORD_PX, v.phase === 'Focus' ? d.ink : d.accent);
    drawTime(d, ctx, x, o.time || v.time);
    if (v.count) text(x, v.count, L.countX || CX, L.count, d.quietFace, SMALL_PX, d.ink);
    var st = o.waiting ? 'Waiting' : o.paused ? 'Paused' : v.state;
    if (st) text(x, st, CX, L.state, d.quietFace, SMALL_PX, o.waiting ? d.accent : d.ink);

    x.drawImage(dotFrame(d, v.filled, v.cells), CX - DOT_W / 2, L.dots - DOT_H / 2);
    // pomodoro-editor §7's second ring preview: the look's glass under another app's words, so a
    // ring colour is judged over what it will actually stand on, not only on its own ground.
    if (o.other) otherApp(x, d);
    if (o.ring) {                                       // the OS's own ring, as the looks preview shows it
      var col = d.ring[({ focus: 'focus', short: 'short', long: 'long' })[o.view || 'focus']];
      x.lineWidth = 7; x.lineCap = 'butt'; x.strokeStyle = rgba(col, 0.9 * 0.85);
      x.beginPath(); x.arc(CX, CX, 227.5, -Math.PI / 2, -Math.PI / 2 + (v === VIEWS.focus ? 0.62 : 0.78) * 2 * Math.PI); x.stroke();
    }
    return size === G || !size ? c : shrink(c, size);
  }
  // a stand-in for someone else's app, as the looks sketch draws it (docs/specs/pomodoro-looks-
  // preview.html's drawOtherApp, by value): the look's own ground, its rows covered by another
  // app's panel and words, so a ring colour is seen over a busy screen and not only its own.
  function otherApp(x, d) {
    var w = 336, h = 248, px = CX - w / 2, py = 126, r = 26;
    x.fillStyle = rgba(d.groundFoot, 0.92); roundRect(x, px, py, w, h, r); x.fill();
    text(x, '18°', CX + 10, py + 92, d.bodyFace, 84, '#eef4fb');
    text(x, 'Partly cloudy', CX, py + 160, d.bodyFace, 26, '#9db8d6');
    x.strokeStyle = '#ffd27a'; x.lineWidth = 6; x.beginPath(); x.arc(CX - 108, py + 34, 20, 0, 2 * Math.PI); x.stroke();
    text(x, 'weather \u00b7 another app', CX, py + 212, d.quietFace, 16, '#6f8aa8');
  }
  // the time, as the device sets it: the baked glyph cells, one advance apart, centred
  function drawTime(d, ctx, x, s, col) {
    var adv = ctx.cell.adv, ch = ('' + s).split(''), w = 0;
    ch.forEach(function (c) { w += c === ' ' ? ctx.cell.space : adv; });
    var px = CX - w / 2, top = LAYOUT[d.look].time - ctx.cell.h / 2;
    x.save();
    ch.forEach(function (c) {
      var g = c === ' ' ? null : ctx.fig.glyphs[CHARSET.indexOf(c)];
      if (g) x.drawImage(g, px, top);
      px += adv;
    });
    x.restore();
  }
  /* ---- the values: pm_values_t, little-endian (apps/pomodoro/pomodoro.h) ---- */
  var HEX6 = /^#[0-9a-f]{6}$/i;
  // the figures' own ink: Station's digits are lit on black flaps, every other look's
  // figures are its ink (the sketch's flapTiles colour, taken by value)
  function figInk(d) { return HEX6.test(d.figInk) ? d.figInk : (LOOKS[d.look].fig || d.ink); }
  function valuesBytes(d) {
    d = complete(d);
    var b = new Uint8Array(16), dv = new DataView(b.buffer);
    [d.ring.focus, d.ring.short, d.ring.long].forEach(function (c, k) {
      dv.setUint32(4 * k, HEX6.test(c) ? parseInt(c.slice(1), 16) & 0xFFFFFF : 0, true);
    });
    b[12] = 1; b[13] = 2; b[14] = 3; b[15] = 0;                 // the slots this bake writes (§2)
    return b;
  }

  /* ---- the chimes: the design's three phase ends, as the slots the values name ----
   * Slot 1 is a focus's end and strikes the recipe plain; slots 2 and 3 are the breaks' ends
   * and strike its brighter variant (owner question 2), so a design that changes nothing
   * pays two waves — the kit's sounds() shares one plane between equal waveforms. */
  function pcmOf(u) {
    if (!u) throw new Error('an upload with nothing in it: a chosen upload names a wave this design carries');
    var a = u.pcm !== undefined ? u.pcm : u.samples !== undefined ? u.samples : u;
    if (typeof a.length === 'number' && a.length >= 0 && typeof a === 'object' && !Array.isArray(a) && a.length) {
      var out = new Int16Array(a.length);
      for (var i = 0; i < a.length; i++) out[i] = a[i] | 0;
      return out;
    }
    if (Array.isArray(a)) return Int16Array.from(a.map(function (v) { return v | 0; }));
    if (a instanceof Int16Array) return a;
    if (a && typeof a === 'object') {                            // a JSON round trip can leave {"0":v,"1":v}
      var ks = Object.keys(a).filter(function (k) { return /^\d+$/.test(k); }).sort(function (p, q) { return p - q; });
      if (ks.length) { var o2 = new Int16Array(ks.length); ks.forEach(function (k, j) { o2[j] = a[k] | 0; }); return o2; }
    }
    throw new Error('an upload this bake cannot read as samples: pass an Int16Array, an array, or their JSON form');
  }
  // what the design asks each slot to strike: {slot, rec} or {slot, upload}, plus the variant
  function soundPlan(d) {
    var plan = [{ slot: 1, sel: d.chimes.focus, variant: '' },
                { slot: 2, sel: d.chimes.short, variant: 'brighter' },
                { slot: 3, sel: d.chimes.long, variant: 'brighter' }];
    return plan.map(function (p) {
      var s = p.sel || {};
      if (s.upload !== undefined) {
        var u = (d.uploads || [])[s.upload];
        if (!u) throw new Error('chime ' + p.slot + ' names upload ' + s.upload + ' and this design carries ' + (d.uploads || []).length);
        var pcm = pcmOf(u);
        if (pcm.length < 2) throw new Error('upload ' + s.upload + ' carries ' + pcm.length + ' samples: a chime is at least two');
        if (pcm.length > UPLOAD_MAX_S * rate())
          throw new Error('upload ' + s.upload + ' is ' + (pcm.length / rate()).toFixed(1) + ' s: a pomodoro chime is at most ' + UPLOAD_MAX_S.toFixed(1) + ' s');
        return { slot: p.slot, pcm: pcm, name: u.name || ('upload ' + s.upload) };
      }
      if (s.rec === undefined) throw new Error('chime ' + p.slot + ' names neither a recipe nor an upload');
      var rec = CHIMES[s.rec];
      if (!rec) throw new Error('chime ' + p.slot + ' names ' + JSON.stringify(s.rec) + ': no such recipe (the set is ' + CHIME_KEYS.join(', ') + ')');
      var pcm2 = root.GorbBake.synth(rec, { variant: p.variant, max_s: UPLOAD_MAX_S });
      return { slot: p.slot, pcm: pcm2, name: rec.label + (p.variant ? ' (brighter)' : '') };
    });
  }

  // the bake's serial is its content (radar-bake.js, weather-bake.js): FNV-1a, never 0 or 1
  function serialOf(d, doc) {
    var bytes = doc && doc.length ? doc : new TextEncoder().encode(JSON.stringify(d)), h = 0x811c9dc5;
    for (var i = 0; i < bytes.length; i++) { h ^= bytes[i]; h = Math.imul(h, 0x01000193) >>> 0; }
    return h > 1 ? h : h + 2;
  }
  // the colour the OS draws its overlays in over this look (§64.9): the owner's, or the
  // look's own first ring colour — the container's word, and the design's field
  function overlayOf(d) {
    if (d && HEX6.test(d.overlay)) return d.overlay;
    var r = d && d.ring && d.ring.focus;
    return HEX6.test(r) ? r : '#D62828';
  }
  function overlayWord(d) { return (0x01000000 | (parseInt(overlayOf(d).slice(1), 16) & 0xFFFFFF)) >>> 0; }

  // every refusal the bake makes, in one place and before a byte of art is drawn
  function refuse(d) {
    if (!d || typeof d !== 'object') throw new Error('a design that is not an object');
    if (!LOOKS[d.look]) throw new Error('look ' + JSON.stringify(d.look) + ': no such look (' + LOOK_KEYS.join(', ') + ')');
    if (!(d.px >= PX_MIN && d.px <= PX_MAX))
      throw new Error('the time at ' + d.px + ' px: the figures are ' + PX_MIN + '..' + PX_MAX + ' px');
    if (!HEX6.test(d.ink) || !HEX6.test(d.accent) || !HEX6.test(d.quiet))
      throw new Error('the look inks a colour that is not #rrggbb');
    ['focus', 'short', 'long'].forEach(function (ph) {
      if (!HEX6.test(d.ring[ph])) throw new Error('the ' + ph + ' ring colour ' + JSON.stringify(d.ring[ph]) + ': a ring colour is #rrggbb');
      var s = d.chimes[ph] || {};
      if (s.rec === undefined && s.upload === undefined) throw new Error('the ' + ph + ' chime names neither a recipe nor an upload');
    });
    (d.uploads || []).forEach(function (u, k) { var p = pcmOf(u); if (p.length > UPLOAD_MAX_S * rate())
      throw new Error('upload ' + k + ' is ' + (p.length / rate()).toFixed(1) + ' s: a pomodoro chime is at most ' + UPLOAD_MAX_S.toFixed(1) + ' s'); });
  }
  function shrink(c, size) {
    var cur = c;
    while (cur.width / 2 >= size) {
      var h = canvas(cur.width >> 1, cur.height >> 1), hx = h.getContext('2d');
      hx.imageSmoothingEnabled = true; hx.imageSmoothingQuality = 'high'; hx.drawImage(cur, 0, 0, h.width, h.height);
      cur = h;
    }
    var o = canvas(size, size), ox = o.getContext('2d');
    ox.imageSmoothingEnabled = true; ox.imageSmoothingQuality = 'high'; ox.drawImage(cur, 0, 0, size, size);
    return o;
  }

  /* ---- the bake ---- */
  async function bake(d, o) {
    o = o || {};
    if (!d || !LOOKS[d.look]) throw new Error('look ' + JSON.stringify(d && d.look) + ': no such look (' + LOOK_KEYS.join(', ') + ')');
    d = complete(d);
    refuse(d);
    var GB = root.GorbBake, St = GB.stage3({ dither: true });
    var AUX = GB.classId('aux'), TEXT = GB.classId('text'), PLATE = GB.classId('plate');
    var ORDER = 2;                                       // the app poses nothing: each row stands where it is baked

    // the chimes first: their planes go in at the head of the bank, which is what moves the
    // plate and sets plate_off (design §3), so this call runs before the first row()
    var chimes = soundPlan(d);
    St.sounds(chimes.map(function (c) { return { slot: c.slot, pcm: c.pcm, name: c.name }; }), { pattern: 'none' });

    await loadFaces(d);
    var fig = await font(d.face, d.px, figInk(d), { keep: FIGURES, figures: true });
    var words = await font(d.bodyFace, WORD_PX, d.accent, {});
    var small = await font(d.quietFace, SMALL_PX, d.ink, {});
    var ctx = { cell: { w: fig.w, h: fig.h, adv: fig.adv, space: fig.space } };

    // one plate: the ground and the instrument as one opaque panel. No plate at slot 1 —
    // that slot is the OS's glass overlay, painted over every row this app owns, and the
    // furniture is not an overlay (plateCanvas). The labels and dots ride above it.
    St.row({ n: St.id(), class: PLATE, slot: 0, x: 0, y: 0, w: G, h: G, order: 0, alpha: 255, n_frames: 1 }, plane(plateCanvas(d, ctx)), false, true);

    // the dots at 13 and 14: a hollow track per cell count, a fill per count, the app
    // picking both off pm_dots and centring them with one shared pose offset
    St.rowPick(Object.assign({ n: St.id(), class: AUX, w: DOT_W, h: DOT_H, pivot_x: DOT_W >> 1, pivot_y: DOT_H >> 1, alpha: 255 },
      { slot: SLOT.dots, x: CX - (DOT_W >> 1), y: LAYOUT[d.look].dots - (DOT_H >> 1), order: ORDER }),
      dotTrack(d).map(plane), true, false);
    St.rowPick(Object.assign({ n: St.id(), class: AUX, w: DOT_W, h: DOT_H, pivot_x: DOT_W >> 1, pivot_y: DOT_H >> 1, alpha: 255 },
      { slot: SLOT.fill, x: CX - (DOT_W >> 1), y: LAYOUT[d.look].dots - (DOT_H >> 1), order: ORDER + 1 }),
      dotFill(d).map(plane), true, false);

    [fig, words, small].forEach(function (fo, fi) {
      St.font({ n: St.id(), class: TEXT, slot: fi, x: 0, y: 0, w: fo.w, h: fo.h, order: 0,
        pivot_x: Math.min(fo.space, fo.w), pivot_y: fo.track }, fo.glyphs.map(planeOrNull), [figInk(d), d.accent, d.ink][fi]);
    });

    // the labels, each box centred where the look's rows stand (§2's table): the app poses at
    // 0,0, so the bake places them. Tones: the time is figures at every tone, so it rides its
    // own cell under the warning tone too; the phase's accent tone is a break's colour; the
    // choice's tone is the accent always. The quiet tone shares the small ink font — three
    // fonts, four tones (§5) — because the accent is the one that has to be itself.
    var L = LAYOUT[d.look], TB = timeBox(d, ctx);
    var LAB = [
      { slot: SLOT.time,   t: [0, NONE, NONE, 0], w: 5 * TB.adv + 8, h: fig.h + 4, x: CX, y: L.time, pad: 0 },
      { slot: SLOT.phase,  t: [2, 1, NONE, NONE], w: 300, h: small.h + 4, x: CX, y: L.phase, pad: 0 },
      { slot: SLOT.count,  t: [2, NONE, NONE, NONE], w: 200, h: small.h + 4, x: L.countX || CX, y: L.count, pad: 0 },
      { slot: SLOT.state,  t: [2, NONE, NONE, 1], w: 220, h: small.h + 4, x: CX, y: L.state, pad: 0 },
      { slot: SLOT.head,   t: [1, 1, 1, 1], w: 380, h: WORD_PX * 2, x: CX, y: L.head, pad: WORD_PX, backing: true }
    ];
    LAB.forEach(function (l) {
      var panel = canvas(l.w, l.h);
      if (l.backing) {                     // a backing in the ground's own colour at this row, so
        var x = panel.getContext('2d'), r = l.h / 2;   // the choice replaces the caption it covers
        x.fillStyle = groundAt(d, l.y);
        x.beginPath(); x.moveTo(r, 0); x.arcTo(l.w, 0, l.w, l.h, r); x.arcTo(l.w, l.h, 0, l.h, r);
        x.arcTo(0, l.h, 0, 0, r); x.arcTo(0, 0, l.w, 0, r); x.closePath(); x.fill();
      }
      St.row(Object.assign({ n: St.id(), class: AUX, kind: 2, src: GB.SOURCES.indexOf('text'), w: l.w, h: l.h,
        pivot_x: l.w >> 1, pivot_y: l.h >> 1, alpha: 255, n_frames: 1, str: tones(l.t), digit: 1, end: 1, radius: l.backing ? 8 : 1 },
        { slot: l.slot, x: l.x - (l.w >> 1), y: l.y - (l.h >> 1), order: ORDER }), plane(panel), true, false);
    });
    function planeOrNull(g) { return g ? plane(g) : null; }
    function tones(t) { return t.reduce(function (s, fn, k) { return s + (fn << (4 * k)); }, 0); }

    St.values(valuesBytes(d), VALUES_VER);
    St.preview({ plane: plane(await picture(d, G, { view: 'focus' })), x: 0, y: 0, order: 3 });
    // TODO(§64.9): the overlay colour ships when the OS reads one — values ver 2, or a new scalar.
    // Until then it rides the design region, which is where the next slice will find it.
    var out = St.finish({ name: d.name || 'Pomodoro', slug: 'pomodoro', serial: o.serial || serialOf(d, o.design),
                          abi: GB.GEOM_ABI6, design: o.design || null, overlay: overlayWord(d),
                          bakedS: o.bakedS != null ? o.bakedS : Math.floor(Date.now() / 1000) });
    return { bytes: out.bytes, meter: out.meter, est: out.est, rows: St.elements.length, cell: ctx.cell,
             sounds: chimes.map(function (c) { return { slot: c.slot, name: c.name, samples: c.pcm.length }; }) };
  }

  root.PomodoroBake = {
    SCHEMA: SCHEMA, G: G, CX: CX, SLOT: SLOT, VALUES_VER: VALUES_VER, N_TRACK: N_TRACK, N_FILL: N_FILL,
    FACES: FACES, CHIMES: CHIMES, CHIME_KEYS: CHIME_KEYS, DOTS: DOTS, DOT_TITLES: DOT_TITLES,
    PARTS: PARTS, LAYOUT: LAYOUT, LOOKS: LOOK_KEYS, PRESETS: LOOK_KEYS.slice(),
    STARTERS: STARTERS, STARTER_TITLES: STARTER_TITLES, STARTER_NOTES: STARTER_NOTES,
    VIEWS: Object.keys(VIEWS), PX_MIN: PX_MIN, PX_MAX: PX_MAX, UPLOAD_MAX_S: UPLOAD_MAX_S, PAD_ALPHA: PAD_ALPHA,
    preset: preset, blank: blank, complete: complete, scheme: scheme, frame: frame,
    groundCanvas: groundCanvas, plateCanvas: plateCanvas, furnitureCanvas: furnitureCanvas, fontContext: fontContext,
    dotFrame: dotFrame, dotTrack: dotTrack, dotFill: dotFill, picture: picture, shrink: shrink, font: font,
    valuesBytes: valuesBytes, soundPlan: soundPlan, serialOf: serialOf, overlayOf: overlayOf,
    overlayWord: overlayWord, bake: bake
  };
})(typeof window !== 'undefined' ? window : globalThis);
