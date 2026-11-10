#!/usr/bin/env node
// migrate.js — the migration runner: designs in, opened headless through an editor's own Open,
// out again as that editor would save them.
//
//   node migrate.js --runtime <dir> --editor <dir> [--slug <slug>] [--chrome <path>]
//                   [--bake <out dir>] [--json <file>] [--timeout <seconds>] <design file>...
//   node migrate.js --runtime <dir> --editor <dir> [--slug <slug>] [--chrome <path>]
//                   --save-current <file>
//
// Who calls it (orb-os-net design §6.5 point 3): the site's migration check, which opens the
// current version of every stored design in the newest editor of that app before a new version
// goes live, and `orbsdk.py check-migrations`, which runs it over an app's `editor/_fixtures/`.
// One run holds one editor and a list of design files; it prints one line per design and exits 0
// only when every design opened, settled to the same document a second time through the same
// door, and — with `--bake` — baked.
//
// It reaches the editor through `window.GorbRunner` (theme@1/session.js's last line), so it
// drives the session exactly as a person's Open does — `api.apply` over `api.check` and the
// app's own `apply` — and never through app code: any editor that runs on theme@1 works here,
// clock, flights, weather or one from another repo. The page is served on 127.0.0.1 by this
// process, with a `GorbHost` of kind "web" and no library behind it, so nothing an editor asks
// the hub for ever leaves the loopback interface. The Chromium handling is editors-smoke's: one
// `--headless=new --dump-dom` load, a fresh profile, and the browser killed as soon as the probe
// says it is done rather than at the end of its virtual-time budget.
//
// Nothing outside `<out>` of `--bake`, `--json` and `--save-current` is written: never the
// editor folder, never the runtime, never a design file. The directory is named with a leading
// underscore, so the hub's `//go:embed web` leaves it out and nothing serves it.
'use strict';
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const TYPES = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.json': 'application/json; charset=utf-8',
  '.woff2': 'font/woff2', '.ttf': 'font/ttf', '.png': 'image/png', '.svg': 'image/svg+xml',
};
const typeOf = (f) => TYPES[path.extname(f).toLowerCase()] || 'application/octet-stream';
const USAGE = 'usage: migrate.js --runtime <dir> --editor <dir> [--slug <slug>] [--chrome <path>]'
  + ' [--bake <dir>] [--json <file>] [--timeout <s>] <design file>...\n'
  + '  or: migrate.js --runtime <dir> --editor <dir> [--slug <slug>] [--chrome <path>] --save-current <file>';
const OPTS = { runtime: 1, editor: 1, slug: 1, chrome: 1, bake: 1, json: 1, timeout: 1, 'save-current': 1 };
const VERBOSE = !!process.env.VERBOSE;

function usage(why) {
  if (why) console.log(why);
  console.log(USAGE);
  process.exit(2);
}

const opt = {}, files = [];
for (let i = 2; i < process.argv.length; i++) {
  const a = process.argv[i];
  if (a === '--') { files.push.apply(files, process.argv.slice(i + 1)); break; }
  if (a.startsWith('--')) {
    const key = a.slice(2);
    if (!(key in OPTS)) usage('unknown option --' + key);
    opt[key] = process.argv[++i];
    continue;
  }
  files.push(a);
}
if (!opt.runtime || !opt.editor) usage('both --runtime and --editor are needed');
const RUNTIME = path.resolve(opt.runtime), EDITOR = path.resolve(opt.editor);
const SAVE = opt['save-current'] ? path.resolve(opt['save-current']) : null;
const BAKE = opt.bake ? path.resolve(opt.bake) : null;
const REPORT = opt.json ? path.resolve(opt.json) : null;
const TIMEOUT = opt.timeout !== undefined ? Number(opt.timeout) : 600;
if (!(TIMEOUT > 0)) usage('--timeout wants a number of seconds');

// the editor: one unpacked region — an `extract-editors` <out>/<slug>/ folder, or an unzipped
// editor — whose editor.json names the entry document, the runtime and the design schema.
const manifest = (() => {
  try { return JSON.parse(fs.readFileSync(path.join(EDITOR, 'editor.json'), 'utf8')); }
  catch (e) { usage('no editor.json to read in ' + EDITOR + ': ' + (e.message || e)); }
})();
// host@1: a hosted editor names no entry — Orb Manager synthesizes its page, index.html
// (extract.go's hostedEntryName, the same document the hub opens a bare /editor/<ip>/<slug>/
// with and editors-smoke reads as row.editor.entry). Follow that: the manifest's entry when it
// names one, else the hub's hosted page when the editor is hosted, else there is nothing to open.
let ENTRY = typeof manifest.entry === 'string' && manifest.entry ? manifest.entry : null;
if (!ENTRY && manifest.host) ENTRY = 'index.html';
if (!ENTRY) usage('editor.json in ' + EDITOR + ' names no entry');
const SCHEMA = Number.isInteger(manifest.schema) && manifest.schema >= 1 ? manifest.schema : null;

// the slug the page is served under: --slug, else the stamp the region build wrote, else nothing
// to build a URL out of. The stamp is one line the build adds, so it is read as text.
function stampOf(dir) {
  let text = null;
  try { text = fs.readFileSync(path.join(dir, 'orb-app.js'), 'utf8'); } catch (e) { return null; }
  const m = /window\.__ORB_APP\s*=\s*(\{[\s\S]*?\})\s*;/.exec(text);
  if (!m) return null;
  try { return JSON.parse(m[1]); } catch (e) { return null; }
}
const stamp = stampOf(EDITOR);
const SLUG = opt.slug || (stamp && stamp.slug);
if (!SLUG) usage('no slug: the editor has no orb-app.js; pass --slug');
if (!/^[a-z0-9_-]{1,32}$/.test(SLUG)) usage('not a slug: ' + SLUG);
const BASE = '/studio/' + SLUG + '/';

// the hosted entry page. A hosted editor (editor.json "host": 1) names no entry: Orb Manager
// serves its own, host@1/page.html filled at extract time (editor.go's hostedEditor and
// hostedTmpl). A packed region has no such file, so for a run against one this fills THE SAME
// template from this runtime, so the page can never drift from the hub's. The template's only
// actions are {{.Title}}, {{range .Extras}} and {{range .Modules}}; anything else is refused.
const HOSTED = !manifest.entry && !!manifest.host;
// editor.go's hostedExtras: the runtime ids a hosted page loads only when the app lists them
const HOSTED_EXTRAS = [['maps@1', '/runtime/maps@1/orbmap.js']];
function hostedPageHtml() {
  const tmpl = readTree(RUNTIME, 'host@1/page.html');
  if (!tmpl) usage('the runtime has no host@1/page.html to serve the hosted editor with');
  const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
    .replace(/>/g, '&gt;').replace(/"/g, '&#34;').replace(/'/g, '&#39;');
  const listed = {};
  (manifest.runtime || []).forEach((id) => { listed[id] = true; });
  const data = {
    Title: SLUG + ' theme editor',
    Extras: HOSTED_EXTRAS.filter((x) => listed[x[0]]).map((x) => x[1]),
    Modules: (manifest.module || []).filter((f) =>
      f && !/[\/\\]/.test(f) && !f.startsWith('.') && /\.js$/i.test(f) && readTree(EDITOR, f)),
  };
  let out = tmpl.toString('utf8')
    .replace(/\{\{range \.(Extras|Modules)\}\}([\s\S]*?)\{\{end\}\}/g,
      (_, k, body) => data[k].map((v) => body.replace(/\{\{\.\}\}/g, esc(v))).join(''))
    .replace(/\{\{\.Title\}\}/g, esc(data.Title));
  if (/\{\{/.test(out)) usage('host@1/page.html uses a template action this runner does not fill');
  return out;
}

// the browser: --chrome, then $CHROME, then the names a Linux box answers to — editors-smoke's
// search with the Debian stable name added.
function chromePath() {
  const named = opt.chrome || process.env.CHROME || '';
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

// The designs, in the order given. A file that is not JSON never reaches the page — the probe
// would only have to be told to fail it, and the answer is known here — so it is a FAIL for
// that one file and the run goes on with the rest.
const designs = [];
for (const f of files) {
  const name = path.basename(f);
  let text = null, why = null;
  try { text = fs.readFileSync(f, 'utf8'); } catch (e) { why = 'cannot be read: ' + (e.message || e); }
  if (why === null) { try { text = JSON.parse(text); } catch (e) { why = 'not JSON: ' + (e.message || e); } }
  designs.push(why === null ? { file: f, name, doc: text } : { file: f, name, err: why });
}
if (!SAVE && !designs.length) usage('no design files, and no --save-current');
if (SAVE && designs.length) usage('--save-current takes the editor as it stands, so name no design file');

/* The probe, injected right after the entry document's <head>. ES5, because it is written into a
 * page as text and must run before anything else on it has decided what it is. It records what
 * went wrong, takes the session the editor hands it, and works through the designs one at a
 * time, posting each result as soon as it is known so a killed browser keeps what finished. */
const HOST_INFO = JSON.stringify({ kind: 'web', api: '/studio-api', send: false, publish: false, inbox: false });
const PROBE_BASE = `(function(){
var errs=[],pair=null,started=false;
function p(m){if(errs.length<40)errs.push(String(m));}
window.addEventListener('error',function(ev){var t=ev.target;var src=(t&&(t.src||t.href))||'';
if(src.indexOf('{{')>=0||src.indexOf('%7B%7B')>=0)return;
p(ev&&ev.message?'uncaught: '+ev.message:'resource failed: '+src);},true);
window.addEventListener('unhandledrejection',function(ev){p('rejected: '+((ev&&ev.reason&&(ev.reason.message||ev.reason))||'?'));});
function post(u,b,t){return fetch(u,{method:'POST',headers:{'content-type':t||'application/json'},body:b});}
function ask(u){return fetch(u).then(function(r){return r.json();});}
function msg(e){return (e&&e.message)?e.message:String(e);}
function refuse(s){return {refuse:s};}
function clone(o){return JSON.parse(JSON.stringify(o));}
function noMade(d){var c=clone(d);delete c.made;return c;}
/* the document as its author would compare it: keys in order at every depth, so two objects
 * that hold the same fields are one string however the editor happened to build them */
function canon(v){
if(v===null||typeof v!=='object')return v===undefined?'null':JSON.stringify(v);
if(v instanceof Array){var a=[];for(var i=0;i<v.length;i++)a.push(canon(v[i]));return '['+a.join(',')+']';}
var k=Object.keys(v).sort(),o=[];
for(var j=0;j<k.length;j++){if(v[k[j]]!==undefined)o.push(JSON.stringify(k[j])+':'+canon(v[k[j]]));}
return '{'+o.join(',')+'}';
}
/* a design applied with host.apply reaches api.doc only once the editor has rendered it: the two
 * frames are that render, the timer the scheduler's own quiet after it (flights and weather keep
 * their design in React state behind a ref a render refreshes). Headless --dump-dom paints no
 * frames for a page with no animation, so the two-frame wait carries a watchdog of its own: a
 * page that renders nothing still settles, a page that renders just waited the same two frames. */
function settle(){return new Promise(function(r){
var done=false;function fin(){if(done)return;done=true;setTimeout(r,200);}
requestAnimationFrame(function(){requestAnimationFrame(fin);});setTimeout(fin,800);});}
function ready(){
return new Promise(function(res){
var n=0;(function step(){
var y=false;try{y=!!(pair.host.ready&&pair.host.ready());}catch(e){y=false;}
if(y)return res(true);
if(++n>=300)return res(false);
setTimeout(step,100);
})();});
}
function finish(){
var h=pair&&pair.host,a=pair&&pair.api,s=null,v=null,g=null;
try{if(h&&h.schema)s=h.schema();}catch(e){}
try{if(a&&a.version)v=a.version();}catch(e){}
try{if(a&&a.slug)g=a.slug();}catch(e){}
return post('/__runner/done',JSON.stringify({errors:errs,session:started,schema:s,version:v,slug:g}));
}
function one(d,i,api,host,bake){
var out={i:i,name:d.name,ok:true,why:null,from:null,to:null,appVersion:null,stable:false,bake:null};
return Promise.resolve().then(function(){
var why=api.apply(clone(d.doc),'the migration runner');
if(typeof why==='string')throw refuse(why);
return settle().then(function(){return api.doc(true);}).then(function(d1){
out.from=(d.doc&&d.doc.schema!==undefined)?d.doc.schema:null;
out.to=(d1&&d1.schema!==undefined)?d1.schema:null;
out.appVersion=(d1&&d1.appVersion!==undefined)?d1.appVersion:null;
var why2=api.apply(clone(d1),'the migration runner');
if(typeof why2==='string')throw refuse('it opened, but its own save does not reopen: '+why2);
return settle().then(function(){return api.doc(true);}).then(function(d2){
out.stable=canon(noMade(d1))===canon(noMade(d2));
if(!bake)return;
if(typeof host.bake!=='function'){out.bake='this editor cannot bake';return;}
return Promise.resolve().then(function(){return host.bake();}).then(function(b){
if(b instanceof ArrayBuffer)b=new Uint8Array(b);
if(!(b instanceof Uint8Array)){out.bake='the editor’s bake returned nothing';return;}
out.bake=b.length;
return post('/__runner/bake?i='+i,b,'application/octet-stream');
},function(e){out.bake=msg(e);});
 });
 });
 }).then(null,function(e){out.ok=false;out.why=(e&&e.refuse)?e.refuse:('threw: '+msg(e));})
 .then(function(){return post('/__runner/result',JSON.stringify(out));});
}
function each(list,i,api,host,bake){
if(i>=list.length)return Promise.resolve();
if(list[i].skip)return each(list,i+1,api,host,bake);
return one(list[i],i,api,host,bake).catch(function(){}).then(function(){return each(list,i+1,api,host,bake);});
}
function failAll(why){
return ask('/__runner/designs').then(function(list){
var c=Promise.resolve();
(list||[]).forEach(function(d,i){if(d.skip)return;c=c.then(function(){return post('/__runner/result',JSON.stringify({i:i,name:d.name,ok:false,why:why}));});});
return c;
});
}
function run(){
var api=pair.api,host=pair.host;
ask('/__runner/designs').then(function(list){
return ready().then(function(y){
if(y===false)return failAll('the editor never became ready');
if(__SAVE__)return api.doc(true).then(function(d){return post('/__runner/save',JSON.stringify(d));},
function(e){return post('/__runner/save',JSON.stringify({save:false,why:'threw: '+msg(e)}));});
return each(list||[],0,api,host,__BAKE__);
});
},function(e){p('the runner answered nothing: '+msg(e));}).then(finish);
}
window.GorbRunner=function(api,host){pair={api:api,host:host};if(started)return;started=true;setTimeout(run,0);};
/* no session at all: post done at the same 30 s of page time the ready wait costs, so a page that
 * never built one is an answer rather than a wall-clock timeout for the whole run */
setTimeout(function(){if(!started)finish();},30000);
})();`;
const PROBE = PROBE_BASE.replace('__SAVE__', SAVE ? 'true' : 'false').replace('__BAKE__', BAKE ? 'true' : 'false');

// what the page posts back, and what the run saw: one object, because there is one page.
const state = { requests: [], apis: [], results: {}, done: null, saved: null,
  bakeErr: null, killed: '', kill: null };

const readTree = (root, rel) => {
  if (!rel || rel.split('/').some((s) => s === '' || s === '.' || s === '..')) return null;
  const f = path.join(root, rel);
  if (!f.startsWith(root + path.sep)) return null;
  try { return fs.statSync(f).isFile() ? fs.readFileSync(f) : null; } catch (e) { return null; }
};
const send = (res, status, type, body) => { res.writeHead(status, { 'content-type': type, 'cache-control': 'no-store' }); res.end(body); };
const json = (res, status, v) => send(res, status, 'application/json; charset=utf-8', JSON.stringify(v));
const withBody = (req, cb) => { let t = ''; req.on('data', (c) => { t += c; }); req.on('end', () => cb(t)); };
const withRaw = (req, cb) => { const cs = []; req.on('data', (c) => { cs.push(c); }); req.on('end', () => cb(Buffer.concat(cs))); };

// an export-runtime tree has one folder per id; the source tree keeps the vendored libraries
// under vendor/ and the hub's catalog maps the ids onto them, so both are answered from here.
function runtimeFile(rel) {
  const at = rel.indexOf('/');
  if (at < 0) return null;
  return readTree(RUNTIME, rel) || readTree(path.join(RUNTIME, 'vendor'), rel.slice(at + 1));
}
// the hub opens a bare editor URL — and its editor.json, which is the same document to a
// browser — with the entry document, not the manifest; so the smoke suite, so here.
function entryOf(res) {
  // a packed hosted region ships no page (extract-editors writes it): serve the same one the hub
  // would, from the manifest's runtime and module, so a run against an unzipped region opens.
  let doc = readTree(EDITOR, ENTRY);
  if (!doc && HOSTED) doc = Buffer.from(hostedPageHtml(), 'utf8');
  if (!doc) return send(res, 404, 'text/plain; charset=utf-8', 'not found');
  const text = doc.toString('utf8');
  const scripts = '<script>window.GorbHost = ' + HOST_INFO + ';</script><script>' + PROBE + '</script>';
  const m = /<head[^>]*>/i.exec(text);
  return send(res, 200, 'text/html; charset=utf-8', m ? text.slice(0, m.index + m[0].length) + scripts + text.slice(m.index + m[0].length) : scripts + text);
}
const bakeNameOf = (d) => path.basename(d.file).replace(/\.[^.]*$/, '') + '.orbtheme';

const server = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://127.0.0.1'), p = u.pathname;
  state.requests.push(req.method + ' ' + req.url);
  if (VERBOSE) console.log('  ' + req.method + ' ' + req.url);
  if (p.startsWith('/api/') || p.startsWith('/studio-api/')) state.apis.push(req.method + ' ' + p);
  if (req.method === 'POST' && p === '/__runner/result') {
    return withBody(req, (t) => {
      try { const r = JSON.parse(t); if (r && Number.isInteger(r.i)) state.results[r.i] = r; } catch (e) { /* a result that cannot be read is no result */ }
      json(res, 204, { ok: true });
    });
  }
  if (req.method === 'POST' && p === '/__runner/bake') {
    const i = Number(u.searchParams.get('i')), d = designs[i];
    return withRaw(req, (b) => {
      if (!d || !BAKE) return json(res, 400, { ok: false, error: 'no bake asked for' });
      try {
        fs.mkdirSync(BAKE, { recursive: true });
        fs.writeFileSync(path.join(BAKE, bakeNameOf(d)), b);
        json(res, 200, { ok: true, bytes: b.length });
      } catch (e) { state.bakeErr = e.message || String(e); json(res, 500, { ok: false, error: state.bakeErr }); }
    });
  }
  if (req.method === 'POST' && p === '/__runner/save') {
    return withBody(req, (t) => {
      try { state.saved = JSON.parse(t); } catch (e) { state.saved = { save: false, why: 'the editor posted nothing: ' + t.slice(0, 80) }; }
      json(res, 204, { ok: true });
    });
  }
  if (req.method === 'POST' && p === '/__runner/done') {
    return withBody(req, (t) => {
      try { state.done = JSON.parse(t) || {}; } catch (e) { state.done = { errors: [] }; }
      json(res, 204, { ok: true });
      // the page is finished: the browser would otherwise sit out its hour of virtual time
      state.killed = 'done';
      if (state.kill) state.kill();
    });
  }
  if (req.method === 'GET' && p === '/__runner/designs') {
    return json(res, 200, SAVE ? { save: true } : designs.map((d) => (d.err ? { name: d.name, skip: true } : { name: d.name, doc: d.doc })));
  }
  if (p.startsWith('/runtime/') && req.method === 'GET') {
    const f = runtimeFile(p.slice('/runtime/'.length));
    return f ? send(res, 200, typeOf(p), f) : send(res, 404, 'text/plain; charset=utf-8', 'not found');
  }
  if (p === BASE || p.startsWith(BASE)) {
    const file = p.slice(BASE.length);
    if (req.method !== 'GET') return json(res, 405, { ok: false, error: 'the runner serves' });
    if (file === '' || file === 'editor.json') return entryOf(res);
    const f = readTree(EDITOR, file);
    return f ? send(res, 200, typeOf(file), f) : send(res, 404, 'text/plain; charset=utf-8', 'not found');
  }
  if (p === '/studio-api/library/entries' || p === '/api/library/entries') {
    // an empty library is the truth: designs are on the command line here, not on a hub
    return withBody(req, () => json(res, 200, { ok: true, data: [] }));
  }
  // recorded, because an editor that asks a hub question in a run is worth naming, but never a
  // failure: the run is about the design, and there is nothing behind these routes to fail at.
  if (p.startsWith('/studio-api/') || p.startsWith('/api/')) {
    return withBody(req, () => json(res, 404, { ok: false, error: 'the migration runner has no library' }));
  }
  return withBody(req, () => send(res, 404, 'text/plain; charset=utf-8', 'not found'));
});

// the browser: one load, killed early by the /__runner/done handler above
function openChrome(url) {
  return new Promise((resolve) => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'orb-runner-'));
    const args = ['--headless=new', '--no-sandbox', '--disable-gpu', '--no-first-run',
      '--user-data-dir=' + dir, '--virtual-time-budget=3600000', '--dump-dom', url];
    const child = spawn(CHROME, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let dom = '', err = '';
    child.stdout.on('data', (d) => { dom += d; });
    child.stderr.on('data', (d) => { err += d; });
    state.kill = () => { try { child.kill('SIGKILL'); } catch (e) { /* gone */ } };
    const timer = setTimeout(() => { state.killed = 'timeout'; state.kill(); }, TIMEOUT * 1000);
    child.on('exit', (code) => {
      clearTimeout(timer);
      state.kill = null;
      try { child.stdout.destroy(); child.stderr.destroy(); } catch (e) { /* already gone */ }
      fs.rmSync(dir, { recursive: true, force: true });
      // Chromium's complaints about a box with no dbus and no GPU are not the editor's
      const bad = err.split('\n').filter((l) => l && !/ERROR:(dbus|gpu|vaapi|bluez|field_trial|viz_main|angle|media)/
        .test(l)).join(' ').slice(0, 300);
      resolve({ code, dom, err: bad });
    });
  });
}

/* one design's line, and whether it passed. A design with no result — the browser died before
 * the page got to it — fails as itself, with what the run knows about why. */
const errors0 = () => (state.done && state.done.errors) || [];
function verdict(d) {
  const v = { file: d.file, name: d.name, ok: false, why: null, from: null, to: null,
    appVersion: null, stable: false, bake: null };
  if (d.err) { v.why = d.err; return v; }
  const r = state.results[designs.indexOf(d)];
  if (!r) {
    const noSession = state.done && state.done.session === false;
    v.why = noSession
      ? 'the editor never started a theme@1 session' + (errors0().length ? ': ' + errors0().join(' | ') : '')
      : 'no answer from the editor before the timeout';
    return v;
  }
  v.from = r.from; v.to = r.to; v.appVersion = r.appVersion; v.stable = !!r.stable; v.bake = r.bake;
  if (!r.ok) { v.why = r.why; return v; }
  if (!r.stable) { v.why = 'opened, but a second open and save changed it'; return v; }
  if (SCHEMA !== null && r.to !== null && r.to !== SCHEMA) {
    v.why = 'it reached schema ' + r.to + ', which is not this editor’s ' + SCHEMA; return v;
  }
  if (BAKE && typeof r.bake !== 'number') { v.why = 'no bake: ' + r.bake; return v; }
  v.ok = true; return v;
}

async function main() {
  await new Promise((res, rej) => { server.on('error', rej); server.listen(0, '127.0.0.1', res); });
  const url = 'http://127.0.0.1:' + server.address().port + BASE;
  const page = await openChrome(url);
  server.close();
  const editorVersion = (state.done && state.done.version) || (stamp && stamp.version) || null;
  const errors = (state.done && state.done.errors) || [];
  let exit = 0;
  if (SAVE) {
    const s = state.saved;
    if (!s) { console.log('the editor never posted a document' + (page.err ? ': ' + page.err : '')); exit = 1; }
    else if (s.save === false) { console.log('the editor could not give up its document: ' + s.why); exit = 1; }
    else {
      fs.mkdirSync(path.dirname(SAVE), { recursive: true });
      fs.writeFileSync(SAVE, JSON.stringify(s, null, 1) + '\n');
      console.log('saved ' + ((state.done && state.done.slug) || SLUG) + ' schema '
        + (s.schema === undefined ? '?' : s.schema) + ' to ' + SAVE);
    }
  } else {
    let k = 0;
    for (const d of designs) {
      const v = verdict(d);
      if (!v.ok) { console.log('FAIL ' + v.name + ': ' + v.why); exit = 1; continue; }
      k++;
      console.log('ok   ' + v.name + ': schema ' + v.from + ' -> ' + v.to + ', saved as ' + v.appVersion + ', stable'
        + (BAKE ? ', baked ' + v.bake + ' B' : ''));
    }
    console.log('migrated ' + k + ' of ' + designs.length + ' designs');
    if (BAKE && state.bakeErr) { console.log('FAIL bake: the baked bytes could not be written: ' + state.bakeErr); exit = 1; }
  }
  if (errors.length) console.log('note: the page said: ' + errors.join(' | '));
  if (REPORT) {
    fs.writeFileSync(REPORT, JSON.stringify({
      format: 1,
      editor: { slug: SLUG, version: editorVersion, schema: SCHEMA },
      results: designs.map(verdict),
      pageErrors: errors,
      requests: state.apis,
    }, null, 1) + '\n');
  }
  process.exit(exit);
}

main().catch((e) => { console.log('the runner itself failed: ' + (e && e.stack || e)); process.exit(2); });
