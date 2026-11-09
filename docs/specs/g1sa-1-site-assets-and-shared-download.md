# theme@1 on a site: art by hash, Save by design id, the shared download; editors honour a shared name

## Goal

Follow-up to G1's hosted mode (`docs/specs/g1-1-gorbhost-seam.md`), for orb-os-net slice W4b.
The site (orb-os-net) now:

1. stores a design's **art by hash**: in the site's stored form each image is a ref string
   plus a sidecar, and the bytes are fetched on demand;
2. saves **by design id**: Save overwrites the opened design only when its name is unchanged,
   and a renamed Save or a Save As makes a new design;
3. **never stores a baked theme**. Other people get a theme by opening the app's editor on the
   site, signed out, at `?shared=<design id>&download=1`. The editor fetches a "design bundle",
   bakes it in the browser under the shared name `<name> by @<handle>`, and downloads it.

This step makes `theme@1` (the OS session runtime every editor shares) do all three. It only
does so when the page's `GorbHost` sets the new flags, so with `kind: "hub"` (Orb Manager)
nothing changes. It also makes the clock, flights and weather editors use the session's
**bake name** (the shared name, when one is set) for the name they bake.

## Read first

- `tools/orb-manager/web/runtime/theme@1/session.js`: all of it. Especially the GorbHost block
  (it must stay **byte-identical** with the one in `theme@1/send.js`), `hubJson`, `saveDraft`,
  `api.saveDraft`, `designOf`, `api.openEntry`, `api.apply`, `openFile`, `openTheme` (its
  `kind === 'web'` branch), `api.boot`, `download`, and the opening cover.
- `tools/orb-manager/web/runtime/theme@1/send.js`: its copy of the GorbHost block.
- `tools/orb-manager/web/runtime/_selftest/headless-host.test.js` and `headless-session.test.js`:
  the suites you extend (plain node, `ok(cond, name)`, a fake `fetch`, the made-up "tint" app),
  and `headless-test.js`, which runs them all.
- `apps/clock/editor/Watch Composer.dc.html`: `themeName()`, `_export3Body` (around the line
  `const nm = String(this.themeName() || 'watch')`), `bakeBytes`, and the session construction
  (`content`, `bake`, `apply`).
- `apps/flights/editor/flights-editor.js` and `apps/weather/editor/weather-editor.js`: `bakeNow`
  (it calls `B.bake(dd, ...)`, and the bake takes its name from `dd.name`) and `download`.
- `docs/specs/g1-1-gorbhost-seam.md`: the rules of the seam (editors read flags, never `kind`).

## Contract

### GorbHost flags (both copies of the block)

`hostInfo()` adds two booleans, each `raw.<flag> === true`, and both false otherwise:
- `assets`: the host stores art by hash and accepts refs.
- `designIds`: the host saves by design id.

The orb-os-net site sets `{kind: "web", api: "/studio-api", send: false, publish: false, inbox: false, assets: true, designIds: true}`.

### Art by hash (only when `hostInfo().assets`)

**The site's stored form.** Any JSON string (at any depth) that was a data URL is replaced by
the ref `orb-asset:<sha256 hex>`, and the document carries a top-level sidecar:

```json
"orbAssets": [ { "sha256": "<64 lower-case hex>", "prefix": "data:image/png;base64", "size": 12345 } ]
```

The original string is exactly `prefix + "," + base64(bytes)`, with standard padded base64.

**Opening.** Every open from the host adds `&refs=1` to its query: `designOf` (the
`/library/design` read) and the web branch of `openTheme` (`/theme/design?id=`). After
parsing, and before `api.apply`, the document goes through `inflate(doc, source)`:
- For each sidecar entry, get the bytes. With no `source`, that is a `fetch(hostUrl('/asset?sha256=' + h))`
  (a non-2xx answer is a refusal: `could not fetch an image of this design (<status>)`). With a
  bundle `source` (below), it is base64-decoded from `bundle.assets[h]` (missing: `the bundle is missing an image`).
- Check `sha256(bytes) === h` with `crypto.subtle.digest('SHA-256', ...)`. A mismatch is a
  refusal: `an image of this design came back changed`. This is the check that the editor got
  back what was sent.
- Replace every string equal to `orb-asset:<h>` with the rebuilt data URL, and remember
  `known.set(dataUrl, {sha256: h, prefix, size})`.
- Delete `orbAssets`. A document without a sidecar comes through unchanged.
- A refusal in inflate is an open failure, worded as each open path already words its
  failures (for example `Could not open “X”: <reason>`).

**Saving.** In the internal `saveDraft(name, doc, extra)`, build the body object as today, then
deep-copy it (a JSON round trip) and walk it:
- A string found in `known` becomes `orb-asset:<sha256>`, and its entry goes into the body's
  `orbAssets` (one per hash, sorted by `sha256`).
- A string that the site would store as an asset is left inline (it is uploaded). That means it
  starts with `data:`, its part before the first `,` ends in `;base64`, its payload decodes to at
  least 512 bytes, and re-encoding the decoded bytes gives exactly the payload. Hash it now,
  and after the save succeeds add it to `known`, so the next save sends a ref.
- Nothing else changes. With `assets` false the body is exactly today's, so the hub sees no change.

### Save by design id (only when `hostInfo().designIds`)

The session keeps `api.opened`, `{id, name}` or null: the site design the editor holds now.

- `openEntry(nm)`: `{id: entry.design, name: nm}` when the library entry has a `design`, else null.
- The web branch of `openTheme` (`?theme=<id>`): after the library is read, the entry whose
  `design === id` gives the name, so `{id, name: entry.name}`. With no such entry, null.
- `openFile`, a shared open (below), and a fresh editor all mean null.
- `saveDraft` adds `&id=<opened.id>` when `api.opened` is set, else `&saveAs=1`.
- A new `api.saveAs(entry, quiet)`: the same as `api.saveDraft` but always `&saveAs=1`.
- After a successful save whose answer carries `data.design`, `api.opened = {id: data.design, name: entry.name}`.
  The site answers a renamed Save as a Save As, and refuses a name the owner already has with a
  409. Its sentence comes through `hubJson` into the existing `Not saved: …` line. Don't add a
  dialog.

### The shared download (web branch of `openTheme`)

When the page's query has `shared=<id>` (`/^[A-Za-z0-9_-]{22}$/`), it takes precedence over `theme`:
1. While it runs, the opening cover says `Opening the shared design…`. `boot` doesn't read the
   library or move old browser designs when `shared` is in the query.
2. `fetch(hostUrl('/shared/bundle?id=' + id))`. A non-2xx answer gives
   `Could not open that design: <the JSON error, or "the server answered <status>">`.
3. Check `bundle.format === 'orb-design-bundle' && bundle.bundle === 1`. Otherwise:
   `Could not open that design: that is not a design bundle`.
4. `inflate(bundle.design, bundle)`, then `api.apply` (which records the shared name, below),
   then `api.restart()`. Set `api.opened = null`.
5. If the query also has `download=1`:
   - Say `Baking “<shared name>” for download…`.
   - `bytes = await host.bake()`. With no `host.bake`, say
     `Not downloaded: this editor cannot bake here`.
   - If `root.GorbBake && GorbBake.verifyBundle3`, read the baked name
     (`verifyBundle3(bytes).name`). If it isn't the shared name, wait 1 s and bake again, up to
     3 bakes in all, because a React editor applies a design on its next render. If it still
     doesn't match, say `Not downloaded: the bake came out named “<got>”`.
   - On success, `download(new Blob([bytes], {type: 'application/octet-stream'}), fileName)`,
     where `fileName` is the shared name with every character in `\/:*?"<>|` and every control
     character replaced by `_`, plus `.orbtheme`. Then say `Downloaded “<shared name>”`.
   - A null `bytes`: `Not downloaded: ` + the editor's own status line, or `the bake did not finish`.

### The bake name

- `api.apply(o, from)` sets `api.sharedName` to `o.shared.name` when that is a non-empty string
  of at most 55 UTF-8 bytes, and to null otherwise. It does this on **every** apply, so opening
  anything else clears it.
- `api.bakeName(own)` returns `api.sharedName || own`.
- Clock: in `_export3Body`, the name baked (`nm`) is
  `this.session.bakeName(String(this.themeName() || 'watch').replace(/\u0000/g, '').trim())`.
  Any other value in that function derived from the theme name (the serial, the status line)
  uses the same `nm`.
- Flights and weather: `bakeNow` bakes `Object.assign({}, dd, {name: ses.bakeName(dd.name)})`
  instead of `dd` (keep `dd` for everything that isn't the bake), and its status lines use that
  name.
- `content(withArt)` / `doc()` never write `shared` into a saved design.

## Behaviour and edge cases

Every case below is a check in the node suites:

- `hostInfo()` with `{kind:"web", assets:true, designIds:true}` gives both flags true. With
  `{kind:"hub"}`, nothing, or `assets: "yes"`, they are false. The two GorbHost blocks are
  still byte-identical (the existing check).
- Under `assets`, opening an entry whose stored form has 2 refs: the fetches are `/library/design?...&refs=1`
  and then two `/asset?sha256=` calls. `host.apply` receives the data URLs inline and no `orbAssets`.
  Saving it unchanged posts a body with two refs and their sidecar, and no `data:` string of
  512+ bytes. Changing one image posts one ref and one inline data URL. The save after that
  posts two refs.
- An asset answer with the wrong bytes: the open is refused with
  `an image of this design came back changed`, and `host.apply` is never called.
- Without the flags (hub, or no GorbHost): the request URLs and bodies are exactly today's.
  The existing suites pass unchanged.
- `designIds`: a fresh save sends `&saveAs=1`. After `openEntry('One')` (entry `design: "D1"`),
  a save sends `&id=D1`. After an answer with `data.design: "D2"`, the next save sends `&id=D2`.
  `api.saveAs` always sends `&saveAs=1`. A 409 answer gives `Not saved: <the site's sentence>`.
- Shared download with a fake bundle and a fake `host.bake` that returns bytes: one download
  named `<shared name>.orbtheme` (with `Skeleton by @greg` → `Skeleton by @greg.orbtheme`).
  `api.bakeName('Skeleton')` is `Skeleton by @greg` during the bake. A later apply of a
  document without `shared` makes it `Skeleton` again.
- A bundle whose `format` is wrong is refused, with no apply.
- The clock suite (`apps/clock/editor/_selftest/headless-test.js`) still passes, both plain and
  under `GORB_HOST='{"kind":"hub"}'`. Add a clock check that a design applied with
  `shared.name` bakes (through the suite's existing bake path) a bundle whose header name is
  that shared name, if the suite has a bake path that can reach `_export3Body`. Otherwise,
  check `session.bakeName` from the clock's session.

## Acceptance criteria

1. `cd tools/orb-manager/web/runtime/_selftest && PY=python3 node headless-test.js` exits 0
   with no FAIL lines.
2. `cd tools/orb-manager/web/runtime/_selftest && node headless-host.test.js && node headless-session.test.js` exits 0.
3. `cd apps/clock/editor/_selftest && node headless-test.js` exits 0.
4. `cd apps/clock/editor/_selftest && GORB_HOST='{"kind":"hub"}' NODE_OPTIONS="--require=$PWD/../../../../tools/orb-manager/web/runtime/_selftest/gorb-host-preload.js" node headless-test.js` exits 0.
5. `node -e "for (const f of ['apps/flights/editor/flights-editor.js','apps/weather/editor/weather-editor.js','tools/orb-manager/web/runtime/theme@1/session.js','tools/orb-manager/web/runtime/theme@1/send.js']) new Function(require('fs').readFileSync(f,'utf8'))"` exits 0 (they still parse).

## Out of scope

- The shelf `.orbapp` containers under `tools/orb-manager/apps/` and every generated file.
  Regenerating them needs clang and lld, which this toolchain doesn't have, so the orchestrator
  regenerates them afterwards. Don't run `tools/make_factory_apps.py`, and don't edit generated
  output by hand.
- Any Go code, the hub's handlers, and the contract fixtures (`editor-api.json`): the hub's
  behaviour doesn't change.
- `version.txt` bumps. A Save As button in the editors' UI (a renamed Save is the Save As
  path). The radar editor (it has no theme@1 session).

## Notes

- The ref form exists only on the site. Never send `&refs=1`, refs or a sidecar unless
  `hostInfo().assets` is true.
- Base64 in the browser: encode in chunks (`String.fromCharCode.apply` over slices of at most
  32 KB, then `btoa`) so large images don't overflow the stack. Node 20+ has `btoa`, `atob` and
  `crypto.subtle` as globals, so the suites need no polyfill.
