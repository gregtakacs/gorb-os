#!/usr/bin/env node
// runner-check.js — the migration runner, checked against an editor that is not one of ours.
//
//   node runner-check.js [--chrome <path>] [--only <n>]
//
// A scratch editor named tint (design schema 3, version v1.3.0) is built in a temp folder from
// one HTML page with holes in it, and `_runner/migrate.js` is run over it as a child process
// with `--runtime` at the source tree: the runner's contract is the shapes it reports, the
// sentences it prints and the exit codes it returns, and every check here is one of those.
// Deliberately not named headless-*.test.js, like editors-smoke: this needs Chromium. The
// variants are made by substituting the host's own methods, never by another page, because
// every claim below is about one editor and what the runner does to it.
// `make check-runner` runs it.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const HERE = __dirname;
const RUNTIME = path.join(HERE, '..');
const MIGRATE = path.join(RUNTIME, '_runner', 'migrate.js');
const arg = (n, f) => { const i = process.argv.indexOf('--' + n); return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : f; };

function chromePath() {
  const named = arg('chrome', process.env.CHROME || '');
  if (named) return fs.existsSync(named) ? named : null;
  for (const c of ['chromium', 'chromium-browser', 'google-chrome', 'google-chrome-stable']) {
    for (const dir of (process.env.PATH || '').split(path.delimiter)) {
      if (dir && fs.existsSync(path.join(dir, c))) return path.join(dir, c);
    }
  }
  return null;
}
const CHROME = chromePath();
if (!CHROME) {
  console.log('no Chromium: pass --chrome <path>, set CHROME, or put chromium, chromium-browser or google-chrome on PATH');
  process.exit(2);
}

let fails = 0, checked = 0;
const ok = (c, n, x) => {
  checked++;
  if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : ''));
  if (!c) fails++;
};

// PART 2
/* the scratch editor. One page, five holes: @MIGRATE@ the steps, @CONTENT@ and @APPLY@ the
 * host's own two halves of a design, @READY@ the start check, @BAKE@ the bake, and @STAMP@
 * the schema written into orb-app.js. The host keeps {color, size} in a plain object; step 1
 * adds size, step 2 renames colour. Variants fill the holes; the page never changes. */
const PAGE = `<!doctype html>
<html><head><meta charset="utf-8"><title>tint</title></head><body>
<div id="out"></div>
<script src="orb-app.js"></script>
<script src="/runtime/theme@1/session.js"></script>
<script>
var design = {color: 'red', size: 10}, status = '';
var api = OrbTheme.session({
  slug: 'tint', keys: function () { return ['color', 'size']; },
  state: function () { return {design: design, status: status}; },
  set: function () {}, say: function (s) { status = s; },
  restore: function (snap) { design = snap.design; },
  ready: function () { return @READY@; },
  schema: function () { return 3; },
  migrate: function () { return @MIGRATE@; },
  content: async function () { return @CONTENT@; },
  apply: function (doc) { return @APPLY@; },
  bake: async function () { return @BAKE@; }
});
api.boot().catch(function () {});
window.__status = function () { return status; };
</script>
</body></html>`;
const STEPS = "{ 1: function (o) { o.size = 10; return o; }, 2: function (o) { o.color = o.colour; delete o.colour; return o; } }";
const CONTENT = "{color: design.color, size: design.size}";
const APPLY = "typeof doc.color === 'string' ? (design = {color: doc.color, size: doc.size}, null) : 'that design names no color'";
const BAKE = "new TextEncoder().encode('BAKED:' + design.color)";
function editorDir(name, holes) {
  const dir = path.join(TMP, name);
  fs.mkdirSync(dir, { recursive: true });
  const h = Object.assign({ MIGRATE: STEPS, CONTENT, APPLY, READY: 'true', BAKE, STAMP: '3' }, holes);
  fs.writeFileSync(path.join(dir, 'index.html'), PAGE.replace(/@([A-Z]+)@/g, (m, k) => h[k]));
  fs.writeFileSync(path.join(dir, 'editor.json'),
    JSON.stringify({ editor: 1, entry: 'index.html', runtime: ['theme@1'], schema: +h.STAMP }));
  fs.writeFileSync(path.join(dir, 'orb-app.js'),
    'window.__ORB_APP = {"os_min":"0.1","schema":' + h.STAMP + ',"slug":"tint","version":"v1.3.0"};\n');
  return dir;
}
function design(dir, name, doc) {
  const f = path.join(dir, name);
  fs.writeFileSync(f, typeof doc === 'string' ? doc : JSON.stringify(doc));
  return f;
}
const D = (schema, version, extra) => Object.assign({ format: 'wc-design', schema, app: 'tint',
  appVersion: version, made: '2026-01-01T00:00:00.000Z' }, extra);

// every hub-route request any run of this suite made, gathered from the reports
const HUB_SEEN = [];

// the scratch editors, their design files, reports and bake folders all live here
const TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'orb-runner-check-'));

// one child run of the runner, as any caller runs it: its lines, its exit code, its report
function run(editor, args, json, timeout) {
  return new Promise((resolve) => {
    const rep = path.join(TMP, json + '.json');
    try { fs.rmSync(rep, { force: true }); } catch (e) { /* the first run */ }
    const all = [MIGRATE, '--runtime', RUNTIME, '--editor', editor, '--chrome', CHROME,
      '--timeout', String(timeout || 60), '--json', rep].concat(args);
    const t0 = Date.now();
    const child = spawn(process.execPath, all, { stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    child.stdout.on('data', (d) => { out += d; });
    child.stderr.on('data', (d) => { out += d; });
    child.on('exit', (code) => {
      let report = null;
      try { report = JSON.parse(fs.readFileSync(rep, 'utf8')); } catch (e) { /* none written */ }
      HUB_SEEN.push(...(report && report.requests || []).filter((q) => q.includes(' /api/')));
      resolve({ code, out, report, secs: (Date.now() - t0) / 1000 });
    });
  });
}
const named = (rep, name) => (rep && rep.results || []).find((r) => r.name === name) || null;
const why = (r) => (r && r.why) || '';

(async () => {
  const ONLY = arg('only', null);
  const check = async (n, f) => { if (ONLY && ONLY !== n) return; await f(); };

  // 1. the three schemas an editor with two steps reads all open, and the report says so
  await check('1', async () => {
    const ed = editorDir('c1', {});
    const a = design(ed, 'schema-1.orbdesign', D(1, 'v1.0.0', { colour: 'red' })),
      b = design(ed, 'schema-2.orbdesign', D(2, 'v1.1.0', { colour: 'red', size: 4 })),
      c = design(ed, 'schema-3.orbdesign', D(3, 'v1.3.0', { color: 'red', size: 10 }));
    const r = await run(ed, [a, b, c], 'c1');
    ok(r.code === 0, '1 exit 0', 'code ' + r.code + ' ' + r.out.slice(0, 160));
    ok(/migrated 3 of 3 designs/.test(r.out), '1 the count line', r.out.slice(0, 200));
    for (const [nm, from] of [['schema-1.orbdesign', 1], ['schema-2.orbdesign', 2], ['schema-3.orbdesign', 3]]) {
      const d = named(r.report, nm);
      ok(d && d.ok === true && d.from === from && d.to === 3 && d.stable === true,
        '1 ' + nm + ' ok from ' + from + ' to 3, stable', JSON.stringify(d));
    }
  });

  // 2. ruling 21: the schema alone decides, so a same-schema design from any version opens
  await check('2', async () => {
    const ed = editorDir('c2', {});
    const a = design(ed, 'future.orbdesign', D(3, 'v9.0.0', { color: 'blue', size: 2 }));
    const r = await run(ed, [a], 'c2');
    const d = named(r.report, 'future.orbdesign');
    ok(r.code === 0 && d && d.ok && d.stable, '2 a design from v9.0.0 opens', r.out.slice(0, 200));
  });

  // 3. a schema the editor has never heard of is refused, and only that design
  await check('3', async () => {
    const ed = editorDir('c3', {});
    const a = design(ed, 'good.orbdesign', D(3, 'v1.3.0', { color: 'red', size: 10 })),
      b = design(ed, 'schemafour.orbdesign', D(4, 'v2.0.0', { color: 'red', size: 10 }));
    const r = await run(ed, [a, b], 'c3');
    const bad = named(r.report, 'schemafour.orbdesign'), good = named(r.report, 'good.orbdesign');
    ok(r.code === 1, '3 exit 1', 'code ' + r.code);
    ok(bad && bad.ok === false && /reads schema 3/.test(why(bad)), '3 the reads-schema-3 sentence', why(bad));
    ok(good && good.ok === true, '3 the other design still ok', JSON.stringify(good));
  });

  // 4. a missing step is named as the gap it is; the step it does have still runs
  await check('4', async () => {
    const ed = editorDir('c4', { MIGRATE: '{ 2: function (o) { o.color = o.colour; delete o.colour; return o; } }' });
    const a = design(ed, 'schema-1.orbdesign', D(1, 'v1.0.0', { colour: 'red' })),
      b = design(ed, 'schema-2.orbdesign', D(2, 'v1.1.0', { colour: 'red', size: 4 }));
    const r = await run(ed, [a, b], 'c4');
    const s1 = named(r.report, 'schema-1.orbdesign'), s2 = named(r.report, 'schema-2.orbdesign');
    ok(s1 && /no step from schema 1 to 2/.test(why(s1)), '4 the no-step sentence names the gap', why(s1));
    ok(s2 && s2.ok === true, '4 the schema-2 design still opens', JSON.stringify(s2));
    ok(r.code === 1, '4 exit 1', 'code ' + r.code);
  });

  // 5. another app's design fails by name, and a file that is not JSON never reaches the page
  await check('5', async () => {
    const ed = editorDir('c5', {});
    const a = design(ed, 'clocks.orbdesign', D(3, 'v1.3.0', { app: 'clock', color: 'red', size: 10 })),
      b = design(ed, 'broken.orbdesign', '{"format": "wc-design", ');
    const r = await run(ed, [a, b], 'c5');
    ok(named(r.report, 'clocks.orbdesign') && /clock/.test(why(named(r.report, 'clocks.orbdesign'))),
      '5 the foreign design fails by name', why(named(r.report, 'clocks.orbdesign')));
    ok(named(r.report, 'broken.orbdesign') && /not JSON/.test(why(named(r.report, 'broken.orbdesign'))),
      '5 the broken file fails as not JSON', why(named(r.report, 'broken.orbdesign')));
    ok(r.code === 1 && /migrated 0 of 2 designs/.test(r.out), '5 exit 1 and the count line', r.out.slice(0, 200));
  });

  // 6. an editor whose save moves the document again is not stable, in the runner's own words
  await check('6', async () => {
    const ed = editorDir('c6', { CONTENT: "{color: design.color, size: design.size, n: window.__n = (window.__n || 0) + 1}" });
    const a = design(ed, 'moves.orbdesign', D(3, 'v1.3.0', { color: 'red', size: 10 }));
    const r = await run(ed, [a], 'c6');
    ok(r.code === 1, '6 exit 1', 'code ' + r.code);
    ok(/opened, but a second open and save changed it/.test(r.out), '6 the second-open sentence', r.out.slice(0, 200));
  });

  // 7. an editor that contradicts its own editor.json never starts a session, and says why
  await check('7', async () => {
    const ed = editorDir('c7', { STAMP: '2' });
    const a = design(ed, 'one.orbdesign', D(1, 'v1.0.0', { colour: 'red' })),
      b = design(ed, 'three.orbdesign', D(3, 'v1.3.0', { color: 'red', size: 10 }));
    const r = await run(ed, [a, b], 'c7');
    ok(r.code === 1, '7 exit 1', 'code ' + r.code);
    ok(/never started a theme@1 session/.test(r.out), '7 no session, named', r.out.slice(0, 240));
    ok(/reads design schema 3, but its editor.json says 2/.test(r.out), '7 and the start check says why', r.out.slice(0, 320));
  });

  // 8. --bake writes the editor's bytes, and an editor that bakes nothing fails by name
  await check('8', async () => {
    const ed = editorDir('c8', {});
    const a = design(ed, 'schema-1.orbdesign', D(1, 'v1.0.0', { colour: 'red' }));
    const out = path.join(TMP, 'c8-bake');
    const r = await run(ed, ['--bake', out, a], 'c8');
    let bytes = '';
    try { bytes = fs.readFileSync(path.join(out, 'schema-1.orbtheme'), 'utf8'); } catch (e) { /* none */ }
    ok(r.code === 0 && bytes === 'BAKED:red', '8 the bake is the editor’s own bytes', JSON.stringify(bytes) + ' ' + r.out.slice(0, 160));
    const ed2 = editorDir('c8b', { BAKE: 'Promise.resolve(null)' });
    const b = design(ed2, 'schema-1.orbdesign', D(1, 'v1.0.0', { colour: 'red' }));
    const r2 = await run(ed2, ['--bake', path.join(TMP, 'c8bb'), b], 'c8b');
    const d2 = named(r2.report, 'schema-1.orbdesign');
    ok(r2.code === 1 && d2 && /bake returned nothing/.test(why(d2)), '8 a bake of nothing is a failure', why(d2));
  });

  // 9. --save-current is the editor's own document, envelope and all
  await check('9', async () => {
    const ed = editorDir('c9', {});
    const f = path.join(TMP, 'c9.orbdesign');
    const r = await run(ed, ['--save-current', f], 'c9');
    let d = null;
    try { d = JSON.parse(fs.readFileSync(f, 'utf8')); } catch (e) { /* none */ }
    ok(r.code === 0 && d && d.format === 'wc-design' && d.schema === 3 && d.app === 'tint' && d.appVersion === 'v1.3.0',
      '9 the saved document', r.out.slice(0, 200) + ' ' + JSON.stringify(d).slice(0, 160));
  });

  // 10. an editor that never becomes ready costs the ready wait, not the wall clock
  await check('10', async () => {
    const ed = editorDir('c10', { READY: 'false' });
    const a = design(ed, 'wait.orbdesign', D(3, 'v1.3.0', { color: 'red', size: 10 }));
    const r = await run(ed, [a], 'c10');
    ok(r.code === 1 && /never became ready/.test(r.out), '10 never became ready', r.out.slice(0, 200));
    ok(r.secs < 60, '10 in under 60 s wall', r.secs.toFixed(1) + ' s');
  });

  // 11. and nothing in any of it asked a hub: the report's requests name every API call seen
  await check('11', async () => {
    ok(HUB_SEEN.length === 0, '11 no run reached /api/', HUB_SEEN.join(' '));
  });

  fs.rmSync(TMP, { recursive: true, force: true });
  console.log(fails ? fails + ' FAILED' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw: ' + ((e && e.stack) || e)); process.exit(1); });
