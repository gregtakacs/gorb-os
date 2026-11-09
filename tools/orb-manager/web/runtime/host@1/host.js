/* host@1 — the theme editor page Orb Manager owns (theme-editor-kit-plan §8, E1, E3).
 *
 * The hub serves this page for an app whose editor.json says "host": 1, and loads the app's
 * module after it. The module registers what only the app knows; everything else is here,
 * the same for every app: the start step (four ways, one at a time: new, a template, the
 * library, import), the Render step (save to library, send to orb, export), the stage with its frame
 * clock, its render modes and its view controls, Reset, the fit, history and the send. An
 * app cannot replace any of it.
 *
 *   OrbHost.register({
 *     slug, title,                   // 'radar', 'Radar': the library, the crumb, the start's title
 *     schema, migrate,               // the design schema, and its steps {n: fn} (optional)
 *     check(d), complete(d),         // why a design is not this app's (or null); fill its gaps (optional)
 *     blank: {note, make()},         // Start fresh
 *     templates: [{key, name, note, make()}],
 *     start,                         // optional: the template the editor opens on (else the first)
 *     startNote,                     // optional: a line for the start step's pane
 *     picture(d, px),                // async: the design drawn small, for tiles and the library
 *     pictureKey(),                  // optional: what the pictures are drawn over now; a new key
 *                                    // draws the start's tiles again (Radar's, once home's map comes)
 *     steps: [[key, label]],         // the editing steps, between Start and Render
 *     parts: [{id, label, icon, note(d), also: [step]}],   // the left pane, bottom to top; picking
 *                                    // one opens its step (also: other steps it stays lit for)
 *     eyes,                          // false: the parts cannot be hidden in the preview
 *     fields(step, c),               // the right pane's fields; c: {d, K, upd(path, v), set(fn), base,
 *                                    // say, getIn, view, env, ui, setUi}
 *     views: [{key, label, tip, kind, options, init, show(d, env), active(view), idle}],
 *                                    // what the preview shows, on the stage's upper row, each
 *                                    // under its caption (label): kind 'seg' (default), 'select',
 *                                    // or 'place' (home and the orb's set places, the host's own
 *                                    // list; home: false leaves home out, and none set, no control); options [[value, label, tip]] or a function (d, env,
 *                                    // {places, unit}) of them; show: when the control is there; active: when it
 *                                    // acts (idle: its tip when not). ?<key>= in the address
 *                                    // leads, then init, then the first option
 *     ui,                            // optional: the app's own preview state (a selection), outside history
 *     play: {fps(d), want(d), on(view, d), what},   // optional: a frame clock at fps, running while
 *                                    // on(view, d); want: the rate the orb is asked for (else fps);
 *                                    // what: 'the weather', for the button's tip
 *     paceBy: [viewKey | {key, with, label}],   // optional: the views whose options change what
 *                                    // moves; Render lists the estimated frame rate of each option,
 *                                    // the other views as on the glass but for `with` ({show: 'now'})
 *     layers: true,                  // optional: draw splits into 'under' and 'live', so the host
 *                                    // offers Panel and Split (the orb's blend) beside Design
 *     setup(c),                      // optional, once: c {say, orb, env(patch)} loads what draw needs
 *     draw(x, d, c),                 // one frame on the 466 px canvas; c {view, env, hidden, memo,
 *                                    // ui, step, t, dt, layer ('all'|'under'|'live'), part, redraw, say,
 *                                    // home, unit, places, place}
 *     pointer: {down(pt, c), move(pt, c), up(c), cursor(c)},   // optional: the glass's pointer, in glass px
 *     toDoc(d, withArt), fromDoc(doc),   // optional (§8.12): an app whose design files have a shape of
 *                                    // their own (the clock's): the document's own fields from a
 *                                    // design (async), and a design from a document (null: none in it)
 *     panes: {left(c), right(c), stage(c), tool(c), overlay(c)},   // optional (§8.12): the app's own React for a
 *                                    // slot, in place of the parts list, the fields, the canvas (then
 *                                    // draw is optional), or after the lower toolbar's controls;
 *                                    // c {d, step, goStep, view, env, ui, setUi, say, push, set, chosen, size};
 *                                    // overlay: over the stage, off the start (the clock's diagnostics)
 *     adopt(d),                      // optional (§8.12): a stateful app's design, handed to it when the
 *                                    // host changes it (a template, an opened design, Undo, Reset, the
 *                                    // first); what the app changes itself it hands back with c.push
 *     bake(d, {design}),             // async: {bytes}, the design baked to an .orbtheme (a throw says why not)
 *     fit(d, view),                  // {rows, cap, want, pace}: OrbTheme.fit's question
 *   })
 *
 * The places are the OS's (home and three more, set on the Orb tab): the host reads them
 * from the orb once, for every app, as {key, name, lat, lon, os, zone, offset_min} with home
 * named Home as the apps name it, offset_min its zone's offset now (null when the orb does
 * not say); c.home is home's, c.place the place view's choice (else home), c.unit the
 * orb's distance unit. An app reads its maps for them with maps@1.
 *
 * A design is the app's object, and it carries its theme's `name`; the host stamps `base`,
 * the start it came from. c.part(name, key, make, {fresh}) keeps a drawn part until its key
 * changes: make() may answer a promise, and the frame is drawn again when it lands; until
 * then the last one stands, or nothing with fresh (for a part others are keyed on). Plain
 * React.createElement, no JSX. */
(function (root) {
  'use strict';
  var R = root.React, h = R.createElement, S = root.OrbStudio, T = root.OrbTheme;
  var H = root.OrbHost = root.OrbHost || {};
  var G = 466;

  var list = function (a) { return Array.isArray(a) ? a : []; };
  var clone = function (o) { return JSON.parse(JSON.stringify(o)); };
  var getIn = function (o, path) { return path.split('.').reduce(function (x, k) { return x == null ? x : x[k]; }, o); };
  var setIn = function (d, path, v) {
    var n = clone(d), o = n, ks = path.split('.');
    for (var i = 0; i < ks.length - 1; i++) { if (o[ks[i]] == null) o[ks[i]] = {}; o = o[ks[i]]; }
    o[ks[ks.length - 1]] = v;
    return n;
  };
  var err = function (e) { return e && e.message ? e.message : String(e); };
  // a design's identity for the kept bake
  var keyOf = function (d) { try { return JSON.stringify(d); } catch (e) { return null; } };
  H.SIZE_PAUSE_MS = 1200;
  H.PACE_BASE = 60; H.PACE_MS = 500; H.PACE_WARM_MS = 1000;
  /* a view's measured rate, said: '~12 of 20 fps', 'still', 'measuring…' */
  H.paceWords = function (p) {
    if (!p || p.pending) return 'measuring…';
    if (p.still) return 'still: nothing moves';
    if (p.why) return 'not measured: ' + p.why;
    return '~' + p.est + ' of ' + p.want + ' fps';
  };
  // the live size, said (studio@1's, one wording for every editor)
  H.sizeWords = function (z, max) { return S.sizeWords(z, max); };
  function q(name) { try { return new URLSearchParams(root.location.search).get(name); } catch (e) { return null; } }
  H.getIn = getIn; H.setIn = setIn;

  var tmpl = function (A, k) { return list(A.templates).filter(function (x) { return x.key === k; })[0] || null; };
  // a start made into a design: the host stamps the start's key as its base
  function made(A, key) {
    var t = key === 'blank' ? null : tmpl(A, key);
    var d = (t || A.blank).make(); d.base = t ? key : 'blank';
    return d;
  }
  // what a colour row offers back as its preset, and what Reset returns to: the design's start
  function baseOf(A, d) { return made(A, tmpl(A, d.base) ? d.base : 'blank'); }
  // a view's options, fixed or of the design and what setup loaded
  var optsOf = function (w, d, env, geo) {
    if (w.kind === 'place') {
      var ps = geo && geo.places && geo.places.length ? geo.places : w.home === false ? [] : [{ key: 'home', name: 'Home' }];
      if (w.home === false) ps = ps.filter(function (p) { return p.key !== 'home'; });
      return ps.map(function (p) { return [p.key, p.name, p.os ? 'Place ' + p.os + ': ' + p.name : 'Home']; });
    }
    return typeof w.options === 'function' ? list(w.options(d, env || {}, geo || {})) : list(w.options);
  };
  /* the OS's places for an orb: home (named Home) and its set places, and its distance unit */
  H.places = async function (ip) {
    var get = async function (path) { var r = await fetch('/orb/' + ip + '/api/v1/' + path); if (!r.ok) throw new Error(path + ' answered ' + r.status); return r.json(); };
    var out = { places: [], unit: 'km' };
    var hm = await get('home');
    // each with its zone and its offset now (minutes east of UTC): home's the orb's own, a
    // place's its own zone's (OS v1.2.38; older firmware answers none, and the offset is null)
    var zoneOf = function (zone, off) { return { zone: zone || '', offset_min: typeof off === 'number' ? off : null }; };
    if (hm && hm.home && hm.home.set) out.places.push(Object.assign({ key: 'home', name: 'Home', lat: +hm.home.lat, lon: +hm.home.lon, os: 0, place: hm.home.place || '' },
      zoneOf(hm.home.zone || hm.zone_label, hm.tz_offset)));
    try {
      var pl = await get('places');
      list(pl && pl.places).forEach(function (p) {
        if (p && p.set) out.places.push(Object.assign({ key: 'p' + (p.i + 1), name: p.name || 'Place ' + (p.i + 1), lat: +p.lat, lon: +p.lon, os: p.i + 1 }, zoneOf(p.zone, p.tz_offset)));
      });
    } catch (e) { /* home alone */ }
    try { var un = await get('units'); if (un && /^(km|mi|nm)$/.test(un.dist)) out.unit = un.dist; } catch (e) { /* km */ }
    return out;
  };
  // the views' first options (or the address's): what the stage shows until a control is pressed
  H.viewInit = function (A, d) {
    var v = {};
    list(A.views).forEach(function (w) {
      var o = optsOf(w, d, {}), at = q(w.key), hit = o.filter(function (x) { return String(x[0]) === at; })[0];
      // a place named in the address is kept as given: the places are read after this
      v[w.key] = hit ? hit[0] : w.kind === 'place' && /^(home|p[1-3])$/.test(at || '') ? at : w.init != null ? w.init : o.length ? o[0][0] : null;
    });
    return v;
  };

  function Editor(p) {
    var A = p.app;
    var u = function (v) { return R.useState(v); };
    var STEPS = [['start', 'Start']].concat(list(A.steps)).concat([['render', 'Render']]);
    var first = list(A.steps).length ? A.steps[0][0] : 'render';
    var dS = u(function () {
      var k = q('preset');
      return made(A, k === 'blank' || tmpl(A, k) ? k : A.start && tmpl(A, A.start) ? A.start : (list(A.templates)[0] || { key: 'blank' }).key);
    }), d = dS[0], setD = dS[1];
    var stepS = u(function () { var s = q('step'); return STEPS.some(function (x) { return x[0] === s; }) ? s : 'start'; }), step = stepS[0], setStep = stepS[1];
    // the start's panel: '' the four ways, 'template' or 'library' (?start= opens one)
    var spS = u(function () { var v = q('start'); return v === 'template' || v === 'library' ? v : ''; }), startPick = spS[0], setStartPick = spS[1];
    // whether a design has been chosen: until then the start is all there is (no steps, name,
    // layers, readouts or Send, nothing baked or measured). New, a template's Continue, a
    // library entry, an import, or an address naming a design or a step chooses one.
    var chS = u(function () { var st = q('step'); return !!(q('preset') || (st && st !== 'start')); }), chosen = chS[0], setChosen = chS[1];
    var stS = u(''), status = stS[0], setStatus = stS[1];
    var viewS = u(function () { return H.viewInit(A, d); }), view = viewS[0], setView = viewS[1];
    var hidS = u({}), hidden = hidS[0], setHidden = hidS[1];
    var envS = u({}), env = envS[0], setEnv = envS[1];
    var geoS = u({ places: [], unit: 'km', read: false }), geo = geoS[0], setGeo = geoS[1];
    var uiS = u(function () { return Object.assign({}, A.ui || {}); }), ui = uiS[0], setUiS = uiS[1];
    var playS = u(true), playing = playS[0], setPlaying = playS[1];
    var rendS = u(function () { var r = q('render'); return A.layers && T.RENDERS.some(function (x) { return x[0] === r; }) ? r : 'design'; }), rend = rendS[0], setRend = rendS[1];
    var wipeS = u(233), wipe = wipeS[0], setWipe = wipeS[1];
    var armS = u(0), resetArm = armS[0] && Date.now() - armS[0] < 4000, setArm = armS[1];
    var sendS = u(null), sending = sendS[0], setSending = sendS[1];
    var dupS = u(null), dup = dupS[0], setDup = dupS[1];
    var askS = u(null), ask = askS[0], setAsk = askS[1];
    var tickS = u(0), setTick = tickS[1];
    var sizeS = u({ state: 'waiting' }), size = sizeS[0], setSize = sizeS[1];
    var paceS = u(0), setPaceTick = paceS[1], paces = R.useRef({}).current, paceRun = R.useRef({ chain: Promise.resolve() }).current;
    var baked = R.useRef({ key: null, r: null, ms: 0, chain: Promise.resolve() }).current;
    var delS = u(null), mineDel = delS[0], setMineDel = delS[1];
    var drafts = R.useRef({}).current, memo = R.useRef({}).current, thumbs = R.useRef({}).current, parts = R.useRef({}).current;
    var clock = R.useRef({ t: 0, dt: 0 }).current;
    var canvasRef = R.useRef(null), stateRef = R.useRef({}), live = R.useRef({});
    var say = R.useCallback(function (s) { setStatus(s); }, []);
    var redraw = R.useCallback(function () { setTick(function (n) { return n + 1; }); }, []);
    var setUi = R.useCallback(function (x) { setUiS(function (o) { return Object.assign({}, o, typeof x === 'function' ? x(o) : x); }); }, []);
    stateRef.current = { d: d, status: status };
    live.current = { d: d, view: view, env: env, geo: geo, hidden: hidden, ui: ui, step: step, rend: rend, wipe: wipe };

    var scrRef = R.useRef(null);
    var vw = R.useMemo(function () {
      return S.viewer({ get: function () { return scrRef.current; }, set: function (x) { setScr(function (o) { return Object.assign({}, o, x); }); }, say: say });
    }, []);
    var scrS = u(function () { var v = q('view'); return vw.init(T.VIEWS.some(function (x) { return x[0] === v; }) ? v : 'fit'); }), setScr = scrS[1];
    scrRef.current = scrS[0];

    // home, the unit, the places, and the place view's choice (else home)
    var geoOf = function (g, v) {
      var pv = list(A.views).filter(function (w) { return w.kind === 'place'; })[0];
      var home = g.places.filter(function (p) { return p.key === 'home'; })[0] || null;
      // the control's choice; one the orb does not have is the control's first, as it shows
      var cand = pv ? g.places.filter(function (p) { return pv.home !== false || p.key !== 'home'; }) : [];
      var place = pv ? cand.filter(function (p) { return p.key === v[pv.key]; })[0] || cand[0] || (pv.home === false ? null : home) : home;
      return { home: home, unit: g.unit, places: g.places, place: place, placesRead: g.read };
    };
    var complete = function (x) { return A.complete ? A.complete(x) : x; };
    var ses = R.useMemo(function () {
      return T.session({
        slug: A.slug, keys: function () { return ['d']; }, state: function () { return stateRef.current; },
        set: redraw, ready: function () { return true; }, say: say,
        restore: function (snap, text) { setD(snap.d); say(text); },
        schema: function () { return A.schema; }, migrate: function () { return A.migrate || {}; },
        // an app whose files have a shape of their own converts both ways (toDoc, fromDoc)
        content: async function (withArt) { return A.toDoc ? A.toDoc(stateRef.current.d, withArt) : { design: stateRef.current.d }; },
        apply: function (doc) {
          var dd = A.fromDoc ? A.fromDoc(doc) : doc.design;
          var why = !dd ? 'that design carries no theme' : A.check(dd);
          if (!why) { setD(complete(dd)); setChosen(true); setStep(function (s) { return s === 'start' ? first : s; }); }
          return why;
        },
        bake: async function () { var r = await bakeNow(); return r ? r.bytes : null; },
      });
    }, []);
    var sender = R.useMemo(function () {
      return T.sender({ say: say, ask: function (x) { setDup(x); }, sending: function (x) { setSending(x); } });
    }, []);
    R.useEffect(function () { ses.track(); }, [d]);
    /* a stateful app (§8.12): the design it changes itself it pushes here (c.push); one the
     * host changes (a template, an opened design, Undo, Reset) it is handed (A.adopt), the
     * first one included */
    var fromApp = R.useRef(false);
    var push = R.useCallback(function (nd) { fromApp.current = true; setD(nd); }, []);
    R.useEffect(function () {
      if (fromApp.current) { fromApp.current = false; return; }
      if (A.adopt) A.adopt(d);
    }, [d]);
    R.useEffect(function () { ses.boot().catch(function () {}).then(function () { return ses.openTheme(); }).catch(function () {}); }, []);
    R.useEffect(function () {
      var o = ses.hubOrb(); if (!o) return;
      H.places(o.ip).then(function (g) { setGeo(Object.assign({ read: true }, g)); },
        function (e) { setGeo({ places: [], unit: 'km', read: true }); say('The orb’s home and places could not be read: ' + err(e)); });
    }, []);
    R.useEffect(function () {
      if (A.setup) A.setup({ say: say, orb: ses.hubOrb(), env: function (x) { setEnv(function (o) { return Object.assign({}, o, x); }); } });
    }, []);

    /* ---- the stage: the app draws a frame; the host keeps its parts, its clock and the blend ---- */
    // a drawn part, kept until its key changes; a promise draws the frame again when it lands
    var part = function (name, key, make, o) {
      var e = parts[name];
      if (e && e.key === key) return e.value;
      // fresh: nothing until this key's own lands (a part another part is keyed by)
      e = parts[name] = { key: key, value: e && !(o && o.fresh) ? e.value : null };
      var v;
      try { v = make(); } catch (x) { say('The preview could not draw: ' + err(x)); return e.value; }
      if (v && typeof v.then === 'function') {
        v.then(function (r) { if (parts[name] === e) { e.value = r; redraw(); } }, function (x) { say('The preview could not draw: ' + err(x)); });
        return e.value;               // the last one stands until the new one lands
      }
      e.value = v;
      return v;
    };
    var ctxOf = function (L, layer) {
      return Object.assign({ view: L.view, env: L.env, hidden: L.hidden, ui: L.ui, step: L.step, memo: memo, part: part, t: clock.t, dt: clock.dt,
        layer: layer, redraw: redraw, say: say, setUi: setUi }, geoOf(L.geo, L.view));
    };
    function drawNow() {
      var c = canvasRef.current; if (!c) return;
      var L = live.current, x = c.getContext('2d');
      try {
        if (!A.draw) return;                 // an app with its own stage draws it itself
        A.draw(x, L.d, ctxOf(L, 'all'));
        if (!A.layers || L.rend === 'design') return;
        // Panel and Split: the still layers as the bake stores them, the moving ones blended
        // over them the way the orb blends them (theme@1/preview's panel mirror)
        var off = function (layer) {
          var oc = document.createElement('canvas'); oc.width = oc.height = G;
          var ox = oc.getContext('2d'); A.draw(ox, L.d, ctxOf(L, layer));
          return ox.getImageData(0, 0, G, G).data;
        };
        T.panel(x, { under: off('under'), live: [{ px: off('live'), still: false }] }, { mode: L.rend, wipe: L.wipe });
      } catch (e) { say('The preview could not be drawn: ' + err(e)); }
    }
    R.useEffect(function () { drawNow(); });
    var playOn = !!(chosen && A.play && (!A.play.on || A.play.on(view, d)));
    var fps = A.play ? Math.max(1, +(typeof A.play.fps === 'function' ? A.play.fps(d) : A.play.fps) || 30) : 0;
    R.useEffect(function () {
      if (!playOn || !playing) { clock.dt = 0; return; }
      // dt is a tick's alone: a frame drawn for any other reason moves nothing
      var tk = T.ticker(fps, false, function () { clock.dt = 1000 / fps; clock.t += clock.dt; drawNow(); clock.dt = 0; });
      return function () { tk.stop(); clock.dt = 0; };
    }, [playOn, playing, fps]);

    // the glass's pointer, in glass px
    var ptOf = function (e) { var c = canvasRef.current, b = c.getBoundingClientRect(); return [(e.clientX - b.left) * G / b.width, (e.clientY - b.top) * G / b.height]; };
    var pc = function () { var L = live.current; return { d: L.d, view: L.view, env: L.env, ui: L.ui, step: L.step, set: setD, setUi: setUi, say: say }; };
    var P = A.pointer || null;
    var onDown = P && P.down ? function (e) {
      if (P.down(ptOf(e), pc())) { try { e.currentTarget.setPointerCapture(e.pointerId); } catch (x) { /* headless */ } }
    } : undefined;
    var onMove = P && P.move ? function (e) { P.move(ptOf(e), pc()); } : undefined;
    var onUp = P && P.up ? function () { P.up(pc()); } : undefined;

    var upd = function (path, v) { setD(function (old) { return setIn(old, path, v); }); };
    // what an app's own panes are given (A.panes, §8.12)
    var paneCtx = function () { return Object.assign({ d: d, step: step, goStep: setStep, view: view, env: env, ui: ui, setUi: setUi, say: say, push: push, set: setD, chosen: chosen }, geoOf(geo, view)); };
    var PN = A.panes || {};
    var base = R.useMemo(function () { return baseOf(A, d); }, [d.base]);

    /* ---- the bake, the send, the export ---- */
    // one bake at a time, and the kept one for the design it was made from: Send and Export
    // reuse the live readout's bake when nothing has changed since
    function bakeOf(dd) {
      var k = keyOf(dd);
      var run = function () {
        if (baked.key === k && baked.r) return baked.r;
        return (async function () {
          var doc = new TextEncoder().encode(JSON.stringify(A.toDoc ? await ses.doc(true) : Object.assign(await ses.doc(true), { design: dd })));
          var t0 = Date.now(), r = await A.bake(dd, { design: doc });
          baked.ms = Date.now() - t0; baked.key = k; baked.r = r;
          return r;
        })();
      };
      var p = baked.chain.then(run, run);
      baked.chain = p.catch(function () {});
      return p;
    }
    async function bakeNow() {
      var dd = stateRef.current.d;
      try {
        if (!(baked.key === keyOf(dd) && baked.r)) say('Baking “' + dd.name + '”…');
        var r = await bakeOf(dd);
        say('Baked “' + dd.name + '”: ' + Math.round(r.bytes.length / 1024) + ' KB');
        return r;
      } catch (e) { say('Not baked: ' + err(e)); return null; }
    }
    /* ---- the live size (E5): the design baked again a pause after the last edit. The pause
     * is twice the last bake (at least 1.2 s, at most 6 s), so a slow design is not baked at
     * every breath; an edit while it waits or bakes starts the wait again ---- */
    R.useEffect(function () {
      if (!ses.hubOrb() || !chosen) return;
      var k = keyOf(d);
      if (baked.key === k && baked.r) { setSize({ state: 'done', key: k, bytes: baked.r.bytes.length, ms: baked.ms }); return; }
      setSize(function (o) { return o.state === 'waiting' ? o : Object.assign({}, o, { state: 'waiting' }); });
      var id = setTimeout(function () {
        if (keyOf(stateRef.current.d) !== k) return;
        setSize(function (o) { return Object.assign({}, o, { state: 'baking' }); });
        bakeOf(stateRef.current.d).then(function (r) {
          if (keyOf(stateRef.current.d) === k) setSize({ state: 'done', key: k, bytes: r.bytes.length, ms: baked.ms });
        }, function (e) {
          if (keyOf(stateRef.current.d) === k) setSize({ state: 'error', key: k, why: err(e) });
        });
      }, Math.min(6000, Math.max(H.SIZE_PAUSE_MS, 2 * baked.ms)));
      return function () { clearTimeout(id); };
    }, [d, chosen]);

    /* ---- the frame rate (E6): what a view holds on the orb, from what its preview draws.
     * Half a second of the view is drawn at 60 fps on a canvas of its own, from a fresh memo
     * (the live preview's state is never touched) after a second's warm-up, and priced by
     * OrbTheme.pace. A view where the app does not play is still. Measured once per design
     * and view, after the size settles, one at a time. ---- */
    var wantOf = function (dd) {
      var w = A.play && A.play.want != null ? A.play.want : A.play && A.play.fps;
      return +(typeof w === 'function' ? w(dd) : w) || 0;
    };
    async function sampleView(dd, v) {
      var base = H.PACE_BASE, n = Math.round(base * H.PACE_MS / 1000) + 1, warm = Math.round(base * H.PACE_WARM_MS / 1000);
      var oc = document.createElement('canvas'); oc.width = oc.height = G;
      var ox = oc.getContext('2d', { willReadFrequently: true });
      var m = {}, t = 0, dt = 1000 / base, frames = [], L = live.current, noop = function () {};
      for (var k = 0; k < warm + n; k++) {
        A.draw(ox, dd, Object.assign({ view: v, env: L.env, hidden: {}, ui: L.ui, step: 'render', memo: m, part: part, t: t, dt: k ? dt : 0,
          layer: 'all', redraw: noop, say: noop, setUi: noop }, geoOf(L.geo, v)));
        if (k >= warm) frames.push(ox.getImageData(0, 0, G, G).data);
        t += dt;
        if (k % 6 === 5) await new Promise(function (r) { setTimeout(r, 0); });
      }
      return frames;
    }
    var paceKey = function (k, v) { return k + '|' + JSON.stringify(v); };
    // the Render table: each paceBy view's options, each a view to measure ([label, view])
    var paceRows = function (dd) {
      var by = list(A.paceBy).map(function (x) { return typeof x === 'string' ? { key: x } : x; });
      if (!by.length) return [{ label: null, rows: [['This view', view]] }];
      return by.map(function (b) {
        var w = list(A.views).filter(function (x) { return x.key === b.key; })[0];
        if (!w) return { label: b.key, rows: [] };
        return { label: b.label || w.label || b.key, rows: optsOf(w, dd, env, geo).map(function (o) {
          var v = Object.assign({}, view, b.with || {}); v[b.key] = o[0]; return [o[1], v]; }) };
      });
    };
    function paceOf(dd, k, v) {
      var pk = paceKey(k, v);
      if (paces[pk]) return paces[pk];
      var run = async function () {
        if (paces[pk] && paces[pk].done) return paces[pk];
        if (keyOf(stateRef.current.d) !== k) return null;          // overtaken by an edit
        var r;
        // an app with a pace model of its own (the clock's _devPace) and no frame clock of the
        // host's says its rate itself
        var own = !A.play ? A.fit(dd, v) : null;
        if (own && typeof own.pace === 'number' && own.want) r = { est: own.pace, want: own.want };
        else if (!A.play || (A.play.on && !A.play.on(v, dd))) r = { still: true };
        else { try { r = T.pace(await sampleView(dd, v), { base: H.PACE_BASE, want: wantOf(dd) }); } catch (e) { r = { why: err(e) }; } }
        paces[pk] = Object.assign({ done: true }, r);
        setPaceTick(function (n) { return n + 1; });
        return paces[pk];
      };
      var p = paceRun.chain.then(run, run);
      paceRun.chain = p.catch(function () {});
      paces[pk] = { pending: true };
      return paces[pk];
    }
    // the view on the glass, once the size settles; and on Render, every option of each paceBy view
    R.useEffect(function () {
      if (!ses.hubOrb() || size.state !== 'done' || size.key !== keyOf(d)) return;
      var dd = stateRef.current.d, k = size.key;
      paceOf(dd, k, view);
      if (step !== 'render') return;
      paceRows(dd).forEach(function (g) { g.rows.forEach(function (r) { paceOf(dd, k, r[1]); }); });
    }, [size.state, size.key, JSON.stringify(view), step]);
    async function stemOf(nm) {
      try {
        var dg = new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(A.slug + '/' + nm)));
        return root.GorbBake.cardStem(A.slug, nm, dg);
      } catch (e) { return null; }
    }
    async function thumbOf(dd) {
      try {
        var im = await A.picture(dd, 192), c = document.createElement('canvas'); c.width = c.height = 96;
        var x = c.getContext('2d'); x.fillStyle = '#000'; x.fillRect(0, 0, 96, 96); x.drawImage(im, 0, 0, 96, 96);
        return c.toDataURL('image/jpeg', 0.82);
      } catch (e) { return null; }
    }
    function send() {
      var nm = d.name;
      sender.send(nm, async function () {
        // the library keeps it first: a send the orb refuses still leaves the design on the hub
        await ses.saveDraft({ name: nm, doc: await ses.doc(true), thumb: await thumbOf(stateRef.current.d) }, true);
        var r = await bakeNow(); if (!r || sender.cancelled()) return;
        await sender.post(r.bytes, { name: nm, stem: await stemOf(nm), summary: Math.round(r.bytes.length / 1024) + ' KB' });
        ses.libraryRefresh();
      });
    }
    async function exportTheme() {
      var r = await bakeNow(); if (!r) return;
      var nm = ((await stemOf(d.name)) || A.slug + '-theme') + '.orbtheme';
      var w = await T.saveFile(new Blob([r.bytes], { type: 'application/octet-stream' }), nm);
      say(w === 'saved' ? 'Exported “' + d.name + '” as ' + nm : w === 'downloaded' ? 'Exported “' + d.name + '” to your Downloads folder as ' + nm : 'Not exported');
    }
    async function saveLib() { await ses.saveDraft({ name: d.name, doc: await ses.doc(true), thumb: await thumbOf(d) }); }
    // import: into the library first, then open; a name already there asks before it is replaced
    function importFile(fl) {
      ses.importFile(fl, {
        nameOf: function (doc) { var dd = A.fromDoc ? A.fromDoc(doc) : doc && doc.design; return dd && typeof dd.name === 'string' ? dd.name : ''; },
        thumb: function (doc) { return thumbOf(complete(A.fromDoc ? A.fromDoc(doc) : doc.design)); },
        confirm: function (nm) {
          return new Promise(function (res) {
            var done = function (v) { setAsk(null); res(v); };
            setAsk({ title: '“' + nm + '” is already in the library', yes: 'Replace it',
              body: 'Importing replaces the library’s “' + nm + '” with this file, its unsent changes included. No orb is touched.',
              on: function () { done(true); }, cancel: function () { done(false); } });
          });
        },
      });
    }
    // Reset: a second press within 4 s puts the design back to its start, its name kept, undoable
    var baseName = d.base === 'blank' || !tmpl(A, d.base) ? 'Blank' : tmpl(A, d.base).name;
    function reset() {
      if (!resetArm) { setArm(Date.now()); say('Press Reset again to put “' + d.name + '” back to ' + baseName); return; }
      setArm(0);
      setD(function (o) { var n = complete(baseOf(A, o)); n.name = o.name; return n; });
      say('Reset to ' + baseName + ' — Undo brings your version back');
    }

    if (!ses.hubOrb()) return h('div', { className: 'st-sheet' }, h('div', { className: 'st-sheet-in' },
      h('h2', null, A.title + ' theme editor'), h('div', { className: 'ui-mut' }, 'Open this editor from Orb Manager: its Themes tab has an Edit for each app.')));

    var paceNow = size.state === 'done' && size.key === keyOf(d) ? paces[paceKey(size.key, view)] : null;
    var verdict = T.fit(Object.assign({}, A.fit(d, view), paceNow && paceNow.done && paceNow.est ? { pace: paceNow.est, want: paceNow.want } : {}));
    var sz = H.sizeWords(size, root.GorbBake && root.GorbBake.THEME_UPLOAD_MAX), fit = S.fitProps(verdict, verdict.note + ' · ' + sz.line);
    fit = Object.assign({}, fit, { text: fit.text + ' · ' + sz.short }, sz.over ? { dot: '#f85149' } : {});

    /* ---- the right pane: the step's fields, the start's hint, or Render ---- */
    var K = S.fieldKit({ drafts: drafts, refresh: redraw, scope: function () { return step; } });
    var right;
    if (step === 'render') right = h('div', { className: 'hs-list' },
      h(S.RenderPanel, {
        // the live size and each paceBy view's rate lead the Render step, in every app
        blocks: [
          { title: 'Size', body: h('span', null, sz.line), over: sz.over, tip: 'The theme baked again a moment after your last edit: what Send to orb and Export send.' },
          { title: 'Frame rate', tip: 'What each view holds on the orb, estimated from what its preview moves, priced as the orb’s compositor paints it.',
            body: paceRows(d).map(function (g, gi) {
              return h('div', { key: gi, className: 'hs-pace' }, g.label ? h('span', { className: 'ui-mut' }, g.label) : null,
                g.rows.map(function (r, i) {
                  var pz = size.state === 'done' && size.key === keyOf(d) ? paces[paceKey(size.key, r[1])] : null;
                  return h('span', { key: i }, h('i', null, r[0]), ' ', size.state === 'done' ? H.paceWords(pz) : 'when the size is measured');
                }));
            }) },
        ],
        badge: S.badgeProps(verdict, verdict.note),
        save: { on: saveLib, note: 'keep “' + d.name + '” in the library', tip: 'Keep this design in the hub library under its name, without baking or sending it. Under a theme’s name it is that theme’s unsent changes until Send to orb bakes them in.' },
        send: { on: send, note: 'save “' + d.name + '”, bake it and store it on the orb', tip: 'Save it to the hub library, bake it, then send it to the orb this editor was opened for.' },
        downloads: [
          { label: 'Export .orbtheme', note: 'bake it and save the file where you choose', on: exportTheme, title: 'The same .orbtheme Send to orb bakes, saved as a file: ready for any orb’s Themes tab, with its design inside so the editor can open it again.' },
        ],
      }));
    else if (step === 'start') right = h('div', { className: 'hs-list' }, h('div', { className: 'ui-mut' },
      'Start new, from one of the app’s templates, from a design in your library, or from a file you import. The steps after it: ' +
      list(A.steps).map(function (s) { return s[1]; }).join(', ') + ', and Render to save, send or export it.' + (A.startNote ? ' ' + A.startNote : '')));
    else {
      var fields = [];
      if (PN.right) right = PN.right(paneCtx());
      else try { fields = A.fields(step, Object.assign({ d: d, K: K, upd: upd, set: setD, base: base, say: say, getIn: getIn, view: view, env: env, ui: ui, setUi: setUi }, geoOf(geo, view))) || []; }
      catch (e) { fields = [K.Nt('', 'This step could not be drawn: ' + err(e))]; }
      if (!PN.right) right = h('div', { className: 'hs-list' }, h(S.Fields, { fields: fields }));
    }

    /* ---- the left pane: the app's parts ---- */
    var items = list(A.parts).map(function (x) { return { id: x.id, band: 'parts', label: x.label, movable: false }; });
    var partOf = function (id) { return list(A.parts).filter(function (x) { return x.id === id; })[0] || {}; };
    var lit = (list(A.parts).filter(function (x) { return x.id === step || list(x.also).indexOf(step) >= 0; })[0] || {}).id || step;
    var eyes = A.eyes !== false;
    var left = items.length ? h(S.Layers, { title: 'Parts', rows: S.layerRows(items, {
      sel: lit, heads: { parts: 'Bottom to top' },
      icon: function (it) { return partOf(it.id).icon || ''; },
      note: function (it) { var n = partOf(it.id).note; try { return n ? n(d) : ''; } catch (e) { return ''; } },
      hidden: function (it) { return eyes && !!hidden[it.id]; }, move: function () { return { ok: false, reason: 'these parts keep their order' }; },
      onMove: function () {}, pick: function (it) { if (STEPS.some(function (s) { return s[0] === it.id; })) setStep(it.id); },
      eyeTip: function (it) { return !eyes ? '' : hidden[it.id] ? 'Show it in the preview' : 'Hide it in the preview (the bake keeps it)'; },
      toggleEye: function (it) { if (!eyes) return; var n = Object.assign({}, hidden); n[it.id] = !n[it.id]; setHidden(n); },
      drag: { start: function () {}, over: function () {}, drop: function () {}, end: function () {} },
    }) }) : null;

    /* ---- the start sheet: Start fresh, the templates, the library, import ---- */
    var drawThumb = function (k, c) {
      if (!c) return;
      var paint = function (im) { var x = c.getContext('2d'); x.clearRect(0, 0, c.width, c.height); x.drawImage(im, 0, 0, c.width, c.height); };
      var tk = k + '|' + (A.pictureKey ? A.pictureKey() : '');
      if (thumbs[tk] && thumbs[tk].im) { paint(thumbs[tk].im); return; }
      if (!thumbs[tk]) thumbs[tk] = { p: A.picture(made(A, k), 192).then(function (im) { thumbs[tk].im = im; return im; }) };
      thumbs[tk].p.then(paint, function () { /* the tile stays dark */ });
    };
    var pick = function (k, what) { setD(made(A, k)); ses.restart(); say('Starting from ' + what + ': Continue when you are ready'); };
    // the start (§8.10): four ways, one at a time; New goes straight to editing
    var sheet = step === 'start' ? h(S.StartFlow, {
      title: 'Start a ' + A.title.toLowerCase() + ' theme',
      pick: startPick, setPick: setStartPick,
      fresh: { note: (A.blank.note ? A.blank.note + ': ' : '') + 'a blank design, every part present',
        on: function () { setD(made(A, 'blank')); ses.restart(); say('A new blank design'); setChosen(true); setStep(first); } },
      tiles: list(A.templates).map(function (t) {
        return { key: t.key, name: t.name, short: t.note, tip: 'Start from ' + t.name + ': ' + t.note,
          cls: d.base === t.key ? 'on' : '', draw: function (c) { drawThumb(t.key, c); }, on: function () { pick(t.key, t.name); } };
      }),
      library: {
        rows: S.libraryRows(ses.library(), { version: ses.version(), armed: mineDel, arm: setMineDel,
          meta: function (m) { var t = m.design && tmpl(A, m.design.base); return t ? t.name + ' · ' : ''; },
          open: function (nm) { ses.openEntry(nm); }, forget: function (nm, what) { ses.forgetEntry(nm, what); } }),
        empty: 'Nothing saved yet. Save to library on the Render step keeps a design here; Send to orb bakes it into a theme under the same name; Import a file adds one.' },
      importFile: { accept: '.orbtheme,.orbdesign,.json', on: function (e) { var fl = e.target.files[0]; e.target.value = ''; if (fl) importFile(fl); } },
      foot: [{ label: 'Continue with ' + d.name, primary: true, on: function () { setChosen(true); setStep(first); } }],
    }) : null;

    /* ---- the stage's controls: the app's views, as it declares them ---- */
    var g = vw.glass(), tool = vw.tool();
    var pickView = function (k, v) { setView(function (o) { var n = Object.assign({}, o); n[k] = v; return n; }); };
    var viewCtl = list(A.views).map(function (w) {
      if (w.show && !w.show(d, env)) return null;
      var opts = optsOf(w, d, env, geo), on = !w.active || w.active(view);
      if (!opts.length) return null;
      var cur = opts.some(function (o) { return o[0] === view[w.key]; }) ? view[w.key] : opts.length ? opts[0][0] : null;
      var tip = on ? (w.tip || w.label || '') : (w.idle || '');
      var ctl = w.kind === 'select' || (w.kind === 'place' && opts.length > 4)
        ? h('select', { value: String(cur), disabled: !on, style: { width: '12em', flex: '0 0 auto', opacity: on ? 1 : 0.45 }, title: tip,
            onChange: function (e) { var o = opts.filter(function (x) { return String(x[0]) === e.target.value; })[0]; if (o) pickView(w.key, o[0]); } },
            opts.map(function (o) { return h('option', { key: String(o[0]), value: String(o[0]) }, o[1]); }))
        : h('span', { title: on ? '' : tip, style: { opacity: on ? 1 : 0.45, pointerEvents: on ? 'auto' : 'none' } }, h(S.Seg, { opts: opts.map(function (o) {
            return { label: o[1], tip: o[2] || tip, cls: cur === o[0] ? 'on' : '', pick: function () { pickView(w.key, o[0]); } };
          }) }));
      return h('span', { key: w.key, className: 'hs-view' }, w.label ? h('span', { className: 'ui-mut st-lab' }, w.label) : null, ctl);
    });
    var what = (A.play && A.play.what) || 'the preview';
    var cursor = P && P.cursor ? P.cursor(pc()) : undefined;

    return h(S.Frame, null,
      h(S.TopBar, {
        back: function () { try { root.parent.postMessage({ type: 'theme.close' }, '*'); } catch (e) { /* not framed */ } },
        backTip: 'Back to the Themes page', crumb: A.title,
        name: chosen ? { value: d.name, placeholder: 'Theme name', title: 'The theme’s name on the orb', onChange: function (e) { upd('name', e.target.value); } } : null,
        steps: chosen ? STEPS.map(function (s) { return { label: s[1], cls: s[0] === step ? 'on' : '', pick: function () { if (s[0] === 'start') setStartPick(''); setStep(s[0]); } }; }) : [],
        undo: chosen ? { on: function () { ses.undo(); }, tip: 'Undo', op: ses.undoStack.length ? 1 : 0.35 } : null,
        redo: chosen ? { on: function () { ses.redo(); }, tip: 'Redo', op: ses.redoStack.length ? 1 : 0.35 } : null,
        extra: chosen && step !== 'start' ? [{ label: resetArm ? 'Press again to reset' : 'Reset to ' + baseName, title: 'Put this theme back to the start it came from, its name kept; Undo brings your version back', on: reset }] : [],
        fit: chosen ? fit : null, primary: chosen ? { label: 'Send to orb', on: send, title: 'Bake and send this theme' } : null,
      }),
      h(S.Pane, { side: 'left' }, chosen ? (PN.left ? PN.left(paneCtx()) : left) : null),
      h(S.Stage, null,
        h(S.Glass, Object.assign({ status: status }, g),
          PN.stage ? PN.stage(Object.assign(paneCtx(), { size: g.size })) :
          h('canvas', { ref: canvasRef, width: G, height: G, onPointerDown: onDown, onPointerMove: onMove, onPointerUp: onUp, onPointerCancel: onUp,
            style: { width: g.size + 'px', height: g.size + 'px', cursor: cursor, touchAction: P ? 'none' : undefined } })),
        sheet,
        PN.overlay && step !== 'start' ? PN.overlay(paneCtx()) : null,
        h(S.ScreenPanel, vw.panel()),
        // the upper row is what the preview shows: the frame clock and the app's views; it is
        // always there once an app plays, so the controls never move under the pointer
        A.play || viewCtl.some(Boolean) ? h(S.Toolbar, {
          play: A.play ? { icon: playing && playOn ? '❚❚' : '▶', label: playOn ? (playing ? 'Pause ' : 'Play ') + what : (A.play.idle || what + ' does not move in this view'),
            on: function () { if (playOn) setPlaying(!playing); } } : null,
        }, viewCtl) : null,
        // the lower is how it is shown: the view of the glass, the render, this screen
        h(S.Toolbar, {
          view: tool.view, zoom: tool.zoom, screen: vw.toolScreen(),
          render: A.layers ? T.RENDERS.map(function (r) { return { label: r[1], tip: r[2], cls: r[0] === rend ? 'on' : '', pick: function () { setRend(r[0]); } }; }) : null,
          wipe: A.layers && rend === 'compare' ? { value: wipe, on: function (e) { setWipe(+e.target.value); } } : null,
        }, PN.tool ? PN.tool(paneCtx()) : null)),
      h(S.Pane, { side: 'right' }, right),
      h(S.OpeningModal, { line: ses.opening }),
      h(S.SendModal, S.sendProps(sending, status, function () { sender.cancel(); })),
      h(S.DupModal, S.dupProps(dup, function () { sender.replace(); }, function () { sender.dismiss(); })),
      h(S.Confirm, Object.assign({ open: !!ask }, ask || {})));
  }

  H.Editor = Editor;
  // the app's module calls this once; the page mounts the editor on it
  H.register = function (app) {
    H.app = app;
    var el = typeof document !== 'undefined' && document.getElementById('root');
    if (el && !H.mounted) { H.mounted = true; root.ReactDOM.createRoot(el).render(h(Editor, { app: app })); }
  };
})(typeof window !== 'undefined' ? window : globalThis);
