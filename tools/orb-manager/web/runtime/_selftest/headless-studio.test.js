#!/usr/bin/env node
// studio@1 (theme-editor-kit-plan K6): the editor shell, with no app around it. Each component
// is rendered through a recording stand-in for React (createElement builds a plain tree, and
// function components are expanded), so the suite reads what a component draws for its props;
// the builders are held to the theme@1 services' own answers.
//
//   node headless-studio.test.js
'use strict';
const path = require('path');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };

global.window = globalThis;
const FRAG = Symbol('fragment');
// hooks: a ref is a box; a layout effect is kept so a check can run it after attaching a node
const effects = [];
global.React = { Fragment: FRAG, createElement: (type, props, ...kids) => ({ type, props: props || {}, kids: kids.flat(Infinity).filter((k) => k !== null && k !== undefined && k !== false) }),
  useRef: (v) => ({ current: v }), useLayoutEffect: (fn) => effects.push(fn) };
global.GorbBake = require(path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'));
require(path.join(__dirname, '..', 'theme@1', 'preview.js'));
require(path.join(__dirname, '..', 'theme@1', 'measure.js'));
require(path.join(__dirname, '..', 'studio@1', 'studio.js'));
const S = OrbStudio, T = OrbTheme;
// expand function components, so the tree is DOM-shaped
const expand = (n) => {
  if (n == null || typeof n !== 'object') return n;
  if (typeof n.type === 'function') return expand(n.type(Object.assign({}, n.props, n.kids.length ? { children: n.kids.length === 1 ? n.kids[0] : n.kids } : {})));
  return { type: n.type, props: n.props, kids: n.kids.map(expand).filter((k) => k !== null && k !== undefined) };
};
const render = (C, p) => expand(C(p || {}));
const all = (n, f, out) => { out = out || []; if (n && typeof n === 'object') { if (f(n)) out.push(n); (n.kids || []).forEach((k) => all(k, f, out)); } return out; };
const text = (n) => (n == null ? '' : typeof n !== 'object' ? String(n) : (n.kids || []).map(text).join(''));
const by = (n, tag, cls) => all(n, (x) => x.type === tag && (!cls || String(x.props.className || '').split(' ').includes(cls)));
// a fresh handler per use, so a check that finds a button by its handler finds that one
const mkfn = () => () => {};
Object.defineProperty(global, 'fn', { get: mkfn });

// ---- the frame and the top bar
{
  const f = render(S.Frame, { children: 'x' });
  ok(f.type === 'div' && f.props.className === 'st', 'the frame is one grid');
  const back = fn, undo = fn, send = fn, name = fn;
  const t = render(S.TopBar, { back, crumb: 'Radar', name: { value: 'Scope', placeholder: 'Untitled', title: 'Name', onChange: name },
    steps: [{ label: 'Start', cls: '', pick: fn }, { label: 'Planes', cls: 'on', pick: fn }],
    undo: { on: undo, tip: 'Undo', op: 1 }, redo: { on: fn, tip: 'Nothing to redo', op: 0.4 },
    extra: [{ label: 'Reset Scope', on: fn }], fit: { dot: '#3fb950', text: 'Fits · 15 fps', tip: 't' }, primary: { label: 'Send to orb', on: send } });
  ok(t.type === 'header' && t.props.className === 'st-top', 'the top bar is one header');
  const btns = by(t, 'button');
  ok(btns[0].props.onClick === back && text(btns[0]) === '←', 'it has the way back first');
  ok(by(t, 'input', 'st-name')[0].props.value === 'Scope' && by(t, 'input', 'st-name')[0].props.onChange === name, 'then the name field');
  ok(by(t, 'nav', 'st-steps')[0].kids.map(text).join() === 'Start,Planes' && by(t, 'nav', 'st-steps')[0].kids[1].props.className === 'on', 'then the steps, the current one marked');
  ok(btns.some((b) => b.props.onClick === undo && b.props.style.opacity === 1) && btns.some((b) => text(b) === 'Reset Scope'), 'undo, redo and the app’s own buttons');
  ok(text(by(t, 'div', 'st-fit')[0]) === 'Fits · 15 fps' && btns[btns.length - 1].props.onClick === send && text(btns[btns.length - 1]) === 'Send to orb', 'the fit dot, and the primary action last');
  const bare = render(S.TopBar, { crumb: 'Radar' });
  ok(!by(bare, 'button').length && !by(bare, 'input').length, 'a bare top bar draws only its crumb and steps');
}

// ---- panes, stage, glass, toolbar
{
  ok(render(S.Pane, { side: 'left' }).props.className === 'st-side st-left' && render(S.Pane, { side: 'right' }).type === 'aside' && render(S.Pane, { side: 'right' }).props.className === 'st-side st-right', 'a pane on either side');
  ok(render(S.Stage, {}).type === 'main' && render(S.Stage, {}).props.className === 'st-stage', 'the stage');
  const ref = { current: null }, down = fn;
  const g = render(S.Glass, { size: 300, round: '50%', scrollRef: ref, onPointerDown: down, zoomTag: 'Fit · 1.00×', status: 'Reopened', children: { type: 'canvas', props: {}, kids: [] } });
  const sc = by(g, 'div', 'st-scroll')[0], gl = by(g, 'div', 'st-glass')[0];
  ok(typeof sc.props.ref === 'function' && sc.props.onPointerDown === down && gl.props.style.width === '300px' && gl.props.style.borderRadius === '50%', 'the glass is sized and round in a scroll it measures');
  ok(by(gl, 'canvas').length === 1, 'and holds the app’s canvas');
  ok(text(by(g, 'div', 'st-ztag')[0]) === 'Fit · 1.00×' && text(by(g, 'div', 'st-toast')[0]) === 'Reopened', 'the zoom tag and the status over it');
  ok(!by(render(S.Glass, { size: 300 }), 'div', 'st-toast').length, 'no status, no toast');
  // Fit: the glass measures its own scroll after a render and reports the size Fit draws at
  ok(sc.props.ref.length === 1, 'the scroll takes a ref callback the glass measures with');
  const fits = [];
  effects.length = 0;
  const g2 = render(S.Glass, { size: 466, scrollRef: ref, onFit: (px) => fits.push(px) });
  by(g2, 'div', 'st-scroll')[0].props.ref({ getBoundingClientRect: () => ({ width: 596, height: 580 }) });
  effects.forEach((e) => e());
  ok(fits.join() === '540' && ref.current && ref.current.getBoundingClientRect().width === 596, 'Fit is the pane’s smaller side less the ring’s margin, and the app’s ref holds the scroll', fits.join());
  effects.forEach((e) => e());
  ok(fits.length === 1, 'an unchanged pane reports nothing');
  ok(S.fitOf(150, 150) === 160 && S.fitOf(1240, 1032) === 992, 'never under 160');
  const play = fn, wipe = fn;
  const tb = render(S.Toolbar, { play: { icon: '▶', label: 'Play', on: play }, time: [{ label: 'Live', cls: 'on' }], view: T.VIEWS.map(([id, label]) => ({ label })), zoom: null,
    render: T.RENDERS.map(([id, label]) => ({ label })), wipe: { value: 200, on: wipe }, children: { type: 'button', props: { className: 'mine' }, kids: ['⚙'] } });
  ok(by(tb, 'button', 'st-play')[0].props.onClick === play && by(tb, 'div', 'ui-seg').length === 3, 'play, then the time, view and render choices');
  ok(by(tb, 'input')[0].props.value === 200 && by(tb, 'input')[0].props.onChange === wipe, 'the split’s wipe while splitting');
  ok(by(tb, 'button', 'mine').length === 1, 'and the app’s own tools after them');
  ok(by(render(S.Toolbar, { view: [{ label: 'Fit' }], zoom: T.ZOOMS.map((z) => ({ label: z + '×' })) }), 'div', 'ui-seg').length === 2, 'the zoom stops only while magnifying');
}

// ---- true size's instruments, from the screen's own answers
{
  const V = T.view({ view: 'true', cal: 1.25, calibrated: true });
  const solve = fn, card = fn;
  const p = S.calProps(V, { min: 0.5, max: 3, step: 0.005, value: 1.25, onChange: fn, up: fn, down: fn, solve, reset: fn, solveCard: card });
  ok(p.show && p.ruler.px === V.rulerPx && p.ruler.ticks.length === 41 && p.cal.text === V.calText && p.card.w === V.card.w && p.card.solve === card, 'the instruments carry the view’s ruler and the card at this calibration');
  const c = render(S.Calibrate, p);
  ok(by(c, 'div', 'st-ruler')[0].props.style.width === V.rulerPx + 'px' && by(c, 'div', 'st-tick').length === 41 && by(c, 'div', 'st-tlab').length === 5, 'the ruler: forty-one ticks, five labels');
  ok(by(c, 'div', 'st-card')[0].props.style.width === V.card.w + 'px' && by(c, 'input').filter((i) => i.props.onChange === card).length === 1, 'the bank card outline and its reading');
  ok(by(c, 'input').filter((i) => i.props.onChange === solve).length === 1 && /True size: /.test(text(c)), 'the ruler’s reading, and the caption');
  const fit = render(S.Calibrate, S.calProps(T.view({ view: 'fit', cal: 1 }), {}));
  ok(!by(fit, 'div', 'st-ruler').length && /Fitted to the stage/.test(text(fit)), 'outside True size only the caption shows');
  ok(!by(render(S.Calibrate, S.calProps(V, { solve })), 'div', 'st-card').length, 'and with no card handler, no card');
}

// ---- the send and the duplicate question, from the sender's own state
{
  ok(render(S.SendModal, S.sendProps(null, '', fn)) === null, 'no send, no modal');
  const cancel = fn;
  const a = S.sendProps({ canCancel: true, cancelling: false }, 'Sending "Scope" — 40%', cancel);
  ok(a.open && a.line === 'Sending "Scope" — 40%' && /Cancel stops it cleanly/.test(a.hint) && !a.noCancel && a.cancelLabel === 'Cancel' && a.onCancel === cancel, 'while the bytes are the page’s, Cancel is live');
  const b = S.sendProps({ canCancel: false, cancelling: false }, '', cancel);
  ok(b.noCancel && b.line === 'Building the theme…' && /cannot be stopped part way/.test(b.hint), 'once the orb holds the job, it waits');
  const c = S.sendProps({ canCancel: false, cancelling: true }, 'x', cancel);
  ok(c.cancelLabel === 'Cancelling…' && /stops before anything reaches the orb/.test(c.hint), 'a cancel in flight says so');
  const m = render(S.SendModal, a);
  ok(by(m, 'div', 'st-modal').length === 1 && /Sending to the orb/.test(text(m)) && by(m, 'button')[0].props.onClick === cancel && !by(m, 'button')[0].props.disabled, 'the modal draws the line, the hint and Cancel');
  ok(by(render(S.SendModal, b), 'button')[0].props.disabled === true, 'and disables Cancel when it cannot stop');
  ok(render(S.DupModal, S.dupProps(null, fn, fn)) === null, 'no question, no modal');
  const rep = fn, dis = fn, d = render(S.DupModal, S.dupProps({ nm: 'Scope', stem: 'radar-scope-000000000000', note: 'a different build' }, rep, dis));
  ok(/already on the card/.test(text(d)) && /radar-scope-000000000000/.test(text(d)) && /a different build/.test(text(d)), 'the duplicate names the theme, its card stem and the hub’s note');
  ok(by(d, 'button').map((x) => x.props.onClick === rep ? 'R' : x.props.onClick === dis ? 'C' : '?').join('') === 'CR' && by(d, 'div', 'st-modal')[0].props.onClick === dis, 'Cancel and Replace, and a click outside cancels');
}

// ---- the Render panel and the fit, from the verdict
{
  const V = T.fit({ rows: 3, cap: 24, want: 15, est: { est_fps: 9 } });
  ok(S.fitProps(V, 'tip').text === V.short && S.fitProps(V).dot === V.color, 'the top bar’s dot is the verdict');
  const send = fn, dl = fn;
  const r = render(S.RenderPanel, { badge: S.badgeProps(V, 'tip'), send: { note: 'pushes to 192.0.2.7', on: send }, downloads: [{ label: 'Download .orbtheme', note: 'FORMAT 3', on: dl }], note: '1/1 art asset held' });
  ok(text(by(r, 'div', 'st-badge')[0]).includes(V.text) && text(by(r, 'div', 'st-badge')[0]).includes(V.note) && by(r, 'div', 'st-badge')[0].props.style.borderColor === V.border, 'the badge is the verdict');
  const big = by(r, 'button', 'st-big');
  ok(big.length === 2 && big[0].props.onClick === send && /Send to orb/.test(text(big[0])) && big[1].props.onClick === dl, 'Send first, then the downloads');
  ok(/1\/1 art asset held/.test(text(r)), 'and the app’s warning line');
  ok(by(render(S.RenderPanel, { downloads: [] }), 'button').length === 0, 'off a hub there is no Send');
}

console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
