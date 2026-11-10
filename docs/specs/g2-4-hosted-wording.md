# G2 step 4: the editors' words under a web host, and one release of each editor

## Goal

Owner ruling (2026-11-08), part of G2: when an editor runs on the site
(`window.GorbHost = {kind: "web", send: false, …}`, design §6.3), its visible text still talks
about the orb and the hub: "Send to orb", "pushes to the orb this hub has loaded",
"…from the hub", "on the hub as a draft". On the site Send is a download, and there is no
hub. Under a web host every label, note, tip and status line an editor shows must fit the
site; under `kind: "hub"` and with no `GorbHost` **every string stays byte-for-byte what it
is today**. The wording is driven from the runtime (`theme@1`, `studio@1`) as far as it can
be, so each editor changes as little as possible. Then clock, flights and weather each get
**one** version bump and one rebuild, carrying this step and steps 1–3.

## Read first

- `/home/greg/docker/Projects/orb-os-net/docs/design.md` (read-only mount): §6.3 (hosted
  mode: the flags, "an editor never checks `kind`, only the flags"), §0 ruling 5.
- `tools/orb-manager/web/runtime/theme@1/session.js` in full: the GorbHost block
  (`hostInfo`, `hostUrl`), every `say(…)` and thrown sentence; `theme@1/send.js`: the
  `send: false` path (G1 step 1), the status lines.
- `tools/orb-manager/web/runtime/studio@1/studio.js`: `StartSheet` (the library block,
  the empty-library line), `libraryRows`, `OpeningModal`, `SendModal`, `RenderPanel`,
  `sendProps`, and how the top bar draws `primary`.
- `apps/flights/editor/flights-editor.js` (around lines 540-600: the Render panel props,
  the start sheet, the library props, the top bar `primary`) and
  `apps/weather/editor/weather-editor.js` (around lines 480-545, the same places).
- `apps/clock/editor/Watch Composer.dc.html`: `isHub`/`hubSend3`, `sendNote`, `sendTip`,
  `themeNote`, the `primary`/`send`/`save`/`downloads` props near lines 3950-3980, the
  library rows near 4740, and the inline `<sc-if … mineEmpty>` library line near line 104.
- `tools/orb-manager/web/runtime/_selftest/editors-smoke.js` (G1) and the runtime suites
  `headless-studio.test.js`, `headless-host.test.js`, `headless-session.test.js`.
- `apps/clock/editor/_selftest/README.md`: the snippet that regenerates the extracted
  `component.js` after a composer edit (BUILDING.md § Apps, steps 1-3).

## Contract

### 1. The rule

- **Send wording follows the `send` flag** (`OrbTheme.hostInfo().send`). With `send: false`
  there is no "Send to orb" anywhere: the action that bakes is **Download .orbtheme**.
- **Where things live follows the host.** Under a web host there is no "hub", "this hub",
  "the hub's shelf/library", "the orb this editor was opened for", and no "Rebake all on the
  Themes tab". The library is "your library" / "the library". Sentences that describe
  what a theme does on an orb in general ("drawn live on the orb", "costs the orb nothing",
  "The theme's name on the orb") are about the product, not the host, and stay.
- **Editors do not read `kind`.** Add to `theme@1/session.js`, **outside** the
  byte-identical GorbHost block, right after it:
  ```js
  // the host's words: an editor passes the hub's sentence and the site's, and never reads kind
  OrbTheme.hostText = OrbTheme.hostText || function (hub, web) { return OrbTheme.hostInfo().kind === 'web' ? web : hub; };
  ```
  Runtime files may read `kind` (session.js already does); app code uses `hostText` or
  the flags. `studio@1` must keep working when `theme@1` is not loaded (radar loads
  neither; any editor might load studio alone): there it uses the hub wording.

### 2. The runtime

Under a web host, with today's strings kept exactly under the hub:

- `studio@1`: `RenderPanel`'s `send` button label (`Download .orbtheme` when `send` is
  false); `SendModal`'s title (`Baking the theme`) and `sendProps`' hints (one line:
  `The .orbtheme downloads when the bake is done.`; Cancel behaviour unchanged);
  `StartSheet`'s empty-library line; `libraryRows`' `tagTip`s and `delTip` (no hub, no
  Themes tab: e.g. `Baked: in your library` for a theme, and for a stale one, that
  opening it and downloading it again rebakes it); `OpeningModal`'s hint (no "orb's card").
- `theme@1/session.js`: every status line or error that names the hub
  (`The library could not be read from the hub`, `the hub answered <n>`, `the hub has no
  design for …`, the `offHub` sentence, `Saved … — Send to orb bakes them in`) gets a web
  form without the hub (`the server answered <n>`, `Download .orbtheme bakes them in`, …).
  Lines only the hub path can reach (`openTheme`'s card/shelf branch, `editor/source`,
  Rebake all's) stay as they are.
- `theme@1/send.js`: check the `send: false` path's lines are already site-shaped (G1); only
  change a line that a web host can reach and that names the hub or "Send to orb".

### 3. The editors (as little as possible)

- **flights** and **weather**: the top bar `primary` (`label: 'Send to orb'`, title `Bake and
  send this theme`) → under `send: false` `Download .orbtheme` / `Bake this theme and download
  it, to import in Orb Manager`. The Render panel's `send.note`/`send.tip` and `save.note`/`save.tip`,
  the start sheet's hint ("…open one from the hub library…") and the library's `empty`
  line → `hostText(hub, web)` forms that keep the hub text verbatim and drop the hub/orb-
  this-editor-was-opened-for wording under web (e.g. send note `save “X”, bake it and download
  it`, tip `Save it to your library, bake it, then download the .orbtheme: import it in Orb
  Manager to put it on an orb.`; save note `keep “X” in your library as a draft`).
- **clock**: under `send: false`, the composer behaves as its non-hub branch does for the
  send: the top bar's primary is its existing `Download .orbtheme` (`exportTheme`), and the
  Render panel has no `send` entry (its `downloads` list already offers Download .orbtheme);
  `save` stays (the library is the site's), with web wording for its note and tip. The
  `sendNote` sentence "pushes to the orb this hub has loaded", the inline empty-library line,
  the library rows' tips, and any other line a web host can reach that names the hub get web
  forms through `OrbTheme.hostText`. After editing the composer, regenerate
  `apps/clock/editor/_selftest/component.js` with the README's snippet so the clock suites
  test the shipped source.
- Find the rest yourself: grep each editor (and `studio@1`, `theme@1`) for `hub`, `Send to orb`,
  `this orb`, `the orb this`, `Themes tab`, `card` in user-visible strings, and decide per
  string whether a web host can show it. In your report, list **every** string you changed as
  a table: file, the hub text (unchanged), the web text. List the hub-only strings you left
  alone and why in one line each.

### 4. Tests

- Runtime suites: `headless-studio.test.js` (or a new block in `headless-host.test.js`)
  renders `RenderPanel`, `SendModal`/`sendProps`, `StartSheet`'s empty line,
  `libraryRows` and `OpeningModal` under no `GorbHost`, `{kind:'hub'}` and the web
  description: the hub two give exactly today's strings, the web one gives the new ones and
  no string containing `hub`, `Send to orb` or `Themes tab`. The same for the session's
  lines you changed, and `OrbTheme.hostText`.
- `editors-smoke.js`, a new check per theme@1 editor and case: take the dumped DOM with
  `<script>…</script>` and `<style>…</style>` removed, and collect the text nodes and the
  `title`, `placeholder`, `aria-label` and `alt` attribute values. **web**: none contains
  `Send to orb`, `this hub`, `the hub`, `hub’s`, `hub library`, `on the hub` (case-
  insensitive); **none/hub**: the collected strings are identical for the two cases, and,
  for flights and weather, include `Send to orb`. (The smoke loads the start of each editor;
  the Render step's strings are covered by the runtime suite and by your table.) Print the
  offending strings in the FAIL line.
- The clock suites stay green (`make check-editor`); flip no clock assertion about hub text.

### 5. One release of each editor

- Bump `apps/clock/version.txt` `v1.2.29` → `v1.2.30`, `apps/flights/version.txt`
  `v1.4.2` → `v1.4.3`, `apps/weather/version.txt` `v1.5.1` → `v1.5.2`. Nothing else's
  version moves (radar and color_picker are unchanged; the root `version.txt` is the
  firmware's).
- `python3 tools/make_factory_apps.py` (clang 21 is in this worker,
  `ORB_CLANG=/usr/lib/llvm-21/bin/clang` set). Only `tools/orb-manager/apps/{clock,flights,weather}.orbapp`
  change; radar and color_picker stay byte-identical.
- Anything else that pins the shelf's stamps or hashes (a Go test, `extract_test.go`'s pin,
  `tools/make_default_theme.py --check`, `tools/rebake_shelf.py --check`, the default theme,
  release notes tooling) is updated only as far as its own `--check`/test demands; list each
  in the report. If the default theme or a shelf theme must be rebaked because the clock's
  release changed, say so and run that tool's own rebuild rather than editing bytes.

## Acceptance criteria

From the repo root, `export PATH=/usr/local/go/bin:$PATH`.

1. `make check-web` → `all green`, including the new wording checks.
2. `make check-editor` → all suites green.
3. `make check-apps` → passes (shelf fresh, default theme and rebake checks fresh).
4. `make check-runner && make check-migrations` → pass (the rebuilt editors still open
   every fixture).
5. `make -C tools/orb-manager check` → passes.
6. `git diff --stat <base>` shows `version.txt` changes for exactly clock, flights, weather.

## Out of scope

- New buttons or flows (Publish, Send to my Orb Manager): only words, and the clock's
  send-vs-download choice under `send: false` described above.
- Hub-only text, `main/`, `components/`, `.github/`, `orbsdk.py`, the runner.
- Rewording anything under the hub: if a hub string looks wrong, list it as a suggestion.

## Notes

- Keep the voice of the existing strings (plain sentences, curly quotes where the file uses
  them, `’` escapes where the composer uses them).
- `GorbHost` block in `send.js`/`session.js` must stay byte-identical (a suite checks it):
  `hostText` goes outside it.
- Commits: plain subject lines in the repo's style, no Co-Authored-By or tool-credit trailers.
