# G1 step 4: the `orb-web` release file, the shipped-editor smoke suite, and the docs

## Goal

Slice G1 of the orb-os-net design (§12), last step. Every gorb-os release gains one more
asset, `orb-web-<version>.tar.gz`, listed in the release's `sha256sums.txt` like every
other file (§6.1): the editor runtime laid out as the hub serves it, the shelf `.orbapp`
files, and the hub contract fixtures. A local smoke suite opens the **shipped** editors
(clock, flights, weather, radar), unpacked from that file with `orb-manager
extract-editors`, in headless Chromium, under no `GorbHost`, under `{kind: "hub"}` and
under `{kind: "web"}`, and checks that the hub cases behave identically and the web case
never touches `/api/`. The docs catch up, including the out-of-date `editor` row in
`apps/README.md`.

## Read first

- `/home/greg/docker/Projects/orb-os-net/docs/design.md` (read-only mount): §1 (the
  `apps/README.md` sentence), §6.1, §6.3, §6.4, §9.2.
- `.github/workflows/release.yml` in full: the build job's steps and artifact names, and the
  publish job's `pattern: dist-*` download and its `Checksums` step.
- `tools/orb-manager/packaging/dist.sh`: shell style (`set -eu`, `die`, usage lines).
- `tools/orb-manager/Makefile`: `manager`, `release`, `dist`, `contract` targets and
  comment style. Root `Makefile`: `check-editor`, `sdk`.
- `tools/orb-manager/extract.go` (step 3): the two commands and their reports
  (`editors.json`, `runtime.json`). `tools/orb-manager/contract/` (step 2).
- `tools/orb-manager/web/runtime/_selftest/fake-hub.js` and `headless-host.test.js`
  (step 1), and `theme@1/session.js`'s `GorbHost` block.
- `tools/release_notes.sh` lines 70–92; `RELEASING.md` (the file table at the top);
  `BUILDING.md` (§ Automated builds, § Cutting a release); `apps/README.md` (the key table,
  and the `os_min` paragraph near line 155); `apps/sdk/orbsdk.py` `editor_dir` /
  `editor_region` (what the `editor` key really does today).

## Contract

### 1. `tools/orb-manager/packaging/orb-web.sh`

```
usage: orb-web.sh <version> <out dir> <orb-manager binary>
```

Run from anywhere (it locates the package dir like `dist.sh`). It builds
`<out dir>/orb-web-<version>.tar.gz` containing exactly one top folder
`orb-web-<version>/` with:

- `runtime/` — `<binary> export-runtime <stage>/orb-web-<version>/runtime` (so the
  release's own Linux build lays out the runtime, `runtime.json` included);
- `apps/` — a copy of every `tools/orb-manager/apps/*.orbapp`;
- `contract/` — a copy of `tools/orb-manager/contract/` (the fixture and its README);
- `version.txt` — the `<version>` argument and a newline.

Requirements: `set -eu`; refuse (`die`) a missing binary, an existing output file, or an
empty `apps/`; stage in a `mktemp -d` that is removed on exit (`trap`); the archive is
reproducible: `tar --sort=name --mtime='1980-01-01 00:00:00Z' --owner=0 --group=0
--numeric-owner --mode='u+rw,go+r,go-w'` piped through `gzip -n -9`, so two runs on the
same tree produce identical bytes. It prints the archive path and its `sha256sum` line.
GNU tar is assumed (Linux; the release builds it on ubuntu).

### 2. Make targets

- `tools/orb-manager/Makefile`: target `web` (add to `.PHONY`), with a `## web:` comment:
  depends on `manager`, then
  `packaging/orb-web.sh $(VERSION) $(BIN_DIR)/web $(BIN_DIR)/orb-manager` after removing
  any old `$(BIN_DIR)/web/orb-web-$(VERSION).tar.gz`.
- Root `Makefile`: target `web` → `$(MAKE) -C tools/orb-manager web`, and target
  `check-web` (both in `.PHONY`, both with a one-line comment in the header list):
  `check-web` runs `make web`, unpacks the archive into a fresh temp dir, runs
  `tools/orb-manager/bin/orb-manager extract-editors <unpacked>/apps <tmp>/editors`, then
  `$(NODE) tools/orb-manager/web/runtime/_selftest/editors-smoke.js --runtime <unpacked>/runtime --editors <tmp>/editors`,
  and removes the temp dir. `check-web` is **not** added to `check` (it needs Go, a
  Chromium and a few minutes); say so in its comment.

### 3. `.github/workflows/release.yml`

In the `build` job, after the `App SDK` upload step, add (comment in the file's voice):

```yaml
      # The web release (orb-os-net design §6.1): the runtime as the hub serves it, the
      # shelf .orbapp files and the hub's editor API fixtures, for a site that hosts the
      # editors. The Linux build just made lays out the runtime, so it is this release's.
      - name: Web release file
        run: packaging/orb-web.sh '${{ steps.v.outputs.version }}' bin/web bin/release/orb-manager-linux-amd64/orb-manager
      - uses: actions/upload-artifact@v7
        with:
          name: dist-web
          path: tools/orb-manager/bin/web/*.tar.gz
```

Nothing else in the workflow changes: the `publish` job's `pattern: dist-*` already
downloads it into `dist/`, `Checksums` lists it in `sha256sums.txt`, and
`gh release create … dist/*` attaches it.

### 4. `tools/release_notes.sh`

After the `orb-sdk-…` line, one line:
`echo "orb-web-$VERSION.tar.gz holds the theme editors' runtime, the shelf apps and the hub's editor API fixtures, for a website that hosts the editors."`
preceded by an `echo` (blank line), matching the SDK line.

### 5. The smoke suite `tools/orb-manager/web/runtime/_selftest/editors-smoke.js`

Plain node (no npm packages), not named `headless-*.test.js` (the runtime runner must not
pick it up: it needs Chromium and the extracted files).

```
node editors-smoke.js --runtime <dir> --editors <dir> [--chrome <path>] [--only <slug>]
```

- Chromium: `--chrome`, else `$CHROME`, else the first of `chromium`, `chromium-browser`,
  `google-chrome` on `PATH`; none → exit 2 with a sentence.
- Reads `<editors>/editors.json`; every row with status `ok` is an editor to smoke (today:
  clock, flights, radar, weather).
- Starts one `http.createServer` on `127.0.0.1:0` that answers:
  - `GET /runtime/<id>/<file>` from `<runtime>/<id>/<file>` (refuse `..`; 404 otherwise);
  - hub layout: `/editor/192.0.2.7/<slug>/` → the entry file, `/editor/192.0.2.7/<slug>/<file>`
    → `<editors>/<slug>/<file>`;
  - web layout: `/studio/<slug>/` and `/studio/<slug>/<file>`, the same files;
  - `/api/*` and `/studio-api/*`: a `fake-hub.js` instance (one per page load), with
    `/studio-api/` mapped onto `/api/` before it is handed to the fake; plus
    `/api/orb/known` → `{ok:true,data:{ip:null}}` and `/api/theme/stem` →
    `{ok:false,error:'no stem here'}`. A route neither the fake nor these answer is a 404
    `{ok:false,error:'no route'}`;
  - `POST /__smoke` — the probe's report (below);
  - every request is recorded per page load as `METHOD path?query`.
  Content types by extension (`.html`, `.js`, `.css`, `.json`, `.woff2`, `.ttf`, `.png`,
  `.svg`), `application/octet-stream` otherwise.
- The entry document is served with two `<script>` elements inserted right after the
  opening `<head …>` tag (or at the very start when there is none): first
  `window.GorbHost = <json>;` when the case sets one, then the probe. The probe records
  `error` events and `unhandledrejection`s, and 6 s after `load` POSTs to `/__smoke`:
  `{errors: [...], host: (window.OrbTheme && OrbTheme.hostInfo) ? OrbTheme.hostInfo() : null,
  nodes: document.body ? document.body.getElementsByTagName('*').length : 0, title: document.title}`.
- For each editor, three cases, each its own Chromium run
  (`--headless=new --no-sandbox --disable-gpu --no-first-run --user-data-dir=<tmp>
  --virtual-time-budget=20000 --dump-dom <url>`, 120 s wall timeout):
  - `none`: no `GorbHost`, at `/editor/192.0.2.7/<slug>/`;
  - `hub`: `{kind:'hub'}`, same URL;
  - `web`: `{kind:'web', api:'/studio-api', send:false, publish:true, inbox:true}`, at
    `/studio/<slug>/`.
- Checks, printed `PASS`/`FAIL <slug> <case>: <what>` lines in the runtime suites' style,
  then `all green (N checks)` or `N FAILED`, exit 0/1:
  1. the probe reported (the page ran);
  2. `errors` is empty;
  3. `nodes` > 20 (the editor drew something);
  4. for an editor whose `editor.json` runtime lists `theme@1`: `host` is
     `{kind:'hub', api:'/api', send:true, publish:false, inbox:false}` for `none` and
     `hub`, and the web object for `web`;
  5. the sorted, de-duplicated list of `/api/` and `/studio-api/` requests (method and path,
     without the query) is identical for `none` and `hub`;
  6. `web` made no request to `/api/`, made no request containing `editor/source`, and,
     for a `theme@1` editor, asked `GET /studio-api/library/entries` at least once;
  7. no case requested anything outside `/runtime/`, its own editor folder, the API
     prefixes, `/__smoke` and `/favicon.ico` (list any stray paths in the FAIL line).
- `VERBOSE=1` prints every request list.

If a check fails because of a real difference in an editor (not the suite), do not change
the editor: report it under DEVIATIONS with the evidence, and mark the check as a known
failure in the suite only if I tell you to.

### 6. Docs

- `apps/README.md`, the key table: replace the `editor` row's text with what is true today
  (the SDK packs any app's editor): `editor` | the app's theme editor: names a folder of
  the app (`"editor": "editor"`) holding `editor.json` (`entry`, the `runtime` ids it
  needs) and the editor's files. `orbsdk.py` packs that folder into the container's editor
  region, which Orb Manager opens from the Themes page. Mention in a short clause that the
  clock's region is built by `tools/make_clock_region.py`. Check the `os_min` paragraph
  still reads right after the change.
- `RELEASING.md`: add `orb-web-<tag>.tar.gz` to the file table (For: "a website that hosts
  the theme editors: the runtime, the shelf apps, the editor API fixtures"; Signed with:
  "none (the sha256sums cover it)"), and one sentence in "What is in the box" or a new
  short paragraph naming what it holds and `make web` to build it locally.
- `BUILDING.md`: in § Automated builds, add `orb-web` to the release workflow's "Does"
  cell; in the hub section, one line for `make web` and `make check-web`.
- `tools/orb-manager/README.md`: in the **Headless commands** section (step 3), one line
  saying `packaging/orb-web.sh` / `make web` assemble the release file from them.

## Acceptance criteria

1. `make -C tools/orb-manager web` → prints `bin/web/orb-web-<version>.tar.gz` and its
   sha256; `tar -tzf` lists only `orb-web-<version>/` paths with `runtime/runtime.json`,
   `apps/clock.orbapp`, `contract/editor-api.json`, `version.txt`.
2. Running `make -C tools/orb-manager web` twice in a row gives the same sha256.
3. `make check-web` → the smoke suite ends `all green`.
4. `python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/release.yml'))"`
   → no error, if PyYAML is available; otherwise
   `ruby -e 'require "yaml"; YAML.load_file(".github/workflows/release.yml")'` or skip and
   say so.
5. `sh -n tools/orb-manager/packaging/orb-web.sh && sh -n tools/release_notes.sh` → ok.
6. `cd tools/orb-manager && go test -count=1 ./...` → ok, and the runtime and clock node
   suites still pass (`make check-editor` minus the Python lines is fine if a Python
   dependency is missing; say which).

## Out of scope

- Any change to the editors (`apps/*/editor/`), the runtime JS, the hub's Go code (except
  nothing — this step should not need Go changes; if it does, stop and explain).
- `ci.yml` (no new CI job in this step), signing, `dist.sh`.
- Do not modify `main/`, `components/`, `apps/` other than `apps/README.md`.

## Notes

- `VERSION` in the hub Makefile is `git describe --tags --match 'v[0-9]*' --always --dirty`;
  in a worktree with uncommitted changes it ends in `-dirty`, which is fine for local runs.
- Commits: plain subject lines in the repo's style (`Release: ...`, `Hub: ...`), no
  Co-Authored-By or tool-credit trailers.

## Notes from step 3

- The hub answers `GET /editor/<ip>/<slug>/editor.json` with the **entry document**, not the
  manifest (`serveEditor`'s manifest branch). `extract-editors` still writes `editor.json`
  to disk. The smoke server must mirror the hub here: a request for `editor.json` under an
  editor folder answers the entry file. No editor is expected to ask for it.
- In the worker, Go is at `/usr/local/go/bin/go`; put it on `PATH` (`export
  PATH=/usr/local/go/bin:$PATH`) before `make`, since a login shell resets `PATH`.
