#!/usr/bin/env node
'use strict';
// D43 P7 (§8): the design in the theme. The editor writes the whole watch as a design
// document, the content serial folds it (a design-only change is a new theme to the card),
// and the hub's Edit door (?theme=) reopens it, refusing another app's design or a newer
// schema by name.
require('./headless-stub.js');
const E = global.WatchEngine;
global.React = { createRef: () => ({ current: null }) };
require('./component.js');
const G = require(require('./runtime-paths.js').BAKER);
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };

const DCLogic = function () { };
DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
DCLogic.prototype.forceUpdate = function () { };
const mk = () => {
  const it = new (global.COMP_CLASS(DCLogic))({});
  it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
  it.state.dial = 'modern'; it.state.playing = false; it.state.hidden = {};
  it.buildMovement = () => {};
  return it;
};

(async () => {
  window.__ORB_APP = { slug: 'clock', version: 'v1.2.2' };
  const a = mk();
  const sub = a.design().comps.find((c) => c.type === 'subdial').id;
  a.setComp(sub, { size: 47 }); a.setState({ caseStyle: 'none', caseOpen: 210, themeName: 'Field test', train: 'orbital', glassStyle: 'flat' });
  const doc = await a.designDoc(true);
  ok(doc.format === 'wc-design' && doc.schema === 1 && doc.app === 'clock' && doc.appVersion === 'v1.2.2' && doc.art === true,
    'the document names its format, schema, app and the app’s version', JSON.stringify([doc.format, doc.schema, doc.app, doc.appVersion]));
  const bare = await a.designDoc(false);
  ok(bare.art === false && bare.assets.length === 0 && Object.keys(bare.sounds.up || {}).length === 0, 'without art it carries no asset or uploaded-sound bytes');
  // a JSON round trip, as the bytes in the theme are
  const back = JSON.parse(new TextDecoder().decode(new TextEncoder().encode(JSON.stringify(doc))));
  const b = mk(); b.state.dial = 'skeleton';
  ok(b.applyDesignDoc(back, 'the card') === null, 'the editor reopens its own design');
  ok(b.state.dial === 'modern' && b.design().comps.find((c) => c.id === sub).size === 47 && b.state.caseStyle === 'none' && b.state.caseOpen === 210
    && b.state.themeName === 'Field test' && b.state.train === 'orbital' && b.state.glassStyle === 'flat',
    'the whole watch comes back: parts, case, opening, movement, glass and name', JSON.stringify([b.state.dial, b.state.caseStyle, b.state.train]));
  ok(/Reopened “Field test” from the card/.test(b.state.status) && /Send replaces it/.test(b.state.status), 'and says where it came from and what Send does', b.state.status);
  ok(/an?other|not clock/.test(mk().applyDesignDoc(Object.assign({}, back, { app: 'hsb' })) || '') || /hsb app/.test(mk().applyDesignDoc(Object.assign({}, back, { app: 'hsb' })) || ''), 'another app’s design is refused by name');
  const newer = mk().applyDesignDoc(Object.assign({}, back, { schema: 2, appVersion: 'v1.3.0' }));
  ok(/clock v1\.3\.0/.test(newer || '') && /reads schema 1/.test(newer || '') && /update the app/.test(newer || ''), 'a newer schema names the version that made it and the fix', newer);
  ok(/not one this editor wrote/.test(mk().applyDesignDoc({ format: 'wckit' }) || ''), 'a foreign document is refused');
  const later = mk().applyDesignDoc(Object.assign({}, back, { appVersion: 'v1.10.0' }));
  ok(/clock v1\.10\.0, newer than this editor \(v1\.2\.2\)/.test(later || ''), 'a design from a newer clock is refused by version, so no field it added is dropped on save', later);
  ok(mk().applyDesignDoc(Object.assign({}, back, { appVersion: 'v1.2.1' })) === null, 'while one from an older clock opens');
  window.__ORB_APP = { slug: 'clock', version: 'dev' };
  ok(mk().applyDesignDoc(Object.assign({}, back, { appVersion: 'v9.0.0' })) === null, 'an unpackaged (dev) editor opens anything');
  window.__ORB_APP = { slug: 'clock', version: 'v1.2.2' };
  ok(/no watch/.test(mk().applyDesignDoc(Object.assign({}, back, { dial: 'nope' })) || ''), 'a design naming no preset this editor has is refused');

  // the serial folds the design: same dial, different design, a different theme to the card
  const opts = { plate: { rgba: new Uint8Array(16), x: 0, y: 0, w: 2, h: 2 }, scalars: {} }, pol = { carrier: 'still' };
  const s0 = a.serialContent3(G, 'X', 1, 1, pol, opts);
  const s1 = a.serialContent3(G, 'X', 1, 1, pol, Object.assign({}, opts, { design: new TextEncoder().encode('{"a":1}') }));
  const s2 = a.serialContent3(G, 'X', 1, 1, pol, Object.assign({}, opts, { design: new TextEncoder().encode('{"a":2}') }));
  const s2b = a.serialContent3(G, 'X', 1, 1, pol, Object.assign({}, opts, { design: new TextEncoder().encode('{"a":2}') }));
  ok(s0 !== s1 && s1 !== s2 && s2 === s2b, 'the content serial moves with the design and only with it', [s0, s1, s2].join(','));

  // the Render step: no choice to make, every theme carries its whole design
  const c = mk(); const rv = c.renderVals();
  ok(!('designShipOpts' in rv) && typeof rv.exportDesign === 'function', 'the design always rides, with its art: there is nothing to choose');

  // the Edit door: ?theme= through the hub, card by default, the shelf on request
  const seen = [];
  global.fetch = async (u) => { seen.push(u); return { ok: true, arrayBuffer: async () => new TextEncoder().encode(JSON.stringify(back)).buffer }; };
  global.location = { search: '?orb=192.168.2.34&app=clock&theme=a1b2c3d4e5f6', protocol: 'http:' };
  const d = mk(); d.state.dial = 'skeleton';
  ok(await d.openThemeParam() && d.state.dial === 'modern' && seen[0] === '/api/theme/design?ip=192.168.2.34&id=a1b2c3d4e5f6', 'Edit from the card asks the hub for that orb’s theme', seen[0]);
  ok(d._snap === null && !d._undo.length, 'and the reopened theme starts the session’s history');
  global.location = { search: '?theme=Night&src=shelf', protocol: 'http:' };
  await mk().openThemeParam();
  ok(seen[1] === '/api/theme/design?name=Night', 'Edit from the shelf names the shelf file', seen[1]);
  global.fetch = async () => ({ ok: false, status: 404, json: async () => ({ error: 'this theme carries no design, so it cannot be reopened' }) });
  global.location = { search: '?orb=1.2.3.4&theme=abc', protocol: 'http:' };
  const e = mk();
  ok(!(await e.openThemeParam()) && /carries no design/.test(e.state.status), 'a theme without a design says so in the hub’s words', e.state.status);
  global.location = { search: '?theme=../etc', protocol: 'http:' };
  ok(!(await mk().openThemeParam()), 'a theme id that is not one is not fetched');
  global.location = { search: '', protocol: 'http:' };
  ok(!(await mk().openThemeParam()), 'and with no ?theme= nothing is asked');

  console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})();
