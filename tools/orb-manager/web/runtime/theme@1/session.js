/* theme@1/session — an editor's session: history, autosave, My designs, and the design file
 * (theme-editor-kit-plan K2).
 *
 * Any app's editor gets, without carrying it:
 *   - undo and redo over snapshots of the keys the app names, with a burst of edits (a
 *     slider drag, a typed number) merged into one step;
 *   - autosave to this browser, and picking the last session up on load;
 *   - My designs: named designs kept in this browser;
 *   - the design document's envelope (format, schema, app, app version, made), the
 *     migration walk (composer-stack-plan ruling 12), and its refusals: another app's
 *     design, a schema with no step, a design from a newer release of the app;
 *   - opening a design from a file (an .orbtheme's design region, or a design file) or
 *     from the hub's Edit door (?theme=, the card or the hub's shelf), and downloads.
 *
 * Every key is per app (?app=, else the host's slug), so two apps' editors in one browser
 * never read each other's sessions. Plain JS, no React. The app keeps what only it knows:
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
 *     compact(snap),               // optional: a snapshot as autosave keeps it (drop heavy bytes)
 *     adopt(body),                 // a stored session back as a snapshot, or null to ignore it
 *     resumeHint,                  // optional: a sentence after "Picked up where you left off"
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
      saved: null,                 // autosave: null = not attempted, else whether the browser kept it
    };
    var saveT = null, mine = null;
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
    api.sessionKey = function () { return 'wc.session.' + api.slug(); };
    api.mineKey = function () { return 'wc.mine.' + api.slug(); };

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
      api.save();
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

    /* ---- autosave: the newest snapshot, written to this browser a moment after the edits stop */
    api.body = function (snap) {
      return Object.assign({ v: 1, at: Date.now() }, host.compact ? host.compact(snap) : snap);
    };
    api.save = function () {
      if (saveT) clearTimeout(saveT);
      saveT = setTimeout(function () {
        saveT = null;
        try { root.localStorage.setItem(api.sessionKey(), JSON.stringify(api.body(api.snap || api.snapOf(host.state())))); api.saved = true; }
        catch (e) { api.saved = false; }
      }, 700);
    };
    // on load: the last session in this browser, if the app accepts it
    api.load = function () {
      var b = null;
      try { b = JSON.parse(root.localStorage.getItem(api.sessionKey()) || 'null'); } catch (e) { b = null; }
      if (!b || b.v !== 1) return false;
      var snap = host.adopt(b);
      if (!snap) return false;
      var when = new Date(+b.at || Date.now());
      api.restart();
      host.set(Object.assign(snap, { status: 'Picked up where you left off (saved ' + when.toLocaleString() + ').' + (host.resumeHint ? ' ' + host.resumeHint : '') }));
      return true;
    };

    /* ---- My designs: named design documents in this browser. Read once and kept (a sheet
     * that lists them renders every frame); every write goes through mineWrite. */
    api.mineList = function () {
      if (mine) return mine.slice();
      var l = []; try { l = JSON.parse(root.localStorage.getItem(api.mineKey()) || '[]'); } catch (e) { l = []; }
      mine = Array.isArray(l) ? l.filter(function (x) { return x && x.name && x.doc; }) : [];
      return mine.slice();
    };
    api.mineWrite = function (l) {
      try { root.localStorage.setItem(api.mineKey(), JSON.stringify(l)); mine = l.slice(); return true; }
      catch (e) { say('Not saved: this browser refused to store it (' + (e && e.name || 'storage') + ')'); return false; }
    };
    // entry: {name, doc, ...anything the app lists by}; a save under a held name replaces it
    api.mineSave = function (entry) {
      var l = api.mineList().filter(function (x) { return x.name !== entry.name; });
      l.unshift(Object.assign({ at: Date.now() }, entry));
      if (api.mineWrite(l)) { say('Saved “' + entry.name + '” to My designs'); return true; }
      return false;
    };
    api.mineDelete = function (nm) {
      if (api.mineWrite(api.mineList().filter(function (x) { return x.name !== nm; }))) say('Removed “' + nm + '” from My designs');
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
    // a design back into the editor: the envelope's refusals, then the app's own
    api.apply = function (o, from) {
      var c = api.check(o);
      return c.why || host.apply(c.doc, from);
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
        return true;
      } catch (e) { say('Could not reopen that theme: ' + (e && e.message ? e.message : String(e))); return false; }
    };

    return api;
  };
})(typeof window !== 'undefined' ? window : globalThis);
