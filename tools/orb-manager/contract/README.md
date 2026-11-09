# editor-api.json — what an editor asks a library, and what the hub answers

One JSON document: 25 scripted requests an editor of `theme@1` makes against an app's
library, and the answer the hub's own handlers gave each one, recorded in order from an
empty library. `contract_test.go` writes it by running the scenario through the same
`s.routes(mux)` the binary serves, so the file is not a description of the API, it is the
API's behaviour, copied out.

**Who replays it.** orb-os-net, the site that serves the same paths to a hosted editor
(design §6.3, §6.4): its test suite posts every recorded request to its own endpoint and
fails on any difference in status, content type, or the fields and types of the body. That
makes the hub the one who owns the contract — gorb-os cannot change an endpoint the editor
uses without changing this file, and the site then fails until it follows.

**Normalisation.** Every JSON member named `at` — the millisecond stamps the hub writes
when it saves, and reads back off a file — is recorded as `0`, at any depth, in requests
and responses alike. Nothing else is normalised: names, stems, shelf file names and serials
in this scenario are the hub's own doing and repeat exactly, which
`TestEditorAPIContractIsDeterministic` proves by building the fixture twice, in two fresh
hubs, and requiring the bytes to agree.

**Regenerate it** after changing any handler the scenario touches:

    make -C tools/orb-manager contract

`TestEditorAPIContract` otherwise reads the committed file and fails, naming the first
line where it and the handlers part. `volatile` lists the keys the generator normalises.

**This file is generated. Never edit it by hand** — a hand edit is a difference from the
handlers that the next run wipes out.

The send door's `/api/orb/known`, `/api/theme/upload`, `/api/theme/commit` and
`/api/theme/upload-status`, and `/editor/source`, are deliberately not here: a hosted
editor never calls them (§6.3). A replaying server needs no orb, and no design the test
put on disk by hand — it creates every entry it later reads through these endpoints.
