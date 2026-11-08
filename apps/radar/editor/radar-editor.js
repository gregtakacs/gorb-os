/* radar-editor.js — the radar's part of its theme editor (radar-plan §2.9, R7;
 * theme-editor-kit-plan §8, E1).
 *
 * Orb Manager owns the page (host@1): the start step, the library, import, the Render step,
 * the stage and its controls, the fit, history and the send. What is the radar's is here,
 * registered with OrbHost: the design's fields, its parts, its views (the place, the zoom and
 * the time), and the preview, drawn on the owner's own map of the chosen place. The map is built from the Map tiles
 * source through the hub, as the Orb tab's Maps preview builds it (maps@1), and drawn by
 * the same rasteriser the orb's matches, in the design's colours; the rain over it is made
 * up, coloured through the design's ramp. The bake is radar-bake.js. */
(function (root) {
  'use strict';
  var B = root.RadarBake;
  var G = 466, CX = 233;
  // the preview's zooms: three of the radar's six (apps/radar/main.c ZOOM), its map levels z6+step
  var ZOOMS = [[1, 'Region'], [3, 'Area'], [4, 'City']];
  var TIMES = [['now', 'Now', 'The newest frame'], ['past', '−40 min', 'A past frame'], ['next', '+30 min', 'A forecast frame: the time in the forecast colour']];
  var LIVE_ROWS = 13 + 2;                        // the theme's rows the radar poses, and its two layers

  var clone = function (o) { return JSON.parse(JSON.stringify(o)); };
  function checkDesign(x) {
    if (!x || !x.map || !x.rain || !x.words || !x.glass || !x.ground) return 'that design has no radar theme in it';
    if (!Array.isArray(x.rain) || x.rain.length < 2) return 'that design’s rain has fewer than two colours';
    return null;
  }
  // a colour's relative luminance (WCAG): the lightness check's measure
  function lum(c) {
    var n = parseInt(String(c).slice(1), 16), f = function (v) { v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * f(n >> 16) + 0.7152 * f((n >> 8) & 255) + 0.0722 * f(n & 255);
  }

  // one level of a place's map, built on the hub from the Map tiles source (maps@1)
  function level(pl, z) { return root.GorbMaps.fetchLevel(pl.lat, pl.lon, z); }
  // the radar's view radius at a zoom (radar.c rd_radius_m) and the scale bar there (rd_scale)
  function radiusM(step, lat) { return Math.floor(156543.03392 * Math.cos(lat * Math.PI / 180) / Math.pow(2, 6 + step) * 233 + 0.5); }
  function scaleOf(radius, unit, max) {
    var u = unit === 'mi' ? 1609.344 : unit === 'nm' ? 1852 : 1000, mpp = radius / 233, best = 1;
    [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000].forEach(function (n) { if (n * u / mpp <= max) best = n; });
    return { len: best, px: Math.floor(best * u / mpp + 0.5) };
  }

  /* ---- the preview's rain: a squall line and showers, the same every time, in dBZ ---- */
  var RAIN_CELLS = [[150, 300, 70, 52], [210, 270, 55, 44], [280, 250, 60, 38], [340, 230, 45, 30], [120, 160, 40, 26],
                    [330, 120, 35, 22], [250, 330, 30, 48], [190, 220, 25, 58], [380, 330, 38, 20]];
  var rainField = (function () {
    var f = new Float32Array(G * G);
    for (var y = 0; y < G; y++) for (var x = 0; x < G; x++) {
      var v = 0;
      RAIN_CELLS.forEach(function (c) {
        var dx = (x - c[0]) / c[2], dy = (y - c[1]) / (c[2] * 0.7), e = Math.exp(-(dx * dx + dy * dy));
        v = Math.max(v, c[3] * e + 6 * e * Math.sin(x * 0.07 + y * 0.05));
      });
      f[y * G + x] = v;
    }
    return f;
  })();
  function rainCanvas(d, shift) {
    var c = document.createElement('canvas'); c.width = c.height = G;
    var x = c.getContext('2d'), im = x.createImageData(G, G), lut = B.rainLut(d, 15);   // the app's default floor
    for (var i = 0; i < G * G; i++) {
      var px = i % G, src = rainField[i - Math.max(0, Math.min(px, shift | 0))] || 0;
      var v = Math.max(0, Math.min(255, Math.round((src + 32) * 2))), o = 4 * v;
      im.data[4 * i] = lut[o]; im.data[4 * i + 1] = lut[o + 1]; im.data[4 * i + 2] = lut[o + 2]; im.data[4 * i + 3] = lut[o + 3];
    }
    x.putImageData(im, 0, 0);
    return c;
  }

  /* ---- the map, drawn in the design: what the orb's map service draws ---- */
  function rolesAt(d, step) {
    var on = 0;
    B.ROLES.forEach(function (r, k) {
      if ((d.map.off || []).indexOf(r) >= 0) return;
      if (((d.map.from || {})[r] | 0) > step) return;
      on |= 1 << k;
    });
    return on;
  }
  function mapCanvas(d, bytes, step) {
    var GM = root.GorbMaps, M = d.map;
    var width = B.ROLES.map(function (r) { return Math.max(0, Math.min(4, (M.width && M.width[r]) | 0)); });
    var dash = B.ROLES.map(function (r) { return B.DASH[(M.dash && M.dash[r]) || 'solid'] | 0; });
    var marks = { city: 1, town: 2, village: 4, airport: 8 }, mo = 0;
    (M.marks || []).forEach(function (k) { mo |= marks[k] | 0; });
    var lv = GM.pickLevel(Math.round((d.__lat || 0) * 1e6), radiusM(step, d.__lat || 0));   // millionths
    var r = GM.render(bytes, { scale: lv.scale, rot_rad: 0, role_on: rolesAt(d, step), mark_on: mo, width: width.concat([0]), dash: dash.concat([0]) });
    var pal = B.ROLES.map(function (k) { return B.argb(M[k], 1); }).concat([0]);
    var c = document.createElement('canvas'); c.width = c.height = GM.W;
    var x = c.getContext('2d'), im = x.createImageData(GM.W, GM.W);
    GM.paint(r.raster, pal, im.data);
    x.putImageData(im, 0, 0);
    return { c: c, marks: r.marks };
  }

  // one frame of the preview: plate, map, what goes under the rain, rain, glass, dot, words
  function drawFrame(x, d, P, hidden, time, step, home, unit) {
    x.clearRect(0, 0, G, G);
    if (P.plate && !hidden.ground) x.drawImage(P.plate, 0, 0);
    var mp = P.map && P.map[step];
    if (mp && !hidden.map) x.drawImage(mp.c, 0, 0);
    var U = d.under || {};
    if (!hidden.overlay && U.colour && (U.rings || U.cross)) {
      x.strokeStyle = U.colour; x.lineWidth = U.width || 1;
      for (var k = 1; k <= (U.rings | 0); k++) { x.beginPath(); x.arc(CX, CX, 232 * k / U.rings, 0, 2 * Math.PI); x.stroke(); }
      if (U.cross) { x.beginPath(); x.moveTo(1, CX); x.lineTo(G - 1, CX); x.moveTo(CX, 1); x.lineTo(CX, G - 1); x.stroke(); }
    }
    if (P.rain && !hidden.rain) x.drawImage(P.rain[time], 0, 0);
    if (!hidden.overlay) {
      if (P.glass) x.drawImage(P.glass, 0, 0);
      if (P.dot) x.drawImage(P.dot, CX - 8, CX - 8);
    }
    if (hidden.words) return;
    var W = d.words, SW = W.scale || { face: W.nameFace, px: W.namePx, colour: W.quiet };
    var say = function (s, cx, cy, face, px, colour) {
      x.font = px + 'px "' + face + '", sans-serif'; x.textAlign = 'center'; x.textBaseline = 'middle';
      x.fillStyle = colour; x.fillText(s, cx, cy);
    };
    // the landmarks' names, as main.c draw_names places them: off the dot, inside the rim
    var n = 0, boxes = [];
    (mp ? mp.marks : []).some(function (m) {
      if (n >= Math.min(6, d.map.names | 0)) return true;
      var nm = m.name, w = 13 * nm.length + 12, X = Math.round(m.x), Y = Math.round(m.y) + 22, dx = X - CX, dy = Y - CX;
      if (dx * dx + dy * dy > 185 * 185 || dx * dx + dy * dy < 40 * 40 || Y < 130 || Y > 330) return false;
      if (boxes.some(function (b) { return Math.abs(X - b[0]) < (w + b[2]) / 2 && Math.abs(Y - b[1]) < 32; })) return false;
      boxes.push([X, Y, w]); n++;
      say(nm, X, Y, W.nameFace, W.namePx, W.quiet);
      return false;
    });
    say(home ? home.name : 'Home', CX, 58, W.face, W.px, W.ink);
    var t = TIMES.filter(function (q) { return q[0] === time; })[0][1].replace('−', '-');
    say(t, CX, 98, W.face, W.px, time === 'next' ? W.forecast : W.ink);
    // the scale bar and its length (main.c draw_scale), in the theme's scale
    var sc = d.scale, sb = scaleOf(radiusM(step, home ? home.lat : 33), unit, sc.max), x0 = CX - sb.px / 2;
    if (sc.edge) { x.fillStyle = sc.edge; x.fillRect(x0 - 2, 439, sb.px + 4, sc.width + 2); x.fillRect(x0 - 2, 440 - sc.tick - 1, 4, sc.tick + 2); x.fillRect(x0 + sb.px - 2, 440 - sc.tick - 1, 4, sc.tick + 2); }
    x.fillStyle = sc.ink; x.fillRect(x0, 440, sb.px, sc.width);
    if (sc.tick) { x.fillRect(x0, 440 - sc.tick, 2, sc.tick); x.fillRect(x0 + sb.px - 2, 440 - sc.tick, 2, sc.tick); }
    say(sb.len + ' ' + unit, CX, 414, SW.face, SW.px, SW.colour);
  }

  /* ---- the preview's parts, each drawn again only when its own part of the design changes ---- */
  function partsOf(d, c) {
    var P = c.memo, pl = c.place, st = c.view.zoom;
    // the chosen place's map at the toolbar's zoom (maps@1), one kept per zoom so a step back is at once
    P.map = {};
    if (pl && root.GorbMaps) {
      var lk = pl.lat + '|' + pl.lon + '|' + st;
      var bytes = c.part('level' + st, lk, function () { return level(pl, 6 + st); }, { fresh: true });
      if (!bytes && P.building !== lk) { P.building = lk; c.say('Building the map of ' + pl.name + ' for the preview…'); }
      if (bytes) {
        if (P.building === lk) { P.building = null; c.say(''); }
        P.map[st] = c.part('map' + st, JSON.stringify(d.map) + '|' + lk, function () { return mapCanvas(Object.assign({}, d, { __lat: pl.lat }), bytes, st); });
      }
    } else if (c.placesRead && !pl && !P.homeless) { P.homeless = true; c.say('Set home on the Orb tab to preview the map'); }
    var kRain = JSON.stringify(d.rain);
    if (P.kRain !== kRain) { P.kRain = kRain; P.rain = { now: rainCanvas(d, 0), past: rainCanvas(d, 40), next: rainCanvas(d, -30) }; }
    var kArt = JSON.stringify(d.ground) + JSON.stringify(d.glass) + JSON.stringify(d.dot);
    if (P.kArt !== kArt) { P.kArt = kArt; P.plate = B.plateCanvas(d); P.glass = B.glassCanvas(d); P.dot = B.dotCanvas(d); }
    // the faces the words draw in, loaded before they draw
    var W = d.words, SW = W.scale || {}, kF = [W.face, W.nameFace, SW.face].join('|');
    if (P.kF !== kF) {
      P.kF = kF;
      Promise.all([W.face, W.nameFace, SW.face].filter(Boolean).map(function (f) { return document.fonts.load('26px "' + f + '"', 'Győr 0123'); }))
        .catch(function () {}).then(c.redraw);
    }
    return P;
  }

  /* ---- the fields of each step ---- */
  function fields(step, c) {
    var d = c.d, K = c.K, upd = c.upd, setD = c.set, base = c.base, say = c.say, getIn = c.getIn;
    var opt = function (pairs) { return pairs.map(function (p) { return { v: p[0], label: p[1] }; }); };
    var C = function (label, path, X) {
      var v = getIn(d, path);
      return K.Co(label, v == null ? 'none' : v, function (nv) { upd(path, nv === 'none' ? null : nv); }, '', path,
        Object.assign({ noAuto: true, preset: getIn(base, path) }, X || {}));
    };
    var num = function (label, path, min, max, stp, fmt) { var v = +getIn(d, path); return K.Rg(label, v, min, max, stp, function (nv) { upd(path, nv); }, fmt ? fmt(v) : String(v)); };
    var px = function (v) { return v + ' px'; };
    var pct = function (v) { return Math.round(v * 100) + '%'; };
    var out = [], M = d.map;
    if (step === 'ground') {
      out.push(K.H('Ground'));
      out.push(C('Centre', 'ground.top'));
      out.push(C('Rim', 'ground.foot'));
      out.push(K.Nt('', 'Under the map: it shows where the map has no land or sea, and while a map is still being made. The map covers it everywhere else.'));
    } else if (step === 'map') {
      var det = B.detailOf(M);
      out.push(K.H('Detail'));
      out.push(K.Ch('Detail', det, opt(Object.keys(B.DETAILS).map(function (k) { return [k, B.DETAILS[k].title]; }).concat(det === 'custom' ? [['custom', 'Your own']] : [])), function (v) {
        if (v === 'custom') return;
        setD(function (o) { var n = clone(o); n.map.off = B.DETAILS[v].off.slice(); n.map.from = clone(B.DETAILS[v].from); return n; });
      }));
      out.push(K.Nt('', det === 'custom' ? 'Your own mix of features, set one by one below.' : B.DETAILS[det].note + '. Each feature has its own switch below.'));
      out.push(K.H('Names'));
      out.push(K.Ch('Names', B.namesOf(M), opt(Object.keys(B.NAMES).map(function (k) { return [k, B.NAMES[k].title]; })), function (v) { upd('map.names', B.NAMES[v].n); }));
      out.push(K.Nt('', 'How many towns and airports are named, the biggest first. Names keep off the place’s dot, off each other and inside the rim.'));
      [['city', 'Town dots'], ['airport', 'Airport marks']].forEach(function (mk) {
        var on = (M.marks || []).indexOf(mk[0]) >= 0;
        out.push(K.Ch(mk[1], on ? 'on' : 'off', opt([['on', 'Shown'], ['off', 'Off']]), function (v) {
          setD(function (o) { var n = clone(o), s = (n.map.marks || []).filter(function (x) { return x !== mk[0]; }); if (v === 'on') s.push(mk[0]); n.map.marks = s; return n; });
        }));
      });
      out.push(K.H('Colour scheme'));
      out.push(K.Ch('Scheme', '', opt(B.STARTERS.map(function (k) { return [k, B.STARTER_TITLES[k]]; })), function (v) { setD(function (o) { return B.scheme(o, v); }); say('The map took ' + B.STARTER_TITLES[v] + '’s colours: change any of them below'); }));
      B.COLOUR_ROLES.forEach(function (r) {
        var off = (M.off || []).indexOf(r) >= 0, line = B.LINE_ROLES.indexOf(r) >= 0, road = /^road/.test(r);
        out.push(K.H(B.ROLE_TITLES[r]));
        if (r !== 'water' && r !== 'land') out.push(K.Ch('Drawn', off ? 'off' : 'on', opt([['on', 'Yes'], ['off', 'No']]), function (v) {
          setD(function (o) { var n = clone(o), s = (n.map.off || []).filter(function (x) { return x !== r; }); if (v === 'off') s.push(r); n.map.off = s; return n; });
        }));
        if (off) return;
        out.push(C('Colour', 'map.' + r));
        if (line) {
          out.push(K.Rg('Width', (M.width && M.width[r]) | 0, 1, 4, 1, function (v) { upd('map.width.' + r, v); }, px((M.width && M.width[r]) | 0)));
          out.push(K.Ch('Line', (M.dash && M.dash[r]) || 'solid', opt([['solid', 'Solid'], ['short', 'Short dashes'], ['long', 'Long dashes'], ['dot', 'Dotted'], ['dashdot', 'Dash-dot']]), function (v) { upd('map.dash.' + r, v); }));
        }
        if (road) out.push(K.Ch('From', String((M.from && M.from[r]) | 0), opt([['0', 'Wide'], ['1', 'Region'], ['2', 'Local'], ['3', 'Area'], ['4', 'City'], ['5', 'Close']]), function (v) { upd('map.from.' + r, +v); }));
      });
      out.push(K.Nt('', 'The preview is your own home map at Region, Area and City (the toolbar), drawn the way the orb draws it. Roads thin out as the view widens: each is drawn from its own zoom in.'));
    } else if (step === 'rain') {
      out.push(K.H('Rain colours'));
      d.rain.forEach(function (s, i) {
        out.push(K.H('From ' + s[0] + ' dBZ'));
        out.push(K.Rg('Strength', s[0], 10, 75, 1, function (v) { var r = clone(d.rain); r[i][0] = v; r.sort(function (a, b) { return a[0] - b[0]; }); upd('rain', r); }, s[0] + ' dBZ'));
        out.push(K.Co('Colour', s[1], function (v) { var r = clone(d.rain); r[i][1] = v; upd('rain', r); }, '', 'rain' + i, { noAuto: true, preset: base.rain[i] ? base.rain[i][1] : undefined }));
        out.push(K.Rg('Opacity', s[2], 0.1, 1, 0.05, function (v) { var r = clone(d.rain); r[i][2] = v; upd('rain', r); }, pct(s[2])));
        if (d.rain.length > 2) out.push(K.Ch('', '', [{ v: 'x', label: 'Remove this colour' }], function () { var r = clone(d.rain); r.splice(i, 1); upd('rain', r); }));
      });
      if (d.rain.length < 10) out.push(K.Ch('', '', [{ v: 'add', label: 'Add a colour' }], function () {
        var r = clone(d.rain), last = r[r.length - 1]; r.push([Math.min(75, last[0] + 5), last[1], last[2]]); upd('rain', r);
      }));
      var Ls = d.rain.map(function (s) { return lum(s[1]); }), close = 0;
      for (var i = 1; i < Ls.length; i++) if (Math.abs(Ls[i] - Ls[i - 1]) < 0.03) close++;
      out.push(K.Nt('', close
        ? close + ' neighbouring colour' + (close === 1 ? ' is' : 's are') + ' close in lightness to the one before: a colour-blind eye, or a dim glass, may not tell them apart. Spread them in lightness, not only hue.'
        : 'Each colour starts at its strength and runs to the next. Light rain is 15-30 dBZ, heavy 40-50, hail and severe storms 55 and up. The app’s Lightest rain setting hides the weakest.'));
    } else if (step === 'overlay') {
      var gl = d.glass, U = d.under || {};
      out.push(K.H('Range rings over the rain'));
      out.push(K.Rg('Rings', gl.rings.length, 0, 4, 1, function (v) { var r = []; for (var k = 1; k <= v; k++) r.push(k / v); upd('glass.rings', r); }, gl.rings.length ? String(gl.rings.length) : 'none'));
      out.push(C('Colour', 'glass.ring'));
      out.push(num('Opacity', 'glass.ringA', 0.05, 1, 0.05, pct));
      out.push(K.Ch('Cross', gl.cross ? 'on' : 'off', opt([['on', 'Drawn'], ['off', 'None']]), function (v) { upd('glass.cross', v === 'on'); }));
      out.push(K.H('Rings under the rain'));
      out.push(K.Rg('Rings', U.rings | 0, 0, 6, 1, function (v) { upd('under.rings', v); if (v && !U.colour) upd('under.colour', gl.ring); }, (U.rings | 0) ? String(U.rings) : 'none'));
      out.push(K.Ch('Cross', U.cross ? 'on' : 'off', opt([['on', 'Drawn'], ['off', 'None']]), function (v) { upd('under.cross', v === 'on'); if (v === 'on' && !U.colour) upd('under.colour', gl.ring); }));
      if ((U.rings | 0) || U.cross) {
        out.push(C('Colour', 'under.colour'));
        out.push(num('Width', 'under.width', 1, 3, 1, px));
      }
      out.push(K.Nt('', 'On the map, under the rain: the weather covers them where it falls. The rings above the rain show through it.'));
      out.push(K.H('Time track'));
      out.push(C('Past', 'glass.track'));
      out.push(C('Forecast', 'glass.forecast'));
      out.push(num('Opacity', 'glass.trackA', 0.1, 1, 0.05, pct));
      out.push(K.Nt('', 'A tick on the rim for each frame, Now at the top, the past anticlockwise and the forecast clockwise; the cursor rides it as you turn the knob.'));
      out.push(K.H('Place dot'));
      out.push(C('Colour', 'dot.colour'));
      out.push(C('Ring', 'dot.ring'));
      out.push(num('Size', 'dot.r', 2, 7, 0.5, px));
      out.push(K.H('Scale bar'));
      out.push(C('Bar', 'scale.ink'));
      out.push(C('Edge', 'scale.edge', { none: true }));
      out.push(num('Thickness', 'scale.width', 2, 8, 1, px));
      out.push(num('End ticks', 'scale.tick', 0, 24, 1, function (v) { return v ? v + ' px' : 'none'; }));
      out.push(num('Longest', 'scale.max', 60, 200, 5, px));
      out.push(K.Nt('', 'A round length (1, 2, 5, 10…) in the orb’s unit, the longest that fits the bar.'));
    } else if (step === 'words') {
      var W = d.words, SW = W.scale || {};
      var faces = B.FACES.map(function (k) { return { v: k, label: k }; });
      out.push(K.H('Place and time'));
      out.push(K.Sl('Face', W.face, faces, function (v) { upd('words.face', v); }));
      out.push(num('Size', 'words.px', 26, 34, 1, px));
      out.push(C('Colour', 'words.ink'));
      out.push(C('Forecast', 'words.forecast'));
      out.push(K.Nt('', 'The place at the top and the frame’s time under it, the time in the forecast colour while it shows the forecast. 26 px is the least the glass reads from two feet.'));
      out.push(K.H('Landmark names'));
      out.push(K.Sl('Face', W.nameFace, faces, function (v) { upd('words.nameFace', v); }));
      out.push(num('Size', 'words.namePx', 26, 32, 1, px));
      out.push(C('Colour', 'words.quiet'));
      out.push(K.H('Scale length'));
      out.push(K.Sl('Face', SW.face || W.nameFace, faces, function (v) { upd('words.scale.face', v); }));
      out.push(num('Size', 'words.scale.px', 26, 32, 1, px));
      out.push(C('Colour', 'words.scale.colour'));
      out.push(K.H('Zoom and status'));
      out.push(num('Zoom size', 'words.zoomPx', 34, 48, 1, px));
      out.push(K.Tx('Backing', W.backing, function (v) { upd('words.backing', v); }));
      out.push(K.Nt('', 'The zoom’s name while you choose it, and the status line, stand on this backing (a CSS colour, e.g. rgba(4,10,14,0.78)) so they read over map and rain.'));
    }
    return out;
  }

  var app = {
    slug: 'radar', title: 'Radar', schema: B.SCHEMA,
    check: checkDesign, complete: B.complete,
    blank: { note: B.STARTER_NOTES.blank, make: function () { return B.preset('blank'); } },
    templates: B.STARTERS.map(function (k) { return { key: k, name: B.STARTER_TITLES[k], note: B.STARTER_NOTES[k], make: function () { return B.preset(k); } }; }),
    picture: function (d, px) { return B.picture(d, px); },
    steps: [['ground', 'Ground'], ['map', 'Map'], ['rain', 'Rain'], ['overlay', 'Overlay'], ['words', 'Words']],
    parts: [
      { id: 'ground', label: 'Ground', icon: '◍' },
      { id: 'map', label: 'Map', icon: '▦', note: function (d) { var k = B.detailOf(d.map); return B.DETAILS[k] ? B.DETAILS[k].title : 'your own'; } },
      { id: 'rain', label: 'Rain', icon: '☂', note: function (d) { return d.rain.length + ' colours'; } },
      { id: 'overlay', label: 'Overlay', icon: '◎', note: function (d) { return d.glass.rings.length + ' rings'; } },
      { id: 'words', label: 'Words', icon: 'A', note: function (d) { return d.words.face; } },
    ],
    fields: fields,
    views: [
      { key: 'place', kind: 'place', label: 'Place', tip: 'Where the preview is: home or one of the orb’s places, as turning the knob steps them' },
      { key: 'zoom', label: 'Zoom', options: ZOOMS.map(function (z) { return [z[0], z[1], 'The map at ' + z[1]]; }) },
      { key: 'time', label: 'Time', options: TIMES },
    ],
    draw: function (x, d, c) {
      drawFrame(x, d, partsOf(d, c), c.hidden, c.view.time, c.view.zoom, c.place, c.unit);
    },
    bake: function (d, o) { return B.bake(d, o); },
    fit: function () { return { rows: LIVE_ROWS, cap: 64 }; },
  };

  root.RadarEditor = { app: app, drawFrame: drawFrame, checkDesign: checkDesign, rolesAt: rolesAt, scaleOf: scaleOf, radiusM: radiusM };
  if (root.OrbHost) root.OrbHost.register(app);
})(typeof window !== 'undefined' ? window : globalThis);
