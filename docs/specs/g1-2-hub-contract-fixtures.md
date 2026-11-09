# G1 step 2: hub contract fixtures, recorded from the hub's real handlers

## Goal

Slice G1 of the orb-os-net design (§12), second step: gorb-os owns the contract between an
editor and whatever serves its library (§6.4). A hub test drives the hub's **real** Go
handlers (the same `s.routes(mux)` the binary serves) through one scripted scenario and
records every request and response as a committed fixture file. The same test fails when
the handlers' answers change and the fixture was not regenerated, so a change to an
endpoint the editor uses is always a fixture change. A website that mirrors the hub
(orb-os-net) replays the file against its own endpoints; that replay is not part of this
repo.

## Read first

- `/home/greg/docker/Projects/orb-os-net/docs/design.md` (read-only mount): §0 ruling 7,
  §6.3 (the table: which endpoints a hosted editor calls), §6.4.
- `tools/orb-manager/designs.go`, `libentries.go`, `shelfkeep.go` (`keepTheme`),
  `theme.go` `handleThemeDesign` (the `?name=` shelf branch only), `firmware.go`
  `jsonOK`/`jsonErr`: the handlers being recorded and their envelopes.
- `tools/orb-manager/designs_test.go`: `rawCall`, `libHub`, `designDoc`, `withDesign`,
  `orbfake.Container`. **Reuse these helpers**; do not copy them.
- `tools/orb-manager/web/runtime/theme@1/session.js`: every `OrbTheme.hostUrl('/…')` call
  (added in G1 step 1). These are the paths the fixture must cover.
- `tools/orb-manager/Makefile`: target style and the `## name:` comment convention.

## Contract

### Files

- `tools/orb-manager/contract_test.go` — the generator and its guard (package `main`).
- `tools/orb-manager/contract/editor-api.json` — the generated fixture, committed.
- `tools/orb-manager/contract/README.md` — a short page (≤ 40 lines): what the file is, who
  replays it, the normalisation rule, how to regenerate it, and that it is generated
  (never edited by hand).
- `tools/orb-manager/Makefile` — new target `contract` (and add it to `.PHONY`):
  `ORB_WRITE_CONTRACT=1 $(GO) test -count=1 -run '^TestEditorAPIContract$$' .`, with a
  `## contract:` comment in the file's style.

### The fixture format (`contract/editor-api.json`)

One JSON object, written with two-space indentation and a trailing newline, with fields in
this order (use structs with ordered fields, not maps, for everything except JSON bodies):

```json
{
  "format": 1,
  "about": "<one sentence: what this is and where it comes from>",
  "generator": "tools/orb-manager/contract_test.go TestEditorAPIContract",
  "volatile": ["at"],
  "steps": [
    {
      "name": "save a draft",
      "request": {
        "method": "POST",
        "path": "/api/design/save",
        "query": [["app", "clock"], ["name", "Big sub"]],
        "contentType": "application/json",
        "body": { "...": "the JSON body as a JSON value" }
      },
      "response": {
        "status": 200,
        "contentType": "application/json",
        "body": { "ok": true, "data": { "...": "..." } }
      }
    }
  ]
}
```

Rules:

- `query` is an ordered list of `[key, value]` pairs, exactly as the request sent them.
  Omit `query` when there is none.
- A request or response body is recorded as `body` (a JSON value) when its content type is
  `application/json` and it parses; as `bodyBase64` (standard base64) for
  `application/octet-stream` (the theme bytes `library/keep` takes); as `bodyText` (a
  string) for anything else, such as the `http.Error` plain-text answers. Exactly one of
  the three, or none for an empty body. `contentType` is the header with any `; charset=`
  parameter kept as the handler wrote it; omit it when the handler set none.
- **Normalisation:** every JSON object member whose key is listed in `volatile` (only
  `"at"`, the millisecond timestamps the hub stamps) has its value replaced by `0` if it is
  a number, everywhere in every recorded body, request or response, at any depth. Nothing
  else is normalised: names, stems, file names and serials in this scenario are
  deterministic, and the test must prove it (below).
- Steps run in order against **one** fresh hub (`libHub`); a replaying server must run them
  in order from an empty library, so the scenario creates everything it later reads through
  the API itself. No step may depend on files the test writes directly to disk.
- The hub's control token header is sent (the hub needs it) but is not recorded.

### The scenario (exact order; `app=clock` unless stated)

1. `GET /api/library/entries?app=clock` → empty list.
2. `POST /api/design/save?app=clock&name=Big sub` with `designDoc("Big sub", "v1.2.16", 8)`.
3. `POST /api/design/save?app=clock&name=Big sub` again with `designDoc("Big sub", "v1.2.16", 12)` (replaces).
4. `GET /api/library/entries?app=clock` → one draft.
5. `GET /api/library/design?app=clock&name=Big sub` → the saved document.
6. `POST /api/library/keep?app=clock` with
   `withDesign(orbfake.Container("Tourbillon"), string(designDoc("Tourbillon", "v1.2.12", 4)))`
   as `application/octet-stream`.
7. `POST /api/design/save?app=clock&name=Tourbillon` with `designDoc("Tourbillon", "v1.2.16", 4)`.
8. `GET /api/library/entries?app=clock` → `Big sub` (draft) and `Tourbillon` (changed).
9. `GET /api/library/design?app=clock&name=Tourbillon` → the draft.
10. `GET /api/library/design?app=clock&name=Tourbillon&from=theme` → the theme's design.
11. `GET /api/theme/design?name=<stem>` where `<stem>` is the `theme.stem` step 8 listed for
    Tourbillon (the shelf branch of the hub's Edit door) → the theme's design.
12. `POST /api/library/keep?app=clock&fold=0` with
    `withDesign(orbfake.Container("Tourbillon"), string(designDoc("Tourbillon", "v1.2.16", 6)))`.
13. `GET /api/library/entries?app=clock` → Tourbillon still `changed`, made with v1.2.16.
14. `POST /api/library/forget?app=clock&name=Tourbillon&what=draft`.
15. `POST /api/library/forget?app=clock&name=Tourbillon&what=all`.
16. `GET /api/library/entries?app=clock` → `Big sub` only.
17. `GET /api/library/entries?app=flights` → empty (apps never see each other's entries).
18. Refusals, each one step: `GET /api/library/entries?app=..%2Fx`;
    `POST /api/design/save?app=flights&name=x` with a clock design;
    `POST /api/design/save?app=clock&name=x` with body `{"format":"zip"}`;
    `POST /api/design/save?app=clock&name=%20` (blank name);
    `GET /api/library/design?app=clock&name=Nope`;
    `POST /api/library/forget?app=clock&name=Big%20sub&what=some`;
    `POST /api/library/keep?app=flights` with a clock theme;
    `GET /api/design/save?app=clock&name=x` (wrong method).

Every step's `name` is a short lowercase phrase saying what it does ("keep a baked theme",
"refuse another app's design", …). Record whatever the handler answers; the test asserts
the answers make sense (below), it does not invent them.

### The test (`contract_test.go`)

- `TestEditorAPIContract`: builds the fixture by running the scenario through
  `s.routes(mux)` of a `libHub` server (through a helper like `rawCall` that also captures
  the response content type), then:
  - with `ORB_WRITE_CONTRACT=1` in the environment, writes `contract/editor-api.json`;
  - otherwise reads the committed file and fails unless it is byte-identical to what it just
    built, with the message
    `contract/editor-api.json is out of date with the hub's handlers: run make -C tools/orb-manager contract and commit the file`
    plus the first differing line number.
  - Either way it asserts the scenario behaved: steps 1–17 answer 200 with `ok: true`
    (or the raw design document for steps 5, 9, 10, 11); step 8 lists exactly the two
    names with states `draft` and `changed`; step 16 lists exactly `Big sub`; every
    refusal in step 18 answers a 4xx, and those with JSON bodies carry `ok: false` and a
    non-empty `error`.
- `TestEditorAPIContractIsDeterministic`: builds the fixture twice, in two fresh hubs, and
  requires the two encodings to be byte-identical (this is what proves `at` is the only
  volatile value).
- `TestEditorAPIContractCoversTheRuntime`: reads
  `web/runtime/theme@1/session.js`, collects every path in `OrbTheme.hostUrl('<path>` (the
  part before `?`), and requires each one, prefixed with `/api`, to be the `path` of at
  least one fixture step, except `/editor/source` (it reports on an orb, and a hosted
  editor never calls it; design §6.3). It fails naming any path that is missing. It also
  fails if it finds fewer than six such paths (so a refactor that hides them from the scan
  cannot pass silently).

Only send's endpoints (`/orb/known`, `/theme/upload`, `/theme/commit`,
`/theme/upload-status`) and `/editor/source` stay out of the fixture: a hosted editor never
calls them (§6.3). Say so in `about` and in the README.

## Acceptance criteria

1. `cd tools/orb-manager && go test -count=1 -run 'EditorAPIContract' -v .` → the three
   tests PASS.
2. `make -C tools/orb-manager contract && git status --porcelain tools/orb-manager/contract`
   → no change (the committed file is what the generator writes).
3. `cd tools/orb-manager && go vet ./... && go test -count=1 ./...` → ok.
4. `python3 -c "import json;d=json.load(open('tools/orb-manager/contract/editor-api.json'));print(len(d['steps']))"`
   → 25.

## Out of scope

- Any change to a handler's behaviour. If a step answers something surprising, record it,
  and say so in your report; do not "fix" the handler.
- The JS runtime, the editors, `fake-hub.js`, the release workflow, `extract-editors`.
- Do not modify `main/`, `components/`, `apps/`, `.github/`, `web/`.

## Notes

- `designDoc`'s third argument is an art length; keep the arts small, as listed, so the
  fixture stays a few tens of KB.
- `encoding/json` escapes `<`, `>`, `&` by default; use an encoder with
  `SetEscapeHTML(false)` so the file reads naturally, and `SetIndent("", "  ")`.
- Commits: plain subject lines in the repo's style (`Hub: ...`), no Co-Authored-By or
  tool-credit trailers.
