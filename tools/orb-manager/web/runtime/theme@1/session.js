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
 *     design, a schema with no step, a design from a newer release of the app;
 *   - opening a design from a file (an .orbtheme's design region, or a design file), a
 *     library entry (?entry=) or a card theme (?theme=), and the theme download.
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
 *     bake(),                      // optional, async: the open design baked to .orbtheme bytes, or
 *                                  // null (the app's status says why); Rebake all needs it
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
    };
    var lib = [];
    var say = function (s) { host.say(s); };
    var keys = function () { return host.keys(); };
    var burstMs = function () { return host.burstMs ? host.burstMs() : 600; };
    var undoMax = function () { return host.undoMax ? host.undoMax() : 100; };
    var download = function (blob, name) { return (host.download || OrbTheme.download)(blob, name); };
    /* An open is a fetch and a decode, megabytes off an orb's card for a converted theme, and
     * until it lands the start sheet's presets sit under the pointer: a click there picks a dial
     * the open then overwrites. The editor covers itself while api.opening holds a line. Every
     * open clears it however it ends. The hub's Edit door (?theme=) and ?entry= start covered:
     * the fonts and the library load before boot reaches the open. */
    var opening = function (line) { api.opening = line || null; host.set({}); };
    (function () {
      var q = null; try { q = new URLSearchParams(root.location.search); } catch (e) { q = null; }
      if (!q) return;
      if (/^[A-Za-z0-9_.-]{1,64}$/.test(q.get('theme') || '')) api.opening = 'Opening the theme’s design from the ' + (q.get('src') === 'shelf' ? 'hub’s shelf' : 'card') + '…';
      else if (q.get('entry')) api.opening = 'Opening “' + q.get('entry') + '” from the library…';
    })();

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

    /* ---- the library (K10): the app's entries on the hub, one per name. The list is fetched
     * and kept (a sheet that lists it renders every frame). An entry is
     *   {name, state: 'draft'|'theme'|'changed', at, draft: {at, appVersion, extra}|null,
     *    theme: {stem, made_with, can_reopen, stale}|null, ...draft.extra}
     * Off the hub there is no library, and the theme download is the way to take one along. */
    var app = function () { return '?app=' + encodeURIComponent(api.slug()); };
    var offHub = 'the library is kept on the hub: open this editor from the hub, or download the .orbtheme';
    var err = function (e) { return e && e.message ? e.message : String(e); };
    var extrasOf = function (x) { var o = {}; Object.keys(x).forEach(function (k) { if (k !== 'name' && k !== 'doc' && k !== 'at') o[k] = x[k]; }); return o; };
    var saveDraft = function (name, doc, extra) {
      return hubJson('/api/design/save' + app() + '&name=' + encodeURIComponent(name),
        { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(Object.assign({}, doc, { saved: { extra: extra || {} } })) });
    };
    api.library = function () { return lib.slice(); };
    api.libraryEntry = function (nm) { return lib.find(function (e) { return e.name === nm; }) || null; };
    api.libraryRefresh = async function () {
      if (!api.hubOrb()) { lib = []; return false; }
      try {
        var l = await hubJson('/api/library/entries' + app());
        lib = (Array.isArray(l) ? l : []).map(function (e) { return Object.assign({}, (e.draft && e.draft.extra) || {}, e); });
      } catch (e) { say('The library could not be read from the hub: ' + err(e)); return false; }
      host.set({});
      return true;
    };
    // entry: {name, doc, ...anything the app lists by}. A draft under a theme's name sits beside
    // it as that theme's unsent changes; under a draft's name it replaces it.
    // quiet (Send to orb's first step): no line unless it fails, so the send's own lines stand
    api.saveDraft = async function (entry, quiet) {
      if (!api.hubOrb()) { if (!quiet) say('Not saved: ' + offHub); return false; }
      try { await saveDraft(entry.name, entry.doc, extrasOf(entry)); }
      catch (e) { say('Not saved: ' + err(e)); return false; }
      await api.libraryRefresh();
      if (quiet) return true;
      var e = api.libraryEntry(entry.name);
      say('Saved “' + entry.name + '” to the library' + (e && e.state === 'changed' ? ' as unsent changes to its theme — Send to orb bakes them in' : ' as a draft'));
      return true;
    };
    // an entry's newest design (its draft, else its theme's), or its theme's with from='theme'
    var designOf = async function (nm, from) {
      var r = await fetch('/api/library/design' + app() + '&name=' + encodeURIComponent(nm) + (from ? '&from=' + from : ''));
      var o = null; try { o = await r.json(); } catch (e) { o = null; }
      if (!r.ok || !o || o.format !== 'wc-design') throw new Error((o && o.error) || 'the hub has no design for “' + nm + '”');
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
      if (!api.lastMade) say('Opened “' + nm + '” from the library');
      api.sayOpened('“' + nm + '”');
      return true;
    };
    // what: 'draft' discards a theme's unsent changes; 'all' takes the entry out of the library
    api.forgetEntry = async function (nm, what) {
      if (!api.hubOrb()) { say(offHub); return false; }
      try { await hubJson('/api/library/forget' + app() + '&name=' + encodeURIComponent(nm) + '&what=' + (what === 'draft' ? 'draft' : 'all'), { method: 'POST' }); }
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
          var kept = await hubJson('/api/library/keep' + app() + '&fold=0', { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: bytes });
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

    /* ---- boot: drop the browser's old autosave; under the hub, move any My designs this
     * browser still holds from before K9 into the library as drafts (references only: they
     * never held their art), read the library, ask the hub what the orb runs, then do what
     * the page was opened for: ?entry=<name> opens that entry, ?rebake=1 runs Rebake all. */
    // a boot that throws never reaches the open it was covering for
    api.boot = async function () {
      try { return await boot(); } catch (e) { opening(null); throw e; }
    };
    var boot = async function () {
      try { root.localStorage.removeItem(api.sessionKey()); } catch (e) { /* no storage */ }
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
      try {
        var src = await hubJson('/api/editor/source?ip=' + encodeURIComponent(hub.ip) + '&slug=' + encodeURIComponent(hub.slug));
        // an orb behind the hub keeps its own editor, so its bakes are ones its app reads
        api.orbNote = src && src.relation === 'newer' && src.orb_version && src.hub_version
          ? 'this is the orb’s own ' + api.slug() + ' ' + src.orb_version + ' editor; the hub has ' + src.hub_version + ' — update the app on the orb to edit with it' : '';
      } catch (e) { api.orbNote = ''; }
      if (api.orbNote) say('Heads up: ' + api.orbNote);
      var q = null; try { q = new URLSearchParams(root.location.search); } catch (e) { q = null; }
      var entry = q && q.get('entry');
      if (entry) await api.openEntry(entry);
      else if (q && q.get('rebake') === '1') await api.rebakeAll();
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
    var openTheme = async function () {
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
