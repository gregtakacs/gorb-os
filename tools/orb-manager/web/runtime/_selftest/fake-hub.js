// A stand-in for the hub's library routes, for node suites that drive theme@1/session
// (theme-editor-kit-plan K9, K10): drafts (/api/design/save), the entries an app's library
// lists, an entry's design, keeping a baked theme, forgetting, and /api/editor/source. It
// keeps libentries.go's rules: one entry per name, a draft beside a theme is "changed",
// keeping a theme folds its draft unless fold=0. Anything else goes to `rest`.
//
//   const hub = require('.../fake-hub.js')({ source: { relation: 'newer', orb_version: 'v1.2.0' } });
//   global.fetch = (url, opts) => hub.fetch(url, opts);
//   hub.drafts  // Map app -> Map name -> the saved document
//   hub.themes  // Map app -> Map name -> {design, stale, bytes}: the shelf
'use strict';
module.exports = function fakeHub(opts) {
  opts = opts || {};
  const drafts = new Map(), themes = new Map(), calls = [];
  const res = (status, body) => ({ ok: status < 300, status, json: async () => JSON.parse(JSON.stringify(body)), arrayBuffer: async () => new TextEncoder().encode(JSON.stringify(body)).buffer });
  const env = (data) => res(200, { ok: true, data });
  const no = (error) => res(400, { ok: false, error });
  const of = (m, app) => { if (!m.has(app)) m.set(app, new Map()); return m.get(app); };
  // a theme's name and design as the baker writes them: the header's name field (word 14,
  // 56 bytes) and the design the last three header words point at
  const readTheme = (b) => {
    const u = b instanceof Uint8Array ? b : new Uint8Array(b), dv = new DataView(u.buffer, u.byteOffset, u.byteLength);
    let end = 56; for (let i = 0; i < 56; i++) if (u[56 + i] === 0) { end = i; break; }
    const name = new TextDecoder().decode(u.subarray(56, 56 + end));
    const off = dv.getUint32(29 * 4, true), size = dv.getUint32(30 * 4, true);
    let design = null; try { design = size ? JSON.parse(new TextDecoder().decode(u.subarray(off, off + size))) : null; } catch (e) { design = null; }
    return { name, design, bytes: u.length };
  };
  const hub = {
    drafts, themes, calls, readTheme,
    source: opts.source || { source: 'library', relation: 'same' },
    refuse: null,                       // a sentence: every save and keep answers with it
    at: 1000,
    entries(app) {
      const names = new Set([...of(drafts, app).keys(), ...of(themes, app).keys()]);
      return [...names].map((name) => {
        const d = of(drafts, app).get(name), t = of(themes, app).get(name);
        return {
          name, state: d && t ? 'changed' : t ? 'theme' : 'draft', at: Math.max(d ? d.saved.at : 0, t ? t.at || 0 : 0),
          draft: d ? { name, at: d.saved.at, appVersion: d.appVersion, extra: d.saved.extra } : undefined,
          theme: t ? { stem: name.toLowerCase(), made_with: t.design ? t.design.appVersion : '', can_reopen: !!t.design, stale: t.stale || '', bytes: t.bytes || 0 } : undefined,
        };
      }).sort((a, b) => b.at - a.at);
    },
    async fetch(url, o) {
      calls.push(url);
      const u = new URL(url, 'http://hub'), app = u.searchParams.get('app'), name = u.searchParams.get('name');
      if (u.pathname === '/api/editor/source') return env(hub.source);
      if (!u.pathname.startsWith('/api/design/') && !u.pathname.startsWith('/api/library/')) return opts.rest ? opts.rest(url, o) : no('no route ' + u.pathname);
      if (!/^[a-z0-9_-]{1,32}$/.test(app || '')) return no('expected ?app=<slug>');
      const ds = of(drafts, app), ts = of(themes, app);
      switch (u.pathname) {
        case '/api/design/save': {
          if (hub.refuse) return no(hub.refuse);
          let d; try { d = JSON.parse(o.body); } catch (e) { return no('that is not a design document'); }
          if (!d || d.format !== 'wc-design') return no('that is not a design document');
          if (d.app && d.app !== app) return no('that design is the ' + d.app + ' app’s, not ' + app + '’s');
          d.saved = { name, at: ++hub.at, extra: (d.saved && d.saved.extra) || undefined };
          ds.set(name, d);
          return env({ name });
        }
        case '/api/library/entries':
          return env(hub.entries(app));
        case '/api/library/design': {
          const d = ds.get(name), t = ts.get(name);
          if (d && !(u.searchParams.get('from') === 'theme' && t)) return res(200, d);
          if (t && t.design) return res(200, t.design);
          return no(t ? 'this theme carries no design, so it cannot be reopened' : 'no “' + name + '” in the ' + app + ' library on this hub');
        }
        case '/api/library/keep': {
          if (hub.refuse) return no(hub.refuse);
          const t = readTheme(o.body);
          ts.set(t.name, { design: t.design, stale: '', bytes: t.bytes, at: ++hub.at });
          if (u.searchParams.get('fold') !== '0') ds.delete(t.name);
          return env({ name: t.name });
        }
        case '/api/library/forget': {
          if (!ds.has(name) && !ts.has(name)) return no('no “' + name + '” in the ' + app + ' library on this hub');
          ds.delete(name);
          if (u.searchParams.get('what') === 'all') ts.delete(name);
          return env({ name });
        }
      }
      return no('no route ' + u.pathname);
    },
  };
  return hub;
};
