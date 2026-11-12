# overlay-4: bump the four apps and re-bake the shelf

> **Read first: `docs/specs/overlay-design.md` §7 (why the bump is its own step) and §8 check 7.**
> Depends on overlay-2 and overlay-3.

## Goal

Put the field on the owner's dial. Bump each themed app's `version.txt` by one patch, re-bake its
shipped default theme with `make rebake`, and prove the four shelf themes now carry a `0x01` tag with
their own look's accent — so "Match the app" has something to match the moment a pomodoro ring asks
for it, without the owner opening an editor.

## Read first

- `docs/specs/overlay-design.md` §8 check 7.
- `tools/rebake_shelf.py` — its whole docstring (the bump rule, `--check`, the per-app drivers,
  `DEFAULTS` at :43-45) and `SHELF`.
- `Makefile` — `check-apps` (four gates: `orbsdk.py binds`, `make_factory_apps.py --check`,
  `make_default_theme.py --check`, `rebake_shelf.py --check`) and `rebake`.
- `apps/clock/editor/_selftest/bank3-drive.html` and each app's `_selftest/bake.html` — the drivers
  `rebake_shelf` actually runs.
- `docs/app-platform-plan.md` §52 (the format's version discipline).
- The versions this step starts from: clock `v1.7.1`, radar `v1.7.0`, weather `v1.7.1`, flights
  `v1.7.2` (`apps/*/version.txt`, read this session — re-read them, they move on `main`).

## Contract

1. Four `version.txt` bumps, each the next patch on whatever the branch holds: clock → `v1.7.2`,
   radar → `v1.7.1`, weather → `v1.7.2`, flights → `v1.7.3`. Nothing else in the apps changes: no
   `app.json`, no `main.c`, no schema.
2. `make rebake` (needs a Chromium; `ORB_CHROME=` picks one). That is the only way these bytes are
   made — a hand-edited container is not a bake, and the gate below is the bake's own.
3. `python3 tools/rebake_shelf.py --check` → each default names its app's release.
4. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` → all four gates green, including the two the
   container change could disturb: `make_default_theme.py --check` and `make_factory_apps.py --check`.
5. The read-back: for each of `tools/orb-manager/themes/*.orbtheme`, print the header's name, the
   overlay word via `orbbundle.file_overlay`, and the decoded RGB; require tag `0x01` and the accent
   step 3's rule predicts for that shipped design: `radar-scope.orbtheme` `#3ddc84`,
   `weather-scope.orbtheme` `#FFB23C`, `flights-scope.orbtheme` `#39FF14` — all three asserted. The
   clock's `skeleton.orbtheme` is **printed, not asserted**: the skeleton preset has no wheel with an
   accent and no `handLume`, so the rule says `#ff2d55`, and the run reports whatever it really reads.
   It is the interesting one either way, because it proves the derivation is a function over a design
   and not four colours pasted into four bakes. Any mismatch is a derivation bug: report it against
   step 3, never hand-patch a theme.

## Behaviour and edge cases

- **One app rebakes dirty.** If an app's own gate refuses (a budget, a font, the clock's residency
  check), stop and report that app; do not ship three of four with a stale fourth. `--check` will not
  let a stale one through `make check-apps` anyway, so a partial step is a red step.
- **No chromium on the box**: `make rebake` cannot run. Run `python3 tools/rebake_shelf.py --check`
  anyway and say plainly that the shelf was not re-baked here; do not hand-write container bytes to
  fake it, and do not bump versions without the bake (that is a card that lies about who made it).
- **The owner's card** already holds one of the four old themes: the hub offers an Update, and the old
  copy keeps working, drawing the ring in the app's own colours. That is what the tag is for, and no
  migration or cache flush is needed — `orb_theme.c` re-reads the region at bind.
- **`make web` / `make dist`** are release acts, not this step's; name them in the summary as the
  orchestrator's to run.

## Acceptance criteria

1. `python3 tools/rebake_shelf.py --check` → exit 0, all four lines fresh.
2. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` → exit 0.
3. `python3 -c "import sys,glob;sys.path.insert(0,'tools');import orbbundle as ob;W=sorted((p,ob.file_overlay(p)) for p in glob.glob('tools/orb-manager/themes/*.orbtheme'));print([(p.split('/')[-1],hex(w)) for p,w in W]);r=dict(W);assert len(W)==4 and all(w>>24==1 for _,w in W);assert r['tools/orb-manager/themes/radar-scope.orbtheme']==0x013ddc84 and r['tools/orb-manager/themes/weather-scope.orbtheme']==0x01ffb23c and r['tools/orb-manager/themes/flights-scope.orbtheme']==0x0139ff14;print('clock skeleton reads',hex(r['tools/orb-manager/themes/skeleton.orbtheme']))"`
   → exit 0, all four words `0x01`-tagged, the three map accents exact, and the clock's line printed.
   Its transcript goes in the evidence directory: the one output that shows the slice works end to end
   on real shipped bytes. The chain's verify command is exactly this one.
4. `git diff --name-only HEAD -- main apps tools | grep -v version.txt` → empty: the step touched
   versions and themes and nothing else.
5. Each theme file's byte size or CRC differs from its size at overlay-3's tip (`sha256sum` before and
   after the `make rebake`): a theme whose bytes did not move was not re-baked, and four unchanged themes
   with bumped versions is this step failing quietly.
6. `python3 tools/host_orb_bundle_c_test.py | tail -1` → still `RESULT: pass`, since the reader's
   contract is unchanged and a step that re-bakes should not disturb the suite that pins it.

## Out of scope

Any change to the derivation, the editors, the container, the ring or `orb_theme.c` — this step turns
the knobs and runs the ovens. No hub UI, no `.orbdesign` fixture edits, no plan-text edits (the repo
keeps no "shipped in" convention for rulings: `docs/app-platform-plan.md` names no step files, checked
by grep), no `make dist` / `make web`, and no shelf addition for the pomodoro — `apps/pomodoro` has no
theme yet, and its own overlay colour arrives with its editor slice (design §9).

## Notes

- The bump is what the hub shows as "made with v1.7.2", and `rebake_shelf.py` exists because a theme
  baked by an older editor than its app is a lie on the card. Shipping the byte change without the bump
  would leave four shelf themes stating a version that could not have produced them.
- `--check` compares the design's version stamp and not the bytes, which is why overlay-3 — which
  changes only what a bake *would* write — stayed green while the shelf was stale, and why this step is
  where the bytes actually move.
- If the orchestrator wants the field on the dial without the version churn, the alternative is leaving
  the shelf alone and letting owners re-bake by hand: the four shipped themes then answer "no colour"
  and every matching ring behaves as "From the look". Honest, and a worse first impression of §64.9.
