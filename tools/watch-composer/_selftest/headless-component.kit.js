const { ok, eq, wait, mk, E, txt, i1, d, fails, checked } = global.__T;

(async () => {
  // ---- manifest.assets[] ------------------------------------------------------------
  const assets = await i1.kitAssets([], d);
  eq(assets.length, 4, 'kitAssets lists both backdrops, the hand and the flare');
  const bg = assets.find((a) => a.id === 'bg-1');
  ok(bg && bg.naturalSize[0] === 500 && bg.drawnSize[0] === 442, 'kitAssets names source and drawn box', JSON.stringify(bg && [bg.naturalSize, bg.drawnSize]));
  ok(bg && /^[0-9a-f]{64}$/.test(bg.sha256 || ''), 'kitAssets carries a real SHA-256 where Web Crypto offers one');
  ok(assets.every((a) => a.placed === true), 'every entry records that the design uses it');
  const hand = assets.find((a) => a.id === 'hand-1');
  eq(hand.usedBy, ['hand centre-minute'], 'the manifest says which slot used which asset');
  const files = [];
  await i1.kitAssets(files, d);
  ok(files.length === 4 && files.every((f) => f.name.indexOf('sources/') === 0), 'the kit zip gets a sources/ payload per asset');
  ok(assets.every((a) => /^sources\//.test(a.file)), 'and the manifest points at them');

  // ---- a dangling reference is reported, never hidden -------------------------------
  const i4 = mk();
  i4.state.designs.t.bgArt = 'nowhere-5';
  const dang = await i4.kitAssets([], i4.state.designs.t);
  eq(dang.length, 1, 'one reference listed');
  ok(dang[0].missing === true && /wckit/.test(dang[0].note || ''), 'an unheld reference ships as missing with the fix named', dang[0].note);
  const fc = { subLayers: [{ id: 'c1', transparent: true }, { id: 'c2' }] };
  eq(i4.compHost(fc, { host: 'c1' }), 'dial', 'compHost: a plate that does not ship hands its comps to the dial');
  eq(i4.compHost(fc, { host: 'c2' }), 'c2', 'compHost: a real plate keeps its own comps');
  eq(i4.compHost(fc, { host: 'dial' }), 'dial', 'compHost: the dial is untouched');

  // ---- .wckit round trip ------------------------------------------------------------
  const i5 = mk();
  i5.state.designs.t = JSON.parse(JSON.stringify(d));
  let captured = null;
  i5.download = (blob, name) => { captured = { blob, name }; };
  await i5.exportBundle();
  ok(!!captured && /wckit$/.test(captured.name), 'exportBundle downloads a .wckit', captured && captured.name);
  const body = JSON.parse(await captured.blob.text());
  eq(body.format + ':' + body.version, 'wckit:1', 'the bundle is named and versioned');
  ok(body.assets.length === 4 && body.assets.every((a) => a.dataUrl), 'the bundle carries the bytes a Design ID cannot');
  ok(body.design.bgArt === 'bg-1' && body.design.sfArt === 'sf-1' && body.design.hands['centre-minute'].art === 'hand-1', 'the bundle carries the design with every reference, both backdrop slots included');
  ok(assets.find((a) => a.id === 'sf-1').usedBy.join() === 'dial surface', 'the surface asset is attributed to the dial face, not the plate');

  E.assetClear();
  ok(!E.assetKnown('bg-1'), 'memory wiped: this is the second browser');
  const i6 = mk();
  i6.faces = {}; i6.buildMovement = () => { };
  i6.importBundle(txt(JSON.stringify(body)));
  await wait(40);
  ok(E.assetReady('bg-1') && E.assetReady('flare-1'), 'importBundle brings the art into a browser that never had it');
  eq(i6.state.designs.t.bgArt, 'bg-1', 'importBundle restores the design');
  eq(i6.state.designs.t.hands['centre-minute'].pivotX, 60, 'the authored pivot survives the round trip');

  // ---- refusals name themselves ------------------------------------------------------
  let msg = '';
  i6.setState = function (p) { const n = typeof p === 'function' ? p(this.state) : p; Object.assign(this.state, n); if (n.status) msg = n.status; };
  i6.importBundle(txt('not json at all')); await wait(20);
  ok(/not JSON/.test(msg), 'a file that is not JSON is refused in words', msg);
  i6.importBundle(txt(JSON.stringify({ hello: 1 }))); await wait(20);
  ok(/wckit/.test(msg), 'JSON without the format header is refused', msg);
  i6.importBundle(txt(JSON.stringify({ format: 'wckit', version: 9, design: { comps: [] } }))); await wait(20);
  ok(/version 9/.test(msg), 'a future version is refused rather than half-read', msg);
  // both phases must reach their counts: a suite that silently skipped a block would
  // otherwise report green while checking nothing
  if (checked() < 30) console.log('FAIL only ' + checked() + ' checks ran across both phases');
  const bad = fails() + (checked() < 30 ? 1 : 0);
  console.log(bad ? '\n' + bad + ' FAILED' : '\nall green (' + checked() + ' checks)');
  process.exit(bad ? 1 : 0);
})();
