# H4 hub fix: "Upload as a new design" sends the picked entry under the typed name

## Goal

`POST /api/site/upload` (`tools/orb-manager/siteupload.go`) takes `{app, name, visibility?,
replaceId?}` and uses `name` twice: to find the local library entry (`lib.entryFold(app, name)`)
and as the name on the site. The dialog's **Upload as a new design** (`web/app.js`,
`siteUploadChosenName` / `siteUploadSend`) puts the typed new name in `name`, so the hub looks for
a local entry of that new name and answers `no “<new name>” in the clock library on this hub`:
the feature cannot work. Separate the two names.

## Read first

`docs/specs/h4-design.md` §6 and §7, `docs/specs/h4-hub-2-upload-doors.md`,
`docs/specs/h4-hub-3-upload-ui.md`; `tools/orb-manager/siteupload.go`, `siteupload_test.go`,
`web/app.js` (the `siteUpload*` block), `ui_test.go` (the H4 UI tests).

## Contract

1. The door's body gains `siteName` (optional). `name` always names the local entry (looked up as
   today). The name sent to the site as `?name=` is `siteName` when present, else the entry's own
   name. `siteName` passes `designName` (the same rule as `name`); a bad one is refused with that
   rule's sentence before anything is dialed.
2. The dialog always sends the picker's entry as `name`. **Upload as a new design** sends the typed
   field as `siteName`. **Replace** sends the clash answer's stored name as `siteName` and its id as
   `replaceId` (as now, but in `siteName`). A plain upload sends no `siteName`.
3. The success line, the link and every existing copy text are unchanged.

## Tests

- Go (`siteupload_test.go`, the fake-site rig): an entry `Aurora` uploaded with
  `siteName: "Aurora 2"` reaches the fake site as `?name=Aurora+2` with Aurora's design bytes; a bad
  `siteName` is refused without a dial; a Replace sends `siteName` = the stored name and `replaceId`.
  Name the test `TestSiteUploadAsNewUsesTheTypedName`.
- UI (`ui_test.go`, however the existing H4 UI tests pin `app.js`): Upload as a new design posts
  `name` = the picked entry and `siteName` = the typed field. Name it
  `TestSiteUploadAsNewSendsBothNames`.

## Out of scope

Everything else; any file outside `tools/orb-manager/siteupload.go`, `siteupload_test.go`,
`web/app.js`, `ui_test.go`, and the two H4 hub specs' wording if you update the contract there.
