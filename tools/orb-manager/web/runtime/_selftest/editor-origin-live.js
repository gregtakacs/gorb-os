#!/usr/bin/env node
// editor-origin-live.js — the S2 live gate: every editor flow, driven in a real browser,
// with the split ports standing and nothing mocked.
//
//   node editor-origin-live.js [--chrome <path>] [--keep] [--json [file]] [--only <check>] [--timeout <s>]
//
// S2 moved the editors off the control port and onto their own: an editor document now
// lives at <app>.localhost:<editorPort>, behind a token minted per launch, behind a route
// allowlist, behind a CSP that lets it reach nothing else. Unit suites prove each wall on
// its own (editororigin_test.go pokes the guard with hand-built requests); what no Go
// test can prove is that the *product* still works with every browser rule in force —
// that app.js can still frame an editor across the origin boundary, that the editor's own
// fetches still carry the shim's credential, that no cookie or violation rides along, and
// that a third-party editor which *tries* to reach the control doors comes back with
// nothing. That is this file. It builds the hub and the bench fake orb, seeds a stale
// theme onto the shelf, and drives headless Chromium over CDP through the four live flows
// (edit on the app's own origin, Rebake all, Send to orb, Import from a site) plus a
// hostile third-party editor, then reports eight checks:
//
//   edit-on-own-origin  the clock editor opens inside the Themes pane on clock.localhost
//                       (not the control origin), boots its session, takes an edit through
//                       its own apply door, and the hub files the draft the pane's Save
//                       button pressed changed.
//   rebake-all          the Themes tab shows "Rebake all (1)" for the seeded stale theme,
//                       the press opens the editor with ?rebake=1, and the library entry
//                       comes back holding a fresh baked theme under the same name.
//   send-to-orb         the editor's own Send to orb press puts the bake on the fake orb's
//                       card listing by name.
//   import-from-site    a link pasted into the Themes tab's import door is fetched from a
//                       local stand-in site, baked in the hub's own copy of the editor on
//                       the app's origin, and lands in the library with its shared name.
//   no-cookie-on-editor no request that reached the editor port carried a Cookie header,
//                       and no editor document sees one.
//   no-csp-violation    no CSP violation was logged during any of the four flows.
//   hostile-refused     a third-party app's editor page, served on its own origin, aims
//                       30+ probes at privileged doors — every one refused, most by the
//                       editor port's own route table, the rest by CSP.
//   hostile-did-nothing none of those probes moved a theme, an entry, a card row or a
//                       fleet member: the before-and-after snapshots are equal.
//
// It is deliberately NOT part of `make check` (it needs Go, Chromium and minutes — like
// check-web); `make check-editor-origin` runs it. Exits 0 only on 8/8.
'use strict';
const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
const zlib = require('zlib');
const crypto = require('crypto');
const { spawn } = require('child_process');

const REPO = path.resolve(__dirname, '..', '..', '..', '..', '..');
const SHELF_THEME = path.join(REPO, 'tools', 'orb-manager', 'themes', 'skeleton.orbtheme');
const SHELF_CLOCK = path.join(REPO, 'tools', 'orb-manager', 'apps', 'clock.orbapp');
const GO_ROOT = path.join(REPO, 'tools', 'orb-manager');   // go.mod lives in the hub's own directory

function arg(name, fallback) {
  const i = process.argv.indexOf('--' + name);
  return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback;
}
const FLAGS = {
  chrome: arg('chrome', process.env.CHROME || ''),
  keep: process.argv.includes('--keep'),
  json: process.argv.includes('--json'),
  // --json takes an optional file: the report goes to stdout either way, and to that
  // file as well when one is named, so a harness can require() it after the run.
  jsonPath: (function () {
    const i = process.argv.indexOf('--json');
    const v = i > 0 ? process.argv[i + 1] : '';
    return v && v.slice(0, 2) !== '--' ? v : '';
  })(),
  only: arg('only', ''),
  timeout: Number(arg('timeout', '300')),
};

// ---------- small plumbing the run needs ----------

function freePort() {
  return new Promise((res, rej) => {
    const srv = net.createServer();
    srv.listen(0, '127.0.0.1', () => {
      const p = srv.address().port;
      srv.close(() => res(p));
    });
    srv.on('error', rej);
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function waitFor(name, ms, fn) {
  const t0 = Date.now();
  for (;;) {
    let v = null, err = null;
    try { v = await fn(); } catch (e) { err = e; }
    if (v) return v;
    if (Date.now() - t0 > ms) throw new Error('waited ' + (ms / 1000) + 's for ' + name + (err ? ': ' + err.message : ''));
    await sleep(150);
  }
}

async function httpJSON(url, opts, ms) {
  const r = await fetch(url, Object.assign({ signal: AbortSignal.timeout(ms || 15000) }, opts));
  const body = await r.text();
  let j = null;
  try { j = JSON.parse(body); } catch (e) { /* callers of the raw variants decide */ }
  if (!j) throw new Error(url + ' answered ' + r.status + ': ' + body.slice(0, 160));
  return { status: r.status, env: j };
}

// spawnLogged starts a child with its output appended to a log in <dir>, and returns
// the process plus its kill switch. Every child is its own process group so teardown
// takes the whole tree, not just the shell's first child.
function spawnLogged(cmd, args, opts) {
  const out = fs.openSync(opts.log, 'a');
  const p = spawn(cmd, args, {
    cwd: opts.cwd || REPO, env: opts.env || process.env,
    stdio: ['ignore', out, out], detached: true,
  });
  fs.closeSync(out);
  return {
    proc: p,
    kill() { try { process.kill(-p.pid, 'SIGKILL'); } catch (e) { /* already reaped */ } },
  };
}

// ---------- the stale theme seed ----------
// designs_test.go forges its stale shelf themes with withABI(withDesign(...)) — a real
// container with the geometry-ABI word restamped out of the set the firmware reads (3
// for apps, 6 for a clock) and the header checksum recomputed over the 128 bytes with
// word 28 zeroed. The same surgery here, in Node, on the shipped Skeleton default: the
// seed is a Skeleton bake the current orb would refuse, carrying a design renamed to
// "S2 rebake", so the Themes tab must show it stale, Rebake all must open its design,
// and the keep must replace it in place. The file lands in the run's theme library
// before the hub starts, exactly like a theme that has been sitting on a shelf since
// the last firmware moved.

const HDR_WORDS = 32, W_ABI = 2, W_NAME = 14, NAME_MAX = 56, W_HDR_CRC = 28,
  W_DESIGN_OFF = 29, W_DESIGN_SIZE = 30, W_DESIGN_CRC = 31, SECTOR = 4096;

function restampStale(srcPath, outPath, newName, staleABI) {
  const b = fs.readFileSync(srcPath);
  const w = (i) => b.readUInt32LE(i * 4);
  if (w(0) !== 0x474f5254 || w(1) !== 3) throw new Error('seed: skeleton.orbtheme is not a FORMAT 3 container');
  if (w(W_DESIGN_SIZE) === 0) throw new Error('seed: skeleton.orbtheme carries no design to rename');
  const design = JSON.parse(b.subarray(w(W_DESIGN_OFF), w(W_DESIGN_OFF) + w(W_DESIGN_SIZE)).toString('utf8'));
  const nameBuf = Buffer.from(newName, 'utf8');
  if (nameBuf.length > NAME_MAX - 1) throw new Error('seed: name over the header field');
  design.themeName = newName;
  // Append past the old design rather than rewriting it, so the header, the banks
  // and the art — every byte with a checksum — stay byte-identical, and the file
  // goes one design longer, the way a re-kept theme would.
  const docBuf = Buffer.from(JSON.stringify(design), 'utf8');
  const out = Buffer.concat([b, docBuf]);
  const hdr = out.subarray(0, HDR_WORDS * 4);
  hdr.fill(0, W_NAME * 4, W_NAME * 4 + NAME_MAX);
  nameBuf.copy(hdr, W_NAME * 4);
  hdr.writeUInt32LE(staleABI, W_ABI * 4);
  hdr.writeUInt32LE(b.length, W_DESIGN_OFF * 4);
  hdr.writeUInt32LE(docBuf.length, W_DESIGN_SIZE * 4);
  hdr.writeUInt32LE(zlib.crc32(docBuf) >>> 0, W_DESIGN_CRC * 4);
  hdr.writeUInt32LE(0, W_HDR_CRC * 4);
  hdr.writeUInt32LE(zlib.crc32(hdr) >>> 0, W_HDR_CRC * 4);
  fs.writeFileSync(outPath, out);
  return { bytes: out.length, geomSize: w(5), artSize: w(7) };
}

// ---------- the hostile app ----------
// A third-party app as an orb would ship it: one .orbapp whose editor region holds an
// index.html and one plain script. No editor.json, so the hub serves the document as
// the app wrote it (with the shim first in its head, like any editor document). The
// page probes the doors a captured editor would want: the privileged control verbs
// that do not exist on the editor port at all, the library and send doors under scopes
// that are not its own, another app's editor origin, the control port itself through
// fetch and through an <img> (both CSP's business, not the network's), and a card
// proxy path off the three-read allowlist. It never reads anything privileged and never
// mutates anything — by construction it cannot — and it records every attempt.

const HOSTILE_JS = `(function () {
  var R = [];
  function rec(name, where, status, extra) {
    var row = { name: name, where: where, status: status };
    if (extra) row.error = String(extra).slice(0, 120);
    R.push(row);
  }
  function probe(name, where, init) {
    return fetch(location.origin + init.path, Object.assign({ redirect: 'manual' }, init))
      .then(function (r) { rec(name, where, r.status); },
            function (e) { rec(name, where, 0, e && e.name + ': ' + e.message); });
  }
  function probeAbs(name, url, init) {
    return fetch(url, Object.assign({ redirect: 'manual' }, init))
      .then(function (r) { rec(name, 'csp-or-network', r.status ? r.status : 0); },
            function (e) { rec(name, 'csp-blocked', 0, e && e.name + ': ' + e.message); });
  }
  var body = { headers: { 'Content-Type': 'application/json' } };
  var junk = { headers: { 'Content-Type': 'application/octet-stream' }, body: new Uint8Array(256) };
  var seq = Promise.resolve();
  function go(name, where, init) { seq = seq.then(function () { return probe(name, where, init); }); }
  function goAbs(name, url, init) { seq = seq.then(function () { return probeAbs(name, url, init); }); }
  function imgProbe(name, url) {
    seq = seq.then(function () {
      return new Promise(function (done) {
        var im = document.createElement('img');
        im.onload = function () { rec(name, 'csp-or-network', -1); done(); };
        im.onerror = function () { rec(name, 'csp-blocked', 0, 'img blocked'); done(); };
        im.src = url;
      });
    });
  }
  // The doors that exist only on the control port: on this listener they are not even
  // routes. The guard's own 404/405 is the answer — no envelope to read, no state to move.
  go('control:orb/cmd', 'editor-port', Object.assign({ method: 'POST', path: '/api/orb/cmd', body: '{"cmd":"reboot"}' }, body));
  go('control:orb/allow', 'editor-port', Object.assign({ method: 'POST', path: '/api/orb/allow', body: '{"ip":"127.0.0.1"}' }, body));
  go('control:orb/add', 'editor-port', Object.assign({ method: 'POST', path: '/api/orb/add', body: '{"ip":"127.0.0.1"}' }, body));
  go('control:orb/forget', 'editor-port', Object.assign({ method: 'POST', path: '/api/orb/forget', body: '{"ip":"127.0.0.1"}' }, body));
  go('control:orb/update', 'editor-port', Object.assign({ method: 'POST', path: '/api/orb/update', body: '{}' }, body));
  go('control:library/install', 'editor-port', { method: 'POST', path: '/api/library/install?ip=127.0.0.1&only=clock' });
  go('control:library/update', 'editor-port', { method: 'POST', path: '/api/library/update?ip=127.0.0.1' });
  go('control:apps/import', 'editor-port', Object.assign({ method: 'POST', path: '/api/apps/import', body: '{}' }, body));
  go('control:firmware/upload', 'editor-port', { method: 'POST', path: '/api/firmware/upload', body: junk.body, headers: junk.headers });
  go('control:flash', 'editor-port', Object.assign({ method: 'POST', path: '/api/flash', body: '{}' }, body));
  go('control:provision', 'editor-port', Object.assign({ method: 'POST', path: '/api/provision', body: '{}' }, body));
  go('control:fleet/set', 'editor-port', Object.assign({ method: 'POST', path: '/api/fleet/set', body: '{}' }, body));
  go('control:fleet/forget', 'editor-port', Object.assign({ method: 'POST', path: '/api/fleet/forget', body: '{}' }, body));
  go('control:site/import', 'editor-port', Object.assign({ method: 'POST', path: '/api/site/import', body: '{"link":"https://orb-os.net/t/AAAAAAAAAAAAAAAAAAAAAA"}' }, body));
  // Reads the control port keeps to itself, aimed at this port: 404 because they were
  // never registered here.
  ['fleet', 'orbs', 'state', 'console', 'discovered', 'roster', 'config'].forEach(function (p) {
    go('read:' + p, 'editor-port', { method: 'GET', path: '/api/' + p });
  });
  // Doors that DO exist here, under scopes this app does not hold: the answer is the
  // reject envelope, and the mutation stops at the guard.
  go('scope:keep-foreign', 'editor-port', Object.assign({ method: 'POST', path: '/api/library/keep?app=clock&fold=0', body: junk.body, headers: junk.headers }));
  go('scope:keep-own-junk', 'editor-port', Object.assign({ method: 'POST', path: '/api/library/keep?app=hostile&fold=0', body: junk.body, headers: junk.headers }));
  go('scope:upload-junk', 'editor-port', Object.assign({ method: 'POST', path: '/api/theme/upload?ip=127.0.0.1', body: junk.body, headers: junk.headers }));
  go('scope:design-save-foreign', 'editor-port', Object.assign({ method: 'POST', path: '/api/design/save?app=clock&name=x', body: '{}' }, body));
  go('scope:entries-foreign', 'editor-port', { method: 'GET', path: '/api/library/entries?app=clock' });
  go('scope:design-foreign', 'editor-port', { method: 'GET', path: '/api/theme/design?app=clock&id=AAAAAAAAAAAAAAAAAAAAAA' });
  go('scope:stem-foreign', 'editor-port', { method: 'GET', path: '/api/theme/stem?slug=clock&name=x' });
  go('scope:forget-foreign', 'editor-port', { method: 'POST', path: '/api/library/forget?app=clock&name=x&what=all' });
  go('ticket:site-bundle', 'editor-port', { method: 'GET', path: '/api/site/bundle?ticket=00000000000000000000000000000000' });
  go('ticket:site-keep', 'editor-port', Object.assign({ method: 'POST', path: '/api/site/keep?ticket=00000000000000000000000000000000', body: junk.body, headers: junk.headers }));
  // The card proxy: three GETs allowed, and everything else refused inside the door.
  go('proxy:status', 'editor-port', { method: 'GET', path: '/orb/127.0.0.1/api/v1/status' });
  go('proxy:reboot', 'editor-port', { method: 'GET', path: '/orb/127.0.0.1/api/v1/reboot' });
  go('proxy:upload-post', 'editor-port', { method: 'POST', path: '/orb/127.0.0.1/api/v1/sd/theme' });
  go('proxy:stranger-home', 'editor-port', { method: 'GET', path: '/orb/198.51.100.9/api/v1/home' });
  // Another app's editor, from this one's document: the path may not name someone else.
  go('crossapp:clock-doc', 'editor-port', { method: 'GET', path: '/editor/hub/clock/?et=hostile.nope' });
  go('crossapp:clock-json', 'editor-port', { method: 'GET', path: '/editor/hub/clock/editor.json' });
  go('traversal:up', 'editor-port', { method: 'GET', path: '/editor/hub/hostile/../../clock/editor.json' });
  // And the control port itself: connect-src/img-src say no before the network hears
  // about it. These prove the CSP layer, which the port's refusals alone would not.
  goAbs('csp:cmd', 'http://127.0.0.1:%%CTL%%/api/orb/cmd', Object.assign({ method: 'POST', body: '{"cmd":"reboot"}' }, body));
  goAbs('csp:fleet', 'http://127.0.0.1:%%CTL%%/api/fleet');
  goAbs('csp:index', 'http://127.0.0.1:%%CTL%%/');
  goAbs('csp:clock-origin', 'http://clock.localhost:%%EDP%%/editor/hub/clock/editor.json');
  imgProbe('csp:img-cmd', 'http://127.0.0.1:%%CTL%%/api/orb/cmd?x=' + Date.now());
  imgProbe('csp:img-index', 'http://127.0.0.1:%%CTL%%/?x=' + Date.now());
  seq.then(function () {
    window.__hostile = { done: true, results: R, cookie: document.cookie, at: Date.now() };
  });
})();`;

const HOSTILE_HTML = `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>hostile</title></head>
<body><p>third-party editor probe</p>
<script src="hostile.js"></script></body></html>`;

// buildHostileApp writes the .orbapp with the repo's own Python author — the same
// orbapp.write_container every app's build calls — so the hub's ParseApp sees a real
// container with a real editor region, not a hand-rolled ZIP pretending to be one.
function buildHostileApp(outPath, ctlPort, edPort) {
  const js = HOSTILE_JS.replaceAll('%%CTL%%', String(ctlPort)).replaceAll('%%EDP%%', String(edPort));
  const dir = path.dirname(outPath);
  const stage = fs.mkdtempSync(path.join(dir, 'hostile-stage-'));
  const script = path.join(stage, 'mk.py');
  fs.writeFileSync(path.join(stage, 'index.html'), HOSTILE_HTML);
  fs.writeFileSync(path.join(stage, 'hostile.js'), js);
  fs.writeFileSync(script, ('import sys, zipfile, io, os\n' +
    'sys.path.insert(0, @TOOLS@)\n' +
    'import orbapp\n' +
    'z = io.BytesIO()\n' +
    'with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as w:\n' +
    '    d = @STAGE@\n' +
    '    for n in ("index.html", "hostile.js"):\n' +
    '        i = zipfile.ZipInfo(n, date_time=(1980, 1, 1, 0, 0, 0))\n' +
    '        i.external_attr = 0o644 << 16\n' +
    '        w.writestr(i, open(os.path.join(d, n), "rb").read())\n' +
    'orbapp.write_container(@OUT@, "hostile", "Hostile", version="v0.0.1", editor=z.getvalue())\n')
      .replace('@TOOLS@', JSON.stringify(path.join(REPO, 'tools')))
      .replace('@STAGE@', JSON.stringify(stage))
      .replace('@OUT@', JSON.stringify(outPath)));
  const r = spawnSync3('python3', [script]);
  fs.rmSync(stage, { recursive: true, force: true });
  if (r.status !== 0) throw new Error('hostile .orbapp build failed: ' + r.stderr.slice(0, 400));
  return outPath;
}

function spawnSync3(cmd, args, cwd) {
  const { spawnSync } = require('child_process');
  return spawnSync(cmd, args, { cwd: cwd || REPO, encoding: 'utf8' });
}

// ---------- CDP over the platform WebSocket (no npm, no puppeteer) ----------
// The minimum the gate needs: one browser connection, auto-attach flattened so a
// cross-origin editor iframe becomes a session we can evaluate inside (the hub page
// cannot reach that frame's window — that is the whole point of S2), and the event
// taps (Log, Network) that prove what did NOT happen.

class CDP {
  constructor(url) {
    this.url = url;
    this.id = 0;
    this.pending = new Map();
    this.listeners = [];
    this.ws = new WebSocket(url);
    this.open = new Promise((res, rej) => { this.ws.onopen = res; this.ws.onerror = () => rej(new Error('CDP socket failed')); });
    this.ws.onmessage = (e) => this._onMessage(JSON.parse(e.data));
  }
  _onMessage(m) {
    if (m.id && this.pending.has(m.id)) {
      const p = this.pending.get(m.id);
      this.pending.delete(m.id);
      if (m.error) p.reject(new Error(m.method + ': ' + m.error.message));
      else p.resolve(m.result);
      return;
    }
    for (const fn of this.listeners) fn(m);
  }
  send(method, params, sessionId) {
    const id = ++this.id;
    return new Promise((res, rej) => {
      this.pending.set(id, { resolve: res, reject: rej });
      this.ws.send(JSON.stringify({ id, method, params: params || {}, sessionId }));
    });
  }
  on(fn) { this.listeners.push(fn); }
  close() { try { this.ws.close(); } catch (e) { /* gone */ } }
}

class Driver {
  constructor(cdp, edPort) {
    this.cdp = cdp;
    this.edPort = edPort;
    this.sessions = new Map();          // sessionId -> {type, url}
    this.csp = [];                      // {t, session, url, text} — every security entry logged
    this.reqUrl = new Map();            // requestId -> url, so the extra-info taps can name their request
    this.cookieHits = [];               // requests to the editor port that carried a Cookie header
    this.setCookieHits = [];            // editor-port responses that tried to Set-Cookie
    this.mainSession = null;
    cdp.on((m) => this._event(m));
  }
  async start() {
    await this.cdp.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true });
  }
  onEditorPort(u) {
    try { return new URL(u).port === String(this.edPort); } catch (e) { return false; }
  }
  _event(m) {
    if (m.method === 'Target.attachedToTarget') {
      const { sessionId, targetInfo } = m.params;
      this.sessions.set(sessionId, { type: targetInfo.type, url: targetInfo.url });
      // every fresh context gets the taps a check reads
      this.cdp.send('Runtime.enable', {}, sessionId).catch(() => {});
      this.cdp.send('Log.enable', {}, sessionId).catch(() => {});
      this.cdp.send('Network.enable', {}, sessionId).catch(() => {});
      this.cdp.send('Network.setCacheDisabled', { cacheDisabled: true }, sessionId).catch(() => {});
    } else if (m.method === 'Target.detachedFromTarget') {
      this.sessions.delete(m.params.sessionId);
    } else if (m.method === 'Target.targetInfoChanged') {
      const info = this.sessions.get(m.params.sessionId);
      if (info && m.params.targetInfo) { info.url = m.params.targetInfo.url; info.type = m.params.targetInfo.type; }
    } else if (m.method === 'Log.entryAdded') {
      const e = m.params.entry;
      if (e.source === 'security' || /content security policy/i.test(e.text || '')) {
        this.csp.push({ t: Date.now(), session: m.sessionId || '', url: e.url || '', text: String(e.text || '').slice(0, 300) });
      }
    } else if (m.method === 'Network.requestWillBeSent' || m.method === 'Network.responseReceived') {
      this.reqUrl.set(m.params.requestId, (m.params.request && m.params.request.url) || m.params.response.url);
    } else if (m.method === 'Network.requestWillBeSentExtraInfo') {
      const url = this.reqUrl.get(m.params.requestId) || '';
      const hs = m.params.headers || {};
      for (const k of Object.keys(hs)) {
        if (k.toLowerCase() === 'cookie' && this.onEditorPort(url)) {
          this.cookieHits.push({ t: Date.now(), url: url.slice(0, 160), value: String(hs[k]).slice(0, 80) });
        }
      }
    } else if (m.method === 'Network.responseReceivedExtraInfo') {
      const url = this.reqUrl.get(m.params.requestId) || '';
      const hs = m.params.headers || {};
      for (const k of Object.keys(hs)) {
        if (k.toLowerCase() === 'set-cookie' && this.onEditorPort(url)) {
          this.setCookieHits.push({ t: Date.now(), url: url.slice(0, 160), value: String(hs[k]).slice(0, 80) });
        }
      }
    }
  }
  async newTab(url) {
    const t = await this.cdp.send('Target.createTarget', { url });
    const s = await waitFor('tab session for ' + url, 20000, async () => {
      for (const [sid, info] of this.sessions) if (info.type === 'page' && info.url === url) return sid;
      await sleep(100);
      return null;
    });
    return { targetId: t.targetId, sessionId: s };
  }
  async openFirstPage(url) {
    // the browser starts with one empty tab; drive it to `url` and take its session
    const s = await waitFor('first page session', 20000, async () => {
      for (const [sid, info] of this.sessions) if (info.type === 'page') return sid;
      await sleep(100);
      return null;
    });
    this.mainSession = s;
    await this.cdp.send('Page.enable', {}, s);
    // page-level auto-attach as well: the cross-origin editor iframe is this page's
    // child target, and the flattened session is how the gate reads inside it.
    await this.cdp.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, s);
    await this.cdp.send('Page.navigate', { url }, s);
    return s;
  }
  evalIn(sessionId, expression) {
    return this.cdp.send('Runtime.evaluate',
      { expression, awaitPromise: true, returnByValue: true }, sessionId)
      .then((r) => {
        if (r.exceptionDetails) {
          const d = r.exceptionDetails;
          throw new Error('page threw: ' + ((d.exception && d.exception.description) || d.text).slice(0, 300));
        }
        return r.result.value;
      });
  }
  // editorSession finds the attached session whose URL is on <scheme>://<label>.localhost:<port>.
  async editorSession(label, port, contains) {
    return waitFor('editor session for ' + label, 30000, async () => {
      for (const [sid, info] of this.sessions) {
        if ((info.type === 'iframe' || info.type === 'page') &&
            info.url.startsWith('http://' + label + '.localhost:' + port) &&
            (!contains || info.url.includes(contains))) return sid;
      }
      await sleep(100);
      return null;
    });
  }
}

async function launchChromium(profileDir, logFile) {
  const exe = chromePath();
  if (!exe) throw new Error('no Chromium found (set --chrome or $CHROME)');
  const args = [
    '--headless=new', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage',
    '--disable-background-networking', '--disable-component-update', '--no-first-run',
    '--disable-extensions', '--mute-audio',
    '--host-resolver-rules=MAP *.localhost 127.0.0.1',
    '--user-data-dir=' + profileDir, '--remote-debugging-port=0',
  ];
  const p = spawnLogged(exe, args, { log: logFile });
  const portFile = path.join(profileDir, 'DevToolsActivePort');
  const port = Number(await waitFor('DevToolsActivePort', 20000, async () => {
    try { const s = fs.readFileSync(portFile, 'utf8').split('\n')[0].trim(); return /^\d+$/.test(s) ? s : null; } catch (e) { return null; }
  }));
  return { proc: p, port };
}

function chromePath() {
  const named = FLAGS.chrome;
  if (named) return fs.existsSync(named) ? named : null;
  if (process.env.CHROME && fs.existsSync(process.env.CHROME)) return process.env.CHROME;
  for (const c of ['chromium', 'chromium-browser', 'google-chrome', 'chrome']) {
    for (const dir of (process.env.PATH || '').split(path.delimiter)) {
      if (dir && fs.existsSync(path.join(dir, c))) return path.join(dir, c);
    }
  }
  return null;
}

// ---------- the stand-in site ----------
// orb-os.net's contract, reduced to the one door the hub uses: GET /api/hub/theme/<id>
// answers the design bundle for the link the operator pasted. The hub fetches this
// itself (no browser, no CORS), checks it against siteimport.go's rules, holds a ticket.

function startFakeSite() {
  const state = { id: randId22(), bundle: null, hits: [] };
  const srv = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://127.0.0.1');
    if (req.method === 'GET' && u.pathname === '/api/hub/theme/' + state.id && state.bundle) {
      state.hits.push(u.pathname);
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(state.bundle));
      return;
    }
    state.hits.push(u.pathname);
    res.writeHead(404, { 'Content-Type': 'application/json' });
    res.end('{"error":"no such design"}');
  });
  return new Promise((res) => srv.listen(0, '127.0.0.1', () => res({
    port: srv.address().port, state,
    close: () => new Promise((r) => srv.close(r)),
  })));
}

function randId22() {
  const abc = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-';
  const b = crypto.randomBytes(22);
  let s = '';
  for (let i = 0; i < 22; i++) s += abc[b[i] % abc.length];
  return s;
}


// ---------- the run ----------

async function main() {
  const t0 = Date.now();
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'orb-live-'));
  const dirs = {
    home: path.join(tmp, 'home'), apps: path.join(tmp, 'apps'), themes: path.join(tmp, 'themes'),
    bin: path.join(tmp, 'bin'), profile: path.join(tmp, 'profile'), logs: path.join(tmp, 'logs'),
  };
  for (const d of Object.values(dirs)) fs.mkdirSync(d, { recursive: true });
  fs.mkdirSync(path.join(dirs.home, '.config'), { recursive: true });
  const children = [];
  let site = null, cdp = null, browser = null;
  const killAll = async () => {
    if (cdp) cdp.close();
    if (browser) browser.proc.kill();
    for (const c of children) c.kill();
    if (site) await site.close();
    if (!FLAGS.keep) fs.rmSync(tmp, { recursive: true, force: true });
  };

  const checks = [];
  const record = (name, ok, detail) => {
    checks.push({ name, ok, detail: detail === undefined ? null : detail });
    say((ok ? 'PASS ' : 'FAIL ') + name + (detail ? ' — ' + detail : ''));
    return ok;
  };
  const flowWindows = [];
  let failed = false;
  let driver = null;
  try {
    // ---- build: hub and fake orb, from this tree, every run
    say('building hub and orbfake (go)…');
    for (const [out, pkg] of [['orb-manager', '.'], ['orbfake', './cmd/orbfake']]) {
      const r = spawnSync3(process.env.GO || 'go', ['build', '-o', path.join(dirs.bin, out), pkg], GO_ROOT);
      if (r.status !== 0) throw new Error('go build ' + pkg + ': ' + String(r.stderr || r.error || '').slice(0, 400));
    }

    // ---- seed: the clock (and the hostile app) on the app shelf, one stale bake on the theme shelf
    fs.copyFileSync(SHELF_CLOCK, path.join(dirs.apps, 'clock.orbapp'));
    const ctlPort = await freePort(), edPort = await freePort(), fakePort = await freePort();
    site = await startFakeSite();
    buildHostileApp(path.join(dirs.apps, 'hostile.orbapp'), ctlPort, edPort);
    const seed = restampStale(SHELF_THEME, path.join(dirs.themes, 's2-seed.orbtheme'), 'S2 rebake', 5);
    say('seeded stale "S2 rebake" (' + seed.bytes + ' B, abi ' + 5 + ') on the theme shelf');

    // ---- processes: the fake orb, then the hub dialling it on -orb-port
    const orb = spawnLogged(path.join(dirs.bin, 'orbfake'),
      ['-addr', '127.0.0.1:' + fakePort, '-fw', 'v1.2.30'],
      { log: path.join(dirs.logs, 'orbfake.log') });
    children.push(orb);
    const hub = spawnLogged(path.join(dirs.bin, 'orb-manager'), [
      '-no-window', '-addr', '127.0.0.1:' + ctlPort, '-editor-addr', '127.0.0.1:' + edPort,
      '-orb-port', String(fakePort), '-library-apps', dirs.apps, '-library-themes', dirs.themes,
      '-no-autoinstall', '-probe-cables=false',
    ], {
      log: path.join(dirs.logs, 'hub.log'),
      env: Object.assign({}, process.env, { HOME: dirs.home, XDG_CONFIG_HOME: path.join(dirs.home, '.config') }),
    });
    children.push(hub);
    await waitFor('orbfake status', 40000, () => httpJSON('http://127.0.0.1:' + fakePort + '/api/v1/status', {}, 2000).catch(() => null));
    const indexHtml = await waitFor('hub index', 40000, async () => {
      const r = await fetch('http://127.0.0.1:' + ctlPort + '/', { signal: AbortSignal.timeout(2000) }).catch(() => null);
      if (!r || !r.ok) return null;
      return await r.text();
    });
    const token = (indexHtml.match(/window\.__ORB_TOKEN__ = "([^"]+)"/) || [])[1];
    if (!token) throw new Error('no launch token in the served index.html');
    const api = async (p, opts) => {
      const base = Object.assign({}, opts || {});
      delete base.headers;
      const r = await fetch('http://127.0.0.1:' + ctlPort + p, Object.assign({
        signal: AbortSignal.timeout(120000),
      }, base, { headers: Object.assign({ 'X-Orb-Token': token }, (opts || {}).headers || {}) }));
      const t = await r.text();
      let j = null; try { j = JSON.parse(t); } catch (e) { /* raw below */ }
      return { status: r.status, env: j, text: t };
    };

    // ---- adopt the fake, install the clock, file the draft the stale theme has unsent changes to
    const add = await api('/api/orb/add', { method: 'POST', body: JSON.stringify({ ip: '127.0.0.1' }), headers: { 'Content-Type': 'application/json' } });
    if (!add.env || add.env.ok !== true) throw new Error('adopt 127.0.0.1: ' + add.text.slice(0, 200));
    const inst = await api('/api/library/install?ip=127.0.0.1&only=clock', { method: 'POST' });
    if (inst.status !== 200 || /"ok"\s*:\s*false/.test(inst.text)) throw new Error('install clock: ' + inst.text.slice(0, 200));
    const skel = fs.readFileSync(SHELF_THEME);
    const draftDoc = JSON.parse(skel.subarray(skel.readUInt32LE(29 * 4), skel.readUInt32LE(29 * 4) + skel.readUInt32LE(30 * 4)).toString('utf8'));
    draftDoc.themeName = 'S2 rebake';
    const draftSave = await api('/api/design/save?app=clock&name=' + encodeURIComponent('S2 rebake'),
      { method: 'POST', body: JSON.stringify(draftDoc), headers: { 'Content-Type': 'application/json' } });
    if (!draftSave.env || draftSave.env.ok !== true) throw new Error('design/save: ' + draftSave.text.slice(0, 200));

    // ---- browser: one tab on the hub, CDP taps on every context that appears
    browser = await launchChromium(dirs.profile, path.join(dirs.logs, 'chrome.log'));
    const version = await httpJSON('http://127.0.0.1:' + browser.port + '/json/version', {}, 5000);
    cdp = new CDP(version.env.webSocketDebuggerUrl);
    await cdp.open;
    driver = new Driver(cdp, edPort);
    await driver.start();
    const page = await driver.openFirstPage('http://127.0.0.1:' + ctlPort + '/');
    await waitFor('hub page booted', 30000, async () =>
      await driver.evalIn(page, '!!window.editorFrameSrc && !!window.loadThemes && !!document.querySelector("#mn-themes")').catch(() => null));

    const G = { api, driver, page, dirs, ctlPort, edPort, fakePort, site, flowWindows, token, record,
      docCookies: [], editorDocs: 0 };
    await runFlows(G);

    // ---- the hostile app: a third-party editor opened at its own origin, top level
    const before = await snapshotState(api, fakePort, dirs);
    const hostileOrig = await api('/api/editor/origin?app=hostile');
    if (!hostileOrig.env || hostileOrig.env.ok !== true) throw new Error('editor/origin hostile: ' + hostileOrig.text.slice(0, 160));
    const hostileUrl = hostileOrig.env.data.origin + '/editor/hub/hostile/?app=hostile&et=' +
      encodeURIComponent(hostileOrig.env.data.token);
    const hTab = await driver.newTab(hostileUrl);
    await waitFor('hostile probes done', FLAGS.timeout * 1000, async () => {
      const n = await driver.evalIn(hTab.sessionId,
        'window.__hostile && window.__hostile.done ? window.__hostile.results.length : null').catch(() => null);
      return n ? Number(n) : null;
    });
    const attempts = await driver.evalIn(hTab.sessionId, 'window.__hostile.results');
    const after = await snapshotState(api, fakePort, dirs);

    const refusedStatus = (s) => [400, 401, 403, 404, 405, 409, 410, 415, 421, 422, 429, 501, 502].includes(s);
    const isRefused = (r) => (r.where === 'editor-port' && refusedStatus(r.status)) ||
      (r.where === 'csp-blocked' && r.status === 0);
    const leaky = attempts.filter((r) => !isRefused(r));
    record('hostile-refused', attempts.length >= 30 && leaky.length === 0,
      attempts.length + ' attempts, ' + (attempts.length - leaky.length) + ' refused' +
      (leaky.length ? '; unexpected: ' + leaky.map((l) => l.name + '@' + l.where + ':' + l.status).join(', ') : ''));
    const drift = diffSnapshots(before, after);
    record('hostile-did-nothing', drift.length === 0, drift.length ? drift.join('; ') : 'entries, card, shelf and app list all identical');

    // ---- the two checks that read the passive taps, not a flow
    const docCookie = G.editorDocs > 0 && G.docCookies.length === G.editorDocs && G.docCookies.every((c) => c === '');
    record('no-cookie-on-editor',
      driver.cookieHits.length === 0 && driver.setCookieHits.length === 0 && docCookie,
      driver.cookieHits.length + ' Cookie header(s) and ' + driver.setCookieHits.length +
      ' Set-Cookie header(s) crossed the editor port; ' + G.editorDocs + ' editor documents all read document.cookie empty');
    const inFlows = driver.csp.filter((e) => flowWindows.some((w) => e.t >= w.t0 && e.t <= (w.t1 || Infinity)));
    record('no-csp-violation', inFlows.length === 0,
      driver.csp.length + ' security entries logged across the run (the hostile probes CSP-blocked log ' +
      (driver.csp.length - inFlows.length) + ' of them as expected), ' + inFlows.length + ' inside the four flows' +
      (inFlows.length ? ': ' + inFlows[0].text.slice(0, 140) : ''));

    // ---- verdict
    const ORDER = ['edit-on-own-origin', 'rebake-all', 'send-to-orb', 'import-from-site',
      'no-cookie-on-editor', 'no-csp-violation', 'hostile-refused', 'hostile-did-nothing'];
    const byName = {};
    for (const c of checks) byName[c.name] = c;
    const pass = ORDER.filter((n) => byName[n] && byName[n].ok);
    if (FLAGS.json) {
      // the verdict in the report itself: under --json the one line on stdout is this
      // JSON, so a pipe that greps the tail for the pass line must be able to — and
      // must not when the run failed
      const summary = 'editor-origin-live: ' + pass.length + '/8 passed';
      const report = JSON.stringify({
        passed: pass, failed: ORDER.filter((n) => pass.indexOf(n) < 0),
        summary: summary,
        checks: byName, hostile: attempts, csp: driver.csp, ms: Date.now() - t0,
      }) + '\n';
      process.stdout.write(report);
      if (FLAGS.jsonPath) {
        try { fs.writeFileSync(path.resolve(FLAGS.jsonPath), report); }
        catch (e) { say('could not write ' + FLAGS.jsonPath + ': ' + ((e && e.message) || e)); failed = true; }
      }
    }
    say('editor-origin-live: ' + pass.length + '/8 passed');
    for (const n of ORDER) {
      if (!byName[n] || !byName[n].ok) say('  not passed: ' + n + (byName[n] && byName[n].detail ? ' — ' + byName[n].detail : ' — never ran'));
    }
    failed = pass.length !== ORDER.length;
    if (failed && !FLAGS.keep) {
      for (const l of ['hub.log', 'orbfake.log', 'chrome.log']) {
        try {
          const tailTxt = fs.readFileSync(path.join(dirs.logs, l), 'utf8').split('\n').slice(-40).join('\n');
          say('--- ' + l + ' (last 40) ---\n' + tailTxt);
        } catch (e) { /* no log */ }
      }
    }
    if (failed && FLAGS.keep) say('kept ' + tmp);
  } catch (e) {
    say('run failed: ' + ((e && e.stack) || e));
    if (!FLAGS.keep) {
      for (const l of ['hub.log', 'orbfake.log']) {
        try { say('--- ' + l + ' ---\n' + fs.readFileSync(path.join(dirs.logs, l), 'utf8').split('\n').slice(-40).join('\n')); } catch (err) { /* none yet */ }
      }
    }
    failed = true;
  } finally {
    await killAll();
  }
  process.exit(failed ? 1 : 0);
}

// ---------- the four flows, in the order that lets each hand the next its proof ----------
//
// C2 runs first because it is the only flow that consumes the seeded stale bake, and it
// must consume it before anything can edit what it becomes. C1 edits that entry on its own
// origin and saves, so C3 sends what C1 changed, and C4 carries C1's edited bytes through
// the site door and back — one design proven to survive the whole set of doors.

async function runFlows(G) {
  const { api, driver, page, dirs, edPort, fakePort, site, flowWindows, record } = G;
  const only = FLAGS.only ? FLAGS.only.split(',').map((s) => s.trim()) : null;
  const want = (n) => !only || only.indexOf(n) >= 0;
  const card = () => httpJSON('http://127.0.0.1:' + fakePort + '/api/v1/sd', {}, 8000).then((r) => r.env || {});
  const entryList = (r) => {
    const d = r.env && r.env.data;
    return Array.isArray(d) ? d : (d && Array.isArray(d.entries) ? d.entries : []);
  };
  const entryNamed = async (name) => {
    const r = await api('/api/library/entries?app=clock');
    return entryList(r).find((e) => e.name === name) || null;
  };
  // editorReady waits for a live editor session whose CURRENT location (not the URL it
  // attached with — an iframe target's targetInfo lags its navigations) is the clock
  // editor on the editor port, and whose document has loaded; it returns the session id
  // and takes the cookie read with it.
  const editorHref = (sid) => driver.evalIn(sid, 'location.href').catch(() => null);
  const isClockEditor = (href) => !!href && href.startsWith('http://clock.localhost:' + edPort);
  const editorReady = (contains, ms) => waitFor('editor session ' + contains, ms || 60000, async () => {
    for (const [sid, info] of driver.sessions) {
      if (info.type !== 'iframe' && info.type !== 'page') continue;
      const href = await editorHref(sid);
      if (!isClockEditor(href)) continue;
      if (contains && href.indexOf(contains) < 0) continue;
      const cookie = await driver.evalIn(sid, 'document.readyState === "complete" ? document.cookie : null').catch(() => null);
      if (cookie !== null) { G.docCookies.push(String(cookie)); G.editorDocs++; return sid; }
    }
    await sleep(150);
    return null;
  });
  const openEditorVia = async (expression, contains) => {
    const said = await driver.evalIn(page, expression);
    if (said !== 'clicked') throw new Error('the press did not land: ' + said);
    return editorReady(contains, 90000);
  };
  // hookSession opens the very URL the hub just framed, as its own top-level tab, and
  // installs the migration seam (host.js calls window.GorbRunner with its session at
  // mount — the same seam check-migrations uses) at document start. A cross-origin
  // iframe is a flattened session we can read and click inside, but the Page domain's
  // document-start injection is top-level-only; the same document at the same origin,
  // loaded standalone, is the honest place to drive an edit. Everything the button did
  // — the origin, the token, the entry, the Save press — stays the editor's own.
  const hookSession = async (iframeSid) => {
    const href = await editorHref(iframeSid);
    if (!isClockEditor(href)) throw new Error('the framed editor went to ' + href);
    const tab = await driver.newTab('about:blank');
    await driver.cdp.send('Page.enable', {}, tab.sessionId);
    await driver.cdp.send('Page.addScriptToEvaluateOnNewDocument',
      { source: 'window.GorbRunner = function (s) { window.__SES = s; };' }, tab.sessionId);
    await driver.cdp.send('Page.navigate', { url: href }, tab.sessionId);
    const hooked = await waitFor('hooked editor tab', 90000, async () => {
      for (const [s2] of driver.sessions) {
        const h2 = await editorHref(s2);
        if (!isClockEditor(h2) || h2 !== href) continue;
        const ok = await driver.evalIn(s2, '!!window.__SES && document.readyState === "complete"').catch(() => null);
        if (ok) {
          G.docCookies.push(String(await driver.evalIn(s2, 'document.cookie').catch(() => 'unreadable')));
          G.editorDocs++;
          return s2;
        }
      }
      await sleep(200);
      return null;
    });
    return hooked;
  };
  const faceState = () => driver.evalIn(page, '(function(){var l=document.querySelector("#mn-themes");' +
    'return JSON.stringify({rows:themeAppsRows.map(function(a){return [a.slug,a.has_editor,!!a.editor_url];}),' +
    'state:document.querySelector("#mn-themestate").textContent,items:l?l.children.length:-1,' +
    'html:l?l.innerHTML.slice(0,300):""});})()').catch((e) => 'probe failed: ' + e.message);
  // pressAndWait keeps pressing until the label shows up: the studio paints its panel
  // after the render pass lands, a beat after the design opens.
  const pressAndWait = async (sid, selector, src, ms) => {
    const seen = await waitFor('button ' + src, ms || 40000, async () => {
      const r = await driver.evalIn(sid, clickByText(selector, src));
      return r === 'clicked' ? true : null;
    });
    void seen;
    return 'clicked';
  };
  const buttonsDump = (sid) => driver.evalIn(sid, '(function(){return [].slice.call(document.querySelectorAll("button"))' +
    '.map(function(b){return (b.className||"?")+":"+String(b.textContent).slice(0,40);}).join(" | ").slice(0,600);})()')
    .catch((e) => 'dump failed: ' + e.message);
  // toRenderStep switches the open editor to its Render step, where the clock editor
  // paints Save to library (its own start-sheet words say as much). Exact-text match:
  // a bare /Render/ would hit the "Render" field label beside the tabs.
  const toRenderStep = (sid) => waitFor('Render step tab', 30000, async () => {
    const r = await driver.evalIn(sid, '(function(){var b=[].slice.call(document.querySelectorAll("button"))' +
      '.find(function(x){return x.textContent.trim()==="Render";});if(!b)return "no-tab";b.click();return "clicked";})()');
    return r === 'clicked' ? true : null;
  });
  // clickByText is the DOM's own press: find a visible button by its text and click it,
  // which runs the page's real handler (the studio's buttons carry labels, not ids).
  const clickByText = (selector, src, flags) =>
    '(function(){var bs=[].slice.call(document.querySelectorAll(' + JSON.stringify(selector) + '));' +
    'var rx=new RegExp(' + JSON.stringify(src) + ', ' + JSON.stringify(flags || '') + ');' +
    'var m=bs.filter(function(x){return rx.test(x.textContent);})[0];' +
    'if(!m)return "no-button";m.click();return "clicked";})()';

  // ---- C2 rebake-all: the hub's own button, against the stale bake seeded on the shelf
  if (want('rebake-all')) {
    flowWindows.push({ t0: Date.now(), t1: null });
    try {
      const before = await entryNamed('S2 rebake');
      if (!before || !before.theme || !before.theme.stale) throw new Error('seed did not arrive stale: ' + JSON.stringify(before && before.theme));
      await driver.evalIn(page, '(loadThemes("127.0.0.1").then(function(){return themeAppsRows.length>0}))');
      const press = await driver.evalIn(page, clickByText('#mn-themes button', 'Rebake all'));
      if (press !== 'clicked') throw new Error('no Rebake all button on the Themes face: ' + press + ' — ' + await faceState());
      await editorReady('rebake=1').catch(() => null);   // cookie tap; rebake runs on its own
      const after = await waitFor('stale bake rebaked', FLAGS.timeout * 1000, async () => {
        const e = await entryNamed('S2 rebake').catch(() => null);
        return e && e.theme && !e.theme.stale && e.theme.bytes > 0 ? e : null;
      });
      record('rebake-all', true, 'stale abi-5 “S2 rebake” baked again at ' + after.theme.bytes + ' B by the clock editor');
    } catch (e) {
      record('rebake-all', false, String((e && e.message) || e).slice(0, 700));
    }
    flowWindows[flowWindows.length - 1].t1 = Date.now();
  }

  // ---- C1 edit-on-own-origin: Edit opens the frame on clock.localhost; a real change
  // made through the live session and saved by the real button must move the hub's draft.
  if (want('edit-on-own-origin')) {
    flowWindows.push({ t0: Date.now(), t1: null });
    try {
      const press = await driver.evalIn(page,
        // the same call the row's Edit button makes (app.js wires it to exactly this open);
        // the button itself is found by index across rows, so the entry is named directly
        '(function(){var row=themeAppsRows.find(function(a){return a.slug==="clock"});' +
        'if(!row)return "no-row";openThemeEditor(row,{entry:"S2 rebake"});return "clicked";})()');
      if (press !== 'clicked') throw new Error('the Edit open did not fire: ' + press);
      let sid;
      try { sid = await editorReady('entry=S2'); }
      catch (e) {
        const dump = JSON.stringify(Array.from(driver.sessions).map(([k, v]) => [v.type, String(v.url).slice(0, 120)]));
        const veilTxt = await driver.evalIn(page, '(function(){var v=document.querySelector("#mn-theme-editor .edveil");' +
          'var f=document.querySelector("#mn-theme-editor-frame");return JSON.stringify({veil:v?v.textContent.slice(0,160):null,' +
          'frame:f?f.src.slice(0,160):null,hidden:document.querySelector("#mn-theme-editor").hidden});})()').catch((x) => 'probe: ' + x.message);
        throw new Error(e.message + ' — sessions=' + dump + ' — pane=' + veilTxt);
      }
      const origin = await driver.evalIn(sid, 'location.origin');
      if (origin !== 'http://clock.localhost:' + edPort) throw new Error('frame is on ' + origin);
      const hs = await hookSession(sid);
      // the ?theme= door opens asynchronously: the editor boots on its default design and
      // reopens the themed one a moment later; wait for the reopen before editing, or the
      // save lands under the default design's name
      await waitFor('the opened theme is S2 rebake', 60000, async () => {
        const nm = await driver.evalIn(hs, '__SES.doc(false).then(function(d){return String(d.themeName);})');
        if (nm === 'S2 rebake') return true;
        throw new Error('themeName ' + nm);
      }).catch((e) => { throw new Error('the hooked editor never reopened the themed design — ' + e.message); });
      const docStr = await driver.evalIn(hs, '__SES.doc(false).then(function(d){return JSON.stringify(d)})');
      const doc = JSON.parse(docStr);
      const comps = doc.design && doc.design.comps;
      if (!comps || !comps.length) throw new Error('design carries no comps to prove a change');
      // the dial colour is the edit: canonDesign keeps a hex colour as written (a comp
      // size is renormalized from the dial the moment a design is applied), so this one
      // field proves the design really moved through the editor's session
      doc.design.color = doc.design.color === '#3a2f1f' ? '#1f2f3a' : '#3a2f1f';
      const applied = await driver.evalIn(hs,
        'Promise.resolve(__SES.apply(JSON.parse(' + JSON.stringify(JSON.stringify(doc)) + '), "the gate")).then(function(w){return typeof w === "string" ? w : "applied"})');
      if (applied !== 'applied') throw new Error('apply said: ' + applied);
      // the read-back rides the clock controller's own adopt of the host's design, one
      // React render after the apply: give it the same daylight the size pass has
      const back = await waitFor('applied design in the live doc', 30000, async () => {
        const b = await driver.evalIn(hs, '__SES.doc(false).then(function(d){var t=JSON.stringify(d.design||d);return d.design && d.design.color === ' + JSON.stringify(doc.design.color) + ' ? "ok" : "nope " + t.slice(0, 300);})');
        if (b === 'ok') return true;
        throw new Error('read back ' + b);
      }).catch((e) => { throw new Error('the applied colour never reached the live design — ' + e.message); });
      void back;
      await toRenderStep(hs);
      await pressAndWait(hs, 'button', 'Save to library').catch(async (e) => {
        throw new Error('no Save to library button — ' + e.message + ' — buttons: ' + await buttonsDump(hs));
      });
      await waitFor('draft carries the new colour', 90000, async () => {
        const r = await api('/api/library/design?app=clock&name=' + encodeURIComponent('S2 rebake') + '&from=draft');
        let d = null;
        try { d = JSON.parse(r.text); } catch (e) { /* the refusal envelope */ }
        if (d && d.design && d.design.color === doc.design.color) return true;
        await new Promise((res) => setTimeout(res, 400));
        throw new Error(r.text.slice(0, 240));
      });
      G.editDoc = doc;
      G.tab = hs;
      record('edit-on-own-origin', true, 'frame origin ' + origin + '; dial colour ' + doc.design.color + ' saved through the hub and back as the draft');
    } catch (e) {
      record('edit-on-own-origin', false, String((e && e.message) || e).slice(0, 700));
    }
    flowWindows[flowWindows.length - 1].t1 = Date.now();
  }

  // ---- C3 send-to-orb: the editor's own Send to orb bakes and pushes; the card must show it
  if (want('send-to-orb')) {
    flowWindows.push({ t0: Date.now(), t1: null });
    try {
      const sid = G.tab;
      if (!sid) throw new Error('the edited editor tab did not carry over from the edit flow');
      await toRenderStep(sid);
      await pressAndWait(sid, 'button', 'Send to orb|Download \\.orbtheme').catch(async (e) => {
        throw new Error('no Send button in the open editor — ' + e.message + ' — buttons: ' + await buttonsDump(sid));
      });
      const row = await waitFor('card carries the sent bake', FLAGS.timeout * 1000, async () => {
        const env = await card();
        const t = (env.themes || []).find((x) => x.name === 'S2 rebake');
        return t && t.stored !== false ? t : null;
      });
      record('send-to-orb', true, 'fake card row "' + row.name + '" id=' + row.id + ' bytes=' + row.bytes + ' after the Send press');
    } catch (e) {
      record('send-to-orb', false, String((e && e.message) || e).slice(0, 700));
    }
    flowWindows[flowWindows.length - 1].t1 = Date.now();
  }

  // ---- C4 import-from-site: the stand-in site serves C1's edited design as a bundle;
  // the Themes tab's real link box fetches it, bakes it and lands it in the library.
  if (want('import-from-site')) {
    flowWindows.push({ t0: Date.now(), t1: null });
    try {
      const src = JSON.parse(JSON.stringify(G.editDoc || JSON.parse((await api('/api/library/design?app=clock&name=' +
        encodeURIComponent('S2 rebake') + '&from=draft')).text)));
      src.themeName = 'S2 import by @greg';
      site.state.bundle = {
        format: 'orb-design-bundle', bundle: 1, id: site.state.id, version: 1,
        app: 'clock', name: 'S2 import', handle: 'greg', sharedName: 'S2 import by @greg',
        design: src, assets: {},
      };
      const link = 'http://127.0.0.1:' + site.port + '/t/' + site.state.id;
      await driver.evalIn(page, '(function(){var b=document.querySelector("#mn-site-btn");if(b)b.click();' +
        'var l=document.querySelector("#mn-site-link");l.value=' + JSON.stringify(link) + ';return "clicked";})()');
      const press = await driver.evalIn(page, '(function(){var g=document.querySelector("#mn-site-go");if(!g)return "no-go";g.click();return "clicked";})()');
      if (press !== 'clicked') throw new Error('the go button never pressed: ' + press);
      await editorReady('import=').catch(() => null);   // cookie tap on the import editor
      const stateText = await waitFor('import landed', FLAGS.timeout * 1000, async () => {
        const t = await driver.evalIn(page, 'document.querySelector("#mn-site-state").textContent');
        return /^✓/.test(t) ? t : null;
      });
      const landed = await entryNamed('S2 import by @greg') || await entryNamed('S2 import');
      if (!landed) throw new Error('✓ said ' + stateText.slice(0, 60) + ' but no import entry stands');
      record('import-from-site', true, stateText.slice(0, 90) + '; hub fetched ' + site.state.hits.length + ' bundle request(s)');
    } catch (e) {
      record('import-from-site', false, String((e && e.message) || e).slice(0, 700));
    }
    flowWindows[flowWindows.length - 1].t1 = Date.now();
  }
}

// ---------- state pictures for hostile-did-nothing ----------

async function snapshotState(api, fakePort, dirs) {
  const r = await api('/api/library/entries?app=clock');
  const d0 = r.env && r.env.data;
  const list = Array.isArray(d0) ? d0 : (d0 && Array.isArray(d0.entries) ? d0.entries : []);
  const cardEnv = await httpJSON('http://127.0.0.1:' + fakePort + '/api/v1/sd', {}, 8000).then((x) => x.env || {}).catch(() => ({}));
  return {
    entries: JSON.stringify(list.map((e) => ({
      name: e.name, state: e.state, draft: e.draft && e.draft.bytes,
      theme: e.theme && { stale: e.theme.stale || '', bytes: e.theme.bytes, stem: e.theme.stem },
    })).sort((a, b) => String(a.name).localeCompare(String(b.name)))),
    card: JSON.stringify((cardEnv.themes || []).map((t) => [t.name, t.serial, t.bytes, t.stored, t.why || ''])),
    shelf: fs.readdirSync(dirs.themes).sort().join(),
    apps: fs.readdirSync(dirs.apps).sort().join(),
  };
}

function diffSnapshots(a, b) {
  const out = [];
  for (const k of ['entries', 'card', 'shelf', 'apps']) {
    if (a[k] !== b[k]) out.push(k + ' changed under the hostile app');
  }
  return out;
}

function say(s) { process.stderr.write(s + '\n'); }

main().catch((e) => { console.error(e && e.stack || e); process.exit(1); });
