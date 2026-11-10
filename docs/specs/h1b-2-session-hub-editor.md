# theme@1 session: an editor opened at `/editor/hub/<slug>/` is a hub page with no orb

## Goal

Slice H1b of the orb-os-net design (`~/docker/Projects/orb-os-net/docs/design.md`, ruling 32).
Step h1b-1 gave the hub `GET /editor/hub/<slug>/<file>`: Orb Manager's own copy of an app's
editor, served from its app library with no orb. Today the shared runtime only treats a page
as "on the hub" when its path is `/editor/<ipv4>/<slug>/` (`api.hubOrb()` in
`theme@1/session.js`). Opened at `/editor/hub/<slug>/`, `hubOrb()` answers `null`, so the
library isn't read and `?import=<ticket>` refuses with the off-hub sentence.

This step makes the runtime recognise the orb-less hub page: `api.hubOrb()` answers
`{ip: null, slug: <slug>}` there, exactly the shape it already answers under a web host. With
that, the H1 import (`?import=<ticket>`) works unchanged on the orb-less page, and so do the
library, Save to library and opening library entries. Nothing else changes.

## Read first

- `tools/orb-manager/web/runtime/theme@1/session.js`:
  - `api.hubOrb` (~line 159–165) and every caller of `api.hubOrb()` (grep it): the library
    (~320), saves (~334), open/forget/rebake (~361–387), `boot` (~430–455), `importFile` (~538),
    `openImport` (~636–670);
  - `offHub` (~line 300).
- `tools/orb-manager/web/runtime/host@1/host.js` lines ~288–300, 407, 482, 542: it already
  handles `hubOrb()` answering `{ip: null}` (the web case: no places lookup).
- `tools/orb-manager/web/runtime/theme@1/send.js` lines ~68–95 (`findOrb`): with no `?orb=`
  it asks `/api/orb/known`. Leave it alone.
- `tools/orb-manager/web/runtime/_selftest/headless-gorbhost.test.js` section 11
  ("the hub import"), especially `irun(o)` and its `o.pathname` (~line 494–677).
- `docs/specs/h1-2-session-import.md`: the import's status lines.

## Contract

**Another branch is changing `session.js` at the same time** (the `hubOrb`/`hubJson` lines
~159–178, `boot`, and around ~325–345). So that the two merge cleanly:

- **Do not edit `api.hubOrb`'s body, `hubJson`, `boot`, or any existing line of
  `session.js`.** Only add lines.
- Add one block **immediately after the `offHub` / `err` / `extrasOf` lines (~300–303) and
  before `var saveDraft`**, nowhere else:

```js
    /* Orb Manager's own copy of an app's editor, opened with no orb (orb-os-net ruling 32):
     * /editor/hub/<slug>/ is a hub page whose library is the hub's, with no orb behind it.
     * It answers what a web host answers, {ip: null, slug}. */
    var hubOrbOfPath = api.hubOrb;
    api.hubOrb = function () {
      var o = hubOrbOfPath();
      if (o || OrbTheme.hostInfo().kind === 'web') return o;
      var m = /^\/editor\/hub\/([a-z0-9_-]{1,32})\//i.exec(String((root.location && root.location.pathname) || ''));
      return m ? { ip: null, slug: m[1] } : null;
    };
```

  (You may reword the comment; keep the behaviour, the regex and the placement.)
- Add one bullet to the header comment list at the top of `session.js`, **after** the existing
  `?import=` bullet (an added line only):
  `on the hub with no orb (/editor/hub/<slug>/): Orb Manager's own copy of the editor; the
  hub's library as on any hub page, no orb to ask (orb-os-net ruling 32).`

Consequences that need no code (check them in the test):

- `boot()` asks `/api/editor/source?ip=null&slug=…`; the hub answers 400 and boot's existing
  `catch` leaves `api.orbNote` empty. That is acceptable: don't add a guard (it would edit boot).
- `openImport` runs as on an orb's page: bundle, bake, keep, `theme.imported`.

## Behaviour and edge cases

- `/editor/192.0.2.7/tint/` still answers `{ip: '192.0.2.7', slug: 'tint'}`.
- `/editor/hub/tint/` answers `{ip: null, slug: 'tint'}` under `kind: 'hub'` (or no GorbHost).
- `/editor/hub/tint/` under `kind: 'web'` answers what web answers today
  (`{ip: null, slug: api.slug()}`); the wrapper must not change web.
- `/editor/hub/` (no slug), `/editor/hubx/tint/`, `/` → `null`, as today.

## Acceptance criteria

Run in `tools/orb-manager/web/runtime/_selftest` unless stated otherwise:

1. `node headless-gorbhost.test.js` → `all green`, with new checks added for "the hub's own
   editor with no orb (/editor/hub/<slug>/)". Add them as added lines only: either a new
   section 12 with its own small fakes, or new checks inside section 11 before its cleanup
   lines, reusing `irun({pathname: '/editor/hub/tint/'})`. They check at least:
   - `ses.hubOrb()` on `/editor/hub/tint/` is `{ip: null, slug: 'tint'}`; on
     `/editor/192.0.2.7/tint/` it is unchanged; on `/editor/hub/` and `/` it is `null`;
   - under `{kind: 'web'}` on `/editor/hub/tint/` it equals what web answers on `/`;
   - a good import on `/editor/hub/tint/?import=<t>` reads `/api/library/entries`, bakes
     under `Skeleton by @greg`, posts exactly the bake to `/api/site/keep?ticket=<t>`, ends
     `Imported “Skeleton by @greg” into the library`, and posts `theme.imported` to the
     parent with `app` and `name`;
   - the fake hub answering `/api/editor/source` with 400 `{ok:false,error:"expected ?ip=<ipv4>&slug=<app>"}`
     doesn't stop the import, and no `Heads up` line appears.
2. `git diff h1 -- tools/orb-manager/web/runtime/_selftest/headless-gorbhost.test.js` shows
   only added lines, and `git diff h1 -- tools/orb-manager/web/runtime/theme@1/session.js`
   shows only added lines.
3. `PY=python3 node headless-test.js` passes; `node headless-session.test.js` → `all green`;
   `node headless-send.test.js` passes.
4. From the repo root: `cd apps/clock/editor/_selftest && node headless-test.js` passes, and
   `GORB_HOST='{"kind":"hub"}' NODE_OPTIONS="--require=$PWD/../../../../tools/orb-manager/web/runtime/_selftest/gorb-host-preload.js" node headless-test.js` passes.
5. From the repo root: `make check-web && make check-runner` pass.

## Out of scope

- `send.js`, `host.js`, any app's editor, any Go, `app.js`, `index.html`.
- The GorbHost blocks in `session.js` and `send.js` (they must stay byte-identical).
- Guarding boot's `/editor/source` call, or anything else that edits an existing line.
