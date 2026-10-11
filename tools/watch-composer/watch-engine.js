// Layered, fully procedural watch-face engine (466×466), built on DialRenderer's materials.
  (() => {
    const P0 = CanvasRenderingContext2D.prototype; if (P0.__wcSafe) return; P0.__wcSafe = 1;
    const rg = P0.createRadialGradient, ar = P0.arc, pa = Path2D.prototype.arc, nn = (v) => (v > 0 ? v : 0);
    P0.createRadialGradient = function (x0, y0, r0, x1, y1, r1) { return rg.call(this, x0, y0, nn(r0), x1, y1, nn(r1)); };
    P0.arc = function (x, y, r, a0, a1, ccw) { return ar.call(this, x, y, nn(r), a0, a1, ccw); };
    Path2D.prototype.arc = function (x, y, r, a0, a1, ccw) { return pa.call(this, x, y, nn(r), a0, a1, ccw); };
  })();
// Stack bottom→top: static movement plate · geartrain (both lit by the inner light) · discs under dial · dial · discs under elevated dials ·
// elevated dials · hands · glass · case & outer light. A design = chapter ring + an ordered list of complications;
// the layout it produces decides where every hand sits and how long it is.
(function () {
  const S = 466, C = 233, DR = 221, WIN = 160, TAU = Math.PI * 2, OFF = 3000;
  const SERIF0 = '"Cormorant Garamond", Georgia, serif', SANS0 = '"IBM Plex Sans", "Helvetica Neue", Arial, sans-serif';
  let SERIF = SERIF0, SANS = SANS0;
  const FONTS = {
    auto: ['Ring default'], garamond: ['Garamond', '"Cormorant Garamond", Georgia, serif'], bodoni: ['Bodoni', '"Bodoni Moda", Didot, serif'],
    plex: ['Plex Sans', SANS0], jost: ['Geometric (Jost)', '"Jost", Futura, sans-serif'], barlow: ['Condensed (Barlow)', '"Barlow Condensed", "Arial Narrow", sans-serif'],
    playfair: ['Playfair Display', '"Playfair Display", Georgia, serif'], cinzel: ['Cinzel (inscription)', '"Cinzel", Georgia, serif'], abril: ['Abril Fatface', '"Abril Fatface", Georgia, serif'], dmserif: ['DM Serif Display', '"DM Serif Display", Georgia, serif'],
    blackletter: ['Blackletter (Unifraktur)', '"UnifrakturMaguntia", serif'], oswald: ['Oswald', '"Oswald", "Arial Narrow", sans-serif'], bebas: ['Bebas Neue', '"Bebas Neue", "Arial Narrow", sans-serif'],
    grotesk: ['Space Grotesk', '"Space Grotesk", sans-serif'], spacemono: ['Space Mono', '"Space Mono", monospace'], michroma: ['Michroma (wide)', '"Michroma", sans-serif'], orbitron: ['Orbitron', '"Orbitron", sans-serif'],
    audiowide: ['Audiowide', '"Audiowide", sans-serif'], syne: ['Syne', '"Syne", sans-serif'], unbounded: ['Unbounded', '"Unbounded", sans-serif'],
    bungee: ['Funky · Bungee', '"Bungee", sans-serif'], bungeeshade: ['Funky · Bungee Shade', '"Bungee Shade", sans-serif'], monoton: ['Funky · Monoton', '"Monoton", sans-serif'], righteous: ['Funky · Righteous', '"Righteous", sans-serif'],
    rubikmono: ['Funky · Rubik Mono', '"Rubik Mono One", sans-serif'], majormono: ['Funky · Major Mono', '"Major Mono Display", monospace'], glitch: ['Funky · Rubik Glitch', '"Rubik Glitch", sans-serif'], rampart: ['Funky · Rampart', '"Rampart One", sans-serif'],
    shrikhand: ['Funky · Shrikhand', '"Shrikhand", serif'], lobster: ['Funky · Lobster', '"Lobster", cursive'], pacifico: ['Funky · Pacifico', '"Pacifico", cursive'], creepster: ['Funky · Creepster', '"Creepster", cursive'],
    blackops: ['Funky · Black Ops', '"Black Ops One", sans-serif'], silkscreen: ['Pixel · Silkscreen', '"Silkscreen", monospace'], pressstart: ['Pixel · Press Start', '"Press Start 2P", monospace'], vt323: ['Pixel · VT323', '"VT323", monospace'],
  };
  const FONT_NAME = (k) => { const f = FONTS[k] && FONTS[k][1], m = f && f.match(/"([^"]+)"/); return m ? m[1] : null; };
  const TYPEFACES = Object.fromEntries(Object.entries(FONTS).filter(([k]) => k !== 'auto').map(([k, v]) => [k, v[0]]));
  function setFace(k) { const f = FONTS[k] && FONTS[k][1]; SERIF = f || SERIF0; SANS = f || SANS0; }
  const DAYS = ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'];
  const MONTHS = ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC'];
  const ROMAN = ['XII', 'I', 'II', 'III', 'IIII', 'V', 'VI', 'VII', 'VIII', 'IX', 'X', 'XI'];
  const L = () => window.DialRenderer._;
  const cv = (w, h) => { const c = document.createElement('canvas'); c.width = Math.ceil(w); c.height = Math.ceil(h == null ? w : h); return c; };
  const P = (x, y, r, a) => [x + Math.cos(a) * r, y + Math.sin(a) * r];
  const rad = (d) => (d * Math.PI) / 180;
  const rotAbout = (x, y, a) => { const c = Math.cos(a), s = Math.sin(a), dx = x - C, dy = y - C; return [C + dx * c - dy * s, C + dx * s + dy * c]; };
  // inner = axis of the warm sources under the dial ring; outer = direction the room light comes from (canvas radians)
  const LIGHT = { inner: 0, outer: rad(225), outerK: 1 };
  const OUTER0 = rad(225);
  function spun(src, a) { if (!a) return src; const c = cv(S), x = c.getContext('2d'); x.translate(C, C); x.rotate(a); x.translate(-C, -C); x.drawImage(src, 0, 0); x.setTransform(1, 0, 0, 1, 0, 0); return c; }
  const away = (m, L0) => { const a = (L0 || LIGHT).outer + Math.PI; return [Math.cos(a) * m, Math.sin(a) * m]; };
  const moonPhase = (d) => d && d.__moon != null ? d.__moon : ((((d.getTime() - Date.UTC(2000, 0, 6, 18, 14)) / 864e5 / 29.530588853) % 1) + 1) % 1;
  // preview helper: every calendar complication steps once per n (date, weekday, month, moon), time stays live
  function cycleDate(base, n) {
    const d = new Date(base.getTime()), dd = (n % 31) + 1, wd = n % 7, mo = n % 12;
    d.getDate = () => dd; d.getDay = () => wd; d.getMonth = () => mo; d.__moon = (n % 30) / 30; d.toDateString = () => 'cycle-' + n;
    return d;
  }
  // Kit preview pose: the marketing look every shipped theme is judged by. Hands at
  // 10:09:36 (both needles off the 12, mirrors reading up), and the moon held at a
  // waxing gibbous -- phase 0.34 lights ~77% of the disc with the terminator clear of
  // both the new-moon empty frame and the full-moon coin, so a shipped preview.png
  // always shows a moon rather than a bare sky.
  // toDateString is overridden on purpose: F.comps and F.dateFlares cache their canvas
  // by date.toDateString(), so a pose that kept the real string would be served the
  // already-cached live-date canvas (tonight's real phase) instead of this one.
  const PREVIEW_MOON = 0.34;
  function previewDate(base) {
    const d = new Date(base ? base.getTime() : Date.now());
    d.setHours(10, 9, 36, 0); d.__moon = PREVIEW_MOON; d.toDateString = () => 'preview-1009';
    return d;
  }
  const TURN = {
    hour: (t) => ((t.getHours() % 12) + t.getMinutes() / 60 + t.getSeconds() / 3600) / 12,
    minute: (t) => (t.getMinutes() + t.getSeconds() / 60) / 60,
    second: (t) => (t.getSeconds() + t.getMilliseconds() / 1000) / 60,
    date: (t) => (t.getDate() - 1) / 31,
    day: (t) => t.getDay() / 7,
    month: (t) => (t.getMonth() + (t.getDate() - 1) / 31) / 12,
    hours24: (t) => (t.getHours() + t.getMinutes() / 60 + t.getSeconds() / 3600) / 24,
  };
  // A GMT hand is a second hour hand wound to another zone: the same 24-hour
  // revolution, offset in WHOLE hours so it still lands squarely on a bezel hour.
  const gmtTurn = (off) => { const o = Math.round(+off || 0); return (t) => ((((t.getHours() + o) % 24) + 24) % 24 + t.getMinutes() / 60 + t.getSeconds() / 3600) / 24; };
  const SUBKIND = { seconds: 'second', date: 'date', day: 'day', month: 'month', hours: 'hour' };
  // Centre-pointer kinds that read a printed perimeter scale, in stack order (lowest first).
  const CAL_HANDS = ['hours24', 'gmt', 'day', 'date'];
  const PERIMS = { none: 'No perimeter', hours24: '24 hours', date: 'Date 1–31', day: 'Day of week' };
  // One spec per perimeter scale: splits, which are emphasised, their label, and the tick
  // geometry. Index 0 always sits at 12 o'clock and runs clockwise, matching TURN above.
  const PERIM_SPEC = {
    hours24: { n: 24, maj: (i) => i % 6 === 0, lab: (i) => String(i || 24), fs: 9.5, tw: 2.4, qw: 0.9, tl: 9 },
    date: { n: 31, maj: (i) => (i + 1) % 5 === 0 || i === 30, lab: (i) => String(i + 1), fs: 9, tw: 2.2, qw: 0.8, tl: 8 },
    day: { n: 7, maj: () => true, lab: (i) => DAYS[i], fs: 11, tw: 3, qw: 3, tl: 10 },
  };
  // A dial carries ONE additional centre pointer besides hour/minute/second. Its role is the
  // single choice; the printed ring is derived from it (EXTRA_RING), never chosen separately,
  // so a ring can never disagree with the hand that reads it.
  const EXTRA_HANDS = { hours24: '24-hour', gmt: 'GMT · second time zone', day: 'Day of week', date: 'Day of month' };
  const EXTRA_RING = { hours24: 'hours24', gmt: 'hours24', day: 'day', date: 'date' };
  const PERIM_KEYS = ['perim', 'perimShrink', 'perimColor', 'perimFont', 'perimOrient', 'perimSize', 'perimPos', 'perimRot'];
  // The panel this dial ships on is a 466x466 CO5300 AMOLED 1.75 in across, so one engine px
  // is 0.095 mm on the wrist. A preview drawn 2.7x the real dial flatters small type, and that
  // is how a dial gets authored with day names nobody can read at arm's length. The composer
  // therefore quotes print sizes in millimetres and can draw the dial at true size.
  const PANEL = { px: S, inch: 1.75, cssInch: 96 };
  const MM_PER_PX = (PANEL.inch * 25.4) / PANEL.px;
  const pxMm = (px) => px * MM_PER_PX;
  // CSS px the whole dial occupies at true size, and the zoom factor that means (1.0 = today's
  // 466 px stage). 1 CSS in is nominally 96 px, but the page cannot measure the screen it lands
  // on: Chrome draws 1 CSS px as 1 device px at 100% zoom, so the same 168 px is 1.75 in on a
  // 96 ppi monitor and 0.9 in on a 185 ppi one, and browser zoom shifts it again. True size
  // therefore carries a user calibration, and the composer draws a ruler of RULER_MM the user
  // matches against a real one. The claimed size stays 1.75 in at any calibration — what the
  // calibration decides is whether the CSS px chosen actually realise it on this screen.
  const REAL_CSS = Math.round(PANEL.inch * PANEL.cssInch);
  const REAL_Z = REAL_CSS / PANEL.px;
  const RULER_MM = 40;
  const realCss = (cal) => Math.max(60, Math.round(REAL_CSS * (cal > 0 ? cal : 1)));
  const rulerPx = (cal) => Math.round((RULER_MM / 25.4) * PANEL.cssInch * (cal > 0 ? cal : 1));
  const screenInch = (css, cal) => css / (PANEL.cssInch * (cal > 0 ? cal : 1));
  // Ring rotation: which label of a turned ring sits nearest 12, and the turn that puts a
  // chosen division there. Both read the same spec the print does, so the control row and the
  // dial can never disagree about where the first day of the week or month starts.
  const perimRotDeg = (D) => ((((+D.perimRot || 0) % 360) + 360) % 360);
  const perimRotAt = (spec, i, between) => perimRotDeg({ perimRot: -(((i + (between ? 0.5 : 0)) / spec.n) * 360) });
  function ringTop(spec, rot, between) {
    let best = 0, bd = 1e9;
    for (let i = 0; i < spec.n; i++) {
      const a = (((i + (between ? 0.5 : 0)) / spec.n) * 360 + rot + 180) % 360 - 180;
      if (Math.abs(a) < bd) { bd = Math.abs(a); best = i; }
    }
    return { i: best, lab: spec.lab(best), off: +bd.toFixed(2) };
  }
  // The rotator's preset chips: every division of a small ring (the seven days), the quarters
  // of a big one. 31 chips for the date ring would be a wall, not a control.
  function ringStarts(spec, between) {
    const idx = spec.n <= 12 ? Array.from({ length: spec.n }, (_, i) => i) : [0, Math.round(spec.n / 4), Math.round(spec.n / 2), Math.round((3 * spec.n) / 4)];
    return idx.map((i) => ({ v: +perimRotAt(spec, i, between).toFixed(2), label: spec.lab(i) + ' at 12' }));
  }

  const SEC_STYLES = { classic: 'Classic counterweight', lollipop: 'Lollipop', ring: 'Open ring', arrowtip: 'Arrow tip', paddle: 'Lume paddle', bolt: 'Lightning', crescent: 'Crescent tail', star: 'Star tail', needle: 'Long needle', station: 'Station · disc at tip', flieger: 'Flieger · three lume discs' };
  const HAND_STYLES = { station: 'Station bar', aviator: 'Aviator sword', cathedral: 'Cathedral', poire: 'Poire', fleur: 'Fleur-de-lis', serpentine: 'Serpentine', louis: 'Louis XV', breguet: 'Breguet', spade: 'Spade', leaf: 'Leaf', baton: 'Baton', sword: 'Sword', dauphine: 'Dauphine', pencil: 'Pencil', skeleton: 'Skeleton', arrow: 'Arrow', broad: 'Broad arrow', crescent: 'Crescent tip', diamond: 'Diamond tip', sunburst: 'Sunburst tip', pierced: 'Pierced ring', pennant: 'Pennant' };
  const HAND_FINISHES = { blued: 'Blued', gilt: 'Gilt', black: 'Black', steel: 'Steel', white: 'White', orange: 'Orange', red: 'Red', magenta: 'Magenta', yellow: 'Yellow', lime: 'Lime', mint: 'Mint', cyan: 'Cyan', violet: 'Violet' };
  const DENSITY = { all: () => true, even: (h) => h % 2 === 0, quarters: (h) => h % 3 === 0, twelve: (h) => h === 0, none: () => false };
  const NUMERALS = { all: 'All twelve', even: 'Even hours', quarters: 'Quarters', twelve: '12 only', none: 'None' };
  const OPENWORK = { none: 'Solid dial', x: 'X bridges', xarch: 'X bridges + arch', cross: 'Cross', star: 'Six spokes', ring: 'Chapter ring only' };
  const SURFACES = { enamel: 'Enamel', guilloche: 'Sunburst', azure: 'Azurage', matte: 'Grained', metal: 'Silvered metal', carbon: 'Carbon · woven', forged: 'Carbon · forged' };
  const DIAL_COLORS = {
    cream: ['#efe5cb', '#c9b388'], porcelain: ['#f8f4ea', '#e0d9c8'], ivory: ['#f3ead6', '#d8c9a6'], white: ['#fbfaf7', '#e2e0da'], salmon: ['#efc2a4', '#c98b6a'], sky: ['#c9dcea', '#8eaac2'],
    slate: ['#5a636d', '#2b3036'], navy: ['#2a3f66', '#0f1a2e'], forest: ['#2f5243', '#122219'], burgundy: ['#6b2430', '#2c0b11'], charcoal: ['#2c2823', '#0f0d0a'], black: ['#26241f', '#0c0b09'],
    magenta: ['#ff2a9d', '#a10c5c'], bubblegum: ['#ff9fd2', '#e0559c'], red: ['#ff3b3b', '#a3121a'], tangerine: ['#ff8a1f', '#c24a00'], sunflower: ['#ffd12a', '#d08f00'], acid: ['#e4ff3a', '#93b300'],
    lime: ['#7dff4a', '#2f9e12'], mint: ['#7ff5c8', '#22b383'], turquoise: ['#1fe0c4', '#06807a'], electric: ['#2f7bff', '#0a2fb0'], violet: ['#9a4dff', '#4214a8'], ultraviolet: ['#4b17d6', '#14054a'],
  };
  const COLOR_GROUPS = { classic: ['cream', 'porcelain', 'ivory', 'white', 'salmon', 'sky', 'slate', 'navy', 'forest', 'burgundy', 'charcoal', 'black'], vivid: ['magenta', 'bubblegum', 'red', 'tangerine', 'sunflower', 'acid', 'lime', 'mint', 'turquoise', 'electric', 'violet', 'ultraviolet'] };
  const COLOR_NAMES = Object.fromEntries(Object.keys(DIAL_COLORS).map((k) => [k, k[0].toUpperCase() + k.slice(1)]));
  const NO_TINT = ['cream', 'porcelain', 'ivory', 'white', 'charcoal', 'black'];
  const isHex = (v) => typeof v === 'string' && /^#[0-9a-f]{6}$/i.test(v);
  const shade = (hex, k) => '#' + [1, 3, 5].map((i) => Math.max(0, Math.min(255, Math.round(parseInt(hex.slice(i, i + 2), 16) * k))).toString(16).padStart(2, '0')).join('');
  function colorEn(base, key) {
    if (isHex(key)) { const n = parseInt(key.slice(1), 16), lum = (0.299 * (n >> 16) + 0.587 * ((n >> 8) & 255) + 0.114 * (n & 255)) / 255, dark = lum < 0.5, sat = Math.max(n >> 16, (n >> 8) & 255, n & 255) - Math.min(n >> 16, (n >> 8) & 255, n & 255);
      return { a: key, b: shade(key, dark ? 0.45 : 0.8), ink: dark ? '#f6f3ec' : '#1a140c', stain: 0, dark: dark ? 1 : 0, tint: sat < 18 ? null : key }; }
    if (!DIAL_COLORS[key]) key = 'cream';
    const c = DIAL_COLORS[key], n = parseInt(c[0].slice(1), 16), lum = (0.299 * (n >> 16) + 0.587 * ((n >> 8) & 255) + 0.114 * (n & 255)) / 255, dark = lum < 0.5;
    return { a: c[0], b: c[1], ink: dark ? (NO_TINT.includes(key) ? '#efe4c8' : '#f6f3ec') : '#1a140c', stain: 0, dark: dark ? 1 : 0, tint: NO_TINT.includes(key) ? null : c[0] };
  }
  const RING_FONT = (print) => (['roman', 'arabic', 'regulator'].includes(print) ? 'garamond' : 'plex');
  const HAS_NUMS = ['roman', 'arabic', 'regulator', 'sans', 'railway', 'sector', 'pilot'];
  const BEZEL = { mariner: 1, tachy: 1 }, BEZEL_IN = 193.5;
  const ROUND_MOON = { sphere: 1, graphic: 1, neon: 1 };
  const PLATES = { train: 'Movement default', classic: 'Warm gilt', anthracite: 'Anthracite graining', stripes: 'Geneva stripes', sandblast: 'Sandblasted', carbon: 'Carbon · woven', forged: 'Carbon · forged' };
  const tickTurn = (n) => (!n ? TURN.second : (t) => Math.floor((t.getSeconds() + t.getMilliseconds() / 1000) * n) / n / 60);
  const SHAPES = { circle: 'Circle', square: 'Square', hex: 'Hexagon', oct: 'Octagon', slot: 'Slot', arch: 'Arch' };
  const COMP_TYPES = { opening: 'Opening', subdial: 'Subdial', window: 'Window', moon: 'Moon phase', flare: 'Flare' };
  const FLARE_ICONS = { star: 'Star', spark: 'Spark', bolt: 'Bolt', crown: 'Crown', heart: 'Heart', shield: 'Shield', diamond: 'Diamond', cross: 'Cross', ring: 'Ring', chevron: 'Chevron', triangle: 'Triangle', flame: 'Flame', eye: 'Eye', chevL: 'Chevron left', chevR: 'Chevron right', plus: 'Plus (slim)', arrowL: 'Arrow left', arrowR: 'Arrow right', rect: 'Rectangle', pill: 'Pill', dot: 'Dot', datetext: 'Date (custom format)', text: 'Text logo', arctext: 'Curved text', image: 'Image (upload)' };
  const FLARE_SVG = {
    chevL: 'M20 2h-6.5L4 12l9.5 10H20l-9.5-10z', chevR: 'M4 2h6.5l9.5 10-9.5 10H4l9.5-10z', plus: 'M10.4 3h3.2v7.4H21v3.2h-7.4V21h-3.2v-7.4H3v-3.2h7.4z',
    arrowL: 'M22 9.8H9.5V4.5L2 12l7.5 7.5v-5.3H22z', arrowR: 'M2 9.8h12.5V4.5L22 12l-7.5 7.5v-5.3H2z', rect: 'M1 6.5h22v11H1z', pill: 'M7.5 6.5h9a5.5 5.5 0 0 1 0 11h-9a5.5 5.5 0 0 1 0-11z', dot: 'M12 3a9 9 0 1 0 0 18a9 9 0 1 0 0-18z',
    star: 'M12 2l2.9 6.6 7.1.6-5.4 4.7 1.6 7L12 17.3 5.8 20.9l1.6-7L2 9.2l7.1-.6z', spark: 'M12 1c1 6 5 10 11 11-6 1-10 5-11 11-1-6-5-10-11-11 6-1 10-5 11-11z',
    bolt: 'M13.5 1L4 13.5h6.5L9 23l10-13h-6.5z', crown: 'M3 19h18l1.2-12-5.7 4.5L12 3.5l-4.5 8L1.8 7z', heart: 'M12 21.5s-9-5.6-9-12a5 5 0 0 1 9-3 5 5 0 0 1 9 3c0 6.4-9 12-9 12z',
    shield: 'M12 1.5l9 3.4v6.4c0 5.5-3.9 9.8-9 11.7-5.1-1.9-9-6.2-9-11.7V4.9z', diamond: 'M12 1.5l8.5 10.5L12 22.5 3.5 12z', cross: 'M9.5 2h5v7.5H22v5h-7.5V22h-5v-7.5H2v-5h7.5z',
    ring: 'M12 2.5a9.5 9.5 0 1 0 0 19a9.5 9.5 0 1 0 0-19zm0 3.5a6 6 0 1 1 0 12a6 6 0 1 1 0-12z', chevron: 'M2 5l10 7 10-7v5.5l-10 7-10-7z', triangle: 'M2 3.5h20L12 21.5z',
    flame: 'M12 1c1.2 4.4 7 6.6 7 13.3A7 7 0 0 1 5 14.3c0-3.3 2.2-5.6 3.4-7.7 0 2.3 1.1 3.4 2.3 3.4C10.7 6.6 9.8 4.3 12 1z', eye: 'M12 4.5C5.5 4.5 1 12 1 12s4.5 7.5 11 7.5S23 12 23 12s-4.5-7.5-11-7.5zm0 3.7a3.8 3.8 0 1 1 0 7.6a3.8 3.8 0 1 1 0-7.6z',
  };
  let FLARE_P = null;
  // ---------------------------------------------------------------------------
  // Uploaded art. An asset record holds the BYTES plus everything a re-render
  // needs, so a design can be rebuilt from a bundle alone and the art never has
  // to be re-supplied by hand: `dataUrl` is the source of record, `el` is only a
  // session cache of the decoded image, `sha` is the identity the Design ID and
  // the kit manifest reference it by. IMAGES stays as the element view the flare
  // path already reads, so the existing code keeps working.
  // ---------------------------------------------------------------------------
  const ASSETS = new Map();
  const IMAGES = {};
  const INVISIBLE_ALPHA = 6;        // == prep_watch_kit.py INVISIBLE: one rule for "no ink"
  const MAX_ASSET_BYTES = 1.5e6;    // localStorage is per-origin and small; refuse big files at ingest
  const ASSET_TOTAL_BYTES = 4e6;    // warn past this much art held in the browser
  // FNV-1a-32 over the bytes: an identity for dedupe and for spotting a stale
  // cache, NOT a security claim. The kit manifest asks for a real sha256 too
  // (sha256Of below) and records honestly when the browser will not give one.
  const sha32 = (u8) => { let h = 0x811c9dc5; for (let i = 0; i < u8.length; i++) { h ^= u8[i]; h = (h + (h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24)) >>> 0; } return ('0000000' + h.toString(16)).slice(-8); };
  const bytesOf = (dataUrl) => { const b = atob(String(dataUrl).slice(String(dataUrl).indexOf(',') + 1)), u = new Uint8Array(b.length); for (let i = 0; i < b.length; i++) u[i] = b.charCodeAt(i); return u; };
  const textOf = (dataUrl) => { try { return new TextDecoder().decode(bytesOf(dataUrl)); } catch (e) { return ''; } };
  /* SubtleCrypto is absent outside a secure context, file:// included, so the
   * 64-hex digest is best effort. null means "the browser would not", which the
   * manifest records rather than invents. */
  function sha256Of(u8) {
    if (!window.crypto || !window.crypto.subtle || !window.crypto.subtle.digest) return Promise.resolve(null);
    return window.crypto.subtle.digest('SHA-256', u8).then((buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('')).catch(() => null);
  }
  /* A viewBox-only SVG carries no intrinsic size, so the browser's fallback
   * (300x150 in Chrome) silently decides how drawImage scales it: the art lands
   * blurry or gone. Stamp explicit width/height off the viewBox before decode. */
  const isSvgUrl = (u) => /^data:image\/svg/i.test(String(u));
  function svgStamp(dataUrl) {
    const t = textOf(dataUrl);
    const vb = t.match(/viewBox\s*=\s*["']\s*([-\d.eE+]+)[\s,]+([-\d.eE+]+)[\s,]+([-\d.eE+]+)[\s,]+([-\d.eE+]+)\s*["']/);
    if (!vb) return dataUrl;
    const w = +vb[3], h = +vb[4];
    if (!(w > 0 && h > 0)) return dataUrl;
    const tag = t.match(/<svg\b[^>]*>/i);
    if (!tag) return dataUrl;
    let open = tag[0];
    open = /\bwidth\s*=/.test(open) ? open.replace(/\bwidth\s*=\s*["'][^"']*["']/i, 'width="' + w + '"') : open.replace(/<svg/i, '<svg width="' + w + '"');
    open = /\bheight\s*=/.test(open) ? open.replace(/\bheight\s*=\s*["'][^"']*["']/i, 'height="' + h + '"') : open.replace(/<svg/i, '<svg height="' + h + '"');
    const fixed = t.replace(tag[0], open);
    return 'data:image/svg+xml;base64,' + btoa(unescape(encodeURIComponent(fixed)));
  }
  function assetPut(rec) {
    if (!rec || !rec.id || !rec.dataUrl) return null;
    const a = { id: rec.id, name: rec.name || rec.id, mime: rec.mime || (isSvgUrl(rec.dataUrl) ? 'image/svg+xml' : 'image/png'),
      kind: rec.kind || (isSvgUrl(rec.dataUrl) ? 'svg' : 'png'), dataUrl: rec.dataUrl, sha: rec.sha || null,
      w: rec.w || 0, h: rec.h || 0, el: null, missing: false };
    if (!a.sha) { try { a.sha = sha32(bytesOf(a.dataUrl)); } catch (e) { a.sha = 'unread'; } }
    ASSETS.set(a.id, a); return a;
  }
  /* Decode, and for a bitmap drop texels the eye cannot weigh so they cost the
   * device nothing and so prep_watch_kit's fold proof sees the same alpha the
   * bake does. Re-encoding happens once, here -- never per draw. */
  function assetLoad(a, done) {
    const im = new Image();
    im.onload = () => {
      a.missing = false;
      if (a.kind === 'svg') { a.el = im; IMAGES[a.id] = im; a.w = a.w || im.naturalWidth || im.width || 0; a.h = a.h || im.naturalHeight || im.height || 0; if (typeof window.__assetDirty === 'function') window.__assetDirty(a.id); done(a); return; }
      a.w = im.naturalWidth || im.width || 0; a.h = im.naturalHeight || im.height || 0;
      if (!a.w || !a.h) { a.el = im; IMAGES[a.id] = im; done(a); return; }
      let cleaned = a.dataUrl;
      try {
        const c = cv(a.w, a.h), x = c.getContext('2d');
        x.drawImage(im, 0, 0, a.w, a.h);
        const d = x.getImageData(0, 0, a.w, a.h), p = d.data; let faint = 0;
        for (let i = 3; i < p.length; i += 4) if (p[i] > 0 && p[i] < INVISIBLE_ALPHA) { p[i] = 0; faint++; }
        if (faint) { x.putImageData(d, 0, 0); cleaned = c.toDataURL('image/png'); }
      } catch (e) { /* a weird source can taint the canvas: keep the original bytes */ }
      const finish = (el) => { a.el = el; IMAGES[a.id] = el; if (typeof window.__assetDirty === 'function') window.__assetDirty(a.id); done(a); };
      if (cleaned === a.dataUrl) { finish(im); return; }
      a.dataUrl = cleaned; try { a.sha = sha32(bytesOf(cleaned)); } catch (e) { }
      const im2 = new Image();
      im2.onload = () => finish(im2); im2.onerror = () => finish(im); im2.src = cleaned;
    };
    im.onerror = () => { a.missing = true; a.el = null; IMAGES[a.id] = null; done(a); };
    im.src = a.dataUrl;
  }
  /* The one front door for an upload: read, normalise, decode, report. A failure
   * carries the measured numbers, because "your art is too small" only helps if
   * it says how small and how big it needed to be. */
  function assetFromFile(file) {
    return new Promise((res) => {
      if (!file) { res({ error: 'No file chosen' }); return; }
      const mime = String(file.type || '');
      const png = mime === 'image/png', svg = mime === 'image/svg+xml';
      if (!png && !svg) { res({ error: 'Accepted types are image/png and image/svg+xml (got ' + (mime || 'unknown') + ')' }); return; }
      if (file.size > MAX_ASSET_BYTES) { res({ error: file.name + ' is ' + (file.size / 1024).toFixed(0) + ' KB; the limit is ' + (MAX_ASSET_BYTES / 1e6).toFixed(1) + ' MB' }); return; }
      const rd = new FileReader();
      rd.onerror = () => res({ error: 'That file could not be read' });
      rd.onload = () => {
        let url = String(rd.result);
        if (svg) url = svgStamp(url);
        let sha = 'unread'; try { sha = sha32(bytesOf(url)); } catch (e) { }
        const stem = (file.name.replace(/\.[a-z0-9]+$/i, '').replace(/[^\w.-]+/g, '-') || 'art').slice(0, 40);
        const a = assetPut({ id: stem + '-' + sha.slice(0, 8), name: file.name, mime, dataUrl: url, sha });
        assetLoad(a, () => a.missing ? res({ error: (svg ? 'That SVG could not be decoded' : 'That PNG could not be decoded') + ': ' + file.name }) : res(a));
      };
      rd.readAsDataURL(file);
    });
  }
  const assetGet = (id) => (id ? ASSETS.get(id) || null : null);
  /* Ready means drawable in this session: bytes present AND decoded. A design
   * restored from a bare Design ID holds references with no bytes, and the UI has
   * to be able to say so instead of quietly drawing nothing. */
  const assetReady = (id) => { const a = assetGet(id); return !!(a && a.el && !a.missing); };
  const assetKnown = (id) => !!assetGet(id);
  const assetCount = () => ASSETS.size;
  function assetBytes() { let n = 0; ASSETS.forEach((a) => { n += a.dataUrl.length; }); return n; }
  /* Refuse a bitmap that would be resampled UP into its box: the device rotates
   * with antialiasing off (clock_view.c lv_image_set_antialias false), so that
   * loss is plainly visible and stays the author's call, not a silent default. */
  /* Refuse a bitmap that would be resampled UP into its box: the device rotates with
   * antialiasing off (clock_view.c lv_image_set_antialias false), so that loss is plainly
   * visible and stays the author's call, not a silent default. The scale that matters is
   * the one the chosen fit will actually apply -- cover fills and so magnifies the short
   * axis, contain fits and magnifies nothing, stretch ignores the aspect on each axis. */
  function assetFits(id, w, h, fit, zoom) {
    const a = assetGet(id);
    if (!a) return { ok: false, why: 'asset ' + id + ' is not in this browser — load its .wckit bundle' };
    const z = zoom == null ? 1 : Math.max(0.05, +zoom || 1);
    if (a.kind === 'svg') return { ok: true, vector: true, srcW: a.w, srcH: a.h, drawW: w * z, drawH: h * z };
    w = w * z; h = h * z;                        // the zoomed box is the box that gets drawn, so it is the box that gets priced
    const iw = a.w || 1, ih = a.h || 1;
    const f = fit || 'cover';
    const k = f === 'contain' ? Math.min(w / iw, h / ih) : f === 'stretch' ? Math.max(w / iw, h / ih) : Math.max(w / iw, h / ih);
    if (k > 1.02) return { ok: false, upscale: true, fit: f, srcW: a.w, srcH: a.h, drawW: w, drawH: h, scale: +k.toFixed(3),
      why: a.name + ' is ' + a.w + '×' + a.h + ' but ' + f + '-fills a ' + Math.round(w) + '×' + Math.round(h) + ' box at ' + k.toFixed(2) + '× — re-export the art at or above ' + Math.ceil(iw * k) + '×' + Math.ceil(ih * k) };
    return { ok: true, fit: f, srcW: a.w, srcH: a.h, drawW: w, drawH: h, scale: +k.toFixed(3) };
  }
  /* Draw centred on (o.x, o.y) into a w×h box. cover fills the box and lets the
   * caller clip (the dial already clips to DR); contain letterboxes inside it;
   * stretch ignores the source aspect. Returns false when the asset cannot draw,
   * so a caller can flag the gap instead of shipping a silently blank layer. */
  /* One implementation of "where does the art actually land", used by the draw path
   * AND by the composer's alignment guide, so the frame on the screen can never drift
   * away from the pixels the engine laid down. zoom multiplies the box the fit runs in,
   * which keeps cover/contain/stretch meaning what they always meant; callers clip
   * separately (artInDial clips to the dial), so zooming in cannot spill past the rehaut. */
  function artBox(id, o) {
    o = o || {};
    const a = assetGet(id);
    const z = o.zoom == null ? 1 : Math.max(0.05, +o.zoom || 1);
    const w = (o.w == null ? 2 * DR : +o.w) * z, h = (o.h == null ? 2 * DR : +o.h) * z;
    let sw = w, sh = h;
    if (a && a.w && a.h) {
      const fit = o.fit || 'cover';
      if (fit === 'cover') { const k = Math.max(w / a.w, h / a.h); sw = a.w * k; sh = a.h * k; }
      else if (fit === 'contain') { const k = Math.min(w / a.w, h / a.h); sw = a.w * k; sh = a.h * k; }
    }
    return { x: +o.x || 0, y: +o.y || 0, w: sw, h: sh, box: w, ok: !!(a && a.w && a.h) };
  }
  function drawAsset(ctx, id, o) {
    const a = assetGet(id);
    if (!a || !a.el || a.missing) return false;
    const b = artBox(id, o);
    if (!(b.w > 0 && b.h > 0)) return false;
    ctx.save();
    if (ctx.imageSmoothingQuality != null) ctx.imageSmoothingQuality = 'high';
    ctx.translate(b.x, b.y);
    if (o.rot) ctx.rotate(o.rot);
    if (o.alpha != null) ctx.globalAlpha *= o.alpha;
    ctx.drawImage(a.el, -b.w / 2, -b.h / 2, b.w, b.h);
    ctx.restore();
    return true;
  }
  /* Art laid into a static layer. The clip is the rehaut, and the rehaut only exists when
   * the composer draws the case: under "none" the author's own image carries the device's
   * metal, and clipping it to the dial circle would cut their case off and leave a black
   * ring in its place -- so the clip opens to the whole panel and their pixels decide where
   * the watch ends. Both backdrop slots (plate and dial surface) draw through here so they
   * behave identically. Returns false when the id has no bytes, letting the caller report
   * the gap instead of shipping a blank layer. */
  function artInDial(ctx, id, o) {
    if (!id || !assetReady(id)) return false;
    ctx.save();
    const r = o.clipR == null ? DR : +o.clipR;
    if (r > 0) { ctx.beginPath(); ctx.arc(C, C, r, 0, TAU); ctx.clip(); }
    const drew = drawAsset(ctx, id, { x: C + (+o.x || 0), y: C + (+o.y || 0), w: o.size == null ? 2 * DR : +o.size, h: o.size == null ? 2 * DR : +o.size,
      fit: o.fit || 'cover', zoom: o.zoom, alpha: o.opacity == null ? 1 : +o.opacity });
    ctx.restore();
    return drew;
  }
  /* Alpha-weighted centroid of the ink in a canvas: the same "the hub sits on the
   * pivot" intent bake_clock.py:snap_pivot_to_hub enforces at bake time, applied
   * here so the authored number starts right instead of being corrected later. */
  function inkCentroidOf(canvas) {
    const w = canvas.width, h = canvas.height, x = canvas.getContext('2d');
    let d; try { d = x.getImageData(0, 0, w, h).data; } catch (e) { return [w / 2, h / 2]; }
    let sw = 0, sx = 0, sy = 0;
    for (let i = 3; i < d.length; i += 4) { const a = d[i]; if (!a) continue; const p = (i - 3) / 4, px = p % w, py = (p / w) | 0; sw += a; sx += px * a; sy += py * a; }
    return sw ? [sx / sw, sy / sw] : [w / 2, h / 2];
  }
  /* Tight box around the visible ink, alpha >= 8 so an antialiased halo cannot stretch
   * it. The composer frames this inside the (possibly padded) plane so an author sees
   * the gap between the artwork and an arbour that sits outside it. */
  function inkBoundsOf(canvas) {
    const w = canvas.width, h = canvas.height, x = canvas.getContext('2d');
    let d; try { d = x.getImageData(0, 0, w, h).data; } catch (e) { return [0, 0, w, h]; }
    let x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (let i = 3; i < d.length; i += 4) {
      if (d[i] < 8) continue;
      const p = (i - 3) / 4, px = p % w, py = (p / w) | 0;
      if (px < x0) x0 = px; if (px > x1) x1 = px; if (py < y0) y0 = py; if (py > y1) y1 = py;
    }
    return x1 < 0 ? [0, 0, w, h] : [x0, y0, x1 + 1, y1 + 1];
  }
  /* The hand's length in source pixels: the farthest visible ink measured along
   * the direction the device will point this art, from the arbour. That slice --
   * not the image's frame -- is what Length 100% normalizes to, so a 3000 px
   * export and a 40 px doodle of the same drawing land the same tip on the
   * chapter. th is the standing turn in radians (the author's facing plus the
   * tilt of the subdial the hand rides): rotating the plane about the pivot
   * carries the ink with it, so the slice that reads at the time angle sits at
   * -th in the unrotated image. Measured once per asset and pivot from a copy
   * reduced to at most 512 px; at that size the sub-pixel error is under one
   * sprite pixel at the scales the composer actually draws. */
  function inkTipOf(a, gx, gy, th) {
    const snap = !(isFinite(gx) && isFinite(gy));
    const key = (snap ? 's' : Math.round(gx) + ',' + Math.round(gy)) + '@' + Math.round((th * 180) / Math.PI);
    a._tips = a._tips || {};
    if (a._tips[key] != null) return a._tips[key];
    const s0 = Math.min(1, 512 / Math.max(a.w, a.h)), rw = Math.max(1, Math.round(a.w * s0)), rh = Math.max(1, Math.round(a.h * s0));
    const c = cv(rw, rh), x = c.getContext('2d');
    x.drawImage(a.el, 0, 0, rw, rh);
    let d; try { d = x.getImageData(0, 0, rw, rh).data; } catch (e) { return (a._tips[key] = 0); }
    let px = snap ? NaN : gx * s0, py = snap ? NaN : gy * s0;
    if (snap) {                                                  // no authored arbour: the ink's own centre of mass is the hub
      let sw = 0, sx = 0, sy = 0;
      for (let i = 3; i < d.length; i += 4) { const al = d[i]; if (al < 8) continue; const p = (i - 3) / 4; sw += al; sx += (p % rw) * al; sy += ((p / rw) | 0) * al; }
      if (!sw) return (a._tips[key] = 0);
      px = sx / sw; py = sy / sw;
    }
    const sn = Math.sin(th), cs = Math.cos(th);
    let best = 0;
    for (let i = 3; i < d.length; i += 4) {
      if (d[i] < 8) continue;
      const p = (i - 3) / 4, u = (px - (p % rw)) * sn + (py - ((p / rw) | 0)) * cs;
      if (u > best) best = u;
    }
    return (a._tips[key] = best / s0);
  }
  /* A hand sprite from art. Everything downstream (drawHands, the shadow
   * silhouette, the manifest's pivot/centre/size, prep's integral-margin proof)
   * reads exactly { c, px, py } off handSprite, so an image-backed record takes
   * the same path with no change anywhere else. (px,py) is the authored pivot in
   * sprite pixels, or the hub-ink centroid when the author did not set one.
   *
   * o.fitTo is the reach the tip of this art must land at, in device px -- the
   * length the drawn hand of the same role has at Length 100%, times the author's
   * Length slider. The scale then comes out of the measure in either direction:
   * a huge upload shrinks, a small one enlarges, and the upload's own resolution
   * never decides how long the hand is. With no measurable ink the old fit-to-box
   * still decides, so an art-free stub and a vector frame keep their numbers.
   *
   * The pivot is NOT required to be inside the artwork. A hand whose arbour is
   * hidden behind a subdial, or one drawn so its hub sits off the ink, legitimately
   * puts it outside the visible bbox, and the author must be able to say so. The
   * device rotates the plane about its stored pivot, so the pivot has to sit inside
   * the plane: rather than reject the number, grow the canvas transparently until it
   * does. The ink never moves -- the whole image shifts by the pad and the pivot
   * shifts with it, so the point that lands on the arbour is the same point either
   * way, and prep's _placed() paste at (cx-px, cy-py) needs no change at all. */
  function handSpriteAsset(id, o) {
    const a = assetGet(id);
    if (!a || !a.el || a.missing) return null;
    const maxW = (o && o.maxW) || 466, maxH = (o && o.maxH) || 466;
    const w = a.w, h = a.h; if (!w || !h) return null;
    // zoom is the author's, on top of the fit-to-length fit: the pivot is a point IN
    // the image, so it scales with it and the arbour stays on the same texel. Past 1
    // on a raster that is an upscale, which the composer states rather than hides --
    // the ceiling of 8 keeps a mistyped 800 from allocating a canvas the size of a wall.
    const z = !o || o.zoom == null ? 1 : Math.min(8, Math.max(0.05, +o.zoom || 1));
    const gx = o && o.pivotX, gy = o && o.pivotY;
    const given = gx != null && gy != null && isFinite(+gx) && isFinite(+gy);
    // The standing turn the device applies on top of the data angle: the author's
    // facing, plus the tilt of the subdial this pointer rides on. The measuring
    // pass looks for the tip along that axis, so Length means the same thing for
    // art drawn at 12 and for art drawn across the movement.
    const th = rad(+(o && o.rot0) || 0) + (+(o && o.aimRad) || 0);
    let k;
    if (o && o.fitTo != null && isFinite(+o.fitTo) && +o.fitTo > 0) {
      const tip = inkTipOf(a, given ? +gx : NaN, given ? +gy : NaN, th);
      k = tip > 0.5 ? (+o.fitTo / tip) * z : Math.min(1, maxW / w, maxH / h) * z;
      k = Math.min(k, 3000 / Math.max(w, h));       // no canvas the size of a wall, whatever the two numbers ask for
    } else k = Math.min(1, maxW / w, maxH / h) * z;
    const cw = Math.ceil(w * k), ch = Math.ceil(h * k);
    const c = cv(cw, ch), x = c.getContext('2d');
    if (x.imageSmoothingQuality != null) x.imageSmoothingQuality = 'high';
    x.drawImage(a.el, 0, 0, cw, ch);
    let px = given ? (+gx * k) : NaN, py = given ? (+gy * k) : NaN;
    let snapped = !given;
    if (!isFinite(px) || !isFinite(py)) { const hub = inkCentroidOf(c); px = hub[0]; py = hub[1]; }
    let ink = inkBoundsOf(c);
    // PAD keeps the pivot clear of the plane edge, so a whole-pixel rounding at bake
    // time cannot drop it exactly on the last column the rotator samples.
    const PAD = 2, off = px < PAD || py < PAD || px > cw - PAD || py > ch - PAD;
    if (off) {
      const bx = Math.min(0, Math.floor(px) - PAD), tx = Math.max(cw, Math.ceil(px) + PAD);
      const by = Math.min(0, Math.floor(py) - PAD), ty = Math.max(ch, Math.ceil(py) + PAD);
      const p = cv(tx - bx, ty - by);
      p.getContext('2d').drawImage(c, -bx, -by);
      px -= bx; py -= by; c.width = p.width; c.height = p.height;
      c.getContext('2d').drawImage(p, 0, 0);
      ink = [ink[0] - bx, ink[1] - by, ink[2] - bx, ink[3] - by];
    }
    return { c, px, py, ink, asset: id, name: a.name, srcW: a.w, srcH: a.h, scale: k, zoom: z, rot0: +((o && o.rot0) || 0), snapped, padded: given && off };
  }
  /* Serialise for localStorage and .wckit: the references a Design ID carries,
   * plus the bytes a bundle carries. */
  function assetDump(withBytes) {
    const out = [];
    ASSETS.forEach((a) => { const r = { id: a.id, name: a.name, mime: a.mime, kind: a.kind, sha: a.sha, w: a.w, h: a.h }; if (withBytes) r.dataUrl = a.dataUrl; out.push(r); });
    return out;
  }
  function assetRestore(list) {
    let added = 0, needBytes = 0;
    (Array.isArray(list) ? list : []).forEach((r) => {
      if (!r || !r.id) return;
      const have = ASSETS.get(r.id);
      if (have && (have.el || !r.dataUrl)) return;
      if (!r.dataUrl) { needBytes++; return; }
      assetLoad(assetPut(r), () => { if (typeof window.__assetDirty === 'function') window.__assetDirty(); }); added++;
    });
    return { added, needBytes };
  }
  function assetDrop(id) { ASSETS.delete(id); delete IMAGES[id]; }
  function assetClear() { ASSETS.clear(); for (const k in IMAGES) delete IMAGES[k]; }

  const COMP_STYLES = {
    opening: { bevel: 'Bevelled', lip: 'Lit lip (centred)', frame: 'Gilt frame', plain: 'Plain cut' },
    subdial: { enamel: 'Enamel', guilloche: 'Sunburst', azure: 'Azurage', matte: 'Grained', metal: 'Silvered metal', carbon: 'Carbon · woven', forged: 'Carbon · forged', transparent: 'Transparent · no plate (art lives in the dial)' },
    window: { frame: 'Gilt frame', bevel: 'Bevelled', plain: 'Plain cut' },
    moon: { classic: 'Navy & gold', silver: 'Silver night', black: 'Black sky', sphere: 'Realistic sphere', graphic: 'Graphic', neon: 'Neon' },
    flare: { print: 'Printed', applied: 'Applied metal', lume: 'Lume', engraved: 'Engraved' },
  };
  /* The metal an aperture's raised frame is struck in -- the 'frame' style on an
   * opening or a window. Gilt is the movement's own finish and stays the default;
   * each other entry is a standing hi/mid/lo gradient so the ring still reads as
   * metal under the lights instead of a flat crayon line. */
  const FRAME_COLORS = { gilt: 'Gilt (movement metal)', steel: 'Steel', rose: 'Rose gold', rhodium: 'White gold', gunmetal: 'Gunmetal', black: 'Blackened' };
  const FRAME_PALS = {
    steel: { hi: '#f4f6f8', mid: '#8b9198', lo: '#3a3e44' },
    rose: { hi: '#f6cbb6', mid: '#a8705e', lo: '#5d3122' },
    rhodium: { hi: '#f7f9fb', mid: '#b3b9c0', lo: '#4a5057' },
    gunmetal: { hi: '#d4d7db', mid: '#7f858c', lo: '#2c3034' },
    black: { hi: '#6a6a68', mid: '#232120', lo: '#080706' },
  };
  const SUB_DISPLAYS = { classic: 'Classic track', railway: 'Railway', dots: 'Dots', minimal: 'Ticks only', bold: 'Bold numerals', numerals: 'Numerals only', sector: 'Sector' };
  const SCALES = { seconds: 'Seconds', date: 'Date 1–31', day: 'Day of week', month: 'Month', hours: 'Hours', none: 'No scale' };
  const SHOWS = { date: 'Date', day: 'Day', month: 'Month', custom: 'Custom format' };
  // date formatting: moment/Unicode-style tokens (YYYY MMMM MMM MM M DDDD DDD DD D Do dddd ddd dd d E WW W Q) and strftime (%Y %y %B %b %m %d %e %j %A %a %u %w %V %U %q); [text] is literal
  const LDAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'], LMONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
  function fmtDate(fmt, d, cs) {
    const Y = d.getFullYear(), M = d.getMonth(), D = d.getDate(), wd = d.getDay(), p2 = (n) => String(n).padStart(2, '0');
    const doy = Math.round((Date.UTC(Y, M, D) - Date.UTC(Y, 0, 1)) / 864e5) + 1;
    const iso = (() => { const t = new Date(Date.UTC(Y, M, D)), dn = (wd + 6) % 7; t.setUTCDate(t.getUTCDate() - dn + 3); const y0 = new Date(Date.UTC(t.getUTCFullYear(), 0, 4)); return 1 + Math.round(((t - y0) / 864e5 - 3 + ((y0.getUTCDay() + 6) % 7)) / 7); })();
    const wsun = Math.floor((doy + 6 - wd) / 7), ord = (n) => n + (n % 100 >= 11 && n % 100 <= 13 ? 'th' : ['th', 'st', 'nd', 'rd'][n % 10] || 'th');
    const T = { YYYY: Y, YY: p2(Y % 100), MMMM: LMONTHS[M], MMM: LMONTHS[M].slice(0, 3), MM: p2(M + 1), M: M + 1, DDDD: String(doy).padStart(3, '0'), DDD: doy, Do: ord(D), DD: p2(D), D, dddd: LDAYS[wd], ddd: LDAYS[wd].slice(0, 3), dd: LDAYS[wd].slice(0, 2), d: wd, E: wd || 7, WW: p2(iso), W: iso, Q: Math.floor(M / 3) + 1 };
    const S = { Y, y: p2(Y % 100), B: LMONTHS[M], b: LMONTHS[M].slice(0, 3), h: LMONTHS[M].slice(0, 3), m: p2(M + 1), d: p2(D), e: String(D).padStart(2, ' '), j: String(doy).padStart(3, '0'), A: LDAYS[wd], a: LDAYS[wd].slice(0, 3), u: wd || 7, w: wd, V: p2(iso), U: p2(wsun), q: Math.floor(M / 3) + 1, '%': '%' };
    const out = String(fmt == null ? '' : fmt).replace(/\[([^\]]*)\]|%([-]?)([A-Za-z%])|YYYY|YY|MMMM|MMM|MM|M|DDDD|DDD|Do|DD|D|dddd|ddd|dd|d|E|WW|W|Q/g, (m, lit, dash, sp) => {
      if (lit != null) return '\u0000' + lit + '\u0001';
      if (sp != null) { let v = S[sp]; if (v == null) return m; v = String(v); return dash ? v.replace(/^[0 ]+(?=.)/, '') : v; }
      return String(T[m]);
    });
    const parts = out.split(/(\u0000[^\u0001]*\u0001)/);
    return parts.map((p) => (p[0] === '\u0000' ? p.slice(1, -1) : cs === 'lower' ? p.toLowerCase() : cs === 'asis' ? p : p.toUpperCase())).join('');
  }
  const PRINTS = { roman: 'Roman', arabic: 'Breguet arabic', regulator: 'Regulator minutes', baton: 'Applied batons', minimal: 'Minimal batons', sans: 'Modern sans', railway: 'Railway', sector: 'Sector', diver: 'Lume plots', station: 'Station bars', pilot: 'Pilot · triangle & dots', none: 'Blank' };
  const REACH = { roman: 205, arabic: 200, regulator: 206, baton: 200, minimal: 205, sans: 205, railway: 202, sector: 205, diver: 200, station: 214, pilot: 204, none: 205 };
  const SIZE_RANGE = { opening: [12, 200, 1], subdial: [26, 110, 0.5], window: [0.6, 1.8, 0.05], moon: [14, 110, 1], flare: [4, 120, 0.5] };
  const WIN_SIZE = { date: [30, 22], day: [52, 22], month: [52, 22], custom: [72, 22] };
  // Presets are just starting designs. angle: degrees clockwise from 12 · dist: px from the host's centre · rot: degrees.
  const PRESETS = {
    skeleton: { name: 'Skeleton', blurb: 'Open centre · Roman chapter · small seconds', hands: 'breguet', handFinish: 'blued', design: {
      print: 'roman', centre: { hour: 1, minute: 1, second: 0 }, comps: [
        { id: 'c1', type: 'opening', host: 'dial', angle: 0, dist: 0, rot: 0, size: 160, style: 'lip', show: 'free', hour: 12 },
        { id: 'c2', type: 'subdial', host: 'dial', angle: 135, dist: 80.6, rot: 0, size: 78.5, style: 'enamel', scale: 'seconds', open: 0 },
      ] } },
    calendar: { name: 'Triple calendar', blurb: 'Open heart · day & month windows · pointer date + moon', hands: 'leaf', handFinish: 'gilt', design: {
      print: 'arabic', centre: { hour: 1, minute: 1, second: 1 }, comps: [
        { id: 'c1', type: 'opening', host: 'dial', angle: 349, dist: 138, rot: 0, size: 41, style: 'bevel', show: 'balance', hour: 12 },
        { id: 'c2', type: 'window', host: 'dial', angle: 270, dist: 91, rot: 0, size: 1, style: 'frame', shows: 'day', disc: 'auto' },
        { id: 'c3', type: 'window', host: 'dial', angle: 90, dist: 91, rot: 0, size: 1, style: 'frame', shows: 'month', disc: 'auto' },
        { id: 'c4', type: 'subdial', host: 'dial', angle: 180, dist: 102, rot: 0, size: 62, style: 'guilloche', scale: 'date', open: 0 },
        { id: 'c5', type: 'moon', host: 'c4', angle: 0, dist: 2, rot: 0, size: 29, style: 'classic' },
      ] } },
    modern: { name: 'Modern', blurb: 'Railway track · hex escapement window · date at 3', hands: 'dauphine', handFinish: 'steel', design: {
      print: 'railway', numerals: 'quarters', surface: 'guilloche', color: 'slate', aging: 0.3, stains: 0, dust: 0.4, centre: { hour: 1, minute: 1, second: 1 }, comps: [
        { id: 'c1', type: 'opening', host: 'dial', angle: 315, dist: 140, rot: 0, size: 42, shape: 'hex', style: 'bevel', show: 'balance', hour: 12 },
        { id: 'c2', type: 'window', host: 'dial', angle: 90, dist: 150, rot: 0, size: 1, style: 'plain', shows: 'date', disc: 'dark' },
        { id: 'c3', type: 'subdial', host: 'dial', angle: 180, dist: 104, rot: 0, size: 58, style: 'azure', scale: 'seconds', open: 0, color: 'black' },
      ] } },
    sector: { name: 'Sector', blurb: 'Bauhaus sector rings · geometric type · orange seconds', hands: 'pencil', handFinish: 'black', design: {
      print: 'sector', numerals: 'all', font: 'jost', surface: 'metal', color: 'white', aging: 0, stains: 0, dust: 0, centre: { hour: 1, minute: 1, second: 1 }, hands: { 'centre-second': { finish: 'orange' } }, comps: [
        { id: 'c1', type: 'opening', host: 'dial', angle: 0, dist: 0, rot: 0, size: 30, shape: 'circle', style: 'bevel', show: 'balance', hour: 12 },
      ] } },
    diver: { name: 'Broad arrow', blurb: 'Black grained dial · lume plots · broad arrow hands', hands: 'broad', handFinish: 'steel', design: {
      print: 'diver', numerals: 'all', surface: 'matte', color: 'black', aging: 0.4, stains: 0, dust: 0.5, centre: { hour: 1, minute: 1, second: 1 }, comps: [
        { id: 'c1', type: 'window', host: 'dial', angle: 90, dist: 150, rot: 0, size: 1, style: 'plain', shows: 'date', disc: 'dark' },
        { id: 'c2', type: 'opening', host: 'dial', angle: 0, dist: 0, rot: 0, size: 26, shape: 'slot', style: 'bevel', show: 'balance', hour: 12 },
      ] } },
    orbital: { name: 'Orbital arrow', blurb: 'Open dial · arrow tourbillon sweeps the seconds', hands: 'skeleton', handFinish: 'steel', train: 'orbital', design: {
      print: 'railway', numerals: 'none', font: 'barlow', color: 'black', aging: 0, stains: 0, dust: 0, centre: { hour: 1, minute: 1, second: 0 }, comps: [
        { id: 'c1', type: 'opening', host: 'dial', angle: 0, dist: 0, rot: 0, size: 200, shape: 'circle', style: 'lip', show: 'free', hour: 12 },
      ] } },
    aviator: { name: 'Aviator', blurb: 'Pilot dial · triangle and dots at 12 · lumed spear hands', hands: 'aviator', handFinish: 'black', caseMetal: 'steel', design: {
      print: 'pilot', numerals: 'all', font: 'grotesk', numColor: '#e8d6a0', surface: 'matte', color: 'black', aging: 0.15, stains: 0, dust: 0.3, handLume: '#e8d6a0', secStyle: 'flieger', centre: { hour: 1, minute: 1, second: 1 }, comps: [] } },
    opentour: { name: 'Open tourbillon', blurb: 'X-bridge skeleton · carbon depth · tourbillon at 6 · tachymetre', hands: 'baton', handFinish: 'steel', train: 'skeltour', caseStyle: 'tachy', caseMetal: 'gunmetal', cfg: { temp: 4000, glow: 0.35, finish: 'rhodium' }, design: {
      print: 'baton', numerals: 'all', font: 'grotesk', surface: 'metal', color: 'slate', aging: 0, stains: 0, dust: 0.15, openwork: 'xarch', owBar: 17, owRing: 178, owHub: 24, owRim: 7, owInlay: 1, owDepth: 1.4,
      handLume: '#f4f1e6', secStyle: 'needle', centre: { hour: 1, minute: 1, second: 1 }, comps: [
        { id: 'c1', type: 'opening', host: 'dial', angle: 180, dist: 107, rot: 0, size: 56, shape: 'circle', style: 'bevel', show: 'balance', hour: 12 },
        { id: 'c2', type: 'subdial', host: 'dial', angle: 270, dist: 92, rot: 0, size: 50, style: 'metal', scale: 'hours', open: 0.55, color: 'charcoal', tstyle: 'minimal', height: 1.3 },
        { id: 'c3', type: 'subdial', host: 'dial', angle: 90, dist: 92, rot: 0, size: 50, style: 'metal', scale: 'seconds', open: 0.55, color: 'charcoal', tstyle: 'minimal', height: 1.3 },
        { id: 'c4', type: 'flare', host: 'dial', angle: 0, dist: 105, rot: 0, size: 6.5, style: 'applied', icon: 'arctext', text: 'TOURBILLON', font: 'grotesk', weight: 'bold', spacing: 0.3, color: 'auto' },
      ] } },
    kitetour: { name: 'Kite tourbillon', blurb: 'Open dial · kite carriage with blued inlay sweeps the seconds', hands: 'dauphine', handFinish: 'steel', train: 'kite', caseMetal: 'steel', cfg: { temp: 4200, glow: 0.45 }, design: {
      print: 'minimal', numerals: 'all', font: 'grotesk', surface: 'metal', color: 'slate', aging: 0, stains: 0, dust: 0.1, centre: { hour: 1, minute: 1, second: 0 }, comps: [
        { id: 'c1', type: 'opening', host: 'dial', angle: 0, dist: 0, rot: 0, size: 200, shape: 'circle', style: 'bevel', show: 'free', hour: 12 },
      ] } },
    station: { name: 'Station', blurb: 'Railway-clock style · bold bars · red disc seconds', hands: 'station', handFinish: 'black', caseStyle: 'polished', caseMetal: 'steel', design: {
      print: 'station', numerals: 'all', numColor: '#111111', handShade: 'flat', color: 'white', aging: 0, stains: 0, dust: 0, secStyle: 'station', centre: { hour: 1, minute: 1, second: 1 }, hands: { 'centre-second': { finish: 'red' } }, comps: [
        { id: 'c1', type: 'flare', host: 'dial', angle: 0, dist: 108, rot: 0, size: 9, style: 'print', icon: 'plus', color: '#ffffff', bg: '#e3000f', bgShape: 'rect', bgW: 2.4, bgH: 1.05 },
      ] } },
    luna: { name: 'Luna', blurb: 'Midnight sunburst · big realistic moon at 6', hands: 'dauphine', handFinish: 'gilt', design: {
      print: 'minimal', numerals: 'all', font: 'bodoni', surface: 'guilloche', color: 'navy', aging: 0, stains: 0, dust: 0.2, centre: { hour: 1, minute: 1, second: 1 }, comps: [
        { id: 'c1', type: 'moon', host: 'dial', angle: 180, dist: 96, rot: 0, size: 92, style: 'sphere' },
      ] } },
    neon: { name: 'Neon moon', blurb: 'Violet forged carbon · neon moon · vivid hands', hands: 'arrow', handFinish: 'cyan', design: {
      print: 'sans', numerals: 'quarters', font: 'jost', surface: 'forged', color: 'violet', aging: 0, stains: 0, dust: 0, centre: { hour: 1, minute: 1, second: 1 }, hands: { 'centre-second': { finish: 'magenta' } }, comps: [
        { id: 'c1', type: 'moon', host: 'dial', angle: 0, dist: 100, rot: 0, size: 70, style: 'neon' },
      ] } },
    phase: { name: 'Phase', blurb: 'Porcelain · graphic moon subdial · date at 3', hands: 'pencil', handFinish: 'black', design: {
      print: 'railway', numerals: 'all', font: 'jost', color: 'porcelain', aging: 0, stains: 0, dust: 0, centre: { hour: 1, minute: 1, second: 1 }, hands: { 'centre-second': { finish: 'red' } }, comps: [
        { id: 'c1', type: 'subdial', host: 'dial', angle: 180, dist: 100, rot: 0, size: 56, style: 'azure', scale: 'none', open: 0, color: 'white' },
        { id: 'c2', type: 'moon', host: 'c1', angle: 0, dist: 0, rot: 0, size: 70, style: 'graphic' },
        { id: 'c3', type: 'window', host: 'dial', angle: 90, dist: 150, rot: 0, size: 1, style: 'plain', shows: 'date', disc: 'light' },
      ] } },
    regulator: { name: 'Regulator', blurb: 'Centre minutes · hours at 12 with date · open seconds', hands: 'spade', handFinish: 'blued', design: {
      print: 'regulator', centre: { hour: 0, minute: 1, second: 0 }, comps: [
        { id: 'c1', type: 'subdial', host: 'dial', angle: 0, dist: 103, rot: 0, size: 64, style: 'azure', scale: 'hours', open: 0 },
        { id: 'c2', type: 'subdial', host: 'dial', angle: 180, dist: 103, rot: 0, size: 64, style: 'enamel', scale: 'seconds', open: 0.64 },
        { id: 'c3', type: 'window', host: 'c1', angle: 180, dist: 32, rot: 0, size: 1, style: 'frame', shows: 'date', disc: 'auto' },
      ] } },
    perpetual: { name: 'Perpetual calendar', blurb: 'Day · date · month subdials · moon at 6', hands: 'baton', handFinish: 'gilt', design: {
      print: 'baton', centre: { hour: 1, minute: 1, second: 1 }, comps: [
        { id: 'c1', type: 'subdial', host: 'dial', angle: 0, dist: 102, rot: 0, size: 50, style: 'azure', scale: 'month', open: 0 },
        { id: 'c2', type: 'subdial', host: 'dial', angle: 90, dist: 102, rot: 0, size: 50, style: 'azure', scale: 'date', open: 0 },
        { id: 'c3', type: 'subdial', host: 'dial', angle: 270, dist: 102, rot: 0, size: 50, style: 'azure', scale: 'day', open: 0 },
        { id: 'c4', type: 'subdial', host: 'dial', angle: 180, dist: 102, rot: 0, size: 50, style: 'azure', scale: 'none', open: 0 },
        { id: 'c5', type: 'moon', host: 'c4', angle: 180, dist: 8, rot: 0, size: 34, style: 'classic' },
      ] } },
  };
  const NEW_COMP = {
    opening: { angle: 0, dist: 120, rot: 0, size: 34, style: 'bevel', show: 'free', hour: 12 },
    subdial: { angle: 180, dist: 100, rot: 0, size: 52, style: 'enamel', scale: 'seconds', open: 0 },
    window: { angle: 90, dist: 150, rot: 0, size: 1, style: 'frame', shows: 'date', disc: 'auto' },
    moon: { angle: 180, dist: 110, rot: 0, size: 32, style: 'classic' },
    flare: { angle: 0, dist: 110, rot: 0, size: 16, style: 'print', icon: 'star', color: 'auto', opacity: 1, text: 'BRAND' },
  };

  // ---------- apertures: one path builder cuts the host, casts its shadow and frames the opening
  let MC = null;
  const winFont = (c, k) => '700 ' + Math.round((c.shows === 'date' || c.type === 'date' ? 18 : 15) * (k || 1)) + 'px ' + ((FONTS[c.wf || c.wfont] && FONTS[c.wf || c.wfont][1]) || SERIF);
  function customWidth(c) {
    MC = MC || cv(4).getContext('2d'); MC.font = winFont({ wfont: c.wfont, shows: 'custom' }, 1);
    const sp = 'letterSpacing' in MC; if (sp) MC.letterSpacing = '1.5px';
    if (sp) MC.letterSpacing = '0px';
    // width from rounded per-glyph advances over every date this year and next, so the dial window matches the device text kit
    const adv = {}, A = (ch) => adv[ch] ?? (adv[ch] = Math.max(1, Math.round(MC.measureText(ch).width + 1.5)));
    let w = 0; const y0 = new Date().getFullYear();
    for (let t = new Date(y0, 0, 1); t.getFullYear() <= y0 + 1; t.setDate(t.getDate() + 1)) { let tw = 0; for (const ch of fmtDate(c.fmt || 'ddd DD MMM', t, c.fcase)) tw += A(ch); w = Math.max(w, tw); }
    return Math.max(22, Math.ceil(w + 2 * Math.max(2.5, 3) + 10));
  }
  function holePath(h) {
    return (ctx, dx = 0, dy = 0) => {
      const x = h.x + dx, y = h.y + dy;
      if (h.kind === 'circle') { ctx.moveTo(x + h.r, y); ctx.arc(x, y, h.r, 0, TAU); return; }
      const m = ctx.getTransform(); ctx.translate(x, y); ctx.rotate(h.rot || 0);
      if (h.kind === 'rect') ctx.roundRect(-h.w / 2, -h.h / 2, h.w, h.h, h.rad || 0);
      else if (h.kind === 'slot') ctx.roundRect(-h.w / 2, -h.h / 2, h.w, h.h, h.h / 2);
      else if (h.kind === 'arch') { ctx.moveTo(-h.w / 2, h.h / 2); ctx.lineTo(-h.w / 2, -h.h / 2 + h.w / 2); ctx.arc(0, -h.h / 2 + h.w / 2, h.w / 2, Math.PI, TAU); ctx.lineTo(h.w / 2, h.h / 2); ctx.closePath(); }
      else if (h.kind === 'poly') { for (let i = 0; i < h.n; i++) { const a = -Math.PI / 2 + (i / h.n) * TAU + Math.PI / h.n; i ? ctx.lineTo(Math.cos(a) * h.r, Math.sin(a) * h.r) : ctx.moveTo(Math.cos(a) * h.r, Math.sin(a) * h.r); } ctx.closePath(); }
      else {
        const a = h.a, hh = a * 0.36;
        ctx.moveTo(-a, 0); ctx.arc(0, 0, a, Math.PI, TAU);
        ctx.lineTo(a / 2 + hh, 0); ctx.arc(a / 2, 0, hh, 0, Math.PI, true);
        ctx.lineTo(-a / 2 + hh, 0); ctx.arc(-a / 2, 0, hh, 0, Math.PI, true);
        ctx.closePath();
      }
      ctx.setTransform(m);
    };
  }
  function holeBox(h) {
    if (h.kind === 'circle' || h.kind === 'poly') return [h.x - h.r, h.y - h.r, h.x + h.r, h.y + h.r];
    if (h.kind === 'moon' && !h.rot) return [h.x - h.a, h.y - h.a, h.x + h.a, h.y];
    if (h.rot) { const r = h.kind === 'moon' ? h.a : Math.hypot(h.w, h.h) / 2; return [h.x - r, h.y - r, h.x + r, h.y + r]; }
    return [h.x - h.w / 2, h.y - h.h / 2, h.x + h.w / 2, h.y + h.h / 2];
  }
  // true shape test (in the opening's own rotated frame), so a round centre window no longer hides numerals beside it
  function inside(h, x, y, m) {
    if (h.hidden) return false;
    if (h.kind === 'circle') return Math.hypot(x - h.x, y - h.y) < h.r + m;
    const c = Math.cos(-(h.rot || 0)), sn = Math.sin(-(h.rot || 0)), dx = x - h.x, dy = y - h.y, lx = dx * c - dy * sn, ly = dx * sn + dy * c;
    if (h.kind === 'poly') return Math.hypot(lx, ly) < h.r + m;
    if (h.kind === 'moon') return Math.abs(lx) < h.a + m && ly > -h.a - m && ly < m;
    return Math.abs(lx) < h.w / 2 + m && Math.abs(ly) < h.h / 2 + m;
  }
  const blocked = (x, y, holes, m) => holes.some((h) => inside(h, x, y, m));

  // absolute position of every complication; children of a subdial ride on its centre and rotation
  function geom(design, phi) {
    const _ = L(), comps = design.comps || [], byId = {}, G = {};
    comps.forEach((c) => (byId[c.id] = c));
    const place = (c, depth) => {
      if (G[c.id]) return G[c.id];
      let hx = C, hy = C, hr = 0, host = 'dial';
      const hc = c.type !== 'subdial' && c.host && c.host !== 'dial' ? byId[c.host] : null;
      if (hc && hc.type === 'subdial' && depth < 4) { const hg = place(hc, depth + 1); hx = hg.x; hy = hg.y; hr = hg.rot; host = hc.id; }
      let x, y;
      if (c.type === 'opening' && c.show === 'balance') { const q = rotAbout(_.BAL0.x, _.BAL0.y, phi || 0); x = q[0]; y = q[1]; host = 'dial'; hx = C; hy = C; hr = 0; }
      else { const a = rad(+c.angle || 0) + hr, d = +c.dist || 0; x = hx + Math.sin(a) * d; y = hy - Math.cos(a) * d; }
      return (G[c.id] = { id: c.id, x, y, rot: rad(+c.rot || 0) + hr, host, hx, hy, hrot: hr });
    };
    comps.forEach((c) => place(c, 0));
    return G;
  }
  function layout(design, phi) {
    const G = geom(design, phi), list = design.comps || [], comps = [], flares = [], subs = [], dialHoles = [], subHoles = {}, hits = [], subZ = {};
    let wearIn = 0;
    list.forEach((c) => {
      if (c.type !== 'subdial') return;
      const g = G[c.id], r = Math.max(10, +c.size || 50);
      const s = { id: c.id, x: g.x, y: g.y, r, rot: g.rot, scale: c.scale || 'none', style: c.style || 'enamel', color: c.color || 'match', open: c.open > 0 ? r * Math.min(0.85, +c.open) : 0,
        t: { font: c.tfont, orient: c.torient, dir: c.tdir, weight: c.tweight, size: c.tsize, style: c.tstyle, color: c.tcolor }, height: c.height == null ? 1 : +c.height, tex: c.tex, wa: c.weaveAngle, ws: c.weaveScale };
      subs.push(s); subHoles[c.id] = []; subZ[c.id] = 1 + subs.length * 0.01;
      // The open-centre punch exists to expose the movement under a plate that is
      // otherwise opaque. A transparent subdial lays no plate, so punching would only
      // erase the author's baked-in art: the layout skips it and exportKit skips the
      // layer, which is the same "no subdial in this kit" route prep already supports.
      s.transparent = s.style === 'transparent';
      if (s.open && !s.transparent) dialHoles.push({ kind: 'circle', x: s.x, y: s.y, r: s.open + 4, hidden: true });
      hits.push({ id: c.id, x: g.x, y: g.y, r: r + 2.5, z: subZ[c.id] });
    });
    list.forEach((c) => {
      if (c.type === 'subdial') return;
      const g = G[c.id], sz = +c.size || 1, onSub = g.host !== 'dial' && subHoles[g.host] ? g.host : null;
      // A transparent subdial ships no plate of its own, so anything hosted on it has
      // to be carried by the dial instead -- otherwise it exists only in the pointer
      // hand's geometry and vanishes from both the preview and the kit.
      const thost = onSub ? subs.some((q) => q.id === onSub && q.transparent) : false;
      if (c.type === 'flare') {
        const onPlate = c.host === 'plate';
        flares.push(Object.assign({}, c, { x: g.x, y: g.y, rot: g.rot, hx: g.hx, hy: g.hy, host: onPlate ? 'plate' : onSub || 'dial' }));
        hits.push({ id: c.id, x: g.x, y: g.y, r: Math.max(8, sz * Math.max(c.icon === 'text' ? 2.2 : 1.15, c.bg && c.bg !== 'none' ? Math.max(+c.bgW || 1.6, +c.bgH || 1) : 0)), z: onPlate ? -1 : (onSub ? subZ[onSub] : 0) + 0.008 });
        return;
      }
      let hole;
      if (c.type === 'opening') {
        const centred = Math.hypot(g.x - C, g.y - C) < 4, sh = c.shape || 'circle', b = { x: g.x, y: g.y, rot: g.rot };
        hole = sh === 'square' ? Object.assign(b, { kind: 'rect', w: sz * 1.8, h: sz * 1.8, rad: sz * 0.2 })
          : sh === 'hex' ? Object.assign(b, { kind: 'poly', n: 6, r: sz }) : sh === 'oct' ? Object.assign(b, { kind: 'poly', n: 8, r: sz })
          : sh === 'slot' ? Object.assign(b, { kind: 'slot', w: sz * 2.6, h: sz * 1.2 }) : sh === 'arch' ? Object.assign(b, { kind: 'arch', w: sz * 1.5, h: sz * 2 })
          : { kind: 'circle', x: g.x, y: g.y, r: sz };
        hole.style = c.style === 'lip' && !(centred && sh === 'circle') ? 'frame' : c.style || 'bevel';
        if (hole.style === 'frame' && c.frameColor && c.frameColor !== 'gilt') hole.fc = c.frameColor;
        if (centred && sh === 'circle' && !onSub) wearIn = Math.max(wearIn, sz);
        if (onSub && !thost) { subHoles[onSub].push(hole); dialHoles.push(Object.assign({}, hole, { hidden: true })); } else dialHoles.push(hole);
        const bb = holeBox(hole);
        hits.push({ id: c.id, x: g.x, y: g.y, r: Math.max(bb[2] - bb[0], bb[3] - bb[1]) / 2, z: onSub ? subZ[onSub] + 0.005 : 0 });
        return;
      }
      if (c.type === 'moon') hole = ROUND_MOON[c.style] ? { kind: 'circle', x: g.x, y: g.y, r: sz * 0.62, style: c.style === 'sphere' ? 'frame' : 'bevel' } : { kind: 'moon', x: g.x, y: g.y, a: sz, rot: g.rot, style: 'frame' };
      else {
        const ws = c.shows === 'custom' ? [customWidth(c), 22] : WIN_SIZE[c.shows] || WIN_SIZE.date;
        const w = ws[0] * sz * (+c.ww || 1), h = ws[1] * sz * (+c.wh || 1);
        /* A round date window is a real complication (a cyclops under a domed crystal,
         * an opening punched through a enamel dial for a single numeral), so the
         * aperture shape is the author's and not a consequence of the data it shows.
         * The circle takes the LONGER side as its diameter: flipping a square window
         * to round then keeps the text it framed instead of shrinking to the short
         * axis and clipping the glyph. */
        hole = c.wshape === 'circle' ? { kind: 'circle', x: g.x, y: g.y, r: Math.max(w, h) / 2, style: c.style || 'frame' }
          : { kind: 'rect', x: g.x, y: g.y, w, h, rad: 2.5 * sz, rot: g.rot, style: c.style || 'frame' };
        if (hole.style === 'frame' && c.frameColor && c.frameColor !== 'gilt') hole.fc = c.frameColor;
      }
      // A dial-hosted window/moon is an opaque data disc, not a viewport: seal its
      // aperture so the shipped dial carries no hole there. The value is composited
      // over the dial at frame build (once a day); the bank never DMA's under it. A
      // subdial-hosted disc is NOT sealed -- the subdial is opaque already, so its
      // punch stays to show the disc, and the geartrain under it is culled by the
      // subdial itself. Skeleton openings ('opening', below) keep their holes.
      if (!onSub || thost) hole.seal = true;
      comps.push({ id: c.id, type: c.type === 'moon' ? 'moon' : c.shows || 'date', fmt: c.fmt || 'ddd DD MMM', fcase: c.fcase, style: c.style, disc: c.disc || 'auto', dc: c.discColor, tc: c.textColor, wf: c.wfont, mc: c.moonColor, mc2: c.moonColor2, mbg: c.moonBg, host: onSub || 'dial', hole, k: sz });
      (onSub && !thost ? subHoles[onSub] : dialHoles).push(hole);
      const b = holeBox(hole);
      hits.push({ id: c.id, x: (b[0] + b[2]) / 2, y: (b[1] + b[3]) / 2, r: Math.max(b[2] - b[0], b[3] - b[1]) / 2 + 2, z: onSub ? subZ[onSub] + 0.005 : 0 });
    });
    hits.sort((a, b) => a.z - b.z || b.r - a.r);
    return { comps, flares, subs, dialHoles, subHoles, wearIn, hits, G };
  }

  function cutHoles(ctx, holes, pal, glow) {
    const _ = L();
    ctx.save(); ctx.globalCompositeOperation = 'destination-out'; ctx.fillStyle = '#000';
    holes.forEach((h) => { ctx.beginPath(); holePath(h)(ctx); ctx.fill(); });
    ctx.restore();
    const vis = holes.filter((h) => !h.hidden); if (!vis.length) return;
    // overlapping openings read as one merged opening: shadows come only from real dial edges, rims vanish inside a neighbour
    const bx = vis.map(holeBox), ov = (i, j) => bx[i][0] < bx[j][2] + 6 && bx[j][0] < bx[i][2] + 6 && bx[i][1] < bx[j][3] + 6 && bx[j][1] < bx[i][3] + 6;
    const solid = cv(S), so = solid.getContext('2d'); so.fillStyle = '#000'; so.fillRect(0, 0, S, S);
    so.globalCompositeOperation = 'destination-out'; holes.forEach((h) => { so.beginPath(); holePath(h)(so); so.fill(); });
    const SL = cv(S), sl = SL.getContext('2d'), BL = cv(S), bl = BL.getContext('2d');
    const erase = (x, list) => { if (!list.length) return; x.save(); x.setTransform(1, 0, 0, 1, 0, 0); x.globalCompositeOperation = 'destination-out'; x.filter = 'none'; x.fillStyle = '#000'; list.forEach((o) => { x.beginPath(); holePath(o)(x); x.fill(); }); x.restore(); };
    vis.forEach((h, i) => {
      const p = holePath(h), b = bx[i], k = Math.max(0.25, Math.min(1, (b[3] - b[1]) / 320)), ak = 0.45 + 0.55 * k;
      const near = vis.filter((o, j) => j !== i && ov(i, j)), before = vis.filter((o, j) => j < i && ov(i, j));
      sl.clearRect(0, 0, S, S);
      [[5, 8, 0.85], [-5, 8, 0.85], [16, 28, 0.75], [-16, 28, 0.75], [30, 50, 0.5], [-30, 50, 0.5], [0, 6, 0.5]].forEach(([oy, blr, al]) => {
        sl.save(); sl.beginPath(); p(sl); sl.clip();
        sl.shadowColor = 'rgba(0,0,0,' + al * ak + ')'; sl.shadowBlur = blr * k; sl.shadowOffsetY = oy * k;
        sl.drawImage(solid, 0, 0); sl.restore();
      });
      erase(sl, before); ctx.drawImage(SL, 0, 0);
      bl.clearRect(0, 0, S, S); let lt = null;
      if (h.style === 'lip') {
        let g = bl.createRadialGradient(h.x, h.y, h.r - 22, h.x, h.y, h.r);
        g.addColorStop(0, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,.4)');
        bl.fillStyle = g; bl.beginPath(); p(bl); bl.fill();
        bl.strokeStyle = 'rgba(0,0,0,.65)'; bl.lineWidth = 2.5; bl.beginPath(); bl.arc(h.x, h.y, h.r + 0.5, 0, TAU); bl.stroke();
        bl.strokeStyle = 'rgba(255,250,235,.35)'; bl.lineWidth = 1; bl.beginPath(); bl.arc(h.x, h.y, h.r + 2.6, 0, TAU); bl.stroke();
        lt = cv(S); const lx = lt.getContext('2d'); lx.globalCompositeOperation = 'lighter';
        g = lx.createRadialGradient(h.x, h.y, h.r - 6, h.x, h.y, h.r + 22);
        const t1 = _.tint('255,150,60'); g.addColorStop(0, `rgba(${t1},0)`); g.addColorStop(0.3, `rgba(${t1},${Math.min(1, 0.08 * glow)})`); g.addColorStop(1, `rgba(${t1},0)`);
        lx.fillStyle = g; lx.beginPath(); lx.arc(h.x, h.y, h.r + 22, 0, TAU); lx.arc(h.x, h.y, h.r + 1.5, 0, TAU, true); lx.fill();
        _.sideWash(lx, 0.75, 70, '255,170,70', 0.55 * glow, h.r + 1);
        _.specular(lx, h.r - 1.5, 3.2, [[0, 0.42, 1], [Math.PI, 0.42, 1]], '255,165,70', 0.95 * glow, 4, 21);
        _.specular(lx, h.r + 2.4, 1.8, [[0, 0.3, 0.8], [Math.PI, 0.3, 0.8], [Math.PI * 1.3, 0.35, 0.35]], '255,232,190', 0.7 * glow, 2, 22);
      } else if (h.style === 'frame') {
        // The raised ring is struck in the author's metal; gilt -- no choice made -- is
        // still the movement's own finish, so an untouched dial keeps the palette it
        // always had and the hairlines that go with its warmth.
        const fp = h.fc && FRAME_PALS[h.fc] ? FRAME_PALS[h.fc] : pal;
        bl.beginPath(); p(bl); bl.lineWidth = 2.6; bl.strokeStyle = _.grad(bl, h.x, h.y, (b[2] - b[0]) / 2 + 3, fp); bl.stroke();
        bl.beginPath(); p(bl, -0.5, -0.6); bl.lineWidth = 0.6; bl.strokeStyle = fp === pal ? 'rgba(255,240,200,.55)' : 'rgba(255,255,255,.5)'; bl.stroke();
        bl.beginPath(); p(bl, 0.4, 0.8); bl.lineWidth = 0.6; bl.strokeStyle = 'rgba(0,0,0,.55)'; bl.stroke();
      } else if (h.style !== 'plain') {
        bl.beginPath(); p(bl); bl.lineWidth = 2.2; bl.strokeStyle = 'rgba(0,0,0,.55)'; bl.stroke();
        const CH = _.tint('255,205,130');
        bl.save(); bl.filter = 'blur(0.6px)'; bl.lineWidth = 1.3;
        const ix = Math.cos(LIGHT.inner) * 1.3, iy = Math.sin(LIGHT.inner) * 1.3;
        bl.beginPath(); p(bl, ix, iy); bl.strokeStyle = `rgba(${CH},${Math.min(1, 0.4 * glow)})`; bl.stroke();
        bl.beginPath(); p(bl, -ix, -iy); bl.strokeStyle = `rgba(${CH},${Math.min(1, 0.28 * glow)})`; bl.stroke();
        bl.restore();
      }
      erase(bl, near); ctx.drawImage(BL, 0, 0);
      if (lt) { let sp = spun(lt, LIGHT.inner); if (sp === lt && near.length) { sp = cv(S); sp.getContext('2d').drawImage(lt, 0, 0); } erase(sp.getContext('2d'), near); ctx.save(); ctx.globalCompositeOperation = 'lighter'; ctx.drawImage(sp, 0, 0); ctx.restore(); }
    });
  }

  // A dial that carries an outer calendar ring cannot also keep its chapter print and
  // its hour/minute hands out there. So the whole chapter -- numerals and minute track
  // alike -- pulls inward about the dial centre by ONE shared factor, which the hands
  // read too, and the freed band outside prints the perimeter scale. perimShrink lets
  // the user trade chapter size against ring width.
  const perimK = (D) => (D && D.perim && D.perim !== 'none' && PERIM_SPEC[D.perim]) ? Math.min(1, Math.max(0.7, +D.perimShrink || 0.88)) : 1;
  function perimGeom(D) {
    const k = perimK(D); if (k === 1) return null;
    // ring sits just inside the reed; ticks occupy the outer part of the band, labels the
    // inner part, and the hand tip is asked to stop on the ticks, not past them. perimSize
    // grows the printed label only -- the band it lives in follows it, so a wide day never
    // has to overlap the chapter print, and perimPos 'between' moves the labels into the
    // space between two divisions (with their pointer, so the pair still reads).
    const spec = PERIM_SPEC[D.perim], out = DR - 3, tl = spec.tl;
    const fs = spec.fs * Math.min(2.6, Math.max(0.6, +D.perimSize || 1)), between = D.perimPos === 'between';
    const rot = perimRotDeg(D), rotR = (rot * Math.PI) / 180;
    const tick1 = out - tl, lab = tick1 - fs * 0.52 - 3;
    // offTurn is what the pointer that reads this ring must add to its own angle: half a
    // division when the labels moved into the gaps, plus the whole ring rotation the user set.
    return { spec, k, out, inr: lab - fs * 0.62 - 3, tl, fs, between, rot, rotR, offTurn: (between ? 0.5 / spec.n : 0) + rot / 360, tick0: out, tick1, lab, reach: out - tl * 0.35 };
  }
  function drawPerimeter(ctx, D, en, holes) {
    const g = perimGeom(D); if (!g) return;
    const spec = g.spec, okc = (v) => v && v !== 'auto';
    const NI = okc(D.perimColor) ? D.perimColor : (D.perimColor === 'ink' ? en.ink : (D.numColor && D.numColor !== 'auto' ? D.numColor : en.ink));
    const radial = D.perimOrient === 'radial';
    const A = (i, n) => ((i + (g.between ? 0.5 : 0)) / n) * TAU - Math.PI / 2 + g.rotR;
    const TA = (i, n) => (i / n) * TAU - Math.PI / 2 + g.rotR;
    ctx.save(); inkCtx(ctx, en); ctx.strokeStyle = NI; ctx.fillStyle = NI;
    ctx.lineWidth = 0.6;
    ctx.beginPath(); ctx.arc(C, C, g.out + 1.6, 0, TAU); ctx.stroke();
    ctx.beginPath(); ctx.arc(C, C, g.inr - 1.6, 0, TAU); ctx.stroke();
    for (let i = 0; i < spec.n; i++) {
      const a = TA(i, spec.n), p1 = P(C, C, g.tick1, a), p2 = P(C, C, g.tick0, a);
      if (blocked(p2[0], p2[1], holes, 1)) continue;
      ctx.lineWidth = spec.maj(i) ? spec.tw : spec.qw;
      ctx.beginPath(); ctx.moveTo(p1[0], p1[1]); ctx.lineTo(p2[0], p2[1]); ctx.stroke();
    }
    const fk = D.perimFont && D.perimFont !== 'auto' && D.perimFont !== 'same' ? D.perimFont : (D.font && D.font !== 'auto' ? D.font : 'auto');
    setFace(fk);
    ctx.font = '600 ' + g.fs.toFixed(2) + 'px ' + (FONTS[fk] ? FONTS[fk][1] : SANS);
    for (let i = 0; i < spec.n; i++) {
      const a = A(i, spec.n), q = P(C, C, g.lab, a); if (blocked(q[0], q[1], holes, g.fs * 0.55)) continue;
      if (radial) { ctx.save(); ctx.translate(q[0], q[1]); ctx.rotate(a + Math.PI / 2); ctx.fillText(spec.lab(i), 0, 0.5); ctx.restore(); }
      else ctx.fillText(spec.lab(i), q[0], q[1] + 0.5);
    }
    ctx.restore();
  }
  // ---------- printing
  function inkCtx(ctx, en) { ctx.strokeStyle = en.ink; ctx.fillStyle = en.ink; ctx.globalAlpha = 0.86; ctx.textAlign = 'center'; ctx.textBaseline = 'middle'; }
  function printMain(ctx, D, en, holes, NX) {
    const show = DENSITY[D.numerals] || DENSITY.all, radial = (D.numOrient || (D.print === 'roman' ? 'radial' : 'upright')) === 'radial';
    const NI = D.numColor && D.numColor !== 'auto' ? D.numColor : en.ink, ns = Math.max(0.3, +D.numScale || 1);
    const okc = (v) => v && v !== 'auto', hS = (h) => ns * (h === 0 ? +D.n12Scale || 1 : h != null && h % 3 === 0 ? +D.nQScale || 1 : 1);
    const fam = (k) => (k && FONTS[k] && FONTS[k][1]) || null, F12 = fam(D.n12Font), FQ = fam(D.nQFont);
    const hF = (h) => { const f = h === 0 ? F12 : h != null && h % 3 === 0 ? FQ : null; if (!f) return ctx.font; const m = ctx.font.match(/^(.*?[\d.]+px)\s+(.*)$/); return m ? m[1] + ' ' + f : ctx.font; };
    const hC = (h, base) => (h === 0 && okc(D.n12Color) ? D.n12Color : h != null && h % 3 === 0 && h !== 0 && okc(D.nQColor) ? D.nQColor : base);
    ctx.save(); inkCtx(ctx, en);
    const X = NX || ctx;
    const ring = (r) => { ctx.beginPath(); ctx.arc(C, C, r, 0, TAU); ctx.stroke(); };
    const tickN = (a, r0, r1, lw, col) => { const p1 = P(C, C, r0, a), p2 = P(C, C, r1, a); if (blocked(p2[0], p2[1], holes, 1)) return; X.save(); X.lineCap = 'butt'; X.globalAlpha = NX ? 1 : ctx.globalAlpha; X.strokeStyle = col; X.lineWidth = lw; X.beginPath(); X.moveTo(p1[0], p1[1]); X.lineTo(p2[0], p2[1]); X.stroke(); X.restore(); };
    const A = (i, n) => (i / n) * TAU - Math.PI / 2;
    const tick = (a, r0, r1, lw) => { const p1 = P(C, C, r0, a), p2 = P(C, C, r1, a); if (blocked(p2[0], p2[1], holes, 1)) return; ctx.lineWidth = lw; ctx.beginPath(); ctx.moveTo(p1[0], p1[1]); ctx.lineTo(p2[0], p2[1]); ctx.stroke(); };
    const txt = (s, a, r, h) => {
      const sc = h == null ? ns : hS(h);
      const x = C + Math.cos(a) * r, y = C + Math.sin(a) * r;
      if (blocked(x, y, holes, 6 * sc)) return;
      X.save(); X.font = hF(h); X.globalAlpha = NX ? 1 : ctx.globalAlpha; X.textAlign = 'center'; X.textBaseline = 'middle';
      X.translate(x, y); if (radial) X.rotate(a + Math.PI / 2); X.scale(sc, sc); X.fillStyle = hC(h, NI);
      if (!NX) { X.shadowColor = 'rgba(0,0,0,.4)'; const sk = devS(X); X.shadowBlur = 1.3 * sk; X.shadowOffsetY = sk; } X.fillText(s, 0, 1); X.restore();
    };
    const dot = (r, rr, h) => { const q = P(C, C, r, A(h, 12)); if (blocked(q[0], q[1], holes, 2)) return; ctx.beginPath(); ctx.arc(q[0], q[1], rr, 0, TAU); ctx.fill(); };
    const P_ = D.print;
    if (P_ === 'roman') {
      ctx.lineWidth = 0.8; ring(203); ring(215);
      for (let i = 0; i < 60; i++) { const big = i % 5 === 0; tick(A(i, 60), big ? 201 : 205, 215, big ? 2.4 : 0.9); }
      ctx.globalAlpha = 0.92; ctx.font = '700 31px ' + SERIF;
      ROMAN.forEach((s, h) => { if (show(h)) txt(s, A(h, 12), 181, h); });
    } else if (P_ === 'arabic') {
      ctx.lineWidth = 0.7; ring(208); ring(216);
      for (let i = 0; i < 60; i++) tick(A(i, 60), 208, 216, i % 5 ? 0.7 : 1.8);
      for (let i = 0; i < 12; i++) dot(212, 2.1, i);
      ctx.globalAlpha = 0.92; ctx.font = 'italic 600 30px ' + SERIF;
      for (let h = 0; h < 12; h++) if (show(h)) txt(String(h || 12), A(h, 12), 190, h);
    } else if (P_ === 'regulator') {
      ctx.lineWidth = 0.8; ring(204); ring(216);
      for (let i = 0; i < 60; i++) { const big = i % 5 === 0; tick(A(i, 60), big ? 199 : 204, 216, big ? 2 : 0.8); }
      ctx.globalAlpha = 0.92; ctx.font = '600 19px ' + SERIF;
      for (let h = 0; h < 12; h++) if (show(h)) txt(String(h * 5 || 60), A(h, 12), 188, h);
    } else if (P_ === 'baton') {
      ctx.lineWidth = 0.7; ring(210); ring(218);
      for (let i = 0; i < 60; i++) tick(A(i, 60), 210, 218, i % 5 ? 0.7 : 1.6);
    } else if (P_ === 'minimal') {
      for (let i = 0; i < 60; i++) tick(A(i, 60), 211, 217, i % 5 ? 0.6 : 1.2);
      ctx.globalAlpha = 0.95;
      for (let h = 0; h < 12; h++) tickN(A(h, 12), show(h) ? 208 - 26 * hS(h) : 200, 208, show(h) ? (h % 3 ? 2.6 : 4.2) * Math.sqrt(hS(h)) : 2, hC(h, NI));
    } else if (P_ === 'sans') {
      for (let i = 0; i < 60; i++) tick(A(i, 60), 210, 217, i % 5 ? 0.6 : 1.6);
      ctx.globalAlpha = 0.95;
      for (let h = 0; h < 12; h++) {
        if (!show(h)) { dot(195, 2.8, h); continue; }
        ctx.font = '600 ' + (h % 3 ? 26 : 40) + 'px ' + SANS; txt(String(h || 12), A(h, 12), h % 3 ? 188 : 182, h);
      }
    } else if (P_ === 'sector') {
      ctx.lineWidth = 0.9; ring(217); ring(205); ring(172); ring(150);
      for (let i = 0; i < 60; i++) tick(A(i, 60), i % 5 ? 209 : 205, 217, i % 5 ? 0.8 : 2.2);
      ctx.lineWidth = 0.7;
      [0, 1, 2, 3].forEach((q) => { const a = A(q, 4); for (let d = 6; d < 150; d += 6) { const p1 = P(C, C, d - 6, a), p2 = P(C, C, d, a); if (blocked(p2[0], p2[1], holes, 1) || blocked(p1[0], p1[1], holes, 1)) continue; ctx.beginPath(); ctx.moveTo(p1[0], p1[1]); ctx.lineTo(p2[0], p2[1]); ctx.stroke(); } });
      ctx.globalAlpha = 0.95; ctx.font = '500 19px ' + SANS;
      for (let h = 0; h < 12; h++) if (show(h)) txt(String(h || 12), A(h, 12), 161, h);
      ctx.font = '500 11px ' + SANS;
      for (let i = 1; i <= 12; i++) txt(String(i * 5), A(i, 12), 191);
    } else if (P_ === 'diver') {
      ctx.lineWidth = 0.8;
      for (let i = 0; i < 60; i++) tick(A(i, 60), 209, 218, i % 5 ? 0.9 : 2.4);
      for (let h = 0; h < 12; h++) {
        if (!show(h)) continue;
        const hs = hS(h), a = A(h, 12), q = P(C, C, 204 - 16 * hs, a); if (blocked(q[0], q[1], holes, 4 * hs)) continue;
        X.save(); X.translate(q[0], q[1]); X.rotate(a + Math.PI / 2); X.scale(hs, hs); X.globalAlpha = 1; X.beginPath();
        if (h === 0) { X.moveTo(-13, -12); X.lineTo(13, -12); X.lineTo(0, 12); X.closePath(); }
        else if (h % 3 === 0) X.rect(-5.5, -16, 11, 32);
        else { X.moveTo(9, 0); X.arc(0, 0, 9, 0, TAU); }
        if (!NX) { X.shadowColor = 'rgba(0,0,0,.6)'; const sk = devS(X); X.shadowBlur = 2 * sk; X.shadowOffsetY = 1.2 * sk; }
        X.fillStyle = hC(h, okc(D.numColor) ? D.numColor : '#efe8d2'); X.fill(); X.shadowColor = 'transparent';
        if (!NX) { X.lineWidth = 1.4 / hs; X.strokeStyle = 'rgba(190,190,190,.9)'; X.stroke(); }
        X.restore();
      }
    } else if (P_ === 'station') {
      for (let i = 0; i < 60; i++) {
        const hr = i % 5 === 0, h = i / 5, big = hr && show(h), hs = big ? hS(h) : 1;
        if (big) tickN(A(i, 60), DR * (1 - 0.234 * hs), DR * 0.998, DR * 0.055 * Math.sqrt(hs), hC(h, NI));
        else tickN(A(i, 60), DR * 0.868, DR * 0.986, DR * 0.0265, NI);
      }
    } else if (P_ === 'pilot') {
      for (let i = 0; i < 60; i++) if (i % 5) tickN(A(i, 60), 197, 214, 3, NI);
      const six = show(6);
      for (let h = 0; h < 12; h++) if (!(h === 6 && six)) tickN(A(h, 12), 180, 214, 8.5 * Math.sqrt(hS(h)), hC(h, NI));
      if (six) tickN(A(6, 12), 140, 190, 8.5 * Math.sqrt(hS(6)), hC(6, NI));
      ctx.globalAlpha = 0.97; ctx.font = '600 44px ' + SANS;
      for (let h = 1; h < 12; h++) if (h !== 6 && show(h)) txt(String(h), A(h, 12), 150, h);
      if (show(0)) {
        const hs = hS(0), a = A(0, 12), q = P(C, C, 141.5, a);
        if (!blocked(q[0], q[1], holes, 12 * hs)) {
          X.save(); X.translate(q[0], q[1]); X.rotate(a + Math.PI / 2); X.scale(hs, hs); X.globalAlpha = NX ? 1 : 0.97; X.fillStyle = hC(0, NI);
          if (!NX) { X.shadowColor = 'rgba(0,0,0,.4)'; const sk = devS(X); X.shadowBlur = 1.3 * sk; X.shadowOffsetY = sk; }
          X.beginPath(); X.moveTo(0, -25.5); X.lineTo(28, 25.5); X.lineTo(-28, 25.5); X.closePath(); X.fill();
          [-1, 1].forEach((sd) => { X.beginPath(); X.arc(sd * 27, -10.5, 6.6, 0, TAU); X.fill(); });
          X.restore();
        }
      }
    } else if (P_ === 'railway') {
      ctx.lineWidth = 0.8; ring(207); ring(217);
      for (let i = 0; i < 60; i++) tick(A(i, 60), i % 5 ? 207 : 203, 217, i % 5 ? 0.7 : 2.2);
      ctx.globalAlpha = 0.95; ctx.font = '500 21px ' + SANS;
      for (let h = 0; h < 12; h++) if (show(h)) txt(String(h || 12), A(h, 12), 188, h);
    }
    ctx.restore();
  }
  function applied(ctx, D, pal, holes) {
    if (D.print !== 'baton') return;
    const show = DENSITY[D.numerals] || DENSITY.all, ns = Math.max(0.3, +D.numScale || 1);
    for (let h = 0; h < 12; h++) {
      const a = (h / 12) * TAU - Math.PI / 2, m = P(C, C, 192, a);
      if (!show(h) || blocked(m[0], m[1], holes, 2)) continue;
      (h === 0 ? [-4.5, 4.5] : [0]).forEach((o) => {
        const hs = ns * (h === 0 ? +D.n12Scale || 1 : h % 3 === 0 ? +D.nQScale || 1 : 1), w = (h % 3 === 0 ? 6 : 4.5) * Math.sqrt(hs), r0 = 204 - (h % 3 === 0 ? 28 : 20) * hs;
        ctx.save(); ctx.translate(C, C); ctx.rotate(a); ctx.translate(0, o);
        ctx.save(); ctx.shadowColor = 'rgba(0,0,0,.65)'; const sk = devS(ctx); ctx.shadowBlur = 3 * sk; [ctx.shadowOffsetX, ctx.shadowOffsetY] = away(2.1 * sk);
        ctx.fillStyle = pal.lo; ctx.fillRect(r0, -w / 2, 204 - r0, w); ctx.restore();
        const g = ctx.createLinearGradient(0, -w / 2, 0, w / 2);
        g.addColorStop(0, pal.hi); g.addColorStop(0.48, pal.mid); g.addColorStop(0.52, pal.lo); g.addColorStop(1, pal.mid);
        ctx.fillStyle = g; ctx.fillRect(r0, -w / 2, 204 - r0, w);
        ctx.restore();
      });
    }
  }
  function printSub(ctx, s, en, holes, dialFont) {
    const t = s.t || {}, { x, y, r } = s, k = r / 46, bi = s.open || 0, sz = +t.size || 1, ccw = t.dir === 'ccw', radial = t.orient === 'radial';
    setFace(t.font && t.font !== 'dial' ? t.font : dialFont);
    const wt = t.weight === 'regular' ? '500 ' : t.weight === 'italic' ? 'italic 600 ' : '700 ';
    const font = (px) => wt + Math.max(5, Math.round(px * sz)) + 'px ' + SERIF;
    ctx.save(); inkCtx(ctx, en);
    if (t.color && t.color !== 'auto') { ctx.fillStyle = t.color; ctx.strokeStyle = t.color; }
    const A = (i, n) => (ccw ? -1 : 1) * (i / n) * TAU - Math.PI / 2 + (s.rot || 0);
    const ring = (rr, lw) => { ctx.lineWidth = lw; ctx.beginPath(); ctx.arc(x, y, rr, 0, TAU); ctx.stroke(); };
    const tick = (a, r0, r1, lw) => { const m = P(x, y, (r0 + r1) / 2, a); if (blocked(m[0], m[1], holes, 1.5)) return; const p1 = P(x, y, r0, a), p2 = P(x, y, r1, a); ctx.lineWidth = lw; ctx.beginPath(); ctx.moveTo(p1[0], p1[1]); ctx.lineTo(p2[0], p2[1]); ctx.stroke(); };
    const txt = (str, a, rr) => {
      const q = P(x, y, rr, a); if (blocked(q[0], q[1], holes, 3)) return;
      if (radial) { ctx.save(); ctx.translate(q[0], q[1]); ctx.rotate(a + Math.PI / 2); ctx.fillText(str, 0, 0.5); ctx.restore(); } else ctx.fillText(str, q[0], q[1] + 0.5);
    };
    const ro = bi ? r - 3 : r - 4 * k, st = t.style || 'classic';
    if (st !== 'classic') { subDisplay(ctx, s, st, { A, ring, tick, txt, font, ro, k, bi, x, y, holes }); ctx.restore(); setFace(dialFont); return; }
    ring(ro, 0.7);
    if (s.scale === 'seconds') {
      const ri = bi ? ro - 5 : ro - 7.5 * k; ring(ri, 0.5);
      for (let i = 0; i < 60; i++) { const big = i % 5 === 0; tick(A(i, 60), big ? ri : (ri + ro) / 2, ro, big ? 2 : 0.8); }
      ctx.font = font(bi ? Math.min(12 * k, (ri - bi) * 0.8) : 12 * k);
      [60, 10, 20, 30, 40, 50].forEach((n, i) => txt(String(n), A(i, 6), bi ? (bi + ri) / 2 : r - 20 * k));
    } else if (s.scale === 'date') {
      const ri = ro - 5 * k; ring(ri, 0.5);
      for (let i = 0; i < 31; i++) tick(A(i, 31), ri, ro, (i + 1) % 5 === 0 || i === 30 ? 1.6 : 0.6);
      ctx.font = font(10 * k);
      [5, 10, 15, 20, 25, 31].forEach((n) => txt(String(n), A(n - 1, 31), r - 15 * k));
    } else if (s.scale === 'day') {
      for (let i = 0; i < 7; i++) tick(A(i, 7), ro - 3 * k, ro, 1.4);
      ctx.font = font(8.6 * k);
      DAYS.forEach((d, i) => txt(d, A(i, 7), r - 14 * k));
    } else if (s.scale === 'month') {
      for (let i = 0; i < 12; i++) tick(A(i, 12), ro - 3 * k, ro, 1.2);
      ctx.font = font(7.4 * k);
      MONTHS.forEach((m, i) => txt(m, A(i, 12), r - 13 * k));
    } else if (s.scale === 'hours') {
      const ri = ro - 5 * k; ring(ri, 0.5);
      for (let i = 0; i < 48; i++) tick(A(i, 48), i % 4 ? (ri + ro) / 2 : ri, ro, i % 4 ? 0.7 : 1.8);
      ctx.font = font(11 * k);
      for (let i = 0; i < 12; i++) txt(String(i || 12), A(i, 12), r - 16 * k);
    }
    ctx.restore();
    setFace(dialFont);
  }

  function subDisplay(ctx, s, st, o) {
    const { A, ring, tick, txt, font, ro, k, bi, x, y, holes } = o, r = s.r;
    const SC = {
      seconds: { n: 60, maj: (i) => i % 5 === 0, lab: [60, 10, 20, 30, 40, 50].map((v, i) => [String(v), i * 10]), fs: 12, lr: 20, w: 7.5, tw: 2 },
      date: { n: 31, maj: (i) => (i + 1) % 5 === 0 || i === 30, lab: [5, 10, 15, 20, 25, 31].map((v) => [String(v), v - 1]), fs: 10, lr: 15, w: 5, tw: 1.6 },
      day: { n: 7, maj: () => true, lab: DAYS.map((d, i) => [d, i]), fs: 8.6, lr: 14, w: 3, tw: 1.4 },
      month: { n: 12, maj: () => true, lab: MONTHS.map((m, i) => [m, i]), fs: 7.4, lr: 13, w: 3, tw: 1.2 },
      hours: { n: 48, maj: (i) => i % 4 === 0, lab: Array.from({ length: 12 }, (_, i) => [String(i || 12), i * 4]), fs: 11, lr: 16, w: 5, tw: 1.8 },
    }[s.scale];
    if (!SC) return;
    const ri = bi ? ro - 5 : ro - SC.w * k, many = SC.n > 12;
    let lr = bi ? (bi + ri) / 2 : r - SC.lr * k, fs = bi ? Math.min(SC.fs * k, (ri - bi) * 0.8) : SC.fs * k;
    const dot = (a, rr, rad) => { const q = P(x, y, rr, a); if (blocked(q[0], q[1], holes, 1.5)) return; ctx.beginPath(); ctx.arc(q[0], q[1], rad, 0, TAU); ctx.fill(); };
    if (st === 'bold' || st === 'numerals') { fs *= 1.4; if (!bi) lr -= SC.fs * k * 0.3; }
    if (st === 'railway') {
      const rt = ro - 3.5 * k; ring(ro, 0.7); ring(rt, 0.6);
      for (let i = 0; i < SC.n; i++) { const m = SC.maj(i); tick(A(i, SC.n), m ? rt - 2.5 * k : rt, ro, m ? SC.tw : 0.6); }
      if (!bi) lr = Math.min(lr, rt - 2.5 * k - fs * 0.8);
    } else if (st === 'dots') {
      for (let i = 0; i < SC.n; i++) { const m = SC.maj(i); if (m) dot(A(i, SC.n), ro - 2.2 * k, 1.5 * k); else if (many) dot(A(i, SC.n), ro - 2.2 * k, 0.55 * k); }
    } else if (st === 'minimal') {
      for (let i = 0; i < SC.n; i++) { const m = SC.maj(i); tick(A(i, SC.n), m ? ro - 7 * k : ro - 2.5 * k, ro, m ? SC.tw * 1.2 : 0.6); }
    } else if (st === 'bold') {
      for (let i = 0; i < SC.n; i++) if (SC.maj(i)) tick(A(i, SC.n), ro - 4 * k, ro, SC.tw * 1.7);
    } else if (st === 'sector') {
      ring(ro, 0.7); if (many) ring(ri, 0.5);
      for (let i = 0; i < SC.n; i++) { const m = SC.maj(i); tick(A(i, SC.n), many ? (m ? ri : (ri + ro) / 2) : ro - 3 * k, ro, m ? SC.tw * 0.8 : 0.6); }
      const rin = lr - fs * 0.8;
      if (rin > (bi || 6)) {
        ring(rin, 0.6);
        for (let q = 0; q < 4; q++) { const a = A(q, 4); for (let d = bi || 0; d < rin - 0.5; d += 4) tick(a, d, Math.min(rin, d + 4), 0.6); }
      }
    }
    if (st !== 'minimal') { ctx.font = font(fs); SC.lab.forEach(([str, i]) => txt(str, A(i, SC.n), lr)); }
  }
  // ---------- dial & elevated dials
  function surfaceShade(ctx, x, y, r) {
    let g = ctx.createRadialGradient(x, y, r * 0.55, x, y, r);
    g.addColorStop(0, 'rgba(255,250,235,.05)'); g.addColorStop(0.85, 'rgba(60,35,10,0)'); g.addColorStop(1, 'rgba(40,22,6,.45)');
    ctx.fillStyle = g; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
    const lx = Math.cos(LIGHT.outer) * r, ly = Math.sin(LIGHT.outer) * r, k = Math.min(1.6, LIGHT.outerK);
    g = ctx.createLinearGradient(x + lx, y + ly, x - lx, y - ly);
    g.addColorStop(0, 'rgba(255,248,225,' + 0.2 * k + ')'); g.addColorStop(0.5, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(40,22,6,.3)');
    ctx.fillStyle = g; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
  }
  // carbon fibre: woven twill (bundle brightness follows the light) or forged (random flakes)
  function carbon(ctx, x, y, r, kind, seed, T, tint, W) {
    ctx.save(); ctx.fillStyle = '#0b0c0e'; ctx.fillRect(x - r - 2, y - r - 2, 2 * r + 4, 2 * r + 4);
    const La = LIGHT.outer, tk = Math.min(1.8, T), wa = rad(W && W.ang != null ? +W.ang : 0), wsc = Math.max(0.3, W && W.sc != null ? +W.sc : 1);
    if (kind === 'carbon') {
      const c = 8 * wsc, lh = 0.3 + 0.7 * Math.abs(Math.cos(La - wa)), lv = 0.3 + 0.7 * Math.abs(Math.sin(La - wa));
      ctx.save(); ctx.translate(x, y); ctx.rotate(wa); ctx.translate(-x, -y);
      for (let gy = Math.floor((y - r * 1.45) / c) * c; gy < y + r * 1.45; gy += c) for (let gx = Math.floor((x - r * 1.45) / c) * c; gx < x + r * 1.45; gx += c) {
        if (Math.hypot(gx + c / 2 - x, gy + c / 2 - y) > r + c * 1.5) continue;
        const i = Math.round(gx / c), j = Math.round(gy / c), hz = (((i + j) % 4) + 4) % 4 < 2, b = (hz ? lh : lv) * tk;
        const g = hz ? ctx.createLinearGradient(0, gy, 0, gy + c) : ctx.createLinearGradient(gx, 0, gx + c, 0), hi = Math.round(22 + 58 * b);
        g.addColorStop(0, 'rgb(7,7,9)'); g.addColorStop(0.5, 'rgb(' + hi + ',' + hi + ',' + (hi + 4) + ')'); g.addColorStop(1, 'rgb(7,7,9)');
        ctx.fillStyle = g; ctx.fillRect(gx, gy, c + 0.25, c + 0.25);
      }
      ctx.restore();
    } else {
      const R = L().rng(seed * 7 + 11);
      for (let n = 0; n < (2400 * (r / DR) * (r / DR) + 200) / Math.max(0.5, wsc); n++) {
        const a = R() * TAU, d = Math.sqrt(R()) * (r + 6), px = x + Math.cos(a) * d, py = y + Math.sin(a) * d, rot = R() * Math.PI, w = (4 + R() * 16) * wsc, h = (2 + R() * 7) * wsc;
        const lum = Math.pow(Math.abs(Math.cos(rot - La)), 3), v = Math.round(12 + 20 * R() + 72 * lum * tk);
        ctx.save(); ctx.translate(px, py); ctx.rotate(rot); ctx.beginPath();
        for (let q = 0; q < 5; q++) { const aa = (q / 5) * TAU, rr = 0.6 + R() * 0.5, qx = (Math.cos(aa) * w) / 2 * rr, qy = (Math.sin(aa) * h) / 2 * rr; q ? ctx.lineTo(qx, qy) : ctx.moveTo(qx, qy); }
        ctx.closePath(); ctx.fillStyle = 'rgba(' + v + ',' + v + ',' + (v + 3) + ',.88)'; ctx.fill(); ctx.restore();
      }
    }
    if (tint) {
      ctx.globalCompositeOperation = 'color'; ctx.globalAlpha = 0.9; ctx.fillStyle = tint; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
      ctx.globalCompositeOperation = 'soft-light'; ctx.globalAlpha = 0.55; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
      ctx.globalCompositeOperation = 'source-over'; ctx.globalAlpha = 1;
    }
    const g = ctx.createLinearGradient(x - r, y - r, x + r, y + r);
    g.addColorStop(0, 'rgba(255,255,255,.09)'); g.addColorStop(0.5, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,.28)');
    ctx.fillStyle = g; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
    ctx.restore();
  }
  // shared by the main dial and every subdial: base colour, finish texture, then light
  function surface(ctx, s, en, seed, recess) {
    const _ = L(), { x, y, r } = s, T = s.tex == null ? 1 : +s.tex;
    ctx.beginPath(); ctx.arc(x, y, r, 0, TAU);
    if (s.style === 'carbon' || s.style === 'forged') { ctx.save(); ctx.clip(); carbon(ctx, x, y, r, s.style, seed, T, en.tint, s.weave); ctx.restore(); }
    else if (s.style === 'metal') {
      const g0 = ctx.createLinearGradient(x - r, y - r, x + r, y + r);
      g0.addColorStop(0, '#ece9e2'); g0.addColorStop(0.5, '#aeaaa2'); g0.addColorStop(1, '#5f5c56');
      ctx.fillStyle = g0; ctx.fill();
      if (en.tint) { ctx.save(); ctx.clip(); ctx.globalCompositeOperation = 'multiply'; ctx.globalAlpha = 0.75; ctx.fillStyle = en.tint; ctx.fillRect(x - r, y - r, 2 * r, 2 * r); ctx.restore(); }
    } else _.enamelFill(ctx, x, y, r, Object.assign({}, en, { stain: 0 }), seed);
    ctx.save(); ctx.beginPath(); ctx.arc(x, y, r, 0, TAU); ctx.clip();
    const lite = (en.dark ? 0.13 : 0.19) * T, dk = (en.dark ? 0.26 : 0.16) * T;
    if (s.style === 'guilloche') {
      const n = Math.round(r * 2.6); ctx.lineWidth = 0.6;
      for (let i = 0; i < n; i++) { const q = P(x, y, r, (i / n) * TAU); ctx.strokeStyle = i % 2 ? 'rgba(255,250,235,' + lite + ')' : 'rgba(0,0,0,' + dk + ')'; ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(q[0], q[1]); ctx.stroke(); }
      const cg = ctx.createConicGradient(LIGHT.outer, x, y);
      [[0, 0.24], [0.25, 0], [0.5, 0.24], [0.75, 0], [1, 0.24]].forEach(([p, a]) => cg.addColorStop(p, 'rgba(255,250,235,' + Math.min(0.8, a * T) + ')'));
      ctx.fillStyle = cg; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
      const cd = ctx.createConicGradient(LIGHT.outer, x, y);
      [[0, 0], [0.25, 0.2], [0.5, 0], [0.75, 0.2], [1, 0]].forEach(([p, a]) => cd.addColorStop(p, 'rgba(0,0,0,' + Math.min(0.7, a * T) + ')'));
      ctx.fillStyle = cd; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
    } else if (s.style === 'azure' || s.style === 'metal') {
      ctx.lineWidth = 0.7;
      for (let q = 2, i = 0; q < r; q += s.style === 'metal' ? 1.2 : 1.5, i++) { ctx.strokeStyle = i % 2 ? 'rgba(255,250,235,' + lite + ')' : 'rgba(0,0,0,' + dk * 0.8 + ')'; ctx.beginPath(); ctx.arc(x, y, q, 0, TAU); ctx.stroke(); }
      const cz = ctx.createConicGradient(LIGHT.outer + Math.PI / 2, x, y);
      [[0, 0.2], [0.07, 0], [0.43, 0], [0.5, 0.2], [0.57, 0], [0.93, 0], [1, 0.2]].forEach(([p, a]) => cz.addColorStop(p, 'rgba(255,250,235,' + Math.min(0.7, a * T) + ')'));
      ctx.fillStyle = cz; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
    } else if (s.style === 'matte') {
      const R = _.rng(seed * 7 + 3), n = Math.round(r * r * 0.5);
      for (let i = 0; i < n; i++) { const a = R() * TAU, d = Math.sqrt(R()) * r; ctx.fillStyle = R() < 0.5 ? 'rgba(0,0,0,' + R() * 0.12 * T + ')' : 'rgba(255,255,255,' + R() * 0.1 * T + ')'; ctx.fillRect(x + Math.cos(a) * d, y + Math.sin(a) * d, 1, 1); }
    }
    surfaceShade(ctx, x, y, r);
    if (recess) {
      const g = ctx.createRadialGradient(x, y, r * 0.75, x, y, r);
      g.addColorStop(0, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,.25)');
      ctx.fillStyle = g; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
    }
    ctx.restore();
  }
  // ageing (patina, yellowing, crazing), stains (blotches, pits, chips) and dust (scratches, specks), each 0…1.5
  const wearOf = (D) => ({ age: D.aging == null ? 1 : +D.aging, stain: D.stains == null ? 1 : +D.stains, dust: D.dust == null ? 1 : +D.dust });
  function weather(ctx, x, y, rIn, rOut, seed, en, W, sc) {
    const age = W.age * sc, st = W.stain * sc, du = W.dust * sc;
    if (age + st + du <= 0.001) return;
    const R = L().rng(seed), dark = !!en.dark;
    const GR = dark ? '120,104,78' : '48,28,8', BL = dark ? '140,120,88' : '70,44,16', AR = dark ? '150,132,100' : '50,30,10';
    ctx.save(); ctx.beginPath(); ctx.arc(x, y, rOut, 0, TAU); if (rIn > 0) ctx.arc(x, y, rIn, 0, TAU, true); ctx.clip('evenodd');
    const pt = () => { const a = R() * TAU, d = rIn + Math.sqrt(R()) * (rOut - rIn); return [x + Math.cos(a) * d, y + Math.sin(a) * d, a, d]; };
    if (age > 0) {
      const ga = dark ? 0.35 : 1, g = ctx.createRadialGradient(x, y, rIn, x, y, rOut);
      g.addColorStop(0, 'rgba(' + GR + ',' + 0.55 * age * ga + ')'); g.addColorStop(0.16, 'rgba(' + GR + ',0)');
      g.addColorStop(0.8, 'rgba(' + GR + ',0)'); g.addColorStop(1, 'rgba(' + GR + ',' + 0.6 * age * ga + ')');
      ctx.fillStyle = g; ctx.fillRect(0, 0, S, S);
      if (!dark) { ctx.globalCompositeOperation = 'multiply'; ctx.fillStyle = 'rgba(205,165,95,' + Math.min(0.35, 0.09 * age) + ')'; ctx.fillRect(0, 0, S, S); ctx.globalCompositeOperation = 'source-over'; }
      ctx.lineCap = 'round';
      for (let i = 0; i < 70 * age; i++) {
        let [px, py] = pt(), dir = R() * TAU;
        ctx.beginPath(); ctx.moveTo(px, py);
        const n = 3 + ((R() * 9) | 0);
        for (let j = 0; j < n; j++) { dir += (R() - 0.5) * 1.6; px += Math.cos(dir) * (3 + R() * 7); py += Math.sin(dir) * (3 + R() * 7); ctx.lineTo(px, py); }
        ctx.strokeStyle = dark ? 'rgba(190,172,140,' + (0.04 + R() * 0.07) * age + ')' : 'rgba(60,38,14,' + (0.12 + R() * 0.22) * age + ')'; ctx.lineWidth = 0.45; ctx.stroke();
      }
      for (let i = 0; i < 40 * age; i++) {
        const [, , a, d] = pt(), span = 0.05 + R() * 0.35;
        ctx.strokeStyle = 'rgba(' + AR + ',' + (0.06 + R() * 0.12) * age * (dark ? 0.6 : 1) + ')'; ctx.lineWidth = 0.4;
        ctx.beginPath(); ctx.arc(x, y, d, a, a + span); ctx.stroke();
      }
    }
    if (st > 0) {
      for (let i = 0; i < 30 * st; i++) {
        const [px, py] = pt(), sr = 10 + R() * 40;
        ctx.save(); ctx.translate(px, py); ctx.rotate(R() * TAU); ctx.scale(1, 0.35 + R() * 0.65);
        const q = ctx.createRadialGradient(0, 0, 0, 0, 0, sr);
        q.addColorStop(0, 'rgba(' + BL + ',' + (0.06 + R() * 0.12) * st * (dark ? 0.6 : 1) + ')'); q.addColorStop(1, 'rgba(' + BL + ',0)');
        ctx.fillStyle = q; ctx.fillRect(-sr, -sr, sr * 2, sr * 2); ctx.restore();
      }
      for (let i = 0; i < 900 * st; i++) {
        const [px, py] = pt();
        ctx.fillStyle = (dark ? 'rgba(32,29,25,' : 'rgba(232,220,192,') + (0.3 + R() * 0.5) * Math.min(1, st) + ')';
        ctx.fillRect(px, py, 0.6 + R() * 1.4, 0.6 + R() * 1.2);
      }
      for (let i = 0; i < 400 * st; i++) {
        const [px, py] = pt();
        ctx.fillStyle = dark ? 'rgba(170,150,115,' + (0.05 + R() * 0.12) + ')' : 'rgba(60,40,18,' + (0.08 + R() * 0.2) + ')';
        ctx.fillRect(px, py, 0.5 + R() * 1.2, 0.5 + R());
      }
      for (let i = 0; i < 14 * st; i++) {
        const [px, py] = pt(), cr = 0.6 + Math.pow(R(), 2) * 2.6;
        ctx.beginPath();
        for (let j = 0; j < 7; j++) { const a = (j / 7) * TAU, rr = cr * (0.6 + R() * 0.7); j ? ctx.lineTo(px + Math.cos(a) * rr, py + Math.sin(a) * rr) : ctx.moveTo(px + Math.cos(a) * rr, py + Math.sin(a) * rr); }
        ctx.closePath(); ctx.fillStyle = dark ? 'rgba(128,116,96,' + (0.5 + R() * 0.35) + ')' : 'rgba(28,16,5,' + (0.45 + R() * 0.35) + ')'; ctx.fill();
        ctx.strokeStyle = dark ? 'rgba(0,0,0,.5)' : 'rgba(255,250,235,.35)'; ctx.lineWidth = 0.5; ctx.stroke();
      }
    }
    if (du > 0) {
      for (let i = 0; i < 140 * du; i++) {
        const [px, py] = pt(), dir = R() * TAU, len = 3 + Math.pow(R(), 2) * 38, cvv = (R() - 0.5) * 0.6;
        const ex = px + Math.cos(dir) * len, ey = py + Math.sin(dir) * len, mx = (px + ex) / 2 + Math.cos(dir + 1.57) * len * cvv, my = (py + ey) / 2 + Math.sin(dir + 1.57) * len * cvv;
        ctx.lineWidth = 0.35 + R() * 0.45;
        ctx.strokeStyle = dark ? 'rgba(255,240,210,' + (0.06 + R() * 0.12) * du + ')' : 'rgba(40,24,8,' + (0.1 + R() * 0.2) * du + ')';
        ctx.beginPath(); ctx.moveTo(px, py); ctx.quadraticCurveTo(mx, my, ex, ey); ctx.stroke();
        ctx.strokeStyle = dark ? 'rgba(0,0,0,' + (0.2 + R() * 0.3) * du + ')' : 'rgba(255,252,240,' + (0.1 + R() * 0.2) * du + ')';
        ctx.beginPath(); ctx.moveTo(px + 0.5, py + 0.5); ctx.quadraticCurveTo(mx + 0.5, my + 0.5, ex + 0.5, ey + 0.5); ctx.stroke();
      }
      for (let i = 0; i < 1400 * du; i++) {
        const [px, py] = pt();
        ctx.fillStyle = R() < 0.5 ? (dark ? 'rgba(0,0,0,' + R() * 0.35 + ')' : 'rgba(50,30,10,' + R() * 0.25 + ')') : 'rgba(255,250,235,' + R() * (dark ? 0.1 : 0.18) + ')';
        ctx.fillRect(px, py, 0.6 + R() * 0.8, 0.6 + R() * 0.8);
      }
    }
    ctx.restore();
  }
  const surfaceEn = (en, style) => (style === 'metal' ? Object.assign({}, en, { ink: '#1a140c', dark: 0 }) : style === 'carbon' || style === 'forged' ? Object.assign({}, en, { ink: '#f2ede2', dark: 1 }) : en);
  // ---------- flares: logos, icons and doodads printed or applied onto their host
  const FW = { bold: '700', regular: '500', italic: 'italic 600' };
  function flareMask(f, date) {
    const m = cv(S), x = m.getContext('2d'), s = (+f.size || 16) * (f.icon === 'arctext' ? 1 : +f.fgScale || 1), txt = f.icon === 'datetext' ? fmtDate(f.fmt == null ? 'ddd DD MMM' : f.fmt, date || new Date(), f.fcase) : String(f.text == null ? 'BRAND' : f.text);
    const fam = (FONTS[f.font] && FONTS[f.font][1]) || SANS, font = (FW[f.weight] || '700') + ' ' + (s * 2).toFixed(1) + 'px ' + fam;
    x.fillStyle = '#fff'; x.textAlign = 'center'; x.textBaseline = 'middle';
    if (f.icon === 'arctext') {
      const R = Math.max(10, Math.hypot(f.x - f.hx, f.y - f.hy)), a0 = Math.atan2(f.x - f.hx, -(f.y - f.hy)), low = Math.cos(a0) < -0.01, sp = (+f.spacing || 0) * s;
      x.font = font;
      const ch = [...txt], ws = ch.map((c) => x.measureText(c).width + sp), tot = ws.reduce((a, b) => a + b, 0) - sp;
      let u = -tot / 2;
      ch.forEach((c, i) => {
        const da = (u + (ws[i] - sp) / 2) / R, an = low ? a0 - da : a0 + da;
        x.save(); x.translate(f.hx + Math.sin(an) * R, f.hy - Math.cos(an) * R); x.rotate(an + (low ? Math.PI : 0)); x.fillText(c, 0, 0); x.restore();
        u += ws[i];
      });
      return m;
    }
    x.translate(f.x, f.y); x.rotate(f.rot || 0);
    if (f.icon === 'text' || f.icon === 'datetext') { x.font = font; if (sp0(f)) x.letterSpacing = sp0(f) * s + 'px'; x.fillText(txt, 0, 0); }
    else if (f.icon === 'image') {
      const im = IMAGES[f.img];
      if (im && im.width) { const k = (2 * s) / Math.max(im.width, im.height); x.drawImage(im, (-im.width * k) / 2, (-im.height * k) / 2, im.width * k, im.height * k); }
      else { x.strokeStyle = '#fff'; x.setLineDash([3, 3]); x.lineWidth = 1.2; x.strokeRect(-s, -s, 2 * s, 2 * s); }
    } else {
      if (!FLARE_P) FLARE_P = Object.fromEntries(Object.entries(FLARE_SVG).map(([k, d]) => [k, new Path2D(d)]));
      x.scale(s / 12, s / 12); x.translate(-12, -12); x.fill(FLARE_P[f.icon] || FLARE_P.star, 'evenodd');
    }
    return m;
  }
  const sp0 = (f) => +f.spacing || 0;
  function finishMask(ctx, m, st, o) {
    const t = cv(S), tx = t.getContext('2d'); tx.drawImage(m, 0, 0);
    if (o.fill) { tx.globalCompositeOperation = 'source-in'; tx.fillStyle = o.fill; tx.fillRect(0, 0, S, S); }
    const [ax, ay] = away(1);
    const edge = (dx, dy, col) => { const e = cv(S), ex = e.getContext('2d'); ex.drawImage(m, 0, 0); ex.globalCompositeOperation = 'destination-out'; ex.drawImage(m, dx, dy); ex.globalCompositeOperation = 'source-in'; ex.fillStyle = col; ex.fillRect(0, 0, S, S); tx.globalCompositeOperation = 'source-atop'; tx.drawImage(e, 0, 0); };
    if (st === 'applied') { edge(ax * 1.3, ay * 1.3, 'rgba(255,250,235,.6)'); edge(-ax * 1.3, -ay * 1.3, 'rgba(0,0,0,.5)'); }
    if (st === 'engraved') { edge(ax * 1.1, ay * 1.1, 'rgba(0,0,0,.55)'); edge(-ax, -ay, 'rgba(255,250,235,.22)'); }
    if (st === 'lume') edge(-ax * 0.8, -ay * 0.8, 'rgba(0,0,0,.18)');
    const op = o.op == null ? 1 : +o.op, ht = +o.height || 0;
    if (ht > 0) {
      ctx.save(); ctx.globalAlpha = op;
      [[1, 0.55, 1.2], [2.6, 0.32, 3.2]].forEach(([d, a, b]) => { ctx.save(); ctx.shadowColor = 'rgba(0,0,0,' + Math.min(0.9, a * Math.min(1.6, 0.5 + ht * 0.5)) + ')'; ctx.shadowBlur = b * (0.6 + ht * 0.8); [ctx.shadowOffsetX, ctx.shadowOffsetY] = away(d * ht); ctx.drawImage(t, 0, 0); ctx.restore(); });
      ctx.restore();
    }
    ctx.save(); ctx.globalAlpha = op;
    if (st === 'applied') {
      [[2.2, 3, 0.6], [0.7, 0.8, 0.5]].forEach(([d, b, a]) => { ctx.save(); ctx.shadowColor = 'rgba(0,0,0,' + a + ')'; ctx.shadowBlur = b; [ctx.shadowOffsetX, ctx.shadowOffsetY] = away(d); ctx.drawImage(t, 0, 0); ctx.restore(); });
      ctx.drawImage(t, 0, 0);
    } else if (st === 'lume') {
      ctx.shadowColor = o.glow || '#e8f4cf'; ctx.shadowBlur = 5; ctx.drawImage(t, 0, 0);
      ctx.shadowColor = 'rgba(0,0,0,.45)'; ctx.shadowBlur = 1.5; ctx.shadowOffsetY = 1; ctx.drawImage(t, 0, 0);
    } else if (st === 'engraved') {
      const hl = cv(S), hx = hl.getContext('2d'); hx.drawImage(m, 0, 0); hx.globalCompositeOperation = 'source-in'; hx.fillStyle = 'rgba(255,250,235,.4)'; hx.fillRect(0, 0, S, S);
      const [ox, oy] = away(0.9); ctx.drawImage(hl, ox, oy); ctx.globalAlpha = op * 0.92; ctx.drawImage(t, 0, 0);
    } else { if (ht >= 0) { ctx.shadowColor = 'rgba(0,0,0,.35)'; ctx.shadowBlur = 1.3; ctx.shadowOffsetY = 1; } ctx.globalAlpha = op * 0.92; ctx.drawImage(t, 0, 0); }
    ctx.restore();
    if (ht < 0) {
      // sunk: the dial's edge casts into the recess, the far wall catches light
      const dd = -ht, I = cv(S), ix = I.getContext('2d');
      ix.save(); [ix.shadowOffsetX, ix.shadowOffsetY] = away(1.6 * dd); ix.shadowBlur = 1.2 + dd * 1.6; ix.shadowColor = 'rgba(0,0,0,' + Math.min(0.85, 0.45 + dd * 0.2) + ')';
      const inv = cv(S), vx = inv.getContext('2d'); vx.fillStyle = '#000'; vx.fillRect(0, 0, S, S); vx.globalCompositeOperation = 'destination-out'; vx.drawImage(m, 0, 0);
      ix.drawImage(inv, 0, 0); ix.restore();
      ix.globalCompositeOperation = 'destination-in'; ix.drawImage(m, 0, 0);
      const e = cv(S), ex = e.getContext('2d'); ex.drawImage(m, 0, 0); ex.globalCompositeOperation = 'destination-out'; const [lx, ly] = away(-1.1 * Math.min(2, dd)); ex.drawImage(m, lx, ly); ex.globalCompositeOperation = 'source-in'; ex.fillStyle = 'rgba(255,250,240,.35)'; ex.fillRect(0, 0, S, S);
      ctx.save(); ctx.globalAlpha = op; ctx.drawImage(I, 0, 0); ctx.drawImage(e, 0, 0); ctx.restore();
    }
  }
  function drawFlares(ctx, list, en, pal, date) {
    const _ = L();
    list.forEach((f) => {
      const m = f.color === 'none' ? null : flareMask(f, date), st = f.style || 'print', s = +f.size || 16, ht = f.height == null ? 0 : +f.height;
      const auto = !f.color || f.color === 'auto', own = f.icon === 'image' && auto && IMAGES[f.img];
      const col = !auto ? f.color : st === 'lume' ? '#e8f4cf' : en.ink;
      if (f.bg && f.bg !== 'none' && f.icon !== 'arctext') {
        const bm = cv(S), bx = bm.getContext('2d'), w = 2 * s * (+f.bgW || 1.6), hh = 2 * s * (+f.bgH || 1), sh = f.bgShape || 'rect';
        bx.translate(f.x + (+f.bgX || 0) * s, f.y + (+f.bgY || 0) * s); bx.rotate(f.rot || 0); bx.fillStyle = '#fff'; bx.beginPath();
        if (sh === 'circle') bx.arc(0, 0, Math.max(w, hh) / 2, 0, TAU);
        else if (sh === 'pill' || sh === 'rounded') { const rr = sh === 'pill' ? Math.min(w, hh) / 2 : Math.min(w, hh) * 0.18; bx.roundRect(-w / 2, -hh / 2, w, hh, rr); }
        else bx.rect(-w / 2, -hh / 2, w, hh);
        bx.fill();
        finishMask(ctx, bm, st === 'applied' || st === 'engraved' ? st : 'print', { fill: f.bg, op: f.opacity == null ? 1 : +f.opacity, height: ht });
      }
      if (f.color === 'none') return;
      finishMask(ctx, m, st, { height: f.bg && f.bg !== 'none' ? 0 : ht, fill: own ? null : st === 'applied' && auto ? _.grad(ctx, f.x, f.y, s * 1.2, pal) : col, glow: col, op: f.opacity == null ? 1 : +f.opacity });
    });
  }
  // openwork: keep only a bridge framework of the dial; the movement shows through everything else
  function openwork(ctx, D, lay) {
    const pat = D.openwork, bw = +D.owBar || 20, ri = +D.owRing || 168, hub = +D.owHub || 36, rim = +D.owRim || 10;
    const K = cv(S), k = K.getContext('2d');
    k.fillStyle = '#fff'; k.strokeStyle = '#fff'; k.lineCap = 'butt'; k.lineJoin = 'round';
    k.beginPath(); k.arc(C, C, DR + 4, 0, TAU); k.arc(C, C, ri, 0, TAU, true); k.fill();
    const spokes = { x: [45, 135, 225, 315], xarch: [45, 135, 225, 315], cross: [0, 90, 180, 270], star: [0, 60, 120, 180, 240, 300], ring: [] }[pat] || [];
    if (spokes.length) {
      k.lineWidth = bw * 0.62; k.beginPath(); k.arc(C, C, hub - bw * 0.31, 0, TAU); k.stroke();
      k.lineWidth = bw; spokes.forEach((d) => { const a = rad(d - 90); k.beginPath(); k.moveTo(...P(C, C, hub - 3, a)); k.lineTo(...P(C, C, ri + 3, a)); k.stroke(); });
    }
    const ar = ri * 0.62;
    if (pat === 'xarch') { k.lineWidth = bw * 0.85; k.beginPath(); k.arc(C, C, ar, rad(-90 - 44), rad(-90 + 44)); k.stroke(); }
    k.lineWidth = rim * 2;
    lay.dialHoles.forEach((h) => { k.beginPath(); holePath(h)(k); k.stroke(); });
    lay.subs.forEach((sb) => { if (sb.transparent) return; k.lineWidth = rim + 3; k.beginPath(); k.arc(sb.x, sb.y, sb.r + rim / 2, 0, TAU); k.stroke(); });
    // brushed-in inlay down the middle of each bar
    if (+D.owInlay) {
      const I = cv(S), ix = I.getContext('2d'); ix.strokeStyle = '#fff'; ix.lineCap = 'round'; ix.lineWidth = bw * 0.42;
      spokes.forEach((d) => { const a = rad(d - 90); ix.beginPath(); ix.moveTo(...P(C, C, hub + bw * 0.5, a)); ix.lineTo(...P(C, C, ri - bw * 0.4, a)); ix.stroke(); });
      ix.globalCompositeOperation = 'source-in'; ix.fillStyle = '#0c0c0e'; ix.fillRect(0, 0, S, S); ix.globalCompositeOperation = 'source-atop';
      const R = L().rng(91); for (let i = 0; i < 9000; i++) { ix.fillStyle = 'rgba(255,255,255,' + (R() * R() * 0.55).toFixed(3) + ')'; ix.fillRect(R() * S, R() * S, 1, 1); }
      ix.globalCompositeOperation = 'destination-out'; const [ax, ay] = away(1);
      ctx.save(); ctx.drawImage(I, 0, 0);
      const e = cv(S), ex = e.getContext('2d'); ex.drawImage(I, 0, 0); ex.globalCompositeOperation = 'destination-out'; ex.drawImage(I, -ax * 1.4, -ay * 1.4); ex.globalCompositeOperation = 'source-in'; ex.fillStyle = 'rgba(0,0,0,.7)'; ex.fillRect(0, 0, S, S);
      ctx.drawImage(e, 0, 0); ctx.restore();
    }
    ctx.save(); ctx.globalCompositeOperation = 'destination-in'; ctx.drawImage(K, 0, 0); ctx.restore();
    // chamfered edges catch the light, then the framework casts onto the movement below
    const [ax, ay] = away(1);
    const edge = (dx, dy, col, w) => { const e = cv(S), ex = e.getContext('2d'); ex.drawImage(K, 0, 0); ex.globalCompositeOperation = 'destination-out'; ex.drawImage(K, dx * w, dy * w); ex.globalCompositeOperation = 'source-in'; ex.fillStyle = col; ex.fillRect(0, 0, S, S); ctx.save(); ctx.globalCompositeOperation = 'source-atop'; ctx.drawImage(e, 0, 0); ctx.restore(); };
    edge(ax, ay, 'rgba(255,252,245,.55)', 1.6); edge(-ax, -ay, 'rgba(0,0,0,.6)', 1.8);
    const dp = +D.owDepth || 1, sh = cv(S), sx = sh.getContext('2d');
    [[6, 5, 0.8], [16, 16, 0.5], [30, 30, 0.3]].forEach(([d, b, a]) => { sx.save(); sx.shadowColor = 'rgba(0,0,0,' + a + ')'; sx.shadowBlur = b * dp; sx.shadowOffsetX = ax * d * dp + 2000; sx.shadowOffsetY = ay * d * dp; sx.drawImage(K, -2000, 0); sx.restore(); });
    sx.globalCompositeOperation = 'destination-out'; sx.drawImage(K, 0, 0);
    sx.globalCompositeOperation = 'destination-in'; sx.beginPath(); sx.arc(C, C, DR, 0, TAU); sx.fill();
    ctx.save(); ctx.globalCompositeOperation = 'destination-over'; ctx.drawImage(sh, 0, 0); ctx.restore();
  }
  function buildDial(D, lay, baseEn, pal, glow, seal, clipR, dynK) {
    const _en = surfaceEn(colorEn(baseEn, D.color), D.surface);
    const kk = dynK == null ? 1 : +dynK;
    if (kk !== 1) return buildDialLens(D, lay, _en, pal, glow, seal, clipR, kk);
    const c = cv(S), ctx = c.getContext('2d'), en = _en;
    surface(ctx, { x: C, y: C, r: DR, style: D.surface || 'enamel', tex: D.texture, weave: { ang: D.weaveAngle, sc: D.weaveScale } }, en, 11, false);
    /* Authored art on the dial face: over the surface material, under every mark the
     * dial prints itself. Everything the user can still add by hand -- chapter print,
     * numerals, applied markers, the perimeter ring -- is drawn after it, and openwork
     * and the aperture punches cut through it, so an art dial reads as a printed dial
     * rather than a picture laid on top. Costs nothing per tick: compose() already
     * blends this one canvas, so the art only changes pixels in a layer that exists. */
    artInDial(ctx, D.sfArt, { x: D.sfX, y: D.sfY, size: D.sfSize, fit: D.sfFit, zoom: D.sfZoom, opacity: D.sfOpacity, clipR: clipR });
    const nst = D.print !== 'baton' && D.print !== 'none' && D.numStyle && D.numStyle !== 'print' ? D.numStyle : null, NM = nst ? cv(S) : null;
    // The perimeter ring lives at the rim, so the chapter print must pull inward to clear
    // it. Scale BOTH the ink context and the numeral-mask context by the same factor about
    // the dial centre, and carry the holes into that same scaled space -- printMain tests
    // its own ink against them, and an unscaled hole list would cull the wrong ticks.
    const pk = perimK(D), holes = lay.dialHoles;
    const pkHoles = pk === 1 ? holes : holes.map((h) => Object.assign({}, h, { x: C + (h.x - C) * pk, y: C + (h.y - C) * pk, r: (h.r || 0) * pk, w: h.w * pk, h: h.h * pk }));
    const push = (x2) => { if (pk === 1 || !x2) return null; x2.save(); x2.translate(C, C); x2.scale(pk, pk); x2.translate(-C, -C); return x2; };
    const pop = (x2) => { if (x2) x2.restore(); };
    const cxP = push(ctx), nxP = NM && push(NM.getContext('2d'));
    printMain(ctx, D, en, pkHoles, NM && NM.getContext('2d'));
    pop(cxP); pop(nxP);
    if (pk !== 1) drawPerimeter(ctx, D, en, holes);
    weather(ctx, C, C, lay.wearIn || 0, DR, 57, en, wearOf(D), 1.5 * (lay.wearIn ? 1 : 0.75));
    applied(ctx, D, pal, lay.dialHoles);
    if (NM) {
      const okc = (v) => v && v !== 'auto', explicit = [D.numColor, D.n12Color, D.nQColor].some(okc);
      finishMask(ctx, NM, nst, { fill: explicit ? null : nst === 'applied' ? L().grad(ctx, C, C, DR * 0.9, pal) : nst === 'lume' ? '#e8f4cf' : null, glow: okc(D.numColor) ? D.numColor : '#e8f4cf' });
    }
    if (D.openwork && D.openwork !== 'none') openwork(ctx, D, lay);
    // flares hosted on a transparent subdial are carried by the dial, which is the
    // only shipped plane left at that spot once the plate is dropped
    const thost = {}; lay.subs.forEach((sb) => { if (sb.transparent) thost[sb.id] = 1; });
    drawFlares(ctx, lay.flares.filter((f) => (f.host === 'dial' || thost[f.host]) && f.icon !== 'datetext'), en, pal);
    // seal: an opaque date/moon disc covers its aperture, so the shipped dial keeps
    // the surface uncut there. Only genuine openings stay punched -- the seal flag
    // (layout) is set on dial-hosted windows/moons and never on an 'opening'.
    cutHoles(ctx, seal ? lay.dialHoles.filter((h) => !h.seal) : lay.dialHoles, pal, glow);
    return { canvas: c, en };
  }
  /* The dial baked through the opening lens -- dynScale under "no case". The engine's own
   * drawing is eased in to fit inside the ring the guide paints, and the author's uploaded
   * art is not: it is painted at exactly the size and place they left it, the one promise
   * of the option. Every other block here is authored against the full dial circle, so each
   * is drawn through one scale about the centre -- the same push/pop the perimeter ring
   * already uses, extended. Erasures (apertures, openwork) run through the same transform,
   * so they cut the art at the same scaled position as everything else and the movement
   * and data discs under a window stay aligned with it. One canvas, one blend per tick. */
  function buildDialLens(D, lay, en, pal, glow, seal, clipR, kk) {
    const c = cv(S), ctx = c.getContext('2d');
    const lens = () => { ctx.save(); ctx.translate(C, C); ctx.scale(kk, kk); ctx.translate(-C, -C); };
    const unlens = () => { ctx.restore(); };
    lens(); surface(ctx, { x: C, y: C, r: DR, style: D.surface || 'enamel', tex: D.texture, weave: { ang: D.weaveAngle, sc: D.weaveScale } }, en, 11, false); unlens();
    artInDial(ctx, D.sfArt, { x: D.sfX, y: D.sfY, size: D.sfSize, fit: D.sfFit, zoom: D.sfZoom, opacity: D.sfOpacity, clipR: clipR });
    const nst = D.print !== 'baton' && D.print !== 'none' && D.numStyle && D.numStyle !== 'print' ? D.numStyle : null, NM = nst ? cv(S) : null;
    // chapter print and its holes go through the lens together -- the same pair perimK scales
    const pk = perimK(D), pkd = pk * kk, holes = lay.dialHoles;
    const pkHoles = pkd === 1 ? holes : holes.map((h) => Object.assign({}, h, { x: C + (h.x - C) * pkd, y: C + (h.y - C) * pkd, r: (h.r || 0) * pkd, w: h.w * pkd, h: h.h * pkd }));
    const push = (x2) => { if (pkd === 1 || !x2) return null; x2.save(); x2.translate(C, C); x2.scale(pkd, pkd); x2.translate(-C, -C); return x2; };
    const pop = (x2) => { if (x2) x2.restore(); };
    const cxP = push(ctx), nxP = NM && push(NM.getContext('2d'));
    printMain(ctx, D, en, pkHoles, NM && NM.getContext('2d'));
    pop(cxP); pop(nxP);
    lens();
    if (pk !== 1) drawPerimeter(ctx, D, en, holes);
    weather(ctx, C, C, lay.wearIn || 0, DR, 57, en, wearOf(D), 1.5 * (lay.wearIn ? 1 : 0.75));
    applied(ctx, D, pal, lay.dialHoles);
    unlens();
    // NM was filled through the push above -- composited at identity, like the plain path, or it lands scaled twice
    if (NM) {
      const okc = (v) => v && v !== 'auto', explicit = [D.numColor, D.n12Color, D.nQColor].some(okc);
      finishMask(ctx, NM, nst, { fill: explicit ? null : nst === 'applied' ? L().grad(ctx, C, C, DR * 0.9 * pkd, pal) : nst === 'lume' ? '#e8f4cf' : null, glow: okc(D.numColor) ? D.numColor : '#e8f4cf' });
    }
    lens();
    if (D.openwork && D.openwork !== 'none') openwork(ctx, D, lay);
    const thost = {}; lay.subs.forEach((sb) => { if (sb.transparent) thost[sb.id] = 1; });
    drawFlares(ctx, lay.flares.filter((f) => (f.host === 'dial' || thost[f.host]) && f.icon !== 'datetext'), en, pal);
    cutHoles(ctx, seal ? lay.dialHoles.filter((h) => !h.seal) : lay.dialHoles, pal, glow);
    unlens();
    return { canvas: c, en };
  }
  // one canvas per subdial, so list order is stacking order (later covers earlier) and each one's openings only cut itself
  function buildSubdials(D, lay, baseEn, pal, glow, SH) {
    const _ = L(), dialFont = D.font || 'auto';
    return lay.subs.map((s, i) => {
      const c = cv(S), ctx = c.getContext('2d');
      const en = surfaceEn(colorEn(baseEn, s.color && s.color !== 'match' ? s.color : D.color), s.style);
      const { x, y, r } = s, k = r / 78.5, holes = lay.subHoles[s.id] || [];
      // No plate at all: no shadow stack, no gradient band, no surface, no rim, no
      // weathering, no print. The canvas stays empty and exportKit drops the layer, so
      // the device pays for the art baked into the dial rather than blending a second
      // full plane over it. The caller keeps the record for handSlots, which still
      // mounts the pointer hand on this centre.
      if (s.transparent) return { id: s.id, canvas: c, en, transparent: true };
      const band = (cx) => { cx.beginPath(); cx.moveTo(x + r + 2.5, y); cx.arc(x, y, r + 2.5, 0, TAU); if (s.open) { cx.moveTo(x + s.open - 2.5, y); cx.arc(x, y, s.open - 2.5, 0, TAU, true); } };
      const ht = s.height, dir = SH ? SH.dir : away(1), dm = SH ? SH.dist : 1, bm = SH ? SH.blur : 1, om = SH ? SH.op : 1;
      if (ht > 0) [[2, 3, 4, 0.8], [8, 13, 20, 0.7], [14, 22, 34, 0.5]].forEach(([ox, oy, b, a]) => {
        const d = Math.hypot(ox, oy) * k * ht * dm;
        ctx.save(); ctx.shadowColor = 'rgba(0,0,0,' + Math.min(1, a * om * Math.min(1, 0.4 + ht * 0.6)) + ')'; ctx.shadowBlur = b * k * Math.max(0.3, ht) * bm; ctx.shadowOffsetX = dir[0] * d; ctx.shadowOffsetY = dir[1] * d;
        band(ctx); ctx.fillStyle = '#000'; ctx.fill(); ctx.restore();
      });
      band(ctx); ctx.fillStyle = _.grad(ctx, x, y, r + 3, pal); ctx.fill();
      ctx.save(); ctx.beginPath(); ctx.arc(x, y, r, 0, TAU); if (s.open) ctx.arc(x, y, s.open, 0, TAU, true); ctx.clip();
      surface(ctx, Object.assign({}, s, { tex: s.tex == null ? D.texture : s.tex, weave: { ang: s.wa == null ? D.weaveAngle : s.wa, sc: s.ws == null ? D.weaveScale : s.ws } }), en, 23 + i * 7, true);
      if (ht < 0) {
        const dd = -ht;
        [[3, 4, 0.75], [9, 14, 0.55]].forEach(([o, b, a]) => {
          const d = o * k * dd * dm;
          ctx.save(); ctx.shadowColor = 'rgba(0,0,0,' + Math.min(1, a * om) + ')'; ctx.shadowBlur = b * k * Math.max(0.4, dd) * bm; ctx.shadowOffsetX = dir[0] * d; ctx.shadowOffsetY = dir[1] * d;
          ctx.beginPath(); ctx.rect(-60, -60, S + 120, S + 120); ctx.arc(x, y, r, 0, TAU); ctx.fillStyle = '#000'; ctx.fill('evenodd'); ctx.restore();
        });
      }
      printSub(ctx, s, en, holes, dialFont);
      setFace(dialFont); drawFlares(ctx, lay.flares.filter((f) => f.host === s.id && f.icon !== 'datetext'), en, pal);
      weather(ctx, x, y, s.open || 0, r, 63 + i * 17, en, wearOf(D), 0.6);
      ctx.restore();
      ctx.strokeStyle = 'rgba(0,0,0,.5)'; ctx.lineWidth = 0.8; ctx.beginPath(); ctx.arc(x, y, r, 0, TAU); ctx.stroke();
      if (ht < 0) { ctx.save(); ctx.beginPath(); ctx.arc(x, y, r, 0, TAU); ctx.clip(); ctx.strokeStyle = 'rgba(255,250,235,' + Math.min(0.4, 0.22 * -ht) + ')'; ctx.lineWidth = 1.2; ctx.beginPath(); ctx.arc(x - dir[0] * 1.2, y - dir[1] * 1.2, r, 0, TAU); ctx.stroke(); ctx.restore(); }
      if (s.open) { ctx.beginPath(); ctx.arc(x, y, s.open, 0, TAU); ctx.stroke(); }
      cutHoles(ctx, holes, pal, glow);
      return { id: s.id, canvas: c, en };
    });
  }

  // ---------- complication discs (drawn under their host, seen through its aperture)
  function drawComp(ctx, c, date, en, pal, seed, ph) {
    const h = c.hole;
    if (c.type === 'moon') { const p = ph == null ? moonPhase(date) : ph; return ROUND_MOON[c.style] ? moonRound(ctx, h.x, h.y, h.r, p, c.style, en, seed, c) : moonDisc(ctx, h.x, h.y, h.a, p, pal, seed, h.rot, c.style); }
    const v = c.blank ? '' : c.type === 'custom' ? fmtDate(c.fmt, date, c.fcase) : c.type === 'date' ? String(date.getDate()) : c.type === 'day' ? DAYS[date.getDay()] : MONTHS[date.getMonth()];
    const k = c.k || 1, round = h.kind === 'circle';
    const w = (round ? 2 * h.r : h.w) + 10 * k, hh = (round ? 2 * h.r : h.h) + 10 * k, dark = c.disc === 'dark' || (c.disc !== 'light' && !!en.dark);
    ctx.save(); ctx.translate(h.x, h.y); ctx.rotate(round ? 0 : (h.rot || 0));
    // A round window clips its disc to the aperture, so the disc, the inner shadow
    // and the glyph all stop at the circle instead of squaring off outside it.
    if (round) { ctx.beginPath(); ctx.arc(0, 0, w / 2, 0, TAU); ctx.clip(); }
    ctx.fillStyle = c.dc && c.dc !== 'auto' ? c.dc : dark ? '#1b1916' : '#f2ecdd'; ctx.fillRect(-w / 2, -hh / 2, w, hh);
    const g = ctx.createLinearGradient(0, -hh / 2, 0, hh / 2);
    g.addColorStop(0, 'rgba(0,0,0,.22)'); g.addColorStop(0.35, 'rgba(0,0,0,0)'); g.addColorStop(0.65, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,.22)');
    ctx.fillStyle = g; ctx.fillRect(-w / 2, -hh / 2, w, hh);
    ctx.fillStyle = c.tc && c.tc !== 'auto' ? c.tc : dark ? '#ecdfbf' : '#18120b'; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.font = winFont(c, k);
    if ('letterSpacing' in ctx) ctx.letterSpacing = c.type === 'date' ? '0px' : 1.5 * k + 'px';
    ctx.fillText(v, c.type === 'date' ? 0 : 0.75 * k, k);
    if ('letterSpacing' in ctx) ctx.letterSpacing = '0px';
    ctx.restore();
  }
  const MOON_LOOK = {
    classic: (pal) => ({ sky: ['#2c4486', '#0b1431'], star: '255,226,150', moon: [pal.hi, pal.mid, pal.lo], crater: 'rgba(80,50,15,.22)', stars: 46 }),
    silver: () => ({ sky: ['#24365c', '#070b16'], star: '232,238,248', moon: ['#ffffff', '#c9cfd8', '#6d7582'], crater: 'rgba(40,46,58,.2)', stars: 60 }),
    black: (pal) => ({ sky: ['#232120', '#040404'], star: '255,236,190', moon: [pal.hi, pal.mid, pal.lo], crater: 'rgba(60,40,12,.25)', stars: 24 }),
  };
  function moonDisc(ctx, x, y0, a, p, pal, seed, rot, style) {
    const _ = L(), R = a * 0.98, rng = _.rng(seed * 13 + 5), lk = (MOON_LOOK[style] || MOON_LOOK.classic)(pal);
    ctx.save(); ctx.translate(x, y0); ctx.rotate((rot || 0) + p * Math.PI);
    let g = ctx.createRadialGradient(0, -a * 0.3, a * 0.1, 0, 0, R);
    g.addColorStop(0, lk.sky[0]); g.addColorStop(1, lk.sky[1]);
    ctx.fillStyle = g; ctx.beginPath(); ctx.arc(0, 0, R, 0, TAU); ctx.fill();
    for (let i = 0; i < lk.stars; i++) {
      const an = rng() * TAU, d = Math.sqrt(rng()) * R * 0.97, q = P(0, 0, d, an), sr = 0.5 + Math.pow(rng(), 3) * 1.6;
      ctx.fillStyle = 'rgba(' + lk.star + ',' + (0.55 + rng() * 0.45) + ')';
      ctx.beginPath();
      for (let j = 0; j < 8; j++) { const rr = j % 2 ? sr * 0.35 : sr * 1.4, aa = (j / 8) * TAU; j ? ctx.lineTo(q[0] + Math.cos(aa) * rr, q[1] + Math.sin(aa) * rr) : ctx.moveTo(q[0] + rr, q[1]); }
      ctx.closePath(); ctx.fill();
    }
    const m = a * 0.33;
    [-Math.PI, 0].forEach((an, j) => {
      const q = P(0, 0, a / 2, an);
      ctx.save(); ctx.shadowColor = 'rgba(0,0,0,.5)'; ctx.shadowBlur = 2; ctx.shadowOffsetY = 1;
      g = ctx.createRadialGradient(q[0] - m * 0.35, q[1] - m * 0.35, m * 0.1, q[0], q[1], m);
      g.addColorStop(0, lk.moon[0]); g.addColorStop(0.6, lk.moon[1]); g.addColorStop(1, lk.moon[2]);
      ctx.fillStyle = g; ctx.beginPath(); ctx.arc(q[0], q[1], m, 0, TAU); ctx.fill(); ctx.restore();
      const r2 = _.rng(seed + j * 31);
      for (let i = 0; i < 7; i++) {
        const ca = r2() * TAU, cd = Math.sqrt(r2()) * m * 0.7, cr = m * (0.08 + r2() * 0.16), cq = P(q[0], q[1], cd, ca);
        ctx.fillStyle = lk.crater; ctx.beginPath(); ctx.arc(cq[0], cq[1], cr, 0, TAU); ctx.fill();
      }
    });
    ctx.restore();
  }

  // round moon apertures: the phase is drawn as a real terminator (lit limb + ellipse), not a turning disc
  function moonRound(ctx, x, y, r, p, style, en, seed, c) {
    const ok = (v) => v && v !== 'auto', mc = c && ok(c.mc) ? c.mc : null, mc2 = c && ok(c.mc2) ? c.mc2 : null, mbg = c && ok(c.mbg) ? c.mbg : null;
    const R0 = r + 6, rr = r * (style === 'graphic' ? 0.74 : 0.8), rng = L().rng(seed * 17 + 3);
    const lit = () => {
      const k = Math.cos(p * TAU), rx = Math.max(0.01, rr * Math.abs(k));
      ctx.beginPath();
      if (p < 0.5) { ctx.arc(x, y, rr, -Math.PI / 2, Math.PI / 2, false); ctx.ellipse(x, y, rx, rr, 0, Math.PI / 2, k > 0 ? -Math.PI / 2 : Math.PI * 1.5, k > 0); }
      else { ctx.arc(x, y, rr, Math.PI / 2, Math.PI * 1.5, false); ctx.ellipse(x, y, rx, rr, 0, Math.PI * 1.5, k > 0 ? Math.PI / 2 : Math.PI * 2.5, k > 0); }
      ctx.closePath();
    };
    const disc = (rad) => { ctx.beginPath(); ctx.arc(x, y, rad, 0, TAU); };
    ctx.save();
    if (style === 'graphic') ctx.fillStyle = en.a;
    else { const g = ctx.createRadialGradient(x, y - r * 0.3, r * 0.1, x, y, R0); g.addColorStop(0, style === 'neon' ? '#1c0634' : '#18223e'); g.addColorStop(1, style === 'neon' ? '#05010b' : '#020309'); ctx.fillStyle = g; }
    disc(R0); ctx.fill();
    if (style !== 'graphic') for (let i = 0; i < 34; i++) { const a = rng() * TAU, d = Math.sqrt(rng()) * R0; ctx.fillStyle = 'rgba(255,255,255,' + (0.25 + rng() * 0.65) + ')'; disc(0.35 + rng() * 0.8); ctx.save(); ctx.translate(Math.cos(a) * d, Math.sin(a) * d); ctx.beginPath(); ctx.arc(x, y, 0.35 + rng() * 0.8, 0, TAU); ctx.fill(); ctx.restore(); }
    if (style === 'sphere') {
      ctx.fillStyle = '#1e2128'; disc(rr); ctx.fill(); ctx.fillStyle = 'rgba(120,140,175,.14)'; ctx.fill();
      ctx.save(); lit(); ctx.clip();
      const g = ctx.createRadialGradient(x - rr * 0.25, y - rr * 0.3, rr * 0.1, x, y, rr);
      g.addColorStop(0, '#fbf8ef'); g.addColorStop(0.6, '#d6d1c4'); g.addColorStop(1, '#8c877c');
      ctx.fillStyle = g; ctx.fillRect(x - rr, y - rr, 2 * rr, 2 * rr); ctx.restore();
      ctx.save(); disc(rr); ctx.clip();
      for (let i = 0; i < 7; i++) { const a = rng() * TAU, d = Math.sqrt(rng()) * rr * 0.7, cx = x + Math.cos(a) * d, cy = y + Math.sin(a) * d, sr = rr * (0.18 + rng() * 0.28); const q = ctx.createRadialGradient(cx, cy, 0, cx, cy, sr); q.addColorStop(0, 'rgba(70,72,84,.32)'); q.addColorStop(1, 'rgba(70,72,84,0)'); ctx.fillStyle = q; ctx.fillRect(cx - sr, cy - sr, 2 * sr, 2 * sr); }
      for (let i = 0; i < 26; i++) { const a = rng() * TAU, d = Math.sqrt(rng()) * rr * 0.92, cx = x + Math.cos(a) * d, cy = y + Math.sin(a) * d, cr = 0.6 + Math.pow(rng(), 2) * rr * 0.12; ctx.lineWidth = 0.6; ctx.strokeStyle = 'rgba(0,0,0,.2)'; ctx.beginPath(); ctx.arc(cx + 0.3, cy + 0.3, cr, 0, TAU); ctx.stroke(); ctx.strokeStyle = 'rgba(255,255,255,.18)'; ctx.beginPath(); ctx.arc(cx - 0.3, cy - 0.3, cr, Math.PI, Math.PI * 1.6); ctx.stroke(); }
      const lg = ctx.createRadialGradient(x, y, rr * 0.65, x, y, rr); lg.addColorStop(0, 'rgba(0,0,0,0)'); lg.addColorStop(1, 'rgba(0,0,0,.38)');
      ctx.fillStyle = lg; ctx.fillRect(x - rr, y - rr, 2 * rr, 2 * rr); ctx.restore();
    } else if (style === 'graphic') {
      if (mbg) { ctx.fillStyle = mbg; disc(rr); ctx.fill(); }
      ctx.fillStyle = mc || en.ink; ctx.globalAlpha = 0.9; lit(); ctx.fill(); ctx.globalAlpha = 1;
      ctx.lineWidth = 1.4; ctx.strokeStyle = mc2 || mc || en.ink; disc(rr); ctx.stroke();
    } else {
      const A1 = mc || '#ff2bd6', A2 = mc2 || '#22e6ff';
      ctx.fillStyle = mbg || '#12061f'; disc(rr); ctx.fill();
      ctx.save(); ctx.shadowColor = A1; ctx.shadowBlur = 14; lit();
      const g = ctx.createLinearGradient(x - rr, y - rr, x + rr, y + rr); g.addColorStop(0, A1); g.addColorStop(1, A2);
      ctx.fillStyle = g; ctx.fill(); ctx.restore();
      ctx.save(); ctx.globalAlpha = 0.65; ctx.lineWidth = 1; ctx.strokeStyle = A2; disc(rr + 3); ctx.stroke(); ctx.restore();
    }
    ctx.restore();
  }

  // ---------- lighting, glass, case
  const CASE_METALS = {
    brass: ['#2c1d0b', '#9c7434', '#f0d79a', '#7d5a24', '#120c05'],
    copper: ['#2a1208', '#9a5232', '#f2b48c', '#7a3a1e', '#140704'],
    gunmetal: ['#0e1012', '#4f555c', '#c9ced4', '#3a3f45', '#060708'],
    steel: ['#202326', '#8b9198', '#f4f6f8', '#6c7178', '#0c0d0e'],
    rose: ['#2a1410', '#a8705e', '#f6cbb6', '#8a5444', '#140806'],
  };
  const CASE_STYLES = { polished: 'Polished', stepped: 'Stepped', coin: 'Coin edge', riveted: 'Riveted', insert: 'Enamel insert', mariner: 'Mariner bezel', tachy: 'Tachymetre', none: 'None' };
  /* The radius the case leaves open, in dial px, and the one place it is worked out.
   * A bezel style owns a fixed opening; "none" hands the number to the author, because
   * the metal that would have defined it is already baked into the device and only its
   * inner edge is known. Everything that has to respect the case -- the glass, the
   * vignette, and the ring the author draws against on the stage -- reads this instead of
   * re-deriving it from the style name. It draws nothing itself and it does NOT decide how
   * big the face is: see faceScale, dynScale and artClipR. */
  const CASE_OPEN_DEFAULT = 221.5, CASE_OPEN_MIN = 90, CASE_OPEN_MAX = 233;
  function caseOpenR(o) {
    const st = o && o.caseStyle, v = o && o.caseOpen;
    if (BEZEL[st]) return BEZEL_IN;
    if (st !== 'none') return CASE_OPEN_DEFAULT;
    const r = +v;
    return Math.min(CASE_OPEN_MAX, Math.max(CASE_OPEN_MIN, isFinite(r) && r > 0 ? r : CASE_OPEN_DEFAULT));
  }
  /* How far the whole face is scaled about the centre, and it is NOT a second reading of
   * the opening. The exported layers are baked through this number (see the kit's bake()),
   * so it decides what the device shows, not merely what the composer previews. Under a
   * composer-drawn case that is the point: the metal covers the rim, so the print and the
   * hands are eased in to stay visible under it, and each style owns one fixed opening to
   * do that with. Under "none" the number is the author's, dragged about with a slider --
   * and a slider that resampled the dial would resample the artwork they uploaded along
   * with it, so a graphic positioned against the real dial would leave it the moment the
   * opening moved. There the option draws nothing at all (see buildOuter) and clips nothing
   * (see artClipR); what it still does is hand the drawn content to dynScale below. */
  function faceScale(o) {
    if (o && o.caseStyle === 'none') return 1;
    const r = caseOpenR(o);
    return r < DR ? r / DR : 1;
  }
  /* How far the engine-drawn content is eased in under "no case", and the one number the
   * opening slider still moves. A drawn case shrinks the whole face (author's art included)
   * through faceScale, because the metal covers the rim and the art is clipped to the dial
   * circle anyway. Under "none" the author's own image carries the device's metal, so it is
   * the one thing the opening may not resample: it is blended at authored size (see the
   * split build in buildDial and the bgL pass in compose), while everything the composer
   * draws -- hands, chapter print, complications, subdials, train, glass -- is turned
   * through this scale about the centre and so lands inside the ring the guide paints. */
  function dynScale(o) {
    if (!o || o.caseStyle !== 'none') return 1;
    const r = caseOpenR(o);
    return r < DR ? r / DR : 1;
  }
  /* How far the author's own uploaded art may reach, and it follows the same fork as
   * faceScale for the same reason: both limits exist to keep content under metal that the
   * composer is drawing. Under a bezel style that is true -- the metal covers the rim, so
   * art is clipped to the dial circle and the print is eased in. Under "none" there is no
   * metal of ours at all, and the author's image is the one that carries the device's case.
   * Clipping it there would cut their bezel off at the dial circle and paint a black ring
   * into the gap, which is the opposite of what the option promises. So the clip opens to
   * the panel's half-diagonal and their pixels say where the watch ends. */
  const PANEL_R = Math.ceil(C * Math.SQRT2);
  function artClipR(o) { return o && o.caseStyle === 'none' ? PANEL_R : DR; }
  const GLASS_STYLES = { domed: 'Domed', flat: 'Flat sapphire', box: 'Box acrylic', none: 'No glass' };

  // all glass and case art is authored for light from the upper left, then turned to the outer-light direction
  function buildGlass(k, style) {
    const c = cv(S), ctx = c.getContext('2d');
    if (style === 'none') return c;
    ctx.save(); ctx.beginPath(); ctx.arc(C, C, DR + 1, 0, TAU); ctx.clip();
    ctx.globalAlpha = Math.min(1, Math.max(0.15, k));
    let g;
    if (style === 'flat') {
      ctx.fillStyle = 'rgba(140,175,235,.05)'; ctx.fillRect(0, 0, S, S);
      ctx.save(); ctx.translate(C, C); ctx.rotate(-Math.PI / 4); ctx.filter = 'blur(3px)';
      g = ctx.createLinearGradient(0, -150, 0, -70);
      g.addColorStop(0, 'rgba(255,255,255,0)'); g.addColorStop(0.5, 'rgba(235,245,255,.13)'); g.addColorStop(1, 'rgba(255,255,255,0)');
      ctx.fillStyle = g; ctx.fillRect(-260, -150, 520, 80);
      g = ctx.createLinearGradient(0, -52, 0, -40);
      g.addColorStop(0, 'rgba(255,255,255,0)'); g.addColorStop(0.5, 'rgba(235,245,255,.07)'); g.addColorStop(1, 'rgba(255,255,255,0)');
      ctx.fillStyle = g; ctx.fillRect(-260, -52, 520, 12);
      ctx.restore();
      g = ctx.createRadialGradient(C, C, DR - 5, C, C, DR + 1);
      g.addColorStop(0, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,.18)');
      ctx.fillStyle = g; ctx.fillRect(0, 0, S, S);
    } else if (style === 'box') {
      ctx.fillStyle = 'rgba(255,228,165,.045)'; ctx.fillRect(0, 0, S, S);
      g = ctx.createRadialGradient(150, 120, 0, 150, 120, 200);
      g.addColorStop(0, 'rgba(255,248,232,.09)'); g.addColorStop(1, 'rgba(255,248,232,0)');
      ctx.fillStyle = g; ctx.fillRect(0, 0, S, S);
      // thick acrylic wall: dark refraction band with a bright inner meniscus
      g = ctx.createRadialGradient(C, C, DR - 26, C, C, DR + 1);
      g.addColorStop(0, 'rgba(0,0,0,0)'); g.addColorStop(0.55, 'rgba(0,0,0,.12)'); g.addColorStop(0.85, 'rgba(0,0,0,.42)'); g.addColorStop(1, 'rgba(0,0,0,.25)');
      ctx.fillStyle = g; ctx.fillRect(0, 0, S, S);
      ctx.save(); ctx.filter = 'blur(1px)'; ctx.lineWidth = 2;
      ctx.strokeStyle = 'rgba(255,245,225,.22)'; ctx.beginPath(); ctx.arc(C, C, DR - 22, Math.PI * 1.02, Math.PI * 1.72); ctx.stroke();
      ctx.strokeStyle = 'rgba(255,245,225,.1)'; ctx.beginPath(); ctx.arc(C, C, DR - 22, Math.PI * 0.05, Math.PI * 0.6); ctx.stroke();
      ctx.lineWidth = 5; ctx.strokeStyle = 'rgba(255,250,240,.16)'; ctx.beginPath(); ctx.arc(C, C, DR - 7, Math.PI * 1.12, Math.PI * 1.5); ctx.stroke();
      ctx.restore();
      const R = L().rng(404); ctx.lineCap = 'round';
      for (let i = 0; i < 70; i++) {
        const x = C + (R() - 0.5) * 400, y = C + (R() - 0.5) * 400, an = R() * TAU, len = 4 + Math.pow(R(), 2) * 40;
        ctx.strokeStyle = 'rgba(255,250,240,' + (0.04 + R() * 0.1) + ')'; ctx.lineWidth = 0.4 + R() * 0.4;
        ctx.beginPath(); ctx.moveTo(x, y); ctx.quadraticCurveTo(x + Math.cos(an + 0.3) * len * 0.5, y + Math.sin(an + 0.3) * len * 0.5, x + Math.cos(an) * len, y + Math.sin(an) * len); ctx.stroke();
      }
    } else {
      ctx.fillStyle = 'rgba(160,185,215,.035)'; ctx.fillRect(0, 0, S, S);
      g = ctx.createRadialGradient(140, 110, 0, 140, 110, 230);
      g.addColorStop(0, 'rgba(255,248,236,.12)'); g.addColorStop(0.55, 'rgba(255,248,236,.03)'); g.addColorStop(1, 'rgba(255,248,236,0)');
      ctx.fillStyle = g; ctx.fillRect(0, 0, S, S);
      ctx.save(); ctx.filter = 'blur(1.5px)'; ctx.beginPath();
      ctx.arc(C, C, 210, Math.PI * 1.06, Math.PI * 1.64); ctx.arc(C + 12, C + 16, 212, Math.PI * 1.64, Math.PI * 1.06, true); ctx.closePath();
      g = ctx.createLinearGradient(30, 300, 330, 20);
      g.addColorStop(0, 'rgba(255,250,240,.02)'); g.addColorStop(0.5, 'rgba(255,250,240,.24)'); g.addColorStop(1, 'rgba(255,250,240,.03)');
      ctx.fillStyle = g; ctx.fill(); ctx.restore();
      g = ctx.createRadialGradient(330, 350, 0, 330, 350, 120);
      g.addColorStop(0, 'rgba(255,240,220,.05)'); g.addColorStop(1, 'rgba(255,240,220,0)');
      ctx.fillStyle = g; ctx.fillRect(0, 0, S, S);
      g = ctx.createRadialGradient(C, C, DR - 14, C, C, DR + 1);
      g.addColorStop(0, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,.3)');
      ctx.fillStyle = g; ctx.fillRect(0, 0, S, S);
      ctx.strokeStyle = 'rgba(255,242,220,.25)'; ctx.lineWidth = 1; ctx.beginPath(); ctx.arc(C, C, DR - 2.5, Math.PI * 0.08, Math.PI * 0.58); ctx.stroke();
    }
    ctx.restore();
    return spun(c, LIGHT.outer - OUTER0);
  }
  // one turned metal ring: radial profile, brushed sheen, wear, grime in the corners
  let DA = 0;
  function metalRing(ctx, r0, r1, st, seed, dim) {
    const _ = L();
    ctx.save(); ctx.beginPath(); ctx.arc(C, C, r1, 0, TAU); ctx.arc(C, C, r0, 0, TAU, true);
    let g = ctx.createRadialGradient(C, C, r0, C, C, r1);
    [0, 0.3, 0.55, 0.8, 1].forEach((p, i) => g.addColorStop(p, st[i]));
    ctx.fillStyle = g; ctx.fill(); ctx.clip('evenodd');
    _.sheen(ctx, C, C, r1, 0.9 * (dim || 1));
    const R3 = _.rng(seed);
    for (let i = 0; i < (r1 - r0) * 24; i++) {
      const a = R3() * TAU + DA, d = r0 + R3() * (r1 - r0), span = 0.004 + Math.pow(R3(), 2) * 0.06, tilt = (R3() - 0.5) * 3;
      ctx.strokeStyle = R3() < 0.55 ? 'rgba(20,12,4,' + (0.2 + R3() * 0.35) + ')' : 'rgba(255,240,200,' + (0.12 + R3() * 0.3) + ')';
      ctx.lineWidth = 0.4 + R3() * 0.4; const p1 = P(C, C, d, a), p2 = P(C, C, d + tilt, a + span);
      ctx.beginPath(); ctx.moveTo(p1[0], p1[1]); ctx.lineTo(p2[0], p2[1]); ctx.stroke();
    }
    g = ctx.createRadialGradient(C, C, r0, C, C, r1);
    g.addColorStop(0, 'rgba(20,10,2,.75)'); g.addColorStop(0.18, 'rgba(20,10,2,0)'); g.addColorStop(0.85, 'rgba(20,10,2,0)'); g.addColorStop(1, 'rgba(20,10,2,.7)');
    ctx.fillStyle = g; ctx.fillRect(0, 0, S, S);
    g = ctx.createLinearGradient(80, 80, 390, 390);
    g.addColorStop(0, 'rgba(255,220,160,.18)'); g.addColorStop(0.55, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,' + (0.35 + (dim ? 0.2 : 0)) + ')');
    ctx.fillStyle = g; ctx.fillRect(0, 0, S, S);
    ctx.restore();
  }
  function buildOuter(glow, style, metal, openR) {
    const _ = L(), c = cv(S), ctx = c.getContext('2d'), st = CASE_METALS[metal] || CASE_METALS.brass;
    // No case: this layer draws NOTHING at all, and that is the whole meaning of the
    // option. The device owns its own metal and the author's own image is what carries it,
    // so any paint of ours lands on top of their artwork. The rim shade, the grooves, the
    // speculars and the inner shadow all belong to a case we are not drawing.
    //
    // It used to ship the opaque mask alone -- rect-minus-circle filled with #000 -- to stop
    // our content reaching the hardware. That was still drawing, and it drew a black circle
    // that marched inward with the opening slider while painting the author's own case out.
    // Nothing here clips either: artInDial lets their art reach the panel, and the pieces the
    // engine draws are eased inside this ring by dynScale -- the slider moves them, this layer
    // never paints over them.
    if (style === 'none') return c;
    DA = -(LIGHT.outer - OUTER0);
    let g = ctx.createRadialGradient(150, 120, 0, 150, 120, 260);
    g.addColorStop(0, 'rgba(' + _.tint('255,214,150') + ',' + Math.min(1, 0.13 * glow) + ')'); g.addColorStop(1, 'rgba(255,214,150,0)');
    ctx.fillStyle = g; ctx.beginPath(); ctx.arc(C, C, 233, 0, TAU); ctx.fill();
    g = ctx.createRadialGradient(C, C, 120, C, C, 233);
    g.addColorStop(0, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,.38)');
    ctx.fillStyle = g; ctx.beginPath(); ctx.arc(C, C, 233, 0, TAU); ctx.fill();
    ctx.save(); ctx.shadowColor = 'rgba(0,0,0,.75)'; ctx.shadowBlur = 9; ctx.shadowOffsetY = 2;
    const RI = openR == null ? (BEZEL[style] ? BEZEL_IN : CASE_OPEN_DEFAULT) : openR;
    ctx.beginPath(); ctx.rect(-50, -50, S + 100, S + 100); ctx.arc(C, C, RI, 0, TAU, true); ctx.fillStyle = '#000'; ctx.fill(); ctx.restore();
    ctx.strokeStyle = 'rgba(0,0,0,.6)'; ctx.lineWidth = 1.5; ctx.beginPath(); ctx.arc(C, C, RI - 0.5, 0, TAU); ctx.stroke();
    const spec = [];
    const groove = (r) => { ctx.strokeStyle = 'rgba(0,0,0,.7)'; ctx.lineWidth = 1; ctx.beginPath(); ctx.arc(C, C, r, 0, TAU); ctx.stroke(); ctx.strokeStyle = 'rgba(255,240,205,.25)'; ctx.lineWidth = 0.6; ctx.beginPath(); ctx.arc(C, C, r + 0.9, 0, TAU); ctx.stroke(); };
    if (BEZEL[style]) {
      const i0 = 196, i1 = style === 'mariner' ? 224.5 : 227.5;
      metalRing(ctx, BEZEL_IN, i0, st, 71, 0.8);
      ctx.save(); ctx.beginPath(); ctx.arc(C, C, i1, 0, TAU); ctx.arc(C, C, i0, 0, TAU, true);
      g = ctx.createRadialGradient(C, C, i0, C, C, i1);
      g.addColorStop(0, '#050506'); g.addColorStop(0.5, '#17181c'); g.addColorStop(1, '#060607');
      ctx.fillStyle = g; ctx.fill(); ctx.clip('evenodd');
      g = ctx.createLinearGradient(80, 80, 390, 390);
      g.addColorStop(0, 'rgba(255,245,230,.16)'); g.addColorStop(0.5, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,.3)');
      ctx.fillStyle = g; ctx.fillRect(0, 0, S, S); ctx.restore();
      metalRing(ctx, i1, 233, st, 77);
      if (style === 'mariner') {
        ctx.save(); ctx.beginPath(); ctx.arc(C, C, 232.4, 0, TAU); ctx.arc(C, C, 226, 0, TAU, true); ctx.clip('evenodd');
        for (let i = 0; i < 120; i++) {
          const a = (i / 120) * TAU + DA, lit = 0.5 + 0.5 * Math.cos(a - OUTER0), p1 = P(C, C, 226, a), p2 = P(C, C, 232.4, a), q1 = P(C, C, 226, a + TAU / 240), q2 = P(C, C, 232.4, a + TAU / 240);
          ctx.lineWidth = 1.8;
          ctx.strokeStyle = 'rgba(255,240,205,' + (0.12 + 0.4 * lit) + ')'; ctx.beginPath(); ctx.moveTo(p1[0], p1[1]); ctx.lineTo(p2[0], p2[1]); ctx.stroke();
          ctx.strokeStyle = 'rgba(0,0,0,' + (0.55 - 0.2 * lit) + ')'; ctx.beginPath(); ctx.moveTo(q1[0], q1[1]); ctx.lineTo(q2[0], q2[1]); ctx.stroke();
        }
        ctx.restore();
      }
      groove(i1); groove(i0 - 0.5);
      spec.push([230, 3, 0.8], [194.8, 1.2, 0.5]);
    } else if (style === 'stepped') {
      metalRing(ctx, 221.5, 226.5, st.map((x, i) => st[Math.min(4, i + 1)]), 71, 0.7);
      metalRing(ctx, 226.5, 233, st, 77);
      groove(226.5);
      spec.push([229.8, 2.8, 0.9], [223.8, 1.2, 0.45]);
    } else if (style === 'insert') {
      ctx.save(); ctx.beginPath(); ctx.arc(C, C, 228.5, 0, TAU); ctx.arc(C, C, 221.5, 0, TAU, true);
      g = ctx.createRadialGradient(C, C, 221.5, C, C, 228.5);
      g.addColorStop(0, '#060504'); g.addColorStop(0.5, '#1b1815'); g.addColorStop(1, '#070605');
      ctx.fillStyle = g; ctx.fill(); ctx.clip('evenodd');
      ctx.strokeStyle = st[2]; ctx.globalAlpha = 0.85;
      for (let i = 0; i < 60; i++) {
        const a = (i / 60) * TAU - Math.PI / 2 + DA, big = i % 5 === 0, p1 = P(C, C, big ? 222.6 : 224.6, a), p2 = P(C, C, 227.4, a);
        ctx.lineWidth = big ? 1.6 : 0.7; ctx.beginPath(); ctx.moveTo(p1[0], p1[1]); ctx.lineTo(p2[0], p2[1]); ctx.stroke();
      }
      ctx.globalAlpha = 1; ctx.fillStyle = st[2];
      ctx.save(); ctx.translate(C, C); ctx.rotate(DA); ctx.translate(-C, -C);
      ctx.beginPath(); ctx.moveTo(C, 233 - 227.6); ctx.lineTo(C - 3.4, 233 - 222.4); ctx.lineTo(C + 3.4, 233 - 222.4); ctx.closePath(); ctx.fill(); ctx.restore();
      g = ctx.createLinearGradient(80, 80, 390, 390);
      g.addColorStop(0, 'rgba(255,240,220,.14)'); g.addColorStop(0.5, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,.3)');
      ctx.fillStyle = g; ctx.fillRect(0, 0, S, S);
      ctx.restore();
      metalRing(ctx, 228.5, 233, st, 77);
      groove(228.5);
      spec.push([230.8, 1.8, 0.85], [225, 2.5, 0.25]);
    } else {
      metalRing(ctx, 221.5, 233, st, 77);
      if (style === 'coin') {
        ctx.save(); ctx.beginPath(); ctx.arc(C, C, 232.4, 0, TAU); ctx.arc(C, C, 224, 0, TAU, true); ctx.clip('evenodd');
        for (let i = 0; i < 160; i++) {
          const a = (i / 160) * TAU + DA, lit = 0.5 + 0.5 * Math.cos(a - OUTER0), p1 = P(C, C, 224, a), p2 = P(C, C, 232.4, a);
          const q1 = P(C, C, 224, a + TAU / 320), q2 = P(C, C, 232.4, a + TAU / 320);
          ctx.lineWidth = 1.5;
          ctx.strokeStyle = 'rgba(255,240,205,' + (0.12 + 0.4 * lit) + ')'; ctx.beginPath(); ctx.moveTo(p1[0], p1[1]); ctx.lineTo(p2[0], p2[1]); ctx.stroke();
          ctx.strokeStyle = 'rgba(0,0,0,' + (0.55 - 0.2 * lit) + ')'; ctx.beginPath(); ctx.moveTo(q1[0], q1[1]); ctx.lineTo(q2[0], q2[1]); ctx.stroke();
        }
        ctx.restore();
        groove(223.6);
        spec.push([227.2, 4.2, 0.45], [222.6, 1.2, 0.5]);
      } else if (style === 'riveted') {
        groove(223.4); groove(231);
        for (let i = 0; i < 12; i++) {
          const a = (i / 12) * TAU - Math.PI / 2 + Math.PI / 12 + DA, q = P(C, C, 227.3, a), r = 3.1;
          ctx.save(); ctx.shadowColor = 'rgba(0,0,0,.7)'; ctx.shadowBlur = 2.5; ctx.shadowOffsetX = 1.2; ctx.shadowOffsetY = 1.4;
          g = ctx.createRadialGradient(q[0] - r * 0.4, q[1] - r * 0.4, 0.3, q[0], q[1], r);
          g.addColorStop(0, st[2]); g.addColorStop(0.55, st[1]); g.addColorStop(1, st[4]);
          ctx.fillStyle = g; ctx.beginPath(); ctx.arc(q[0], q[1], r, 0, TAU); ctx.fill(); ctx.restore();
          ctx.fillStyle = 'rgba(255,250,235,.7)'; ctx.beginPath(); ctx.arc(q[0] - 1, q[1] - 1, 0.7, 0, TAU); ctx.fill();
        }
        spec.push([227.2, 4.2, 0.6], [222.8, 1.4, 0.5]);
      } else spec.push([227.2, 4.2, 0.9], [222.8, 1.4, 0.6]);
    }
    ctx.save(); ctx.globalCompositeOperation = 'lighter';
    const [s1, s2] = spec;
    _.specular(ctx, s1[0], s1[1], [[0, 0.38, 0.9], [Math.PI, 0.38, 0.9], [Math.PI * 1.28, 0.32, 0.75], [Math.PI * 1.52, 0.1, 0.4], [Math.PI * 0.22, 0.18, 0.3]], '255,228,175', s1[2] * glow, 3, 23);
    _.specular(ctx, s2[0], s2[1], [[0, 0.5, 0.6], [Math.PI, 0.5, 0.6]], '255,200,130', s2[2] * glow, 1.5, 24);
    g = ctx.createRadialGradient(C, C, 230, C, C, 330);
    const t2 = _.tint('255,160,70'); g.addColorStop(0, `rgba(${t2},${Math.min(1, 0.2 * glow)})`); g.addColorStop(1, `rgba(${t2},0)`);
    ctx.fillStyle = g; ctx.beginPath(); ctx.rect(0, 0, S, S); ctx.arc(C, C, 233, 0, TAU, true); ctx.fill('evenodd');
    ctx.restore();
    const out = spun(c, LIGHT.outer - OUTER0);
    if (BEZEL[style]) bezelMarks(out.getContext('2d'), style, st);
    return out;
  }
  // bezel scales are drawn after the light-turn so the numbers stay put
  function bezelMarks(ctx, style, st) {
    const mk = '#efebe0', A = (f) => f * TAU - Math.PI / 2;
    ctx.save(); ctx.fillStyle = ctx.strokeStyle = mk; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    const line = (a, r0, r1, lw) => { const p1 = P(C, C, r0, a), p2 = P(C, C, r1, a); ctx.lineWidth = lw; ctx.beginPath(); ctx.moveTo(p1[0], p1[1]); ctx.lineTo(p2[0], p2[1]); ctx.stroke(); };
    const word = (s, a, r, font) => { ctx.save(); ctx.font = font; ctx.translate(C + Math.cos(a) * r, C + Math.sin(a) * r); ctx.rotate(a + Math.PI / 2); ctx.fillText(s, 0, 0); ctx.restore(); };
    if (style === 'mariner') {
      for (let i = 1; i < 60; i++) {
        const a = A(i / 60);
        if (i % 10 === 0) word(String(i), a, 210, '600 16px ' + SANS0);
        else if (i % 5 === 0) { ctx.save(); ctx.translate(C + Math.cos(a) * 210, C + Math.sin(a) * 210); ctx.rotate(a + Math.PI / 2); ctx.fillRect(-1.8, -8, 3.6, 16); ctx.restore(); }
        else if (i < 15) line(a, 214, 221, 1.2);
      }
      const t1 = P(C, C, 202, A(0)), t2 = P(C, C, 222, A(-0.022)), t3 = P(C, C, 222, A(0.022));
      ctx.beginPath(); ctx.moveTo(t1[0], t1[1]); ctx.lineTo(t2[0], t2[1]); ctx.lineTo(t3[0], t3[1]); ctx.closePath(); ctx.fill();
      ctx.fillStyle = '#e9f5d8'; ctx.strokeStyle = '#2a2a2a'; ctx.lineWidth = 1; ctx.beginPath(); ctx.arc(C, C - 215, 3.2, 0, TAU); ctx.fill(); ctx.stroke();
    } else {
      const big = [500, 400, 300, 250, 200, 180, 160, 150, 140, 130, 120, 110, 100, 90, 80, 70, 60], minor = [450, 350, 225, 190, 170, 95, 85, 75, 65];
      big.forEach((v) => { const a = A(60 / v); line(a, 197, 203, 1.2); word(String(v), a, 212, '600 10px ' + SANS0); });
      minor.forEach((v) => line(A(60 / v), 197, 201, 0.8));
      const txt = 'TACHYMETRE';
      for (let i = 0; i < txt.length; i++) word(txt[i], A(0.03 + i * 0.0085), 212, '600 8px ' + SANS0);
    }
    ctx.restore();
  }
  function setLights(o) {
    LIGHT.inner = rad((o.innerAngle == null ? 90 : +o.innerAngle) - 90);
    LIGHT.outer = rad((o.outerAngle == null ? 315 : +o.outerAngle) - 90);
    LIGHT.outerK = o.outerLight == null ? 1 : +o.outerLight;
  }
  const caseMetal = (o) => { if (o.caseMetal && o.caseMetal !== 'auto') return o.caseMetal; const f = resolve(o).finish; return f === 'rhodium' ? 'steel' : CASE_METALS[f] ? f : 'brass'; };
  // standalone builders for picker previews
  function caseLayer(o) { const _ = L(); _.configure(o); setLights(o); _.setTemp(o.outerTemp || 2200, o.outerHue, o.outerSat); const c = buildOuter(LIGHT.outerK, o.caseStyle, caseMetal(o), caseOpenR(o)); _.setTemp(o.temp || 2200, o.hue, o.hueSat); return c; }
  function glassLayer(o) { const _ = L(); _.configure(o); setLights(o); _.setTemp(o.outerTemp || 2200, o.outerHue, o.outerSat); return buildGlass(LIGHT.outerK, o.glassStyle); }

  // ---------- hands: sprites pointing at 12, pivot known — exactly what firmware rotates
  function handFin(k) {
    const F = L().FIN;
    return ({
      blued: { a: '#0d1d48', m: '#24499a', b: '#7aa4ec', edge: 'rgba(4,8,22,.85)' },
      gilt: { a: F.brass.lo, m: F.brass.mid, b: F.brass.hi, edge: 'rgba(40,24,6,.8)' },
      black: { a: '#090908', m: '#1c1b19', b: '#5a5650', edge: 'rgba(0,0,0,.6)' },
      steel: { a: '#3a3e44', m: '#8d939b', b: '#f1f3f5', edge: 'rgba(10,12,14,.8)' },
      white: { a: '#b9b6ae', m: '#e8e5dd', b: '#ffffff', edge: 'rgba(18,18,18,.8)' },
      orange: { a: '#8f340b', m: '#de601c', b: '#ffb37c', edge: 'rgba(40,10,0,.8)' },
      red: { a: '#7a0b10', m: '#e8232d', b: '#ff9a9e', edge: 'rgba(30,0,2,.8)' },
      magenta: { a: '#6e0d47', m: '#ff2a9d', b: '#ffb0dc', edge: 'rgba(30,0,16,.8)' },
      yellow: { a: '#8a6a00', m: '#ffd21a', b: '#fff6b8', edge: 'rgba(30,20,0,.8)' },
      lime: { a: '#2d6b06', m: '#7dff4a', b: '#e2ffcf', edge: 'rgba(6,24,0,.8)' },
      mint: { a: '#1e7a5a', m: '#6ff0c0', b: '#ddfff2', edge: 'rgba(0,24,16,.8)' },
      cyan: { a: '#05616e', m: '#19d6ec', b: '#c4f8ff', edge: 'rgba(0,20,26,.8)' },
      violet: { a: '#2a0f73', m: '#8b46ff', b: '#d6c2ff', edge: 'rgba(12,0,34,.8)' },
    })[k] || { a: '#0d1d48', m: '#24499a', b: '#7aa4ec', edge: 'rgba(4,8,22,.85)' };
  }
  function handParts(style, kind, len, w, tail, sec) {
    const Pp = [], cut = [];
    let span = Math.max(w * 1.6, 3.4), lumeFrom = null, lumeOnly = null;
    const poly = (pts) => (x) => { pts.forEach(([a, b], i) => (i ? x.lineTo(a, b) : x.moveTo(a, b))); x.closePath(); };
    const circ = (cx, cy, r) => (x) => { x.moveTo(cx + r, cy); x.arc(cx, cy, r, 0, TAU); };
    const thick = (pts, hwf) => (x) => {
      const Lt = [], Rt = [];
      pts.forEach((p, i) => { const a = pts[Math.max(0, i - 1)], b = pts[Math.min(pts.length - 1, i + 1)], dx = b[0] - a[0], dy = b[1] - a[1], n = Math.hypot(dx, dy) || 1, nx = -dy / n, ny = dx / n, hh = typeof hwf === 'function' ? hwf(i / (pts.length - 1)) : hwf; Lt.push([p[0] + nx * hh, p[1] + ny * hh]); Rt.push([p[0] - nx * hh, p[1] - ny * hh]); });
      Lt.concat(Rt.reverse()).forEach(([a, b], i) => (i ? x.lineTo(a, b) : x.moveTo(a, b))); x.closePath();
    };
    const star = (cx, cy, ro, ri, n) => poly(Array.from({ length: n * 2 }, (_, i) => { const a = (i / (n * 2)) * TAU - Math.PI / 2, r = i % 2 ? ri : ro; return [cx + Math.cos(a) * r, cy + Math.sin(a) * r]; }));
    if (kind === 'second') {
      const cr = Math.max(w * 1.5, 3), cy = tail * 0.62, ss = sec || 'classic';
      const shaft = (y1, w0, w1) => Pp.push(poly([[-w0, tail], [-w1, y1], [w1, y1], [w0, tail]]));
      const cw = () => { Pp.push(circ(0, cy, cr)); if (style === 'breguet' || style === 'spade') cut.push(circ(0, cy, cr * 0.55)); };
      const big = (k) => Math.max(w * k, len * 0.018 * k);
      if (ss === 'lollipop') { const r = Math.max(w * 1.6, len * 0.03); shaft(-len, w * 0.34, w * 0.16); Pp.push(circ(0, -len * 0.8, r)); Pp.push(circ(0, tail * 0.62, r * 0.92)); span = Math.max(span, r + 1.5); }
      else if (ss === 'ring') { const r = big(1.4); shaft(-len, w * 0.3, w * 0.14); Pp.push(circ(0, -len * 0.8, r)); cut.push(circ(0, -len * 0.8, r * 0.6)); cw(); span = Math.max(span, r + 1.5); }
      else if (ss === 'arrowtip') { const a = big(1.15); shaft(-len * 0.88, w * 0.3, w * 0.16); Pp.push(poly([[-a, -len * 0.84], [0, -len], [a, -len * 0.84], [0, -len * 0.88]])); cw(); span = Math.max(span, a + 1.5); }
      else if (ss === 'paddle') { const a = big(0.85); shaft(-len, w * 0.3, w * 0.14); Pp.push(poly([[-a, -len * 0.6], [-a, -len * 0.84], [0, -len * 0.88], [a, -len * 0.84], [a, -len * 0.6]])); cw(); span = Math.max(span, a + 1.5); }
      else if (ss === 'bolt') {
        const z = big(1.1); Pp.push(thick([[0, tail], [0, -len * 0.3], [z, -len * 0.42], [-z, -len * 0.56], [z * 0.5, -len * 0.66], [0, -len * 0.74], [0, -len]], (t) => w * (0.32 - 0.18 * t)));
        Pp.push(poly([[-w * 0.18, -len * 0.98], [0, -len - 0.5], [w * 0.18, -len * 0.98]])); cw(); span = Math.max(span, z + w + 1.5);
      } else if (ss === 'crescent') { const R = cr * 1.6; shaft(-len, w * 0.3, w * 0.14); Pp.push(circ(0, tail * 0.55, R)); cut.push(circ(0, tail * 0.55 + R * 0.5, R * 0.85)); span = Math.max(span, R + 1.5); }
      else if (ss === 'star') { const R = cr * 1.9; shaft(-len, w * 0.3, w * 0.14); Pp.push(star(0, tail * 0.55, R, R * 0.45, 5)); span = Math.max(span, R + 1.5); }
      else if (ss === 'station') {
        const r = Math.max(w * 3.2, len * 0.095), yc = -len * 0.73, rw = Math.max(1.2, w * 0.95), tl = len * 0.33;
        Pp.push(poly([[-rw, tl], [-rw, yc], [rw, yc], [rw, tl]])); Pp.push(circ(0, yc, r));
        span = Math.max(span, r + 1.5);
      }
      else if (ss === 'flieger') {
        const r = Math.max(w * 1.8, len * 0.05), rw = Math.max(w * 0.42, len * 0.011);
        Pp.push(poly([[-rw, tail * 0.8], [-rw, -len + rw], [rw, -len + rw], [rw, tail * 0.8]])); Pp.push(circ(0, -len + rw, rw));
        [[-len * 0.85, 1], [0, 0.95], [tail * 0.8, 0.9]].forEach(([y, k]) => Pp.push(circ(0, y, r * k)));
        lumeOnly = [[-len * 0.85, 1], [0, 0.95], [tail * 0.8, 0.9]].map(([y, k]) => circ(0, y, r * k * 0.72));
        span = Math.max(span, r + 1.5);
      }
      else if (ss === 'needle') { shaft(-len, w * 0.22, w * 0.07); Pp.push(circ(0, cy, cr * 1.3)); cut.push(circ(0, cy, cr * 0.6)); span = Math.max(span, cr * 1.3 + 1.5); }
      else { shaft(-len, w * 0.3, w * 0.14); cw(); }
      Pp.push(circ(0, 0, Math.max(w * 1.1, 2.4)));
      return { Pp, cut, span: Math.max(span, cr + 1.5), lumeOnly };
    }
    if (style === 'station') {
      const t2 = kind === 'sub' ? tail : len * 0.24;
      Pp.push(poly([[-w * 0.8, t2], [-w * 0.64, -len], [w * 0.64, -len], [w * 0.8, t2]]));
      span = Math.max(span, w * 0.8 + 1.5);
    } else if (style === 'aviator') {
      if (kind === 'hour') {
        Pp.push(poly([[-w * 0.5, 0], [-w * 0.54, -len * 0.52], [-w * 0.72, -len * 0.7], [-w * 1.55, -len * 0.765], [0, -len], [w * 1.55, -len * 0.765], [w * 0.72, -len * 0.7], [w * 0.54, -len * 0.52], [w * 0.5, 0]]));
        lumeFrom = -len * 0.53; span = Math.max(span, w * 1.55 + 1.5);
      } else {
        Pp.push(poly([[-w * 0.45, 0], [-w * 0.5, -len * 0.1], [-w * 1.75, -len * 0.82], [0, -len], [w * 1.75, -len * 0.82], [w * 0.5, -len * 0.1], [w * 0.45, 0]]));
        lumeFrom = -len * 0.2; span = Math.max(span, w * 1.75 + 1.5);
      }
      const Rr = kind === 'hour' ? Math.max(w * 1.3, len * 0.09) : Math.max(w * 1.5, len * 0.062);
      Pp.push(circ(0, 0, Rr)); cut.push(circ(0, 0, Rr * 0.64)); span = Math.max(span, Rr + 1.5);
    } else if (style === 'cathedral') {
      const y0 = -len * 0.2, y1 = -len * 0.8, L1 = y0 - y1, h2 = w * 1.25;
      Pp.push(poly([[-w * 0.24, tail], [-w * 0.2, y0 + 1], [w * 0.2, y0 + 1], [w * 0.24, tail]]));
      Pp.push(poly([[-w * 0.25, y0], [-h2, y0 - L1 * 0.18], [-h2 * 0.85, y0 - L1 * 0.62], [0, y1], [h2 * 0.85, y0 - L1 * 0.62], [h2, y0 - L1 * 0.18], [w * 0.25, y0]]));
      const dia = (yc, a, b) => poly([[0, yc - b], [a, yc], [0, yc + b], [-a, yc]]);
      cut.push(dia(y0 - L1 * 0.3, h2 * 0.5, L1 * 0.13), dia(y0 - L1 * 0.63, h2 * 0.32, L1 * 0.11), dia(y0 - L1 * 0.07, h2 * 0.22, L1 * 0.04));
      Pp.push(poly([[-w * 0.18, y1 + 2], [0, -len], [w * 0.18, y1 + 2]]));
      span = Math.max(span, h2 + 1.5);
    } else if (style === 'poire') {
      const R = w * 1.25, yc = -len * 0.58, yt = yc - R * 2.6;
      const pear = (k) => (x) => { const r = R * k, t = yc - (yc - yt) * k; x.moveTo(0, t); x.quadraticCurveTo(-r * 0.95, yc - r * 0.95, -r, yc); x.arc(0, yc, r, Math.PI, 0, true); x.quadraticCurveTo(r * 0.95, yc - r * 0.95, 0, t); x.closePath(); };
      Pp.push(poly([[-w * 0.22, tail], [-w * 0.18, yc], [w * 0.18, yc], [w * 0.22, tail]]));
      Pp.push(pear(1)); cut.push((x) => { x.save(); x.translate(0, -R * 0.12); pear(0.58)(x); x.restore(); });
      Pp.push(poly([[-w * 0.16, yt + 2], [0, -len], [w * 0.16, yt + 2]]));
      span = Math.max(span, R + 1.5);
    } else if (style === 'fleur') {
      const yc = -len * 0.72;
      Pp.push(poly([[-w * 0.22, tail], [-w * 0.17, yc], [w * 0.17, yc], [w * 0.22, tail]]));
      Pp.push((x) => { x.moveTo(0, yc + w * 0.4); x.quadraticCurveTo(-w * 1.05, yc - (len + yc) * 0.45, 0, -len); x.quadraticCurveTo(w * 1.05, yc - (len + yc) * 0.45, 0, yc + w * 0.4); x.closePath(); });
      [-1, 1].forEach((sd) => { Pp.push(circ(sd * w * 0.95, yc + w * 0.15, w * 0.58)); cut.push(circ(sd * w * 0.95, yc + w * 0.15, w * 0.24)); });
      Pp.push(poly([[-w * 0.8, yc + w * 0.8], [w * 0.8, yc + w * 0.8], [w * 0.8, yc + w * 1.15], [-w * 0.8, yc + w * 1.15]]));
      span = Math.max(span, w * 1.6 + 1.5);
    } else if (style === 'serpentine') {
      const pts = Array.from({ length: 40 }, (_, i) => { const t = i / 39, y = tail - (tail + len * 0.9) * t; return [Math.sin(t * Math.PI * 4) * w * 0.45 * (1 - t * 0.7) * Math.min(1, t * 4), y]; });
      Pp.push(thick(pts, (t) => w * (0.36 - 0.18 * t)));
      Pp.push(poly([[-w * 0.2, -len * 0.89], [0, -len], [w * 0.2, -len * 0.89]]));
      span = Math.max(span, w * 0.9 + 1.5);
    } else if (style === 'louis') {
      const yc = -len * 0.6;
      Pp.push(poly([[-w * 0.22, tail], [-w * 0.18, yc], [w * 0.18, yc], [w * 0.22, tail]]));
      [-1, 1].forEach((sd) => { Pp.push(circ(sd * w * 0.92, yc, w * 0.62)); cut.push(circ(sd * w * 0.92, yc, w * 0.3)); });
      Pp.push((x) => { x.moveTo(0, yc + w * 0.2); x.bezierCurveTo(-w * 1.25, yc - (len + yc) * 0.25, -w * 0.5, -len * 0.9, 0, -len); x.bezierCurveTo(w * 0.5, -len * 0.9, w * 1.25, yc - (len + yc) * 0.25, 0, yc + w * 0.2); x.closePath(); });
      cut.push((x) => { x.moveTo(0, yc - w * 0.6); x.quadraticCurveTo(-w * 0.4, yc - (len + yc) * 0.35, 0, yc - (len + yc) * 0.62); x.quadraticCurveTo(w * 0.4, yc - (len + yc) * 0.35, 0, yc - w * 0.6); x.closePath(); });
      span = Math.max(span, w * 1.6 + 1.5);
    } else if (style === 'broad') {
      if (kind === 'minute') {
        Pp.push(poly([[-w * 0.42, tail], [-w * 0.42, -len + w * 0.6], [0, -len], [w * 0.42, -len + w * 0.6], [w * 0.42, tail]]));
        cut.push(poly([[-w * 0.17, -len * 0.18], [-w * 0.17, -len + w * 1.4], [w * 0.17, -len + w * 1.4], [w * 0.17, -len * 0.18]]));
      } else {
        const hb = -len * 0.7;
        Pp.push(poly([[-w * 0.3, tail], [-w * 0.72, hb], [-w * 1.4, hb], [0, -len], [w * 1.4, hb], [w * 0.72, hb], [w * 0.3, tail]]));
        cut.push(poly([[-w * 0.12, -len * 0.14], [-w * 0.44, hb + w * 0.2], [w * 0.44, hb + w * 0.2], [w * 0.12, -len * 0.14]]));
      }
    } else if (style === 'baton') Pp.push(poly([[-w / 2, tail], [-w * 0.42, -len + w * 0.35], [0, -len], [w * 0.42, -len + w * 0.35], [w / 2, tail]]));
    else if (style === 'sword') Pp.push(poly([[-w * 0.22, tail], [-w * 0.55, -len * 0.18], [0, -len], [w * 0.55, -len * 0.18], [w * 0.22, tail]]));
    else if (style === 'leaf') Pp.push((x) => { x.moveTo(0, tail); x.quadraticCurveTo(-w * 1.5, -len * 0.42, 0, -len); x.quadraticCurveTo(w * 1.5, -len * 0.42, 0, tail); x.closePath(); });
    else if (style === 'dauphine') Pp.push(poly([[-w * 0.3, tail], [-w * 0.8, -len * 0.14], [0, -len], [w * 0.8, -len * 0.14], [w * 0.3, tail]]));
    else if (style === 'pencil') Pp.push(poly([[-w * 0.36, tail], [-w * 0.36, -len + w * 1.1], [0, -len], [w * 0.36, -len + w * 1.1], [w * 0.36, tail]]));
    else if (style === 'skeleton') {
      Pp.push(poly([[-w / 2, tail], [-w * 0.42, -len + w * 0.35], [0, -len], [w * 0.42, -len + w * 0.35], [w / 2, tail]]));
      cut.push(poly([[-w * 0.2, -len * 0.16], [-w * 0.15, -len + w * 1.3], [w * 0.15, -len + w * 1.3], [w * 0.2, -len * 0.16]]));
    } else if (style === 'arrow') {
      Pp.push(poly([[-w * 0.18, tail], [-w * 0.14, -len * 0.74], [w * 0.14, -len * 0.74], [w * 0.18, tail]]));
      Pp.push(poly([[-w * 0.9, -len * 0.7], [0, -len], [w * 0.9, -len * 0.7], [0, -len * 0.78]]));
    } else if (style === 'spade') {
      const s = w * 1.15, y0 = -len * 0.58, y1 = -len * 0.86, ym = y1 + (y0 - y1) * 0.35;
      Pp.push(poly([[-w * 0.2, tail], [-w * 0.16, y0 + 2], [w * 0.16, y0 + 2], [w * 0.2, tail]]));
      Pp.push((x) => { x.moveTo(0, y0); x.bezierCurveTo(-s * 1.25, y0 + w * 0.3, -s * 1.1, ym, 0, y1); x.bezierCurveTo(s * 1.1, ym, s * 1.25, y0 + w * 0.3, 0, y0); x.closePath(); });
      Pp.push(poly([[-w * 0.14, y1 + 3], [0, -len], [w * 0.14, y1 + 3]]));
    } else if (style === 'crescent' || style === 'diamond' || style === 'sunburst' || style === 'pierced' || style === 'pennant') {
      // Pointer hand for a printed perimeter scale: a fine needle whose entire character
      // sits at the extreme tip, where it actually indicates. The needle stays narrow so it
      // never hides the numerals it is read against, which is also why w is the needle's
      // real thickness here -- a fraction of it would vanish at e-ink resolution.
      const R = Math.max(len * 0.085, w * 1.9), tip = [], orn = [], nsw = Math.max(w * 0.5, 1.5), cw = Math.max(w * 0.95, 2.2);
      let cy = -len + R, ny = cy;
      if (style === 'crescent') {
        // a moon at the tip whose opening faces OUTWARD, toward the scale it indicates: the
        // second disc is offset along the hand's own axis, so the tick being read sits in
        // the cup between the horns. cy is hung off the horn line rather than the disc
        // centre, because the reach is only real where there is ink to put the tick under.
        const d = R * 0.62, rc = R * 0.88, horn = (rc * rc - R * R - d * d) / (2 * d);
        cy = -len - horn; ny = cy + R * 0.4;
        orn.push(circ(0, cy, R)); tip.push(orn[0]); cut.push(circ(0, cy - d, rc));
      } else if (style === 'diamond') {
        const a = R * 0.78, b = R * 1.3; cy = -len + b;
        orn.push(poly([[0, cy - b], [a, cy], [0, cy + b], [-a, cy]])); tip.push(orn[0]);
        cut.push(poly([[0, cy - b * 0.5], [a * 0.42, cy], [0, cy + b * 0.5], [-a * 0.42, cy]]));
      } else if (style === 'sunburst') {
        const ro = R * 1.08; cy = -len + ro;
        orn.push(star(0, cy, ro, ro * 0.34, 8)); tip.push(orn[0]); orn.push(circ(0, cy, ro * 0.32)); tip.push(orn[1]);
      } else if (style === 'pierced') {
        const rp = R * 0.98; cy = -len + rp;
        orn.push(circ(0, cy, rp)); tip.push(orn[0]); cut.push(circ(0, cy, rp * 0.54));
      } else {
        const a = R * 1.02, b = R * 1.62; cy = -len + b * 0.62;
        orn.push(poly([[0, -len], [-a, -len + b], [0, -len + b * 0.6], [a, -len + b]])); tip.push(orn[0]);
      }
      // needle, and a lozenge counterweight outboard of the arbour so the hand balances.
      // ny is the needle's end: at the disc centre for the solid heads, sunk into the belly
      // for the crescent, whose middle is scooped out by the opening.
      Pp.push(poly([[-nsw, tail * 0.9], [-nsw * 0.55, ny], [nsw * 0.55, ny], [nsw, tail * 0.9]]));
      Pp.push(poly([[0, tail + cw * 0.85], [cw, tail * 0.55], [0, tail * 0.1], [-cw, tail * 0.55]]));
      orn.forEach((p) => Pp.push(p));
      span = Math.max(span, R * 1.15 + 1.5);
      lumeOnly = tip;
    } else {
      const cy = -len * 0.7, Ro = w * 1.3, Ri = w * 0.8;
      Pp.push(poly([[-w * 0.22, tail], [-w * 0.17, cy], [w * 0.17, cy], [w * 0.22, tail]]));
      Pp.push(circ(0, cy, Ro)); cut.push(circ(0, cy, Ri));
      Pp.push(poly([[-w * 0.24, cy - Ro * 0.85], [0, -len], [w * 0.24, cy - Ro * 0.85]]));
    }
    Pp.push(circ(0, 0, w * 0.95));
    return { Pp, cut, span, lumeFrom, lumeOnly };
  }
  function handSprite(style, kind, len, w, fin, sec, lume, flat) {
    const tail = kind === 'second' ? len * 0.26 : kind === 'sub' ? len * 0.2 : len * 0.16;
    const { Pp, cut, span, lumeFrom, lumeOnly } = handParts(style, kind, len, w, tail, sec);
    const xt = style === 'station' && kind !== 'sub' ? len * 0.24 - tail : kind === 'second' && sec === 'station' ? len * 0.33 - tail : 0;
    const hw = Math.ceil(span + 3), top = 3, c = cv(hw * 2, Math.ceil(top + len + tail + Math.max(0, xt) + span + 4)), x = c.getContext('2d');
    const px = hw, py = top + len;
    x.translate(px, py);
    const out = cv(c.width, c.height), ox = out.getContext('2d');
    x.fillStyle = '#fff'; Pp.forEach((p) => { x.beginPath(); p(x); x.fill(); });
    x.globalCompositeOperation = 'destination-out'; cut.forEach((p) => { x.beginPath(); p(x); x.fill(); });
    x.globalCompositeOperation = 'source-atop';
    let g;
    if (flat) { x.fillStyle = fin.m; x.fillRect(-hw, -py, hw * 2, c.height); }
    else {
      g = x.createLinearGradient(-w * 1.4, 0, w * 1.4, 0);
      g.addColorStop(0, fin.m); g.addColorStop(0.47, fin.b); g.addColorStop(0.5, fin.m); g.addColorStop(1, fin.a);
      x.fillStyle = g; x.fillRect(-hw, -py, hw * 2, c.height);
      g = x.createLinearGradient(0, -len, 0, tail);
      g.addColorStop(0, 'rgba(255,255,255,.1)'); g.addColorStop(0.6, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,.25)');
      x.fillStyle = g; x.fillRect(-hw, -py, hw * 2, c.height);
    }
    if (lume && lume !== 'none' && lume !== 'auto') {
      const lc = cv(c.width, c.height), lx = lc.getContext('2d'), ins = kind === 'second' ? 0.6 : Math.max(0.7, w * 0.2);
      lx.translate(px, py); lx.fillStyle = '#fff'; Pp.forEach((p) => { lx.beginPath(); p(lx); lx.fill(); });
      lx.globalCompositeOperation = 'destination-out'; lx.lineWidth = ins * 2; lx.lineJoin = 'round'; lx.strokeStyle = '#000'; lx.fillStyle = '#000';
      Pp.forEach((p) => { lx.beginPath(); p(lx); lx.stroke(); });
      cut.forEach((p) => { lx.beginPath(); p(lx); lx.fill(); lx.stroke(); });
      if (!lumeOnly) { lx.beginPath(); lx.arc(0, 0, Math.max(w * 1.5, 3.5), 0, TAU); lx.fill(); }
      if (lumeOnly) { lx.globalCompositeOperation = 'destination-in'; lx.beginPath(); lumeOnly.forEach((p) => p(lx)); lx.fill(); lx.globalCompositeOperation = 'destination-out'; }
      else if (kind === 'second') lx.fillRect(-hw, -len * 0.5, hw * 2, len * 0.5 + tail * 0.3); else lx.fillRect(-hw, lumeFrom != null ? lumeFrom : -len * 0.14, hw * 2, c.height);
      lx.globalCompositeOperation = 'source-in'; lx.fillStyle = lume; lx.fillRect(-hw, -py, hw * 2, c.height);
      if (!flat) { lx.globalCompositeOperation = 'source-atop'; } const lg = flat ? null : lx.createLinearGradient(-w, 0, w, 0); if (lg) { lg.addColorStop(0, 'rgba(255,255,255,.18)'); lg.addColorStop(0.5, 'rgba(255,255,255,0)'); lg.addColorStop(1, 'rgba(0,0,0,.16)'); lx.fillStyle = lg; lx.fillRect(-hw, -py, hw * 2, c.height); }
      x.globalCompositeOperation = 'source-over'; x.setTransform(1, 0, 0, 1, 0, 0); x.drawImage(lc, 0, 0); x.translate(px, py);
    }
    // dark edge underneath so the hand reads on any dial colour
    if (flat) { ox.drawImage(c, 0, 0); return { c: out, px, py }; }
    ox.translate(px, py); ox.fillStyle = ox.strokeStyle = fin.edge || '#000'; ox.lineWidth = 1.3; ox.lineJoin = 'round';
    Pp.forEach((p) => { ox.beginPath(); p(ox); ox.fill(); ox.stroke(); });
    ox.globalCompositeOperation = 'destination-out'; ox.lineWidth = 0;
    cut.forEach((p) => { ox.beginPath(); p(ox); ox.save(); ox.clip(); ox.fillRect(-hw, -py, hw * 2, out.height); ox.restore(); });
    ox.globalCompositeOperation = 'source-over'; ox.strokeStyle = fin.edge || '#000'; ox.lineWidth = 1.3;
    cut.forEach((p) => { ox.beginPath(); p(ox); ox.stroke(); });
    ox.setTransform(1, 0, 0, 1, 0, 0); ox.drawImage(c, 0, 0);
    return { c: out, px, py };
  }
  // every hand the layout offers, in natural depth order (subdial hands under the centre stack).
  // The additional calendar pointer is listed only while it is the one the dial shows: a dial has
  // a single spare hand, so the hands list never carries four hidden rows to tick on and off.
  function handSlots(lay, design) {
    const out = [];
    lay.subs.forEach((s) => { if (SUBKIND[s.scale]) out.push({ id: s.id, kind: SUBKIND[s.scale], sub: true }); });
    ['hour', 'minute', 'second'].forEach((k) => out.push({ id: 'centre-' + k, kind: k, sub: false }));
    const ce = (design && design.centre) || {};
    CAL_HANDS.forEach((k) => { if (+ce[k]) out.push({ id: 'centre-' + k, kind: k, sub: false }); });
    return out;
  }
  function handOrder(lay, design) {
    const nat = handSlots(lay, design).map((h) => h.id), saved = (design.order || []).filter((id) => nat.includes(id));
    return saved.concat(nat.filter((id) => !saved.includes(id)));
  }
  function buildHands(lay, design, defStyle, defFin, tick) {
    const secT = tickTurn(tick === 'sweep' || !tick ? 0 : +tick);
    const cfg = design.hands || {}, ce = design.centre || {}, PG = perimGeom(design), R = (REACH[design.print] || 205) * (PG ? PG.k : 1), all = {};
    const pickS = (hc, df) => (hc.style && hc.style !== 'auto' ? hc.style : df || defStyle), pickF = (hc) => (hc.finish && hc.finish !== 'auto' ? hc.finish : defFin), pickL = (hc) => (hc.lume && hc.lume !== 'auto' ? hc.lume : design.handLume), pickFlat = (hc) => (hc.shade && hc.shade !== 'auto' ? hc.shade : design.handShade || '3d') === 'flat';
    lay.subs.forEach((s) => {
      const kind = SUBKIND[s.scale], hc = cfg[s.id] || {}; if (!kind || hc.on === false) return;
      const lk = +hc.len || 1, wk = +hc.wid || 1;
      const sec = kind === 'second', len = (s.r - 5) * 0.93 * lk, w = (sec ? Math.max(1.8, s.r * 0.026) : Math.max(3, s.r * 0.07)) * wk, fin = handFin(pickF(hc));
      const base = sec ? secT : TURN[kind], turn = s.t && s.t.dir === 'ccw' ? (d) => -base(d) : base;
      // Uploaded art replaces the drawn hand wholesale. It still answers { c, px, py },
      // so drawHands, the shadow silhouette and the manifest record need no branch --
      // and the metal hub cap steps aside because the art carries its own. fitTo asks
      // the ink's own tip to land at len, the reach of the drawn hand of this role, so
      // the Length slider reads the same for an upload at any source resolution.
      const art = hc.art && assetReady(hc.art) ? handSpriteAsset(hc.art, { maxW: 2 * len + 8, maxH: 2 * len + 8, pivotX: hc.pivotX, pivotY: hc.pivotY, zoom: hc.zoom, fitTo: len, rot0: +hc.rot0 || 0, aimRad: s.rot || 0 }) : null;
      all[s.id] = { id: s.id, kind, x: s.x, y: s.y, rot: s.rot, sp: art || handSprite(pickS(hc), sec ? 'second' : 'sub', len, w, fin, hc.sec || design.secStyle, pickL(hc), pickFlat(hc)), flat: pickFlat(hc), turn, capR: art ? 0 : Math.max(2.6, w * 1.2), fin, art: art || null, artRot: art ? rad(+hc.rot0 || 0) : 0 };
    });
    const dims = { hour: [0.66, 0.05], minute: [0.96, 0.038], second: [0.98, 0.012] };
    ['hour', 'minute', 'second'].forEach((k) => {
      const id = 'centre-' + k, hc = cfg[id] || {}; if (!ce[k] || hc.on === false) return;
      const lk = +hc.len || 1, wk = +hc.wid || 1;
      const [l, wf] = dims[k], w = Math.max(k === 'second' ? 1.8 : 2.5, R * wf) * wk, fin = handFin(pickF(hc));
      const len = Math.min(216, R * l * lk), art = hc.art && assetReady(hc.art) ? handSpriteAsset(hc.art, { maxW: 2 * len + 8, maxH: 2 * len + 8, pivotX: hc.pivotX, pivotY: hc.pivotY, zoom: hc.zoom, fitTo: len, rot0: +hc.rot0 || 0 }) : null;
      all[id] = { id, kind: k, x: C, y: C, rot: 0, sp: art || handSprite(pickS(hc), k === 'second' ? 'second' : k, len, w, fin, hc.sec || design.secStyle, pickL(hc), pickFlat(hc)), flat: pickFlat(hc), turn: k === 'second' ? secT : TURN[k], capR: art ? 0 : (k === 'second' && (hc.sec || design.secStyle) === 'flieger' ? 2.4 : R * 0.034 * Math.max(1, Math.sqrt(wk))), fin, art: art || null, artRot: art ? rad(+hc.rot0 || 0) : 0 };
    });
    // The four calendar pointers share the centre arbour. Each reaches the ring that feeds
    // it: with a perimeter printed its tip lands on the ticks, without one it stops short
    // of the chapter print. The default tip names the data the hand carries, so the dial
    // reads without a legend -- sun for the 24-hour, diamond for the second-zone GMT,
    // crescent for the day, pennant for the date.
    const CAL_DEF = { hours24: 'sunburst', gmt: 'diamond', day: 'crescent', date: 'pennant' };
    // length applies ONLY with no ring printed, where the tip must fall inside the chapter
    // print -- the numerals print at 181..190 depending on the style, so all four stop at
    // 161..174 and the four heads land at four different radii instead of one clump.
    const CAL_DIM = { hours24: [0.85, 0.0155], gmt: [0.865, 0.015], day: [0.8, 0.017], date: [0.835, 0.0165] };
    CAL_HANDS.forEach((k) => {
      const id = 'centre-' + k, hc = cfg[id] || {}; if (!ce[k] || hc.on === false) return;
      const [l, wf] = CAL_DIM[k], lk = +hc.len || 1, wk = +hc.wid || 1, fin = handFin(pickF(hc));
      const reach = PG ? PG.reach : R * l, len = Math.min(222, reach * lk), w = Math.max(1.7, R * wf) * wk;
      const base = k === 'gmt' ? gmtTurn(design.gmtOffset) : TURN[k];
      // labels between the lines move by half a division, and the pointer that reads them must
      // move with them or it indicates the neighbour; the same holds for the ring rotation the
      // user set. offTurn carries both, and only the hand whose own ring prints picks it up.
      const off = PG && EXTRA_RING[k] === design.perim ? PG.offTurn : 0;
      const turn = off ? ((d) => base(d) + off) : base;
      all[id] = { id, kind: k, x: C, y: C, rot: 0, sp: handSprite(pickS(hc, CAL_DEF[k]), 'hour', len, w, fin, null, pickL(hc), pickFlat(hc)), flat: pickFlat(hc), turn, capR: Math.max(2.2, w * 1.1), fin };
    });
    const list = handOrder(lay, design).filter((id) => all[id]).map((id) => all[id]);
    // each hand sits higher than the one below it, so shadows lengthen up the stack; the cap crowns the top hand of each arbor
    const top = {}, key = (h) => h.x.toFixed(1) + ',' + h.y.toFixed(1);
    list.forEach((h, i) => { top[key(h)] = i; h.h = 2.2 + i * 1.1; });
    list.forEach((h, i) => (h.cap = top[key(h)] === i ? h.capR : 0));
    return list;
  }
  const HT = { c: null };
  /* The absolute angle the device turns this hand to: its own data turn, the tilt of
   * the subdial it is mounted on, and the author's facing correction for uploaded art
   * (an image need not have been drawn pointing at 12). ONE function, so the shadow
   * silhouette, the stage hit test, the alignment guide and the kit's
   * zeroRotationDeg can never disagree about where a hand points. */
  const handAngle = (hd, date) => (hd.freeze != null ? hd.freeze : hd.turn(date) * TAU) + (hd.rot || 0) + (hd.artRot || 0);
  // the standing offset that ships in the manifest, in degrees, so the firmware adds
  // the same facing the composer preview applies
  const handZeroDeg = (hd) => +((((hd.rot || 0) + (hd.artRot || 0)) * 180) / Math.PI).toFixed(2);
  /* Point-in-sprite test in the hand's own turned frame. The pivot may sit outside
   * the ink, so the box is anchored on the pivot rather than on the canvas corner. */
  function handHit(hd, x, y, date, m) {
    if (!hd || !hd.sp) return false;
    const a = handAngle(hd, date), c = Math.cos(-a), s = Math.sin(-a);
    const dx = x - hd.x, dy = y - hd.y, lx = dx * c - dy * s, ly = dx * s + dy * c;
    const pad = m == null ? 2.5 : m;
    return lx >= -hd.sp.px - pad && lx <= hd.sp.c.width - hd.sp.px + pad
      && ly >= -hd.sp.py - pad && ly <= hd.sp.c.height - hd.sp.py + pad;
  }
  function shadowCfg(o, Lt) {
    const auto = !(+o.shAngle >= 0), a = auto ? Lt.outer + Math.PI : rad(+o.shAngle - 90);
    return { dir: [Math.cos(a), Math.sin(a)], deg: Math.round((((a * 180) / Math.PI + 90) % 360 + 360) % 360), auto, dist: o.shDist == null ? 1 : +o.shDist, blur: o.shBlur == null ? 1 : +o.shBlur, op: o.shOp == null ? 1 : +o.shOp, kk: Math.min(1.5, Lt.outerK) };
  }
  /* Canvas casts shadows in DEVICE space: shadowOffsetX/Y and shadowBlur ignore the CTM.
   * compose eases the "no case" opening through one scale about the centre and the chapter
   * pull-in pushes another, so every live cast here pre-multiplies its distances by the
   * CTM's own scale -- sqrt|det|, so a translate or a rotate cannot stretch a distance.
   * Without this the hand's cast stays full size (and, offset by the 3000px silhouette
   * stand-in, drifts by OFF*(1-k)) while the hand shrinks: the projection peels off and
   * the hand reads as lifted off the dial the moment the opening slider moves. */
  const devS = (x) => { const m = x.getTransform && x.getTransform(); if (!m) return 1; const s = Math.sqrt(Math.abs(m.a * m.d - m.b * m.c)); return s > 1e-6 ? s : 1; };
  function drawHands(ctx, hands, date, Lt, SH) {
    SH = SH || shadowCfg({}, Lt);
    const [ax, ay] = SH.dir, kk = SH.kk, dm = SH.dist, bm = SH.blur, om = SH.op, sk = devS(ctx);
    if (!HT.c) HT.c = cv(S);
    const t = HT.c.getContext('2d');
    hands.forEach((hd) => {
      t.setTransform(1, 0, 0, 1, 0, 0); t.clearRect(0, 0, S, S);
      t.translate(hd.x, hd.y); t.rotate(handAngle(hd, date)); t.drawImage(hd.sp.c, -hd.sp.px, -hd.sp.py);
      const h = hd.h;
      [[ax * 1.6 * h * dm, ay * 1.6 * h * dm, 1.4 * h * bm, Math.min(1, 0.5 * kk * om)], [ax * 0.4 * h * dm, (ay * 0.4 * h + 0.3 * h) * dm, 0.6 * h * bm, Math.min(1, 0.35 * om)]].forEach(([ox, oy, b, a]) => {
        ctx.save(); ctx.shadowColor = `rgba(0,0,0,${a})`; ctx.shadowBlur = b * sk; ctx.shadowOffsetX = (OFF + ox) * sk; ctx.shadowOffsetY = oy * sk;
        ctx.drawImage(HT.c, -OFF, 0); ctx.restore();
      });
      ctx.drawImage(HT.c, 0, 0);
      if (hd.cap) {
        const r = hd.cap, f = hd.fin;
        ctx.save(); ctx.shadowColor = 'rgba(0,0,0,' + Math.min(1, 0.55 * om) + ')'; ctx.shadowBlur = 3 * bm * sk; [ctx.shadowOffsetX, ctx.shadowOffsetY] = [ax * 2.2 * dm * sk, ay * 2.2 * dm * sk];
        let g = f.m;
        if (!hd.flat) { g = ctx.createRadialGradient(hd.x - r * 0.35, hd.y - r * 0.35, r * 0.1, hd.x, hd.y, r); g.addColorStop(0, f.b); g.addColorStop(0.6, f.m); g.addColorStop(1, f.a); }
        ctx.fillStyle = g; ctx.beginPath(); ctx.arc(hd.x, hd.y, r, 0, TAU); ctx.fill(); ctx.restore();
        if (!hd.flat) { ctx.fillStyle = 'rgba(0,0,0,.55)'; ctx.beginPath(); ctx.arc(hd.x, hd.y, r * 0.32, 0, TAU); ctx.fill(); }
      }
    });
  }
  function handPreview(ctx, style, fk, n, bg) {
    const fin = handFin(fk), R = n * 0.46;
    ctx.clearRect(0, 0, n, n); ctx.fillStyle = bg; ctx.beginPath(); ctx.arc(n / 2, n / 2, n / 2, 0, TAU); ctx.fill();
    [['hour', 0.6, 0.12, (10 + 9 / 60) / 12], ['minute', 0.9, 0.095, 9 / 60]].forEach(([k, l, wf, turn]) => {
      const sp = handSprite(style, k, R * l, R * wf, fin);
      ctx.save(); ctx.translate(n / 2, n / 2); ctx.rotate(turn * TAU);
      ctx.shadowColor = 'rgba(0,0,0,.5)'; ctx.shadowBlur = 2; ctx.shadowOffsetX = 1; ctx.shadowOffsetY = 1.5;
      ctx.drawImage(sp.c, -sp.px, -sp.py); ctx.restore();
    });
    const g = ctx.createRadialGradient(n / 2 - 1, n / 2 - 1, 0.5, n / 2, n / 2, R * 0.08);
    g.addColorStop(0, fin.b); g.addColorStop(1, fin.a);
    ctx.fillStyle = g; ctx.beginPath(); ctx.arc(n / 2, n / 2, R * 0.1, 0, TAU); ctx.fill();
  }

  // ---------- public builders
  // ---------- movements. Wheel counts are multiples of 4 so every wheel lands on an identical pose each loop.
  const BLACK = { hi: '#70747b', mid: '#2c2f34', lo: '#0e0f11', edge: '#050506' };
  const RHOD = { hi: '#eef1f4', mid: '#9aa1a9', lo: '#3c4147', edge: '#141618' };
  // straight bridge with chamfered, pointed ends
  const bar = (x1, y1, x2, y2, w) => {
    const a = Math.atan2(y2 - y1, x2 - x1), nx = (-Math.sin(a) * w) / 2, ny = (Math.cos(a) * w) / 2, ex = Math.cos(a) * w * 0.45, ey = Math.sin(a) * w * 0.45;
    return [[x1 - ex, y1 - ey], [x1 + nx, y1 + ny], [x2 + nx, y2 + ny], [x2 + ex, y2 + ey], [x2 - nx, y2 - ny], [x1 - nx, y1 - ny]];
  };
  const TRAINS = {
    classic: {
      name: 'Classic', blurb: 'Arched bridges on a warm plate', plate: 'classic',
      gears: { front: [{ x: 178, y: 193, N: 56, p: 0.1 }, { parent: 0, ang: -20, N: 42 }, { parent: 0, ang: 120, N: 35 }, { parent: 1, ang: 80, N: 28 }, { parent: 2, ang: 30, N: 42 }], fm: 2.3,
        rear: [{ x: 270, y: 250, N: 56, p: 0.3 }, { parent: 0, ang: 200, N: 35 }, { parent: 0, ang: -70, N: 42 }], rm: 3.15, bal: { x: 206, y: 98, r: 34 } },
      names: ['Centre wheel', 'Third wheel', 'Fourth wheel', 'Escape wheel', 'Intermediate wheel', 'Barrel', 'Rear wheel A', 'Rear wheel B'],
      parts: [{ k: 'bridge', pts: [[330, 30], [323, 105], [395, 160]], w: 20 }, { k: 'screw', x: 323, y: 105, sl: 0.6 }, { k: 'front' },
        { k: 'bridge', pts: [[40, 330], [126, 283]], w: 22 }, { k: 'post', i: 2 }, { k: 'screw', x: 82, y: 307, sl: 2.1 }, { k: 'balance' },
        { k: 'bridge', pts: [[108, 52], [160, 70], [206, 98]], w: 18 }, { k: 'baljewel' }, { k: 'screw', x: 152, y: 67, sl: 1.2 }],
    },
    tourbillon: {
      name: 'Flying tourbillon', blurb: 'Balance in a cage that turns once a minute', plate: 'classic', cage: 48,
      gears: { front: [{ x: 233, y: 160, N: 70, p: 0 }, { parent: 0, ang: 180, N: 42 }, { parent: 0, ang: 0, N: 42 }, { parent: 0, ang: 118, N: 28 }, { parent: 1, ang: 100, N: 35 }], fm: 2.3,
        rear: [{ x: 320, y: 300, N: 56, p: 0.2 }, { parent: 0, ang: -110, N: 35 }], rm: 2.85, bal: { x: 233, y: 340, r: 32 } },
      names: ['Centre wheel', 'Third wheel', 'Crown wheel', 'Fourth wheel', 'Intermediate wheel', 'Rear wheel A', 'Rear wheel B'],
      parts: [{ k: 'front' }, { k: 'bridge', pts: [[50, 190], [140, 112], [233, 160], [326, 112], [416, 190]], w: 22 }, { k: 'screw', x: 140, y: 112, sl: 0.3 }, { k: 'screw', x: 326, y: 112, sl: 2.4 },
        { k: 'post', i: 0 }, { k: 'bridge', pts: [[38, 262], [90, 247]], w: 20 }, { k: 'post', i: 4 }, { k: 'screw', x: 52, y: 258, sl: 1 }, { k: 'cage' }],
    },
    architect: {
      name: 'Architect', blurb: 'Rhodium wheels with lightening holes on a sandblasted plate', plate: 'sandblast', bridgePal: 'rhodium', finish: 'rhodium', spokes: 'holes',
      gears: { front: [{ x: C, y: C, N: 84, p: 0 }, { parent: 0, ang: 30, N: 42 }, { parent: 0, ang: 210, N: 42 }, { parent: 1, ang: -30, N: 28 }, { parent: 2, ang: 150, N: 35 }], fm: 1.85, rear: [], rm: 5, bal: { x: 150, y: 345, r: 32 } },
      names: ['Centre wheel', 'Third wheel', 'Fourth wheel', 'Escape wheel', 'Seconds wheel'],
      parts: [{ k: 'front' }, { k: 'poly', pts: bar(72, 211, 233, 233, 14) }, { k: 'poly', pts: bar(233, 233, 388, 259, 14) },
        { k: 'screw', x: 80, y: 212, r: 4.5 }, { k: 'screw', x: 380, y: 258, r: 4.5 }, { k: 'post', i: 0 }, { k: 'post', i: 1 }, { k: 'post', i: 2 }, { k: 'post', i: 3 }, { k: 'post', i: 4 },
        { k: 'balance' }, { k: 'poly', pts: bar(150, 345, 62, 392, 14) }, { k: 'baljewel' }, { k: 'screw', x: 70, y: 388, r: 4.5 }],
    },
    orbital: {
      name: 'Orbital arrow', blurb: 'Exotic: an arrow-shaped tourbillon carriage that is the seconds hand', plate: 'sandblast', bridgePal: 'rhodium', finish: 'rhodium', spokes: 'thin',
      orbital: { D: 128, tip: 194, tail: 62 },
      gears: { front: [{ x: C, y: C, N: 98, p: 0 }, { parent: 0, ang: 45, N: 28 }, { parent: 0, ang: 135, N: 28 }, { parent: 0, ang: 225, N: 28 }, { parent: 0, ang: 315, N: 28 }], fm: 1.7, rear: [], rm: 5, bal: { x: C, y: C - 128, r: 28 } },
      names: ['Centre wheel', 'Satellite NE', 'Satellite SE', 'Satellite SW', 'Satellite NW'],
      parts: [{ k: 'front' }, { k: 'post', i: 1 }, { k: 'post', i: 2 }, { k: 'post', i: 3 }, { k: 'post', i: 4 }, { k: 'cage' }],
    },
    openworked: {
      name: 'Openworked', blurb: 'Anthracite plate, black angular bridges, fine spokes', plate: 'anthracite', bridgePal: 'black', spokes: 'thin', finish: 'rhodium',
      gears: { front: [{ x: C, y: C, N: 84, p: 0 }, { parent: 0, ang: -45, N: 42 }, { parent: 0, ang: 135, N: 42 }, { parent: 1, ang: -90, N: 28 }, { parent: 2, ang: 90, N: 28 }], fm: 2, rear: [], rm: 5, bal: { x: 128, y: 128, r: 34 } },
      names: ['Centre wheel', 'Third wheel', 'Fourth wheel', 'Escape wheel', 'Seconds wheel'],
      parts: [{ k: 'front' }, { k: 'poly', pts: bar(60, 300, 300, 60, 20) }, { k: 'poly', pts: bar(144, 322, 392, 290, 18) },
        { k: 'screw', x: 66, y: 294 }, { k: 'screw', x: 294, y: 66 }, { k: 'screw', x: 384, y: 291 }, { k: 'post', i: 2 }, { k: 'post', i: 0 },
        { k: 'balance' }, { k: 'poly', pts: bar(40, 190, 128, 128, 18) }, { k: 'baljewel' }, { k: 'screw', x: 50, y: 183 }],
    },
    kite: {
      name: 'Kite carriage', blurb: 'Bold kite-shaped tourbillon bridge orbiting the dial, blued inlay', plate: 'anthracite', bridgePal: 'rhodium', finish: 'rhodium', spokes: 'thin', rear: 'match',
      orbital: { D: 128, tip: 194, tail: 58, shape: 'kite', inlay: 'blued' },
      gears: { front: [{ x: C, y: C, N: 98, p: 0 }, { parent: 0, ang: 45, N: 28 }, { parent: 0, ang: 135, N: 28 }, { parent: 0, ang: 225, N: 28 }, { parent: 0, ang: 315, N: 28 }], fm: 1.7, rear: [], rm: 5, bal: { x: C, y: C - 128, r: 26 } },
      names: ['Centre wheel', 'Satellite NE', 'Satellite SE', 'Satellite SW', 'Satellite NW'],
      parts: [{ k: 'front' }, { k: 'post', i: 1 }, { k: 'post', i: 2 }, { k: 'post', i: 3 }, { k: 'post', i: 4 }, { k: 'cage' }],
    },
    fineclassic: {
      name: 'Fine classic', blurb: 'Delicate · small-module gilt wheels, slim bridges, Geneva stripes', plate: 'stripes', spokes: 'thin', finish: 'brass',
      gears: { front: [{ x: 200, y: 200, N: 49, p: 0.1 }, { parent: 0, ang: -30, N: 35 }, { parent: 1, ang: -100, N: 21 }, { parent: 0, ang: 120, N: 35 }, { parent: 3, ang: 60, N: 21 }, { parent: 2, ang: -20, N: 28 }], fm: 1.55,
        rear: [{ x: 300, y: 290, N: 56, p: 0.2 }, { parent: 0, ang: 200, N: 35 }, { parent: 0, ang: -80, N: 28 }], rm: 1.7, bal: { x: 128, y: 140, r: 28 } },
      names: ['Centre wheel', 'Third wheel', 'Fourth wheel', 'Intermediate wheel', 'Seconds wheel', 'Escape wheel', 'Barrel', 'Ratchet wheel', 'Crown wheel'],
      parts: [{ k: 'front' }, { k: 'post', i: 0 }, { k: 'post', i: 1 }, { k: 'post', i: 3 }, { k: 'post', i: 5 },
        { k: 'poly', pts: bar(250, 372, 372, 300, 9) }, { k: 'screw', x: 258, y: 367, r: 3.4 }, { k: 'screw', x: 364, y: 305, r: 3.4, sl: 1.2 },
        { k: 'balance' }, { k: 'poly', pts: bar(46, 196, 128, 140, 9) }, { k: 'baljewel' }, { k: 'screw', x: 54, y: 191, r: 3.4, sl: 0.4 }],
    },
    finetour: {
      name: 'Fine tourbillon', blurb: 'Delicate · slender cage at 6, fine rhodium wheels on anthracite', plate: 'anthracite', bridgePal: 'rhodium', finish: 'rhodium', spokes: 'thin', rear: 'match', cage: 38,
      gears: { front: [{ x: C, y: C - 20, N: 56, p: 0 }, { parent: 0, ang: -90, N: 35 }, { parent: 1, ang: -30, N: 21 }, { parent: 1, ang: 210, N: 21 }, { parent: 0, ang: 70, N: 28 }], fm: 1.55,
        rear: [{ x: 120, y: 300, N: 49, p: 0.3 }, { parent: 0, ang: -70, N: 28 }], rm: 1.5, bal: { x: C, y: C + 107, r: 22 } },
      names: ['Centre wheel', 'Third wheel', 'Seconds wheel', 'Intermediate wheel', 'Fourth wheel', 'Barrel', 'Ratchet wheel'],
      parts: [{ k: 'front' }, { k: 'poly', pts: bar(150, 140, 316, 140, 8) }, { k: 'screw', x: 156, y: 140, r: 3.2 }, { k: 'screw', x: 310, y: 140, r: 3.2, sl: 1 },
        { k: 'post', i: 0 }, { k: 'post', i: 1 }, { k: 'post', i: 4 }, { k: 'cage' }],
    },
    filigree: {
      name: 'Filigree', blurb: 'Delicate · seven fine copper wheels around the centre, hairline bridges', plate: 'sandblast', bridgePal: 'rhodium', finish: 'copper', spokes: 'thin', rear: 'match',
      gears: { front: [{ x: C, y: C, N: 63, p: 0 }, { parent: 0, ang: 0, N: 42 }, { parent: 1, ang: -90, N: 28 }, { parent: 0, ang: 180, N: 42 }, { parent: 3, ang: 90, N: 28 }, { parent: 0, ang: -90, N: 28 }, { parent: 0, ang: 90, N: 35 }], fm: 1.4,
        rear: [{ x: 130, y: 120, N: 49, p: 0.1 }, { parent: 0, ang: 30, N: 28 }], rm: 1.35, bal: { x: 336, y: 334, r: 26 } },
      names: ['Centre wheel', 'Third wheel', 'Fourth wheel', 'Intermediate wheel', 'Seconds wheel', 'Minute wheel', 'Hour wheel', 'Barrel', 'Ratchet wheel'],
      parts: [{ k: 'front' }, { k: 'post', i: 0 }, { k: 'post', i: 1 }, { k: 'post', i: 3 }, { k: 'post', i: 5 }, { k: 'post', i: 6 },
        { k: 'bridge', pts: [[52, 300], [110, 336], [200, 352]], w: 8 }, { k: 'screw', x: 110, y: 336, r: 3.2, sl: 0.8 },
        { k: 'balance' }, { k: 'poly', pts: bar(336, 334, 414, 254, 8) }, { k: 'baljewel' }, { k: 'screw', x: 406, y: 262, r: 3.2 }],
    },
    skeltour: {
      name: 'Skeleton tourbillon', blurb: 'Fine small-module wheels on a carbon plate, tourbillon at 6', plate: 'carbon', bridgePal: 'rhodium', finish: 'rhodium', spokes: 'thin', cage: 46, rear: 'match',
      gears: { front: [{ x: C, y: C, N: 70, p: 0 }, { parent: 0, ang: -90, N: 42 }, { parent: 1, ang: -35, N: 28 }, { parent: 1, ang: -145, N: 28 }, { parent: 0, ang: 90, N: 28 }, { parent: 0, ang: 150, N: 28 }, { parent: 0, ang: 30, N: 28 }], fm: 1.35,
        rear: [{ x: 128, y: 312, N: 56, p: 0.2 }, { parent: 0, ang: -60, N: 35 }, { parent: 0, ang: 200, N: 28 }], rm: 1.7, bal: { x: C, y: C + 107, r: 27 } },
      names: ['Centre wheel', 'Third wheel', 'Seconds wheel', 'Intermediate wheel', 'Fourth wheel', 'Hour wheel', 'Minute wheel', 'Barrel', 'Ratchet wheel', 'Keyless wheel'],
      parts: [{ k: 'front' }, { k: 'poly', pts: bar(C - 78, 128, C + 78, 128, 9) }, { k: 'screw', x: C - 72, y: 128, r: 3.6 }, { k: 'screw', x: C + 72, y: 128, r: 3.6, sl: 1.1 },
        { k: 'post', i: 0 }, { k: 'post', i: 1 }, { k: 'post', i: 4 }, { k: 'post', i: 5 }, { k: 'post', i: 6 }, { k: 'cage' }],
    },
    inline: {
      name: 'Inline', blurb: 'Vertical train between rails, Geneva stripes', plate: 'stripes', spokes: 'straight',
      gears: { front: [{ x: C, y: 96, N: 56, p: 0 }, { parent: 0, ang: 90, N: 42 }, { parent: 1, ang: 90, N: 56 }, { parent: 2, ang: 90, N: 28 }], fm: 2,
        rear: [{ x: 120, y: 250, N: 42, p: 0.1 }, { parent: 0, ang: -80, N: 35 }], rm: 2.85, bal: { x: 352, y: 292, r: 34 } },
      names: ['Barrel', 'Centre wheel', 'Third wheel', 'Escape wheel', 'Rear wheel A', 'Rear wheel B'],
      parts: [{ k: 'front' }, { k: 'poly', pts: bar(158, 44, 158, 408, 16) }, { k: 'poly', pts: bar(308, 44, 308, 408, 16) },
        { k: 'screw', x: 158, y: 60 }, { k: 'screw', x: 158, y: 392 }, { k: 'screw', x: 308, y: 60 }, { k: 'screw', x: 308, y: 392 },
        { k: 'balance' }, { k: 'poly', pts: bar(308, 292, 408, 252, 16) }, { k: 'baljewel' }, { k: 'screw', x: 394, y: 258 }],
    },
  };
  const TUNE = { orbD: 128, orbTip: 194, cageD: 107 }, TMEMO = {};
  function tune(o) { if (!o) return; TUNE.orbD = +o.orbD || 128; TUNE.orbTip = +o.orbTip || 194; TUNE.cageD = +o.cageD || 107; }
  const trainOf = (id) => {
    const T = TRAINS[id] || TRAINS.classic;
    if (T.orbital) {
      const D = TUNE.orbD, tip = Math.max(D + 50, TUNE.orbTip), k = 'o' + id + D + ',' + tip;
      if (D === T.orbital.D && tip === T.orbital.tip) return T;
      return TMEMO[k] || (TMEMO[k] = Object.assign({}, T, { orbital: Object.assign({}, T.orbital, { D, tip }), gears: Object.assign({}, T.gears, { bal: Object.assign({}, T.gears.bal, { y: C - D }) }) }));
    }
    if (T.cage) {
      const d = TUNE.cageD, k = 'c' + id + d;
      if (d === T.gears.bal.y - C) return T;
      return TMEMO[k] || (TMEMO[k] = Object.assign({}, T, { gears: Object.assign({}, T.gears, { bal: Object.assign({}, T.gears.bal, { y: C + d }) }) }));
    }
    return T;
  };
  function whiteBalance(c, K) {
    K = +K || 6500; if (Math.abs(K - 6500) < 1) return c;
    const _ = L(), a = _.kelvin(6500), b = _.kelvin(K); let g = a.map((v, i) => v / b[i]);
    const n = 0.299 * g[0] + 0.587 * g[1] + 0.114 * g[2]; g = g.map((v) => v / n);
    const x = c.getContext('2d'), d = x.getImageData(0, 0, c.width, c.height), p = d.data;
    for (let i = 0; i < p.length; i += 4) { p[i] *= g[0]; p[i + 1] *= g[1]; p[i + 2] *= g[2]; }
    x.putImageData(d, 0, 0); return c;
  }
  function drawPlate(ctx, style, G, lr, o) {
    const _ = L(), pa = rad(o && o.plateAngle != null ? +o.plateAngle : 0), ps = Math.max(0.3, o && o.plateScale != null ? +o.plateScale : 1);
    ctx.fillStyle = '#000'; ctx.fillRect(0, 0, S, S);
    ctx.save(); ctx.beginPath(); ctx.arc(C, C, DR, 0, TAU); ctx.clip();
    const hp = rotAbout(175, 160, lr || 0);
    let g = ctx.createRadialGradient(hp[0], hp[1], 10, C, C, DR);
    if (style === 'anthracite') { g.addColorStop(0, '#3b3e43'); g.addColorStop(1, '#0a0b0c'); }
    else if (style === 'stripes') { g.addColorStop(0, '#6f737a'); g.addColorStop(1, '#1b1d20'); }
    else if (style === 'sandblast') { g.addColorStop(0, '#45484d'); g.addColorStop(1, '#131416'); }
    else if (style === 'carbon' || style === 'forged') { g.addColorStop(0, '#1a1b1e'); g.addColorStop(1, '#060607'); }
    else { g.addColorStop(0, '#3b2816'); g.addColorStop(1, '#0a0604'); }
    ctx.fillStyle = g; ctx.fillRect(0, 0, S, S);
    if (style === 'anthracite') {
      ctx.lineWidth = 0.6;
      for (let r = 3; r < DR; r += 2) { ctx.strokeStyle = r % 4 ? 'rgba(255,255,255,.025)' : 'rgba(0,0,0,.08)'; ctx.beginPath(); ctx.arc(C, C, r, 0, TAU); ctx.stroke(); }
      const R = _.rng(17);
      for (let i = 0; i < 260; i++) { const a = R() * TAU, d = Math.sqrt(R()) * DR, x = C + Math.cos(a) * d, y = C + Math.sin(a) * d; ctx.strokeStyle = 'rgba(255,255,255,.035)'; ctx.beginPath(); ctx.arc(x, y, 6, 0, TAU); ctx.stroke(); }
    } else if (style === 'sandblast') {
      const R = _.rng(23);
      for (let i = 0; i < 26000; i++) { const a = R() * TAU, d = Math.sqrt(R()) * DR; ctx.fillStyle = R() < 0.5 ? 'rgba(0,0,0,' + R() * 0.35 + ')' : 'rgba(255,255,255,' + R() * 0.09 + ')'; ctx.fillRect(C + Math.cos(a) * d, C + Math.sin(a) * d, 1, 1); }
      ctx.strokeStyle = 'rgba(255,255,255,.05)'; ctx.lineWidth = 1;
      for (let r = 40; r < DR; r += 60) { ctx.beginPath(); ctx.arc(C, C, r, 0, TAU); ctx.stroke(); }
    } else if (style === 'carbon' || style === 'forged') {
      carbon(ctx, C, C, DR, style, 5, 1, null, { ang: o && o.plateAngle, sc: ps });
    } else if (style === 'stripes') {
      const sw = 16 * ps, ext = DR + sw * 2;
      ctx.save(); ctx.translate(C, C); ctx.rotate(pa);
      for (let x = -ext; x < ext; x += sw) {
        const sg = ctx.createLinearGradient(x, 0, x + sw, 0);
        sg.addColorStop(0, 'rgba(0,0,0,.28)'); sg.addColorStop(0.45, 'rgba(255,255,255,.12)'); sg.addColorStop(0.55, 'rgba(255,255,255,.12)'); sg.addColorStop(1, 'rgba(0,0,0,.28)');
        ctx.fillStyle = sg; ctx.fillRect(x, -ext, sw + 0.3, ext * 2);
      }
      ctx.restore();
    } else {
      ctx.lineWidth = 1;
      for (let r = 8; r < DR; r += 5) { ctx.strokeStyle = 'rgba(255,210,150,' + (r % 10 ? 0.025 : 0.045) + ')'; ctx.beginPath(); ctx.arc(C, C, r, 0, TAU); ctx.stroke(); }
      const R = _.rng(7);
      for (let i = 0; i < 60; i++) { const a = R() * TAU, d = Math.sqrt(R()) * DR, x = C + Math.cos(a) * d, y = C + Math.sin(a) * d; ctx.strokeStyle = 'rgba(255,200,140,.035)'; ctx.beginPath(); ctx.arc(x, y, 9, 0, TAU); ctx.stroke(); }
    }
    const wl = cv(S), wx = wl.getContext('2d'); wx.globalCompositeOperation = 'lighter';
    _.sideWash(wx, 0.62, 230, '255,135,40', 0.75 * G, DR);
    _.sideWash(wx, 0.3, 120, '255,200,120', 0.4 * G, DR);
    ctx.globalCompositeOperation = 'lighter'; ctx.drawImage(spun(wl, lr || 0), 0, 0);
    ctx.restore();
  }
  // carriage staff jewel. Everything is centred on (x, y) (no offset highlight) so the red ink is point-symmetric;
  // the setting is opaque steel, so anti-aliased edges never pass as red and the bank stays registered frame to frame.
  function staffJewel(ctx, x, y, k) {
    k = k || 1;
    ctx.save(); ctx.shadowColor = 'rgba(0,0,0,.55)'; ctx.shadowBlur = 2.5 * k; ctx.shadowOffsetY = 1.2 * k;
    let g = ctx.createRadialGradient(x, y, 0, x, y, 7.5 * k); g.addColorStop(0, '#e9edf1'); g.addColorStop(0.7, '#9aa1a9'); g.addColorStop(1, '#3c4147');
    ctx.fillStyle = g; ctx.beginPath(); ctx.arc(x, y, 7.5 * k, 0, TAU); ctx.fill(); ctx.restore();
    ctx.fillStyle = '#26292d'; ctx.beginPath(); ctx.arc(x, y, 5.6 * k, 0, TAU); ctx.fill();
    g = ctx.createRadialGradient(x, y, 0, x, y, 4.6 * k); g.addColorStop(0, '#ff9c9c'); g.addColorStop(0.45, '#e0303c'); g.addColorStop(0.8, '#a8121c'); g.addColorStop(1, '#3a0307');
    ctx.fillStyle = g; ctx.beginPath(); ctx.arc(x, y, 4.6 * k, 0, TAU); ctx.fill();
  }
  // tourbillon carriage: balance inside a three-arm cage, drawn on its own layer so it can be turned live
  function drawCarriage(ctx, R, pal, gold, t) {
    const _ = L(), x = _.BAL.x, y = _.BAL.y, rot = _.BAL.rot;
    _.drawBalance(ctx, t, pal, gold);
    const frame = (dx, dy) => {
      const X = x + dx, Y = y + dy; ctx.beginPath();
      ctx.moveTo(X + R, Y); ctx.arc(X, Y, R, 0, TAU); ctx.moveTo(X + R - 5, Y); ctx.arc(X, Y, R - 5, 0, TAU, true);
      for (let i = 0; i < 3; i++) {
        const b = rot + (i / 3) * TAU + Math.PI / 6;
        let q = P(X, Y, 9, b - 0.55); ctx.moveTo(q[0], q[1]);
        q = P(X, Y, R - 3, b - 0.1); ctx.lineTo(q[0], q[1]); q = P(X, Y, R - 3, b + 0.1); ctx.lineTo(q[0], q[1]);
        q = P(X, Y, 9, b + 0.55); ctx.lineTo(q[0], q[1]); ctx.closePath();
      }
      ctx.moveTo(X + 11, Y); ctx.arc(X, Y, 11, 0, TAU);
    };
    _.solid(ctx, frame, x, y, R, _.STEEL, { shadows: _.sideShadows(x, 0.7), depth: 3, sheen: 1 });
    for (let i = 0; i < 3; i++) { const q = P(x, y, R - 2.5, rot + (i / 3) * TAU + Math.PI / 6); _.screw(ctx, q[0], q[1], 2.8, rot + i); }
    staffJewel(ctx, x, y);
  }
  // kite carriage: slim kite frame with toothed rails and a recessed inlay groove, spine to a ringed balance
  // window, gothic struts in the windows; the balance opening is cut last so nothing ever covers the escapement
  function drawKite(ctx, O, pal, gold, t, bp) {
    const _ = L(), bx = _.BAL.x, by = _.BAL.y, ang = Math.atan2(by - C, bx - C), fx = Math.cos(ang), fy = Math.sin(ang), sx = -fy, sy = fx;
    const W = (u, v, dx, dy) => [C + fx * u + sx * v + (dx || 0), C + fy * u + sy * v + (dy || 0)];
    const D = O.D, tail = O.tail || 58, tip = Math.min(O.tip, 210), room = tip - D;
    // fit the frame into the room the user left between escapement and tip instead of pushing the tip out
    const kf = Math.max(0.55, Math.min(1, room / (_.BAL.r + 4 + 9 + 30)));
    const ri = _.BAL.r + 4, ringO = ri + 9 * kf, rw = 10 * kf, Wd = ringO + rw + 5 * kf, back = -tail - 16;
    const outer = [[back, 0], [D, -Wd], [tip, 0], [D, Wd]], cen = [(back + D * 2 + tip) / 4, 0];
    // inward offset of a convex polygon in (u,v)
    const inset = (P, d) => {
      const lines = P.map((p, i) => { const q = P[(i + 1) % P.length], ex = q[0] - p[0], ey = q[1] - p[1], n = Math.hypot(ex, ey) || 1; let nx = -ey / n, ny = ex / n;
        const mx = (p[0] + q[0]) / 2, my = (p[1] + q[1]) / 2; if ((cen[0] - mx) * nx + (cen[1] - my) * ny < 0) { nx = -nx; ny = -ny; } return [p[0] + nx * d, p[1] + ny * d, ex, ey]; });
      return lines.map((l, i) => { const m = lines[(i + P.length - 1) % P.length], den = m[2] * l[3] - m[3] * l[2]; if (Math.abs(den) < 1e-6) return [l[0], l[1]]; const tt = ((l[0] - m[0]) * l[3] - (l[1] - m[1]) * l[2]) / den; return [m[0] + m[2] * tt, m[1] + m[3] * tt]; });
    };
    // gear-cut perimeter: small square notches walked along each rail
    const notched = (P) => {
      const out = [];
      P.forEach((p, i) => {
        const q = P[(i + 1) % P.length], ex = q[0] - p[0], ey = q[1] - p[1], Lr = Math.hypot(ex, ey), ux = ex / Lr, uy = ey / Lr;
        let nx = -uy, ny = ux; const mx = (p[0] + q[0]) / 2, my = (p[1] + q[1]) / 2; if ((cen[0] - mx) * nx + (cen[1] - my) * ny < 0) { nx = -nx; ny = -ny; }
        out.push(p); const step = 7.5, m0 = 12, n = Math.floor((Lr - 2 * m0) / step), st = (Lr - n * step) / 2, dp = 2.8;
        for (let k = 0; k < n; k++) { const c0 = st + k * step + step / 2, pt = (o, d) => [p[0] + ux * (c0 + o) + nx * d, p[1] + uy * (c0 + o) + ny * d]; out.push(pt(-2.1, 0), pt(-1.5, dp), pt(1.5, dp), pt(2.1, 0)); }
      });
      return out;
    };
    const scr = (P) => P.map(([u, v]) => W(u, v));
    const area = (P) => P.reduce((a, p, i) => { const q = P[(i + 1) % P.length]; return a + p[0] * q[1] - q[0] * p[1]; }, 0);
    const orient = (P, pos) => ((area(P) > 0) === pos ? P : P.slice().reverse());
    const OUT = orient(scr(notched(outer)), true), INN = orient(scr(inset(outer, rw)), false);
    const rectUV = (u0, u1, hw) => orient(scr([[u0, -hw], [u1, -hw], [u1, hw], [u0, hw]]), true);
    const SP1 = rectUV(12, D - ri - 2, 4), SP2 = rectUV(D + ri + 2, tip - 14, 3);
    const um = Math.max(30, (D - ri) * 0.5), hwAt = (u) => (u <= D ? Wd * (u - back) / (D - back) : Wd * (tip - u) / (tip - D)) - rw * 1.1;
    const strut = (sd) => { const u1 = um + 22, v1 = sd * Math.max(4, hwAt(u1) + 2), p0 = [um, 0], ex = u1 - p0[0], ey = v1, n = Math.hypot(ex, ey), nx = (-ey / n) * 2.4, ny = (ex / n) * 2.4;
      return orient(scr([[p0[0] + nx, nx * 0 + ny], [u1 + nx, v1 + ny], [u1 - nx, v1 - ny], [p0[0] - nx, -ny]]), true); };
    const ST = hwAt(um + 22) > 6 ? [strut(-1), strut(1)] : [];
    const roseR = 8.5;
    const path = (dx, dy) => {
      const poly = (P) => { ctx.moveTo(P[0][0] + dx, P[0][1] + dy); for (let i = 1; i < P.length; i++) ctx.lineTo(P[i][0] + dx, P[i][1] + dy); ctx.closePath(); };
      const circ = (u, r, rev) => { const q = W(u, 0, dx, dy); if (rev) { ctx.moveTo(q[0] + r, q[1]); ctx.arc(q[0], q[1], r, 0, TAU, true); } else { ctx.moveTo(q[0] + r, q[1]); ctx.arc(q[0], q[1], r, 0, TAU); } };
      ctx.beginPath();
      poly(OUT); poly(INN); poly(SP1); poly(SP2); ST.forEach(poly);
      circ(D, ringO); circ(D, ri, true);
      circ(0, 15); circ(-tail, 14);
      if (ST.length) { circ(um, roseR); circ(um, roseR - 3, true); }
    };
    _.drawBalance(ctx, t, pal, gold);
    const K = cv(S), kx = K.getContext('2d');
    const real = ctx; ctx = kx;
    _.solid(kx, path, C, C, tip, bp, { shadows: _.sideShadows(C, 1), depth: 3.2, sheen: 1 });
    // recessed groove: dark channel, lit far wall, shaded near wall, blue enamel line seated in it
    const [ax, ay] = away(1), IC = { blued: ['#0b1730', '#2c56a8', '#9cc0ff'], gold: ['#3a2508', '#b88a3a', '#ffe7a8'], black: ['#000', '#1b1c1f', '#6b6f75'] }[O.inlay || 'blued'] || ['#0b1730', '#2c56a8', '#9cc0ff'];
    const GR = scr(inset(outer, rw / 2));
    const grooveShape = (x, dx, dy) => { x.beginPath(); GR.forEach((p, i) => (i ? x.lineTo(p[0] + dx, p[1] + dy) : x.moveTo(p[0] + dx, p[1] + dy))); x.closePath();
      const a0 = W(14, 0), a1 = W(D - ringO + 1, 0); x.moveTo(a0[0] + dx, a0[1] + dy); x.lineTo(a1[0] + dx, a1[1] + dy);
      const rc = W(D, 0); x.moveTo(rc[0] + ri + 4 + dx, rc[1] + dy); x.arc(rc[0] + dx, rc[1] + dy, ri + 4, 0, TAU); };
    kx.save(); kx.lineJoin = 'round'; kx.lineCap = 'round';
    kx.lineWidth = 3.4; kx.strokeStyle = 'rgba(0,0,0,.62)'; grooveShape(kx, 0, 0); kx.stroke();
    kx.lineWidth = 0.9; kx.strokeStyle = 'rgba(255,252,245,.5)'; grooveShape(kx, ax * 1.7, ay * 1.7); kx.stroke();
    kx.lineWidth = 0.9; kx.strokeStyle = 'rgba(0,0,0,.55)'; grooveShape(kx, -ax * 1.6, -ay * 1.6); kx.stroke();
    kx.lineWidth = 1.3; kx.strokeStyle = IC[1]; grooveShape(kx, ax * 0.35, ay * 0.35); kx.stroke();
    kx.lineWidth = 0.45; kx.strokeStyle = IC[2]; kx.globalAlpha = 0.75; grooveShape(kx, -ax * 0.3, -ay * 0.3); kx.stroke();
    kx.restore();
    // counterweight lightening holes, chamfered then cut
    const holes = [0, 1, 2].map((i) => { const a2 = ang + Math.PI + (i - 1) * 1.15; return [W(-tail, 0)[0] + Math.cos(a2) * 7.5, W(-tail, 0)[1] + Math.sin(a2) * 7.5, 3.1]; });
    holes.forEach(([x, y, r]) => { kx.lineWidth = 1.4; kx.strokeStyle = 'rgba(255,250,240,.35)'; kx.beginPath(); kx.arc(x + ax * 0.8, y + ay * 0.8, r + 0.6, 0, TAU); kx.stroke(); });
    kx.save(); kx.globalCompositeOperation = 'destination-out'; holes.forEach(([x, y, r]) => { kx.beginPath(); kx.arc(x, y, r, 0, TAU); kx.fill(); }); kx.restore();
    [-1, 1].forEach((sd) => { const p = W(-tail + 2, sd * 9.5); _.screw(kx, p[0], p[1], 2.4, ang + sd); });
    if (ST.length) { const q = W(um, 0); _.screw(kx, q[0], q[1], 2.2, ang + 0.4); }
    // the escapement window is always open
    const bq = W(D, 0);
    kx.save(); kx.globalCompositeOperation = 'destination-out'; kx.beginPath(); kx.arc(bq[0], bq[1], ri, 0, TAU); kx.fill(); kx.restore();
    ctx = real;
    ctx.drawImage(K, 0, 0);
    ctx.save(); ctx.beginPath(); ctx.arc(bq[0], bq[1], ri, 0, TAU); ctx.clip();
    ctx.lineWidth = 3; ctx.strokeStyle = 'rgba(0,0,0,.45)'; ctx.filter = 'blur(1.5px)'; ctx.beginPath(); ctx.arc(bq[0] - ax * 1.5, bq[1] - ay * 1.5, ri + 1, 0, TAU); ctx.stroke(); ctx.restore();
    _.jewel(ctx, bq[0], bq[1], gold);
    staffJewel(ctx, C, C);
  }
  // orbital carriage: hub at the dial centre, balance cage part-way out, arrow tip beyond; turned live once a minute
  function drawOrbital(ctx, O, pal, gold, t, bp) {
    const _ = L(), bx = _.BAL.x, by = _.BAL.y, ang = Math.atan2(by - C, bx - C), fx = Math.cos(ang), fy = Math.sin(ang), sx = -fy, sy = fx;
    const W = (u, v, dx, dy) => [C + fx * u + sx * v + dx, C + fy * u + sy * v + dy];
    const D = O.D, ri = _.BAL.r + 4, ro = _.BAL.r + 10;
    _.drawBalance(ctx, t, pal, gold);
    const path = (dx, dy) => {
      const rect = (u0, u1, hw) => { const p = [W(u0, -hw, dx, dy), W(u1, -hw, dx, dy), W(u1, hw, dx, dy), W(u0, hw, dx, dy)]; ctx.moveTo(p[0][0], p[0][1]); p.slice(1).forEach((q) => ctx.lineTo(q[0], q[1])); ctx.closePath(); };
      const circ = (u, r, rev) => { const q = W(u, 0, dx, dy); ctx.moveTo(q[0] + r, q[1]); ctx.arc(q[0], q[1], r, 0, TAU, !!rev); };
      ctx.beginPath();
      circ(0, 17); rect(0, D - ri + 1, 6.5);
      circ(D, ro); circ(D, ri, true); rect(D - 2.5, D + 2.5, ro - 1);
      rect(D + ri - 1, O.tip - 20, 4);
      const a = W(O.tip - 22, -14, dx, dy), b = W(O.tip, 0, dx, dy), c = W(O.tip - 22, 14, dx, dy), m = W(O.tip - 16, 0, dx, dy);
      ctx.moveTo(a[0], a[1]); ctx.lineTo(b[0], b[1]); ctx.lineTo(c[0], c[1]); ctx.lineTo(m[0], m[1]); ctx.closePath();
      rect(-O.tail, 0, 6.5); circ(-O.tail, 21); circ(-O.tail, 9, true);
    };
    _.solid(ctx, path, C, C, O.tip, bp, { shadows: _.sideShadows(C, 1.2), depth: 4, sheen: 1 });
    const q = W(D, 0, 0, 0), h = W(-O.tail, 0, 0, 0);
    _.jewel(ctx, q[0], q[1], gold); staffJewel(ctx, C, C);
    [-15, 15].forEach((v) => { const p = W(-O.tail, v, 0, 0); _.screw(ctx, p[0], p[1], 3.4, ang); });
    [-1, 1].forEach((sd) => { const p = W(D, sd * (ri + 3), 0, 0); _.screw(ctx, p[0], p[1], 3, ang + 0.6); });
    ctx.fillStyle = 'rgba(0,0,0,.6)'; ctx.beginPath(); ctx.arc(h[0], h[1], 2, 0, TAU); ctx.fill();
  }
  function resolve(o) {
    const T = trainOf(o.train);
    return Object.assign({}, o, { plate: !o.plate || o.plate === 'train' ? T.plate : o.plate, spokes: !o.spokes || o.spokes === 'train' ? T.spokes || 'arched' : o.spokes, finish: !o.finish || o.finish === 'train' ? T.finish || 'brass' : o.finish });
  }
  // Inner light: rendered in "light space" (sources on the horizontal axis), then turned to the chosen axis.
  // The train itself is rotated in data, so lighting stays world-fixed.
  function movement(o) {
    tune(o);
    const _ = L(), T = trainOf(o.train), lr = rad((o.innerAngle == null ? 90 : +o.innerAngle) - 90), tr = rad(+o.trainAngle || 0);
    _.setTrain(T.gears);
    o = _.configure(Object.assign(resolve(o), { trainRot: tr - lr }));
    const gold = o.finish === 'brass' ? _.FIN.copper : _.FIN.brass, pal = _.FIN[o.finish] || _.FIN.brass, G = +o.glow || 1, bp = T.bridgePal === 'black' ? BLACK : T.bridgePal === 'rhodium' ? RHOD : _.STEEL;
    const plate = cv(S); drawPlate(plate.getContext('2d'), o.plate && o.plate !== 'train' ? o.plate : T.plate, G, lr, o);
    /* Backdrop slot one, the plate: under the geartrain, so the art reads through an
     * openwork dial and the apertures. Free because the plate is one of the two static
     * base frames -- the art is a one-time store into the bank, not a per-tick blend
     * (docs/fps-budget-findings-2026-10-10.md: the live dial pass runs 447 ns/blend).
     * Slot two, the dial face, is in buildDial and costs nothing for the same kind of
     * reason: that canvas is already blended every tick with or without the art. Under
     * "no case" the backdrop is NOT baked here at all — the face carries it as F.bgL and
     * compose() blends it untransformed, so the opening can move without resampling it. */
    if (o.caseStyle !== 'none') artInDial(plate.getContext('2d'), o.bgArt, { x: o.bgX, y: o.bgY, size: o.bgSize, fit: o.bgFit, zoom: o.bgZoom, opacity: o.bgOpacity, clipR: artClipR(o) });
    const hz = cv(S); hz.getContext('2d').globalCompositeOperation = 'lighter';
    _.sideWash(hz.getContext('2d'), 0.5, 200, '255,150,60', 0.55 * G, DR);
    const haze = spun(hz, lr), rear = cv(S);
    const tp = (pts) => pts.map(([x, y]) => rotAbout(x, y, o.trainRot));
    const bw = rotAbout(_.BAL.x, _.BAL.y, lr);
    const cageAt = T.orbital ? [C, C] : T.cage ? bw : null, cageOff = T.orbital ? Math.atan2(bw[0] - C, -(bw[1] - C)) : 0;
    const drawCageArt = (cx, t) => { if (T.orbital) (T.orbital.shape === 'kite' ? drawKite : drawOrbital)(cx, T.orbital, pal, gold, t, bp); else drawCarriage(cx, T.cage, pal, gold, t); };
    // export path: carriage drawn straight into final kit space (inset scale k about the canvas centre, then a sub-texel shift) so it is never resampled
    function cageFrame(f, X) {
      _.setTrain(T.gears); _.configure(o);
      const c = cv(S), cx = c.getContext('2d');
      cx.translate(X.dx || 0, X.dy || 0); cx.translate(C, C); cx.scale(X.k || 1, X.k || 1); cx.rotate(lr); cx.translate(-C, -C);
      cx.save(); cx.beginPath(); cx.arc(C, C, DR, 0, TAU); cx.clip(); drawCageArt(cx, f / o.frames); cx.restore();
      return c;
    }
    function render(f) {
      _.setTrain(T.gears); _.configure(o);
      const t = f / o.frames, out = cv(S);
      let ctx = out.getContext('2d'), cage = null;
      ctx.save(); ctx.beginPath(); ctx.arc(C, C, DR, 0, TAU); ctx.clip();
      if (_.REAR.length) {
        const rx = rear.getContext('2d'); rx.globalCompositeOperation = 'source-over'; rx.clearRect(0, 0, S, S);
        const rf = o.rearFinish && o.rearFinish !== 'train' ? o.rearFinish : T.rear || 'brass', rpal = rf === 'match' ? pal : rf === 'brass' ? _.BACK : _.FIN[rf] || _.BACK;
        _.REAR.forEach((gr) => _.drawGear(rx, gr, gr.p + gr.v * t, rpal, true, gold));
        rx.globalCompositeOperation = 'source-atop'; rx.fillStyle = rf === 'brass' ? 'rgba(14,7,2,.14)' : 'rgba(0,0,0,.3)'; rx.fillRect(0, 0, S, S);
        ctx.filter = 'blur(1.1px)'; ctx.drawImage(rear, 0, 0); ctx.filter = 'none';
      }
      T.parts.forEach((p) => {
        if (p.k === 'bridge') _.bridge(ctx, tp(p.pts), p.w, bp);
        else if (p.k === 'poly') {
          const pts = tp(p.pts), cx = pts.reduce((a, q) => a + q[0], 0) / pts.length, cy = pts.reduce((a, q) => a + q[1], 0) / pts.length;
          const rr = Math.max(...pts.map((q) => Math.hypot(q[0] - cx, q[1] - cy)));
          const pf = (dx, dy) => { ctx.beginPath(); pts.forEach(([a, b], i) => (i ? ctx.lineTo(a + dx, b + dy) : ctx.moveTo(a + dx, b + dy))); ctx.closePath(); };
          _.solid(ctx, pf, cx, cy, rr, bp, { shadows: _.sideShadows(cx, 1), depth: 4, sheen: 1 });
        } else if (p.k === 'screw') { const q = rotAbout(p.x, p.y, o.trainRot); _.screw(ctx, q[0], q[1], p.r || 5.5, (p.sl || 0) + o.trainRot); }
        else if (p.k === 'front') _.FRONT.forEach((gr) => _.drawGear(ctx, gr, gr.p + gr.v * t, gr.pal, false, gold));
        else if (p.k === 'post') _.post(ctx, _.FRONT[p.i], gold);
        else if (p.k === 'balance') _.drawBalance(ctx, t, pal, gold);
        else if (p.k === 'baljewel') _.jewel(ctx, _.BAL.x, _.BAL.y, gold);
        else if (p.k === 'cage') { cage = cv(S); const cx = cage.getContext('2d'); cx.save(); cx.beginPath(); cx.arc(C, C, DR, 0, TAU); cx.clip(); drawCageArt(cx, t); cx.restore(); }
      });
      ctx.restore();
      const gt = spun(out, lr);
      if (cage) gt.cage = spun(cage, lr);
      return { gt };
    }
    return { frames: o.frames, plate, haze, render, cageFrame: T.cage || T.orbital ? cageFrame : null, cageAt, cageOff, orbital: !!T.orbital, cageR: T.cage || 0, train: o.train };
  }

  function features(train) {
    const _ = L(), T = trainOf(train); _.setTrain(T.gears);
    return [..._.FRONT0, ..._.REAR0].map((g, i) => ({ id: 'g' + i, name: T.names[i] || 'Wheel ' + (i + 1), x: g.x, y: g.y }))
      .concat([{ id: 'balance', name: T.orbital ? 'Orbital carriage' : T.cage ? 'Tourbillon' : 'Balance wheel', x: _.BAL0.x, y: _.BAL0.y }]);
  }

  // train rotation (degrees, clockwise) that swings a feature into an opening, or to an hour for a centred opening
  function aimAngle(featId, op, hour, train) {
    const f = features(train).find((q) => q.id === featId); if (!f || !op) return 0;
    const bh = op.centred ? ((hour || 12) / 12) * 360 - 90 : (Math.atan2(op.y - C, op.x - C) * 180) / Math.PI;
    const bf = (Math.atan2(f.y - C, f.x - C) * 180) / Math.PI;
    return Math.round((((bh - bf) % 360) + 360) % 360);
  }
  // the first aimed opening sets the train; otherwise an escapement opening's angle does; otherwise the manual angle
  function trainAngle(design, manual, train) {
    L().setTrain(trainOf(train).gears);
    const list = design.comps || [];
    const aim = list.find((c) => c.type === 'opening' && c.show && c.show !== 'free' && c.show !== 'balance');
    if (aim) { const g = geom(design, 0)[aim.id]; return { deg: aimAngle(aim.show, { x: g.x, y: g.y, centred: Math.hypot(g.x - C, g.y - C) < 4 }, aim.hour, train), by: aim.id }; }
    const esc = list.find((c) => c.type === 'opening' && c.show === 'balance');
    if (esc) { const _ = L(), nat = (Math.atan2(_.BAL0.x - C, -(_.BAL0.y - C)) * 180) / Math.PI; return { deg: Math.round((((+esc.angle - nat) % 360) + 360) % 360), by: esc.id }; }
    return { deg: Math.round((((+manual || 0) % 360) + 360) % 360), by: null };
  }

  function face(o) {
    tune(o);
    const _ = L(); _.setTrain(trainOf(o.train).gears); o = _.configure(resolve(o));
    const fontKey = o.design.font || 'auto'; setFace(fontKey);
    const design = o.design, en = colorEn(null, o.design.color), pal = _.FIN[o.finish] || _.FIN.brass, G = +o.glow || 1;
    setLights(o);
    const lay = layout(design, rad(+o.trainAngle || 0));
    const style = o.hands || 'breguet', hf = o.handFinish || 'blued', SH = shadowCfg(o, LIGHT);
    const F = {
      design, lay, en, pal, style, handFinish: hf,
      subLayers: buildSubdials(design, lay, en, pal, G, SH),
      hands: buildHands(lay, design, style, hf, o.tick), inset: faceScale(o), dynK: dynScale(o), caseOpen: caseOpenR(o), handOrder: handOrder(lay, design), handSlots: handSlots(lay, design), light: Object.assign({}, LIGHT),
    };
    // Under "no case" the opening pulls the DRAWN dial in through buildDial's lens; under a
    // drawn case everything eases in through compose's inset instead, unsplit.
    const dialK = o.caseStyle === 'none' ? F.dynK : 1;
    const bd = buildDial(design, lay, en, pal, G, false, artClipR(o), dialK); F.dial = bd.canvas; F.dialEn = bd.en;
    // dialSealed: the dial with the date/moon apertures left uncut, so the opaque disc
    // the device composites over them every day seals the geartrain beneath. Shipped as
    // the theme dial and used as the composer occluder -- both must match the bake's
    // opaque static1 cull. Built only when a dial-hosted disc exists; else fall back to
    // the holed dial (no apertures to seal).
    F.dialSealed = lay.dialHoles.some((h) => h.seal) ? buildDial(design, lay, en, pal, G, true, artClipR(o), dialK).canvas : bd.canvas;
    /* The plate-slot backdrop under "no case" is NOT baked into the plate: the base frames
     * are banked once per movement and the opening can move while they are cached, and the
     * one promise of the option is that the author's own image is never resampled. So the
     * face carries it as its own layer and compose() blends it untransformed. */
    F.bgL = null;
    if (o.caseStyle === 'none') { const g = cv(S); if (artInDial(g.getContext('2d'), o.bgArt, { x: o.bgX, y: o.bgY, size: o.bgSize, fit: o.bgFit, zoom: o.bgZoom, opacity: o.bgOpacity, clipR: artClipR(o) })) F.bgL = g; }
    const pf = lay.flares.filter((f) => f.host === 'plate' && f.icon !== 'datetext');
    const DF = lay.flares.filter((f) => f.icon === 'datetext' && f.host !== 'plate');
    // a transparent subdial contributes geometry only: its centre and radius still place
    // the flare, but the ink and its colour are the dial's, because the dial is the plane
    // the art actually prints on
    const hostEn = (id, alt) => { const l = id ? F.subLayers.find((q) => q.id === id) : null; return !l || l.transparent ? F.dialEn : (l.en || alt || F.dialEn); };
    const dfHost = (f) => (f.host === 'dial' ? { x: C, y: C, r: DR, en: F.dialEn } : (() => { const s = lay.subs.find((q) => q.id === f.host); return s ? { x: s.x, y: s.y, r: s.r, en: hostEn(f.host, en) } : { x: C, y: C, r: DR, en: F.dialEn }; })());
    const paintDF = (x, f, date, noClip) => { const h = dfHost(f); setFace(design.font || 'auto'); x.save(); x.beginPath(); if (noClip) x.rect(0, 0, S, S); else x.arc(h.x, h.y, h.r, 0, TAU); x.clip(); drawFlares(x, [f], h.en, pal, date); x.restore(); setFace(fontKey); };
    let dk = null, dv = null;
    F.dateFlares = (date) => {
      if (!DF.length) return null;
      const key = date.toDateString(); if (key === dk) return dv;
      const out = { dial: cv(S), subs: {} };
      DF.forEach((f) => { const t = f.host === 'dial' ? out.dial : out.subs[f.host] || (out.subs[f.host] = cv(S)); paintDF(t.getContext('2d'), f, date); });
      dk = key; dv = out; return out;
    };
    F.dateFlareSprites = () => {
      const res = [], y0 = new Date().getFullYear();
      DF.forEach((f) => {
        const seen = new Map(), idx = {};
        for (let t = new Date(y0, 0, 1); t.getFullYear() <= y0 + 1; t.setDate(t.getDate() + 1)) {
          const sv = fmtDate(f.fmt == null ? 'ddd DD MMM' : f.fmt, t, f.fcase);
          if (!seen.has(sv)) seen.set(sv, [String(seen.size + 1).padStart(4, '0'), new Date(t.getTime())]);
          idx[t.getFullYear() + '-' + String(t.getMonth() + 1).padStart(2, '0') + '-' + String(t.getDate()).padStart(2, '0')] = seen.get(sv)[0];
        }
        const texts = Object.fromEntries([...seen].map(([sv, [n]]) => [n, sv]));
        const fulls = [...seen.values()].map(([n, d]) => { const c = cv(S); paintDF(c.getContext('2d'), f, d); return [n, c]; });
        let b = [S, S, -1, -1];
        fulls.forEach(([, c]) => { const d = c.getContext('2d').getImageData(0, 0, S, S).data; for (let i = 3; i < d.length; i += 4) if (d[i]) { const p = (i - 3) / 4, x = p % S, y = (p / S) | 0; if (x < b[0]) b[0] = x; if (y < b[1]) b[1] = y; if (x > b[2]) b[2] = x; if (y > b[3]) b[3] = y; } });
        if (b[2] < 0) return;
        const bw = b[2] - b[0] + 1, bh = b[3] - b[1] + 1;
        fulls.forEach(([n, c]) => { const sp = cv(bw, bh); sp.getContext('2d').drawImage(c, -b[0], -b[1]); res.push({ id: f.id, type: 'flare-date', host: f.host, value: n, x: b[0], y: b[1], w: bw, h: bh, canvas: sp, format: f.fmt == null ? 'ddd DD MMM' : f.fmt, fcase: f.fcase || 'upper', index: idx, texts }); });
      });
      return res;
    };
    if (pf.length) { F.plateFlares = cv(S); const px = F.plateFlares.getContext('2d'); px.save(); px.beginPath(); px.arc(C, C, DR, 0, TAU); px.clip(); drawFlares(px, pf, { ink: '#e6dfcf', dark: 1 }, pal); px.restore(); }
    const OK = LIGHT.outerK;
    _.setTemp(o.outerTemp || o.temp || 2200, o.outerHue, o.outerSat);
    F.glass = buildGlass(OK, o.glassStyle); F.outer = buildOuter(OK, o.caseStyle, caseMetal(o), caseOpenR(o));
    _.setTemp(o.temp || 2200, o.hue, o.hueSat);
    const hostOf = (c) => (c.host === 'dial' ? { x: C, y: C, r: DR } : lay.subs.find((s) => s.id === c.host));
    const enOf = (c) => (c.host === 'dial' ? F.dialEn : (F.subLayers.find((q) => q.id === c.host) || {}).en || en);
    const paint = (ctx, c, date, i) => { setFace(fontKey); const h = hostOf(c); ctx.save(); ctx.beginPath(); ctx.arc(h.x, h.y, h.r, 0, TAU); ctx.clip(); drawComp(ctx, c, date, enOf(c), pal, i + 1); ctx.restore(); };
    let ck = null, cvals = null;
    F.comps = (date) => {
      const key = date.toDateString();
      if (key === ck) return cvals;
      const a = cv(S), subs = {};
      lay.comps.forEach((c, i) => { const t = c.host === 'dial' ? a : subs[c.host] || (subs[c.host] = cv(S)); paint(t.getContext('2d'), c, date, i); });
      ck = key; cvals = { dial: a, subs };
      return cvals;
    };
    F.shadow = SH;
    F.drawHands = (ctx, date) => drawHands(ctx, F.hands, date, F.light, SH);
    F.handShadow = (hd) => ({ dx: SH.dir[0] * 1.6 * hd.h * SH.dist, dy: SH.dir[1] * 1.6 * hd.h * SH.dist, blur: 1.4 * hd.h * SH.blur, alpha: Math.min(1, 0.5 * SH.kk * SH.op) });
    F.hit = (x, y) => { for (let i = lay.hits.length - 1; i >= 0; i--) { const h = lay.hits[i]; if (Math.hypot(x - h.x, y - h.y) <= h.r) return h.id; } return null; };
    /* What sits under this pixel, top of the stack first -- the drawing order read
     * backwards: the hand sprites are the last thing before the glass, so a hand is
     * tested before the complication it turns across, then the dial disc. F.hit alone
     * answered only about complications, so clicking the face or a hand read as
     * "nothing" and deselected whatever the author had picked.
     * Returns plain { kind, id } targets -- never the record itself, whose canvas would
     * ride into React state and break anything that serialises a selection. */
    F.pickAll = (x, y, date) => {
      const out = [];
      const hs = F.hands || [];
      for (let i = hs.length - 1; i >= 0; i--) { const hd = hs[i]; if (handHit(hd, x, y, date)) out.push({ kind: 'hand', id: hd.id }); }
      for (let i = lay.hits.length - 1; i >= 0; i--) { const h = lay.hits[i]; if (Math.hypot(x - h.x, y - h.y) <= h.r) out.push({ kind: 'comp', id: h.id }); }
      // The dial target means "the art under the cursor": under "no case" the uploaded
      // image keeps its authored reach while the drawn face eases inward, so its pixels
      // stay clickable out to the panel edge, not just inside the shrunk dial circle.
      const r = Math.hypot(x - C, y - C);
      const artLive = o.caseStyle === 'none' && (assetReady(design.sfArt) || assetReady(o.bgArt));
      if (r <= DR || (artLive && r * (F.dynK || 1) <= PANEL_R)) out.push({ kind: 'dial', id: 'dial' });
      return out;
    };
    F.pick = (x, y, date) => F.pickAll(x, y, date)[0] || null;
    F.outline = (id) => lay.hits.find((h) => h.id === id) || null;
    // text kits for custom-format windows and date flares: end caps + a 1px tileable slice of the frame/plate,
    // and every glyph the format can produce, so firmware can assemble any string. Rendered unrotated at kit scale k.
    F.textKits = (k) => {
      k = k || 1;
      const kits = [], y0 = new Date().getFullYear(), Mk = (v) => C + (v - C) * k;
      const charset = (fmt, fcase) => { const set = new Set('0123456789'); for (let t = new Date(y0, 0, 1); t.getFullYear() <= y0 + 1; t.setDate(t.getDate() + 1)) for (const ch of fmtDate(fmt, t, fcase)) set.add(ch); return [...set].sort(); };
      const local = (src, cx, cy, rot, W, H) => { const c = cv(W, H), x = c.getContext('2d'); x.translate(W / 2, H / 2); x.scale(k, k); x.rotate(-(rot || 0)); x.translate(-cx, -cy); x.drawImage(src, 0, 0); return c; };
      const crop = (src, x, y, w, h) => { const c = cv(Math.max(1, w), Math.max(1, h)); c.getContext('2d').drawImage(src, -x, -y); return c; };
      const tileOf = (slice, w) => { const c = cv(Math.max(1, w), slice.height), x = c.getContext('2d'); for (let i = 0; i < w; i++) x.drawImage(slice, i, 0); return c; };
      const iso = (t) => t.getFullYear() + '-' + String(t.getMonth() + 1).padStart(2, '0') + '-' + String(t.getDate()).padStart(2, '0');
      lay.comps.forEach((c, i) => {
        // The built-in day-of-month window rides the same rail as the custom windows:
        // its 31 whole-value PNGs were one 40x32 disc re-inked 31 times (only ~4.5k of
        // the 40k pixels in the set ever differ), while ten digit tiles plus the plate
        // cover every date the field can hold. Weekday and month stay whole-value --
        // SUN..SAT needs 14 letters and JAN..DEC needs ~19, both more files than the
        // 7 / 12 whole-word sprites they replace.
        const isDate = c.type === 'date';
        if (!isDate && c.type !== 'custom') return;
        const h = c.hole, kk = c.k || 1, m = 4;
        // A window's shade depends on where a pixel sits in the WINDOW, never on the string: the
        // host layer (the dial, or the host subdial) carries the frame ring, its drop shadow and
        // the aperture's inner shade, and the engine composes disc+ink FIRST and that host layer
        // SECOND, so the ink sits UNDER the shade. Measured on the modern preset's date field the
        // shade runs α 186 in the middle of a strip to 207 at either edge — a moving digit cannot
        // carry that on its own tile. So it ships once per window as shade.png, full window rect,
        // and the assembler lays it on LAST. Three pieces follow from that ordering:
        //   caps / slice  = the engine crop, shade included (nothing will be drawn over them)
        //   glyph band    = the same crop with the host subtracted INSIDE the hole path, so the
        //                   ink on a tile sits under exactly one shade instead of two
        //   shade.png     = the host clipped to the hole path — the ring band straddles that edge
        //                   and travels with the shade, which is why it is clipped to the path and
        //                   not to the axis-aligned box.
        const hp = holePath(h);
        /* Under "no case" the shipped dial carries the printed ring already eased in by
         * dynScale (buildDial's lens), while this pass crops and reports in authored space.
         * Sample the dial host back through the inverse lens so the caps and the shade are
         * cut from the frame ring that is really there, not from where it would be without
         * the opening. A subdial host is a raw unscaled canvas and passes straight through. */
        const pre = (o.caseStyle === 'none' ? F.dynK : 1) || 1;
        const put = (x, L, inv) => { if (inv && pre !== 1) { x.save(); x.translate(C, C); x.scale(1 / pre, 1 / pre); x.translate(-C, -C); x.drawImage(L, 0, 0); x.restore(); } else x.drawImage(L, 0, 0); };
        // The host plane that actually carries the frame ring at that spot. A
        // transparent subdial lays no plate, so the dial is the layer under the aperture
        // and the shade has to be sampled from it -- sampling the empty sub canvas would
        // ship a window with no frame and no inner shade at all.
        const hostSub = c.host === 'dial' ? null : (F.subLayers.find((q) => q.id === c.host) || null);
        const hostL = hostSub && !hostSub.transparent ? hostSub.canvas : F.dial;
        const hostCut = (keepInside) => {
          if (!hostL) return cv(1);
          const o = cv(S), x = o.getContext('2d');
          put(x, hostL, hostL === F.dial);
          x.globalCompositeOperation = keepInside ? 'destination-in' : 'destination-out';
          x.beginPath(); hp(x); x.fillStyle = '#000'; x.fill();
          return o;
        };
        // band plate: the disc with the host's OUTSIDE-the-path art (ring, drop shadow) laid in,
        // and nothing from inside the path — anything the band leaves transparent here is filled
        // by the strip underneath it, which was cut from the full engine crop.
        const paintPlate = (ctx) => {
          if (c.host !== 'dial') put(ctx, F.dial, true);
          paint(ctx, Object.assign({}, c, { fmt: '', blank: true }), new Date(), i);
          ctx.drawImage(hostCut(false), 0, 0);
        };
        const full = cv(S), fx = full.getContext('2d');
        if (c.host === 'dial') { paint(fx, Object.assign({}, c, { fmt: '', blank: true }), new Date(), i); put(fx, F.dial, true); }
        else { put(fx, F.dial, true); paint(fx, Object.assign({}, c, { fmt: '', blank: true }), new Date(), i); if (hostL) fx.drawImage(hostL, 0, 0); }
        const bandOnly = cv(S), bx0 = bandOnly.getContext('2d');
        paintPlate(bx0);
        const Wc = Math.ceil((h.w + 2 * m) * k), Hc = Math.ceil((h.h + 2 * m) * k), lc = local(full, h.x, h.y, h.rot, Wc, Hc);
        const e = Math.ceil((Math.max(h.rad || 2.5, 3) + m + 2) * k), mid = Math.floor(Wc / 2), slice = crop(lc, mid, 0, 1, Hc);
        const band = crop(local(bandOnly, h.x, h.y, h.rot, Wc, Hc), mid, 0, 1, Hc);
        const shadeArt = { canvas: local(hostCut(true), h.x, h.y, h.rot, Wc, Hc), w: Wc, h: Hc };
        const en0 = enOf(c), dark = c.disc === 'dark' || (c.disc !== 'light' && !!en0.dark), ink = c.tc && c.tc !== 'auto' ? c.tc : dark ? '#ecdfbf' : '#18120b';
        setFace(fontKey);
        // the date field prints with no tracking and no 0.75k nudge (drawComp), so the
        // tiles have to be cut on those same metrics or the assembled run sits wrong
        const font = winFont(c, kk * k), ls = isDate ? 0 : 1.5 * kk * k, mx = cv(4).getContext('2d'); mx.font = font;
        const glyphs = (isDate ? '0123456789'.split('') : charset(c.fmt, c.fcase)).map((ch) => {
          const wNo = mx.measureText(ch).width, w = Math.max(1, Math.round(wNo + ls)), t = tileOf(band, w), tx = t.getContext('2d');
          tx.font = font; tx.fillStyle = ink; tx.textAlign = 'left'; tx.textBaseline = 'middle'; tx.fillText(ch, Math.round(ls / 2), Hc / 2 + kk * k);
          return { ch, canvas: t, w, h: Hc, adv: w, ox: 0 };
        });
        const idx = {};
        if (isDate) for (let d = 1; d <= 31; d++) idx[String(d).padStart(2, '0')] = String(d);   // padded key, unpadded ink: what to draw on the 5th is '5', not '05'
        else for (let t = new Date(y0, 0, 1); t.getFullYear() <= y0 + 1; t.setDate(t.getDate() + 1)) idx[iso(t)] = fmtDate(c.fmt, t, c.fcase);
        const advOf = {}; glyphs.forEach((g) => { advOf[g.ch] = g.adv; });
        const widestOf = (str) => [...str].reduce((a, ch) => a + (advOf[ch] || 0), 0);
        const widest = Math.max(0, ...Object.values(idx).map(widestOf));
        // The date field is authored, not fitted: the strip is exactly the plate the dial was
        // cut for, so the caps land on the frame ring they were cropped from. padX is the
        // clearance that is left over once the widest run is centred — the run is drawn LAST
        // and may legally overlap the cap zone, because that zone is flat disc there. It only
        // grows past the plate if the digits no longer fit the aperture at all.
        const runW = widest, spill = Math.max(0, runW - Math.floor(h.w * k));
        const padX = isDate ? Math.max(0, Math.floor((Wc - runW) / 2)) : Math.round((4 + m) * k);
        const fitWidth = isDate ? (spill ? e * 2 + runW + 2 * Math.ceil(k) : Wc) : Math.max(Wc, e * 2 + 2 * padX + widest);
        if (isDate && spill) console.warn(c.id + ': the widest day needs ' + runW + 'px and the aperture is ' + h.w + 'px — the run spills the frame');
        kits.push({ id: c.id, kind: isDate ? 'date' : 'window', host: c.host, centre: [+Mk(h.x).toFixed(2), +Mk(h.y).toFixed(2)], rotDeg: +((h.rot || 0) * 180 / Math.PI).toFixed(2), height: Hc,
          capL: { canvas: crop(lc, 0, 0, e, Hc), w: e, h: Hc }, capR: { canvas: crop(lc, Wc - e, 0, e, Hc), w: e, h: Hc }, slice: { canvas: slice, w: 1, h: Hc },
          shade: shadeArt,
          glyphs, padX, fitWidth, widest, format: isDate ? 'D' : c.fmt, fcase: c.fcase || 'upper', byDate: idx, opaque: true });
      });
      DF.forEach((f) => {
        const sz = (+f.size || 16), plate = f.bg && f.bg !== 'none', s2 = sz * (plate ? +f.fgScale || 1 : 1), pad = 6;
        const ph = plate ? 2 * sz * (+f.bgH || 1) : 0, pw = plate ? 2 * sz * (+f.bgW || 1.6) : 0, shp = f.bgShape || 'rect';
        const fam = (FONTS[f.font] && FONTS[f.font][1]) || SANS, font = (FW[f.weight] || '700') + ' ' + (s2 * 2 * k).toFixed(1) + 'px ' + fam, ls = (+f.spacing || 0) * s2 * k;
        const mx = cv(4).getContext('2d'); mx.font = font;
        const Hc = Math.ceil((plate ? ph : s2 * 2.8) * k + 2 * pad * k), base = Object.assign({}, f, { x: C, y: C, rot: 0, bgX: 0, bgY: 0 });
        const render = (ff) => { const full = cv(S); paintDF(full.getContext('2d'), ff, new Date(), true); return full; };
        let capL = null, capR = null, slice = null, Wc = 0, e = 0;
        if (plate) {
          Wc = Math.ceil((pw + 2 * pad) * k);
          const pl = local(render(Object.assign({}, base, { color: 'none' })), C, C, 0, Wc, Hc);
          const r = shp === 'pill' || shp === 'circle' ? ph / 2 : shp === 'rounded' ? Math.min(pw, ph) * 0.18 : 0;
          e = Math.ceil((r + pad + 2) * k);
          slice = crop(pl, Math.floor(Wc / 2), 0, 1, Hc); capL = { canvas: crop(pl, 0, 0, e, Hc), w: e, h: Hc }; capR = { canvas: crop(pl, Wc - e, 0, e, Hc), w: e, h: Hc };
        }
        const glyphs = charset(f.fmt == null ? 'ddd DD MMM' : f.fmt, f.fcase).map((ch) => {
          const wNo = mx.measureText(ch).width, adv = Math.max(1, Math.round(wNo + ls));
          const g = render(Object.assign({}, base, { icon: 'text', text: ch, spacing: 0, bg: 'none', height: plate ? 0 : f.height }));
          if (plate) {
            const w = adv, t = tileOf(slice, w), lg = local(g, C, C, 0, w, Hc), tx = t.getContext('2d'); tx.drawImage(lg, 0, 0);
            return { ch, canvas: t, w, h: Hc, adv, ox: 0 };
          }
          const ox = Math.ceil(pad * k), w = adv + 2 * ox;
          return { ch, canvas: local(g, C, C, 0, w, Hc), w, h: Hc, adv, ox };
        });
        const idx = {}; for (let t = new Date(y0, 0, 1); t.getFullYear() <= y0 + 1; t.setDate(t.getDate() + 1)) idx[iso(t)] = fmtDate(f.fmt == null ? 'ddd DD MMM' : f.fmt, t, f.fcase);
        const advOf = {}; glyphs.forEach((g) => { advOf[g.ch] = g.adv; });
        const widestOf = (str) => [...str].reduce((a, ch) => a + (advOf[ch] || 0), 0);
        const widest = Math.max(0, ...Object.values(idx).map(widestOf));
        // padX is the inner padding the assembler keeps between a cap and the run. Reading it
        // off today's string made the manifest shift whenever the export day changed. Derive it
        // from the widest string the format can produce: the authored plate survives whenever the
        // run fits inside it, and the strip only grows past the plate (by tiling the slice) when
        // it does not -- so a long date never overruns the caps it is drawn between.
        const padX = plate ? Math.max(Math.ceil(2 * k), Math.floor((Wc - e * 2 - widest) / 2)) : 0;
        const fitWidth = plate ? Math.max(Wc, e * 2 + widest + 2 * padX) : widest;
        kits.push({ id: f.id, kind: 'flare', host: f.host, centre: [+Mk(f.x).toFixed(2), +Mk(f.y).toFixed(2)], rotDeg: +((f.rot || 0) * 180 / Math.PI).toFixed(2), height: Hc,
          capL, capR, slice: slice ? { canvas: slice, w: 1, h: Hc } : null, glyphs, padX, fitWidth, widest,
          format: f.fmt == null ? 'ddd DD MMM' : f.fmt, fcase: f.fcase || 'upper', byDate: idx, opaque: !!plate });
      });
      setFace(fontKey);
      return kits;
    };
    F.compSprites = () => {
      const out = [];
      lay.comps.forEach((c, i) => {
        // 'custom' ships its letters through textKits(); so does the day-of-month field
        // now (see textKits), so neither needs a whole-value sprite here.
        if (c.type === 'custom' || c.type === 'date') return;
        const vals = c.type === 'date' ? Array.from({ length: 31 }, (_, k) => [String(k + 1).padStart(2, '0'), new Date(2026, 0, k + 1)])
          : c.type === 'day' ? DAYS.map((d, k) => [d, new Date(2026, 1, 1 + k)])
          : c.type === 'month' ? MONTHS.map((m, k) => [m, new Date(2026, k, 1)])
          : c.type === 'custom' ? (() => { const y0 = new Date().getFullYear(), seen = new Map(), idx = {}; for (let t = new Date(y0, 0, 1); t.getFullYear() <= y0 + 1; t.setDate(t.getDate() + 1)) { const sv = fmtDate(c.fmt, t, c.fcase); if (!seen.has(sv)) seen.set(sv, [String(seen.size + 1).padStart(4, '0'), new Date(t.getTime())]); idx[t.getFullYear() + '-' + String(t.getMonth() + 1).padStart(2, '0') + '-' + String(t.getDate()).padStart(2, '0')] = seen.get(sv)[0]; } c.index = idx; c.texts = Object.fromEntries([...seen].map(([sv, [n]]) => [n, sv])); return [...seen.values()]; })() : null;
        const b = holeBox(c.hole), pad = c.type === 'moon' ? 0 : 5 * (c.k || 1);
        const bx = Math.floor(b[0] - pad), by = Math.floor(b[1] - pad), bw = Math.ceil(b[2] + pad) - bx, bh = Math.ceil(b[3] + pad + (c.type === 'moon' && c.hole.kind === 'moon' && !c.hole.rot ? 2 : 0)) - by;
        const list = vals || Array.from({ length: 30 }, (_, k) => [String(k).padStart(2, '0'), k / 30]);
        list.forEach(([name, v]) => {
          const full = cv(S), fx = full.getContext('2d');
          if (c.type === 'moon') { setFace(fontKey); const h = hostOf(c); fx.save(); fx.beginPath(); fx.arc(h.x, h.y, h.r, 0, TAU); fx.clip(); drawComp(fx, c, null, enOf(c), pal, i + 1, v); fx.restore(); }
          else paint(fx, c, v, i);
          const sp = cv(bw, bh); sp.getContext('2d').drawImage(full, -bx, -by);
          out.push({ id: c.id, type: c.type, host: c.host, value: name, ...(c.type === 'custom' ? { format: c.fmt, fcase: c.fcase || 'upper', index: c.index, texts: c.texts } : {}), x: bx, y: by, w: bw, h: bh, canvas: sp });
        });
      });
      return out;
    };
    return F;
  }

  // base = movement() result (static plate, inner-light haze, optional live tourbillon); gt = one geartrain frame
  function drawCage(ctx, base, gt, date) {
    if (!gt || !gt.cage || !base || !base.cageAt) return;
    const [x, y] = base.cageAt;
    ctx.save(); ctx.translate(x, y); ctx.rotate(TURN.second(date) * TAU - (base.cageOff || 0)); ctx.translate(-x, -y); ctx.drawImage(gt.cage, 0, 0); ctx.restore();
  }
  function compose(out, base, gt, F, date, hidden) {
    hidden = hidden || {};
    out.setTransform(1, 0, 0, 1, 0, 0); out.globalCompositeOperation = 'source-over'; out.globalAlpha = 1; out.filter = 'none';
    out.fillStyle = '#000'; out.fillRect(0, 0, S, S);
    const k = F.inset || 1;
    let ctx = out;
    if (k < 1) { if (!F._in) F._in = cv(S); ctx = F._in.getContext('2d'); ctx.setTransform(1, 0, 0, 1, 0, 0); ctx.clearRect(0, 0, S, S); ctx.fillStyle = '#000'; ctx.fillRect(0, 0, S, S); }
    /* The opening under "no case": everything the engine RENDERS is eased in to fit inside
     * the ring the guide paints, and the author's own images are not. The dial arrives
     * already baked through this scale (buildDial's lens) so it blends as it stands, and
     * the plate-slot backdrop rides F.bgL untransformed -- the two pieces that must never
     * be resampled by the slider. A drawn case never reaches this: it eases the whole face
     * (art included) through the inset pass above instead, exactly as always. */
    const dk = F.dynK || 1;
    const dz = (fn) => { if (dk === 1) return fn(); ctx.save(); ctx.translate(C, C); ctx.scale(dk, dk); ctx.translate(-C, -C); fn(); ctx.restore(); };
    if (!hidden.movement && base) dz(() => ctx.drawImage(base.plate, 0, 0));
    if (!hidden.movement && F.bgL) ctx.drawImage(F.bgL, 0, 0);
    if (!hidden.movement && F.plateFlares) dz(() => ctx.drawImage(F.plateFlares, 0, 0));
    if (!hidden.geartrain && gt) dz(() => { ctx.drawImage(gt, 0, 0); drawCage(ctx, base, gt, date); });
    if (base && !(hidden.movement && hidden.geartrain)) dz(() => { ctx.globalCompositeOperation = 'lighter'; ctx.drawImage(base.haze, 0, 0); ctx.globalCompositeOperation = 'source-over'; });
    const cp = F.comps(date), df = F.dateFlares ? F.dateFlares(date) : null;
    if (!hidden.complications) dz(() => ctx.drawImage(cp.dial, 0, 0));
    if (!hidden.dial) { ctx.drawImage(F.dial, 0, 0); if (df) dz(() => ctx.drawImage(df.dial, 0, 0)); }
    F.subLayers.forEach((sl) => {
      if (!hidden.complications && cp.subs[sl.id]) dz(() => ctx.drawImage(cp.subs[sl.id], 0, 0));
      if (!hidden.subdials) { dz(() => ctx.drawImage(sl.canvas, 0, 0)); if (df && df.subs[sl.id]) dz(() => ctx.drawImage(df.subs[sl.id], 0, 0)); }
    });
    if (!hidden.hands) dz(() => F.drawHands(ctx, date));
    if (!hidden.glass) dz(() => ctx.drawImage(F.glass, 0, 0));
    if (k < 1) { out.save(); out.translate(C, C); out.scale(k, k); out.translate(-C, -C); out.drawImage(F._in, 0, 0); out.restore(); }
    if (!hidden.outer) out.drawImage(F.outer, 0, 0);
  }
  function canonDesign(d) {
    if (!d.surface || d.surface === 'enamel') { delete d.surface; delete d.texture; }
    if (d.texture != null && +d.texture === 1) delete d.texture;
    if ((!DIAL_COLORS[d.color] && !isHex(d.color)) || d.color === 'cream') delete d.color;
    ['aging', 'stains', 'dust'].forEach((k) => { if (d[k] != null && +d[k] === 1) delete d[k]; });
    if (!HAS_NUMS.includes(d.print) || d.numerals === 'none' || d.numOrient === (d.print === 'roman' ? 'radial' : 'upright')) delete d.numOrient;
    if (!d.font || d.font === 'auto' || d.font === RING_FONT(d.print)) delete d.font;
    if (!d.numColor || d.numColor === 'auto') delete d.numColor;
    ['numScale', 'n12Scale', 'nQScale'].forEach((k) => { if (d[k] != null && +d[k] === 1) delete d[k]; });
    if (!d.numStyle || d.numStyle === 'print') delete d.numStyle;
    if (!d.openwork || d.openwork === 'none') ['openwork', 'owBar', 'owRing', 'owHub', 'owRim', 'owInlay', 'owDepth'].forEach((k) => delete d[k]);
    if (!d.secStyle || d.secStyle === 'classic') delete d.secStyle;
    // ONE additional centre pointer besides hour/minute/second: what it indicates is the choice, and
    // the printed ring is derived from it through EXTRA_RING, so a ring can never disagree with the
    // hand that reads it. A design naming several (an old preset, an old design ID) keeps the one its
    // ring points at, else the first in stack order. perimShrink 1.00 leaves the chapter at full
    // size, which leaves nothing out there to print, and is how the ring is switched off.
    if (d.extra !== 'none' && !EXTRA_HANDS[d.extra]) {
      const ce0 = d.centre || {};
      d.extra = CAL_HANDS.find((k) => +ce0[k] && EXTRA_RING[k] === d.perim) || CAL_HANDS.find((k) => +ce0[k]) || 'none';
    }
    if (!EXTRA_HANDS[d.extra]) d.extra = 'none';
    if (d.centre) CAL_HANDS.forEach((k) => { if (k !== d.extra) delete d.centre[k]; });
    if (d.extra === 'none') {
      if (d.centre) CAL_HANDS.forEach((k) => delete d.centre[k]);
      // A dial with no additional hand must canonicalise to exactly the shape it had before the
      // feature existed. The PRESENCE of the key, not only its value, changes the JSON the design
      // id hashes, so shipping extra:'none' would re-key every dial on the shelf and file an
      // unchanged watch under a new kit name.
      delete d.extra;
      PERIM_KEYS.forEach((k) => delete d[k]);
    } else {
      d.centre = Object.assign({}, d.centre); d.centre[d.extra] = 1;
      d.perim = EXTRA_RING[d.extra];
      const ps = +d.perimShrink;
      if (!(ps >= 0.7 && ps <= 1) || ps === 0.88) delete d.perimShrink;
      if (!d.perimColor || d.perimColor === 'auto' || d.perimColor === 'ink') delete d.perimColor;
      if (!d.perimFont || d.perimFont === 'auto' || d.perimFont === 'same') delete d.perimFont;
      if (!d.perimOrient || d.perimOrient === 'upright') delete d.perimOrient;
      if (d.perimSize != null && +d.perimSize === 1) delete d.perimSize;
      if (!d.perimPos || d.perimPos === 'ontick') delete d.perimPos;
      if (d.perimRot != null && perimRotDeg(d) === 0) delete d.perimRot;
    }
    if (d.gmtOffset != null && (Math.round(+d.gmtOffset || 0) === 0 || !(d.centre && d.centre.gmt))) delete d.gmtOffset;
    /* Uploaded art is REFERENCED here, never embedded: a design carries the asset id
     * (and the placed geometry), the bytes live in the browser's asset store, and a
     * .wckit bundle is what ships them. Every default is deleted on purpose -- a
     * design with no art must serialise to exactly the JSON it produced before this
     * feature existed, or every design id already in the field would change. */
    const blank = (v) => v == null || v === '';
    const stripArt = (p, defSize) => {
      const keys = [p + 'Art', p + 'Fit', p + 'X', p + 'Y', p + 'Size', p + 'Opacity'];
      if (blank(d[p + 'Art'])) { keys.forEach((k) => delete d[k]); return; }
      if (!d[p + 'Fit'] || d[p + 'Fit'] === 'cover') delete d[p + 'Fit'];
      [p + 'X', p + 'Y'].forEach((k) => { if (+d[k] === 0 || blank(d[k])) delete d[k]; });
      if (blank(d[p + 'Zoom']) || +d[p + 'Zoom'] === 1) delete d[p + 'Zoom'];   // 1 is the identity: a design that never zooms must not read as a different one
      if (blank(d[p + 'Size']) || +d[p + 'Size'] === defSize) delete d[p + 'Size'];
      if (blank(d[p + 'Opacity']) || +d[p + 'Opacity'] === 1) delete d[p + 'Opacity'];
    };
    stripArt('bg', 442);
    stripArt('sf', 442);
    if (d.hands) Object.keys(d.hands).forEach((k) => {
      const h = d.hands[k]; if (!h) return;
      // an art-backed hand keeps its reference plus the authored pivot, in source
      // pixels, so a re-render can rebuild the sprite without the file. The pivot is
      // allowed outside the ink (an arbour hidden under a subdial is a real case), so
      // it is kept on its own terms rather than as an all-or-nothing pair -- only
      // (0,0), which no author means and which the centroid fallback serves better,
      // counts as unset. rot0 is the facing correction for art that was not drawn
      // pointing at 12; with no art there is nothing to correct.
      if (blank(h.art)) { ['art', 'pivotX', 'pivotY', 'rot0', 'zoom'].forEach((q) => delete h[q]); return; }
      if (+h.pivotX === 0 && +h.pivotY === 0) { delete h.pivotX; delete h.pivotY; }
      if (blank(h.rot0) || +h.rot0 === 0) delete h.rot0;
      if (blank(h.zoom) || +h.zoom === 1) delete h.zoom;
    });
    if (!d.handLume || d.handLume === 'none' || d.handLume === 'auto') delete d.handLume;
    if (!d.handShade || d.handShade === '3d') delete d.handShade;
    if (d.surface !== 'carbon') delete d.weaveAngle; else if (d.weaveAngle != null && +d.weaveAngle === 0) delete d.weaveAngle;
    if (d.surface !== 'carbon' && d.surface !== 'forged') delete d.weaveScale; else if (d.weaveScale != null && +d.weaveScale === 1) delete d.weaveScale;
    ['n12Font', 'nQFont'].forEach((k) => { if (!d[k] || d[k] === 'same') delete d[k]; });
    ['n12Color', 'nQColor'].forEach((k) => { if (!d[k] || d[k] === 'auto') delete d[k]; });
    (d.comps || []).forEach((c) => {
      if (c.type === 'opening' && (!c.shape || c.shape === 'circle')) { c.rot = 0; delete c.shape; }
      if (c.type === 'moon' && ROUND_MOON[c.style]) c.rot = 0;
      if (c.type === 'moon') ['moonColor', 'moonColor2', 'moonBg'].forEach((k) => { if (!c[k] || c[k] === 'auto' || !['graphic', 'neon'].includes(c.style)) delete c[k]; });
      if (c.type === 'subdial') {
        if (c.tfont === 'dial') delete c.tfont; if (c.tstyle === 'classic') delete c.tstyle; if (!c.tcolor || c.tcolor === 'auto') delete c.tcolor; if (c.torient === 'upright') delete c.torient; if (c.tdir === 'cw') delete c.tdir; if (c.tweight === 'bold') delete c.tweight; if (c.tsize != null && +c.tsize === 1) delete c.tsize;
        if (c.scale === 'none') ['tfont', 'torient', 'tdir', 'tweight', 'tsize', 'tstyle', 'tcolor'].forEach((k) => delete c[k]);
        if (c.color === 'match') delete c.color;
        if (c.height != null && +c.height === 1) delete c.height;
        if (c.style !== 'carbon') delete c.weaveAngle; if (c.style !== 'carbon' && c.style !== 'forged') delete c.weaveScale;
        if (c.tex != null && (c.style === 'enamel' || !c.style)) delete c.tex;
        if (!(+c.open > 0)) c.open = 0;
      }
      if (c.type === 'flare') { if (c.opacity != null && +c.opacity === 1) delete c.opacity; if (!c.bg || c.bg === 'none' || c.icon === 'arctext') ['bg', 'bgShape', 'bgW', 'bgH', 'bgX', 'bgY', 'fgScale'].forEach((k) => delete c[k]); if (c.fgScale != null && +c.fgScale === 1) delete c.fgScale; if (!c.color) c.color = 'auto'; if (!['text', 'arctext', 'datetext'].includes(c.icon)) { delete c.font; delete c.weight; delete c.spacing; } if (!['text', 'arctext'].includes(c.icon)) delete c.text; if (c.icon !== 'datetext') { delete c.fmt; delete c.fcase; } else if (c.fcase === 'upper') delete c.fcase; if (c.height != null && +c.height === 0) delete c.height; if (c.icon !== 'image') delete c.img; if (c.icon === 'arctext') c.rot = 0; }
      if (c.type === 'window' && c.shows !== 'custom') { delete c.fmt; delete c.fcase; delete c.fit; }
      if (c.type === 'window' && c.fcase === 'upper') delete c.fcase; if (c.type === 'window') delete c.fit;
      if (c.type === 'window') ['discColor', 'textColor', 'wfont'].forEach((k) => { if (!c[k] || c[k] === 'auto' || c[k] === 'dial') delete c[k]; });
      if (c.type === 'window') ['ww', 'wh'].forEach((k) => { if (c[k] != null && +c[k] === 1) delete c[k]; });
      // rect is the identity for a window, so a design that never asks for a round
      // aperture keeps the Design ID it had before the shape was authorable
      if (c.type === 'window') { if (!c.wshape || c.wshape === 'rect') delete c.wshape; } else delete c.wshape;
      // gilt is the movement's own metal: a design that never picked a frame colour
      // serialises exactly as it did before the choice existed, and a frame colour on
      // a comp that draws no raised frame (another style, a moon, a subdial) is noise
      {
        const framed = (c.type === 'window' && (!c.style || c.style === 'frame')) || (c.type === 'opening' && (c.style === 'frame' || c.style === 'lip'));
        if (!framed || !c.frameColor || c.frameColor === 'gilt' || !FRAME_PALS[c.frameColor]) delete c.frameColor;
      }
    });
    if (d.hands) {
      Object.keys(d.hands).forEach((id) => { const h = d.hands[id]; if (h.style === 'auto') delete h.style; if (h.finish === 'auto') delete h.finish; if (h.len != null && +h.len === 1) delete h.len; if (h.wid != null && +h.wid === 1) delete h.wid; if (h.on !== false) delete h.on; if (!h.sec || h.sec === 'auto') delete h.sec; if (!h.lume || h.lume === 'auto') delete h.lume; if (!h.shade || h.shade === 'auto') delete h.shade; if (!Object.keys(h).length) delete d.hands[id]; });
      if (!Object.keys(d.hands).length) delete d.hands;
    }
    return d;
  }

  /* The bank depth and the teeth-per-loop constant live in dial-renderer.js; the
   * composer's copy and the packer both read them back through here so no
   * second copy of the number exists to drift. */
  window.WatchEngine = { SIZE: S, PRESETS,
    TEETH_PER_LOOP: window.DialRenderer.TEETH_PER_LOOP,
    bankFrames: (f) => window.DialRenderer.bankFrames(f), DIAL_COLORS, COLOR_GROUPS, RING_FONT, HAS_NUMS, PLATES, BEZEL, canonDesign, caseMetalOf: (o) => caseMetal(o), TYPEFACES, SUB_DISPLAYS, OPENWORK, SEC_STYLES, NEW_COMP, TRAINS, HAND_STYLES, HAND_FINISHES, COMP_TYPES, COMP_STYLES, FRAME_COLORS, SCALES, SHOWS, PRINTS, NUMERALS, SURFACES, COLOR_NAMES, SHAPES, SIZE_RANGE, TURN, CASE_STYLES, CASE_METALS, GLASS_STYLES, PERIMS, CAL_HANDS, EXTRA_HANDS, EXTRA_RING, perimGeom, PANEL, MM_PER_PX, pxMm, REAL_CSS, REAL_Z, RULER_MM, realCss, rulerPx, screenInch, perimRotDeg, ringTop, ringStarts, handParts, caseLayer, glassLayer, movement, face, compose, drawCage, handPreview, moonPhase, cycleDate, previewDate, PREVIEW_MOON, isHex, fmtDate, features, aimAngle, trainAngle, tune, wb: whiteBalance, FONT_NAME, FLARE_ICONS, IMAGES,
    // uploaded art: the registry the composer persists, and the two draw helpers
    // (drawAsset for a placed backdrop, handSpriteAsset for a sprite-backed hand)
    assetFromFile, assetGet, assetReady, assetKnown, assetFits, assetBytes, assetDump, assetRestore, assetDrop, assetClear,
    drawAsset, artInDial, handSpriteAsset, artBox, caseOpenR, faceScale, dynScale, artClipR, PANEL_R, CASE_OPEN_MIN, CASE_OPEN_MAX, handHit, handAngle, handZeroDeg, inkCentroidOf, sha256Of, assetBytesOf: bytesOf, ASSETS, INVISIBLE_ALPHA, MAX_ASSET_BYTES, ASSET_TOTAL_BYTES };
})();
