# H2 design: linking a hub to an account, My orb-os.net, and `/me` connections

> **Lead review, 2026-11-10 (binding; it overrides anything below that disagrees).**
> 1. `/connect` parameter rules (§2.1): `redirect` must match
>    `^http://127\.0\.0\.1:([0-9]{4,5})/callback$` with the port 1024–65535 (a port is required);
>    `challenge` is exactly 43 base64url characters; `state` is 16–128 base64url characters
>    (`[A-Za-z0-9_-]`); `version` is 1–32 characters of `[0-9A-Za-z.+-]`; `label` is 1–64 characters
>    after trimming, refused when it holds a control (Cc), format (Cf, which covers bidi controls and
>    zero-width characters) or line/paragraph separator character. The page shows the
>    `<label> · <Platform>, Orb Manager <version>` line, and `/connect` answers with
>    `Content-Security-Policy: frame-ancestors 'none'` and `X-Frame-Options: DENY` (no framing:
>    the Allow button must not be clickjackable).
> 2. Allow / Not now answer **303** (not 302), `cache-control: no-store`.
> 3. `POST /api/hub/token` refusals are **400** `{"error":"invalid_grant"}` (RFC 6749 §5.2), not 401.
>    The relink delete is scoped to the grant's `(userId, installId)`. A code presented with a
>    wrong verifier is used up too (claim first, then compare; a failed compare does not roll the
>    claim back). The 200 body is
>    `{"token":"<secret>","hubId":"<link id>","site":"<canonical site origin>","handle":<string|null>}`.
> 4. Expiry is `(lastSeenAt ?? createdAt)` older than 90 days, everywhere (caller, sweep). Never
>    `createdAt` alone.
> 5. Hub link state (§5): `site-link.json` is `{site, token?, linkedAt, handle?, disconnected?}`.
>    A 401 from the site on any bearer call **rewrites** the file as
>    `{site, handle, disconnected: true}` (no token), so the hub shows **"Disconnected from
>    orb-os.net"** with **Reconnect** after a restart too. Disconnect in Orb Manager removes the
>    file. `GET /api/site/link-status` answers
>    `{"ok":true,"data":{"state":"none"|"pending"|"linked"|"disconnected","site"?,"handle"?,"linkedAt"?}}`
>    and never the token. The token is sent only to the origin recorded in the file, and only while
>    that origin equals `siteReleaseBase()`; a file naming another site reads as `none`.
> 6. Connect (§5): a press while an attempt is pending **replaces** it (the old listener is closed).
>    The callback listener answers 400 to a wrong path, method, Host (anything but
>    `127.0.0.1:<its port>`) or state, and such a request does **not** end the attempt (a page that
>    guesses can't cancel your sign-in). Only a callback with the right state ends it: `code` (then
>    the exchange) or `error=access_denied`. After a successful exchange, a token the hub held
>    before is revoked best effort (`DELETE /api/hub/token` with the old bearer), so an install that
>    relinks to another account leaves no live token behind.
> 7. Ending sessions on `/me` (§6): **Sign out** on a row ends that session by its session **id**
>    (`POST /api/me/session/sign-out {id}`; the server finds the caller's session with that id; a
>    session token never reaches the browser, not even a hint). The current row's Sign out works
>    and signs this browser out. **Sign out everywhere else** (`POST /api/me/sign-out-elsewhere`)
>    ends every other browser session (and, by the cascade, their studio sessions); this browser
>    and the Orb Manager connections stay. **Sign out everywhere** (`POST /api/me/sign-out-everywhere`)
>    ends every browser session including this one, every `hub_link` and every unused
>    `connect_grant` of the account; the page then goes to `/`. That last one is the gate's
>    "Sign out everywhere ends both".
> 8. There is no account-deletion path on the site today; the `userId` cascade is asserted by
>    deleting a user row with Prisma in `tests/hub-me.test.ts`, not through `tests/delete.test.ts`.
> 9. S2 stays intact (hub): a test sends the editor token (and the editor origin's Host) to every
>    new `/api/site/*` route on the control listener and sees each refused, and with `-site` off a
>    fake site receives **no request at all** across a whole hub run of those routes.

Slice H2 of docs/design.md §12. One document, two repos: the site steps run in
orb-os-net (`h2-site-chain.json`) and the hub steps in gorb-os
(`h2-hub-chain.json`). Before the hub chain runs, copy this file,
`h2-hub-chain.json` and every `h2-hub-*.md` into the gorb-os checkout under
`docs/specs/`, so the hub steps can cite them by repo-relative path.

Gate row (§12 H2): link two Orb Managers and see both; relinking one replaces it;
disconnect from each side, then the next call is refused; Sign out everywhere ends
both. Every clause is mapped to a named test at the end.

## 1. Scope

In: the PKCE connect flow (§7.2 steps 1–4) with install ids; the hub token and its
routes under `/api/hub/`; My orb-os.net in Orb Manager (design list with Import by
own-design id, which extends the H1 bundle door to the owner's private designs);
`/me` sections Connected Orb Managers and Signed-in browsers, with per-row
Disconnect and Sign out, Sign out everywhere else, and the studio-session
cascade that makes per-browser sign out real.

Out: the inbox and Send (§7.3), `/api/hub/resolve`, §7.4's site updates for
imported themes — all H3. Orb Manager's Upload stays hidden (see §8). No new
scopes beyond "read your designs": a hub reads designs and imports them, it never
writes, publishes or deletes anything on the site.

## 2. The wire contract (site surface)

The hub chain builds against this section and nothing else; its fake site
implements it verbatim. All bodies are JSON with `cache-control: no-store`. The
routes under `/api/hub/` ride the `orb-hub` router: no Google login, no browser
session, no studio cookie reaches them. `tests/hub-routes.test.ts`'s ban list stays
byte-identical — hub routes authenticate through `src/lib/hub-caller.ts` (§4),
never through session machinery.

### 2.1 `GET /connect` — the approve page (session page, not under `/api/hub/`)

Query, all required: `install` (32 lowercase hex — the G3 install id), `label`
(the hub's computer name, ≤ 64 bytes), `platform` (one of `windows` `macos`
`linux`), `version` (the hub's version string, ≤ 64), `redirect`
(must match `^http://127\.0\.0\.1(:[0-9]{1,5})?/callback$` — loopback only, no
path beyond `/callback`, no query), `challenge` (base64url, 43–128 chars, from
RFC 7636's unreserved set), `state` (≤ 512 printable chars, opaque). A signed-out
GET redirects to `/login?next=%2Fconnect…` exactly as `/me` does
(src/app/me/page.tsx). Any required param failing its rule renders the page as a
refusal card with the verbatim body line
`That is not a valid Orb Manager connect request.` and no form.

The page says (verbatim, with the hub's own values): heading
`Link Orb Manager to orb-os.net`; question
`Allow Orb Manager on <label> to read your designs?`; and — only when a live link
with the same `install` already exists on this account — the line
`This replaces the existing connection with this install.` Buttons: `Allow` and
`Not now`. The form is a plain POST to `/api/connect` carrying the seven params
again as hidden inputs plus `decision`.

### 2.2 `POST /api/connect` (session, main host; `requireCaller(req, "*")`)

Same-origin only: the request's `origin` must equal its host (the fence's rule,
checked again here, not just in the proxy). Body
`{install,label,platform,version,redirect,challenge,state,decision:"allow"|"deny"}`,
validated with the §2.1 rules.

- `deny` → 302 to `<redirect>?error=access_denied&state=<state>`. No grant stored.
- `allow` → 302 to `<redirect>?code=<code>&state=<state>`, where `code` is 32
  random bytes base64url. The site stores a `connect_grant` row: sha256 hex of the
  code (unique), the challenge, the user id, install, label, platform, hubVersion,
  `expiresAt = now + 60 s`, `usedAt = null`. Nothing is linked yet; approval alone
  grants nothing (§7.2 step 3 needs the verifier).

### 2.3 `POST /api/hub/token` — the exchange (no auth; throttled)

Body `{code, verifier}`. Throttled first,
`checkHubThrottle(clientIpFromHeaders(...))` like the theme door: over budget →
429 `{"error":"too_many_requests"}` with `Retry-After`. Then: look the grant by
sha256(code); require unexpired and `usedAt = null`; require `verifier` to be
43–128 chars of `[A-Za-z0-9\-._~]` and
`base64url(sha256(verifier)) === challenge`. Every failure — unknown, used,
expired, wrong verifier, malformed body — answers the same
401 `{"error":"invalid_grant"}`; the exchange never says which.

On success, in one transaction: mark the grant used; delete any live `hub_link`
row with the same `(userId, installId)` — relinking replaces, old token dies —
and insert the new link with a fresh token from `mintToken()` (32 bytes
base64url, hash stored; src/lib/dev-token.ts). Answer 200
`{"token":"<secret>","hubId":"<link id>","site":"<canonical site origin>"}`.

### 2.4 `DELETE /api/hub/token` (hub bearer)

Revokes this hub's own link: delete the row → 200 `{"ok":true}`. Missing or dead
bearer → 401 `{"error":"unauthenticated"}` (already-gone also answers 401; a hub
forgetting its token after a site-side Disconnect treats 401 as done, §6).

### 2.5 `GET /api/hub/designs` (hub bearer)

The owner's designs, newest first, at most 500, one row per non-deleted design
that has a current version:
`{"designs":[{"id","app","name","version","visibility","updatedAt"}]}` (`version`
is the `DesignVersion.n` of the current version, `updatedAt` ISO). Optional
`?app=<slug>` filters to one app. Each accepted call bumps the link's
`lastSeenAt` — that column is the 90-day clock and the `/me` row's "last seen".

### 2.6 `GET /api/hub/theme/<id>` — the H1 door, with a bearer

Same route, one extension. No `Authorization` header: today's behaviour,
unchanged. A bearer that does not resolve to a live link:
401 `{"error":"unauthenticated"}` even for a public design — a hub holding a dead
token must learn it is dead, or it would keep showing a list it cannot import
from. A live bearer and a design owned by that link's user: the **own bundle** —
same `{"format":"orb-design-bundle","bundle":1,…}` shape, same identity headers —
built from the owner's own current version: `name` and `sharedName` both the
design's own name, `handle` the owner's handle, `design` the owner's stored
document with no `shared` object. Bearer resolved but design not owned and not
public/unlisted: 404 `{"error":"not_found"}`, exactly what a stranger gets;
deleted: 410. Import-by-own-id (§5) rides this door, so what the hub stores
beside the import is an H1 `ThemeOrigin` — one import path, one record.

## 3. Data (site)

Migration `20261114090000_h2_hub_links`, one file, raw SQL (it deletes rows):

- `hub_link`: `id` cuid pk; `userId` → user Cascade; `installId`, `label`,
  `platform`, `hubVersion` Strings; `tokenHash` String **unique**; `createdAt`
  default now; `lastSeenAt DateTime?`. `@@unique([userId, installId])`,
  `@@index([userId])`, `@@map("hub_link")`.
- `connect_grant`: `id` cuid pk; `codeHash` String unique; `userId`, `installId`,
  `label`, `platform`, `hubVersion`, `challenge` Strings; `expiresAt` DateTime;
  `usedAt DateTime?`. `@@index([expiresAt])`, `@@map("connect_grant")`.
- `studio_session`: add `sessionId String NOT NULL` → `Session.id` **onDelete
  Cascade** + `@@index([sessionId])`. The migration first `DELETE FROM
  "studio_session"` — pre-H2 rows have no parent to name, and they are ≤ 8 h
  live, so nobody loses more than an open tab.

§5's `hub_link` row listed `scopes` and `revokedAt`; there are neither. One scope
in H2 (§1), and revocation deletes the row — a revoked hash kept on disk buys
nothing, and every "is it live" check becomes one query. `connect_grant` is not
in §5's table because §5 lists data at rest, not the state a 60-second single-use
code needs; §7.2's step 3 cannot exist without it. `studio_session.sessionId` is
H2's own addition and §7.2's Sign-out-per-browser row is the reason: Better Auth
deletes the session row, the cascade takes its studio tabs with it; without the
FK a signed-out browser keeps editing for hours. The launch payload
(`src/lib/launch-code.ts` `LaunchClaims`) gains `s` (the session id); `Caller`
gains `sessionId?: string`, set only by the session branch of `createCallerFns`;
`createStudioSession` takes and stores it; `findStudioSession` reads through the
relation and refuses when the parent `Session` is past its own `expiresAt`, so an
expired-but-not-yet-pruned session carries no studio tab (§4.4).

## 4. Who the hub caller is (`src/lib/hub-caller.ts`)

`getCaller` (§4.3) stays the browser/session/dev-token seam it is; the hub bearer
does not become a fourth branch there — the `/api/hub/` routes self-authenticate
(the compose chain sends them no session to consult) and the ban-list test stays
untouched. A new pure module owns the hub branch, shaped like the two it imitates:

- `requireHubCaller(req, db): Promise<HubCaller>` — parses the `authorization`
  header with `parseBearer`, resolves it against `hub_link.tokenHash` (sha256 hex
  of the presented secret, compared the way `resolveDevToken` does), touches
  `lastSeenAt`, and throws `CallerError(401, "unauthenticated")` for missing,
  malformed, unknown, or 90-day-stale tokens. `HubCaller` is
  `{ userId, linkId, scopes: ["designs:read"] }`.
- `hubCallerErrorResponse(e)` — the routes' one-liner, same shape as
  `callerErrorResponse`.

`src/app/api/hub/token/route.ts` and `designs/route.ts` import only this module
(and a new `src/lib/hub-token.ts` for mint/verify, which imports the pure half of
`dev-token.ts`), never `@/lib/caller-default` — that is what the ban list guards,
and it guards it by staying exactly as written. The theme route consults the
bearer through the same `requireHubCaller` and maps a refusal to 401 §2.6 *only
when an `authorization` header was present*; with no header the H1 anonymous path
runs unchanged.

`HUB_ROUTES` in `tests/hub-routes.test.ts` grows to the new inventory, and its
`access` value grows the per-method form (§9's test comment says declare it with
its access — that is this):

```ts
"releases/latest/route.ts":   { methods: ["GET"], access: "anyone" },
"theme/[id]/route.ts":        { methods: ["GET"], access: { GET: "anyone-or-hub-token" } },
"designs/route.ts":           { methods: ["GET"], access: "hub-token" },
"token/route.ts":             { methods: ["POST", "DELETE"], access: { POST: "code-verifier", DELETE: "hub-token" } },
```

with `type HubAccess = "anyone" | "anyone-or-hub-token" | "hub-token" |
"code-verifier"` and `access: HubAccess | Partial<Record<HttpMethod, HubAccess>>`.

## 5. My orb-os.net (hub surface, for the hub chain and its UI step)

The hub is the PKCE client (RFC 8252 loopback). New file `sitelink.go`, new
credential file, five control routes (all behind `requireControl`, registered
only when `!s.siteHidden`, none on the editor-API listener):

- `POST /api/site/connect` — mints `verifier` (32 random bytes base64url) and
  `state` (16 random bytes hex), opens the loopback listener on `127.0.0.1:0`,
  and returns `{"ok":true,"data":{"url":…}}` where url is
  `<siteReleaseBase()>/connect?install=<installID>&label=<hostname>&platform=<p>&version=<v>&redirect=http%3A%2F%2F127.0.0.1%3A<port>%2Fcallback&challenge=<b64u(sha256(verifier))>&state=<state>`
  in that exact param order (`openBrowser` is handed the same string; a hub whose
  browser will not open shows the url for pasting).
- The one-time loopback `GET /callback` — on `error=access_denied` or state
  mismatch: a "not connected" page, nothing stored; on `code`: POSTs
  `{code, verifier}` to `<site>/api/hub/token` through `siteHTTP()` (no
  redirects, 10 s timeout); on 200 stores the token and answers the browser a
  plain "Orb Manager is connected" page; a 401 answers "that sign-in did not go
  through". The listener dies after the exchange, the denial, or 10 idle minutes.
- `GET /api/site/link-status` → `{"ok":true,"data":{"linked":<bool>}}`, true only
  with a stored token.
- `GET /api/site/designs` → the site's §2.5 list, passed through verbatim as
  `data`, sent with the bearer. A 401 clears the stored token (the §7.2 "next
  call is refused → Disconnected, Reconnect" rule, read here as *this call is
  refused*) and answers `{"ok":false,"error":"disconnected"}`.
- `POST /api/site/disconnect` → best-effort `DELETE /api/hub/token` (its outcome
  never blocks), then forget the token;
  `{"ok":true,"data":{"disconnected":true}}` either way.
- `POST /api/site/import` grows an optional `{"designId":"…"}` body (mutually
  exclusive with `link`): fetches §2.6's own bundle with the bearer, and a 401 on
  *that* fetch clears the link exactly as `designs` does. One-time ticket,
  `ThemeOrigin`, everything else unchanged — an imported own design tags "from
  <site>" and is re-importable by id; it carries no credit because it is yours.

The token lives in `site-link.json`, mode 0600, whole-and-rename, beside
`state.json` in the config dir, path derived from `Dir(s.statePath)` so a test
that sets `statePath` moves it too — `state.json` itself never carries the token
and `TestStateFileHoldsNoSecrets` stays green with the new file existing.
`installID` (G3's 32-hex id) is already minted and stable; `/connect` sends it,
and nothing derives a second identity.

## 6. How a connection ends (§7.2's four ways, site side)

- **Disconnect on `/me`** (site step 4): `POST /api/me/hub/disconnect`
  `{id:"<link id>"}` (session, `requireCaller(req,"*")`) deletes the row. 200
  `{"ok":true}`; an id that isn't the caller's is 404 `{"error":"not_found"}`.
  The hub's next call is refused and the hub clears its token.
- **Disconnect in Orb Manager**: §5's route; the site sees §2.4.
- **90 days unused**: `runSweep` adds `hub_link` rows with `lastSeenAt` (or
  `createdAt` when never seen) older than 90 days, and expired `connect_grant`
  rows. Sweep result gains `{hubs, grants}` counts.
- **Sign out everywhere / delete account**: `/me`'s button revokes every session
  *and* every `hub_link` and pending grant for the user; account deletion rides
  the userId Cascade. Either way both hubs are dead on their next call.

## 7. The gate, as named tests

| Clause | Test |
|---|---|
| link two Orb Managers, see both | site `tests/hub-link.test.ts` "two installs, two rows, both tokens work"; hub `TestSiteConnectEndToEnd` with a fake site holding both links |
| relinking replaces | `tests/hub-link.test.ts` "second exchange, same install: one row, old token answers 401" |
| disconnect from each side, next call refused | `tests/hub-link.test.ts` (site DELETE + `/api/me/hub/disconnect`), hub `TestSiteDisconnectForgetsTheToken`, `TestSiteDesigns401ClearsTheLink` |
| Sign out everywhere ends both | `tests/hub-me.test.ts` (every session gone including this one, hub rows gone, both tokens dead) |
| the door shapes | `tests/hub-routes.test.ts` (new inventory, per-method access, untouched ban list), `tests/hub-connect.test.ts`, `tests/hub-theme-throttle.test.ts` extended for the bearer matrix |

## 8. Owner questions and holds

- **Upload stays hidden.** `#mn-lib-upload` remains in `loadLibThemes`'s hidden
  list; H2 opens reading only. (a) ship H2 with Upload dark — assumed here; (b)
  fold the keep-theme upload in — its own hub+site step, not this slice.
- The `/connect` CSRF posture: `POST /api/connect` sits behind the fence's
  same-origin rule *and* rechecks origin itself. A hostile page can start a
  connect (the challenge is worthless to it — it never sees the loopback
  callback), but cannot complete one for a victim it cannot redirect. Recorded,
  not open.
- `HUB_MAX_KEYS` fail-closed on the token route is accepted: a hub behind busy
  NAT can 429; the grant's 60 s may expire first and the hub's failure page says
  "start Connect again", which is true.

