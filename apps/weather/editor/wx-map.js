/* wx-map.js — the map under the weather's reading (flights-map-plan §5).
 *
 * The weather's own map module (apps share no code: the tracker carries its own), cut to
 * what the weather needs: one area round the shown place, no names. The design's `map`
 * part, its looks, the theme's values (main.c wx_values_t, layout 1, 108 B) and the
 * preview's view of the OS's map (maps@1 GorbMaps.render, the orb's rasteriser to the
 * nibble). A hint behind the reading, so the ground is not bare:
 *   - water: lakes and the sea filled (the smallest left out), rivers drawn, never creeks;
 *   - roads: motorways, or the main roads too; airports as a small symbol;
 *   - town dots, built-up areas a tint, state and country lines.
 * A look sets the colours, lines and marks; Strength scales every opacity at once. */
(function (root) {
  'use strict';
  var R = { water: 0, land: 1, urban: 2, lake: 3, river: 4, road2: 5, road1: 6, state: 7, border: 8, coast: 9,
    city: 10, airport: 11, runway: 12, road3: 13, road4: 14 };
  var DASH = { solid: 0, short: 1, long: 2, dot: 3, dashdot: 4 };
  var VALUES_VER = 1, CLIP = 96;                        // inside the bezel's ticks (r 192)
  var AREAS = [[20, 'Neighbourhood · 20 km'], [40, 'Area · 40 km'], [80, 'Region · 80 km'], [150, 'Wide · 150 km']];
  var INKS = [['water', 'Water'], ['river', 'Rivers'], ['urban', 'Towns, built up'], ['road1', 'Motorways'], ['road2', 'Main roads'],
    ['airport', 'Airports'], ['town', 'Town dots'], ['border', 'Borders']];
  var LINES = [['road1', 'Motorways'], ['road2', 'Main roads'], ['river', 'Rivers'], ['border', 'Borders']];
  var LOOKS = [['hint', 'Hint', 'One ink, thin and quiet: the land’s lines behind the reading'],
    ['chart', 'Chart', 'A flight chart’s colours: blue water, yellow towns, magenta airports'],
    ['drafting', 'Drafting', 'Pen on drafting paper: dashed roads, dotted rivers, outlined marks']];
  var SYMBOLS = [['diamond', 'Diamond'], ['diamondLine', 'Diamond outline'], ['ring', 'Ring'], ['ticked', 'Ring with ticks'], ['dot', 'Dot']];
  var SHAPE = { dot: 1, ring: 2, diamond: 3, diamondLine: 4, square: 5, squareLine: 6, ticked: 7 };
  var LAKE_FLOOR = 6, RIVER_FLOOR = 6, URBAN_FLOOR = 10;   // 12 px lakes, 48 px rivers, 20 px built-up crumbs
  var clone = function (o) { return JSON.parse(JSON.stringify(o)); };

  function look(kind, p) {
    if (kind === 'chart') return {
      ink: { water: '#8CC4EA', river: '#4F8FCB', urban: '#F2D04B', road1: '#8E4A6E', road2: '#8E4A6E', airport: '#C2187A', town: '#2B2B2B', border: '#6C6C7A' },
      alpha: { water: 0.4, river: 0.6, urban: 0.25, road1: 0.45, road2: 0.3, airport: 0.85, town: 0.55, border: 0.35 },
      line: { road1: [2, 'solid'], road2: [1, 'solid'], river: [1, 'solid'], border: [1, 'long'] }, symbol: 'ticked', town: 'dot' };
    if (kind === 'drafting') return {
      ink: { water: p.ink, river: p.ink, urban: p.ink, road1: p.ink, road2: p.ink, airport: p.ink, town: p.ink, border: p.ink },
      alpha: { water: 0.1, river: 0.4, urban: 0, road1: 0.45, road2: 0.3, airport: 0.7, town: 0.55, border: 0.3 },
      line: { road1: [1, 'long'], road2: [1, 'short'], river: [1, 'dot'], border: [1, 'dashdot'] }, symbol: 'ring', town: 'squareLine' };
    return {
      ink: { water: p.water, river: p.water, urban: p.ink, road1: p.ink, road2: p.ink, airport: p.accent, town: p.ink, border: p.ink },
      alpha: { water: 0.28, river: 0.35, urban: 0.06, road1: 0.22, road2: 0.15, airport: 0.55, town: 0.4, border: 0.2 },
      line: { road1: [2, 'solid'], road2: [1, 'solid'], river: [1, 'solid'], border: [1, 'long'] }, symbol: 'diamond', town: 'dot' };
  }
  // each preset's inks and its start: the same looks as the matching flights families
  var FAMILY = {
    scope: { on: true, look: 'hint', p: { ink: '#3DBB6E', water: '#2E9C88', accent: '#9CFFC4' } },
    brass: { on: true, look: 'hint', p: { ink: '#C9A24E', water: '#7FA3B0', accent: '#E8C46A' } },
    blueprint: { on: true, look: 'drafting', p: { ink: '#F4F8FF', water: '#9FE3FF', accent: '#FFFFFF' } },
    paper: { on: true, look: 'chart', p: { ink: '#2F5C8C', water: '#8FB0C6', accent: '#B85C00' } },
    celestial: { on: true, look: 'hint', p: { ink: '#8C7BFF', water: '#5A6BD8', accent: '#FFD27A' } },
    station: { on: true, look: 'hint', p: { ink: '#555555', water: '#6A8AA8', accent: '#E3000F' } },
    sectional: { on: true, look: 'chart', p: { ink: '#1F4E8C', water: '#8CC4EA', accent: '#C2187A' } },
    silver: { on: true, look: 'hint', p: { ink: '#C8CED6', water: '#7E9CB8', accent: '#F2F4F7' } },
    blank: { on: false, look: 'hint', p: { ink: '#9A9A9A', water: '#6A7A8A', accent: '#D0D0D0' } },
  };
  function paletteOf(base) { return (FAMILY[base] || FAMILY.scope).p; }
  function preset(base) {
    var F = FAMILY[base] || FAMILY.scope, L = look(F.look, F.p);
    // airports start off: round a city the airline fields alone are a handful of marks
    return { on: F.on, look: F.look, strength: 0.5, area: 40, water: 'all', roads: 'motorways', airports: false, symbol: L.symbol,
      dots: true, urban: true, borders: true, ink: L.ink, alpha: L.alpha, line: L.line, town: L.town };
  }
  function applyLook(m, kind, base) {
    var L = look(kind, paletteOf(base)), n = clone(m);
    n.look = kind; n.ink = L.ink; n.alpha = L.alpha; n.line = L.line; n.symbol = L.symbol; n.town = L.town;
    return n;
  }

  function bit(r) { return 1 << R[r]; }
  function roles(m) {
    if (!m.on) return { role_on: 0, mark_on: 0 };
    var on = 0, marks = 0;
    if (m.water !== 'off') on |= bit('water') | bit('lake');
    if (m.water === 'all') on |= bit('coast') | bit('river');
    if (m.urban) on |= bit('urban');
    if (m.roads !== 'off') on |= bit('road1');
    if (m.roads === 'main') on |= bit('road2');
    if (m.borders) on |= bit('state') | bit('border');
    if (m.airports) { on |= bit('airport'); marks |= 8; }
    if (m.dots) { on |= bit('city'); marks |= 1; }
    return { role_on: on || bit('land'), mark_on: marks };
  }
  function argb(hex, a) {
    var n = parseInt(String(hex || '#000000').slice(1), 16) >>> 0, al = Math.max(0, Math.min(255, Math.round(a * 255)));
    return ((al << 24) | (n & 0xFFFFFF)) >>> 0;
  }
  function alphaOf(m, key) { return Math.max(0, Math.min(1, (m.alpha[key] || 0) * (m.strength / 0.5))); }
  function pal(m) {
    var P = new Array(16).fill(0), put = function (role, key) { P[R[role]] = argb(m.ink[key], alphaOf(m, key)); };
    put('water', 'water'); put('lake', 'water'); put('coast', 'river'); put('river', 'river'); put('urban', 'urban');
    put('road1', 'road1'); put('road2', 'road2'); put('state', 'border'); put('border', 'border');
    put('airport', 'airport'); put('city', 'town');
    return P;
  }
  function strokes(m) {
    var w = new Array(16).fill(0), d = new Array(16).fill(0);
    var ln = function (role, key) { var l = m.line[key] || [1, 'solid']; w[R[role]] = Math.max(1, Math.min(4, l[0] | 0)); d[R[role]] = DASH[l[1]] | 0; };
    ln('road1', 'road1'); ln('road2', 'road2'); ln('river', 'river'); ln('coast', 'river'); ln('state', 'border'); ln('border', 'border');
    w[R.river] |= RIVER_FLOOR << 3;
    w[R.airport] = (SHAPE[m.symbol] || 3) | 5 << 3;
    w[R.city] = (SHAPE[m.town] || 1) | 3 << 3;
    d[R.airport] = 1;                                 // the airline airports: a small field is no landmark here
    d[R.lake] = LAKE_FLOOR; d[R.water] = 2; d[R.urban] = URBAN_FLOOR;
    return { width: w, dash: d };
  }

  /* ---- the values: wx_values_t, little-endian, 108 B ---- */
  function valuesBytes(m) {
    var b = new Uint8Array(108), dv = new DataView(b.buffer), P = pal(m), S = strokes(m), Ro = roles(m), k;
    for (k = 0; k < 16; k++) dv.setUint32(4 * k, P[k], true);
    for (k = 0; k < 16; k++) { b[64 + k] = S.width[k]; b[80 + k] = S.dash[k]; }
    dv.setUint16(96, Ro.role_on, true);
    b[98] = Ro.mark_on; b[99] = CLIP; b[100] = 0;
    dv.setUint16(102, Math.max(5, Math.min(400, m.area | 0)), true);
    return b;
  }

  /* ---- the preview: the OS's view of the owner's home map ---- */
  function levelOf(lat, areaKm) { return root.GorbMaps.pickLevel(Math.round(lat * 1e6), areaKm * 1000); }
  function view(m, bytes, lat) {
    var GM = root.GorbMaps, lv = lat == null ? { scale: 1 } : levelOf(lat, m.area), Ro = roles(m), S = strokes(m);   // null: a made-up place
    var r = GM.render(bytes, { scale: lv.scale, rot_rad: 0, role_on: Ro.role_on, mark_on: Ro.mark_on, width: S.width, dash: S.dash, clip: CLIP });
    if (r.err) throw new Error('the map could not be drawn (' + r.err + ')');
    var c = document.createElement('canvas'); c.width = c.height = 466;
    var x = c.getContext('2d'), im = x.createImageData(466, 466);
    GM.paint(r.raster, pal(m), im.data);
    x.putImageData(im, 0, 0);
    return c;
  }

  root.WeatherMap = {
    R: R, VALUES_VER: VALUES_VER, CLIP: CLIP, AREAS: AREAS, INKS: INKS, LINES: LINES, LOOKS: LOOKS, SYMBOLS: SYMBOLS,
    preset: preset, applyLook: applyLook, roles: roles, pal: pal, strokes: strokes, valuesBytes: valuesBytes,
    alphaOf: alphaOf, view: view, levelOf: levelOf,
  };
})(typeof window !== 'undefined' ? window : globalThis);
