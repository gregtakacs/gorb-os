// A stand-in for the hub's K9 routes, for node suites that drive theme@1/session: the
// design store (/api/design/list|get|save|delete, filed by name per app, as designs.go
// files them) and /api/editor/source. Anything else goes to `rest`, so a suite can keep
// its own answers for the routes it already stubs.
//
//   const hub = require('.../fake-hub.js')({ source: { relation: 'newer', orb_version: 'v1.2.0' } });
//   global.fetch = (url, opts) => hub.fetch(url, opts);
//   hub.designs  // Map app -> Map name -> the stored document
'use strict';
module.exports = function fakeHub(opts) {
  opts = opts || {};
  const designs = new Map(), calls = [];
  const res = (status, body) => ({ ok: status < 300, status, json: async () => JSON.parse(JSON.stringify(body)), arrayBuffer: async () => new TextEncoder().encode(JSON.stringify(body)).buffer });
  const env = (data) => res(200, { ok: true, data });
  const no = (error) => res(400, { ok: false, error });
  const hub = {
    designs, calls,
    source: opts.source || { source: 'library', relation: 'same' },
    refuse: null,                       // a sentence: every save answers with it
    at: 1000,
    async fetch(url, o) {
      calls.push(url);
      const u = new URL(url, 'http://hub'), app = u.searchParams.get('app'), name = u.searchParams.get('name');
      if (u.pathname === '/api/editor/source') return env(hub.source);
      if (!u.pathname.startsWith('/api/design/')) return opts.rest ? opts.rest(url, o) : no('no route ' + u.pathname);
      if (!/^[a-z0-9_-]{1,32}$/.test(app || '')) return no('expected ?app=<slug>');
      const mine = designs.get(app) || new Map(); designs.set(app, mine);
      switch (u.pathname) {
        case '/api/design/list':
          return env([...mine.values()].sort((a, b) => b.saved.at - a.saved.at).map((d) => ({ name: d.saved.name, at: d.saved.at, bytes: JSON.stringify(d).length, appVersion: d.appVersion, extra: d.saved.extra })));
        case '/api/design/get':
          return mine.has(name) ? res(200, mine.get(name)) : no('no design named “' + name + '” on this hub');
        case '/api/design/save': {
          if (hub.refuse) return no(hub.refuse);
          let d; try { d = JSON.parse(o.body); } catch (e) { return no('that is not a design document'); }
          if (!d || d.format !== 'wc-design') return no('that is not a design document');
          if (d.app && d.app !== app) return no('that design is the ' + d.app + ' app’s, not ' + app + '’s');
          d.saved = { name, at: ++hub.at, extra: (d.saved && d.saved.extra) || undefined };
          mine.set(name, d);
          return env({ name });
        }
        case '/api/design/delete':
          mine.delete(name);
          return env({ name });
      }
      return no('no route ' + u.pathname);
    },
  };
  return hub;
};
