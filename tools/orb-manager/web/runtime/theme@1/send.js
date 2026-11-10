/* theme@1/send — send a baked theme to the orb through the hub (theme-editor-kit-plan K1).
 *
 * Any app's editor calls this with the bytes GorbBake built; it owns everything between
 * the press and the verdict, so no app carries it:
 *   - finding the orb (the hub names it with ?orb=, else /api/orb/known);
 *   - the identity precheck (§32.7: the header and geometry region, never the bake);
 *   - the duplicate question, holding the refused bake for Replace, and the commit token
 *     that lets a Replace file the copy the orb already kept (§32.6, §32.9);
 *   - the upload with live progress (XHR), the hub's witness of the legs the page cannot
 *     see (/api/theme/upload-status), and the store verdict;
 *   - one modal act (owner ruling, 2026-11-03): from the press to the verdict the editor
 *     tells the hub it is busy, guards the window, and allows a cancel only while the bytes
 *     are still the page's; the verdict then stays up with a button that closes it (owner,
 *     2026-11-08: no close until it is done, and done is unmistakable), unless the send was
 *     cancelled or stopped at the duplicate question, which takes the screen.
 *
 * Plain JS, no React: the editor gives callbacks and draws the state its own way.
 *
 *   const s = OrbTheme.sender({
 *     say(text),            // the status line
 *     ask(dup),             // the duplicate question {nm, stem, note, pre}, or null to close it
 *     sending(state),       // {canCancel, cancelling} while a send runs; {done, outcome: 'ok'|'warn'|'fail'}
 *                           // once it ends (the dialog stays with its verdict until cancel()); null to close
 *     orb(ip),              // optional: the orb address once found (for a label)
 *     orbIp(),              // optional: where to send; default s.findOrb()
 *     download(blob, name), // optional: how a web host (send: false) saves the bake; default OrbTheme.download
 *   });
 *   s.send(name, bake)      // a press: Replace the held bake for this name, else run bake()
 *   s.post(bytes, {name, stem, summary}, overwrite)   // what bake() calls with its bytes
 *   s.replace() · s.dismiss() · s.cancel() · s.cancelled() · s.findOrb(fresh) · s.held · s.active
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

  OrbTheme.sender = function (host) {
    var api = {
      held: null,              // the bake the last duplicate refused: {bytes, nm, stem, summary, commit}
      active: false,           // a send is running
      cancelRequested: false,
    };
    var st = null, xhr = null, unload = null, orbCache = null, lastDup = null;
    // how the running send ended: 'fail' until a path says otherwise ('asked': the duplicate question)
    var outcome = 'fail';
    var say = function (s) { host.say(s); };
    var setSending = function (v) { st = v; host.sending(v); };
    var orbIp = function () { return host.orbIp ? host.orbIp() : api.findOrb(); };

    /* The learned orb address. The hub opens an editor for one orb and names it (?orb=);
     * otherwise /api/orb/known answers what the Manage card pre-fills from. The editor
     * keeps no address of its own, so a board re-plugged under a new lease cannot leave
     * Send dialing the old one. A found address is cached; "no orb" is asked again, and
     * so is everything when fresh is set (an editor that keeps its own cache). */
    api.findOrb = async function (fresh) {
      // a web host with send: false has no orb to find: no request, no cache, and the query is ignored
      if (!OrbTheme.hostInfo().send) { if (host.orb) host.orb(null); return null; }
      if (orbCache && !fresh) return orbCache;
      var q = null; try { q = new URLSearchParams(location.search); } catch (e) { q = null; }
      var named = q && q.get('orb'), ip = null;
      if (named && /^(\d{1,3}\.){3}\d{1,3}$/.test(named)) ip = named;
      else {
        try {
          var r = await fetch(OrbTheme.hostUrl('/orb/known'));
          var j = await r.json();
          if (j && j.ok && j.data && j.data.ip) ip = j.data.ip;
        } catch (e) { ip = null; }
      }
      orbCache = ip;
      if (host.orb) host.orb(ip);
      return ip;
    };

    // the editor tells the hub it is busy, so the hub will not close the pane, and guards the window
    var busy = function (on) {
      // '*': the hub page is on another origin (S2); it checks source and origin itself
      try { root.parent.postMessage({ type: 'theme.busy', on: !!on }, '*'); } catch (e) { /* standalone */ }
      if (typeof root.addEventListener !== 'function') return;
      if (on && !unload) {
        unload = function (e) { e.preventDefault(); e.returnValue = ''; };
        root.addEventListener('beforeunload', unload);
      } else if (!on && unload) {
        root.removeEventListener('beforeunload', unload);
        unload = null;
      }
    };
    var begin = function () { api.cancelRequested = false; api.active = true; outcome = 'fail'; setSending({ canCancel: true, cancelling: false }); busy(true); };
    var end = function () {
      xhr = null; api.active = false; busy(false);
      setSending(api.cancelRequested || outcome === 'asked' ? null : { done: true, outcome: outcome });
    };
    // past this point the orb holds the job: no cancel, only the wait
    var committed = function () { if (st) setSending({ canCancel: false, cancelling: false }); };

    /* One modal act around fn: the bake, the precheck and the crossing all run inside it. */
    api.run = async function (fn) {
      if (api.active) return;
      begin();
      try { await fn(); } finally { end(); }
    };
    api.cancel = function () {
      if (st && st.done) { setSending(null); return; }   // the verdict's Done or Close
      if (!st || !st.canCancel || st.cancelling) return;
      api.cancelRequested = true;
      setSending({ canCancel: false, cancelling: true });
      if (xhr) xhr.abort();
    };
    // the cancel checkpoint: a cancel pressed mid-bake lands here, before anything leaves
    api.cancelled = function () {
      if (!api.cancelRequested) return false;
      say('Send cancelled — nothing was sent to the orb.');
      return true;
    };

    /* A press. When the last send stopped at a duplicate, the bake is held and this press
     * ships THOSE bytes, with no re-bake, so the replace is byte-for-byte the theme the
     * owner was asked about. A changed name (or any new bake) drops the hold. */
    api.send = function (name, bake) {
      return api.run(async function () {
        var a = api.held;
        api.held = null;
        if (a && a.nm === name) await shipHeld(a);
        else await bake();
      });
    };
    /* The modal's Replace: ships the held bake and closes the question. If the hold was
     * already spent (a racing press, or the name changed) there is nothing to send. */
    api.replace = async function () {
      var a = api.held;
      host.ask(null);
      if (!a || api.active) return;
      api.held = null;
      await api.run(function () { return shipHeld(a); });
    };
    /* The modal's Cancel: the held bytes are discarded, not deferred, so the next press
     * is a fresh bake. */
    api.dismiss = function () {
      api.held = null;
      host.ask(null);
      say('Send cancelled — the card keeps its current "' + (lastDup && lastDup.nm || 'theme') + '"');
    };
    var askDup = function (d) { lastDup = d; outcome = 'asked'; host.ask(d); };

    /* The held bake's two roads to one file on the glass. With a commit token the device
     * already verified and kept these bytes, so Replace is an empty POST; a refused or lost
     * commit (410: stale, card swapped, reboot between) says so and ships the bytes. */
    var shipHeld = async function (a) {
      if (a.commit) {
        var ip = await orbIp();
        if (ip) {
          committed();
          say('Replacing "' + a.nm + '" on ' + ip + ' from the copy already on the card — no second upload');
          try {
            var r = await fetch(OrbTheme.hostUrl('/theme/commit'), { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ ip: ip, commit: a.commit }) });
            var j = await r.json().catch(function () { return null; });
            if (r.status === 200 && j && j.ok) {
              var stt = (j.data && j.data.state) || 'stored';
              var rebo = (j.data && j.data.reboot === 'automatic');
              outcome = 'ok';
              say('Sent to ' + ip + ' as "' + a.nm + '" (' + a.stem + ') — the new build replaced the old copy on its card · ' + stt + (rebo ? ' — rebooting onto it' : ''));
              return;
            }
            say('The staged copy was refused (' + ((j && j.error) || ('HTTP ' + r.status)) + ') — sending the file itself');
          } catch (e) {
            say('The commit could not be sent (' + (e && e.message ? e.message : String(e)) + ') — sending the file itself');
          }
        }
      }
      await api.post(a.bytes, { name: a.nm, stem: a.stem, summary: a.summary }, true);
    };

    /* Push the bytes to the orb through the hub's upload door, watched end to end. A
     * duplicate is asked about BEFORE megabytes cross the room (?precheck=1: the header and
     * the geometry region, where the owning app's slug lives); a precheck that could not
     * read the card fails closed and holds the bake; a late device 409 (same name and
     * serial, different payload) arms the same question. The final line's store verdict is
     * the hub's waitForStore witness, not an assumption. meta.summary is the editor's own
     * account of the bake (its policy and cost), carried into that line. */
    api.post = async function (bytes, meta, overwrite) {
      var nm = meta.name, stem = meta.stem, summary = meta.summary;
      var hold = function (commit) { return { bytes: bytes, nm: nm, stem: stem, summary: summary, commit: commit }; };
      if (api.cancelled()) return;
      if (!OrbTheme.hostInfo().send) {
        // Send to orb is a download here (web host, send: false): the bytes never leave the page
        var file = (meta.stem || meta.name || 'theme') + '.orbtheme';
        var dn = host.download || OrbTheme.download;
        if (dn) {
          dn(new Blob([bytes], { type: 'application/octet-stream' }), file);
          say('Downloaded "' + meta.name + '" as ' + file + ' — import it in Orb Manager to send it to an orb');
        } else {
          say('Not sent: this page cannot send to an orb, and it has no download');
        }
        return;
      }
      var ip = await orbIp();
      if (!ip) {
        say('Not sent: this hub is not talking to an orb — load one on the Manage card first, then press again. (The download still ships without a hub.)');
        return;
      }
      var door = OrbTheme.hostUrl('/theme/upload') + '?ip=' + encodeURIComponent(ip);
      if (!overwrite) {
        say('Asking ' + ip + ' whether "' + nm + '" is already on the card…');
        var pre = null, preErr = '';
        try {
          var gsz = 0;
          try { gsz = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(20, true); } catch (e) { gsz = 0; }
          var head = bytes.slice(0, Math.min(bytes.byteLength, 128 + Math.min(gsz, 0xF000)));
          var ask = async function (body) {
            var pr = await fetch(door + '&precheck=1', { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: body });
            var txt = await pr.text();
            var j = null;
            try { j = JSON.parse(txt); } catch (e) { j = null; }
            if (!j) {
              // a body that is not the hub's JSON is still an answer: say which it was
              var snip = (txt || '').replace(/\s+/g, ' ').trim().slice(0, 160);
              return { ok: false, status: pr.status,
                error: 'the hub answered HTTP ' + pr.status + ' with something that was not its JSON' + (snip ? ': ' + snip : ' (an empty body)') };
            }
            j.status = pr.status;
            return j;
          };
          // The head slice is the only precheck body, with one exception that is the hub's
          // own instruction: a refusal naming the byte count the index needs ("send the
          // first N B of the file") is answered once with exactly that many.
          var needOf = function (m) { var g = /send the first (\d+) B of the file/.exec(m || ''); return g ? (+g[1]) : 0; };
          var pre1 = null, pre1Err = '';
          try { pre1 = await ask(head); } catch (e) { pre1Err = (e && e.message) ? String(e.message) : String(e); }
          if (pre1 && !pre1.ok) {
            var need = needOf(pre1.error);
            if (need > head.byteLength && need <= bytes.byteLength) {
              try { pre = await ask(bytes.slice(0, need)); }
              catch (e) { preErr = (e && e.message) ? String(e.message) : String(e); }
            }
          }
          if (!pre && pre1) pre = pre1;
          // a fetch that threw is a transport fact: keep its message for the reason below
          if (!pre && !preErr && pre1) preErr = pre1.error || ('the hub answered HTTP ' + pre1.status);
          else if (!pre && !preErr && pre1Err) preErr = pre1Err;
        } catch (e) { pre = null; preErr = (e && e.message) ? String(e.message) : String(e); }
        if (pre && pre.ok && pre.data && pre.data.exact) {
          // §32.9: the card already holds this exact bake. Stop before the upload: a re-send
          // would re-arm an idempotent store and reboot the orb onto the face it is drawing.
          api.held = null; outcome = 'ok';   // the orb already draws these bytes: done, not a failure
          say('"' + nm + '" is already on the card, byte for byte — nothing sent. To reboot onto it or re-store it, use its store button on the Manage card.');
          return;
        }
        if (pre && pre.ok && pre.data && pre.data.duplicate) {
          api.held = hold();
          askDup({ nm: nm, stem: stem, note: (pre.data.note || 'the card holds a different build of this name'), pre: true });
          return;
        }
        // No clean answer (a dead fetch, an HTTP error, or "clear" with a note: the hub's
        // spelling for "I could not ask the orb") means the card state is unknown. Stop
        // before the upload and hold the bake, so the next press forces the replace over
        // the unknown; that is the owner's call, not this page's silent default. The hub's
        // own sentence rides the line whenever it has one.
        if (!pre || !pre.ok || !pre.data || pre.data.note) {
          api.held = hold();
          var why = (pre && pre.error) || (pre && pre.data && pre.data.note)
            || (pre && pre.status && pre.status !== 200 ? 'the hub answered HTTP ' + pre.status : '')
            || preErr || (pre ? '' : 'the hub never answered');
          say('Nothing sent: the hub could not get a clean read of "' + nm + '" on ' + ip +
            (why ? ' — ' + why : '') +
            ' — so it will not ship over a card it cannot check. Press Send again to replace it anyway.');
          return;
        }
      }
      if (api.cancelled()) return;
      say('Sending "' + nm + '" to ' + ip + (overwrite ? ' (replacing)' : '') + ' — 0%');
      var watch = watchSend(ip, nm);
      var code = 0, j = {};
      try {
        var up = await push(door + (overwrite ? '&overwrite=1' : ''), bytes,
          function (sent, total) { say('Sending "' + nm + '" to ' + ip + ' — ' + Math.floor(sent * 100 / (total || 1)) + '% of ' + total + ' B to the hub'); });
        code = up.status; j = up.j;
      } catch (e) {
        watch.stop();
        if (api.cancelled()) return;
        say('Send failed: ' + (e && e.message ? e.message : String(e)));
        return;
      }
      watch.stop();
      if (code !== 200 || j.ok === false) {
        var why2 = (j && j.error) || ('HTTP ' + code);
        if (!overwrite && String(why2).indexOf('already on the card') >= 0) {
          // The device kept the bytes it just verified and named the keep with a commit
          // token, so Replace costs an empty POST; an old firmware sends no token.
          api.held = hold(String(j.commit || ''));
          askDup({ nm: nm, stem: stem, note: String(why2) + (j.commit ? ' — the orb kept this upload; Replace files the copy already on its card.' : ''), pre: false });
          return;
        }
        say('Not sent: ' + why2);
        return;
      }
      var d = (j && j.data) || {};
      var storeLine = {
        stored: d.back === false ? 'stored into the one slot, and the orb has not answered since its reboot — check the Manage card'
          : 'stored into the one slot, and the orb is back drawing it',
        torn: 'the copy tore before its header: the base face boots until one re-store' + (d.store_note ? ' (' + d.store_note + ')' : ''),
        refused: 'the store refused: ' + (d.store_note || 'no reason from the orb'),
        lost: 'filed on the card, and the store outcome stayed unreported past the wait — check the Manage card'
      }[d.store] || 'stored on the card';
      outcome = d.store === 'torn' || d.store === 'refused' ? 'fail' : d.store === 'lost' || d.back === false ? 'warn' : 'ok';
      say('Sent to ' + ip + ' as "' + (d.name || nm) + '" (' + (d.id || stem) + ') — ' + (summary ? summary + ' · ' : '') + storeLine);
    };

    /* XHR for the one thing fetch will not tell a page: how many body bytes the browser
     * has handed the transport. Resolves {status, j} on any HTTP answer and rejects only
     * when the request itself dies. */
    var push = function (url, bytes, onProgress) {
      return new Promise(function (resolve, reject) {
        var x = new XMLHttpRequest();
        xhr = x;
        x.open('POST', url);
        x.setRequestHeader('Content-Type', 'application/octet-stream');
        var lastPaint = 0;
        x.upload.onprogress = function (e) {
          var now = Date.now();
          if (now - lastPaint < 120 && e.loaded < e.total) return;
          lastPaint = now;
          onProgress(e.loaded, e.total);
          // every byte is the hub's now, and the hub dials the orb with it: no cancel past here
          if (e.loaded >= e.total) committed();
        };
        x.onload = function () {
          var j = {};
          try { j = JSON.parse(x.responseText); } catch (e) { /* not JSON: the status line carries the code */ }
          resolve({ status: x.status, j: j });
        };
        x.onerror = function () { reject(new Error('the hub closed the connection mid-send')); };
        x.onabort = function () { reject(new Error('cancelled')); };
        x.send(bytes);
      });
    };

    /* The hub's own witness, polled while the POST is blocked at the hub: the hub-to-orb
     * byte count and the store phases, relayed in words rather than an invented percent. */
    var watchSend = function (ip, nm) {
      var words = {
        uploading: function (s) { return 'Sending "' + nm + '" — the hub is dialing the orb: ' + Math.floor((s.sent || 0) * 100 / ((s.total || 1))) + '% of ' + (s.total || 0) + ' B'; },
        queued: function () { return '"' + nm + '" is on the orb, the store is armed — the copy comes next'; },
        storing: function () { return '"' + nm + '" is storing into the slot — progress rides the orb glass; the orb reboots itself when the copy lands'; },
        rebooting: function () { return '"' + nm + '" is stored — the orb is rebooting onto it; waiting for it to come back'; },
        duplicate: function () { return 'the card holds a different "' + nm + '" — press Send to orb again to replace it'; },
        exact: function () { return '"' + nm + '" is already on the card, byte for byte — nothing was sent'; },
        rejected: function (s) { return 'the orb refused the upload' + (s.note ? ': ' + s.note : ''); }
      };
      var dead = false;
      var timer = setInterval(async function () {
        if (dead) return;
        try {
          var r = await fetch(OrbTheme.hostUrl('/theme/upload-status') + '?ip=' + encodeURIComponent(ip));
          var j = await r.json().catch(function () { return null; });
          var s = j && j.ok && j.data;
          if (s && words[s.phase]) say(words[s.phase](s));
        } catch (e) { /* a lost poll is noise; the POST's own answer settles everything */ }
      }, 700);
      return { stop: function () { dead = true; clearInterval(timer); } };
    };

    return api;
  };
})(typeof window !== 'undefined' ? window : globalThis);
