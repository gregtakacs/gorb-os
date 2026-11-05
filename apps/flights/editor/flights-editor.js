/* flights-editor.js — the flight tracker's theme editor (flight-tracker-design §12, §12.5).
 *
 * A small editor on the OS's kit (theme-editor-kit-plan): the shell's frame, top bar, start
 * sheet, glass, toolbar, screen panel, fields, layers panel and Render step (studio@1); the
 * session's library and undo, the sender, the preview's views and the fit verdict
 * (theme@1); the stage bake (baker@1, through flights-bake.js). What is the tracker's is
 * here: the design and its fields, the parts list, and the preview's simulated traffic under
 * the turning arm. Plain React, createElement only. */
(function (root) {
  'use strict';
  var R = root.React, h = R.createElement, S = root.OrbStudio, T = root.OrbTheme, B = root.FlightsBake;
  var A = function () { return root.FlightsArt; };
  var G = 466, CX = 233, LIVE_ROWS = 1 + 15 + 2 + 4, ROW_CAP = 32, CARD_MARGIN = 20;
  var STEPS = [['start', 'Start'], ['scope', 'Scope'], ['arm', 'Arm'], ['planes', 'Planes'], ['words', 'Words'], ['frames', 'Frames'], ['render', 'Render']];
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
  var getIn = function (o, path) { return path.split('.').reduce(function (x, k) { return x == null ? x : x[k]; }, o); };
  var setIn = function (d, path, v) {
    var n = clone(d), o = n, ks = path.split('.');
    for (var i = 0; i < ks.length - 1; i++) o = o[ks[i]];
    o[ks[ks.length - 1]] = v;
    return n;
  };
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
  // a picture read from a file, at most `max` px on its longer side (an SVG kept as drawn)
  function readPicture(f, max) {
    return new Promise(function (ok, bad) {
      var rd = new FileReader();
      rd.onerror = function () { bad(new Error('the file could not be read')); };
      rd.onload = function () {
        if (/svg/.test(f.type)) { ok(rd.result); return; }
        B.image(rd.result).then(function (im) {
          var k = Math.min(1, max / Math.max(im.width, im.height)), c = document.createElement('canvas');
          c.width = Math.max(1, Math.round(im.width * k)); c.height = Math.max(1, Math.round(im.height * k));
          c.getContext('2d').drawImage(im, 0, 0, c.width, c.height);
          ok(c.toDataURL('image/png'));
        }, bad);
      };
      rd.readAsDataURL(f);
    });
  }

  /* ---- the preview's parts, each drawn again only when its own part of the design changes ---- */
  function usePart(key, make, say) {
    var st = R.useState(null), gen = R.useRef(0);
    R.useEffect(function () {
      var my = ++gen.current;
      make().then(function (v) { if (my === gen.current) st[1](v); }, function (e) { say('The preview could not draw: ' + (e && e.message ? e.message : e)); });
    }, [key]);
    return st[0];
  }
  function useParts(d, say) {
    var plate = usePart(JSON.stringify(d.scope), function () { return B.plateCanvas(d); }, say);
    var arm = usePart(JSON.stringify(d.arm) + d.scope.face, async function () { var a = B.arm(d); return { c: await B.rast(a.svg, a.size[0], a.size[1]), piv: a.pivot }; }, say);
    var planes = usePart(JSON.stringify(d.planes), async function () {
      var mk = B.markers(d);
      return { frames: await B.planeFrames(d), px: B.planeBox(d),
        sel: await B.rast(mk[0][2], 56, 56), em: await B.rast(mk[1][2], 64, 64) };
    }, say);
    var words = usePart(JSON.stringify(d.words) + JSON.stringify(d.frames), async function () {
      var L = B.layout(d), out = { layout: L, panels: {} };
      for (var n in L) out.panels[n] = await B.panelCanvas(d, n);
      var z = B.sizes(d);
      try { await document.fonts.load(z.body + 'px "' + d.words.face + '"'); await document.fonts.load(z.quiet + 'px "' + d.words.face + '"'); } catch (e) { /* fallback face */ }
      return out;
    }, say);
    return { plate: plate, arm: arm, planes: planes, words: words };
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
    say('range', ['80 nm'], 'quiet');
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

  // the start sheet's pictures, drawn once per preset
  var thumbs = {};
  function drawThumb(k, c) {
    var put = function (im) { var x = c.getContext('2d'); x.clearRect(0, 0, c.width, c.height); x.drawImage(im, 0, 0, c.width, c.height); };
    if (thumbs[k] && thumbs[k].im) { put(thumbs[k].im); return; }
    if (!thumbs[k]) thumbs[k] = { p: B.thumb(B.preset(k), 192).then(function (im) { thumbs[k].im = im; return im; }) };
    thumbs[k].p.then(put, function () { /* the tile stays dark */ });
  }

  /* ---- the editor ---- */
  // ?preset=, ?step=, ?view=, ?render= and ?mode= open the editor where a link (or a check) wants it
  var Q = (function () { try { return new URLSearchParams(root.location.search); } catch (e) { return null; } })();
  var q = function (k, ok, dflt) { var v = Q && Q.get(k); return v && ok.indexOf(v) >= 0 ? v : dflt; };
  var col = function (pairs) { return pairs.map(function (p) { return p[0]; }); };

  function Editor() {
    var a = A();
    var u = function (v) { return R.useState(v); };
    var dS = u(function () { return B.preset(q('preset', B.PRESETS.concat(['blank']), 'modern')); }), d = dS[0], setD = dS[1];
    var stepS = u(q('step', col(STEPS), 'start')), step = stepS[0], setStep = stepS[1];
    var stS = u(''), status = stS[0], setStatus = stS[1];
    var rendS = u(q('render', ['design', 'panel', 'compare'], 'design')), rend = rendS[0], setRend = rendS[1];
    var modeS = u(q('mode', col(MODES), 'info')), mode = modeS[0], setMode = modeS[1];
    var playS = u(true), playing = playS[0], setPlaying = playS[1];
    var wipeS = u(233), wipe = wipeS[0], setWipe = wipeS[1];
    var hidS = u({}), hidden = hidS[0], setHidden = hidS[1];
    var sendS = u(null), sending = sendS[0], setSending = sendS[1];
    var dupS = u(null), dup = dupS[0], setDup = dupS[1];
    var libS = u(0), setLibTick = libS[1];
    var delS = u(null), mineDel = delS[0], setMineDel = delS[1];
    var drafts = R.useRef({}).current, canvasRef = R.useRef(null), tRef = R.useRef(0), stateRef = R.useRef({}), frameSt = R.useRef({}).current;
    var say = R.useCallback(function (s) { setStatus(s); }, []);
    stateRef.current = { d: d, status: status };

    // the screen: the OS's one controller (studio@1 S.viewer) over this editor's state
    var scrRef = R.useRef(null);
    var vw = R.useMemo(function () {
      return S.viewer({ get: function () { return scrRef.current; }, set: function (p) { setScr(function (o) { return Object.assign({}, o, p); }); }, say: say });
    }, []);
    var scrS = u(function () { return vw.init(q('view', col(T.VIEWS), 'fit')); }), setScr = scrS[1];
    scrRef.current = scrS[0];

    var parts = useParts(d, say);
    var ses = R.useMemo(function () {
      return T.session({
        slug: 'flights', keys: function () { return ['d']; }, state: function () { return stateRef.current; },
        set: function () { setLibTick(function (n) { return n + 1; }); }, ready: function () { return true; }, say: say,
        restore: function (snap, text) { setD(snap.d); say(text); }, schema: function () { return B.SCHEMA; },
        migrate: function () { return { 1: function (o) { o.design = B.upgrade1(o.design); return o; } }; },
        content: async function () { return { design: stateRef.current.d }; },
        apply: function (doc) { var why = checkDesign(doc.design); if (!why) { setD(B.complete(doc.design)); setStep(function (s) { return s === 'start' ? 'scope' : s; }); } return why; },
        bake: async function () { var r = await bakeNow(); return r ? r.bytes : null; },
      });
    }, []);
    var sender = R.useMemo(function () {
      return T.sender({ say: say, ask: function (x) { setDup(x); }, sending: function (x) { setSending(x); } });
    }, []);
    R.useEffect(function () { ses.track(); }, [d]);
    R.useEffect(function () { ses.boot().then(function () { return ses.openTheme(); }).catch(function () {}); }, []);
    var upd = function (path, v) { setD(function (old) { return setIn(old, path, v); }); };
    // what each colour row offers back as its preset: the start this design came from, the blank one included
    var base = R.useMemo(function () { return B.preset(d.base === 'blank' || a.families[d.base] ? d.base : 'modern'); }, [d.base]);

    /* the bake, the send, the download */
    async function bakeNow() {
      var dd = stateRef.current.d;
      if (dd.planes.family === 'custom' && !(dd.planes.custom && dd.planes.custom.image)) { say('Not baked: pick your plane picture on the Planes step first'); return null; }
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
        var dg = new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode('flights/' + nm)));
        return root.GorbBake.cardStem('flights', nm, dg);
      } catch (e) { return null; }
    }
    function send() {
      var nm = d.name;
      sender.send(nm, async function () {
        // the library keeps it first: a send the orb refuses still leaves the design on the hub
        await ses.saveDraft({ name: nm, doc: await ses.doc(true), thumb: await draftThumb() }, true);
        var r = await bakeNow(); if (!r || sender.cancelled()) return;
        await sender.post(r.bytes, { name: nm, stem: await stemOf(nm), summary: Math.round(r.bytes.length / 1024) + ' KB' });
        ses.libraryRefresh();
      });
    }
    async function download() {
      var r = await bakeNow(); if (!r) return;
      T.download(new Blob([r.bytes], { type: 'application/octet-stream' }), ((await stemOf(d.name)) || 'flights-theme') + '.orbtheme');
    }
    async function saveDraft() { await ses.saveDraft({ name: d.name, doc: await ses.doc(true), thumb: await draftThumb() }); }
    // a small picture of the design for the hub's lists: a draft has no bake to show
    async function draftThumb() {
      try {
        var im = await B.thumb(stateRef.current.d, 192), c = document.createElement('canvas'); c.width = c.height = 96;
        var x = c.getContext('2d'); x.fillStyle = '#000'; x.fillRect(0, 0, 96, 96); x.drawImage(im, 0, 0, 96, 96);
        return c.toDataURL('image/jpeg', 0.82);
      } catch (e) { return null; }
    }

    /* the preview: the frame clock, Design, Panel and Split */
    R.useEffect(function () {
      var tk = playing ? T.ticker(B.SWEEP_FPS, false, function () { tRef.current += 1000 / B.SWEEP_FPS; draw(); }) : null;
      draw();
      return function () { if (tk) tk.stop(); };
    });
    function draw() {
      var c = canvasRef.current; if (!c) return;
      var x = c.getContext('2d'), dd = stateRef.current.d;
      drawFrame(x, dd, parts, tRef.current, hidden, mode, false, frameSt);
      if (rend === 'design' || !parts.plate) return;
      // Panel and Split: the plate as the bake stores it, the moving rows blended over it
      // the way the orb blends them (theme@1/preview's panel mirror)
      var lc = document.createElement('canvas'); lc.width = lc.height = G;
      var lx = lc.getContext('2d');
      drawFrame(lx, dd, parts, tRef.current, hidden, mode, true, { dy: frameSt.dy });
      T.panel(x, { under: parts.plate.getContext('2d').getImageData(0, 0, G, G).data,
                   live: [{ px: lx.getImageData(0, 0, G, G).data, still: false }] }, { mode: rend, wipe: wipe });
    }

    /* the fit: the live rows the theme ships against the orb's, at the estimate once baked */
    // rows only (owner, 2026-11-04): unlike the clock there is no designed rate to fall short
    // of, and the estimate is not a live measure, so the verdict claims no fps at all
    var verdict = T.fit({ rows: LIVE_ROWS, cap: ROW_CAP });

    /* ---- the fields of each step ---- */
    var K = S.fieldKit({ drafts: drafts, refresh: function () { setLibTick(function (n) { return n + 1; }); }, scope: function () { return step; } });
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
      var dLabel = { scope: 'Graticule', chart: 'Grid', brass: 'Bezel metal', blueprint: 'Grid', galaxy: 'Nebula' }[sc.ground];
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
      if (sc.ground !== 'image') fields.push(C(sc.ground === 'brass' ? 'Enamel' : sc.ground === 'chart' ? 'Paper' : sc.ground === 'galaxy' ? 'Space' : 'Face', 'scope.face'));
      if (dLabel) fields.push(C(dLabel, 'scope.detail'));
      if (sc.ground === 'galaxy') fields.push(onOff('Stars', 'scope.grid', 'Dense', 'Sparse'));
      if (sc.ground === 'scope' || sc.ground === 'chart' || sc.ground === 'blueprint') {
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

    /* ---- the left pane: the parts ---- */
    var items = [['scope', 'Scope'], ['arm', 'Arm'], ['planes', 'Planes'], ['words', 'Words']].map(function (x) {
      return { id: x[0], band: 'parts', label: x[1], movable: false };
    });
    var left = h(S.Layers, { title: 'Parts', rows: S.layerRows(items, {
      sel: step === 'frames' ? 'words' : step, heads: { parts: 'The scope, bottom to top' },
      icon: function (it) { return { scope: '◎', arm: '◜', planes: '✈', words: 'A' }[it.id]; },
      note: function (it) {
        return { scope: B.GROUND_TITLES[sc.ground], arm: d.arm.kind === 'none' ? 'none' : d.arm.kind + ' · ' + B.period(d) + ' s',
          planes: d.planes.family === 'custom' ? 'your own' : a.families[d.planes.family].title, words: d.words.face }[it.id];
      },
      hidden: function (it) { return !!hidden[it.id]; }, move: function () { return { ok: false, reason: 'the tracker’s parts keep their order' }; },
      onMove: function () {}, pick: function (it) { setStep(it.id); },
      eyeTip: function (it) { return hidden[it.id] ? 'Show it in the preview' : 'Hide it in the preview (the bake keeps it)'; },
      toggleEye: function (it) { var n = Object.assign({}, hidden); n[it.id] = !n[it.id]; setHidden(n); },
      drag: { start: function () {}, over: function () {}, drop: function () {}, end: function () {} },
    }) });

    /* ---- the right pane ---- */
    var right = step === 'render'
      ? h(R.Fragment, null, h('div', { className: 'fl-sec' }, 'Render'),
          h('div', { className: 'fl-list' }, h(S.RenderPanel, {
            badge: S.badgeProps(verdict, 'The live rows against the 32 an app seats'),
            send: { on: send, note: 'save “' + d.name + '”, bake it and store it on the orb', tip: 'Save it to the hub library, bake it, then send it to the orb this editor was opened for. The library keeps the baked theme once the orb has stored it.' },
            save: ses.hubOrb() ? { on: saveDraft, note: 'keep “' + d.name + '” on the hub as a draft', tip: 'Keep this design in the hub library under its name, without baking or sending it. Under a theme’s name it is that theme’s unsent changes until Send to orb bakes them in.' } : null,
            downloads: [{ label: 'Download .orbtheme', note: 'the baked file', on: download }, { label: 'Check the bake', note: 'bake now and check its size', on: bakeNow }],
          })))
      : step === 'start'
        ? h('div', { className: 'fl-list' }, h('div', { className: 'ui-mut' }, 'Pick a starting point, or open one from the hub library, then Continue. The steps after it: Scope, Arm, Planes, Words, Frames, and Render to send it.'))
        : h('div', { className: 'fl-list' }, h(S.Fields, { fields: fields }));

    /* ---- the start sheet ---- */
    var famOf = function (dd) { return a.families[dd.base] ? dd.base : dd.planes && a.families[dd.planes.family] ? dd.planes.family : null; };
    var sheet = step === 'start' ? h(S.StartSheet, {
      title: 'Start a flight tracker theme',
      scratch: { label: 'Blank', short: 'Plain greys, nothing preset', cls: d.base === 'blank' ? 'on' : '',
        tip: 'Every part present, in plain greys on a plain ground: a blank slate to build your own look on',
        on: function () { setD(B.preset('blank')); ses.restart(); say('Starting from a blank theme: Continue when you are ready'); } },
      tiles: B.PRESETS.map(function (k) {
        var f = a.families[k];
        var pd = B.preset(k);
        return { key: k, name: f.title, short: B.GROUND_TITLES[pd.scope.ground] + ' · ' + (B.ARMS.find(function (r) { return r[0] === pd.arm.kind; }) || [0, pd.arm.kind])[1],
          tip: 'Start from ' + f.title + ': ' + f.band, cls: d.base === k ? 'on' : '', draw: function (c) { drawThumb(k, c); },
          on: function () { setD(B.preset(k)); ses.restart(); say('Starting from ' + f.title + ': Continue when you are ready'); } };
      }),
      library: { accept: '.orbtheme,.orbdesign,.json',
        openFile: function (e) { var fl = e.target.files[0]; if (fl) ses.openFile(fl); },
        rows: S.libraryRows(ses.library(), { version: ses.version(), armed: mineDel, arm: setMineDel,
          meta: function (m) { var f = m.design && famOf(m.design); return f ? a.families[f].title + ' · ' : ''; },
          open: function (nm) { ses.openEntry(nm); }, forget: function (nm, what) { ses.forgetEntry(nm, what); } }),
        empty: ses.hubOrb() ? null : 'The library is on the hub: open this editor from the hub to save and open designs there.' },
      foot: [{ label: 'Continue with ' + d.name, primary: true, on: function () { setStep('scope'); } }],
    }) : null;

    /* ---- the frame ---- */
    var g = vw.glass(), tool = vw.tool();
    return h(S.Frame, null,
      h(S.TopBar, {
        back: function () { try { root.parent.postMessage({ type: 'theme.close' }, '*'); } catch (e) { /* not framed */ } },
        backTip: 'Back to the Themes page', crumb: 'Flights',
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
        h(S.Toolbar, {
          play: { icon: playing ? '❚❚' : '▶', label: playing ? 'Pause the traffic' : 'Play the traffic', on: function () { setPlaying(!playing); } },
          view: tool.view, zoom: tool.zoom,
          render: T.RENDERS.map(function (r) { return { label: r[1], tip: r[2], cls: r[0] === rend ? 'on' : '', pick: function () { setRend(r[0]); } }; }),
          wipe: rend === 'compare' ? { value: wipe, on: function (e) { setWipe(+e.target.value); } } : null,
          screen: vw.toolScreen(),
        }, h(S.Seg, { opts: MODES.map(function (md) { return { label: md[1], tip: md[2], cls: md[0] === mode ? 'on' : '', pick: function () { setMode(md[0]); } }; }) }))),
      h(S.Pane, { side: 'right' }, right),
      h(S.SendModal, S.sendProps(sending, status, function () { sender.cancel(); })),
      h(S.DupModal, S.dupProps(dup, function () { sender.replace(); }, function () { sender.dismiss(); })));
  }

  root.FlightsEditor = { Editor: Editor, drawFrame: drawFrame, checkDesign: checkDesign, TRAFFIC: TRAFFIC, where: where, cardDy: cardDy };
  if (typeof document !== 'undefined' && document.getElementById('root'))
    root.ReactDOM.createRoot(document.getElementById('root')).render(h(Editor));
})(typeof window !== 'undefined' ? window : globalThis);
