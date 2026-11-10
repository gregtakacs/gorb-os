# theme@1 on a site: a save refused because the editor was updated reopens the design in the new editor

## Goal

orb-os-net design §6.5, row "A tab left open across an update". This is the editor half of
orb-os-net slice M1. A site saves only from its newest live editor. When a tab that was left
open sends a save after a newer editor went live, the site refuses it with a 409 that names the
new version. `theme@1`'s session then has to keep the work:

1. it puts the design document into `sessionStorage`;
2. it reloads into the new version;
3. on boot, the new editor finds the stashed document and opens it through `api.apply`, which
   goes through `api.check` and so runs any migration steps;
4. it clears the stash, and the user saves again.

Nothing is lost. The studio origin is the same for every version of an app, so the new editor
can read what the old one stored. On a hub (`kind` "hub", or no `GorbHost`) nothing changes.

## Read first

- `tools/orb-manager/web/runtime/theme@1/session.js`, the whole file. Key parts:
  - `hubJson` (around line 167): the `{ok, data}` reader that throws the server's sentence;
  - `saveDraft` / `saveEntry` (around 304–345): `saveEntry`'s catch is where a refused save
    says `Not saved: …`;
  - `boot` (around 422–460): what a page open does after the library loads. It opens `?entry=`
    or runs `?rebake=1`, and an import page opens nothing;
  - `api.apply`, `api.check`, `api.migrate`, `api.restart`, `api.sayOpened`, and `api.opened`
    (the `{id, name}` of the site design this page holds, set by the `designIds` flag);
  - `OrbTheme.hostInfo()` / `hostText`: editors read the flags and never read `kind`, but the
    session itself may check `hostInfo().kind === 'web'` as it already does.
- `tools/orb-manager/web/runtime/host@1/host.js` line ~290. The page calls `ses.boot()` and then
  `ses.openTheme()`. `openTheme` with no `?theme=` / `?shared=` / `?import=` in the query does
  nothing. Confirm this; the reload below drops the query so that it does nothing.
- `tools/orb-manager/web/runtime/_selftest/headless-session.test.js`. It is the suite to extend:
  - it fakes `localStorage`, `location` and `fetch` (via `fake-hub.js`);
  - it already runs cases under a web `GorbHost` (around line 386, `global.GorbHost = host`, with
    `/studio-api` mapped onto the fake hub).
- `tools/orb-manager/web/runtime/_selftest/headless-gorbhost.test.js`, the web-host suite H1
  extended. Only read it. Do not change any existing check in it.

## Contract

**The site's refusal** (orb-os-net M1 defines it; the hub never sends it). It is the answer to
`POST <api>/design/save…`:

```
HTTP 409
{"ok":false,"error":"Clock's editor was updated","updated":{"version":"v1.6.2"}}
```

- `updated.version` is the version to reload into. It matches `/^v\d+\.\d+\.\d+$/`, and it is
  the first path segment of the studio origin's editor URL (`/<version>/`).
- `error` is a sentence to show. Never parse it.

**`hubJson`:** when the answer is refused and `j.updated` is an object whose `version` matches
the grammar above, the thrown Error carries `e.updated = { version: <that string> }`. Nothing
else about `hubJson` changes.

**The stash.** `sessionStorage` key `orb.handoff.<slug>`, where slug is `api.slug()`. The value
is a JSON string:

```
{"format":1,"app":"<slug>","to":"<updated.version>","from":"<api.version()>","name":"<entry.name>",
 "doc":<the design document entry.doc, as given to saveEntry — art inline, never refs>,
 "extra":<extrasOf(entry)>,"opened":<api.opened or null>,"at":<Date.now()>}
```

Add `api.handoffKey = function () { return 'orb.handoff.' + api.slug(); };` beside
`api.sessionKey`.

**In `saveEntry`'s catch**, only when `OrbTheme.hostInfo().kind === 'web'` and `e.updated` is
set:

1. Write the stash. If `sessionStorage` is missing, or `setItem` throws (quota: a design can be
   up to 10 MB), do not reload. Say exactly:
   `Not saved: <e.message>, and this design is too big to carry over to it: use Export to keep it as a file, then reload and import it`
   and return false.
2. Otherwise say `Not saved: <e.message>. Reopening your design in <to>…` (U+2026). Then call
   `root.location.replace(<target>)` and return false. `<target>` is:
   - the current `location.pathname` with its first segment replaced by `<to>`, when that first
     segment matches `/^v\d+\.\d+\.\d+$/` (for example `/v1.6.1/` becomes `/v1.6.2/`, and
     `/v1.6.1/index.html` becomes `/v1.6.2/index.html`);
   - otherwise `'/' + to + '/'`.

   The query is always dropped. The new page must not reopen `?theme=<id>`'s stored version
   over the stash.
- Every other refusal behaves exactly as today (`Not saved: <message>`). So does any refusal on
  a hub, even one that carries `updated`.
- `saveDraft` itself is unchanged. The quiet path (Send's first step, `quiet` true) gets the same
  hand-off: a quiet save that is refused with `updated` still stashes and reloads.

**In `boot`**, only under `kind` "web":

- It runs after `api.libraryRefresh()` and after the `?shared=` early return (a shared link page
  never reads a stash).
- If `sessionStorage.getItem(api.handoffKey())` holds a stash with `format === 1` and
  `app === api.slug()`:
  1. remove the key first, so a stash that makes the editor throw can't loop;
  2. `var why = api.apply(stash.doc, 'the design you were editing')`;
  3. if `why` is set, say
     `Your design from <from> could not be reopened in <api.version()>: <why>` and continue boot
     as if there were no stash;
  4. otherwise:
     - call `api.restart()`;
     - set `api.opened` to `stash.opened` when it is an object with an `id` matching
       `/^[A-Za-z0-9_-]{22}$/` and a string `name`, and to `null` otherwise;
     - say `<App>'s editor was updated to <api.version()>: your design is back as you left it. Save it again to keep it`.
       `<App>` is the `<slug>` (the session has no title; keep it simple and use `api.slug()`
       verbatim);
     - skip the `?entry=` / `?rebake=1` handling.
- A stash that does not parse, has another `app`, or has another `format` is removed and ignored.
- On a hub, `boot` never reads or removes the key.

## Behaviour and edge cases

- The stash is per tab (`sessionStorage`) and per app (the key). Two tabs of one app each carry
  their own design.
- `api.apply` goes through `api.check`. So:
  - a stash of an older schema is migrated by the new editor's steps;
  - a stash with no step is refused with `check`'s own sentence, in the could-not-be-reopened
    line.
- After a successful reopen, the next Save posts with `&id=<opened.id>` when `opened` came back,
  or with `&saveAs=1` when it was null. That is today's `saveDraft` rule; nothing new is needed.
- A refused save with `updated`, where the page is already on `updated.version`, still stashes
  and reloads. The site decides, and the reload re-binds its session. Do not add a loop guard
  beyond the remove-before-apply above.
- No new UI, no new buttons, and no change to `send.js` or the GorbHost block (keep that block
  byte-identical between `send.js` and `session.js`).
- Update the header comment of `session.js` with one bullet for the hand-off (orb-os-net §6.5).

## Acceptance criteria

New checks in `headless-session.test.js`, under a web `GorbHost` (imitate the block around line
386). Fake `global.sessionStorage` with a Map-backed object, the same way `localStorage` is
faked, and fake `location.replace` to record its argument. The checks:

1. A save answered 409 with `{"ok":false,"error":"Clock's editor was updated","updated":{"version":"v9.9.9"}}`:
   - `saveDraft` returns false;
   - `location.replace` was called with `/v9.9.9/` (pathname `/v1.0.0/`, query dropped);
   - the stash under `orb.handoff.<slug>` parses, and has `format` 1, `to` `v9.9.9`, the entry's
     name and doc, and the `opened` the session held;
   - the status line starts `Not saved: Clock's editor was updated. Reopening your design in v9.9.9`.
2. The same answer when `sessionStorage.setItem` throws: no `location.replace`, and the line
   contains `too big to carry over`.
3. A 409 without `updated`, and a 400: no stash, no replace, and today's `Not saved: <error>`.
4. The same 409 with `updated` under a hub (no `GorbHost`): no stash, no replace.
5. A boot with a stash whose doc is one schema older than the fake host's schema (give the fake
   host a migrate step):
   - `apply` received the migrated doc (schema bumped);
   - the stash key is gone;
   - `api.opened` equals the stash's `opened`;
   - the line contains `your design is back as you left it`;
   - a following `saveDraft` posts with `&id=<that id>`.
6. A boot with a stash whose schema has no step: the key is gone, the line starts
   `Your design from`, and boot still returns true.
7. A boot with a stash for another app, or one that is not JSON: the key is removed and nothing
   is applied.
8. A boot under a hub with a stash present: the key is untouched.

All of these must pass (the chain's verify runs them):

- `cd tools/orb-manager/web/runtime/_selftest && node headless-session.test.js` → last line `all green (…)`
- `node headless-gorbhost.test.js`, `node headless-send.test.js` and `PY=python3 node headless-test.js` → still green
- `cd apps/clock/editor/_selftest && node headless-test.js` → green
- `make check-web` and `make check-runner` at the repo root → green
- `python3 apps/sdk/orbsdk.py check-migrations apps/clock/ apps/flights/ apps/weather/ apps/radar/ apps/color_picker/` → green
- `git diff h1 -- tools/orb-manager/web/runtime/_selftest/headless-gorbhost.test.js | grep '^-[^-]'` → no output

## Out of scope

- No change to Go code, contract fixtures, `.orbapp` files, app versions, `send.js`, `host@1`,
  `studio@1` or any app's editor.
- No rebuilds of `.orbapp` or `.orbtheme` files: the change is in the runtime only, which a site
  serves from the release it loaded.
- Do not touch the GorbHost block, `docs/` (apart from nothing), or the H1 import code paths.

## Notes

- The site side (the 409 itself, and the session that moves to the newest version when its
  entry page is served) is orb-os-net M1. It is not in this repo.
- `entry.doc` is the document as the editor built it, with art inline. `saveDraft` refifies a
  copy, so the stash never holds refs that the new page could not inflate.
