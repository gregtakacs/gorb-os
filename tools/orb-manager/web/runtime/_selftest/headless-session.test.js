#!/usr/bin/env node
// theme@1/session (theme-editor-kit-plan K2, K9): the OS's session, driven by a small made-up
// app ("tint": a colour and a size) rather than the clock, so nothing clock-shaped can hide in
// it. History with bursts, no autosave, per-app slugs, the app's library on the hub (K10,
// fake-hub.js: drafts, themes, unsent changes, opening at the newest design, discarding,
// forgetting, ?entry=, Rebake all), the move of a browser's old designs, the hub's word on
// the orb's release, the design envelope with its migrations and refusals, the older-design
// note, opening a design file, an .orbtheme's design and the hub's Edit door.
//
//   node headless-session.test.js
'use strict';
const path = require('path');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
setTimeout(() => { console.log('FAIL timed out (' + checked + ' checks run)'); process.exit(1); }, 20000);

// the window: a browser store that can be made to refuse, an address, and the hub
const store = new Map(); let refuse = false;
global.window = globalThis;
global.localStorage = {
  getItem: (k) => (store.has(k) ? store.get(k) : null),
  setItem: (k, v) => { if (refuse) { const e = new Error('full'); e.name = 'QuotaExceededError'; throw e; } store.set(k, String(v)); },
  removeItem: (k) => { store.delete(k); },
};
global.location = { origin: 'http://hub', search: '', pathname: '/' };
let hub = null;
const fakeHub = require(path.join(__dirname, 'fake-hub.js'));
let fh = fakeHub({ rest: (url, o) => hub(url, o) });
global.fetch = async (url, o) => fh.fetch(url, o);
const ON_HUB = '/editor/192.0.2.7/tint/';
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

require(path.join(__dirname, '..', 'theme@1', 'session.js'));

// the made-up app: two fields are its design; `sel` is view state that history ignores
const mk = (opts) => {
  const app = { state: { color: 'red', size: 10, name: '', sel: null, status: '' }, applied: [], downloads: [], schema: 1, steps: {} };
  app.set = (p) => Object.assign(app.state, p);
  app.ses = OrbTheme.session(Object.assign({
    slug: 'tint',
    keys: () => ['color', 'size', 'name'],
    state: () => app.state, set: app.set,
    ready: () => true,
    say: (t) => { app.state.status = t; },
    restore: (snap, text) => app.set(Object.assign({}, snap, { status: text })),
    burstMs: () => -1,
    schema: () => app.schema, migrate: () => app.steps,
    content: async (withArt) => ({ color: app.state.color, size: app.state.size, name: app.state.name, art: withArt, assets: withArt ? [{ id: 'a1' }] : [] }),
    apply: (o, from) => { if (!o.color) return 'that design has no tint in it'; app.applied.push(from); app.set({ color: o.color, size: o.size, name: o.name || '' }); return null; },
    download: (blob, name) => app.downloads.push({ blob, name }),
  }, opts || {}));
  return app;
};
const edit = (app, p) => { app.set(p); app.ses.track(); };

(async () => {
  // ---- history
  {
    const a = mk();
    a.ses.track();
    edit(a, { color: 'blue' }); edit(a, { size: 12 });
    ok(a.ses.undoStack.length === 2, 'two edits, two undo steps', a.ses.undoStack.length);
    edit(a, { sel: 'x' });
    ok(a.ses.undoStack.length === 2, 'a change outside the named keys is not a step');
    a.ses.undo();
    ok(a.state.size === 10 && a.state.color === 'blue' && a.state.status === 'Undone', 'undo puts the last step back', JSON.stringify(a.state));
    a.ses.track();
    a.ses.undo();
    ok(a.state.color === 'red' && /back to where this session started/.test(a.state.status), 'and says when it reaches the start', a.state.status);
    a.ses.track();
    a.ses.redo(); a.ses.track();
    ok(a.state.color === 'blue' && a.ses.redoStack.length === 1, 'redo walks forward');
    edit(a, { size: 30 });
    ok(!a.ses.redoStack.length, 'a new edit drops the redo branch');
    a.ses.restart(); a.ses.undo();
    ok(/Nothing to undo/.test(a.state.status), 'a restart empties the history', a.state.status);
    // a burst: edits inside the window are one step
    const b = mk({ burstMs: () => 60000 });
    b.ses.track(); edit(b, { size: 1 }); edit(b, { size: 2 }); edit(b, { size: 3 });
    ok(b.ses.undoStack.length === 1, 'a burst of edits is one undo step', b.ses.undoStack.length);
    // the depth is bounded
    const c = mk({ undoMax: () => 3 });
    c.ses.track(); for (let i = 0; i < 6; i++) edit(c, { size: i });
    ok(c.ses.undoStack.length === 3, 'history keeps only the depth it is given', c.ses.undoStack.length);
  }

  // ---- no autosave (K9): edits write nothing to this browser, and boot drops the old copy
  {
    store.clear();
    store.set('wc.session.tint', JSON.stringify({ v: 1, at: 1, color: 'green' }));
    const a = mk();
    a.ses.track(); edit(a, { color: 'green', name: 'Mine' });
    await wait(800);
    ok(store.size === 1 && !('save' in a.ses) && !('load' in a.ses), 'an edit writes nothing to the browser, and there is no autosave to call');
    await a.ses.boot();
    ok(!store.has('wc.session.tint'), 'boot drops the session a browser kept before K9');
    location.search = '?app=clock';
    const other = mk();
    ok(other.ses.slug() === 'clock' && other.ses.mineKey() === 'wc.mine.clock', '?app= names the app');
    location.search = '?app=../evil';
    ok(mk().ses.slug() === 'tint', 'and a slug that is not one falls back to the host’s');
    location.search = '';
  }

  // ---- the library, on the hub (K10)
  {
    store.clear();
    const G = require(path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'));
    const bake = (name, design) => G.writeBundle3({ name, design: design ? new TextEncoder().encode(JSON.stringify(design)) : undefined,
      sections: [{ slug: 'tint', elements: [], scalars: {}, sprites: [], art: new Uint8Array(16) }] }).bytes;
    const off = mk();
    ok(!(await off.ses.saveDraft({ name: 'One', doc: { format: 'wc-design' } })) && /Not saved: the library is kept on the hub/.test(off.state.status), 'off the hub a save says where the library lives', off.state.status);
    ok(!(await off.ses.boot()) && !off.ses.library().length, 'and boot reads nothing');
    location.pathname = ON_HUB;
    const doc = (color, ver) => ({ format: 'wc-design', schema: 1, app: 'tint', appVersion: ver || 'v1.3.0', color, size: 5, assets: [{ id: 'a1', dataUrl: 'data:,x' }] });
    const a = mk();
    ok(await a.ses.saveDraft({ name: 'One', doc: doc('teal'), dial: 'round' }) && /Saved “One” to the library as a draft/.test(a.state.status), 'a design is saved to the library as a draft', a.state.status);
    await a.ses.saveDraft({ name: 'Two', doc: doc('plum') });
    await a.ses.saveDraft({ name: 'One', doc: doc('gold'), dial: 'square' });
    const l = a.ses.library();
    ok(l.length === 2 && l[0].name === 'One' && l[0].state === 'draft' && l[0].dial === 'square', 'a save under a draft’s name replaces it, newest first, with the app’s listing extras', JSON.stringify(l.map((e) => [e.name, e.state, e.dial])));
    ok(fh.drafts.get('tint').get('One').assets[0].dataUrl === 'data:,x', 'the hub holds the draft with its art');
    // a theme on the shelf, and a draft saved under its name: one entry, with unsent changes
    fh.themes.get('tint').set('Reef', { design: doc('coral', 'v1.2.0+1111111'), at: 5 });
    await a.ses.saveDraft({ name: 'Reef', doc: doc('navy') });
    const reef = a.ses.libraryEntry('Reef');
    ok(reef && reef.state === 'changed' && /as unsent changes to its theme/.test(a.state.status), 'a draft under a theme’s name is that theme’s unsent changes', a.state.status);
    const b = mk(); await b.ses.boot();
    ok(b.ses.library().length === 3, 'another page of the same app sees the same library');
    b.ses.track(); edit(b, { size: 6 }); edit(b, { size: 7 });
    ok(await b.ses.openEntry('Reef') && b.state.color === 'navy' && b.applied[0] === 'the library' && !b.ses.undoStack.length && b.ses.snap === null, 'an entry opens at its newest design, its unsent changes, and its history starts there', b.state.color);
    ok(await b.ses.forgetEntry('Reef', 'draft') && b.ses.libraryEntry('Reef').state === 'theme' && /Discarded the unsent changes to “Reef”/.test(b.state.status), 'Discard changes leaves the theme as it was baked', b.state.status);
    window.__ORB_APP = { slug: 'tint', version: 'v1.3.0+6a5c661' };
    ok(await b.ses.openEntry('Reef') && b.state.color === 'coral' && /made with tint v1\.2\.0\+1111111; saving or sending it updates it to v1\.3\.0\+6a5c661/.test(b.state.status), 'a theme an older editor made opens with the update note', b.state.status);
    ok(await b.ses.openEntry('One') && /Opened “One” from the library/.test(b.state.status), 'and one this release made just says it opened', b.state.status);
    ok(!(await b.ses.openEntry('Nope')) && /no “Nope” in the tint library/.test(b.state.status), 'a name the hub does not hold says so', b.state.status);
    await a.ses.forgetEntry('Two', 'all');
    ok(!a.ses.libraryEntry('Two') && /Removed “Two” from the library/.test(a.state.status), 'Delete takes the entry out of the library');
    fh.refuse = 'the hub has no writable library directory to keep designs in';
    ok(!(await a.ses.saveDraft({ name: 'Three', doc: doc('red') })) && /Not saved: the hub has no writable library directory/.test(a.state.status), 'a refused save says so in the hub’s words', a.state.status);
    fh.refuse = null;
    // ?entry= opens that entry once the library is read
    location.search = '?entry=One';
    const e = mk(); await e.ses.boot();
    ok(e.state.color === 'gold' && e.applied[0] === 'the library', '?entry= opens the entry the Themes tab pressed Edit on', e.state.color);
    location.search = '';
    // a browser's designs from before K9 move into the library as drafts, then the key goes
    store.set('wc.mine.tint', JSON.stringify([{ name: 'Old', at: 5, doc: doc('olive'), dial: 'round' }, { name: 'Legacy id', id: 'WCD5-abc' }]));
    const m = mk(); await m.ses.boot();
    ok(m.ses.library().some((x) => x.name === 'Old' && x.state === 'draft' && x.dial === 'round') && !store.has('wc.mine.tint'), 'boot moves the browser’s old designs into the library and drops the key', JSON.stringify(m.ses.library().map((x) => x.name)));
    ok(!m.ses.library().some((x) => x.name === 'Legacy id'), 'an entry with no document is not moved');
    fh.refuse = 'disk full';
    store.set('wc.mine.tint', JSON.stringify([{ name: 'Kept', doc: doc('rust') }]));
    await mk().ses.boot();
    ok(store.has('wc.mine.tint'), 'a move the hub refuses leaves the browser’s copy for the next boot');
    fh.refuse = null; store.clear();
    // the hub's word on the orb's release
    fh.source = { source: 'orb', relation: 'newer', hub_version: 'v1.3.0+6a5c661', orb_version: 'v1.2.9+0000000' };
    const n = mk(); await n.ses.boot();
    ok(/this is the orb’s own tint v1\.2\.9\+0000000 editor; the hub has v1\.3\.0\+6a5c661 — update the app on the orb to edit with it/.test(n.ses.orbNote) && /^Heads up: /.test(n.state.status), 'an orb behind the hub is edited with its own editor, and says the hub has a newer one', n.ses.orbNote);
    fh.source = { source: 'library', relation: 'same', hub_version: 'v1.3.0+6a5c661', orb_version: 'v1.3.0+6a5c661' };
    const sm = mk(); await sm.ses.boot();
    ok(sm.ses.orbNote === '', 'and the same release says nothing');
    // Rebake all: each stale theme, from its own design (never the unsent changes beside it),
    // kept on the shelf in its place with its changes left standing; one without a design is named
    fh.themes.get('tint').set('Stale A', { design: doc('amber', 'v1.1.0'), stale: 'bundle geometry v5', at: 6 });
    fh.themes.get('tint').set('Stale B', { design: null, stale: 'bundle geometry v5', at: 7 });
    fh.themes.get('tint').set('Fine', { design: doc('lime'), at: 8 });
    await a.ses.saveDraft({ name: 'Stale A', doc: doc('unsent') });
    const baked = [];
    const r = mk({ bake: async () => { baked.push(r.state.color); return bake(r.state.name || 'Stale A', doc(r.state.color)); } });
    r.set = (p) => Object.assign(r.state, p);
    const out = await r.ses.rebakeAll();
    ok(out && out.done.join() === 'Stale A' && baked.join() === 'amber', 'Rebake all rebakes each stale theme from the design it carries, not its unsent changes', JSON.stringify([out, baked]));
    ok(out.failed.length === 1 && /Stale B \(it carries no design to rebake from\)/.test(out.failed[0]), 'and names the one it cannot', out.failed[0]);
    const sa = r.ses.libraryEntry('Stale A');
    ok(sa && sa.state === 'changed' && !sa.theme.stale && fh.drafts.get('tint').get('Stale A').color === 'unsent', 'the rebaked theme replaces the stale one and its unsent changes stand', JSON.stringify(sa && sa.theme));
    ok(/Rebaked 1 of 2: Stale A · not rebaked: Stale B/.test(r.state.status), 'and the status says what happened', r.state.status);
    ok((await r.ses.rebakeAll()).done.length === 0 && /not rebaked: Stale B/.test(r.state.status), 'a second press finds only what it cannot rebake', r.state.status);
    const nb = mk(); ok((await nb.ses.rebakeAll()) === null && /cannot rebake/.test(nb.state.status), 'an editor with no bake says it cannot rebake');
    fh.themes.get('tint').set('Stale C', { design: doc('cyan', 'v1.1.0'), stale: 'bundle geometry v5', at: 9 });
    const w = mk({ bake: async () => bake('Another name', doc('cyan')) });
    const wo = await w.ses.rebakeAll();
    ok(wo.failed.some((f) => /Stale C \(it baked as “Another name”, so the stale “Stale C” is still in the library\)/.test(f)), 'a bake that names itself differently is reported, not counted as done', JSON.stringify(wo));
    location.pathname = '/';
    delete window.__ORB_APP;
  }

  // ---- the design document
  window.__ORB_APP = { slug: 'tint', version: 'v1.3.0+6a5c661' };
  {
    const a = mk(); a.set({ color: 'teal', name: 'Reef' });
    const doc = await a.ses.doc(true);
    ok(doc.format === 'wc-design' && doc.schema === 1 && doc.app === 'tint' && doc.appVersion === 'v1.3.0+6a5c661' && doc.color === 'teal' && doc.art === true,
      'the envelope names format, schema, app and release, round the app’s content', JSON.stringify(doc).slice(0, 120));
    ok(Object.keys(doc).slice(0, 5).join() === 'format,schema,app,appVersion,made', 'the envelope comes first, so the bytes a theme carries keep their order');
    const b = mk();
    ok(b.ses.apply(JSON.parse(JSON.stringify(doc)), 'the card') === null && b.state.color === 'teal' && b.applied[0] === 'the card', 'the app’s own design opens');
    ok(/not one this editor wrote/.test(b.ses.apply({ format: 'wckit' })), 'a foreign document is refused');
    ok(/the clock app’s, not tint’s/.test(b.ses.apply(Object.assign({}, doc, { app: 'clock' }))), 'another app’s design is refused by name');
    ok(/that design has no tint in it/.test(b.ses.apply(Object.assign({}, doc, { color: '' }))), 'then the app’s own refusal');
    const newer = b.ses.apply(Object.assign({}, doc, { appVersion: 'v1.4.0+0000000' }));
    ok(/made by tint v1\.4\.0\+0000000, newer than this editor \(v1\.3\.0\+6a5c661\)/.test(newer || ''), 'a design from a newer release is refused, so no field it added is dropped', newer);
    ok(b.ses.apply(Object.assign({}, doc, { appVersion: 'v1.3.0+fffffff' })) === null, 'another build of this release opens: the hash never orders');
    ok(b.ses.apply(Object.assign({}, doc, { appVersion: 'v1.2.9' })) === null, 'and one from an older release opens');
    const future = b.ses.apply(Object.assign({}, doc, { schema: 2 }));
    ok(/design schema 2\); this editor \(v1\.3\.0\+6a5c661\) reads schema 1/.test(future || ''), 'a newer schema names what this editor reads', future);
    // migrations: one pure step per bump, never guessed
    const m = mk(); m.schema = 3; m.steps = { 1: (d) => Object.assign(d, { color: 'from-1' }), 2: (d) => Object.assign(d, { size: 77 }) };
    const mig = m.ses.migrate(Object.assign({}, doc));
    ok(mig.doc && mig.doc.schema === 3 && mig.doc.color === 'from-1' && mig.doc.size === 77 && doc.schema === 1, 'an older design walks forward one step at a time, leaving the original alone');
    m.steps = { 1: (d) => d };
    ok(/no step from schema 2 to 3/.test(m.ses.apply(Object.assign({}, doc)) || ''), 'a missing step is refused by name');
  }

  // ---- opening a file: an .orbdesign someone already has (K10 retired the download), an .orbtheme
  {
    const a = mk(); a.set({ color: 'navy', name: 'Deep sea' });
    ok(!('exportDoc' in a.ses), 'K10: there is no design-file download; the theme is the one file that leaves the hub');
    const fileOf = async (bytes, name) => ({ name, arrayBuffer: async () => bytes.buffer });
    const enc = new TextEncoder();
    const body = enc.encode(JSON.stringify(await a.ses.doc(true)));
    const b = mk();
    b.ses.track(); edit(b, { size: 5 });
    ok(await b.ses.openFile(await fileOf(body, 'Deep sea-1.orbdesign')) && b.state.color === 'navy' && b.applied[0] === 'Deep sea-1.orbdesign', 'a design file opens');
    ok(!b.ses.undoStack.length && b.ses.snap === null, 'and is where the history starts');
    ok(!(await b.ses.openFile(await fileOf(enc.encode('nope'), 'x.txt'))) && /neither an \.orbtheme nor a design file/.test(b.state.status), 'a file that is neither is refused', b.state.status);
    // an .orbtheme: its design region, read by the OS baker
    let read = null;
    window.GorbBake = { readDesign3: (bytes) => { read = bytes.length; return bytes[4] === 1 ? { design: body } : { design: null, why: 'that theme carries no design' }; } };
    const theme = new Uint8Array([0x54, 0x52, 0x4f, 0x47, 1, 0, 0, 0]);
    const c = mk();
    ok(await c.ses.openFile(await fileOf(theme, 'reef.orbtheme')) && read === 8 && c.state.color === 'navy', 'an .orbtheme opens through its design region');
    theme[4] = 0;
    ok(!(await c.ses.openFile(await fileOf(theme, 'bare.orbtheme'))) && /Not opened: that theme carries no design/.test(c.state.status), 'and one without a design says so', c.state.status);
    // the hub's Edit door
    const asked = [];
    hub = async (url) => { asked.push(url); return /bad/.test(url) ? { ok: false, status: 404, json: async () => ({ error: 'no such theme on the card' }) } : { ok: true, status: 200, arrayBuffer: async () => body.buffer }; };
    location.search = '?theme=tint-reef-000000000000&orb=192.0.2.7';
    const d = mk();
    ok(await d.ses.openTheme() && asked[0] === '/api/theme/design?ip=192.0.2.7&id=tint-reef-000000000000' && d.applied[0] === 'the card', 'the Edit door opens a card theme through the hub', asked[0]);
    ok(/^Opened the theme from the card/.test(d.state.status), 'and says it opened: the Opening line does not outlive the open (Gold, 2026-11-08)', d.state.status);
    location.search = '?theme=tint-reef-000000000000&src=shelf';
    const e = mk(); await e.ses.openTheme();
    ok(asked[1] === '/api/theme/design?name=tint-reef-000000000000' && e.applied[0] === 'the hub’s shelf', 'and a shelf theme by name', asked[1]);
    location.search = '?theme=bad-000000000000&orb=192.0.2.7';
    const f = mk();
    ok(!(await f.ses.openTheme()) && /Could not reopen that theme: no such theme on the card/.test(f.state.status), 'a theme the hub cannot find is said in the hub’s words', f.state.status);
    location.search = '?theme=../../etc';
    ok(!(await mk().ses.openTheme()), 'a theme name that is not a card stem is not asked for');
    location.search = '';
    ok(!(await mk().ses.openTheme()), 'and no ?theme= is no open');
  }

  // ---- import (theme-editor-kit-plan §8.1 ruling 5): into the library first, then open
  {
    const G = require(path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'));
    window.GorbBake = G;
    location.pathname = ON_HUB; location.search = '';
    const enc = new TextEncoder();
    const fileOf = (bytes, name) => ({ name, arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) });
    const dsn = (name, color, app) => ({ format: 'wc-design', schema: 1, app: app || 'tint', appVersion: 'v1.3.0', color, size: 7, name });
    const bake = (name, design) => G.writeBundle3({ name, design: design ? enc.encode(JSON.stringify(design)) : undefined,
      sections: [{ slug: 'tint', elements: [], scalars: {}, sprites: [], art: new Uint8Array(16) }] }).bytes;
    let asks = [];
    // the question is asked with no opening cover over it (a cover would take its clicks)
    let coveredAsk = null;
    const o = (yes) => ({ nameOf: (d) => d.name, thumb: async (d) => 'data:image/jpeg;base64,' + d.color, confirm: async (nm) => { asks.push(nm); coveredAsk = a.ses.opening || c0.ses.opening; return yes; } });
    const keeps = () => fh.calls.filter((u) => /\/api\/library\/keep/.test(u)).length;
    const a = mk(); await a.ses.boot();
    let c0 = a;
    const k0 = keeps();
    ok(await a.ses.importFile(fileOf(bake('Coral', dsn('Coral', 'coral')), 'coral.orbtheme'), o(true)), 'an .orbtheme imports');
    ok(keeps() === k0 + 1 && fh.themes.get('tint').has('Coral'), 'it is kept on the hub shelf, as a theme', String(keeps() - k0));
    ok(a.ses.libraryEntry('Coral') && a.ses.libraryEntry('Coral').state === 'theme', 'and is in the library before it opens');
    ok(a.state.color === 'coral' && /Imported “Coral” into the library as a theme/.test(a.state.status), 'then it opens', a.state.status);
    ok(!asks.length, 'a new name asks nothing');
    const b = mk(); await b.ses.boot();
    ok(await b.ses.importFile(fileOf(enc.encode(JSON.stringify(dsn('Kelp', 'green'))), 'Kelp.orbdesign'), o(true)) &&
      fh.drafts.get('tint').has('Kelp') && b.ses.libraryEntry('Kelp').state === 'draft', 'a design file imports as a draft');
    ok(b.ses.libraryEntry('Kelp').thumb === 'data:image/jpeg;base64,green' && b.state.color === 'green', 'with its picture, and opens', b.state.status);
    // a name already in the library asks; a no leaves the library's own as it was
    asks = [];
    const c = mk(); await c.ses.boot(); c.set({ color: 'grey' }); c0 = c;
    ok(!(await c.ses.importFile(fileOf(bake('Coral', dsn('Coral', 'red')), 'coral2.orbtheme'), o(false))), 'a refused replace imports nothing');
    ok(asks[0] === 'Coral' && fh.themes.get('tint').get('Coral').design.color === 'coral' && c.state.color === 'grey', 'it asked, kept the library’s own and opened nothing', c.state.status);
    ok(/is still the library’s own/.test(c.state.status), 'and says so', c.state.status);
    ok(coveredAsk === null, 'and asked with no opening cover over the question', String(coveredAsk));
    ok(await c.ses.importFile(fileOf(bake('Coral', dsn('Coral', 'red')), 'coral2.orbtheme'), o(true)) && fh.themes.get('tint').get('Coral').design.color === 'red', 'a yes replaces it');
    // refusals come before anything is kept
    const k1 = keeps();
    ok(!(await c.ses.importFile(fileOf(bake('Alien', dsn('Alien', 'blue', 'clock')), 'alien.orbtheme'), o(true))) && keeps() === k1 &&
      /Not imported: that design is the clock app’s/.test(c.state.status), 'another app’s theme is refused, and not kept', c.state.status);
    ok(!(await c.ses.importFile(fileOf(enc.encode('nope'), 'x.txt'), o(true))) && /neither an \.orbtheme nor a design file/.test(c.state.status), 'a file that is neither is refused');
    ok(!(await c.ses.importFile(fileOf(enc.encode(JSON.stringify(dsn('', 'red'))), 'anon.orbdesign'), o(true))) && /names no theme/.test(c.state.status), 'a design with no name is refused', c.state.status);
    // a theme with no design is kept (it installs) but does not open
    const d = mk(); await d.ses.boot(); d.set({ color: 'grey' });
    ok(await d.ses.importFile(fileOf(bake('Bare'), 'bare.orbtheme'), o(true)) && fh.themes.get('tint').has('Bare') && d.state.color === 'grey' &&
      /carries no design, so it installs from the Themes tab but cannot be opened here/.test(d.state.status), 'a theme with no design is kept, not opened', d.state.status);
    location.pathname = '/';
    ok(!(await mk().ses.importFile(fileOf(bake('Off', dsn('Off', 'red')), 'off.orbtheme'), o(true))), 'off the hub nothing imports');
    // the export's Save dialog, and the download where there is none
    const blob = { size: 3 }, wrote = [];
    let picked = null;
    window.showSaveFilePicker = async (x) => { picked = x; return { createWritable: async () => ({ write: async (b) => wrote.push(b), close: async () => {} }) }; };
    ok(await OrbTheme.saveFile(blob, 'reef.orbtheme') === 'saved' && picked.suggestedName === 'reef.orbtheme' && wrote[0] === blob, 'export asks where to save, and writes there');
    window.showSaveFilePicker = async () => { const e = new Error('closed'); e.name = 'AbortError'; throw e; };
    ok(await OrbTheme.saveFile(blob, 'reef.orbtheme') === 'cancelled', 'closing the dialog saves nothing');
    const dl = []; const was = OrbTheme.download; OrbTheme.download = (b, n) => dl.push(n);
    delete window.showSaveFilePicker;
    ok(await OrbTheme.saveFile(blob, 'reef.orbtheme') === 'downloaded' && dl[0] === 'reef.orbtheme', 'a browser with no dialog downloads it');
    OrbTheme.download = was;
  }

  // ---- the site's two flags, seen from the hub: nothing at all changes
  {
    location.pathname = ON_HUB; location.search = '';
    const asked = [];
    const inner = global.fetch;
    global.fetch = async (u, o) => { asked.push(String(u)); return inner(u, o); };
    const b64of = (n, seed) => { const u = new Uint8Array(n); for (let i = 0; i < n; i++) u[i] = (seed * 7 + i * 13) & 0xff; return btoa(Array.from(u, (x) => String.fromCharCode(x)).join('')); };
    const url = 'data:image/png;base64,' + b64of(600, 3);
    const artDoc = () => ({ format: 'wc-design', schema: 1, app: 'tint', appVersion: 'v1.3.0', color: 'teal', size: 5, name: 'Hashed', art: [url, { deep: url }] });
    const a = mk();
    ok(await a.ses.saveDraft({ name: 'Hashed', doc: artDoc() }), 'on the hub a design with a large image still saves');
    const held = fh.drafts.get('tint').get('Hashed');
    ok(held.art[0] === url && held.art[1].deep === url, 'the hub holds that art inline, exactly as it did', JSON.stringify(held.art).slice(0, 30));
    ok(!('orbAssets' in held) && JSON.stringify(held).indexOf('orb-asset:') < 0, 'with no refs and no sidecar asked for by anything', JSON.stringify(held).slice(0, 40));
    ok(a.ses.opened === null && a.ses.sharedName === null && a.ses.bakeName('Mine') === 'Mine', 'and the session holds no design id and its own bake name');
    await a.ses.libraryRefresh();
    ok(await a.ses.openEntry('Hashed') && !asked.some((u2) => /refs=1|\/asset\?/.test(u2)), 'an open asks for no ref form and no image', asked.join(' ').slice(0, 90));
    ok(await a.ses.saveAs({ name: 'Hashed two', doc: artDoc() }) && !asked.some((u2) => /saveAs|&id=/.test(u2)), 'and so does a Save As', asked.slice(-1)[0]);
    const s = mk();
    location.search = '?shared=abcdefghijklmnopqrstuv';
    ok(s.ses.opening === null, 'a hub page opened with ?shared= gets no shared cover');
    ok((await s.ses.boot()) === true && s.ses.library().length > 0, 'a hub page ignores ?shared= and reads its library as it always did');
    ok(!(await s.ses.openTheme()) && !asked.some((u2) => /shared\/bundle/.test(u2)), 'and its openTheme asks the hub for no bundle', asked.slice(-1)[0]);
    location.search = '';
    global.fetch = inner;
  }

  // ---- the bake name
  {
    const a = mk();
    ok(a.ses.bakeName('Mine') === 'Mine' && a.ses.sharedName === null, 'an editor bakes under its own name');
    a.ses.apply({ format: 'wc-design', schema: 1, app: 'tint', appVersion: 'v1.3.0', color: 'teal', shared: { name: 'Skeleton by @greg' } }, 'the shared design');
    ok(a.ses.sharedName === 'Skeleton by @greg' && a.ses.bakeName('Mine') === 'Skeleton by @greg', 'a design opened under a shared name bakes under that name', a.ses.sharedName);
    a.ses.apply({ format: 'wc-design', schema: 1, app: 'tint', appVersion: 'v1.3.0', color: 'teal', shared: { name: '—'.repeat(20) } }, 'the shared design');
    ok(a.ses.sharedName === null, 'a name longer than 55 bytes is not a shared name', String(a.ses.sharedName));
    a.ses.apply({ format: 'wc-design', schema: 1, app: 'tint', appVersion: 'v1.3.0', color: 'teal', shared: { name: '  ' } }, 'the shared design');
    ok(a.ses.sharedName === '  ' && a.ses.bakeName('Mine') === '  ', 'a short one stands as it was written');
    a.ses.apply({ format: 'wc-design', schema: 1, app: 'tint', appVersion: 'v1.3.0', color: 'teal' }, 'the card');
    ok(a.ses.sharedName === null && a.ses.bakeName('Mine') === 'Mine', 'and opening anything else puts the editor’s own name back');
    a.ses.apply({ format: 'wc-design', schema: 1, app: 'tint', appVersion: 'v1.3.0', color: 'teal', shared: 'not an object' }, 'the card');
    ok(a.ses.sharedName === null, 'a shared field that is not an object is no shared name');
    ok(a.ses.bakeName(undefined) === undefined, 'with nothing to say, bakeName says nothing');
  }

  console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw: ' + (e && e.stack || e)); process.exit(1); });
