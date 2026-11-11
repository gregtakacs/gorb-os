#!/usr/bin/env node
// rename-live.js — fu2-4's live gate: what an owner actually SEES when a clock design whose
// STORED name breaks S3b's basic-character rule (checkThemeName's `^[A-Za-z0-9 ._-]+$`,
// sitelimits.go) is opened and re-saved from the editor. Ruling 35 is newer than some names a
// hub already holds; such an entry must still open, list and forget under its old name, and
// saving it must dead-end no more: the studio answers with a rename sheet pre-filled with a
// cleaned suggestion, and the accept posts an ordinary save through the unchanged hub door.
//
//   node rename-live.js [--out DIR] [--chrome <path>] [--keep]
//
// It builds the hub and the bench fake orb from THIS tree, seeds three clock designs straight
// into the library's designs folder exactly as the hub would have filed them — the two bad
// names `Nöel ✨` (accented letter + emoji) and `a/b` (a path separator), plus an ordinary
// `Plain` control — then drives headless Chromium over CDP through the owner's flow per name:
// list entries, open the design in the clock editor from the Themes tab, change nothing, go
// to the Render step, press Save to library. For a bad name it asserts the rename sheet opens
// with the expected suggestion, that Cancel posts nothing, and that the accept saves once
// under the clean name while the old entry keeps listing, opening and forgetting; for `Plain`
// it asserts the ordinary save with no sheet. It screenshots at each of those moments, reads
// the .st-toast line the editor paints, and records every /api/design/save with its answer.
// The picture lands in DIR/report.json.
//
// It is a gate: it prints `rename-live: N/N passed` and exits non-zero on any failed check or
// an unfinished run. It needs Go, Chromium and minutes (like editor-origin-live.js), so it is
// not part of `make check`; `make check-rename-live` runs it.
'use strict';
const fs = require('fs');
const net = require('net');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const { spawn, spawnSync } = require('child_process');

const REPO = path.resolve(__dirname, '..', '..', '..', '..', '..');
const SHELF_THEME = path.join(REPO, 'tools', 'orb-manager', 'themes', 'skeleton.orbtheme');
const SHELF_CLOCK = path.join(REPO, 'tools', 'orb-manager', 'apps', 'clock.orbapp');
const GO_ROOT = path.join(REPO, 'tools', 'orb-manager');

function arg(name, fallback) {
  const i = process.argv.indexOf('--' + name);
  return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback;
}
const FLAGS = {
  chrome: arg('chrome', process.env.CHROME || ''),
  keep: process.argv.includes('--keep'),
  out: path.resolve(REPO, arg('out', './.rename-shots')),
};

// The names under the probe. `Nöel ✨` and `a/b` are not made of the characters
// checkThemeName allows; `Plain` is an ordinary name kept as the CONTROL: it proves the flow
// itself works, so anything the two bad names do differently is about the NAME, not the probe.
const NAMES = [
  { label: 'noel-emoji', name: 'Nöel ✨', basic: false, suggest: 'Noel' },
  { label: 'slash', name: 'a/b', basic: false, suggest: 'a b' },
  { label: 'plain', name: 'Plain', basic: true, suggest: null },
];

function say(s) { process.stderr.write(s + '\n'); }
// the gate's counter: every check names itself, a failure says what it saw
let nChecks = 0, nFails = 0;
function check(name, cond, seen) {
  nChecks++;
  if (!cond) { nFails++; say('FAIL ' + name + (seen === undefined ? '' : '  [' + String(seen).slice(0, 300) + ']')); }
  else if (process.env.VERBOSE) say('PASS ' + name);
  return !!cond;
}
function freePort() {
  return new Promise((res, rej) => {
    const srv = net.createServer();
    srv.listen(0, '127.0.0.1', () => { const p = srv.address().port; srv.close(() => res(p)); });
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
function spawnLogged(cmd, args, opts) {
  const out = fs.openSync(opts.log, 'a');
  const p = spawn(cmd, args, {
    cwd: opts.cwd || REPO, env: opts.env || process.env, stdio: ['ignore', out, out], detached: true,
  });
  fs.closeSync(out);
  return { proc: p, kill() { try { process.kill(-p.pid, 'SIGKILL'); } catch (e) { /* reaped */ } } };
}
function spawnSync3(cmd, args, cwd) {
  return spawnSync(cmd, args, { cwd: cwd || REPO, encoding: 'utf8' });
}

// ---------- designFile, ported from designs.go so the seed lands where the hub would file it ----------
// A name's file is a readable stem plus a hash of the EXACT name: two names that fold to one
// stem never share a file, and the same name always lands on the same one. This port must
// match the Go byte for byte — a seeded draft must be the file the hub itself would have
// written for that name, or a save-over would create a SECOND entry and the probe would be
// lying about the owner's situation.
function designFile(name) {
  const lower = String(name).toLowerCase();   // Go strings.ToLower, then a rune walk
  let s = '', dash = false;
  for (const r of lower) {                     // JS for..of walks code points, like Go range-over-runes
    const isAl = (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9');
    if (isAl) { s += r; dash = false; }
    else if (!dash && s.length > 0) { s += '-'; dash = true; }
    if (s.length >= 40) break;                 // Go breaks AFTER writing, on the builder's byte length
  }
  let stem = s.replace(/^-+/, '').replace(/-+$/, '');   // Go strings.Trim(s, "-")
  if (stem === '') stem = 'design';
  const h = crypto.createHash('sha256').update(name, 'utf8').digest();   // sha256 of the raw name
  return stem + '-' + h.subarray(0, 4).toString('hex') + '.orbdesign';
}

// The clock design's real shape, read out of the shipped Skeleton default exactly the way
// editor-origin-live.js reads its draftDoc: the .orbtheme container's design section (header
// words 29/30/31 are its offset, size and CRC). This is the document the hub files, so a
// seeded draft is one the owner could really have made.
const W_DESIGN_OFF = 29, W_DESIGN_SIZE = 30;
function shelfDesignDoc() {
  const b = fs.readFileSync(SHELF_THEME);
  const w = (i) => b.readUInt32LE(i * 4);
  if (w(0) !== 0x474f5254 || w(1) !== 3) throw new Error('seed: skeleton.orbtheme is not a FORMAT 3 container');
  const off = w(W_DESIGN_OFF), size = w(W_DESIGN_SIZE);
  if (size === 0) throw new Error('seed: skeleton.orbtheme carries no design to clone');
  const doc = JSON.parse(b.subarray(off, off + size).toString('utf8'));
  if (doc.format !== 'wc-design') throw new Error('seed: skeleton design is not a wc-design document');
  return doc;
}

// seedDesignFiles writes each probe design into the library's designs folder as the hub would
// have filed it: the file the name's designFile names, a copy of the clock's real design whose
// saved.name (and themeName, the field the editor opens it under) is the probe name. The hub
// creates designs/<app> beside the writable themes dir on its first save; we make it by hand
// before the hub starts, so an owner meeting these designs finds them already sitting there.
function seedDesignFiles(designsDir, names) {
  fs.mkdirSync(designsDir, { recursive: true });
  const made = [];
  for (const n of names) {
    const doc = shelfDesignDoc();
    doc.themeName = n.name;
    doc.saved = { name: n.name, at: Date.now() };
    const fileName = designFile(n.name);
    const file = path.join(designsDir, fileName);
    const bytes = Buffer.from(JSON.stringify(doc), 'utf8');
    fs.writeFileSync(file, bytes);
    made.push({ label: n.label, name: n.name, fileName: fileName, file: file, bytes: bytes.length });
  }
  return made;
}

// ---------- CDP over the platform WebSocket (no npm, no puppeteer), per the live gate ----------
class CDP {
  constructor(url) {
    this.url = url; this.id = 0; this.pending = new Map(); this.listeners = [];
    this.ws = new WebSocket(url);
    this.open = new Promise((res, rej) => { this.ws.onopen = res; this.ws.onerror = () => rej(new Error('CDP socket failed')); });
    this.ws.onmessage = (e) => this._onMessage(JSON.parse(e.data));
  }
  _onMessage(m) {
    if (m.id && this.pending.has(m.id)) {
      const p = this.pending.get(m.id); this.pending.delete(m.id);
      if (m.error) p.reject(new Error(m.method + ': ' + m.error.message)); else p.resolve(m.result);
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

// Driver tracks every attached target and, on the two write doors the probe cares about, the
// request that reached them and the answer the hub gave. Bodies are pulled at responseReceived
// while the request is still in the Network buffer.
class Driver {
  constructor(cdp, edPort) {
    this.cdp = cdp; this.edPort = edPort;
    this.sessions = new Map();      // sessionId -> {type,url}
    this.requests = [];             // captured /api/design/save and /api/library/keep
    this.reqUrl = new Map();        // requestId -> {url, sessionId}
    this.pendingBody = new Map();   // requestId -> record awaiting its answer body
    this.mainSession = null;
    cdp.on((m) => this._event(m));
  }
  async start() {
    await this.cdp.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true });
  }
  _isProbeUrl(u) { return /\/api\/(design\/save|library\/keep)\b/.test(u); }
  _event(m) {
    if (m.method === 'Target.attachedToTarget') {
      const { sessionId, targetInfo } = m.params;
      this.sessions.set(sessionId, { type: targetInfo.type, url: targetInfo.url });
      this.cdp.send('Runtime.enable', {}, sessionId).catch(() => {});
      this.cdp.send('Network.enable', {}, sessionId).catch(() => {});
      this.cdp.send('Network.setCacheDisabled', { cacheDisabled: true }, sessionId).catch(() => {});
    } else if (m.method === 'Target.detachedFromTarget') {
      this.sessions.delete(m.params.sessionId);
    } else if (m.method === 'Target.targetInfoChanged') {
      const info = this.sessions.get(m.params.sessionId);
      if (info && m.params.targetInfo) { info.url = m.params.targetInfo.url; info.type = m.params.targetInfo.type; }
    } else if (m.method === 'Network.requestWillBeSent') {
      const url = (m.params.request && m.params.request.url) || '';
      const sid = m.sessionId || '';
      this.reqUrl.set(m.params.requestId, { url, sessionId: sid });
      if (this._isProbeUrl(url)) {
        const rec = { url, method: (m.params.request && m.params.request.method) || '',
          postData: (m.params.request && m.params.request.postData) || null,
          status: null, responseBody: null, at: Date.now() };
        this.requests.push(rec);
        this.pendingBody.set(m.params.requestId, rec);
      }
    } else if (m.method === 'Network.responseReceived') {
      const info = this.reqUrl.get(m.params.requestId);
      const rec = this.pendingBody.get(m.params.requestId);
      if (!rec) return;
      rec.status = m.params.response.status;
      this.cdp.send('Network.getResponseBody', { requestId: m.params.requestId }, (info && info.sessionId) || undefined)
        .then((r) => { rec.responseBody = r && r.body != null ? (r.base64Encoded ? '<binary>' : r.body) : null; })
        .catch((e) => { rec.responseBody = '<unreadable: ' + e.message + '>'; })
        .finally(() => this.pendingBody.delete(m.params.requestId));
    }
  }
  async openFirstPage(url) {
    const s = await waitFor('first page session', 20000, async () => {
      for (const [sid, info] of this.sessions) if (info.type === 'page') return sid;
      await sleep(100); return null;
    });
    this.mainSession = s;
    await this.cdp.send('Page.enable', {}, s);
    await this.cdp.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, s);
    await this.cdp.send('Page.navigate', { url }, s);
    return s;
  }
  evalIn(sessionId, expression) {
    return this.cdp.send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true }, sessionId)
      .then((r) => {
        if (r.exceptionDetails) {
          const d = r.exceptionDetails;
          throw new Error('page threw: ' + ((d.exception && d.exception.description) || d.text).slice(0, 300));
        }
        return r.result.value;
      });
  }
  async newTab(url) {
    const t = await this.cdp.send('Target.createTarget', { url });
    const s = await waitFor('tab session for ' + url, 20000, async () => {
      for (const [sid, info] of this.sessions) if (info.type === 'page' && info.url === url) return sid;
      await sleep(100); return null;
    });
    return { targetId: t.targetId, sessionId: s };
  }
  async screenshot(sessionId, outPath) {
    const r = await this.cdp.send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true }, sessionId);
    const buf = Buffer.from(r.data, 'base64');
    fs.writeFileSync(outPath, buf);
    return buf.length;
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
  for (const c of ['chromium', 'chromium-browser', 'google-chrome', 'chrome']) {
    for (const dir of (process.env.PATH || '').split(path.delimiter)) {
      if (dir && fs.existsSync(path.join(dir, c))) return path.join(dir, c);
    }
  }
  return null;
}

// ---------- the flow inside a live editor document, in the live gate's own idiom ----------
// clickByText finds a visible button by its text and presses it, which runs the page's real
// handler (the studio's buttons carry labels, not ids). Exact-text for the step tab so a bare
// /Render/ does not hit the "Render" field label beside the tabs.
function clickByTextExpr(selector, src, flags) {
  return '(function(){var bs=[].slice.call(document.querySelectorAll(' + JSON.stringify(selector) + '));' +
    'var rx=new RegExp(' + JSON.stringify(src) + ', ' + JSON.stringify(flags || '') + ');' +
    'var m=bs.filter(function(x){return rx.test(x.textContent);})[0];' +
    'if(!m)return "no-button";m.click();return "clicked";})()';
}
async function pressAndWait(driver, sid, selector, src, ms) {
  await waitFor('button ' + src, ms || 40000, async () =>
    (await driver.evalIn(sid, clickByTextExpr(selector, src))) === 'clicked' ? true : null);
  return 'clicked';
}
async function toRenderStep(driver, sid) {
  await waitFor('Render step tab', 30000, async () => {
    const r = await driver.evalIn(sid, '(function(){var b=[].slice.call(document.querySelectorAll("button"))' +
      '.find(function(x){return x.textContent.trim()==="Render";});if(!b)return "no-tab";b.click();return "clicked";})()');
    return r === 'clicked' ? true : null;
  });
}
function buttonsDump(driver, sid) {
  return driver.evalIn(sid, '(function(){return [].slice.call(document.querySelectorAll("button"))' +
    '.map(function(b){return (b.className||"?")+":"+String(b.textContent).slice(0,40);}).join(" | ").slice(0,600);})()')
    .catch((e) => 'dump failed: ' + e.message);
}
// statusDump reads the visible status/error text the editor paints: studio.js's Glass draws the
// session's status line as a .st-toast (an .st-err when it reads as an error), the same seam
// the live gate reads for the editor's verdict. `err` says whether that line is an error class.
async function readStatus(driver, sid) {
  const s = await driver.evalIn(sid, '(function(){var out=[].slice.call(document.querySelectorAll(".st-toast,.st-err,.st-note"))' +
    '.map(function(e){return {text:String(e.textContent||"").replace(/\\s+/g," ").trim(),' +
    'err:/(^|\\s)st-err(\\s|$)/.test(String(e.className))};}).filter(function(x){return x.text;});' +
    'return JSON.stringify(out);})()').catch(() => '[]');
  try { return JSON.parse(s); } catch (e) { return []; }
}
// the rename sheet session.js paints (fu2-4): its open state and field, and pressing a button
// of its own footer — scoped to .st-rename so a studio button of a like label is never the one.
async function sheetState(driver, sid) {
  const s = await driver.evalIn(sid, '(function(){var m=document.querySelector(".st-rename");' +
    'if(!m)return JSON.stringify({open:false});var i=m.querySelector("input");' +
    'var t=m.querySelector(".st-mtitle");var b=m.querySelector(".st-mbody");var f=m.querySelector(".st-mfoot");' +
    'return JSON.stringify({open:true,value:i?i.value:null,title:t?t.textContent:null,body:b?b.textContent:null,' +
    'buttons:[].slice.call(f?f.children:[]).map(function(b2){return {text:b2.textContent,disabled:!!b2.disabled};})});})()').catch(() => '{"open":false}');
  try { return JSON.parse(s); } catch (e) { return { open: false }; }
}
// the writes the editor made to the doors that take a design — the probe's own seam
function saveWrites(reqs) {
  return (reqs || []).filter((r) => /\/api\/(design\/save|library\/keep)/.test(String(r.url)));
}
async function sheetPress(driver, sid, label) {
  return driver.evalIn(sid, '(function(){var m=document.querySelector(".st-rename");if(!m)return "no-sheet";' +
    'var b=[].slice.call(m.querySelectorAll(".st-mfoot button")).find(function(x){return x.textContent===' + JSON.stringify(label) + '});' +
    'if(!b||b.disabled)return "no-button";b.click();return "clicked";})()');
}

// ---------- reaching the design the owner reaches: the Themes-tab Edit, then a driveable tab ----------
// editorReady finds the framed editor session whose CURRENT location (not the URL it attached
// with — an iframe's targetInfo lags its navigations) is the clock editor on the editor port,
// carrying `contains`, with its document loaded. Returns the session id.
async function editorReady(driver, edPort, contains, ms) {
  const isClock = (h) => !!h && h.startsWith('http://clock.localhost:' + edPort);
  return waitFor('editor session ' + contains, ms || 90000, async () => {
    for (const [sid, info] of driver.sessions) {
      if (info.type !== 'iframe' && info.type !== 'page') continue;
      const href = await driver.evalIn(sid, 'location.href').catch(() => null);
      if (!isClock(href)) continue;
      if (contains && href.indexOf(contains) < 0) continue;
      const done = await driver.evalIn(sid, 'document.readyState === "complete" ? "done" : null').catch(() => null);
      if (done) return sid;
    }
    await sleep(150); return null;
  });
}
// hookSession opens the very URL the hub just framed, as its own top-level tab, and installs
// the session seam (host.js calls window.GorbRunner(session) at mount — the seam check-migrations
// uses) at document start. A cross-origin iframe is a flattened session we can read but the
// Page domain's document-start injection is top-level-only; the same document at the same
// origin, loaded standalone, is the honest place to drive an edit. Everything the Edit press
// chose — origin, token, entry — stays the editor's own.
async function hookSession(driver, edPort, iframeSid) {
  const href = await driver.evalIn(iframeSid, 'location.href').catch(() => null);
  if (!href || !href.startsWith('http://clock.localhost:' + edPort)) throw new Error('framed editor went to ' + href);
  const tab = await driver.newTab('about:blank');
  await driver.cdp.send('Page.enable', {}, tab.sessionId);
  await driver.cdp.send('Page.addScriptToEvaluateOnNewDocument',
    { source: 'window.GorbRunner = function (s) { window.__SES = s; };' }, tab.sessionId);
  await driver.cdp.send('Page.navigate', { url: href }, tab.sessionId);
  return waitFor('hooked editor tab', 90000, async () => {
    for (const [s2] of driver.sessions) {
      const h2 = await driver.evalIn(s2, 'location.href').catch(() => null);
      if (h2 !== href) continue;
      const ok = await driver.evalIn(s2, '!!window.__SES && document.readyState === "complete"').catch(() => null);
      if (ok) return s2;
    }
    await sleep(200); return null;
  });
}

// flowForName walks one design through the owner's flow and fills `found` as it goes.
// `edPort` is the hub's editor port; `api` dials the hub control port with the launch token.
async function flowForName(driver, page, api, edPort, n, found) {
  const listUrl = '/api/library/entries?app=clock';
  const entryList = (env) => { const d = env && env.data; return Array.isArray(d) ? d : (d && Array.isArray(d.entries) ? d.entries : []); };

  // 1. what the hub lists for this app, and whether this name is in it
  const lr = await api(listUrl);
  const listed = entryList(lr.env).find((e) => e.name === n.name) || null;
  check(n.name + ': the hub lists the seeded entry from the start', !!listed, lr.text.slice(0, 200));
  found.entryListedAtOpen = { status: lr.status, listed: !!listed, state: listed && listed.state,
    draftBytes: listed && listed.draft && listed.draft.bytes };

  // a mark into the captured writes, so the requests below belong to THIS design's flow
  const reqMark = driver.requests.length;
  // landed(nm): a /api/design/save POST under this name has reached the hub and been answered.
  // A 200 is the whole question here: the hub's envelope puts its fat data before the ok flag,
  // and the editor's own ack — checked below, and the library list beside it — says which way
  // the flag fell.
  const landed = async (nm) => saveWrites(driver.requests.slice(reqMark)).filter((r) => {
    let name = null; try { name = new URL(r.url).searchParams.get('name'); } catch (e) { /* no query */ }
    return name === nm && /\/api\/design\/save/.test(String(r.url)) && r.status != null && r.status >= 200 && r.status < 300;
  }).length > 0;

  // 2. open the design the way an owner does: the Themes-tab row's Edit button (app.js wires it
  //    to exactly this call), named directly since the button sits at a row index.
  await driver.evalIn(page, 'loadThemes("127.0.0.1")').catch(() => null);
  const press = await driver.evalIn(page,
    '(function(){var row=themeAppsRows.find(function(a){return a.slug==="clock"});' +
    'if(!row)return "no-row";openThemeEditor(row,{entry:' + JSON.stringify(n.name) + '});return "clicked";})()');
  if (press !== 'clicked') { found.openWay = 'Themes-tab Edit unavailable: ' + press; throw new Error('the Themes-tab open did not fire: ' + press); }
  const contains = 'entry=' + encodeURIComponent(n.name);
  const iframeSid = await editorReady(driver, edPort, contains);
  const hs = await hookSession(driver, edPort, iframeSid);
  found.opened = true; found.openWay = 'Themes-tab Edit → openThemeEditor({entry}) framed on clock.localhost:' + edPort + ', driven in a hooked top-level tab';

  // the design under ?entry= opens asynchronously off the hub; wait until the live document
  // really carries the probed name before pressing anything, or the save lands under a default.
  await waitFor('opened name ' + n.name, 60000, async () => {
    const nm = await driver.evalIn(hs, '__SES.doc(false).then(function(d){return String(d.themeName);})').catch(() => null);
    if (nm === n.name) return true;
    throw new Error('themeName ' + nm);
  }).catch((e) => { throw new Error('the editor never opened the probed design — ' + e.message); });

  // 3. screenshot after opening (full page of the hooked editor tab)
  found.statusAfterOpen = await readStatus(driver, hs);
  found.screenshots.opened = path.join(FLAGS.out, n.label + '-1-opened.png');
  await driver.screenshot(hs, found.screenshots.opened);

  // 4. change nothing, go to the Render step, press Save to library
  await toRenderStep(driver, hs);
  await pressAndWait(driver, hs, 'button', 'Save to library').catch(async (e) => {
    throw new Error('no Save to library button — ' + e.message + ' — buttons: ' + await buttonsDump(driver, hs));
  });
  await sleep(700);   // let the save round-trip and the status line settle before reading
  found.sheet = await sheetState(driver, hs);
  found.screenshots.sheet = path.join(FLAGS.out, n.label + '-2-rename-sheet.png');
  await driver.screenshot(hs, found.screenshots.sheet);
  if (!n.basic) {
    // fu2-4: a held name outside the rule opens the rename sheet, never the flat refusal
    check(n.name + ': pressing Save opens the rename sheet', found.sheet.open, JSON.stringify(found.sheet));
    check(n.name + ': the sheet is prefilled with the cleaned suggestion', found.sheet.value === n.suggest, found.sheet.value);
    check(n.name + ': the sheet states the rule it is working around',
      /can use only letters/.test(String(found.sheet.body || '')), String(found.sheet.body || '').slice(0, 160));
    // cancelling saves nothing and says so without an error
    check(n.name + ': Cancel presses', (await sheetPress(driver, hs, 'Cancel')) === 'clicked', JSON.stringify(found.sheet.buttons));
    await sleep(600);
    const cancelled = await readStatus(driver, hs);
    check(n.name + ': cancelling says the rename was cancelled, not that the save failed',
      cancelled.some((x) => /Rename cancelled/.test(x.text)) && !cancelled.some((x) => x.err && /Not saved/.test(x.text)),
      JSON.stringify(cancelled));
    check(n.name + ': cancelling posted no save',
      saveWrites(driver.requests.slice(reqMark)).length === 0,
      JSON.stringify(driver.requests.slice(reqMark).map((r) => [r.method, String(r.url).slice(0, 90)])));
    const listedCancel = entryList((await api(listUrl)).env);
    check(n.name + ': after cancelling the library holds no new name',
      !listedCancel.find((e) => e.name === n.suggest), JSON.stringify(listedCancel.map((e) => e.name)));
    // press again and accept the suggestion
    await pressAndWait(driver, hs, 'button', 'Save to library');
    await waitFor('rename sheet a second time', 10000, async () => (await sheetState(driver, hs)).open ? true : null);
    check(n.name + ': Accept posts the save', (await sheetPress(driver, hs, 'Rename and save')) === 'clicked');
    await waitFor('the renamed save to land', 30000, () => landed(n.suggest)).catch(() => null);
    await sleep(700);
  } else {
    check(n.name + ': an ordinary name is saved with no sheet', !found.sheet.open, JSON.stringify(found.sheet));
    await waitFor('the save to land', 30000, () => landed(n.name)).catch(() => null);
  }
  found.statusAfterSave = await readStatus(driver, hs);
  found.screenshots.afterSave = path.join(FLAGS.out, n.label + '-3-after-save.png');
  await driver.screenshot(hs, found.screenshots.afterSave);

  // 5. 3 s later — a note fades (studio.js NOTE_MS is 4000), an error and an ack hold; what
  //    survives says whether the editor is telling the owner something it wants them to read.
  await sleep(3000);
  found.statusAfter3s = await readStatus(driver, hs);
  found.screenshots.after3s = path.join(FLAGS.out, n.label + '-3-after-3s.png');
  await driver.screenshot(hs, found.screenshots.after3s);

  // 6. the writes the editor made to the two doors that can take a name, and their answers
  const win = driver.requests.slice(reqMark);
  found.requests = win.map((r) => {
    let name = null; try { name = new URL(r.url).searchParams.get('name'); } catch (e) { /* no query */ }
    return { url: String(r.url).slice(0, 400), bearsName: name, method: r.method, status: r.status,
      answer: r.responseBody == null ? null : String(r.responseBody).slice(0, 800) };
  });
  const bearing = found.requests.filter((r) => r.bearsName === n.name);
  const savePosts = found.requests.filter((r) => /\/api\/design\/save/.test(r.url) && r.method === 'POST');
  found.saveVerdict = win.length === 0
    ? 'no request reached /api/design/save or /api/library/keep (the editor stopped it first)'
    : (bearing.length === 0
        ? 'a write was attempted, but under another name: ' + JSON.stringify(found.requests.map((r) => [r.bearsName, r.status]))
        : bearing.map((r) => r.status + ' ' + (r.answer || '')).join(' | '));
  const underSuggest = n.basic ? n.name : n.suggest;
  const okUnder = savePosts.filter((r) => r.bearsName === underSuggest && r.status >= 200 && r.status < 300);
  if (n.basic) {
    check(n.name + ': the ordinary save posted once, under its own name',
      savePosts.length === 1 && okUnder.length === 1, JSON.stringify(savePosts.map((r) => [r.bearsName, r.status])));
  } else {
    check(n.name + ': the accept posted exactly one save, under the clean name',
      savePosts.length === 1 && okUnder.length === 1, JSON.stringify(savePosts.map((r) => [r.bearsName, r.status])));
    check(n.name + ': the editor says it renamed and saved',
      found.statusAfterSave.concat(found.statusAfter3s).some((x) => /Renamed to/.test(x.text)),
      JSON.stringify(found.statusAfterSave.concat(found.statusAfter3s)));
  }

  // 7. does the entry still list, open, and can it be forgotten afterwards
  const afterSaveList = entryList((await api(listUrl)).env);
  const stillListed = afterSaveList.find((e) => e.name === n.name) || null;
  check(n.name + ': the entry still lists under its own name', !!stillListed, JSON.stringify(afterSaveList.map((e) => e.name)));
  if (!n.basic) {
    check(n.name + ': the renamed design lists beside the old entry',
      !!afterSaveList.find((e) => e.name === n.suggest), JSON.stringify(afterSaveList.map((e) => e.name)));
  }
  const opens = await api('/api/library/design?app=clock&name=' + encodeURIComponent(n.name));
  let opensOk = false, opensErr = null;
  try { const o = JSON.parse(opens.text); opensOk = opens.status === 200 && o && o.format === 'wc-design'; opensErr = (o && o.error) || null; }
  catch (e) { opensErr = opens.text.slice(0, 160); }
  check(n.name + ': the old entry still opens under the old name', opensOk, opensErr);

  // forget: on a copy for the basic control (so Plain is not lost by a probe that only means to
  // show it survives); for the bad names, forget the very entry an owner would be stuck with.
  const forgetName = n.basic ? (n.name + ' copy') : n.name;
  if (n.basic) {
    const d = await api('/api/library/design?app=clock&name=' + encodeURIComponent(n.name));
    let doc = null; try { doc = JSON.parse(d.text); } catch (e) { /* fall through */ }
    if (doc) {
      doc.themeName = forgetName;
      await api('/api/design/save?app=clock&name=' + encodeURIComponent(forgetName),
        { method: 'POST', body: JSON.stringify(doc), headers: { 'Content-Type': 'application/json' } });
    }
  }
  const fr = await api('/api/library/forget?app=clock&name=' + encodeURIComponent(forgetName) + '&what=all', { method: 'POST' });
  const goneList = entryList((await api(listUrl)).env);
  const gone = !goneList.find((e) => e.name === forgetName);
  check(forgetName + ': it still forgets under that name', gone && fr.env && fr.env.ok === true, fr.text.slice(0, 200));
  found.afterForget = { forgetName, status: fr.status, ok: !!(fr.env && fr.env.ok === true),
    answer: fr.text.slice(0, 300), goneFromList: gone, openedErr: opensErr };

  const st = found.statusAfterSave[0] || found.statusAfter3s[0] || null;
  found.visibleAfterSave = st ? st.text : null;
  const line = n.name + ' [' + (n.basic ? 'basic' : 'NOT basic') + '] -> editor said: ' +
    (st ? (st.err ? 'error: ' : 'note: ') + JSON.stringify(st.text) : '(no status line)') +
    '; reached hub: ' + (bearing.length ? 'yes' : 'no') + '; still lists: ' + !!stillListed +
    '; opens: ' + opensOk + '; forgettable: ' + gone;
  say('  ' + line);
  return line;
}

async function httpJSON(url, ms) {
  const r = await fetch(url, { signal: AbortSignal.timeout(ms || 5000) });
  const t = await r.text();
  let j = null; try { j = JSON.parse(t); } catch (e) { /* raw below */ }
  if (!j) throw new Error(url + ' answered ' + r.status + ': ' + t.slice(0, 160));
  return { status: r.status, env: j };
}

// ---------- the run ----------
async function main() {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'orb-rename-'));
  const dirs = {
    home: path.join(tmp, 'home'), apps: path.join(tmp, 'apps'), themes: path.join(tmp, 'themes'),
    bin: path.join(tmp, 'bin'), profile: path.join(tmp, 'profile'), logs: path.join(tmp, 'logs'),
  };
  for (const d of Object.values(dirs)) fs.mkdirSync(d, { recursive: true });
  fs.mkdirSync(path.join(dirs.home, '.config'), { recursive: true });
  fs.mkdirSync(FLAGS.out, { recursive: true });
  const children = [];
  let cdp = null, browser = null;
  const killAll = () => {
    if (cdp) cdp.close();
    if (browser) browser.proc.kill();
    for (const c of children) c.kill();
    if (!FLAGS.keep) fs.rmSync(tmp, { recursive: true, force: true });
  };
  const report = {
    generated: new Date().toISOString(), node: process.version, out: FLAGS.out,
    rule: { where: 'checkThemeName in tools/orb-manager/sitelimits.go', re: '^[A-Za-z0-9 ._-]+$',
      mirror: 'checkName in web/runtime/theme@1/session.js (the editor asks before it posts)',
      refusal: 'a design\u2019s name can use only letters (A\u2013Z, a\u2013z), numbers, spaces, dots, underscores and dashes' },
    seed: [], entriesAtStart: null, names: [],
  };
  try {
    say('rename-live: building hub and orbfake from this tree (go)…');
    for (const [out, pkg] of [['orb-manager', '.'], ['orbfake', './cmd/orbfake']]) {
      const r = spawnSync3(process.env.GO || 'go', ['build', '-o', path.join(dirs.bin, out), pkg], GO_ROOT);
      if (r.status !== 0) throw new Error('go build ' + pkg + ': ' + String(r.stderr || r.error || '').slice(0, 400));
    }

    // the clock app on the shelf; three clock designs filed into the library as the hub would
    fs.copyFileSync(SHELF_CLOCK, path.join(dirs.apps, 'clock.orbapp'));
    const designsDir = path.join(path.dirname(dirs.themes), 'designs', 'clock'); // <root>/designs/<app>
    report.seed = seedDesignFiles(designsDir, NAMES);
    for (const s of report.seed) say('  seeded ' + s.name + ' -> ' + s.fileName + ' (' + s.bytes + ' B)');

    const ctlPort = await freePort(), edPort = await freePort(), fakePort = await freePort();
    const orb = spawnLogged(path.join(dirs.bin, 'orbfake'),
      ['-addr', '127.0.0.1:' + fakePort, '-fw', 'v1.2.30'], { log: path.join(dirs.logs, 'orbfake.log') });
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

    await waitFor('orbfake status', 40000, () => httpJSON('http://127.0.0.1:' + fakePort + '/api/v1/status', 2000).catch(() => null));
    const indexHtml = await waitFor('hub index', 40000, async () => {
      const r = await fetch('http://127.0.0.1:' + ctlPort + '/', { signal: AbortSignal.timeout(2000) }).catch(() => null);
      if (!r || !r.ok) return null; return await r.text();
    });
    const token = (indexHtml.match(/window\.__ORB_TOKEN__ = "([^"]+)"/) || [])[1];
    if (!token) throw new Error('no launch token in the served index.html');
    const api = async (p, opts) => {
      const base = Object.assign({}, opts || {}); delete base.headers;
      const r = await fetch('http://127.0.0.1:' + ctlPort + p, Object.assign({ signal: AbortSignal.timeout(120000) }, base,
        { headers: Object.assign({ 'X-Orb-Token': token }, (opts || {}).headers || {}) }));
      const t = await r.text(); let j = null; try { j = JSON.parse(t); } catch (e) { /* raw below */ }
      return { status: r.status, env: j, text: t };
    };

    // the Themes tab lists the apps an orb wears: adopt the fake and install the clock so the
    // Themes face stands up with a clock row whose Edit reaches the hub's clock editor.
    const add = await api('/api/orb/add', { method: 'POST', body: JSON.stringify({ ip: '127.0.0.1' }), headers: { 'Content-Type': 'application/json' } });
    if (!add.env || add.env.ok !== true) throw new Error('adopt 127.0.0.1: ' + add.text.slice(0, 200));
    const inst = await api('/api/library/install?ip=127.0.0.1&only=clock', { method: 'POST' });
    if (inst.status !== 200 || /"ok"\s*:\s*false/.test(inst.text)) throw new Error('install clock: ' + inst.text.slice(0, 200));

    // what the hub answers for the library's clock entries before any owner touches one
    const entries0 = await api('/api/library/entries?app=clock');
    report.entriesAtStart = { status: entries0.status, env: entries0.env, text: entries0.text.slice(0, 4000) };

    // ---- browser: one tab on the hub, CDP taps on every context that appears
    browser = await launchChromium(dirs.profile, path.join(dirs.logs, 'chrome.log'));
    const version = await httpJSON('http://127.0.0.1:' + browser.port + '/json/version', 5000);
    cdp = new CDP(version.env.webSocketDebuggerUrl);
    await cdp.open;
    const driver = new Driver(cdp, edPort);
    await driver.start();
    const page = await driver.openFirstPage('http://127.0.0.1:' + ctlPort + '/');
    await waitFor('hub page booted', 30000, async () =>
      await driver.evalIn(page, '!!window.editorFrameSrc && !!window.loadThemes && !!document.querySelector("#mn-themes")').catch(() => null));

    // ---- the owner's flow, per name (a failure on one name is recorded, the run carries on)
    for (const n of NAMES) {
      const found = { label: n.label, name: n.name, basic: n.basic, designFile: designFile(n.name),
        opened: false, openWay: null, sheet: null, entryListedAtOpen: null,
        statusAfterOpen: [], statusAfterSave: [], statusAfter3s: [],
        requests: [], saveVerdict: null, visibleAfterSave: null,
        screenshots: {}, afterForget: null, note: null };
      report.names.push(found);
      try { found.note = await flowForName(driver, page, api, edPort, n, found); }
      catch (e) {
        found.note = 'flow failed: ' + String((e && e.message) || e).slice(0, 500);
        say('  ' + n.name + ': ' + found.note);
        check('the whole flow for ' + n.name + ' completed', false, found.note);
      }
    }

    report.capturedWrites = driver.requests.length;
    report.checks = { total: nChecks, failed: nFails };
    fs.writeFileSync(path.join(FLAGS.out, 'report.json'), JSON.stringify(report, null, 2) + '\n');
    const pngs = fs.readdirSync(FLAGS.out).filter((f) => f.endsWith('.png'));
    say('rename-live: report.json + ' + pngs.length + ' PNG(s) under ' + FLAGS.out);
    killAll();
    // the floor is the point: a run that bailed out early counted nothing and may not pass
    if (nChecks < 30) check('the run reached its full set of checks', false, nChecks + ' counted');
    say('rename-live: ' + (nChecks - nFails) + '/' + nChecks + ' passed');
    process.exit(nFails === 0 ? 0 : 1);
  } catch (e) {
    report.error = String((e && e.stack) || e);
    try { fs.writeFileSync(path.join(FLAGS.out, 'report.json'), JSON.stringify(report, null, 2) + '\n'); } catch (w) { /* best effort */ }
    say('rename-live run failed: ' + String((e && e.stack) || e));
    for (const l of ['hub.log', 'orbfake.log', 'chrome.log']) {
      try { say('--- ' + l + ' (tail) ---\n' + fs.readFileSync(path.join(dirs.logs, l), 'utf8').split('\n').slice(-30).join('\n')); } catch (err) { /* none yet */ }
    }
    killAll();
    process.exit(1);
  }
}

main().catch((e) => { say('rename-live fatal: ' + (e && e.stack || e)); process.exit(1); });
