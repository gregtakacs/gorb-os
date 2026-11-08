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
  var MAPS_CACHE = 'gorb-maps-tiles-v1';         // the Orb tab's tile cache: the same tiles
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

  /* ---- the owner's home map, built on the hub from the Map tiles source ---- */
  async function hubData(path) {
    var r = await fetch(path), j = null;
    try { j = await r.json(); } catch (e) { j = null; }
    if (!r.ok || !j || j.ok === false) throw new Error((j && j.error) || ('the hub answered ' + r.status));
    return j.data;
  }
  async function tile(url) {
    var cache = root.caches ? await root.caches.open(MAPS_CACHE).catch(function () { return null; }) : null;
    var r = cache ? await cache.match(url) : null;
    if (!r) {
      r = await fetch('/api/maps/fetch?u=' + encodeURIComponent(url));
      if (r.status === 204 || r.status === 404) return null;
      if (!r.ok) throw new Error((await r.text()).trim() || 'the tile host answered ' + r.status);
      if (cache) await cache.put(url, r.clone()).catch(function () {});
    }
    var b = new Uint8Array(await r.arrayBuffer());
    if (b.length > 1 && b[0] === 0x1f && b[1] === 0x8b)
      b = new Uint8Array(await new Response(new Blob([b]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());
    return b;
  }
  var tmplP = null;
  function template() {
    if (!tmplP) tmplP = (async function () {
      var src = (await hubData('/api/maps/source')).tilejson;
      var r = await fetch('/api/maps/fetch?u=' + encodeURIComponent(src));
      if (!r.ok) throw new Error('the Map tiles source answered ' + r.status);
      var tj = await r.json();
      if (!tj.tiles || !tj.tiles.length) throw new Error('the Map tiles source names no tiles');
      return { url: tj.tiles[0], maxzoom: tj.maxzoom === undefined ? 14 : +tj.maxzoom };
    })();
    tmplP.catch(function () { tmplP = null; });
    return tmplP;
  }
  // one level's bytes for a place, its tiles four at a time (tabs.js mapsLevel's way)
  async function level(home, z) {
    var GM = root.GorbMaps, tm = await template();
    if (z > tm.maxzoom) throw new Error('the Map tiles source stops at z' + tm.maxzoom);
    var want = GM.tilesFor(home.lat, home.lon, z), tiles = [], next = 0;
    var worker = async function () {
      while (next < want.length) {
        var t = want[next++], b = await tile(tm.url.replace('{z}', t.z).replace('{x}', t.x).replace('{y}', t.y));
        if (b) tiles.push({ x: t.ox, y: t.y, layers: GM.readTile(b) });
      }
    };
    await Promise.all([worker(), worker(), worker(), worker()]);
    return GM.build(tiles, home.lat, home.lon, z).bytes;
  }
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
    var lv = GM.pickLevel(d.__lat || 0, radiusM(step, d.__lat || 0));
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
          var hm = await (await fetch('/orb/' + o.ip + '/api/v1/home')).json();
          if (!hm.home || !hm.home.set) { say('Set home on the Orb tab to preview the map'); return; }
          var hp = { lat: +hm.home.lat, lon: +hm.home.lon, name: hm.home.place || 'Home' };
          setHome(hp);
          try { var un = await (await fetch('/orb/' + o.ip + '/api/v1/units')).json(); if (un && un.dist) setUnit(un.dist); } catch (e) { /* km */ }
          for (var i = 0; i < ZOOMS.length; i++) {
            var st = ZOOMS[i][0];
            say('Building your home map for the preview (' + ZOOMS[i][1] + ')…');
            var b = await level(hp, 6 + st);
            setBytes(function (m) { var n = Object.assign({}, m); n[st] = b; return n; });
          }
          say('');
        } catch (e) { say('The map preview could not be built: ' + (e && e.message ? e.message : e)); }
      })();
    }, []);

    /* the parts, each drawn again when its own part of the design changes */
    var keyMap = JSON.stringify(d.map) + JSON.stringify(Object.keys(mapBytes)) + (home ? home.lat : '');
    R.useEffect(function () {
      var dd = Object.assign({}, d, { __lat: home ? home.lat : 0 }), m = {};
      Object.keys(mapBytes).forEach(function (st) {
        try { m[st] = mapCanvas(dd, mapBytes[st], +st); } catch (e) { say('The map could not be drawn: ' + e.message); }
      });
      setParts(function (p) { return Object.assign({}, p, { map: m }); });
    }, [keyMap]);
    R.useEffect(function () {
      setParts(function (p) { return Object.assign({}, p, { rain: { now: rainCanvas(d, 0), past: rainCanvas(d, 40), next: rainCanvas(d, -30) } }); });
    }, [JSON.stringify(d.rain)]);
    R.useEffect(function () {
      setParts(function (p) { return Object.assign({}, p, { plate: B.plateCanvas(d), glass: B.glassCanvas(d), dot: B.dotCanvas(d) }); });
    }, [JSON.stringify(d.ground) + JSON.stringify(d.glass) + JSON.stringify(d.dot)]);
    // the faces the words draw in, loaded before they draw
    R.useEffect(function () {
      var W = d.words, SW = W.scale || {};
      Promise.all([W.face, W.nameFace, SW.face].filter(Boolean).map(function (f) { return document.fonts.load('26px "' + f + '"', 'Győr 0123'); }))
        .catch(function () {}).then(function () { setLibTick(function (n) { return n + 1; }); });
    }, [d.words.face, d.words.nameFace, (d.words.scale || {}).face]);

    R.useEffect(function () {
      var c = canvasRef.current; if (!c) return;
      drawFrame(c.getContext('2d'), d, parts, hidden, time, zoom, home, unit);
    });

    /* the bake, the send, the download */
    async function bakeNow() {
      var dd = stateRef.current.d;
      try {
        say('Baking “' + dd.name + '”…');
        var doc = new TextEncoder().encode(JSON.stringify(await ses.doc(true)));
        var r = await B.bake(dd, { design: doc });
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
        var im = await B.picture(stateRef.current.d, 192), c = document.createElement('canvas'); c.width = c.height = 96;
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
      var gl = d.glass, U = d.under || {};
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
      fields.push(K.Nt('', 'A tick on the rim for each frame, Now at the top, the past anticlockwise and the forecast clockwise; the cursor rides it as you turn the knob.'));
      fields.push(K.H('Place dot'));
      fields.push(C('Colour', 'dot.colour'));
      fields.push(C('Ring', 'dot.ring'));
      fields.push(num('Size', 'dot.r', 2, 7, 0.5, px));
      fields.push(K.H('Scale bar'));
      fields.push(C('Bar', 'scale.ink'));
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
      B.picture(B.preset(k), 128).then(function (im) { thumbs[k] = im; paint(im); }, function () {});
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

  root.RadarEditor = { Editor: Editor, drawFrame: drawFrame, checkDesign: checkDesign, rolesAt: rolesAt, scaleOf: scaleOf, radiusM: radiusM };
  if (typeof document !== 'undefined' && document.getElementById('root'))
    root.ReactDOM.createRoot(document.getElementById('root')).render(h(Editor));
})(typeof window !== 'undefined' ? window : globalThis);
