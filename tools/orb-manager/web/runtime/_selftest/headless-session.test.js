#!/usr/bin/env node
// theme@1/session (theme-editor-kit-plan K2): the OS's session, driven by a small made-up
// app ("tint": a colour and a size) rather than the clock, so nothing clock-shaped can hide in
// it. History with bursts, autosave and pick-up, per-app keys, My designs, the design
// envelope with its migrations and refusals, opening a design file, an .orbtheme's design and
// the hub's Edit door, and the design-file download.
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
};
global.location = { origin: 'http://hub', search: '' };
let hub = null;
global.fetch = async (url) => hub(url);
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
    compact: (snap) => Object.assign({}, snap, { heavy: undefined }),
    adopt: (b) => (b.color ? { color: b.color, size: b.size, name: b.name } : null),
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

  // ---- autosave and pick-up, per app
  {
    store.clear();
    const a = mk();
    a.ses.track(); edit(a, { color: 'green', name: 'Mine' });
    await wait(800);
    const saved = JSON.parse(store.get('wc.session.tint') || 'null');
    ok(saved && saved.v === 1 && saved.color === 'green' && saved.name === 'Mine' && a.ses.saved === true, 'autosave writes the snapshot under the app’s key', JSON.stringify(saved));
    ok(saved && !('heavy' in saved && saved.heavy !== undefined) && typeof saved.at === 'number', 'compacted by the app, stamped with when');
    const b = mk();
    ok(b.ses.load() && b.state.color === 'green' && /Picked up where you left off/.test(b.state.status), 'a new page picks it up', b.state.status);
    ok(!b.ses.undoStack.length && b.ses.snap === null, 'and its history starts there');
    store.set('wc.session.tint', JSON.stringify({ v: 1, at: 1 }));
    ok(!mk().ses.load(), 'a stored session the app does not accept is ignored');
    store.set('wc.session.tint', '{bad json');
    ok(!mk().ses.load(), 'and so is one that does not parse');
    location.search = '?app=clock';
    const other = mk();
    ok(other.ses.sessionKey() === 'wc.session.clock' && other.ses.mineKey() === 'wc.mine.clock', '?app= keys the session per app');
    location.search = '?app=../evil';
    ok(mk().ses.slug() === 'tint', 'and a slug that is not one falls back to the host’s');
    location.search = '';
    refuse = true;
    const r = mk(); r.ses.track(); edit(r, { size: 99 }); await wait(800);
    ok(r.ses.saved === false, 'a browser that refuses the write is recorded, not claimed');
    refuse = false;
  }

  // ---- My designs
  {
    store.clear();
    const a = mk();
    ok(a.ses.mineSave({ name: 'One', doc: { x: 1 } }) && /Saved “One”/.test(a.state.status), 'a design is saved by name', a.state.status);
    a.ses.mineSave({ name: 'Two', doc: { x: 2 } });
    a.ses.mineSave({ name: 'One', doc: { x: 3 } });
    const l = a.ses.mineList();
    ok(l.length === 2 && l[0].name === 'One' && l[0].doc.x === 3 && typeof l[0].at === 'number', 'a save under a held name replaces it, newest first', JSON.stringify(l.map((x) => x.name)));
    ok(mk().ses.mineList().length === 2, 'and another page of the same app sees them');
    a.ses.mineDelete('Two');
    ok(a.ses.mineList().length === 1 && /Removed “Two”/.test(a.state.status), 'a design is removed by name');
    refuse = true;
    ok(!a.ses.mineSave({ name: 'Three', doc: {} }) && /Not saved: this browser refused to store it \(QuotaExceededError\)/.test(a.state.status), 'a refused write says so', a.state.status);
    refuse = false;
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

  // ---- opening, and the design file
  {
    const a = mk(); a.set({ color: 'navy', name: 'Deep sea' });
    await a.ses.exportDoc('Deep sea/1');
    const dl = a.downloads[0];
    ok(dl && dl.name === 'Deep sea-1.orbdesign' && /Downloaded Deep sea-1\.orbdesign \(1 KB, 1 image\)/.test(a.state.status), 'the design file downloads under a safe name and says what it holds', a.state.status);
    const fileOf = async (bytes, name) => ({ name, arrayBuffer: async () => bytes.buffer });
    const enc = new TextEncoder();
    const body = new Uint8Array(await dl.blob.arrayBuffer());
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

  console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw: ' + (e && e.stack || e)); process.exit(1); });
