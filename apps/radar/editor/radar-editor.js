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
  // the look, drawn as the orb draws it: radar-bake.js frame() (the baked picture's drawing too)
  var MINUTES = { now: 0, past: -40, next: 30 };
  function drawFrame(x, d, P, hidden, time, step, home, unit, sweepDeg) {
    B.frame(x, d, { plate: P.plate, map: P.map && P.map[step], rain: P.rain && P.rain[time], glass: P.glass, dot: P.dot, sweep: P.sweep },
      { minutes: MINUTES[time], place: home ? home.name : 'Home', unit: unit, radius: B.radiusM(step, home ? home.lat : 33),
        sweepDeg: sweepDeg || 0, hidden: hidden });
  }

  // home's map at Region, once the editor has read it: the baked picture and the start's
  // pictures are drawn over it (the made-up place stands in until then)
  var seen = { home: null, bytes: null, unit: 'km' };
  function pictureOpts() {
    return seen.home && seen.bytes ? { map: { bytes: seen.bytes, lat: seen.home.lat, step: 1 }, place: seen.home.name, unit: seen.unit } : { unit: seen.unit };
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
        P.map[st] = c.part('map' + st, JSON.stringify(d.map) + '|' + lk, function () { return B.mapCanvas(d, bytes, st, pl.lat); });
        if (st === 1 && c.home && pl.key === c.home.key) { seen.home = c.home; seen.bytes = bytes; }
      }
    } else if (c.placesRead && !pl && !P.homeless) { P.homeless = true; c.say('Set home on the Orb tab to preview the map'); }
    var kRain = JSON.stringify(d.rain) + d.forecast;
    if (P.kRain !== kRain) { P.kRain = kRain; P.rain = { now: B.rainCanvas(d, 0, false), past: B.rainCanvas(d, 40, false), next: B.rainCanvas(d, -30, true) }; }
    var kArt = JSON.stringify(d.ground) + JSON.stringify(d.glass) + JSON.stringify(d.dot) + JSON.stringify(d.sweep) + JSON.stringify(d.layout);
    if (P.kArt !== kArt) {
      P.kArt = kArt; P.plate = B.plateCanvas(d); P.glass = B.glassCanvas(d); P.dot = B.dotCanvas(d);
      P.sweep = d.sweep && d.sweep.on ? B.sweepCanvas(d) : null;
    }
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
      out.push(K.H("The OS's overlay"));
      out.push(K.Co("The OS's overlay", B.overlayOf(d), function (nv) { upd('overlay', nv); }, '', 'overlay',
        { noAuto: true, preset: (B.preset(d.base).glass || {}).ring }));
      out.push(K.Nt('', 'The colour the orb’s own overlays — the ring, the battery, the notices — are drawn in over this look. It starts as the glass ring’s colour; set it to something else to make the OS’s marks stand apart.'));
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
      var gl = d.glass, U = d.under || {}, Ly = d.layout || {};
      out.push(K.H('Layout'));
      out.push(K.Ch('Time track', Ly.track || 'round', opt([['bottom', 'On the bottom rim'], ['round', 'Round the rim']]), function (v) {
        setD(function (o) { var n = clone(o); n.layout = Object.assign({}, o.layout, v === 'bottom' ? { track: v, timeY: 346, statusY: 392, placeY: 52 } : { track: v, timeY: 98, statusY: 362, placeY: 58 }); return n; });
      }));
      out.push(K.Ch('Scale', Ly.scale || 'bar', opt([['ring', 'A ring at a round distance'], ['bar', 'A bar']]), function (v) { upd('layout.scale', v); }));
      out.push(K.Ch('Place', Ly.caps ? 'caps' : 'as', opt([['caps', 'In capitals'], ['as', 'As written']]), function (v) { upd('layout.caps', v === 'caps'); }));
      out.push(K.Nt('', 'On the bottom rim (the approved look): the time sits low over a track of the frames, past to the left of Now, forecast to the right. The ring is drawn at the scale’s round length and labelled on it.'));
      out.push(K.H('Forecast'));
      out.push(K.Ch('Marked', d.forecast || 'none', opt(B.FORECAST_TITLES), function (v) { upd('forecast', v); }));
      out.push(K.Nt('', 'How the next hour’s frames are drawn, so a forecast never looks observed. The toolbar’s +30 min shows it.'));
      var Sw = d.sweep || { on: false };
      out.push(K.H('Sweep'));
      out.push(K.Ch('Sweep', Sw.on ? 'on' : 'off', opt([['on', 'Turning'], ['off', 'None']]), function (v) { upd('sweep.on', v === 'on'); }));
      if (Sw.on) {
        out.push(C('Colour', 'sweep.colour'));
        out.push(num('Trail', 'sweep.tail', 0, 90, 5, function (v) { return v + '°'; }));
        out.push(num('One turn', 'sweep.period', 2, 30, 1, function (v) { return v + ' s'; }));
        out.push(K.Nt('', 'A line turning round the glass with a fading trail: Scope’s signature. It is a moving row, so it costs a little each frame.'));
      }
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
      out.push(K.Nt('', (Ly.track === 'bottom' ? 'A tick on the bottom rim for each frame, Now at the bottom, the past to its left and the forecast to its right' : 'A tick on the rim for each frame, Now at the top, the past anticlockwise and the forecast clockwise') + '; the cursor rides it as you turn the knob.'));
      out.push(K.H('Place dot'));
      out.push(C('Colour', 'dot.colour'));
      out.push(C('Ring', 'dot.ring'));
      out.push(num('Size', 'dot.r', 2, 7, 0.5, px));
      out.push(K.H(Ly.scale === 'ring' ? 'Scale' : 'Scale bar'));
      if (Ly.scale !== 'ring') out.push(C('Bar', 'scale.ink'));
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
      out.push(C('Backing', 'words.backing'));
      out.push(num('Opacity', 'words.backingA', 0, 1, 0.02, pct));
      out.push(K.Nt('', 'The zoom’s name while you choose it, and the status line, stand on this backing so they read over the map and the rain.'));
    }
    return out;
  }

  var app = {
    slug: 'radar', title: 'Radar', schema: B.SCHEMA,
    check: checkDesign, complete: B.complete,
    blank: { note: B.STARTER_NOTES.blank, make: function () { return B.preset('blank'); } },
    templates: B.STARTERS.map(function (k) { return { key: k, name: B.STARTER_TITLES[k], note: B.STARTER_NOTES[k], make: function () { return B.preset(k); } }; }),
    picture: function (d, px) { return B.picture(d, px, pictureOpts()); },
    // the start's pictures drawn again once home's map has come
    pictureKey: function () { return seen.home && seen.bytes ? 'home' : 'sample'; },
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
    // the sweep turns as on the orb, while the design has one
    play: { fps: 20, on: function (v, d) { return !!(d && d.sweep && d.sweep.on); }, what: 'the sweep', idle: 'Nothing turns: this design has no sweep' },
    draw: function (x, d, c) {
      seen.unit = c.unit;
      var per = Math.max(2, (d.sweep && d.sweep.period) | 0) * 1000;
      drawFrame(x, d, partsOf(d, c), c.hidden, c.view.time, c.view.zoom, c.place, c.unit, d.sweep && d.sweep.on ? (c.t % per) * 360 / per : 0);
    },
    bake: function (d, o) { return B.bake(d, Object.assign({}, o, { picture: pictureOpts() })); },
    fit: function () { return { rows: LIVE_ROWS, cap: 64 }; },
  };

  root.RadarEditor = { app: app, drawFrame: drawFrame, checkDesign: checkDesign };
  if (root.OrbHost) root.OrbHost.register(app);
})(typeof window !== 'undefined' ? window : globalThis);
