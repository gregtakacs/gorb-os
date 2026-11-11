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
  var ICON_PX = 72, SICON_PX = 36, N_SICON = 5, BAR_H = 80, N_BAR = 12, BAR_LEN = 8, BAR_RAIN = 3;
  var DAY_W = 88, N_DAYB = 5, DAY_POS = 7, DOT_W = 64, DOT_H = 12, PITCH = 56, DAYS = 5;
  /* The weather's life (OS v1.2.14's 64 rows): slot 32 the clock, 33.. a pool of particle
   * sprites the app moves every frame (main.c PF_*), all naming one bank of PART_FRAMES. */
  var N_PART = 16, PART_W = 48, PART_H = 24;
  var PART_FRAMES = ['drop', 'drop_small', 'flake', 'flake_small', 'mist', 'wisp', 'star'];
  var SLOT = { icon: 0, sicon: 1, bar: 6, day: 18, dots: 23, title: 24, big: 25, line: 26, detail: 27, colA: 28, colB: 29, colC: 30, colD: 31,
    clock: 32, part: 33, halo: 49,
    // §2.10: Hourly's and the 5 days' own words, and Hourly's own icon, at fixed sizes
    // (a title and a line both views share, Hourly's temperature, the 5 days' details), baked
    // only for a font the author resized: at the defaults they draw in Now's own rows
    fTitle: 50, fBig: 51, fLine: 52, fDetail: 55, hIcon: 56,
    bolt: 57 };   // thunder's lightning (fx.h fx_bolt)
  var LIVE_ROWS = 1 + N_SICON + N_BAR + N_DAYB + 1 + 8 + 1 + N_PART + 1 + 4 + 1 + 1;
  var BOLT_W = 48, BOLT_H = 96;
  /* §2.8 sizes (owner, 2026-11-06): the large icon, the big temperature, and one size per
   * font for the words, never under 18 px, and never so large Now's lines run off the glass
   * (the details' longest line at 24 px). Now's and Places' only: Hourly and the 5 days
   * draw at the defaults, in fonts of their own (§2.10).
   * [least, most, default]. */
  var SIZES = { icon: [56, 104, 72], num: [36, 64, 44], text: [18, 26, 18], label: [18, 24, 18] };
  function sizeOf(d, k) { var r = SIZES[k], v = d && d.size ? +d.size[k] : NaN; return v >= r[0] && v <= r[1] ? Math.round(v) : r[2]; }
  /* §2.8 placement: Now's elements, each where its sizes' stack puts it, then moved by the
   * author's Angle (the direction, 0 up, clockwise) and Distance, as the clock editor
   * places a part. main.c's Now places (its NOW_*_Y) are where a theme from before §2.8
   * stands; the bake puts each row off the centre by its offset from there, and main.c
   * poses Now from the baked spot (every other view from its own). */
  var ELEMENTS = [['title', 'Place name'], ['dots', 'Place dots'], ['icon', 'Icon'], ['big', 'Temperature'], ['line', 'Condition'],
    ['detail', 'Details'], ['clock', 'Clock']];
  var APP_AT = { title: [CX, 58], dots: [CX, 88], icon: [CX, 150], big: [CX + 6, 236], line: [CX, 292], detail: [CX, 346], clock: [CX, 404] };
  var PLACE_SLOT = { title: 24, dots: 23, icon: 0, big: 25, line: 26, detail: 27, clock: 32 };
  function placeOf(d, k) {
    var q = d && d.place && d.place[k], a = q ? +q.a || 0 : 0, r = q ? Math.max(0, Math.min(220, +q.r || 0)) : 0;
    return { a: a, r: r, dx: Math.round(r * Math.sin(a * Math.PI / 180)), dy: Math.round(-r * Math.cos(a * Math.PI / 180)) };
  }
  // the elements' own places: main.c's, whatever the sizes (owner, 2026-11-06: an icon or
  // a font's size moves nothing else; the author places each)
  function stackNow() {
    var o = {};
    ELEMENTS.forEach(function (e) { o[e[0]] = APP_AT[e[0]][1]; });
    return o;
  }
  // each element's centre on Now, moved: {k: {x, y}}
  function layoutNow(d) {
    var st = stackNow(d), out = {};
    ELEMENTS.forEach(function (e) {
      var k = e[0], p = placeOf(d, k);
      out[k] = { x: APP_AT[k][0] + p.dx, y: st[k] + p.dy };
    });
    return out;
  }
  // the 5 days' columns, measured in the body font (its glyphs' advances and the label's
  // track), and the day bar narrowed to keep the list inside DAY_SPAN (main.c lays the
  // columns out from these widths)
  var DAY_SPAN = 312, WDAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
  function dayLayout(d) {
    var px = SIZES.text[2], m = canvas(4, 4).getContext('2d'), wd = function (strs) {
      m.font = px + 'px "' + d.words.text + '", sans-serif';
      return Math.ceil(Math.max.apply(null, strs.map(function (t) { return m.measureText(t).width + t.length; }))) + 4;
    };
    var c = [wd(WDAYS), wd(['-88°', '188°']), wd(['-88°', '188°']), wd(['100%'])];
    var fixed = c[0] + SICON_PX - 2 + c[1] - 8 + 4 + 2 + c[2] + c[3] - 2;
    return { cols: c, dw: Math.max(48, Math.min(DAY_W, DAY_SPAN - fixed)) };
  }
  // the fonts by slot: [face role, px, colour role, cell height]. A label's lines stand one
  // cell apart (orb_label.c), so the body and muted fonts, which the day columns draw in,
  // stand in a cell of half the row pitch: a column puts a blank line between its days
  var COL_CELL = PITCH / 2;
  // Three fonts, each a hundred glyphs whatever it is asked for (theme_schema FONT_CHARSET),
  // so each is weighed: the quiet one is the title's and the details' (in the label face,
  // capitals where the theme writes them), and the day columns dim by opacity, not a font.
  var FONTS = [['text', 16, 'ink', COL_CELL], ['label', 14, 'quiet'], ['num', 44, 'ink']];
  var NUM_KEEP = '0123456789-\u00b0';
  // a design's fonts (§2.10): Now's and Places' three at the author's sizes (§2.8), then
  // Hourly's and the 5 days' at the fixed defaults, so the sizes the author sets for the
  // main screen never move them. A fixed font the author's size already equals is not
  // baked again: the rows name the main one. {specs: [[role, px, colour, cell]], main:
  // {role: slot}, fixed: {role: slot}}
  var cellOf = function (px) { return Math.max(COL_CELL, Math.ceil(px * 1.5)); };
  function fontPlan(d) {
    var specs = [], main = {}, fixed = {}, ROLES = [['text', 'ink'], ['label', 'quiet'], ['num', 'ink']];
    ROLES.forEach(function (r) { var px = sizeOf(d, r[0]); main[r[0]] = specs.length; specs.push([r[0], px, r[1], r[0] === 'text' ? cellOf(px) : 0]); });
    ROLES.forEach(function (r) {
      var px = SIZES[r[0]][2];
      if (px === sizeOf(d, r[0])) { fixed[r[0]] = main[r[0]]; return; }
      fixed[r[0]] = specs.length; specs.push([r[0], px, r[1], r[0] === 'text' ? cellOf(px) : 0]);
    });
    return { specs: specs, main: main, fixed: fixed };
  }
  function fontSpecs(d) { return fontPlan(d).specs; }
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
    // looks-design §3a: the Silver look's own pack, cool single-tone engine-turned metal
    guilloche: { title: 'Guilloché', note: 'engine-turned silver; a bright specular streak, one amber bolt', roles: [['metal', 'Body'], ['hi', 'Highlight'], ['deep', 'Shade'], ['warn', 'Lightning']] },
    // looks-design §3b: the Sectional look's own pack, hollow aeronautical chart symbols
    charted: { title: 'Charted', note: 'aeronautical chart symbols; hollow navy linework with a magenta accent', roles: [['ink', 'Chart ink'], ['accent', 'Magenta'], ['paper', 'Paper knockout'], ['warn', 'Magenta']] },
  };
  var GROUNDS = { scope: 'Phosphor scope', brass: 'Brass instrument', blueprint: 'Blueprint', paper: 'Chart paper', celestial: 'Galaxy', station: 'Station dial',
    silver: 'Silvered metal', sectional: 'Sectional chart' };
  var MONO = 'IBM Plex Mono', PLEX = 'IBM Plex Sans', CORM = 'Cormorant Garamond', GROT = 'Space Grotesk', MICH = 'Michroma', JOST = 'Jost';
  var PRESETS_DEF = {
    scope: { name: 'Scope', match: 'Flights · Scope',
      plate: { style: 'scope', c0: '#12361F', c1: '#0E2A1A', c2: '#08170E', detail: '#2A6A44', tick: '#3DBB6E' },
      words: { num: MONO, text: PLEX, label: MONO, caps: true, track: 2, ink: '#E6F0EA', quiet: '#8A9A90' },
      temps: { ramp: ['#2E9C88', '#3DFF8E', '#FFB23C'], rain: '#59D8C8', track: '#1C4A2E', cap: 'round' },
      icons: { pack: 'phosphor', c: { stroke: '#3DFF8E', warn: '#FFB23C' } , shade: '3d', shadow: 'strong' } },
    brass: { name: 'Brass', match: 'Flights · Brass, Radar · Brass, Clock · Aviator',
      plate: { style: 'brass', c0: '#2E332E', c1: '#262A26', c2: '#1A1D1A', detail: '#C9A24E', tick: '#E9DDBF' },
      words: { num: CORM, text: CORM, label: CORM, caps: true, track: 3, ink: '#F2E6C8', quiet: '#A8946A' },
      temps: { ramp: ['#7FA3B0', '#C9A24E', '#FF9A3C'], rain: '#8FB7C9', track: '#3A3A30', cap: 'round' },
      icons: { pack: 'engraved', c: { metal: '#C9A24E', hi: '#F8E7A8', deep: '#6E5320', warn: '#FF9A3C' } , shade: '3d', shadow: 'strong' } },
    blueprint: { name: 'Blueprint', match: 'Flights · Blueprint',
      plate: { style: 'blueprint', c0: '#255C9E', c1: '#1F5290', c2: '#1A4780', detail: '#DCE9F8', tick: '#DCE9F8' },
      words: { num: MONO, text: MONO, label: MONO, caps: true, track: 2, ink: '#FFFFFF', quiet: '#B8CCE6' },
      temps: { ramp: ['#9FE3FF', '#F4F8FF', '#FFD27A'], rain: '#9FE3FF', track: '#2C64A6', cap: 'butt' },
      icons: { pack: 'drafted', c: { line: '#F4F8FF', guide: '#DCE9F8', warn: '#FFD27A' } , shade: '3d', shadow: 'strong' } },
    paper: { name: 'Paper', match: 'Flights · Paper',
      plate: { style: 'paper', c0: '#F1EBDA', c1: '#EAE3CF', c2: '#DDD4BC', detail: '#8FB0C6', tick: '#2F5C8C' },
      words: { num: PLEX, text: PLEX, label: PLEX, caps: false, track: 1, ink: '#14202C', quiet: '#5B5240' },
      temps: { ramp: ['#2F5C8C', '#1E6B2E', '#B85C00'], rain: '#2F5C8C', track: '#D3C9AE', cap: 'round' },
      icons: { pack: 'folded', c: { ink: '#2F5C8C', inkD: '#1F4066', warm: '#D9822B', warmD: '#B5651A', paper: '#FBF8F0', shade: '#D6CCB2', shadow: '#BFB497', warn: '#B85C00' } , shade: '3d', shadow: 'strong' } },
    celestial: { name: 'Celestial', match: 'Flights · Celestial, Clock · Orbit',
      plate: { style: 'celestial', c0: '#251A55', c1: '#1A1240', c2: '#0B0820', detail: '#B04CFF', tick: '#B9B2FF' },
      words: { num: GROT, text: GROT, label: MICH, caps: true, track: 2, ink: '#EDEBFF', quiet: '#9C95C9' },
      temps: { ramp: ['#8C7BFF', '#FFD27A', '#FF8A5C'], rain: '#9FD0FF', track: '#2B2260', cap: 'round' },
      icons: { pack: 'nebula', c: { gold: '#FFD27A', core: '#FFF3C8', orange: '#FF8A5C', violet: '#8C7BFF', lilac: '#C9BEFF', moon: '#D9D4FF', crater: '#ABA3EE', rain: '#9FD0FF' } , shade: '3d', shadow: 'strong' } },
    station: { name: 'Station', match: 'Clock · Station',
      plate: { style: 'station', c0: '#FFFFFF', c1: '#F4F4F2', c2: '#E6E6E3', detail: '#111111', tick: '#111111' },
      words: { num: JOST, text: JOST, label: JOST, caps: true, track: 2, ink: '#111111', quiet: '#555555' },
      temps: { ramp: ['#111111', '#8A8A8A', '#E3000F'], rain: '#111111', track: '#DADAD6', cap: 'butt' },
      icons: { pack: 'pictogram', c: { ink: '#111111', red: '#E3000F' } , shade: '3d', shadow: 'strong' } },
    // flights-map-plan §6 (look parity): Silver, polished metal and graphite, as the tracker's
    // silver and the radar's Silver wear it; looks-design §3a: its own Guilloché pack
    silver: { name: 'Silver', match: 'Flights · Silver, Radar · Silver',
      // graphite under a silver bezel, as the radar's and the tracker's Silver
      plate: { style: 'silver', c0: '#3A3F46', c1: '#2C3138', c2: '#1C1F24', detail: '#C8CED6', tick: '#E4E8EC' },
      words: { num: 'Jost', text: 'Jost', label: 'Jost', caps: true, track: 2, ink: '#F2F4F7', quiet: '#AAB2BD' },
      temps: { ramp: ['#7FC8FF', '#C8CED6', '#FF9A5C'], rain: '#7FC8FF', track: '#3A3F46', cap: 'round' },
      icons: { pack: 'guilloche', c: { metal: '#C8CED6', hi: '#FFFFFF', deep: '#5A626C', warn: '#FF9A5C' } , shade: '3d', shadow: 'strong' } },
    // flights-map-plan §4: a flight chart's look, made up: chart paper, a blue rose, a magenta
    // band inside the rim; looks-design §3b: its own Charted pack, hollow chart symbols
    sectional: { name: 'Sectional', match: 'Flights · Sectional, Radar · Sectional, Clock · Sectional',
      plate: { style: 'sectional', c0: '#F3F5EC', c1: '#EEF1E4', c2: '#E1E7D0', detail: '#2A5DB0', tick: '#2A5DB0' },
      words: { num: 'Barlow Condensed', text: 'Barlow Condensed', label: 'Barlow Condensed', caps: true, track: 1, ink: '#1F2A44', quiet: '#3E5A80' },
      temps: { ramp: ['#2A5DB0', '#2E7D32', '#C2187A'], rain: '#2A5DB0', track: '#D7DDC6', cap: 'round' },
      icons: { pack: 'charted', c: { ink: '#1F2A44', accent: '#C2187A', paper: '#F3F5EC', warn: '#C2187A' } , shade: '3d', shadow: 'strong' } },
  };
  // the starts in every app's order (owner, 2026-11-08: one name and one place per look)
  var PRESETS = ['scope', 'paper', 'brass', 'blueprint', 'station', 'celestial', 'silver', 'sectional'];
  var clone = function (o) { return JSON.parse(JSON.stringify(o)); };
  function preset(k) {
    if (k === 'blank') {
      var b = clone(PRESETS_DEF.scope);
      b.name = 'Blank'; b.base = 'blank'; b.match = '';
      b.plate = { style: 'plain', c0: '#141414', c1: '#101010', c2: '#0A0A0A', detail: '#3A3A3A', tick: '#8A8A8A' };
      b.words = { num: PLEX, text: PLEX, label: PLEX, caps: false, track: 1, ink: '#E6E6E6', quiet: '#8F8F8F' };
      b.temps = { ramp: ['#5B8DB8', '#D8D8D8', '#E0A040'], rain: '#7FB0D8', track: '#2E2E2E', cap: 'round' };
      b.icons = { pack: 'pictogram', c: { ink: '#D8D8D8', red: '#E0A040' }, shade: '3d', shadow: 'strong' };
      b.map = root.WeatherMap.preset('blank');
      return b;
    }
    var d = clone(PRESETS_DEF[k] || PRESETS_DEF.scope);
    d.base = PRESETS_DEF[k] ? k : 'scope';
    d.map = root.WeatherMap.preset(d.base);     // flights-map-plan §5: a hint of the place behind the reading
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
    // a design from before the map keeps its ground as it was: the map starts off
    var y = clone(x);
    if (!y.map) y.map = Object.assign(clone(p.map), { on: false });
    var d = fill(y, p);
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
    } else if (name === 'guilloche') {
      // looks-design §3a: engine-turned silver, cool and single-tone; a specular streak, and the
      // bolt the pack's only colour. Twelve slim engraved rays and turned rings, not Engraved's
      // sixteen-ray sunburst, and no rim anywhere: the metal reads by its own light and shade.
      var M4 = c.metal, H4 = c.hi, D4 = c.deep, W4 = c.warn;
      var arc4 = function (x, y, r, a0, a1) { var p = pt(r, a0, x, y), q = pt(r, a1, x, y); return 'M' + f(p[0]) + ',' + f(p[1]) + 'A' + f(r) + ',' + f(r) + ' 0 0,1 ' + f(q[0]) + ',' + f(q[1]); };
      var turned = function (x, y, R, u) { var o = []; for (var i = 1; i <= 3; i++) o.push(S(circ(x, y, (0.24 + i * 0.22) * R), { stroke: D4, sw: 0.6 * u, op: 0.5 })); return o; };
      pk = {
        sun: function (x, y, R, u) {
          var d = ''; for (var i = 0; i < 12; i++) d += rad(1.02 * R, 1.62 * R, i * 30, x, y);
          return [S(d, { stroke: M4, sw: 3.4 * u, cap: 'butt' }), S(d, { stroke: D4, sw: 0.7 * u, op: 0.7, cap: 'butt' }),
            S(circ(x, y, R), { fill: M4, stroke: D4, sw: 1 * u })].concat(turned(x, y, R, u),
            [S(arc4(x, y, 0.7 * R, -74, -14), { stroke: H4, sw: 2.6 * u, op: 0.9 }),
             S(arc4(x, y, 0.42 * R, -68, -34), { stroke: H4, sw: 1.2 * u, op: 0.6 })]);
        },
        moon: function (x, y, R, u) { return [S(circ(x, y, 1.3 * R), { stroke: D4, sw: 0.6 * u, op: 0.45 }), S(crescent(x, y, R), { fill: M4, stroke: D4, sw: 0.9 * u }), S(arc4(x, y, 0.66 * R, -70, -20), { stroke: H4, sw: 1.5 * u, op: 0.7 })]; },
        cloud: function (cl, u) { var b = cl.m(49, 50); return [S(cl.d, { fill: M4, stroke: D4, sw: 1 * u }), S(cl.sub(0.8), { stroke: D4, sw: 0.6 * u, op: 0.55 }), S(arc4(b[0], b[1], 16 * cl.k, -68, 44), { stroke: H4, sw: 1.7 * u, op: 0.85 })]; },
        rain: function (x, y, u) { return [S(tx(DROP, x, y, u), { fill: M4, stroke: D4, sw: 0.8 * u }), S(ln(x - 1.1 * u, y - 1.2 * u, x - 1.1 * u, y + 1.8 * u), { stroke: H4, sw: 0.8 * u, op: 0.85 })]; },
        drizzle: function (x, y, u) { return [S(circ(x, y, 2.8 * u), { fill: M4, stroke: D4, sw: 0.7 * u }), S(circ(x - 0.9 * u, y - 0.9 * u, 0.85 * u), { fill: H4, op: 0.9 })]; },
        snow: function (x, y, u) { var d = ''; for (var i = 0; i < 3; i++) { var p = pt(6.4 * u, i * 60, x, y), q = pt(6.4 * u, i * 60 + 180, x, y); d += ln(p[0], p[1], q[0], q[1]); } return [S(d, { stroke: M4, sw: 3 * u, cap: 'butt' }), S(d, { stroke: D4, sw: 0.7 * u, op: 0.7, cap: 'butt' }), S(circ(x, y, 2.2 * u), { stroke: D4, sw: 0.7 * u, op: 0.85 }), S(ln(x, y - 6.4 * u, x, y - 3.6 * u), { stroke: H4, sw: 1.1 * u, op: 0.9 })]; },
        bolt: function (ox, oy, u) { return [S(tx(BOLT, ox, oy, u), { fill: W4 })]; },
        fog: function (ls, u) { var o = []; ls.forEach(function (l, i) { o.push(S(ln(l[0], l[1], l[2], l[1]), { stroke: M4, sw: 5.6 * u, op: 1 - i * 0.12 })); o.push(S(ln(l[0] + 2 * u, l[1] + 1.7 * u, l[2] - 2 * u, l[1] + 1.7 * u), { stroke: D4, sw: 1 * u, op: 0.55 })); if (!i) o.push(S(ln(l[0] + 3 * u, l[1] - 1.8 * u, l[2] - 3 * u, l[1] - 1.8 * u), { stroke: H4, sw: 1.4 * u, op: 0.9 })); }); return o; },
      };
    } else if (name === 'charted') {
      // looks-design §3b: aeronautical chart symbols, hollow navy linework over a cream paper
      // knockout, with the magenta accent. Outlined glyphs, not Pictogram's solid blocks.
      var K5 = c.ink, A5 = c.accent, P5 = c.paper, W5 = c.warn;
      // Small-day legibility (owner, 2026-11-11): the chart hairlines vanish at the 36 px day
      // size, so navy strokes scale by size with a 2 px floor rather than with the icon. The
      // magenta accents keep their own weights (unchanged), fog is three solid navy 2 px lines
      // at both sizes, and drizzle and snow are filled navy marks, not hollow.
      var SW = function (k, u) { return Math.max(k * u, 2); };
      pk = {
        sun: function (x, y, R, u) { var t = ''; for (var i = 0; i < 8; i++) t += rad(1.06 * R, i % 2 ? 1.24 * R : 1.5 * R, i * 45, x, y); return [S(circ(x, y, 1.06 * R), { fill: P5 }), S(circ(x, y, R), { stroke: K5, sw: SW(1.6, u) }), S(t, { stroke: K5, sw: SW(1.2, u), cap: 'butt' }), S(poly([[x, y - 1.42 * R], [x + 0.3 * R, y], [x, y + 1.42 * R], [x - 0.3 * R, y]]), { stroke: A5, sw: 1.2 * u }), S(circ(x, y, 0.16 * R), { fill: K5 })]; },
        moon: function (x, y, R, u) { return [S(crescent(x, y, R), { fill: P5, stroke: K5, sw: SW(1.6, u) }), S(crescent(x, y, 0.7 * R), { stroke: K5, sw: SW(0.6, u), op: 0.5 }), S(circ(x + 1.18 * R, y - 0.86 * R, 2.2 * u), { fill: A5 })]; },
        cloud: function (cl, u) { var a = cl.m(28, 74), b = cl.m(72, 74); return [S(cl.d, { fill: P5, stroke: K5, sw: SW(1.7, u) }), S(cl.sub(0.72), { stroke: K5, sw: SW(0.6, u), op: 0.45 }), S(ln(a[0] + 4 * u, a[1] - 2.8 * u, b[0] - 4 * u, b[1] - 2.8 * u), { stroke: A5, sw: 1.4 * u })]; },
        rain: function (x, y, u) { return [S(ln(x + 3.4 * u, y - 7 * u, x - 2.4 * u, y + 6 * u), { stroke: K5, sw: SW(1.5, u), cap: 'butt' }), S(ln(x + 7.6 * u, y - 4 * u, x + 3 * u, y + 4.4 * u), { stroke: K5, sw: SW(1.1, u), op: 0.85, cap: 'butt' })]; },
        drizzle: function (x, y, u) { return [S(circ(x, y, Math.max(2.4 * u, 1.8)), { fill: K5 })]; },
        snow: function (x, y, u) { return [S(tx(STAR4, x, y, Math.max(5.4 * u, 2) / 8), { fill: K5 }), S(circ(x, y, 1.7 * u), { fill: A5 })]; },
        bolt: function (ox, oy, u) { return [S(tx(BOLT, ox, oy, u), { fill: P5, stroke: W5, sw: 1.7 * u })]; },
        fog: function (ls, u) { return ls.slice(0, 3).map(function (l) { return S(ln(l[0], l[1], l[2], l[1]), { stroke: K5, sw: 2, cap: 'butt' }); }); },
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
  /* The icon's finish (§2.7), over every pack: 3D shading lays a light from the top left over
   * the icon's own ink (lit, then shaded toward the bottom right), and a drop shadow sets it
   * on a soft shadow below and right. Both scale with the icon, so the small ones match. */
  var SHADOWS = { off: null, soft: { a: 0.35, blur: 3, off: 2 }, strong: { a: 0.6, blur: 4, off: 3 } };
  function finish(d, c, px) {
    var I = d.icons || {}, k = px / 72;
    if (I.shade === '3d') {
      var x = c.getContext('2d'), g = x.createLinearGradient(0, 0, px, px);
      g.addColorStop(0, 'rgba(255,255,255,0.45)'); g.addColorStop(0.45, 'rgba(255,255,255,0)');
      g.addColorStop(0.55, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,0.4)');
      x.save(); x.globalCompositeOperation = 'source-atop'; x.fillStyle = g; x.fillRect(0, 0, px, px);
      var r = x.createRadialGradient(px * 0.32, px * 0.28, 0, px * 0.32, px * 0.28, px * 0.35);
      r.addColorStop(0, 'rgba(255,255,255,0.3)'); r.addColorStop(1, 'rgba(255,255,255,0)');
      x.fillStyle = r; x.fillRect(0, 0, px, px); x.restore();
    }
    var sh = SHADOWS[I.shadow];
    if (!sh) return c;
    var o = canvas(px, px), y = o.getContext('2d');
    y.shadowColor = 'rgba(0,0,0,' + sh.a + ')'; y.shadowBlur = sh.blur * k * 2;
    y.shadowOffsetX = sh.off * k; y.shadowOffsetY = sh.off * k;
    y.drawImage(c, 0, 0);
    return o;
  }
  async function iconFrames(d, px) {
    var out = [];
    for (var i = 0; i < KINDS.length; i++) out.push(finish(d, await rast(iconSvg(d, KINDS[i], px), px, px), px));
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
    } else if (st === 'silver') {
      // a turned bezel: two fine rings, minute ticks and heavier fives
      o.push(S(circ(CX, CX, 226), { stroke: det, sw: 3, op: 0.7 }), S(circ(CX, CX, 219), { stroke: det, sw: 1, op: 0.6 }));
      o.push(S(tickPath(206, 215, 6, function (a) { return a % 30; }), { stroke: tk, sw: 1, op: 0.6 }), S(tickPath(198, 215, 30), { stroke: tk, sw: 2.4 }));
    } else if (st === 'sectional') {
      // a graticule with a chart's minute ticks, a dashed magenta isogonic line, the magenta
      // band inside the rim, a blue rose
      var mt = '';
      for (var gx = 58; gx < G; gx += 58) for (var m = 0; m < G; m += 11.6) { var tl = Math.round(m / 11.6) % 5 ? 3 : 6; mt += ln(gx - tl, m, gx, m) + ln(m, gx, m, gx + tl); }
      o.push(S(gridPath(58), { stroke: det, sw: 0.7, op: 0.3 }), S(mt, { stroke: det, sw: 0.7, op: 0.3, cap: 'butt' }));
      o.push(S(ln(70, 430, 400, 30), { stroke: '#C2187A', sw: 1.4, op: 0.45, dash: '10 6' }));
      for (var bi = 0; bi < 6; bi++) o.push(S(circ(CX, CX, 230 - bi * 5), { stroke: '#C2187A', sw: 5, op: 0.2 - bi * 0.032 }));
      o.push(S(circ(CX, CX, 196), { stroke: tk, sw: 1.6, op: 0.85 }));
      o.push(S(tickPath(196, 204, 5, function (a) { return a % 30; }), { stroke: tk, sw: 1, op: 0.8 }), S(tickPath(196, 210, 30), { stroke: tk, sw: 1.6 }));
      o.push(S(poly([[CX, 34], [CX - 6, 22], [CX + 6, 22]]), { fill: '#C2187A' }));
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
  /* §2.9 the ground, bottom to top: the face (the look's gradient, a dial finish from the
   * clock's in ground.js, or the author's picture), the look's own markings (rings, ticks,
   * bezel; may be turned off), then wear. One picture under every screen: the plate disc
   * the orb already draws, so none of it costs a frame. */
  var WEAR = { New: [0, 0, 0], Light: [0.3, 0.15, 0.4], Vintage: [1, 1, 1], Relic: [1.5, 1.5, 1.5] };
  function groundOf(p) {
    var GD = root.WeatherGround, lm = GD ? GD.lum(p.c1) : 0.5, n = parseInt(p.c1.slice(1), 16);
    var sat = Math.max(n >> 16, (n >> 8) & 255, n & 255) - Math.min(n >> 16, (n >> 8) & 255, n & 255);
    return { finish: p.finish || 'plain', a: p.c0, b: p.c2, tint: sat < 18 ? null : p.c1, dark: lm < 0.5, tex: p.tex == null ? 1 : +p.tex,
      weaveAng: +p.weaveAng || 0, weaveSc: p.weaveSc == null ? 1 : +p.weaveSc, seed: 7 };
  }
  async function plateCanvas(d) {
    var p = d.plate, GD = root.WeatherGround, c = canvas(G, G), x = c.getContext('2d'), gd = groundOf(p);
    var img = null;
    if (p.img) { try { img = await image(p.img); } catch (e) { img = null; } }
    if (img) {
      var k = Math.max(G / img.width, G / img.height) * (p.imgScale == null ? 1 : +p.imgScale), w = img.width * k, h = img.height * k;
      x.fillStyle = '#000'; x.fillRect(0, 0, G, G);
      x.drawImage(img, CX - w / 2 + (+p.imgX || 0), CX - h / 2 + (+p.imgY || 0), w, h);
      if (+p.imgDim > 0) { x.fillStyle = 'rgba(0,0,0,' + Math.min(0.85, +p.imgDim) + ')'; x.fillRect(0, 0, G, G); }
    } else if (gd.finish === 'plain' || !GD) {
      var defs = '<radialGradient id="g" cx="50%" cy="50%" r="50%"><stop offset="0" stop-color="' + p.c0 + '"/><stop offset="0.6" stop-color="' + p.c1 + '"/><stop offset="1" stop-color="' + p.c2 + '"/></radialGradient>';
      x.drawImage(await rast(svgDoc('0 0 ' + G + ' ' + G, '<rect width="' + G + '" height="' + G + '" fill="url(#g)"/>', defs), G, G), 0, 0);
    } else GD.paint(x, gd);
    if (p.marks !== false) x.drawImage(await rast(pathsSvg(chromePaths(p), G, '', ''), G, G), 0, 0);
    if (GD) GD.wear(x, { dark: img ? true : gd.dark, seed: 7 }, { age: +p.age || 0, stain: +p.stain || 0, dust: +p.dust || 0 });
    return c;
  }

  /* ---- the moving rows' frames ---- */
  // wx_bar_frame: length level by temperature, the rain dot by chance; the sprite points up,
  // its centre at HR_BAR_R (157) from the glass centre, so y = 197 - r
  // the bars' width (2..14 px) and fill: one colour by the hour's temperature, or a gradient
  // along the bar from the cold colour at its inner end to that colour at its tip
  function barWidth(T) { var w = +T.width; return w >= 2 && w <= 14 ? w : 6; }
  // a frame's breadth: the bar and 2 px each side, never under the 10 px the default takes,
  // so a theme pays only for the width it chose
  function barBox(d) { return Math.max(10, barWidth(d.temps) + 4); }
  function barFrames(d) {
    var T = d.temps, out = [], bw = barWidth(T), BAR_W = barBox(d);
    for (var L = 0; L < BAR_LEN; L++) for (var rn = 0; rn < BAR_RAIN; rn++) {
      var c = canvas(BAR_W, BAR_H), x = c.getContext('2d'), r1 = 132, r2 = 148 + L * 6.4, top = ramp(T.ramp, L / (BAR_LEN - 1));
      if (T.fill === 'gradient') {
        var g = x.createLinearGradient(0, 197 - r1, 0, 197 - r2);
        g.addColorStop(0, ramp(T.ramp, 0)); g.addColorStop(1, top);
        x.strokeStyle = g;
      } else x.strokeStyle = top;
      // a round end is drawn inside the bar's length, so a wider bar is no longer
      var ins = T.cap === 'butt' ? 0 : bw / 2;
      x.lineWidth = bw; x.lineCap = T.cap === 'butt' ? 'butt' : 'round';
      x.beginPath(); x.moveTo(BAR_W / 2, 197 - r1 - ins); x.lineTo(BAR_W / 2, 197 - Math.max(r1 + ins + 0.5, r2 - ins)); x.stroke();
      if (rn) { x.fillStyle = T.rain; x.beginPath(); x.arc(BAR_W / 2, 197 - 121, 1.4 + rn * 1.2, 0, 2 * Math.PI); x.fill(); }
      out.push(c);
    }
    return out;
  }
  /* Thunder's bolt (fx.h fx_bolt): a forked zigzag in the pack's lightning colour, with a
   * soft glow, BOLT_W x BOLT_H. The app strikes it over the icon every few seconds. */
  function boltFrame(d) {
    var I = d.icons.c, col = I.warn || I.orange || I.red || d.words.ink, c = canvas(BOLT_W, BOLT_H), x = c.getContext('2d');
    var main = [[26, 3], [17, 34], [27, 36], [14, 66], [24, 68], [12, 93]], fork = [[27, 36], [37, 52], [33, 54], [41, 70]];
    var line = function (pts, w) { x.beginPath(); pts.forEach(function (p, i) { if (i) x.lineTo(p[0], p[1]); else x.moveTo(p[0], p[1]); }); x.lineWidth = w; x.stroke(); };
    x.lineCap = 'round'; x.lineJoin = 'round';
    x.shadowColor = col; x.shadowBlur = 8; x.strokeStyle = col;
    line(main, 3.2); line(fork, 1.8);
    x.shadowBlur = 0; x.strokeStyle = 'rgba(255,255,255,0.85)';
    line(main, 1.2);
    return c;
  }
  /* The particles, one frame each in PART_FRAMES order, PART_W x PART_H with the ink
   * centred: rain in the rain colour, snow and stars in the ink, mist and wisps soft. */
  function partFrames(d) {
    var T = d.temps, W = d.words, out = [], cx = PART_W / 2, cy = PART_H / 2;
    var soft = function (x, rx, ry, col, a) {
      var g = x.createRadialGradient(cx, cy, 0, cx, cy, rx);
      g.addColorStop(0, col); g.addColorStop(1, 'rgba(0,0,0,0)');
      x.save(); x.globalAlpha = a; x.translate(cx, cy); x.scale(1, ry / rx); x.translate(-cx, -cy);
      x.fillStyle = g; x.beginPath(); x.arc(cx, cy, rx, 0, 2 * Math.PI); x.fill(); x.restore();
    };
    PART_FRAMES.forEach(function (k) {
      var c = canvas(PART_W, PART_H), x = c.getContext('2d');
      if (k === 'drop' || k === 'drop_small') {
        var len = k === 'drop' ? 16 : 9, g = x.createLinearGradient(0, cy - len / 2, 0, cy + len / 2);
        g.addColorStop(0, 'rgba(0,0,0,0)'); g.addColorStop(1, T.rain);
        x.strokeStyle = g; x.lineWidth = k === 'drop' ? 2 : 1.5; x.lineCap = 'round';
        x.beginPath(); x.moveTo(cx, cy - len / 2); x.lineTo(cx, cy + len / 2); x.stroke();
      } else if (k === 'flake') {
        x.strokeStyle = W.ink; x.lineWidth = 1.3; x.lineCap = 'round';
        for (var a = 0; a < 3; a++) {
          var t = a * Math.PI / 3; x.beginPath();
          x.moveTo(cx - 5 * Math.cos(t), cy - 5 * Math.sin(t)); x.lineTo(cx + 5 * Math.cos(t), cy + 5 * Math.sin(t)); x.stroke();
        }
      } else if (k === 'flake_small') {
        x.fillStyle = W.ink; x.beginPath(); x.arc(cx, cy, 2, 0, 2 * Math.PI); x.fill();
      } else if (k === 'mist') soft(x, 23, 10, W.quiet, 0.55);
      else if (k === 'wisp') soft(x, 22, 5, W.ink, 0.35);
      else if (k === 'star') {
        x.fillStyle = W.ink; x.beginPath();
        x.moveTo(cx, cy - 4); x.lineTo(cx + 1, cy - 1); x.lineTo(cx + 4, cy); x.lineTo(cx + 1, cy + 1);
        x.lineTo(cx, cy + 4); x.lineTo(cx - 1, cy + 1); x.lineTo(cx - 4, cy); x.lineTo(cx - 1, cy - 1); x.closePath(); x.fill();
      }
      out.push(c);
    });
    return out;
  }
  // wx_day_frame: i <= j on the week's scale, triangular, i-major
  function dayFrames(d, dw) {
    dw = dw || DAY_W;
    var T = d.temps, out = [], DAY_H = barBox(d), X = function (k) { return 5 + k * ((dw - 10) / (DAY_POS - 1)); };
    for (var i = 0; i < DAY_POS; i++) for (var j = i; j < DAY_POS; j++) {
      var c = canvas(dw, DAY_H), x = c.getContext('2d'), cap = T.cap === 'butt' ? 'butt' : 'round';
      // the track and the span keep their lengths whatever the width: a round end is drawn
      // inside them, so the week's scale reads the same at 2 px and at 14
      var bw = barWidth(T), ins = cap === 'butt' ? 0 : bw / 2, seg = function (a, b) {
        x.beginPath(); x.moveTo(a + ins, DAY_H / 2); x.lineTo(Math.max(a + ins + 0.5, b - ins), DAY_H / 2); x.stroke();
      };
      x.lineWidth = bw; x.lineCap = cap; x.strokeStyle = T.track;
      seg(X(0) - 2, X(DAY_POS - 1) + 2);
      var g = x.createLinearGradient(X(i), 0, X(j) + 0.01, 0);
      g.addColorStop(0, ramp(T.ramp, i / (DAY_POS - 1))); g.addColorStop(1, ramp(T.ramp, j / (DAY_POS - 1)));
      x.strokeStyle = g;
      seg(X(i) - 2, X(j) + 2);
      out.push(c);
    }
    return out;
  }
  // the 5 days' cursor: a halo over the day's bar, in the ink, soft
  function haloFrame(d, dw) {
    // the ring stands HALO_GAP off the bar's ink all round, whatever the bar's width
    var HALO_GAP = 5, bw = barWidth(d.temps), len = (dw || DAY_W) - 10 + 4;
    var W = d.words, w = len + 2 * HALO_GAP + 6, h = bw + 2 * HALO_GAP + 6, c = canvas(w, h), x = c.getContext('2d'), r = (h - 6) / 2;
    var path = function () { x.beginPath(); x.moveTo(3 + r, 3); x.arcTo(w - 3, 3, w - 3, h - 3, r); x.arcTo(w - 3, h - 3, 3, h - 3, r); x.arcTo(3, h - 3, 3, 3, r); x.arcTo(3, 3, w - 3, 3, r); x.closePath(); };
    x.globalAlpha = 0.16; x.fillStyle = W.ink; path(); x.fill();
    x.globalAlpha = 0.7; x.shadowColor = W.ink; x.shadowBlur = 5; x.strokeStyle = W.ink; x.lineWidth = 1.5; path(); x.stroke();
    return c;
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
  async function font(face, px, colour, o) {
    o = o || {};
    var spec = px + 'px "' + face + '"', CS = fontSet();
    await loadFace(face, px, fontText(CS));
    var m = canvas(4, 4).getContext('2d');
    m.font = spec;
    var drawn = function (ch) {
      if (!o.caps) return ch;
      return typeof ch === 'string' ? ch.toUpperCase() : { acc: ch.acc.toUpperCase(), car: ch.car.toUpperCase() };
    };
    var top = 0, bottom = 0, w = 1, boxes = CS.map(function (ch) { return boxOf(m, drawn(ch)); });
    var sizer = o.digits ? function (ch) { return /[0-9\-°%: ]/.test(ch); } : function () { return true; };
    boxes.forEach(function (b, i) {
      if (typeof CS[i] !== 'string' || !sizer(CS[i])) return;
      if (b.r > b.l) { top = Math.max(top, b.t); bottom = Math.max(bottom, b.b); w = Math.max(w, b.r - b.l); }
    });
    if (!o.digits) { var fm = m.measureText('Hg'); top = Math.max(top, Math.ceil(fm.fontBoundingBoxAscent || 0)); bottom = Math.max(bottom, Math.ceil(fm.fontBoundingBoxDescent || 0)); }
    top += 1; bottom += 1;
    // a fixed cell: the ink centred in it, clipped if a face is taller than the cell
    if (o.cell) { var spare = o.cell - (top + bottom); top += Math.floor(spare / 2); bottom = o.cell - top; }
    var h = top + bottom, glyphs = boxes.map(function (b, i) {
      // OS v1.2.17: a font keeps only the glyphs `o.keep` names (all when it names none)
      if (o.keep && o.keep.indexOf(CS[i]) < 0) return null;
      var c = canvas(w, h), x = c.getContext('2d');
      x.font = spec; x.textBaseline = 'alphabetic'; x.fillStyle = colour;
      if (b.r > b.l) root.GorbBake.drawGlyph(x, drawn(CS[i]), -b.l, top);
      return c;
    });
    return { w: w, h: h, glyphs: glyphs, space: Math.max(1, Math.round(m.measureText(' ').width)), track: Math.max(0, o.track | 0) };
  }
  async function fonts(d) {
    var W = d.words, out = [];
    var FS = fontSpecs(d);
    for (var i = 0; i < FS.length; i++) {
      var fr = FS[i];
      // the temperature's fonts draw a temperature and nothing else: its digits, the minus
      // and the degree sign are all they keep (OS v1.2.17's sparse fonts)
      out.push(await font(faceOf(d, fr[0]), fr[1], W[fr[2]], { caps: fr[0] === 'label' && W.caps, digits: fr[0] === 'num', track: fr[0] === 'label' ? Math.max(0, Math.min(6, W.track | 0)) : 1, cell: fr[3] || 0,
        keep: fr[0] === 'num' ? NUM_KEEP : null }));
      out[i].colour = fr[2];
    }
    return out;
  }
  // the labels: slot, fonts by tone (normal, accent, warning, quiet), box, alignment, padding;
  // the boxes from the fonts. A day column is nine lines: five days and the blank lines
  // between them (main.c joins its days with "\n\n")
  var toneStr = function (t) { return t.reduce(function (s, fn, k) { return s + (fn << (4 * k)); }, 0); };
  function labels(F, cols, plan) {
    plan = plan || { main: { text: 0, label: 1, num: 2 }, fixed: { text: 0, label: 1, num: 2 } };
    var M = plan.main, X = plan.fixed, colH = (2 * DAYS - 1) * F[X.text].h;
    cols = cols || [64, 42, 42, 46];
    var title = function (slot, f) { return { slot: slot, tones: [f.label, f.text, NONE, NONE], w: 300, h: Math.max(F[f.label].h, F[f.text].h) + 2, align: 1, pad: 1 }; };
    var line = function (slot, f) { return { slot: slot, tones: [f.text, NONE, NONE, f.label], w: 340, h: F[f.text].h + 2, align: 1, pad: 1 }; };
    var detail = function (slot, f) { return { slot: slot, tones: [f.label, f.text, NONE, NONE], w: 360, h: 3 * F[f.label].h + 2, align: 1, pad: 1 }; };
    var out = [
      // Now's and Places', at the author's sizes
      title(SLOT.title, M),
      { slot: SLOT.big, tones: [M.num, NONE, NONE, NONE], w: 200, h: F[M.num].h + 2, align: 1, pad: 1 },
      line(SLOT.line, M), detail(SLOT.detail, M),
      { slot: SLOT.clock, tones: [M.text, NONE, NONE, NONE], w: 160, h: F[M.text].h + 2, align: 1, pad: 1 },
      // the 5 days' columns, always at the fixed size
      { slot: SLOT.colA, tones: [X.text, NONE, NONE, NONE], w: cols[0], h: colH, align: 0, pad: 0, top: true },
      { slot: SLOT.colB, tones: [X.text, NONE, NONE, NONE], w: cols[1], h: colH, align: 2, pad: 0, top: true },
      { slot: SLOT.colC, tones: [X.text, NONE, NONE, NONE], w: cols[2], h: colH, align: 0, pad: 0, top: true },
      { slot: SLOT.colD, tones: [X.text, NONE, NONE, NONE], w: cols[3], h: colH, align: 2, pad: 0, top: true },
    ];
    // Hourly's and the 5 days' own rows (§2.10), where a font they draw in was resized: a
    // label row's panel is its whole box, so one that would match Now's costs bytes for nothing
    var words = X.text !== M.text || X.label !== M.label;
    if (words) out.push(title(SLOT.fTitle, X), line(SLOT.fLine, X), Object.assign(detail(SLOT.fDetail, X), { w: 300 }));
    if (X.num !== M.num) out.push({ slot: SLOT.fBig, tones: [X.num, NONE, NONE, NONE], w: 200, h: F[X.num].h + 2, align: 1, pad: 1 });
    return out;
  }

  /* ---- the pictures: the preview, the start sheet's tiles, the hub's list ---- */
  async function picture(d, size, parts) {
    parts = parts || {};
    var c = canvas(G, G), x = c.getContext('2d');
    x.drawImage(parts.plate || await plateCanvas(d), 0, 0);
    // the map behind the reading (flights-map-plan §5): the editor's home map when it has it,
    // else maps@1's made-up place
    var MP = root.WeatherMap;
    if (parts.map) x.drawImage(parts.map, 0, 0);
    else if (d.map && d.map.on && MP && root.GorbMaps) { try { x.drawImage(MP.view(d.map, root.GorbMaps.sampleMap(), null), 0, 0); } catch (e) { /* none */ } }
    var IP = sizeOf(d, 'icon'), L = layoutNow(d), nu = sizeOf(d, 'num'), lb = sizeOf(d, 'label'), tx = sizeOf(d, 'text');
    var ic = finish(d, await rast(iconSvg(d, 'partly', IP), IP, IP), IP);
    x.drawImage(ic, L.icon.x - IP / 2, L.icon.y - IP / 2);
    var W = d.words;
    await loadFace(W.num, nu); await loadFace(W.label, lb); await loadFace(W.text, tx);
    x.textAlign = 'center'; x.textBaseline = 'middle';
    x.fillStyle = W.quiet; x.font = lb + 'px "' + W.label + '"'; x.fillText(W.caps ? 'HOME' : 'Home', L.title.x, L.title.y);
    x.fillStyle = W.ink; x.font = nu + 'px "' + W.num + '"'; x.fillText('17°', L.big.x, L.big.y);
    x.font = tx + 'px "' + W.text + '"'; x.fillText('Partly cloudy', L.line.x, L.line.y);
    return shrink(c, size);
  }
  /* The glass down to `size` by halves, each a 2:1 average, then the last step: one
   * drawImage from 466 straight to a thumbnail drops most of the rows a fine stroke sits on,
   * and Weather's words are hairlines (owner, 2026-11-07: its preview read as pixel mush). */
  function shrink(c, size) {
    var cur = c;
    while (cur.width / 2 >= size) {
      var h = canvas(Math.round(cur.width / 2), Math.round(cur.height / 2)), hx = h.getContext('2d');
      hx.imageSmoothingEnabled = true; hx.imageSmoothingQuality = 'high';
      hx.drawImage(cur, 0, 0, h.width, h.height);
      cur = h;
    }
    var o = canvas(size, size), ox = o.getContext('2d');
    ox.imageSmoothingEnabled = true; ox.imageSmoothingQuality = 'high';
    ox.drawImage(cur, 0, 0, size, size);
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
    // Now's elements off the centre by their offset from main.c's own places (§2.8)
    var lay = layoutNow(d), moved = {};
    ELEMENTS.forEach(function (e) { var k = e[0]; moved[PLACE_SLOT[k]] = [lay[k].x - APP_AT[k][0], lay[k].y - APP_AT[k][1]]; });
    var placed = function (w, h, slot, order) {
      var o = centred(w, h, slot, order), m = moved[slot];
      if (m) { o.x += m[0]; o.y += m[1]; }
      return o;
    };
    var FP = fontPlan(d), F = await fonts(d), DL = dayLayout(d);
    var IP = sizeOf(d, 'icon'), big = await iconFrames(d, IP);
    St.rowPick(Object.assign({ n: St.id(), class: AUX, w: IP, h: IP, pivot_x: IP >> 1, pivot_y: IP >> 1, alpha: 255 },
      placed(IP, IP, SLOT.icon, 2)), big.map(plane), true, false);
    // Hourly's own icon at the default size, where the author's differs (§2.10)
    if (IP !== ICON_PX) {
      St.rowPick(Object.assign({ n: St.id(), class: AUX, w: ICON_PX, h: ICON_PX, pivot_x: ICON_PX >> 1, pivot_y: ICON_PX >> 1, alpha: 255 },
        centred(ICON_PX, ICON_PX, SLOT.hIcon, 2)), (await iconFrames(d, ICON_PX)).map(plane), true, false);
    }
    var small = await iconFrames(d, SICON_PX), rows = [];
    for (var i = 0; i < N_SICON; i++) rows.push(centred(SICON_PX, SICON_PX, SLOT.sicon + i, 2));
    St.pool({ class: AUX, w: SICON_PX, h: SICON_PX, pivot_x: SICON_PX >> 1, pivot_y: SICON_PX >> 1, alpha: 255 }, small.map(plane), rows, true, false);
    rows = [];
    var BAR_W = barBox(d), DAY_H = barBox(d);
    for (var b = 0; b < N_BAR; b++) rows.push(centred(BAR_W, BAR_H, SLOT.bar + b, 1));
    St.pool({ class: AUX, w: BAR_W, h: BAR_H, pivot_x: BAR_W >> 1, pivot_y: BAR_H >> 1, alpha: 255 }, barFrames(d).map(plane), rows, true, false);
    // the 5 days' cursor under the day bars it rings (one depth, table order)
    var hc = haloFrame(d, DL.dw);
    St.row(Object.assign({ n: St.id(), class: AUX, w: hc.width, h: hc.height, pivot_x: hc.width >> 1, pivot_y: hc.height >> 1, alpha: 255, n_frames: 1 },
      centred(hc.width, hc.height, SLOT.halo, 1)), plane(hc), true, false);
    rows = [];
    for (var k = 0; k < N_DAYB; k++) rows.push(centred(DL.dw, DAY_H, SLOT.day + k, 1));
    St.pool({ class: AUX, w: DL.dw, h: DAY_H, pivot_x: DL.dw >> 1, pivot_y: DAY_H >> 1, alpha: 255 }, dayFrames(d, DL.dw).map(plane), rows, true, false);
    St.rowPick(Object.assign({ n: St.id(), class: AUX, w: DOT_W, h: DOT_H, pivot_x: DOT_W >> 1, pivot_y: DOT_H >> 1, alpha: 255 },
      placed(DOT_W, DOT_H, SLOT.dots, 2)), dotFrames(d).map(plane), true, false);
    for (var fi = 0; fi < F.length; fi++) {
      var fo = F[fi];
      // the glyphs' coverage, coloured on the orb in the font's role colour
      St.font({ n: St.id(), class: TEXT, slot: fi, x: 0, y: 0, w: fo.w, h: fo.h, order: 0,
        pivot_x: Math.min(fo.space, fo.w), pivot_y: fo.track }, fo.glyphs.map(function (g) { return g ? plane(g) : null; }), d.words[fo.colour]);
    }
    var Ls = labels(F, DL.cols, FP);
    for (var li = 0; li < Ls.length; li++) {
      var L = Ls[li];
      St.row(Object.assign({ n: St.id(), class: AUX, kind: 2, src: GB.SOURCES.indexOf('text'), w: L.w, h: L.h,
        pivot_x: L.w >> 1, pivot_y: L.h >> 1, alpha: 255, n_frames: 1, str: toneStr(L.tones), radius: L.pad,
        digit: L.align, end: L.top ? 0 : 1 }, placed(L.w, L.h, L.slot, 4)), plane(canvas(L.w, L.h)), true, false);
    }
    // the particles: one bank, a pool of rows the app moves (main.c), behind the icon and words
    if (!d.fx || d.fx.on !== false) {
      rows = [];
      for (var pk = 0; pk < N_PART; pk++) rows.push(centred(PART_W, PART_H, SLOT.part + pk, 1));
      St.pool({ class: AUX, w: PART_W, h: PART_H, pivot_x: PART_W >> 1, pivot_y: PART_H >> 1, alpha: 255 }, partFrames(d).map(plane), rows, true, false);
      St.row(Object.assign({ n: St.id(), class: AUX, w: BOLT_W, h: BOLT_H, pivot_x: BOLT_W >> 1, pivot_y: BOLT_H >> 1, alpha: 255, n_frames: 1 },
        centred(BOLT_W, BOLT_H, SLOT.bolt, 1)), plane(boltFrame(d)), true, false);
    }
    // §60: the whole glass, deflated by the baker and inflated by the orb (owner, 2026-11-07:
    // at 128 the carousel's disc stretched it ~1.8x and the words could not be read)
    if (d.map && d.map.on) St.values(root.WeatherMap.valuesBytes(d.map), root.WeatherMap.VALUES_VER);
    St.preview({ plane: plane(await picture(d, 466, { plate: plateC })), x: 0, y: 0, order: 3 });
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
    SIZES: SIZES, WEAR: WEAR, groundOf: groundOf, sizeOf: sizeOf, ELEMENTS: ELEMENTS, placeOf: placeOf, stackNow: stackNow, layoutNow: layoutNow, dayLayout: dayLayout,
    haloFrame: haloFrame, fontSpecs: fontSpecs, fontPlan: fontPlan, loadFace: loadFace,
    preset: preset, complete: complete, packStart: packStart, bake: bake, thumb: thumb, picture: picture, serialOf: serialOf,
    plateCanvas: plateCanvas, iconSvg: iconSvg, iconFrames: iconFrames, barFrames: barFrames, dayFrames: dayFrames, dotFrames: dotFrames,
    fonts: fonts, font: font, labels: labels, rast: rast, ramp: ramp, mix: mix, image: image,
    partFrames: partFrames, PART_FRAMES: PART_FRAMES, N_PART: N_PART, boltFrame: boltFrame,
  };
})(typeof window !== 'undefined' ? window : globalThis);
