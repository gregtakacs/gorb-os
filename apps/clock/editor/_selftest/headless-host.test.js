#!/usr/bin/env node
'use strict';
// E7 Stage 2 (theme-editor-kit-plan §8.12): clock-host.js, the clock's part of Orb Manager's
// hosted editor. It registers the stateful contract, its templates and blank pass its own
// check, a design handed over is not handed back (and a template's own field order is the
// same watch), an edit the class makes itself is pushed to the host once, and a file the
// clock writes reads back as the same watch.
require('./headless-stub.js');
const E = global.WatchEngine;
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };

// React, as far as a pane draws it here: elements are records, hooks run at once
global.React = window.React = {
  createElement: (type, props, ...kids) => ({ type, props: props || {}, kids }),
  createRef: () => ({ current: null }),
  useState: (v) => [v, () => {}],
  useEffect: (f) => { f(); },
  Fragment: 'frag',
};
require(require('./runtime-paths.js').STUDIO);   // the shell's parts (fieldKit, Layers) the panes hand the class's values to
let app = null;
window.OrbHost = { register: (a) => { app = a; } };
window.__keys = []; window.addEventListener = (t, f) => { if (t === 'keydown') window.__keys.push(f); };
require('../clock-logic.js');
require('../clock-host.js');

(async () => {
  ok(!!app && app.slug === 'clock', 'clock-host.js registers the clock with the host');
  ok(['adopt', 'toDoc', 'fromDoc', 'bake', 'fit', 'picture'].every((k) => typeof app[k] === 'function'), 'the stateful contract: adopt, toDoc, fromDoc, bake, fit, picture');
  ok(['left', 'right', 'stage', 'tool', 'overlay'].every((k) => typeof app.panes[k] === 'function'), 'and all five panes');
  ok(JSON.stringify(app.steps.map((s) => s[0])) === '["movement","dial","parts","finish"]', 'the composer’s four steps, between the host’s Start and Render');
  ok(!app.play, 'no host play: the clock paces itself (fit)');
  const T = app.templates;
  ok(T.length === Object.keys(E.PRESETS).length && T.some((t) => t.key === app.start), 'a template per preset, the start among them', T.map((t) => t.key).join(','));
  const bad = T.filter((t) => app.check(t.make()));
  ok(!bad.length, 'every template passes the clock’s check', bad.map((t) => t.key).join(','));
  const blank = app.blank.make();
  ok(!app.check(blank) && blank.scratch && blank.dial === 'scratch', 'From scratch is the blank watch');
  ok(app.check({ design: {} }) && app.check(null), 'a file with no watch is refused');

  // the host's c: what a pane is handed
  const pushed = [];
  const c = { d: null, step: 'movement', goStep() {}, size: 466, push: (d) => pushed.push(d), say() {}, ui: {}, setUi() {} };
  const draw = () => { const el = app.panes.left(c); el.type(el.props); };
  draw();
  const ctl = window.ClockHost.ctl();
  ok(!!ctl, 'the first pane makes the controller');
  await ctl.__mount;
  ok(ctl.ready, 'and it mounts');

  const mod = window.ClockHost.templateSnap('modern');
  app.adopt(mod);
  ok(ctl.state.dial === 'modern', 'a design handed over is the class’s watch');
  draw();
  ok(pushed.length === 0, 'and is not handed back', String(pushed.length));
  // the same watch, its fields in another order (the host's stamps, a template's own build)
  const re = {}; Object.keys(mod).reverse().forEach((k) => { re[k] = mod[k]; });
  let sets = 0; const ss = ctl.setState; ctl.setState = function (p, cb) { sets++; return ss.call(this, p, cb); };
  app.adopt(Object.assign(re, { base: 'modern' }));
  ctl.setState = ss;
  ok(sets === 0, 'the same watch in another field order is no change', String(sets));

  // an edit the class makes itself: pushed once, then not again
  const sd = ctl.design().comps.find((x) => x.type === 'subdial');
  ctl.setComp(sd.id, { size: (sd.size || 40) + 3 });
  draw();
  ok(pushed.length === 1 && pushed[0].design.comps.find((x) => x.id === sd.id).size === (sd.size || 40) + 3, 'an edit in the class is pushed to the host', String(pushed.length));
  draw();
  ok(pushed.length === 1, 'once', String(pushed.length));
  sets = 0; ctl.setState = function (p, cb) { sets++; return ss.call(this, p, cb); };
  app.adopt(pushed[0]);
  ctl.setState = ss;
  ok(sets === 0, 'and the host’s copy coming back is no change', String(sets));

  // the host's name is the theme's: a rename in its top bar is what the bake and the file carry,
  // with no movement rebuilt and nothing handed back (Gold, 2026-11-08: a renamed design baked
  // as "Skeleton", and Send offered to replace the orb's own Skeleton with it)
  let builds = 0; const bm = ctl.buildMovement; ctl.buildMovement = function () { builds++; return bm.apply(this, arguments); };
  app.adopt(Object.assign({}, pushed[0], { name: 'E7 bench' }));
  ctl.buildMovement = bm;
  draw();
  ok(ctl.themeName() === 'E7 bench', 'a rename in the host is the class\u2019s theme name', ctl.themeName());
  ok(builds === 0 && pushed.length === 1, 'and rebuilds nothing, pushes nothing', builds + ' builds, ' + pushed.length + ' pushes');
  ok((await app.toDoc(pushed[0], false)).themeName === 'E7 bench', 'the file carries the new name');
  app.adopt(Object.assign({}, pushed[0], { name: 'Modern' }));
  ok(ctl.themeName() === 'Modern', 'and Undo past the rename takes the old name back');

  // the file: the clock's own shape, and back
  const doc = await app.toDoc(pushed[0], false);
  ok(doc && doc.dial === 'modern' && Array.isArray(doc.design.comps), 'the file is the clock’s designContent');
  const back = app.fromDoc(doc);
  ok(back && !app.check(back) && JSON.stringify(back.design.comps) === JSON.stringify(ctl.design().comps), 'which reads back as the same watch');
  ok(app.fromDoc({ dial: 'modern' }) === null, 'a file with no design reads as nothing');

  // a design file's sound upload survives the open (Gold, 2026-11-08: a library draft with its
  // own tick came back with the tick named and its PCM gone), and Undo back to it
  const sd0 = ctl.sndDesign();
  const withUp = Object.assign({}, doc, { themeName: 'Ticker', sounds: Object.assign({}, sd0, { tick: 'upload', up: { tick_loop: { name: 'tick.wav', pcm: [1, 2, 3], loop: true, rate: 22050, first: 0 } } }) });
  const opened = app.fromDoc(withUp);
  ok(!JSON.stringify(opened).includes('"pcm"'), 'the snapshot carries no PCM');
  app.adopt(opened);
  const upNow = () => ((ctl.sndDesign().up || {}).tick_loop || {}).name;
  ok(upNow() === 'tick.wav', 'an opened design\u2019s sound upload is the class\u2019s', String(upNow()));
  ok(ctl.sndDesign().up.tick_loop.pcm instanceof Int16Array && ctl.sndDesign().up.tick_loop.pcm[2] === 3, 'its samples are samples again, not the object JSON made of them');
  // the form a real file holds: a typed array through JSON is an object of indices
  const asSaved = JSON.parse(JSON.stringify(Object.assign({}, withUp, { sounds: Object.assign({}, withUp.sounds, { up: { tick_loop: { name: 'tick.wav', pcm: new Int16Array([-2984, 6262, 15342]), loop: false } } }) })));
  ok(!Array.isArray(asSaved.sounds.up.tick_loop.pcm), 'a saved file holds the samples as an object');
  app.adopt(app.fromDoc(asSaved));
  const revived = ctl.sndDesign().up.tick_loop.pcm;
  ok(revived instanceof Int16Array && revived.length === 3 && revived[0] === -2984 && revived[2] === 15342, 'and they open as the samples they were', String(revived && Array.from(revived)));
  app.adopt(opened);
  const inner = ctl.sndDesign();
  ok(!('upRef' in inner) && !('upSig' in inner) && !('upRef' in (await ctl.designContent(true)).sounds), 'the class holds neither the ref nor the fingerprint, so the design a bake embeds carries neither', Object.keys(inner).join());
  const doc2 = await app.toDoc(opened, true);
  ok(doc2.sounds.up.tick_loop && !('upRef' in doc2.sounds), 'and is in the file it writes, with no ref of the host\u2019s', JSON.stringify(Object.keys(doc2.sounds)));
  app.adopt(window.ClockHost.templateSnap('modern'));
  app.adopt(opened);
  ok(upNow() === 'tick.wav', 'Undo back to the opened design brings its upload back', String(upNow()));

  // a new upload is a change the host sees, even with the voice already "upload" (Gold: the live
  // size stayed put after a second tick.wav)
  draw(); const before = pushed.length;
  ctl.sndUpd((x) => { x.tick = 'upload'; x.up.tick_loop = { name: 'other.wav', pcm: [1, 2, 3, 4], loop: true, rate: 22050, first: 0 }; });
  draw();
  ok(pushed.length === before + 1 && /other\.wav/.test(pushed[pushed.length - 1].sounds.upSig || ''), 'a new sound upload is pushed to the host', (pushed.length - before) + ' pushes');
  ok(!JSON.stringify(pushed[pushed.length - 1]).includes('"pcm"'), 'still without its PCM');
  ok(!('upSig' in (await app.toDoc(pushed[pushed.length - 1], true)).sounds), 'and the file carries no fingerprint');

  // a movement that stops drawing (a hidden window pauses it) is said while the bake waits, and
  // the bake gives up in those words instead of holding Export on "Baking…" (2026-11-08)
  {
    const said = [], st3 = ctl.settled3;
    ctl.settled3 = () => false;
    window.ClockHost.STALL_MS = 400; window.ClockHost.SAY_AFTER_MS = 100;
    let why = null;
    try { await app.bake(window.ClockHost.snap(), { say: (t) => said.push(t) }); } catch (e) { why = e.message; }
    ctl.settled3 = st3; window.ClockHost.STALL_MS = 15000; window.ClockHost.SAY_AFTER_MS = 2000;
    ok(said.some((t) => /Waiting for the movement to finish drawing \(\d+ of \d+ frames\)/.test(t)), 'a wait for the movement is said', said.join(' | '));
    ok(/stopped drawing at \d+ of \d+ frames .*bring it to the front/.test(why || ''), 'and a movement that stops drawing gives up in words', why);
  }
  // the bake drives the class's update itself: with no pane drawing, a movement that needs a
  // rebuild is still rebuilt (Gold 2026-11-08: a re-timed bank sat at 8 of 15 frames)
  {
    let calls = 0; const cdu = ctl.componentDidUpdate, st3 = ctl.settled3;
    ctl.componentDidUpdate = function () { calls++; };
    ctl.settled3 = () => (calls >= 5 ? st3.call(ctl) : null);
    window.ClockHost.STALL_MS = 1500;
    let why = null;
    try { await app.bake(window.ClockHost.snap(), {}); } catch (e) { why = e.message; }
    ctl.componentDidUpdate = cdu; ctl.settled3 = st3; window.ClockHost.STALL_MS = 15000;
    ok(calls >= 5 && !/stopped drawing/.test(why || ''), 'a bake drives the class\u2019s update itself, so a rebuild needs no pane to draw', calls + ' updates · ' + (why || 'baked'));
  }
  ok(window.ClockHost.ctl().REBAKE_SETTLE_MS === 30000, 'a re-timed bake waits 30 s for the rebuilt movement, not 2 minutes');

  // Escape deselects on the hosted page too (the composer bound it in a mount the host skips)
  {
    const keys = window.__keys || [];
    ctl.setState({ sel: 'dial', selKind: 'dial' });
    keys.forEach((f) => f({ key: 'Escape', target: { tagName: 'CANVAS' } }));
    ok(keys.length > 0 && ctl.state.sel == null && ctl.state.selKind == null, 'Escape on the page deselects', keys.length + ' listeners');
    keys.forEach((f) => f({ key: 'z', ctrlKey: true, target: { tagName: 'CANVAS' }, preventDefault() {} }));
    ok(ctl.state.sel == null, 'and Ctrl+Z is not the class\u2019s to take: the host owns undo');
  }
  const f = app.fit();
  ok(f.rows > 0 && f.cap >= f.rows && f.pace > 0, 'the fit: the rows against the cap, and the pace', JSON.stringify(f));

  ctl.stopTimer && ctl.stopTimer();
  console.log((fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'RESULT: PASS — ' + checked + ' checks'));
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw ' + (e && e.stack || e)); process.exit(1); });
