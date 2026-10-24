require('./headless-stub.js');
global.React = { createRef: () => ({ current: null }) };
require('./component.js');
const DCLogic = function () { };
DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
const Comp = global.COMP_CLASS(DCLogic);
const E = global.WatchEngine;
for (const id of ['modern', 'aviator', 'perpetual', 'sector', 'diver', 'skeleton']) {
  if (!E.PRESETS[id]) { console.log('(no preset ' + id + ')'); continue; }
  const i = new Comp({});
  i.ready = true; i.state.dial = id;
  const D = i.design();
  const c = i.cfg();
  const RE = { roman: 205, arabic: 200, regulator: 206, baton: 200, minimal: 205, sans: 205, railway: 202, sector: 205, diver: 200, station: 214, pilot: 204, none: 205 };
  const hc = (D.hands && D.hands['centre-second']) || {};
  const tip = Math.min(216, (RE[D.print] || 205) * 0.98 * (+hc.len || 1));
  const half = (x) => x.size != null ? Math.max(+x.size / 2, x.type === 'window' ? 6 : 18) : 28;
  const hit = (x) => x.host === 'dial' && (x.type === 'subdial' || x.type === 'moon' || x.type === 'window')
    && !(+x.open > 0.5)
    && (+x.dist || 0) + half(x) > 18 && (+x.dist || 0) - half(x) <= tip;
  const crossed = (D.comps || []).filter(hit);
  const out = i._devPace({ fps: 15, measured: true, car: false });
  console.log(id.padEnd(10) + ' print=' + String(D.print).padEnd(8) + ' sec=' + (D.centre && D.centre.second) + ' tick=' + c.tick
    + ' tip=' + tip.toFixed(0) + ' crossed=[' + crossed.map((x) => x.type + '@' + x.dist + 'px s' + x.size).join(', ') + ']'
    + '  -> pace=' + out.pace + ' cross=' + out.cross + ' cov=' + out.cov.toFixed(4));
}
