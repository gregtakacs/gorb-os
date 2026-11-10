# theme@1 session: `?import=<ticket>` on the hub bakes a site bundle into the library

## Goal

Slice H1 of the orb-os-net design (`~/docker/Projects/orb-os-net/docs/design.md` §7.1).
Step h1-1 gave the hub `GET /api/site/bundle?ticket=` (a site's design bundle, held for one
import) and `POST /api/site/keep?ticket=` (files a baked `.orbtheme` named with the bundle's
shared name in the library, with its origin). This step teaches the shared editor runtime,
`theme@1/session.js`, to do the bake: an app's editor opened **on the hub** with
`?import=<ticket>` reads the bundle, opens it, bakes it under its shared name
(`<name> by @<handle>`), and posts the bake to the hub instead of downloading it.

This is W4b's site Download (`?shared=<id>&download=1`, `openShared` in the same file) with
a different source and a different sink. **Reuse that path; don't write a second baker.**
Factor the bake-and-check loop out of `openShared` so both use it. `openShared`'s behaviour
must not change, and its tests (section 10 of `headless-gorbhost.test.js`) must stay green
without being edited.

## Read first

- `tools/orb-manager/web/runtime/theme@1/session.js`, all of it, especially:
  - the header comment (lines 1–45);
  - `sharedOf` (~line 114) and the opening cover (~line 123);
  - `inflate` (~line 210);
  - `boot` (~line 413), and its `?entry=` / `?rebake=1` handling;
  - `openShared` and `openTheme` (~lines 555–660).
- `tools/orb-manager/web/runtime/_selftest/headless-gorbhost.test.js` section 10 ("the shared
  download"): its `run()`, `siteFetch`, `bundleOf`, `designOf2` and fake baker. The new section
  imitates it.
- `tools/orb-manager/web/runtime/host@1/host.js` line ~290: editors call `ses.boot()` and then
  `ses.openTheme()`, so a change inside the session reaches every editor.
- `docs/specs/h1-1-hub-site-import.md`: the hub doors and their error sentences (the
  session quotes the hub's `error` field as it is).

## Contract

### Recognising an import

- Add `importOf(q)`: `q.get('import')` when `OrbTheme.hostInfo().kind === 'hub'` and the
  value matches `/^[0-9a-f]{32}$/`, else `null`. Under `kind: "web"`, `?import=` is ignored completely.
- The opening cover at construction: with an import ticket, `api.opening = 'Opening the imported theme…'`.
  Put this check after `sharedOf` and before `?theme=` in that IIFE.
- `boot()`: with an import ticket, boot does everything it does today **except** the
  `?entry=` open and `?rebake=1`'s Rebake all. Those are skipped, so nothing races the import.
  The library is read as usual.
- `openTheme()`: right after the existing `if (sharedOf(q)) return await openShared(q);`,
  add `if (importOf(q)) return await openImport(q);`. An import wins over `?theme=` and
  `?entry=` on the same URL.

### The shared bake (factored out of openShared)

Factor the code from `var want = …` through the name check into one internal function, for
example `bakeUnder(want, mine)`. It answers `{bytes}` or `{why}`, where `why` is the text
after the `Not downloaded: ` / `Not imported: ` prefix:

- no `host.bake` → `this editor cannot bake here`;
- the bake answered null → the editor's own status line (the `ownLine` rule, `was`
  fallback), else `the bake did not finish`;
- the name check fails after the three attempts → `the bake came out named “<got>”`.

`mine` is the list of status-line prefixes that are the session's own, so they're never
quoted back as the editor's reason. Shared download keeps
`['Opening the shared', 'Baking', 'Downloaded', 'Not downloaded', 'Could not open']`.
Import uses `['Opening the imported', 'Baking', 'Imported', 'Not imported', 'Could not open']`.

`openShared` then says `Not downloaded: ` + why, exactly as today.

### openImport(q)

Status lines are verbatim. `<sn>` is the bundle's `sharedName`.

1. `api.opened = null`; say `Opening the imported theme…`.
2. `fetch(OrbTheme.hostUrl('/site/bundle?ticket=' + ticket))`.
   - not ok → `Could not open that theme: ` + (the JSON body's `error` || `the hub answered <status>`). Return false.
   - not JSON, or `format !== 'orb-design-bundle'` or `bundle !== 1` → `Could not open that theme: that is not a design bundle`. Return false.
3. `var sd = await inflate(bundle.design, bundle)`, then `api.apply(sd, 'the imported theme')`.
   A refusal (a newer schema, another app's design) → `Could not open that theme: ` + why.
   An exception → `Could not open that theme: ` + its message. Return false.
4. `api.restart(); api.sayOpened('the imported theme');`
5. `var want = api.bakeName(<the same own-name expression openShared uses>)`. If
   `want !== bundle.sharedName` → `Not imported: the design does not carry its shared name “<sn>”`. Return true.
6. Say `Baking “<want>” for the library…`, then `bakeUnder(want, mine)`. A `{why}` →
   `Not imported: ` + why. Return true.
7. `fetch(OrbTheme.hostUrl('/site/keep?ticket=' + ticket), {method: 'POST', headers: {'Content-Type': 'application/octet-stream'}, body: bytes})`.
   Read the JSON envelope. Not `ok` → `Not imported: ` + (`error` || `the hub answered <status>`).
   A network failure → `Not imported: ` + its message. Return true.
8. Success: say `Imported “<want>” into the library`, then `await api.libraryRefresh()`.
   Then, when `root.parent && root.parent !== root`, call
   `root.parent.postMessage({ type: 'theme.imported', app: <this session's app slug>, name: want, file: <data.file> }, root.location.origin)`
   inside try/catch. Return true.

Nothing is saved as a draft. `api.opened` stays null. The editor stays open on the imported
design afterwards, so the user can look at it, close it, or send it with the editor's own Send.

### Header comment

Add one bullet to the header list at the top of `session.js`:
`on the hub, an import (?import=<ticket>): a site's design bundle the hub fetched, baked here
under its shared name and kept in the hub's library with where it came from (orb-os-net §7.1).`

## Behaviour and edge cases

- `?import=` with a ticket that isn't 32 lowercase hex: ignored. The page behaves as if it
  were absent (no cover, nothing fetched).
- `?import=` together with `?shared=` on a hub page: `sharedOf` is web-only, so the import runs.
- A bundle whose design has a newer schema than the editor reads: refused at step 3 with
  the session's existing schema sentence. The UI (h1-3) shows that line.
- An unchanged `?shared=` on the hub still does nothing (the existing hub test in
  `headless-session.test.js` must still pass).

## Acceptance criteria

Run in `tools/orb-manager/web/runtime/_selftest` unless stated otherwise:

1. `node headless-gorbhost.test.js` → `all green`. It includes a new section 11, "the hub
   import (?import=<ticket>)". Base it on section 10's fakes, under `GorbHost = {kind: 'hub'}`
   (or no GorbHost), with a fake hub answering `/api/site/bundle` and `/api/site/keep`. It
   checks at least:
   - a good bundle opens, bakes under `Skeleton by @greg`, and posts exactly the baked bytes to
     `/api/site/keep?ticket=<t>`; no download happens; the status ends `Imported “Skeleton by @greg” into the library`;
     `theme.imported` is posted to the parent with the name and app;
   - the bundle's art is inflated: the applied design holds data URLs, not `orb-asset:` refs;
   - a bundle answer of 400 with `{"ok":false,"error":"that import has expired: paste the link again"}` → `Could not open that theme: that import has expired: paste the link again`, and no bake;
   - a keep refusal → `Not imported: <the hub's error>`;
   - a bake that comes out under the owner's name rather than the shared name → `Not imported: the bake came out named “Skeleton”` and nothing posted to keep;
   - a design without `shared.name` → the shared-name sentence, no bake;
   - `?import=NOTHEX` → nothing fetched; under `kind: 'web'`, `?import=<valid>` → nothing fetched from `/site/`;
   - with `?import=<t>&entry=Foo&rebake=1` the boot opens no entry and starts no Rebake all.
2. `node headless-gorbhost.test.js`: section 10 passes unchanged. `git diff a2ec9a0 --
   tools/orb-manager/web/runtime/_selftest/headless-gorbhost.test.js` shows only added lines.
3. `PY=python3 node headless-test.js` → passes. `node headless-session.test.js` → `all green`.
4. From the repo root, `cd apps/clock/editor/_selftest && node headless-test.js && GORB_HOST='{"kind":"hub"}' NODE_OPTIONS="--require=$PWD/../../../../tools/orb-manager/web/runtime/_selftest/gorb-host-preload.js" node headless-test.js` → both pass.
5. From the repo root, `PATH=/usr/local/go/bin:$PATH GOFLAGS=-buildvcs=false make check-web && make check-runner` → both pass.
6. `node -e "new Function(require('fs').readFileSync('tools/orb-manager/web/runtime/theme@1/session.js','utf8'))"` → no error.

## Out of scope

- `theme@1/send.js`. Its GorbHost block must stay byte-identical with `session.js`'s, so
  don't touch either GorbHost block.
- The Themes tab, `app.js`, `index.html` (step h1-3). The hub's Go code (h1-1).
- Editors under `apps/`: no app's editor changes. If one turns out to need a change to bake
  under the shared name on the hub, stop and report `blocked` with what you found.

## Notes

- The curly quotes `“ ”` and the ellipsis `…` are the session's existing style; use them exactly.
- A React editor applies a design on its next render, which is why the bake loop checks the
  header and bakes again (up to three bakes). That logic now lives in the shared function;
  don't simplify it away.
