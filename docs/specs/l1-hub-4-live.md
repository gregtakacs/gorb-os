# L1 hub step 4: the browser gate for "Your library", and the full gate

## Goal

Slice L1 (orb-os-net ruling 42; `docs/specs/l1-design.md`). Steps 1–3 built the seam, the
picker, Orb Manager's doors and every editor's library pick, each proven by node suites. This
step proves the gate's editor half in a real browser, on the release's own layout (the
`make web` tarball, extracted as a site unpacks it): under the site's GorbHost the studio's
pickers offer no file input and a `Your library…` button instead; a flights design picks a
picture a clock design already holds and the save names that picture's sha256 — nothing new;
under a linked Orb Manager both controls show. It also takes the picker screenshot the owner
reviews, and runs the repo's full gate.

## Read first

- `docs/specs/l1-design.md` §4; `docs/specs/l1-hub-1-library-kit.md` (the picker's copy and
  classes), `docs/specs/l1-hub-3-editors.md` (where each editor's pickers are).
- `tools/orb-manager/web/runtime/_selftest/editors-smoke.js` — the whole file: how it serves an
  `export-runtime` tree and an `extract-editors` tree, injects `window.GorbHost`, drives
  headless Chromium over CDP, and answers the site's `/studio-api/*` shapes. The new script is
  its sibling and reuses its server and CDP code (require a shared piece out of it if that is
  cleaner, without changing what `editors-smoke.js` checks).
- The `check-web` target in `Makefile` (~134–142).
- orb-os-net's `src/lib/gorb-host.ts` `GORB_HOST` (the site's object; after L1 it also has
  `library: true`): `{ kind: "web", api: "/studio-api", send: false, publish: false, inbox:
  false, assets: true, designIds: true, library: true }`.

## Contract

New `tools/orb-manager/web/runtime/_selftest/library-live.js`, same arguments as
`editors-smoke.js` (`--runtime <dir> --editors <dir> [--chrome <path>] [--only <slug>]`), run by
`make check-web` right after `editors-smoke.js` (same extracted trees). Its stand-in site
answers, besides what an editor needs to open:

- `GET /studio-api/assets` → `{ ok: true, data: { assets: [A, B] } }` where A is a 64 × 64 PNG
  (generated in the script, real bytes) with `designs: [{ id: 'clockdesign0000000000a', app:
  'clock', name: 'Brass', current: true }]`, and B a 1.0 s sound made with
  `GorbBake.soundFileUrl` (in node, by requiring `baker@1/orbtheme-baker.js` from `--runtime`),
  `designs: [{ … app: 'pomodoro', name: 'Desk' … }]`. Every field of l1-design §2.3 is present.
- `GET /studio-api/asset?sha256=` → A's or B's bytes (`application/octet-stream`), else 404.
- `POST /studio-api/design/save` → recorded, answered as the site would (copy editors-smoke's
  answer shape).

Checks, each printed `PASS <name>` / `FAIL <name>: <why>`, exit 1 on any FAIL:
- `the site's pickers offer no file input: <slug>` for clock, flights, weather and pomodoro —
  open the editor under the site's GorbHost, bring at least one picture or sound picker on screen
  (open a stand-in design that shows one, or press through the editor's own tabs), and assert no
  `input[type=file]` exists except the StartFlow design Import (its `accept` names `.orbtheme`
  or `.orbdesign`), and at least one `Your library…` button.
- `the picker opens on the library: flights` — press `Your library…` on a flights picture row:
  the modal titled `Your library` lists A (with a 48 px thumbnail and `In Brass`), and not B.
- `a clock picture picked in flights stores nothing new` — pick A, then Save: the recorded save
  body names A's sha256 (an `orb-asset:<A>` ref with an `orbAssets` sidecar entry, or a data URL
  whose decoded bytes hash to A) and carries no other new picture.
- `a sound row offers only sounds: pomodoro` — the pomodoro `Your own` row's picker lists B, not A.
- `a linked Orb Manager shows both: clock` — open the clock under `{ kind: 'hub', library: true }`
  at the hub's URL shape: a picture row has both the file input and `Your library…`.
- `an unlinked Orb Manager is unchanged: clock` — with no GorbHost: the file input and no
  library button.
- Screenshot: with the flights picker open, `Page.captureScreenshot` of a 1280 × 800 viewport
  at `deviceScaleFactor: 1` (`Emulation.setDeviceMetricsOverride`), written to
  `$DELEGATE_EVIDENCE_DIR/l1-studio-picker.png` when that variable is set, else to
  `--shots <dir>` if given, else not written. Print `SHOT <path>`.

## Acceptance criteria (the full gate)

1. `make check-sandbox` → `check-sandbox: green apart from the named skips`.
2. `make check-web` → `editors-smoke.js` as before and every `library-live.js` PASS above; a
   `SHOT …/l1-studio-picker.png` line.
3. `make check-pictures check-runner check-migrations check-editor-origin` → pass.
4. `cd tools/orb-manager && go vet ./... && go test ./...` → ok.
5. `python3 tools/check_app_versions.py && python3 tools/rebake_shelf.py --check` → pass.
6. `(cd apps/clock/editor/_selftest && node headless-test.js)` and
   `(cd tools/orb-manager/web/runtime/_selftest && node headless-test.js)` → green.
7. Look at the screenshot (open the PNG); the modal must use the studio's existing look (its
   modal box, its buttons, its fonts), with nothing overlapping or cut off.

## Out of scope

- No product code changes, unless this gate finds a real defect in steps 1–3; then fix it in
  the smallest way, and name it in the report with the check that found it.
- Do not change what `editors-smoke.js` checks.
