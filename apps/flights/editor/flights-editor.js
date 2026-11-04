/* flights-editor.js — the flight tracker's theme editor (flight-tracker-design §12).
 *
 * A small editor on the OS's kit (theme-editor-kit-plan): the shell's frame, top bar, glass,
 * toolbar, fields, layers panel and Render step (studio@1); the session's library and undo,
 * the sender, the preview's views and the fit verdict (theme@1); the stage bake (baker@1,
 * through flights-bake.js). What is the tracker's is here: the design and its fields, the
 * parts list, and the preview's simulated traffic under the turning arm. Plain React,
 * createElement only. */
(function (root) {
  'use strict';
  var R = root.React, h = R.createElement, S = root.OrbStudio, T = root.OrbTheme, B = root.FlightsBake;
  var A = function () { return root.FlightsArt; };
  var G = 466, CX = 233, SWEEP_MS = 4000, LIVE_ROWS = 1 + 15 + 2 + 4, ROW_CAP = 32;
  var STEPS = [['start', 'Start'], ['scope', 'Scope'], ['arm', 'Arm'], ['planes', 'Planes'], ['words', 'Words'], ['render', 'Render']];
  var BAND_OF = function (alt) {        // apps/flights/main.c band_of: the seven bands, ground first
    if (alt === 0) return 0; if (alt == null) return 1;
    return alt < 3000 ? 2 : alt < 10000 ? 3 : alt < 20000 ? 4 : alt < 30000 ? 5 : 6;
  };
  // made-up traffic (review.html's): [shape slot, altitude ft (0 ground, null none), range 0..1, bearing, track, callsign, type]
  var TRAFFIC = [[3, 34000, .82, 40, 220, 'UAL1532', 'B38M'], [4, 38000, .55, 300, 95, 'AAL31', 'B77W'], [3, 12000, .38, 140, 330, 'SWA2210', 'B737'],
    [0, 2500, .22, 230, 60, 'N4521K', 'C172'], [1, 8000, .66, 190, 10, 'ENY3301', 'E75L'], [2, 41000, .9, 110, 280, 'EJA512', 'C68A'],
    [5, 1200, .3, 330, 150, 'N911TH', 'EC35'], [6, null, .72, 270, 45, 'ADSB01', ''], [3, 0, .12, 80, 200, 'DAL88', 'A321'],
    [3, 26000, .48, 20, 170, 'FFT1290', 'A20N'], [4, 31000, .95, 210, 300, 'QTR731', 'A35K'], [1, 14000, .6, 60, 250, 'SKW4410', 'CRJ9']];

  /* ---- the design ---- */
  var clone = function (o) { return JSON.parse(JSON.stringify(o)); };
  var setIn = function (d, path, v) {
    var n = clone(d), o = n, ks = path.split('.');
    for (var i = 0; i < ks.length - 1; i++) o = o[ks[i]];
    o[ks[ks.length - 1]] = v;
    return n;
  };
  // a design opened from anywhere: every field present and in range, or why not
  function checkDesign(x) {
    if (!x || !x.scope || !x.arm || !x.planes || !x.words) return 'that design has no tracker theme in it';
    var a = A();
    if (!a.families[x.planes.family]) return 'that design names a plane family this editor does not carry (' + x.planes.family + ')';
    if (x.arm.kind !== 'none' && x.arm.kind !== 'wedge' && !a.arms[x.arm.kind]) return 'that design names an arm this editor does not carry';
    if (!Array.isArray(x.planes.bands) || x.planes.bands.length !== 7 || !Array.isArray(x.planes.shapes) || x.planes.shapes.length !== 7)
      return 'that design’s planes are not seven shapes and seven bands';
    return null;
  }

  /* ---- the preview's parts, drawn once a part changes: the plate, the arm, the 49 plane
   * frames, the marker rings and the label panels, keyed by the part of the design they draw */
  function useParts(d, say) {
    var st = R.useState({}), parts = st[0], setParts = st[1], gen = R.useRef(0);
    var key = JSON.stringify(d);
    R.useEffect(function () {
      var my = ++gen.current, cancelled = false;
      (async function () {
        try {
          var px = Math.max(30, Math.min(48, d.planes.size | 0)), ar = B.arm(d), frames = [];
          for (var si = 0; si < 7; si++) for (var bi = 0; bi < 7; bi++) frames.push(await B.rast(B.planeSvg(d, si, bi), px, px));
          var panels = {};
          for (var lab of B.LABELS) { var b = B.labelBox(d, lab); panels[lab.name] = { box: b, c: await B.rast(B.panelSvg(d, lab), b[2], b[3]) }; }
          try { await document.fonts.load('16px "' + d.words.face + '"'); await document.fonts.load('14px "' + d.words.face + '"'); } catch (e) { /* fallback face */ }
          var p = { plate: await B.plateCanvas(d), arm: await B.rast(ar.svg, ar.size[0], ar.size[1]), armPiv: ar.pivot, frames: frames, px: px,
            sel: await B.rast(B.markerSvg(28, d.planes.sel), 56, 56), panels: panels };
          if (!cancelled && my === gen.current) setParts(p);
        } catch (e) { say('The preview could not draw: ' + (e && e.message ? e.message : e)); }
      })();
      return function () { cancelled = true; };
    }, [key]);
    return parts;
  }

  // the words the app says, in the theme's face and tones, inside each label's box
  function drawWords(x, d, parts, hidden, cardOn) {
    var W = d.words, P = parts.panels || {};
    var say = function (name, lines, tone, size) {
      var p = P[name]; if (!p) return;
      var b = p.box;
      x.drawImage(p.c, b[0], b[1]);
      x.font = size + 'px "' + W.face + '", sans-serif';
      x.textAlign = 'center'; x.textBaseline = 'middle';
      var col = W[tone];
      if (tone === 'accent' && W.accent2) { var g = x.createLinearGradient(0, b[1], 0, b[1] + b[3]); g.addColorStop(0, W.accent); g.addColorStop(1, W.accent2); col = g; }
      x.fillStyle = col;
      var lh = size * 1.2, y0 = b[1] + b[3] / 2 - (lines.length - 1) * lh / 2;
      lines.forEach(function (s, i) { x.fillText(s, b[0] + b[2] / 2, y0 + i * lh); });
    };
    if (hidden.words) return;
    say('header', [TRAFFIC.length + ' aircraft'], 'body', 16);
    say('footer', ['live  adsb.fi'], 'quiet', 14);
    say('range', ['80 nm'], 'quiet', 14);
    if (cardOn) say('card', ['UAL1532  B38M', 'DFW → SFO', '34000 ft  ↑ 1800 fpm', '452 kt  trk 220'], 'body', 16);
  }

  // one frame of the scope at time t (ms): the plate, the arm turned, the traffic moved along
  // its tracks, the selection ring on the first contact, the words
  // live: the moving layer alone on a clear canvas (Panel's live row), no plate under it
  function drawFrame(x, d, parts, t, hidden, cardOn, live) {
    x.clearRect(0, 0, G, G);
    if (live) { /* the plate is Panel's under */ }
    else if (parts.plate && !hidden.scope) x.drawImage(parts.plate, 0, 0);
    else { x.fillStyle = '#000'; x.fillRect(0, 0, G, G); }
    var rr = A().ring_r;
    if (parts.arm && !hidden.arm) {
      x.save(); x.translate(CX, CX); x.rotate(((t % SWEEP_MS) / SWEEP_MS) * 2 * Math.PI);
      x.drawImage(parts.arm, -parts.armPiv[0], -parts.armPiv[1]); x.restore();
    }
    if (parts.frames && !hidden.planes) {
      TRAFFIC.forEach(function (c, i) {
        var dist = c[2] * rr, br = c[3] * Math.PI / 180, tr = c[4] * Math.PI / 180, v = (c[1] === 0 ? 2 : 8) * t / 1000;
        var px = CX + dist * Math.sin(br) + v * Math.sin(tr), py = CX - dist * Math.cos(br) - v * Math.cos(tr);
        var dx = px - CX, dy = py - CX, r = Math.hypot(dx, dy);
        if (r > rr) { px = CX - dx * (2 * rr - r) / r; py = CX - dy * (2 * rr - r) / r; }   // wrap inside the range
        var f = parts.frames[c[0] * 7 + BAND_OF(c[1])];
        x.save(); x.translate(px, py); if (c[1] !== 0) x.rotate(tr); x.drawImage(f, -parts.px / 2, -parts.px / 2); x.restore();
        if (i === 0 && cardOn && parts.sel) x.drawImage(parts.sel, px - 28, py - 28);
      });
    }
    drawWords(x, d, parts, hidden, cardOn);
  }

  /* ---- the editor ---- */
  // ?step=, ?view= and ?render= open the editor where a link (or a check) wants it
  var Q = (function () { try { return new URLSearchParams(root.location.search); } catch (e) { return null; } })();
  var q = function (k, ok, dflt) { var v = Q && Q.get(k); return v && ok.indexOf(v) >= 0 ? v : dflt; };

  function Editor() {
    var a = A();
    var u = function (v) { var s = R.useState(v); return s; };
    var dS = u(B.preset('modern')), d = dS[0], setD = dS[1];
    var stepS = u(q('step', STEPS.map(function (s) { return s[0]; }), 'start')), step = stepS[0], setStep = stepS[1];
    var stS = u(''), status = stS[0], setStatus = stS[1];
    var viewS = u(q('view', ['fit', 'full', 'true', 'zoom'], 'fit')), view = viewS[0], setView = viewS[1];
    var rendS = u(q('render', ['design', 'panel', 'compare'], 'design')), rend = rendS[0], setRend = rendS[1];
    var zoomS = u(2), zoom = zoomS[0], setZoom = zoomS[1];
    var fitS = u(426), fitPx = fitS[0], setFitPx = fitS[1];
    var calS = u(T.screen.load()), cal = calS[0], setCal = calS[1];
    var playS = u(true), playing = playS[0], setPlaying = playS[1];
    var wipeS = u(233), wipe = wipeS[0], setWipe = wipeS[1];
    var hidS = u({}), hidden = hidS[0], setHidden = hidS[1];
    var cardS = u(true), cardOn = cardS[0], setCardOn = cardS[1];
    var sendS = u(null), sending = sendS[0], setSending = sendS[1];
    var dupS = u(null), dup = dupS[0], setDup = dupS[1];
    var estS = u(null), est = estS[0], setEst = estS[1];
    var libS = u(0), setLibTick = libS[1];
    var drafts = R.useRef({}).current, canvasRef = R.useRef(null), fileRef = R.useRef(null), tRef = R.useRef(0), stateRef = R.useRef({});
    var say = R.useCallback(function (s) { setStatus(s); }, []);
    stateRef.current = { d: d, status: status };

    var parts = useParts(d, say);
    var ses = R.useMemo(function () {
      return T.session({
        slug: 'flights', keys: function () { return ['d']; }, state: function () { return stateRef.current; },
        set: function () { setLibTick(function (n) { return n + 1; }); }, ready: function () { return true; }, say: say,
        restore: function (snap, text) { setD(snap.d); say(text); }, schema: function () { return B.SCHEMA; }, migrate: function () { return {}; },
        content: async function () { return { design: stateRef.current.d }; },
        apply: function (doc) { var why = checkDesign(doc.design); if (!why) setD(doc.design); return why; },
        bake: async function () { var r = await bakeNow(); return r ? r.bytes : null; },
      });
    }, []);
    var sender = R.useMemo(function () {
      return T.sender({ say: say, ask: function (x) { setDup(x); }, sending: function (x) { setSending(x); } });
    }, []);
    R.useEffect(function () { ses.track(); }, [d]);
    R.useEffect(function () { ses.boot().then(function () { return ses.openTheme(); }).catch(function () {}); }, []);
    var upd = function (path, v) { setD(function (old) { return setIn(old, path, v); }); };

    /* the bake, the send, the download */
    async function bakeNow() {
      try {
        say('Baking “' + stateRef.current.d.name + '”…');
        var doc = new TextEncoder().encode(JSON.stringify(await ses.doc(true)));
        var r = await B.bake(stateRef.current.d, { design: doc });
        setEst(r.est);
        say('Baked “' + stateRef.current.d.name + '”: ' + Math.round(r.bytes.length / 1024) + ' KB');
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
        var r = await bakeNow(); if (!r || sender.cancelled()) return;
        await sender.post(r.bytes, { name: nm, stem: await stemOf(nm), summary: Math.round(r.bytes.length / 1024) + ' KB' });
        ses.libraryRefresh();
      });
    }
    async function download() {
      var r = await bakeNow(); if (!r) return;
      T.download(new Blob([r.bytes], { type: 'application/octet-stream' }), ((await stemOf(d.name)) || 'flights-theme') + '.orbtheme');
    }
    async function saveDraft() { await ses.saveDraft({ name: d.name, doc: await ses.doc(true) }); }

    /* the preview: the frame clock, Design, Panel and Split */
    R.useEffect(function () {
      var tk = playing ? T.ticker(15, false, function () { tRef.current += 1000 / 15; draw(); }) : null;
      draw();
      return function () { if (tk) tk.stop(); };
    });
    function draw() {
      var c = canvasRef.current; if (!c) return;
      var x = c.getContext('2d'), dd = stateRef.current.d;
      drawFrame(x, dd, parts, tRef.current, hidden, cardOn);
      if (rend === 'design' || !parts.plate) return;
      // Panel and Split: the plate as the bake stores it, the moving rows blended over it
      // the way the orb blends them (theme@1/preview's panel mirror)
      var lc = document.createElement('canvas'); lc.width = lc.height = G;
      var lx = lc.getContext('2d');
      drawFrame(lx, dd, parts, tRef.current, hidden, cardOn, true);
      T.panel(x, { under: parts.plate.getContext('2d').getImageData(0, 0, G, G).data,
                   live: [{ px: lx.getImageData(0, 0, G, G).data, still: false }] }, { mode: rend, wipe: wipe });
    }

    /* the fit: the live rows the theme ships against the orb's, at the estimate once baked */
    var verdict = T.fit({ rows: LIVE_ROWS, cap: ROW_CAP, want: B.SWEEP_FPS, est: est });
    var V = T.view({ view: view, zoom: zoom, fitPx: fitPx, cal: cal, calibrated: T.screen.calibrated });

    /* ---- the fields of each step ---- */
    var K = S.fieldKit({ drafts: drafts, refresh: function () { setLibTick(function (n) { return n + 1; }); }, scope: function () { return step; } });
    var C = function (label, path, X) { var v = path.split('.').reduce(function (o, k) { return o[k]; }, d); return K.Co(label, v == null ? 'none' : v, function (nv) { upd(path, nv === 'none' ? null : nv); }, '', step, X || { noAuto: true }); };
    var opt = function (pairs) { return pairs.map(function (p) { return { v: p[0], label: p[1] }; }); };
    var fields = [];
    if (step === 'scope') {
      fields.push(K.H('Background'));
      fields.push(K.Ch('Background', d.scope.bg, opt(Object.keys(a.backgrounds).map(function (k) { return [k, a.backgrounds[k].title]; }).concat([['colour', 'Flat colour'], ['image', 'Image']])), function (v) { upd('scope.bg', v); }));
      if (d.scope.bg === 'colour') fields.push(C('Colour', 'scope.colour'));
      if (d.scope.bg === 'image') fields.push(K.Fi('Image', function (e) {
        var f = e.target.files && e.target.files[0]; if (!f) return;
        var rd = new FileReader(); rd.onload = function () { upd('scope.image', rd.result); }; rd.readAsDataURL(f);
      }, d.scope.image ? 'cut to fill the glass' : 'none yet'));
      if (a.backgrounds[d.scope.bg]) fields.push(K.Nt('', a.backgrounds[d.scope.bg].what));
      fields.push(K.H('Range rings'));
      fields.push(K.Rg('Rings', d.scope.rings, 0, 4, 1, function (v) { upd('scope.rings', v); }, d.scope.rings ? d.scope.rings + ' (even fractions of the range)' : 'none'));
      fields.push(C('Ring colour', 'scope.ringColour'));
      fields.push(K.Rg('Ring opacity', d.scope.ringOpacity, 0, 1, 0.05, function (v) { upd('scope.ringOpacity', v); }, Math.round(d.scope.ringOpacity * 100) + '%'));
      fields.push(C('Home dot', 'scope.home'));
    } else if (step === 'arm') {
      fields.push(K.H('Radar arm'));
      fields.push(K.Ch('Arm', d.arm.kind, opt([['wedge', 'Phosphor wedge']].concat(Object.keys(a.arms).filter(function (k) { return k !== 'wedge'; }).map(function (k) { return [k, a.arms[k].title]; })).concat([['none', 'None']])), function (v) { upd('arm.kind', v); }));
      if (d.arm.kind === 'wedge') {
        fields.push(K.Rg('Trail', d.arm.trail, 5, 90, 1, function (v) { upd('arm.trail', v); }, d.arm.trail + '°'));
        fields.push(K.Nt('', 'The trail is drawn every frame; its area is its cost on the orb.'));
        fields.push(C('Colour', 'arm.colour'));
        fields.push(C('Lead line', 'arm.lead'));
      } else if (a.arms[d.arm.kind]) fields.push(K.Nt('', a.arms[d.arm.kind].what));
    } else if (step === 'planes') {
      var f = a.families[d.planes.family], keys = Object.keys(f.shapes).filter(function (k) { return k !== 'single'; });
      fields.push(K.H('Planes'));
      fields.push(K.Ch('Family', d.planes.family, opt(B.PRESETS.map(function (k) { return [k, a.families[k].title]; })), function (v) { upd('planes.family', v); }));
      fields.push(K.Ch('Form', d.planes.form, opt([['types', 'A plane per category'], ['single', 'One detailed plane']]), function (v) { upd('planes.form', v); }));
      fields.push(K.Nt('', d.planes.form === 'single' ? f.single : f.band));
      fields.push(K.Rg('Size', d.planes.size, 30, 48, 1, function (v) { upd('planes.size', v); }, d.planes.size + ' px · ' + (d.planes.size * T.screen.MM_PER_PX).toFixed(1) + ' mm'));
      if (d.planes.form === 'types') {
        fields.push(K.H('Which plane each category wears'));
        B.SHAPES.forEach(function (sh, i) {
          fields.push(K.Sl(B.SHAPE_TITLES[i], d.planes.shapes[i], keys.map(function (k) { return { v: k, label: k }; }), function (v) { var s = d.planes.shapes.slice(); s[i] = v; upd('planes.shapes', s); }));
        });
      }
      fields.push(K.H('Altitude colours'));
      fields.push(K.Ch('Colours', d.planes.colour ? 'on' : 'off', opt([['on', 'By altitude'], ['off', 'Off']]), function (v) { upd('planes.colour', v === 'on'); }));
      if (d.planes.colour) a.bands.forEach(function (b, i) {
        fields.push(K.Co(b[0], d.planes.bands[i], function (v) { var s = d.planes.bands.slice(); s[i] = v; upd('planes.bands', s); }, '', 'band' + i, { noAuto: true }));
      });
      else fields.push(K.Rg('Ground traffic', d.planes.ground, 0.1, 1, 0.05, function (v) { upd('planes.ground', v); }, Math.round(d.planes.ground * 100) + '%'));
      fields.push(K.H('Markers'));
      fields.push(C('Selection ring', 'planes.sel'));
      fields.push(C('Emergency ring', 'planes.em'));
    } else if (step === 'words') {
      fields.push(K.H('Typeface'));
      fields.push(K.Sl('Face', d.words.face, B.FACES.map(function (k) { return { v: k, label: k }; }), function (v) { upd('words.face', v); }));
      fields.push(K.H('Tones'));
      fields.push(C('Body', 'words.body'));
      fields.push(C('Accent', 'words.accent'));
      fields.push(C('Accent fades to', 'words.accent2', { none: true, noAuto: true }));
      fields.push(C('Quiet', 'words.quiet'));
      fields.push(C('Warning', 'words.warning'));
      fields.push(K.H('The info card'));
      fields.push(C('Fill', 'words.card.fill'));
      fields.push(K.Rg('Opacity', d.words.card.opacity, 0.3, 1, 0.02, function (v) { upd('words.card.opacity', v); }, Math.round(d.words.card.opacity * 100) + '%'));
      fields.push(C('Edge', 'words.card.edge'));
      fields.push(K.Ch('Plaques', d.words.plaques ? 'on' : 'off', opt([['off', 'Words on the glass'], ['on', 'On plaques']]), function (v) { upd('words.plaques', v === 'on'); }));
      fields.push(K.Nt('', 'Plaques put the header, footer and range on small panels in the card’s fill and edge: for a bezel engraved to the rim.'));
    }

    /* ---- the left pane: Start's presets and library, else the parts ---- */
    var left;
    if (step === 'start') {
      var lib = ses.library();
      left = h(R.Fragment, null,
        h('div', { className: 'fl-sec' }, 'Start from'),
        h('div', { className: 'fl-list' }, B.PRESETS.map(function (k) {
          return h('button', { key: k, className: 'fl-item', onClick: function () { setD(B.preset(k)); ses.restart(); say('Started from ' + a.families[k].title); setStep('scope'); } },
            a.families[k].title, h('small', null, a.families[k].pair.join(' · ')));
        })),
        h('div', { className: 'fl-sec' }, 'In the hub library'),
        h('div', { className: 'fl-list' }, lib.length ? lib.map(function (e) {
          return h('button', { key: e.name, className: 'fl-item', onClick: function () { ses.openEntry(e.name).then(function (ok) { if (ok) setStep('scope'); }); } },
            e.name, h('small', null, e.state === 'changed' ? 'theme · unsent changes' : e.state));
        }) : h('div', { className: 'ui-mut', style: { padding: '0 4px' } }, ses.hubOrb() ? 'Nothing saved yet.' : 'The library is on the hub: open this editor from the hub.')),
        h('div', { className: 'fl-act' },
          h('button', { onClick: saveDraft, title: 'Keep this design in the hub library as a draft' }, 'Save draft'),
          h('button', { title: 'Open an .orbtheme this editor made, or a design file', onClick: function () { fileRef.current && fileRef.current.click(); } }, 'Open a file'),
          h('input', { ref: fileRef, type: 'file', accept: '.orbtheme,.orbdesign,.json', style: { display: 'none' },
            onChange: function (e) { var fl = e.target.files[0]; if (fl) ses.openFile(fl).then(function (ok) { if (ok) setStep('scope'); }); } })));
    } else {
      var items = [['scope', 'Scope', 'scope'], ['arm', 'Arm', 'arm'], ['planes', 'Planes', 'planes'], ['words', 'Words', 'words']].map(function (x) {
        return { id: x[0], band: 'parts', label: x[1], step: x[2], movable: false };
      });
      left = h(S.Layers, { title: 'Parts', rows: S.layerRows(items, {
        sel: step, heads: { parts: 'The scope, bottom to top' },
        icon: function (it) { return { scope: '◎', arm: '◜', planes: '✈', words: 'A' }[it.id]; },
        note: function (it) { return { scope: d.scope.bg, arm: d.arm.kind, planes: a.families[d.planes.family].title, words: d.words.face }[it.id]; },
        hidden: function (it) { return !!hidden[it.id]; }, move: function () { return { ok: false, reason: 'the tracker’s parts keep their order' }; },
        onMove: function () {}, pick: function (it) { setStep(it.step); },
        eyeTip: function (it) { return hidden[it.id] ? 'Show it in the preview' : 'Hide it in the preview (the bake keeps it)'; },
        toggleEye: function (it) { var n = Object.assign({}, hidden); n[it.id] = !n[it.id]; setHidden(n); },
        drag: { start: function () {}, over: function () {}, drop: function () {}, end: function () {} },
      }) });
    }

    /* ---- the right pane ---- */
    var right = step === 'render'
      ? h(R.Fragment, null, h('div', { className: 'fl-sec' }, 'Render'),
          h('div', { className: 'fl-list' }, h(S.RenderPanel, {
            badge: S.badgeProps(verdict, 'The live rows against the 32 an app seats; the rate from the bake’s estimate once baked'),
            send: { on: send, note: 'bake “' + d.name + '” and store it on the orb', tip: 'Bake, then send to the orb this editor was opened for' },
            downloads: [{ label: 'Download .orbtheme', note: 'the baked file', on: download }, { label: 'Check the bake', note: 'bake now and price it', on: bakeNow }],
          }),
          h('div', { className: 'fl-act' }, h('button', { onClick: saveDraft }, 'Save draft to the library'))))
      : step === 'start'
        ? h('div', { className: 'fl-list' }, h('div', { className: 'ui-mut' }, 'Pick a starting point on the left, or open one from the hub library. Then walk the steps: Scope, Arm, Planes, Words, and Render to send it.'))
        : h(S.Fields, { fields: fields });

    /* ---- the frame ---- */
    var stageCss = view === 'true' ? V.css : view === 'fit' ? fitPx : Math.round(G * V.k);
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
        h(S.Glass, { size: stageCss, onFit: setFitPx, zoomTag: V.tag, status: status },
          h('canvas', { ref: canvasRef, width: G, height: G, className: 'fl-canvas' })),
        h(S.Toolbar, {
          play: { icon: playing ? '❚❚' : '▶', label: playing ? 'Pause the traffic' : 'Play the traffic', on: function () { setPlaying(!playing); } },
          view: T.VIEWS.map(function (v) { return { label: v[1], tip: v[2], cls: v[0] === view ? 'on' : '', pick: function () { setView(v[0]); } }; }),
          zoom: view === 'zoom' ? T.ZOOMS.map(function (z) { return { label: z + '×', cls: z === zoom ? 'on' : '', pick: function () { setZoom(z); } }; }) : null,
          render: T.RENDERS.map(function (r) { return { label: r[1], tip: r[2], cls: r[0] === rend ? 'on' : '', pick: function () { setRend(r[0]); } }; }),
          wipe: rend === 'compare' ? { value: wipe, on: function (e) { setWipe(+e.target.value); } } : null,
        }, h('button', { className: cardOn ? 'on' : '', onClick: function () { setCardOn(!cardOn); }, title: 'Show the info card and the selection ring, as Info mode does' }, 'Card')),
        view === 'true' ? h(S.Calibrate, S.calProps(V, {
          min: T.screen.MIN, max: T.screen.MAX, step: T.screen.step(cal), value: cal,
          onChange: function (e) { var v = T.screen.save(+e.target.value); setCal(v); },
          up: function () { setCal(T.screen.save(cal + T.screen.step(cal))); }, down: function () { setCal(T.screen.save(cal - T.screen.step(cal))); },
          solve: function (e) { var v = T.screen.fromRuler(cal, +e.target.value); if (v) setCal(T.screen.save(v)); },
          solveCard: function (e) { var v = T.screen.fromCard(cal, +e.target.value); if (v) setCal(T.screen.save(v)); },
          reset: function () { setCal(T.screen.save(1)); },
        })) : null),
      h(S.Pane, { side: 'right' }, right),
      h(S.SendModal, S.sendProps(sending, status, function () { sender.cancel(); })),
      h(S.DupModal, S.dupProps(dup, function () { sender.replace(); }, function () { sender.dismiss(); })));
  }

  root.FlightsEditor = { Editor: Editor, drawFrame: drawFrame, checkDesign: checkDesign, TRAFFIC: TRAFFIC };
  if (typeof document !== 'undefined' && document.getElementById('root'))
    root.ReactDOM.createRoot(document.getElementById('root')).render(h(Editor));
})(typeof window !== 'undefined' ? window : globalThis);
