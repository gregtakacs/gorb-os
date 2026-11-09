# G1 step 1: the `GorbHost` seam in `theme@1` (hosted mode)

## Goal

Slice G1 of the orb-os-net design (§12), first step: give the editor runtime's `theme@1`
one seam, a host description the page sets before the editor loads
(`window.GorbHost`), so the same editor can run under Orb Manager (the default,
`kind: "hub"`, where nothing changes) or under a website (`kind: "web"`), where the
hub's paths live under another base, `/studio-api` by default, and Send to orb becomes a
download. No app's editor changes: the seam is entirely inside
`tools/orb-manager/web/runtime/theme@1/session.js` and `send.js`.

## Read first

- `/home/greg/docker/Projects/orb-os-net/docs/design.md` (read-only mount):
  §0 rulings 4 and 7, §1 (the bullet "What the editor asks of the hub"), §6.3 (hosted
  mode, the table), §4.4 point 4. Only these matter for this step.
- `tools/orb-manager/web/runtime/theme@1/session.js` and `send.js`: read both in full.
  Every hub call the editor makes goes through them (plus `/api/theme/stem` in the clock,
  which is out of scope).
- `tools/orb-manager/web/runtime/_selftest/headless-session.test.js`,
  `headless-send.test.js`, `fake-hub.js`, `headless-test.js`: the test style to imitate
  (plain node, `ok(cond, name)`, one process per suite, a timeout guard).
- `apps/flights/editor/flights-editor.js` lines 220-270 and 540-580, and
  `apps/clock/editor/Watch Composer.dc.html` lines 2400-2450: how real editors call
  `ses.hubOrb()`, `sender.send`, `sender.post`, `sender.findOrb`. They must keep working
  unchanged.
- `Makefile` (root), target `check-editor`.

## Contract

### 1. The host block (identical in `send.js` and `session.js`)

Both files get the same block, placed right after
`var OrbTheme = root.OrbTheme = root.OrbTheme || {};`, between these two exact marker lines:

```
  // ---- GorbHost: keep this block byte-identical in theme@1/send.js and theme@1/session.js
  ...
  // ---- end GorbHost
```

Either file may load alone, so each must define it, and whichever loads first wins
(`OrbTheme.hostInfo = OrbTheme.hostInfo || function () { ... };`, same for `hostUrl`).
It defines:

- `OrbTheme.hostInfo()` → a new frozen object `{kind, api, send, publish, inbox}`, read from
  `root.GorbHost` **on every call** (never cached):
  - `raw` = `root.GorbHost` if it is a non-null, non-array object, else `{}`.
  - `kind` = `'web'` if `raw.kind === 'web'`, else `'hub'` (any other value, missing, or
    wrong type is `'hub'`).
  - `api` = `raw.api` when it is a string that matches
    `/^\/[A-Za-z0-9_~-][A-Za-z0-9._~-]*(?:\/[A-Za-z0-9_~-][A-Za-z0-9._~-]*)*$/` **and** none
    of its `/`-separated segments is `.` or `..`; otherwise `'/studio-api'` for `kind
    'web'` and `'/api'` for `kind 'hub'`. (So `https://x`, `//x`, `/a/`, `/a//b`, `a`,
    `/a b`, `/a/../b`, `/..`, `''` and non-strings all fall back.)
  - `send` = `raw.send` if it is a boolean, else `kind === 'hub'`.
  - `publish` = `raw.publish === true`; `inbox` = `raw.inbox === true`.
- `OrbTheme.hostUrl(p)` → `OrbTheme.hostInfo().api + p`, where `p` is a hub path with its
  leading `/api` removed, e.g. `hostUrl('/library/entries?app=clock')` →
  `/api/library/entries?app=clock` under the hub, `/studio-api/library/entries?app=clock`
  under `{kind:'web'}`.

A short comment above the block says what it is: the page sets `window.GorbHost` before
the editor loads; with no `GorbHost`, or `kind: "hub"`, nothing changes; editors read the
flags (`send`, `publish`, `inbox`) through `OrbTheme.hostInfo()` and never `kind`
(orb-os-net design §6.3). Keep it to a few lines; no design-doc essays.

### 2. `session.js`

- Every hub request goes through `OrbTheme.hostUrl`: `/api/design/save`,
  `/api/library/entries`, `/api/library/design`, `/api/library/forget`,
  `/api/library/keep`, `/api/theme/design`, `/api/editor/source`. After the change the
  string `'/api/` must not appear in `session.js` outside comments.
- `api.hubOrb()`: when `OrbTheme.hostInfo().kind === 'web'` it returns
  `{ ip: null, slug: api.slug() }` (the site's library stands where the hub's is; there is no
  orb); otherwise exactly today's behaviour. This is what makes the editors' existing
  `ses.hubOrb() ? … : …` library UI work unchanged on a site.
- `boot()`: under `kind 'web'` it does not call `editor/source` (there is no orb to compare
  with) and leaves `api.orbNote` as `''`. Everything else in boot is unchanged (the
  localStorage clean-up and move, `libraryRefresh`, `?entry=`, `?rebake=1`).
- `openTheme()` under `kind 'web'`: `?theme=<id>` (same id regex as today) fetches
  `OrbTheme.hostUrl('/theme/design?id=' + encodeURIComponent(id))`; `orb` and `src` are
  ignored. Copy text, verbatim, under `kind 'web'` only:
  - the opening line (both the initial `api.opening` set at construction and the `say`):
    `Opening the design…`
  - failure prefix: `Could not open that design: ` + (the server's `error`, else
    `the server answered <status>`, else the exception message, or the `apply` refusal)
  - `host.apply(doc, 'the design')`, and on success `api.sayOpened('the design')`.
  Under `kind 'hub'` every string stays exactly as it is today.
- Nothing else in `session.js` changes behaviour.

### 3. `send.js`

- Every hub request goes through `OrbTheme.hostUrl` (`/orb/known`, `/theme/commit`,
  `/theme/upload` with its `&precheck=1` and `&overwrite=1`, `/theme/upload-status`).
  After the change `'/api/` must not appear in `send.js` outside comments.
- When `OrbTheme.hostInfo().send` is `false`:
  - `api.findOrb(fresh)` makes no request, sets nothing in its cache, calls `host.orb(null)`
    when the host has `orb`, and resolves `null`. The `?orb=` query is ignored.
  - `api.post(bytes, meta, overwrite)` makes no request and constructs no
    `XMLHttpRequest`. After the `api.cancelled()` checkpoint (unchanged) it saves the bytes
    with `(host.download || OrbTheme.download)(new Blob([bytes], { type:
    'application/octet-stream' }), file)` where `file` = `(meta.stem || meta.name ||
    'theme') + '.orbtheme'`, then says, verbatim:
    `Downloaded "<name>" as <file> — import it in Orb Manager to send it to an orb`
    (`<name>` is `meta.name`). If neither `host.download` nor `OrbTheme.download` exists it
    says `Not sent: this page cannot send to an orb, and it has no download` instead.
  - `api.send`, `api.run`, `api.replace`, `api.dismiss`, cancel and the busy message stay as
    they are (a send under `send: false` still runs as one modal act and ends in the
    download).
- `OrbTheme.sender(host)` accepts an optional `host.download(blob, name)`; add it to the
  usage comment at the top of the file.
- When `send` is `true` (the hub default, or a web host that sets `send: true`) the
  behaviour is exactly today's, at `hostUrl` paths.

### 4. Tests

New suite `tools/orb-manager/web/runtime/_selftest/headless-host.test.js` (picked up by
the existing `headless-test.js` runner by its name). Imitate `headless-session.test.js`.
It must check at least:

1. **Normalisation table** for `OrbTheme.hostInfo()`: `GorbHost` undefined, `null`, `[]`,
   `'web'`, `{}`, `{kind:'hub'}`, `{kind:'web'}`, `{kind:'WEB'}`, `{kind:'web', api:'/x/y'}`,
   each bad `api` listed in §1, `{kind:'hub', send:false}`, `{kind:'web', send:true}`,
   `{publish:'yes', inbox:1}` (both stay false). The result is frozen, and changing
   `GorbHost` between two calls changes the answer.
2. **The block is shared**: the text between the two marker lines is byte-identical in
   `send.js` and `session.js`; requiring only `send.js` in a fresh process defines
   `hostInfo`/`hostUrl` (spawn a child `node -e` for this, or a second small file run
   with `child_process.spawnSync`; keep it inside this suite).
3. **Hub is unchanged**: drive one scripted session scenario through `fake-hub.js` (a
   `slug: 'tint'` app like `headless-session.test.js`'s, at
   `location.pathname = '/editor/192.0.2.7/tint/'`): boot, save a draft, list, open the
   entry, forget the draft, keep (via `rebakeAll` or `api.saveDraft` + a theme in
   `hub.themes`, your choice), and `openTheme` with `?theme=x&src=shelf` answered by the
   `rest` hook. Record every `[method, url]` the fetch stub sees. Run the scenario three
   times, in fresh sessions with fresh fake hubs: `GorbHost` undefined,
   `{kind:'hub'}`, and `{kind:'hub', api:'/api'}`. The three recordings are equal, and
   every url starts with `/api/`.
4. **Web mode**: the same scenario with `GorbHost = {kind:'web', api:'/studio-api',
   send:false, publish:true, inbox:true}` at `location.pathname = '/v1.4.2/'` and
   `?theme=abc123` for the open. The fetch stub maps a `/studio-api/` prefix onto
   `/api/` before handing it to the fake hub, and records the original. Every recorded
   url starts with `/studio-api/`; none starts with `/api/`; there is no `editor/source`
   call; the recording equals the hub recording with `/api/` replaced by `/studio-api/`,
   minus the `editor/source` call and with the theme open as
   `/studio-api/theme/design?id=abc123`; `ses.hubOrb()` is truthy; the draft round-trips;
   the open's status line and `apply` source are the web strings of §2.
5. **Sender, `send: false`**: `findOrb()` resolves `null` with zero fetches and calls
   `host.orb(null)`, also with `?orb=192.0.2.9` in the query; `post()` makes zero fetches and
   constructs zero `XMLHttpRequest`s (stub the constructor and count), calls the download
   once with the exact bytes and the name `<stem>.orbtheme` (and `<name>.orbtheme` when
   `meta.stem` is null), and sets the exact status line; `send(name, bake)` whose `bake`
   calls `post` ends with `active === false` and the download made.
6. **Sender, web with `send: true`** and `api:'/studio-api'`: `findOrb()` asks
   `/studio-api/orb/known`; a `post` asks `/studio-api/theme/upload?ip=…&precheck=1` first.

### 5. The clock suite under an explicit `kind: "hub"`

- New file `tools/orb-manager/web/runtime/_selftest/gorb-host-preload.js`: when
  `process.env.GORB_HOST` is set, `globalThis.GorbHost = JSON.parse(process.env.GORB_HOST)`;
  otherwise it does nothing. One short header comment saying it exists so a suite can run
  under an explicit host description via `NODE_OPTIONS=--require=<this file>`.
- Root `Makefile`, `check-editor`: after the existing clock line, add a second run of the
  clock suite under the explicit hub description:
  `cd apps/clock/editor/_selftest && GORB_HOST='{"kind":"hub"}' NODE_OPTIONS="--require=$(CURDIR)/tools/orb-manager/web/runtime/_selftest/gorb-host-preload.js" $(NODE) headless-test.js`
  with a one-line `@#` comment above it. Leave every other line of the target as it is.

## Acceptance criteria

Run from the repo root. All must pass.

1. `cd tools/orb-manager/web/runtime/_selftest && PY=python3 node headless-test.js` →
   `all 12 suites green` (one more than the 11 before).
2. `cd tools/orb-manager/web/runtime/_selftest && node headless-host.test.js` → exit 0, no
   `FAIL` line.
3. `cd apps/clock/editor/_selftest && node headless-test.js` → all suites green.
4. `cd apps/clock/editor/_selftest && GORB_HOST='{"kind":"hub"}' NODE_OPTIONS="--require=$PWD/../../../../tools/orb-manager/web/runtime/_selftest/gorb-host-preload.js" node headless-test.js`
   → all suites green.
5. `grep -n "'/api/" tools/orb-manager/web/runtime/theme@1/session.js tools/orb-manager/web/runtime/theme@1/send.js`
   → only comment lines (ideally nothing).
6. `cd tools/orb-manager && go test -count=1 -run 'JS|Runtime|Editor|Vendor' . && go vet .`
   → ok (the hub embeds `web/`; its JS smoke checks must still pass).

## Out of scope

- Any change under `apps/` (the editors, their `version.txt`, `.orbapp` files). An editor
  change is a new app release; this step must not need one.
- Publish / Send to my Orb Manager buttons or actions, the signed-out browser library, any
  site code, `/api/theme/stem`, `studio@1`, `support@1`, `preview.js`, `measure.js`.
- The hub's Go handlers, `fake-hub.js`'s routes (extend it only if a test truly needs it,
  and say so), `tools/orb-manager/web/app.js`.
- Do not modify `main/`, `components/`, `apps/`, `.github/`.

## Notes

- Keep the code style of the files: ES5 `var`/`function` inside the IIFE, single quotes,
  terse comments that say why, the existing status-line voice.
- `send.js` is loaded before `session.js` by every shelf editor, but the clock's node
  suites load them separately; that is why the block lives in both.
- The site will inject `<script>window.GorbHost = {...}</script>` into the editor's entry
  document before any runtime script; nothing in this step depends on how.
- Commits: plain subject lines in the repo's style (`Editor runtime: ...`), no
  Co-Authored-By or tool-credit trailers.
