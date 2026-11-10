#!/usr/bin/env node
'use strict';
// D43 P7 (§8): the design in the theme. The editor writes the whole watch as a design
// document, the content serial folds it (a design-only change is a new theme to the card),
// and the hub's Edit door (?theme=) reopens it, refusing another app's design or a newer
// schema by name.
require('./headless-stub.js');
const E = global.WatchEngine;
global.React = { createRef: () => ({ current: null }) };
require('../clock-logic.js');
const G = require(require('./runtime-paths.js').BAKER);
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };

const DCLogic = function () { };
DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
DCLogic.prototype.forceUpdate = function () { };
const mk = () => {
  const it = new (global.ClockLogic(DCLogic))({});
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
  // a name typed in another session (autosaved) never rides onto a new source
  const nameless = Object.assign({}, back); delete nameless.themeName;
  const st = mk(); st.setState({ themeName: 'Bench Send' });
  ok(st.applyDesignDoc(nameless, 'the card') === null && st.state.themeName === '' && st.themeName() === E.PRESETS.modern.name,
    'a reopened design without a name shows its dial’s name, not the one left in the field', JSON.stringify(st.state.themeName));
  const pk = mk(); pk.setState({ themeName: 'Bench Send' }); pk.renderVals().dials.find((x) => /Luna|Skeleton/.test(x.name)).pick();
  ok(pk.state.themeName === '' && pk.themeName() === E.PRESETS[pk.state.dial].name, 'picking a preset shows the preset’s name', JSON.stringify([pk.state.dial, pk.state.themeName]));
  const fs = mk(); fs.setState({ themeName: 'Bench Send' }); fs.fromScratch();
  ok(fs.state.themeName === '', 'and so does starting from scratch', JSON.stringify(fs.state.themeName));
  // a design started from scratch is saved, sent and reopened as its own watch (it had borrowed Skeleton's slot)
  fs.addComp('hand');
  const sdoc = JSON.parse(JSON.stringify(await fs.designDoc(false))), rs = mk(); rs.state.dial = 'skeleton';
  ok(sdoc.dial === 'scratch' && rs.applyDesignDoc(sdoc, 'the card') === null && rs.state.dial === 'scratch' && rs.design().noDial === true && rs.design().comps.length === 1 && rs.themeName() === 'Untitled',
    'a scratch design reopens as Untitled, its own watch, with no dial and its one hand', JSON.stringify([sdoc.dial, rs.state.dial, rs.themeName()]));
  ok(/an?other|not clock/.test(mk().applyDesignDoc(Object.assign({}, back, { app: 'hsb' })) || '') || /hsb app/.test(mk().applyDesignDoc(Object.assign({}, back, { app: 'hsb' })) || ''), 'another app’s design is refused by name');
  const newer = mk().applyDesignDoc(Object.assign({}, back, { schema: 2, appVersion: 'v1.3.0' }));
  ok(/clock v1\.3\.0/.test(newer || '') && /reads schema 1/.test(newer || '') && /update the app/.test(newer || ''), 'a newer schema names the version that made it and the fix', newer);
  ok(/not one this editor wrote/.test(mk().applyDesignDoc({ format: 'wckit' }) || ''), 'a foreign document is refused');
  // option b: an older schema walks forward through one pure step per bump (here a pretend schema 2 that renamed themeName)
  const m2 = mk(); m2.DESIGN_SCHEMA = 2; m2.DESIGN_MIGRATE = { 1: (d) => Object.assign(d, { title: d.themeName, themeName: undefined }) };
  const mig = m2.migrateDesign(back);
  ok(mig.doc && mig.doc.schema === 2 && mig.doc.title === 'Field test' && back.schema === 1 && back.themeName === 'Field test', 'an older design is migrated one step per bump, leaving the original untouched', JSON.stringify(mig.doc && [mig.doc.schema, mig.doc.title]));
  const m3 = mk(); m3.DESIGN_SCHEMA = 3; m3.DESIGN_MIGRATE = { 1: (d) => d };
  ok(/no step from schema 2 to 3/.test(m3.applyDesignDoc(back) || ''), 'and a missing step is refused by name, never guessed');
  ok(Object.keys(mk().DESIGN_MIGRATE).length === 0, 'schema 1 needs no step');
  const later = mk().applyDesignDoc(Object.assign({}, back, { appVersion: 'v1.10.0' }));
  ok(later === null, 'a same-schema design from a newer clock opens: only the schema says what this editor reads (ruling 21)', later);
  ok(mk().applyDesignDoc(Object.assign({}, back, { appVersion: 'v1.2.1' })) === null, 'while one from an older clock opens');
  window.__ORB_APP = { slug: 'clock', version: 'v1.2.2+6a5c661' };
  const hashed = mk().applyDesignDoc(Object.assign({}, back, { appVersion: 'v1.3.0+3f9a1c2' }));
  ok(hashed === null && mk().applyDesignDoc(Object.assign({}, back, { appVersion: 'v1.2.2+0000000' })) === null,
    'a stamp carrying a build hash opens the same way: a newer release of this schema is nothing to drop', hashed);
  window.__ORB_APP = { slug: 'clock', version: 'v1.2.2' };
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
  ok(!('designShipOpts' in rv) && !('exportDesign' in rv), 'the design always rides, with its art: there is nothing to choose, and no design file to download (K10)');

  // the Edit door is the host's (theme@1/session.js openTheme, in the runtime's session suite)

  console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})();
