/* weather-editor.js — the weather app's theme editor (docs/weather-plan.md §2.6).
 *
 * On the OS's kit (theme-editor-kit-plan) as the flights editor does it: Start (the six
 * starter themes and Blank), Ground, Icons (the theme's own pack and its colours),
 * Temperature, Words, and Render to send it. The app's places are its settings, not its
 * theme: they live on the hub's App settings page (settings.json names them, owner
 * 2026-11-06), never here.
 * The preview draws the app's four screens (Now, Places, Hourly, 5 days) from the very
 * frames the bake stores, laid out as apps/weather/main.c poses them. Plain React,
 * createElement only. */
(function (root) {
  'use strict';
  var R = root.React, h = R.createElement, S = root.OrbStudio, T = root.OrbTheme, B = root.WeatherBake;
  var G = 466, CX = 233, ROW_CAP = 32;
  var STEPS = [['start', 'Start'], ['ground', 'Ground'], ['icons', 'Icons'], ['temps', 'Temperature'], ['words', 'Words'], ['render', 'Render']];
  var VIEWS = [['now', 'Now', 'Home on the glass, unarmed'], ['places', 'Places', 'Armed: turning steps the places'],
    ['hours', 'Hourly', 'A press: the next twelve hours on a clock face, the cursor on one'], ['days', '5 days', 'Another press: the next five days']];
  // the preview's made-up forecast (°C), the mockups' own
  var NOW = { name: 'Home', kind: 'partly', t: 17, feels: 15, wind: 14, hi: 18, lo: 10 };
  var PLACE2 = { name: 'Lisbon', kind: 'clear', t: 22, feels: 22, wind: 9, hi: 24, lo: 15 };
  var HOURS = [[14, 17, 'partly', 10], [15, 18, 'partly', 10], [16, 18, 'cloudy', 20], [17, 17, 'cloudy', 40], [18, 16, 'rain', 70], [19, 15, 'rain', 80],
    [20, 14, 'drizzle', 55], [21, 13, 'cloudy', 30], [22, 12, 'partlyN', 10], [23, 12, 'clearN', 5], [0, 11, 'clearN', 0], [1, 11, 'clearN', 0]];
  var DAYS = [['Today', 'rain', 10, 18, 80], ['Sat', 'partly', 10, 18, 10], ['Sun', 'clear', 7, 17, 0], ['Mon', 'cloudy', 9, 15, 40], ['Tue', 'thunder', 11, 16, 90]];

  var clone = function (o) { return JSON.parse(JSON.stringify(o)); };
  var getIn = function (o, path) { return path.split('.').reduce(function (x, k) { return x == null ? x : x[k]; }, o); };
  var setIn = function (d, path, v) {
    var n = clone(d), o = n, ks = path.split('.');
    for (var i = 0; i < ks.length - 1; i++) o = o[ks[i]];
    o[ks[ks.length - 1]] = v;
    return n;
  };
  function checkDesign(x) {
    if (!x || !x.plate || !x.words || !x.temps || !x.icons) return 'that design has no weather theme in it';
    if (!B.PACKS[x.icons.pack]) return 'that design names an icon pack this editor does not carry (' + x.icons.pack + ')';
    if (!Array.isArray(x.temps.ramp) || x.temps.ramp.length !== 3) return 'that design’s temperature colours are not three stops';
    return null;
  }

  /* ---- the preview: one screen, drawn from the bake's own frames ---- */
  function usePart(key, make, say) {
    var st = R.useState(null), gen = R.useRef(0);
    R.useEffect(function () {
      var my = ++gen.current;
      make().then(function (v) { if (my === gen.current) st[1](v); }, function (e) { say('The preview could not draw: ' + (e && e.message ? e.message : e)); });
    }, [key]);
    return st[0];
  }
  function useParts(d, say) {
    var plate = usePart(JSON.stringify(d.plate), function () { return B.plateCanvas(d); }, say);
    var icons = usePart(JSON.stringify(d.icons) + d.plate.c1, async function () { return { big: await B.iconFrames(d, B.ICON_PX), small: await B.iconFrames(d, B.SICON_PX) }; }, say);
    var temps = usePart(JSON.stringify(d.temps) + JSON.stringify(d.words), async function () { return { bars: B.barFrames(d), days: B.dayFrames(d), dots: B.dotFrames(d) }; }, say);
    var words = usePart(JSON.stringify(d.words), async function () {
      var W = d.words;
      try { await document.fonts.load('16px "' + W.text + '"'); await document.fonts.load('14px "' + W.label + '"'); await document.fonts.load('44px "' + W.num + '"'); } catch (e) { /* fallback */ }
      return true;
    }, say);
    return { plate: plate, icons: icons, temps: temps, words: words };
  }
  var KIND = function (k) { return B.KINDS.indexOf(k); };
  function drawView(x, d, P, view) {
    x.clearRect(0, 0, G, G);
    if (P.plate) x.drawImage(P.plate, 0, 0); else { x.fillStyle = '#000'; x.fillRect(0, 0, G, G); }
    if (!P.icons || !P.temps || !P.words) return;
    var W = d.words, caps = function (s) { return W.caps ? s.toUpperCase() : s; };
    var text = function (s, y, face, px, col, al, xx, op) {
      x.globalAlpha = op == null ? 1 : op;
      x.font = px + 'px "' + face + '", sans-serif'; x.fillStyle = col; x.textAlign = al || 'center'; x.textBaseline = 'middle';
      x.fillText(s, xx == null ? CX : xx, y);
      x.globalAlpha = 1;
    };
    var blit = function (c, cx, cy, op) { x.globalAlpha = op == null ? 1 : op; x.drawImage(c, cx - c.width / 2, cy - c.height / 2); x.globalAlpha = 1; };
    if (view === 'now' || view === 'places') {
      var p = view === 'now' ? NOW : PLACE2;
      text(view === 'places' ? p.name : caps(p.name), 58, view === 'places' ? W.text : W.label, view === 'places' ? 16 : 14, view === 'places' ? W.ink : W.quiet);
      blit(P.temps.dots[6 + (view === 'places' ? 1 : 0)], CX, 88);
      blit(P.icons.big[KIND(p.kind)], CX, 150);
      text(p.t + '°', 236, W.num, 44, W.ink, 'center', CX + 6);
      text(p.kind === 'clear' ? 'Clear' : 'Partly cloudy', 292, W.text, 16, W.ink);
      text(caps('Feels ' + p.feels + '°   Wind ' + p.wind + ' km/h'), 318, W.label, 14, W.quiet);
      text(caps('H ' + p.hi + '°   L ' + p.lo + '°'), 340, W.label, 14, W.quiet);
    } else if (view === 'hours') {
      var lo = 11, hi = 18, sel = 3;
      HOURS.forEach(function (hr, k) {
        var L = Math.max(0, Math.min(B.BAR_LEN - 1, Math.round((hr[1] - lo) / (hi - lo) * (B.BAR_LEN - 1))));
        var rain = hr[3] < 30 ? 0 : hr[3] < 70 ? 1 : 2, f = P.temps.bars[L * B.BAR_RAIN + rain];
        var a = (hr[0] % 12) * 30 * Math.PI / 180;
        x.save(); x.globalAlpha = k === sel ? 1 : 120 / 255;
        x.translate(CX + 157 * Math.sin(a), CX - 157 * Math.cos(a)); x.rotate(a);
        x.drawImage(f, -f.width / 2, -f.height / 2); x.restore(); x.globalAlpha = 1;
      });
      var s = HOURS[sel];
      text('17:00', 146, W.label, 14, W.quiet);
      blit(P.icons.big[KIND(s[2])], CX, 196);
      text(s[1] + '°', 256, W.num, 44, W.ink, 'center', CX + 6);
      text(caps('Rain ' + s[3] + '%   Cloudy'), 306, W.label, 14, W.quiet);
    } else {
      text(caps('5 days'), 62, W.label, 14, W.quiet);
      var wlo = 7, whi = 18, pos = function (v) { return Math.round((v - wlo) / (whi - wlo) * (B.DAY_POS - 1)); };
      DAYS.forEach(function (dy, i) {
        var y = 120 + 56 * i, a = pos(dy[2]), b = pos(dy[3]), fi = B.DAY_POS * a - a * (a - 1) / 2 + (b - a);
        text(dy[0], y, W.text, 16, W.ink, 'left', 78);
        blit(P.icons.small[KIND(dy[1])], 158, y);
        text(dy[2] + '°', y, W.text, 16, W.ink, 'right', 210, 170 / 255);
        blit(P.temps.days[fi], 258, y);
        text(dy[3] + '°', y, W.text, 16, W.ink, 'left', 304);
        text(dy[4] + '%', y, W.text, 16, W.ink, 'right', 390, 170 / 255);
      });
    }
  }

  var thumbs = {};
  function drawThumb(k, c) {
    var put = function (im) { var x = c.getContext('2d'); x.clearRect(0, 0, c.width, c.height); x.drawImage(im, 0, 0, c.width, c.height); };
    if (thumbs[k] && thumbs[k].im) { put(thumbs[k].im); return; }
    if (!thumbs[k]) thumbs[k] = { p: B.thumb(B.preset(k), 192).then(function (im) { thumbs[k].im = im; return im; }) };
    thumbs[k].p.then(put, function () { /* the tile stays dark */ });
  }

  var Q = (function () { try { return new URLSearchParams(root.location.search); } catch (e) { return null; } })();
  var q = function (k, ok, dflt) { var v = Q && Q.get(k); return v && ok.indexOf(v) >= 0 ? v : dflt; };
  var col = function (pairs) { return pairs.map(function (p) { return p[0]; }); };

  function Editor() {
    var u = function (v) { return R.useState(v); };
    var dS = u(function () { return B.preset(q('preset', B.PRESETS.concat(['blank']), 'scope')); }), d = dS[0], setD = dS[1];
    var stepS = u(q('step', col(STEPS), 'start')), step = stepS[0], setStep = stepS[1];
    var stS = u(''), status = stS[0], setStatus = stS[1];
    var viewS = u(q('show', col(VIEWS), 'now')), view = viewS[0], setView = viewS[1];
    var sendS = u(null), sending = sendS[0], setSending = sendS[1];
    var dupS = u(null), dup = dupS[0], setDup = dupS[1];
    var libS = u(0), setLibTick = libS[1];
    var delS = u(null), mineDel = delS[0], setMineDel = delS[1];
    var drafts = R.useRef({}).current, canvasRef = R.useRef(null), stateRef = R.useRef({});
    var say = R.useCallback(function (s) { setStatus(s); }, []);
    stateRef.current = { d: d, status: status };

    var scrRef = R.useRef(null);
    var vw = R.useMemo(function () {
      return S.viewer({ get: function () { return scrRef.current; }, set: function (p) { setScr(function (o) { return Object.assign({}, o, p); }); }, say: say });
    }, []);
    var scrS = u(function () { return vw.init(q('view', col(T.VIEWS), 'fit')); }), setScr = scrS[1];
    scrRef.current = scrS[0];

    var parts = useParts(d, say);
    var ses = R.useMemo(function () {
      return T.session({
        slug: 'weather', keys: function () { return ['d']; }, state: function () { return stateRef.current; },
        set: function () { setLibTick(function (n) { return n + 1; }); }, ready: function () { return true; }, say: say,
        restore: function (snap, text) { setD(snap.d); say(text); }, schema: function () { return B.SCHEMA; },
        migrate: function () { return {}; },
        content: async function () { return { design: stateRef.current.d }; },
        apply: function (doc) { var why = checkDesign(doc.design); if (!why) { setD(B.complete(doc.design)); setStep(function (s) { return s === 'start' ? 'ground' : s; }); } return why; },
        bake: async function () { var r = await bakeNow(); return r ? r.bytes : null; },
      });
    }, []);
    var sender = R.useMemo(function () {
      return T.sender({ say: say, ask: function (x) { setDup(x); }, sending: function (x) { setSending(x); } });
    }, []);
    R.useEffect(function () { ses.track(); }, [d]);
    R.useEffect(function () { ses.boot().catch(function () {}).then(function () { return ses.openTheme(); }).catch(function () {}); }, []);
    var upd = function (path, v) { setD(function (old) { return setIn(old, path, v); }); };
    var base = R.useMemo(function () { return B.preset(d.base === 'blank' || B.PRESETS_DEF[d.base] ? d.base : 'scope'); }, [d.base]);

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
        var dg = new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode('weather/' + nm)));
        return root.GorbBake.cardStem('weather', nm, dg);
      } catch (e) { return null; }
    }
    async function draftThumb() {
      try {
        var im = await B.thumb(stateRef.current.d, 192), c = document.createElement('canvas'); c.width = c.height = 96;
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
      T.download(new Blob([r.bytes], { type: 'application/octet-stream' }), ((await stemOf(d.name)) || 'weather-theme') + '.orbtheme');
    }
    async function saveDraft() { await ses.saveDraft({ name: d.name, doc: await ses.doc(true), thumb: await draftThumb() }); }

    R.useEffect(function () {
      var c = canvasRef.current; if (!c) return;
      drawView(c.getContext('2d'), d, parts, view);
    });
    var verdict = T.fit({ rows: B.LIVE_ROWS, cap: ROW_CAP });

    /* ---- the fields of each step ---- */
    var K = S.fieldKit({ drafts: drafts, refresh: function () { setLibTick(function (n) { return n + 1; }); }, scope: function () { return step; } });
    var opt = function (pairs) { return pairs.map(function (p) { return { v: p[0], label: p[1] }; }); };
    var C = function (label, path, presetPath) {
      return K.Co(label, getIn(d, path), function (nv) { upd(path, nv); }, '', path, { noAuto: true, preset: getIn(base, presetPath || path) });
    };
    var faces = B.FACES.map(function (k) { return { v: k, label: k }; });
    var fields = [];
    if (step === 'ground') {
      var P = d.plate;
      fields.push(K.H('Ground'));
      fields.push(K.Ch('Look', P.style, opt(Object.keys(B.GROUNDS).map(function (k) { return [k, B.GROUNDS[k]]; }).concat([['plain', 'Plain']])), function (v) {
        setD(function (o) { var n = clone(o), src = B.PRESETS_DEF[v]; n.plate = src ? clone(src.plate) : Object.assign({}, o.plate, { style: v }); return n; });
      }));
      fields.push(C('Centre', 'plate.c0')); fields.push(C('Face', 'plate.c1')); fields.push(C('Rim', 'plate.c2'));
      fields.push(C(P.style === 'brass' ? 'Bezel metal' : P.style === 'celestial' ? 'Nebula' : 'Detail', 'plate.detail'));
      fields.push(C('Ticks', 'plate.tick'));
      fields.push(K.Nt('', 'The ground is one picture under every screen; the icons draw over its face colour.'));
    } else if (step === 'icons') {
      var I = d.icons, pk = B.PACKS[I.pack];
      fields.push(K.H('Icon pack'));
      fields.push(K.Ch('Pack', I.pack, opt(Object.keys(B.PACKS).map(function (k) { return [k, B.PACKS[k].title]; })), function (v) {
        setD(function (o) { var n = clone(o); n.icons = { pack: v, c: B.packStart(v) }; return n; });
      }));
      fields.push(K.Nt('', pk.note + '. The theme keeps its own copy of the pack, in its own colours.'));
      fields.push(K.H('Colours'));
      pk.roles.forEach(function (r) {
        fields.push(K.Co(r[1], I.c[r[0]], function (nv) { upd('icons.c.' + r[0], nv); }, '', 'icons.' + r[0], { noAuto: true, preset: B.packStart(I.pack)[r[0]] }));
      });
      fields.push(K.Nt('', 'Every condition at the sizes the orb draws: ' + B.ICON_PX + ' px on Now and Hourly, ' + B.SICON_PX + ' px on the days. Small icons carry two of shape, colour and detail, never all three.'));
    } else if (step === 'temps') {
      fields.push(K.H('Temperature colours'));
      fields.push(K.Co('Cold', d.temps.ramp[0], function (v) { var s = d.temps.ramp.slice(); s[0] = v; upd('temps.ramp', s); }, '', 'ramp0', { noAuto: true, preset: base.temps.ramp[0] }));
      fields.push(K.Co('Mild', d.temps.ramp[1], function (v) { var s = d.temps.ramp.slice(); s[1] = v; upd('temps.ramp', s); }, '', 'ramp1', { noAuto: true, preset: base.temps.ramp[1] }));
      fields.push(K.Co('Hot', d.temps.ramp[2], function (v) { var s = d.temps.ramp.slice(); s[2] = v; upd('temps.ramp', s); }, '', 'ramp2', { noAuto: true, preset: base.temps.ramp[2] }));
      var lum = function (c) { var n = parseInt(c.slice(1), 16), f = function (v) { v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); }; return 0.2126 * f(n >> 16) + 0.7152 * f((n >> 8) & 255) + 0.0722 * f(n & 255); };
      var Ls = d.temps.ramp.map(lum), spread = Math.max.apply(null, Ls) - Math.min.apply(null, Ls);
      fields.push(K.Nt('', spread < 0.08 ? 'These three are close in lightness: a colour-blind eye may not tell cold from hot. Spread them.' : 'The hourly bars and the day bars run cold to hot through these.'));
      fields.push(C('Rain', 'temps.rain'));
      fields.push(C('Bar track', 'temps.track'));
      fields.push(K.Ch('Bar ends', d.temps.cap, opt([['round', 'Round'], ['butt', 'Square']]), function (v) { upd('temps.cap', v); }));
    } else if (step === 'words') {
      var W = d.words;
      fields.push(K.H('Typefaces'));
      fields.push(K.Sl('Figures', W.num, faces, function (v) { upd('words.num', v); }));
      fields.push(K.Sl('Words', W.text, faces, function (v) { upd('words.text', v); }));
      fields.push(K.Sl('Titles', W.label, faces, function (v) { upd('words.label', v); }));
      fields.push(K.Ch('Titles in', W.caps ? 'caps' : 'as written', opt([['caps', 'Capitals'], ['as written', 'As written']]), function (v) { upd('words.caps', v === 'caps'); }));
      fields.push(K.Rg('Letter spacing', W.track, 0, 2, 1, function (v) { upd('words.track', v); }, W.track + ' px'));
      fields.push(K.H('Colours'));
      fields.push(C('Words', 'words.ink')); fields.push(C('Quiet', 'words.quiet'));
      fields.push(K.Nt('', 'Quiet is the place name, the time and the details under the temperature; the lows and the rain column of the days are the words, dimmed.'));
    }

    var right = step === 'render'
      ? h(R.Fragment, null, h('div', { className: 'fl-sec' }, 'Render'),
          h('div', { className: 'fl-list' }, h(S.RenderPanel, {
            badge: S.badgeProps(verdict, 'The live rows against the 32 an app seats'),
            send: { on: send, note: 'save “' + d.name + '”, bake it and store it on the orb', tip: 'Save it to the hub library, bake it, then send it to the orb this editor was opened for.' },
            save: ses.hubOrb() ? { on: saveDraft, note: 'keep “' + d.name + '” on the hub as a draft', tip: 'Keep this design in the hub library under its name, without baking or sending it.' } : null,
            downloads: [{ label: 'Download .orbtheme', note: 'the baked file', on: download }, { label: 'Check the bake', note: 'bake now and check its size', on: bakeNow }],
          })))
      : step === 'start'
        ? h('div', { className: 'fl-list' }, h('div', { className: 'ui-mut' }, 'Pick a starting point, or open one from the hub library, then Continue. The steps after it: Ground, Icons, Temperature, Words, and Render to send it. The places the app shows are on the hub’s App settings page.'))
        : h('div', { className: 'fl-list' }, h(S.Fields, { fields: fields }));

    var items = [['ground', 'Ground'], ['icons', 'Icons'], ['temps', 'Temperature'], ['words', 'Words']].map(function (x) { return { id: x[0], band: 'parts', label: x[1], movable: false }; });
    var left = h(S.Layers, { title: 'Parts', rows: S.layerRows(items, {
      sel: step, heads: { parts: 'The theme, bottom to top' },
      icon: function (it) { return { ground: '◎', icons: '☀', temps: '▮', words: 'A' }[it.id]; },
      note: function (it) { return { ground: B.GROUNDS[d.plate.style] || 'Plain', icons: B.PACKS[d.icons.pack].title, temps: 'cold to hot', words: d.words.text }[it.id]; },
      hidden: function () { return false; }, move: function () { return { ok: false, reason: 'the theme’s parts keep their order' }; },
      onMove: function () {}, pick: function (it) { setStep(it.id); }, eyeTip: function () { return ''; }, toggleEye: function () {},
      drag: { start: function () {}, over: function () {}, drop: function () {}, end: function () {} },
    }) });

    var sheet = step === 'start' ? h(S.StartSheet, {
      title: 'Start a weather theme',
      scratch: { label: 'Blank', short: 'Plain greys, nothing preset', cls: d.base === 'blank' ? 'on' : '', tip: 'Every part present, in plain greys on a plain ground',
        on: function () { setD(B.preset('blank')); ses.restart(); say('Starting from a blank theme: Continue when you are ready'); } },
      tiles: B.PRESETS.map(function (k) {
        var pd = B.PRESETS_DEF[k];
        return { key: k, name: pd.name, short: B.PACKS[pd.icons.pack].title + ' icons · ' + pd.match, tip: 'Start from ' + pd.name + ': ' + B.PACKS[pd.icons.pack].note,
          cls: d.base === k ? 'on' : '', draw: function (c) { drawThumb(k, c); },
          on: function () { setD(B.preset(k)); ses.restart(); say('Starting from ' + pd.name + ': Continue when you are ready'); } };
      }),
      library: { accept: '.orbtheme,.orbdesign,.json',
        openFile: function (e) { var fl = e.target.files[0]; if (fl) ses.openFile(fl); },
        rows: S.libraryRows(ses.library(), { version: ses.version(), armed: mineDel, arm: setMineDel,
          meta: function (m) { return m.design && B.PACKS[m.design.icons && m.design.icons.pack] ? B.PACKS[m.design.icons.pack].title + ' · ' : ''; },
          open: function (nm) { ses.openEntry(nm); }, forget: function (nm, what) { ses.forgetEntry(nm, what); } }),
        empty: ses.hubOrb() ? null : 'The library is on the hub: open this editor from the hub to save and open designs there.' },
      foot: [{ label: 'Continue with ' + d.name, primary: true, on: function () { setStep('ground'); } }],
    }) : null;

    var g = vw.glass(), tool = vw.tool();
    return h(S.Frame, null,
      h(S.TopBar, {
        back: function () { try { root.parent.postMessage({ type: 'theme.close' }, '*'); } catch (e) { /* not framed */ } },
        backTip: 'Back to the Themes page', crumb: 'Weather',
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
          h(S.Seg, { opts: VIEWS.map(function (v) { return { label: v[1], tip: v[2], cls: v[0] === view ? 'on' : '', pick: function () { setView(v[0]); } }; }) }))),
      h(S.Pane, { side: 'right' }, right),
      h(S.OpeningModal, { line: ses.opening }),
      h(S.SendModal, S.sendProps(sending, status, function () { sender.cancel(); })),
      h(S.DupModal, S.dupProps(dup, function () { sender.replace(); }, function () { sender.dismiss(); })));
  }

  root.WeatherEditor = { Editor: Editor, drawView: drawView, checkDesign: checkDesign };
  if (typeof document !== 'undefined' && document.getElementById('root'))
    root.ReactDOM.createRoot(document.getElementById('root')).render(h(Editor));
})(typeof window !== 'undefined' ? window : globalThis);
