const { ok, eq, wait, mk, E, txt, i1, d, fails, checked } = global.__T;

(async () => {
  // ---- manifest.assets[] ------------------------------------------------------------
  const assets = await i1.kitAssets(d);
  eq(assets.length, 4, 'kitAssets lists both backdrops, the hand and the flare');
  const bg = assets.find((a) => a.id === 'bg-1');
  ok(bg && bg.naturalSize[0] === 500 && bg.drawnSize[0] === 442, 'kitAssets names source and drawn box', JSON.stringify(bg && [bg.naturalSize, bg.drawnSize]));
  ok(bg && /^[0-9a-f]{64}$/.test(bg.sha256 || ''), 'kitAssets carries a real SHA-256 where Web Crypto offers one');
  ok(assets.every((a) => a.placed === true), 'every entry records that the design uses it');
  const hand = assets.find((a) => a.id === 'hand-1');
  eq(hand.usedBy, ['hand centre-minute'], 'the manifest says which slot used which asset');
  ok(assets.every((a) => /^data:image\//.test(a.dataUrl || '')), 'and each carries its bytes inline, as the design does');

  // ---- a dangling reference is reported, never hidden -------------------------------
  const i4 = mk();
  i4.state.designs.t.bgArt = 'nowhere-5';
  const dang = await i4.kitAssets(i4.state.designs.t);
  eq(dang.length, 1, 'one reference listed');
  ok(dang[0].missing === true && /design file or theme/.test(dang[0].note || ''), 'an unheld reference ships as missing with the fix named', dang[0].note);
  const fc = { subLayers: [{ id: 'c1', transparent: true }, { id: 'c2' }] };
  eq(i4.compHost(fc, { host: 'c1' }), 'dial', 'compHost: a plate that does not ship hands its comps to the dial');
  eq(i4.compHost(fc, { host: 'c2' }), 'c2', 'compHost: a real plate keeps its own comps');
  eq(i4.compHost(fc, { host: 'dial' }), 'dial', 'compHost: the dial is untouched');

  // ---- the design file round trip (D43 P7: the .orbtheme's design, on its own) --------
  // this suite's dial 't' is no shipped preset; Open accepts only a design naming one, so it is given one here
  E.PRESETS.t = E.PRESETS.t || { name: 'Test dial', design: { comps: [] } };
  const fileOf = (name, data) => ({ name, arrayBuffer: async () => { const u = typeof data === 'string' ? new TextEncoder().encode(data) : data; return u.buffer.slice(u.byteOffset, u.byteOffset + u.length); } });
  const i5 = mk();
  i5.state.designs.t = JSON.parse(JSON.stringify(d));
  let captured = null;
  i5.download = (blob, name) => { captured = { blob, name }; };
  await i5.exportDesign();
  ok(!!captured && /\.orbdesign$/.test(captured.name), 'Download design file saves an .orbdesign', captured && captured.name);
  const body = JSON.parse(await captured.blob.text());
  eq(body.format + ':' + body.schema, 'wc-design:1', 'the design file is named and stamped with its schema');
  ok(body.assets.length === 4 && body.assets.every((a) => a.dataUrl), 'the design file carries every uploaded image\u2019s bytes');
  ok(body.design.bgArt === 'bg-1' && body.design.sfArt === 'sf-1' && global.hand3(body.design, 'centre-minute').art === 'hand-1', 'the design file carries the design with every reference, both backdrop slots included');
  ok(assets.find((a) => a.id === 'sf-1').usedBy.join() === 'dial surface', 'the surface asset is attributed to the dial face, not the plate');
  E.assetClear();
  ok(!E.assetKnown('bg-1'), 'memory wiped: this is the second browser');
  const i6 = mk();
  i6.faces = {}; i6.buildMovement = () => { };
  await i6.openFile(fileOf('watch.orbdesign', JSON.stringify(body)));
  await wait(40);
  ok(E.assetReady('bg-1') && E.assetReady('flare-1'), 'Open brings the art into a browser that never had it');
  eq(i6.state.designs.t.bgArt, 'bg-1', 'Open restores the design');
  eq(global.hand3(i6.state.designs.t, 'centre-minute').pivotX, 60, 'the authored pivot survives the round trip');

  // ---- refusals name themselves ------------------------------------------------------
  let msg = '';
  i6.setState = function (p) { const n = typeof p === 'function' ? p(this.state) : p; Object.assign(this.state, n); if (n.status) msg = n.status; };
  await i6.openFile(fileOf('x.orbdesign', 'not json at all'));
  ok(/neither an \.orbtheme nor a design file/.test(msg), 'a file that is neither is refused in words', msg);
  await i6.openFile(fileOf('x.orbdesign', JSON.stringify({ hello: 1 })));
  ok(/not one this editor wrote/.test(msg), 'JSON that is not a design is refused', msg);
  await i6.openFile(fileOf('x.orbdesign', JSON.stringify(Object.assign({}, body, { schema: 9, appVersion: 'v9.0.0' }))));
  ok(/schema 9/.test(msg) && /v9\.0\.0/.test(msg), 'a future schema is refused rather than half-read, naming the version that made it', msg);
  const G = require(require('./runtime-paths.js').BAKER);
  const bare = G.writeBundle3({ name: 'Bare', sections: [{ slug: 'tst', elements: [], scalars: {}, sprites: [], art: new Uint8Array(16) }] }).bytes;
  await i6.openFile(fileOf('bare.orbtheme', bare));
  ok(/carries no design/.test(msg), 'an .orbtheme baked without a design says it cannot be reopened', msg);

  /* ---- the authored arbour and facing cross a browser boundary ----------------------
   * The design file (and the theme) is the thing that has to carry a pivot outside the
   * sprite and a facing that is not north. Both are numbers the bake reads, and a round
   * trip that quietly drops either ships a wrong hand. */
  const i7 = mk();
  i7.state.designs.t = { print: 'roman', comps: global.hands3({ shows: 'hour', id: 'centre-hour' }, { shows: 'minute', id: 'centre-minute', art: 'hand-1', pivotX: -18, pivotY: 44, rot0: 90, zoom: 1.5 }, { shows: 'second', id: 'centre-second', art: 'hand-1', rot0: 45 }) };
  i7.state.designs.t.bgArt = 'bg-1';
  let cap7 = null;
  i7.download = (blob, name) => { cap7 = { blob, name }; };
  await i7.exportDesign();
  const b7 = JSON.parse(await cap7.blob.text());
  const hm = global.hand3(b7.design, 'centre-minute');
  ok(hm.pivotX === -18 && hm.pivotY === 44, 'a negative pivot, i.e. an arbour off the visible ink, is written into the design file', JSON.stringify([hm.pivotX, hm.pivotY]));
  ok(hm.rot0 === 90, 'the facing goes with it, which is the only thing that makes non-north art read right', String(hm.rot0));
  ok(hm.zoom === 1.5, 'and so does the hand zoom', String(hm.zoom));
  ok(global.hand3(b7.design, 'centre-second').rot0 === 45, 'each hand carries its own facing independently');
  const canon7 = E.canonDesign(JSON.parse(JSON.stringify(b7.design)));
  ok(global.hand3(canon7, 'centre-second').zoom === undefined, 'a zoom of 1 is dropped, so an untouched hand does not change the design');
  // the same design, read out of a baked theme rather than a design file
  const themed = G.writeBundle3({ name: 'Themed', design: new TextEncoder().encode(JSON.stringify(b7)), sections: [{ slug: 'tst', elements: [], scalars: {}, sprites: [], art: new Uint8Array(16) }] }).bytes;
  E.assetClear();
  const i8 = mk();
  i8.faces = {}; i8.buildMovement = () => { };
  await i8.openFile(fileOf('themed.orbtheme', themed));
  await wait(40);
  const r8 = global.hand3(i8.state.designs.t, 'centre-minute');
  ok(r8.pivotX === -18 && r8.pivotY === 44 && r8.rot0 === 90 && r8.zoom === 1.5, 'Open reads the design out of an .orbtheme, all four numbers intact', JSON.stringify(r8));
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
