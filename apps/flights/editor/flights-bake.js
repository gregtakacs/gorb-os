/* flights-bake.js — the tracker's theme, baked in the browser (flight-tracker-design §12.2, §12.5).
 *
 * A design (schema 2, below) drawn on canvases and assembled by the OS's stage bake
 * (GorbBake.stage3) into the rows apps/flights/main.c reads:
 *   - the plate: the ground, the cross, the bezel's ticks and figures, the range rings and the
 *     home dot, every one of them the design's numbers (§12.5: the scope is adjustable);
 *   - slot 0: the radar arm, a sweeping seconds row at SWEEP_FPS whose vmax is one turn's
 *     seconds (main.c reads it: the arm's speed);
 *   - slots 1..15: the plane pool, one bank of 49 frames (7 shapes x 7 bands, shape-major),
 *     from a family's drawings or the author's own picture;
 *   - slots 16, 17: the selection and emergency rings;
 *   - fonts 0..3 (body, accent, quiet, warning) and the labels: the header, footer and range
 *     at slots 20, 21, 23, and the info card at 22, or at 24 when it keeps clear of its contact;
 *   - the range pill at slot 38 in font 5: the range, large on a backing, while a press-and-turn
 *     changes it (as radar's zoom);
 *   - the picker's preview.
 * Needs a document (Image, canvas, document.fonts), flights-art.js and GorbBake. */
(function (root) {
  'use strict';
  var A = function () { return root.FlightsArt; };
  var M = function () { return root.FlightsMap; };

  var SHAPES = ['light', 'turboprop', 'bizjet', 'airliner', 'heavy', 'helicopter', 'generic'];
  var SHAPE_TITLES = ['Light', 'Turboprop', 'Business jet', 'Airliner', 'Heavy', 'Helicopter', 'Other'];
  var PLANE_ROWS = 15, SWEEP_FPS = 20, SEL_SLOT = 16, EM_SLOT = 17, SEL_R = 28, EM_R = 32;
  // the far-contact dots (apps/flights/main.c DOT_SLOTS): the nine slots an app's rows leave
  // free under 32, one per tracked aircraft past the PLANE_ROWS nearest
  var DOT_SLOTS = [18, 19, 25, 26, 27, 28, 29, 30, 31];
  var DOTS = [['bands', 'Dots by altitude'], ['plain', 'Dots, one colour'], ['off', 'Not shown']];
  var CARD_SLOT = 22, CARD_CLEAR_SLOT = 24, NONE = 0xF, G = 466, CX = 233;
  var PERIOD_MIN = 2, PERIOD_MAX = 30;            // main.c SWEEP_S_MIN/MAX: one turn, seconds
  var CHARSET = (function () {
    var c = [];
    for (var i = 0x20; i < 0x7F; i++) c.push(String.fromCharCode(i));
    return c.concat(['↑', '↓', '→', '←', '°']);
  })();
  // the fonts by slot, the tone each draws (§11.5: 14 px is the smallest)
  var FONTS = ['body', 'accent', 'quiet', 'warning'];
  // the labels (main.c LAB_SLOT0): fonts by tone (normal, accent, warning, quiet)
  var LABELS = [
    { slot: 20, name: 'header', tones: [0, 1, NONE, NONE] },
    { slot: 21, name: 'footer', tones: [2, NONE, 3, NONE] },
    { slot: CARD_SLOT, name: 'card', tones: [0, NONE, NONE, NONE], panel: true },
    { slot: 23, name: 'range', tones: [6, NONE, NONE, NONE] },
  ];
  var CARD_LINES = 4;
  // the range while it changes (main.c PILL_SLOT): large on a backing, as radar's zoom, in its
  // own font past the map names' (FONTS.length)
  var PILL_SLOT = 38, PILL_FONT = FONTS.length + 1, PILL_PX = 36, PILL_W = 340, PILL_GROW = 16;
  // the scale's length label (main.c LAB_RNG), fscale-2: its own font past the pill's (font 5)
  var SCALE_FONT = FONTS.length + 2;
  // the runtime's faces (fonts@1) the editor offers
  var FACES = ['IBM Plex Sans', 'IBM Plex Mono', 'Space Grotesk', 'Space Mono', 'Barlow Condensed', 'Jost',
    'Oswald', 'Michroma', 'Orbitron', 'Playfair Display', 'Cormorant Garamond', 'DM Serif Display', 'VT323'];
  var GROUNDS = ['scope', 'chart', 'sectional', 'brass', 'blueprint', 'galaxy', 'colour', 'image'];
  var GROUND_TITLES = { scope: 'Phosphor scope', chart: 'Aviation chart', sectional: 'Sectional chart', brass: 'Brass instrument', blueprint: 'Blueprint', galaxy: 'Galaxy', colour: 'Flat colour', image: 'Picture' };
  // the frames (§12.6): plain shapes, the families' own materials, and the author's picture
  var FRAME_STYLES = [['none', 'None'], ['square', 'Square'], ['rounded', 'Rounded'], ['pill', 'Pill'], ['double', 'Double line'],
    ['brass', 'Brass'], ['silver', 'Silver'], ['bronze', 'Bronze'], ['gold', 'Gold'], ['drafting', 'Drafting'], ['chart', 'Chart'], ['picture', 'Your picture']];
  var METALS = { brass: '#C9A24E', silver: '#C8CED6', bronze: '#A0703C', gold: '#E3B84A' };
  // a plane's heading cue (§12.8): drawn into its picture, so it turns with the track
  var HEADINGS = [['none', 'None'], ['trail', 'Trail'], ['arrow', 'Arrow']];
  var MARKS = [['ring', 'Ring'], ['dashed', 'Dashed'], ['double', 'Double'], ['brackets', 'Brackets'], ['crosshair', 'Crosshair'], ['diamond', 'Diamond']];
  var ARMS = [['wedge', 'Phosphor wedge'], ['line', 'Line'], ['needle', 'Instrument needle'], ['pointer', 'Brass pointer'], ['drafting', 'Drafting line'], ['none', 'None']];

  /* ---- the design (schema 2) and the presets: the five families, each on its ground ---- */
  var SCHEMA = 2;
  var PRESETS = ['modern', 'silver', 'brass', 'blueprint', 'paper', 'celestial'];
  // each ground's own scope: the old art's numbers, now the design's
  var SCOPES = {
    scope: { ground: 'scope', face: '#0E2A1A', detail: '#1C4A2E', grid: true,
      cross: { on: true, style: 'hatched', colour: '#2E7A4C', opacity: 0.6 },
      ticks: { on: true, colour: '#3DBB6E', r: 197, len: 10, width: 1.6, minor: 5, major: 30, style: 'line', edge: false },
      labels: { kind: 'degrees', every: 30, face: 'IBM Plex Mono', size: 15, colour: '#3DBB6E', turn: 'upright', skip: true, r: 213 },
      lubber: { on: false, colour: '#3DBB6E' }, rings: { n: 2, colour: '#2A6A44', opacity: 0.8, width: 1.5, dash: 'solid' }, home: { colour: '#FFFFFF', size: 3 } },
    chart: { ground: 'chart', face: '#EAE3CF', detail: '#8FB0C6', grid: true,
      cross: { on: false, style: 'solid', colour: '#2F5C8C', opacity: 0.5 },
      ticks: { on: true, colour: '#2F5C8C', r: 184, len: 12, width: 1.4, minor: 5, major: 10, style: 'line', edge: true },
      labels: { kind: 'tens', every: 30, face: 'IBM Plex Sans', size: 15, colour: '#2F5C8C', turn: 'radial', skip: true, r: 158 },
      lubber: { on: true, colour: '#2F5C8C' }, rings: { n: 2, colour: '#2F5C8C', opacity: 0.55, width: 1.5, dash: 'solid' }, home: { colour: '#2F5C8C', size: 3 } },
    brass: { ground: 'brass', face: '#262A26', detail: '#C9A24E', grid: false,
      cross: { on: false, style: 'solid', colour: '#C9A24E', opacity: 0.4 },
      ticks: { on: true, colour: '#E9DDBF', r: 197, len: 10, width: 2, minor: 5, major: 30, style: 'line', edge: false },
      labels: { kind: 'compass', every: 30, face: 'Cormorant Garamond', size: 17, colour: '#E9DDBF', turn: 'upright', skip: true, r: 206 },
      lubber: { on: true, colour: '#E8C46A' }, rings: { n: 2, colour: '#C9A24E', opacity: 0.45, width: 1.5, dash: 'solid' }, home: { colour: '#E8C46A', size: 3 } },
    blueprint: { ground: 'blueprint', face: '#1F5290', detail: '#FFFFFF', grid: true,
      cross: { on: true, style: 'dashdot', colour: '#DCE9F8', opacity: 0.55 },
      ticks: { on: true, colour: '#DCE9F8', r: 197, len: 10, width: 1.6, minor: 5, major: 30, style: 'line', edge: false },
      labels: { kind: 'degrees', every: 30, face: 'IBM Plex Mono', size: 15, colour: '#F4F8FF', turn: 'upright', skip: true, r: 213 },
      lubber: { on: false, colour: '#F4F8FF' }, rings: { n: 2, colour: '#DCE9F8', opacity: 0.45, width: 1.5, dash: 'solid' }, home: { colour: '#FFFFFF', size: 3 } },
    // flights-map-plan §4: a flight chart's look, made up: chart paper, a blue rose and blue
    // rings, a magenta band inside the rim
    sectional: { ground: 'sectional', face: '#EEF1E4', detail: '#2A5DB0', grid: true,
      cross: { on: false, style: 'dashed', colour: '#C2187A', opacity: 0.5 },
      ticks: { on: true, colour: '#2A5DB0', r: 197, len: 9, width: 1.4, minor: 5, major: 30, style: 'line', edge: true },
      labels: { kind: 'degrees', every: 30, face: 'Barlow Condensed', size: 17, colour: '#2A5DB0', turn: 'radial', skip: true, r: 214 },
      lubber: { on: true, colour: '#C2187A' }, rings: { n: 2, colour: '#2A5DB0', opacity: 0.85, width: 2, dash: 'solid' }, home: { colour: '#C2187A', size: 3.5 } },
    galaxy: { ground: 'galaxy', face: '#1A1240', detail: '#B04CFF', grid: true,
      cross: { on: false, style: 'dotted', colour: '#8C7BFF', opacity: 0.4 },
      ticks: { on: true, colour: '#B9B2FF', r: 197, len: 8, width: 1.4, minor: 5, major: 30, style: 'dot', edge: false },
      labels: { kind: 'cardinal', every: 90, face: 'Michroma', size: 14, colour: '#D9D4FF', turn: 'upright', skip: true, r: 213 },
      lubber: { on: true, colour: '#FFE9A8' }, rings: { n: 2, colour: '#8C7BFF', opacity: 0.35, width: 1, dash: 'dotted' }, home: { colour: '#FFE9A8', size: 3 } },
  };
  // the grid's weight by ground, px (§12.6)
  var GRID_W = { scope: 0.8, chart: 0.8, sectional: 0.7, blueprint: 0.6, galaxy: 1 };
  Object.keys(SCOPES).forEach(function (k) { SCOPES[k].gridWidth = GRID_W[k] || 0.8; });
  var ARM_LOOKS = {
    wedge: { colour: '#3DFF8E', tailColour: '#3DFF8E', lead: '#D8FFE8', tail: 30, halo: 3 },
    line: { colour: '#3DFF8E', tailColour: '#3DFF8E', lead: '#D8FFE8', tail: 0, halo: 2 },
    needle: { colour: '#F1F2EE', tailColour: '#F1F2EE', lead: '#F0662E', tail: 0, halo: 0 },
    pointer: { colour: '#C9A24E', tailColour: '#E8C46A', lead: '#C9A24E', tail: 0, halo: 0 },
    drafting: { colour: '#F4F8FF', tailColour: '#F4F8FF', lead: '#F4F8FF', tail: 0, halo: 0 },
    none: { colour: '#3DFF8E', tailColour: '#3DFF8E', lead: '#D8FFE8', tail: 0, halo: 0 },
  };
  var WORDS = {
    scope: { face: 'IBM Plex Sans', size: 16, body: '#E6F0EA', accent: '#39FF14', accent2: null, quiet: '#8A9A90', warning: '#FFB23C' },
    chart: { face: 'IBM Plex Sans', size: 16, body: '#14202C', accent: '#1E6B2E', accent2: null, quiet: '#5B5240', warning: '#B85C00' },
    brass: { face: 'Cormorant Garamond', size: 17, body: '#F2E6C8', accent: '#F8E7A8', accent2: '#B8892E', quiet: '#A8946A', warning: '#FF9A3C' },
    blueprint: { face: 'IBM Plex Mono', size: 15, body: '#FFFFFF', accent: '#9FE3FF', accent2: null, quiet: '#B8CCE6', warning: '#FFD27A' },
    galaxy: { face: 'Space Grotesk', size: 16, body: '#EDEBFF', accent: '#FFD27A', accent2: '#FF8A5C', quiet: '#9C95C9', warning: '#FF6B6B' },
    sectional: { face: 'Barlow Condensed', size: 19, body: '#1F2A44', accent: '#C2187A', accent2: null, quiet: '#3E5A80', warning: '#C2410C' },
  };
  var FRAMES = {
    scope: { style: 'rounded', edge: 1.5, pad: 8, fill: '#101820', opacity: 0.82, line: '#2A6A44' },
    chart: { style: 'rounded', edge: 1.5, pad: 8, fill: '#F4EEDC', opacity: 0.94, line: '#2F5C8C' },
    brass: { style: 'double', edge: 1.5, pad: 8, fill: '#161815', opacity: 0.88, line: '#C9A24E' },
    blueprint: { style: 'square', edge: 1.5, pad: 8, fill: '#1C4A84', opacity: 0.88, line: '#DCE9F8' },
    galaxy: { style: 'pill', edge: 1.25, pad: 8, fill: '#0B0820', opacity: 0.8, line: '#8C7BFF' },
    sectional: { style: 'rounded', edge: 1.5, pad: 8, fill: '#FBFBF5', opacity: 0.94, line: '#2A5DB0' },
  };
  // the scale each start draws (fscale-design §Review): ring/bar, its ink and edge; keyed by base
  var SCALE = {
    modern: { style: 'ring', ink: '#3DBB6E', edge: '#020805' },
    paper: { style: 'ring', ink: '#2F5C8C', edge: '#EAE3CF' },
    brass: { style: 'ring', ink: '#C9A24E', edge: '#161815' },
    blueprint: { style: 'bar', ink: '#DCE9F8', edge: '#1C4A84' },
    celestial: { style: 'ring', ink: '#8C7BFF', edge: '#0B0820' },
    silver: { style: 'ring', ink: '#C8CED6', edge: '#2C3138' },
    station: { style: 'ring', ink: '#111111', edge: '#FFFFFF' },
    sectional: { style: 'bar', ink: '#2A5DB0', edge: '#FBFBF5' },
    blank: { style: 'ring', ink: '#8F8F8F', edge: '#101010' },
  };
  // give a design its scale: the ring/bar, its look and its label's font (fscale-2)
  function applyScale(d) {
    var s = SCALE[d.base] || SCALE.blank, W = d.words, z = sizes(d);
    d.layout = d.layout || {};
    d.layout.scale = s.style;
    d.scale = { ink: s.ink, edge: s.edge, width: 2, tick: 9, max: 150 };
    W.scale = { face: W.face, px: z.quiet, colour: W.quiet };
  }
  // what a family brings beyond its ground: its own frame, and the celestial arm
  var FAMILY = {
    silver: { frame: 'silver' }, brass: { frame: 'brass' }, blueprint: { frame: 'drafting' }, paper: { frame: 'chart' },
    celestial: { frame: 'pill', heading: 'trail', arm: { kind: 'line', colour: '#C9BEFF', tailColour: '#8C7BFF', tail: 28, halo: 4 } },
  };
  // the starts beyond the plane families: Sectional (flights-map-plan §4) flies the Modern
  // silhouettes in a flight chart's inks
  // one name per look in every app (owner, 2026-11-08), in the radar's and the weather's order;
  // Station joins the tracker for parity (flights-map-plan §6)
  var STARTS = ['modern', 'paper', 'brass', 'blueprint', 'station', 'celestial', 'silver', 'sectional'];
  var LOOK_TITLES = { modern: 'Scope', paper: 'Paper', brass: 'Brass', blueprint: 'Blueprint', station: 'Station', celestial: 'Celestial',
    silver: 'Silver', sectional: 'Sectional' };
  var START_INFO = { sectional: { band: 'Flat silhouettes in a flight chart’s inks on chart paper: magenta, blue and yellow.' },
    station: { band: 'The railway clock’s look: black silhouettes on white, a red arm.' } };
  var SECTIONAL_BANDS = ['#8A8A8A', '#A0A4AA', '#C2187A', '#D9480F', '#2E7D32', '#2A5DB0', '#1F2A44'];
  function startInfo(k) {
    var f = A().families[k] || {}, x = START_INFO[k] || {};
    return { title: LOOK_TITLES[k] || f.title || k, band: x.band || f.band || '', single: f.single };
  }
  var clone = function (o) { return JSON.parse(JSON.stringify(o)); };
  function scopeOf(ground) { return clone(SCOPES[ground] || SCOPES.scope); }

  // the colour the OS draws its overlays in over this look (§64.9): the owner's, else the design's
  // own word accent (live, so an owner's earlier change to it moves the default), else that ground's
  // preset accent. Fallback the scope green #39FF14.
  var OVL_HEX = /^#[0-9a-f]{6}$/i;
  function overlayOf(d) {
    if (d && OVL_HEX.test(d.overlay)) return d.overlay;
    var a = d && d.words && d.words.accent;
    if (OVL_HEX.test(a)) return a;
    var g = (d && d.scope && d.scope.ground) || 'scope';
    a = WORDS[g] && WORDS[g].accent;
    return OVL_HEX.test(a) ? a : '#39FF14';
  }
  // the container's word (§2): the tag 0x01 names a colour, and only overlayOf answers the string
  function overlayWord(d) { return (0x01000000 | (parseInt(overlayOf(d).slice(1), 16) & 0xFFFFFF)) >>> 0; }
  function armOf(kind, period) { return Object.assign({ kind: kind, period: period || 8, length: 1 }, clone(ARM_LOOKS[kind] || ARM_LOOKS.wedge)); }
  // the blank start (§12.9): every part present, in plain greys on a plain ground, so nothing
  // is a family's look to work against; its own values are what the colour rows offer back
  function blank() {
    var d = preset('modern');
    d.name = 'Blank'; d.base = 'blank';
    d.scope = { ground: 'colour', face: '#101010', detail: '#2E2E2E', grid: false, gridWidth: 0.8, image: null,
      cross: { on: true, style: 'solid', colour: '#3A3A3A', opacity: 0.6 },
      ticks: { on: true, colour: '#8A8A8A', r: 197, len: 10, width: 1.4, minor: 5, major: 30, style: 'line', edge: false },
      labels: { kind: 'degrees', every: 30, face: 'IBM Plex Sans', size: 14, colour: '#9A9A9A', turn: 'upright', skip: true, r: 213 },
      lubber: { on: false, colour: '#BDBDBD' }, rings: { n: 2, colour: '#3A3A3A', opacity: 0.8, width: 1, dash: 'solid' }, home: { colour: '#FFFFFF', size: 3 } };
    d.arm = Object.assign(armOf('line', 8), { colour: '#BDBDBD', tailColour: '#BDBDBD', lead: '#FFFFFF', tail: 0, halo: 0,
      shadow: { on: false, opacity: 0.6, blur: 2.5 }, layer: 'under' });
    Object.assign(d.planes, { family: 'modern', form: 'types', colour: false, ground: 0.5, groundGrey: true, sel: '#FFFFFF', em: '#FF3B30',
      dots: { show: 'plain', colour: '#D0D0D0', size: 6 },
      heading: { style: 'none', length: 1, colour: '#FFFFFF' } });
    d.words = { face: 'IBM Plex Sans', size: 16, body: '#E6E6E6', accent: '#FFFFFF', accent2: null, quiet: '#8F8F8F', warning: '#E0A040' };
    d.frames = { style: 'rounded', edge: 1, pad: 8, fill: '#1A1A1A', opacity: 0.85, line: '#4A4A4A', image: null, corner: 10,
      card: { width: 240, place: 'auto' }, shadow: { on: false, blur: 4, opacity: 0.55, drop: 3 } };
    d.map = M().preset('blank', d.words.face, d.words.size);
    applyScale(d);
    return d;
  }
  function sectional() {
    var d = preset('modern');
    d.name = 'Sectional'; d.base = 'sectional';
    d.scope = scopeOf('sectional'); d.scope.image = null;
    d.arm = Object.assign(armOf('line', 8), { colour: '#C2187A', tailColour: '#C2187A', lead: '#C2187A', tail: 18, halo: 0,
      shadow: { on: false, opacity: 0.6, blur: 2.5 }, layer: 'under', length: 1 });
    Object.assign(d.planes, { family: 'modern', colour: true, bands: SECTIONAL_BANDS.slice(), ground: 0.7, groundGrey: false,
      sel: '#1F2A44', em: '#E00000', selStyle: 'ring', emStyle: 'double', dots: { show: 'bands', colour: '#1F2A44', size: 6 },
      shadow: { on: true, opacity: 0.35, blur: 1.5 } });
    d.words = clone(WORDS.sectional);
    d.frames = Object.assign(clone(FRAMES.sectional), { image: null, corner: 8, card: { width: 240, place: 'auto' },
      shadow: { on: true, blur: 3, opacity: 0.3, drop: 2 } });
    d.map = M().preset('sectional', d.words.face, d.words.size);
    applyScale(d);
    return d;
  }
  function station() {
    var d = preset('modern');
    d.name = 'Station'; d.base = 'station';
    d.scope = { ground: 'colour', face: '#F4F4F2', detail: '#111111', grid: false, gridWidth: 0.8, image: null,
      cross: { on: false, style: 'solid', colour: '#111111', opacity: 0.4 },
      ticks: { on: true, colour: '#111111', r: 199, len: 14, width: 3, minor: 6, major: 30, style: 'line', edge: false },
      labels: { kind: 'none', every: 30, face: 'Jost', size: 17, colour: '#111111', turn: 'upright', skip: true, r: 212 },
      lubber: { on: true, colour: '#E3000F' }, rings: { n: 2, colour: '#111111', opacity: 0.35, width: 1.5, dash: 'solid' },
      home: { colour: '#E3000F', size: 4 } };
    d.arm = Object.assign(armOf('line', 8), { colour: '#E3000F', tailColour: '#E3000F', lead: '#E3000F', tail: 0, halo: 0,
      shadow: { on: false, opacity: 0.6, blur: 2.5 }, layer: 'under', length: 1 });
    Object.assign(d.planes, { family: 'modern', colour: true, bands: ['#8A8A8A', '#B0B0B0', '#E3000F', '#D9480F', '#2E7D32', '#1F4E8C', '#111111'],
      ground: 0.7, groundGrey: false, sel: '#111111', em: '#E3000F', dots: { show: 'bands', colour: '#111111', size: 6 } });
    d.words = { face: 'Jost', size: 18, body: '#111111', accent: '#E3000F', accent2: null, quiet: '#555555', warning: '#E3000F' };
    d.frames = { style: 'square', edge: 1.5, pad: 8, fill: '#FFFFFF', opacity: 0.94, line: '#111111', image: null, corner: 4,
      card: { width: 240, place: 'auto' }, shadow: { on: false, blur: 3, opacity: 0.3, drop: 2 } };
    d.map = M().preset('station', d.words.face, d.words.size);
    applyScale(d);
    return d;
  }
  function preset(fam) {
    if (fam === 'blank') return blank();
    if (fam === 'sectional') return sectional();
    if (fam === 'station') return station();
    var a = A(), f = a.families[fam], bg = f.pair[0], look = a.backgrounds[bg].look, X = FAMILY[fam] || {};
    var d = {
      name: LOOK_TITLES[fam] || f.title, base: fam,
      scope: scopeOf(bg),
      arm: armOf(f.pair[1], 8),
      planes: { family: fam, form: 'types', size: a.plane_px, colour: true, bands: a.bands.map(function (b) { return b[1]; }),
        ground: 0.6, groundGrey: false, heading: { style: X.heading || 'none', length: 1, colour: '#E8E4FF' }, sel: look.sel, em: '#FF2020', custom: { image: null },
        selStyle: 'ring', selWidth: 2, emStyle: 'ring', emWidth: 2.5, shadow: { on: false, opacity: 0.6, blur: 2.5 },
        dots: { show: 'bands', colour: '#D0D0D0', size: 6 } },
      words: clone(WORDS[bg]),
      frames: Object.assign(clone(FRAMES[bg]), { image: null, corner: 10, card: { width: 240, place: 'auto' },
        shadow: { on: true, blur: 4, opacity: 0.55, drop: 3 } }),
    };
    d.arm.shadow = { on: false, opacity: 0.6, blur: 2.5 };
    d.arm.layer = 'under';   // §12.9: under the planes, or over them (still under the rings and words)
    if (X.arm) Object.assign(d.arm, X.arm);
    if (X.frame) d.frames.style = X.frame;
    if (fam === 'silver') {                     // look parity: graphite under a silver bezel, as the radar's and the weather's Silver
      d.scope = Object.assign(scopeOf('brass'), { face: '#2C3138', detail: '#C8CED6', image: null });
      d.scope.ticks.colour = '#E4E8EC'; d.scope.labels.colour = '#E4E8EC'; d.scope.labels.face = 'Jost';
      d.scope.lubber.colour = '#FFFFFF'; d.scope.rings.colour = '#C8CED6'; d.scope.home.colour = '#FFFFFF';
      d.words = { face: 'Jost', size: 17, body: '#F2F4F7', accent: '#7FC8FF', accent2: null, quiet: '#AAB2BD', warning: '#FF9A5C' };
    }
    d.map = M().preset(fam, d.words.face, d.words.size);
    applyScale(d);
    return d;
  }
  // a design saved before a field existed takes that field from its preset (§12.6: added
  // fields, no new schema); what it has is kept
  function complete(x) {
    var fam = x.base === 'blank' || x.base === 'sectional' || x.base === 'station' || A().families[x.base] ? x.base : A().families[x.planes && x.planes.family] ? x.planes.family : 'modern';
    var fill = function (o, dflt) {
      Object.keys(dflt).forEach(function (k) {
        if (o[k] === undefined) o[k] = clone(dflt[k]);
        else if (dflt[k] && typeof dflt[k] === 'object' && !Array.isArray(dflt[k]) && o[k] && typeof o[k] === 'object') fill(o[k], dflt[k]);
      });
      return o;
    };
    // a design from before the map keeps its scope as it was: the map starts off
    var p = preset(fam), y = clone(x);
    if (!y.map) y.map = Object.assign(clone(p.map), { on: false });
    var d = fill(y, p);
    delete d.frames.plaques;
    if (d.scope.gridWidth == null) d.scope.gridWidth = GRID_W[d.scope.ground] || 0.8;
    return d;
  }
  // schema 1 -> 2: the background named a whole drawing; it is now that drawing's numbers
  function upgrade1(x) {
    var bg = x.scope.bg, fam = x.planes.family in A().families ? x.planes.family : 'modern';
    var d = preset(fam), ground = SCOPES[bg] ? bg : bg === 'colour' || bg === 'image' ? bg : 'scope';
    d.name = x.name;
    d.scope = scopeOf(SCOPES[bg] ? bg : A().families[fam].pair[0]);
    d.scope.ground = ground;
    if (ground === 'colour') d.scope.face = x.scope.colour || d.scope.face;
    d.scope.image = x.scope.image || null;
    d.scope.rings = { n: x.scope.rings | 0, colour: x.scope.ringColour, opacity: +x.scope.ringOpacity, width: 1.5, dash: 'solid' };
    d.scope.home = { colour: x.scope.home, size: 3 };
    d.arm = armOf(x.arm.kind, 4);                  // schema 1 swept at main.c's old 4 s
    if (x.arm.kind === 'wedge') { d.arm.tail = +x.arm.trail || 30; d.arm.colour = d.arm.tailColour = x.arm.colour; d.arm.lead = x.arm.lead; }
    d.planes = { family: fam, form: x.planes.form === 'single' ? 'single' : 'types', size: x.planes.size, colour: !!x.planes.colour,
      bands: x.planes.bands.slice(), ground: x.planes.colour ? 1 : +x.planes.ground, sel: x.planes.sel, em: x.planes.em, custom: { image: null } };
    var w = x.words;
    d.words = { face: w.face, size: 16, body: w.body, accent: w.accent, accent2: w.accent2 || null, quiet: w.quiet, warning: w.warning };
    d.frames = { style: 'rounded', edge: 1.5, pad: 8, fill: w.card.fill, opacity: +w.card.opacity, line: w.card.edge,
      card: { width: 240, place: 'bottom' } };
    return complete(d);
  }

  /* ---- colour ---- */
  function rgbOf(hex) { var n = parseInt(String(hex || '#000').slice(1), 16) || 0; return [(n >> 16) & 255, (n >> 8) & 255, n & 255]; }
  function hexOf(c) { return '#' + c.map(function (v) { return Math.max(0, Math.min(255, Math.round(v))).toString(16).padStart(2, '0'); }).join(''); }
  // k < 0 toward black, k > 0 toward white
  function shade(hex, k) { var c = rgbOf(hex); return hexOf(c.map(function (v) { return k < 0 ? v * (1 + k) : v + (255 - v) * k; })); }
  function rgba(hex, a) { var c = rgbOf(hex); return 'rgba(' + c[0] + ',' + c[1] + ',' + c[2] + ',' + a + ')'; }
  var DASHES = { solid: [], dashed: [8, 6], dotted: [1.5, 4], dashdot: [14, 4, 2, 4], hatched: [] };

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
  function pt(r, deg) { var t = deg * Math.PI / 180; return [r * Math.sin(t), -r * Math.cos(t)]; }

  /* ---- the plate: the ground, the cross, the bezel, the rings and the home dot ---- */
  function circle(x, r) { x.beginPath(); x.arc(CX, CX, r, 0, 2 * Math.PI); }
  function line(x, x0, y0, x1, y1) { x.beginPath(); x.moveTo(x0, y0); x.lineTo(x1, y1); x.stroke(); }
  function grid(x, step, colour, op, w, major) {
    for (var k = -Math.ceil(CX / step); k <= Math.ceil(CX / step); k++) {
      if (!major && k === 0) continue;
      var o = CX + k * step, big = major && k % major === 0;
      x.strokeStyle = rgba(colour, big ? op * 2.8 : op); x.lineWidth = big ? w * 1.5 : w;
      line(x, o, 0, o, G); line(x, 0, o, G, o);
    }
  }
  // deep space (make_art.bg_galaxy's sky, from the design's colours): the space colour fading
  // to black, the galaxy's band, a nebula in the detail colour and a teal one, and a fixed
  // sky of stars; `dense` (the grid switch) doubles them
  function galaxy(x, f, dt, dense) {
    var rg = x.createRadialGradient(CX, CX, 0, CX, CX, CX * 1.2);
    rg.addColorStop(0, f); rg.addColorStop(1, shade(f, -0.85));
    x.fillStyle = rg; x.fillRect(0, 0, G, G);
    var blob = function (cx, cy, rx, ry, rot, col, a) {
      x.save(); x.translate(cx, cy); x.rotate(rot * Math.PI / 180); x.scale(1, ry / rx);
      var g = x.createRadialGradient(0, 0, 0, 0, 0, rx); g.addColorStop(0, rgba(col, a)); g.addColorStop(1, rgba(col, 0));
      x.fillStyle = g; x.beginPath(); x.arc(0, 0, rx, 0, 2 * Math.PI); x.fill(); x.restore();
    };
    blob(CX, CX, 300, 70, -32, '#E9DDFF', 0.22);
    blob(130, 150, 120, 80, 20, dt, 0.45);
    blob(340, 330, 110, 70, -15, '#2FD4FF', 0.35);
    var seed = 1987, rnd = function () { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
    for (var i = 0; i < (dense ? 640 : 320); i++) {
      var sx = rnd() * G, sy = rnd() * G, r = [0.5, 0.6, 0.7, 0.8, 1, 1.3][Math.floor(rnd() * 6)];
      x.fillStyle = 'rgba(255,255,255,' + (0.35 + rnd() * 0.65).toFixed(2) + ')';
      x.beginPath(); x.arc(sx, sy, r, 0, 2 * Math.PI); x.fill();
    }
    x.strokeStyle = 'rgba(255,248,231,0.6)'; x.lineWidth = 0.7;
    for (var j = 0; j < 9; j++) {
      var bx = 30 + rnd() * (G - 60), by = 30 + rnd() * (G - 60);
      x.fillStyle = '#FFF8E7'; x.beginPath(); x.arc(bx, by, 1.6, 0, 2 * Math.PI); x.fill();
      line(x, bx - 6, by, bx + 6, by); line(x, bx, by - 6, bx, by + 6);
    }
  }
  var BLOBS = [[120, 140, 70, 3, 0.4], [345, 320, 95, 4, 1.7], [330, 110, 50, 5, 2.9]];
  // flights-map-plan §4: chart paper with soft terrain tints, a graticule ticked like a
  // chart's, a dashed magenta isogonic line, and the magenta band a chart shades inside an
  // airspace's edge, here inside the rim
  var TINTS = [[110, 120, 120, '#E4ECD2', 5, 0.3], [360, 330, 140, '#EFE6CB', 4, 1.9], [380, 90, 80, '#E6EDD8', 3, 2.6], [90, 380, 90, '#F1E9D2', 5, 0.9]];
  function sectionalGround(x, sc, gw) {
    var f = sc.face, dt = sc.detail;
    x.fillStyle = f; x.fillRect(0, 0, G, G);
    TINTS.forEach(function (b) {
      x.beginPath();
      for (var i = 0; i <= 64; i++) {
        var t = 2 * Math.PI * i / 64, rr = b[2] * (1 + 0.16 * Math.sin(b[4] * t + b[5]) + 0.07 * Math.sin(2 * b[4] * t + b[5] * 2));
        x[i ? 'lineTo' : 'moveTo'](b[0] + rr * Math.cos(t), b[1] + rr * Math.sin(t));
      }
      x.fillStyle = b[3]; x.fill();
    });
    if (sc.grid) {
      x.strokeStyle = rgba(dt, 0.32); x.lineWidth = gw;
      for (var k = 1; k < 8; k++) {
        var o = k * 58 + 1;
        line(x, o, 0, o, G); line(x, 0, o, G, o);
        for (var m = 0; m < G; m += 11.6) {                // the minute ticks along each line
          var tl = Math.round(m / 11.6) % 5 ? 3 : 6;
          line(x, o - tl, m, o, m); line(x, m, o, m, o + tl);
        }
      }
    }
    x.save(); x.setLineDash([10, 6]); x.strokeStyle = 'rgba(194,24,122,0.45)'; x.lineWidth = 1.4;
    line(x, 70, 430, 400, 30); x.restore();
    var band = x.createRadialGradient(CX, CX, 196, CX, CX, 232);
    band.addColorStop(0, 'rgba(194,24,122,0)'); band.addColorStop(0.55, 'rgba(194,24,122,0.10)'); band.addColorStop(1, 'rgba(194,24,122,0.32)');
    circle(x, 232); x.fillStyle = band; x.fill();
  }
  async function ground(x, sc) {
    var f = sc.face, dt = sc.detail, gw = Math.max(0.3, Math.min(4, +sc.gridWidth || 0.8));
    x.fillStyle = '#000'; x.fillRect(0, 0, G, G);
    if (sc.ground === 'galaxy') { galaxy(x, f, dt, sc.grid); return; }
    if (sc.ground === 'image' && sc.image) {
      var im = await image(sc.image), k = Math.max(G / im.width, G / im.height);
      x.drawImage(im, (G - im.width * k) / 2, (G - im.height * k) / 2, im.width * k, im.height * k);
      return;
    }
    if (sc.ground === 'colour') { x.fillStyle = f; x.fillRect(0, 0, G, G); return; }
    if (sc.ground === 'chart') {
      x.fillStyle = f; x.fillRect(0, 0, G, G);
      BLOBS.forEach(function (b) {
        [[1.0, shade(f, -0.04)], [0.6, shade(f, -0.09)]].forEach(function (st) {
          x.beginPath();
          for (var i = 0; i <= 48; i++) {
            var t = 2 * Math.PI * i / 48, rr = b[2] * st[0] * (1 + 0.18 * Math.sin(b[3] * t + b[4]) + 0.08 * Math.sin(2 * b[3] * t + b[4] * 2));
            x[i ? 'lineTo' : 'moveTo'](b[0] + rr * Math.cos(t), b[1] + rr * Math.sin(t));
          }
          x.fillStyle = st[1]; x.fill();
        });
      });
      if (sc.grid) { x.lineWidth = gw; for (var k2 = 1; k2 < 8; k2++) { var o = k2 * 58 + 1; x.strokeStyle = rgba(dt, 0.45); line(x, o, 0, o, G); line(x, 0, o, G, o); } }
      x.strokeStyle = rgba(shade(dt, -0.35), 0.45); x.lineWidth = 1.6;
      line(x, 40, 380, 430, 60); line(x, 60, 90, 420, 410);
      return;
    }
    if (sc.ground === 'sectional') { sectionalGround(x, sc, gw); return; }
    if (sc.ground === 'brass') {
      x.fillStyle = '#0B0A08'; x.fillRect(0, 0, G, G);
      var mg = x.createLinearGradient(0, 0, G, G);
      [[0, shade(dt, 0.45)], [0.35, shade(dt, -0.35)], [0.55, shade(dt, 0.25)], [0.8, shade(dt, -0.55)], [1, shade(dt, 0.1)]].forEach(function (s) { mg.addColorStop(s[0], s[1]); });
      circle(x, CX - 1); x.fillStyle = mg; x.fill();
      var eg = x.createRadialGradient(CX, CX * 0.9, 0, CX, CX * 0.9, 216 * 1.1);
      eg.addColorStop(0, f); eg.addColorStop(1, shade(f, -0.65));
      circle(x, 216); x.fillStyle = eg; x.fill(); x.strokeStyle = shade(dt, -0.8); x.lineWidth = 2; x.stroke();
      circle(x, 211); x.strokeStyle = rgba(dt, 0.6); x.lineWidth = 1.2; x.stroke();
      [45, 135, 225, 315].forEach(function (d) {
        var p = pt(224, d);
        x.beginPath(); x.arc(CX + p[0], CX + p[1], 4.5, 0, 2 * Math.PI); x.fillStyle = dt; x.fill(); x.strokeStyle = shade(dt, -0.55); x.lineWidth = 0.8; x.stroke();
        x.save(); x.translate(CX + p[0], CX + p[1]); x.rotate((d + 20) * Math.PI / 180); x.lineWidth = 1.2; line(x, -3, 0, 3, 0); x.restore();
      });
      return;
    }
    var rg = x.createRadialGradient(CX, CX, 0, CX, CX, CX);
    if (sc.ground === 'blueprint') { rg.addColorStop(0, f); rg.addColorStop(1, shade(f, -0.3)); }
    else { rg.addColorStop(0, f); rg.addColorStop(0.78, shade(f, -0.5)); rg.addColorStop(1, shade(f, -0.9)); }
    x.fillStyle = sc.ground === 'blueprint' ? shade(f, -0.3) : shade(f, -0.9); x.fillRect(0, 0, G, G);
    circle(x, CX); x.fillStyle = rg; x.fill();
    if (sc.grid) {
      if (sc.ground === 'blueprint') grid(x, G / 40, dt, 0.07, gw, 5);
      else grid(x, 46, dt, 0.35, gw, 0);
    }
  }
  function cross(x, c) {
    if (!c.on) return;
    var R = A().ring_r;
    x.save(); x.strokeStyle = rgba(c.colour, c.opacity); x.lineWidth = 1; x.setLineDash(DASHES[c.style] || []);
    line(x, CX, CX - R, CX, CX + R); line(x, CX - R, CX, CX + R, CX);
    x.setLineDash([]);
    if (c.style === 'hatched') for (var k = -8; k <= 8; k++) {
      if (!k) continue;
      var o = CX + k * 23;
      line(x, o, CX - 3, o, CX + 3); line(x, CX - 3, o, CX + 3, o);
    }
    x.restore();
  }
  function ticks(x, t) {
    if (!t.on) return;
    var major = Math.max(1, t.major | 0), step = (t.minor | 0) > 0 ? t.minor | 0 : major;
    x.save(); x.strokeStyle = x.fillStyle = t.colour;
    if (t.edge) { circle(x, t.r); x.lineWidth = Math.max(1, t.width * 0.8); x.stroke(); }
    for (var d = 0; d < 360; d += step) {
      var big = d % major === 0, len = big ? t.len : t.len / 2, w = big ? t.width : Math.max(0.6, t.width * 0.6);
      if (t.style === 'dot') {
        var p = pt(t.r - t.len / 2, d);
        x.beginPath(); x.arc(CX + p[0], CX + p[1], big ? w * 1.4 : w * 0.9, 0, 2 * Math.PI); x.fill();
      } else if (t.style === 'wedge') {
        x.save(); x.translate(CX, CX); x.rotate(d * Math.PI / 180);
        var hw = big ? w * 1.8 : w * 1.1;
        x.beginPath(); x.moveTo(-hw, -t.r); x.lineTo(hw, -t.r); x.lineTo(0, -t.r + len); x.closePath(); x.fill(); x.restore();
      } else {
        var a = pt(t.r, d), b = pt(t.r - len, d);
        x.lineWidth = w; line(x, CX + a[0], CX + a[1], CX + b[0], CX + b[1]);
      }
    }
    x.restore();
  }
  var COMPASS = { 0: 'N', 90: 'E', 180: 'S', 270: 'W' };
  function figure(kind, d) {
    if (kind === 'degrees') return String(d).padStart(3, '0');
    if (kind === 'tens') return String(d / 10);
    if (kind === 'compass') return COMPASS[d] || String(d);
    return COMPASS[d] || '';
  }
  async function figures(x, l) {
    if (l.kind === 'none') return;
    await loadFace(l.face, l.size);
    var every = l.kind === 'cardinal' ? 90 : Math.max(10, l.every | 0);
    x.save(); x.fillStyle = l.colour; x.font = '600 ' + l.size + 'px "' + l.face + '", sans-serif';
    x.textAlign = 'center'; x.textBaseline = 'middle';
    for (var d = 0; d < 360; d += every) {
      if (l.skip && (d === 0 || d === 180)) continue;     // the header sits over north, the card over south
      var s = figure(l.kind, d), p = pt(l.r, d);
      if (!s) continue;
      if (l.turn === 'radial') { x.save(); x.translate(CX + p[0], CX + p[1]); x.rotate(d * Math.PI / 180); x.fillText(s, 0, 0); x.restore(); }
      else x.fillText(s, CX + p[0], CX + p[1]);
    }
    x.restore();
  }
  function lubber(x, sc) {
    if (!sc.lubber.on) return;
    var base = (sc.ticks.on ? sc.ticks.r - sc.ticks.len : A().ring_r) - 2;
    x.fillStyle = sc.lubber.colour;
    x.beginPath(); x.moveTo(CX, CX - base + 13); x.lineTo(CX + 6, CX - base); x.lineTo(CX - 6, CX - base); x.closePath(); x.fill();
  }
  function rings(x, sc) {
    var R = A().ring_r, r = sc.rings, n = Math.max(0, Math.min(4, r.n | 0));
    x.save(); x.strokeStyle = rgba(r.colour, r.opacity); x.lineWidth = r.width; x.setLineDash(DASHES[r.dash] || []);
    for (var k = 1; k <= n; k++) { circle(x, R * k / n); x.stroke(); }
    x.restore();
    if (sc.home.size > 0) { x.beginPath(); x.arc(CX, CX, sc.home.size, 0, 2 * Math.PI); x.fillStyle = sc.home.colour; x.fill(); }
  }
  async function plateCanvas(d) {
    var c = canvas(G, G), x = c.getContext('2d'), sc = d.scope;
    await ground(x, sc);
    cross(x, sc.cross);
    ticks(x, sc.ticks);
    await figures(x, sc.labels);
    lubber(x, sc);
    if (sc.ground !== 'colour' && sc.ground !== 'image') { circle(x, CX - 2); x.strokeStyle = 'rgba(0,0,0,0.45)'; x.lineWidth = 4; x.stroke(); }
    rings(x, sc);
    return c;
  }

  /* ---- the arm: any blade, its length, a fading tail behind it and a halo along it, pivot
   * at (0, 0) pointing up; the row turns it clockwise ---- */
  function tailSvg(L, deg, colour) {
    var n = Math.max(8, Math.round(deg * 0.8)), s = '';
    for (var i = 0; i < n; i++) {
      var a0 = -deg * i / n, a1 = -deg * (i + 1) / n, p0 = pt(L, a0), p1 = pt(L, a1);
      s += '<path d="M0 0L' + p0[0].toFixed(2) + ' ' + p0[1].toFixed(2) + 'A' + L + ' ' + L + ' 0 0 0 ' + p1[0].toFixed(2) + ' ' + p1[1].toFixed(2) +
        'Z" fill="' + colour + '" fill-opacity="' + (0.42 * Math.pow(1 - i / n, 2)).toFixed(3) + '"/>';
    }
    return s;
  }
  function needle(L, blade, tip) {
    var ink = '#1A1D21', st = 'fill="' + blade + '" stroke="' + ink + '" stroke-width="1.2" stroke-linejoin="round"';
    return { back: 42, w: 14, body: '<path d="M0 ' + (-L) + 'L3.2 -24L4 0L-4 0L-3.2 -24Z" ' + st + '/>' +
      '<path d="M0 ' + (-L) + 'L1.9 ' + (-L + 34) + 'L-1.9 ' + (-L + 34) + 'Z" fill="' + tip + '"/>' +
      '<path d="M-3.6 0L-4.5 26L4.5 26L3.6 0Z" ' + st + '/><circle cx="0" cy="30" r="7.5" ' + st + '/>' +
      '<circle cx="0" cy="0" r="11" fill="' + ink + '"/><circle cx="0" cy="0" r="8.5" fill="#B9BEC4"/>' +
      '<circle cx="0" cy="0" r="5.5" fill="#E4E7EA" stroke="#8E959C" stroke-width="0.8"/><circle cx="0" cy="0" r="1.8" fill="' + ink + '"/>' };
  }
  function pointer(L, metal) {
    var ink = '#2B1A08', ring = function (y, ro, ri) {
      return 'M' + (-ro) + ' ' + y + 'a' + ro + ' ' + ro + ' 0 1 0 ' + 2 * ro + ' 0a' + ro + ' ' + ro + ' 0 1 0 ' + (-2 * ro) + ' 0Z' +
        'M' + (-ri) + ' ' + y + 'a' + ri + ' ' + ri + ' 0 1 1 ' + 2 * ri + ' 0a' + ri + ' ' + ri + ' 0 1 1 ' + (-2 * ri) + ' 0Z';
    };
    var st = 'fill="url(#arm-metal)" stroke="' + ink + '" stroke-width="1" stroke-linejoin="round"', y = L - 30;   // the moon ring, 30 px in from the tip (make_art.arm_pointer)
    return { back: 40, w: 14, body: '<defs><linearGradient id="arm-metal" gradientUnits="userSpaceOnUse" x1="-9" y1="0" x2="9" y2="0">' +
      '<stop offset="0" stop-color="' + shade(metal, -0.4) + '"/><stop offset=".35" stop-color="' + shade(metal, 0.55) + '"/>' +
      '<stop offset=".6" stop-color="' + metal + '"/><stop offset="1" stop-color="' + shade(metal, -0.45) + '"/></linearGradient></defs>' +
      '<path d="M-2.6 ' + (-(y - 8)) + 'L2.6 ' + (-(y - 8)) + 'L4.2 -10L-4.2 -10Z" ' + st + '/>' +
      '<path d="' + ring(-y, 8, 4.5) + '" fill-rule="evenodd" ' + st + '/>' +
      '<path d="M0 ' + (-L) + 'L3.2 ' + (-(y + 7)) + 'L-3.2 ' + (-(y + 7)) + 'Z" ' + st + '/>' +
      '<path d="M-3.4 0L-2.6 22L2.6 22L3.4 0Z" ' + st + '/><path d="' + ring(28, 7.5, 3) + '" fill-rule="evenodd" ' + st + '/>' +
      '<circle cx="0" cy="0" r="11" fill="' + ink + '"/><circle cx="0" cy="0" r="9" fill="url(#arm-metal)"/>' +
      '<circle cx="0" cy="0" r="5" fill="' + shade(metal, -0.3) + '"/>' +
      '<line x1="-4" y1="0" x2="4" y2="0" stroke="' + ink + '" stroke-width="1.4" transform="rotate(30)"/>' };
  }
  function drafting(L, colour, hub) {
    var t = '';
    for (var y = 46; y < L - 20; y += 46) t += '<line x1="-3" y1="' + (-y) + '" x2="3" y2="' + (-y) + '" stroke="' + colour + '" stroke-width="1.4"/>';
    return { back: 20, w: 9, body: '<line x1="0" y1="-6" x2="0" y2="' + (-L + 12) + '" stroke="' + colour + '" stroke-width="2.4"/>' +
      '<path d="M0 ' + (-L) + 'L5.5 ' + (-L + 14) + 'L-5.5 ' + (-L + 14) + 'Z" fill="' + colour + '"/>' + t +
      '<line x1="0" y1="6" x2="0" y2="14" stroke="' + colour + '" stroke-width="2"/><line x1="-5" y1="14" x2="5" y2="14" stroke="' + colour + '" stroke-width="2"/>' +
      '<circle cx="0" cy="0" r="6.5" fill="' + hub + '" stroke="' + colour + '" stroke-width="2"/>' +
      '<line x1="-3.5" y1="0" x2="3.5" y2="0" stroke="' + colour + '" stroke-width="1.2"/><line x1="0" y1="-3.5" x2="0" y2="3.5" stroke="' + colour + '" stroke-width="1.2"/>' };
  }
  function arm(d) {
    var m = d.arm, R = A().ring_r;
    if (m.kind === 'none') return { svg: svgDoc('0 0 2 2', ''), pivot: [1, 1], size: [2, 2] };
    var L = Math.round(R * Math.max(0.5, Math.min(1, +m.length || 1))) - (m.kind === 'needle' || m.kind === 'pointer' ? 4 : 0);
    var tail = Math.max(0, Math.min(90, +m.tail || 0)), halo = Math.max(0, Math.min(10, +m.halo || 0));
    var blade = m.kind === 'needle' ? needle(L, m.colour, m.lead) : m.kind === 'pointer' ? pointer(L, m.colour)
      : m.kind === 'drafting' ? drafting(L, m.colour, shade(d.scope.face, -0.2))
      : { back: 4, w: 4, body: '<line x1="0" y1="0" x2="0" y2="' + (-L) + '" stroke="' + (m.kind === 'wedge' ? m.lead : m.colour) + '" stroke-width="2"/>' };
    var glow = halo ? '<filter id="halo" x="-200%" y="-5%" width="500%" height="110%"><feGaussianBlur stdDeviation="' + (halo * 0.55).toFixed(2) + '"/></filter>' : '';
    var body = (tail ? tailSvg(L, tail, m.tailColour) : '') +
      (halo ? '<line x1="0" y1="0" x2="0" y2="' + (-L) + '" stroke="' + m.colour + '" stroke-width="' + (2 + halo * 1.2).toFixed(1) + '" filter="url(#halo)"/>' : '') + blade.body;
    // the shadow: a soft dark copy of the whole arm under it, baked into the art (§12.6). It
    // falls all round, not to one side: the art turns with the arm, so a side would turn too
    var sh = m.shadow && m.shadow.on ? Math.max(0.5, Math.min(6, +m.shadow.blur || 2.5)) : 0;
    if (sh) {
      glow += '<filter id="shadow" x="-50%" y="-10%" width="200%" height="120%"><feGaussianBlur in="SourceAlpha" stdDeviation="' + sh + '"/>' +
        '<feComponentTransfer><feFuncA type="linear" slope="' + Math.max(0.1, Math.min(1, +m.shadow.opacity || 0.6)) + '"/></feComponentTransfer>' +
        '<feMerge><feMergeNode/><feMergeNode in="SourceGraphic"/></feMerge></filter>';
      body = '<g filter="url(#shadow)">' + body + '</g>';
    }
    var pad = Math.ceil(halo * 1.5 + sh * 2.5);
    var x0 = -Math.ceil(Math.max(blade.w, tail ? L * Math.sin(tail * Math.PI / 180) : 0)) - pad - 4;
    var x1 = blade.w + pad + 4, y0 = -L - 4 - Math.max(Math.ceil(halo), Math.ceil(sh * 2.5)), y1 = blade.back + 4 + Math.ceil(sh * 2.5);
    var vb = [x0, y0, x1 - x0, y1 - y0];
    return { svg: svgDoc(vb.join(' '), body, glow), pivot: [-x0, -y0], size: [vb[2], vb[3]] };
  }
  function period(d) { return Math.max(PERIOD_MIN, Math.min(PERIOD_MAX, Math.round(+d.arm.period || 8))); }

  /* ---- the planes: a family's drawings (one per category, or its one detailed plane), or
   * the author's own picture, tinted by altitude or as drawn. Ground traffic fades to its
   * opacity, and to nothing when it is off. ---- */
  function planeSvg(d, si, bi) {
    var a = A(), f = a.families[d.planes.family] || a.families.modern, key = d.planes.form === 'single' ? 'single' : SHAPES[si];
    var body;
    if (d.planes.colour) body = f.shapes[key].split('{{B}}').join(d.planes.bands[bi]);
    else if (f.neutral) body = f.shapes[key].split('{{B}}').join(f.neutral);
    else body = f.plain[key];
    return svgDoc('0 0 100 100', body, a.gradients);
  }
  function planePx(d) { return Math.max(30, Math.min(48, d.planes.size | 0)); }
  // the shadow's softness, px, 0 when off; and the frame each plane is drawn in: its size
  // and room for its shadow round it
  function planeShadow(d) { var s = d.planes.shadow; return s && s.on ? Math.max(0.5, Math.min(5, +s.blur || 2.5)) : 0; }
  function planeBox(d) { return planePx(d) + 2 * Math.ceil(planeShadow(d) * 2.5); }
  // a far contact's dot: its box (the dot and a 1 px dark rim) and its frames, the 7 altitude
  // bands or one colour; none when the design shows no far aircraft
  function dotPx(d) { var t = d.planes.dots || {}; return Math.max(4, Math.min(10, t.size | 0 || 6)); }
  function dotBox(d) { return dotPx(d) + 2; }
  function dotFrames(d) {
    var t = d.planes.dots || {}, show = t.show || 'off';
    if (show === 'off') return [];
    var cols = show === 'bands' ? d.planes.bands.slice(0, 7) : [t.colour || '#D0D0D0'];
    return cols.map(function (col) {
      var b = dotBox(d), c = canvas(b, b), x = c.getContext('2d'), r = dotPx(d) / 2;
      x.beginPath(); x.arc(b / 2, b / 2, r, 0, 2 * Math.PI);
      x.fillStyle = col; x.fill();
      x.lineWidth = 1; x.strokeStyle = 'rgba(0,0,0,0.55)'; x.stroke();
      return c;
    });
  }
  // a plane drawn in its frame, its shadow under it
  function shadowed(c, d) {
    var sh = planeShadow(d);
    if (!sh) return c;
    var fw = planeBox(d), m = (fw - c.width) >> 1, o = canvas(fw, fw), x = o.getContext('2d'), k = canvas(c.width, c.height), kx = k.getContext('2d');
    kx.drawImage(c, 0, 0); kx.globalCompositeOperation = 'source-in'; kx.fillStyle = '#000'; kx.fillRect(0, 0, c.width, c.height);
    x.filter = 'blur(' + sh + 'px)'; x.globalAlpha = Math.max(0.1, Math.min(1, +d.planes.shadow.opacity || 0.6));
    x.drawImage(k, m, m);
    x.filter = 'none'; x.globalAlpha = 1; x.drawImage(c, m, m);
    return o;
  }
  // the author's picture in a px square, fitted; with altitude colours on, tinted with `tint`
  // (its light parts take the band's colour, its dark parts stay dark)
  function customCanvas(im, px, tint) {
    var c = canvas(px, px), x = c.getContext('2d'), k = Math.min(px / im.width, px / im.height);
    var w = im.width * k, h = im.height * k, ox = (px - w) / 2, oy = (px - h) / 2;
    x.drawImage(im, ox, oy, w, h);
    if (tint) {
      x.globalCompositeOperation = 'multiply'; x.fillStyle = tint; x.fillRect(0, 0, px, px);
      x.globalCompositeOperation = 'destination-in'; x.drawImage(im, ox, oy, w, h);
    }
    return c;
  }
  // the heading cue on a plane's picture: a trail fading behind it, or an arrow at its nose.
  // It takes the altitude colour when the planes wear one, else its own.
  function headed(c, d, colour) {
    var hd = d.planes.heading, st = hd && hd.style;
    if (st !== 'trail' && st !== 'arrow') return c;
    // the body gives the cue room: smaller, and forward of the trail or back from the arrow
    var px = c.width, m = px / 2, o = canvas(px, px), x = o.getContext('2d');
    var k = st === 'trail' ? 0.68 : 0.8, by = st === 'trail' ? m - px * 0.15 : m + px * 0.07;
    if (st === 'trail') {
      var len = Math.max(0.3, Math.min(1, +hd.length || 1)), y1 = by + (px - 1 - by) * len, w = px * 0.22;
      var gr = x.createLinearGradient(0, by, 0, y1);
      gr.addColorStop(0, rgba(colour, 0.95)); gr.addColorStop(0.35, rgba(colour, 0.55)); gr.addColorStop(1, rgba(colour, 0));
      x.beginPath(); x.moveTo(m - w, by); x.quadraticCurveTo(m - w * 0.5, (by + y1) / 2, m, y1); x.quadraticCurveTo(m + w * 0.5, (by + y1) / 2, m + w, by); x.closePath();
      x.fillStyle = gr; x.fill();
    }
    x.drawImage(c, m - m * k, by - m * k, px * k, px * k);
    if (st === 'arrow') {
      var a = px * 0.14;
      x.beginPath(); x.moveTo(m, 1.5); x.lineTo(m + a, 1.5 + a * 1.05); x.lineTo(m, 1.5 + a * 0.7); x.lineTo(m - a, 1.5 + a * 1.05); x.closePath();
      x.lineJoin = 'round'; x.lineWidth = 1.2; x.strokeStyle = 'rgba(0,0,0,0.55)'; x.stroke(); x.fillStyle = colour; x.fill();
    }
    return o;
  }
  function rgba(hex, al) {
    var h = String(hex || '#ffffff').replace('#', ''); if (h.length === 3) h = h.replace(/./g, '$&$&');
    var n = parseInt(h, 16) || 0;
    return 'rgba(' + (n >> 16 & 255) + ',' + (n >> 8 & 255) + ',' + (n & 255) + ',' + al + ')';
  }
  async function planeFrames(d) {
    var px = planePx(d), out = [], g = Math.max(0, Math.min(1, +d.planes.ground)), custom = d.planes.family === 'custom';
    var im = custom && d.planes.custom && d.planes.custom.image ? await image(d.planes.custom.image) : null;
    for (var si = 0; si < 7; si++)
      for (var bi = 0; bi < 7; bi++) {
        var c;
        if (custom) {
          if (si > 0 && out.length >= 7) { out.push(out[bi]); continue; }      // one picture: every category wears it
          c = im ? customCanvas(im, px, d.planes.colour ? d.planes.bands[bi] : null) : canvas(px, px);
        } else c = await rast(planeSvg(d, si, bi), px, px);
        c = shadowed(headed(c, d, d.planes.colour ? d.planes.bands[bi] : (d.planes.heading && d.planes.heading.colour) || '#FFFFFF'), d);
        if (bi === 0 && (g < 1 || d.planes.groundGrey)) {   // ground traffic recedes, greys, or is off
          var x = c.getContext('2d'), img = x.getImageData(0, 0, c.width, c.height), D = img.data;
          for (var i = 0; i < D.length; i += 4) {
            if (d.planes.groundGrey) { var l = Math.round(0.299 * D[i] + 0.587 * D[i + 1] + 0.114 * D[i + 2]); D[i] = D[i + 1] = D[i + 2] = l; }
            D[i + 3] = Math.round(D[i + 3] * g);
          }
          x.putImageData(img, 0, 0);
        }
        out.push(c);
      }
    return out;
  }
  // a marker ring of radius r: its style (MARKS) and weight (§12.6)
  function markerSvg(r, colour, style, width) {
    var w = Math.max(1, Math.min(6, +width || 2)), rr = r - w / 2 - 0.5, st = 'fill="none" stroke="' + colour + '" stroke-width="' + w + '"';
    var c = function (rad, extra, ww) {
      return '<circle cx="' + r + '" cy="' + r + '" r="' + rad.toFixed(2) + '" fill="none" stroke="' + colour + '" stroke-width="' + (ww || w) + '"' + (extra || '') + '/>';
    };
    var C = 2 * Math.PI * rr, body;
    if (style === 'dashed') body = c(rr, ' stroke-dasharray="' + (C / 16).toFixed(2) + ' ' + (C / 16).toFixed(2) + '"');
    else if (style === 'double') body = c(rr) + c(Math.max(2, rr - w * 2.2), '', Math.max(1, w * 0.6));
    else if (style === 'brackets') body = c(rr, ' stroke-dasharray="' + (C * 0.14).toFixed(2) + ' ' + (C * 0.11).toFixed(2) + '" stroke-dashoffset="' + (C * 0.195).toFixed(2) + '"');
    else if (style === 'crosshair') {
      body = c(rr, '', Math.max(1, w * 0.6));
      for (var k = 0; k < 4; k++) body += '<line x1="' + r + '" y1="' + (r - rr - w / 2) + '" x2="' + r + '" y2="' + (r - rr * 0.55) + '" stroke="' + colour + '" stroke-width="' + w + '" transform="rotate(' + k * 90 + ' ' + r + ' ' + r + ')"/>';
    } else if (style === 'diamond') {
      var q = rr * 0.98;
      body = '<path d="M' + r + ' ' + (r - q) + 'L' + (r + q) + ' ' + r + 'L' + r + ' ' + (r + q) + 'L' + (r - q) + ' ' + r + 'Z" ' + st + ' stroke-linejoin="round"/>';
    } else body = c(rr);
    return svgDoc('0 0 ' + 2 * r + ' ' + 2 * r, body);
  }
  function markers(d) {
    var p = d.planes;
    return [[SEL_SLOT, SEL_R, markerSvg(SEL_R, p.sel, p.selStyle, p.selWidth)], [EM_SLOT, EM_R, markerSvg(EM_R, p.em, p.emStyle, p.emWidth)]];
  }

  /* ---- the words: their sizes, the boxes they stand in, and the frames round them ---- */
  function sizes(d) { var b = Math.max(14, Math.min(22, d.words.size | 0 || 16)), q = Math.max(14, b - 2); return { body: b, accent: b, quiet: q, warning: q }; }
  // every label's box [x, y, w, h], its padding and whether it wears a frame: only the card
  // does; the header, footer and range are words on the glass (owner, §12.7: no plaques)
  function layout(d) {
    var z = sizes(d), F = d.frames, pad = Math.max(2, Math.min(16, F.pad | 0));
    var lineH = function (px) { return Math.round(px * 1.35); };
    var hh = lineH(z.body) + 4, fh = lineH(z.quiet) + 4;
    var cw = Math.max(200, Math.min(320, F.card.width | 0 || 240)), ch = CARD_LINES * lineH(z.body) + 2 * pad;
    var cy = F.card.place === 'top' ? 44 : 422 - ch;
    var box = function (w, h, y) { return [CX - (w >> 1), y, w, h]; };
    // the range's box: sized for the longest the figure can say ("2000 km", 8 cells) in the
    // scale's own font, centred on the glass (main.c re-poses it over the ring or the foot bar)
    var sp = Math.max(14, Math.min(40, (d.words.scale && d.words.scale.px) || z.quiet));
    var rw = (Math.floor(sp / 2) + 1) * 8 + 8, rh = sp + 6;
    return {
      header: { box: box(240, hh, 10), pad: 2, frame: false },
      footer: { box: box(240, fh, 456 - fh), pad: 2, frame: false },
      range: { box: [CX - (rw >> 1), CX - (rh >> 1), rw, rh], pad: 2, frame: false },
      card: cardBox(d, box(cw, ch, cy), pad),
    };
  }
  /* the card's drop shadow (owner, §12.7): the card never turns, so its shadow falls one way,
   * `drop` px down and a third of that right. It is drawn inside the label's plane, so the
   * plane grows by the shadow's reach on every side and the card stands where it did: `box`
   * is the plane, `inner` the card, and the words keep their padding from the card's edge */
  function cardShadow(d) {
    var s = d.frames.shadow;
    if (!s || !s.on || d.frames.style === 'none') return null;
    var blur = Math.max(0.5, Math.min(6, +s.blur || 4)), drop = Math.max(0, Math.min(8, +s.drop || 0));
    return { blur: blur, drop: drop, opacity: Math.max(0.1, Math.min(1, +s.opacity || 0.55)), reach: Math.ceil(blur * 2 + drop) };
  }
  function cardBox(d, b, pad) {
    // the reach rides in the label's padding, which a plane caps at 32 px (LABEL_PAD_MAX)
    var sh = cardShadow(d), m = sh ? Math.min(sh.reach, 32 - pad) : 0;
    if (sh) sh.reach = m;
    return { box: [b[0] - m, b[1] - m, b[2] + 2 * m, b[3] + 2 * m], inner: b, pad: pad + m, frame: true, shadow: sh,
      clear: d.frames.card.place === 'auto' };
  }
  function frameSvg(d, b, isCard) {
    var F = d.frames, w = b[2], h = b[3], e = Math.max(0, Math.min(4, +F.edge || 0)), st = F.style;
    if (st === 'none' || st === 'picture') return svgDoc('0 0 ' + w + ' ' + h, '');
    var metal = METALS[st];
    if (metal || st === 'drafting' || st === 'chart') e = Math.max(e, metal ? 2.5 : 1);
    var rx = st === 'square' || st === 'drafting' ? 0 : st === 'pill' ? h / 2 : st === 'chart' ? 2 : Math.min(isCard ? 10 : 6, h / 2), i = e / 2 + 0.25;
    var rect = function (inset, extra, rr) {
      return '<rect x="' + inset + '" y="' + inset + '" width="' + (w - 2 * inset) + '" height="' + (h - 2 * inset) + '" rx="' + Math.max(0, rr) + '" ' + extra + '/>';
    };
    var fill = rect(i, 'fill="' + F.fill + '" fill-opacity="' + F.opacity + '"', rx), defs = '', r = '';
    if (metal) {
      // a bevelled metal bezel: a lit-to-shaded band, a dark seat inside it, a bright edge
      defs = '<linearGradient id="m" x1="0" y1="0" x2="0.35" y2="1"><stop offset="0" stop-color="' + shade(metal, 0.55) + '"/>' +
        '<stop offset="0.45" stop-color="' + metal + '"/><stop offset="0.55" stop-color="' + shade(metal, -0.35) + '"/><stop offset="1" stop-color="' + shade(metal, 0.2) + '"/></linearGradient>';
      r = fill + rect(i, 'fill="none" stroke="url(#m)" stroke-width="' + e + '"', rx) +
        rect(e + 0.6, 'fill="none" stroke="' + shade(metal, -0.7) + '" stroke-opacity="0.8" stroke-width="0.8"', rx - e) +
        rect(0.4, 'fill="none" stroke="' + shade(metal, 0.7) + '" stroke-opacity="0.7" stroke-width="0.6"', rx);
    } else if (st === 'drafting') {
      // a ruled box with corner brackets and a centre tick on each long side
      var L = Math.min(10, h / 3), c = 'stroke="' + F.line + '" stroke-width="' + (e * 1.6) + '" fill="none"', z = e;
      r = fill + rect(i, 'fill="none" stroke="' + F.line + '" stroke-opacity="0.7" stroke-width="' + e + '"', 0) +
        ['M' + z + ' ' + (z + L) + 'V' + z + 'H' + (z + L), 'M' + (w - z - L) + ' ' + z + 'H' + (w - z) + 'V' + (z + L),
         'M' + (w - z) + ' ' + (h - z - L) + 'V' + (h - z) + 'H' + (w - z - L), 'M' + (z + L) + ' ' + (h - z) + 'H' + z + 'V' + (h - z - L),
         'M' + w / 2 + ' ' + z + 'V' + (z + 4), 'M' + w / 2 + ' ' + (h - z) + 'V' + (h - z - 4)].map(function (dd) { return '<path d="' + dd + '" ' + c + '/>'; }).join('');
    } else if (st === 'chart') {
      // a chart's legend box: a rule, and a hairline inside it
      r = fill + rect(i, 'fill="none" stroke="' + F.line + '" stroke-width="' + e + '"', rx) +
        rect(i + e + 1.5, 'fill="none" stroke="' + F.line + '" stroke-opacity="0.6" stroke-width="0.7" stroke-dasharray="3 2"', 0);
    } else {
      r = rect(i, 'fill="' + F.fill + '" fill-opacity="' + F.opacity + '"' + (e ? ' stroke="' + F.line + '" stroke-width="' + e + '"' : ''), rx);
      if (st === 'double' && e) r += rect(i + e + 2, 'fill="none" stroke="' + F.line + '" stroke-width="' + Math.max(0.75, e * 0.6) + '" stroke-opacity="0.8"', rx - e - 2);
    }
    return svgDoc('0 0 ' + w + ' ' + h, r, defs);
  }
  // the pill's backing wears the card's frame (fscale-3, §7.5): the same frameSvg the labels
  // use, so it reads as a small card — the design's fill, line, edge and style — not a flat
  // grey lozenge. A 'rounded' frame keeps the pill's own rounded silhouette; every other
  // style takes the frame's shape. An empty frame (`none`/`picture`) draws nothing behind it.
  async function pillCanvas(d, w, h) {
    var F = d.frames;
    var look = Object.assign({}, d, { frames: Object.assign({}, F, { style: F.style === 'rounded' ? 'pill' : F.style }) });
    return await rast(frameSvg(look, [0, 0, w, h], false), w, h);
  }
  function panelSvg(d, name) {
    var L = layout(d)[name], b = L.box;
    return L.frame ? frameSvg(d, b, name === 'card') : svgDoc('0 0 ' + b[2] + ' ' + b[3], '');
  }
  /* the author's picture as a frame (§12.6): cut in nine, so its corners keep their shape
   * at any box and its edges and middle stretch. The corners are the picture's outer sixth,
   * drawn `corner` px on the glass. The size it suits: three times the card */
  function pictureFit(d) { var c = layout(d).card.inner; return [c[2] * 3, c[3] * 3]; }
  function nine(im, w, h, corner, opacity) {
    var c = canvas(w, h), x = c.getContext('2d'), iw = im.width, ih = im.height;
    var S = Math.max(1, Math.round(Math.min(iw, ih) / 6)), D = Math.max(2, Math.min(corner, w / 2, h / 2));
    var sx = [0, S, iw - S, iw], sy = [0, S, ih - S, ih], dx = [0, D, w - D, w], dy = [0, D, h - D, h];
    x.globalAlpha = Math.max(0.1, Math.min(1, +opacity || 1));
    for (var r = 0; r < 3; r++) for (var q = 0; q < 3; q++)
      if (sx[q + 1] > sx[q] && sy[r + 1] > sy[r] && dx[q + 1] > dx[q] && dy[r + 1] > dy[r])
        x.drawImage(im, sx[q], sy[r], sx[q + 1] - sx[q], sy[r + 1] - sy[r], dx[q], dy[r], dx[q + 1] - dx[q], dy[r + 1] - dy[r]);
    return c;
  }
  // a label's panel, ready to draw: its frame drawn, or the author's picture cut to it
  async function panelCanvas(d, name) {
    var L = layout(d)[name], b = L.inner || L.box, F = d.frames, c;
    if (L.frame && F.style === 'picture' && F.image) c = nine(await image(F.image), b[2], b[3], +F.corner || 10, F.opacity);
    else c = await rast(L.frame ? frameSvg(d, b, name === 'card') : svgDoc('0 0 ' + b[2] + ' ' + b[3], ''), b[2], b[3]);
    if (!L.shadow) return c;
    var sh = L.shadow, o = canvas(L.box[2], L.box[3]), x = o.getContext('2d');
    x.shadowColor = 'rgba(0,0,0,' + sh.opacity + ')'; x.shadowBlur = sh.blur * 2;
    x.shadowOffsetX = Math.round(sh.drop / 3); x.shadowOffsetY = sh.drop;
    x.drawImage(c, sh.reach, sh.reach);
    return o;
  }
  // one font: every CHARSET glyph in one cell, its ink from the cell's left edge on a shared
  // baseline (bake_theme.font's rule), in a colour or a top-to-bottom gradient
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
  async function font(face, px, colour, colour2) {
    var spec = px + 'px "' + face + '"', CS = fontSet();
    await loadFace(face, px, fontText(CS));
    var m = canvas(4, 4).getContext('2d');
    m.font = spec;
    var top = 0, bottom = 0, w = 1, boxes = CS.map(function (ch) { return boxOf(m, ch); });
    boxes.forEach(function (b) {
      if (b.r > b.l) { top = Math.max(top, b.t); bottom = Math.max(bottom, b.b); w = Math.max(w, b.r - b.l); }
      top = Math.max(top, Math.ceil(b.asc || 0)); bottom = Math.max(bottom, Math.ceil(b.desc || 0));
    });
    var h = top + bottom, glyphs = boxes.map(function (b, i) {
      var c = canvas(w, h), x = c.getContext('2d');
      x.font = spec; x.textBaseline = 'alphabetic';
      if (colour2) { var g = x.createLinearGradient(0, 0, 0, h); g.addColorStop(0, colour); g.addColorStop(1, colour2); x.fillStyle = g; }
      else x.fillStyle = colour;
      if (b.r > b.l) root.GorbBake.drawGlyph(x, CS[i], -b.l, top);
      return c;
    });
    return { w: w, h: h, glyphs: glyphs, space: Math.max(1, Math.round(m.measureText(' ').width)), track: 1 };
  }
  // the picker's picture (and the start sheet's): the plate, the arm and a few contacts
  var THUMB = [[3, 6, 70, -90, 220], [4, 5, -110, -20, 95], [0, 2, 40, 60, 330], [2, 4, -60, 100, 15], [5, 3, 110, 40, 120]];
  async function picture(plateC, armC, armPiv, frames, px, size, mapC) {
    var a = A(), c = canvas(a.glass, a.glass), x = c.getContext('2d');
    x.drawImage(plateC, 0, 0);
    if (mapC) x.drawImage(mapC, 0, 0);
    x.drawImage(armC, a.cx - armPiv[0], a.cx - armPiv[1]);
    THUMB.forEach(function (p) {
      x.save(); x.translate(a.cx + p[2], a.cx + p[3]); x.rotate(p[4] * Math.PI / 180);
      x.drawImage(frames[p[0] * 7 + p[1]], -px / 2, -px / 2); x.restore();
    });
    return size === a.glass ? c : shrink(c, size);
  }
  // a picture made smaller in halving steps, smoothed, so the 1-2 px lines of a map, a ring or a
  // word survive a thumbnail (one step from 466 px drops most of them)
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
  // a design's picture, drawing only the frames the picture shows
  async function thumb(d, size) {
    var px = planeBox(d), ar = arm(d), frames = [];
    var all = d.planes.family === 'custom' ? await planeFrames(d) : null;
    for (var si = 0; si < 7; si++) for (var bi = 0; bi < 7; bi++) {
      var used = THUMB.some(function (p) { return p[0] === si && p[1] === bi; });
      frames.push(all ? all[si * 7 + bi] : used ? shadowed(await rast(planeSvg(d, si, bi), planePx(d), planePx(d)), d) : null);
    }
    return picture(await plateCanvas(d), await rast(ar.svg, ar.size[0], ar.size[1]), ar.pivot, frames, px, size, await mapPicture(d));
  }
  // the map under the picture (flights-map-plan §6): maps@1's made-up place, as the orb draws
  // it at the third range, its names in the map's face
  async function mapPicture(d) {
    if (!d.map || !d.map.on || !root.GorbMaps) return null;
    try {
      var v = M().view(d.map, root.GorbMaps.sampleMap(), null, 80, 2), m = d.map;
      try { await document.fonts.load((m.nameSize | 0) + 'px "' + (m.nameFace || d.words.face) + '"'); } catch (e) { /* the fallback */ }
      var x = v.canvas.getContext('2d');
      x.globalAlpha = M().alphaOf(m, 'names'); x.fillStyle = m.ink.names;
      x.font = (m.nameSize | 0) + 'px "' + (m.nameFace || d.words.face) + '", sans-serif'; x.textAlign = 'center'; x.textBaseline = 'middle';
      v.names.forEach(function (n) { x.fillText(n.text, n.x, n.y); });
      return v.canvas;
    } catch (e) { return null; }
  }
  var toneStr = function (t) { return t.reduce(function (s, f, k) { return s + (f << (4 * k)); }, 0); };

  /* ---- the bake ---- */
  // the design baked to .orbtheme bytes: {bytes, meter, est, rows}. `design` is the
  // design document's bytes to carry (optional).
  async function bake(d, o) {
    o = o || {};
    var a = A(), GB = root.GorbBake, S = GB.stage3({ dither: true }), AUX = GB.classId('aux'), TEXT = GB.classId('text');
    var plateC = await plateCanvas(d);
    S.row({ n: S.id(), class: GB.classId('plate'), slot: 0, x: 0, y: 0, w: a.glass, h: a.glass, order: 0, alpha: 255, n_frames: 1 },
      plane(plateC), false, true);
    var ar = arm(d), armC = await rast(ar.svg, ar.size[0], ar.size[1]);
    S.row({ n: S.id(), class: AUX, slot: 0, x: a.cx - ar.pivot[0], y: a.cx - ar.pivot[1], w: ar.size[0], h: ar.size[1],
      pivot_x: ar.pivot[0], pivot_y: ar.pivot[1], order: d.arm.layer === 'over' ? 3 : 1, alpha: 255, n_frames: 1,
      src: GB.SOURCES.indexOf('second'), span: 3600, vmax: period(d), cadence: 1, cad_arg: SWEEP_FPS, end: 0 }, plane(armC), true, true);
    var px = planeBox(d), frames = await planeFrames(d);
    var rows = [];
    for (var i = 0; i < PLANE_ROWS; i++) rows.push({ slot: 1 + i, x: a.cx - (px >> 1), y: a.cx - (px >> 1), order: 2 });
    S.pool({ class: AUX, w: px, h: px, pivot_x: px >> 1, pivot_y: px >> 1, alpha: 255 }, frames.map(plane), rows, true, true);
    var dots = dotFrames(d);
    if (dots.length) {
      // by altitude: one pool of the 7 band frames the app picks from; one colour: nine still
      // rows of the one dot (a picked row needs two frames to pick between)
      var db = dotBox(d), dslots = DOT_SLOTS.map(function (slot) { return { slot: slot, x: a.cx - (db >> 1), y: a.cx - (db >> 1), order: 2 }; });
      if (dots.length > 1)
        S.pool({ class: AUX, w: db, h: db, pivot_x: db >> 1, pivot_y: db >> 1, alpha: 255 }, dots.map(plane), dslots, true, true);
      else for (var ds of dslots)
        S.row({ n: S.id(), class: AUX, slot: ds.slot, x: ds.x, y: ds.y, w: db, h: db, pivot_x: db >> 1, pivot_y: db >> 1,
          order: 2, alpha: 255, n_frames: 1 }, plane(dots[0]), true, true);
    }
    for (var mk of markers(d)) {
      var md = 2 * mk[1];
      S.row({ n: S.id(), class: AUX, slot: mk[0], x: a.cx - mk[1], y: a.cx - mk[1], w: md, h: md, pivot_x: mk[1], pivot_y: mk[1],
        order: 3, alpha: 255, n_frames: 1 }, plane(await rast(mk[2], md, md)), true, true);
    }
    var W = d.words, Z = sizes(d), Lay = layout(d);
    for (var fi = 0; fi < FONTS.length; fi++) {
      // a font is its glyphs' coverage, coloured on the orb (top, and the accent's foot);
      // tones of one size share one set of glyphs
      var tone = FONTS[fi], foot = tone === 'accent' ? W.accent2 : null, f = await font(W.face, Z[tone], '#FFFFFF', null);
      S.font({ n: S.id(), class: TEXT, slot: fi, x: 0, y: 0, w: f.w, h: f.h, order: 0,
        pivot_x: Math.min(f.space, f.w), pivot_y: f.track }, f.glyphs.map(plane), W[tone], foot);
    }
    var Mp = d.map && d.map.on ? d.map : null;
    if (Mp) {
      // the map's names (main.c names_draw): font 4, six labels over the ground and under the
      // arm, their opacity the map's; the values say what each range draws
      var nf = await font(Mp.nameFace || W.face, Mp.nameSize | 0 || 18, '#FFFFFF', null);
      S.font({ n: S.id(), class: TEXT, slot: FONTS.length, x: 0, y: 0, w: nf.w, h: nf.h, order: 0,
        pivot_x: Math.min(nf.space, nf.w), pivot_y: nf.track }, nf.glyphs.map(plane), Mp.ink.names, null);
      var nw = 240, nh = nf.h + 2, na = Math.round(255 * M().alphaOf(Mp, 'names'));
      for (var ni = 0; ni < M().N_NAMES; ni++)
        S.row({ n: S.id(), class: AUX, slot: M().NAME_SLOT0 + ni, kind: 2, src: GB.SOURCES.indexOf('text'), x: a.cx - (nw >> 1), y: a.cx - (nh >> 1),
          w: nw, h: nh, pivot_x: nw >> 1, pivot_y: nh >> 1, order: 1, alpha: Math.max(1, na), n_frames: 1, str: toneStr([FONTS.length, NONE, NONE, NONE]),
          radius: 1, digit: 1, end: 1 }, plane(canvas(nw, nh)), true, false);
    }
    // the scale's length label (fscale-2): its own font 6, from the theme's quiet words, on every
    // theme (map or no map); its values row is written next, unconditionally, at layout 2
    var sW = W.scale || {}, spx = Math.max(14, Math.min(40, sW.px == null ? Z.quiet : sW.px | 0));
    var sfc = await font(sW.face || W.face, spx, '#FFFFFF', null);
    S.font({ n: S.id(), class: TEXT, slot: SCALE_FONT, x: 0, y: 0, w: sfc.w, h: sfc.h, order: 0,
      pivot_x: Math.min(sfc.space, sfc.w), pivot_y: sfc.track }, sfc.glyphs.map(plane), sW.colour || W.quiet, null);
    // the values row always: the map's own, or an all-off blank; the scale region written on top
    S.values(scaleBytes(M().valuesBytes(Mp || M().preset('blank', W.face, W.size)), d), M().VALUES_VER);
    for (var lab of LABELS) {
      var L = Lay[lab.name], b = L.box, slot = lab.name === 'card' && L.clear ? CARD_CLEAR_SLOT : lab.slot;
      S.row({ n: S.id(), class: AUX, slot: slot, kind: 2, src: GB.SOURCES.indexOf('text'), x: b[0], y: b[1], w: b[2], h: b[3],
        pivot_x: b[2] >> 1, pivot_y: b[3] >> 1, order: 4, alpha: 255, n_frames: 1, str: toneStr(lab.tones), radius: L.pad,
        digit: 1, end: 1 }, plane(await panelCanvas(d, lab.name)), true, false);
    }
    var pf = await font(W.face, PILL_PX, '#FFFFFF', null), ph = pf.h + 2 + PILL_GROW;
    // the pill's words wear the theme's accent (fscale-3, §7.5): the scope green, with the
    // accent's own foot where a family has one, exactly as the accent font (slot 1) does
    S.font({ n: S.id(), class: TEXT, slot: PILL_FONT, x: 0, y: 0, w: pf.w, h: pf.h, order: 0,
      pivot_x: Math.min(pf.space, pf.w), pivot_y: pf.track }, pf.glyphs.map(plane), W.accent, W.accent2 || null);
    S.row({ n: S.id(), class: AUX, slot: PILL_SLOT, kind: 2, src: GB.SOURCES.indexOf('text'), x: a.cx - (PILL_W >> 1), y: a.cx - (ph >> 1),
      w: PILL_W, h: ph, pivot_x: PILL_W >> 1, pivot_y: ph >> 1, order: 4, alpha: 255, n_frames: 1, str: toneStr([PILL_FONT, NONE, NONE, NONE]),
      radius: 8, digit: 1, end: 1 }, plane(await pillCanvas(d, PILL_W, ph)), true, false);
    // §60: the whole glass, deflated by the baker and inflated by the orb (owner, 2026-11-07)
    S.preview({ plane: plane(await picture(plateC, armC, ar.pivot, frames, px, 466, await mapPicture(d))), x: 0, y: 0, order: 3 });
    // the bake time rides the header (word 10) so the hub can tell a newer copy from an older one;
    // it is not content, so the serial above stays the design's own
    var out = S.finish({ name: d.name || 'Flights', slug: 'flights', serial: o.serial || serialOf(d, o.design), abi: GB.GEOM_ABI6, design: o.design || null,
                         overlay: overlayWord(d),
                         bakedS: o.bakedS != null ? o.bakedS : Math.floor(Date.now() / 1000) });
    return { bytes: out.bytes, meter: out.meter, est: out.est, rows: S.elements.length };
  }

  // the bake's serial is its content (§12.9): the hub calls a card copy with the same name
  // and serial "already there, byte for byte" and sends nothing, so a constant serial made
  // every re-bake of a name look like the copy on the card. FNV-1a over the design document
  // (which carries every choice the bake reads) and the art release; never 0 or 1.
  function serialOf(d, doc) {
    var bytes = doc && doc.length ? doc : new TextEncoder().encode(JSON.stringify(d)), h = 0x811c9dc5;
    var mix = function (b) { h ^= b; h = Math.imul(h, 0x01000193) >>> 0; };
    for (var i = 0; i < bytes.length; i++) mix(bytes[i]);
    var tag = 'art:' + (A().version || '') + ':px:' + planePx(d);
    for (var k = 0; k < tag.length; k++) mix(tag.charCodeAt(k) & 255);
    return h > 1 ? h : h + 2;
  }

  // the values row this app reads: the map's 120 B (or an all-off blank), grown to 136 B and
  // the scale region (fscale-design's table, main.c fl_values_t layout 2) written over its tail
  function scaleBytes(base, d) {
    var b = new Uint8Array(136), dv = new DataView(b.buffer), W = d.words || {}, SW = W.scale || {},
        S = d.scale || {}, ring = !(d.layout && d.layout.scale === 'bar'), i;
    for (i = 0; i < 120; i++) b[i] = base[i];
    var lbl = SW.px == null ? sizes(d).quiet : SW.px | 0, al = function (h) {
      var n = parseInt(String(h || '#000000').slice(1), 16) >>> 0;
      return (((255 << 24) | n) >>> 0);
    };
    b[117] = ring ? 1 : 0;
    b[118] = Math.max(14, Math.min(40, lbl));
    b[119] = Math.max(2, Math.min(8, S.width == null ? 2 : S.width | 0));
    b[120] = Math.max(0, Math.min(24, S.tick == null ? 9 : S.tick | 0));
    b[121] = Math.max(60, Math.min(200, S.max == null ? 150 : S.max | 0));
    b[122] = 0; b[123] = 0;
    dv.setUint32(124, al(S.ink || '#4A7A62'), true);
    dv.setUint32(128, S.edge ? al(S.edge) : 0, true);
    dv.setUint32(132, al(SW.colour || W.quiet || '#8A9A90'), true);
    return b;
  }

  root.FlightsBake = { SCHEMA: SCHEMA, SHAPES: SHAPES, SHAPE_TITLES: SHAPE_TITLES, LABELS: LABELS, FACES: FACES, PRESETS: PRESETS, GROUNDS: GROUNDS,
    SCALE: SCALE, SCALE_FONT: SCALE_FONT, scaleBytes: scaleBytes,
    STARTS: STARTS, startInfo: startInfo,
    GROUND_TITLES: GROUND_TITLES, ARMS: ARMS, SCOPES: SCOPES, ARM_LOOKS: ARM_LOOKS, PERIOD_MIN: PERIOD_MIN, PERIOD_MAX: PERIOD_MAX,
    PLANE_ROWS: PLANE_ROWS, SWEEP_FPS: SWEEP_FPS, PILL_SLOT: PILL_SLOT, pillCanvas: pillCanvas, CARD_SLOT: CARD_SLOT, CARD_CLEAR_SLOT: CARD_CLEAR_SLOT,
    DOT_SLOTS: DOT_SLOTS, DOTS: DOTS, dotFrames: dotFrames, dotBox: dotBox,
    FRAME_STYLES: FRAME_STYLES, MARKS: MARKS, HEADINGS: HEADINGS, serialOf: serialOf, GRID_W: GRID_W,
    preset: preset, upgrade1: upgrade1, complete: complete, pictureFit: pictureFit, panelCanvas: panelCanvas, planeBox: planeBox, markers: markers, scopeOf: scopeOf, armOf: armOf, bake: bake, thumb: thumb, period: period, overlayOf: overlayOf, overlayWord: overlayWord,
    // the preview draws from the same parts
    planeSvg: planeSvg, planeFrames: planeFrames, planePx: planePx, arm: arm, markerSvg: markerSvg, panelSvg: panelSvg, layout: layout, sizes: sizes,
    plateCanvas: plateCanvas, rast: rast, font: font, svgImage: svgImage, image: image, shade: shade,
  };
})(typeof window !== 'undefined' ? window : globalThis);
