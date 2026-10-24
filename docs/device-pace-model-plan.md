# Task: make the Composer's device-pace prediction proportional

You are editing the gorb-os Theme Editor. Work only inside
`tools/watch-composer/` unless a step below names another path. Read this whole
file before editing anything.

## Symptom to fix

The hands tab and the Movement tab append a device-pace prediction to the
frame-rate readout: `15 fps · device ~4 fps`. The `modern` preset claims 4. The
dial ran faster than that on real glass, and the shipped theme still beats at
its banked tier. The `4` is a clamp copied from a different dial.

The regression landed in one commit:

```
7649661 fix(composer): price the sweep-across-plates tax on banked carriers too
```

That commit changed one guard inside `_devPace` from
`if (!b.measured || b.rows > 0 || b.car)` to `if (!b.measured || b.car)`. It was
written because the sealed perpetual measured 48 train rows and so tripped its
own exemption. The removal pulled every banked dial with a date window down to
the perpetual's floor with it.

## Files and anchors

`tools/watch-composer/Watch Composer.dc.html`:

| Line | Symbol | Role |
|---|---|---|
| 496 | `b.dev = this._devPace(b);` | the only call site |
| 503 | `devFps()` | `min(budget().fps, b.dev.pace)` |
| 521-543 | `_devPace(b)` | the model you are rewriting |
| 1048, 1060 | `componentDidUpdate`, `startTimer` | the preview timer reads `devFps()` |
| 3326, 3382 | the two readouts | render `' · device ~' + HB.dev.pace` |

`tools/watch-composer/watch-engine.js` lines 2416-2446 define the two shadow
gates. Do not change the engine. The fix belongs in the Composer's prediction
only.

## Current model, verbatim

```js
  _devPace(b) {
    const out = { pace: b.fps, cross: 0, why: '' };
    if (!b.measured || b.car) return out;
    const c = this.cfg(), D = this.design();
    if (!(D.centre && D.centre.second === 1)) return out;
    if (c.tick && c.tick !== 'sweep') return out;
    const RE = { roman: 205, arabic: 200, regulator: 206, baton: 200, minimal: 205, sans: 205,
      railway: 202, sector: 205, diver: 200, station: 214, pilot: 204, none: 205 };
    const hc = (D.hands && D.hands['centre-second']) || {};
    const tip = Math.min(216, (RE[D.print] || 205) * 0.98 * (+hc.len || 1));
    const half = (x) => x.size != null ? Math.max(+x.size / 2, 18) : 28;
    const hit = (x) => x.host === 'dial' && (x.type === 'subdial' || x.type === 'moon' || x.type === 'window')
      && (+x.dist || 0) + half(x) > 18 && (+x.dist || 0) - half(x) <= tip;
    const n = (D.comps || []).filter(hit).length;
    if (!n) { out.why = 'Still dial sweeps clear of every plate: the device holds the tier rate, as the aviator does.'; return out; }
    out.cross = n;
    const shOn = +c.shMove > 0.01 && +c.shOp > 0.01;
    out.pace = Math.max(4, Math.min(b.fps, shOn ? 4 : 8));
    if (out.pace < b.fps) out.why = 'Seconds sweep crosses ' + n + ' live plate' + (n > 1 ? 's' : '') +
      ': each tick re-blits them under the wedge. Panel-measured 4 fps with cast shadows; ' +
      (shOn ? 'Cast shadows off predicts ~8.' : 'with the twins it measured 4.');
    return out;
  }
```

## Why the model is wrong: four defects, each mapped to an edit

The firmware cost is real, so keep the term. `gfx_commit` in
`main/gfx/orb_gfx.c` (row-major commit, comment at line 463 onward) restores
each marked row from the canvas and repaints every layer overlapping it, in
table order. `orb_sched_commit_layers` in `main/gfx/orb_sched.c` line 749 builds
that job list. A seconds wedge that clips a plate therefore forces that plate to
re-blit. The prediction has the right shape and the wrong arithmetic.

1. **The clamp is binary.** `Math.max(4, Math.min(b.fps, shOn ? 4 : 8))` returns
   4 for one crossed plate and for four. Current measured output:

   | preset | crossed | claims |
   |---|---|---|
   | `aviator` | none | 15 |
   | `sector` | none | 15 |
   | `skeleton` | centre seconds off | 15 |
   | `modern` | date window at r=150, subdial at r=104 | **4** |
   | `diver` | date window at r=150 only | **4** |
   | `perpetual` | four subdials at r=102 | 4 |

   Fix with Edit 1.

2. **No duty-cycle term.** The subdial at r=104 with a 29 px radius sits under
   the wedge for about 32 degrees of 360. A date aperture at r=150 sits under it
   for a few. `modern` pays any crossed-plate cost on roughly 13 percent of
   ticks and the bare-ring cost on the other 87. The model charges the worst
   tick on every tick. Fix with Edit 1.

3. **`half()` over-prizes an aperture.** The `Math.max(+x.size / 2, 18)` floor
   turns a window declared `size: 1` into a 36 px plate. A date window exposes a
   few pixels of disc behind the hand. Fix with Edit 2.

4. **The banked exemption was deleted rather than narrowed.** A carrier with a
   bank already pays a push inside its measured rows, and `b.fps` already
   reflects it. The crossed-plate term is marginal cost on top of that. It
   should not cut a banked dial two tier steps when its arc coverage is small.
   Fix with Edit 3.

## Edit 1: price the crossed arc, not the plate count

Replace the count `n` with a merged angular coverage fraction, then map that
fraction to a tier.

Comps already carry the geometry. Every entry in `D.comps` names `angle` and
`dist`, so do not derive the bearing from `asin`. Use the half-angle at the
plate's own radius for its width only:

```js
const w = (Math.asin(Math.min(1, half(x) / Math.max(1, +x.dist || 1))) * 180) / Math.PI;
```

Build one interval `[angle - w, angle + w]` per hit plate. Merge intervals
that overlap on the circle, so two plates sharing a bearing cost the sweep once.
Divide the merged total by 360 for `cov` in 0..1. Report it as `out.cov`. Keep
`out.cross` alive, because the existing tests and labels read it.

Map coverage to a pace with the two panel anchors:

```js
// anchors: aviator cov 0.00 -> tier 15 ; perpetual cov 0.317 -> 4 at tier 15
const K_TWIN = 2.31, K_PLAIN = 1.16;
const raw = b.fps * (1 - (shOn ? K_TWIN : K_PLAIN) * cov);
```

Snap `raw` to the tier ladder, descending: the highest of `[15, 12, 8, 4]` that
does not exceed `raw`, floored at 4. The glass runs tiers, so a fraction is not
a legal answer. Confirm these targets before writing anything else:

| preset | `cov` (twins on) | expected pace |
|---|---|---|
| `aviator`, `sector` | 0.00 | 15 |
| `diver` | ~0.01 | 12 |
| `modern` | ~0.10 | 8 |
| `perpetual` | ~0.32 | 4 |

`modern` must land at 8, not 4. `perpetual` must stay at 4. If your numbers
disagree, your merge is wrong, not the table.

## Edit 2: size a plate by what it exposes

Split the floor in `half()` by type. A subdial or moon really is a disc, so keep
its 18 px floor. A window exposes an aperture, so drop its floor to 6:

```js
const half = (x) => x.size != null
  ? Math.max(+x.size / 2, x.type === 'window' ? 6 : 18)
  : 28;
```

Add one exclusion to `hit()`: a comp whose `open` field exceeds `0.5` is a cut
into the dial rather than an opaque plate, so the wedge costs nothing crossing
it. Skip those.

## Edit 3: bound the tax for a banked carrier

Do not restore the `b.rows > 0` exemption. `7649661` killed it for the right
reason: `rowPrice()` measures plate motion with the hands hidden, so rows prove
nothing about the seconds wedge. Keep the term applying to banked carriers.

Bound it instead. When `b.rows > 0` the tick already pays a bank push, so the
marginal crossed-plate cost may not cut more than one tier step below `b.fps`:

```js
const step = [15, 12, 8, 4].filter((t) => t <= b.fps)[1] || 4;
const floorStep = b.rows > 0 ? step : 4;
out.pace = Math.max(floorStep, [15, 12, 8, 4].find((t) => t <= raw) || 4);
```

`perpetual` still reports 4, because its coverage reaches the floor on its own
merit. `modern` recovers a step. Record both constants in the comment above the
function, each naming the commit that measured it, so the next author re-fits
data instead of lore.

## Rewrite the `why` note

The note renders beside the readout at lines 3326 and 3382. State the coverage
percentage, the plate count, the shadow state, and the escape hatch. Keep it
under 200 characters. The current text claims `Panel-measured 4 fps`. That is
false for a dial never measured, so replace it with the predicted pace and its
cause. Name the `Cast shadows` control as the lever when twins are what push the
number down.

## Also fix: the shadow chip can lie about a legacy design

`tools/watch-composer/watch-engine.js` line 2430 defaults `mv` to 1 when the key
is absent. A theme authored before the `shMove` gate can carry `shOp: 0` with no
`shMove`, so the bake ships no twins while the `Cast shadows` row at line 3329
renders `on`. Derive the chip from the gate that actually drives the bake, or
migrate `shOp: 0` to `shMove: 0` in `canonDesign`. Do not leave the control and
the export disagreeing.

## Verification: run all of it, in this order

The suites need the node env the repo already uses. Nothing here needs a device.

```sh
export PATH=/home/greg/.local/share/nodeenv-gorb/bin:$PATH
cd /home/greg/docker/Projects/gorb-os/tools/watch-composer/_selftest
```

1. Regenerate the extracted component, or the node suites test stale code. Use
   the exact snippet in `_selftest/README.md` lines 375-384.

2. Syntax-check, then run the headless suites:

   ```sh
   node --check component.js
   node headless-test.js
   ```

   All suites must pass. `headless-ui.test.js` stubs `budget()` with a `dev`
   block; extend that stub with the new `cov` field so the label path stays
   exercised.

3. Prove the targets with the geometry probe. `probe_pace.cjs` sits untracked in
   `_selftest/` from the investigation that produced this plan; it mounts the
   real `Component` against `headless-stub.js` and prints the live `_devPace`
   output per preset, so it needs no new harness. Commit it with the fix.

   ```sh
   node probe_pace.cjs
   ```

   It prints one line per preset with `crossed=` and `-> pace=`. Compare against
   the Edit 1 table. `modern` must read 8 and `perpetual` must read 4. Update the
   probe to print `cov` as well, and keep it checked in as the regression trip.

4. Sync the embedded Composer copy, or the packaged hub serves the old model.
   `tools/orb-manager/web/composer/Watch Composer.dc.html` is a committed
   duplicate of the source. `TestEmbeddedComposerMatchesSource` fails when the
   two drift apart, and that drift already exists in the working tree today:

   ```sh
   cd /home/greg/docker/Projects/gorb-os/tools/orb-manager
   make composer
   go test -run TestEmbeddedComposerMatchesSource ./...
   ```

5. Run the Chrome suites that need a browser, per the run instructions in
   `_selftest/README.md` and the headless-Chrome recipe in `.clinerules`:
   `export-test.html`, `bake3-test.html`, `motion-test.html`. This box reaches the
   host Chrome directly at `/mnt/c/Program Files/Google/Chrome/Application/chrome.exe`
   and runs them `--headless=new --dump-dom` (launch detached, and strip `\r`).
   Verified this pass: `export-test.html` and `motion-test.html` report `RESULT: PASS`.
   `bake3-test.html` fails its `stillTrips` check on aviator/station/perpetual (48
   rows), a pre-existing failure reproduced on the parent commit and unrelated to the
   pace model — report it as a known pre-existing break, not as this change's doing.

6. Update `_selftest/README.md` and `docs/watch-composer-plan.md` to describe the
   coverage model. Note that the earlier `docs/fps-budget-findings-2026-10-10.md`
   flush model backs the cost mechanism.

## Constraints

- Change only `_devPace`, its `why` text, `half`, `hit`, and the chip-state bug.
  Leave `CAPS`, the tier ladder, and `_budget` alone.
- Do not touch the engine's shadow gates. The two-gate design is correct: `shOp`
  is art cost, `shMove` is per-tick re-blit cost.
- Never predict below 4. That is the scheduler floor, not a guess.
- Never predict a pace that exceeds the tier in `b.fps`.
- A dial with no sweeping centre seconds, or a `tick` choice other than `sweep`,
  or a caged carrier, keeps `pace = b.fps`. Preserve those early returns exactly.
- Keep `out.cross` populated. Existing assertions and the label use it.

## Commit messages

Two commits, in this order:

```
fix(composer): price the sweep-across-plates tax by crossed arc, not plate count
fix(composer): derive the cast-shadow chip from the gate the bake reads
```

Body of the first names the anchor presets and their measured rates, and cites
`7649661` as the commit that removed the banked exemption this change bounds.

## Report back

State the `pace` value the probe prints for each of `modern`, `diver`, `aviator`,
`sector`, `perpetual`. State whether the Go embed test passes. Name any Chrome
suite you could not run.
