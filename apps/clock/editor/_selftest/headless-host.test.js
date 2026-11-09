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

  // the file: the clock's own shape, and back
  const doc = await app.toDoc(pushed[0], false);
  ok(doc && doc.dial === 'modern' && Array.isArray(doc.design.comps), 'the file is the clock’s designContent');
  const back = app.fromDoc(doc);
  ok(back && !app.check(back) && JSON.stringify(back.design.comps) === JSON.stringify(ctl.design().comps), 'which reads back as the same watch');
  ok(app.fromDoc({ dial: 'modern' }) === null, 'a file with no design reads as nothing');

  const f = app.fit();
  ok(f.rows > 0 && f.cap >= f.rows && f.pace > 0, 'the fit: the rows against the cap, and the pace', JSON.stringify(f));

  ctl.stopTimer && ctl.stopTimer();
  console.log((fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'RESULT: PASS — ' + checked + ' checks'));
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw ' + (e && e.stack || e)); process.exit(1); });
