# Weather: give Silver and Sectional their own icon packs

## Goal

Step 1 of the `looks` slice (`docs/specs/looks-design.md` §3, §5, §6; closes the first bullet of
`docs/app-platform-plan.md` §64.4). Weather's Silver and Sectional looks today *borrow* other
looks' icon packs (Silver → `engraved`, Sectional → `pictogram`), against weather-plan §0 ruling 6
("packs are never shared"). Add two new packs — **Guilloché** for Silver and **Charted** for
Sectional — each its own design, point the two looks at them, fix the one stale name in weather's
Scope `match`, then bump/rebake/ship weather. No product code beyond `apps/weather/editor/*` and
weather's shipped artefacts.

## Read first

- `docs/specs/looks-design.md` §3 (both packs' briefs and roles), §5 (shipping), §6–§7 (gate,
  pictures). `docs/app-platform-plan.md` §64.4.
- `apps/weather/editor/weather-bake.js`: `PACKS` (`:124`, `{title, note, roles}`),
  `makePack` (`:297`, one branch per pack returning `{frame?, sun, moon, cloud, rain, drizzle,
  snow, bolt, fog}`), the shared helpers (`S`, `circ`, `ell`, `ln`, `rad`, `poly`, `tx`,
  `crescent`, `pt`, `mix`, `f`) and shape constants (`CLOUD`, `BUMPS`, `BOLT`, `DROP`, `STAR4`), `iconPaths`
  (`:385`), `packOf`/`packStart` (`:222`,`:406`), `PRESETS_DEF` (`:135`; silver `:168`, sectional
  `:176`), `KINDS` (`:26`), `ICON_PX`/`SICON_PX` (`:30`), `finish`/`SHADOWS` (`:411`).
- `apps/weather/editor/weather-editor.js`: the Icons step (`:356`–`:371`) enumerates `B.PACKS` for
  the Pack dropdown and `pk.roles` for the colour pickers — a new pack appears with **no editor
  edit**. The 40-px two-of-three note is at `:371`.
- `_selftest/bake.html` (bakes every `B.PRESETS` + variants, one `B64 <name> …` each) and
  `_selftest/bake_check.py` (`WEATHER_BAKE_KEEP=<dir>` writes each `.orbtheme` — the byte check).
- Look anchors: `radar-bake.js:161` (`PRESETS.sectional`) and `watch-engine.js:1067` (clock
  `sectional`) for Charted's inks; `PRESETS_DEF.silver`'s graphite/silver plate for Guilloché.

## Contract

Modify only: `apps/weather/editor/weather-bake.js`, `apps/weather/version.txt`,
`tools/orb-manager/apps/weather-scope.orbtheme` (by rebake), `tools/orb-manager/apps/weather.orbapp`
(by make_factory_apps). Do **not** edit `weather-editor.js` (the Icons step picks new packs up from
`B.PACKS`) or any other look's `pack:`/preset field.

- Add to `PACKS` (`weather-bake.js:124`):
  - `guilloche: { title: 'Guilloché', note: 'engine-turned silver; a bright specular streak, one amber bolt', roles: [['metal','Body'],['hi','Highlight'],['deep','Shade'],['warn','Lightning']] }`
  - `charted: { title: 'Charted', note: 'aeronautical chart symbols; hollow navy linework with a magenta accent', roles: [['ink','Chart ink'],['accent','Magenta'],['paper','Paper knockout'],['warn','Magenta']] }`
- Add two `makePack` branches (before the `else` pictogram branch, `:297`):
  `if (name === 'guilloche') { … } else if (name === 'charted') { … }`, each returning a `pk` that
  draws **all** of `sun, moon, cloud, rain, drizzle, snow, bolt, fog` (optionally `frame`). Follow
  design §3a/§3b: cool single-tone engine-turned silver with a specular streak and a turned sun for
  Guilloché (one amber `warn` bolt, no rim); hollow navy chart glyphs with a magenta accent and a
  cream `paper` knockout for Charted. Colours come only from the role set `c`.
- Point the two looks at their packs (`PRESETS_DEF`):
  - `silver.icons = { pack: 'guilloche', c: { metal: '#C8CED6', hi: '#FFFFFF', deep: '#5A626C', warn: '#FF9A5C' }, shade: '3d', shadow: 'strong' }`
  - `sectional.icons = { pack: 'charted', c: { ink: '#1F2A44', accent: '#C2187A', paper: '#F3F5EC', warn: '#C2187A' }, shade: '3d', shadow: 'strong' }`
  (keep `shade: '3d'`, `shadow: 'strong'`).
- Stale name (design §1): `PRESETS_DEF.scope.match` `'Flights · Modern'` → `'Flights · Scope'`
  (`:136`) — the only Scope-field change.
- `apps/weather/version.txt`: `v1.7.1` → `v1.7.2`.

## Behaviour and edge cases

- Silver and Sectional now bake their **own** packs; no two looks share a pack.
- The Icons step lists Guilloché and Charted with their notes; each role is recolourable;
  `packStart('guilloche')`/`packStart('charted')` return the new presets' colours (each pack worn by
  exactly one theme), so Blank and the role resets offer them.
- The presets' 3D shading + strong drop shadow still apply (pack-agnostic `finish`).
- Every `KINDS` condition draws in both packs (a condition a pack does not draw would leave
  `iconPaths` emitting an empty shape and the bake losing a frame — check all ten).

## Acceptance criteria

Each is a command; run them all. `$EVID` = `$DELEGATE_EVIDENCE_DIR`.

1. `node --check apps/weather/editor/weather-bake.js && node --check apps/weather/editor/weather-editor.js` → both exit 0 (no output).
2. New packs present and wired (fails if absent):
   `grep -q "guilloche: { title: 'Guilloché'" apps/weather/editor/weather-bake.js && grep -q "charted: { title: 'Charted'" apps/weather/editor/weather-bake.js && grep -q "pack: 'guilloche'" apps/weather/editor/weather-bake.js && grep -q "pack: 'charted'" apps/weather/editor/weather-bake.js` → exit 0.
3. Nothing borrowed, six original packs still worn:
   `! grep -q "pack: 'engraved', c: { metal: '#C8CED6'" apps/weather/editor/weather-bake.js && ! grep -q "pack: 'pictogram', c: { ink: '#1F2A44'" apps/weather/editor/weather-bake.js && for p in phosphor engraved drafted folded nebula pictogram; do grep -q "pack: '$p'" apps/weather/editor/weather-bake.js || exit 1; done` → exit 0.
4. Stale name gone: `! grep -q "Flights · Modern" apps/weather/editor/weather-bake.js` → exit 0.
5. Real bake, all themes (needs Chromium; `bake_check.py` finds it via `tools/orbchrome.py`):
   `python3 apps/weather/editor/_selftest/bake_check.py 2>&1 | tee "$EVID/wx-bake.txt" | tail -1`
   → last line `20 themes baked, 0 failed`.
6. **Existing looks byte-identical** (design §6). Before editing run
   `WEATHER_BAKE_KEEP="$EVID/wx-before" python3 apps/weather/editor/_selftest/bake_check.py` (empty dir);
   after, `WEATHER_BAKE_KEEP="$EVID/wx-after" …`. Then
   `for n in paper brass blueprint station celestial blank map-saved-before paper-no-caps-mono saved-before-fields wide-gradient-bars icons-3d-shadow ground-carbon-relic ground-picture; do cmp "$EVID/wx-before/$n.orbtheme" "$EVID/wx-after/$n.orbtheme" || exit 1; done`
   → silent/exit 0 (record the transcript). Only `silver`, `sectional` and the Scope-derived names change.
7. Ship: `version.txt` = `v1.7.2`; `python3 tools/rebake_shelf.py weather`;
   `python3 tools/make_factory_apps.py`; `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` → passes.
8. Pictures (design §7): write `$EVID/wx-icons.html` that loads the runtime baker + `ground.js` +
   `weather-bake.js` (stage like `bake.html`), and for `B.preset('silver')` and `B.preset('sectional')`
   draws the ground (`B.plateCanvas`) and **all ten** `B.KINDS` at `B.sizeOf(d,'icon')` (72) and at
   `B.SICON_PX` (36); screenshot to `$EVID/wx-silver-icons.png` and `$EVID/wx-sectional-icons.png`
   with `chromium --headless=new --no-sandbox --hide-scrollbars --window-size=1400,900
   --screenshot=… "file://$EVID/wx-icons.html#<look>"` → Silver reads as engine-turned metal,
   Sectional as chart symbols, at true size, with no coloured rims.

## Out of scope

- Any other app, the other six weather packs, `weather-editor.js`, the kit/studio, firmware, hub Go.
- Do not rename the shelf file (`weather-scope.orbtheme`); do not touch `KINDS`, slots, the bake's
  element plan, fonts, temperature ramps, or the map.

## Notes

- A new pack must be **drawn differently**, not Engraved/Pictogram recoloured — that is still one
  shared drawing (weather-plan ruling 6, `gorb-os-weather-and-units.md`).
- `WEATHER_BAKE_KEEP` must be an empty dir before each run; take the "before" capture on the base
  tree (before editing) and the "after" capture once the edits are in.
- The Scope `.orbtheme` **will** change (its carried `match` string); that is the sanctioned
  stale-name edit (design §6), not a regression.
