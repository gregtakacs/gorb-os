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

  /* ---- the authored arbour and facing cross a browser boundary ----------------------
   * A Design ID carries references only, so the .wckit is the thing that has to carry
   * a pivot outside the sprite and a facing that is not north. Both are numbers the
   * bake reads, and a round trip that quietly drops either ships a wrong hand. */
  const i7 = mk();
  i7.state.designs.t = { print: 'roman', comps: [], centre: { hour: 1, minute: 1, second: 1 }, hands: { 'centre-minute': { art: 'hand-1', pivotX: -18, pivotY: 44, rot0: 90, zoom: 1.5 }, 'centre-second': { art: 'hand-1', rot0: 45 } } };
  i7.state.designs.t.bgArt = 'bg-1';
  let cap7 = null;
  i7.download = (blob, name) => { cap7 = { blob, name }; };
  await i7.exportBundle();
  const b7 = JSON.parse(await cap7.blob.text());
  const hm = b7.design.hands['centre-minute'];
  ok(hm.pivotX === -18 && hm.pivotY === 44, 'a negative pivot, i.e. an arbour off the visible ink, is written into the bundle', JSON.stringify([hm.pivotX, hm.pivotY]));
  ok(hm.rot0 === 90, 'the facing goes with it, which is the only thing that makes non-north art read right', String(hm.rot0));
  ok(hm.zoom === 1.5, 'and so does the hand zoom', String(hm.zoom));
  ok(b7.design.hands['centre-second'].rot0 === 45, 'each hand carries its own facing independently');
  const canon7 = E.canonDesign(JSON.parse(JSON.stringify(b7.design)));
  ok(canon7.hands['centre-second'].zoom === undefined, 'a zoom of 1 is dropped, so an untouched hand does not change the Design ID');
  E.assetClear();
  const i8 = mk();
  i8.faces = {}; i8.buildMovement = () => { };
  i8.importBundle(txt(JSON.stringify(b7)));
  await wait(40);
  const r8 = i8.state.designs.t.hands['centre-minute'];
  ok(r8.pivotX === -18 && r8.pivotY === 44 && r8.rot0 === 90 && r8.zoom === 1.5, 'the second browser gets all four numbers back intact', JSON.stringify(r8));
  const sp7 = E.handSpriteAsset('hand-1', { maxW: 466, maxH: 466, pivotX: r8.pivotX, pivotY: r8.pivotY, zoom: r8.zoom, rot0: r8.rot0 });
  ok(sp7 && sp7.px >= 0 && sp7.px <= sp7.c.width && sp7.py >= 0 && sp7.py <= sp7.c.height, 'and that restored pivot still lands inside the plane the device will rotate', JSON.stringify(sp7 && [sp7.c.width, sp7.c.height, +sp7.px.toFixed(1), +sp7.py.toFixed(1)]));
  ok(sp7 && sp7.rot0 === 90, 'the facing reaches the sprite record the manifest is built from');
  // both phases must reach their counts: a suite that silently skipped a block would
  // otherwise report green while checking nothing
  if (checked() < 30) console.log('FAIL only ' + checked() + ' checks ran across both phases');
  const bad = fails() + (checked() < 30 ? 1 : 0);
  console.log(bad ? '\n' + bad + ' FAILED' : '\nall green (' + checked() + ' checks)');
  process.exit(bad ? 1 : 0);
})();
