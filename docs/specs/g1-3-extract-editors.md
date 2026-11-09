# G1 step 3: `orb-manager extract-editors` and `orb-manager export-runtime`

## Goal

Slice G1 of the orb-os-net design (§12), third step. A website that hosts app editors must
serve exactly what Orb Manager serves, without a second implementation of the `.orbapp`
format (ruling 15). So the hub binary gets two headless commands:

- `orb-manager extract-editors <dir-or-file> <out>` unpacks the editor region of every
  `.orbapp` it is given, using the hub's own container reader and the hub's own editor
  rules (entry resolution, runtime check, per-file limit), writes each app's editor files
  under `<out>/<slug>/`, and writes a machine-readable report `<out>/editors.json` saying,
  per file, whether its editor is servable and why not.
- `orb-manager export-runtime <out>` writes the editor runtime laid out exactly as the hub
  serves it at `/runtime/<id>/<file>`, for the ids this binary carries, plus a report.

Tests prove the extracted and exported bytes are byte-for-byte what the hub's own handlers
serve, and that a hostile archive cannot write outside `<out>`.

## Read first

- `/home/greg/docker/Projects/orb-os-net/docs/design.md` (read-only mount): §0 rulings 8
  and 15, §6.1 (steps 2–4), §6.2's table (the checks a site makes with this command).
- `tools/orb-manager/editor.go` in full: `serveEditor`, `editorBytes`, `editorMember`,
  `rawEntryName`, `isHTMLDoc`, `editorMaxEntry`, `editorManifestName`.
- `tools/orb-manager/runtime.go` in full: `runtimeCatalog`, `runtimeHas`, `runtimeIDs`,
  `serveRuntime`, `editorRuntimeMissing`, `runtimeRefused` (its sentence).
- `tools/orb-manager/internal/orbnet/appcontainer.go`: `ParseApp`, `AppEditorRegion`,
  `AppContainer.CapNames`.
- `tools/orb-manager/library.go`: `appOSMin`.
- `tools/orb-manager/main.go` `main()` (the first ~60 lines) and `var version`.
- `tools/orb-manager/editor_test.go`: `zipWith`, `editorHub`, `editorGet` — reuse them.
- `tools/orb-manager/apps/*.orbapp`: the shipped shelf (clock, color_picker, flights,
  radar, weather).

## Contract

### Dispatch (`main.go`)

At the very top of `main()`, before any flag parsing, log redirection, instance lock or
network, call one function from the new file:

```go
if code, ok := runSubcommand(os.Args[1:], os.Stdout, os.Stderr); ok {
	os.Exit(code)
}
```

`runSubcommand(args []string, stdout, stderr io.Writer) (code int, handled bool)` handles
`extract-editors` and `export-runtime` and returns `handled == false` for anything else
(so every existing flag keeps working; a first argument starting with `-` is never a
subcommand). Exit codes: `0` the run completed (rows in the report may still be skipped),
`1` an input or output problem (unreadable input dir, `<out>` not empty, a write that
failed), `2` usage (wrong argument count; the usage line goes to stderr).

Usage lines, verbatim:

```
usage: orb-manager extract-editors <dir of .orbapp files | file.orbapp> <out dir>
usage: orb-manager export-runtime <out dir>
```

### Files

- `tools/orb-manager/extract.go`: `runSubcommand`, both commands.
- `tools/orb-manager/extract_test.go`: the tests below.
- `tools/orb-manager/runtime.go`: factor the sentence `runtimeRefused` builds into
  `func runtimeMissingSentence(slug string, missing []string) string` and use it from
  both places. No other change to `runtime.go`'s behaviour.
- `tools/orb-manager/README.md`: a short section **Headless commands** (≤ 25 lines) with
  the two usage lines, what each writes, the exit codes, and that orb-os-net's release pull
  runs them (no more about the site than that).
- `tools/orb-manager/Makefile`: nothing required. Do not add targets in this step.

### `extract-editors <in> <out>`

- `<in>` is a directory (its regular files whose name ends in `.orbapp`, not recursive,
  sorted by name) or one file (any name). A directory with no `.orbapp` file is exit 1:
  `no .orbapp files in <in>`.
- `<out>` must not exist, or be an empty directory; otherwise exit 1:
  `<out> is not empty: extract-editors writes into a fresh directory`. It is created
  with `0o755`.
- All writes go through `os.OpenRoot(<out>)` (an `*os.Root`), never through paths joined by
  hand, so nothing can land outside `<out>` whatever a member is called.
- Per input file, one row in the report, in input order:

```go
type extractRow struct {
	File    string         `json:"file"`              // base name of the input file
	SHA256  string         `json:"sha256"`            // of the whole input file, lowercase hex
	Bytes   int            `json:"bytes"`
	Status  string         `json:"status"`            // "ok" | "skipped" | "no-editor" | "unreadable"
	Reason  string         `json:"reason,omitempty"`  // why, for every status but "ok"
	Slug    string         `json:"slug,omitempty"`
	Title   string         `json:"title,omitempty"`
	Version string         `json:"version,omitempty"`
	Caps    []string       `json:"caps,omitempty"`    // AppContainer.CapNames()
	OSMin   string         `json:"os_min,omitempty"`  // appOSMin
	Editor  *extractEditor `json:"editor,omitempty"`  // only when status is "ok"
}
type extractEditor struct {
	Dir     string        `json:"dir"`     // the slug: files are at <out>/<dir>/<name>
	Entry   string        `json:"entry"`   // the document the hub opens for /editor/<ip>/<slug>/
	Runtime []string      `json:"runtime"` // editor.json's runtime ids, as listed ([] when none)
	Files   []extractFile `json:"files"`   // every member written, in archive order
}
type extractFile struct {
	Name   string `json:"name"`
	Bytes  int    `json:"bytes"`
	SHA256 string `json:"sha256"`
}
```

- The report `<out>/editors.json`:
  `{"format": 1, "hub": <main.version>, "runtime": runtimeIDs(), "apps": [rows]}`, two-space
  indent, `SetEscapeHTML(false)`, trailing newline. It is written last, after every app.
- Rules, in order, for one input file:
  1. Read error, or `orbnet.ParseApp` refuses it → `unreadable`, reason = the error text
     with any leading `orbnet: ` removed. No slug etc.
  2. Another file earlier in this run already had the same slug → `skipped`, reason
     `<file> is also the <slug> app; only the first is extracted`. Header fields filled.
  3. No editor region (`EditorSize == 0`) → `no-editor`, reason `this app ships no editor`.
  4. `orbnet.AppEditorRegion` errors, or the region is not a ZIP → `unreadable`, reason the
     error (same `orbnet: ` trim; for the ZIP case
     `the editor region for <slug> is not a ZIP archive: <err>`, as `editorRegionDialed`
     words it).
  5. Member checks, over every ZIP entry; the first failure makes the row `skipped` and
     **nothing** of that app is written:
     - the name must be a plain relative path: non-empty; no NUL; no `\`; no `:`; not
       starting with `/`; every `/`-separated segment non-empty and neither `.` nor `..`.
       A name ending in `/` that is a directory entry is ignored (it carries no bytes) but
       its name is still checked. Reason:
       `the editor carries a member named %q, which is not a plain relative path`.
     - the entry is a regular file (`f.Mode().Type() == 0`, or a directory entry as above);
       a symlink or any other type → `the editor member %q is not a plain file`.
     - no two members with the same name → `the editor carries %q twice`; no two names that
       are equal under `strings.ToLower` → `the editor members %q and %q differ only in case`.
     - no member name is also a directory prefix of another (`a` and `a/b`) →
       `the editor member %q is both a file and a folder`.
     - each member's bytes are read with the hub's `editorBytes` (its per-file limit); an
       error is the reason as `editorBytes` words it.
  6. The entry, exactly as `serveEditor` resolves a bare `/editor/<ip>/<slug>/`: with an
     `editor.json` member, `rawEntryName`; an empty answer → `skipped`,
     `editor.json names no entry the hub can open`. With no `editor.json`, `index.html`.
     The entry must be a member → else `skipped`, `the editor has no <entry>`.
  7. Runtime: with an `editor.json`, `editorRuntimeMissing`; any id missing → `skipped`,
     reason `runtimeMissingSentence(slug, missing)` (the hub's own refusal sentence).
  8. Otherwise write every non-directory member to `<slug>/<name>` (creating
     sub-folders with `0o755`, files `0o644`, `O_EXCL`), then the row is `ok`.
     Write each app into `.<slug>.partial` first and rename it to `<slug>` when complete
     (`Root.Rename`), so a failure never leaves a half app under its real name. A write
     error is fatal: exit 1 with the error, leaving whatever partial folders exist.
- stdout, one line per row: `%-10s %s` of status and
  `<slug> <version> (<n> files)` for `ok`, `<file>: <reason>` otherwise. Nothing else on
  stdout. Errors on stderr, prefixed `orb-manager: `.

Keep the per-archive logic (steps 4–8) in a function that takes the region bytes and the
`*os.Root`, so the tests can feed it crafted ZIPs directly without building containers.

### `export-runtime <out>`

- Same `<out>` rule and wording as above (`export-runtime writes into a fresh directory`),
  and `os.OpenRoot` for every write.
- For each id in `runtimeIDs()` (sorted): the files the hub would serve for it from the
  embedded `webFS`: for an entry with no `files`, every regular file under
  `web/runtime/<dir>/` at any depth; for an entry with `files`, those files plus every
  regular file under `<dir>/<prefix>` when `prefix` is set. Each is written to
  `<out>/<id>/<rel>` with its exact bytes.
- Report `<out>/runtime.json`:
  `{"format": 1, "hub": <version>, "ids": [{"id", "vendor", "files": [{"name","bytes","sha256"}]}]}`
  (ids sorted, files sorted by name), same encoding rules as `editors.json`.
- stdout: one line per id, `<id> <n files> <total bytes> B`.

## Tests (`extract_test.go`)

1. `TestExtractEditorsMatchesWhatTheHubServes`: run `extract-editors` (through
   `runSubcommand`, exit 0) on `apps/` into a temp dir. Then for every row: `clock`,
   `flights`, `weather` and `radar` must be `ok`, `color_picker` is whatever the shipped
   file truthfully is (assert the status it has today and say which in a comment). For each
   `ok` app: build `editorHub(t, ip, region, nil)` with the app's real region
   (`orbnet.AppEditorRegion` of the shipped file), and for **every** listed file GET
   `/editor/<ip>/<slug>/<url-escaped name>` with the token: 200 and a body byte-identical to
   the extracted file; GET `/editor/<ip>/<slug>/` answers the extracted `entry` file's
   bytes; the number of files equals the number of non-directory members in the region;
   every row's `sha256`/`bytes` match the file on disk.
2. `TestExportRuntimeMatchesWhatTheHubServes`: run `export-runtime`; for every file in
   `runtime.json`, GET `/runtime/<id>/<name>` from `newHub()` through `s.routes` with the
   token: 200 and identical bytes; and the ids equal `runtimeIDs()`; and walking `<out>`
   finds exactly the files the report lists plus `runtime.json` (nothing else).
3. `TestExtractEditorsRefusesUnsafeArchives`: table-driven over crafted regions, each fed to
   the per-archive function with a fresh `*os.Root` on a temp `out` that sits inside a
   temp `parent` dir: names `../x`, `/abs`, `a/../../b`, `a\b`, `C:/x`, `a//b`, `./a`,
   `a/./b`, a NUL name, a symlink entry (`FileHeader.SetMode(fs.ModeSymlink|0o777)`), a
   duplicate name, a case clash (`A.js`, `a.js`), `a` and `a/b`, a member of
   `editorMaxEntry+1` bytes, `editor.json` with no entry, `editor.json` naming a missing
   entry, a runtime id this hub lacks (`nope@9`), no `editor.json` and no `index.html`.
   Each is `skipped` with the reason the contract gives, and afterwards `parent` holds only
   `out`, and `out` holds nothing (no partial folder left either).
4. `TestExtractEditorsRows`: a garbage file and a truncated copy of a shipped `.orbapp` are
   `unreadable`; two copies of `flights.orbapp` in one dir → the second is `skipped` with
   the duplicate reason; a non-empty `<out>` is exit 1 and writes nothing; a wrong argument
   count is exit 2 with the usage line on stderr; `runSubcommand([]string{"-addr", "x"}, …)`
   and `runSubcommand(nil, …)` return `handled == false`.

## Acceptance criteria

1. `cd tools/orb-manager && go test -count=1 -run 'Extract|ExportRuntime' -v .` → all PASS.
2. `cd tools/orb-manager && go build -o /tmp/om . && rm -rf /tmp/ed /tmp/rt && /tmp/om extract-editors apps /tmp/ed && /tmp/om export-runtime /tmp/rt && ls /tmp/ed /tmp/rt`
   → exit 0; `/tmp/ed` holds `clock flights radar weather editors.json` (and nothing for
   color_picker unless it ships an editor); `/tmp/rt` holds one folder per runtime id and
   `runtime.json`.
3. `cd tools/orb-manager && go vet ./... && make crosscheck && go test -count=1 ./...` → ok.

## Out of scope

- Any change to how the hub serves editors or the runtime (`serveEditor`, `serveRuntime`)
  beyond the one sentence helper.
- The orb-web release file, the release workflow, the JS runtime, the editors, `apps/`.
- Do not modify `main/`, `components/`, `apps/`, `.github/`, `web/`, `internal/orbnet`.

## Notes

- `os.Root` (`OpenRoot`, `Root.MkdirAll`, `Root.OpenFile`, `Root.Rename`,
  `Root.RemoveAll`) is in the Go this module requires (`go.mod`: 1.27).
- Do not log through `log` in the commands; write to the given `stdout`/`stderr` so the
  tests can read them.
- Commits: plain subject lines in the repo's style (`Hub: ...`), no Co-Authored-By or
  tool-credit trailers.
