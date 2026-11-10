/* flights-map.js — the map under the tracker's scope (flights-map-plan §2.3).
 *
 * The design's `map` part, the looks that start it, and the two things the bake makes of
 * it: the theme's values (main.c fl_values_t, layout 1) and the preview's view of the OS's
 * map (maps@1 GorbMaps.render, the orb's rasteriser to the nibble). What is drawn at each
 * range is decided here, once, for the bake and the preview alike:
 *   - water: lakes and the sea filled, the coast and the rivers drawn (never streams: the
 *     map tool keeps none), the smallest lakes left out by the floor;
 *   - roads: motorways at every range, the main roads only at the nearest;
 *   - airports: a symbol, or the runways at the nearest range and the symbol wider out (where the
 *     runways are drawn, an airport with no runways in the map data keeps its symbol);
 *     the airline airports alone (an IATA code) or every airfield;
 *   - towns: a dot and a name, the cities or the towns too; urban areas a tint; state and
 *     country lines.
 * A look sets the colours, the lines and the marks; Strength scales every opacity at once,
 * so the map stays a hint under the traffic. Needs GorbMaps (maps@1) for the preview. */
(function (root) {
  'use strict';
  var R = { water: 0, land: 1, urban: 2, lake: 3, river: 4, road2: 5, road1: 6, state: 7, border: 8, coast: 9,
    city: 10, airport: 11, runway: 12, road3: 13, road4: 14 };
  var DASH = { solid: 0, short: 1, long: 2, dot: 3, dashdot: 4 };
  var N_RANGES = 4, N_NAMES = 6, NAME_SLOT0 = 32, VALUES_VER = 2, CLIP = 92;   // main.c RING_R / 2
  // the colour rows the Map step offers, in the order it lists them
  var INKS = [['water', 'Water'], ['river', 'Rivers'], ['urban', 'Towns, built up'], ['road1', 'Motorways'], ['road2', 'Main roads'],
    ['airport', 'Airports'], ['runway', 'Runways'], ['town', 'Town dots'], ['border', 'Borders'], ['names', 'Names']];
  var LINES = [['road1', 'Motorways'], ['road2', 'Main roads'], ['river', 'Rivers'], ['border', 'Borders'], ['runway', 'Runways']];
  var LOOKS = [['hint', 'Hint', 'One ink, thin and quiet: the land’s lines under the traffic'],
    ['chart', 'Chart', 'A flight chart’s colours: blue water, yellow towns, magenta airports'],
    ['drafting', 'Drafting', 'Pen on drafting paper: dashed roads, dotted rivers, outlined marks']];
  var SYMBOLS = [['diamond', 'Diamond'], ['diamondLine', 'Diamond outline'], ['ring', 'Ring'], ['ticked', 'Ring with ticks'], ['square', 'Square'], ['dot', 'Dot']];
  var LAKES = [['big', 'Big lakes only'], ['medium', 'Lakes and reservoirs'], ['all', 'Every pond']];
  var FLOOR = { big: 16, medium: 6, all: 0 };            // dash[LAKE]: a piece under 2x this px both ways is left out
  var NAMES = [['off', 'None'], ['few', 'A few'], ['normal', 'Some'], ['many', 'Many']];
  var N_OF = { off: 0, few: 2, normal: 4, many: 6 };
  var RIVER_FLOOR = 6;                                    // 8 px steps (OS v1.2.38): 48 px

  var clone = function (o) { return JSON.parse(JSON.stringify(o)); };

  /* ---- the looks: each family's inks dressed three ways ---- */
  // p: { ink, water, accent, face }: the family's line colour, its water and its airports
  function look(kind, p) {
    if (kind === 'chart') return {
      ink: { water: '#8CC4EA', river: '#4F8FCB', urban: '#F2D04B', road1: '#8E4A6E', road2: '#8E4A6E', airport: '#C2187A', runway: '#C2187A',
        town: '#2B2B2B', border: '#6C6C7A', names: p.names || '#1F2A44' },
      alpha: { water: 0.5, river: 0.75, urban: 0.3, road1: 0.55, road2: 0.4, airport: 0.95, runway: 0.85, town: 0.7, border: 0.45, names: 0.85 },
      line: { road1: [2, 'solid'], road2: [1, 'solid'], river: [1, 'solid'], border: [1, 'long'], runway: [3, 'solid'] },
      symbol: 'ticked', town: 'dot' };
    if (kind === 'drafting') return {
      ink: { water: p.ink, river: p.ink, urban: p.ink, road1: p.ink, road2: p.ink, airport: p.ink, runway: p.ink, town: p.ink, border: p.ink, names: p.ink },
      alpha: { water: 0.12, river: 0.55, urban: 0, road1: 0.6, road2: 0.4, airport: 0.85, runway: 0.75, town: 0.7, border: 0.4, names: 0.75 },
      line: { road1: [1, 'long'], road2: [1, 'short'], river: [1, 'dot'], border: [1, 'dashdot'], runway: [2, 'solid'] },
      symbol: 'ring', town: 'squareLine' };
    return {
      ink: { water: p.water, river: p.water, urban: p.ink, road1: p.ink, road2: p.ink, airport: p.accent, runway: p.accent, town: p.ink, border: p.ink, names: p.ink },
      alpha: { water: 0.4, river: 0.5, urban: 0.1, road1: 0.35, road2: 0.25, airport: 0.75, runway: 0.6, town: 0.55, border: 0.28, names: 0.6 },
      line: { road1: [2, 'solid'], road2: [1, 'solid'], river: [1, 'solid'], border: [1, 'long'], runway: [2, 'solid'] },
      symbol: 'diamond', town: 'dot' };
  }
  // each family's map: its inks, the look it starts as, and whether it starts on
  var FAMILY = {
    modern: { on: true, look: 'hint', p: { ink: '#3DBB6E', water: '#2E9C88', accent: '#9CFFC4' } },
    silver: { on: true, look: 'hint', p: { ink: '#C8CED6', water: '#7E9CB8', accent: '#F2F4F7' } },
    brass: { on: true, look: 'hint', p: { ink: '#C9A24E', water: '#7FA3B0', accent: '#E8C46A' } },
    blueprint: { on: true, look: 'drafting', p: { ink: '#F4F8FF', water: '#9FE3FF', accent: '#FFFFFF' } },
    paper: { on: true, look: 'chart', p: { ink: '#2F5C8C', water: '#8FB0C6', accent: '#B85C00', names: '#14202C' } },
    celestial: { on: true, look: 'hint', p: { ink: '#8C7BFF', water: '#5A6BD8', accent: '#FFD27A' } },
    sectional: { on: true, look: 'chart', p: { ink: '#1F4E8C', water: '#8CC4EA', accent: '#C2187A', names: '#1F2A44' } },
    station: { on: true, look: 'hint', p: { ink: '#555555', water: '#6A8AA8', accent: '#E3000F', names: '#333333' } },
    blank: { on: false, look: 'hint', p: { ink: '#9A9A9A', water: '#6A7A8A', accent: '#D0D0D0' } },
  };
  function paletteOf(base) { return (FAMILY[base] || FAMILY.modern).p; }
  // the map part a family starts with (off, for a design saved before the map: complete())
  // the names default to the words' own face and size: the bake shares one set of glyphs
  // between fonts of one face and size, so they cost the theme nothing (the shelf's art pool
  // is ~8 MB for four themes; a font of their own was 165 KB)
  function preset(base, face, size) {
    var F = FAMILY[base] || FAMILY.modern, L = look(F.look, F.p);
    return { on: F.on, look: F.look, strength: 0.5, water: 'all', lakes: 'medium', roads: 'main', airports: 'runways', symbol: L.symbol,
      fields: 'airline', dots: true, names: 'normal', which: 'cities', codes: false, urban: true, borders: true,
      ink: L.ink, alpha: L.alpha, line: L.line, town: L.town, nameFace: face || 'IBM Plex Sans', nameSize: size || 16 };
  }
  // a look over a design's map: its inks, opacities, lines and marks; what is drawn is kept
  function applyLook(m, kind, base) {
    var L = look(kind, paletteOf(base)), n = clone(m);
    n.look = kind; n.ink = L.ink; n.alpha = L.alpha; n.line = L.line; n.symbol = L.symbol; n.town = L.town;
    return n;
  }

  /* ---- what each range draws ---- */
  function bit(r) { return 1 << R[r]; }
  // ranges nearest first (main.c RANGE_NAME: Close, City, Area, Local): the roles drawn, the marks dotted and the names shown
  function ranges(m) {
    var out = [];
    for (var k = 0; k < N_RANGES; k++) {
      var on = 0, marks = 0, near = k === 0;
      if (m.on) {
        if (m.water !== 'off') on |= bit('water') | bit('lake');
        if (m.water === 'all') { on |= bit('coast'); if (k < 3) on |= bit('river'); }
        if (m.urban) on |= bit('urban');
        if (m.roads !== 'off') on |= bit('road1');
        if (m.roads === 'main' && near) on |= bit('road2');
        if (m.borders) on |= bit('state') | bit('border');
        if (m.airports === 'runways' && near) { on |= bit('runway'); marks |= 8 | 0x80; }   // the airport kind bit and BARE: a runway-less airport keeps its symbol
        if (m.airports === 'symbols' || (m.airports === 'runways' && !near)) { on |= bit('airport'); marks |= 8; }
        if (m.dots) { on |= bit('city'); marks |= 1 | (m.which === 'towns' ? 2 : 0); }
        if (!on) on = bit('land');                        // the layer still opens: nothing drawn
      }
      out.push({ role_on: on, mark_on: marks, names: m.on ? N_OF[m.names] | 0 : 0 });
    }
    return out;
  }
  function argb(hex, a) {
    var n = parseInt(String(hex || '#000000').slice(1), 16) >>> 0, al = Math.max(0, Math.min(255, Math.round(a * 255)));
    return ((al << 24) | (n & 0xFFFFFF)) >>> 0;
  }
  // the role's colour at the design's strength: 0.5 is the look's own opacity, 1 doubles it
  function alphaOf(m, key) { return Math.max(0, Math.min(1, (m.alpha[key] || 0) * (m.strength / 0.5))); }
  function pal(m) {
    var P = new Array(16).fill(0), put = function (role, key) { P[R[role]] = argb(m.ink[key], alphaOf(m, key)); };
    put('water', 'water'); put('lake', 'water'); put('coast', 'river'); put('river', 'river'); put('urban', 'urban');
    put('road1', 'road1'); put('road2', 'road2'); put('state', 'border'); put('border', 'border');
    put('airport', 'airport'); put('runway', 'runway'); put('city', 'town');
    return P;                                         // land and the spare roles: clear
  }
  function shapeLook(shape, r) { return root.GorbMaps ? root.GorbMaps.look(shape, r) : 0; }
  // widths and dashes (main.c passes both to the view verbatim): the lines' own, the marks'
  // looks and rank caps, the lakes' floor
  function strokes(m) {
    var w = new Array(16).fill(0), d = new Array(16).fill(0);
    var ln = function (role, key) { var l = m.line[key] || [1, 'solid']; w[R[role]] = Math.max(1, Math.min(4, l[0] | 0)); d[R[role]] = DASH[l[1]] | 0; };
    ln('road1', 'road1'); ln('road2', 'road2'); ln('river', 'river'); ln('coast', 'river'); ln('state', 'border'); ln('border', 'border'); ln('runway', 'runway');
    w[R.river] |= RIVER_FLOOR << 3;                   // the creeks the tiles call rivers: a piece under 48 px goes
    w[R.airport] = shapeLook(m.symbol, 6);
    w[R.city] = shapeLook(m.town, m.town === 'dot' ? 3 : 4);
    d[R.airport] = m.fields === 'airline' ? 1 : 0;
    d[R.lake] = FLOOR[m.lakes] | 0;
    d[R.water] = 2;                                   // a sea's crumbs at the window's edge
    d[R.urban] = 10;                                  // the built-up area's crumbs: under 20 px
    return { width: w, dash: d };
  }
  function nameKinds(m) { return (m.which === 'towns' ? 3 : 1) | (m.codes ? 8 : 0); }

  /* ---- the values: fl_values_t, little-endian, 120 B ---- */
  function valuesBytes(m) {
    var b = new Uint8Array(120), dv = new DataView(b.buffer), P = pal(m), S = strokes(m), Rg = ranges(m), k;
    for (k = 0; k < 16; k++) dv.setUint32(4 * k, P[k], true);
    for (k = 0; k < 16; k++) { b[64 + k] = S.width[k]; b[80 + k] = S.dash[k]; }
    for (k = 0; k < N_RANGES; k++) {
      dv.setUint16(96 + 2 * k, Rg[k].role_on, true);
      b[104 + k] = Rg[k].mark_on;
      b[108 + k] = Rg[k].names;
    }
    b[112] = nameKinds(m);
    b[113] = 0;                                       // any rank: the OS keeps the best 32
    b[114] = 0;                                       // the layer's depth: over the ground, under the arm
    b[115] = CLIP;
    b[116] = Math.max(14, Math.min(40, m.nameSize | 0));
    return b;
  }

  /* ---- the preview: the OS's view of the owner's map, at one range ---- */
  var RING_R = 184, G = 466, CX = 233;
  function radiusM(rangeKm) { return Math.round(rangeKm * 1000 * CX / RING_R); }
  // the level a range draws from (orb_map.c pick_level, as maps@1 has it)
  function levelOf(lat, rangeKm) { return root.GorbMaps.pickLevel(Math.round(lat * 1e6), radiusM(rangeKm)); }
  // one range's map: a canvas of the layer as the orb blends it, and the names main.c would place
  function view(m, bytes, lat, rangeKm, k) {
    var GM = root.GorbMaps, lv = lat == null ? { scale: 1 } : levelOf(lat, rangeKm), Rg = ranges(m)[k], S = strokes(m);   // null: a made-up place at its own scale
    var r = GM.render(bytes, { scale: lv.scale, rot_rad: 0, role_on: Rg.role_on, mark_on: Rg.mark_on, width: S.width, dash: S.dash, clip: CLIP });
    if (r.err) throw new Error('the map could not be drawn (' + r.err + ')');
    var c = document.createElement('canvas'); c.width = c.height = G;
    var x = c.getContext('2d'), im = x.createImageData(G, G);
    GM.paint(r.raster, pal(m), im.data);
    x.putImageData(im, 0, 0);
    return { canvas: c, names: names(m, r.marks, Rg.names) };
  }
  // main.c names_draw: best rank first, kept inside the rings, off home, the range figure and each other
  function names(m, marks, want) {
    var kinds = nameKinds(m), cw = (m.nameSize >> 1) + 1, lh = (m.nameSize | 0) + 6, out = [], seen = 0;
    for (var i = 0; i < marks.length && seen < 32 && out.length < want; i++) {
      var mk = marks[i];
      if (mk.x < -64 || mk.x > G + 64 || mk.y < -64 || mk.y > G + 64) continue;
      seen++;
      if (!((kinds >> mk.kind) & 1)) continue;
      var nm = new TextEncoder().encode(mk.name).subarray(0, 23), text = new TextDecoder().decode(nm);
      var w = cw * nm.length + 8, x = Math.round(mk.x), y = Math.round(mk.y) + (lh >> 1) + 6, dx = x - CX, dy = y - CX;
      if (dx * dx + dy * dy > (RING_R - 24) * (RING_R - 24)) continue;
      if (dx * dx + dy * dy < 900) continue;
      if (x + (w >> 1) > CX + 70 && y + (lh >> 1) > CX + 116) continue;
      var hit = out.some(function (o) { return Math.abs(x - o.x) < ((w + o.w) >> 1) && Math.abs(y - o.y) < lh; });
      if (!hit) out.push({ x: x, y: y, w: w, text: text });
    }
    return out;
  }

  root.FlightsMap = {
    R: R, N_RANGES: N_RANGES, N_NAMES: N_NAMES, NAME_SLOT0: NAME_SLOT0, VALUES_VER: VALUES_VER, CLIP: CLIP,
    INKS: INKS, LINES: LINES, LOOKS: LOOKS, SYMBOLS: SYMBOLS, LAKES: LAKES, NAMES: NAMES, DASHES: Object.keys(DASH),
    preset: preset, applyLook: applyLook, ranges: ranges, pal: pal, strokes: strokes, valuesBytes: valuesBytes,
    alphaOf: alphaOf, view: view, levelOf: levelOf, radiusM: radiusM, paletteOf: paletteOf,
  };
})(typeof window !== 'undefined' ? window : globalThis);
