/* radar-editor.js — the radar's theme editor (radar-plan §2.9, R7).
 *
 * A small editor on the OS's kit, Flights' editor its model: the shell's frame, top bar,
 * start sheet, glass, toolbar, screen panel, fields, layers panel and Render step
 * (studio@1); the session's library and undo, the sender, the preview's views and the fit
 * (theme@1); the stage bake (baker@1, through radar-bake.js). What is the radar's is here:
 * the design's fields, and the preview, drawn on the owner's own home map. The map is
 * built from the Map tiles source through the hub, as the Orb tab's Maps preview builds it
 * (maps@1), and drawn by the same rasteriser the orb's matches, in the design's colours;
 * the rain over it is made up, coloured through the design's ramp. Plain React,
 * createElement only. */
(function (root) {
  'use strict';
  var R = root.React, h = R.createElement, S = root.OrbStudio, T = root.OrbTheme, B = root.RadarBake;
  var G = 466, CX = 233;
  var STEPS = [['start', 'Start'], ['ground', 'Ground'], ['map', 'Map'], ['rain', 'Rain'], ['overlay', 'Overlay'], ['words', 'Words'], ['render', 'Render']];
  // the preview's zooms: three of the radar's six (apps/radar/main.c ZOOM), its map levels z6+step
  var ZOOMS = [[1, 'Region'], [3, 'Area'], [4, 'City']];
  var TIMES = [['now', 'Now', 'The newest frame'], ['past', '−40 min', 'A past frame'], ['next', '+30 min', 'A forecast frame: the time in the forecast colour']];
  var LIVE_ROWS = 13 + 2;                        // the theme's rows the radar poses, and its two layers

  var clone = function (o) { return JSON.parse(JSON.stringify(o)); };
  var getIn = function (o, path) { return path.split('.').reduce(function (x, k) { return x == null ? x : x[k]; }, o); };
  var setIn = function (d, path, v) {
    var n = clone(d), o = n, ks = path.split('.');
    for (var i = 0; i < ks.length - 1; i++) { if (o[ks[i]] == null) o[ks[i]] = {}; o = o[ks[i]]; }
    o[ks[ks.length - 1]] = v;
    return n;
  };
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

  // one level of the owner's home map, built on the hub from the Map tiles source (maps@1)
  function level(home, z) { return root.GorbMaps.fetchLevel(home.lat, home.lon, z); }
  // the look, drawn as the orb draws it: radar-bake.js frame() (the baked picture's drawing too)
  var MINUTES = { now: 0, past: -40, next: 30 };
  function drawFrame(x, d, P, hidden, time, step, home, unit, sweepDeg) {
    B.frame(x, d, { plate: P.plate, map: P.map && P.map[step], rain: P.rain && P.rain[time], glass: P.glass, dot: P.dot, sweep: P.sweep },
      { minutes: MINUTES[time], place: home ? home.name : 'Home', unit: unit, radius: B.radiusM(step, home ? home.lat : 33),
        sweepDeg: sweepDeg || 0, hidden: hidden });
  }

  function q(name, allowed, dflt) {
    try { var v = new URLSearchParams(root.location.search).get(name); return allowed.indexOf(v) >= 0 ? v : dflt; } catch (e) { return dflt; }
  }
  var col = function (pairs) { return pairs.map(function (p) { return p[0]; }); };

  function Editor() {
    var u = function (v) { return R.useState(v); };
    var dS = u(function () { return B.preset(q('preset', B.STARTERS.concat(['blank']), 'scope')); }), d = dS[0], setD = dS[1];
    var stepS = u(q('step', col(STEPS), 'start')), step = stepS[0], setStep = stepS[1];
    var stS = u(''), status = stS[0], setStatus = stS[1];
    var zS = u(1), zoom = zS[0], setZoom = zS[1];
    var tS = u('now'), time = tS[0], setTime = tS[1];
    var hidS = u({}), hidden = hidS[0], setHidden = hidS[1];
    var sendS = u(null), sending = sendS[0], setSending = sendS[1];
    var dupS = u(null), dup = dupS[0], setDup = dupS[1];
    var libS = u(0), setLibTick = libS[1];
    var delS = u(null), mineDel = delS[0], setMineDel = delS[1];
    var homeS = u(null), home = homeS[0], setHome = homeS[1];
    var unitS = u('km'), unit = unitS[0], setUnit = unitS[1];
    var bytesS = u({}), mapBytes = bytesS[0], setBytes = bytesS[1];
    var partsS = u({}), parts = partsS[0], setParts = partsS[1];
    var drafts = R.useRef({}).current, canvasRef = R.useRef(null), stateRef = R.useRef({});
    var say = R.useCallback(function (s) { setStatus(s); }, []);
    stateRef.current = { d: d, status: status };

    var scrRef = R.useRef(null);
    var vw = R.useMemo(function () {
      return S.viewer({ get: function () { return scrRef.current; }, set: function (p) { setScr(function (o) { return Object.assign({}, o, p); }); }, say: say });
    }, []);
    var scrS = u(function () { return vw.init(q('view', col(T.VIEWS), 'fit')); }), setScr = scrS[1];
    scrRef.current = scrS[0];

    var ses = R.useMemo(function () {
      return T.session({
        slug: 'radar', keys: function () { return ['d']; }, state: function () { return stateRef.current; },
        set: function () { setLibTick(function (n) { return n + 1; }); }, ready: function () { return true; }, say: say,
        restore: function (snap, text) { setD(snap.d); say(text); }, schema: function () { return B.SCHEMA; },
        migrate: function () { return {}; },
        content: async function () { return { design: stateRef.current.d }; },
        apply: function (doc) { var why = checkDesign(doc.design); if (!why) { setD(B.complete(doc.design)); setStep(function (s) { return s === 'start' ? 'map' : s; }); } return why; },
        bake: async function () { var r = await bakeNow(); return r ? r.bytes : null; },
      });
    }, []);
    var sender = R.useMemo(function () {
      return T.sender({ say: say, ask: function (x) { setDup(x); }, sending: function (x) { setSending(x); } });
    }, []);
    R.useEffect(function () { ses.track(); }, [d]);
    R.useEffect(function () { ses.boot().catch(function () {}).then(function () { return ses.openTheme(); }).catch(function () {}); }, []);
    var upd = function (path, v) { setD(function (old) { return setIn(old, path, v); }); };
    var base = R.useMemo(function () { return B.preset(B.STARTERS.indexOf(d.base) >= 0 ? d.base : 'scope'); }, [d.base]);

    /* the owner's home and units, then its map at each preview zoom */
    R.useEffect(function () {
      var o = ses.hubOrb();
      if (!o) { say('Open this editor from the hub to preview it on your own home map'); return; }
      (async function () {
        try {
          var hp = await root.GorbMaps.orbHome(o.ip);
          if (!hp) { say('Set home on the Orb tab to preview the map'); return; }
          setHome(hp);
          setUnit(hp.unit);
          for (const zz of ZOOMS) {                     // const: each update keeps its own level
            const st = zz[0];
            say('Building your home map for the preview (' + zz[1] + ')…');
            const b = await level(hp, 6 + st);
            setBytes(function (m) { var n = Object.assign({}, m); n[st] = b; return n; });
          }
          say('');
        } catch (e) { say('The map preview could not be built: ' + (e && e.message ? e.message : e)); }
      })();
    }, []);

    /* the parts, each drawn again when its own part of the design changes */
    var keyMap = JSON.stringify(d.map) + JSON.stringify(Object.keys(mapBytes)) + (home ? home.lat : '');
    R.useEffect(function () {
      var m = {};
      Object.keys(mapBytes).forEach(function (st) {
        try { m[st] = B.mapCanvas(d, mapBytes[st], +st, home ? home.lat : 0); } catch (e) { say('The map could not be drawn: ' + e.message); }
      });
      setParts(function (p) { return Object.assign({}, p, { map: m }); });
    }, [keyMap]);
    R.useEffect(function () {
      setParts(function (p) { return Object.assign({}, p, { rain: { now: B.rainCanvas(d, 0, false), past: B.rainCanvas(d, 40, false), next: B.rainCanvas(d, -30, true) } }); });
    }, [JSON.stringify(d.rain) + d.forecast]);
    R.useEffect(function () {
      setParts(function (p) { return Object.assign({}, p, { plate: B.plateCanvas(d), glass: B.glassCanvas(d), dot: B.dotCanvas(d),
        sweep: d.sweep && d.sweep.on ? B.sweepCanvas(d) : null }); });
    }, [JSON.stringify(d.ground) + JSON.stringify(d.glass) + JSON.stringify(d.dot) + JSON.stringify(d.sweep) + JSON.stringify(d.layout)]);
    // the sweep turns in the preview as on the orb
    var swS = u(0), sweepDeg = swS[0], setSweepDeg = swS[1];
    R.useEffect(function () {
      if (!d.sweep || !d.sweep.on) return;
      var per = Math.max(2, d.sweep.period | 0) * 1000, t = setInterval(function () { setSweepDeg((Date.now() % per) * 360 / per); }, 50);
      return function () { clearInterval(t); };
    }, [d.sweep && d.sweep.on, d.sweep && d.sweep.period]);
    // the faces the words draw in, loaded before they draw
    R.useEffect(function () {
      var W = d.words, SW = W.scale || {};
      Promise.all([W.face, W.nameFace, SW.face].filter(Boolean).map(function (f) { return document.fonts.load('26px "' + f + '"', 'Győr 0123'); }))
        .catch(function () {}).then(function () { setLibTick(function (n) { return n + 1; }); });
    }, [d.words.face, d.words.nameFace, (d.words.scale || {}).face]);

    R.useEffect(function () {
      var c = canvasRef.current; if (!c) return;
      drawFrame(c.getContext('2d'), d, parts, hidden, time, zoom, home, unit, sweepDeg);
    });

    // the baked picture over the owner's own map at Region, when the editor has it
    function pictureOpts() {
      return home && mapBytes[1] ? { map: { bytes: mapBytes[1], lat: home.lat, step: 1 }, place: home.name, unit: unit } : { unit: unit };
    }
    /* the bake, the send, the download */
    async function bakeNow() {
      var dd = stateRef.current.d;
      try {
        say('Baking “' + dd.name + '”…');
        var doc = new TextEncoder().encode(JSON.stringify(await ses.doc(true)));
        var r = await B.bake(dd, { design: doc, picture: pictureOpts() });
        say('Baked “' + dd.name + '”: ' + Math.round(r.bytes.length / 1024) + ' KB');
        return r;
      } catch (e) { say('Not baked: ' + (e && e.message ? e.message : e)); return null; }
    }
    async function stemOf(nm) {
      try {
        var dg = new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode('radar/' + nm)));
        return root.GorbBake.cardStem('radar', nm, dg);
      } catch (e) { return null; }
    }
    async function draftThumb() {
      try {
        var im = await B.picture(stateRef.current.d, 192, pictureOpts()), c = document.createElement('canvas'); c.width = c.height = 96;
        var x = c.getContext('2d'); x.fillStyle = '#000'; x.fillRect(0, 0, 96, 96); x.drawImage(im, 0, 0, 96, 96);
        return c.toDataURL('image/jpeg', 0.82);
      } catch (e) { return null; }
    }
    function send() {
      var nm = d.name;
      sender.send(nm, async function () {
        await ses.saveDraft({ name: nm, doc: await ses.doc(true), thumb: await draftThumb() }, true);
        var r = await bakeNow(); if (!r || sender.cancelled()) return;
        await sender.post(r.bytes, { name: nm, stem: await stemOf(nm), summary: Math.round(r.bytes.length / 1024) + ' KB' });
        ses.libraryRefresh();
      });
    }
    async function download() {
      var r = await bakeNow(); if (!r) return;
      T.download(new Blob([r.bytes], { type: 'application/octet-stream' }), ((await stemOf(d.name)) || 'radar-theme') + '.orbtheme');
    }
    async function saveDraft() { await ses.saveDraft({ name: d.name, doc: await ses.doc(true), thumb: await draftThumb() }); }

    var verdict = T.fit({ rows: LIVE_ROWS, cap: 64 });

    /* ---- the fields of each step ---- */
    var K = S.fieldKit({ drafts: drafts, refresh: function () { setLibTick(function (n) { return n + 1; }); }, scope: function () { return step; } });
    var opt = function (pairs) { return pairs.map(function (p) { return { v: p[0], label: p[1] }; }); };
    var C = function (label, path, X) {
      var v = getIn(d, path);
      return K.Co(label, v == null ? 'none' : v, function (nv) { upd(path, nv === 'none' ? null : nv); }, '', path,
        Object.assign({ noAuto: true, preset: getIn(base, path) }, X || {}));
    };
    var num = function (label, path, min, max, stp, fmt) { var v = +getIn(d, path); return K.Rg(label, v, min, max, stp, function (nv) { upd(path, nv); }, fmt ? fmt(v) : String(v)); };
    var px = function (v) { return v + ' px'; };
    var pct = function (v) { return Math.round(v * 100) + '%'; };
    var fields = [], M = d.map;
    if (step === 'ground') {
      fields.push(K.H('Ground'));
      fields.push(C('Centre', 'ground.top'));
      fields.push(C('Rim', 'ground.foot'));
      fields.push(K.Nt('', 'Under the map: it shows where the map has no land or sea, and while a map is still being made. The map covers it everywhere else.'));
    } else if (step === 'map') {
      var det = B.detailOf(M);
      fields.push(K.H('Detail'));
      fields.push(K.Ch('Detail', det, opt(Object.keys(B.DETAILS).map(function (k) { return [k, B.DETAILS[k].title]; }).concat(det === 'custom' ? [['custom', 'Your own']] : [])), function (v) {
        if (v === 'custom') return;
        setD(function (o) { var n = clone(o); n.map.off = B.DETAILS[v].off.slice(); n.map.from = clone(B.DETAILS[v].from); return n; });
      }));
      fields.push(K.Nt('', det === 'custom' ? 'Your own mix of features, set one by one below.' : B.DETAILS[det].note + '. Each feature has its own switch below.'));
      fields.push(K.H('Names'));
      fields.push(K.Ch('Names', B.namesOf(M), opt(Object.keys(B.NAMES).map(function (k) { return [k, B.NAMES[k].title]; })), function (v) { upd('map.names', B.NAMES[v].n); }));
      fields.push(K.Nt('', 'How many towns and airports are named, the biggest first. Names keep off the place’s dot, off each other and inside the rim.'));
      [['city', 'Town dots'], ['airport', 'Airport marks']].forEach(function (mk) {
        var on = (M.marks || []).indexOf(mk[0]) >= 0;
        fields.push(K.Ch(mk[1], on ? 'on' : 'off', opt([['on', 'Shown'], ['off', 'Off']]), function (v) {
          setD(function (o) { var n = clone(o), s = (n.map.marks || []).filter(function (x) { return x !== mk[0]; }); if (v === 'on') s.push(mk[0]); n.map.marks = s; return n; });
        }));
      });
      fields.push(K.H('Colour scheme'));
      fields.push(K.Ch('Scheme', '', opt(B.STARTERS.map(function (k) { return [k, B.STARTER_TITLES[k]]; })), function (v) { setD(function (o) { return B.scheme(o, v); }); say('The map took ' + B.STARTER_TITLES[v] + '’s colours: change any of them below'); }));
      B.COLOUR_ROLES.forEach(function (r) {
        var off = (M.off || []).indexOf(r) >= 0, line = B.LINE_ROLES.indexOf(r) >= 0, road = /^road/.test(r);
        fields.push(K.H(B.ROLE_TITLES[r]));
        if (r !== 'water' && r !== 'land') fields.push(K.Ch('Drawn', off ? 'off' : 'on', opt([['on', 'Yes'], ['off', 'No']]), function (v) {
          setD(function (o) { var n = clone(o), s = (n.map.off || []).filter(function (x) { return x !== r; }); if (v === 'off') s.push(r); n.map.off = s; return n; });
        }));
        if (off) return;
        fields.push(C('Colour', 'map.' + r));
        if (line) {
          fields.push(K.Rg('Width', (M.width && M.width[r]) | 0, 1, 4, 1, function (v) { upd('map.width.' + r, v); }, px((M.width && M.width[r]) | 0)));
          fields.push(K.Ch('Line', (M.dash && M.dash[r]) || 'solid', opt([['solid', 'Solid'], ['short', 'Short dashes'], ['long', 'Long dashes'], ['dot', 'Dotted'], ['dashdot', 'Dash-dot']]), function (v) { upd('map.dash.' + r, v); }));
        }
        if (road) fields.push(K.Ch('From', String((M.from && M.from[r]) | 0), opt([['0', 'Wide'], ['1', 'Region'], ['2', 'Local'], ['3', 'Area'], ['4', 'City'], ['5', 'Close']]), function (v) { upd('map.from.' + r, +v); }));
      });
      fields.push(K.Nt('', 'The preview is your own home map at Region, Area and City (the toolbar), drawn the way the orb draws it. Roads thin out as the view widens: each is drawn from its own zoom in.'));
    } else if (step === 'rain') {
      fields.push(K.H('Rain colours'));
      d.rain.forEach(function (s, i) {
        fields.push(K.H((i ? 'From ' : 'From ') + s[0] + ' dBZ'));
        fields.push(K.Rg('Strength', s[0], 10, 75, 1, function (v) { var r = clone(d.rain); r[i][0] = v; r.sort(function (a, b) { return a[0] - b[0]; }); upd('rain', r); }, s[0] + ' dBZ'));
        fields.push(K.Co('Colour', s[1], function (v) { var r = clone(d.rain); r[i][1] = v; upd('rain', r); }, '', 'rain' + i, { noAuto: true, preset: base.rain[i] ? base.rain[i][1] : undefined }));
        fields.push(K.Rg('Opacity', s[2], 0.1, 1, 0.05, function (v) { var r = clone(d.rain); r[i][2] = v; upd('rain', r); }, pct(s[2])));
        if (d.rain.length > 2) fields.push(K.Ch('', '', [{ v: 'x', label: 'Remove this colour' }], function () { var r = clone(d.rain); r.splice(i, 1); upd('rain', r); }));
      });
      if (d.rain.length < 10) fields.push(K.Ch('', '', [{ v: 'add', label: 'Add a colour' }], function () {
        var r = clone(d.rain), last = r[r.length - 1]; r.push([Math.min(75, last[0] + 5), last[1], last[2]]); upd('rain', r);
      }));
      var Ls = d.rain.map(function (s) { return lum(s[1]); }), close = 0;
      for (var i = 1; i < Ls.length; i++) if (Math.abs(Ls[i] - Ls[i - 1]) < 0.03) close++;
      fields.push(K.Nt('', close
        ? close + ' neighbouring colour' + (close === 1 ? ' is' : 's are') + ' close in lightness to the one before: a colour-blind eye, or a dim glass, may not tell them apart. Spread them in lightness, not only hue.'
        : 'Each colour starts at its strength and runs to the next. Light rain is 15-30 dBZ, heavy 40-50, hail and severe storms 55 and up. The app’s Lightest rain setting hides the weakest.'));
    } else if (step === 'overlay') {
      var gl = d.glass, U = d.under || {}, Ly = d.layout || {};
      fields.push(K.H('Layout'));
      fields.push(K.Ch('Time track', Ly.track || 'round', opt([['bottom', 'On the bottom rim'], ['round', 'Round the rim']]), function (v) {
        setD(function (o) { var n = clone(o); n.layout = Object.assign({}, o.layout, v === 'bottom' ? { track: v, timeY: 346, statusY: 392, placeY: 52 } : { track: v, timeY: 98, statusY: 362, placeY: 58 }); return n; });
      }));
      fields.push(K.Ch('Scale', Ly.scale || 'bar', opt([['ring', 'A ring at a round distance'], ['bar', 'A bar']]), function (v) { upd('layout.scale', v); }));
      fields.push(K.Ch('Place', Ly.caps ? 'caps' : 'as', opt([['caps', 'In capitals'], ['as', 'As written']]), function (v) { upd('layout.caps', v === 'caps'); }));
      fields.push(K.Nt('', 'On the bottom rim (the approved look): the time sits low over a track of the frames, past to the left of Now, forecast to the right. The ring is drawn at the scale’s round length and labelled on it.'));
      fields.push(K.H('Forecast'));
      fields.push(K.Ch('Marked', d.forecast || 'none', opt(B.FORECAST_TITLES), function (v) { upd('forecast', v); }));
      fields.push(K.Nt('', 'How the next hour’s frames are drawn, so a forecast never looks observed. The toolbar’s +30 min shows it.'));
      var Sw = d.sweep || { on: false };
      fields.push(K.H('Sweep'));
      fields.push(K.Ch('Sweep', Sw.on ? 'on' : 'off', opt([['on', 'Turning'], ['off', 'None']]), function (v) { upd('sweep.on', v === 'on'); }));
      if (Sw.on) {
        fields.push(C('Colour', 'sweep.colour'));
        fields.push(num('Trail', 'sweep.tail', 0, 90, 5, function (v) { return v + '°'; }));
        fields.push(num('One turn', 'sweep.period', 2, 30, 1, function (v) { return v + ' s'; }));
        fields.push(K.Nt('', 'A line turning round the glass with a fading trail: Scope’s signature. It is a moving row, so it costs a little each frame.'));
      }
      fields.push(K.H('Range rings over the rain'));
      fields.push(K.Rg('Rings', gl.rings.length, 0, 4, 1, function (v) { var r = []; for (var k = 1; k <= v; k++) r.push(k / v); upd('glass.rings', r); }, gl.rings.length ? String(gl.rings.length) : 'none'));
      fields.push(C('Colour', 'glass.ring'));
      fields.push(num('Opacity', 'glass.ringA', 0.05, 1, 0.05, pct));
      fields.push(K.Ch('Cross', gl.cross ? 'on' : 'off', opt([['on', 'Drawn'], ['off', 'None']]), function (v) { upd('glass.cross', v === 'on'); }));
      fields.push(K.H('Rings under the rain'));
      fields.push(K.Rg('Rings', U.rings | 0, 0, 6, 1, function (v) { upd('under.rings', v); if (v && !U.colour) upd('under.colour', gl.ring); }, (U.rings | 0) ? String(U.rings) : 'none'));
      fields.push(K.Ch('Cross', U.cross ? 'on' : 'off', opt([['on', 'Drawn'], ['off', 'None']]), function (v) { upd('under.cross', v === 'on'); if (v === 'on' && !U.colour) upd('under.colour', gl.ring); }));
      if ((U.rings | 0) || U.cross) {
        fields.push(C('Colour', 'under.colour'));
        fields.push(num('Width', 'under.width', 1, 3, 1, px));
      }
      fields.push(K.Nt('', 'On the map, under the rain: the weather covers them where it falls. The rings above the rain show through it.'));
      fields.push(K.H('Time track'));
      fields.push(C('Past', 'glass.track'));
      fields.push(C('Forecast', 'glass.forecast'));
      fields.push(num('Opacity', 'glass.trackA', 0.1, 1, 0.05, pct));
      fields.push(K.Nt('', (Ly.track === 'bottom' ? 'A tick on the bottom rim for each frame, Now at the bottom, the past to its left and the forecast to its right' : 'A tick on the rim for each frame, Now at the top, the past anticlockwise and the forecast clockwise') + '; the cursor rides it as you turn the knob.'));
      fields.push(K.H('Place dot'));
      fields.push(C('Colour', 'dot.colour'));
      fields.push(C('Ring', 'dot.ring'));
      fields.push(num('Size', 'dot.r', 2, 7, 0.5, px));
      fields.push(K.H(Ly.scale === 'ring' ? 'Scale' : 'Scale bar'));
      if (Ly.scale !== 'ring') fields.push(C('Bar', 'scale.ink'));
      fields.push(C('Edge', 'scale.edge', { none: true }));
      fields.push(num('Thickness', 'scale.width', 2, 8, 1, px));
      fields.push(num('End ticks', 'scale.tick', 0, 24, 1, function (v) { return v ? v + ' px' : 'none'; }));
      fields.push(num('Longest', 'scale.max', 60, 200, 5, px));
      fields.push(K.Nt('', 'A round length (1, 2, 5, 10…) in the orb’s unit, the longest that fits the bar.'));
    } else if (step === 'words') {
      var W = d.words, SW = W.scale || {};
      var faces = B.FACES.map(function (k) { return { v: k, label: k }; });
      fields.push(K.H('Place and time'));
      fields.push(K.Sl('Face', W.face, faces, function (v) { upd('words.face', v); }));
      fields.push(num('Size', 'words.px', 26, 34, 1, px));
      fields.push(C('Colour', 'words.ink'));
      fields.push(C('Forecast', 'words.forecast'));
      fields.push(K.Nt('', 'The place at the top and the frame’s time under it, the time in the forecast colour while it shows the forecast. 26 px is the least the glass reads from two feet.'));
      fields.push(K.H('Landmark names'));
      fields.push(K.Sl('Face', W.nameFace, faces, function (v) { upd('words.nameFace', v); }));
      fields.push(num('Size', 'words.namePx', 26, 32, 1, px));
      fields.push(C('Colour', 'words.quiet'));
      fields.push(K.H('Scale length'));
      fields.push(K.Sl('Face', SW.face || W.nameFace, faces, function (v) { upd('words.scale.face', v); }));
      fields.push(num('Size', 'words.scale.px', 26, 32, 1, px));
      fields.push(C('Colour', 'words.scale.colour'));
      fields.push(K.H('Zoom and status'));
      fields.push(num('Zoom size', 'words.zoomPx', 34, 48, 1, px));
      fields.push(K.Tx('Backing', W.backing, function (v) { upd('words.backing', v); }));
      fields.push(K.Nt('', 'The zoom’s name while you choose it, and the status line, stand on this backing (a CSS colour, e.g. rgba(4,10,14,0.78)) so they read over map and rain.'));
    }

    /* ---- the left pane: the parts ---- */
    var items = [['ground', 'Ground'], ['map', 'Map'], ['rain', 'Rain'], ['overlay', 'Overlay'], ['words', 'Words']].map(function (x) {
      return { id: x[0], band: 'parts', label: x[1], movable: false };
    });
    var left = h(S.Layers, { title: 'Parts', rows: S.layerRows(items, {
      sel: step, heads: { parts: 'The radar, bottom to top' },
      icon: function (it) { return { ground: '◍', map: '▦', rain: '☂', overlay: '◎', words: 'A' }[it.id]; },
      note: function (it) {
        return { ground: '', map: B.DETAILS[B.detailOf(M)] ? B.DETAILS[B.detailOf(M)].title : 'your own', rain: d.rain.length + ' colours',
                 overlay: d.glass.rings.length + ' rings', words: d.words.face }[it.id];
      },
      hidden: function (it) { return !!hidden[it.id]; }, move: function () { return { ok: false, reason: 'the radar’s parts keep their order' }; },
      onMove: function () {}, pick: function (it) { setStep(it.id); },
      eyeTip: function (it) { return hidden[it.id] ? 'Show it in the preview' : 'Hide it in the preview (the bake keeps it)'; },
      toggleEye: function (it) { var n = Object.assign({}, hidden); n[it.id] = !n[it.id]; setHidden(n); },
      drag: { start: function () {}, over: function () {}, drop: function () {}, end: function () {} },
    }) });

    /* ---- the right pane ---- */
    var right = step === 'render'
      ? h(R.Fragment, null, h('div', { className: 'rd-sec' }, 'Render'),
          h('div', { className: 'rd-list' }, h(S.RenderPanel, {
            badge: S.badgeProps(verdict, 'The rows the radar poses against the 64 a stage seats'),
            send: { on: send, note: 'save “' + d.name + '”, bake it and store it on the orb', tip: 'Save it to the hub library, bake it, then send it to the orb this editor was opened for.' },
            save: ses.hubOrb() ? { on: saveDraft, note: 'keep “' + d.name + '” on the hub as a draft', tip: 'Keep this design in the hub library under its name, without baking or sending it.' } : null,
            downloads: [{ label: 'Download .orbtheme', note: 'the baked file', on: download }, { label: 'Check the bake', note: 'bake now and check its size', on: bakeNow }],
          })))
      : step === 'start'
        ? h('div', { className: 'rd-list' }, h('div', { className: 'ui-mut' }, 'Pick a starting point, or open one from the hub library, then Continue. The steps after it: Ground, Map, Rain, Overlay, Words, and Render to send it.'))
        : h('div', { className: 'rd-list' }, h(S.Fields, { fields: fields }));

    /* ---- the start sheet ---- */
    var thumbs = R.useRef({}).current;
    var drawThumb = function (k, c) {
      if (!c) return;
      var paint = function (im) { var x = c.getContext('2d'); x.clearRect(0, 0, c.width, c.height); x.drawImage(im, 0, 0, c.width, c.height); };
      if (thumbs[k]) { paint(thumbs[k]); return; }
      B.picture(B.preset(k), 128, pictureOpts()).then(function (im) { thumbs[k] = im; paint(im); }, function () {});
    };
    var sheet = step === 'start' ? h(S.StartSheet, {
      title: 'Start a radar theme',
      scratch: { label: 'Blank', short: B.STARTER_NOTES.blank, cls: d.base === 'blank' ? 'on' : '',
        tip: 'Every part present, in plain greys: a blank slate to build your own look on',
        on: function () { var n = B.preset('blank'); n.base = 'blank'; setD(n); ses.restart(); say('Starting from a blank theme: Continue when you are ready'); } },
      tiles: B.STARTERS.map(function (k) {
        return { key: k, name: B.STARTER_TITLES[k], short: B.STARTER_NOTES[k], tip: 'Start from ' + B.STARTER_TITLES[k] + ': ' + B.STARTER_NOTES[k],
          cls: d.base === k ? 'on' : '', draw: function (c) { drawThumb(k, c); },
          on: function () { var n = B.preset(k); n.base = k; setD(n); ses.restart(); say('Starting from ' + B.STARTER_TITLES[k] + ': Continue when you are ready'); } };
      }),
      library: { accept: '.orbtheme,.orbdesign,.json',
        openFile: function (e) { var fl = e.target.files[0]; if (fl) ses.openFile(fl); },
        rows: S.libraryRows(ses.library(), { version: ses.version(), armed: mineDel, arm: setMineDel,
          meta: function (m) { return m.design && B.STARTER_TITLES[m.design.base] ? B.STARTER_TITLES[m.design.base] + ' · ' : ''; },
          open: function (nm) { ses.openEntry(nm); }, forget: function (nm, what) { ses.forgetEntry(nm, what); } }),
        empty: ses.hubOrb() ? null : 'The library is on the hub: open this editor from the hub to save and open designs there.' },
      foot: [{ label: 'Continue with ' + d.name, primary: true, on: function () { setStep('ground'); } }],
    }) : null;

    /* ---- the frame ---- */
    var g = vw.glass(), tool = vw.tool();
    return h(S.Frame, null,
      h(S.TopBar, {
        back: function () { try { root.parent.postMessage({ type: 'theme.close' }, '*'); } catch (e) { /* not framed */ } },
        backTip: 'Back to the Themes page', crumb: 'Radar',
        name: { value: d.name, placeholder: 'Theme name', title: 'The theme’s name on the orb', onChange: function (e) { upd('name', e.target.value); } },
        steps: STEPS.map(function (s) { return { label: s[1], cls: s[0] === step ? 'on' : '', pick: function () { setStep(s[0]); } }; }),
        undo: { on: function () { ses.undo(); }, tip: 'Undo', op: ses.undoStack.length ? 1 : 0.35 },
        redo: { on: function () { ses.redo(); }, tip: 'Redo', op: ses.redoStack.length ? 1 : 0.35 },
        fit: S.fitProps(verdict, verdict.note), primary: { label: 'Send to orb', on: send, title: 'Bake and send this theme' },
      }),
      h(S.Pane, { side: 'left' }, left),
      h(S.Stage, null,
        h(S.Glass, Object.assign({ status: status }, g),
          h('canvas', { ref: canvasRef, width: G, height: G, style: { width: g.size + 'px', height: g.size + 'px' } })),
        sheet,
        h(S.ScreenPanel, vw.panel()),
        h(S.Toolbar, { view: tool.view, zoom: tool.zoom, screen: vw.toolScreen() },
          h(S.Seg, { opts: ZOOMS.map(function (z) { return { label: z[1], tip: 'The map at ' + z[1], cls: z[0] === zoom ? 'on' : '', pick: function () { setZoom(z[0]); } }; }) }),
          h(S.Seg, { opts: TIMES.map(function (t) { return { label: t[1], tip: t[2], cls: t[0] === time ? 'on' : '', pick: function () { setTime(t[0]); } }; }) }))),
      h(S.Pane, { side: 'right' }, right),
      h(S.OpeningModal, { line: ses.opening }),
      h(S.SendModal, S.sendProps(sending, status, function () { sender.cancel(); })),
      h(S.DupModal, S.dupProps(dup, function () { sender.replace(); }, function () { sender.dismiss(); })));
  }

  root.RadarEditor = { Editor: Editor, drawFrame: drawFrame, checkDesign: checkDesign };
  if (typeof document !== 'undefined' && document.getElementById('root'))
    root.ReactDOM.createRoot(document.getElementById('root')).render(h(Editor));
})(typeof window !== 'undefined' ? window : globalThis);
