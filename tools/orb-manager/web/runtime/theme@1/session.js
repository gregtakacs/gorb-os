/* theme@1/session — an editor's session: history, the app's library on the hub, and the
 * design document (theme-editor-kit-plan K2, K9, K10).
 *
 * Any app's editor gets, without carrying it:
 *   - undo and redo over snapshots of the keys the app names, with a burst of edits (a
 *     slider drag, a typed number) merged into one step;
 *   - the library (K10): the app's entries on the hub, one per name — a draft, a theme,
 *     or a theme with unsent changes — saved, opened at their newest design, discarded or
 *     forgotten, and Rebake all for themes baked in a format the orb no longer reads. The
 *     hub's disk is the store (K9): nothing depends on this browser, there is no autosave,
 *     and a reload starts from a preset or a library entry;
 *   - what the hub serves: whether this editor is newer than the app on the orb, said
 *     before a send, and a note when a design opened was made by an older editor;
 *   - the design document's envelope (format, schema, app, app version, made), the
 *     migration walk (composer-stack-plan ruling 12), and its refusals: another app's
 *     design, a newer schema, or an older one with no step (ruling 21: only the schema says
 *     what an editor can read, so a design from a newer release of the app opens);
 *   - opening a design from a file (an .orbtheme's design region, or a design file), a
 *     library entry (?entry=) or a card theme (?theme=), and the theme download;
 *   - on a site that asks for it (GorbHost's assets and designIds flags): art by hash, so the
 *     site stores each image once and a design carries refs; and saving by design id, so Save
 *     overwrites the design this page opened and a renamed Save makes a new one;
 *   - a site's shared link (?shared=<id>&download=1): a design bundle in, baked here under the
 *     shared name <name> by @<handle> and downloaded, with nothing of it saved.
 *   - on the hub, an import (?import=<ticket>): a site's design bundle the hub fetched, baked here
 *     under its shared name and kept in the hub's library with where it came from (orb-os-net §7.1).
 *   - on a site, a save it refused because its editor was updated (orb-os-net §6.5): the design goes
 *     into this tab's sessionStorage, the tab reloads into the version the site named, and that
 *     editor opens the design back through the same checks and migrations any open runs.
 *
 * Everything is per app (?app=, else the host's slug), so two apps' editors never read
 * each other's designs. Plain JS, no React. The app keeps what only it knows:
 * which state is the design, how to check a restored one, and what the design holds.
 *
 *   const ses = OrbTheme.session({
 *     slug,                        // the app's slug when the page names none (?app=)
 *     keys(),                      // the state keys one snapshot holds (read on each call)
 *     state(), set(patch),         // the editor's state, and a state update
 *     ready(),                     // false until the editor can render a design
 *     say(text),                   // the status line
 *     restore(snap, text),         // put a snapshot back (undo/redo); the app keeps its selection rules
 *     burstMs(), undoMax(),        // optional: the burst window (600 ms) and the depth (100)
 *     schema(), migrate(),         // the design schema this editor reads — the build stamps
 *                                  // editor.json's in __ORB_APP.schema and the session
 *                                  // refuses to start if the two differ — and its steps {n: fn}
 *     content(withArt),            // async: the design's own fields, merged into the envelope
 *     apply(doc, from),            // put a checked design into the editor: null, or why not
 *     download(blob, name),        // optional: the page's download (default OrbTheme.download)
 *     bake(),                      // optional, async: the open design baked to .orbtheme bytes, or
 *                                  // null (the app's status says why); Rebake all needs it
 *   });
 */
(function (root) {
  'use strict';
  var OrbTheme = root.OrbTheme = root.OrbTheme || {};

  // GorbHost: the page sets window.GorbHost before the editor loads; with no GorbHost, or
  // kind "hub", nothing changes (the Orb Manager default). Under kind "web" the hub's paths
  // sit under another base and Send to orb becomes a download; editors read the flags (send,
  // publish, inbox, assets, designIds) through OrbTheme.hostInfo() and never kind
  // (orb-os-net design §6.3).
  // ---- GorbHost: keep this block byte-identical in theme@1/send.js and theme@1/session.js
  OrbTheme.hostInfo = OrbTheme.hostInfo || function () {
    var raw = root.GorbHost;
    if (!raw || typeof raw !== 'object' || Array.isArray(raw)) raw = {};
    var kind = raw.kind === 'web' ? 'web' : 'hub';
    var api = typeof raw.api === 'string' &&
      /^\/[A-Za-z0-9_~-][A-Za-z0-9._~-]*(?:\/[A-Za-z0-9_~-][A-Za-z0-9._~-]*)*$/.test(raw.api) &&
      !raw.api.split('/').some(function (s) { return s === '.' || s === '..'; })
      ? raw.api : (kind === 'web' ? '/studio-api' : '/api');
    var send = typeof raw.send === 'boolean' ? raw.send : kind === 'hub';
    return Object.freeze({ kind: kind, api: api, send: send, publish: raw.publish === true, inbox: raw.inbox === true, assets: raw.assets === true, designIds: raw.designIds === true });
  };
  // a hub path, its leading api segment removed, at this host's base
  OrbTheme.hostUrl = OrbTheme.hostUrl || function (p) { return OrbTheme.hostInfo().api + p; };
  // ---- end GorbHost
  // the host's words: an editor passes the hub's sentence and the site's, and never reads kind
  OrbTheme.hostText = OrbTheme.hostText || function (hub, web) { return OrbTheme.hostInfo().kind === 'web' ? web : hub; };

  // a file the browser saves: the one download every editor's export goes through
  OrbTheme.download = function (blob, name) {
    var a = document.createElement('a'); a.href = URL.createObjectURL(blob); a.download = name;
    document.body.appendChild(a); a.click(); a.remove(); setTimeout(function () { URL.revokeObjectURL(a.href); }, 4000);
  };
  /* a file saved where the person chooses (§8.1 ruling 6): the browser's Save dialog where it
   * has one (the app window's Chrome does: 127.0.0.1 is a secure context), else the download.
   * Answers 'saved', 'downloaded', or 'cancelled' when the dialog was closed. */
  OrbTheme.saveFile = async function (blob, name) {
    if (typeof root.showSaveFilePicker !== 'function') { OrbTheme.download(blob, name); return 'downloaded'; }
    var fh;
    try {
      fh = await root.showSaveFilePicker({ suggestedName: name, types: [{ description: 'Orb theme', accept: { 'application/octet-stream': ['.orbtheme'] } }] });
    } catch (e) {
      if (e && e.name === 'AbortError') return 'cancelled';
      OrbTheme.download(blob, name); return 'downloaded';
    }
    var w = await fh.createWritable(); await w.write(blob); await w.close();
    return 'saved';
  };

  OrbTheme.session = function (host) {
    var api = {
      undoStack: [], redoStack: [],
      snap: null, snapAt: 0,       // the newest snapshot, and when the burst it belongs to began
      restoring: false,
      orbNote: '',                 // set by boot() when the orb's app is behind the hub's
      lastMade: null,              // the older release a design just opened was made by, else null
      opening: null,               // while a design is on its way in, the line saying so; else null
      opened: null,                // under a host that saves by id: the site design this holds, {id, name}
      sharedName: null,            // the shared name a design was opened under, while it stands
    };
    var lib = [];
    var say = function (s) { host.say(s); };
    var keys = function () { return host.keys(); };
    var burstMs = function () { return host.burstMs ? host.burstMs() : 600; };
    var undoMax = function () { return host.undoMax ? host.undoMax() : 100; };
    var download = function (blob, name) { return (host.download || OrbTheme.download)(blob, name); };
    // ?shared=<design id> (a site's shared link): the id, when this page was opened for one
    var sharedOf = function (q) {
      var s = (q && q.get('shared')) || '';
      return OrbTheme.hostInfo().kind === 'web' && /^[A-Za-z0-9_-]{22}$/.test(s) ? s : null;
    };
    // ?import=<ticket> (the hub's Import from orb-os.net): the ticket, when this page was opened
    // on the hub for one; a site page ignores it entirely
    var importOf = function (q) {
      var t = (q && q.get('import')) || '';
      return OrbTheme.hostInfo().kind === 'hub' && /^[0-9a-f]{32}$/.test(t) ? t : null;
    };
    /* An open is a fetch and a decode, megabytes off an orb's card for a converted theme, and
     * until it lands the start sheet's presets sit under the pointer: a click there picks a dial
     * the open then overwrites. The editor covers itself while api.opening holds a line. Every
     * open clears it however it ends. The hub's Edit door (?theme=) and ?entry= start covered:
     * the fonts and the library load before boot reaches the open. */
    var opening = function (line) { api.opening = line || null; host.set({}); };
    (function () {
      var q = null; try { q = new URLSearchParams(root.location.search); } catch (e) { q = null; }
      if (!q) return;
      if (sharedOf(q)) api.opening = 'Opening the shared design…';
      else if (importOf(q)) api.opening = 'Opening the imported theme…';
      else if (/^[A-Za-z0-9_.-]{1,64}$/.test(q.get('theme') || '')) api.opening = OrbTheme.hostInfo().kind === 'web' ? 'Opening the design…' : 'Opening the theme’s design from the ' + (q.get('src') === 'shelf' ? 'hub’s shelf' : 'card') + '…';
      else if (q.get('entry')) api.opening = 'Opening “' + q.get('entry') + '” from the library…';
    })();

    /* design §6.5 point 1: the schema this editor reads and the one its editor.json names are
     * one fact stated twice, so the build stamps the latter beside the release and a session
     * that disagrees with itself never starts. No stamp (an unpackaged editor, an app built
     * before the SDK wrote one, a node suite) and a stamp with no schema both skip it. */
    var stamp = root && typeof root.__ORB_APP === 'object' ? root.__ORB_APP : null;
    if (stamp && stamp.schema !== undefined && stamp.schema !== host.schema())
      throw new Error('this editor reads design schema ' + host.schema() + ', but its editor.json says ' + stamp.schema + ': the two must agree — set editor.json’s schema to the editor’s and rebuild the app');

    api.slug = function () {
      var q = null; try { q = new URLSearchParams(location.search); } catch (e) { q = null; }
      var a = q && q.get('app');
      return a && /^[a-z0-9_-]{1,32}$/i.test(a) ? a : host.slug;
    };
    // the release the region build stamped (window.__ORB_APP), or 'dev' for an unpackaged editor
    api.version = function () { var a = typeof root !== 'undefined' && root.__ORB_APP; return (a && a.version) || 'dev'; };
    // the keys this browser held before K9: read once to move My designs, then dropped
    api.sessionKey = function () { return 'wc.session.' + api.slug(); };
    // the design a site's stale-editor refusal handed to this tab (orb-os-net §6.5)
    api.handoffKey = function () { return 'orb.handoff.' + api.slug(); };
    api.mineKey = function () { return 'wc.mine.' + api.slug(); };
    // the orb this page was served for: /editor/<ip>/<slug>/…, or null off the hub. Under a
    // web host there is no orb, and the site's library stands where the hub's does.
    api.hubOrb = function () {
      if (OrbTheme.hostInfo().kind === 'web') return { ip: null, slug: api.slug() };
      var m = /^\/editor\/(\d{1,3}(?:\.\d{1,3}){3})\/([a-z0-9_-]{1,32})\//i.exec(String((root.location && root.location.pathname) || ''));
      return m ? { ip: m[1], slug: m[2] } : null;
    };
    // a hub route's {ok, data} answer, or a throw carrying the hub's own sentence. A site's
    // refusal naming a newer editor (orb-os-net M1) rides on the throw as e.updated.
    var VER = /^v\d+\.\d+\.\d+$/;
    var hubJson = async function (path, opts) {
      var r = await fetch(path, opts), j = null;
      try { j = await r.json(); } catch (e) { j = null; }
      if (!r.ok || !j || j.ok === false) {
        var x = new Error((j && j.error) || OrbTheme.hostText('the hub answered ' + r.status, 'the server answered ' + r.status));
        if (j && j.updated && typeof j.updated === 'object' && !Array.isArray(j.updated) &&
          typeof j.updated.version === 'string' && VER.test(j.updated.version)) x.updated = { version: j.updated.version };
        throw x;
      }
      return j.data;
    };

    /* ---- art by hash (orb-os-net W4b): under a host that sets the assets flag the site keeps
     * every image once, by its sha256, and a design carries refs (`orb-asset:<hex>`) plus a
     * top-level `orbAssets` sidecar instead of bytes. An open inflates the refs back to data
     * URLs, checking each answer's hash; a save sends what the session has already seen as refs
     * with their sidecar, and every other image inline for the site to upload — after which it
     * is known too, so the next save sends a ref. Never under the hub. */
    var known = new Map();                     // the exact data URL a save can send as a ref
    var b64 = function (bytes) {               // chunked: a megabyte image would overflow apply
      var s = '', CH = 32768;
      for (var i = 0; i < bytes.length; i += CH) s += String.fromCharCode.apply(null, bytes.subarray(i, i + CH));
      return btoa(s);
    };
    var unb64 = function (t) { var b = atob(t), u = new Uint8Array(b.length); for (var i = 0; i < b.length; i++) u[i] = b.charCodeAt(i); return u; };
    var hexOf = function (buf) { var u = new Uint8Array(buf), s = ''; for (var i = 0; i < u.length; i++) s += (u[i] < 16 ? '0' : '') + u[i].toString(16); return s; };
    var shaHex = async function (bytes) { return hexOf(await crypto.subtle.digest('SHA-256', bytes)); };
    var utf8Len = function (s) { try { return new TextEncoder().encode(s).length; } catch (e) { return s.length; } };
    // a string the site would store as an asset: a data URL whose base64 payload decodes clean
    // (re-encoding gives it back exactly) and holds at least 512 bytes; anything else stays put
    var assetOf = function (s) {
      if (typeof s !== 'string' || s.slice(0, 5) !== 'data:') return null;
      var c = s.indexOf(','); if (c < 0) return null;
      var prefix = s.slice(0, c);
      if (prefix.slice(-7) !== ';base64') return null;
      var pay = s.slice(c + 1), bytes = null;
      try { bytes = unb64(pay); } catch (e) { return null; }
      if (!bytes || bytes.length < 512 || b64(bytes) !== pay) return null;
      return { prefix: prefix, bytes: bytes };
    };
    var assetBytes = async function (h, source) {
      if (source) {
        var t = source.assets && typeof source.assets[h] === 'string' ? source.assets[h] : null;
        if (!t) throw new Error('the bundle is missing an image');
        try { return unb64(t); } catch (e) { throw new Error('the bundle is missing an image'); }
      }
      var r = await fetch(OrbTheme.hostUrl('/asset?sha256=' + h));
      if (!r.ok) throw new Error('could not fetch an image of this design (' + r.status + ')');
      return new Uint8Array(await r.arrayBuffer());
    };
    var putRefs = function (v, byRef) {
      if (Array.isArray(v)) { for (var i = 0; i < v.length; i++) { var x = v[i]; if (typeof x === 'string') { if (byRef.has(x)) v[i] = byRef.get(x); } else putRefs(x, byRef); } }
      else if (v && typeof v === 'object') { for (var k in v) { if (!Object.prototype.hasOwnProperty.call(v, k)) continue; var y = v[k]; if (typeof y === 'string') { if (byRef.has(y)) v[k] = byRef.get(y); } else putRefs(y, byRef); } }
      return v;
    };
    // a host document back in editor form: every ref replaced by the bytes it stands for, the
    // sidecar gone. `source` is a shared design bundle (its assets are base64 in the bundle);
    // without one the bytes come from the host, one fetch per image.
    var inflate = async function (o, source) {
      var list = o && Array.isArray(o.orbAssets) ? o.orbAssets : null;
      if (list) delete o.orbAssets;
      if (!list || !list.length) return o;
      var byRef = new Map();
      for (var i = 0; i < list.length; i++) {
        var e = list[i], h = e && typeof e.sha256 === 'string' ? e.sha256 : '';
        if (!/^[0-9a-f]{64}$/.test(h)) continue;
        if (typeof e.prefix !== 'string' || e.prefix.slice(-7) !== ';base64') continue;
        var bytes = await assetBytes(h, source);
        if (await shaHex(bytes) !== h) throw new Error('an image of this design came back changed');
        var url = e.prefix + ',' + b64(bytes);
        known.set(url, { sha256: h, prefix: e.prefix, size: bytes.length });
        byRef.set('orb-asset:' + h, url);
      }
      return putRefs(o, byRef);
    };
    // the save body, walked in place: known data URLs become refs and their sidecar entries.
    // Returns the data URLs this save sent inline, so they join `known` once it landed.
    var refify = async function (body) {
      var strs = [];
      (function walk(v) {
        if (Array.isArray(v)) { for (var i = 0; i < v.length; i++) { var x = v[i]; if (typeof x === 'string') strs.push([v, i, x]); else walk(x); } }
        else if (v && typeof v === 'object') { for (var k in v) { if (!Object.prototype.hasOwnProperty.call(v, k)) continue; var y = v[k]; if (typeof y === 'string') strs.push([v, k, y]); else walk(y); } }
      })(body);
      var side = new Map(), fresh = new Map();
      for (var j = 0; j < strs.length; j++) {
        var s = strs[j][2], e = known.get(s);
        if (e) {
          side.set(e.sha256, { sha256: e.sha256, prefix: e.prefix, size: e.size });
          strs[j][0][strs[j][1]] = 'orb-asset:' + e.sha256;
          continue;
        }
        var a = assetOf(s);
        if (!a) continue;
        if (!fresh.has(s)) fresh.set(s, { sha256: await shaHex(a.bytes), prefix: a.prefix, size: a.bytes.length });
      }
      if (side.size) body.orbAssets = Array.from(side.values()).sort(function (a, b) { return a.sha256 < b.sha256 ? -1 : a.sha256 > b.sha256 ? 1 : 0; });
      else delete body.orbAssets;
      return Array.from(fresh, function (kv) { return [kv[0], kv[1]]; });
    };

    /* ---- history. A snapshot is the named keys' values by reference: an editor that
     * replaces the value an edit touches makes a snapshot free and a changed key one !==. */
    api.snapOf = function (st) { var o = {}; keys().forEach(function (k) { o[k] = st[k]; }); return o; };
    api.snapSame = function (a, b) { return !!a && !!b && keys().every(function (k) { return a[k] === b[k]; }); };
    // call on every update: a changed design is an undo step, or joins the burst it is part of
    api.track = function () {
      if (!host.ready()) return;
      var cur = api.snapOf(host.state());
      if (!api.snap) { api.snap = cur; return; }
      if (api.snapSame(cur, api.snap)) return;
      var now = Date.now();
      if (api.restoring) api.restoring = false;
      else {
        if (now - api.snapAt > burstMs() || !api.undoStack.length) { api.undoStack.push(api.snap); if (api.undoStack.length > undoMax()) api.undoStack.shift(); }
        api.redoStack = [];
      }
      api.snap = cur; api.snapAt = now;
    };
    // a design opened or picked up is where the session's history starts (the first update adopts it)
    api.restart = function () { api.snap = null; api.undoStack = []; api.redoStack = []; };
    var restore = function (snap, text) { api.restoring = true; api.snapAt = 0; host.restore(snap, text); };
    api.undo = function () {
      if (!api.undoStack.length) { say('Nothing to undo'); return; }
      var prev = api.undoStack.pop(); api.redoStack.push(api.snap || api.snapOf(host.state()));
      restore(prev, 'Undone' + (api.undoStack.length ? '' : ' — back to where this session started'));
    };
    api.redo = function () {
      if (!api.redoStack.length) { say('Nothing to redo'); return; }
      var next = api.redoStack.pop(); api.undoStack.push(api.snap || api.snapOf(host.state()));
      restore(next, 'Redone');
    };

    /* ---- the library (K10): the app's entries on the hub, one per name. The list is fetched
     * and kept (a sheet that lists it renders every frame). An entry is
     *   {name, state: 'draft'|'theme'|'changed', at, draft: {at, appVersion, extra}|null,
     *    theme: {stem, made_with, can_reopen, stale}|null, ...draft.extra}
     * Off the hub there is no library, and the theme download is the way to take one along. */
    var app = function () { return '?app=' + encodeURIComponent(api.slug()); };
    var offHub = OrbTheme.hostText('the library is kept on the hub: open this editor from the hub, or download the .orbtheme',
      'the library is kept by the site: open this editor from one of your designs, or download the .orbtheme');
    var err = function (e) { return e && e.message ? e.message : String(e); };
    var extrasOf = function (x) { var o = {}; Object.keys(x).forEach(function (k) { if (k !== 'name' && k !== 'doc' && k !== 'at') o[k] = x[k]; }); return o; };
    /* Orb Manager's own copy of an app's editor, opened with no orb (orb-os-net ruling 32):
     * /editor/hub/<slug>/ is a hub page whose library is the hub's, with no orb behind it.
     * It answers what a web host answers, {ip: null, slug}. */
    var hubOrbOfPath = api.hubOrb;
    api.hubOrb = function () {
      var o = hubOrbOfPath();
      if (o || OrbTheme.hostInfo().kind === 'web') return o;
      var m = /^\/editor\/hub\/([a-z0-9_-]{1,32})\//i.exec(String((root.location && root.location.pathname) || ''));
      return m ? { ip: null, slug: m[1] } : null;
    };
    var saveDraft = async function (name, doc, extra, asNew) {
      var flags = OrbTheme.hostInfo();
      var body = Object.assign({}, doc, { saved: { extra: extra || {} } });
      var fresh = [];
      if (flags.assets) { body = JSON.parse(JSON.stringify(body)); fresh = await refify(body); }
      var u = OrbTheme.hostUrl('/design/save') + app() + '&name=' + encodeURIComponent(name);
      if (flags.designIds) u += asNew || !api.opened || !api.opened.id ? '&saveAs=1' : '&id=' + encodeURIComponent(api.opened.id);
      var data = await hubJson(u, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
      // the site answers a renamed Save as a Save As: from now on this is that design
      if (flags.designIds && data && typeof data.design === 'string' && data.design) api.opened = { id: data.design, name: name };
      for (var i = 0; i < fresh.length; i++) known.set(fresh[i][0], fresh[i][1]);
      return data;
    };
    api.library = function () { return lib.slice(); };
    api.libraryEntry = function (nm) { return lib.find(function (e) { return e.name === nm; }) || null; };
    api.libraryRefresh = async function () {
      if (!api.hubOrb()) { lib = []; return false; }
      try {
        var l = await hubJson(OrbTheme.hostUrl('/library/entries') + app());
        lib = (Array.isArray(l) ? l : []).map(function (e) { return Object.assign({}, (e.draft && e.draft.extra) || {}, e); });
      } catch (e) { say(OrbTheme.hostText('The library could not be read from the hub: ', 'The library could not be read: ') + err(e)); return false; }
      host.set({});
      return true;
    };
    /* a site that refuses a save because its editor was updated carries the design into the newer
     * one (orb-os-net §6.5): the document rides this tab's sessionStorage, the tab reloads into the
     * version the site named, and that editor opens what it finds there. The url keeps the pathname
     * and drops the query on purpose: the new page opens the stash, not what ?theme= names. */
    var handoffUrl = function (to) {
      var p = String((root.location && root.location.pathname) || ''), s = p.indexOf('/', 1), seg = s < 0 ? p.slice(1) : p.slice(1, s);
      if (!VER.test(seg)) return '/' + to + '/';
      return s < 0 ? '/' + to : '/' + to + p.slice(s);
    };
    // false when the design cannot be carried: no sessionStorage, or one the document is too big for
    var handoff = function (to, entry) {
      var ss = null; try { ss = root.sessionStorage; } catch (e) { ss = null; }
      if (!ss || typeof ss.setItem !== 'function') return false;
      try {
        ss.setItem(api.handoffKey(), JSON.stringify({ format: 1, app: api.slug(), to: to, from: api.version(),
          name: entry.name, doc: entry.doc, extra: extrasOf(entry), opened: api.opened || null, at: Date.now() }));
      } catch (e) { return false; }
      return true;
    };
    // entry: {name, doc, ...anything the app lists by}. A draft under a theme's name sits beside
    // it as that theme's unsent changes; under a draft's name it replaces it.
    // quiet (Send to orb's first step): no line unless it fails, so the send's own lines stand
    // asNew: always a new design, whatever this session was opened from (Save As, and a renamed
    // Save under a host that saves by id — the site makes the new design and answers its id)
    var saveEntry = async function (entry, quiet, asNew) {
      if (!api.hubOrb()) { if (!quiet) say('Not saved: ' + offHub); return false; }
      try { await saveDraft(entry.name, entry.doc, extrasOf(entry), asNew); }
      catch (e) {
        // a site's stale refusal is the one that costs nothing: hand the design to the new editor
        var to = OrbTheme.hostInfo().kind === 'web' && e && e.updated ? e.updated.version : null;
        if (!to) { say('Not saved: ' + err(e)); return false; }
        if (!handoff(to, entry)) { say('Not saved: ' + err(e) + ', and this design is too big to carry over to it: use Export to keep it as a file, then reload and import it'); return false; }
        say('Not saved: ' + err(e) + '. Reopening your design in ' + to + '…');
        root.location.replace(handoffUrl(to));
        return false;
      }
      await api.libraryRefresh();
      if (quiet) return true;
      var e = api.libraryEntry(entry.name);
      say('Saved “' + entry.name + '” to the library' + (e && e.state === 'changed'
        ? OrbTheme.hostText(' as unsent changes to its theme — Send to orb bakes them in', ' as unsent changes to its theme — Download .orbtheme bakes them in')
        : ' as a draft'));
      return true;
    };
    api.saveDraft = function (entry, quiet) { return saveEntry(entry, quiet, false); };
    api.saveAs = function (entry, quiet) { return saveEntry(entry, quiet, true); };
    // an entry's newest design (its draft, else its theme's), or its theme's with from='theme'
    var designOf = async function (nm, from) {
      var u = OrbTheme.hostUrl('/library/design') + app() + '&name=' + encodeURIComponent(nm) + (from ? '&from=' + from : '') + (OrbTheme.hostInfo().assets ? '&refs=1' : '');
      var r = await fetch(u);
      var o = null; try { o = await r.json(); } catch (e) { o = null; }
      if (!r.ok || !o || o.format !== 'wc-design') throw new Error((o && o.error) || OrbTheme.hostText('the hub has no design for “' + nm + '”', 'the server has no design for “' + nm + '”'));
      if (OrbTheme.hostInfo().assets) o = await inflate(o, null);
      return o;
    };
    api.openEntry = async function (nm) {
      opening('Opening “' + nm + '” from the library…');
      try { return await openEntry(nm); } finally { opening(null); }
    };
    var openEntry = async function (nm) {
      if (!api.hubOrb()) { say(offHub); return false; }
      var o; try { o = await designOf(nm); } catch (e) { say('Could not open “' + nm + '”: ' + err(e)); return false; }
      var why = api.apply(o, 'the library');
      if (why) { say('Could not open “' + nm + '”: ' + why); return false; }
      api.restart();
      if (OrbTheme.hostInfo().designIds) {
        var ent = api.libraryEntry(nm);
        api.opened = ent && ent.design ? { id: ent.design, name: nm } : null;
      }
      if (!api.lastMade) say('Opened “' + nm + '” from the library');
      api.sayOpened('“' + nm + '”');
      return true;
    };
    // what: 'draft' discards a theme's unsent changes; 'all' takes the entry out of the library
    api.forgetEntry = async function (nm, what) {
      if (!api.hubOrb()) { say(offHub); return false; }
      try { await hubJson(OrbTheme.hostUrl('/library/forget') + app() + '&name=' + encodeURIComponent(nm) + '&what=' + (what === 'draft' ? 'draft' : 'all'), { method: 'POST' }); }
      catch (e) { say('Not removed: ' + err(e)); return false; }
      await api.libraryRefresh();
      say(what === 'draft' ? 'Discarded the unsent changes to “' + nm + '”' : 'Removed “' + nm + '” from the library');
      return true;
    };
    /* Rebake all: every theme baked in a format the orb no longer reads, rebaked from the
     * design it carries (the theme's, never unsent changes beside it) and kept on the shelf in
     * its place, without sending. One at a time, said as it goes; nothing rebakes on its own. */
    api.rebakeAll = async function () {
      if (!api.hubOrb()) { say(offHub); return null; }
      if (!host.bake) { say('This editor cannot rebake its themes'); return null; }
      await api.libraryRefresh();
      var todo = lib.filter(function (e) { return e.theme && e.theme.stale; });
      if (!todo.length) { say('Nothing in the library needs a rebake'); return { done: [], failed: [] }; }
      var done = [], failed = [];
      for (var i = 0; i < todo.length; i++) {
        var e = todo[i], tag = (i + 1) + ' of ' + todo.length + ' — “' + e.name + '”';
        if (!e.theme.can_reopen) { failed.push(e.name + ' (it carries no design to rebake from)'); continue; }
        say('Rebaking ' + tag + '…');
        try {
          var o = await designOf(e.name, 'theme');
          var why = api.apply(o, 'the library');
          if (why) throw new Error(why);
          api.restart();
          var bytes = await host.bake();
          if (!bytes) throw new Error(String(host.state().status || 'the bake did not finish'));
          var kept = await hubJson(OrbTheme.hostUrl('/library/keep') + app() + '&fold=0', { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: bytes });
          // the bake names itself from the design; one that came out under another name is a
          // second entry, and the stale one still stands
          if (kept && kept.name && kept.name !== e.name) throw new Error('it baked as “' + kept.name + '”, so the stale “' + e.name + '” is still in the library');
          done.push(e.name);
        } catch (x) { failed.push(e.name + ' (' + err(x) + ')'); }
      }
      await api.libraryRefresh();
      say('Rebaked ' + done.length + ' of ' + todo.length + (done.length ? ': ' + done.join(', ') : '') +
        (failed.length ? ' · not rebaked: ' + failed.join('; ') : '') + (done.length ? ' — Install puts a rebaked theme on an orb' : ''));
      return { done: done, failed: failed };
    };

    /* the other half of a site's stale-editor refusal (orb-os-net §6.5): the design an old tab
     * stashed opens here, in the editor the site reloaded this tab into. The key goes before the
     * apply, so a design this editor cannot read never comes back to loop on the next load. A hub
     * neither writes nor reads it: nothing on the hub goes stale under a tab's feet. */
    var handBack = function () {
      var ss = null; try { ss = root.sessionStorage; } catch (e) { ss = null; }
      if (!ss || typeof ss.getItem !== 'function') return false;
      var raw = null; try { raw = ss.getItem(api.handoffKey()); } catch (e) { return false; }
      if (typeof raw !== 'string') return false;
      try { ss.removeItem(api.handoffKey()); } catch (e) { /* already gone */ }
      var stash = null; try { stash = JSON.parse(raw); } catch (e) { stash = null; }
      if (!stash || stash.format !== 1 || stash.app !== api.slug()) return false;
      var why = api.apply(stash.doc, 'the design you were editing');
      if (why) { say('Your design from ' + stash.from + ' could not be reopened in ' + api.version() + ': ' + why); return false; }
      api.restart();
      var o = stash.opened;
      api.opened = o && typeof o === 'object' && typeof o.id === 'string' && /^[A-Za-z0-9_-]{22}$/.test(o.id) && typeof o.name === 'string' ? o : null;
      say(api.slug() + "'s editor was updated to " + api.version() + ': your design is back as you left it. Save it again to keep it');
      return true;
    };

    /* ---- boot: drop the browser's old autosave; under the hub, move any My designs this
     * browser still holds from before K9 into the library as drafts (references only: they
     * never held their art), read the library, ask the hub what the orb runs, then do what
     * the page was opened for: ?entry=<name> opens that entry, ?rebake=1 runs Rebake all.
     * On a site, a design a stale refusal stashed opens first and takes their place. */
    // a boot that throws never reaches the open it was covering for
    api.boot = async function () {
      try { return await boot(); } catch (e) { opening(null); throw e; }
    };
    var boot = async function () {
      try { root.localStorage.removeItem(api.sessionKey()); } catch (e) { /* no storage */ }
      // a shared link opens a design nobody owns: this browser's library has nothing to do with it
      var qs = null; try { qs = new URLSearchParams(root.location.search); } catch (e) { qs = null; }
      if (sharedOf(qs)) return true;
      var hub = api.hubOrb();
      if (!hub) return false;
      var old = null; try { old = JSON.parse(root.localStorage.getItem(api.mineKey()) || 'null'); } catch (e) { old = null; }
      if (Array.isArray(old)) {
        var left = old.filter(function (x) { return x && x.name && x.doc; }), moved = 0;
        for (var i = 0; i < left.length; i++) {
          try { await saveDraft(left[i].name, left[i].doc, extrasOf(left[i])); moved++; }
          catch (e) { /* kept in the browser for the next boot */ }
        }
        if (moved === left.length) { try { root.localStorage.removeItem(api.mineKey()); } catch (e) { /* no storage */ } }
      }
      await api.libraryRefresh();
      if (OrbTheme.hostInfo().kind !== 'web') try {
        var src = await hubJson(OrbTheme.hostUrl('/editor/source') + '?ip=' + encodeURIComponent(hub.ip) + '&slug=' + encodeURIComponent(hub.slug));
        // an orb behind the hub keeps its own editor, so its bakes are ones its app reads
        api.orbNote = src && src.relation === 'newer' && src.orb_version && src.hub_version
          ? 'this is the orb’s own ' + api.slug() + ' ' + src.orb_version + ' editor; the hub has ' + src.hub_version + ' — update the app on the orb to edit with it' : '';
      } catch (e) { api.orbNote = ''; }
      if (api.orbNote) say('Heads up: ' + api.orbNote);
      var q = null; try { q = new URLSearchParams(root.location.search); } catch (e) { q = null; }
      // a design a stale refusal handed to this version of the editor opens instead of the open
      // this page was asked for, which is whatever the pre-reload tab had been asked for
      if (OrbTheme.hostInfo().kind === 'web' && handBack()) return true;
      // an import page reads its library but opens and rebakes nothing: the import is what it came for
      if (!importOf(q)) {
        var entry = q && q.get('entry');
        if (entry) await api.openEntry(entry);
        else if (q && q.get('rebake') === '1') await api.rebakeAll();
      }
      return true;
    };

    /* ---- the design document. Every theme an editor bakes carries its design after the
     * banks, so it can be opened again from the card or the shelf; a design file is the same
     * document on its own. The OS keeps the bytes and never reads past the envelope. */
    api.doc = async function (withArt) {
      return Object.assign({ format: 'wc-design', schema: host.schema(), app: api.slug(), appVersion: api.version(), made: new Date().toISOString() },
        await host.content(withArt));
    };
    // an older schema walked forward one pure step at a time; {doc: null, why} when a step is missing
    api.migrate = function (o) {
      var d = o, n = +o.schema, steps = host.migrate();
      while (n < host.schema()) {
        var step = steps[n];
        if (typeof step !== 'function') return { doc: null, why: 'that design is schema ' + o.schema + ' and this editor has no step from schema ' + n + ' to ' + (n + 1) };
        d = Object.assign(step(JSON.parse(JSON.stringify(d))), { schema: n + 1 }); n++;
      }
      return { doc: d, why: null };
    };
    // the envelope's checks, in the order they are refused: {doc} ready for host.apply, or {why}
    api.check = function (o) {
      var me = api.slug(), schema = host.schema(), n = Number(o && o.schema);
      if (!o || o.format !== 'wc-design') return { why: 'that design is not one this editor wrote' };
      if (o.app && o.app !== me) return { why: 'that design is the ' + o.app + ' app’s, not ' + me + '’s' };
      if (Number.isInteger(n) && n >= 1 && n < schema) { var m = api.migrate(o); if (!m.doc) return { why: m.why }; o = m.doc; }
      if (!(Number.isInteger(n) && n >= 1) || n > schema) return { why: 'that design was made by ' + (o.app || me) + ' ' + (o.appVersion || '?') + ' (design schema ' + o.schema + '); this editor (' + api.version() + ') reads schema ' + schema + ' — ' + OrbTheme.hostText('update the app on this orb', 'update the app on your orb to read it') };
      // the schema alone decides (ruling 21): any change to what a design holds raises it, so
      // a newer release under this schema holds nothing this editor would drop on the next save
      return { doc: o };
    };
    // a design back into the editor: the envelope's refusals, then the app's own. A design an
    // older release made opens, and lastMade keeps that release so the opener can say so
    var relOf = function (v) { var g = /^v(\d+)\.(\d+)\.(\d+)(?:\+[0-9A-Za-z.-]+)?$/.exec(String(v || '')); return g ? [+g[1], +g[2], +g[3]] : null; };
    api.apply = function (o, from) {
      // the shared name a design was opened under (a site's ?shared= link): every apply sets it,
      // so opening anything else — a file, an entry, a fresh dial — puts the editor's own name back
      var sn = o && o.shared && typeof o.shared.name === 'string' ? o.shared.name : '';
      api.sharedName = sn && utf8Len(sn) <= 55 ? sn : null;
      var c = api.check(o);
      var why = c.why || host.apply(c.doc, from);
      var a = relOf(o && o.appVersion), b = relOf(api.version());
      api.lastMade = !why && a && b && (a[0] - b[0] || a[1] - b[1] || a[2] - b[2]) < 0 ? o.appVersion : null;
      return why;
    };
    // after an open: when an older release made the design, say that saving or sending updates it
    api.sayOpened = function (what) {
      if (api.lastMade) say('Opened ' + what + ' — made with ' + api.slug() + ' ' + api.lastMade + '; saving or sending it updates it to ' + api.version());
    };
    // the name an editor bakes under: the shared name while one stands, else its own
    api.bakeName = function (own) { return api.sharedName || own; };
    // open a file: a baked .orbtheme (its design read out of it) or a design file
    api.openFile = async function (file) {
      opening('Opening ' + (file && file.name ? '“' + file.name + '”' : 'the file') + '…');
      try { return await openFile(file); } finally { opening(null); }
    };
    var openFile = async function (file) {
      var G = root.GorbBake, bytes, text = null;
      try { bytes = new Uint8Array(await file.arrayBuffer()); } catch (e) { say('That file could not be read'); return false; }
      if (bytes.length >= 4 && bytes[0] === 0x54 && bytes[1] === 0x52 && bytes[2] === 0x4f && bytes[3] === 0x47) {   // 'TROG': an .orbtheme
        var rd; try { rd = G.readDesign3(bytes); } catch (e) { say('Not opened: ' + (e && e.message ? e.message : String(e))); return false; }
        if (!rd.design) { say('Not opened: ' + rd.why); return false; }
        text = new TextDecoder().decode(rd.design);
      } else text = new TextDecoder().decode(bytes);
      var o = null; try { o = JSON.parse(text); } catch (e) { say('Not opened: that is neither an .orbtheme nor a design file'); return false; }
      var why = api.apply(o, file.name);
      if (why) { say('Not opened: ' + why); return false; }
      api.restart();
      api.opened = null;              // a file is nobody's design: the next save makes one
      api.sayOpened(file.name);
      return true;
    };
    /* import a file (§8.1 ruling 5): into the library first, then open it. An .orbtheme is
     * kept on the shelf as a theme (it folds a draft of its name, as any kept theme does); a
     * design file is saved as a draft, as it came. o: {nameOf(doc), thumb(doc), confirm(name)}:
     * the theme's name in a design, its picture, and whether to replace an entry of that name. */
    // the opening cover goes up only after the replace question: over it, the question cannot be answered
    api.importFile = async function (file, o) {
      try { return await importFile(file, o || {}); } finally { opening(null); }
    };
    var importFile = async function (file, o) {
      var G = root.GorbBake, bytes, text = null, theme = false;
      if (!api.hubOrb()) { say('Not imported: ' + offHub); return false; }
      try { bytes = new Uint8Array(await file.arrayBuffer()); } catch (e) { say('That file could not be read'); return false; }
      if (bytes.length >= 4 && bytes[0] === 0x54 && bytes[1] === 0x52 && bytes[2] === 0x4f && bytes[3] === 0x47) {   // 'TROG': an .orbtheme
        theme = true;
        var rd; try { rd = G.readDesign3(bytes); } catch (e) { say('Not imported: ' + err(e)); return false; }
        if (rd.design) text = new TextDecoder().decode(rd.design);
      } else text = new TextDecoder().decode(bytes);
      var doc = null;
      if (text != null) { try { doc = JSON.parse(text); } catch (e) { doc = null; } }
      if (!theme && !doc) { say('Not imported: that is neither an .orbtheme nor a design file'); return false; }
      var c = doc ? api.check(doc) : { why: null, doc: null };
      if (c.why) { say('Not imported: ' + c.why); return false; }
      var name = c.doc && o.nameOf ? o.nameOf(c.doc) : '';
      if (!theme && !name) { say('Not imported: that design names no theme'); return false; }
      if (name && api.libraryEntry(name) && o.confirm && !(await o.confirm(name))) { say('Not imported: “' + name + '” is still the library’s own'); return false; }
      opening('Importing ' + (file && file.name ? '“' + file.name + '”' : 'the file') + '…');
      try {
        if (theme) {
          var kept = await hubJson('/api/library/keep' + app(), { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: bytes });
          name = (kept && kept.name) || name;
        } else await saveDraft(name, doc, { thumb: o.thumb ? await o.thumb(c.doc) : null });
      } catch (e) { say('Not imported: ' + err(e)); return false; }
      await api.libraryRefresh();
      if (!c.doc) { say('Imported “' + name + '” into the library. It carries no design, so it installs from the Themes tab but cannot be opened here'); return true; }
      var why = api.apply(doc, file.name);
      if (why) { say('Imported “' + name + '” into the library, but it did not open: ' + why); return true; }
      api.restart();
      say('Imported “' + name + '” into the library' + (theme ? ' as a theme' : ' as a draft'));
      api.sayOpened('“' + name + '”');
      return true;
    };
    // ?theme=<card stem> (the hub's Edit door): the design comes through the hub, off the card or the shelf
    api.openTheme = async function () {
      try { return await openTheme(); } finally { opening(null); }
    };
    /* the bake an open hands over: `want` the name it must come out under, `mine` the status-line
     * prefixes that are this session's own (so they are never quoted back as the editor's reason),
     * and `line` the line said before the first bake. Answers {bytes} the bake made, or {why}: the
     * reason it did not, with no prefix on it, for the caller to say in its own sentence. */
    var bakeUnder = async function (want, mine, line) {
      if (!host.bake) return { why: 'this editor cannot bake here' };
      var ownLine = function (s) {
        for (var i = 0; i < mine.length; i++) if (s.slice(0, mine[i].length) === mine[i]) return '';
        return s;
      };
      var was = ''; try { was = ownLine(String((host.state && host.state().status) || '')); } catch (e) { was = ''; }
      say(line);
      // a React editor applies a design on its next render, so the first bake can still name the
      // watch before it: check the header and bake again, three bakes at most
      var vfy = root.GorbBake && typeof root.GorbBake.verifyBundle3 === 'function' ? root.GorbBake.verifyBundle3 : null;
      var bytes = null, got = want;
      for (var attempt = 0; attempt < 3; attempt++) {
        bytes = await host.bake();
        if (bytes === null || bytes === undefined) break;
        got = vfy ? (function (v) { v = v || {}; return String(v.name || (v.header && v.header.name) || ''); })(vfy(bytes)) : want;   // verifyBundle3 answers {header, sections}
        if (got === want || attempt === 2) break;
        await new Promise(function (r) { setTimeout(r, 1000); });
      }
      if (bytes === null || bytes === undefined) {
        var st = ''; try { st = ownLine(String((host.state && host.state().status) || '')); } catch (e) { st = ''; }
        return { why: st || was || 'the bake did not finish' };
      }
      if (got !== want) return { why: 'the bake came out named “' + got + '”' };
      return { bytes: bytes };
    };
    /* ?shared=<design id>&download=1 (a site's share link, no sign-in): the design bundle
     * stands for the design and its art, the editor bakes it under the shared name
     * `<name> by @<handle>` and the file is what other people get. Nothing of it is saved:
     * the site keeps no baked theme, and this session holds no design id. */
    var openShared = async function (q) {
      api.opened = null;
      say('Opening the shared design…');
      var bundle = null;
      try {
        var rb = await fetch(OrbTheme.hostUrl('/shared/bundle?id=' + encodeURIComponent(q.get('shared'))));
        if (!rb.ok) { var se = ''; try { se = (await rb.json()).error || ''; } catch (e) { se = ''; } say('Could not open that design: ' + (se || 'the server answered ' + rb.status)); return false; }
        try { bundle = JSON.parse(new TextDecoder().decode(new Uint8Array(await rb.arrayBuffer()))); } catch (e) { bundle = null; }
        if (!bundle || bundle.format !== 'orb-design-bundle' || bundle.bundle !== 1) { say('Could not open that design: that is not a design bundle'); return false; }
        var sd = await inflate(bundle.design, bundle);
        var why = api.apply(sd, 'the shared design');
        if (why) { say('Could not open that design: ' + why); return false; }
      } catch (e) { say('Could not open that design: ' + (e && e.message ? e.message : String(e))); return false; }
      api.restart();
      api.sayOpened('the shared design');
      if (q.get('download') !== '1') return true;
      var want = api.bakeName(String((sd && (sd.themeName || sd.name || (sd.design && sd.design.name))) || 'design'));
      var mine = ['Opening the shared', 'Baking', 'Downloaded', 'Not downloaded', 'Could not open'];
      var made = await bakeUnder(want, mine, 'Baking “' + want + '” for download…');
      if (made.why) { say('Not downloaded: ' + made.why); return true; }
      download(new Blob([made.bytes], { type: 'application/octet-stream' }), String(want).replace(/[\\/:*?"<>|\x00-\x1f\x7f]/g, '_') + '.orbtheme');
      say('Downloaded “' + want + '”');
      return true;
    };
    /* ?import=<ticket> (the hub's Import from orb-os.net, orb-os-net design §7.1): a site's design
     * bundle the hub fetched and holds for one import. It opens here, bakes under the shared name
     * the bundle carries, and the bake is what the hub files in its library beside where it came
     * from. Nothing is saved as a draft, and the editor stays on the design: it can be looked at,
     * closed, or sent with the editor's own Send. */
    var openImport = async function (q) {
      var ticket = importOf(q);
      api.opened = null;
      say('Opening the imported theme…');
      var bundle = null, sd = null;
      try {
        var rb = await fetch(OrbTheme.hostUrl('/site/bundle?ticket=' + ticket));
        if (!rb.ok) { var se = ''; try { se = (await rb.json()).error || ''; } catch (e) { se = ''; } say('Could not open that theme: ' + (se || 'the hub answered ' + rb.status)); return false; }
        try { bundle = JSON.parse(new TextDecoder().decode(new Uint8Array(await rb.arrayBuffer()))); } catch (e) { bundle = null; }
        if (!bundle || bundle.format !== 'orb-design-bundle' || bundle.bundle !== 1) { say('Could not open that theme: that is not a design bundle'); return false; }
        sd = await inflate(bundle.design, bundle);
        var why = api.apply(sd, 'the imported theme');
        if (why) { say('Could not open that theme: ' + why); return false; }
      } catch (e) { say('Could not open that theme: ' + (e && e.message ? e.message : String(e))); return false; }
      api.restart();
      api.sayOpened('the imported theme');
      var sn = typeof bundle.sharedName === 'string' ? bundle.sharedName : '';
      var want = api.bakeName(String((sd && (sd.themeName || sd.name || (sd.design && sd.design.name))) || 'design'));
      if (want !== sn) { say('Not imported: the design does not carry its shared name “' + sn + '”'); return true; }
      var mine = ['Opening the imported', 'Baking', 'Imported', 'Not imported', 'Could not open'];
      var made = await bakeUnder(want, mine, 'Baking “' + want + '” for the library…');
      if (made.why) { say('Not imported: ' + made.why); return true; }
      var data = null;
      try {
        var rk = await fetch(OrbTheme.hostUrl('/site/keep?ticket=' + ticket), { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: made.bytes });
        var env = null; try { env = await rk.json(); } catch (e) { env = null; }
        if (!rk.ok || !env || env.ok === false) { say('Not imported: ' + ((env && env.error) || 'the hub answered ' + rk.status)); return true; }
        data = env.data || null;
      } catch (e) { say('Not imported: ' + (e && e.message ? e.message : String(e))); return true; }
      say('Imported “' + want + '” into the library');
      await api.libraryRefresh();
      // the page that opened this editor (the Themes tab's import door) takes the name it filed
      try { if (root.parent && root.parent !== root) root.parent.postMessage({ type: 'theme.imported', app: api.slug(), name: want, file: data && data.file }, root.location.origin); } catch (e) { /* nothing asked to be told */ }
      return true;
    };
    var openTheme = async function () {
      var q = null; try { q = new URLSearchParams(location.search); } catch (e) { q = null; }
      if (sharedOf(q)) return await openShared(q);   // a site's share link: sharedOf is web-only
      if (importOf(q)) return await openImport(q);    // the hub's import: importOf is hub-only
      var id = q && q.get('theme'); if (!id || !/^[A-Za-z0-9_.-]{1,64}$/.test(id)) return false;
      if (OrbTheme.hostInfo().kind === 'web') {
        // under a web host the id is a site design, not a card theme: orb and src are ignored
        say('Opening the design…');
        try {
          var rw = await fetch(OrbTheme.hostUrl('/theme/design?id=' + encodeURIComponent(id)) + (OrbTheme.hostInfo().assets ? '&refs=1' : ''));
          if (!rw.ok) { var we = ''; try { we = (await rw.json()).error || ''; } catch (e) { we = ''; } say('Could not open that design: ' + (we || 'the server answered ' + rw.status)); return false; }
          var ow = JSON.parse(new TextDecoder().decode(new Uint8Array(await rw.arrayBuffer())));
          if (OrbTheme.hostInfo().assets) ow = await inflate(ow, null);
          var whyw = api.apply(ow, 'the design');
          if (whyw) { say('Could not open that design: ' + whyw); return false; }
          api.restart();
          if (OrbTheme.hostInfo().designIds) {
            await api.libraryRefresh();
            var ent = lib.filter(function (e) { return e.design === id; })[0];
            api.opened = ent ? { id: id, name: ent.name } : null;
          }
          api.sayOpened('the design');
          return true;
        } catch (e) { say('Could not open that design: ' + (e && e.message ? e.message : String(e))); return false; }
      }
      var ip = q.get('orb') || '', src = q.get('src') === 'shelf' ? 'shelf' : 'card';
      say('Opening the theme’s design from the ' + (src === 'shelf' ? 'hub’s shelf' : 'card') + '…');
      try {
        var r = await fetch(OrbTheme.hostUrl('/theme/design?') + (src === 'shelf' ? 'name=' + encodeURIComponent(id) : 'ip=' + encodeURIComponent(ip) + '&id=' + encodeURIComponent(id)));
        if (!r.ok) { var w = ''; try { w = (await r.json()).error || ''; } catch (e) { w = ''; } say('Could not reopen that theme: ' + (w || 'the hub answered ' + r.status)); return false; }
        var o = JSON.parse(new TextDecoder().decode(new Uint8Array(await r.arrayBuffer())));
        var why = api.apply(o, src === 'shelf' ? 'the hub’s shelf' : 'the card');
        if (why) { say('Could not reopen that theme: ' + why); return false; }
        api.restart();
        // as the library's open does: the "Opening…" line must not outlive the open
        if (!api.lastMade) say('Opened the theme from the ' + (src === 'shelf' ? 'hub’s shelf' : 'card'));
        api.sayOpened('the theme from the ' + (src === 'shelf' ? 'hub’s shelf' : 'card'));
        return true;
      } catch (e) { say('Could not reopen that theme: ' + (e && e.message ? e.message : String(e))); return false; }
    };

    // the migration runner (_runner/migrate.js) hands itself the session and its host here,
    // so it can open designs through the same check and apply a person's Open takes; a page
    // that sets no window.GorbRunner never reaches this
    if (typeof root.GorbRunner === 'function') root.GorbRunner(api, host);

    return api;
  };
})(typeof window !== 'undefined' ? window : globalThis);
