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
  useRef: (v) => ({ current: v }), useLayoutEffect: (fn) => effects.push(fn), useEffect: () => {},
  // a state is what the next check sets: STATE.next, else the initial value
  useState: (v) => [STATE.next !== undefined ? STATE.next : v, (x) => { STATE.set = x; }] };
const STATE = { next: undefined, set: undefined };
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

// ---- status notes (owner, 2026-11-09): prompts fade, work in progress stays, errors keep a ×
{
  const K = S.noteKind;
  ok(['Starting from Skeleton: Continue when you are ready', 'Redone', 'Opened “Sea” from the library', 'Exported “Sea” as x.orbtheme', 'Not exported'].every((t) => K(t) === 'note'),
     'a prompt or an acknowledgement is a note that fades');
  ok(['Baking “Sea”…', 'Waiting for the movement to finish drawing (8 of 15 frames) — keep this window in front', 'Sending "Sea" to 1.2.3.4 — 40% of 3 B to the hub'].every((t) => K(t) === 'busy'),
     'work in progress stays while it runs');
  ok(['Not baked: the movement stopped drawing at 8 of 15 frames', 'Not sent: HTTP 500', 'Could not reopen that theme: gone', 'Send failed: reset', 'Not baked: the Tint bake did not answer in 120 s'].every((t) => K(t) === 'error'),
     'an error is an error');
  let gone = 0;
  const err = render(S.Glass, { size: 466, status: 'Not baked: no hand at all', onDismiss: () => { gone++; } });
  const x = by(err, 'button', 'st-x')[0];
  ok(by(err, 'div', 'st-err').length === 1 && x && (x.props.onClick(), gone === 1), 'an error carries a × that dismisses it');
  const nt = render(S.Glass, { size: 466, status: 'Redone' });
  ok(by(nt, 'div', 'st-fade').length === 1 && !by(nt, 'button', 'st-x').length, 'a note fades and has nothing to press');
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
  ok(by(render(S.SendModal, b), 'button')[0].props.disabled === true && b.cancelLabel === 'Waiting for the orb…', 'and, when it cannot stop, its button waits for the orb instead of offering a Cancel it cannot keep', b.cancelLabel);
  // owner, 2026-11-08: the verdict stays up, and a button closes it
  const v = S.sendProps({ done: true, outcome: 'ok' }, 'Sent to 192.0.2.7 as "Scope"', cancel), vm = render(S.SendModal, v);
  ok(v.open && /Sent to the orb/.test(text(vm)) && /as "Scope"/.test(text(vm)) && by(vm, 'button')[0].props.onClick === cancel && !by(vm, 'button')[0].props.disabled && text(by(vm, 'button')[0]) === 'Done',
     'a sent theme ends on its verdict with Done', text(vm));
  const vf = S.sendProps({ done: true, outcome: 'fail' }, 'Not sent: HTTP 500', cancel), vw = S.sendProps({ done: true, outcome: 'warn' }, 'x', cancel);
  ok(vf.title === 'Not sent' && vf.cancelLabel === 'Close' && !vf.noCancel && vw.cancelLabel === 'Done' && /check the orb/.test(vw.title), 'a failure ends on Close, an unwitnessed store on a check', vf.title + ' / ' + vw.title);
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
  const sv = fn, rs = by(render(S.RenderPanel, { send: { note: 'x', on: send }, save: { note: 'keep it', on: sv }, downloads: [] }), 'button', 'st-big');
  // §8.1 ruling 6: Save to library, Send to orb, then the export
  ok(rs.length === 2 && rs[0].props.onClick === sv && /Save to library/.test(text(rs[0])) && rs[1].props.onClick === send, 'Save to library sits above Send');
}

// ---- K7: the field kit, the fields, the layers panel
{
  const drafts = {}; let redraws = 0, got = null;
  const K = S.fieldKit({ drafts, refresh: () => redraws++, scope: () => 'arm', inherit: [{ re: /^Same as scope/, src: 'the scope' }, { re: /^Default\b/, src: 'the defaults' }] });
  const set = (v) => { got = v; };
  const r = K.Rg('Trail', 0.5, -10, 10, 0.1, set, '0.5');
  ok(r.isRange && r.isRow && !r.isHead && r.min === -10 && r.valText === '0.5', 'a slider row');
  r.on({ target: { value: '0.2' } }); ok(got === 0, 'it snaps to 0 within 1.5% of its span', got);
  r.on({ target: { value: '3.05' } }); ok(got === 3, 'and to a whole number within 0.6% on a fine one', got);
  r.onNum({ target: { value: '99' } }); ok(got === 10, 'a typed value is clamped to the range', got);
  r.onNum({ target: { value: '' } }); ok(got === 10, 'and an empty box changes nothing');
  const sl = K.Sl('Ring colour', 'inherit', [{ v: 'inherit', label: 'Same as scope' }, { v: 'red', label: 'Red' }], set);
  ok(sl.isSelect && sl.opts[0].sel && sl.hasLink && /the scope/.test(sl.linkText) && sl.linkCursor === 'default', 'a select that follows its source says so');
  const sl2 = K.Sl('Ring colour', 'red', [{ v: 'inherit', label: 'Same as scope' }, { v: 'red', label: 'Red' }], set);
  ok(sl2.linkText === '↺ re-link' && (sl2.relink(), got === 'inherit'), 'set here, one click re-links it', got);
  ok(!K.Sl('Band', 'a', [{ v: 'a', label: 'Match movement' }], set).hasLink, 'a choice no inherit rule names is a plain choice');
  const dflt = K.Sl('Length', 'def', [{ v: 'def', label: 'Default (the arm’s)' }, { v: 'long', label: 'Long' }], set);
  ok(dflt.hasLink && /the defaults/.test(dflt.linkText), 'a later rule applies when the first names no option', dflt.linkText);
  const swIn = K.Sw('Band', 'inherit', [{ k: 'b1', label: 'Green', bg: '#0f0' }], set, [{ label: 'Same as scope', text: '=' }], () => 'inherit');
  ok(swIn.hasLink && /the scope/.test(swIn.linkText), 'an extra option stands for the value vOf names, so following reads as linked', swIn.linkText);
  const ch = K.Ch('Arm', 'wedge', [{ v: 'wedge', label: 'Wedge' }, { v: 'needle', label: 'Needle' }], set);
  ok(ch.isChips && ch.opts[0].cls === 'on' && ch.opts[1].cls === '' && (ch.opts[1].pick(), got === 'needle'), 'chips mark the choice and pick');
  const sw = K.Sw('Band', 'b2', [{ k: 'b1', label: 'Green', bg: '#0f0' }, { k: 'b2', label: 'Amber', bg: '#fa0' }], set, [{ label: 'Same as scope', text: '=', bg: '#1c2330', bd: '#2b3540', pick: () => set('inherit') }], (o) => 'inherit');
  ok(sw.isSwatch && sw.valText === 'Amber' && sw.opts.length === 3 && sw.opts[2].bd === '#e6edf3' && sw.hasLink && !('v' in sw.opts[0]), 'swatches name the chosen one; an extra option stays as given', JSON.stringify(sw.opts[0]));
  const co = K.Co('Ink', '#112233', set);
  S.setPalette('classic');
  ok(co.isColor && co.value === '#112233' && co.hexTitle === 'rgb 17, 34, 51' && co.opts.length === 1 + S.PALETTES.classic.length && !co.picker && !co.pre, 'a colour: Auto and the classic palette, no picker, no preset named');
  ok(S.PALETTES.classic.slice(0, S.INKS.length).every((c, i) => c[0] === S.INKS[i][0]), 'the classic palette starts with the inks');
  co.onHex({ target: { value: '#12' } });
  ok(drafts['arm:Ink'] === '#12' && redraws === 1 && K.Co('Ink', '#112233', set).hexText === '#12', 'a half-typed hex is kept as a draft under its scope and redrawn');
  co.onHex({ target: { value: '10, 20, 30' } });
  ok(got === '#0a141e' && !('arm:Ink' in drafts), 'r, g, b is read as a colour and clears the draft', got);
  ok(S.parseColor('#abc') === '#aabbcc' && S.parseColor('rgb(300,0,0)') === null && S.parseColor('nope') === null, 'colour parsing');
  ok(K.Co('Ink', 'none', set, 'Auto', 'x', { none: true }).valText === 'None' && K.Co('Ink', '', set, 'Auto', 'x', { noAuto: true }).opts.length === S.PALETTES.classic.length, 'None and no-Auto');
  // §12.5: the preset's colour first, and the palette chosen once for every row
  const pc = K.Co('Ink', '#112233', set, '', 'x', { noAuto: true, preset: '#3DFF8E' });
  ok(pc.pre && pc.pre.bg === '#3DFF8E' && pc.pre.text === 'P' && (pc.pre.pick(), got === '#3DFF8E'), 'the preset’s colour is offered first, and picks it');
  ok(K.Co('Ink', '#3dff8e', set, '', 'x', { preset: '#3DFF8E' }).pre.bd === '#e6edf3', 'the preset swatch is marked while the colour is the preset’s, in any case');
  const tabs = pc.palTabs.map((t) => t.label).join();
  pc.palTabs[1].pick();
  const vv = K.Co('Ink', '#112233', set, '', 'x', { noAuto: true });
  ok(tabs === 'Classic,Vivid,Any' && S.palette === 'vivid' && vv.opts.length === S.PALETTES.vivid.length && vv.palTabs[1].on && redraws > 1, 'Vivid shows the vivid palette on every row after', tabs);
  S.setPalette('any');
  const va = K.Co('Ink', '#112233', set, '', 'x', { noAuto: true });
  ok(va.picker && va.opts.length === 0, 'Any is the system picker, no swatches');
  S.setPalette('classic');
  const rows = [1, 2, 3, 4, 5, 6, 7].map((i) => K.Nt('n' + i, 'x'));
  let opened = 0;
  const f5 = K.fold(rows, 5, false, () => opened++);
  ok(f5.length === 6 && /More \(2\)/.test(f5[5].opts[0].label) && (f5[5].opts[0].pick(), opened === 1), 'a long run folds to its first rows and a More chip');
  ok(K.fold(rows, 5, true, () => {}).length === 8 && K.fold(rows.slice(0, 3), 5, false, () => {}).length === 3, 'open shows all with Less; a short run is not folded');
  // the component
  const F = render(S.Fields, { fields: [K.H('Arm'), r, sl, co, K.Tx('Name', 'Scope', set), K.Fi('Image', set), K.Nt('', 'a note')] });
  ok(by(F, 'div', 'st-sec').length === 1 && by(F, 'div', 'st-row').length === 5, 'one head, five rows: the note is no row of its own');
  const rowsF = by(F, 'div', 'st-row');
  ok(rowsF[0].props.className === 'st-row rng' && rowsF.slice(1).every((x) => x.props.className === 'st-row wide'), 'a slider row gives the value column to the slider; every other row is wide');
  // a slider's reading: the number (in px, a multiple or a percentage) is the box's; words sit under it
  const slv = (vt, v) => { const X = render(S.Fields, { fields: [K.Rg('Size', v, 0, 400, 1, set, vt)] }); return { u: by(X, 'span', 'st-u').map(text), c: by(X, 'div', 'st-cname').map(text).join('') }; };
  const s1 = slv('120px', 120), s2 = slv('80%', 0.8), s3 = slv('4 px past the rings', 4), s4 = slv('closed', 0);
  ok(!s1.c && !s2.c, 'a slider reading only its number (in px, or as a percentage) is not repeated', JSON.stringify([s1, s2]));
  ok(s3.c === '4 px past the rings' && s4.c === 'closed', 'words sit under the slider, short or long', JSON.stringify([s3, s4]));
  ok(!s1.u.length && !s3.u.length && !s4.u.length, 'and nothing rides beside the number box, so every slider in a pane is one width');
  // a section folds: a closed head hides its rows until the next head, and says so with its arrow
  let shutClicks = 0;
  const FF = render(S.Fields, { fields: [K.Tx('Name', 'x', set), K.H('Shut', null, { open: false, toggle: () => shutClicks++ }), K.Rg('Hidden', 1, 0, 2, 1, set, '1'),
    K.H('Open', null, { open: true, toggle: () => {} }), K.Rg('Shown', 1, 0, 2, 1, set, '1')] });
  const secs = by(FF, 'div', 'st-sec'), rowsFF = by(FF, 'div', 'st-row');
  ok(secs.length === 2 && rowsFF.length === 2 && text(secs[0]).indexOf('▸') === 0 && text(secs[1]).indexOf('▾') === 0, 'a closed section hides its rows; the Name above every section always shows', rowsFF.length);
  secs[0].props.onClick(); ok(shutClicks === 1, 'clicking a section head toggles it');
  // a reading prints its value on the row, its why on the hover
  const RR = render(S.Fields, { fields: [K.Rd('Frame rate', '12 fps', 'why')] });
  ok(text(by(RR, 'div', 'st-cname')[0]) === '12 fps' && by(RR, 'label')[0].props.title === 'why', 'a reading prints its value, its why rides the hover');
  // a canvas row hands its ref to the app, which draws into it
  const ref = () => {}, CC = render(S.Fields, { fields: [K.Cv('Pivot', ref, 112, 96, set, set)] });
  ok(by(CC, 'canvas').length === 1 && by(CC, 'canvas')[0].props.ref === ref && by(CC, 'canvas')[0].props.width === 112, 'a canvas row carries the app\u2019s ref and its pixel size');
  ok(by(rowsF[0], 'input').length === 2 && by(rowsF[1], 'select')[0].props.value === 'inherit' && by(rowsF[1], 'span').some((x) => /the scope/.test(text(x))), 'a slider with its number box; a select with its link');
  ok(by(rowsF[2], 'button', 'st-sw').length === 1 && by(rowsF[2], 'button', 'st-sw-cur').length === 1 && by(rowsF[2], 'input').length === 1 && !by(rowsF[2], 'div', 'st-pal-box').length,
    'a colour row, shut: the colour itself and its hex box');
  STATE.next = true;
  const openRow = by(render(S.Fields, { fields: [co] }), 'div', 'st-row')[0];
  STATE.next = undefined;
  ok(by(openRow, 'div', 'st-pal-box').length === 1 && by(openRow, 'button', 'st-sw').length === 1 + co.opts.length && by(openRow, 'div', 'st-pal')[0].kids.length === 3,
    'open: the palette tabs and the palette’s swatches under it');
  const lab5 = by(rowsF[4], 'label')[0];
  ok(lab5.props.title === 'a note' && by(rowsF[4], 'span', 'st-tip').length === 1 && !/a note/.test(text(F)), 'a note is the hover text of the row above it, marked, and not printed');
  const G2 = render(S.Fields, { fields: [K.H('Arm'), K.Nt('', 'about arms'), K.Nt('Rule', 'a labelled note')] });
  ok(by(G2, 'div', 'st-sec')[0].props.title === 'about arms' && by(G2, 'div', 'st-row').length === 1 && by(by(G2, 'div', 'st-row')[0], 'span', 'st-tip')[0].props.title === 'a labelled note',
    'under a head it is the head’s; a labelled note keeps a row with only its mark');
}
{
  // §12.5: the screen, one controller for every editor: the app's state, the OS's meaning
  let st = { view: 'fit', zoom: 2, fitPx: 400, cal: 1, open: false }; const said = [];
  const V = S.viewer({ get: () => st, set: (p) => { st = Object.assign({}, st, p); }, say: (t) => said.push(t) });
  ok(V.init().view === 'fit' && V.init('true').view === 'true', 'an editor opens on the view it names');
  V.setView('zoom'); ok(st.view === 'zoom' && st.zoom === 2, 'Magnify keeps a zoom');
  V.setZoom(9); ok(st.zoom === 4, 'a zoom past 4 is 4');
  V.setView('true'); ok(V.glass().size === T.screen.realCss(1) && /True size/.test(V.glass().zoomTag), 'True size draws the panel’s real width');
  V.solveRuler('20'); ok(st.cal === T.screen.fromRuler(1, 20) && V.calibrated() && /Ruler matched at 20 mm/.test(said.pop()), 'a ruler reading solves the calibration and says so');
  V.solveCard('900'); ok(/between 20 and 400/.test(said.pop()) && st.cal === T.screen.fromRuler(1, 20), 'an impossible card reading is refused, the calibration kept');
  V.onFit(401); ok(st.fitPx === 400, 'Fit ignores a 1 px wobble'); V.onFit(420); ok(st.fitPx === 420, 'and follows a real resize');
  const tb = V.toolScreen(); tb.on(); ok(st.open === true && V.panel().open && V.panel().cal.show, 'the ⚙ opens the screen panel, its instruments shown in True size');
  const tl = V.tool(); ok(tl.view.length === T.VIEWS.length && tl.view.filter((v) => v.cls === 'on').length === 1 && tl.zoom === null, 'the view options, one marked; zoom only while magnifying');
  const sp = render(S.ScreenPanel, V.panel());
  ok(sp.props.className === 'st-diag' && by(sp, 'div', 'st-cal').length === 1 && by(sp, 'div', 'st-card').length === 1, 'the screen panel holds the ruler and the bank card');
  ok(render(S.ScreenPanel, { open: false }) === null, 'a shut panel draws nothing');
  const tool = render(S.Toolbar, { screen: { open: true, on: fn } });
  ok(by(tool, 'button').some((b) => text(b) === '⚙' && /\bon\b/.test(b.props.className)), 'the toolbar carries the screen button');
}
{
  // §12.5: the start sheet and the library's rows
  const picked = [], forgot = [];
  let armed = null;
  const rows = S.libraryRows([{ name: 'Night', state: 'changed', at: 0 }, { name: 'Day', state: 'draft', at: 0, theme: { stale: 'old rows' } }],
    { version: '1.2.0', armed: 'Day', arm: (n) => { armed = n; }, open: (n) => picked.push(n), forget: (n, w) => forgot.push(n + ':' + w), meta: () => 'Brass · ' });
  ok(rows[0].tag === 'theme · unsent changes' && rows[0].changed && rows[1].tag === 'theme · needs a rebake' && /^Brass · /.test(rows[0].meta), 'each entry says what it is');
  rows[0].load(); rows[0].discard(); rows[0].del(); rows[1].del();
  ok(picked.join() === 'Night' && forgot.join() === 'Night:draft,Day:all' && armed === null && rows[1].delLabel === 'Press again', 'Open, Discard changes, and Delete asked twice');
  // the start (§8.10): four ways, each saying what it holds; one panel at a time
  let picked2 = null, freshed = 0;
  const imp = fn, go = fn, base = { title: 'Start a tint theme', setPick: (p) => { picked2 = p; }, fresh: { note: 'Plain greys', on: () => { freshed++; } },
    tiles: [{ key: 'a', name: 'Alpha', short: 's', cls: 'on', on: fn }, { key: 'b', name: 'Beta', on: fn }],
    library: { rows, empty: 'Nothing saved yet.' }, importFile: { accept: '.orbtheme', on: imp }, foot: [{ label: 'Continue with Alpha', primary: true, on: go }] };
  const four = render(S.StartFlow, base), ways = by(four, 'button', 'st-choice');
  ok(ways.map((b) => text(by(b, 'b')[0])).join('|') === 'New|From a template|From my library|Import a file', 'four ways to start, in the owner’s order', ways.map((b) => text(by(b, 'b')[0])).join('|'));
  ok(/2 looks to start from/.test(text(ways[1])) && /2 saved designs/.test(text(ways[2])) && /\.orbtheme/.test(text(ways[3])), 'each says what it holds', ways.map(text).join(' / '));
  ok(!by(four, 'div', 'st-gc').length && !by(four, 'div', 'st-lib-row').length && !by(four, 'div', 'st-sheet-foot').length, 'no previews, no library rows and no Continue under the four');
  ways[0].props.onClick(); ways[1].props.onClick();
  ok(freshed === 1 && picked2 === 'template', 'New starts at once; a template opens its panel');
  ways[2].props.onClick(); ok(picked2 === 'library', 'the library opens its panel');
  const file = by(four, 'input').filter((i) => i.props.type === 'file');
  ok(file.length === 1 && file[0].props.onChange === imp && file[0].props.accept === '.orbtheme', 'Import is the one file input');
  const tpl = render(S.StartFlow, Object.assign({}, base, { pick: 'template' }));
  ok(by(tpl, 'h2').map(text).join() === 'From a template' && by(tpl, 'div', 'st-gc').length === 2 && by(tpl, 'canvas').length === 2 && !by(tpl, 'div', 'st-lib-row').length,
     'the template panel: the looks as pictures, nothing else');
  ok(by(tpl, 'button').some((b) => b.props.onClick === go && text(b) === 'Continue with Alpha' && b.props.className === 'ui-primary'), 'and Continue');
  const backB = by(tpl, 'button', 'st-back')[0]; backB.props.onClick();
  ok(picked2 === '' && /All ways to start/.test(text(backB)), 'a way back to the four');
  const lib = render(S.StartFlow, Object.assign({}, base, { pick: 'library' }));
  ok(by(lib, 'h2').map(text).join() === 'From my library' && by(lib, 'div', 'st-lib-row').length === 2 && !by(lib, 'div', 'st-gc').length && !by(lib, 'div', 'st-sheet-foot').length,
     'the library panel: your designs, nothing else');
  const none = render(S.StartFlow, Object.assign({}, base, { pick: 'library', library: { rows: [], empty: 'Nothing saved yet.' } }));
  ok(by(none, 'div', 'st-lib-empty').length === 1 && /Nothing saved yet/.test(text(render(S.StartFlow, Object.assign({}, base, { library: { rows: [] } })))), 'an empty library says so, on its card and in its panel');
  ok(!('StartSheet' in S), 'one way to start: the old sheet is gone');
}
{
  // layerRows: bands, depth, selection, arrows, eye, tooltip; a made-up radar stack
  const items = [{ id: 'glass', band: 'top', label: 'Glass', movable: false }, { id: 'arm', band: 'live', label: 'Arm', movable: true },
    { id: 'jet', band: 'live', label: 'Jet', parent: 'arm', movable: true }, { id: 'tag', band: 'live', label: 'Tag', parent: 'jet', movable: false }, { id: 'plate', band: 'base', label: 'Scope', movable: false }];
  const moved = [], picked = [], eyes = [];
  const rows = S.layerRows(items, { sel: 'jet', heads: { top: 'On top', live: 'Moving', base: 'Plate' }, icon: (it) => it.id[0], note: (it) => (it.id === 'arm' ? 'turns' : ''), tag: () => '',
    warn: (it) => (it.id === 'tag' ? 'too small' : ''), hidden: (it) => it.id === 'glass', move: (id, d) => ({ ok: !(id === 'arm' && d < 0), reason: 'R2: the arm stays under the glass' }),
    onMove: (id, d) => moved.push(id + d), pick: (it) => picked.push(it.id), eyeTip: (it) => 'eye ' + it.id, toggleEye: (it) => eyes.push(it.id),
    dragging: 'tag', drag: { start: () => {}, over: () => {}, drop: () => {}, end: () => {} } });
  ok(rows.filter((x) => x.isHead).map((x) => x.label).join() === 'On top,Moving,Plate' && rows.length === 8, 'a head per band', rows.filter((x) => x.isHead).map((x) => x.label).join());
  const R = (id) => rows.find((x) => x.id === id);
  ok(R('arm').indent === 0 && R('jet').indent === 14 && R('tag').indent === 28 && R('tag').pad === 38, 'a row indents by its depth under its parent');
  ok(R('jet').cls === 'on child' && R('jet').bg === '#1f3a5f' && R('tag').cls === ' child dragging', 'the selection and the dragged row are marked');
  ok(R('glass').op === 0.45 && R('glass').eye === 'hidden' && R('glass').eyeIcon === '○' && R('arm').eye === 'shown', 'a hidden layer is dimmed and its eye closed');
  ok(R('arm').upOp === 0.35 && R('arm').upTip === 'R2: the arm stays under the glass' && R('arm').downTip === 'Down one' && R('glass').arrows === 'hidden', 'arrows say why they cannot move');
  ok(R('tag').warn === ' ⚠' && R('arm').title === 'Arm · turns · Drag onto another layer, or use the arrows' && R('glass').title === 'Glass · R2: the arm stays under the glass', 'warnings flag the row; the explanation is the tooltip', R('glass').title);
  const ev = { stopPropagation() { }, preventDefault() { } };
  R('arm').down(ev); R('jet').pick(); R('glass').eyeToggle(ev);
  ok(moved.join() === 'arm1' && picked.join() === 'jet' && eyes.join() === 'glass', 'the arrows, the pick and the eye call the app');
  const gap = S.layerGap({ ok: false, indent: 14, label: '⊘ R1', drop: () => picked.push('gap') });
  ok(gap.isGap && gap.cls === 'no' && (gap.drop(ev), picked.includes('gap')), 'a refused drop gap');
  const L = render(S.Layers, { title: 'Layers', cap: { text: '3 / 32', color: '#8b98a5', tip: 'live rows' }, rows: rows.concat([gap]), warnings: [{ text: 'R8: no' }], add: { label: '+ Add', on: () => {} }, children: { type: 'button', props: { className: 'mine' }, kids: ['Switch'] } });
  ok(by(L, 'div', 'st-lr').length === 5 && by(L, 'div', 'st-band').length === 3 && by(L, 'div', 'st-gap').length === 1 && by(L, 'div', 'st-warnrow').length === 1, 'the panel: rows, bands, the gap, the warnings');
  ok(by(L, 'button', 'st-add').length === 1 && by(by(L, 'div', 'st-foot')[0], 'button', 'mine').length === 1 && /3 \/ 32/.test(text(L)), 'the add button, the app’s footer and the cap');
  ok(by(by(L, 'div', 'st-lr')[1], 'button').length === 3, 'each row: up, down and the eye');
}

{
  // ---- the host's words (orb-os-net design §6.3): the same shell, the site's sentences.
  // theme@1's session.js is what answers which host this is, so load it here.
  global.location = global.location || { origin: 'http://site', search: '', pathname: '/studio/tint/' };
  global.addEventListener = global.addEventListener || function () {};
  global.removeEventListener = global.removeEventListener || function () {};
  global.localStorage = global.localStorage || { getItem: () => null, setItem: () => {}, removeItem: () => {} };
  require(path.join(__dirname, '..', 'theme@1', 'session.js'));
  const WEB = { kind: 'web', api: '/studio-api', send: false, publish: true, inbox: true };
  // the start flow's empty-library line, host-worded the way host@1's host.js now passes it
  const EMB = 'Nothing in the library yet. Save to library on the Render step keeps a design on the hub as a draft; Send to orb bakes it into a theme under the same name.';
  const EWB = 'Nothing in the library yet. Save to library on the Render step keeps a design in your library as a draft; Download .orbtheme bakes it into a theme under the same name.';
  const shell = (host) => {
    global.GorbHost = host;
    const on = fn;
    const rp = render(S.RenderPanel, { send: { note: '', tip: 't', on: on }, save: { note: 'keep it', tip: 'st', on: fn }, downloads: [] });
    const big = by(rp, 'button', 'st-big');
    const rows = S.libraryRows([{ name: 'Night', state: 'changed', at: 0 }, { name: 'Day', state: 'draft', at: 0, theme: { stale: 'old rows' } },
      { name: 'Old', state: 'theme', at: 0, theme: {} }], { version: '1.2.0', armed: null, arm: fn, open: fn, forget: fn, meta: () => '' });
    const sheet = render(S.StartFlow, { pick: 'library', library: { rows: [], empty: (OrbTheme.hostText ? OrbTheme.hostText(EMB, EWB) : EMB) } });
    const out = {
      send: text(big[1]),
      save: text(big[0]),
      sendHint: S.sendProps({ canCancel: true, cancelling: false }, 'x', on).hint,
      modalTitle: text(by(render(S.SendModal, S.sendProps({ canCancel: true, cancelling: false }, 'x', on)), 'div', 'st-mtitle')[0]),
      openHint: text(by(render(S.OpeningModal, { line: 'Opening…' }), 'div', 'st-hint')[0]),
      empty: text(by(sheet, 'div', 'st-lib-empty')[0]),
      tips: rows.map((r) => r.tagTip).join(' | '),
      delTip: rows[0].delTip,
    };
    delete global.GorbHost;
    return out;
  };
  const none = shell(undefined), hubbed = shell({ kind: 'hub' }), webbed = shell(WEB);
  const HUB = {
    send: 'Send to orb', save: 'Save to librarykeep it',
    sendHint: 'Nothing has reached the orb yet, so Cancel stops it cleanly.',
    modalTitle: 'Sending to the orb',
    openHint: 'A theme’s design comes with its art, and from an orb’s card that can take a while. The editor opens on the theme when it arrives.',
    empty: 'Nothing in the library yet. Save to library on the Render step keeps a design on the hub as a draft; Send to orb bakes it into a theme under the same name.',
    tips: 'A baked theme with changes saved beside it: Open shows the changes, Send to orb bakes them in'
      + ' | An orb on the current firmware would refuse this bake (old rows). Rebake all on the Themes tab rebakes it from its design.'
      + ' | Baked: on the hub shelf, ready to install',
    delTip: 'Take it out of the hub’s library, baked theme and all (asks twice). No orb is touched.',
  };
  ok(JSON.stringify(none) === JSON.stringify(hubbed), 'no GorbHost and kind "hub" draw the shell with the same words');
  ok(JSON.stringify(hubbed) === JSON.stringify(HUB), 'and they are today’s strings, byte for byte', JSON.stringify(hubbed));
  ok(!/hub|Send to orb|Themes tab/i.test(JSON.stringify(webbed)), 'under a web host no word of the shell names the hub, a send or the Themes tab',
    JSON.stringify(webbed).slice(0, 400));
  ok(webbed.send === 'Download .orbtheme' && webbed.modalTitle === 'Baking the theme'
    && webbed.sendHint === 'The .orbtheme downloads when the bake is done.'
    && /Download \.orbtheme bakes them in/.test(webbed.tips) && /in your library, ready to download/.test(webbed.tips),
    'the web shell says download, bake and your library', JSON.stringify(webbed).slice(0, 200));
  const keepInfo = OrbTheme.hostInfo, keepText = OrbTheme.hostText;
  delete OrbTheme.hostText; delete OrbTheme.hostInfo;      // an editor page with no theme@1
  const noTheme = shell(WEB);
  OrbTheme.hostInfo = keepInfo; OrbTheme.hostText = keepText;
  ok(JSON.stringify(noTheme) === JSON.stringify(HUB), 'with theme@1 not loaded the shell keeps the hub’s words rather than break', JSON.stringify(noTheme).slice(0, 200));
}

// ---- S3b: the SVG rules, the hub's own checkSvgArt read the same files (sitelimits.go)
{
  const holds = (w) => 'an SVG in it holds ' + w + ", which a design's SVGs can't (no scripts, external links or entities)";
  const HANDLER = holds('an event handler');
  const LINK = holds('an outside link');
  const NOTSVG = "an image in it says it is an SVG but isn't one";
  const size = (kb) => 'an SVG in it is ' + kb + " KB, and a design's SVGs can be at most 256 KB";
  const vectors = [
    ['plain rect', '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><rect width="10" height="10"/></svg>', null],
    ['fragment href', '<svg xmlns="http://www.w3.org/2000/svg"><defs><linearGradient id="g"/></defs><rect fill="url(#g)" width="4" height="4"/><use href="#g"/></svg>', null],
    ['embedded raster', '<svg xmlns="http://www.w3.org/2000/svg"><image href="data:image/png;base64,iVBORw0KGgo=" width="4" height="4"/></svg>', null],
    ['xlink fragment', '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"><use xlink:href="#g"/></svg>', null],
    ['handler', '<svg xmlns="http://www.w3.org/2000/svg" onload="fetch(\'//x\')">', HANDLER],
    ['handler no space', '<svg xmlns="http://www.w3.org/2000/svg"><rect onclick=alert(1)/></svg>', HANDLER],
    ['outside href', '<svg xmlns="http://www.w3.org/2000/svg"><image href="https://example.com/a.png"/></svg>', LINK],
    ['outside single-quoted href', '<svg xmlns="http://www.w3.org/2000/svg"><use xlink:href=\'//example.com/x.svg\'/></svg>', LINK],
    ['outside url()', '<svg xmlns="http://www.w3.org/2000/svg"><rect fill="url(https://example.com/a.png)" width="2" height="2"/></svg>', LINK],
    ['data url of another kind', '<svg xmlns="http://www.w3.org/2000/svg"><rect fill="url(data:text/html;base64,PHN2Zz4=)" width="2" height="2"/></svg>', LINK],
    ['not an svg', 'hello, not markup at all, and long enough to read', NOTSVG],
    // ruling 35's shared table (the site's svg-art tests and the hub's TestSvgArtSharedVectors use it too)
    ['decl, comment, fragment links', '<?xml version="1.0"?><!-- c --><svg xmlns="http://www.w3.org/2000/svg"><use href="#a"/><rect id="a" fill="url(#g)"/></svg>', null],
    ['script', '<svg><script>alert(1)</script></svg>', holds('a script')],
    ['event handler', '<svg onload="x()"></svg>', HANDLER],
    ['doctype', '<?xml version="1.0"?><!DOCTYPE svg [<!ENTITY a "aa">]><svg/>', holds('a DOCTYPE')],
    ['outside image', '<svg><image href="https://evil.example/x.png"/></svg>', LINK],
    ['outside url in style', '<svg><rect style="fill:url(http://evil.example/p)"/></svg>', LINK],
    ['javascript url', '<svg><a xlink:href="javascript:x()"/></svg>', holds('a javascript: URL')],
    ['foreignObject', '<svg><foreignObject/></svg>', holds('foreignObject')],
    ['html wrapper', '<html><svg/></html>', NOTSVG],
    ['prefixed script', '<svg xmlns:x="http://www.w3.org/2000/svg"><x:script>1</x:script></svg>', holds('a script')],
    ['prefixed iframe', '<svg><h:iframe xmlns:h="http://www.w3.org/1999/xhtml"/></svg>', holds('an iframe')],
    ['nul', '<svg>\u0000</svg>', "an SVG in it can't be read as text"],
    ['svg not first', '<rect/><svg/>', NOTSVG],
    ['@import', '<svg><style>@import "x.css";</style></svg>', holds('an @import')],
  ];
  for (const [name, text, want] of vectors) ok(S.checkSvg(text) === want, 'checkSvg: ' + name, String(S.checkSvg(text)));
  const open = '<svg xmlns="http://www.w3.org/2000/svg">', close = '</svg>';
  const pad = (n) => { let t = open; while (t.length < n - close.length) t += ' '; return t + close; };
  ok(S.SVG_MAX_BYTES === 262144, 'checkSvg: the ceiling is 256 KB');
  ok(S.checkSvg(pad(262144)) === null, 'checkSvg: exactly 256 KB is kept');
  ok(S.checkSvg(pad(262145)) === size(257), 'checkSvg: one byte over is refused as 257 KB', String(S.checkSvg(pad(262145))));
  ok(S.checkSvg(pad(3 * 1024 * 1024)) === size(3072), 'checkSvg: a 3 MB SVG is refused by its weight');
  const heavy = open + '<rect onload="x"/>' + ' '.repeat(262144) + close;
  ok(heavy.length === 262208 && S.checkSvg(heavy) === size(257),
    'checkSvg: weight is asked before handlers', String(S.checkSvg(heavy)) + ' at ' + heavy.length + ' B');
  ok(S.checkSvg(null) === NOTSVG && S.checkSvg('') === NOTSVG, 'checkSvg: nothing to look at is not an SVG');
}

console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
