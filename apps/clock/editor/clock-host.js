/* clock-host.js — the clock's part of its theme editor on Orb Manager's page (host@1;
 * theme-editor-kit-plan §8.12, E7 Stage 2).
 *
 * The clock is a stateful app: its logic class (the Watch Composer's, `clock-logic.js`) keeps
 * the watch, and this module runs it under a stand-in for the dc runtime's DCLogic, then
 * registers with OrbHost:
 *   - the host's design is a snapshot of the watch (`snap`): the class pushes it whenever it
 *     changes the watch itself, and adopts one whenever the host changes it (a template, an
 *     opened design, Undo, Reset);
 *   - its files keep the clock's own shape (toDoc / fromDoc: the class's designContent);
 *   - its panes are the composer's: the layers with their mechanical switch, the inspector,
 *     the stage canvas and its diagnostics, and the toolbar's play, time, render and guide.
 * What the host owns for every app it owns here too: the start, the library, import, the
 * Render step, Send, Export, history and the live size. The class's own session boot, its
 * undo stacks, start sheet, Render panel and live size are not used. */
(function (root) {
  'use strict';
  var R = root.React, h = R.createElement, S = root.OrbStudio;
  var E = function () { return root.WatchEngine; };
  var cx = function () { return Array.prototype.filter.call(arguments, Boolean).join(' '); };
  var list = function (a) { return Array.isArray(a) ? a : []; };

  /* ---- the clock's own styles: the layers list, the inspector's rows (from the composer) ---- */
  var CSS = [
    '.wc-warn{color:var(--warn)}',
    '.wc-switch{display:inline-flex;align-items:center;gap:7px;background:none;border:0;padding:0;color:var(--mut)}',
    '.wc-switch i{width:26px;height:14px;border-radius:7px;background:#2b3540;position:relative;flex:none}',
    '.wc-switch i::after{content:"";position:absolute;left:2px;top:2px;width:10px;height:10px;border-radius:50%;background:var(--mut);transition:left .12s}',
    '.wc-switch.on{color:var(--fg)}.wc-switch.on i{background:var(--prim)}.wc-switch.on i::after{left:14px;background:#fff}',
    '.wc-ihead{padding:10px 14px 8px;border-bottom:1px solid var(--line);display:flex;flex-direction:column;gap:6px}',
    '.wc-ititle{display:flex;justify-content:space-between;align-items:center;gap:8px;font-size:15px;font-weight:600;min-width:0}',
    '.wc-ititle>span:first-child{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}',
    '.wc-iacts{display:flex;gap:4px;flex:none}.wc-sm{padding:3px 8px;font-size:12px}.wc-subs{align-self:flex-start}',
    '.wc-ibody{flex:1;overflow-y:auto;min-height:0;padding:4px 14px 16px;display:flex;flex-direction:column;gap:8px}',
    '.wc-ifoot{padding:10px 14px;border-top:1px solid var(--line);display:flex;gap:8px}.wc-ifoot>button{flex:1}',
    '.wc-sec{font-size:11px;letter-spacing:.06em;text-transform:uppercase;color:var(--mut);padding:10px 0 2px;border-top:1px solid var(--line);margin-top:4px}',
    '.wc-sec.first{border-top:0;margin-top:0}.wc-hint{font-size:12px;color:var(--mut)}',
    '.wc-cards{display:grid;grid-template-columns:1fr;gap:6px}.wc-card{text-align:left;white-space:normal;padding:8px 10px;border-radius:7px}',
    '.wc-card b{display:block;font-weight:600}.wc-card span{display:block;color:var(--dim);font-size:11px;margin-top:2px}',
    '.wc-tiles{display:grid;grid-template-columns:repeat(auto-fill,minmax(64px,1fr));gap:6px}',
    '.wc-tile{display:flex;flex-direction:column;align-items:center;gap:4px;padding:6px 4px;border-radius:7px;border:1px solid;cursor:pointer;font-size:11px;white-space:nowrap}',
    '.wc-grid2{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:6px}',
    '.wc-right{display:flex;flex-direction:column;min-height:0;height:100%}',
  ].join('\n');
  if (typeof document !== 'undefined' && document.head && !document.getElementById('wc-css')) {
    var st = document.createElement('style'); st.id = 'wc-css'; st.textContent = CSS; document.head.appendChild(st);
  }

  /* ---- the dc runtime's DCLogic, as the class expects it: setState merges at once (as the
   * clock's suites' stub does), the panes draw again, and componentDidUpdate runs after ---- */
  var subs = [], ver = 0, scheduled = false;
  function notify() {
    ver++;
    if (scheduled) return;
    scheduled = true;
    Promise.resolve().then(function () { scheduled = false; subs.slice().forEach(function (f) { f(ver); }); });
  }
  function DCLogic(props) { this.props = props || {}; this.state = {}; }
  DCLogic.prototype.setState = function (p, cb) {
    Object.assign(this.state, typeof p === 'function' ? p(this.state) : p);
    notify();
    if (cb) Promise.resolve().then(cb);
  };
  DCLogic.prototype.forceUpdate = function () { notify(); };
  DCLogic.prototype.componentDidMount = function () {};
  DCLogic.prototype.componentDidUpdate = function () {};
  DCLogic.prototype.componentWillUnmount = function () {};
  DCLogic.prototype.renderVals = function () { return {}; };

  /* the class, hosted: the host keeps history, so the class's own steps aside */
  var Base = root.ClockLogic(DCLogic);
  class Hosted extends Base {
    // a re-timed bake waits for the rebuilt movement 30 s, not the class's 2 minutes (a class
    // field is the instance's own, so it is set here rather than shadowed by an accessor)
    constructor(props) { super(props); this.REBAKE_SETTLE_MS = 30000; }
    trackHistory() { /* the host's history (snapshots of the snapshot) */ }
  }

  // lastKey: the watch the class holds; handedKey: the host's form of the one it was last handed
  // (a template's fields, before the class fills in its defaults): either is "already have it"
  var ctl = null, host = null, lastKey = null, handedKey = null, vals = null, valsVer = -1;
  // the class's values, once per change: four panes read them each draw
  function V() { if (valsVer !== ver || !vals) { vals = ctl.renderVals(); valsVer = ver; } return vals; }

  /* ---- the snapshot the host keeps: the watch, as the class's designContent holds it, with
   * the theme's name; uploads stay in the class (they are its art and sounds, not history) */
  function snap() {
    var s = ctl.state;
    var snd = ctl.sndDesign ? ctl.sndDesign() : null;
    return { name: ctl.themeName(), dial: s.dial, design: ctl.design(), cfg: ctl.cfg(), seeds: s.seeds, hands: s.hands, handFinish: s.handFinish,
      caseStyle: s.caseStyle, caseMetal: s.caseMetal, glassStyle: s.glassStyle, train: s.train, caseOpen: s.caseOpen, themeName: s.themeName,
      scratch: !!s.scratch, sounds: snd ? sndSnap(snd) : null, base: s.scratch ? 'blank' : s.dial };
  }
  // the sound design without its PCM; the file's ref only while the class still holds those very
  // uploads, and a fingerprint of them, so a new upload is a change the host sees (its live size,
  // its history, its unsent changes) even when the voice was already "upload"
  var upSig = function (up) {
    return Object.keys(up || {}).sort().map(function (k) { var u = up[k] || {}; return k + ':' + (u.name || '') + ':' + (u.pcm ? u.pcm.length : 0); }).join(',');
  };
  function sndSnap(snd) {
    var o = Object.assign({}, snd, { up: {}, upSig: upSig(snd.up) });
    delete o.upRef;
    Object.keys(ups).forEach(function (r) { if (ups[r] === snd.up) o.upRef = r; });
    return o;
  }
  // a snapshot's identity in one field order, whoever built it (the host stamps base, a
  // template builds its fields in its own order): the same watch is the same key
  var FIELDS = ['dial', 'design', 'cfg', 'seeds', 'hands', 'handFinish', 'caseStyle', 'caseMetal', 'glassStyle', 'train', 'caseOpen', 'themeName', 'scratch', 'sounds'];
  var keyOf = function (d) {
    try { return JSON.stringify(FIELDS.map(function (k) { return d[k] === undefined ? null : d[k]; })); } catch (e) { return String(Math.random()); }
  };
  // the theme's name is the host's (its top bar): the class bakes and files under themeName,
  // so the host's name is the class's typed one
  var nameOf = function (d) { return typeof d.name === 'string' && d.name.trim() ? d.name.trim() : d.themeName || ''; };
  // a file's sound uploads (PCM) stay out of the snapshots, as the class's own do: the file's
  // ride under a ref (sounds.upRef) the snapshot carries, so an opened design, and Undo back
  // to it, keeps them
  var ups = {}, upN = 0;
  // a file's samples back to the Int16Array the sound engine reads: JSON writes a typed array as
  // {"0": v, "1": v, ...}, and read back as that object every sample was silence (Gold, 2026-11-08:
  // a reopened design shipped its uploaded tick as zeros); a plain array is taken as well
  var pcmOf = function (p) {
    if (!p || p instanceof Int16Array) return p;
    if (Array.isArray(p)) return Int16Array.from(p);
    if (typeof p !== 'object') return p;
    var n = 0; while (p[n] !== undefined) n++;
    var o = new Int16Array(n); for (var i = 0; i < n; i++) o[i] = p[i];
    return o;
  };
  var upsOf = function (up) {
    var o = {};
    Object.keys(up || {}).forEach(function (k) { o[k] = Object.assign({}, up[k], up[k] && up[k].pcm ? { pcm: pcmOf(up[k].pcm) } : {}); });
    return o;
  };
  // the class's state from a snapshot (the watch only: selection and view are the class's)
  function stateOf(d) {
    var Ee = E();
    var o = { dial: d.dial, designs: Object.assign({}, ctl.state.designs), themeName: nameOf(d), scratch: !!d.scratch };
    o.designs[d.dial] = Ee.canonDesign(JSON.parse(JSON.stringify(d.design)));
    ['cfg', 'seeds', 'hands', 'handFinish', 'caseStyle', 'caseMetal', 'glassStyle', 'train', 'caseOpen'].forEach(function (k) { if (d[k] != null) o[k] = d[k]; });
    if (d.sounds) {
      var held = ((ctl.state.sounds || {})[d.dial] || {}).up || {};
      o.sounds = Object.assign({}, ctl.state.sounds); o.sounds[d.dial] = Object.assign({}, d.sounds, { up: (d.sounds.upRef && ups[d.sounds.upRef]) || held });
      // the ref and the fingerprint are the snapshot's, never the class's: its files and bakes carry neither
      delete o.sounds[d.dial].upRef; delete o.sounds[d.dial].upSig;
    }
    return o;
  }
  // a template, as designed (the clock's pick after From scratch: no author's light rides over)
  function templateSnap(id) {
    var P = E().template(id);
    return { name: P.name, dial: id, design: E().canonDesign(JSON.parse(JSON.stringify(P.design))), cfg: Object.assign({}, P.cfg || {}),
      seeds: { spokes: 1, pinion: 1, center: 1, finish: 1, etching: 1 }, hands: 'auto', handFinish: 'auto', caseStyle: P.caseStyle || 'polished',
      caseMetal: P.caseMetal || 'auto', glassStyle: P.glassStyle || 'domed', train: P.train || 'classic', caseOpen: 221.5, themeName: '', scratch: false, sounds: null };
  }
  function scratchSnap() {
    var P = E().SCRATCH;
    return { name: 'Untitled', dial: 'scratch', design: E().canonDesign(JSON.parse(JSON.stringify(P.design || { comps: [], noDial: true }))), cfg: Object.assign({}, P.cfg),
      seeds: { spokes: 1, pinion: 1, center: 1, finish: 1, etching: 1 }, hands: 'auto', handFinish: 'auto', caseStyle: P.caseStyle, caseMetal: P.caseMetal,
      glassStyle: P.glassStyle, train: P.train, caseOpen: 221.5, themeName: '', scratch: true, sounds: null };
  }
  // the watch a snapshot draws, small: the class's thumbnail walk on the snapshot's state
  function pictureOf(d, px) {
    var st0 = ctl.state, Ee = E();
    try {
      ctl.state = Object.assign({}, st0, stateOf(d));
      var o = ctl.opts(), hd = ctl.handDef(), when = new Date(); when.setHours(10, 9, 36, 0);
      var mv = Ee.movement(o), gt = mv.render(0).gt, fc = Ee.face(Object.assign({}, o, { design: ctl.design(), hands: hd.style, handFinish: hd.finish }));
      var sc = document.createElement('canvas'); sc.width = sc.height = 466;
      Ee.compose(sc.getContext('2d'), mv, gt, fc, when, {});
      var c = document.createElement('canvas'); c.width = c.height = px; c.getContext('2d').drawImage(sc, 0, 0, px, px);
      return c;
    } finally { ctl.state = st0; ctl.primeStage(); }
  }

  /* ---- the controller: made once, when the host first draws a pane ---- */
  function ensure(c) {
    if (ctl) return ctl;
    host = c;
    ctl = new Hosted({});
    // the composer's mount, less what the host owns (the session's boot and the Edit door, the
    // calibration, the history's keys)
    ctl.__mount = (async function () {
      while (!root.WatchEngine || !root.DialRenderer || !root.DialRenderer._) await new Promise(function (r) { setTimeout(r, 50); });
      var fams = ['700 31px "Cormorant Garamond"', 'italic 600 30px "Cormorant Garamond"', '600 40px "IBM Plex Sans"', '500 21px "IBM Plex Sans"', 'italic 600 30px "IBM Plex Sans"'];
      try { await Promise.race([Promise.all(fams.map(function (f) { return document.fonts.load(f); })), new Promise(function (r) { setTimeout(r, 4000); })]); } catch (e) { /* fallback faces */ }
      ctl.ready = true;
      if (pending) { var p = pending; pending = null; adopt(p); }
      ctl.buildMovement(); ctl.startTimer(); notify();
      // The dc page ran componentDidUpdate after every render, and the ticker rendered every
      // frame; hosted, the panes draw only when the class's state changes, and a re-time or a
      // plan learned in a bake changes the movement's key without any. Then nothing rebuilt it:
      // the bank sat half-built (8 of 15 frames), the balance stopped and the readout said 4 fps
      // for Broad arrow's 15 (Gold, 2026-11-08). So the controller keeps the class's own loop.
      ctl.__pump = setInterval(function () {
        if (!ctl.ready) return;
        try { ctl.componentDidUpdate(); } catch (e) { /* the class says so on its status line */ }
        pushIfChanged();
      }, 100);
    })();
    return ctl;
  }
  var pending = null;
  // the host's design into the class (a template, an opened design, Undo, Reset, the first)
  function adopt(d) {
    if (!ctl || !ctl.ready) { pending = d; return; }
    var k = keyOf(d);
    if (k === lastKey || k === handedKey) {
      // the same watch, perhaps renamed: the name alone, with no rebuild and nothing pushed back
      if (nameOf(d) !== (ctl.state.themeName || '')) { ctl.setState({ themeName: nameOf(d) }); lastKey = keyOf(snap()); }
      return;
    }
    ctl.setState(Object.assign(stateOf(d), { sel: null, selKind: null }));
    ctl.faces = {}; if (ctl.buildMovement) ctl.buildMovement();
    lastKey = keyOf(snap()); handedKey = k;
  }
  // after each draw: what the class changed itself goes to the host
  function pushIfChanged() {
    if (!ctl || !ctl.ready || !host) return;
    var d = snap(), k = keyOf(d);
    if (k === lastKey) return;
    lastKey = k; handedKey = null;
    host.push(d);
  }

  /* a pane: draws again on every change of the class, and runs its componentDidUpdate once */
  function useCtl(c) {
    ensure(c); host = c;
    var t = R.useState(0), set = t[1];
    R.useEffect(function () { var f = function () { set(function (n) { return n + 1; }); }; subs.push(f); return function () { subs.splice(subs.indexOf(f), 1); }; }, []);
    return V();
  }
  var didVer = -1;
  function afterDraw() {
    if (!ctl || !ctl.ready || didVer === ver) return;
    didVer = ver;
    try { ctl.componentDidUpdate(); } catch (e) { /* the class says so on its status line */ }
    pushIfChanged();
  }

  /* the step: the host's bar and the class's own walk agree. The host's step is handed to the
   * class; a step the class takes itself (a part picked on the dial) is handed to the host */
  function useStep(c, Rv) {
    R.useEffect(function () {
      if (!ctl || !ctl.ready || c.step === 'start' || c.step === 'render') return;
      var at = ctl.stepNow()[0];
      if (at !== c.step && ctl.__hostStep !== c.step) { ctl.__hostStep = c.step; ctl.goStep(c.step); }
      else if (at !== c.step && ctl.__hostStep === c.step) { ctl.__hostStep = at; c.goStep(at); }
    });
  }

  /* ---- the panes ---- */
  function Left(c) {
    var Rv = useCtl(c); R.useEffect(afterDraw);
    var L = ctl.studioVals(Rv).studioLayers || {};
    return h(S.Layers, L,
      h('button', { className: cx('wc-switch', Rv.mechCls), onClick: Rv.toggleMech, title: 'Mechanical: warn when the stack could not be built as a real movement (R8). Warnings never block a move or a bake.' },
        h('i', null), 'Mechanical'),
      h('span', { className: 'wc-warn' }, Rv.warnCount));
  }
  function Stage(c) {
    var Rv = useCtl(c); R.useEffect(afterDraw);
    return h('canvas', { ref: ctl.mainRef, width: 466, height: 466, onPointerDown: Rv.onDown, onPointerMove: Rv.onMove, onPointerUp: Rv.onUp, onPointerCancel: Rv.onUp,
      style: { width: c.size + 'px', height: c.size + 'px', cursor: Rv.cursor } });
  }
  function Overlay(c) {
    var Rv = useCtl(c);
    if (!Rv.diagOpen) return null;
    return h('div', { className: 'st-diag' },
      h('div', { className: 'ui-mono ui-mut' }, Rv.metaLabel),
      h('div', { style: { display: 'flex', gap: '6px', alignItems: 'center', flexWrap: 'wrap', justifyContent: 'center' } },
        h('span', { className: 'ui-mono', style: { minWidth: '110px' } }, Rv.frameLabel),
        h('button', { onClick: Rv.prev, title: 'Previous bank frame' }, '←'),
        h('button', { onClick: Rv.next, title: 'Next bank frame' }, '→'),
        h('button', { onClick: Rv.toggleSlow, title: 'Play at a fifth of the pace' }, Rv.slowLabel)),
      h('div', { className: 'ui-mono ui-mut', style: { fontSize: '11px', textAlign: 'center' } }, Rv.seamText));
  }
  function Tool(c) {
    var Rv = useCtl(c), T = ctl.studioVals(Rv).studioTool || {}, g = Rv.g || {};
    var seg = function (o) { return o ? h(S.Seg, { opts: o }) : null; };
    return h(R.Fragment, null,
      T.play ? h('button', { className: 'st-play', onClick: T.play.on, title: T.play.label }, T.play.icon) : null,
      seg(T.time),
      T.render ? h('span', { className: 'ui-mut st-lab' }, 'Render') : null, seg(T.render),
      T.wipe ? h('input', { type: 'range', min: 0, max: 466, step: 1, value: T.wipe.value, onChange: T.wipe.on, title: 'Where the split sits', style: { width: '120px' } }) : null,
      h('button', { className: cx('ui-ghost', g.cls), onClick: Rv.pickGuide, title: (g.label || 'Guide') + ': the alignment guide. Auto shows it while a dial or hand is selected and during a drag; press to cycle on, off, auto' }, '⌖'),
      h('button', { className: cx('ui-ghost', Rv.diagCls), onClick: Rv.toggleDiag, title: 'Diagnostics: frame stepping, the seam check and the bank' }, '⌗'));
  }
  function tiles(opts, sz) {
    return h('div', { className: 'wc-tiles' }, list(opts).map(function (t, i) {
      return h('div', { key: i, className: 'wc-tile', onClick: t.pick, style: { borderColor: t.bd, background: t.bg, color: t.fg } },
        h('canvas', { ref: t.ref, width: sz, height: sz, style: { display: 'block', width: '100%', height: 'auto', borderRadius: '50%' } }), t.label || t.name || '');
    }));
  }
  function Right(c) {
    var Rv = useCtl(c); R.useEffect(afterDraw); useStep(c, Rv);
    var F = ctl.studioVals(Rv).studioFields || { fields: [] };
    return h('div', { className: 'wc-right' },
      h('div', { className: 'wc-ihead' },
        h('div', { className: 'ui-label' }, Rv.inspKind),
        h('div', { className: 'wc-ititle' }, h('span', null, Rv.inspTitle),
          h('span', { className: 'wc-iacts' },
            Rv.hasTabReset ? h('button', { className: 'ui-ghost wc-sm', onClick: Rv.tabReset, title: Rv.tabResetLabel }, 'Reset') : null,
            Rv.hasSelAny ? h('button', { className: 'ui-ghost wc-sm', onClick: Rv.clearSel, title: Rv.clearLabel, style: { opacity: Rv.clearOp } }, '✕') : null)),
        Rv.hasSubs ? h('div', { className: 'ui-seg wc-subs' }, list(Rv.subOpts).map(function (t, i) { return h('button', { key: i, className: t.cls, onClick: t.pick }, t.label); })) : null),
      h('div', { className: 'wc-ibody', ref: ctl.paneRef },
        Rv.isMove ? h('div', { className: 'wc-cards', style: { paddingTop: '6px' } }, list(Rv.trainOpts).map(function (t, i) {
          return h('button', { key: i, className: 'wc-card', onClick: t.pick, title: t.blurb, style: { background: t.bg, borderColor: t.bd, color: t.fg } }, h('b', null, t.name), h('span', null, t.short));
        })) : null,
        Rv.isComps && Rv.noSel ? h(R.Fragment, null,
          h('div', { className: 'wc-hint', style: { paddingTop: '8px' } }, 'Select a part on the dial or in Layers, or add one:'),
          h('div', { className: 'wc-cards' }, list(Rv.catOpts).map(function (a, i) {
            return h('button', { key: i, className: 'wc-card', onClick: a.go, title: a.tip, style: { opacity: a.op, cursor: a.cursor } }, h('b', null, a.label), h('span', null, a.blurb));
          }))) : null,
        Rv.isCase ? h(R.Fragment, null, h('div', { className: 'wc-sec first' }, 'Case'), tiles(Rv.caseOpts, 112),
          h('div', { className: 'wc-sec', 'data-a': 'case-glass' }, 'Glass'), tiles(Rv.glassOpts, 112)) : null,
        Rv.handGrid ? h(R.Fragment, null,
          h('div', { className: 'wc-sec first', style: { display: 'flex', justifyContent: 'space-between', alignItems: 'center' } }, 'Default style',
            h('button', { className: 'ui-ghost wc-sm', onClick: Rv.resetHands, title: 'Every hand back to the default style' }, 'Reset')),
          tiles(Rv.handOpts, 104)) : null,
        h(S.Fields, F)),
      Rv.hasDel ? h('div', { className: 'wc-ifoot' }, h('button', { className: 'ui-danger', onClick: Rv.delSel }, 'Remove ' + Rv.delLabel)) : null);
  }

  /* ---- what the host asks of the clock ---- */
  var presets = function () { return root.WatchEngine ? Object.keys(root.WatchEngine.PRESETS) : []; };
  var app = {
    slug: 'clock', title: 'Clock', schema: 1,
    migrate: {},
    check: function (d) { return d && d.design && Array.isArray(d.design.comps) && root.WatchEngine && (d.scratch || root.WatchEngine.template(d.dial)) ? null : 'that design has no watch in it'; },
    blank: { note: 'Nothing yet: no dial, hands, case or glass', make: scratchSnap },
    get templates() {
      return presets().map(function (id) {
        var P = root.WatchEngine.template(id);
        return { key: id, name: P.name, note: String(P.blurb || '').split(/ · |[.;:—]/).slice(0, 2).join(' · ').trim(), make: function () { return templateSnap(id); } };
      });
    },
    start: 'skeleton',
    picture: async function (d, px) { if (!ctl || !ctl.ready) await new Promise(function (r) { setTimeout(r, 300); }); return pictureOf(d, px); },
    steps: [['movement', 'Movement'], ['dial', 'Dial'], ['parts', 'Parts'], ['finish', 'Finish']],
    adopt: adopt,
    // the clock's files: designContent's own shape, both ways
    toDoc: async function (d, withArt) {
      var doc = await ctl.designContent(withArt);
      if (doc.sounds) { doc.sounds = Object.assign({}, doc.sounds); delete doc.sounds.upRef; delete doc.sounds.upSig; }
      return doc;
    },
    fromDoc: function (doc) {
      if (!doc || !doc.design || !Array.isArray(doc.design.comps)) return null;
      var Ee = root.WatchEngine, T0 = Ee && Ee.template(doc.dial);
      if (Ee && doc.assets) Ee.assetRestore(doc.assets);
      var up = doc.sounds && doc.sounds.up && upsOf(doc.sounds.up), ref = null;
      if (up && Object.keys(up).length) { ref = 'up' + (++upN); ups[ref] = up; }
      return { name: doc.themeName || (T0 ? T0.name : 'Untitled'), dial: doc.dial, design: doc.design, cfg: doc.cfg || {}, seeds: doc.seeds, hands: doc.hands, handFinish: doc.handFinish,
        caseStyle: doc.caseStyle, caseMetal: doc.caseMetal, glassStyle: doc.glassStyle, train: doc.train, caseOpen: doc.caseOpen, themeName: doc.themeName || '',
        scratch: doc.dial === 'scratch', sounds: doc.sounds ? Object.assign({}, doc.sounds, { up: {}, upSig: upSig(up) }, ref ? { upRef: ref } : {}) : null, base: doc.dial };
    },
    // the bake: the composer's own walk on the watch the host holds (adopted first)
    bake: async function (d, o) {
      // Rebake all bakes at boot, before any pane has drawn and made the controller
      if (!ctl) ensure(host);
      await ctl.__mount;
      // The class rebuilds its movement only from componentDidUpdate, which the hosted editor runs
      // when a pane draws; a bake must not wait on the page drawing (play paused, nothing changing:
      // Gold 2026-11-08, a re-timed bank stuck at 8 of 15 frames). So the bake drives it itself, as
      // the shelf's driver page does (bank3-drive.html), from the hand-over to the bytes.
      var pump = setInterval(function () { try { ctl.componentDidUpdate(); } catch (e) { /* the bake says */ } }, 40);
      try {
        // the watch the host holds; a hand-over that rebuilds the movement is waited out
        var was = lastKey + '|' + handedKey; adopt(d);
        if (lastKey + '|' + handedKey !== was) await new Promise(function (r) { setTimeout(r, 300); });
        // the movement must be drawn before it is baked; it draws only while the window is
        // visible, so a wait is said, and one with no new frame in STALL_MS gives up in words
        var say = (o && o.say) || function () {}, t0 = Date.now(), moved = t0, seen = -1, told = '';
        while (!ctl.settled3() || ctl.rendering) {
          var n = (ctl.gt || []).filter(Boolean).length, of = ctl.B || 0;
          if (n !== seen) { seen = n; moved = Date.now(); }
          if (Date.now() - t0 > root.ClockHost.SAY_AFTER_MS) {
            var line = 'Waiting for the movement to finish drawing (' + n + ' of ' + of + ' frames) \u2014 keep this window in front';
            if (line !== told) { told = line; say(line); }
          }
          if (Date.now() - moved > root.ClockHost.STALL_MS)
            throw new Error('the movement stopped drawing at ' + n + ' of ' + of + ' frames \u2014 if this window is hidden or minimised, bring it to the front and press again');
          await new Promise(function (r) { setTimeout(r, 250); });
        }
        ctl._quietBake = true; ctl._quietLast = ''; ctl._bakedBytes = null;
        try { await ctl._export3Body({ send: 'bytes' }); } finally { ctl._quietBake = false; }
        var bytes = ctl._bakedBytes; ctl._bakedBytes = null;
        if (!bytes) throw new Error(String(ctl._quietLast || 'the bake did not finish').replace(/^Not baked: /, ''));
        return { bytes: bytes };
      } finally { clearInterval(pump); }
    },
    // the fit: the clock's own rows and its pace model (_devPace), not the host's sampling
    fit: function () {
      if (!ctl || !ctl.ready) return { rows: 0, cap: 0 };
      // the composer's own verdict's inputs (its Render badge): the rows against the dial's cap,
      // the rate the design asks, and _devPace's estimate of what the glass holds
      var inp = ctl.stackInput(), rc = inp && root.OrbStack ? root.OrbStack.rowCount(inp) : { rows: 0, cap: 24 };
      var b = ctl.budget ? ctl.budget() : null, want = b ? (b.dev && b.dev.need) || b.fps : 0, pace = b && b.dev && b.dev.pace ? b.dev.pace : want;
      return { rows: rc.rows, cap: rc.cap, want: want, pace: pace };
    },
    panes: {
      left: function (c) { return h(Left, c); },
      right: function (c) { return h(Right, c); },
      stage: function (c) { return h(Stage, c); },
      tool: function (c) { return h(Tool, c); },
      overlay: function (c) { return h(Overlay, c); },
    },
  };
  root.ClockHost = { STALL_MS: 15000, SAY_AFTER_MS: 2000, app: app, snap: function () { return ctl && snap(); }, ctl: function () { return ctl; }, templateSnap: templateSnap, scratchSnap: scratchSnap };
  if (root.OrbHost) root.OrbHost.register(app);
})(typeof window !== 'undefined' ? window : globalThis);
