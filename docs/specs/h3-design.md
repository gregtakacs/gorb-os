# H3 — Inbox, resolve, and the handle-change migration

## The rulings

Five rulings of `docs/design.md` §0, plus §7.3, §7.4, §7.6 and §8.4, bind everything below.
The contract sections turn them into routes, columns and sentences:

> - **23 (updates follow the design):** a design renamed by its owner, or whose shared name
>   changed with a handle change, reaches orbs as an update that renames the theme; the site
>   resolves an old name to its design through the handle history (§8.4).
> - **24 (a handle change warns first and offers to migrate):** published themes' shared names
>   move to the new handle at once (**Rename them now**), or each on its next update
>   (**Rename each on its next update**) (§8.4).
> - **25 (each Orb Manager is its own connection):** sending picks among them (§7.3).
> - **22/27 (two names, credited copy):** other people receive `<name> by @<handle>` from a
>   `sharedCopy` row; the site keeps no bakes (§8.4).
> - **44 (bearer doors name their scope):** every new hub door checks an explicit scope.

design.md §12's H3 gate: *Send reaches only the chosen Orb Managers within a minute, with
per-computer state; a Replace with a new name, and a handle change with each option, reach an
orb as a rename; a theme imported by file, and one with no file, resolves by name, through a
retired handle too (no bake hashes: ruling 27).* One reading of that gate, fixed by review:
**a design's name never changes on the site today** — ruling 29 makes a rename a new design,
and H4's Replace keeps the name its case rule preserves — so the only rename a hub is offered
in practice comes from a handle change. The contracts still handle the general case: a version
whose name differs from its predecessor's resolves to the new credited name, and the hub
offers that rename (a site test writes such a version row by hand to prove it, §4; a hub test
offers it, hub-3).

## 1. Data: the inbox, pinned to a version

### 1.1 The row

One new model, one migration (`prisma/migrations/20261121090000_h3_inbox_items`):

```prisma
model InboxItem {
  id          String    @id @default(cuid())
  designId    String
  design      Design    @relation(fields: [designId], references: [id])
  versionId   String    // the design's current version at send time
  hubLinkId   String    // the chosen Orb Manager's hub_link row
  state       String    @default("waiting") // "waiting" | "delivered"
  createdAt   DateTime  @default(now())
  deliveredAt DateTime?
  @@index([hubLinkId, state])
  @@index([designId])
  @@map("inbox_item")
}
```

`Design` gains the inverse `inboxItems InboxItem[]`. `HubLink` is referenced by id **without**
a relation field — a disconnect deletes the link's rows in one statement (§6.3), and a relink
re-keys them (also §6.3). The migration is `CREATE TABLE` plus the two indexes and nothing
else. The item is pinned to the **version**, exactly as §7.3 words it — and the bundle door
serves *that* version (§3.3), so the pin is the promise the delivery keeps, not just the prune
hint. `sentAt` in every response is `createdAt`.

### 1.2 A send is not a share — and a send is not only the owner's

An inbox item travels from a sender's account to the sender's own Orb Managers. Sending your
own design changes nothing about it (any visibility, through `ownBundle`'s door branch);
sending **someone else's shared design** never changes it either — the send makes sure the
design's current version has a credited copy (the same lazy `ensureSharedCopy` the bundle door
performs, §2.1) and pins that version. `/send` is the §4.4 main-site confirmation for
something leaving the shared-or-private space: not a second `/publish`, no visibility write,
no `publishedAt`, no handle requirement beyond what sharing already enforced.

## 2. The send door and the `/send` page

### 2.1 `POST /api/designs/[id]/send` (session, `*`)

Body `{"linkIds": ["<hub_link id>", …]}`, 1–50 ids, de-duplicated. Every id must name a live
`hub_link` row of the caller's account (H2's `listMyHubs` rows are what the picker was drawn
from); one id that is not the caller's, or is not live, refuses the **whole** send with 404
`{"error":"not_found","message":"No such Orb Manager."}` — a send is a human pressing a
button once, and a half-delivered send is exactly the ambiguity the §4.4 page exists to
remove.

**Who may send what:** the design is alive (`deletedAt` null) and either the caller's own
(any visibility) or shared now (`visibility ≠ "private"`). Anything else — another account's
private design, a deleted design, a purged design, a non-id — is the delete route's existing
one-sentence 404 (§8.3's rule: the design is not the caller's to know about). For the
not-the-caller's branch the door first calls `ensureSharedCopy(designId)` — exactly the call
`sharedBundle` makes lazily, so a design shared at an older version grows its copy before the
item pins the current one — then writes the rows in one transaction reading
`currentVersionId` (null → 409 `{"error":"no_version","message":"Nothing is saved to send
yet."}`). 201 `{"sent":[{"linkId","linkLabel","itemId"},…]}`, `no-store`.

### 2.2 `/send?design=<id>`

Built on `/publish`'s skeleton (`src/app/publish/page.tsx`): signed-in or redirected, and
`notFound()` for a design the caller may not send (2.1's permission, same sentences) or that
is not a design id at all. The header carries the design's name and app. The body is the
picker: every connected Orb Manager as a checkbox row (label, version, last seen — the
`HubLinkRow` shape `listMyHubs` returns, the same wording `/me`'s Connected Orb Managers
section uses), **all checked by default** (ruling 25: the picker's whole point is choosing
among them, and "send to all" is the press people mean). The confirm button reads `Send to
<n> Orb Manager` / `Send to <n> Orb Managers` (one is singular); zero checked disables it.

Below the list, §8's per-computer state: every item **of this caller's links** for this
design, newest send first, one row per computer — `<label> · waiting` or `<label> · delivered
<time>`, the relative-time wording `/me` sessions rows already use. Sending re-renders the
page with the new rows visible. An account with no connected Orb Manager sees the picker
replaced by one sentence linking to `/me`, and a design never sent by this caller shows
`Nothing sent yet.` above the picker. On `/t/<id>` the **Send to my Orb Manager** link stands
where `Share…` stands today for owners — and, unlike `Share…`, for **every signed-in viewer
who may send the design** (2.1's permission): a shared design's page offers the send to
everyone who can see it.

## 3. The inbox doors (hub bearer, `designs:read`, throttled)

Three doors, one concern. `GET /api/hub/inbox` and `POST /api/hub/inbox/deliver` in
`src/app/api/hub/inbox/route.ts` / `.../deliver/route.ts`, the bundle in
`src/app/api/hub/inbox/[itemId]/bundle/route.ts`. All three open
`checkHubThrottle` → `requireHubCaller` → `requireHubScope(caller, "designs:read")` — the
theme door's exact order (a token lookup is a DB hit, and ruling 44's fourth bullet applies
to every bearer door, not just public ones) — and join `tests/sf1-scopes.test.ts`'s
`BEARER_ROUTES` with that scope. Every answer is `no-store`.

### 3.1 `GET /api/hub/inbox`

This link's waiting items, oldest first, at most 32, **filtered to what may still be served
to this link**: the design is alive and (shared now **or** owned by this link's account).
An item whose design went private or deleted between send and poll is not answered (§8.3's
lifecycle, and review 9): it stays waiting until the thirty-day drop, and if deletion is what
hid it, the delete drop took it already.

```json
{"items":[{"id":"…","designId":"…","version":4,"app":"clock",
           "name":"Skeleton","sentAt":"<iso>"}]}
```

`version` is the pinned version's `n`; `name` its own name; `sentAt` ISO `createdAt`. The
item is **not** consumed by reading — delivery is a claim (3.2), and a hub that fetches the
bundle and dies before claiming re-asks and gets the same item. The empty answer is
`{"items":[]}`.

### 3.2 `POST /api/hub/inbox/deliver`

Body `{"ids":[…]}`, 1–32 non-empty strings (else 400 `{"error":"bad_request","message":
"Send {\"ids\": […]}."}`). One `updateMany` scoped to `id in ids AND hubLinkId =
caller.linkId AND state = "waiting"`, the touched ids re-read in the same transaction: 200
`{"delivered":[…],"missing":[…]}` — an id already delivered, unknown, foreign, hidden by
3.1's filter, or its design unshared answers `missing`, never an error (the answer says what
happened). `deliveredAt = now` once and once only: a double claim's second answer is
`missing`, which is why the hub writes its **disk file first and claims after** (7.1), and
why a claim that says `missing` deletes the held file.

### 3.3 `GET /api/hub/inbox/[itemId]/bundle`

The **pinned version's** bundle, for exactly the one link that was sent the item — §7.3's
"pinned to the version that was current", which is what makes a send reproducible after the
owner replaces again. Checks, in order: throttle; caller; scope; item exists, is `waiting`,
and `hubLinkId = caller.linkId`; the design passes 3.1's serve-filter. Any failure of the
last two — unknown id, foreign item, delivered item, hidden design — is one 404
`{"error":"not_found"}`, byte-identical, the theme door's stranger-shaped absence. The
success path builds the bundle for the **pinned version** through the *same builders the
theme door calls, grown one optional parameter*: `ownBundle(designId, ownerId, app?,
versionId?)` for a design the caller owns (the owner's own name at that version),
`sharedBundle(designId, app?, versionId?)` otherwise (that version's newest credited copy —
which the send made sure exists, 2.1). Same `DesignBundle` bytes, same headers the theme door
sets (`x-orb-theme-name` included), `no-store`. A pinned version whose row was pruned —
possible only between this step and the pin landing in step 5 — answers the same 404; after
step 5 a waiting item's version cannot die. The hub's poll fetches **this** door (7.1), not
`/api/hub/theme/[id]`.

## 4. `GET /api/hub/resolve` (ruling 23)

`src/app/api/hub/resolve/route.ts`, one method, throttled like the theme door. **A bearer is
optional and, when present, must hold `designs:read`** (ruling 44): `Authorization` present →
`requireHubCaller` + `requireHubScope` with the designs door's exact refusals; absent → the
caller is a stranger. Joins `BEARER_ROUTES` with its call built on a bearer; the anonymous arm
is asserted outside the loop, as the theme door's is.

Two query shapes, one route:

| Query | Means |
|---|---|
| `?site=<canonical site>&designId=<22>&version=<n>` | the hub's own origin record (`ThemeOrigin` sidecar; the exact URL `resolveOrigin` in `siteimport.go` builds) |
| `?site=<canonical site>&app=<slug>&name=<shared name>` | §7.4 point 3: a theme with no record, asked by name **and app** |

`version` is echoed-back context, never a filter — the answer is the design's **current**
version and current shared name, which is the whole point. `site` is required on both paths
(the hub always sends it; the client checks the host before the socket opens), `app` is
required on the name path (400 without it: one shared name can exist in two apps, and the
hub always knows its theme's app). `version` on the id path must be a positive integer,
`designId` 22 characters of `[A-Za-z0-9_-]`, `app` the site's ordinary app slug. Any other
request is 400 `{"error":"bad_request"}`, `no-store`.

**The answer, and its one law (review 1): the name resolve gives is the name the bundle door
would give the same caller.** The handler locates a design, checks visibility, and then calls
`ownBundle`/`sharedBundle` — the very functions `/api/hub/theme/[id]` calls, with the owner
test (`caller.userId === design.ownerId`) deciding which — and serializes the four identity
fields of the returned `DesignBundle`:

```json
{"designId":"…","app":"clock","version":6,"sharedName":"Skeleton by @greg"}
```

An owner's bearer answers the design's own name at the current version (what `ownBundle`
credits nobody with), and every other caller answers the credited copy's name —
`sharedBundle` creating the current version's copy lazily, exactly as a bundle fetch would.
No parallel read, no shadow copy of the naming rule: the two doors cannot drift, and a hub
whose shelf holds `Skeleton` is never offered a same-version "rename" that bakes `Skeleton`
again. Art and sizes stay untouched: four fields and nothing else.

**Visibility.** By **id**: another account's design answers only when shared now (unlisted or
public — the id is the link a share already handed out); the caller's own design answers at
any visibility. By **name**: another account's design answers only when **public** (an
unlisted one resolves by id only — a name is guessable, an id is a link); the caller's own
design answers at any visibility. Deleted or purged anywhere is the gone answer (410 on the
id path, 404 on a name).

**Gone and miss are one body.** Anything the route will not answer about — private from a
stranger, unlisted by name, deleted, purged, and every name-path miss (no match, several
matches, a homemade name) — answers **404 (or 410 for a deleted design on the id path) with
`{"gone":true}`**, `application/json`, `no-store`: the one body §7.4 and `resolveSaysGone`
read, now on both paths, so the hub's two clients behave one way at the wire. The name
path's silence toward a stranger's theme is hub-side policy (hub-3: a by-name gone marks and
never rows), not a different body.

**The name path, in order.** Split the name on the **first** ` by @` — the site's own
`sharedName` writer put it there, and an S3 design name may not contain it. The handle half
must pass `checkHandleGrammar`; a name that fails is homemade → the miss answer. Look the
handle up **current first** (`user.handle`), then retired (`handle` table → `userId`): a
retired handle resolves forever to its account (§8.4's history). That account's **alive,
public-or-own, app-matching** designs are candidates, and a candidate matches when **any
version's** `sharedCopy.sharedName` equals the asked bytes — not only the current version's,
because *Rename each on its next update* leaves the old name on an older version's copy —
**or** its current version computes to the name now (`sharedName(name, currentHandle)`, for a
shared design whose copy was never made). Zero distinct designs or two or more → the miss
answer; one → the answer above. A design found through an old copy still answers its
**current** version and **current** credited name: old name in, present answer out — which is
ruling 23's sentence, mechanically.

## 5. The handle-change migration (ruling 24)

### 5.1 The dialog grows

`src/components/handle-form.tsx`'s confirm panel gains, between ruling 11's warning sentence
and the buttons, §8.4's sentence (the copy table) — and above it a preview fetched from
`GET /api/me/handle-change?handle=<candidate>` (session, `*`, grammar-checked candidate,
`no-store`), answering `{"shared":[{"id","name","app"}],"tooLong":[{"id","name"}]}` over the
account's alive non-private designs, each computed against the **candidate** handle. Zero
shared designs: the panel is today's, byte for byte, and the POST may omit `migrate`. Shared
designs listed: two radio choices — `Rename them now` (default) and `Rename each on its next
update` — and any non-fitting one blocks the change with today's `handle_names_too_long` 409,
which the panel already prints verbatim. After a *now* change, the success view prints one
line per design the copy builder refused (§8's row) when the answer names any.

### 5.2 The POST grows one field

`POST /api/me/handle` body gains optional `"migrate": "now" | "next"`. A grammar-invalid value
is 400 `{"error":"bad_request"}` beside the existing rows; the success object gains
`"migrated": "now" | "next" | null` (null on the no-shared-designs path) and `"notMoved":
[{"id","name","reason"}]` (empty on every clean path). The rule: **when the account has any
alive shared design, `migrate` is required** — 409
`{"error":"migrate_required","message":"Choose how your shared themes move to the new handle."}`
without it. The form always sends one; the refusal is the guard for the human who scripts the
door, because silently choosing *next* for someone who meant *now* is the surprise ruling 24
was written to prevent. `setHandle` grows the choice as a parameter, so dialog, route and
library agree through the signature.

### 5.3 `migrate: "now"` — the second credited copy, after the commit

`setHandle`'s transaction stays what it is: lock, refusals, the fit pre-check (a name that
would not fit the candidate still blocks the whole change with `handle_names_too_long` before
anything moves), the swap, the retirement of the old handle into the history. It ends there.
When `migrate === "now"` it then returns the set of designs to move, and the **copies are
made after that transaction commits** (review 13: the transaction holds an advisory lock and
file bytes do not belong under it). For each alive non-private design, at its **current
version**: one credited-copy row with `sharedName(name, candidate)` — created through
**one function in `sharing.ts` that `ensureSharedCopy` also calls** (extract it — say
`makeSharedCopy(design, version, handle)` — so there is one copy builder: its document +
`shared` object and stored file, not a second), with `skipDuplicates` on the existing
`designVersionId_handle` unique key, which makes a crash between commit and copies
re-runnable and the whole pass idempotent. No new `designVersion`, no `publishedAt` change,
nothing deleted: old copy rows stay, so old names keep resolving through the history (§4),
and a *now* pass that dies halfway leaves a design whose newest copy is still the old name —
which is exactly *next*'s state, and the next *now*-pass or Replace completes it.

A design whose copy is **refused** (`name_refused`, `art_refused` from the builder — a stored
art file that can no longer pass today's limits) is skipped, the handle change still stands,
and the answer lists it in `notMoved` with the code as `reason`; the dialog prints §8's line
per entry. A design whose current version has no copy gets its first one, candidate-named.
The resolve answer for *next* is the old copy's name until the next Replace, whose version's
first copy carries the new handle through the unmodified `ensureSharedCopy` — the ruling's
words made mechanical.

### 5.4 Where it lands on the orb — and the one hub gate that must move

The rename reaches an orb through machinery that exists: §4's resolve answers the current
credited name, and G3's apply bakes, installs, selects and removes in that order. But G3's
row builder skips a same-version answer — `updates.go:246` drops anything with
`ans.Version <= o.Version` before it ever computes `Renamed` — and apply re-checks the same
gate at `updates.go:498`. *Rename them now* leaves the version alone, so **hub-3 relaxes
exactly this**: a row is drawn, and apply runs, when the version is equal **and** the answer's
shared name differs from the shelf title (a pure rename); `skip`/`keep` choices and the
skip-record semantics carry over unchanged, keyed by the same version. `updateBake` needs no
change — it re-fetches the current bundle for `ans.Version`, and re-baking the same version
under a new name is what a rename always was.

## 6. Lifecycle: pins, drops, the move, the 30-day end

### 6.1 The prune pin (§8.2 point 2)

`pruneVersions` (`src/lib/storage.ts`) keeps the last 10 versions **plus every version an
undelivered (`state = "waiting"`) `InboxItem` points at** — the comment at storage.ts:127 is
the promise; this slice keeps it. A delivered item pins nothing (its bundle sits on the hub's
disk). The pin read is one `findMany` of the design's waiting `versionId`s, beside the
`remixOf` pins, from the same `tx`. This is load-bearing, not tidying: 3.3 serves the pinned
version, so a waiting item's version must survive.

### 6.2 A delete drops the items (§8.3)

`deleteDesign` (`src/lib/library.ts`), inside the tombstone transaction, gains
`tx.inboxItem.deleteMany({where:{designId}})` — every account's items, waiting or delivered.
The comment at library.ts:896 is the promise. A sender who was mid-poll simply stops seeing
the item; the bundle door already answers the delete 404 and the claim door answers it
`missing`.

### 6.3 Moves, drops, and the 30-day end

A **relink** (the exchange's replace, `src/app/api/hub/token/route.ts:158`) **moves** waiting
items rather than deleting them (review 12): in the same transaction, read the replaced row's
id, and after the new row is created `inboxItem.updateMany` from the old id to the new — a
computer that reconnects is the same computer, and its undelivered sends are still waiting for
it. Delivered rows do not move (they are the history of the old row; re-keying them would
rewrite what happened). **Disconnects** drop: the bearer's own door (`:232`, same transaction)
and `/api/me/hub/disconnect` (items first, then the row) each `deleteMany` by `hubLinkId` —
an item whose link is gone can never be delivered. Account deletion already cascades where
H2's delete sites run.

And the sweep gains `purgeStaleInbox` (`src/lib/purge.ts`, beside `purgeDeletedDesigns`,
called from `runSweep`, a seventh key `inbox`): every waiting item older than 30 days deleted
— `state = "waiting" AND createdAt < now − 30d`; delivered rows are left. §7.3's "an
undelivered item is dropped after thirty days", made the hourly cron, fail-open like the
file purge, one named `INBOX_MAX_AGE_MS`.

## 7. The hub: a hold that survives a restart, resolve, and themes on orbs only

### 7.1 The poll loop and the disk hold

`tools/orb-manager/siteinbox.go`, new, with `go s.siteInboxLoop(cableCtx)` in `main.go` beside
`go s.probeLiveness(cableCtx)` (:868) — the file copies `liveness.go:529`'s ticker shape and
`release.go:254`'s once-then-tick rhythm. Interval `siteInboxEvery = 60s`, first tick jittered
like the liveness sweep's. Each pass, per live site link (`liveSiteLink`, sitelink.go:196 — a
gateway is a site, and its link carries the bearer):

1. `GET <siteBase>/api/hub/inbox` with the link's bearer, `If-None-Match` unused, timeout
   `siteFetchTimeout` reused. Any error, or a non-200: **log one line and move on**; the hold
   on disk is what makes a missed poll harmless.
2. For each item not already held: **write the hold file first** —
   `<configDir>/site-inbox/<itemId>.json`, directory `0700`, file `0600`, atomic write
   (temp+rename), envelope `{itemId, designId, name, app, version, sentAt, claimed:false,
   raw:"<base64 bundle>"}`. Fetch the bundle from
   `GET <siteBase>/api/hub/inbox/<itemId>/bundle` **before** writing it in (the raw bytes are
   the hold — 3.3's door, not the theme door: an item that is pinned to version 4 while the
   design sits at 7 must still deliver 4, review 8). A fetch failure: no file, ask again next
   minute. At most `siteInboxMax = 32` files: when full, **stop fetching** — remaining
   items stay waiting on the site and are asked again in a minute; nothing is dropped, nothing
   is refused. The write is `fsync`-ed and only then:
3. `POST /api/hub/inbox/deliver` `{"ids":[…]}` for the held-and-unclaimed ids of this pass.
   Answered `delivered` → set `claimed:true` in the file and rewrite it atomically. Answered
   `missing`, or the door errors → **delete the file** (the site says this item is not for
   delivery to us; a re-fetch next minute re-reads the truth).
4. Restart recovery: every unclaimed file is re-claimed (step 3) before anything else; every
   claimed file is offered to the shelf (step 5). A restart mid-hold therefore loses nothing:
   the item is still `waiting` on the site until the claim, and the bytes are on disk after it.
5. The tray's **Add** answers `POST /api/site/inbox/add {"itemId":…}`: read the hold file, run
   it through the *existing* `checkBundle`, file the bytes in the existing `siteTicketStore`
   and answer **exactly what `handleSiteImport` answers** (ticket + `site,id,version,app,name,
   sharedName,handle,schema`) — the browser's editor flow from there is byte-identical to a
   paste import, bake included, and `/api/site/keep` writes the `.origin.json` sidecar through
   the unchanged path with the envelope's identity. Add sets `imported:true` in the file.
6. The file dies when its news is on the shelf: a later pass finds a library theme whose
   origin record names this envelope's `designId` + `version`, and deletes the file (the
   settle-after-success rule, and the reason a keep that never arrived leaves a retriable file
   rather than a hole). Dismissing deletes the file and asks `deliver` for the id on the way
   out (a claim the site answers `missing` is exactly what a dismiss means).

The tray itself: `siteInboxList()` feeds `GET /api/site/inbox` (requireControl, the same idiom
`/api/site/designs` uses) and a **From orb-os.net** block in the Themes tab beside the
`/api/updates` section — §8's waiting/delivered wording, `Nothing waiting.` when the directory
is empty or unreadable. Add and Dismiss are posts to `/api/site/inbox/add` and
`/api/site/inbox/dismiss`.

### 7.2 Name resolve, and what a hub does with each answer

`resolveOrigin` in `siteimport.go` (line 638) grows the by-name path; the answer policy:

| Answer | Origin-record path (§7.4 point 2) | By-name path (§7.4 point 3) |
|---|---|---|
| 200 with identity | tracked row; apply offers rename | tracked row; apply offers rename |
| **gone** (404/410 `{"gone":true}`) | `.origin.gone` marker, row stays (today's `markGone` shape) | **`.origin.gone` marker on the shelf theme, no row, nothing offered** — the name was public once and is not anymore; the marker is the one quiet fact |
| 400 | log line, **no row** | log line, **no row** |
| transport error / timeout | log line, **no row** — *ask again next time* | log line, **no row** |

The `can't trace` row G3 drew for a by-name gone is **dropped** (review 5): the shelf marker
carries the news, and a row that appears every time the dialog opens is the noise the review
called it. `resolveSaysGone` (siteimport.go:713) stays the gone test on both paths. Every
resolve call goes to **the site the hub is configured with** — `siteReleaseBase()`
(release.go:70), the same base a bake is fetched from, whether or not the design's owner's
account is linked — gated by `siteAllowed`, with `siteBearerFor(site)` (sitelink.go:211) as
`Authorization` when a link has a token and no header at all when none does. The hub never
discovers a different site from the name's handle: one configured site, one origin of truth,
and a ` by @<someone>` name from elsewhere is simply a miss there. **The client sends
`&app=`** on every by-name call (the site's `app` field on the theme entry; item 3).

The pure-rename gate moves as §5.4 says: equal version with a different name is a row and an
apply, and the apply path is `applyThemeUpdate`'s existing NeedBake arm (ask at updates.go:383
/ the answer at :647), whose payload already carries `OldName`/`NewName`/`Renamed`.

### 7.3 Themes on orbs only (design.md §7.4's third case, its own step)

A theme installed from a file long ago, or pushed to an orb by hand, has **no library entry and
no origin record** — design.md §7.4's third case. Hub-4 (`docs/specs/h3-hub-4-orb-only-names.md`)
enumerates them from the orbs' own cards (`forEachOrbCard`, updates.go:715: every granted IP's
cached `ThemeList` — `ThemeEntry`'s `Name`, `App`, `Stored`, `Active` at
internal/orbnet/themes.go:61), skipping any title the library already holds under the same name
(that is the tracked case) and any homemade name (`checkSharedName` fails) or card with no app.
Pressing **Track this theme** asks the configured site by **name + app** — one call, the same
shape a file import uses — and the three possible outcomes are hub-3's policy verbatim: a gone
answer writes a durable ledger mark (`<configDir>/site-orb/`) and **no row**, a NotYet or a
transport failure logs and records nothing, and an answer adopts the theme by handing a freshly
built `ThemeOrigin` to **the existing apply path** (`applyThemeUpdate` → `updateBake`): fetch the
current bundle through the theme door, bake (or `NeedBake`+ticket, on a hub whose baker is a
browser), file the shelf theme with its origin record, install on the orbs that answered, select,
and **remove the old name after the install and select**. From that pass on it is an ordinary
tracked theme and the card no longer enumerates. The door answers `applyPayload`, the same object
`/api/updates/apply` answers: one web flow, not a second. Tests run against `internal/orbfake`
(grounded: `orbfake.New(orbfake.Theme{…})`, `Orb.Calls()`) plus the httptest-site rig
`siteimport_test.go` uses.

## 8. The copy table

| Where | Sentence |
|---|---|
| `/send` header | `Send <name> to your Orb Managers.` |
| `/send` confirm (n=1) | `Send to 1 Orb Manager` |
| `/send` confirm (n≠1) | `Send to <n> Orb Managers` |
| `/send`, no links | `No Orb Manager is connected to this account yet. Connect one from /me.` |
| `/send`, never sent | `Nothing sent yet.` |
| `/send` state row (waiting) | `<label> · waiting` |
| `/send` state row (delivered) | `<label> · delivered <relative time>` |
| handle dialog, ruling 24's line | `Themes already on your computers keep their old names until the next update.` |
| handle dialog, choice A | `Rename them now` |
| handle dialog, choice B | `Rename each on its next update` |
| handle dialog, preview header | `This changes the shared name of <n> design(s):` |
| handle success, per unmoved design | `<name> kept its old shared name: the new name would not fit.` |
| tray row, waiting | `<name> · from <site> · waiting` |
| tray row, delivered | `<name> · from <site> · delivered <relative time>` |
| tray, empty | `Nothing waiting.` |
| tray, Dismiss | `Dismissed.` |
| orb-only tray, empty | `Nothing untracked.` |
| orb-only tray, gone name | `<name> is gone from the site.` |

By-name gone/NotYet/errors print **no row at all** (review 5): the `.origin.gone` marker and one
hub log line are the whole surface. The record path's existing Gone row and
"could not be checked" note (updates.go's own strings) are unchanged.

Refusal bodies name their code in one `message` sentence and nothing else:
`not_found` / `no_version` / `migrate_required` / `bad_request` as contracted above, and the
inherited `unauthorized` / `insufficient_scope` / `rate_limited` shapes.

## 9. The step specs, and how they chain

Seven step specs, two chains, in `docs/specs/`. Site chain (repo `orb-os-net`):

| # | Spec | One line |
|---|---|---|
| 1 | `h3-site-1-inbox-doors.md` | model + migration + the three bearer inbox doors (throttled, bundle door serves the pinned version), sends incl. other accounts' shared designs, scope tests |
| 2 | `h3-site-2-resolve.md` | `/api/hub/resolve` both paths, parity with the bundle door, visibility table, gone-for-miss |
| 3 | `h3-site-3-send-page.md` | `/send` page + `/t/[id]` link for every may-send viewer + state table |
| 4 | `h3-site-4-handle-change.md` | preview door, `migrate` field, `makeSharedCopy` post-commit, `notMoved`, dialog copy |
| 5 | `h3-site-5-inbox-lifecycle.md` | prune pin, delete drop, disconnect drop, relink move, 30-day sweep |

Hub chain (repo `gorb-os`'s `tools/orb-manager`, continues G1–G3; step 1 is already merged):

| # | Spec | One line |
|---|---|---|
| 1 | `h3-hub-1-editor-commit-scope.md` | (merged) editor commit scopes — unchanged, first link |
| 2 | `h3-hub-2-inbox-client.md` | poll loop, **durable disk hold**, pinned-version bundle fetch, claim-after-fsync, tray |
| 3 | `h3-hub-3-name-resolve.md` | by-name + by-id resolve on the configured site, gone/NotYet policy, the pure-rename gate |
| 4 | `h3-hub-4-orb-only-names.md` | themes that live only on an orb: found by card, asked by name+app, adopted through the existing apply, marked when gone |

**Chain guards.** A step's `verify` runs its gates against **the branch its predecessor
finished on**, named in the chain JSON — never against `h3` — because every step's diff
against `h3` contains its predecessors' work and a scope gate written that way passes only by
accident. Step 1 of each chain is the exception and diffs against the base (`h3` for the site
chain, `h3`-lineage `HEAD` for the hub chain). Hub guards run **from the repo root** (pathspecs
are cwd-relative; `tools/orb-manager` prefix inside `tools/orb-manager` matches nothing) —
`git -C "$(git rev-parse --show-toplevel)" …` inside each verify. Scopes:

- site steps: `! git diff --name-only <prev> | grep -qvE '^(src/|tests/|prisma/migrations/20261121090000_h3_inbox_items/|docs/specs/)'`
- hub steps 2–4: `! git -C "$(git rev-parse --show-toplevel)" diff --name-only <prev> | grep -qvE '^tools/orb-manager/'`

Site steps' test gates run vitest with `--reporter=verbose` and **grep the named new tests'
titles** out of it (review 16) — an exit code alone proves nothing about which tests ran;
each step spec lists the titles its verify greps. Hub steps gate `go test ./...` (and
`go vet`) in `tools/orb-manager` as G1–G3 did.

## 10. Worked example, end to end (the whole slice in one minute of watching)

`Skeleton` at version 4, public, owner `greg`. Greg opens `/t/<id>`, presses **Send to my Orb
Manager**, checks the one computer, presses **Send to 1 Orb Manager**: the page now reads
`Kitchen · waiting`. Within 60 seconds the hub polls, gets the item, fetches
`/api/hub/inbox/<id>/bundle` — version 4's bytes even though Greg replaces again at once —
writes `site-inbox/<itemId>.json`, claims, keeps the file through a restart test, imports, and
the tray row flips to `delivered`. `orb themes update` offers nothing yet (same name). Greg
changes his handle to `g2` choosing **Rename them now**: the current version grows a second
credited copy, `Skeleton by @g2` (the old `@greg` copy stays). The hub's next update pass
resolves by id, the answer's name differs at the same version — the gate hub-3 moved — and the
orb is renamed, old name removed after select. `Skeleton by @greg` still resolves (§4's
retired-handle lookup) to the design, and the answer carries `Skeleton by @g2`, the same bytes
the bundle door hands that stranger in `x-orb-theme-name`. On a second computer that received
the design under Greg's account, the same resolve answers Greg's **own** name at the current
version (`ownBundle`'s name, not a credited one) because that bearer is Greg's — one law, both
doors.

A theme pushed to an orb by hand years ago (`Clockface`, app `clock`, no library file):
hub-4 asks `?site=…&app=clock&name=Clockface by @g2`, gets the identity, bakes through
NeedBake, installs, selects, removes `Clockface`, keeps the new name in the library with its
origin — next pass it is ordinary. Ask the same name while the design is unlisted:
`{"gone":true}` for a stranger, a shelf marker, no row.

## 11. Traceability

| Ruling / gate | Where this slice lands it |
|---|---|
| 23 renames follow the design | §4 (resolve by id/name), §5.3+§7.2 (name moves, version may not), hub-3's gate at §7.2 |
| 24 warn first, migrate offered | §5.1 preview, §5.2 required `migrate`, §5.3 second copy, §8 sentences |
| 25 each hub its own connection | §2.1 whole-send refusal, §2.2 default-all picker, §6.3 per-link items |
| 27 credited copies, no bakes on the site | §4 (resolve calls the same builders; no bake on any path) |
| 29 rename = new design | §0's gate reading: the only in-place rename is §5's; §4 still answers a version-name difference honestly |
| 44 bearer doors name scope | §3 (three doors, `designs:read`, throttled), §4 (optional bearer, same scope) |
| §7.3 pinned version, per-computer state, 30 days | §1.1 pin + §3.3 pinned bundle + §6.1 pin vs prune + §6.3 sweep |
| §7.4 file import, gone, no-record theme | §7.1 (import = existing path), §7.2 (answer policy), §7.3 → hub-4 |
| §8.2 point 2, §8.3 lifecycle | §6.1, §6.2 |
| §8.4 handle history | §4 (retired resolves), §5 (old copies stay), §10's paragraphs |
| §12's H3 gate | §9 chains' verifies; hub-2's restart test and hub-4's orbfake test carry its two hard halves |

This document answers review round 2's items 1–16: the resolve/bundle-name law (1),
any-version matching (2), `&app=` (3), the visibility table (4), gone-for-miss and the dropped
trace row (5), the configured-site rule (6), orb-only names as its own step (7), the
pinned-version bundle door (8), sends of others' shared designs (9), throttling on all doors
(10), the disk hold (11), relink moving items (12), post-commit copies with `notMoved` (13),
the gate wording (14), prev-branch guards (15), and verbose-title test gates (16).

