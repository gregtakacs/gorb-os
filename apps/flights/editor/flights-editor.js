/* flights-editor.js — the flight tracker's part of its theme editor (flight-tracker-design
 * §12, §12.5; theme-editor-kit-plan §8, E3).
 *
 * Orb Manager owns the page (host@1): the start step, the library, import, the Render step,
 * the stage with its frame clock, Design, Panel and Split and its controls, Reset, the fit,
 * history and the send. What is the tracker's is here, registered with OrbHost: the design
 * and its fields, the parts list, and the preview's simulated traffic under the turning arm,
 * on the owner's own home map at the toolbar's range. */
(function (root) {
  'use strict';
  var T = root.OrbTheme, B = root.FlightsBake, M = root.FlightsMap;
  var word = function (hub, web) { return T && T.hostText ? T.hostText(hub, web) : hub; };
  var A = function () { return root.FlightsArt; };
  // the rows the tracker poses (arm, planes, rings, labels, the far dots) and, with a map,
  // its six names; an app seats CLK_ROW_MAX of them
  var G = 466, CX = 233, ROW_CAP = 64, CARD_MARGIN = 20;
  var liveRows = function (d) { return 1 + 15 + 2 + 4 + (d.planes.dots && d.planes.dots.show !== 'off' ? 9 : 0) + (d.map && d.map.on ? 6 : 0); };
  // main.c RANGES, nearest first, by the orb's distance unit; and a unit's km
  var RANGES = { km: [20, 40, 80, 140], mi: [10, 25, 50, 90], nm: [10, 20, 40, 80] }, UNIT_KM = { km: 1, mi: 1.609344, nm: 1.852 };
  var MODES = [['roam', 'Roam', 'The scope as it roams: the count in the header, the feed in the footer'],
    ['info', 'Info', 'A contact picked: the header in the accent tone, the info card and the selection ring'],
    ['alert', 'Alert', 'The feed is down: the footer in the warning tone']];
  var BAND_OF = function (alt) {        // apps/flights/main.c band_of: the seven bands, ground first
    if (alt === 0) return 0; if (alt == null) return 1;
    return alt < 3000 ? 2 : alt < 10000 ? 3 : alt < 20000 ? 4 : alt < 30000 ? 5 : 6;
  };
  // main.c stack_key: the ground under everything, no altitude above it, then by feet
  var STACK = function (alt) { return alt === 0 ? -2 : alt == null ? -1 : alt; };
  // made-up traffic: [shape slot, altitude ft (0 ground, null none), range 0..1, bearing, track, callsign, type, px/s]
  var TRAFFIC = [[3, 34000, .82, 40, 130, 'UAL1532', 'B38M', 9], [4, 38000, .55, 300, 95, 'AAL31', 'B77W', 10], [3, 12000, .38, 140, 60, 'SWA2210', 'B737', 7],
    [0, 2500, .22, 230, 150, 'N4521K', 'C172', 4], [1, 8000, .66, 190, 280, 'ENY3301', 'E75L', 6], [2, 41000, .9, 110, 20, 'EJA512', 'C68A', 11],
    [5, 1200, .3, 330, 240, 'N911TH', 'EC35', 4], [6, null, .72, 270, 0, 'ADSB01', '', 5], [3, 0, .12, 80, 200, 'DAL88', 'A321', 1.5],
    [3, 26000, .48, 20, 110, 'FFT1290', 'A20N', 8], [4, 31000, .95, 210, 300, 'QTR731', 'A35K', 10], [1, 14000, .6, 60, 330, 'SKW4410', 'CRJ9', 6]];
  var SEL = 0, EMERGENCY = 6;

  /* ---- the design ---- */
  var clone = function (o) { return JSON.parse(JSON.stringify(o)); };
  // a design opened from anywhere: every part present and in range, or why not
  function checkDesign(x) {
    if (!x || !x.scope || !x.arm || !x.planes || !x.words || !x.frames) return 'that design has no tracker theme in it';
    if (!x.scope.ticks || !x.scope.labels || !x.scope.rings || !x.scope.home || !x.scope.cross || !x.scope.lubber) return 'that design’s scope is missing a part';
    if (x.planes.family !== 'custom' && !A().families[x.planes.family]) return 'that design names a plane family this editor does not carry (' + x.planes.family + ')';
    if (!B.ARMS.some(function (a) { return a[0] === x.arm.kind; })) return 'that design names an arm this editor does not carry';
    if (!Array.isArray(x.planes.bands) || x.planes.bands.length !== 7) return 'that design’s planes are not seven altitude bands';
    if (!x.frames.card) return 'that design has no info card';
    return null;
  }
  // a picture read from a file, at most `max` px on its longer side (never past the 1024 the
  // shelf keeps art inside). Since S3b an SVG comes back as the text it arrived as — the kit
  // checks it against the design's rules and refuses the file here if it breaks one — and a
  // raster is drawn down to pixels. The studio kit does the work when it is on the page.
  function readPicture(f, max) {
    var cap = Math.min(max, 1024);
    if (root.OrbStudio && typeof root.OrbStudio.fitPicture === 'function') {
      return root.OrbStudio.fitPicture(f, { max: cap }).then(function (r) { return r.dataUrl; });
    }
    return new Promise(function (ok, bad) {
      var rd = new FileReader();
      rd.onerror = function () { bad(new Error('the file could not be read')); };
      rd.onload = function () {
        var url = String(rd.result);
        var svg = /svg/.test(f.type) || /^data:image\/svg/i.test(url);
        B.image(url).then(function (im) {
          var w = im.naturalWidth || im.width || 0, h = im.naturalHeight || im.height || 0;
          if (svg && !(w > 0 && h > 0)) { w = cap; h = cap; }
          var long = Math.max(w, h) || 1;
          var k = svg ? cap / long : Math.min(1, cap / long);
          if (!isFinite(k) || k <= 0) k = 1;
          var c = document.createElement('canvas');
          c.width = Math.max(1, Math.round(w * k)); c.height = Math.max(1, Math.round(h * k));
          c.getContext('2d').drawImage(im, 0, 0, c.width, c.height);
          ok(c.toDataURL('image/png'));
        }, bad);
      };
      rd.readAsDataURL(f);
    });
  }

  /* ---- the preview's parts, each drawn again only when its own part of the design changes
   * (the host keeps them, c.part) ---- */
  // the orb's distance unit, as main.c reads it (a unit it has no ranges for is km)
  var unitOf = function (c) { return RANGES[c.unit] ? c.unit : 'km'; };
  function partsOf(d, c) {
    var plate = c.part('plate', JSON.stringify(d.scope), function () { return B.plateCanvas(d); });
    var arm = c.part('arm', JSON.stringify(d.arm) + d.scope.face, async function () { var a = B.arm(d); return { c: await B.rast(a.svg, a.size[0], a.size[1]), piv: a.pivot }; });
    var planes = c.part('planes', JSON.stringify(d.planes), async function () {
      var mk = B.markers(d);
      return { frames: await B.planeFrames(d), px: B.planeBox(d),
        sel: await B.rast(mk[0][2], 56, 56), em: await B.rast(mk[1][2], 64, 64) };
    });
    var words = c.part('words', JSON.stringify(d.words) + JSON.stringify(d.frames), async function () {
      var L = B.layout(d), out = { layout: L, panels: {} };
      for (var n in L) out.panels[n] = await B.panelCanvas(d, n);
      var z = B.sizes(d);
      try { await document.fonts.load(z.body + 'px "' + d.words.face + '"'); await document.fonts.load(z.quiet + 'px "' + d.words.face + '"'); } catch (e) { /* fallback face */ }
      return out;
    });
    // the owner's home map at the toolbar's range, drawn as the orb draws it, names and all;
    // a level kept per range, so a step back is at once
    var home = c.home, unit = unitOf(c), rng = c.view.range, map = null;
    if (home && d.map && d.map.on && root.GorbMaps) {
      var rangeKm = RANGES[unit][rng] * UNIT_KM[unit], z = M.levelOf(home.lat, rangeKm).z;
      var b = c.part('level' + rng, home.lat + '|' + home.lon + '|' + z, function () { return root.GorbMaps.fetchLevel(home.lat, home.lon, z); }, { fresh: true });
      if (b) map = c.part('map' + rng, JSON.stringify(d.map) + '|' + z + '|' + home.lat, function () {
        return document.fonts.load((d.map.nameSize | 0) + 'px "' + (d.map.nameFace || d.words.face) + '"').catch(function () {})
          .then(function () { return M.view(d.map, b, home.lat, rangeKm, rng); });
      });
    }
    if (words) words = Object.assign({}, words, { rangeText: RANGES[unit][rng] + ' ' + unit });
    return { plate: plate, arm: arm, planes: planes, words: words, map: map };
  }

  /* ---- the traffic: each contact flies a straight chord across its circle and comes back in
   * where it left, so the scope never empties; ground traffic keeps to the field ---- */
  function where(c, t, rr) {
    var R0 = c[1] === 0 ? rr * 0.3 : rr, br = c[3] * Math.PI / 180, tr = c[4] * Math.PI / 180;
    var px = c[2] * R0 * Math.sin(br), py = -c[2] * R0 * Math.cos(br), dx = Math.sin(tr), dy = -Math.cos(tr);
    var b = px * dx + py * dy, q = Math.sqrt(Math.max(0, b * b - (px * px + py * py - R0 * R0)));
    var s0 = -b - q, len = 2 * q || 1, s = s0 + (((-s0 + c[7] * t / 1000) % len) + len) % len;
    return [CX + px + s * dx, CX + py + s * dy];
  }
  // where the card stands: home, or mirrored across the middle while its contact is under it
  // (main.c card_dy, hysteresis and all)
  function cardDy(box, x, y, was) {
    var cx = box[0] + box[2] / 2, cy = box[1] + box[3] / 2, mx = box[2] / 2 + CARD_MARGIN, my = box[3] / 2 + CARD_MARGIN;
    if (Math.abs(x - cx) < mx && Math.abs(y - cy) < my) return 2 * (CX - cy);
    return Math.abs(x - cx) >= mx + CARD_MARGIN || Math.abs(y - cy) >= my + CARD_MARGIN ? 0 : was;
  }

  // the map's layer as the orb blends it, and its names where main.c puts them
  function drawMap(x, d, mp) {
    x.drawImage(mp.canvas, 0, 0);
    var m = d.map;
    x.save();
    x.globalAlpha = M.alphaOf(m, 'names');
    x.fillStyle = m.ink.names; x.font = (m.nameSize | 0) + 'px "' + (m.nameFace || d.words.face) + '", sans-serif';
    x.textAlign = 'center'; x.textBaseline = 'middle';
    mp.names.forEach(function (n) { x.fillText(n.text, n.x, n.y); });
    x.restore();
  }

  // the words the app says, in the theme's face and tones, inside each label's box
  function drawWords(x, d, P, mode, cardY) {
    var W = d.words, Z = B.sizes(d);
    var say = function (name, lines, tone, dy) {
      var p = P.layout[name], b = p.box, y0b = b[1] + (dy || 0), size = Z[tone];
      x.drawImage(P.panels[name], b[0], y0b);
      x.font = size + 'px "' + W.face + '", sans-serif';
      x.textAlign = 'center'; x.textBaseline = 'middle';
      var col = W[tone];
      if (tone === 'accent' && W.accent2) { var g = x.createLinearGradient(0, y0b, 0, y0b + b[3]); g.addColorStop(0, W.accent); g.addColorStop(1, W.accent2); col = g; }
      x.fillStyle = col;
      var lh = Math.round(size * 1.35), y0 = y0b + b[3] / 2 - (lines.length - 1) * lh / 2;
      lines.forEach(function (s, i) { x.fillText(s, b[0] + b[2] / 2, y0 + i * lh); });
    };
    var c = TRAFFIC[SEL];
    say('header', [mode === 'roam' ? TRAFFIC.length + ' aircraft' : '1 of ' + TRAFFIC.length], mode === 'roam' ? 'body' : 'accent');
    say('footer', [mode === 'alert' ? 'feed down 3 min' : 'live  adsb.fi'], mode === 'alert' ? 'warning' : 'quiet');
    say('range', [P.rangeText || '80 nm'], 'quiet');
    if (mode !== 'roam') say('card', [c[5] + '  ' + c[6], 'DFW → SFO', c[1] + ' ft  ↑ 1800 fpm', '452 kt  trk ' + c[4]], 'body', cardY);
  }

  // one frame of the scope at time t (ms): the plate, the arm turned, the traffic flown along
  // its chords and stacked by altitude, the rings on their contacts, the words. live: the
  // moving layer alone on a clear canvas (Panel's live row), no plate under it. st: what one
  // frame leaves the next (the card's place)
  function drawFrame(x, d, parts, t, hidden, mode, live, st) {
    x.clearRect(0, 0, G, G);
    if (live) { /* the plate is Panel's under */ }
    else if (parts.plate && !hidden.scope) x.drawImage(parts.plate, 0, 0);
    else { x.fillStyle = '#000'; x.fillRect(0, 0, G, G); }
    if (!live && parts.map && !hidden.map && d.map && d.map.on) drawMap(x, d, parts.map);
    var rr = A().ring_r, ms = B.period(d) * 1000;
    // the arm under the planes, or over them and under the rings (§12.9), as the orb draws it
    var drawArm = function () {
      if (!parts.arm || hidden.arm || d.arm.kind === 'none') return;
      x.save(); x.translate(CX, CX); x.rotate(((t % ms) / ms) * 2 * Math.PI);
      x.drawImage(parts.arm.c, -parts.arm.piv[0], -parts.arm.piv[1]); x.restore();
    };
    var over = d.arm.layer === 'over';
    if (!over) drawArm();
    var P = parts.planes, sel = null;
    if (P && !hidden.planes) {
      var order = TRAFFIC.map(function (c, i) { return i; }).sort(function (a, b) { return STACK(TRAFFIC[a][1]) - STACK(TRAFFIC[b][1]); });
      order.forEach(function (i) {
        var c = TRAFFIC[i], p = where(c, t, rr), f = P.frames[c[0] * 7 + BAND_OF(c[1])];
        x.save(); x.translate(p[0], p[1]); if (c[1] !== 0) x.rotate(c[4] * Math.PI / 180); x.drawImage(f, -P.px / 2, -P.px / 2); x.restore();
        if (i === SEL) sel = p;
      });
      if (over) drawArm();
      if (mode !== 'roam' && sel) x.drawImage(P.sel, sel[0] - 28, sel[1] - 28);
      var e = where(TRAFFIC[EMERGENCY], t, rr); x.drawImage(P.em, e[0] - 32, e[1] - 32);   // N911TH squawks 7700 throughout
    } else if (over) drawArm();
    if (parts.words && !hidden.words) {
      var cb = parts.words.layout.card;
      if (st) st.dy = cb.clear && sel ? cardDy(cb.box, sel[0], sel[1], st.dy || 0) : 0;
      drawWords(x, d, parts.words, mode, st ? st.dy : 0);
    }
  }

  /* ---- the fields of each step ---- */
  function fields(step, c) {
    var a = A(), d = c.d, K = c.K, upd = c.upd, setD = c.set, base = c.base, say = c.say, getIn = c.getIn, home = c.home;
    var opt = function (pairs) { return pairs.map(function (p) { return { v: p[0], label: p[1] }; }); };
    // a colour at a path, the preset's own offered first
    var C = function (label, path, X) {
      var v = getIn(d, path);
      return K.Co(label, v == null ? 'none' : v, function (nv) { upd(path, nv === 'none' ? null : nv); }, '', path,
        Object.assign({ noAuto: true, preset: getIn(base, path) }, X || {}));
    };
    var onOff = function (label, path, on, off) { return K.Ch(label, getIn(d, path) ? 'on' : 'off', opt([['on', on || 'On'], ['off', off || 'Off']]), function (v) { upd(path, v === 'on'); }); };
    var num = function (label, path, min, max, stp, fmt) { var v = +getIn(d, path); return K.Rg(label, v, min, max, stp, function (nv) { upd(path, nv); }, fmt ? fmt(v) : String(v)); };
    var pick = function (label, path, pairs) { return K.Ch(label, getIn(d, path), opt(pairs), function (v) { upd(path, v); }); };
    var pct = function (v) { return Math.round(v * 100) + '%'; };
    var fields = [], sc = d.scope;
    if (step === 'scope') {
      var dLabel = { scope: 'Graticule', chart: 'Grid', sectional: 'Grid', brass: 'Bezel metal', blueprint: 'Grid', galaxy: 'Nebula' }[sc.ground];
      fields.push(K.H('Ground'));
      fields.push(K.Ch('Ground', sc.ground, opt(B.GROUNDS.map(function (k) { return [k, B.GROUND_TITLES[k]]; })), function (v) {
        setD(function (o) { var n = clone(o), g = B.SCOPES[v]; n.scope.ground = v; if (g) { n.scope.face = g.face; n.scope.detail = g.detail; n.scope.grid = g.grid; n.scope.gridWidth = g.gridWidth; } return n; });
      }));
      if (B.SCOPES[sc.ground]) fields.push(K.Ch('', '', [{ v: 'look', label: 'Take this ground’s whole look' }], function () {
        setD(function (o) { var n = clone(o), im = n.scope.image; n.scope = B.scopeOf(sc.ground); n.scope.image = im; return n; }); say('The scope took the ' + B.GROUND_TITLES[sc.ground] + '’s ticks, figures, cross and rings');
      }));
      if (sc.ground === 'image') fields.push(K.Fi('Picture', function (e) {
        var f = e.target.files && e.target.files[0]; if (!f) return;
        readPicture(f, 932).then(function (v) { upd('scope.image', v); }, function (er) { say('Not read: ' + er.message); });
      }, sc.image ? 'cut to fill the glass' : 'none yet'));
      if (sc.ground !== 'image') fields.push(C(sc.ground === 'brass' ? 'Enamel' : sc.ground === 'chart' || sc.ground === 'sectional' ? 'Paper' : sc.ground === 'galaxy' ? 'Space' : 'Face', 'scope.face'));
      if (dLabel) fields.push(C(dLabel, 'scope.detail'));
      if (sc.ground === 'galaxy') fields.push(onOff('Stars', 'scope.grid', 'Dense', 'Sparse'));
      if (sc.ground === 'scope' || sc.ground === 'chart' || sc.ground === 'sectional' || sc.ground === 'blueprint') {
        fields.push(onOff('Grid', 'scope.grid', 'Shown', 'Off'));
        if (sc.grid) fields.push(num('Grid weight', 'scope.gridWidth', 0.3, 4, 0.1, function (v) { return v.toFixed(1) + ' px'; }));
      }
      fields.push(K.H('Bezel ticks'));
      fields.push(onOff('Ticks', 'scope.ticks.on', 'Shown', 'Off'));
      if (sc.ticks.on) {
        fields.push(C('Colour', 'scope.ticks.colour'));
        fields.push(pick('Style', 'scope.ticks.style', [['line', 'Lines'], ['dot', 'Dots'], ['wedge', 'Wedges']]));
        fields.push(pick('Every', 'scope.ticks.minor', [[0, 'Majors only'], [1, '1°'], [2, '2°'], [5, '5°'], [10, '10°']]));
        fields.push(pick('Major every', 'scope.ticks.major', [[10, '10°'], [15, '15°'], [30, '30°'], [45, '45°'], [90, '90°']]));
        fields.push(num('Length', 'scope.ticks.len', 4, 24, 1, function (v) { return v + ' px'; }));
        fields.push(num('Weight', 'scope.ticks.width', 0.5, 4, 0.1, function (v) { return v.toFixed(1) + ' px'; }));
        fields.push(num('Radius', 'scope.ticks.r', 150, 228, 1, function (v) { return v + ' px'; }));
        fields.push(onOff('Rim line', 'scope.ticks.edge', 'Drawn', 'None'));
      }
      fields.push(K.H('Bearing figures'));
      fields.push(pick('Figures', 'scope.labels.kind', [['none', 'None'], ['cardinal', 'N E S W'], ['compass', 'N 30 60 E'], ['degrees', '030 060'], ['tens', '3 6 9']]));
      if (sc.labels.kind !== 'none') {
        if (sc.labels.kind !== 'cardinal') fields.push(pick('Every', 'scope.labels.every', [[30, '30°'], [45, '45°'], [90, '90°']]));
        fields.push(K.Sl('Typeface', sc.labels.face, B.FACES.map(function (k) { return { v: k, label: k }; }), function (v) { upd('scope.labels.face', v); }));
        fields.push(num('Size', 'scope.labels.size', 10, 28, 1, function (v) { return v + ' px'; }));
        fields.push(C('Colour', 'scope.labels.colour'));
        fields.push(pick('Turn', 'scope.labels.turn', [['upright', 'Upright'], ['radial', 'With the rose']]));
        fields.push(num('Radius', 'scope.labels.r', 110, 226, 1, function (v) { return v + ' px'; }));
        fields.push(onOff('North, south', 'scope.labels.skip', 'Left clear', 'Figured'));
        fields.push(K.Nt('', 'Left clear: the header sits over north and the info card over south.'));
      }
      fields.push(K.H('Cross'));
      fields.push(onOff('Cross', 'scope.cross.on', 'Drawn', 'None'));
      if (sc.cross.on) {
        fields.push(pick('Style', 'scope.cross.style', [['solid', 'Solid'], ['dashed', 'Dashed'], ['dashdot', 'Dash-dot'], ['hatched', 'Hatched']]));
        fields.push(C('Colour', 'scope.cross.colour'));
        fields.push(num('Opacity', 'scope.cross.opacity', 0.1, 1, 0.05, pct));
      }
      fields.push(K.H('Lubber mark'));
      fields.push(onOff('North mark', 'scope.lubber.on', 'Drawn', 'None'));
      if (sc.lubber.on) fields.push(C('Colour', 'scope.lubber.colour'));
      fields.push(K.H('Range rings'));
      fields.push(num('Rings', 'scope.rings.n', 0, 4, 1, function (v) { return v ? String(v) : 'none'; }));
      if (sc.rings.n) {
        fields.push(pick('Style', 'scope.rings.dash', [['solid', 'Solid'], ['dashed', 'Dashed'], ['dotted', 'Dotted']]));
        fields.push(num('Weight', 'scope.rings.width', 0.5, 4, 0.1, function (v) { return v.toFixed(1) + ' px'; }));
        fields.push(C('Colour', 'scope.rings.colour'));
        fields.push(num('Opacity', 'scope.rings.opacity', 0.1, 1, 0.05, pct));
      }
      fields.push(K.H('Home'));
      fields.push(num('Dot', 'scope.home.size', 0, 8, 0.5, function (v) { return v ? v + ' px' : 'none'; }));
      if (sc.home.size) fields.push(C('Colour', 'scope.home.colour'));
    } else if (step === 'map') {
      var mp = d.map, base0 = d.base;
      fields.push(K.H('Map'));
      fields.push(onOff('Map', 'map.on', 'Under the scope', 'None'));
      if (!mp.on) fields.push(K.Nt('', 'A hint of the land under the traffic: the main roads, the water, the airports and the towns, from your home map (Orb Manager makes it when home is set). It stops at the outer ring.'));
      if (mp.on) {
        fields.push(K.Ch('Look', mp.look, opt(M.LOOKS), function (v) { setD(function (o) { var n = clone(o); n.map = M.applyLook(o.map, v, base0); return n; }); }));
        fields.push(K.Nt('', (M.LOOKS.find(function (l) { return l[0] === mp.look; }) || [0, 0, ''])[2] + '. A look sets the colours, lines and marks below; what is drawn stays.'));
        fields.push(num('Strength', 'map.strength', 0.1, 1, 0.05, function (v) { return Math.abs(v - 0.5) < 0.001 ? 'the look’s own' : Math.round(v / 0.5 * 100) + '% of the look'; }));
        fields.push(K.Nt('', 'Every opacity at once: lower keeps the map a hint under the aircraft, higher makes it a chart.'));
        fields.push(K.H('What is drawn'));
        fields.push(pick('Water', 'map.water', [['all', 'Lakes, sea and rivers'], ['lakes', 'Lakes and sea'], ['off', 'None']]));
        if (mp.water !== 'off') fields.push(pick('Lakes', 'map.lakes', M.LAKES));
        fields.push(pick('Roads', 'map.roads', [['motorways', 'Motorways'], ['main', 'Motorways, and main roads at the nearest range'], ['off', 'None']]));
        fields.push(pick('Airports', 'map.airports', [['runways', 'Runways at the nearest range, a symbol wider out'], ['symbols', 'A symbol'], ['off', 'None']]));
        if (mp.airports !== 'off') {
          fields.push(pick('Symbol', 'map.symbol', M.SYMBOLS));
          fields.push(pick('Which', 'map.fields', [['airline', 'Airline airports'], ['all', 'Every airfield']]));
        }
        fields.push(onOff('Town dots', 'map.dots', 'Drawn', 'None'));
        fields.push(pick('Names', 'map.names', M.NAMES));
        if (mp.dots || mp.names !== 'off') fields.push(pick('Towns', 'map.which', [['cities', 'Cities'], ['towns', 'Cities and towns']]));
        if (mp.names !== 'off') fields.push(onOff('Airport codes', 'map.codes', 'Named', 'Not named'));
        fields.push(onOff('Built-up areas', 'map.urban', 'Tinted', 'Not tinted'));
        fields.push(onOff('Borders', 'map.borders', 'State and country lines', 'None'));
        fields.push(K.Nt('', 'The main roads and the runways are drawn at the nearest range only; wider out, the motorways and the airport symbols. A river shorter than 48 px (a creek the map calls a river) is left out. Press the toolbar’s ranges to see each.'));
        fields.push(K.H('Colours'));
        M.INKS.forEach(function (ik) {
          if (ik[0] === 'urban' && !mp.urban) return;
          if ((ik[0] === 'airport' || ik[0] === 'runway') && mp.airports === 'off') return;
          if (ik[0] === 'runway' && mp.airports !== 'runways') return;
          if (ik[0] === 'town' && !mp.dots) return;
          if (ik[0] === 'names' && mp.names === 'off') return;
          if ((ik[0] === 'water' || ik[0] === 'river') && mp.water === 'off') return;
          if (ik[0] === 'river' && mp.water !== 'all') return;
          if ((ik[0] === 'road1' || ik[0] === 'road2') && mp.roads === 'off') return;
          if (ik[0] === 'road2' && mp.roads !== 'main') return;
          if (ik[0] === 'border' && !mp.borders) return;
          fields.push(C(ik[1], 'map.ink.' + ik[0]));
          fields.push(num('Opacity', 'map.alpha.' + ik[0], 0, 1, 0.05, pct));
        });
        fields.push(K.H('Lines'));
        M.LINES.forEach(function (ln) {
          if (ln[0] === 'runway' && mp.airports !== 'runways') return;
          if ((ln[0] === 'road1' && mp.roads === 'off') || (ln[0] === 'road2' && mp.roads !== 'main')) return;
          if (ln[0] === 'river' && mp.water !== 'all') return;
          if (ln[0] === 'border' && !mp.borders) return;
          var L = mp.line[ln[0]];
          fields.push(K.Ch(ln[1], L[1], opt([['solid', 'Solid'], ['long', 'Dashed'], ['short', 'Short dashes'], ['dot', 'Dotted'], ['dashdot', 'Dash-dot']]),
            function (v) { upd('map.line.' + ln[0], [L[0], v]); }));
          fields.push(K.Rg('Weight', L[0], 1, 4, 1, function (v) { upd('map.line.' + ln[0], [v, L[1]]); }, L[0] + ' px'));
        });
        if (mp.dots) fields.push(pick('Town dot', 'map.town', [['dot', 'Dot'], ['ring', 'Ring'], ['square', 'Square'], ['squareLine', 'Square outline']]));
        if (mp.names !== 'off') {
          fields.push(K.H('Names'));
          fields.push(K.Sl('Typeface', mp.nameFace || d.words.face, B.FACES.map(function (k) { return { v: k, label: k }; }), function (v) { upd('map.nameFace', v); }));
          fields.push(num('Size', 'map.nameSize', 14, 22, 1, function (v) { return v + ' px'; }));
          fields.push(K.Nt('', (mp.nameFace || d.words.face) === d.words.face && (mp.nameSize | 0) === (d.words.size | 0)
            ? 'In the words’ own face and size, the names share their letters: they add nothing to the theme.'
            : 'Another face or size carries a set of letters of its own: about 150 KB more on the orb.'));
        }
        fields.push(K.Nt('', home ? 'The preview is your own home map, ' + home.name + ', drawn the way the orb draws it.' : 'Open this editor from the hub, with home set on the Orb tab, to preview the map on your own home.'));
      }
    } else if (step === 'arm') {
      var m = d.arm;
      fields.push(K.H('Radar arm'));
      fields.push(K.Ch('Style', m.kind, opt(B.ARMS), function (v) {
        setD(function (o) { var n = clone(o); n.arm = Object.assign(B.armOf(v, o.arm.period), { length: o.arm.length, layer: o.arm.layer, shadow: o.arm.shadow }); return n; });
      }));
      if (m.kind !== 'none') {
        fields.push(num('One turn', 'arm.period', B.PERIOD_MIN, B.PERIOD_MAX, 1, function (v) { return v + ' s'; }));
        var jump = Math.round(2 * Math.PI * a.ring_r * m.length / (B.period(d) * B.SWEEP_FPS));
        fields.push(K.Nt('', 'Its tip jumps ' + jump + ' px a frame at ' + B.SWEEP_FPS + ' fps: a far jump tears; a slower turn, a tail or a halo smooths it.'));
        fields.push(pick('Depth', 'arm.layer', [['under', 'Under the planes'], ['over', 'Over the planes']]));
        fields.push(K.Nt('', 'Under the planes, they pass over the arm; over them, the arm sweeps across the planes and stays under the selection and emergency rings and the words. The orb draws it the same way.'));
        fields.push(num('Length', 'arm.length', 0.5, 1, 0.01, pct));
        fields.push(C(m.kind === 'pointer' ? 'Metal' : m.kind === 'needle' ? 'Blade' : 'Colour', 'arm.colour'));
        if (m.kind === 'wedge' || m.kind === 'needle') fields.push(C(m.kind === 'needle' ? 'Tip' : 'Lead line', 'arm.lead'));
        fields.push(K.H('Tail and halo'));
        fields.push(num('Tail', 'arm.tail', 0, 90, 1, function (v) { return v ? v + '°' : 'none'; }));
        if (m.tail) fields.push(C('Tail colour', 'arm.tailColour'));
        fields.push(num('Halo', 'arm.halo', 0, 10, 0.5, function (v) { return v ? v + ' px' : 'none'; }));
        fields.push(K.Nt('', 'The halo is a soft glow of the arm’s colour along its blade, as wide as the slider: it blurs the step between frames.'));
        fields.push(K.H('Shadow'));
        fields.push(onOff('Shadow', 'arm.shadow.on', 'Under it', 'None'));
        if (m.shadow && m.shadow.on) {
          fields.push(num('Softness', 'arm.shadow.blur', 0.5, 6, 0.5, function (v) { return v + ' px'; }));
          fields.push(num('Strength', 'arm.shadow.opacity', 0.1, 1, 0.05, pct));
        }
        fields.push(K.Nt('', 'Tail, halo and shadow are baked into the arm’s picture: no extra drawing, only a few px more area to blend each frame.'));
      }
    } else if (step === 'planes') {
      var p = d.planes, custom = p.family === 'custom';
      fields.push(K.H('Planes'));
      if (!custom) fields.push(K.Ch('Draw', p.form, opt([['types', 'By category'], ['single', 'One plane']]), function (v) { upd('planes.form', v); }));
      fields.push(K.Ch('Family', p.family, opt(B.PRESETS.map(function (k) { return [k, a.families[k].title]; }).concat([['custom', 'Your own']])), function (v) {
        setD(function (o) { var n = clone(o); n.planes.family = v; if (v === 'custom') n.planes.form = 'single'; return n; });
      }));
      if (custom) {
        fields.push(K.Fi('Picture', function (e) {
          var f = e.target.files && e.target.files[0]; if (!f) return;
          readPicture(f, 192).then(function (v) { upd('planes.custom.image', v); say('Your plane: one picture for every aircraft, turned to its track'); }, function (er) { say('Not read: ' + er.message); });
        }, p.custom && p.custom.image ? 'drawn pointing up, nose to the top' : 'none yet: PNG, SVG or WebP, nose up', 'image/png,image/svg+xml,image/webp'));
        fields.push(K.Nt('', 'Your own picture is one plane for every aircraft: categories need a family’s drawings.'));
      } else fields.push(K.Nt('', p.form === 'single' ? a.families[p.family].single : a.families[p.family].band));
      fields.push(num('Size', 'planes.size', 30, 48, 1, function (v) { return (v * T.screen.MM_PER_PX).toFixed(1) + ' mm'; }));
      fields.push(K.H('Altitude colours'));
      fields.push(onOff(custom ? 'Tint' : 'Colours', 'planes.colour', 'By altitude', custom ? 'As drawn' : 'Off'));
      if (p.colour) a.bands.forEach(function (bd, i) {
        fields.push(K.Co(bd[0], p.bands[i], function (v) { var s = d.planes.bands.slice(); s[i] = v; upd('planes.bands', s); }, '', 'band' + i, { noAuto: true, preset: base.planes.bands[i] }));
      });
      fields.push(K.H('Heading'));
      fields.push(pick('Show', 'planes.heading.style', B.HEADINGS));
      if (p.heading && p.heading.style !== 'none') {
        if (p.heading.style === 'trail') fields.push(num('Length', 'planes.heading.length', 0.3, 1, 0.05, pct));
        if (!p.colour) fields.push(C('Colour', 'planes.heading.colour'));
        fields.push(K.Nt('', (p.heading.style === 'trail' ? 'A trail fading out behind each one' : 'An arrow at the nose') + ', drawn into its picture so it turns with the track' +
          (p.colour ? ', in the altitude colour.' : '.') + ' For pictures that show no direction of their own: planets, or your own round icon.'));
      }
      fields.push(K.H('Ground traffic'));
      fields.push(K.Ch('Ground', p.ground > 0 ? 'on' : 'off', opt([['on', 'Shown'], ['off', 'Off']]), function (v) { upd('planes.ground', v === 'on' ? (p.ground > 0 ? p.ground : 0.6) : 0); }));
      if (p.ground > 0) {
        fields.push(num('Opacity', 'planes.ground', 0.1, 1, 0.05, pct));
        fields.push(onOff('Colour', 'planes.groundGrey', 'Greyscale', 'As in the air'));
      }
      fields.push(K.H('Far aircraft'));
      var dt = p.dots || { show: 'off' };
      fields.push(pick('Past the nearest ' + B.PLANE_ROWS, 'planes.dots.show', B.DOTS));
      if (dt.show === 'plain') fields.push(C('Colour', 'planes.dots.colour'));
      if (dt.show !== 'off') fields.push(num('Size', 'planes.dots.size', 4, 10, 1, function (v) { return v + ' px'; }));
      fields.push(K.Nt('', dt.show === 'off'
        ? 'Only the nearest ' + B.PLANE_ROWS + ' are drawn, and the header says how many more are tracked.'
        : 'The tracked aircraft past the nearest ' + B.PLANE_ROWS + ' show as dots, up to ' + B.DOT_SLOTS.length +
          (dt.show === 'bands' ? ', in their altitude colour.' : ', all one colour.')));
      fields.push(K.H('Shadow'));
      fields.push(onOff('Shadow', 'planes.shadow.on', 'Under each', 'None'));
      if (p.shadow && p.shadow.on) {
        fields.push(num('Softness', 'planes.shadow.blur', 0.5, 5, 0.5, function (v) { return v + ' px'; }));
        fields.push(num('Strength', 'planes.shadow.opacity', 0.1, 1, 0.05, pct));
        fields.push(K.Nt('', 'A soft shadow all round, baked into each plane: it turns with the plane, so it has no side. Each plane’s box grows by ' + (B.planeBox(d) - B.planePx(d)) + ' px.'));
      }
      fields.push(K.H('Selection ring'));
      fields.push(pick('Style', 'planes.selStyle', B.MARKS));
      fields.push(num('Weight', 'planes.selWidth', 1, 6, 0.5, function (v) { return v + ' px'; }));
      fields.push(C('Colour', 'planes.sel'));
      fields.push(K.H('Emergency ring'));
      fields.push(pick('Style', 'planes.emStyle', B.MARKS));
      fields.push(num('Weight', 'planes.emWidth', 1, 6, 0.5, function (v) { return v + ' px'; }));
      fields.push(C('Colour', 'planes.em'));
      fields.push(K.Nt('', 'On the helicopter squawking 7700 in the preview; on the orb, round any aircraft in an emergency.'));
    } else if (step === 'words') {
      var W = d.words, Z = B.sizes(d), F = d.frames;
      var sample = function (tone, text) {
        var st = { color: W[tone], background: F.style === 'none' ? (sc.ground === 'image' ? '#000' : sc.face) : F.fill, fontFamily: '"' + W.face + '", sans-serif', fontSize: Z[tone] + 'px', borderColor: F.line };
        if (tone === 'accent' && W.accent2) Object.assign(st, { background: 'linear-gradient(' + W.accent + ',' + W.accent2 + ') text', color: 'transparent', WebkitBackgroundClip: 'text', backgroundColor: F.fill });
        return K.Sa('', text, st);
      };
      fields.push(K.H('Typeface'));
      fields.push(K.Sl('Face', W.face, B.FACES.map(function (k) { return { v: k, label: k }; }), function (v) { upd('words.face', v); }));
      fields.push(num('Size', 'words.size', 14, 22, 1, function (v) { return v + ' px'; }));
      fields.push(K.Nt('', 'The quiet and warning words are 2 px smaller, never under 14.'));
      fields.push(K.H('Body'));
      fields.push(C('Colour', 'words.body')); fields.push(sample('body', 'UAL1532  B38M'));
      fields.push(K.Nt('', 'The info card, and the aircraft count while the scope roams.'));
      fields.push(K.H('Accent'));
      fields.push(C('Colour', 'words.accent')); fields.push(C('Fades to', 'words.accent2', { none: true })); fields.push(sample('accent', '1 of 12'));
      fields.push(K.Nt('', 'The header in Info and Zoom: the preview’s Info shows it.'));
      fields.push(K.H('Quiet'));
      fields.push(C('Colour', 'words.quiet')); fields.push(sample('quiet', 'live  adsb.fi'));
      fields.push(K.Nt('', 'The footer while the feed is live, and the range figure.'));
      fields.push(K.H('Warning'));
      fields.push(C('Colour', 'words.warning')); fields.push(sample('warning', 'feed down 3 min'));
      fields.push(K.Nt('', 'The footer when the feed is down or no home is set: Alert shows it.'));
    } else if (step === 'frames') {
      var Fr = d.frames, L = B.layout(d);
      fields.push(K.H('Frames'));
      fields.push(pick('Style', 'frames.style', B.FRAME_STYLES));
      if (Fr.style === 'picture') {
        var fit = B.pictureFit(d);
        fields.push(K.Fi('Picture', function (e) {
          var f = e.target.files && e.target.files[0]; if (!f) return;
          readPicture(f, 1200).then(function (v) { upd('frames.image', v); }, function (er) { say('Not read: ' + er.message); });
        }, Fr.image ? 'cut in nine to every frame' : 'none yet', 'image/png,image/svg+xml,image/webp'));
        fields.push(K.Nt('', 'Best: a PNG about ' + fit[0] + ' × ' + fit[1] + ' px (three times the card), its corners in the outer sixth. The corners keep their shape; the edges and middle stretch to the card.'));
        fields.push(num('Corners', 'frames.corner', 4, 32, 1, function (v) { return v + ' px'; }));
        fields.push(num('Opacity', 'frames.opacity', 0.3, 1, 0.02, pct));
      } else if (Fr.style !== 'none') {
        fields.push(C('Fill', 'frames.fill'));
        fields.push(num('Opacity', 'frames.opacity', 0.3, 1, 0.02, pct));
        fields.push(num('Edge', 'frames.edge', 0, 4, 0.25, function (v) { return v ? v + ' px' : 'none'; }));
        if (Fr.edge && !/^(brass|silver|bronze|gold)$/.test(Fr.style)) fields.push(C('Edge colour', 'frames.line'));
      }
      fields.push(num('Padding', 'frames.pad', 2, 16, 1, function (v) { return v + ' px'; }));
      if (Fr.style !== 'none') {
        fields.push(K.H('The card’s shadow'));
        fields.push(onOff('Shadow', 'frames.shadow.on', 'Under it', 'None'));
        if (Fr.shadow && Fr.shadow.on) {
          fields.push(num('Softness', 'frames.shadow.blur', 0.5, 6, 0.5, function (v) { return v + ' px'; }));
          fields.push(num('Drop', 'frames.shadow.drop', 0, 8, 1, function (v) { return v + ' px'; }));
          fields.push(num('Strength', 'frames.shadow.opacity', 0.1, 1, 0.05, pct));
        }
      }
      fields.push(K.H('The info card'));
      fields.push(num('Width', 'frames.card.width', 200, 320, 4, function (v) { return v + ' × ' + L.card.inner[3] + ' px'; }));
      fields.push(pick('Place', 'frames.card.place', [['auto', 'Auto'], ['bottom', 'Bottom'], ['top', 'Top']]));
      fields.push(K.Nt('', Fr.card.place === 'auto'
        ? 'Auto: at the bottom, moving to the top while the plane it describes flies under it.'
        : 'The card stays at the ' + Fr.card.place + ', over whatever flies under it.'));
    }

    return fields;
  }

  // the moving layers alone, or the still ones the bake stores (Panel's two halves), or the whole frame
  function draw(x, d, c) {
    var P = partsOf(d, c), st = c.memo.frame || (c.memo.frame = {}), mode = c.view.mode;
    if (c.layer === 'under') {
      x.clearRect(0, 0, G, G);
      if (P.plate) x.drawImage(P.plate, 0, 0);
      if (P.map && !c.hidden.map && d.map && d.map.on) drawMap(x, d, P.map);
      return;
    }
    if (c.layer === 'live') { drawFrame(x, d, P, c.t, c.hidden, mode, true, { dy: st.dy }); return; }
    drawFrame(x, d, P, c.t, c.hidden, mode, false, st);
  }

  var app = {
    slug: 'flights', title: 'Flights', schema: B.SCHEMA,
    migrate: { 1: function (o) { o.design = B.upgrade1(o.design); return o; } },
    check: checkDesign, complete: B.complete,
    blank: { note: 'Plain greys, nothing preset', make: function () { return B.preset('blank'); } },
    templates: B.STARTS.map(function (k) {
      var f = B.startInfo(k), pd = B.preset(k);
      return { key: k, name: f.title, note: B.GROUND_TITLES[pd.scope.ground] + ' · ' + (B.ARMS.find(function (r) { return r[0] === pd.arm.kind; }) || [0, pd.arm.kind])[1],
        make: function () { return B.preset(k); } };
    }),
    start: 'modern',
    picture: function (d, px) { return B.thumb(d, px); },
    steps: [['scope', 'Scope'], ['map', 'Map'], ['arm', 'Arm'], ['planes', 'Planes'], ['words', 'Words'], ['frames', 'Frames']],
    parts: [
      { id: 'scope', label: 'Scope', icon: '◎', note: function (d) { return B.GROUND_TITLES[d.scope.ground]; } },
      { id: 'map', label: 'Map', icon: '▦', note: function (d) { return d.map && d.map.on ? (M.LOOKS.find(function (l) { return l[0] === d.map.look; }) || [0, ''])[1] : 'none'; } },
      { id: 'arm', label: 'Arm', icon: '◜', note: function (d) { return d.arm.kind === 'none' ? 'none' : d.arm.kind + ' · ' + B.period(d) + ' s'; } },
      { id: 'planes', label: 'Planes', icon: '✈', note: function (d) { return d.planes.family === 'custom' ? 'your own' : A().families[d.planes.family].title; } },
      { id: 'words', label: 'Words', icon: 'A', note: function (d) { return d.words.face; }, also: ['frames'] },
    ],
    fields: fields,
    views: [
      { key: 'mode', label: 'Shows', tip: 'What the scope shows', options: MODES, init: 'info' },
      { key: 'range', label: 'Range', tip: 'The map’s range', init: 2, show: function (d) { return !!(d.map && d.map.on); },
        options: function (d, env, g) {
          var unit = RANGES[g.unit] ? g.unit : 'km';
          return RANGES[unit].map(function (r, k) { return [k, r + ' ' + unit, 'The map at the ' + ['nearest', 'second', 'third', 'widest'][k] + ' range, as a press steps it on the orb']; });
        } },
    ],
    play: { fps: function () { return B.SWEEP_FPS; }, what: 'the traffic' },
    paceBy: ['mode'],
    layers: true,
    draw: draw,
    bake: function (d, o) {
      if (d.planes.family === 'custom' && !(d.planes.custom && d.planes.custom.image)) return Promise.reject(new Error('pick your plane picture on the Planes step first'));
      return B.bake(d, o);
    },
    // the rows; the host adds the measured rate against the sweep's (theme-editor-kit-plan
    // §8.1 ruling 9 took back the rows-only verdict of 2026-11-04)
    fit: function (d) { return { rows: liveRows(d), cap: ROW_CAP }; },
  };

  root.FlightsEditor = { app: app, drawFrame: drawFrame, checkDesign: checkDesign, TRAFFIC: TRAFFIC, where: where, cardDy: cardDy };
  if (root.OrbHost) root.OrbHost.register(app);
})(typeof window !== 'undefined' ? window : globalThis);
