# H4 hub step 1: scopes on the link — the file, the connect URL, the exchange's answer

## Goal

Slice H4, step 1 of 3 on Orb Manager (docs/specs/h4-design.md §6's first paragraph). The hub
carries a scope set on its credential: the connect URL asks for read and write, the exchange
stores what the site **answered**, `link-status` reports it, and a `site-link.json` written
before H4 still loads as the read-only link it always was. No upload door yet — but every
later hub step reads scopes from here, so this step decides what a scope is on this side.

## Read first

- `docs/specs/h4-design.md` §6 ("`site-link.json`"), §1.2 (the wire's grammar and the
  parameter's position), §7's hub rows.
- `tools/orb-manager/sitelink.go` — `siteLink` (line 96), `saveSiteLink`/`loadSiteLink`/
  `loadSiteLinkRaw`/`liveSiteLink`/`disconnectedSiteLink`, `siteConnectURL` (line 270, the
  hand-assembled field list whose order a test pins), `siteTokenExchange` (line 538, whose
  three named returns become four), `siteFinishWithCode` (line 479, the exchange's caller),
  `handleSiteLinkStatus` (line 602, the `linked` branch's field list).
- `tools/orb-manager/sitelink_test.go` — `TestSiteConnectURLIsTheContract` (byte-exact wire
  test to grow), `TestSiteLinkFileRoundTrip`, `TestStateFileHoldsNoSecrets`, the fake site
  rig, and `TestSiteConnectEndToEnd`: this step's tests join these, not a new file.
- `tools/orb-manager/designs.go` and `libentries.go` — only to confirm they own nothing that
  changes here (they don't; step 2 does).

## Contract

- `siteLink` gains `Scopes []string \`json:"scopes,omitempty"\`` — after `Handle`, before
  `Disconnected`. `omitempty` on both sides of the file's life: a read-only link writes no key
  at all, so the file an H4 hub holds looks like an H2 hub's.
- The known set is declared once: `const siteScopeRead = "designs:read"`, `const
  siteScopeWrite = "designs:write"`, and `var siteScopesAsked = []string{siteScopeRead,
  siteScopeWrite}`. `siteScopesAsked` is the connect URL's value joined with a space; nothing
  else asks for a scope.
- `siteConnectURL` inserts `{"scope", strings.Join(siteScopesAsked, " ")}` between the
  `version` and `redirect` fields of the same hand-assembled list, still through
  `url.QueryEscape` — so the wire carries `scope=designs%3Aread+designs%3Awrite` and the pinned
  test says so. No other field moves.
- `siteTokenExchange` grows a fourth result, the scopes: `(token, handle string, scopes
  []string, err error)`. Its answer struct gains `Scopes []string \`json:"scopes"\``; the
  value is then run through `normalizeSiteScopes(scopes)` (new, same file):
  drop unknown words, drop duplicates, sort ascending, and **empty in any form (absent, `null`,
  `[]`, all-unknown) **or a set that does not contain `designs:read`** comes back as
  `[]string{siteScopeRead}`** — the site refuses to mint a write-only link (§10's first
  answer), so a hub that is handed one has been told something it cannot act on, and the
  conservative reading is the one that costs a relink rather than a failed upload. A 200 with a token and a junk
  `scopes` is still a link, and it is a read-only link. The exchange's failure wording does not
  change for any scope answer: the hub never refuses a link because the site gave it less than
  it asked for.
- `siteFinishWithCode` stores `Scopes: scopes` on the row it writes. Saving and loading are
  otherwise untouched, and `siteBearerFor`, `liveSiteLink`, `forgetSiteLink` and the revoke
  path read scopes for no reason and must not start.
- `handleSiteLinkStatus`'s `linked` branch gains `"scopes": <the normalized set>` — always the
  array, never absent, so the page needs no defaulting: a link whose file has no key answers
  `["designs:read"]`. `pending`, `disconnected` and `none` answer no `scopes` key at all, as
  they answer no `handle` when there is none.
- `loadSiteLinkRaw`/`loadSiteLink` gain no defaulting: the **readers** normalize, through one
  helper `siteLinkScopes(l *siteLink) []string` (nil link → `nil`; missing/empty/unknown →
  read-only, in sorted order) that `link-status` and step 2's door both call. The file on disk
  is not repaired by loading it.

## Behaviour and edge cases

- **The wire, byte for byte**: `TestSiteConnectURLIsTheContract` grows its expected string with
  the encoded `scope` field in its new place and still asserts the whole URL against a literal,
  so a field reordering is a failing test rather than a review catch.
- **An H2 file loads unchanged**: write `site-link.json` by hand with no `scopes` key (the
  exact bytes H2's step 1 wrote, taken from that step's round-trip test) and assert
  `loadSiteLink` returns the same token/site/handle as before, `liveSiteLink` is non-nil, and
  `siteLinkScopes` answers read-only. New test:
  `TestSiteLinkFileWithoutScopesStillLoads`.
- **The exchange stores the answer, not the ask** — `TestSiteExchangeScopesAreTheAnswer` with
  the fake site answering each of: `["designs:read","designs:write"]`, `["designs:read"]`,
  absent, `null`, `[]`, `["designs:write"]` alone, `["bogus:all","designs:write",
  "designs:write"]`, and a 200 with no token (still the old failure, and no file written).
  The stored file's `scopes` for each, in order: both, read-only, read-only, read-only,
  read-only, read-only (a write-only answer is the site refusing `designs:read`, and the hub
  reads that as the conservative thing it is), and write+read for the last.
- **A read-only answer does not stop the link**: the fake site answering one scope still
  finishes the connect with the success page and a file on disk. Nothing in this step may make
  a connect fail on scopes.
- **`link-status` never leaks the token**: the new `scopes` field sits beside `site`, `handle`
  and `linkedAt`, and `TestStateFileHoldsNoSecrets` plus the existing status-door tests stay
  green untouched.
- `TestSiteLinkFileRoundTrip` grows a scopes case (both, and the empty/read-only default
  writing no key at all — assert the saved bytes contain no `scopes` substring for read-only).
- Editor-API listener: the site doors are absent there already (`TestSiteLinkRoutesRefuse
  EditorCreds`); nothing new is registered this step, so it stays green with no edit.

## Acceptance criteria

Each command runs and its result is reported.

1. `export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH && cd tools/orb-manager && go vet ./... && go test -run 'TestSite(Connect|Link|Exchange)|TestStateFileHoldsNoSecrets|TestEditorAPI' -count=1 -v . 2>&1 | tee "$DELEGATE_EVIDENCE_DIR/h4-hub-1.log"` → every listed test PASS, no `--- FAIL` line. Covers the grown contract test, the new
   `TestSiteLinkFileWithoutScopesStillLoads`, `TestSiteExchangeScopesAreTheAnswer` and the
   grown round-trip.
2. `grep -q 'scope=designs%3Aread+designs%3Awrite' sitelink_test.go &&
   grep -q '"scope", strings.Join(siteScopesAsked' sitelink.go` → exit 0.
3. `grep -q 'json:"scopes,omitempty"' sitelink.go && grep -q
   'func siteLinkScopes' sitelink.go && grep -q '"scopes"'
   sitelink.go` → exit 0.
4. `git diff --exit-code h4 -- web internal
   state.go designs.go
   libentries.go main.go` → exit 0 (no UI, no door, no new
   file: this step is `sitelink.go` and `sitelink_test.go` only).
5. `cd tools/orb-manager && go test ./... -count=1` → green, the whole hub suite.

## Out of scope

- No `POST /api/site/upload`, no `GET /api/site/local-designs`, no `siteUploadTimeout`, no
  dialog, no button state, no `link-status` field beyond `scopes`. Do not touch
  `tools/orb-manager/web/**`, `main.go`'s route tables, `designs.go`, `libentries.go` or
  `siteimport.go`.
- No scope-editing UI, no "ask again" button, no retry of a connect that returned fewer scopes,
  and no refusing an upload locally for a scope the hub could have checked for free — step 2
  sends the request and reads the answer, and step 3 is the only place that predicts it.

## Notes

- Sorting the set in `normalizeSiteScopes` (rather than preserving the site's order) is what
  makes both sides' tests writable as equality checks; the site sorts on its side too, so the
  two ends agree on the spelling of one consent.
- Keep the exchange's error strings exactly as they are: `siteFinishWithCode`'s wording and
  `TestSiteConnectWrongStateKeepsWaiting`-style tests depend on the failure paths not growing a
  scope clause.
- `siteScopesAsked` is a `var` of the two words rather than a joined string constant, so step 3
  can reuse the words for the read-only sentence without re-spelling them.
- The revoke-on-relink path (`revokeSiteToken`) sends no scopes and needs none: the site's
  `DELETE` is owner-identity-scoped, not consent-scoped.
