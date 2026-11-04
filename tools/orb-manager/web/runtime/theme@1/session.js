/* theme@1/session — an editor's session: history, My designs, and the design file
 * (theme-editor-kit-plan K2, K9).
 *
 * Any app's editor gets, without carrying it:
 *   - undo and redo over snapshots of the keys the app names, with a burst of edits (a
 *     slider drag, a typed number) merged into one step;
 *   - My designs: named designs kept as files on the hub, art included (K9: the hub's
 *     disk is the store, so nothing an editor keeps depends on this browser; there is
 *     no autosave, and a reload starts from a preset, a saved design or a theme);
 *   - what the hub serves: whether this editor is newer than the app on the orb, said
 *     before a send, and a note when a design opened was made by an older editor;
 *   - the design document's envelope (format, schema, app, app version, made), the
 *     migration walk (composer-stack-plan ruling 12), and its refusals: another app's
 *     design, a schema with no step, a design from a newer release of the app;
 *   - opening a design from a file (an .orbtheme's design region, or a design file) or
 *     from the hub's Edit door (?theme=, the card or the hub's shelf), and downloads.
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
 *     schema(), migrate(),         // the design schema this editor reads, and its steps {n: fn}
 *     content(withArt),            // async: the design's own fields, merged into the envelope
 *     apply(doc, from),            // put a checked design into the editor: null, or why not
 *     download(blob, name),        // optional: the page's download (default OrbTheme.download)
 *   });
 */
(function (root) {
  'use strict';
  var OrbTheme = root.OrbTheme = root.OrbTheme || {};

  // a file the browser saves: the one download every editor's export goes through
  OrbTheme.download = function (blob, name) {
    var a = document.createElement('a'); a.href = URL.createObjectURL(blob); a.download = name;
    document.body.appendChild(a); a.click(); a.remove(); setTimeout(function () { URL.revokeObjectURL(a.href); }, 4000);
  };

  OrbTheme.session = function (host) {
    var api = {
      undoStack: [], redoStack: [],
      snap: null, snapAt: 0,       // the newest snapshot, and when the burst it belongs to began
      restoring: false,
      orbNote: '',                 // set by boot() when this editor is newer than the orb's app
      lastMade: null,              // the older release a design just opened was made by, else null
    };
    var mine = [];
    var say = function (s) { host.say(s); };
    var keys = function () { return host.keys(); };
    var burstMs = function () { return host.burstMs ? host.burstMs() : 600; };
    var undoMax = function () { return host.undoMax ? host.undoMax() : 100; };
    var download = function (blob, name) { return (host.download || OrbTheme.download)(blob, name); };

    api.slug = function () {
      var q = null; try { q = new URLSearchParams(location.search); } catch (e) { q = null; }
      var a = q && q.get('app');
      return a && /^[a-z0-9_-]{1,32}$/i.test(a) ? a : host.slug;
    };
    // the release the region build stamped (window.__ORB_APP), or 'dev' for an unpackaged editor
    api.version = function () { var a = typeof root !== 'undefined' && root.__ORB_APP; return (a && a.version) || 'dev'; };
    // the keys this browser held before K9: read once to move My designs, then dropped
    api.sessionKey = function () { return 'wc.session.' + api.slug(); };
    api.mineKey = function () { return 'wc.mine.' + api.slug(); };
    // the orb this page was served for: /editor/<ip>/<slug>/…, or null off the hub
    api.hubOrb = function () {
      var m = /^\/editor\/(\d{1,3}(?:\.\d{1,3}){3})\/([a-z0-9_-]{1,32})\//i.exec(String((root.location && root.location.pathname) || ''));
      return m ? { ip: m[1], slug: m[2] } : null;
    };
    // a hub route's {ok, data} answer, or a throw carrying the hub's own sentence
    var hubJson = async function (path, opts) {
      var r = await fetch(path, opts), j = null;
      try { j = await r.json(); } catch (e) { j = null; }
      if (!r.ok || !j || j.ok === false) throw new Error((j && j.error) || ('the hub answered ' + r.status));
      return j.data;
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

    /* ---- My designs: design documents filed on the hub by name (K9), art included. The
     * list is fetched and kept (a sheet that lists them renders every frame); off the hub
     * there is nowhere to keep one, and the design file is the way to take it along. */
    var q = function (nm, name) { return '/api/design/' + nm + '?app=' + encodeURIComponent(api.slug()) + (name != null ? '&name=' + encodeURIComponent(name) : ''); };
    var offHub = 'My designs are kept on the hub: open this editor from the hub, or download the design file';
    var err = function (e) { return e && e.message ? e.message : String(e); };
    var extrasOf = function (x) { var o = {}; Object.keys(x).forEach(function (k) { if (k !== 'name' && k !== 'doc' && k !== 'at') o[k] = x[k]; }); return o; };
    var post = function (nm, name, doc) {
      return hubJson(q(nm, name), { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: doc ? JSON.stringify(doc) : '' });
    };
    api.mineList = function () { return mine.slice(); };
    api.mineRefresh = async function () {
      if (!api.hubOrb()) { mine = []; return false; }
      try {
        var l = await hubJson(q('list'));
        mine = (Array.isArray(l) ? l : []).map(function (x) { return Object.assign({}, x.extra || {}, { name: x.name, at: x.at, appVersion: x.appVersion, bytes: x.bytes }); });
      } catch (e) { say('My designs could not be read from the hub: ' + err(e)); return false; }
      host.set({});
      return true;
    };
    // entry: {name, doc, ...anything the app lists by}; a save under a held name replaces it
    api.mineSave = async function (entry) {
      if (!api.hubOrb()) { say('Not saved: ' + offHub); return false; }
      try { await post('save', entry.name, Object.assign({}, entry.doc, { saved: { extra: extrasOf(entry) } })); }
      catch (e) { say('Not saved: ' + err(e)); return false; }
      await api.mineRefresh();
      say('Saved “' + entry.name + '” to My designs on the hub');
      return true;
    };
    api.mineOpen = async function (nm) {
      if (!api.hubOrb()) { say(offHub); return false; }
      var o = null;
      try { var r = await fetch(q('get', nm)); o = r.ok ? await r.json() : null; } catch (e) { o = null; }
      if (!o || o.format !== 'wc-design') { say('Could not open “' + nm + '”: the hub has no design by that name'); return false; }
      var why = api.apply(o, 'My designs');
      if (why) { say('Could not open “' + nm + '”: ' + why); return false; }
      api.restart();
      api.sayOpened('“' + nm + '”');
      return true;
    };
    api.mineDelete = async function (nm) {
      if (!api.hubOrb()) { say(offHub); return false; }
      try { await post('delete', nm); } catch (e) { say('Not removed: ' + err(e)); return false; }
      await api.mineRefresh();
      say('Removed “' + nm + '” from My designs');
      return true;
    };

    /* ---- boot: drop the browser's old autosave; under the hub, move any My designs this
     * browser still holds from before K9 (references only: they never held their art), read
     * the hub's list, and ask the hub what the orb runs. Off the hub there is nothing to read. */
    api.boot = async function () {
      try { root.localStorage.removeItem(api.sessionKey()); } catch (e) { /* no storage */ }
      var hub = api.hubOrb();
      if (!hub) return false;
      var old = null; try { old = JSON.parse(root.localStorage.getItem(api.mineKey()) || 'null'); } catch (e) { old = null; }
      if (Array.isArray(old)) {
        var left = old.filter(function (x) { return x && x.name && x.doc; }), moved = 0;
        for (var i = 0; i < left.length; i++) {
          try { await post('save', left[i].name, Object.assign({}, left[i].doc, { saved: { extra: extrasOf(left[i]) } })); moved++; }
          catch (e) { /* kept in the browser for the next boot */ }
        }
        if (moved === left.length) { try { root.localStorage.removeItem(api.mineKey()); } catch (e) { /* no storage */ } }
      }
      await api.mineRefresh();
      try {
        var src = await hubJson('/api/editor/source?ip=' + encodeURIComponent(hub.ip) + '&slug=' + encodeURIComponent(hub.slug));
        api.orbNote = src && src.relation === 'newer' && src.orb_version
          ? 'this editor is ' + api.version() + '; the orb runs ' + api.slug() + ' ' + src.orb_version + ' — update the app on the orb before sending' : '';
      } catch (e) { api.orbNote = ''; }
      if (api.orbNote) say('Heads up: ' + api.orbNote);
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
      var me = api.slug(), schema = host.schema();
      if (!o || o.format !== 'wc-design') return { why: 'that design is not one this editor wrote' };
      if (o.app && o.app !== me) return { why: 'that design is the ' + o.app + ' app’s, not ' + me + '’s' };
      if (o.schema >= 1 && o.schema < schema) { var m = api.migrate(o); if (!m.doc) return { why: m.why }; o = m.doc; }
      if (!(o.schema >= 1) || o.schema > schema) return { why: 'that design was made by ' + (o.app || me) + ' ' + (o.appVersion || '?') + ' (design schema ' + o.schema + '); this editor (' + api.version() + ') reads schema ' + schema + ' — update the app on this orb' };
      // a newer release may have added fields this editor would silently drop on the next
      // save: refuse, naming the release. The +build (content hash) never orders.
      var ver = function (v) { var g = /^v(\d+)\.(\d+)\.(\d+)(?:\+[0-9A-Za-z.-]+)?$/.exec(String(v || '')); return g ? [+g[1], +g[2], +g[3]] : null; };
      var theirs = ver(o.appVersion), mine2 = ver(api.version());
      if (theirs && mine2 && (theirs[0] - mine2[0] || theirs[1] - mine2[1] || theirs[2] - mine2[2]) > 0)
        return { why: 'that design was made by ' + (o.app || me) + ' ' + o.appVersion + ', newer than this editor (' + api.version() + ') — update the app on this orb, then open it' };
      return { doc: o };
    };
    // a design back into the editor: the envelope's refusals, then the app's own. A design an
    // older release made opens, and lastMade keeps that release so the opener can say so
    var relOf = function (v) { var g = /^v(\d+)\.(\d+)\.(\d+)(?:\+[0-9A-Za-z.-]+)?$/.exec(String(v || '')); return g ? [+g[1], +g[2], +g[3]] : null; };
    api.apply = function (o, from) {
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
    // the design file: the design a baked theme carries, on its own
    api.exportDoc = async function (name) {
      var doc = await api.doc(true), nm = String(name || 'design').replace(/[^\w .-]+/g, '-').trim() || 'design';
      var body = JSON.stringify(doc), n = Array.isArray(doc.assets) ? doc.assets.length : 0;
      download(new Blob([body], { type: 'application/json' }), nm + '.orbdesign');
      say('Downloaded ' + nm + '.orbdesign (' + Math.max(1, Math.round(body.length / 1024)) + ' KB' + (n ? ', ' + n + ' image' + (n > 1 ? 's' : '') : '') + ')');
    };
    // open a file: a baked .orbtheme (its design read out of it) or a design file
    api.openFile = async function (file) {
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
      api.sayOpened(file.name);
      return true;
    };
    // ?theme=<card stem> (the hub's Edit door): the design comes through the hub, off the card or the shelf
    api.openTheme = async function () {
      var q = null; try { q = new URLSearchParams(location.search); } catch (e) { q = null; }
      var id = q && q.get('theme'); if (!id || !/^[A-Za-z0-9_.-]{1,64}$/.test(id)) return false;
      var ip = q.get('orb') || '', src = q.get('src') === 'shelf' ? 'shelf' : 'card';
      say('Opening the theme’s design from the ' + (src === 'shelf' ? 'hub’s shelf' : 'card') + '…');
      try {
        var r = await fetch('/api/theme/design?' + (src === 'shelf' ? 'name=' + encodeURIComponent(id) : 'ip=' + encodeURIComponent(ip) + '&id=' + encodeURIComponent(id)));
        if (!r.ok) { var w = ''; try { w = (await r.json()).error || ''; } catch (e) { w = ''; } say('Could not reopen that theme: ' + (w || 'the hub answered ' + r.status)); return false; }
        var o = JSON.parse(new TextDecoder().decode(new Uint8Array(await r.arrayBuffer())));
        var why = api.apply(o, src === 'shelf' ? 'the hub’s shelf' : 'the card');
        if (why) { say('Could not reopen that theme: ' + why); return false; }
        api.restart();
        api.sayOpened('the theme from the ' + (src === 'shelf' ? 'hub’s shelf' : 'card'));
        return true;
      } catch (e) { say('Could not reopen that theme: ' + (e && e.message ? e.message : String(e))); return false; }
    };

    return api;
  };
})(typeof window !== 'undefined' ? window : globalThis);
