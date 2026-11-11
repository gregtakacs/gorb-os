# H4 — Upload to orb-os.net from Orb Manager: the `designs:write` scope and the upload route

## The ruling

Ruling 43 (`docs/design.md` §0, 2026-11-11, amends §7.2's single scope) — binding, and the
contract below turns that text into routes, columns and sentences:

> - **A second scope, `designs:write`**, granted only with consent text that names it ("read
>   and add designs"). A connection made before H4 keeps `designs:read` only: the site refuses
>   its uploads, and Orb Manager says "Reconnect to allow uploads".
> - **Visibility:** the upload dialog offers Private (the default), Unlisted and Public.
>   Unlisted and Public need the account to have a handle (otherwise Orb Manager says so and
>   links to `/me`), and they go through the site's normal share path, with its rules and
>   limits; there is no second route around them.
> - **Never overwrite without confirmation.** When the account already has a design of that
>   app with that name (ignoring case), Orb Manager asks, like a local Save As onto an
>   existing file: **Replace** (an explicit confirm), **Upload as a new design** (the name
>   prefilled and editable) or **Cancel**. Replace adds a version to that design (W5), so the
>   old one can be restored on the site, and its share link stays the same.
> - **What is sent:** the design and its art, never a bake. It passes every check a save on
>   the site applies: ruling 35's art and SVG rules and its under-10-byte refusal, the name
>   rules (basic characters, unique per account and app ignoring case), the 250 MB storage
>   limit, the app must exist on the site, and the design's schema must be one the newest
>   live editor can open. Sounds stay inline as today (ruling 41 changes that in L0).
> - **The result** links to the design on the site.

design.md §12's gate: *an Orb Manager linked before H4 cannot upload until relinked with the new
consent; an upload lands in the account's library under the name rules and storage limits; a
read-only token is refused.*

## 1. Two scopes where H2 had one

### 1.1 The columns

`hub_link` and `connect_grant` each gain `scopes String[]` (Postgres `text[]`), non-null,
default `["designs:read"]`. One migration, `20261119090000_h4_hub_scopes`: the two
`ADD COLUMN`s with that default, nothing else. The default **is** the backfill — every row
that exists today was approved under H2's single-scope consent, and `designs:read` says
exactly that. `connect_grant.scopes` records what the owner approved; `hub_link.scopes`
records what the exchange minted. Both are written and read sorted, without duplicates.

No other table changes, and no `tests/fixtures` change: an upload writes ordinary
`design`/`designVersion` rows. An uploaded design is not a new kind of thing.

### 1.2 The wire

`/connect` and `POST /api/connect` grow one **optional** parameter, `scope`:

- Its value is one or more known scope words separated by single spaces, URL-encoded (the
  hub sends `designs:read designs:write`).
- It must contain `designs:read`. A `scope` naming only `designs:write` is refused: the read
  doors are what a linked hub is for, and a link that cannot list its own account is a
  support ticket, not a feature.
- Repeated `scope` parameters, an empty value, an unknown word, a duplicate word, or leading
  or trailing spaces → refused as an invalid connect request: the page's existing sentence,
  the route's existing 400. There is no partial connect (H2 §2.1).
- **Absent means `designs:read`, and the request is H2's byte-for-byte.** A hub from before
  H4 links exactly as it did, sees the old heading, gets a read-only link, and the upload
  door answers it 403. Nothing about it changes.

`POST /api/hub/token`'s success object gains `scopes`: the array as stored, sorted. An older
hub ignores an unknown key in that answer (`encoding/json`'s rule), so the field is additive
on that side too. Every refusal of the exchange stays the one `{"error":"invalid_grant"}`.

`GET /api/hub/designs` (H2's read door) is untouched: a `designs:write` link can still read,
because `designs:write` is granted **beside** `designs:read`, never instead of it.

### 1.3 The caller and the doors

`src/lib/hub-caller.ts` stops hard-coding one scope:

```ts
export type HubScope = "designs:read" | "designs:write";   // words declared in connect.ts,
export const HUB_SCOPES: readonly HubScope[] = ["designs:read", "designs:write"]; // re-exported
export interface HubCaller { userId: string; linkId: string; scopes: HubScope[] }
export function hasHubScope(c: HubCaller, s: HubScope): boolean;
export function requireHubScope(c: HubCaller, s: HubScope): void;
```

`src/lib/connect.ts` owns the two words, because it owns the `scope` parameter's grammar;
`hub-caller.ts` re-exports them so a route imports one module.

`requireHubScope` throws `CallerError(403, "insufficient_scope")` and is the only source of
403 in the hub seam — which means `CallerError`'s code union in `src/lib/caller.ts` gains that
one member (it is `"unauthenticated" | "forbidden"` today; `SessionError`'s parallel union and
every existing `forbidden` throw are untouched). `requireHubCaller` returns the scopes the row
carries, filtered through
`HUB_SCOPES`, so a token minted by a future site cannot widen a present door by accident; it
still raises only 401 `unauthenticated`. `hubCallerErrorResponse` already maps a
`CallerError` to `{error: <code>}` at its status, so a live token that lacks a scope answers
**403 `{"error":"insufficient_scope"}`** — never 401, because 401 is the word that makes Orb
Manager rewrite its link file as disconnected (§5's rule from H2), and a scope refusal must
leave a working credential exactly where it is. That helper sets no `cache-control`, as H2 left
it; the upload door answers its own 401 and 403 through its local envelope helper (§2) so every
byte it writes carries `no-store`, and the two existing hub doors keep their H2 bytes.

### 1.4 Consent and `/me`

`src/app/connect/page.tsx` chooses its heading from the parsed request — the one place the
words "read" and "add" appear:

- `designs:read` only: `Allow Orb Manager on <label> to read your designs?`, unchanged.
- both: `Allow Orb Manager on <label> to read and add designs?`, with the subline
  `Orb Manager can add designs to your account, add a new version to a design when you confirm a Replace, and share what it adds. It cannot delete anything.`
  The sentence names the three things the scope actually permits — a new design, a confirmed
  Replace's version, and sharing what was added — and closes on the one verb an owner cannot get
  back, because a subline that said only "add" would leave the Replace and the share as
  surprises found later in the library.

Deny/Allow, the throttle, the redirect rules and the frameability ruling are untouched.
`POST /api/connect` writes the parsed scopes onto the grant it creates.


`/me` → Connected Orb Managers gains the scope words in the row that already names the
machine, the version and the last-seen time: `Read designs` or `Read and add designs`. No
new control — a connection's scopes change only by reconnecting, which replaces the row
(H2 §2.3), so there is nothing to toggle.

## 2. The upload door: `POST /api/hub/designs`

One new method on H2's existing path, in the existing file — the collection a hub lists with
`GET` is the collection it adds to with `POST`. It lands under the `orb-hub` router with no
compose change, and `tests/hub-routes.test.ts`'s `HUB_ROUTES` inventory grows the method with
`access: { GET: "hub-token", POST: "hub-token" }` before the route is written.

**Request.** `Authorization: Bearer`, `Content-Type: application/json`, `Cache-Control:
no-store`; the body is the design document, exactly the bytes Orb Manager answers at
`/api/library/design` with for that entry — the wrapper, the content and the art inline as data
URLs, its draft when it has one and otherwise the design its theme carries (§6).
Never a bake (ruling 43). Query parameters:

| Parameter | Meaning |
|---|---|
| `app` | **required**, `APP_SLUG_RE` — the app the design is filed under |
| `name` | **required**, through `requireDesignName` — the name the design is filed under |
| `visibility` | `private` (the default when absent) \| `unlisted` \| `public` |
| `replaceId` | the design this upload replaces: adds a version to it (the confirm's **Replace**) |

App and name come **from the query, exactly as at the studio save door**, and the wrapper is
cross-checked against them: `app` must pass `APP_SLUG_RE` and `name` must pass `requireDesignName`
(both the save door's own sentences, §2's `invalid_request` row), and the route calls
`checkDesign(bytes, app)` with the query's app, so `checkDesign`'s cross-app sentence
(`that design is the X app’s, not Y’s`) is reachable from this door exactly as it is from the save
door. The document's own `saved.name` is **ignored**: `saveDraft` rebuilds `saved` from its
`name` argument (library.ts:620 stores `saved = {name, at}` over the parsed document), so a
document that names itself differently is filed under the query's name and nothing needs
rewriting on the way. That is why **Upload as a new design** is the same request with the edited
name in the query: the hub never edits a design document, and the file in its library stays the
bytes it wrote.

**The refusal order is fixed**, because it is the studio save door's order with the bearer's
two checks in front (`design/save/route.ts`, `designs.go`): throttle, credential, scope, query
grammar (`app`, `name`), size, parse, wrapper, schema gate, name rules, clash, share preflight,
write. The save door checks grammar before it reads the body; this door cannot, because a
stranger must not learn anything about the site from a parameter check — hence credential and
scope first. Every refusal at this door — and only at this door — has one envelope:

```json
{"error": "<code>", "message": "<the sentence Orb Manager shows>"}
```

with `cache-control: no-store` on all of them, and these codes:

| Status | `error` | when | `message` |
|---|---|---|---|
| 429 | `too_many_requests` | the per-IP hub throttle; `Retry-After` set | `too many requests, try again shortly` |
| 401 | `unauthenticated` | missing/malformed/unknown/90-day bearer | (no message: it names no fact worth learning) |
| 403 | `insufficient_scope` | live bearer without `designs:write` | `this Orb Manager may only read designs: reconnect to allow uploads` |
| 400 | `invalid_request` | `app` absent or outside `APP_SLUG_RE`; `name` absent, blank, over 64 characters, or holding a control character | the save door's own sentences: `expected ?app=<slug>`, `a design needs a name`, `a design's name is at most 64 characters`, `a design's name cannot hold control characters` |
| 413 | `too_large` | body over `MAX_FILE_BYTES` (10 MB) | `that design is over the 10 MB a saved design may be` |
| 400 | `not_json` | body is not a JSON object | `that is not a design document` |
| 400 | `invalid_design` | `checkDesign(bytes, <query app>)` refuses the wrapper, its app, its schema field or the art | `checkDesign`'s own sentence, verbatim (ruling 35's bounds, the under-10-byte refusal, the cross-app sentence and `that design has no schema this site can read` all live there) |
| 409 | `app_not_live` | no `app` row, or no live release with an editor | `there is no <title> app on this site to upload to`, or `save-check`'s existing `<title> has no editor on this site at the moment, so nothing can be saved` |
| 409 | `needs_remake` | `head.schema` below the newest live editor's `minSchema` floor | `<title> <newest.version> cannot open a schema <k> design: it needs a remake` |
| 409 | `schema_refused` | `head.schema` above the newest live editor's schema | save-check's existing `that design is schema <k>, but <title> <newest.version> saves schema <s>` |
| 409 | `name_taken` | the account holds that name for that app ignoring case, no `replaceId` | the save door's own 409 sentence, plus `existing: {"id","name","visibility"}` |
| 409 | `replace_mismatch` | a valid `replaceId` whose stored name is not the request's `name`, ignoring case | `that design is named “<stored>”, not “<name>”` (§4) |
| 409 | `no_handle` | `visibility=unlisted`/`public` and the account has no handle — §5's **preflight**, before any write | `Pick a handle on your account page before sharing.` |
| 409 | `name_too_long` | the preflight: sharing needs a handle and `<name> by @<handle>` does not fit | `nameTooLongMessage`, verbatim. A *private* upload whose own name overfits reaches the same code at 400, from `saveDraft` |
| 400 | `name_refused` | `siteDesignName` refuses the name | its own sentence, verbatim |
| 400 | `storage_full` | the account's 250 MB limit | `checkStorage`'s sentence, with the over-bytes figure |
| 400 | `art_refused` | the design's picture breaks ruling 35's bounds inside `saveDraft`, which refuses before it writes | the existing `ERR_ART_NOT_HELD` sentence, verbatim |
| 404 | `not_found` | a `replaceId` that is not a design id, is not the caller's, is another app's, is deleted, or is purged | `No such design.` |

A share refused **after** the design was stored is not in this table: it answers 201 with the
design and a `shareRefused` member (§5), because a refusal envelope must never describe a design
that is sitting in the owner's library.

Everything in the table except `replace_mismatch` names a refusal the site's save or share path
already makes, and the door
re-names rather than re-words it: the same checks, the same sentences, reached through the same
helpers (`requireDesignName`, `siteDesignName`, `designByFoldedName`, `nameFits`/`nameTooLongMessage`,
`checkStorage`, `setVisibility`'s `ShareError`), and the same **status** each helper already answers with at the
save door — 409 for the clash and for every preflight `ShareError` (through `SHARE_ERROR_STATUS`),
400 for the grammar checks, the name rules and the storage limit, which are 400 there and stay
400 here. Only the *code* is
new, and it is how `uploadDesign` knows which row to write: `ShareError` already carries one, so
`src/lib/library-error.ts`'s `LibraryError` gains an optional `code` (a fourth constructor
argument, `readonly code?: string`, defaulting to none), set at exactly the five sites an upload
can reach — `name_refused` (`siteDesignName`), `name_too_long` (`checkNameFits`), `name_taken`
(`refuseTakenName`), `storage_full` (`checkStorage`) and `art_refused` (the two
`ERR_ART_NOT_HELD` throws). No existing throw changes its message or its status, and the studio
save door's envelope (`{ok:false,error:<sentence>}`) is untouched, because `door()` reads only
`message` and `status`. The upload door adds no rule of its own beyond §4's Replace match; it is
the site's save path with a different credential and a different shape of answer.

The 429 is `checkHubThrottle` as `/api/hub/token` calls it (`{error:"too_many_requests"}`
plus `retry-after`): the design door's **GET stays unthrottled exactly as H2 left it** — a
bearer read costs one indexed query — while the POST is throttled because an upload is a write
amplifier, the same reason the token door is. One shared per-address window, as the module
documents: an Orb Manager that uploads 31 designs in a minute also finds `/api/hub/token` and
the bundle door briefly shut, which is what a per-IP abuse window means.

A `visibility` outside those three words is 400 `{"error":"invalid_visibility","message":
"visibility must be private, unlisted or public"}` — the parameter's grammar, not the share
path, so it is refused the same way in step 2 and step 3.

*Step-2 footnote:* until the site's step 3 lands, the door also refuses a **valid**
`unlisted`/`public` with 409 `{"error":"invalid_visibility","message":"this Orb Manager does
not upload shared designs yet"}`, because a half-built share must never store a private design
and call it shared. Step 3 deletes that guard and its tests; nothing in the final table depends
on it.

**Success** is 201 with:

```json
{"id": "<design id>", "app": "clock", "name": "Skeleton", "version": 3,
 "visibility": "private", "url": "https://orb-os.net/t/<design id>"}
```

`url` is built from the site's configured origin (`authEnv(process.env).origin`, the same
source `POST /api/hub/token` answers its `site` from) — never from a request header, so a
forged `Host` cannot point an owner's link somewhere else. `version` is the version number
this upload wrote: 1 for a new design, `n+1` for a Replace.

When the upload asked to share and §5's preflight passed but `setVisibility` then refused the
stored design, the answer is this same 201 with `"visibility":"private"` (what the row actually
is) and one extra member:

```json
{"id": "…", "app": "clock", "name": "Skeleton", "version": 3, "visibility": "private",
 "url": "https://orb-os.net/t/…",
 "shareRefused": {"error": "name_too_long", "message": "<the site's sentence, verbatim>"}}
```

The design was stored, so the answer says so; `shareRefused` names why it is not shared. §5
lists the five refusals that can still happen post-save. There is no `shareRefused` member at
all when the upload asked for `private`, when the share succeeded, or on a Replace.

## 3. The write path: reuse the save, not the session

### 3.1 `src/lib/upload-check.ts` (new, pure)

```ts
export interface UploadRelease {
  version: string; status: string; hasEditor: boolean;
  schema: number | null; minSchema: number | null;
}
export interface UploadGate {
  title: string;                                       // App.title, or the slug with no row
  appExists: boolean;
  newest: { version: string; schema: number | null; minSchema: number } | null;
}
export type UploadRefusal =
  | { status: 409; error: "app_not_live"; message: string }
  | { status: 409; error: "needs_remake"; message: string }
  | { status: 409; error: "schema_refused"; message: string };

export function uploadGate(app: { title: string } | null, releases: UploadRelease[]): UploadGate;
export function uploadSchemaRefusal(g: UploadGate, head: { schema: number }): UploadRefusal | null;
```

`uploadGate`'s `newest` is §6.1's winner and nothing else: it hands `newestLive` (from
`editor-version.ts`, the same function `save-check.ts` calls, over `{version, status,
hasEditor}`) the releases it was given, then reads that version's row back for `schema` and
`minSchema ?? 1`. Both functions are pure over rows the caller read — `save-check.ts`'s shape,
so the whole matrix is test bytes and no database — and `uploadSchemaRefusal` returns **data**,
not a `Response`: §3.2's rule that the route owns the envelope applies here too. It answers
three of §2's rows — no app row or no live editor, `head.schema` below the floor, `head.schema`
above the newest schema — and nothing else.

**The "editor was updated" refusal is deliberately not here.** `editorRefusal` is a rule
about the *session* that posted: a stale tab must reload before it writes. An upload posts
bytes, not a session, so there is no tab to hand off; what an upload must satisfy is "the
newest live editor can open this", which is the schema gate. A hub whose local editor is
older than the site's newest live release therefore uploads fine, and the version lands
stamped with the `appVersion` its document carries, exactly as a keep from a file does.

### 3.2 `uploadDesign` in `src/lib/library.ts`

```ts
export interface UploadRequest {
  userId: string; app: string; name: string; bytes: Uint8Array;
  visibility: Visibility; replaceId?: string;
}
export interface UploadFailure {
  status: 400 | 404 | 409;
  error: "name_taken" | "replace_mismatch" | "no_handle" | "name_too_long" | "name_refused"
       | "storage_full" | "art_refused" | "not_found";
  message: string;
  existing?: { id: string; name: string; visibility: Visibility };
}
export interface ShareRefused {
  error: "not_found" | "no_handle" | "name_too_long" | "name_refused" | "art_refused";
  message: string;
}
export type UploadResult =
  | { ok: true; id: string; app: string; name: string; version: number; visibility: Visibility;
      shareRefused?: ShareRefused }
  | { ok: false; failure: UploadFailure };
export async function uploadDesign(req: UploadRequest): Promise<UploadResult>;
```

`ok: true` means the design is in the library, and `visibility` is the row's real value, so a
share that `setVisibility` refused after the save lands as `{ok: true, visibility: "private",
shareRefused: {error, message}}` and never as a failure (§5); the route's 201 serialises
`shareRefused` as the member of the same name, and omits it when it is absent.

It is the composition the route needs and nothing else; the route owns the envelope, and this
returns **data** — §3.1's rule for the gate holds for the write path too. In
order (§3.2's own order, matching §5's promise that a refused share writes nothing): the share
preflight when visibility is not `private` — `user.handle` present, then `nameFits` — the
`replaceId` resolution of §4 including its name match, and the advisory clash read, then
`saveDraft(userId, app, name, bytes, target)` — **the same function the studio door calls**,
which is where the name rules, ruling 35's art split, the under-10-byte refusal, the
name-fits rule, the storage check, the ten-version prune and the file writes already live —
then `setVisibility(userId, id, visibility)` when visibility is not `private` (ruling 43's "no
second route around the share path"), whose refusal after the save is §5's `shareRefused` answer
and not a failure. A refusal that does fail the call is named, not re-worded: a `ShareError` already
carries its code and maps through `SHARE_ERROR_STATUS`; a `LibraryError` carries a sentence and a
status and gains the optional `code` of §2 at the five sites an upload can reach, so
`uploadDesign` never has to recognise a sentence to know which row to write, and the sentence
always comes through verbatim. A clash is the one refusal `uploadDesign` reads for itself, before
`saveDraft`: §4's advisory fold read, whose row is what `existing` carries. `saveDraft`'s own
in-lock `refuseTakenName` stays the authority and still fires when the two disagree (a name taken
between the read and the lock), and that answer is the same 409 `name_taken` with the same
sentence and **no** `existing` — the hub then shows the sentence and offers no Replace. The
upload never calls
`ensureSharedCopy` itself and never sets `remixOf`: an upload is a private-by-default save
that may also be shared in the same act.

## 4. The name clash, and Replace

The site's rule for a save under a name the account already has for that app is a 409
(ruling 35 as amended, `refuseTakenName`), and the upload keeps it: *never overwrite without
confirmation* means **the site never decides**. The door's 409 therefore carries the existing
design's identity so the hub can ask a human:

```json
{"error":"name_taken","message":"you already have a design named “Skeleton” (names are
 compared without regard to case, because an orb's card ignores it)",
 "existing":{"id":"…","name":"Skeleton","visibility":"private"}}
```

`message` is `refuseTakenName`'s existing sentence, verbatim, and `existing` is the row's id,
its **stored** name and its visibility — the three facts the confirm needs. The read that fills
them is `uploadDesign`'s own, outside the `(owner, app)` lock and therefore advisory: it exists
to answer the ordinary clash with an id, and `refuseTakenName` inside the lock stays the rule,
answering the race with the same 409 and no `existing`. The fold is
`asciiFold`, which `library.ts` exports for this step so the clash read and the save's
refusal can never disagree about what "the same name" means.

**Replace** is the same upload sent again with the same `?app=`, the existing design's **stored**
name as `?name=`, and `?replaceId=<that id>`. The route resolves the id before `saveDraft`: not
the caller's own, not of the query's app, deleted, purged, or not a 22-character id → 404
`not_found`, the one shape H2's theme door uses so a probe cannot tell the four apart. A valid
`replaceId` whose stored name does not equal the request's `name` once both are folded
(`asciiFold`) is 409 `replace_mismatch`, message `that design is named “<stored>”, not “<name>”`,
and nothing is written. A Replace is what the human confirmed against the design they could see
named on the card; a request whose name points somewhere else is not that confirmation, whatever
the id says. The match is this door's own read of the row, not `saveDraft`'s refusal: `saveDraft`
would answer a renamed save with `refuseTakenName`'s clash sentence, which would tell the caller
about a design they just named instead of the one they asked to replace.

A case-only difference is not a mismatch, and it does not change the site's spelling either:
`saveDraft` adds a version to the opened design only when the name matches it *exactly*, so the
route passes the **stored** name and the 201 answers with it. A matched Replace goes to
`saveDraft` as `{id: replaceId}` — W4b-3's Replace: same id, same link, `n+1`, the ten-version
prune applied. An upload never renames (ruling 29:
nothing that saves renames) — which is why the hub sends a Replace with the clash answer's
`existing.name`, the site's own spelling, rather than the name its library file happens to hold.

A Replace leaves `visibility` alone whatever the query says. The dialog's visibility control
is not in front of the human who presses Replace on a design they can see is public, and
silently unsharing it is the surprise the confirmation exists to prevent. Visibility is the
site's share path's to change.

## 5. Visibility: the share path, handle checked first

`visibility` absent or `private` writes the design private and never reads a handle.
`unlisted` and `public` are checked **before** anything is written — `user.handle` present,
and `nameFits(name, handle)` — so a refused share leaves no design behind, exactly as
`setVisibility` refuses on the site's own pages. The save then runs, `setVisibility` runs
after it commits, and the credited copy, the `publishedAt` stamp and the `name_too_long`
refusal are the same code the `/publish` page and `PATCH /api/designs/[id]/visibility` use.
One design uploaded `public` is a public design by every rule in §8.4, because there is one
path that shares anything.

**A share refused after the save is not an error.** The preflight above cannot be exact — the
handle is read outside the design's lock — so `setVisibility` still answers after the bytes are
in the library, and everything it can still refuse is a fact about the share, not about the
design: `not_found` (the row was deleted, or its versions purged, between the save and this
read), `no_handle` (another browser took the handle), `name_too_long` (another browser changed
the handle, and the stored name no longer fits it), `name_refused` (`ensureSharedCopy` re-runs
`siteDesignName` on the stored name) and `art_refused` (the same call's `AssetError` re-check of
the bundle's pictures, sharing.ts:144/158). Each of those answers **201** with the design,
`"visibility":"private"` and the §2 `shareRefused` member naming the code and the site's own
sentence; the upload stays saved and private, and nothing about it is undone. A door that
answered 409 there would describe a design that exists as though it did not, and a hub that
believed it would offer to send the same bytes twice.

## 6. Orb Manager's side

Three places carry it: the credential, the doors, the dialog.

**`site-link.json`** gains `"scopes": ["designs:read","designs:write"]`. A file written
before H4 has no key and reads as `["designs:read"]` — the same default the site's migration
gives its rows, so both sides of one link age the same way. `siteConnectURL` grows the
`scope` parameter, after `version` and before `redirect`, with the value
`designs:read designs:write`: the hub always asks for both and the owner's consent decides.
`siteTokenExchange` reads `scopes`, filters it through the known set, and stores what the
site **answered**, not what the hub asked for — a site that answers one scope mints a
read-only link, and the hub must show that rather than assume its request was granted.
`GET /api/site/link-status` gains `scopes` in its `linked` answer. The token stays the only
secret in the file, and `TestStateFileHoldsNoSecrets` still passes untouched.

**`POST /api/site/upload`** (`requireControl`, registered only when `-site` is on, and absent
from the editor-API listener exactly like the other site doors): body
`{"app","name","visibility"?,"replaceId"?}` naming an **entry** in this hub's library. It reads
that entry's design the way `handleLibDesign`'s default branch does — `os.ReadFile` of
`designFile(name)` under `designs/<app>/` when the entry has a draft, otherwise the design its
theme carries, `orbnet.ShelfTheme(bytes)` and then `t.Design` — never `&from=theme` and never the
bake's own bytes, because a bake cannot be opened again and an upload must be (ruling 43). An
entry that has neither, or a theme whose bake carries no design, is refused locally with
`that theme carries no design, so it can't be uploaded`, the neighbour of the existing
`this theme carries no design, so it cannot be reopened` (`orbnet.ErrNoDesign` with its prefix
trimmed, which is what `handleLibDesign` prints); this door's wording is its own because what it
refuses is the upload. A design that has a draft uploads **its draft**, unsent changes and all,
exactly as the editor's Save would: what a human means by "upload this" is what they can see.
The bytes are refused over 10 MB up front with the site's own sentence — no point dialing a
megabyte to be told — and POSTed to
`<l.Site>/api/hub/designs?app=<app>&name=<name>` (plus `&visibility=` and `&replaceId=` when the
body named them, all through `url.QueryEscape`) with the bearer, `Authorization`, `Accept`,
`User-Agent` and `Cache-Control: no-store` as every other bearer call sets them (the base is the
link file's, as `fetchSiteDesigns` already dials it, and `routesSiteLink` has already checked it
equals `siteReleaseBase()`). The name in that query is the body's `name`, forwarded verbatim; on a
Replace the dialog therefore sends the clash answer's `existing.name` rather than the local
spelling, because §4's `replace_mismatch` compares the query's name with the stored one and a
case-only difference in what the hub itself calls the design would refuse the human's own
confirmation (§6's dialog). The answer:

- **201** → `{"ok":true,"data":<the site's object>}`, its `url` unmodified. A `shareRefused`
  member inside that object is part of the site's answer and passes through untouched — the hub
  does not turn it into a refusal, because the site did not refuse the upload (§5).
- **403 `insufficient_scope`** → `{"ok":false,"error":"Reconnect to allow uploads"}`, the
  credential kept: the refusal is about the link's scopes, not its life.
- **409 `name_taken`** → `{"ok":false,"error":"name_taken","message":<the site's message>,
  "nameTaken":<the site's existing
  object>}`, so the dialog can offer Replace and Upload as a new design. The clash is matched on
  the site's `error` **word** (only the two credential answers are matched on status, because
  status is all they say), and a clash whose body carries no `existing` — the race §3.2 names —
  is the same answer without `nameTaken`, which the dialog draws as the sentence alone.
- **401** → `forgetSiteLink` and `disconnected from orb-os.net`, H2's rule unchanged.
- **anything else, including a stall** → the site's `message` when it sent one, else
  `orb-os.net did not answer`; the credential kept. This is how `replace_mismatch`,
  `invalid_request`, `invalid_design`, `not_json`, `too_large`, the schema gate's three 409s,
  `storage_full`, `name_refused`, the save path's `art_refused` and `not_found` reach a human:
  one pass-through, in the site's words, with no hub-side table of codes to fall out of date.

It uses a new `siteUploadTimeout` of 120 s, a var for the reason `siteCallTimeout` is one:
10 MB over a home line is not a 10-second request. **One attempt, no retry** — a retry would
turn one confirmed Replace into two versions.

**`GET /api/site/local-designs`** is the dialog's picker, and it lists what can be uploaded:
every library entry that has a design, `{"designs":[{"app","name","bytes","at","appVersion"}]}`,
newest first, at most 500 rows. Drafts only would hide most of a real library — a design baked
and sent to an orb keeps its design **inside** the theme and its draft folded away (`keepTheme`),
so an owner whose library is full of baked themes would be told they hold nothing. The read is
`s.library().entries(app)` per app, keeping an entry when it has a `Draft` or a `Theme` whose
`can_reopen` is true (the same flag `/api/library/entries` answers and the same flag
`handleLibDesign`'s theme branch depends on: a bake with no design inside has nothing to upload);
`bytes` and `appVersion` come from whichever half would actually upload, so a theme-only row's
size is the carried design's and not the bake's — which is what the door's 10 MB pre-check then
weighs, honestly. The apps walked are those with a `designs/<app>/` folder **or** a shelf theme,
`shelfThemes` over the themes directory with `shelfApp` naming each one's app, because a theme
directory with no designs folder is a library too. No existing door does this:
`/api/library/entries` is per app, and `designs.go` has no listing of its own.

**The dialog** hangs on `#mn-lib-upload`, the button H2's UI already reserved on the Theme
library toolbar, disabled with a "coming in a later release" title. It is enabled only when
`link-status` says `linked` **and** the scopes include `designs:write`; a read-only link draws
the ruling's sentence `Reconnect to allow uploads`, verbatim, in the toolbar's state span
`#mn-lib-state` beside a `Reconnect` button (`#mn-lib-reconnect`) whose press calls the **same**
handler `#mn-site-connect` already uses — one reconnect flow, not a copy of it — and that
sentence is the only place upload is mentioned to a hub that cannot upload. It picks an entry
(app and name from `/api/site/local-designs`), offers Private (default), Unlisted and Public,
and posts `/api/site/upload`. Unlisted or Public on an account with no handle — which the page
knows from `link-status` without dialing — is answered locally with `Unlisted and public designs
need a handle: set one on <site>`, where `<site>` is an anchor to
`<site>/me`. A `name_taken` answer opens the confirm: `orb-os.net already has
a design called “<name>”.` with **Replace**, **Upload as a new design** (the name prefilled
and editable) and **Cancel**. Pressing **Replace** does not send anything: it first says what
pressing it twice more will do — `Replace adds a new version of “<name>” on orb-os.net. It stays
<visibility> and keeps its link, and the current version can be restored on the site.` with
**Replace** and **Cancel**, the two names being the clash answer's `existing.name` and
`existing.visibility`, so the human confirms the consequence rather than the collision, and the
sentence answers the two questions a Replace really raises (what happens to the version count,
and what happens to what is there now). Success draws `Uploaded to orb-os.net as <visibility>`,
the word the site answered — `private`, `unlisted` or `public` — beside an anchor
`Open <name> on orb-os.net` whose `href` is the site's `url` set as a property. A `shareRefused`
in that answer draws the same line with the site's sentence on the end, `Uploaded to orb-os.net
as private: <message>`, and the same anchor: the design is on the site and only its share failed,
so the line says both. Foreign
bytes go into `textContent` and `href`, never into markup, as every other site string in that
page does.

## 7. Copy strings (the whole set; nothing else's text moves)

| Where | Verbatim |
|---|---|
| `/connect` heading, read only (unchanged) | `Allow Orb Manager on <label> to read your designs?` |
| `/connect` heading, read and write | `Allow Orb Manager on <label> to read and add designs?` |
| `/connect` subline, both scopes | `Orb Manager can add designs to your account, add a new version to a design when you confirm a Replace, and share what it adds. It cannot delete anything.` |
| `/me` connection row, scopes | `Read designs` / `Read and add designs` |
| Upload 403, site | `{"error":"insufficient_scope","message":"this Orb Manager may only read designs: reconnect to allow uploads"}` |
| Upload 400, bad `visibility` word | `visibility must be private, unlisted or public` |
| Upload 409, step 2 only | `this Orb Manager does not upload shared designs yet` |
| Upload 413, site | `that design is over the 10 MB a saved design may be` |
| Upload 400, site, unparsable body | `that is not a design document` |
| Upload 409, app absent from the site | `there is no <app> app on this site to upload to` |
| Upload 409, no live editor | `<title> has no editor on this site at the moment, so nothing can be saved` (existing sentence) |
| Upload 409, below the floor | `<title> <newest.version> cannot open a schema <k> design: it needs a remake` |
| Upload 409, above the newest schema | `that design is schema <k>, but <title> <newest.version> saves schema <s>` (existing sentence) |
| Upload 400, bad or missing `app`/`name` | the save door's four: `expected ?app=<slug>`, `a design needs a name`, `a design's name is at most 64 characters`, `a design's name cannot hold control characters` |
| Upload 409, Replace's name does not match | `that design is named “<stored>”, not “<name>”` (new to this door, §4) |
| Upload 201, site | `{"id","app","name","version","visibility","url"}`, plus `shareRefused: {"error","message"}` only when §5's post-share refusal happened |
| Upload, every other refusal | no new text: `name_taken`, `no_handle`, `name_too_long`, `name_refused`, `storage_full`, `art_refused` and `not_found` re-use the sentence the save or share path already writes (§2 names the helper each comes from) |
| Hub, upload button | `Upload to orb-os.net` (existing id `#mn-lib-upload`) |
| Hub, read-only link | `Reconnect to allow uploads` |
| Hub, entry with no design | `that theme carries no design, so it can't be uploaded` |
| Hub, no handle | `Unlisted and public designs need a handle: set one on <site>` — `<site>` links to `<site>/me` |
| Hub, clash confirm | `orb-os.net already has a design called “<name>”.` with buttons `Replace`, `Upload as a new design`, `Cancel` |
| Hub, Replace confirm | `Replace adds a new version of “<name>” on orb-os.net. It stays <visibility> and keeps its link, and the current version can be restored on the site.` with buttons `Replace`, `Cancel` |
| Hub, over 10 MB before dialing | `that design is over the 10 MB a saved design may be` |
| Hub, success | `Uploaded to orb-os.net as private` / `… as unlisted` / `… as public`, link `Open <name> on orb-os.net` |
| Hub, stored but unshared | `Uploaded to orb-os.net as private: <message>`, same link |

## 8. Steps

Site chain (`h4-site-chain.json`, base `h4`, service `postgres`):

1. **h4-site-1-scopes-and-consent** — the migration and both columns, `parseConnectParams`
   and `ConnectRequest` growing `scopes`, the connect page's two headings and its subline,
   `/api/connect` storing them, `/api/hub/token` minting and answering them,
   `hub-caller.ts`'s `HubScope`/`requireHubScope`/403, and `/me`'s scope words. Nothing can
   upload yet; everything about a scope is decided here.
2. **h4-site-2-upload-private** — `src/lib/upload-check.ts`, `uploadDesign` in
   `src/lib/library.ts`, the `POST` half of `src/app/api/hub/designs/route.ts` and the
   `HUB_ROUTES` declaration, with `private` uploads only: throttle, 401, 403, the query's
   `app`/`name` grammar, cap, wrapper, app/schema gates, the by-name 409 with `existing`,
   `replace_mismatch`, and Replace by `replaceId`.
 3. **h4-site-3-upload-visibility** — `visibility=` through the preflight and
     `setVisibility`, `no_handle` and `name_too_long` before anything is stored, the 201 +
     `shareRefused` answer for a share refused after the save, Replace never
     touching visibility, and the shared design's `url` and gallery presence proven through the
     same 201 step 2 already answers. 

Hub chain (`h4-hub-chain.json`, base `h4`): **h4-hub-1-scopes-on-the-link** (the credential,
the connect URL's parameter, the exchange's answer, `link-status`, the read-only default for
an old file), **h4-hub-2-upload-doors** (`POST /api/site/upload` reading an entry's design the
way `handleLibDesign` does, the 403/name_taken/401
matrix, `GET /api/site/local-designs` over design-bearing entries, the door-shut and
editor-listener tests), and
**h4-hub-3-upload-ui** (the dialog, the confirm pair including what Replace says before it
runs, the read-only and no-handle lines, the success line's visibility, and the
link back).

**The chains run in parallel.** No hub step needs a built site: every hub test drives a fake
site from §2's contract, written in `sitelink_test.go`'s existing fake-site rig, and every
site test posts a document built by the tests' own wrapper helper. The hub's step 1 also
proves the H2 file shape still loads with no `scopes` key. The integration order after both
merge is: relink, upload private, clash and Replace, share on upload — design.md §12's gate,
its three clauses in that order.

## 9. Worked example

GREG-PC linked to `@greg` before H4, so its link is `["designs:read"]`. Its library holds a
clock design `Big Ben`, 4 MB of PNG art inline.

- Upload `Big Ben`: the request carries only the bearer — scopes are a fact of the connect,
  not of the call — so the site answers 403 `insufficient_scope`, `this Orb Manager may only
  read designs: reconnect to allow uploads`. Orb Manager's state line
  becomes `Reconnect to allow uploads`; the file on disk still holds the token, and `/me`
  still lists the connection as `Read designs` until the owner relinks.
- The owner reconnects, approves `read and add designs`. `/me` says `Read and add designs`,
  the dialog's button enables.
- Upload `Big Ben`, Private → `POST …/api/hub/designs?app=clock&name=Big%20Ben&visibility=private`
  with the draft's bytes, and the site answers 201 `{"id":"…","name":"Big Ben","version":1,
  "visibility":"private","url":"https://orb-os.net/t/…"}`; it appears on `/me`, not in the
  gallery; the art is stored by hash and counted once against 250 MB.
- The library's `Weather` entry is a baked theme with no draft — its design is inside the
  bake. It is listed, because it carries a design the editor can reopen; uploading it sends the
  `orbnet.ShelfTheme` design, never the bake, and the row's `bytes` is that design's size.
- Upload the same entry renamed to `big ben` → 409 `name_taken` with
  `existing.name: "Big Ben"` — case-folded, so the site's spelling is the one offered back.
  **Replace** first says `Replace adds a new version of “Big Ben” on orb-os.net. It stays
  private and keeps its link, and the current version can be restored on the site.`; pressing
  Replace re-posts `?app=clock&name=Big%20Ben&replaceId=…` — the site's own spelling, not the
  hub file's — and the design keeps its id, gains version 2, and its `/t/` link never changed.
- Pressing Replace on a dialog that has meanwhile been used to change the name field, or a
  stale `existing` from an earlier clash, is 409 `replace_mismatch`: `that design is named
  “Big Ben”, not “big ben 2”`, and nothing was written.
- **Upload as a new design** edits the name to `Big Ben II` and re-posts the same bytes with
  `?name=Big%20Ben%20II`: the hub edited a query parameter, never the document.
- Upload `Big Ben 2` as Public with no handle → the hub answers locally, before dialing, with
  `Unlisted and public designs need a handle: set one on orb-os.net`, because `link-status` had
  already said the handle was empty; a hub that dialed anyway would get 409 `no_handle` and
  would still have stored nothing.
- Owner sets a handle, uploads `Skyline` as Public, and another browser drops the handle in the
  seconds between the preflight and the share: 201 with `"visibility":"private"` and
  `"shareRefused":{"error":"no_handle",…}` — the hub draws `Uploaded to orb-os.net as private:
  Pick a handle on your account page before sharing.` and the link to the design, which is
  genuinely there.

## 10. Owner questions

- **`scope` must contain `designs:read`** — a write-only link is refused rather than
  supported. *Picked:* every hub flow that shows an upload result reads first, and a scope
  that can add but never list is a credential an owner would have to debug.
- **Scope refusal is 403, and only 401 clears the hub's credential** — ruling 43's sentence
  "Reconnect to allow uploads" only makes sense if the hub still has a connection to
  reconnect. *Picked.*
- **App and name ride the query, as at the studio save door, and the document's `saved.name`
  is ignored** — `saveDraft` rebuilds `saved` from its `name` argument (library.ts:620), so the
  site's answer can only ever name what the request named, and **Upload as a new design** is one
  edited query parameter. Orb Manager sends design bytes it wrote and never rewrites them: an
  upload that edited the document would leave the hub's library file and the site's copy
  disagreeing about a name, which is the one thing the two must share. *Picked.*
- **A Replace must name the design it replaces** — the id alone is not the confirmation, since
  the human confirmed a *named* design: a mismatch is 409 `replace_mismatch` and nothing is
  written, rather than a version landing on an id the dialog no longer showed. *Picked.*
- **A share refused after the save answers 201, not an error** — the design is in the library,
  and §5's five post-save refusals are facts about the share. An error envelope would invite the
  hub to resend bytes that are already stored. *Picked.*
- **Any library entry with a design can upload, not only a draft** — sending a theme to an orb
  folds the draft away, so drafts-only would hide most of a baked library; the hub reads the
  entry's design exactly as `/api/library/design` does, so what uploads is what Reopen would
  open. *Picked.*
- **Case-only clash on Replace keeps the site's spelling** (the stored name wins) rather than
  renaming or refusing: it matches `saveDraft`'s rule that a save never renames, and the
  human already confirmed *this* design. *Picked.*
- **Replace leaves visibility alone**, so an upload can never unshare (or share) a design
  behind the owner's back. *Picked.*
- **No session-staleness refusal on upload** (§3.1): a hub on an older app version can still
  upload a design the newest live editor opens. *Picked:* the ruling's list of checks is about
  the design, and the stale-tab rule is about a tab.
- **An old `site-link.json` with no `scopes` key reads as read-only**, mirroring the
  migration's default on the site's side. *Picked.*

## 11. Deliberately not here

No overwrite-by-default, and no "always upload as new": the 409 is the feature. No editing an
uploaded design from Orb Manager, no delete, no un-share, no rename, no version list — the
bearer has exactly the two scopes §1 names, and `designs:write` means *add designs and add
versions of them, and share what it added*: it never deletes, never renames and never unshares,
and the only change it can make to something already on the site is the new version a human
confirmed.
No sound split (L0), no new art rules (fu2 and S3 already run inside `checkDesign` and
`saveDraft`), no asset-library picker (L1's "Your library" is the hub's own and its own
slice), no inbox, no `/send` (H3), no bakes (ruling 27), no compose or router change (the
`orb-hub` prefix already covers the method), no site-side upload throttle of its own beyond
`checkHubThrottle`, no batch or folder upload, and no upload from an app's editor page in the
hub: the dialog is the Manage view's, where the site section lives. `hub_link.scopes` is never
editable in place — reconnecting replaces the row, which is how H2 already works.

