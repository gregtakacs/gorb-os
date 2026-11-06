/* weather-bake.js — the weather app's theme, baked in the browser (docs/weather-plan.md §2.4, §2.6).
 *
 * A design (schema 1, below) drawn on canvases and assembled by the OS's stage bake
 * (GorbBake.stage3) into the rows apps/weather/main.c reads, every one baked centred on the
 * glass for the app to pose:
 *   - the plate: the ground and its bezel (rings, ticks, grid, stars, the lubber);
 *   - slot 0: the large icon, ten picked frames (the conditions, wx.h WX_*);
 *   - slots 1..5: the small icons, one bank of the same ten;
 *   - slots 6..17: the hour bars, one bank of 24 (8 lengths x 3 rain levels, wx_bar_frame);
 *   - slots 18..22: the day bars, one bank of 28 (lo/hi positions, wx_day_frame);
 *   - slot 23: the place dots, 10 frames (wx_dot_frame);
 *   - fonts 0..2 (body, quiet, big) and the labels at 24..31;
 *   - the picker's preview.
 * Every theme carries its own icon pack (§0 ruling 6): a drawing style with named colours,
 * baked into the theme's frames. Needs a document (Image, canvas, document.fonts) and GorbBake. */
(function (root) {
  'use strict';

  var SCHEMA = 1, G = 466, CX = 233, NONE = 0xF;
  var CHARSET = (function () {
    var c = [];
    for (var i = 0x20; i < 0x7F; i++) c.push(String.fromCharCode(i));
    return c.concat(['↑', '↓', '→', '←', '°']);
  })();
  // the ten conditions, in the app's frame order (wx.h WX_CLEAR..WX_THUNDER)
  var KINDS = ['clear', 'clearN', 'partly', 'partlyN', 'cloudy', 'fog', 'drizzle', 'rain', 'snow', 'thunder'];
  var KIND_TITLES = ['Clear', 'Clear night', 'Partly cloudy', 'Partly, night', 'Cloudy', 'Fog', 'Drizzle', 'Rain', 'Snow', 'Thunder'];
  // main.c's slots and sizes
  // the sizes are the pool's (§4 item 4): a theme stands beside a 5 MB clock and the tracker
  var ICON_PX = 72, SICON_PX = 36, N_SICON = 5, BAR_W = 10, BAR_H = 80, N_BAR = 12, BAR_LEN = 8, BAR_RAIN = 3;
  var DAY_W = 88, DAY_H = 10, N_DAYB = 5, DAY_POS = 7, DOT_W = 64, DOT_H = 12, PITCH = 56, DAYS = 5;
  var SLOT = { icon: 0, sicon: 1, bar: 6, day: 18, dots: 23, title: 24, big: 25, line: 26, detail: 27, colA: 28, colB: 29, colC: 30, colD: 31 };
  var LIVE_ROWS = 1 + N_SICON + N_BAR + N_DAYB + 1 + 8;
  // the fonts by slot: [face role, px, colour role, cell height]. A label's lines stand one
  // cell apart (orb_label.c), so the body and muted fonts, which the day columns draw in,
  // stand in a cell of half the row pitch: a column puts a blank line between its days
  var COL_CELL = PITCH / 2;
  // Three fonts, each a hundred glyphs whatever it is asked for (theme_schema FONT_CHARSET),
  // so each is weighed: the quiet one is the title's and the details' (in the label face,
  // capitals where the theme writes them), and the day columns dim by opacity, not a font.
  var FONTS = [['text', 16, 'ink', COL_CELL], ['label', 14, 'quiet'], ['num', 44, 'ink']];
  var FACES = ['IBM Plex Sans', 'IBM Plex Mono', 'Space Grotesk', 'Space Mono', 'Barlow Condensed', 'Jost',
    'Oswald', 'Michroma', 'Orbitron', 'Playfair Display', 'Cormorant Garamond', 'DM Serif Display', 'VT323'];

  /* ---- the six starter themes (§0 ruling 5), each on a flights or clock look ---- */
  var PACKS = {
    phosphor: { title: 'Phosphor', note: 'glowing radar strokes; blips for drizzle, crosshairs for snow', roles: [['stroke', 'Stroke'], ['warn', 'Lightning']] },
    engraved: { title: 'Engraved', note: 'polished metal with an engraved inner line; a sixteen-ray sunburst', roles: [['metal', 'Metal'], ['hi', 'Highlight'], ['deep', 'Engraving'], ['warn', 'Lightning']] },
    drafted: { title: 'Drafted', note: 'linework over dashed construction circles; hexagon snow, arrowed rain', roles: [['line', 'Line'], ['guide', 'Construction'], ['warn', 'Lightning']] },
    folded: { title: 'Folded', note: 'two-tone paper facets with a cast shadow; a pinwheel sun', roles: [['ink', 'Ink'], ['inkD', 'Ink fold'], ['warm', 'Sun'], ['warmD', 'Sun fold'], ['paper', 'Paper'], ['shade', 'Paper fold'], ['shadow', 'Shadow'], ['warn', 'Lightning']] },
    nebula: { title: 'Nebula', note: 'translucent glows; a cratered full moon, meteor rain, star snow', roles: [['gold', 'Sun'], ['core', 'Sun core'], ['orange', 'Lightning'], ['violet', 'Cloud'], ['lilac', 'Cloud light'], ['moon', 'Moon'], ['crater', 'Craters'], ['rain', 'Rain']] },
    pictogram: { title: 'Pictogram', note: 'solid railway pictograms with one accent: the sun and the bolt', roles: [['ink', 'Ink'], ['red', 'Accent']] },
  };
  var GROUNDS = { scope: 'Phosphor scope', brass: 'Brass instrument', blueprint: 'Blueprint', paper: 'Chart paper', celestial: 'Galaxy', station: 'Station dial' };
  var MONO = 'IBM Plex Mono', PLEX = 'IBM Plex Sans', CORM = 'Cormorant Garamond', GROT = 'Space Grotesk', MICH = 'Michroma', JOST = 'Jost';
  var PRESETS_DEF = {
    scope: { name: 'Scope', match: 'Flights · Modern',
      plate: { style: 'scope', c0: '#12361F', c1: '#0E2A1A', c2: '#08170E', detail: '#2A6A44', tick: '#3DBB6E' },
      words: { num: MONO, text: PLEX, label: MONO, caps: true, track: 2, ink: '#E6F0EA', quiet: '#8A9A90' },
      temps: { ramp: ['#2E9C88', '#3DFF8E', '#FFB23C'], rain: '#59D8C8', track: '#1C4A2E', cap: 'round' },
      icons: { pack: 'phosphor', c: { stroke: '#3DFF8E', warn: '#FFB23C' } } },
    brass: { name: 'Vintage brass', match: 'Flights · Vintage brass, Clock · Aviator',
      plate: { style: 'brass', c0: '#2E332E', c1: '#262A26', c2: '#1A1D1A', detail: '#C9A24E', tick: '#E9DDBF' },
      words: { num: CORM, text: CORM, label: CORM, caps: true, track: 3, ink: '#F2E6C8', quiet: '#A8946A' },
      temps: { ramp: ['#7FA3B0', '#C9A24E', '#FF9A3C'], rain: '#8FB7C9', track: '#3A3A30', cap: 'round' },
      icons: { pack: 'engraved', c: { metal: '#C9A24E', hi: '#F8E7A8', deep: '#6E5320', warn: '#FF9A3C' } } },
    blueprint: { name: 'Blueprint', match: 'Flights · Blueprint',
      plate: { style: 'blueprint', c0: '#255C9E', c1: '#1F5290', c2: '#1A4780', detail: '#DCE9F8', tick: '#DCE9F8' },
      words: { num: MONO, text: MONO, label: MONO, caps: true, track: 2, ink: '#FFFFFF', quiet: '#B8CCE6' },
      temps: { ramp: ['#9FE3FF', '#F4F8FF', '#FFD27A'], rain: '#9FE3FF', track: '#2C64A6', cap: 'butt' },
      icons: { pack: 'drafted', c: { line: '#F4F8FF', guide: '#DCE9F8', warn: '#FFD27A' } } },
    paper: { name: 'Paper', match: 'Flights · Paper',
      plate: { style: 'paper', c0: '#F1EBDA', c1: '#EAE3CF', c2: '#DDD4BC', detail: '#8FB0C6', tick: '#2F5C8C' },
      words: { num: PLEX, text: PLEX, label: PLEX, caps: false, track: 1, ink: '#14202C', quiet: '#5B5240' },
      temps: { ramp: ['#2F5C8C', '#1E6B2E', '#B85C00'], rain: '#2F5C8C', track: '#D3C9AE', cap: 'round' },
      icons: { pack: 'folded', c: { ink: '#2F5C8C', inkD: '#1F4066', warm: '#D9822B', warmD: '#B5651A', paper: '#FBF8F0', shade: '#D6CCB2', shadow: '#BFB497', warn: '#B85C00' } } },
    celestial: { name: 'Celestial', match: 'Flights · Celestial, Clock · Orbit',
      plate: { style: 'celestial', c0: '#251A55', c1: '#1A1240', c2: '#0B0820', detail: '#B04CFF', tick: '#B9B2FF' },
      words: { num: GROT, text: GROT, label: MICH, caps: true, track: 2, ink: '#EDEBFF', quiet: '#9C95C9' },
      temps: { ramp: ['#8C7BFF', '#FFD27A', '#FF8A5C'], rain: '#9FD0FF', track: '#2B2260', cap: 'round' },
      icons: { pack: 'nebula', c: { gold: '#FFD27A', core: '#FFF3C8', orange: '#FF8A5C', violet: '#8C7BFF', lilac: '#C9BEFF', moon: '#D9D4FF', crater: '#ABA3EE', rain: '#9FD0FF' } } },
    station: { name: 'Station', match: 'Clock · Station',
      plate: { style: 'station', c0: '#FFFFFF', c1: '#F4F4F2', c2: '#E6E6E3', detail: '#111111', tick: '#111111' },
      words: { num: JOST, text: JOST, label: JOST, caps: true, track: 2, ink: '#111111', quiet: '#555555' },
      temps: { ramp: ['#111111', '#8A8A8A', '#E3000F'], rain: '#111111', track: '#DADAD6', cap: 'butt' },
      icons: { pack: 'pictogram', c: { ink: '#111111', red: '#E3000F' } } },
  };
  var PRESETS = Object.keys(PRESETS_DEF);
  var clone = function (o) { return JSON.parse(JSON.stringify(o)); };
  function preset(k) {
    if (k === 'blank') {
      var b = clone(PRESETS_DEF.scope);
      b.name = 'Blank'; b.base = 'blank'; b.match = '';
      b.plate = { style: 'plain', c0: '#141414', c1: '#101010', c2: '#0A0A0A', detail: '#3A3A3A', tick: '#8A8A8A' };
      b.words = { num: PLEX, text: PLEX, label: PLEX, caps: false, track: 1, ink: '#E6E6E6', quiet: '#8F8F8F' };
      b.temps = { ramp: ['#5B8DB8', '#D8D8D8', '#E0A040'], rain: '#7FB0D8', track: '#2E2E2E', cap: 'round' };
      b.icons = { pack: 'pictogram', c: { ink: '#D8D8D8', red: '#E0A040' } };
      return b;
    }
    var d = clone(PRESETS_DEF[k] || PRESETS_DEF.scope);
    d.base = PRESETS_DEF[k] ? k : 'scope';
    return d;
  }
  // a design saved before a field existed takes it from its preset
  function complete(x) {
    var p = preset(x.base === 'blank' || PRESETS_DEF[x.base] ? x.base : 'scope');
    var fill = function (o, dflt) {
      Object.keys(dflt).forEach(function (k) {
        if (o[k] === undefined) o[k] = clone(dflt[k]);
        else if (dflt[k] && typeof dflt[k] === 'object' && !Array.isArray(dflt[k]) && o[k] && typeof o[k] === 'object') fill(o[k], dflt[k]);
      });
      return o;
    };
    var d = fill(clone(x), p);
    // a pack's colours are its own: a pack changed in the editor takes that pack's starting colours
    var roles = PACKS[d.icons.pack] ? PACKS[d.icons.pack].roles : PACKS.pictogram.roles;
    var start = packStart(d.icons.pack);
    roles.forEach(function (r) { if (!d.icons.c[r[0]]) d.icons.c[r[0]] = start[r[0]]; });
    return d;
  }
  // the colours a pack starts from: those of the starter theme that wears it
  function packStart(pack) {
    for (var k in PRESETS_DEF) if (PRESETS_DEF[k].icons.pack === pack) return clone(PRESETS_DEF[k].icons.c);
    return clone(PRESETS_DEF.station.icons.c);
  }

  /* ---- rasterising ---- */
  function canvas(w, h) { var c = document.createElement('canvas'); c.width = w; c.height = h; return c; }
  function image(src) {
    return new Promise(function (ok, bad) {
      var im = new Image();
      im.onload = function () { ok(im); };
      im.onerror = function () { bad(new Error('an image did not draw')); };
      im.src = src;
    });
  }
  function svgDoc(vb, body, defs) {
    return '<svg xmlns="http://www.w3.org/2000/svg" viewBox="' + vb + '">' + (defs ? '<defs>' + defs + '</defs>' : '') + body + '</svg>';
  }
  function svgImage(svg) { return image('data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svg)); }
  function plane(c) {
    var x = c.getContext('2d').getImageData(0, 0, c.width, c.height);
    return { w: c.width, h: c.height, rgba: new Uint8Array(x.data.buffer) };
  }
  async function rast(svg, w, h) {
    var c = canvas(w, h);
    c.getContext('2d').drawImage(await svgImage(svg), 0, 0, w, h);
    return c;
  }
  async function loadFace(face, px, text) {
    try { await document.fonts.load(px + 'px "' + face + '"', text || 'N0123456789'); } catch (e) { /* the fallback face draws */ }
  }
  function hex(c) { return [parseInt(c.slice(1, 3), 16), parseInt(c.slice(3, 5), 16), parseInt(c.slice(5, 7), 16)]; }
  function mix(a, b, t) { var A = hex(a), B = hex(b); return '#' + A.map(function (v, i) { return Math.round(v + (B[i] - v) * t).toString(16).padStart(2, '0'); }).join(''); }
  function ramp(st, t) { t = Math.max(0, Math.min(1, t)); return t < 0.5 ? mix(st[0], st[1], t * 2) : mix(st[1], st[2], (t - 0.5) * 2); }

  /* ---- path geometry (the design mockups' own, docs/weather-plan.md §2.4) ---- */
  var DEG = Math.PI / 180, f = function (n) { return Math.round(n * 100) / 100; };
  function pt(r, a, cx, cy) { if (cx === undefined) { cx = CX; cy = CX; } return [cx + r * Math.sin(a * DEG), cy - r * Math.cos(a * DEG)]; }
  function circ(cx, cy, r) { return 'M' + f(cx - r) + ',' + f(cy) + 'a' + f(r) + ',' + f(r) + ' 0 1,0 ' + f(2 * r) + ',0a' + f(r) + ',' + f(r) + ' 0 1,0 ' + f(-2 * r) + ',0Z'; }
  function ell(cx, cy, rx, ry) { return 'M' + f(cx - rx) + ',' + f(cy) + 'a' + f(rx) + ',' + f(ry) + ' 0 1,0 ' + f(2 * rx) + ',0a' + f(rx) + ',' + f(ry) + ' 0 1,0 ' + f(-2 * rx) + ',0Z'; }
  function rotEll(cx, cy, rx, ry, rot) {
    var c = Math.cos(rot * DEG), s = Math.sin(rot * DEG), x1 = cx + rx * c, y1 = cy + rx * s, x2 = cx - rx * c, y2 = cy - rx * s;
    return 'M' + f(x1) + ',' + f(y1) + 'A' + f(rx) + ',' + f(ry) + ' ' + rot + ' 1,0 ' + f(x2) + ',' + f(y2) + 'A' + f(rx) + ',' + f(ry) + ' ' + rot + ' 1,0 ' + f(x1) + ',' + f(y1) + 'Z';
  }
  function ln(x1, y1, x2, y2) { return 'M' + f(x1) + ',' + f(y1) + 'L' + f(x2) + ',' + f(y2); }
  function rad(r1, r2, a, cx, cy) { var p = pt(r1, a, cx, cy), q = pt(r2, a, cx, cy); return ln(p[0], p[1], q[0], q[1]); }
  function poly(ps) { return 'M' + ps.map(function (p) { return f(p[0]) + ',' + f(p[1]); }).join('L') + 'Z'; }
  function tx(d, x, y, k) { var i = 0; return d.replace(/-?\d*\.?\d+/g, function (m) { var v = parseFloat(m); var o = (i++ % 2 === 0) ? x + v * k : y + v * k; return String(f(o)); }); }
  function crescent(cx, cy, R) {
    var p1 = pt(R, 20, cx, cy), p2 = pt(R, 160, cx, cy), r2 = 1.05 * R;
    return 'M' + f(p1[0]) + ',' + f(p1[1]) + 'A' + f(R) + ',' + f(R) + ' 0 1,0 ' + f(p2[0]) + ',' + f(p2[1]) + 'A' + f(r2) + ',' + f(r2) + ' 0 0,1 ' + f(p1[0]) + ',' + f(p1[1]) + 'Z';
  }
  function S(d, o) { return Object.assign({ d: d, fill: 'none', stroke: 'none', sw: 0, op: 1, dash: 'none', cap: 'round' }, o || {}); }
  function rng(seed) { var s = seed; return function () { s = (s * 1664525 + 1013904223) % 4294967296; return s / 4294967296; }; }
  function tickPath(r1, r2, step, keep) { var d = ''; for (var a = 0; a < 360; a += step) { if (!keep || keep(a)) d += rad(r1, r2, a); } return d; }
  function gridPath(step) { var d = ''; for (var v = step; v < G; v += step) d += ln(v, 0, v, G) + ln(0, v, G, v); return d; }
  function pathsSvg(paths, size, defs, bg) {
    var body = (bg || '') + paths.map(function (p) {
      return '<path d="' + p.d + '" fill="' + p.fill + '" stroke="' + p.stroke + '" stroke-width="' + p.sw + '" opacity="' + p.op +
        '" stroke-dasharray="' + p.dash + '" stroke-linecap="' + p.cap + '" stroke-linejoin="round"/>';
    }).join('');
    return svgDoc('0 0 ' + size + ' ' + size, body, defs);
  }

  /* ---- the icon packs: the shapes, in a 100-unit box ---- */
  var CLOUD = 'M24,74 C14,74 10,66 12,60 C14,53 21,50 27,52 C28,40 38,32 50,33 C61,34 68,42 69,50 C77,49 86,54 86,63 C86,70 80,74 74,74 Z';
  var BUMPS = [[24, 62, 12], [49, 50, 17], [76, 63, 10]];
  var BOLT = 'M56,50 L40,74 L51,74 L45,94 L64,66 L53,66 L60,50 Z';
  var DROP = 'M0,-8 C2.5,-3.5 5,-0.5 5,2.5 C5,5.6 2.8,7.6 0,7.6 C-2.8,7.6 -5,5.6 -5,2.5 C-5,-0.5 -2.5,-3.5 0,-8 Z';
  var STAR4 = 'M0,-8 C1,-2 2,-1 8,0 C2,1 1,2 0,8 C-1,2 -2,1 -8,0 C-2,-1 -1,-2 0,-8 Z';
  function cloudObj(ox, oy, u, k, dx, dy) {
    var cx = ox + (48 + dx) * u, cy = oy + (54 + dy) * u, kk = u * k;
    var sub = function (k2, ddx, ddy) { k2 = k2 || 1; ddx = ddx || 0; ddy = ddy || 0; return tx(CLOUD, cx + ddx - 48 * kk * k2, cy + ddy - 54 * kk * k2, kk * k2); };
    return { d: sub(1), sub: sub, m: function (x, y) { return [cx + (x - 48) * kk, cy + (y - 54) * kk]; }, k: kk };
  }
  function makePack(name, c, bg) {
    var pk;
    if (name === 'phosphor') {
      var G1 = c.stroke, W1 = c.warn;
      var glow = function (d, u, o) { o = o || {}; var col = o.col || G1, dash = o.dash || 'none'; return [S(d, { stroke: col, sw: 7 * u, op: 0.16, dash: dash }), S(d, { stroke: col, sw: (o.w || 2.2) * u, dash: dash, op: o.op || 1 })]; };
      pk = {
        sun: function (x, y, R, u) { var r = ''; for (var i = 0; i < 8; i++) r += rad(1.4 * R, 1.85 * R, i * 45, x, y); return glow(circ(x, y, R), u).concat(glow(r, u), [S(circ(x, y, R * 0.3), { fill: G1, op: 0.9 })]); },
        moon: function (x, y, R, u) { return glow(crescent(x, y, R), u).concat([S(circ(x + R, y - R * 0.75, 6 * u), { fill: G1, op: 0.18 }), S(circ(x + R, y - R * 0.75, 2.6 * u), { fill: G1 }), S(circ(x + R * 1.25, y + R * 0.25, 1.7 * u), { fill: G1, op: 0.75 })]); },
        cloud: function (cl, u) { return [S(cl.d, { fill: bg })].concat(glow(cl.d, u)); },
        rain: function (x, y, u) { return glow(ln(x + 3 * u, y - 8 * u, x - 3 * u, y + 7 * u), u, { dash: f(3.5 * u) + ' ' + f(3 * u) }); },
        drizzle: function (x, y, u) { return [S(circ(x, y, 6 * u), { fill: G1, op: 0.16 }), S(circ(x, y, 2.6 * u), { fill: G1 })]; },
        snow: function (x, y, u) { return glow(ln(x - 5.5 * u, y, x + 5.5 * u, y) + ln(x, y - 5.5 * u, x, y + 5.5 * u), u, { w: 1.8 }); },
        bolt: function (ox, oy, u) { var d = tx(BOLT, ox, oy, u); return [S(d, { fill: bg })].concat(glow(d, u, { col: W1 })); },
        fog: function (ls, u) { var o = []; ls.forEach(function (l, i) { o = o.concat(glow(ln(l[0], l[1], l[2], l[1]), u, { dash: i % 2 ? f(12 * u) + ' ' + f(5 * u) : 'none', op: 1 - i * 0.15 })); }); return o; },
      };
    } else if (name === 'engraved') {
      var metal = c.metal, hi = c.hi, deep = c.deep, W2 = c.warn;
      pk = {
        sun: function (x, y, R, u) { var ps = []; for (var i = 0; i < 32; i++) ps.push(pt(i % 2 ? 1.28 * R : 1.8 * R, i * 11.25, x, y)); return [S(poly(ps), { fill: metal, stroke: deep, sw: 1.1 * u }), S(circ(x, y, R), { fill: hi, stroke: deep, sw: 1.4 * u }), S(circ(x, y, 0.68 * R), { stroke: deep, sw: 0.9 * u })]; },
        moon: function (x, y, R, u) { return [S(crescent(x, y, R), { fill: hi, stroke: deep, sw: 1.3 * u }), S(tx(STAR4, x + 1.05 * R, y - 0.8 * R, 0.7 * u), { fill: metal, stroke: deep, sw: 0.6 * u })]; },
        cloud: function (cl, u) { return [S(cl.d, { fill: metal, stroke: deep, sw: 1.4 * u }), S(cl.sub(0.78, 0, u), { stroke: deep, sw: 0.9 * u, op: 0.8 })]; },
        rain: function (x, y, u) { return [S(tx(DROP, x, y, u), { fill: hi, stroke: deep, sw: 0.9 * u })]; },
        drizzle: function (x, y, u) { return [S(circ(x, y, 2.6 * u), { fill: hi, stroke: deep, sw: 0.8 * u })]; },
        snow: function (x, y, u) { var d = ''; for (var i = 0; i < 3; i++) { var p = pt(6.5 * u, i * 60, x, y), q = pt(6.5 * u, i * 60 + 180, x, y); d += ln(p[0], p[1], q[0], q[1]); } return [S(d, { stroke: deep, sw: 3.4 * u }), S(d, { stroke: hi, sw: 1.6 * u })]; },
        bolt: function (ox, oy, u) { return [S(tx(BOLT, ox, oy, u), { fill: W2, stroke: deep, sw: 1.2 * u })]; },
        fog: function (ls, u) { var o = []; ls.forEach(function (l) { var d = ln(l[0], l[1], l[2], l[1]); o.push(S(d, { stroke: deep, sw: 7.4 * u })); o.push(S(d, { stroke: metal, sw: 5.6 * u })); o.push(S(ln(l[0] + 3 * u, l[1], l[2] - 3 * u, l[1]), { stroke: deep, sw: 0.8 * u, op: 0.8 })); }); return o; },
      };
    } else if (name === 'drafted') {
      var Wc = c.line, Gd = c.guide, Y = c.warn;
      var dsh = function (u) { return f(2.2 * u) + ' ' + f(2.6 * u); };
      pk = {
        frame: function (cx, cy, u) { return [S(circ(cx, cy, 47 * u), { stroke: Gd, sw: 0.8 * u, op: 0.3, dash: dsh(u) }), S(ln(cx - 4 * u, cy, cx + 4 * u, cy) + ln(cx, cy - 4 * u, cx, cy + 4 * u), { stroke: Gd, sw: 0.8 * u, op: 0.45 })]; },
        sun: function (x, y, R, u) { var r = ''; for (var i = 0; i < 8; i++) r += rad(1.5 * R, 1.9 * R, i * 45, x, y); return [S(circ(x, y, 1.28 * R), { stroke: Gd, sw: 0.9 * u, op: 0.55, dash: dsh(u) }), S(circ(x, y, R), { stroke: Wc, sw: 1.6 * u }), S(r, { stroke: Wc, sw: 1.6 * u })]; },
        moon: function (x, y, R, u) { return [S(circ(x, y, R), { stroke: Gd, sw: 0.9 * u, op: 0.5, dash: dsh(u) }), S(crescent(x, y, R), { stroke: Wc, sw: 1.6 * u })]; },
        cloud: function (cl, u) { var o = [S(cl.d, { fill: bg })]; BUMPS.forEach(function (b) { var m = cl.m(b[0], b[1]); o.push(S(circ(m[0], m[1], b[2] * cl.k), { stroke: Gd, sw: 0.8 * u, op: 0.4, dash: dsh(u) })); }); o.push(S(cl.d, { stroke: Wc, sw: 1.6 * u })); return o; },
        rain: function (x, y, u) { var x2 = x - 3 * u, y2 = y + 7 * u; return [S(ln(x + 3 * u, y - 7 * u, x2, y2) + ln(x2, y2, x2 - 0.5 * u, y2 - 4 * u) + ln(x2, y2, x2 + 3.2 * u, y2 - 2.4 * u), { stroke: Wc, sw: 1.4 * u })]; },
        drizzle: function (x, y, u) { return [S(ln(x, y - 3 * u, x, y + 3 * u), { stroke: Wc, sw: 1.4 * u })]; },
        snow: function (x, y, u) { var ps = []; for (var i = 0; i < 6; i++) ps.push(pt(5.2 * u, i * 60 + 30, x, y)); return [S(poly(ps), { stroke: Wc, sw: 1.3 * u }), S(circ(x, y, 1.1 * u), { fill: Wc })]; },
        bolt: function (ox, oy, u) { return [S(tx(BOLT, ox, oy, u), { fill: bg, stroke: Y, sw: 1.6 * u })]; },
        fog: function (ls, u) { var d = ''; ls.forEach(function (l) { d += ln(l[0], l[1], l[2], l[1]) + ln(l[0], l[1] - 3 * u, l[0], l[1] + 3 * u) + ln(l[2], l[1] - 3 * u, l[2], l[1] + 3 * u); }); return [S(d, { stroke: Wc, sw: 1.3 * u })]; },
      };
    } else if (name === 'folded') {
      var navy = c.ink, navyD = c.inkD, warm = c.warm, warmD = c.warmD, pw = c.paper, shade = c.shade, shd = c.shadow, W3 = c.warn;
      pk = {
        sun: function (x, y, R, u) {
          var sh = 2.4 * u, o = [S(circ(x + sh, y + sh, 1.3 * R), { fill: shd, op: 0.55 })];
          for (var i = 0; i < 8; i++) { var a = i * 45, tip = pt(1.9 * R, a, x, y), l = pt(1.12 * R, a - 15, x, y), r = pt(1.12 * R, a + 15, x, y), m = pt(0.95 * R, a, x, y); o.push(S(poly([l, tip, m]), { fill: warm })); o.push(S(poly([m, tip, r]), { fill: warmD })); }
          o.push(S(circ(x, y, R), { fill: warm }));
          var t = pt(R, 0, x, y), b = pt(R, 180, x, y);
          o.push(S('M' + f(t[0]) + ',' + f(t[1]) + 'A' + f(R) + ',' + f(R) + ' 0 0,1 ' + f(b[0]) + ',' + f(b[1]) + 'Z', { fill: warmD, op: 0.55 }));
          return o;
        },
        moon: function (x, y, R, u) { var sh = 2.4 * u; return [S(crescent(x + sh, y + sh, R), { fill: shd }), S(crescent(x, y, R), { fill: navy }), S(tx(STAR4, x + 1.1 * R, y - 0.8 * R, 0.6 * u), { fill: navyD })]; },
        cloud: function (cl, u) { var sh = 2.4 * u; return [S(cl.sub(1, sh, sh), { fill: shd, op: 0.9 }), S(cl.d, { fill: shade }), S(cl.sub(0.95, -1.2 * u, -1.8 * u), { fill: pw })]; },
        rain: function (x, y, u) { var sh = 1.8 * u; return [S(tx(DROP, x + sh, y + sh, u), { fill: shd }), S(tx(DROP, x, y, u), { fill: navy })]; },
        drizzle: function (x, y, u) { return [S(circ(x + 1.5 * u, y + 1.5 * u, 2.8 * u), { fill: shd }), S(circ(x, y, 2.8 * u), { fill: navy })]; },
        snow: function (x, y, u) { var d = '', d2 = '', sh = 1.6 * u; for (var i = 0; i < 3; i++) { var p = pt(6.5 * u, i * 60, x, y), q = pt(6.5 * u, i * 60 + 180, x, y); d += ln(p[0], p[1], q[0], q[1]); d2 += ln(p[0] + sh, p[1] + sh, q[0] + sh, q[1] + sh); } return [S(d2, { stroke: shd, sw: 2.6 * u }), S(d, { stroke: navy, sw: 2.6 * u })]; },
        bolt: function (ox, oy, u) { return [S(tx(BOLT, ox + 2.4 * u, oy + 2.4 * u, u), { fill: shd }), S(tx(BOLT, ox, oy, u), { fill: W3 })]; },
        fog: function (ls, u) { var o = []; ls.forEach(function (l) { o.push(S(ln(l[0] + 2 * u, l[1] + 2 * u, l[2] + 2 * u, l[1] + 2 * u), { stroke: shd, sw: 6 * u })); o.push(S(ln(l[0], l[1], l[2], l[1]), { stroke: shade, sw: 6 * u })); o.push(S(ln(l[0], l[1] - 0.8 * u, l[2], l[1] - 0.8 * u), { stroke: pw, sw: 4.2 * u })); }); return o; },
      };
    } else if (name === 'nebula') {
      var gold = c.gold, core = c.core, org = c.orange, vio = c.violet, lil = c.lilac, mc = c.moon, cr = c.crater, rc = c.rain;
      pk = {
        sun: function (x, y, R) { return [S(circ(x, y, 1.85 * R), { fill: gold, op: 0.1 }), S(circ(x, y, 1.5 * R), { fill: gold, op: 0.16 }), S(circ(x, y, 1.22 * R), { fill: gold, op: 0.28 }), S(circ(x, y, R), { fill: gold }), S(circ(x, y, 0.45 * R), { fill: core, op: 0.85 })]; },
        moon: function (x, y, R, u) { return [S(circ(x, y, 1.32 * R), { stroke: vio, sw: 2.6 * u, op: 0.5 }), S(circ(x, y, R), { fill: mc }), S(circ(x - 0.3 * R, y - 0.25 * R, 0.22 * R), { fill: cr, op: 0.85 }), S(circ(x + 0.36 * R, y + 0.12 * R, 0.16 * R), { fill: cr, op: 0.85 }), S(circ(x - 0.04 * R, y + 0.48 * R, 0.12 * R), { fill: cr, op: 0.85 })]; },
        cloud: function (cl, u) { return [S(cl.d, { fill: vio, op: 0.6 }), S(cl.sub(0.74, -2 * u, -3 * u), { fill: lil, op: 0.38 }), S(cl.sub(0.42, -4 * u, -7 * u), { fill: '#FFFFFF', op: 0.2 })]; },
        rain: function (x, y, u) { return [S(ln(x + 4 * u, y - 9 * u, x - 2 * u, y + 5 * u), { stroke: rc, sw: 1.6 * u, op: 0.6 }), S(circ(x - 2 * u, y + 5 * u, 2 * u), { fill: mix(rc, '#FFFFFF', 0.6) })]; },
        drizzle: function (x, y, u) { return [S(circ(x, y, 4.2 * u), { fill: rc, op: 0.18 }), S(circ(x, y, 1.7 * u), { fill: rc })]; },
        snow: function (x, y, u) { return [S(tx(STAR4, x, y, 0.8 * u), { fill: '#FFFFFF' })]; },
        bolt: function (ox, oy, u) { var d = tx(BOLT, ox, oy, u); return [S(d, { stroke: org, sw: 6 * u, op: 0.2 }), S(d, { fill: org })]; },
        fog: function (ls, u) { return ls.map(function (l, i) { return S(ell((l[0] + l[2]) / 2, l[1], (l[2] - l[0]) / 2, 3.2 * u), { stroke: lil, sw: 1.7 * u, op: 0.85 - i * 0.12, dash: '0.1 ' + f(4.2 * u) }); }); },
      };
    } else {
      var K = c.ink, Rd = c.red;
      pk = {
        sun: function (x, y, R, u) { var r = ''; for (var i = 0; i < 8; i++) r += rad(1.35 * R, 1.85 * R, i * 45, x, y); return [S(r, { stroke: K, sw: 4.6 * u, cap: 'butt' }), S(circ(x, y, R), { fill: Rd })]; },
        moon: function (x, y, R) { return [S(crescent(x, y, R), { fill: K })]; },
        cloud: function (cl, u) { return [S(cl.d, { fill: K, stroke: bg, sw: 4 * u })]; },
        rain: function (x, y, u) { return [S(ln(x, y - 6 * u, x, y + 6 * u), { stroke: K, sw: 4.2 * u, cap: 'butt' })]; },
        drizzle: function (x, y, u) { return [S(circ(x, y, 3.1 * u), { fill: K })]; },
        snow: function (x, y, u) { var d = ''; for (var i = 0; i < 3; i++) { var p = pt(6.5 * u, i * 60, x, y), q = pt(6.5 * u, i * 60 + 180, x, y); d += ln(p[0], p[1], q[0], q[1]); } return [S(d, { stroke: K, sw: 2.8 * u, cap: 'butt' })]; },
        bolt: function (ox, oy, u) { return [S(tx(BOLT, ox, oy, u), { fill: Rd, stroke: bg, sw: 2.6 * u })]; },
        fog: function (ls, u) { return [S(ls.map(function (l) { return ln(l[0], l[1], l[2], l[1]); }).join(''), { stroke: K, sw: 5 * u, cap: 'butt' })]; },
      };
    }
    return pk;
  }
  // one condition's paths, centred on cx, cy in an s-px box
  function iconPaths(pk, kind, cx, cy, s) {
    var u = s / 100, ox = cx - s / 2, oy = cy - s / 2, out = [];
    var M = function (x, y) { return [ox + x * u, oy + y * u]; };
    var add = function (a) { a.forEach(function (p) { out.push(p); }); };
    if (pk.frame) add(pk.frame(cx, cy, u));
    var precip = function (fn) { [[34, 80], [51, 88], [68, 80]].forEach(function (q) { var c = M(q[0], q[1]); add(fn(c[0], c[1], u)); }); };
    if (kind === 'clear') add(pk.sun(cx, cy, 24 * u, u));
    else if (kind === 'clearN') add(pk.moon(cx, cy, 27 * u, u));
    else if (kind === 'partly' || kind === 'partlyN') {
      var c = M(66, 36);
      add(kind === 'partly' ? pk.sun(c[0], c[1], 13 * u, u) : pk.moon(c[0], c[1], 16 * u, u));
      add(pk.cloud(cloudObj(ox, oy, u, 0.9, -6, 10), u));
    } else if (kind === 'cloudy') add(pk.cloud(cloudObj(ox, oy, u, 1.12, 0, -2), u));
    else if (kind === 'fog') add(pk.fog([[20, 34, 80], [12, 48, 88], [20, 62, 80], [30, 76, 70]].map(function (l) { var a = M(l[0], l[1]), b = M(l[2], l[1]); return [a[0], a[1], b[0]]; }), u));
    else {
      add(pk.cloud(cloudObj(ox, oy, u, 1, 0, -12), u));
      if (kind === 'thunder') add(pk.bolt(ox, oy, u));
      else precip(kind === 'rain' ? pk.rain : kind === 'snow' ? pk.snow : pk.drizzle);
    }
    return out;
  }
  function packOf(d) { return makePack(d.icons.pack, d.icons.c, d.plate.c1); }
  function iconSvg(d, kind, px) { return pathsSvg(iconPaths(packOf(d), kind, px / 2, px / 2, px), px); }
  async function iconFrames(d, px) {
    var out = [];
    for (var i = 0; i < KINDS.length; i++) out.push(await rast(iconSvg(d, KINDS[i], px), px, px));
    return out;
  }

  /* ---- the plate: the ground and its bezel, one per starter look ---- */
  function chromePaths(p) {
    var st = p.style, det = p.detail, tk = p.tick, o = [];
    if (st === 'scope') {
      o.push(S(circ(CX, CX, 100), { stroke: det, sw: 1.2, op: 0.45 }), S(circ(CX, CX, 160), { stroke: det, sw: 1.2, op: 0.45 }));
      o.push(S(tickPath(214, 224, 5, function (a) { return a % 30; }), { stroke: tk, sw: 1.2, op: 0.7 }), S(tickPath(204, 224, 30), { stroke: tk, sw: 1.8 }));
    } else if (st === 'brass') {
      o.push(S(circ(CX, CX, 228), { stroke: det, sw: 5 }), S(circ(CX, CX, 221), { stroke: det, sw: 1.2, op: 0.8 }));
      o.push(S(tickPath(207, 215, 6, function (a) { return a % 30; }), { stroke: tk, sw: 1, op: 0.55 }));
      o.push(S(tickPath(200, 215, 30, function (a) { return a !== 0; }), { stroke: tk, sw: 2, op: 0.9 }));
      o.push(S(poly([[CX, 40], [CX - 6, 28], [CX + 6, 28]]), { fill: mix(det, '#FFFFFF', 0.3) }));
    } else if (st === 'blueprint') {
      o.push(S(gridPath(23.3), { stroke: '#FFFFFF', sw: 0.8, op: 0.07 }));
      o.push(S(ln(CX, 8, CX, 458) + ln(8, CX, 458, CX), { stroke: det, sw: 1, op: 0.16, dash: '10 4 2 4' }));
      o.push(S(tickPath(209, 220, 6, function (a) { return a % 30; }), { stroke: tk, sw: 1.1, op: 0.65 }), S(tickPath(200, 220, 30), { stroke: tk, sw: 1.6 }));
    } else if (st === 'paper') {
      o.push(S(gridPath(23.3), { stroke: det, sw: 0.7, op: 0.4 }), S(circ(CX, CX, 222), { stroke: tk, sw: 1.4, op: 0.9 }));
      o.push(S(tickPath(202, 214, 6, function (a) { return a % 30; }), { stroke: tk, sw: 1 }), S(tickPath(198, 214, 30, function (a) { return a !== 0; }), { stroke: tk, sw: 1.6 }));
      o.push(S(poly([[CX, 44], [CX - 5, 34], [CX + 5, 34]]), { fill: tk }));
    } else if (st === 'celestial') {
      var r = rng(7);
      o.push(S(rotEll(CX, CX, 330, 80, -28), { fill: det, op: 0.09 }), S(rotEll(CX, CX, 280, 34, -28), { fill: mix(det, '#8C7BFF', 0.5), op: 0.1 }));
      var b = ['', '', ''];
      for (var i = 0; i < 80; i++) { var a = r() * 360, rr = Math.sqrt(r()) * 228, q = pt(rr, a); b[i % 3] += circ(q[0], q[1], 0.5 + r() * 1.1); }
      o.push(S(b[0], { fill: '#FFFFFF', op: 0.3 }), S(b[1], { fill: '#FFFFFF', op: 0.55 }), S(b[2], { fill: '#FFFFFF', op: 0.85 }));
      var dd = ''; for (var k = 0; k < 360; k += 30) { var z = pt(216, k); dd += circ(z[0], z[1], k % 90 ? 1.4 : 2.4); }
      o.push(S(dd, { fill: tk, op: 0.85 }));
    } else if (st === 'station') {
      o.push(S(tickPath(214, 225, 6, function (a) { return a % 30; }), { stroke: tk, sw: 2.6, cap: 'butt' }), S(tickPath(198, 225, 30), { stroke: tk, sw: 7, cap: 'butt' }));
    } else {
      o.push(S(circ(CX, CX, 222), { stroke: det, sw: 1.2 }), S(tickPath(206, 216, 30), { stroke: tk, sw: 1.6 }));
    }
    return o;
  }
  function plateSvg(d) {
    var p = d.plate;
    var defs = '<radialGradient id="g" cx="50%" cy="50%" r="50%"><stop offset="0" stop-color="' + p.c0 + '"/><stop offset="0.6" stop-color="' + p.c1 + '"/><stop offset="1" stop-color="' + p.c2 + '"/></radialGradient>';
    return pathsSvg(chromePaths(p), G, defs, '<rect width="' + G + '" height="' + G + '" fill="url(#g)"/>');
  }
  async function plateCanvas(d) { return rast(plateSvg(d), G, G); }

  /* ---- the moving rows' frames ---- */
  // wx_bar_frame: length level by temperature, the rain dot by chance; the sprite points up,
  // its centre at HR_BAR_R (157) from the glass centre, so y = 197 - r
  function barFrames(d) {
    var T = d.temps, out = [];
    for (var L = 0; L < BAR_LEN; L++) for (var rn = 0; rn < BAR_RAIN; rn++) {
      var c = canvas(BAR_W, BAR_H), x = c.getContext('2d'), r1 = 132, r2 = 148 + L * 6.4;
      x.strokeStyle = ramp(T.ramp, L / (BAR_LEN - 1)); x.lineWidth = 6; x.lineCap = T.cap === 'butt' ? 'butt' : 'round';
      x.beginPath(); x.moveTo(BAR_W / 2, 197 - r1); x.lineTo(BAR_W / 2, 197 - r2); x.stroke();
      if (rn) { x.fillStyle = T.rain; x.beginPath(); x.arc(BAR_W / 2, 197 - 121, 1.4 + rn * 1.2, 0, 2 * Math.PI); x.fill(); }
      out.push(c);
    }
    return out;
  }
  // wx_day_frame: i <= j on the week's scale, triangular, i-major
  function dayFrames(d) {
    var T = d.temps, out = [], X = function (k) { return 5 + k * (78 / (DAY_POS - 1)); };
    for (var i = 0; i < DAY_POS; i++) for (var j = i; j < DAY_POS; j++) {
      var c = canvas(DAY_W, DAY_H), x = c.getContext('2d'), cap = T.cap === 'butt' ? 'butt' : 'round';
      x.lineWidth = 6; x.lineCap = cap; x.strokeStyle = T.track;
      x.beginPath(); x.moveTo(X(0), DAY_H / 2); x.lineTo(X(DAY_POS - 1), DAY_H / 2); x.stroke();
      var g = x.createLinearGradient(X(i), 0, X(j) + 0.01, 0);
      g.addColorStop(0, ramp(T.ramp, i / (DAY_POS - 1))); g.addColorStop(1, ramp(T.ramp, j / (DAY_POS - 1)));
      x.strokeStyle = g;
      x.beginPath(); x.moveTo(X(i), DAY_H / 2); x.lineTo(X(j) + (i === j ? 0.5 : 0), DAY_H / 2); x.stroke();
      out.push(c);
    }
    return out;
  }
  // wx_dot_frame: n places, the selected one bright
  function dotFrames(d) {
    var W = d.words, out = [];
    for (var n = 1; n <= 4; n++) for (var s = 0; s < n; s++) {
      var c = canvas(DOT_W, DOT_H), x = c.getContext('2d');
      for (var k = 0; k < n; k++) {
        x.globalAlpha = k === s ? 1 : 0.6; x.fillStyle = k === s ? W.ink : W.quiet;
        x.beginPath(); x.arc(DOT_W / 2 + (k - (n - 1) / 2) * 14, DOT_H / 2, k === s ? 3.6 : 2.6, 0, 2 * Math.PI); x.fill();
      }
      out.push(c);
    }
    return out;
  }

  /* ---- the words ---- */
  // the face a font role names
  function faceOf(d, role) { return role === 'num' ? d.words.num : role === 'label' ? d.words.label : d.words.text; }
  // one font: every CHARSET glyph in one cell, its ink from the cell's left edge on a shared
  // baseline (flights-bake.js font's rule). `caps` draws the lower case as capitals (the
  // title's small-caps look); `digits` sizes the cell to the figures, the minus and the
  // degree sign alone, which is all the large font is ever asked for, and clips the rest
  async function font(face, px, colour, o) {
    o = o || {};
    var spec = px + 'px "' + face + '"';
    await loadFace(face, px, CHARSET.join(''));
    var m = canvas(4, 4).getContext('2d');
    m.font = spec;
    var drawn = function (ch) { return o.caps ? ch.toUpperCase() : ch; };
    var top = 0, bottom = 0, w = 1, boxes = CHARSET.map(function (ch) {
      var t = m.measureText(drawn(ch));
      return { l: Math.floor(-t.actualBoundingBoxLeft), r: Math.ceil(t.actualBoundingBoxRight), t: Math.ceil(t.actualBoundingBoxAscent), b: Math.ceil(t.actualBoundingBoxDescent) };
    });
    var sizer = o.digits ? function (ch) { return /[0-9\-°%: ]/.test(ch); } : function () { return true; };
    boxes.forEach(function (b, i) {
      if (!sizer(CHARSET[i])) return;
      if (b.r > b.l) { top = Math.max(top, b.t); bottom = Math.max(bottom, b.b); w = Math.max(w, b.r - b.l); }
    });
    if (!o.digits) { var fm = m.measureText('Hg'); top = Math.max(top, Math.ceil(fm.fontBoundingBoxAscent || 0)); bottom = Math.max(bottom, Math.ceil(fm.fontBoundingBoxDescent || 0)); }
    top += 1; bottom += 1;
    // a fixed cell: the ink centred in it, clipped if a face is taller than the cell
    if (o.cell) { var spare = o.cell - (top + bottom); top += Math.floor(spare / 2); bottom = o.cell - top; }
    var h = top + bottom, glyphs = boxes.map(function (b, i) {
      var c = canvas(w, h), x = c.getContext('2d');
      x.font = spec; x.textBaseline = 'alphabetic'; x.fillStyle = colour;
      if (b.r > b.l) x.fillText(drawn(CHARSET[i]), -b.l, top);
      return c;
    });
    return { w: w, h: h, glyphs: glyphs, space: Math.max(1, Math.round(m.measureText(' ').width)), track: Math.max(0, o.track | 0) };
  }
  async function fonts(d) {
    var W = d.words, out = [];
    for (var i = 0; i < FONTS.length; i++) {
      var fr = FONTS[i];
      out.push(await font(faceOf(d, fr[0]), fr[1], W[fr[2]], { caps: fr[0] === 'label' && W.caps, digits: fr[0] === 'num', track: fr[0] === 'label' ? Math.min(2, W.track) : 1, cell: fr[3] }));
    }
    return out;
  }
  // the labels: slot, fonts by tone (normal, accent, warning, quiet), box, alignment, padding;
  // the boxes from the fonts. A day column is nine lines: five days and the blank lines
  // between them (main.c joins its days with "\n\n")
  var toneStr = function (t) { return t.reduce(function (s, fn, k) { return s + (fn << (4 * k)); }, 0); };
  function labels(F) {
    var colH = (2 * DAYS - 1) * F[0].h;
    return [
      { slot: SLOT.title, tones: [1, 0, NONE, NONE], w: 300, h: Math.max(F[1].h, F[0].h) + 2, align: 1, pad: 1 },
      { slot: SLOT.big, tones: [2, NONE, NONE, NONE], w: 200, h: F[2].h + 2, align: 1, pad: 1 },
      { slot: SLOT.line, tones: [0, NONE, NONE, 1], w: 340, h: F[0].h + 2, align: 1, pad: 1 },
      { slot: SLOT.detail, tones: [1, 0, NONE, NONE], w: 340, h: 3 * F[1].h + 2, align: 1, pad: 1 },
      { slot: SLOT.colA, tones: [0, NONE, NONE, NONE], w: 64, h: colH, align: 0, pad: 0, top: true },
      { slot: SLOT.colB, tones: [0, NONE, NONE, NONE], w: 42, h: colH, align: 2, pad: 0, top: true },
      { slot: SLOT.colC, tones: [0, NONE, NONE, NONE], w: 42, h: colH, align: 0, pad: 0, top: true },
      { slot: SLOT.colD, tones: [0, NONE, NONE, NONE], w: 46, h: colH, align: 2, pad: 0, top: true },
    ];
  }

  /* ---- the pictures: the preview, the start sheet's tiles, the hub's list ---- */
  async function picture(d, size, parts) {
    parts = parts || {};
    var c = canvas(G, G), x = c.getContext('2d');
    x.drawImage(parts.plate || await plateCanvas(d), 0, 0);
    var ic = await rast(iconSvg(d, 'partly', ICON_PX), ICON_PX, ICON_PX);
    x.drawImage(ic, CX - ICON_PX / 2, 150 - ICON_PX / 2);
    var W = d.words;
    await loadFace(W.num, 44); await loadFace(W.label, 14); await loadFace(W.text, 16);
    x.textAlign = 'center'; x.textBaseline = 'middle';
    x.fillStyle = W.quiet; x.font = '14px "' + W.label + '"'; x.fillText(W.caps ? 'HOME' : 'Home', CX, 58);
    x.fillStyle = W.ink; x.font = '44px "' + W.num + '"'; x.fillText('17°', CX + 6, 236);
    x.font = '16px "' + W.text + '"'; x.fillText('Partly cloudy', CX, 292);
    var o = canvas(size, size);
    o.getContext('2d').drawImage(c, 0, 0, size, size);
    return o;
  }
  function thumb(d, size) { return picture(d, size); }

  /* ---- the bake ---- */
  // the design baked to .orbtheme bytes: {bytes, meter, est, rows}. `design` is the design
  // document's bytes to carry (optional).
  async function bake(d, o) {
    o = o || {};
    var GB = root.GorbBake, St = GB.stage3({ dither: true }), AUX = GB.classId('aux'), TEXT = GB.classId('text');
    var plateC = await plateCanvas(d);
    St.row({ n: St.id(), class: GB.classId('plate'), slot: 0, x: 0, y: 0, w: G, h: G, order: 0, alpha: 255, n_frames: 1 }, plane(plateC), false, true);
    var centred = function (w, h, slot, order) { return { slot: slot, x: CX - (w >> 1), y: CX - (h >> 1), order: order }; };
    var big = await iconFrames(d, ICON_PX);
    St.rowPick(Object.assign({ n: St.id(), class: AUX, w: ICON_PX, h: ICON_PX, pivot_x: ICON_PX >> 1, pivot_y: ICON_PX >> 1, alpha: 255 },
      centred(ICON_PX, ICON_PX, SLOT.icon, 2)), big.map(plane), true, false);
    var small = await iconFrames(d, SICON_PX), rows = [];
    for (var i = 0; i < N_SICON; i++) rows.push(centred(SICON_PX, SICON_PX, SLOT.sicon + i, 2));
    St.pool({ class: AUX, w: SICON_PX, h: SICON_PX, pivot_x: SICON_PX >> 1, pivot_y: SICON_PX >> 1, alpha: 255 }, small.map(plane), rows, true, false);
    rows = [];
    for (var b = 0; b < N_BAR; b++) rows.push(centred(BAR_W, BAR_H, SLOT.bar + b, 1));
    St.pool({ class: AUX, w: BAR_W, h: BAR_H, pivot_x: BAR_W >> 1, pivot_y: BAR_H >> 1, alpha: 255 }, barFrames(d).map(plane), rows, true, false);
    rows = [];
    for (var k = 0; k < N_DAYB; k++) rows.push(centred(DAY_W, DAY_H, SLOT.day + k, 1));
    St.pool({ class: AUX, w: DAY_W, h: DAY_H, pivot_x: DAY_W >> 1, pivot_y: DAY_H >> 1, alpha: 255 }, dayFrames(d).map(plane), rows, true, false);
    St.rowPick(Object.assign({ n: St.id(), class: AUX, w: DOT_W, h: DOT_H, pivot_x: DOT_W >> 1, pivot_y: DOT_H >> 1, alpha: 255 },
      centred(DOT_W, DOT_H, SLOT.dots, 2)), dotFrames(d).map(plane), true, false);
    var F = await fonts(d);
    for (var fi = 0; fi < F.length; fi++) {
      var fo = F[fi];
      // the glyphs' coverage, coloured on the orb in the font's role colour
      St.font({ n: St.id(), class: TEXT, slot: fi, x: 0, y: 0, w: fo.w, h: fo.h, order: 0,
        pivot_x: Math.min(fo.space, fo.w), pivot_y: fo.track }, fo.glyphs.map(plane), d.words[FONTS[fi][2]]);
    }
    var Ls = labels(F);
    for (var li = 0; li < Ls.length; li++) {
      var L = Ls[li];
      St.row(Object.assign({ n: St.id(), class: AUX, kind: 2, src: GB.SOURCES.indexOf('text'), w: L.w, h: L.h,
        pivot_x: L.w >> 1, pivot_y: L.h >> 1, alpha: 255, n_frames: 1, str: toneStr(L.tones), radius: L.pad,
        digit: L.align, end: L.top ? 0 : 1 }, centred(L.w, L.h, L.slot, 4)), plane(canvas(L.w, L.h)), true, false);
    }
    St.preview({ plane: plane(await picture(d, 128, { plate: plateC })), x: 0, y: 0, order: 3 });
    var out = St.finish({ name: d.name || 'Weather', slug: 'weather', serial: o.serial || serialOf(d, o.design), abi: GB.GEOM_ABI6, design: o.design || null,
                          bakedS: o.bakedS != null ? o.bakedS : Math.floor(Date.now() / 1000) });
    return { bytes: out.bytes, meter: out.meter, est: out.est, rows: St.elements.length };
  }

  // the bake's serial is its content (flights-bake.js serialOf): FNV-1a over the design
  // document, never 0 or 1
  function serialOf(d, doc) {
    var bytes = doc && doc.length ? doc : new TextEncoder().encode(JSON.stringify(d)), h = 0x811c9dc5;
    for (var i = 0; i < bytes.length; i++) { h ^= bytes[i]; h = Math.imul(h, 0x01000193) >>> 0; }
    return h > 1 ? h : h + 2;
  }

  root.WeatherBake = {
    SCHEMA: SCHEMA, KINDS: KINDS, KIND_TITLES: KIND_TITLES, PACKS: PACKS, GROUNDS: GROUNDS, PRESETS: PRESETS, PRESETS_DEF: PRESETS_DEF,
    FACES: FACES, SLOT: SLOT, LIVE_ROWS: LIVE_ROWS, ICON_PX: ICON_PX, SICON_PX: SICON_PX, BAR_LEN: BAR_LEN, BAR_RAIN: BAR_RAIN, DAY_POS: DAY_POS,
    preset: preset, complete: complete, packStart: packStart, bake: bake, thumb: thumb, picture: picture, serialOf: serialOf,
    plateCanvas: plateCanvas, iconSvg: iconSvg, iconFrames: iconFrames, barFrames: barFrames, dayFrames: dayFrames, dotFrames: dotFrames,
    fonts: fonts, font: font, labels: labels, rast: rast, ramp: ramp, mix: mix, image: image,
  };
})(typeof window !== 'undefined' ? window : globalThis);
