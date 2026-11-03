# av-kit: a real composer kit, kept as a test fixture

`watch-aviator-kit.zip` is a Watch Composer layer-kit export of the `Aviator` preset
(15 base frames at 15 fps, no tourbillon), 6,318,355 B. `DESIGN_ID` is the `WCD5-` seed
that regenerates its design, identical to the zip's `manifest.json` `designId`.

It was the source of the firmware's compiled-in clock bank, which §26 (P6.5) retired
along with FORMAT 2. Now it is the one real, browser-rasterized kit in the tree, and
`tools/orb-manager/web/runtime/_selftest/headless-orbtheme.test.js` bakes it two ways:
the bench path (`tools/prep_watch_kit.py`, then `tools/bake_clock.py --orbtheme`)
against the in-browser `GorbBake.bakeKit`. The two must agree byte for byte apart from
the bake stamp.

The zip is committed, not re-exported, because its pixels come from Chrome's PNG encoder
and a different Chrome build rasterizes antialiased edges a half-pixel differently. The
Python half is deterministic. The kit has no sub-dial and no tourbillon, so it is not an
input for those paths (`apps/clock/editor/_selftest/make_art_kit.py` makes one that is).
