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
  var SLOT = { dot: 0, hand: 1, sweep: 2, time: 8, place: 9, range: 10, status: 11, name0: 12, zoom: 18 };
  var N_NAMES = 6;
  var VALUES_VER = 5;
  var FORECASTS = { none: 0, dim: 1, hatch: 2, desat: 3 };
  var FORECAST_TITLES = [['dim', 'Dimmed'], ['hatch', 'Hatched'], ['desat', 'Desaturated'], ['none', 'As observed']];
  // the map's roles by index (OS v1.2.26's FORMAT 2): road1 motorway and trunk, road2 primary,
  // road3 secondary, road4 tertiary
  var ROLES = ['water', 'land', 'urban', 'lake', 'river', 'road2', 'road1', 'state', 'border', 'coast', 'city', 'airport', 'runway',
               'road3', 'road4'];
  var DASH = { solid: 0, short: 1, long: 2, dot: 3, dashdot: 4 };

  /* ---- the starters (R7 adds the rest) ---- */
  var PRESETS = {
    scope: {
      name: 'Scope', base: 'scope', // the shelf file stays radarscope.orbtheme (owner, 2026-11-08: one name per look)
      ground: { top: '#0a1f14', foot: '#020805' },
      // the rim alone: the range ring is drawn at a round distance by the app (layout.scale)
      glass: { ring: '#3ddc84', ringA: 0.30, rings: [1.0], cross: true, track: '#3ddc84', trackA: 0.6, forecast: '#e0b040' },
      // 26 px: capitals 18 px tall, the floor for a 1.75 in glass read from 2 ft (weather-plan
      // §2.8; owner, 2026-11-08: the 18 and 14 px words were far too small)
      words: { face: 'IBM Plex Mono', px: 26, ink: '#b4ffd2', forecast: '#e0b040', quiet: '#5fbf8a', namePx: 26, nameFace: 'IBM Plex Mono',
               zoomPx: 40, backing: 'rgba(4,10,14,0.78)',     // the zoom's word while it changes, on a backing
               scale: { face: 'IBM Plex Mono', px: 26, colour: '#5fbf8a' } },   // the scale's length
      // the scale bar (layout 3): ink and a dark edge, its thickness, end ticks, longest bar
      scale: { ink: '#b4ffd2', edge: '#020805', width: 2, tick: 9, max: 150 },
      // drawn on the map, under the rain (layout 4): none here, Radarscope's rings are glass art over it
      under: { colour: null, rings: 0, cross: false, width: 1 },
      dot: { colour: '#ffffff', ring: '#3ddc84', r: 4.5 },
      map: {
        // phosphor (the mock-up's Scope): every feature a green, brighter as it matters more
        water: '#04180f', land: '#06140c', urban: '#0b2416', lake: '#082a1a', river: '#175a3a', road2: '#14482e',
        road1: '#1f7a4c', state: '#163e2a', border: '#2fa86a', coast: '#175a3a', city: '#5fd49a', airport: '#c8ffe0', runway: '#1f7a4c',
        road3: '#103522', road4: '#0d2c1c',
        width: { river: 1, road2: 1, road1: 2, state: 1, border: 2, coast: 1, runway: 2, road3: 1, road4: 1 },
        dash: { state: 'long' },
        // the first zoom step each is drawn at (Wide 0 .. Close 5): the roads thin out as the
        // view widens (owner, 2026-11-07)
        from: { road2: 3, road3: 4, road4: 5 },
        off: [], marks: ['city', 'airport'], names: 4
      },
      // dBZ stops: colour, opacity (the noise floor is 10 dBZ)
      // the phosphor's own rain (the mock-up): green glowing to white where it is heaviest
      rain: [[10, '#1f6b46', 0.55], [20, '#2fa86a', 0.7], [30, '#3ddc84', 0.82], [40, '#7dffb0', 0.9],
             [50, '#c8ffe0', 0.95], [60, '#ffffff', 1.0], [70, '#ffffff', 1.0]],
      order: { map: 0, rain: 1, rows: 2 },
      // radar-plan §6.26, the approved mock-up's look (§0 ruling 13): the time low over a
      // track on the bottom rim, a range ring at a round distance with its label, the
      // forecast marked, the place in capitals, and Scope's sweep
      layout: { track: 'bottom', scale: 'ring', caps: true, placeY: 52, timeY: 346, statusY: 392 },
      forecast: 'dim',
      sweep: { on: true, colour: '#3ddc84', tail: 40, period: 6 }
    }
  };
  /* ---- R7: the other starters (radar-plan §2.9, §0 ruling 13), each matching the same-named
   * looks in Weather and Flights; built from a palette over Radarscope's shape ---- */
  function starter(name, P) {
    var d = JSON.parse(JSON.stringify(PRESETS.scope));
    d.name = name;
    d.ground = { top: P.top, foot: P.foot, stars: !!P.stars };
    Object.assign(d.glass, P.glass);
    Object.assign(d.words, P.words, P.words2 || {});
    d.words.scale = Object.assign({}, d.words.scale, { face: d.words.nameFace, colour: d.words.quiet });   // the ring's label, quiet, in the look's face
    d.scale = Object.assign({}, d.scale, { ink: P.words.ink, edge: P.scaleEdge });
    d.dot = P.dot;
    Object.assign(d.map, P.map);
    if (P.dash) d.map.dash = P.dash;
    if (P.width) d.map.width = Object.assign({}, d.map.width, P.width);
    d.rain = P.rain;
    d.forecast = P.forecast || 'dim';
    d.sweep = Object.assign({}, d.sweep, { on: false }, P.sweep || {});
    if (P.layout) Object.assign(d.layout, P.layout);
    if (P.look) d.map.look = P.look;
    d.base = P.base;
    return d;
  }
  PRESETS.paper = starter('Paper', {
    forecast: 'hatch', base: 'paper', layout: { caps: false }, words2: { face: 'Playfair Display', nameFace: 'IBM Plex Sans' },
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
  // Station and Brass as the clock, the tracker and the weather wear them (look parity, owner
  // 2026-11-08): the railway clock's white, black and red, the broadcast rain kept; the brass
  // instrument's dark enamel and brass. The mock-up's TV map (§0 ruling 13) gave way to parity.
  PRESETS.station = starter('Station', {
    forecast: 'desat', base: 'station',
    top: '#ffffff', foot: '#e6e6e3', scaleEdge: '#ffffff',
    glass: { ring: '#111111', ringA: 0.35, track: '#111111', trackA: 0.7, forecast: '#e3000f' },
    words: { face: 'Jost', nameFace: 'Jost', ink: '#111111', forecast: '#e3000f', quiet: '#555555', backing: 'rgba(255,255,255,0.88)' },
    dot: { colour: '#e3000f', ring: '#111111', r: 4.5 },
    map: { water: '#cfd8e0', land: '#f4f4f2', urban: '#e4e4e0', lake: '#c2ced8', river: '#8aa0b4', road2: '#9a9a9a',
           road1: '#555555', state: '#777777', border: '#111111', coast: '#8aa0b4', city: '#111111', airport: '#e3000f',
           runway: '#777777', road3: '#bcbcbc', road4: '#cfcfcf', look: { airport: 'square', city: 'dot' } },
    width: { state: 1, border: 2 },
    rain: [[10, '#00b000', 0.70], [20, '#00e000', 0.80], [30, '#ffff00', 0.88], [40, '#ff9000', 0.92],
           [50, '#ff0000', 0.95], [60, '#ff00ff', 1.0], [70, '#111111', 1.0]]
  });
  PRESETS.vintage = starter('Brass', {
    forecast: 'hatch', base: 'vintage', layout: { caps: false },
    top: '#2e332e', foot: '#151815', scaleEdge: '#151815',
    glass: { ring: '#c9a24e', ringA: 0.45, track: '#e9ddbf', trackA: 0.7, forecast: '#ff9a3c', rim: '#c9a24e' },
    words: { face: 'Cormorant Garamond', nameFace: 'Cormorant Garamond', ink: '#f2e6c8', forecast: '#ff9a3c', quiet: '#a8946a',
             backing: 'rgba(22,24,21,0.85)' },
    dot: { colour: '#e8c46a', ring: '#151815', r: 4.5 },
    map: { water: '#1c2624', land: '#262a26', urban: '#30332c', lake: '#1e2c2a', river: '#6a8a8a', road2: '#7a6430',
           road1: '#c9a24e', state: '#5a5038', border: '#e8c46a', coast: '#6a8a8a', city: '#f2e6c8', airport: '#e8c46a',
           runway: '#8a7340', road3: '#4e4428', road4: '#3e3822' },
    rain: [[10, '#5f8a6a', 0.50], [20, '#7aa070', 0.65], [30, '#c8a040', 0.78], [40, '#d8802a', 0.86],
           [50, '#c84a1a', 0.92], [60, '#a02a3a', 1.0], [70, '#f2e6c8', 1.0]]
  });
  PRESETS.blueprint = starter('Blueprint', {
    forecast: 'hatch', base: 'blueprint',
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
  // flights-map-plan §4: a flight chart's look, made up: chart paper, blue water, yellow
  // towns, magenta airports as a chart's ticked rings, the rain kept off yellow and blue
  PRESETS.sectional = starter('Sectional', {
    forecast: 'hatch', base: 'sectional',
    top: '#eef1e4', foot: '#dfe5cf', scaleEdge: '#eef1e4',
    glass: { ring: '#2a5db0', ringA: 0.55, track: '#c2187a', trackA: 0.7, forecast: '#c2187a' },
    words: { face: 'Barlow Condensed', nameFace: 'Barlow Condensed', ink: '#1f2a44', forecast: '#c2187a', quiet: '#3e5a80',
             backing: 'rgba(238,241,228,0.9)' },
    dot: { colour: '#c2187a', ring: '#1f2a44', r: 4.5 },
    map: { water: '#9fcbe9', land: '#eef1e4', urban: '#f3dc7a', lake: '#9fcbe9', river: '#4f8fcb', road2: '#a8708c',
           road1: '#8e4a6e', state: '#6c6c7a', border: '#4a4a5a', coast: '#4f8fcb', city: '#2b2b2b', airport: '#c2187a',
           runway: '#c2187a', road3: '#c7a6b6', road4: '#d6c2cc', look: { airport: 'ticked', city: 'dot' } },
    dash: { state: 'long', border: 'dashdot' },
    rain: [[10, '#79c779', 0.55], [20, '#2f9a41', 0.72], [30, '#e07b00', 0.82], [40, '#d9480f', 0.88],
           [50, '#b3122b', 0.92], [60, '#8e1a8a', 1.0], [70, '#3b0a57', 1.0]]
  });
  // flights-map-plan §6 (look parity): Celestial, the night sky's violets and gold, and
  // Silver, polished metal and graphite, as the tracker and the weather wear them
  PRESETS.celestial = starter('Celestial', {
    forecast: 'dim', base: 'celestial', sweep: { on: true, colour: '#c9beff', tail: 30, period: 8 },
    top: '#251a55', foot: '#0b0820', scaleEdge: '#0b0820', stars: true,
    glass: { ring: '#8c7bff', ringA: 0.45, track: '#b9b2ff', trackA: 0.7, forecast: '#ffd27a' },
    words: { face: 'Space Grotesk', nameFace: 'Space Grotesk', ink: '#edebff', forecast: '#ffd27a', quiet: '#9c95c9', backing: 'rgba(11,8,32,0.8)' },
    dot: { colour: '#ffe9a8', ring: '#0b0820', r: 4.5 },
    map: { water: '#140f3a', land: '#1a1240', urban: '#241a52', lake: '#1d1650', river: '#5a6bd8', road2: '#5a4fa8',
           road1: '#8c7bff', state: '#4a3f8a', border: '#b9b2ff', coast: '#5a6bd8', city: '#d9d4ff', airport: '#ffd27a',
           runway: '#6a5fb8', road3: '#3d3478', road4: '#332b66' },
    rain: [[10, '#5a8cff', 0.55], [20, '#3fd2ff', 0.7], [30, '#7dffb0', 0.8], [40, '#ffd27a', 0.88],
           [50, '#ff8a5c', 0.92], [60, '#ff4fb0', 1.0], [70, '#ffffff', 1.0]]
  });
  PRESETS.silver = starter('Silver', {
    forecast: 'desat', base: 'silver',
    top: '#3a3f46', foot: '#1c1f24', scaleEdge: '#1c1f24',
    glass: { ring: '#c8ced6', ringA: 0.4, track: '#c8ced6', trackA: 0.65, forecast: '#7fc8ff', rim: '#c8ced6' },
    words: { face: 'Jost', nameFace: 'Jost', ink: '#f2f4f7', forecast: '#7fc8ff', quiet: '#aab2bd', backing: 'rgba(20,22,26,0.8)' },
    dot: { colour: '#ffffff', ring: '#1c1f24', r: 4.5 },
    map: { water: '#232a33', land: '#2c3138', urban: '#363c44', lake: '#28313c', river: '#7e9cb8', road2: '#8a929c',
           road1: '#c8ced6', state: '#6a727c', border: '#e4e8ec', coast: '#7e9cb8', city: '#f2f4f7', airport: '#7fc8ff',
           runway: '#8a929c', road3: '#5a626c', road4: '#4a525c' },
    rain: [[10, '#5ab878', 0.6], [20, '#2e9e5e', 0.75], [30, '#e8d040', 0.85], [40, '#e88a30', 0.9],
           [50, '#d83a3a', 0.94], [60, '#c040c0', 1.0], [70, '#ffffff', 1.0]]
  });
  PRESETS.blank = starter('Blank', {
    base: 'blank',
    top: '#1c1c1c', foot: '#101010', scaleEdge: '#000000',
    glass: { ring: '#9a9a9a', ringA: 0.25, track: '#9a9a9a', trackA: 0.5, forecast: '#c8c8c8' },
    words: { ink: '#e8e8e8', forecast: '#c8c8c8', quiet: '#a8a8a8', backing: 'rgba(0,0,0,0.75)' },
    dot: { colour: '#ffffff', ring: '#000000', r: 4.5 },
    map: { water: '#202428', land: '#2a2a2a', urban: '#323232', lake: '#24282c', river: '#4a5058', road2: '#5a5a5a',
           road1: '#7a7a7a', state: '#4a4a4a', border: '#8a8a8a', coast: '#5a6068', city: '#d0d0d0', airport: '#d0d0d0',
           runway: '#6a6a6a', road3: '#484848', road4: '#3c3c3c' },
    rain: PRESETS.scope.rain
  });
  // one name per look in every app (owner, 2026-11-08): Scope, Paper, Brass, Blueprint, Station,
  // Celestial, Silver, Sectional; 'vintage' stays Brass's key, for the designs saved under it
  var STARTERS = ['scope', 'paper', 'vintage', 'blueprint', 'station', 'celestial', 'silver', 'sectional'];
  var STARTER_TITLES = { scope: 'Scope', paper: 'Paper', station: 'Station', vintage: 'Brass', blueprint: 'Blueprint', celestial: 'Celestial',
    silver: 'Silver', sectional: 'Sectional', blank: 'Blank' };
  var STARTER_NOTES = { scope: 'Phosphor green on black, with the sweep', paper: 'A printed road atlas', station: 'The railway clock: white, black and red',
    vintage: 'Dark enamel and brass', blueprint: 'Cyan lines on blueprint blue', celestial: 'The night sky: violet and gold',
    silver: 'Polished metal and graphite', sectional: 'A flight chart: magenta, blue and yellow', blank: 'Plain greys' };

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
    // a design from before the mock-up's look (§6.26) keeps its own: the track round the rim,
    // the scale bar, no sweep, the forecast in its colour alone
    if (!out.layout) out.layout = { track: 'round', scale: 'bar', caps: false, placeY: 58, timeY: 98, statusY: 362 };
    if (!out.sweep) out.sweep = { on: false, colour: (out.glass && out.glass.ring) || '#5fd4c4', tail: 40, period: 6 };
    if (!out.forecast) out.forecast = 'none';
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
    if (d.ground.stars) {                          // Celestial: a fixed field of stars
      var sd = 7, r = function () { sd = (sd * 16807) % 2147483647; return sd / 2147483647; };
      for (var i = 0; i < 140; i++) {
        var a = r() * 2 * Math.PI, rr = Math.sqrt(r()) * 228, z = r();
        x.fillStyle = 'rgba(255,255,255,' + (0.25 + 0.6 * z).toFixed(2) + ')';
        x.beginPath(); x.arc(CX + rr * Math.cos(a), CX + rr * Math.sin(a), 0.5 + z * 1.1, 0, 2 * Math.PI); x.fill();
      }
    }
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
    if (gl.rim) {                                  // a metal bezel (Brass, Silver), lit from the upper left
      var mg = x.createLinearGradient(0, 0, G, G), m0 = hex(gl.rim);
      var tone = function (k) { return 'rgb(' + m0.map(function (v) { return Math.round(k < 0 ? v * (1 + k) : v + (255 - v) * k); }).join(',') + ')'; };
      [[0, tone(0.45)], [0.35, tone(-0.35)], [0.55, tone(0.25)], [0.8, tone(-0.55)], [1, tone(0.1)]].forEach(function (st) { mg.addColorStop(st[0], st[1]); });
      x.strokeStyle = mg; x.lineWidth = 7; x.beginPath(); x.arc(CX, CX, CX - 4, 0, 2 * Math.PI); x.stroke();
    }
    var bottom = d.layout && d.layout.track === 'bottom';
    for (var m = -110; m <= 60; m += 10) {
      // round: Now at the top, 1.5 degrees a minute; bottom (the mock-up): Now at the bottom,
      // the past to its left, half a degree a minute (main.c draw_ui turns the cursor so)
      var deg = bottom ? 180 - m * 0.5 : m * 1.5, a = (deg - 90) * Math.PI / 180, r0 = CX - 14, r1 = CX - (m % 60 === 0 ? 4 : 8);
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
  // the sweep (Scope's signature, the mock-up's Life): a line from the centre to the rim with
  // a fading trail behind it, pointing up, its pivot the glass's centre; main.c turns it
  // Its box is the wedge's own (the trail's width, the radius's height), so it costs what it shows.
  function sweepBox(d) {
    var L = CX - 4, tail = Math.max(0, Math.min(90, d.sweep.tail | 0));
    return { w: Math.ceil(L * Math.sin(Math.max(tail, 2) * Math.PI / 180)) + 4, h: CX + 1, px: 0, L: L };
  }
  function sweepCanvas(d) {
    var bx = sweepBox(d), c = canvas(bx.w, bx.h), x = c.getContext('2d'), S = d.sweep, L = bx.L, n = Math.max(8, Math.round(S.tail * 0.8));
    var ox = bx.w - 2, oy = CX;                       // the pivot: the box's right edge, at the centre's height
    for (var i = 0; i < n; i++) {
      var a0 = -S.tail * i / n, a1 = -S.tail * (i + 1) / n, t0 = (a0 - 90) * Math.PI / 180, t1 = (a1 - 90) * Math.PI / 180;
      x.fillStyle = rgba(S.colour, 0.34 * Math.pow(1 - i / n, 2));
      x.beginPath(); x.moveTo(ox, oy); x.lineTo(ox + L * Math.cos(t0), oy + L * Math.sin(t0)); x.lineTo(ox + L * Math.cos(t1), oy + L * Math.sin(t1)); x.closePath(); x.fill();
    }
    x.strokeStyle = rgba(S.colour, 0.9); x.lineWidth = 2;
    x.beginPath(); x.moveTo(ox, oy); x.lineTo(ox, oy - L); x.stroke();
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
    var b = new Uint8Array(64 + 16 + 16 + 4 + 4 + 1024 + 16 + 16 + 16 + 16), dv = new DataView(b.buffer), M = d.map, i;
    ROLES.forEach(function (r, k) { dv.setUint32(4 * k, argb(M[r], 1), true); });
    ROLES.forEach(function (r, k) {
      b[64 + k] = Math.max(0, Math.min(4, (M.width && M.width[r]) | 0));
      b[80 + k] = DASH[(M.dash && M.dash[r]) || 'solid'] | 0;
    });
    // OS v1.2.38: a mark's look in its width byte (an older OS draws the old dot and square)
    var LOOK = { dot: 1, ring: 2, diamond: 3, diamondLine: 4, square: 5, squareLine: 6, ticked: 7 };
    if (M.look && LOOK[M.look.airport]) b[64 + 11] = LOOK[M.look.airport] | 6 << 3;
    if (M.look && LOOK[M.look.city]) b[64 + 10] = LOOK[M.look.city] | 3 << 3;
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
    if (d.layout && d.layout.scale === 'ring') S = Object.assign({}, S, { ink: ringInk(d) });   // §6.26: the ring, quiet
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
    var Ly = d.layout || {};                               // layout 5: the mock-up's look
    b[1176] = Ly.track === 'bottom' ? 1 : 0;
    b[1177] = FORECASTS[d.forecast] | 0;
    b[1178] = Ly.scale === 'ring' ? 1 : 0;
    b[1179] = Ly.caps ? 1 : 0;
    dv.setUint16(1180, Ly.placeY || 58, true);
    dv.setUint16(1182, Ly.timeY || 98, true);
    dv.setUint16(1184, Ly.statusY || 362, true);
    b[1186] = d.sweep && d.sweep.on ? Math.max(2, Math.min(30, d.sweep.period | 0)) : 0;
    return b;
  }

  /* ---- the look, drawn: the editor's preview and the baked picture share it ---- */
  // the range ring's colour: the glass ring's, softened toward the ground as its opacity
  // says, made opaque (main.c draws it with an opaque 1 px pen)
  function ringInk(d) {
    var a = hex(d.glass.ring), b = hex(d.ground.top), k = Math.min(1, (d.glass.ringA || 0.3) * 2);
    return '#' + a.map(function (v, i) { return Math.round(b[i] + (v - b[i]) * k).toString(16).padStart(2, '0'); }).join('');
  }
  // the radar's view radius at a zoom (radar.c rd_radius_m) and the round length there (rd_scale)
  function radiusM(step, lat) { return Math.floor(156543.03392 * Math.cos(lat * Math.PI / 180) / Math.pow(2, 6 + step) * 233 + 0.5); }
  function scaleOf(radius, unit, max) {
    var u = unit === 'mi' ? 1609.344 : unit === 'nm' ? 1852 : 1000, mpp = radius / 233, best = 1;
    [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000].forEach(function (n) { if (n * u / mpp <= max) best = n; });
    return { len: best, px: Math.floor(best * u / mpp + 0.5) };
  }
  // a made-up squall line and showers, in dBZ, the same every time
  var RAIN_CELLS = [[150, 300, 70, 52], [210, 270, 55, 44], [280, 250, 60, 38], [340, 230, 45, 30], [120, 160, 40, 26],
                    [330, 120, 35, 22], [250, 330, 30, 48], [190, 220, 25, 58], [380, 330, 38, 20]];
  var rainField = null;
  function field() {
    if (rainField) return rainField;
    rainField = new Float32Array(G * G);
    for (var y = 0; y < G; y++) for (var x = 0; x < G; x++) {
      var v = 0;
      RAIN_CELLS.forEach(function (c) {
        var dx = (x - c[0]) / c[2], dy = (y - c[1]) / (c[2] * 0.7), e = Math.exp(-(dx * dx + dy * dy));
        v = Math.max(v, c[3] * e + 6 * e * Math.sin(x * 0.07 + y * 0.05));
      });
      rainField[y * G + x] = v;
    }
    return rainField;
  }
  // the rain through the design's ramp at the app's default floor (15 dBZ); a forecast frame
  // in the design's treatment, as main.c draws it (dimmed to half, desaturated, or hatched)
  function rainCanvas(d, shift, forecast) {
    var c = canvas(G, G), x = c.getContext('2d'), im = x.createImageData(G, G), lut = rainLut(d, 15), f = field();
    var tr = forecast ? d.forecast : 'none';
    for (var i = 0; i < G * G; i++) {
      var px = i % G, py = (i / G) | 0, src = f[i - Math.max(0, Math.min(px, shift | 0))] || 0;
      var v = Math.max(0, Math.min(255, Math.round((src + 32) * 2))), o = 4 * v, r = lut[o], g = lut[o + 1], bl = lut[o + 2], al = lut[o + 3];
      if (tr === 'dim') al = al >> 1;
      else if (tr === 'desat') { var l = (r * 77 + g * 150 + bl * 29) >> 8; r = (r + 7 * l) >> 3; g = (g + 7 * l) >> 3; bl = (bl + 7 * l) >> 3; }
      else if (tr === 'hatch' && ((px + py) & 7) >= 5) al = 0;
      im.data[4 * i] = r; im.data[4 * i + 1] = g; im.data[4 * i + 2] = bl; im.data[4 * i + 3] = al;
    }
    x.putImageData(im, 0, 0);
    return c;
  }
  // the roles drawn at a zoom step (main.c rd_roles_at)
  function rolesAt(d, step) {
    var on = 0;
    ROLES.forEach(function (r, k) {
      if ((d.map.off || []).indexOf(r) >= 0) return;
      if (((d.map.from || {})[r] | 0) > step) return;
      on |= 1 << k;
    });
    return on;
  }
  var LOOK = { dot: 1, ring: 2, diamond: 3, diamondLine: 4, square: 5, squareLine: 6, ticked: 7 };
  // the map in the design, as the orb's map service draws it (maps@1): a canvas and its marks
  function mapCanvas(d, bytes, step, lat) {
    var GM = root.GorbMaps, M = d.map;
    var width = ROLES.map(function (r) { return Math.max(0, Math.min(4, (M.width && M.width[r]) | 0)); });
    var dash = ROLES.map(function (r) { return DASH[(M.dash && M.dash[r]) || 'solid'] | 0; });
    if (M.look && LOOK[M.look.airport]) width[11] = LOOK[M.look.airport] | 6 << 3;
    if (M.look && LOOK[M.look.city]) width[10] = LOOK[M.look.city] | 3 << 3;
    var marks = { city: 1, town: 2, village: 4, airport: 8 }, mo = 0;
    (M.marks || []).forEach(function (k) { mo |= marks[k] | 0; });
    var info = GM.info(bytes), lv = lat == null ? { scale: 1 } : GM.pickLevel(Math.round(lat * 1e6), radiusM(step, lat));
    if (lat == null && info) lv = { scale: 1 };
    var r = GM.render(bytes, { scale: lv.scale, rot_rad: 0, role_on: rolesAt(d, step), mark_on: mo, width: width.concat([0]), dash: dash.concat([0]) });
    var pal = ROLES.map(function (k) { return argb(M[k], 1); }).concat([0]);
    var c = canvas(G, G), x = c.getContext('2d'), im = x.createImageData(G, G);
    GM.paint(r.raster, pal, im.data);
    x.putImageData(im, 0, 0);
    return { c: c, marks: r.marks };
  }
  /* One frame of the look, as the orb draws it (main.c draw_map/draw_rain/draw_ui): the plate,
   * the map, what goes under the rain, the rain, the range ring and its label, the glass, the
   * sweep, the dot, the time cursor, the names and the words. P: {plate, map: {c, marks},
   * rain, glass, dot, sweep}; o: {time: 'now'|'past'|'next', minutes, place, unit, radius (m),
   * sweepDeg, hidden: {ground, map, rain, overlay, words}}. */
  function frame(x, d, P, o) {
    var hid = o.hidden || {}, Ly = d.layout || {}, W = d.words, SW = W.scale || { face: W.nameFace, px: W.namePx, colour: W.quiet };
    x.clearRect(0, 0, G, G);
    if (P.plate && !hid.ground) x.drawImage(P.plate, 0, 0);
    if (P.map && !hid.map) x.drawImage(P.map.c, 0, 0);
    var U = d.under || {};
    if (!hid.overlay && U.colour && (U.rings || U.cross)) {
      x.strokeStyle = U.colour; x.lineWidth = U.width || 1;
      for (var k = 1; k <= (U.rings | 0); k++) { x.beginPath(); x.arc(CX, CX, 232 * k / U.rings, 0, 2 * Math.PI); x.stroke(); }
      if (U.cross) { x.beginPath(); x.moveTo(1, CX); x.lineTo(G - 1, CX); x.moveTo(CX, 1); x.lineTo(CX, G - 1); x.stroke(); }
    }
    if (P.rain && !hid.rain) x.drawImage(P.rain, 0, 0);
    var sb = scaleOf(o.radius, o.unit, Ly.scale === 'ring' ? 200 : d.scale.max);
    if (!hid.overlay && Ly.scale === 'ring') {               // main.c draw_scale: the ring at a round distance
      x.strokeStyle = ringInk(d); x.lineWidth = 1; x.beginPath(); x.arc(CX, CX, sb.px, 0, 2 * Math.PI); x.stroke();
    }
    if (!hid.overlay) {
      if (P.glass) x.drawImage(P.glass, 0, 0);
      if (P.sweep && d.sweep && d.sweep.on) {
        x.save(); x.translate(CX, CX); x.rotate((o.sweepDeg || 0) * Math.PI / 180); x.drawImage(P.sweep, -(P.sweep.width - 2), -CX); x.restore();
      }
      if (P.dot) x.drawImage(P.dot, CX - 8, CX - 8);
      var min = o.minutes | 0, deg = Ly.track === 'bottom' ? 180 - min * 0.5 : min * 1.5, t = (deg - 90) * Math.PI / 180;
      x.fillStyle = d.glass.track; x.beginPath(); x.arc(CX + 215 * Math.cos(t), CX + 215 * Math.sin(t), 4.5, 0, 2 * Math.PI); x.fill();
    }
    if (hid.words) return;
    var say = function (s, cx, cy, face, px, colour) {
      x.font = px + 'px "' + face + '", sans-serif'; x.textAlign = 'center'; x.textBaseline = 'middle';
      x.fillStyle = colour; x.fillText(s, cx, cy);
    };
    var n = 0, boxes = [];                                    // main.c draw_names
    (P.map ? P.map.marks : []).some(function (m) {
      if (n >= Math.min(6, d.map.names | 0)) return true;
      var nm = m.name, w = 13 * nm.length + 12, X = Math.round(m.x), Y = Math.round(m.y) + 22, dx = X - CX, dy = Y - CX;
      if (dx * dx + dy * dy > 185 * 185 || dx * dx + dy * dy < 40 * 40 || Y < 130 || Y > 330) return false;
      if (boxes.some(function (b) { return Math.abs(X - b[0]) < (w + b[2]) / 2 && Math.abs(Y - b[1]) < 32; })) return false;
      boxes.push([X, Y, w]); n++;
      say(nm, X, Y, W.nameFace, W.namePx, W.quiet);
      return false;
    });
    var place = o.place || 'Home';
    say(Ly.caps ? place.toUpperCase() : place, CX, Ly.placeY || 58, W.face, W.px, W.ink);
    var tt = o.minutes ? (o.minutes > 0 ? '+' : '-') + Math.abs(o.minutes) + ' min' : 'Now';
    say(tt, CX, Ly.timeY || 98, W.face, W.px, o.minutes > 0 ? W.forecast : W.ink);
    if (o.minutes > 0) say('forecast', CX, Ly.statusY || 362, W.nameFace, W.namePx, W.forecast);
    var label = sb.len + ' ' + o.unit;
    if (Ly.scale === 'ring') {                                 // the ring's label, up and right on it
      say(label, CX + Math.round(sb.px * 0.7071) + 30, CX - Math.round(sb.px * 0.7071) - 6, SW.face, SW.px, SW.colour);
    } else {
      var sc = d.scale, x0 = CX - sb.px / 2;
      if (sc.edge) { x.fillStyle = sc.edge; x.fillRect(x0 - 2, 439, sb.px + 4, sc.width + 2); x.fillRect(x0 - 2, 440 - sc.tick - 1, 4, sc.tick + 2); x.fillRect(x0 + sb.px - 2, 440 - sc.tick - 1, 4, sc.tick + 2); }
      x.fillStyle = sc.ink; x.fillRect(x0, 440, sb.px, sc.width);
      if (sc.tick) { x.fillRect(x0, 440 - sc.tick, 2, sc.tick); x.fillRect(x0 + sb.px - 2, 440 - sc.tick, 2, sc.tick); }
      say(label, CX, 414, SW.face, SW.px, SW.colour);
    }
  }

  /* ---- the picture: the picker's preview and the hub's list: the look itself, over the
   * owner's home map when the editor has it (o.map: .orbmap bytes and their latitude), else
   * over maps@1's made-up place ---- */
  async function picture(d, size, o) {
    o = o || {};
    var W = d.words, SW = W.scale || { face: W.nameFace, px: W.namePx };
    await Promise.all([loadFace(W.face, W.px), loadFace(W.nameFace, W.namePx), loadFace(SW.face, SW.px)]);
    var GM = root.GorbMaps, mp = null;
    if (GM) {
      try {
        mp = o.map ? mapCanvas(d, o.map.bytes, o.map.step == null ? 1 : o.map.step, o.map.lat) : mapCanvas(d, GM.sampleMap(), 1, null);
      } catch (e) { mp = null; }
    }
    var P = { plate: plateCanvas(d), map: mp, rain: rainCanvas(d, 0, false), glass: glassCanvas(d), dot: dotCanvas(d),
              sweep: d.sweep && d.sweep.on ? sweepCanvas(d) : null };
    var c = canvas(G, G);
    frame(c.getContext('2d'), d, P, { minutes: 0, place: o.place || 'Home', unit: o.unit || 'mi',
      radius: o.map ? radiusM(o.map.step == null ? 1 : o.map.step, o.map.lat) : 120000, sweepDeg: 300 });
    if (size === G) return c;
    var out = canvas(size, size);
    out.getContext('2d').drawImage(c, 0, 0, size, size);
    return out;
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
    if (d.sweep && d.sweep.on)                             // the sweep, turned by main.c each frame
      // a seconds row at 20 fps, as the tracker's arm is: the stage ticks the app at that rate
      St.row({ n: St.id(), class: AUX, slot: SLOT.sweep, x: CX - (sweepBox(d).w - 2), y: 0, w: sweepBox(d).w, h: sweepBox(d).h,
        pivot_x: sweepBox(d).w - 2, pivot_y: CX,
        order: rowsOrder, alpha: 255, n_frames: 1, src: GB.SOURCES.indexOf('second'), span: 3600,
        vmax: Math.max(2, Math.min(30, d.sweep.period | 0)), cadence: 1, cad_arg: 20, end: 0 }, plane(sweepCanvas(d)), true, false);
    var SW = W_scale(d.words);
    // the forecast's words are the time's glyphs and the scale's the names' (when their faces and
    // sizes agree): one full set each, so the bake shares them (radar-plan §6.26: ~190 KB a theme)
    // glyphs are coverage, coloured on the orb (S.font's colour): drawn white, a set of one face
    // and size is the same bytes whatever its tone, and the bake keeps it once
    var W = d.words, F = [await font(W.face, W.px, '#FFFFFF'), await font(W.face, W.px, '#FFFFFF'), await font(W.nameFace, W.namePx, '#FFFFFF'),
      await font(W.face, W.zoomPx || 34, '#FFFFFF', true), await font(SW.face, SW.px, '#FFFFFF')];
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
    // §60: the whole glass, the look as it is drawn (over the owner's map when the editor gives it)
    St.preview({ plane: plane(await picture(d, G, o.picture)), x: 0, y: 0, order: 3 });
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
    frame: frame, mapCanvas: mapCanvas, rainCanvas: rainCanvas, sweepCanvas: sweepCanvas, radiusM: radiusM, scaleOf: scaleOf,
    rolesAt: rolesAt, FORECASTS: FORECASTS, FORECAST_TITLES: FORECAST_TITLES,
    plateCanvas: plateCanvas, glassCanvas: glassCanvas, font: font
  };
})(typeof window !== 'undefined' ? window : globalThis);
