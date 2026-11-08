/* radar-bake.js — the radar app's theme, baked in the browser (docs/radar-plan.md §2.5,
 * §2.9, §2.11).
 *
 * A design (schema 1) drawn on canvases and assembled by the OS's stage bake
 * (GorbBake.stage3) into what apps/radar/main.c reads:
 *   - the plate: the ground under the map, shown where the map is clear or absent;
 *   - the glass: the range rings, the crosshair and the time track on the rim, over the
 *     map and the rain (the live layers stand between the plate and the rows);
 *   - slot 0: the place's dot; slot 1: the time cursor, a dot on the rim the app turns
 *     (1.5 degrees a minute from Now at the top);
 *   - fonts 0 (ink), 1 (forecast: the ink's glyphs in the forecast colour), 2 (quiet),
 *     3 (the zoom's word, large), 4 (the scale bar's length: words.scale);
 *   - the labels: 8 the time, 9 the place, 10 the scale bar's length, 11 the status (on a backing),
 *     12..17 the landmarks' names, 18 the zoom while it changes (large, on a backing);
 *   - the values (layout 2, apps/radar/radar.h rd_values_t): the map's role palette,
 *     widths, dashes and what it draws, the names shown, the two live layers' depths, the
 *     rain ramp as 256 ARGB words, and the first zoom step each role is drawn at;
 *   - the picker's preview.
 * R5 bakes the one starter the shelf ships (Scope); the editor around this (presets, the
 * map's detail and colours, Paper, Station, Vintage, Blueprint) is R7's.
 * Needs a document (canvas, document.fonts) and GorbBake. */
(function (root) {
  'use strict';

  var SCHEMA = 1, G = 466, CX = 233, NONE = 0xF;
  var CHARSET = (function () { var c = []; for (var i = 0x20; i < 0x7F; i++) c.push(String.fromCharCode(i)); return c.concat(['↑', '↓', '→', '←', '°']); })();
  var SLOT = { dot: 0, hand: 1, time: 8, place: 9, range: 10, status: 11, name0: 12, zoom: 18 };
  var N_NAMES = 6;
  var VALUES_VER = 4;
  // the map's roles by index (OS v1.2.26's FORMAT 2): road1 motorway and trunk, road2 primary,
  // road3 secondary, road4 tertiary
  var ROLES = ['water', 'land', 'urban', 'lake', 'river', 'road2', 'road1', 'state', 'border', 'coast', 'city', 'airport', 'runway',
               'road3', 'road4'];
  var DASH = { solid: 0, short: 1, long: 2, dot: 3, dashdot: 4 };

  /* ---- the starters (R7 adds the rest) ---- */
  var PRESETS = {
    scope: {
      name: 'Radarscope',           // the shelf file is its name (radarscope.orbtheme): weather's is Scope
      ground: { top: '#0b1822', foot: '#05090d' },
      glass: { ring: '#5fd4c4', ringA: 0.30, rings: [0.5, 1.0], cross: true, track: '#5fd4c4', trackA: 0.55, forecast: '#e0b040' },
      // 26 px: capitals 18 px tall, the floor for a 1.75 in glass read from 2 ft (weather-plan
      // §2.8; owner, 2026-11-08: the 18 and 14 px words were far too small)
      words: { face: 'IBM Plex Mono', px: 26, ink: '#d8f4ee', forecast: '#e0b040', quiet: '#9cc2bc', namePx: 26, nameFace: 'IBM Plex Sans',
               zoomPx: 40, backing: 'rgba(4,10,14,0.78)',     // the zoom's word while it changes, on a backing
               scale: { face: 'IBM Plex Sans', px: 26, colour: '#d8f4ee' } },   // the scale bar's length
      // the scale bar (layout 3): ink and a dark edge, its thickness, end ticks, longest bar
      scale: { ink: '#d8f4ee', edge: '#05090d', width: 2, tick: 9, max: 150 },
      // drawn on the map, under the rain (layout 4): none here, Radarscope's rings are glass art over it
      under: { colour: null, rings: 0, cross: false, width: 1 },
      dot: { colour: '#ffffff', ring: '#e0b040', r: 4.5 },   // white: the airports are amber
      map: {
        water: '#0a2233', land: '#101a22', urban: '#18242e', lake: '#0e2c40', river: '#1f5a80', road2: '#4a5966',
        road1: '#6b7b88', state: '#3d4a56', border: '#6a7886', coast: '#2a5470', city: '#b8c4ce', airport: '#e0b040', runway: '#5b6b78',
        road3: '#3a4855', road4: '#2f3b46',
        width: { river: 1, road2: 1, road1: 2, state: 1, border: 2, coast: 1, runway: 2, road3: 1, road4: 1 },
        dash: { state: 'long' },
        // the first zoom step each is drawn at (Wide 0 .. Close 5): the roads thin out as the
        // view widens (owner, 2026-11-07)
        from: { road2: 3, road3: 4, road4: 5 },
        off: [], marks: ['city', 'airport'], names: 4
      },
      // dBZ stops: colour, opacity (the noise floor is 10 dBZ)
      rain: [[10, '#00c850', 0.60], [20, '#00e060', 0.75], [30, '#f0e020', 0.85], [40, '#f09020', 0.91],
             [50, '#e83020', 0.94], [60, '#e020d0', 1.0], [70, '#ffffff', 1.0]],
      order: { map: 0, rain: 1, rows: 2 }
    }
  };
  /* ---- R7: the other starters (radar-plan §2.9, §0 ruling 13), each matching the same-named
   * looks in Weather and Flights; built from a palette over Radarscope's shape ---- */
  function starter(name, P) {
    var d = JSON.parse(JSON.stringify(PRESETS.scope));
    d.name = name;
    d.ground = { top: P.top, foot: P.foot };
    Object.assign(d.glass, P.glass);
    Object.assign(d.words, P.words);
    d.words.scale = Object.assign({}, d.words.scale, { colour: P.words.ink });
    d.scale = Object.assign({}, d.scale, { ink: P.words.ink, edge: P.scaleEdge });
    d.dot = P.dot;
    Object.assign(d.map, P.map);
    if (P.dash) d.map.dash = P.dash;
    if (P.width) d.map.width = Object.assign({}, d.map.width, P.width);
    d.rain = P.rain;
    return d;
  }
  PRESETS.paper = starter('Paper', {
    top: '#f2ead8', foot: '#e3d7bd', scaleEdge: '#f2ead8',
    glass: { ring: '#5a4a36', ringA: 0.25, track: '#5a4a36', trackA: 0.55, forecast: '#b05a20' },
    words: { ink: '#2a2218', forecast: '#a04a18', quiet: '#5a4a36', backing: 'rgba(242,234,216,0.88)' },
    dot: { colour: '#c03a2a', ring: '#2a2218', r: 4.5 },
    map: { water: '#a9c7dd', land: '#efe6d2', urban: '#e2d4ba', lake: '#9fc1d9', river: '#7fa7c6', road2: '#d08a4a',
           road1: '#c0503a', state: '#8a7a9a', border: '#6a5a7a', coast: '#6f97b5', city: '#3a3226', airport: '#7a3a2a',
           runway: '#9a8f7a', road3: '#c4b494', road4: '#d2c6aa' },
    rain: [[10, '#58b858', 0.55], [20, '#2e9e3e', 0.70], [30, '#e8c020', 0.80], [40, '#e07818', 0.88],
           [50, '#c82818', 0.92], [60, '#a01890', 1.0], [70, '#401060', 1.0]]
  });
  PRESETS.station = starter('Station', {
    top: '#3a4552', foot: '#262e38', scaleEdge: '#141a20',
    glass: { ring: '#ffffff', ringA: 0.22, track: '#ffffff', trackA: 0.6, forecast: '#ffd040' },
    words: { ink: '#ffffff', forecast: '#ffd040', quiet: '#c8d2dc', backing: 'rgba(20,26,32,0.80)' },
    dot: { colour: '#ffd040', ring: '#141a20', r: 4.5 },
    map: { water: '#1d3c5a', land: '#3b4652', urban: '#47525e', lake: '#24486a', river: '#3a6a94', road2: '#8a96a2',
           road1: '#b0bac4', state: '#d8dee4', border: '#ffffff', coast: '#7aa4c8', city: '#ffffff', airport: '#ffd040',
           runway: '#8a96a2', road3: '#6a7682', road4: '#5a6672' },
    width: { state: 1, border: 2 },
    rain: [[10, '#00b000', 0.70], [20, '#00e000', 0.80], [30, '#ffff00', 0.88], [40, '#ff9000', 0.92],
           [50, '#ff0000', 0.95], [60, '#ff00ff', 1.0], [70, '#ffffff', 1.0]]
  });
  PRESETS.vintage = starter('Vintage', {
    top: '#dccaa2', foot: '#b89c6c', scaleEdge: '#e8dcc0',
    glass: { ring: '#5a3e1e', ringA: 0.30, track: '#5a3e1e', trackA: 0.6, forecast: '#8a2a1a' },
    words: { face: 'Cormorant Garamond', nameFace: 'Cormorant Garamond', ink: '#3a2810', forecast: '#8a2a1a', quiet: '#5a4228',
             backing: 'rgba(232,220,192,0.88)' },
    dot: { colour: '#8a2a1a', ring: '#3a2810', r: 4.5 },
    map: { water: '#a8b8a8', land: '#d8c8a0', urban: '#ccb890', lake: '#9cb0a0', river: '#7c947c', road2: '#9a6a3a',
           road1: '#7a4a2a', state: '#7a6a4a', border: '#5a3e1e', coast: '#6a8070', city: '#3a2810', airport: '#8a2a1a',
           runway: '#8a7a5a', road3: '#b0946a', road4: '#bea47a' },
    rain: [[10, '#8a9a5a', 0.50], [20, '#6a8a3a', 0.65], [30, '#c8a040', 0.78], [40, '#b8702a', 0.86],
           [50, '#9a3a1a', 0.92], [60, '#6a1a2a', 1.0], [70, '#3a0a1a', 1.0]]
  });
  PRESETS.blueprint = starter('Blueprint', {
    top: '#174a82', foot: '#0c2c58', scaleEdge: '#0c2c58',
    glass: { ring: '#cfe8ff', ringA: 0.35, track: '#cfe8ff', trackA: 0.6, forecast: '#ffe07a' },
    words: { ink: '#eaf4ff', forecast: '#ffe07a', quiet: '#a8cdf0', backing: 'rgba(12,44,88,0.82)' },
    dot: { colour: '#ffffff', ring: '#0c2c58', r: 4.5 },
    map: { water: '#0f3a6c', land: '#164a80', urban: '#1c5490', lake: '#123f74', river: '#6ab0e8', road2: '#8ec8f4',
           road1: '#cfe8ff', state: '#8ec8f4', border: '#eaf4ff', coast: '#a8d4f8', city: '#ffffff', airport: '#ffe07a',
           runway: '#a8cdf0', road3: '#6ab0e8', road4: '#4a90cc' },
    // thin and dashed (§0 ruling 13): state lines and minor roads dashed, borders long-dashed
    dash: { state: 'short', road3: 'short', road4: 'dot', border: 'long' },
    width: { road1: 1, border: 1, runway: 1 },
    // white to cyan, as contours
    rain: [[10, '#ffffff', 0.25], [20, '#e0f4ff', 0.40], [30, '#a0e0ff', 0.55], [40, '#60c8ff', 0.70],
           [50, '#20a8ff', 0.82], [60, '#ffe07a', 0.95], [70, '#ff8040', 1.0]]
  });
  PRESETS.blank = starter('Blank', {
    top: '#1c1c1c', foot: '#101010', scaleEdge: '#000000',
    glass: { ring: '#9a9a9a', ringA: 0.25, track: '#9a9a9a', trackA: 0.5, forecast: '#c8c8c8' },
    words: { ink: '#e8e8e8', forecast: '#c8c8c8', quiet: '#a8a8a8', backing: 'rgba(0,0,0,0.75)' },
    dot: { colour: '#ffffff', ring: '#000000', r: 4.5 },
    map: { water: '#202428', land: '#2a2a2a', urban: '#323232', lake: '#24282c', river: '#4a5058', road2: '#5a5a5a',
           road1: '#7a7a7a', state: '#4a4a4a', border: '#8a8a8a', coast: '#5a6068', city: '#d0d0d0', airport: '#d0d0d0',
           runway: '#6a6a6a', road3: '#484848', road4: '#3c3c3c' },
    rain: PRESETS.scope.rain
  });
  var STARTERS = ['scope', 'paper', 'station', 'vintage', 'blueprint'];
  var STARTER_TITLES = { scope: 'Radarscope', paper: 'Paper', station: 'Station', vintage: 'Vintage', blueprint: 'Blueprint', blank: 'Blank' };
  var STARTER_NOTES = { scope: 'Phosphor green on black', paper: 'A printed road atlas', station: 'The TV weather map',
    vintage: 'Brass and parchment, a sepia map', blueprint: 'Cyan lines on blueprint blue', blank: 'Plain greys' };

  /* ---- the Map step's presets (§2.9): detail, names, a colour scheme ---- */
  var DETAILS = {
    minimal: { title: 'Minimal', note: 'Major roads, borders, lakes and cities',
               off: ['urban', 'river', 'state', 'runway', 'road2', 'road3', 'road4'], from: {} },
    standard: { title: 'Standard', note: 'Adds rivers, towns\u2019 built-up areas, state lines, runways and main roads from Area in',
                off: ['road3', 'road4'], from: { road2: 3 } },
    detailed: { title: 'Detailed', note: 'Adds the smaller roads, each from its own zoom in',
                off: [], from: { road2: 3, road3: 4, road4: 5 } }
  };
  var NAMES = { off: { title: 'Off', n: 0 }, few: { title: 'Few', n: 2 }, normal: { title: 'Normal', n: 4 }, many: { title: 'Many', n: 6 } };
  var COLOUR_ROLES = ['water', 'land', 'urban', 'lake', 'river', 'coast', 'road1', 'road2', 'road3', 'road4', 'state', 'border', 'runway', 'city', 'airport'];
  var ROLE_TITLES = { water: 'Sea', land: 'Land', urban: 'Built-up', lake: 'Lakes', river: 'Rivers', coast: 'Coast',
    road1: 'Motorways', road2: 'Main roads', road3: 'Secondary roads', road4: 'Minor roads', state: 'State lines',
    border: 'Borders', runway: 'Runways', city: 'Town dots', airport: 'Airports' };
  var LINE_ROLES = ['river', 'coast', 'road1', 'road2', 'road3', 'road4', 'state', 'border', 'runway'];
  function detailOf(m) {
    for (var k in DETAILS) {
      var D = DETAILS[k];
      if (D.off.slice().sort().join() === (m.off || []).slice().sort().join() && JSON.stringify(D.from) === JSON.stringify(m.from || {})) return k;
    }
    return 'custom';
  }
  function namesOf(m) { for (var k in NAMES) if (NAMES[k].n === (m.names | 0)) return k; return 'normal'; }
  // a starter's map colours over a design, its line weights and dashes kept
  function scheme(d, k) {
    var src = PRESETS[k].map, n = JSON.parse(JSON.stringify(d));
    COLOUR_ROLES.forEach(function (r) { n.map[r] = src[r]; });
    return n;
  }
  // a design from before a part was added: the missing parts from Radarscope
  function complete(d) {
    var base = preset('scope'), out = JSON.parse(JSON.stringify(d));
    (function fill(o, b) {
      for (var k in b) {
        if (o[k] === undefined) o[k] = JSON.parse(JSON.stringify(b[k]));
        else if (b[k] && typeof b[k] === 'object' && !Array.isArray(b[k]) && o[k] && typeof o[k] === 'object') fill(o[k], b[k]);
      }
    })(out, base);
    return out;
  }

  function preset(k) { return JSON.parse(JSON.stringify(PRESETS[k] || PRESETS.scope)); }
  // the faces the Words step offers: the hub's own (runtime/vendor/fonts.css), each with
  // the accented letters (§6.22)
  var FACES = ['IBM Plex Sans', 'IBM Plex Mono', 'Space Grotesk', 'Space Mono', 'Jost', 'Barlow Condensed', 'Oswald',
               'Cormorant Garamond', 'Playfair Display', 'Bodoni Moda'];
  // the rain's colour per raw value, as valuesBytes writes it (the preview's rain): rgba
  // bytes a value, from the 10 dBZ floor up, under `floorDbz` none
  function rainLut(d, floorDbz) {
    var out = new Uint8ClampedArray(256 * 4), f = floorDbz == null ? 10 : floorDbz;
    for (var i = 0; i < 256; i++) {
      var dbz = i / 2 - 32;
      if (i < 84 || dbz < f) continue;
      var k = 0;
      while (k < d.rain.length - 1 && dbz >= d.rain[k + 1][0]) k++;
      var c = hex(d.rain[k][1]);
      out[4 * i] = c[0]; out[4 * i + 1] = c[1]; out[4 * i + 2] = c[2]; out[4 * i + 3] = Math.round(d.rain[k][2] * 255);
    }
    return out;
  }

  /* ---- drawing helpers ---- */
  function canvas(w, h) { var c = document.createElement('canvas'); c.width = w; c.height = h; return c; }
  function plane(c) {
    var x = c.getContext('2d').getImageData(0, 0, c.width, c.height);
    return { w: c.width, h: c.height, rgba: new Uint8Array(x.data.buffer) };
  }
  function hex(c) { return [parseInt(c.slice(1, 3), 16), parseInt(c.slice(3, 5), 16), parseInt(c.slice(5, 7), 16)]; }
  function argb(c, a) { var h = hex(c); return ((Math.round((a === undefined ? 1 : a) * 255) << 24) | (h[0] << 16) | (h[1] << 8) | h[2]) >>> 0; }
  function rgba(c, a) { var h = hex(c); return 'rgba(' + h[0] + ',' + h[1] + ',' + h[2] + ',' + a + ')'; }
  async function loadFace(face, px, text) {
    try { await document.fonts.load(px + 'px "' + face + '"', text || 'N0123456789'); } catch (e) { /* the fallback face draws */ }
  }

  function plateCanvas(d) {
    var c = canvas(G, G), x = c.getContext('2d'), g = x.createRadialGradient(CX, CX, 0, CX, CX, CX);
    g.addColorStop(0, d.ground.top); g.addColorStop(1, d.ground.foot);
    x.fillStyle = g; x.fillRect(0, 0, G, G);
    return c;
  }
  // the time track: 18 ticks, Now at the top, the past anticlockwise 1.5 degrees a minute,
  // the nowcast clockwise in the forecast colour
  function glassCanvas(d) {
    var c = canvas(G, G), x = c.getContext('2d'), gl = d.glass;
    x.strokeStyle = rgba(gl.ring, gl.ringA); x.lineWidth = 1;
    gl.rings.forEach(function (f) { x.beginPath(); x.arc(CX, CX, f * (CX - 1) - 0.5, 0, 2 * Math.PI); x.stroke(); });
    if (gl.cross) {
      x.setLineDash([2, 6]);
      x.beginPath(); x.moveTo(CX, 30); x.lineTo(CX, G - 30); x.moveTo(30, CX); x.lineTo(G - 30, CX); x.stroke();
      x.setLineDash([]);
    }
    for (var m = -110; m <= 60; m += 10) {
      var a = (m * 1.5 - 90) * Math.PI / 180, r0 = CX - 14, r1 = CX - (m % 60 === 0 ? 4 : 8);
      x.strokeStyle = rgba(m > 0 ? gl.forecast : gl.track, gl.trackA);
      x.lineWidth = m === 0 ? 2.5 : 1.5;
      x.beginPath(); x.moveTo(CX + r0 * Math.cos(a), CX + r0 * Math.sin(a)); x.lineTo(CX + r1 * Math.cos(a), CX + r1 * Math.sin(a)); x.stroke();
    }
    return c;
  }
  function dotCanvas(d) {
    var s = 16, c = canvas(s, s), x = c.getContext('2d');
    x.fillStyle = d.dot.colour; x.beginPath(); x.arc(s / 2, s / 2, d.dot.r, 0, 2 * Math.PI); x.fill();
    x.strokeStyle = d.dot.ring || 'rgba(0,0,0,0.6)'; x.lineWidth = 2; x.stroke();
    return c;
  }
  // the time cursor: a dot at the rim's tick radius, its pivot the glass's centre
  var HAND_W = 12, HAND_H = CX + 1;          // one past the centre, so the pivot is inside it
  function handCanvas(d) {
    var c = canvas(HAND_W, HAND_H), x = c.getContext('2d');
    x.fillStyle = d.glass.track; x.beginPath(); x.arc(HAND_W / 2, 18, 4.5, 0, 2 * Math.PI); x.fill();
    return c;
  }

  // one font: every printable ASCII glyph in one cell, its ink from the cell's left edge on
  // a shared baseline (the weather and flights bakes' rule)
  // radar-plan §6.22: the charset a font bakes, the dotless i and the accent marks after
  // the ASCII and the symbols (GorbBake.FONT_MORE: a mark measures as its carrier letter,
  // its height the accented letter's, and is drawn by GorbBake.drawGlyph)
  function fontSet() { return CHARSET.concat(root.GorbBake.FONT_MORE); }
  function fontText(set) { return set.map(function (e) { return typeof e === 'string' ? e : e.acc + e.car; }).join(''); }
  function boxOf(m, e) {
    var t = m.measureText(typeof e === 'string' ? e : e.car), v = typeof e === 'string' ? t : m.measureText(e.acc);
    return { l: Math.floor(-t.actualBoundingBoxLeft), r: Math.ceil(t.actualBoundingBoxRight),
      t: Math.ceil(v.actualBoundingBoxAscent), b: Math.ceil(v.actualBoundingBoxDescent),
      asc: t.fontBoundingBoxAscent, desc: t.fontBoundingBoxDescent };
  }
  // `plain`: a font that never draws a name (the time, the zoom's word, the scale's length)
  // keeps to the ASCII and the symbols: the marks' cells, and the taller cell the accented
  // capitals want, go where names are drawn
  async function font(face, px, colour, plain) {
    var spec = px + 'px "' + face + '"', CS = plain ? CHARSET : fontSet();
    await loadFace(face, px, fontText(CS));
    var m = canvas(4, 4).getContext('2d');
    m.font = spec;
    var top = 0, bottom = 0, w = 1, boxes = CS.map(function (ch) { return boxOf(m, ch); });
    boxes.forEach(function (b) { if (b.r > b.l) { top = Math.max(top, b.t); bottom = Math.max(bottom, b.b); w = Math.max(w, b.r - b.l); } });
    var fm = m.measureText('Hg');
    top = Math.max(top, Math.ceil(fm.fontBoundingBoxAscent || 0)) + 1;
    bottom = Math.max(bottom, Math.ceil(fm.fontBoundingBoxDescent || 0)) + 1;
    var h = top + bottom, glyphs = boxes.map(function (b, i) {
      var c = canvas(w, h), x = c.getContext('2d');
      x.font = spec; x.textBaseline = 'alphabetic'; x.fillStyle = colour;
      if (b.r > b.l) root.GorbBake.drawGlyph(x, CS[i], -b.l, top);
      return c;
    });
    return { w: w, h: h, glyphs: glyphs, space: Math.max(1, Math.round(m.measureText(' ').width)), track: 1 };
  }

  /* ---- the values: rd_values_t, little-endian ---- */
  function valuesBytes(d) {
    var b = new Uint8Array(64 + 16 + 16 + 4 + 4 + 1024 + 16 + 16 + 16), dv = new DataView(b.buffer), M = d.map, i;
    ROLES.forEach(function (r, k) { dv.setUint32(4 * k, argb(M[r], 1), true); });
    ROLES.forEach(function (r, k) {
      b[64 + k] = Math.max(0, Math.min(4, (M.width && M.width[r]) | 0));
      b[80 + k] = DASH[(M.dash && M.dash[r]) || 'solid'] | 0;
    });
    var on = 0;
    ROLES.forEach(function (r, k) { if ((M.off || []).indexOf(r) < 0) on |= 1 << k; });
    dv.setUint16(96, on, true);
    var marks = { city: 1, town: 2, village: 4, airport: 8 }, mo = 0;
    (M.marks || []).forEach(function (k) { mo |= marks[k] | 0; });
    b[98] = mo;
    b[99] = Math.max(0, Math.min(N_NAMES, M.names | 0));
    b[100] = d.order.map; b[101] = d.order.rain;
    for (i = 0; i < 256; i++) {
      var dbz = i / 2 - 32, w = 0;
      if (i >= 84) {
        var k = 0;
        while (k < d.rain.length - 1 && dbz >= d.rain[k + 1][0]) k++;
        w = argb(d.rain[k][1], d.rain[k][2]);
      }
      dv.setUint32(104 + 4 * i, w, true);
    }
    ROLES.forEach(function (r, k) { b[1128 + k] = Math.max(0, Math.min(5, (M.from && M.from[r]) | 0)); });
    var S = d.scale || {};                                 // layout 3: the scale bar
    dv.setUint32(1144, argb(S.ink || '#d8f4ee', 1), true);
    dv.setUint32(1148, S.edge ? argb(S.edge, 1) : 0, true);
    b[1152] = Math.max(2, Math.min(8, S.width == null ? 2 : S.width | 0));
    b[1153] = Math.max(0, Math.min(24, S.tick == null ? 9 : S.tick | 0));
    b[1154] = Math.max(60, Math.min(200, S.max == null ? 150 : S.max | 0));
    var U = d.under || {};                                 // layout 4: under the rain
    dv.setUint32(1160, U.colour ? argb(U.colour, U.alpha == null ? 1 : U.alpha) : 0, true);
    b[1164] = Math.max(0, Math.min(6, U.rings | 0));
    b[1165] = U.cross ? 1 : 0;
    b[1166] = Math.max(1, Math.min(3, U.width == null ? 1 : U.width | 0));
    return b;
  }

  /* ---- the picture: the picker's preview and the hub's list ---- */
  async function picture(d, size) {
    var c = canvas(G, G), x = c.getContext('2d');
    x.drawImage(plateCanvas(d), 0, 0);
    // a hint of the map and a squall line, so the thumbnail reads as a radar
    x.strokeStyle = d.map.road1; x.lineWidth = 3;
    [[60, 120, 400, 300], [120, 420, 330, 40], [20, 250, 450, 220]].forEach(function (l) { x.beginPath(); x.moveTo(l[0], l[1]); x.lineTo(l[2], l[3]); x.stroke(); });
    var blobs = [[150, 170, 70, 0], [210, 140, 45, 2], [250, 190, 28, 4], [300, 260, 55, 1]];
    blobs.forEach(function (b) { var s = d.rain[b[3]]; x.fillStyle = rgba(s[1], s[2]); x.beginPath(); x.arc(b[0], b[1], b[2], 0, 2 * Math.PI); x.fill(); });
    x.drawImage(glassCanvas(d), 0, 0);
    x.drawImage(dotCanvas(d), CX - 8, CX - 8);
    await loadFace(d.words.face, 40);
    x.fillStyle = d.words.ink; x.font = '40px "' + d.words.face + '"'; x.textAlign = 'center'; x.textBaseline = 'middle';
    x.fillText('Now', CX, 92);
    var o = canvas(size, size);
    o.getContext('2d').drawImage(c, 0, 0, size, size);
    return o;
  }

  /* ---- the bake ---- */
  async function bake(d, o) {
    o = o || {};
    var GB = root.GorbBake, St = GB.stage3({ dither: true }), AUX = GB.classId('aux'), TEXT = GB.classId('text'), PLATE = GB.classId('plate');
    var rowsOrder = d.order.rows;
    St.row({ n: St.id(), class: PLATE, slot: 0, x: 0, y: 0, w: G, h: G, order: 0, alpha: 255, n_frames: 1 }, plane(plateCanvas(d)), false, true);
    St.row({ n: St.id(), class: PLATE, slot: 1, x: 0, y: 0, w: G, h: G, order: 0, alpha: 255, n_frames: 1 }, plane(glassCanvas(d)), true, true);
    var centred = function (w, h, slot) { return { slot: slot, x: CX - (w >> 1), y: CX - (h >> 1), order: rowsOrder }; };
    St.row(Object.assign({ n: St.id(), class: AUX, w: 16, h: 16, pivot_x: 8, pivot_y: 8, alpha: 255, n_frames: 1 }, centred(16, 16, SLOT.dot)),
      plane(dotCanvas(d)), true, false);
    St.row({ n: St.id(), class: AUX, slot: SLOT.hand, x: CX - HAND_W / 2, y: 0, w: HAND_W, h: HAND_H, pivot_x: HAND_W / 2, pivot_y: CX,
      order: rowsOrder, alpha: 255, n_frames: 1 }, plane(handCanvas(d)), true, false);
    var SW = W_scale(d.words);
    var W = d.words, F = [await font(W.face, W.px, W.ink), await font(W.face, W.px, W.forecast, true), await font(W.nameFace, W.namePx, W.quiet),
      await font(W.face, W.zoomPx || 34, W.ink, true), await font(SW.face, SW.px, SW.colour, true)];
    var colours = [W.ink, W.forecast, W.quiet, W.ink, SW.colour];
    F.forEach(function (fo, fi) {
      St.font({ n: St.id(), class: TEXT, slot: fi, x: 0, y: 0, w: fo.w, h: fo.h, order: 0, pivot_x: Math.min(fo.space, fo.w), pivot_y: fo.track },
        fo.glyphs.map(plane), colours[fi]);
    });
    var tones = function (t) { return t.reduce(function (s, fn, k) { return s + (fn << (4 * k)); }, 0); };
    var L = [{ slot: SLOT.time, t: [0, 1, NONE, NONE], w: 200, f: 0 }, { slot: SLOT.place, t: [0, NONE, NONE, NONE], w: 320, f: 0 },
             { slot: SLOT.range, t: [4, NONE, NONE, NONE], w: 200, f: 4 },
             { slot: SLOT.status, t: [2, NONE, NONE, NONE], w: 300, f: 2, backing: true, grow: 10 },
             { slot: SLOT.zoom, t: [3, NONE, NONE, NONE], w: 380, f: 3, backing: true, grow: 16 }];
    for (var k = 0; k < N_NAMES; k++) L.push({ slot: SLOT.name0 + k, t: [2, NONE, NONE, NONE], w: 240, f: 2 });
    L.forEach(function (l) {
      var h = F[l.f].h + 2 + (l.grow || 0), panel = canvas(l.w, h);
      if (l.backing) {           // a rounded backing the words stand on, legible over map and rain
        var x = panel.getContext('2d'), r = h / 2;
        x.fillStyle = W.backing || 'rgba(4,10,14,0.78)';
        x.beginPath(); x.moveTo(r, 0); x.arcTo(l.w, 0, l.w, h, r); x.arcTo(l.w, h, 0, h, r); x.arcTo(0, h, 0, 0, r); x.arcTo(0, 0, l.w, 0, r); x.closePath(); x.fill();
      }
      St.row(Object.assign({ n: St.id(), class: AUX, kind: 2, src: GB.SOURCES.indexOf('text'), w: l.w, h: h,
        pivot_x: l.w >> 1, pivot_y: h >> 1, alpha: 255, n_frames: 1, str: tones(l.t), radius: l.backing ? 8 : 1, digit: 1, end: 1 },
        centred(l.w, h, l.slot)), plane(panel), true, false);
    });
    St.values(valuesBytes(d), VALUES_VER);
    St.preview({ plane: plane(await picture(d, 128)), x: 0, y: 0, order: 3 });
    var out = St.finish({ name: d.name || 'Radar', slug: 'radar', serial: o.serial || serialOf(d, o.design), abi: GB.GEOM_ABI6, design: o.design || null,
                          bakedS: o.bakedS != null ? o.bakedS : Math.floor(Date.now() / 1000) });
    return { bytes: out.bytes, meter: out.meter, est: out.est, rows: St.elements.length };
  }

  // the scale's words: their own font (face, size, colour), the names' when a design has none
  function W_scale(W) { return W.scale || { face: W.nameFace, px: W.namePx, colour: W.quiet }; }

  // the bake's serial is its content (weather-bake.js serialOf): FNV-1a, never 0 or 1
  function serialOf(d, doc) {
    var bytes = doc && doc.length ? doc : new TextEncoder().encode(JSON.stringify(d)), h = 0x811c9dc5;
    for (var i = 0; i < bytes.length; i++) { h ^= bytes[i]; h = Math.imul(h, 0x01000193) >>> 0; }
    return h > 1 ? h : h + 2;
  }

  root.RadarBake = {
    SCHEMA: SCHEMA, SLOT: SLOT, N_NAMES: N_NAMES, VALUES_VER: VALUES_VER, ROLES: ROLES, PRESETS: Object.keys(PRESETS),
    STARTERS: STARTERS, STARTER_TITLES: STARTER_TITLES, STARTER_NOTES: STARTER_NOTES, DETAILS: DETAILS, NAMES: NAMES,
    COLOUR_ROLES: COLOUR_ROLES, ROLE_TITLES: ROLE_TITLES, LINE_ROLES: LINE_ROLES, DASH: DASH, detailOf: detailOf, namesOf: namesOf,
    scheme: scheme, complete: complete, rainLut: rainLut, argb: argb, rgba: rgba, dotCanvas: dotCanvas, FACES: FACES,
    preset: preset, bake: bake, picture: picture, valuesBytes: valuesBytes, serialOf: serialOf,
    plateCanvas: plateCanvas, glassCanvas: glassCanvas, font: font
  };
})(typeof window !== 'undefined' ? window : globalThis);
