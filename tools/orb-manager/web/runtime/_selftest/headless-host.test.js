#!/usr/bin/env node
// host@1 (theme-editor-kit-plan §8): the editor page Orb Manager owns, driven with a made-up
// app ("tint") so nothing of a real app's can hide in it. A small hooks runtime stands in
// for React (state, refs, memos and effects, re-rendering until it settles; components
// below the editor are expanded with throwaway hooks), the hub is fake-hub.js, and the orb's
// home, places and units are answered here. What the host owns is held: the start step's
// order and its starts, the views (the address, init, show, active, the place list, the two
// rows and their captions), the frame clock (dt only on a tick), Panel and Split, kept parts
// (a promise drawing again, fresh), Reset, the pointer in glass px, the app's own state, the
// Render step, and off the hub.
//
//   node headless-host.test.js
'use strict';
const path = require('path');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
setTimeout(() => { console.log('FAIL timed out (' + checked + ' checks run)'); process.exit(1); }, 30000);
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

/* ---- a hooks runtime: enough React for one editor ---- */
const FRAG = Symbol('fragment');
const same = (a, b) => !!a && !!b && a.length === b.length && a.every((x, i) => Object.is(x, b[i]));
let cur = null;
global.window = globalThis;
global.React = {
  Fragment: FRAG,
  createElement: (type, props, ...kids) => ({ type, props: props || {}, kids: kids.flat(Infinity).filter((k) => k !== null && k !== undefined && k !== false) }),
  useState(init) {
    const H = cur, i = H.i++;
    if (!(i in H.hooks)) H.hooks[i] = { v: typeof init === 'function' ? init() : init };
    const slot = H.hooks[i];
    return [slot.v, (x) => { const nv = typeof x === 'function' ? x(slot.v) : x; if (!Object.is(nv, slot.v)) { slot.v = nv; H.dirty = true; } }];
  },
  useRef(v) { const H = cur, i = H.i++; if (!(i in H.hooks)) H.hooks[i] = { current: v }; return H.hooks[i]; },
  useMemo(fn, deps) { const H = cur, i = H.i++, s = H.hooks[i]; if (!s || !same(s.deps, deps)) H.hooks[i] = { v: fn(), deps }; return H.hooks[i].v; },
  useCallback(fn, deps) { return React.useMemo(() => fn, deps); },
  useEffect(fn, deps) {
    const H = cur, i = H.i++, s = H.hooks[i];
    if (!s || !deps || !same(s.deps, deps)) H.effects.push(() => { if (s && s.cleanup) s.cleanup(); const c = fn(); H.hooks[i] = { deps, cleanup: typeof c === 'function' ? c : null }; });
  },
  useLayoutEffect() { /* the glass measures itself in a browser */ },
};
// a canvas: a 2d context that answers every call, and a box 466 px across at the origin
const ctxCalls = [];
const fakeCtx = () => new Proxy({}, { get: (o, k) => k in o ? o[k] : k === 'getImageData' ? () => ({ data: new Uint8ClampedArray(466 * 466 * 4) }) : (...a) => { ctxCalls.push(k); }, set: (o, k, v) => { o[k] = v; return true; } });
const fakeCanvas = () => ({ width: 466, height: 466, getContext: fakeCtx, getBoundingClientRect: () => ({ left: 0, top: 0, width: 233, height: 233 }), toDataURL: () => 'data:image/jpeg;base64,AA' });
global.document = { createElement: () => fakeCanvas(), getElementById: () => null, body: { appendChild() {} } };
const glass = fakeCanvas();
// expand function components with throwaway hooks, and hand the canvas its ref
const expand = (n) => {
  if (n == null || typeof n !== 'object') return n;
  if (typeof n.type === 'function') {
    const was = cur; cur = { hooks: {}, i: 0, effects: [] };
    try { return expand(n.type(Object.assign({}, n.props, n.kids.length ? { children: n.kids.length === 1 ? n.kids[0] : n.kids } : {}))); } finally { cur = was; }
  }
  if (n.type === 'canvas' && n.props.ref && typeof n.props.ref === 'object' && n.props.width === 466) n.props.ref.current = glass;
  return Object.assign({}, n, { kids: n.kids.map(expand), props: Object.assign({}, n.props, n.props.children ? { children: undefined } : {}) });
};
function mount(Comp, props) {
  const H = { hooks: {}, i: 0, effects: [], dirty: false, tree: null };
  const render = () => {
    for (let k = 0; k < 40; k++) {
      H.i = 0; H.effects = []; H.dirty = false; cur = H;
      let t; try { t = Comp(props); } finally { cur = null; }
      H.tree = expand(t);
      H.effects.forEach((e) => e());
      if (!H.dirty) return;
    }
    throw new Error('the editor never settled');
  };
  H.render = render;
  // every effect's cleanup, as an unmount runs them: a stopped editor's clock draws nothing more
  H.unmount = () => { Object.values(H.hooks).forEach((h) => { if (h && typeof h.cleanup === 'function') h.cleanup(); }); H.render = () => {}; };
  H.settle = async (ms) => { for (let k = 0; k < 10; k++) { await wait(ms || 5); if (H.dirty) render(); } };
  render();
  return H;
}
const all = (n, f, out) => { out = out || []; if (n && typeof n === 'object') { if (f(n)) out.push(n); (n.kids || []).forEach((k) => all(k, f, out)); } return out; };
const text = (n) => (n == null ? '' : typeof n !== 'object' ? String(n) : (n.kids || []).map(text).join(''));
const by = (n, tag, cls) => all(n, (x) => x.type === tag && (!cls || String(x.props.className || '').split(' ').includes(cls)));
const button = (H, label) => by(H.tree, 'button').filter((b) => text(b).trim() === label)[0];
const press = async (H, label) => { const b = button(H, label); if (!b) throw new Error('no button ' + label); b.props.onClick({ stopPropagation() {} }); H.render(); await H.settle(); };

/* ---- the world: the runtime, the hub, an orb with home and two places ---- */
global.location = { origin: 'http://hub', search: '', pathname: '/editor/192.0.2.7/tint/' };
global.localStorage = { getItem: () => null, setItem() {}, removeItem() {} };
global.GorbBake = require(path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'));
['send.js', 'session.js', 'preview.js', 'measure.js'].forEach((f) => require(path.join(__dirname, '..', 'theme@1', f)));
require(path.join(__dirname, '..', 'studio@1', 'studio.js'));
require(path.join(__dirname, '..', 'host@1', 'host.js'));
const T = OrbTheme, HOST = OrbHost;
const panels = []; T.panel = (x, layers, o) => { panels.push(o.mode); return true; };
let orbAnswers = {
  home: { home: { set: true, lat: 32.75, lon: -97.33, place: 'near Fort Worth' } },
  places: { places: [{ i: 0, set: true, lat: 38.72, lon: -9.14, name: 'Lisbon' }, { i: 1, set: false }, { i: 2, set: true, lat: 51.5, lon: -0.12, name: 'London' }] },
  units: { dist: 'mi' },
};
const fh = require(path.join(__dirname, 'fake-hub.js'))({ rest: async (url) => {
  const m = /^\/orb\/[\d.]+\/api\/v1\/(\w+)$/.exec(url);
  if (m && orbAnswers[m[1]]) return { ok: true, status: 200, json: async () => JSON.parse(JSON.stringify(orbAnswers[m[1]])) };
  return { ok: false, status: 404, json: async () => ({ error: 'no route ' + url }) };
} });
global.fetch = async (url, o) => fh.fetch(url, o);

/* ---- the made-up app ---- */
const draws = [], made = [];
let lastFields = null, bakeWhy = null;
const tint = (name, color) => ({ name, color, size: 1 });
const app = {
  slug: 'tint', title: 'Tint', schema: 1,
  check: (d) => (d && d.color ? null : 'that design has no tint in it'),
  blank: { note: 'Plain grey', make: () => tint('Blank', 'grey') },
  templates: [{ key: 'sea', name: 'Sea', note: 'Deep blue', make: () => tint('Sea', 'blue') }, { key: 'sun', name: 'Sun', note: 'Warm', make: () => tint('Sun', 'gold') }],
  start: 'sun',
  picture: async () => fakeCanvas(),
  steps: [['colour', 'Colour'], ['shape', 'Shape']],
  parts: [{ id: 'colour', label: 'Colour', icon: 'C', also: ['shape'], note: (d) => d.color }],
  fields: (step, c) => { lastFields = { step, c }; return [c.K.Nt('', 'the ' + step + ' step')]; },
  views: [
    { key: 'size', label: 'Size', options: [['s', 'Small'], ['m', 'Medium'], ['l', 'Large']], init: 'm' },
    { key: 'mood', kind: 'select', label: 'Mood', tip: 'How it feels', options: [['calm', 'Calm'], ['storm', 'Storm']], active: (v) => v.size === 'l', idle: 'Large only' },
    { key: 'extra', label: 'Extra', options: [['a', 'A']], show: (d) => d.color === 'blue' },
    { key: 'where', kind: 'place', label: 'Place' },
  ],
  ui: { picked: 'dot' },
  play: { fps: 100, on: (v) => v.size !== 's', what: 'the tint' },
  layers: true,
  draw: (x, d, c) => {
    const p = c.part('swatch', d.color, () => { made.push(d.color); return d.color === 'blue' ? wait(30).then(() => 'slow ' + d.color) : 'fast ' + d.color; });
    const f = c.part('fresh', d.color, () => wait(30).then(() => 'fresh ' + d.color), { fresh: true });
    draws.push({ layer: c.layer, dt: c.dt, t: c.t, view: Object.assign({}, c.view), place: c.place && c.place.key, home: c.home && c.home.name, unit: c.unit, ui: c.ui, color: d.color, p, f });
  },
  pointer: {
    down: (pt, c) => { app.pointer.at = ['down', Math.round(pt[0]), Math.round(pt[1]), c.step]; c.setUi({ picked: 'grabbed' }); return true; },
    move: (pt, c) => { app.pointer.at = ['move', Math.round(pt[0]), Math.round(pt[1])]; c.set((o) => Object.assign({}, o, { size: 9 })); },
    up: () => { app.pointer.at = ['up']; },
    cursor: (c) => (c.step === 'shape' ? 'grab' : 'default'),
  },
  bake: async () => { if (bakeWhy) throw new Error(bakeWhy); return { bytes: new Uint8Array(4096) }; },
  fit: (d) => ({ rows: d.size === 9 ? 99 : 3, cap: 64 }),
};
const last = () => draws[draws.length - 1];

(async () => {
  // ---- the start step: Start fresh, the templates, the library, Import; the start it opens on
  {
    const H = mount(HOST.Editor, { app });
    await H.settle(20);
    const heads = by(H.tree, 'h2').map(text);
    ok(heads.join('|') === 'Start a tint theme|Library|Import', 'the start step: the starts, the library, then Import', heads.join('|'));
    const tiles = by(H.tree, 'div', 'st-gc').map((t) => text(by(t, 'b')[0]));
    ok(/Start fresh/.test(text(by(H.tree, 'button', 'st-gc')[0])) && tiles.join() === 'Sea,Sun', 'Start fresh, then the app’s templates', tiles.join());
    ok(by(H.tree, 'input').filter((i) => i.props.type === 'text' && /name/i.test(i.props.placeholder || '')).length === 1 && last().color === 'gold', 'it opens on the app’s start (Sun)', last().color);
    ok(by(H.tree, 'div', 'st-gc').filter((t) => /\bon\b/.test(t.props.className))[0] && text(by(H.tree, 'div', 'st-gc').filter((t) => /\bon\b/.test(t.props.className))[0]).indexOf('Sun') >= 0, 'and marks it');
    by(H.tree, 'div', 'st-gc')[0].props.onClick(); H.render(); await H.settle();
    ok(last().color === 'blue' && /Starting from Sea/.test(text(by(H.tree, 'div', 'st-toast')[0])), 'a tile starts from its template', last().color);
    by(H.tree, 'button', 'st-gc')[0].props.onClick(); H.render(); await H.settle();
    ok(last().color === 'grey', 'Start fresh starts from the app’s blank');
    ok(!button(H, 'Reset to Blank'), 'the start step has no Reset');
    await press(H, 'Continue with Blank');
    ok(lastFields && lastFields.step === 'colour', 'Continue goes to the app’s first step', lastFields && lastFields.step);
    H.unmount();
  }

  // ---- the views: the address leads, then init, then the first option; show, active, captions
  {
    location.search = '?size=l&mood=storm&preset=sea&where=p3';
    const H = mount(HOST.Editor, { app });
    await H.settle(20);
    ok(last().view.size === 'l' && last().view.mood === 'storm', 'the address names the views', JSON.stringify(last().view));
    ok(last().color === 'blue', 'and the start (?preset=)');
    H.unmount();
    location.search = '';
    const H2 = mount(HOST.Editor, { app });
    await H2.settle(20);
    ok(last().view.size === 'm' && last().view.mood === 'calm', 'without it a view starts at its init, else its first option', JSON.stringify(last().view));
    const rows = by(H2.tree, 'div', 'st-tool');
    ok(rows.length === 2, 'two rows under the glass', rows.length);
    const caps = by(rows[0], 'span', 'st-lab').map(text);
    ok(caps.join() === 'Size,Mood,Place', 'the upper row: the app’s views under their captions (Extra hidden: the design is not blue)', caps.join());
    ok(by(rows[0], 'button', 'st-play').length === 1 && !by(rows[1], 'button', 'st-play').length, 'the play button leads the upper row');
    ok(/View/.test(text(rows[1])) && /Render/.test(text(rows[1])) && !/Size/.test(text(rows[1])), 'the lower: how the glass is shown, and the render');
    const sel = by(rows[0], 'select')[0];
    ok(sel && sel.props.disabled && sel.props.title === 'Large only', 'a view that does not act here is off, its idle tip said', sel && sel.props.title);
    await press(H2, 'Large');
    const sel2 = by(by(H2.tree, 'div', 'st-tool')[0], 'select')[0];
    ok(!sel2.props.disabled && sel2.props.title === 'How it feels', 'and on when it acts');
    sel2.props.onChange({ target: { value: 'storm' } }); H2.render(); await H2.settle();
    ok(last().view.mood === 'storm', 'a dropdown’s pick reaches the frame');
    // the place list is the host's: home, then the orb's set places
    const placeOpts = by(by(H2.tree, 'div', 'st-tool')[0], 'button').map(text).filter((t) => /Home|Lisbon|London/.test(t));
    ok(placeOpts.join() === 'Home,Lisbon,London', 'a place view lists home and the orb’s set places', placeOpts.join());
    ok(last().place === 'home' && last().home === 'Home' && last().unit === 'mi', 'the frame has home (named Home), the place and the orb’s unit', JSON.stringify([last().place, last().home, last().unit]));
    await press(H2, 'London');
    ok(last().place === 'p3', 'picking a place reaches the frame', last().place);
    H2.unmount();
  }

  // ---- the frame clock: dt on a tick and only there; a view where nothing moves stops it
  {
    location.search = '?size=m';
    draws.length = 0;
    const H = mount(HOST.Editor, { app });
    await H.settle(20);
    const before = draws.length;
    await wait(80);
    const ticks = draws.slice(before);
    ok(ticks.length >= 3 && ticks.every((d) => d.dt === 10) && ticks[ticks.length - 1].t > ticks[0].t, 'it ticks at the app’s rate, each tick 10 ms on', ticks.length + ' ticks');
    H.render();
    ok(last().dt === 0, 'a frame drawn for any other reason moves nothing', last().dt);
    await press(H, '❚❚');
    const n = draws.length; await wait(60);
    ok(draws.length === n, 'pause stops it', draws.length - n);
    await press(H, '▶');
    await press(H, 'Small');
    const play = by(H.tree, 'button', 'st-play')[0];
    ok(play.props.title === 'the tint does not move in this view' && play.props.children === undefined, 'a view it does not play in says so', play.props.title);
    const m = draws.length; await wait(60);
    ok(draws.length === m, 'and does not tick', draws.length - m);
    H.unmount(); location.search = '';
  }

  // ---- Panel and Split: the host draws the layers apart and blends them
  {
    draws.length = 0; panels.length = 0;
    const H = mount(HOST.Editor, { app });
    await H.settle(20);
    await press(H, '❚❚');
    await press(H, 'Panel');
    const layers = draws.slice(-3).map((d) => d.layer).join();
    ok(layers === 'all,under,live' && panels[panels.length - 1] === 'panel', 'Panel: the frame, then its still and moving layers, blended', layers + ' ' + panels.join());
    await press(H, 'Split');
    ok(panels[panels.length - 1] === 'compare' && by(H.tree, 'input').some((i) => i.props.type === 'range'), 'Split blends with the wipe slider');
    await press(H, 'Design');
    const k = panels.length; H.render();
    ok(panels.length === k && last().layer === 'all', 'Design draws the frame alone');
    H.unmount();
  }

  // ---- kept parts: drawn once per key; a promise draws again when it lands; fresh is nothing till then
  {
    location.search = '?preset=sun';
    made.length = 0; draws.length = 0;
    const H = mount(HOST.Editor, { app });
    await H.settle(20);
    await press(H, '❚❚');
    ok(made.filter((c) => c === 'gold').length === 1 && last().p === 'fast gold', 'a part is made once for its key', made.join());
    await wait(50); H.render();
    ok(last().f === 'fresh gold', 'a promised part lands and is drawn');
    by(H.tree, 'div', 'st-gc')[0].props.onClick(); H.render();
    ok(last().p === 'fast gold' && last().f === null, 'while a new key’s promise is out, the last part stands; a fresh one is nothing', JSON.stringify([last().p, last().f]));
    await wait(60); await H.settle();
    ok(last().p === 'slow blue' && last().f === 'fresh blue', 'and both are drawn again when theirs lands', JSON.stringify([last().p, last().f]));
    H.unmount(); location.search = '';
  }

  // ---- Reset, the pointer, the app's own state, the fields' context
  {
    location.search = '?preset=sea&step=shape';
    const H = mount(HOST.Editor, { app });
    await H.settle(20);
    await press(H, '❚❚');
    ok(lastFields.step === 'shape' && lastFields.c.ui.picked === 'dot' && lastFields.c.unit === 'mi' && lastFields.c.home.name === 'Home' && lastFields.c.view.size === 'm', 'the fields see the view, the app’s state, home and the unit');
    const cv = by(H.tree, 'canvas').filter((c) => c.props.width === 466)[0];
    ok(cv.props.style.cursor === 'grab' && cv.props.style.touchAction === 'none', 'the app’s cursor on the glass', cv.props.style.cursor);
    let captured = false;
    cv.props.onPointerDown({ clientX: 100, clientY: 50, pointerId: 1, currentTarget: { setPointerCapture: () => { captured = true; } } });
    H.render(); await H.settle();
    ok(app.pointer.at.join() === 'down,200,100,shape' && captured, 'a press reaches the app in glass px, and is captured', app.pointer.at.join());
    ok(last().ui.picked === 'grabbed', 'the app’s own state reaches the frame');
    cv.props.onPointerMove({ clientX: 10, clientY: 10 }); H.render(); await H.settle();
    ok(app.pointer.at[0] === 'move' && /Over/.test(text(by(H.tree, 'header')[0])), 'a drag edits the design (the fit follows it)', text(by(H.tree, 'header')[0]).slice(0, 120));
    cv.props.onPointerUp({}); ok(app.pointer.at[0] === 'up', 'and lets go');
    await wait(700);                 // past the session's 600 ms burst, as a person is: the drag is its own step
    await press(H, 'Reset to Sea');
    ok(/Press Reset again/.test(text(by(H.tree, 'div', 'st-toast')[0])) && button(H, 'Press again to reset'), 'Reset asks twice');
    H.tree && (await press(H, 'Press again to reset'));
    H.render(); await H.settle();
    ok(!/Over/.test(text(by(H.tree, 'header')[0])) && /Reset to Sea/.test(text(by(H.tree, 'div', 'st-toast')[0])), 'and puts the design back to its start', text(by(H.tree, 'div', 'st-toast')[0]));
    await press(H, '↶');
    ok(/Over/.test(text(by(H.tree, 'header')[0])), 'Undo brings the edit back');
    H.unmount(); location.search = '';
  }

  // ---- the Render step and the live size: baked a pause after the last edit, kept for Send and
  // Export, said in the top bar and on Render; an app's refusal said there; an edit starts the wait again
  {
    HOST.SIZE_PAUSE_MS = 300;
    let bakes = 0;
    const was = app.bake;
    app.bake = async (d, o) => { bakes++; return was(d, o); };
    location.search = '?step=render&preset=sun';
    const H = mount(HOST.Editor, { app });
    await H.settle(20);
    await press(H, '❚❚');
    const big = by(H.tree, 'button', 'st-big').map((b) => text(by(b, 'b')[0]));
    ok(big.join(' / ') === 'Save to library / Send to orb / Export .orbtheme', 'Render: save, send, export (Check the bake is gone)', big.join(' / '));
    const sizeLine = () => text(by(H.tree, 'div', 'hs-size')[0]);
    const fitText = () => text(by(H.tree, 'div', 'st-fit')[0]);
    ok(/Measuring its size when you pause/.test(sizeLine()) && /…$/.test(fitText()), 'before the pause it says it will measure', sizeLine() + ' / ' + fitText());
    await wait(350); await H.settle(10);
    ok(bakes === 1 && /^Size4 KB of the 8,320 KB an orb takes \(0%\) · baked in /.test(sizeLine()), 'after it, the size against what an orb takes', sizeLine());
    ok(/· 4 KB$/.test(fitText()), 'and in the top bar, in the same KB', fitText());
    const e = by(H.tree, 'button', 'st-big')[2];          // Export: the kept bake, no second one
    window.showSaveFilePicker = async () => { const x = new Error('closed'); x.name = 'AbortError'; throw x; };
    e.props.onClick(); await wait(30); H.render(); await H.settle(10);
    ok(bakes === 1, 'Export sends the kept bake: nothing changed, nothing baked again', bakes);
    // an edit starts the wait again; two edits inside it bake once
    by(H.tree, 'input').filter((i) => /name/i.test(i.props.placeholder || ''))[0].props.onChange({ target: { value: 'Sun 2' } }); H.render();
    await wait(60);
    by(H.tree, 'input').filter((i) => /name/i.test(i.props.placeholder || ''))[0].props.onChange({ target: { value: 'Sun 3' } }); H.render();
    ok(/Measuring/.test(sizeLine()), 'an edit puts the size back to measuring', sizeLine());
    await wait(80); await H.settle(5);
    ok(bakes === 1, 'an edit inside the pause restarts it', bakes);
    await wait(400); await H.settle(10);
    ok(bakes === 2 && /^Size4 KB/.test(sizeLine()), 'and the pause after the last edit bakes once', bakes + ' ' + sizeLine());
    // an app's refusal is the readout's line, and Send's
    bakeWhy = 'pick a picture first';
    by(H.tree, 'input').filter((i) => /name/i.test(i.props.placeholder || ''))[0].props.onChange({ target: { value: 'Sun 4' } }); H.render();
    await wait(450); await H.settle(10);
    ok(/Not baked: pick a picture first/.test(sizeLine()) && /not baked$/.test(fitText()), 'an app’s refusal is said on Render and in the top bar', sizeLine());
    bakeWhy = null;
    // a theme an orb will not take says so
    const big2 = HOST.sizeWords({ state: 'done', bytes: 9 << 20, ms: 1500 }, GorbBake.THEME_UPLOAD_MAX);
    ok(big2.over && /an orb will refuse it/.test(big2.line) && big2.short === '9,216 KB', 'a theme past what an orb takes is said to be refused', big2.line);
    H.unmount(); app.bake = was; delete window.showSaveFilePicker;
    location.search = ''; location.pathname = '/';
    const off = mount(HOST.Editor, { app });
    ok(/Open this editor from Orb Manager/.test(text(off.tree)) && !by(off.tree, 'canvas').length, 'off the hub it says where it opens');
    off.unmount();
    location.pathname = '/editor/192.0.2.7/tint/';
  }

  // ---- an orb with no home and no places: the place control is home's alone; home: false hides it
  {
    orbAnswers = { home: { home: { set: false } }, places: { places: [] }, units: {} };
    const H = mount(HOST.Editor, { app: Object.assign({}, app, { views: [app.views[3], { key: 'away', kind: 'place', home: false, label: 'Away' }] }) });
    await H.settle(20);
    const caps = by(by(H.tree, 'div', 'st-tool')[0], 'span', 'st-lab').map(text);
    ok(caps.join() === 'Place' && last().place === null, 'no places set: home’s place view shows Home alone, a home-less one shows no control, and the frame has no place', caps.join() + ' ' + last().place);
    ok(last().unit === 'km', 'and the unit is km', last().unit);
    H.unmount();
  }

  console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw: ' + (e && e.stack || e)); process.exit(1); });
