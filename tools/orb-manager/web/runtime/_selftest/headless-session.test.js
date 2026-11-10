#!/usr/bin/env node
// theme@1/session (theme-editor-kit-plan K2, K9): the OS's session, driven by a small made-up
// app ("tint": a colour and a size) rather than the clock, so nothing clock-shaped can hide in
// it. History with bursts, no autosave, per-app slugs, the app's library on the hub (K10,
// fake-hub.js: drafts, themes, unsent changes, opening at the newest design, discarding,
// forgetting, ?entry=, Rebake all), the move of a browser's old designs, the hub's word on
// the orb's release, the design envelope with its migrations and refusals, the older-design
// note, opening a design file, an .orbtheme's design and the hub's Edit door, and a site's
// stale-editor hand-off: a save refused with a newer version carried through sessionStorage into
// the reloaded editor (orb-os-net §6.5).
//
//   node headless-session.test.js
'use strict';
const path = require('path');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
setTimeout(() => { console.log('FAIL timed out (' + checked + ' checks run)'); process.exit(1); }, 20000);

// the window: a browser store that can be made to refuse, an address, and the hub
const store = new Map(); let refuse = false;
const sstore = new Map(); let ssRefuse = false, replaced = [];
global.window = globalThis;
global.localStorage = {
  getItem: (k) => (store.has(k) ? store.get(k) : null),
  setItem: (k, v) => { if (refuse) { const e = new Error('full'); e.name = 'QuotaExceededError'; throw e; } store.set(k, String(v)); },
  removeItem: (k) => { store.delete(k); },
};
global.sessionStorage = {
  getItem: (k) => (sstore.has(k) ? sstore.get(k) : null),
  setItem: (k, v) => { if (ssRefuse) { const e = new Error('full'); e.name = 'QuotaExceededError'; throw e; } sstore.set(k, String(v)); },
  removeItem: (k) => { sstore.delete(k); },
};
global.location = { origin: 'http://hub', search: '', pathname: '/', replace: (u) => { replaced.push(String(u)); } };
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
    ok(newer === null && b.state.color === 'teal' && b.ses.lastMade === null, 'a same-schema design from a newer release opens, saved as this release (ruling 21)', String(newer));
    ok(b.ses.apply(Object.assign({}, doc, { appVersion: 'v9.0.0' })) === null, 'and so does a much newer release: only the schema says what an editor can read');
    ok(b.ses.apply(Object.assign({}, doc, { appVersion: 'v1.3.0+fffffff' })) === null, 'another build of this release opens: the hash never orders');
    ok(b.ses.apply(Object.assign({}, doc, { appVersion: 'v1.2.9' })) === null, 'and one from an older release opens');
    const future = b.ses.apply(Object.assign({}, doc, { schema: 2 }));
    ok(/design schema 2\); this editor \(v1\.3\.0\+6a5c661\) reads schema 1/.test(future || ''), 'a newer schema names what this editor reads', future);
    const oldbuild = b.ses.apply(Object.assign({}, doc, { schema: 2, appVersion: 'v1.2.0' }));
    ok(/reads schema 1/.test(oldbuild || ''), 'and a newer schema is refused whatever release made it', oldbuild);
    [0, 1.5, 'abc', null].forEach((bad, i) => {
      const d2 = Object.assign({}, doc);
      if (bad === null) delete d2.schema; else d2.schema = bad;
      const w = b.ses.apply(d2);
      ok(/reads schema 1/.test(w || ''), 'a schema that is not a whole number from 1 is refused, not half-read (#' + i + ')', w);
    });
    ok(b.ses.apply(Object.assign({}, doc, { schema: '1' })) === null, 'a schema spelled as a numeric string is still this schema, so it opens');
    // migrations: one pure step per bump, never guessed
    const m = mk(); m.schema = 3; m.steps = { 1: (d) => Object.assign(d, { color: 'from-1' }), 2: (d) => Object.assign(d, { size: 77 }) };
    const mig = m.ses.migrate(Object.assign({}, doc));
    ok(mig.doc && mig.doc.schema === 3 && mig.doc.color === 'from-1' && mig.doc.size === 77 && doc.schema === 1, 'an older design walks forward one step at a time, leaving the original alone');
    m.steps = { 1: (d) => d };
    ok(/no step from schema 2 to 3/.test(m.ses.apply(Object.assign({}, doc)) || ''), 'a missing step is refused by name');
    // the start check (design §6.5 point 1): editor.json's schema and this editor's are one fact
    const startThrow = (stamp) => {
      window.__ORB_APP = stamp;
      try { mk(); return null; } catch (e) { return e; }
    };
    const drift = startThrow({ slug: 'tint', version: 'v1.3.0', schema: 2 });
    ok(drift instanceof Error && /reads design schema 1, but its editor\.json says 2/.test(drift.message),
      'a session whose editor.json names another schema refuses to start', drift && drift.message);
    ok(startThrow({ slug: 'tint', version: 'v1.3.0', schema: 1 }) === null, 'and starts when the two agree');
    ok(startThrow({ slug: 'tint', version: 'v1.3.0' }) === null, 'and when the stamp names no schema: an app built before the key');
    ok(startThrow(null) === null, 'and when there is no stamp at all: an unpackaged editor');
    window.__ORB_APP = { slug: 'tint', version: 'v1.3.0+6a5c661' };
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

  // ---- the host's words (design §6.3): the session's own lines change with the host, and a
  // site's session never says a word about a hub or a Send to orb
  {
    const WEB = { kind: 'web', api: '/studio-api', send: false, publish: true, inbox: true };
    const dsg = (c) => ({ format: 'wc-design', schema: 1, app: 'tint', appVersion: 'v1.3.0', color: c });
    const keepVer = window.__ORB_APP, keepFetch = global.fetch;
    window.__ORB_APP = { slug: 'tint', version: 'v1.3.0' };
    // the same session, the same routes, the same order: two hub runs differ by nothing but the words
    const words = async (host) => {
      global.GorbHost = host; location.pathname = '/editor/192.0.2.7/tint/'; location.search = ''; store.clear();
      fh.drafts.clear(); fh.themes.clear(); fh.calls.length = 0; fh.refuse = null; fh.source = { source: 'library', relation: 'same' };
      fh.themes.set('tint', new Map([['One', { design: dsg('shelved'), stale: '', at: 1, bytes: 64 }]]));
      // a web host addresses /studio-api; map it back onto the fake hub's /api so both run the same routes
      global.fetch = (url, o) => fh.fetch(String(url).replace(/^\/studio-api/, '/api'), o);
      const a = mk();
      await a.ses.boot();
      const out = {};
      out.refused = (a.ses.check({ format: 'wc-design', schema: 9, app: 'tint', appVersion: 'v9.9.9' }) || {}).why || '';
      await a.ses.saveDraft({ name: 'One', doc: dsg('teal') }); out.savedChanged = a.state.status;
      await a.ses.saveDraft({ name: 'Two', doc: dsg('teal') }); out.savedDraft = a.state.status;
      const keep = global.fetch;
      global.fetch = async () => ({ ok: false, status: 404, json: async () => ({ ok: false }) });   // /library/design with no sentence of its own
      await a.ses.openEntry('Missing'); out.missing = a.state.status;
      global.fetch = async () => ({ ok: false, status: 500, json: async () => ({ ok: false }) });
      await a.ses.libraryRefresh(); out.refreshFail = a.state.status;
      global.fetch = keep;
      location.pathname = '/v1.4.2/';                                                                // off any hub's editor url: no library
      await a.ses.saveDraft({ name: 'Three', doc: dsg('teal') }); out.noLibrary = a.state.status;
      await a.ses.openEntry('One'); out.offHubOpen = a.state.status;
      return out;
    };
    const said = await words(undefined), hubbed = await words({ kind: 'hub' }), webbed = await words(WEB);
    delete global.GorbHost;
    global.fetch = keepFetch; if (keepVer) window.__ORB_APP = keepVer; else delete window.__ORB_APP;
    ok(JSON.stringify(said) === JSON.stringify(hubbed), 'every session line is the same string with no GorbHost as with kind "hub"');
    const HUB_SED = {
      refused: 'that design was made by tint v9.9.9 (design schema 9); this editor (v1.3.0) reads schema 1 — update the app on this orb',
      savedChanged: 'Saved “One” to the library as unsent changes to its theme — Send to orb bakes them in',
      savedDraft: 'Saved “Two” to the library as a draft',
      missing: 'Could not open “Missing”: the hub has no design for “Missing”',
      refreshFail: 'The library could not be read from the hub: the hub answered 500',
      noLibrary: 'Not saved: the library is kept on the hub: open this editor from the hub, or download the .orbtheme',
      offHubOpen: 'the library is kept on the hub: open this editor from the hub, or download the .orbtheme',
    };
    ok(Object.keys(HUB_SED).every((k) => hubbed[k] === HUB_SED[k]), 'and each of them is the hub’s sentence, byte for byte',
      Object.keys(HUB_SED).map((k) => k + '=' + hubbed[k]).join(' | '));
    const WEB_SED = {
      refused: 'that design was made by tint v9.9.9 (design schema 9); this editor (v1.3.0) reads schema 1 — update the app on your orb to read it',
      savedChanged: 'Saved “One” to the library as unsent changes to its theme — Download .orbtheme bakes them in',
      savedDraft: 'Saved “Two” to the library as a draft',
      missing: 'Could not open “Missing”: the server has no design for “Missing”',
      refreshFail: 'The library could not be read: the server answered 500',
    };
    ok(Object.keys(WEB_SED).every((k) => webbed[k] === WEB_SED[k]), 'under a web host each is the site’s sentence instead',
      Object.keys(WEB_SED).map((k) => k + '=' + webbed[k]).join(' | '));
    ok(!/hub|Send to orb|Themes tab/i.test(Object.keys(WEB_SED).map((k) => webbed[k]).join(' ')),
      'and no site sentence says hub, Send to orb or Themes tab', Object.keys(WEB_SED).map((k) => webbed[k]).join(' ').slice(0, 300));
    // hostText itself: the hub's sentence is the default, and only kind "web" picks the other
    global.GorbHost = undefined;
    ok(OrbTheme.hostText('on the hub', 'in your library') === 'on the hub', 'no GorbHost is the hub’s words');
    global.GorbHost = { kind: 'hub' };
    ok(OrbTheme.hostText('on the hub', 'in your library') === 'on the hub', 'kind "hub" is the hub’s words');
    global.GorbHost = { kind: 'web' };
    ok(OrbTheme.hostText('on the hub', 'in your library') === 'in your library', 'kind "web" is the site’s words');
    delete global.GorbHost;
  }

  // ---- a tab left open across an update (orb-os-net §6.5): a site that refuses a save because its
  // editor was updated hands the design to the newer one through sessionStorage, and that editor
  // opens it back on boot. Nothing is lost, and the user saves again.
  {
    const WEB = { kind: 'web', api: '/studio-api', designIds: true };
    const WEBART = { kind: 'web', api: '/studio-api', assets: true, designIds: true };
    const keepVer = window.__ORB_APP, keepFetch = global.fetch, ssWas = global.sessionStorage;
    window.__ORB_APP = { slug: 'tint', version: 'v1.0.0' };
    const KEY = 'orb.handoff.tint', ID = 'abcdefghijklmnopqrstuv';
    const UPD = { status: 409, body: { ok: false, error: "Clock's editor was updated", updated: { version: 'v9.9.9' } } };
    const dsg = (c, extra) => Object.assign({ format: 'wc-design', schema: 1, app: 'tint', appVersion: 'v1.0.0', color: c, size: 5 }, extra || {});
    const stashOf = () => JSON.parse(sstore.get(KEY) || 'null');
    const reset = () => { sstore.clear(); store.clear(); replaced.length = 0; fh.drafts.clear(); fh.themes.clear(); fh.calls.length = 0; fh.refuse = null; fh.source = { source: 'library', relation: 'same' }; location.pathname = '/v1.0.0/'; location.search = ''; };
    // the fake hub as a site at /studio-api, with /design/save answered as `ans` says (null: as usual)
    const site = (ans) => async (url, o) => {
      const u = String(url).replace(/^\/studio-api/, '/api');
      if (ans && /^\/api\/design\/save/.test(u)) return { ok: false, status: ans.status, json: async () => JSON.parse(JSON.stringify(ans.body)) };
      return fh.fetch(u, o);
    };
    const saidReload = "Not saved: Clock's editor was updated. Reopening your design in v9.9.9…";
    const saidTooBig = "Not saved: Clock's editor was updated, and this design is too big to carry over to it: use Export to keep it as a file, then reload and import it";
    const web = (host) => { reset(); global.GorbHost = host; global.fetch = site(UPD); };

    ok(mk().ses.handoffKey() === KEY, 'the stash is keyed by the runtime and the app', mk().ses.handoffKey());
    location.search = '?app=clock';
    ok(mk().ses.handoffKey() === 'orb.handoff.clock', 'and two apps of one site never share it');

    // 1. a save refused with a newer version: the design is stashed and the tab reloaded into it
    web(WEB);
    location.search = '?theme=D1';
    const a = mk(); await a.ses.boot();
    a.ses.opened = { id: ID, name: 'One' };
    const t0 = Date.now();
    const r1 = await a.ses.saveDraft({ name: 'One', doc: dsg('teal'), dial: 'round' });
    const st1 = stashOf();
    ok(r1 === false, 'a save the site refused because its editor was updated did not save');
    ok(replaced.length === 1 && replaced[0] === '/v9.9.9/', 'the tab reloaded into the version the site named, at the same page, its query dropped', JSON.stringify(replaced));
    ok(st1 && st1.format === 1 && st1.app === 'tint' && st1.to === 'v9.9.9' && st1.from === 'v1.0.0', 'the design went into sessionStorage with where it came from and where it is going', JSON.stringify(st1 && [st1.format, st1.app, st1.to, st1.from]));
    ok(st1.name === 'One' && JSON.stringify(st1.doc) === JSON.stringify(dsg('teal')) && JSON.stringify(st1.extra) === JSON.stringify({ dial: 'round' }), 'the entry’s name, its document, and the extras the library lists it by', JSON.stringify(st1 && st1.extra));
    ok(JSON.stringify(st1.opened) === JSON.stringify({ id: ID, name: 'One' }), 'and the design id this page held, so the new one saves back over it', JSON.stringify(st1 && st1.opened));
    ok(typeof st1.at === 'number' && st1.at >= t0 && st1.at <= Date.now(), 'stamped with the moment it was written', String(st1 && st1.at));
    ok(a.state.status === saidReload, 'and the line says the site’s sentence and the reload', a.state.status);
    // the document the stash holds is the editor's own, not the ref form the save posts
    web(WEBART);
    const bytes = new Uint8Array(600); for (let i = 0; i < 600; i++) bytes[i] = (i * 7 + 3) & 0xff;
    const sha = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes))).map((x) => (x < 16 ? '0' : '') + x.toString(16)).join('');
    const pic = 'data:image/png;base64,' + btoa(Array.from(bytes, (x) => String.fromCharCode(x)).join(''));
    const artDoc = { format: 'wc-design', schema: 1, app: 'tint', appVersion: 'v1.0.0', color: 'teal', size: 5, art: [pic] };
    global.fetch = site(null);
    const wt = mk(); await wt.ses.boot();
    // the first save posts that image inline for the site to keep, the next one sends the ref it
    // learned from that — the shape this site wants a save to look like, and what the stash is not
    ok(await wt.ses.saveDraft({ name: 'One', doc: artDoc }) && await wt.ses.saveDraft({ name: 'One', doc: artDoc }) &&
      fh.drafts.get('tint').get('One').art[0] === 'orb-asset:' + sha,
      'a site with the assets flag was sent that image as a ref once it knew it', String(fh.drafts.get('tint').get('One').art[0]).slice(0, 22));
    global.fetch = site(UPD);
    wt.ses.opened = { id: ID, name: 'One' };
    ok(!(await wt.ses.saveDraft({ name: 'One', doc: artDoc })) && stashOf().doc.art[0] === pic && artDoc.art[0] === pic,
      'and the stash carries the document as the editor built it, its art inline and its own copy unchanged', String(stashOf().doc.art[0]).slice(0, 24));
    // 2. a design the browser will not hold is not carried, and the tab does not reload
    web(WEB); ssRefuse = true;
    const q = mk(); await q.ses.boot();
    ok(!(await q.ses.saveDraft({ name: 'One', doc: dsg('teal') })) && !replaced.length && !sstore.size,
      'a stash the browser refuses keeps the page where it is');
    ok(q.state.status === saidTooBig, 'and names Export as the way to keep the design', q.state.status);
    ssRefuse = false;
    delete global.sessionStorage;
    const nz = mk(); await nz.ses.boot();
    ok(!(await nz.ses.saveDraft({ name: 'One', doc: dsg('teal') })) && !replaced.length && nz.state.status === saidTooBig,
      'a browser with no sessionStorage at all says the same line and reloads nothing');
    global.sessionStorage = ssWas;
    // 3. every other refusal is exactly today's
    web(WEB); global.fetch = site({ status: 409, body: { ok: false, error: 'you already have a design called “One”' } });
    const n1 = mk(); await n1.ses.boot();
    ok(!(await n1.ses.saveDraft({ name: 'One', doc: dsg('teal') })) && !replaced.length && !sstore.size &&
      n1.state.status === 'Not saved: you already have a design called “One”', 'a 409 naming no new version says only its sentence', n1.state.status);
    global.fetch = site({ status: 400, body: { ok: false, error: 'that is not a design document' } });
    ok(!(await mk().ses.saveDraft({ name: 'One', doc: dsg('teal') })) && !replaced.length && !sstore.size, 'and so does a 400');
    global.fetch = site({ status: 409, body: { ok: false, error: 'the editor was updated', updated: { version: 'latest' } } });
    const n2 = mk();
    ok(!(await n2.ses.saveDraft({ name: 'One', doc: dsg('teal') })) && !replaced.length && !sstore.size && n2.state.status === 'Not saved: the editor was updated',
      'an updated whose version is not one is no hand-off: the grammar is the whole of the trust', n2.state.status);
    global.fetch = site({ status: 409, body: { ok: false, error: 'the editor was updated', updated: 'v9.9.9' } });
    ok(!(await mk().ses.saveDraft({ name: 'One', doc: dsg('teal') })) && !replaced.length && !sstore.size,
      'and an updated that is not an object is none either');
    // 4. a hub given the same answer changes nothing
    const underHub = async (host) => {
      web(undefined); global.GorbHost = host; location.pathname = ON_HUB;
      const h = mk(); await h.ses.boot();
      const no = await h.ses.saveDraft({ name: 'One', doc: dsg('teal') });
      return [no, replaced.length, sstore.size, h.state.status];
    };
    delete global.GorbHost;
    ok(JSON.stringify(await underHub(undefined)) === JSON.stringify([false, 0, 0, "Not saved: Clock's editor was updated"]),
      'under no GorbHost at all the same refusal is today’s: nothing stashed, no reload');
    ok(JSON.stringify(await underHub({ kind: 'hub' })) === JSON.stringify([false, 0, 0, "Not saved: Clock's editor was updated"]),
      'and the same under kind "hub"');
    // 5. the new editor's boot: the stash is migrated by its own steps and opens in its place
    // arms a site page for a boot with `stash` in sessionStorage, and records the urls it asks for
    const arm = (stash, search) => {
      const urls = [];
      reset(); global.GorbHost = WEB; location.search = search || '';
      global.fetch = (u, o) => { urls.push(String(u)); return site(null)(u, o); };
      if (stash !== null) sstore.set(KEY, typeof stash === 'string' ? stash : JSON.stringify(stash));
      return urls;
    };
    const of5 = (o) => Object.assign({ format: 1, app: 'tint', to: 'v1.0.0', from: 'v0.9.9', name: 'One', doc: dsg('teal', { schema: 1, appVersion: 'v0.9.9' }), extra: { dial: 'round' }, opened: { id: ID, name: 'One' }, at: 5 }, o || {});
    const seen5 = [];
    const m5 = mk({ apply: (o, from) => { seen5.push({ o: o, from: from }); m5.set({ color: o.color, size: o.size, name: o.name || '' }); return null; } });
    m5.schema = 2; m5.steps = { 1: (d) => Object.assign(d, { color: 'migrated-' + d.color }) };
    const urls5 = arm(of5(), '?entry=One');
    fh.drafts.set('tint', new Map([['One', { format: 'wc-design', schema: 1, app: 'tint', appVersion: 'v1.0.0', color: 'plum', size: 5, saved: { name: 'One', at: 4, extra: {} } }]]));
    ok((await m5.ses.boot()) === true, 'a boot that reopened a design still says it booted');
    ok(seen5.length === 1 && seen5[0].from === 'the design you were editing' && seen5[0].o.schema === 2 && seen5[0].o.color === 'migrated-teal',
      'the stashed design came through this editor’s check and its step up to this schema', JSON.stringify(seen5[0] && seen5[0].o));
    ok(!sstore.has(KEY), 'the key is gone the moment it is read, so a design that throws cannot loop');
    ok(JSON.stringify(m5.ses.opened) === JSON.stringify({ id: ID, name: 'One' }), 'and the page holds the design id the old one held');
    ok(m5.state.status === "tint's editor was updated to v1.0.0: your design is back as you left it. Save it again to keep it", 'the line says who updated and what to do', m5.state.status);
    ok(!urls5.some((u) => /library\/design/.test(u)), 'and the ?entry= this reloaded page carried was not opened', urls5.join(' '));
    await m5.ses.saveDraft({ name: 'One', doc: dsg('teal') });
    ok(urls5.some((u) => /design\/save\?app=tint&name=One&id=abcdefghijklmnopqrstuv$/.test(u)), 'the next Save overwrites that very design', urls5[urls5.length - 1]);
    // 6. a stash this editor has no step for
    const m6 = mk(); m6.schema = 3; m6.steps = {};
    arm(of5());
    ok((await m6.ses.boot()) === true && !sstore.has(KEY) && !m6.applied.length,
      'a design it cannot read is dropped, and the boot goes on as if there were no stash');
    ok(m6.state.status === 'Your design from v0.9.9 could not be reopened in v1.0.0: that design is schema 1 and this editor has no step from schema 1 to 2',
      'and check’s own sentence is said in full', m6.state.status);
    ok(m6.ses.opened === null, 'the page holds no design id, so the next Save asks for a new one');
    // 7. what is not this app's stash, in any of the ways
    for (const bad of [of5({ app: 'clock' }), of5({ format: 2 }), 'not json at all {', '{"format":1,"app":"tint"', JSON.stringify(of5({ doc: null }))]) {
      const e7 = mk(); arm(bad);
      await e7.ses.boot();
      ok(!sstore.has(KEY) && !e7.applied.length, 'a stash that is not one this editor can reopen is removed and opened by nothing: ' + String(JSON.stringify(bad)).slice(0, 24));
    }
    const e7b = mk(); arm(of5({ doc: null }));
    await e7b.ses.boot();
    ok(e7b.state.status === 'Your design from v0.9.9 could not be reopened in v1.0.0: that design is not one this editor wrote',
      'a stash with no document says why in the same line', e7b.state.status);
    const e7c = mk({ apply: (o, from) => { e7c.set({ color: o.color, size: o.size, name: o.name || '' }); return null; } });
    arm(of5({ opened: { id: 'too-short', name: 'One' } }));
    await e7c.ses.boot();
    ok(e7c.state.color === 'teal' && e7c.ses.opened === null && /your design is back/.test(e7c.state.status),
      'an id that is not one is no design id: the design opens and the next Save makes a new one', JSON.stringify(e7c.ses.opened));
    // 8. a hub never reads or removes the key, in either of its two spellings
    const held8 = JSON.stringify(of5());
    for (const host of [undefined, { kind: 'hub' }]) {
      const h8 = mk(); arm(null); global.GorbHost = host; location.pathname = host === undefined || host.kind === 'hub' ? ON_HUB : '/v1.0.0/';
      sstore.set(KEY, held8);
      await h8.ses.boot();
      ok(sstore.get(KEY) === held8 && !h8.applied.length, 'a hub boot leaves a stash it has no part in exactly where it was');
    }
    // a shared link page reads no stash, and a page already on the version the site named still goes
    const s9 = mk(); arm(of5(), '?shared=' + ID);
    ok((await s9.ses.boot()) === true && sstore.has(KEY) && !s9.applied.length,
      'a ?shared= page returns before the stash, which stands for the next page');
    web(WEB); location.pathname = '/v9.9.9/';
    const r9 = mk(); await r9.ses.boot(); r9.ses.opened = { id: ID, name: 'One' };
    ok(!(await r9.ses.saveDraft({ name: 'One', doc: dsg('teal') })) && replaced.length === 1 && replaced[0] === '/v9.9.9/' && sstore.size === 1,
      'the site decides, even when this page is already on that version: it stashes and reloads', JSON.stringify(replaced));
    web(WEB);
    const q10 = mk(); await q10.ses.boot();
    ok(!(await q10.ses.saveDraft({ name: 'One', doc: dsg('teal') }, true)) && sstore.size === 1 && replaced.length === 1,
      'a quiet save (Send’s first step) is handed over the same way', q10.state.status);
    reset(); delete global.GorbHost; location.pathname = '/'; global.fetch = keepFetch;
    if (keepVer) window.__ORB_APP = keepVer; else delete window.__ORB_APP;
    global.sessionStorage = ssWas;
  }

  console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw: ' + (e && e.stack || e)); process.exit(1); });
