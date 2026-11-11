# fu2-6: a site that can't answer resolve yet is not "no longer shared"

## Goal

Greg's live H2 test imported his own private design "Neon moon" through My orb-os.net, and the
Updates list at once said "Neon moon is no longer shared on orb-os.net; … will stop checking it".
Cause: `resolveOrigin` (tools/orb-manager/siteimport.go ~618) reads ANY 404/410 from
`GET <site>/api/hub/resolve` as Gone, but that route is slice H3's and the site has not built it,
so the site's generic not-found reads as "unshared". After this step only an answer that says so
in its own JSON body is Gone; anything else is "can't check for updates yet", informational, and
the hub keeps checking. Also, an origin on the site this hub is linked to (H2) is resolved with
that link's bearer.

## Read first

- `tools/orb-manager/siteimport.go`: `resolveAnswer`, `resolveOrigin`.
- `tools/orb-manager/updates.go`: `updateItem` (~65), `themeUpdateRows` (~200), the update
  apply path that refuses on `ans.Gone` (~470), and every other place `Gone` or "stop checking"
  is read or written (grep `Gone`, `Keep`, `choiceFor`, `stop checking` in *.go and web/app.js).
- `tools/orb-manager/sitelink.go`: how the link (site + token, `site-link.json`) is loaded, and
  how every bearer call goes only to the site that issued the token (`revokeSiteToken`, the
  designs list call ~679). Reuse that loader and that site comparison; do not add a second one.
- `tools/orb-manager/web/app.js` ~5260–5330: how a theme row with `gone` is drawn.
- `tools/orb-manager/updates_test.go`, `siteimport_test.go`: how tests stand a fake site.

## Contract

- The Gone answer is defined now (H3 will implement it on the site): HTTP 404 or 410 with
  `Content-Type: application/json` and a JSON object body whose `"gone"` member is `true`, e.g.
  `{"gone":true}`. Only that is Gone.
- Any other 404 or 410 (HTML, empty body, JSON without `"gone": true`, unparseable body) returns
  a new answer `resolveAnswer{NotYet: true}` with no error.
- `themeUpdateRows`: a `NotYet` answer produces a theme row with no choices, `gone` false, and
  `note`: `orb-os.net can't check this theme for updates yet` (use the origin site's host via
  `siteHost(o.Site)` in place of `orb-os.net` when it is not orb-os.net). The web page draws it as
  plain information (no warning colour, no "stop checking" sentence).
- The update apply path on `NotYet` refuses with `<name>: <host> can't check for updates yet`
  and changes nothing.
- Nothing about a `NotYet` (or any non-Gone) answer records a choice, removes the origin
  sidecar, or otherwise stops later checks; the next check asks again. If today any code path
  records a "stop checking" state automatically on Gone, keep that for a real Gone only.
- Bearer: when the hub holds a site link whose site equals the origin's site (the same canonical
  comparison sitelink.go uses), `resolveOrigin` sends `Authorization: Bearer <token>`; for any
  other site it sends no Authorization header. The token never appears in an error, a row, or a
  log line. A 401 from resolve with the bearer is reported like any other error (no unlinking
  here).

## Tests

- `TestResolvePlain404IsNotYet`: a fake site answering resolve with an HTML 404 page → the
  Updates rows (through the real route) show the row with the not-yet note and `gone` false; a
  second check hits the site again (count requests); no choice was recorded.
- `TestResolveRouterJSON404IsNotYet`: 404 with `{"error":"not found"}` → NotYet.
- `TestResolveGoneBodyIsGone`: 404 and 410 each with `{"gone":true}` → Gone row as today.
- `TestResolveSendsBearerOnlyToItsSite`: linked to site A: a resolve to A carries
  `Authorization: Bearer <token>`; a resolve to site B (also allowed, e.g. a second loopback
  port configured as GORB_SITE in another subtest, or orb-os.net through a test transport)
  carries none.
- The existing Gone tests are updated to send the new body.

## Design-doc note (lead does it, not you)

The lead adds a line to orb-os-net's design (H3 row / §7.4) defining the Gone body.

## Acceptance criteria

1. `cd tools/orb-manager && go vet ./... && go test -run 'Resolve|Update|SiteImport|SiteLink' -v .` → all PASS, including the four tests above.
2. `sh docs/specs/s3-gotest.sh` → green.
3. `node --check tools/orb-manager/web/app.js` and `make check-web` → pass.

## Out of scope

The site; H3's resolve route; unlinking on 401; any app's bytes.
