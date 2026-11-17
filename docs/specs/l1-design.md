# L1 — the asset library: `/me/assets`, "Your library" in every picker, online editors browse only

## The ruling

Ruling 42 (`docs/design.md` §0, 2026-11-11) — binding:

> Each picture and sound any design in your library references, yours or a remix's, is in your
> asset library, across every app, with its size, a thumbnail or a play button, and the designs
> that use it. Picking one adds a reference and stores nothing new.
> - **Online editors (the site's studio) browse the library and never take a file**: their
>   pickers offer only "Your library". Adding a new picture or sound is its own page on the
>   site, **Assets** (`/me/assets`), the one surface where every upload is validated, scaled
>   down and converted (ruling 35's art rules, ruling 41's sound rules), so that work lives in
>   one place.
> - The library lists only assets the account's own designs reference; nobody browses another
>   account's files.
> - Orb Manager's local editors keep their own file pickers (they work offline), plus "Your
>   library" when linked (H2); anything they send to the site (H4 Upload) passes the same
>   server-side checks as the Assets page.

design.md §12's gate: *an asset from a clock design is picked in a flights design without being
stored again; the studio's pickers offer no file input; an upload on `/me/assets` that is too big
is scaled (pictures) or refused (sounds) by the one shared check; another account's assets never
appear.*

Two repos, like L0. gorb-os (the editors, the runtime kit, Orb Manager's doors) runs first,
because the site's Assets page runs the runtime kit it builds.

## 1. What exists (read 2026-11-17, on `l0`)

Site:
- Art and sounds are stored once by sha256 (`file` rows, `design_version_asset` per version).
  `src/lib/design-assets.ts` `assetRefusal(prefix, bytes)` is the one content check (art header,
  SVG rules, sound header; never decodes), `assetKind(prefix)` says picture / sound.
- `src/lib/library.ts` `heldHashes(tx, userId, hashes)` (~565) is already "the account's library"
  as a predicate: hashes a version of one of the account's non-deleted designs names. A save may
  reuse a ref only for those. `readAsset(userId, app, hash)` (~1092) serves bytes by hash only
  for designs of THAT app the caller may see.
- `src/lib/storage.ts` `storageUsed` counts each hash once per account; `src/lib/file-gc.ts`
  `referencedBy` keeps a file while anything names it.
- The studio origin tells editors `GORB_HOST` (`src/lib/gorb-host.ts`); bearer routes for Orb
  Manager live under `/api/hub/*` (`src/lib/hub-caller.ts`, scopes `designs:read`/`designs:write`).

gorb-os:
- Every picture and sound picker of every editor is studio@1's `fieldKit` file row `Fi(label, on,
  valText, accept)` (`tools/orb-manager/web/runtime/studio@1/studio.js` ~887, rendered as
  `<input type=file>` in `FieldRow` ~939). Call sites: clock `clock-logic.js` 2369, 3878, 4114
  (pictures, through `pickArt` → `watch-engine.js` `assetFromFile`, PNG and SVG only) and 4398
  (sound, `sndUpload`); flights `flights-editor.js` 264, 440, 520 (`readPicture`, 932 px);
  weather `weather-editor.js` 319 (re-drawn as a 932 px JPEG); pomodoro through studio@1's
  `S.Sound` (`studio.js` ~1121, `o.upload`). radar and color_picker take no files. The
  StartFlow's design Import (`.orbtheme,.orbdesign`) is not an asset picker (owner question 6).
- `OrbStudio.fitPicture(file, opts)` (~734) is the one picture helper (scale a raster to fit
  1024, keep an SVG's text, return bytes untouched when inside the bounds).
- `GorbBake.soundFile(pcm)` / `soundRead(url)` (`baker@1/orbtheme-baker.js`, L0) are the one
  sound encoder and reader. The editors' own uploads decode with `decodeAudioData`.
- `theme@1` `OrbTheme.hostInfo()` reads `window.GorbHost` (block kept byte-identical in
  `session.js` and `send.js`); `hostUrl(p)` is `/studio-api`+p on the site, `/api`+p in Orb
  Manager. Orb Manager's editor port (`editororigin.go`) has a per-launch token shim and a
  route allowlist (~559–582); its site link (`sitelink.go`) calls the site with a bearer
  (`handleSiteDesigns` ~822 is the pass-through pattern).

## 2. The library

### 2.1 What is in it

An account's library is the set of hashes that either
- a version (any kept version) of one of the account's own designs with `deletedAt IS NULL`
  names in `design_version_asset` — exactly `heldHashes`' predicate; or
- the account uploaded on `/me/assets` (new table `account_asset`).

and whose `file.mediaType` makes `assetKind("data:" + mediaType + ";base64")` non-null. Nothing
else, ever: not another account's design, not a public design, not a shared copy, not a hash the
caller names. **One SQL predicate** (`libraryHashSql(userId)` in `src/lib/asset-library.ts`)
decides membership, and every door uses it: the list, the bytes doors, a save's reused refs
(`heldHashes` becomes "in the library"), and a remove.

### 2.2 Table `account_asset` (migration `20261122090000_l1_account_asset`)

```prisma
model AccountAsset {
  userId    String
  user      User       @relation(fields: [userId], references: [id], onDelete: Cascade)
  hash      String
  file      StoredFile @relation("AccountAssetFile", fields: [hash], references: [hash], onDelete: Restrict)
  name      String
  createdAt DateTime   @default(now())

  @@id([userId, hash])
  @@index([hash])
  @@map("account_asset")
}
```

- `storageUsed` adds the account's `account_asset` hashes to its hash set (still one SUM over a
  set: an uploaded file a design also uses counts once).
- `file-gc.ts` `referencedBy` gains `OR EXISTS (SELECT 1 FROM "account_asset" aa WHERE aa."hash" = …)`.
- An account's deletion cascades its rows; the purge/sweep then collects what nothing else names.

### 2.3 One asset, as every door answers it (`LibraryAsset`)

```ts
interface LibraryAsset {
  sha256: string;                 // 64 lower-case hex
  kind: "picture" | "sound";
  mediaType: string;              // the file row's, lower-cased: image/png … audio/wav
  prefix: string;                 // "data:" + mediaType + ";base64"
  size: number;                   // bytes
  name: string;                   // the upload's name; else "From " + the newest using design's name
  width: number | null;           // pictures, from readImageHeader (SVG: null)
  height: number | null;
  seconds: number | null;         // sounds, samples / 22050, one decimal (Math.round(x*10)/10)
  uploadedAt: string | null;      // ISO, when this account uploaded it; null otherwise
  usedAt: string;                 // ISO, newest of uploadedAt and its using versions' createdAt
  designs: Array<{ id: string; app: string; name: string; current: boolean }>;
                                  // this account's live designs any kept version of which names it,
                                  // newest first; current = the design's current version names it
}
```

Order: `usedAt` descending, then `sha256` ascending. No paging (the 250 MB storage limit bounds it).

### 2.4 The doors

| Door | Who | Answers |
|---|---|---|
| `GET /studio-api/assets` (every studio origin) | the studio session (`studioApi`); signed out → 401 as other studio routes | `hubOk({ assets: LibraryAsset[] })` |
| `GET /studio-api/asset?sha256=` (widened) | today's rule (a design of this app the caller may see) **or** the hash is in the signed-in caller's library | the bytes, today's opaque headers; otherwise today's 404 |
| `GET /api/hub/assets` | bearer, `designs:read`, the hub throttle | `{ assets: LibraryAsset[] }` (raw, like `/api/hub/designs`) |
| `GET /api/hub/asset?sha256=` | bearer, `designs:read`, the hub throttle | the bytes, the same opaque headers as the studio door; not in the library → 404 `{ error: "no such asset" }` |
| `GET /api/me/assets/file?sha256=` (main origin) | the site session | the bytes for the page's thumbnail/play: `content-type` the file's media type **only if** it is one of the six stored types, `x-content-type-options: nosniff`, `content-disposition: inline`, `content-security-policy: default-src 'none'; style-src 'unsafe-inline'; sandbox`, `cache-control: private, no-cache`, etag the hash; not in the library → 404 |
| `POST /api/me/assets` | the site session, same-origin JSON | §3 |
| `POST /api/me/assets/remove` | the site session, same-origin JSON `{ sha256 }` | §3.4 |
| `GET /me/assets/kit/studio.js`, `GET /me/assets/kit/baker.js` | anyone | §3.2 |

A hash that is malformed, unknown, another account's, or a public design's: the same 404 in every
door (the studio door keeps its existing public/unlisted rule for the editing app only).

## 3. Adding an asset: `/me/assets`

### 3.1 The one check

`src/lib/design-assets.ts` gains

```ts
export function uploadRefusal(prefix: string, bytes: Uint8Array): string | null
```

which is `assetRefusal(prefix, bytes)` with only its subject reworded for a page about one file:
a sentence starting `an image in that design ` starts `That picture ` instead, `an SVG in that
design ` → `That SVG `, `a sound in that design ` → `That sound `. Nothing else in the sentence
changes. So `That sound is 25.0 s long: a design's sounds can be at most 20.0 s` and `That picture
is 2048 × 1536 pixels: a design's pictures can be at most 1024 × 1024` are the shared check's
words. The upload route calls `uploadRefusal` and nothing else to judge content.

### 3.2 Scaling and converting happen in the browser, with gorb-os's own code

The server never decodes. The page loads the pinned editors' runtime files — exactly two, served
by the site from the newest loaded orb-web release's `runtime_file` rows (`loadedAt` desc, the
first release that has both): `/me/assets/kit/baker.js` = `baker@1/orbtheme-baker.js` and
`/me/assets/kit/studio.js` = `studio@1/studio.js`, `content-type: text/javascript; charset=utf-8`,
nosniff, `cache-control: no-cache`, etag the stored hash. No loaded release → 404, and the page
says so (§3.5). (Owner question 3.)

Before posting, the page transforms; it never refuses on content:
- **A picture** (`image/*`): `OrbStudio.fitPicture(file, { max: 1024 })`. A raster over 1024 ×
  1024 comes back scaled (aspect kept); one inside the bounds comes back byte for byte; an SVG
  comes back as its text. If `fitPicture` rejects (an SVG its rules refuse, a file the browser
  cannot draw), the page posts the file's own bytes under its own type, and the server's
  sentence is what the owner reads.
- **A sound** (`audio/*` or a `.wav`): `OrbStudio.fitSound(file)` (new, §4.3). A WAV
  `GorbBake.soundRead` accepts (22,050 Hz mono PCM16 or the orb's ADPCM) is kept byte for byte;
  anything the browser decodes is converted to the editors' file (decodeAudioData, downmixed to
  mono and resampled to 22,050 Hz by an OfflineAudioContext, then `GorbBake.soundFile`). If it
  rejects, the file's own bytes are posted under its own type and the server refuses them. A
  sound longer than 20.0 s is converted and posted, and the server refuses it: too-long and
  too-big sounds are refused by the one shared check, not by the page.
- **Anything else**: posted as it is; the server refuses it (`That picture is not a PNG, …`).

### 3.3 `POST /api/me/assets`

Body JSON `{ "name": string, "data": "data:<type>;base64,<payload>" }`, at most 14 MiB of body, and at most `MAX_FILE_BYTES` (10 MiB) decoded
(413 `{ error: "too_large", message: "That file is too large to add: a picture or sound can be at most 10 MB" }`).
Same-origin only, like the other `/api/me/*` POSTs. Signed out → 401 (the existing caller
error shape).

1. `name` → `assetName(raw, kind)`: the part before the last `.` (if that leaves something),
   NFC, every run of characters outside `[A-Za-z0-9 ._'()-]` → one space, runs of spaces
   collapsed, spaces and periods trimmed from both ends, cut to 60 characters (then trimmed
   again); empty → `Picture` or `Sound`.
2. `data` must be a data URL with `;base64` and a decodable payload; otherwise 400
   `{ error: "asset_refused", message: "That file can't be read: a picture must be a PNG, JPEG, GIF, WebP or SVG and a sound a WAV" }`
   — the answer names the types the site reads at all, the way every other refusal here names the
   bound it hit (the owner, 2026-11-17).
3. `uploadRefusal(prefix, bytes)` → 400 `{ error: "asset_refused", message: <sentence> }`.
4. In one transaction holding the account's handle lock shared and an advisory lock on
   `userId + "/assets"`, and the `file/<hash>` lock: store the file (`putFile`, the save path's
   own store; an existing hash is not written again), insert `account_asset` if absent, run
   `checkStorage` (its own sentence, 409 `{ error: "storage_full", message }`) — a refusal
   rolls the row back and the file is collected if nothing names it.
5. The `account_asset` row is upserted whether or not a design already names the hash (so the
   upload stays in the library after that design changes; an existing row keeps its first
   name). 201 `{ asset: LibraryAsset, added: true }` when the hash was not in the library
   before; 200 `{ asset, added: false }` when it was (uploaded before, or named by a design):
   nothing new is stored either way.

### 3.4 `POST /api/me/assets/remove`

`{ sha256 }`. Removes this account's `account_asset` row for it. If a live design of the account
still names the hash → 409 `{ error: "in_use", message: "That asset is used by a design: change those designs first" }`.
Not an upload of this account → 404 `{ error: "no such asset" }`. 200 `{ removed: true }`, then
`collectFiles([hash])`.

### 3.5 The page

`/me/assets` (server component; signed out → redirect to `/login`, as `/me`). Same layout,
type scale, zinc palette, borders and link style as `src/app/me/page.tsx`; a link `Your pictures and sounds` to it from
`/me`. No new colours, icons or layout ideas.

- Heading `Assets`; under it: `The pictures and sounds your designs use, and the ones you add
  here. Editors on this site pick from this library.`
- **Add** section, heading `Add a picture or a sound`: one file input
  (`accept="image/png,image/jpeg,image/gif,image/webp,image/svg+xml,audio/*,.wav"`), the
  storage line (`<used> of <limit> used`, `formatMB`). States:
  - kit loading: input disabled, `Getting the converter ready…`
  - kit missing (404): input disabled, `Adding needs the editors' converter, which this site has not loaded yet.`
  - working: input disabled, `Adding “<name>”…`
  - added: `Added “<name>”.` plus, when it applies, `Scaled down to <w> × <h> to fit.` or
    `Converted to 22,050 Hz mono for the orb.`
  - already there: `“<name>” is already in your library.`
  - refused: the server's `message`, as is (role="alert").
  - network/other failure: `Something went wrong. Try again.` (the site's `GENERIC_FAILURE`)
- **Pictures** and **Sounds** sections (headings with counts, `Pictures (2)`), each a list of
  rows like `/me`'s design rows: a 48 px thumbnail (`<img>` from `/api/me/assets/file`) or a
  `Play` / `Stop` text button (bytes fetched from the same door, read with
  `GorbBake.soundRead`, played at 22,050 Hz in an AudioContext); the name; the size
  (`formatSize`: `< 1 KB`, `84 KB`, `1.2 MB`); `512 × 512` or `2.4 s`; `Used by` and the design
  names as links to `/t/<id>` (`(older version)` after a name whose current version does not
  use it), or `Not used by any design yet` and a `Remove` button.
- **Empty** (no assets at all): `No pictures or sounds yet. Add one above, or save a design with pictures or sounds in an editor.`
- **Loading** (`loading.tsx`): `Loading your assets…`. **Error** (`error.tsx`, client):
  `Your assets could not be loaded.` and a `Try again` button.
- After an add or a remove the list refreshes (`router.refresh()`).

## 4. The editors and Orb Manager (gorb-os)

### 4.1 The host flags (theme@1, both GorbHost blocks byte-identical)

`hostInfo()` gains two flags:
- `files`: `typeof raw.files === 'boolean' ? raw.files : kind === 'hub'` — whether an editor
  may take a file from the computer. The site's studio leaves it unset (so false).
- `library`: `raw.library === true` — whether "Your library" is offered. The site sets it; Orb
  Manager sets it when the editor document is served while a live site link exists.

### 4.2 The library calls (theme@1 `session.js`)

```js
OrbTheme.library.list()        // → Promise<{ ok: true, assets: LibraryAsset[] } | { ok: false, message }>
                               //   GET hostUrl('/assets'); answer { ok, data: { assets } } | { ok: false, error }
OrbTheme.library.pick(asset)   // → Promise<{ ok: true, url, name, sha256, kind, mediaType } | { ok: false, message }>
                               //   GET hostUrl('/asset?sha256=' + asset.sha256); the bytes' sha256 (crypto.subtle)
                               //   must equal asset.sha256, else { ok: false, message: 'That asset did not arrive whole' };
                               //   url = asset.prefix + ',' + base64(bytes)
```

Failure messages: a non-2xx, an `ok: false` (its `error`), or no network → `{ ok: false, message }`
with the host's sentence or `Your library could not be reached`.

### 4.3 The studio kit (studio@1)

- `Fi(label, on, valText, accept, lib)`: `lib` is `function (pick)` or `{ pick: fn, play: fn }`.
  The row renders `<input type=file>` only when `hostInfo().files`; it renders a `Your library…`
  button (`ui-ghost st-sm`) when `hostInfo().library` and `lib` is given. Under the site with no
  `lib` the row shows its value and no control.
- `S.LibraryPicker` (opened by that button; one at a time): a modal in the existing modal markup
  and `st-*` classes (as `S.DupModal` / `S.SendModal`), titled `Your library`, listing only the
  assets of the row's kind whose `mediaType` the row's `accept` names (`audio/*` → sounds;
  otherwise the listed image types; `Fi`'s default accept list when none is given). Each row: a
  thumbnail (pictures, from `library.pick`'s url, fetched as the row shows) or `▶ Play`
  (sounds, through `lib.play(pcm, name)` when given, pcm from `GorbBake.soundRead`), the name,
  the size, `512 × 512` / `2.4 s`, and `In <design>, <design>` (at most three names, then
  `+N`). Clicking a row (or Enter on it) picks: `library.pick(asset)` then `lib.pick(result)`
  and the modal closes. States: `Opening your library…`; empty: `No pictures in your library yet.`
  / `No sounds in your library yet.` followed by `Add them on orb-os.net, under Assets.`;
  error: the message; `Cancel` closes; Escape closes.
- `S.Sound(Kit, o)` passes `o.library(key, pick)` and `o.play` as the `Your own` row's `lib`.
- `S.fitSound(file) → Promise<{ dataUrl, seconds, samples, converted }>` next to `fitPicture`
  (§3.2's sound rules; rejects with `Error('That file is not a sound this browser can read')`).
  The editors' own upload paths are not changed.

### 4.4 A pick stores nothing new

Each call site's `lib.pick` puts `pick.url` into the same place an upload would, **verbatim**: no
`fitPicture`, no re-draw, no re-encode, no `soundFile`. On the next save `theme@1` turns it into
the same `orb-asset:<sha256>` the clock design holds, and the site stores nothing (a reused ref
or the same bytes, by hash). The picker offers only types the slot can hold, so no pick needs
converting (clock pictures: `image/png,image/svg+xml`). A sound pick keeps the file as L0 keeps
a stored sound (`wav` = `pick.url`; samples from `soundRead` for playback and derived clips).

### 4.5 Orb Manager's doors

On the editor port's allowlist, for every editor token:
- `GET /api/assets` → the linked site's `GET /api/hub/assets` with the bearer, passed through like
  `handleSiteDesigns` (`jsonOKRaw`; no link → `jsonErr(siteMsgNotConnected)`; a 401 → the link is
  forgotten and `siteMsgDisconnected`; anything else → `siteMsgNoAnswer`).
- `GET /api/asset?sha256=<64 hex>` → the site's `GET /api/hub/asset?sha256=`, the bytes passed
  through (at most 10 MiB + 1 read; more → `siteMsgNoAnswer`) as `application/octet-stream`,
  nosniff; a bad hash → 400 without dialing; the site's 404 → 404 `{ ok: false, error: "no such asset" }`.
- The editor document gets `<script>window.GorbHost={"kind":"hub","library":true}</script>`
  right after the token shim when a live link exists as it is served; otherwise nothing changes.

## 5. Steps

gorb-os (`l1-hub-chain.json`, in gorb-os `docs/specs/`):
1. `l1-hub-1-library-kit` — theme@1 flags and `OrbTheme.library`; studio@1 `Fi`, `LibraryPicker`,
   `S.Sound`'s library row, `fitSound`; node and browser selftests.
2. `l1-hub-2-hub-doors` — Orb Manager's `/api/assets`, `/api/asset` and the GorbHost line; Go tests.
3. `l1-hub-3-editors` — clock, flights, weather and pomodoro pick verbatim; app versions, repack.
4. `l1-hub-4-live` — the browser gate (no file input under the site, a flights pick of a clock
   asset keeps its bytes, the picker screenshot) and the full gate.

orb-os-net (`l1-site-chain.json`):
1. `l1-site-1-library` — `account_asset`, `src/lib/asset-library.ts`, `uploadRefusal`, storage,
   sweep and reused refs; the DB-backed proof of the gate.
2. `l1-site-2-doors` — the routes of §2.4 and `GORB_HOST.library`.
3. `l1-site-3-assets-page` — `/me/assets`, its states, the screenshots, the full gate.

## 6. Owner questions (L1 picked an answer and carried on)

1. **Which editors may read the library.** Every studio origin and every Orb Manager editor
   token, as the ruling's "every picker across apps" says; an outside developer's reviewed
   editor could therefore list the owner's pictures and sounds of every app (the studio CSP
   still keeps them from leaving). Alternatives: shelf apps only (a shelf-only shortcut,
   against ruling 45), or a reviewed `library` capability in `editor.json`.
2. **Uploads nobody uses yet stay in the library** (`account_asset`), count against storage
   and can be removed while no design uses them. Alternative: sweep an unused upload after a
   grace period.
3. **The Assets page runs the editors' own code on the main origin**: the pinned orb-web's
   `studio@1/studio.js` and `baker@1/orbtheme-baker.js`, first-party and checksummed when
   loaded, so scaling and conversion stay gorb-os's one implementation. Alternatives: a copy
   in the site (a second encoder), or the upload page on a studio origin.
4. **A WAV the orb already plays is kept as it is** (PCM16 stays PCM16); everything else is
   converted to the editors' file (ADPCM unless under 20 dB). Alternative: always re-encode.
5. **The library is every kept version of live designs** (what storage counts and what a save
   may reuse); an asset only in older versions shows `(older version)`.
6. **The studio's design Import stays.** It opens a whole `.orbtheme`/`.orbdesign`, not an
   asset, and its art passes the save's checks. Alternative: hide it under `files: false` too.
7. **A pick is stored verbatim** even where the editor would draw a new file smaller (flights
   and weather keep 932 px): that is what "stores nothing new" needs. The slot lists only types
   it can hold.
8. **The page's refusal sentences** are the shared check's with the subject reworded (`That
   sound is 25.0 s long: …`), and each one names the bound it hit (the owner, 2026-11-17).
