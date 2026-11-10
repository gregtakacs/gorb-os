# Themes tab: Import from orb-os.net, and the "from <site>" tag on library rows

## Goal

Slice H1 of the orb-os-net design (`~/docker/Projects/orb-os-net/docs/design.md` §7.1), the
last step. Step h1-1 gave the hub `POST /api/site/import` (paste a link, get a ticket for the
fetched design bundle). Step h1-2 taught the editor runtime to bake a ticket's bundle under
its shared name and keep it in the library (`?import=<ticket>`). This step is the user-facing
door. The Themes tab gets an **Import from orb-os.net** button with a link field. Import
fetches the bundle through the hub, opens that app's editor in the Themes tab's editor pane
on the ticket (as Rebake all opens the editor to bake), and shows the result. Library rows
that came from a site wear a tag naming it.

## Read first

- `tools/orb-manager/web/index.html` lines ~370–400: the Themes tab toolbar (`#mn-up-btn`
  "Import a theme", `#mn-up-state`) and the editor pane (`#mn-theme-editor`, `#mn-theme-editor-frame`).
- `tools/orb-manager/web/app.js`:
  - `openThemeEditor(row, theme)` (~line 3820): how the frame URL is built (`&rebake=1`, `&entry=`).
  - `themeAppsRows` and `themeReopenButton` (~line 3806): how an app's editor row is found
    (`row.has_editor && row.editor_url`).
  - `themeBridge(ev)` (~line 3930): the editor-to-page messages (`theme.ready`, `theme.close`,
    `theme.busy`, `theme.save`) and the origin and source checks.
  - `closeThemeEditor()`: closing re-reads the list (`loadThemes(themeIP)`).
  - `loadThemes(ip)` (~line 3606) and the app switch `#mn-themes-tabs` (the `scope` it picks).
  - `themeLibraryRows` (~line 4270) and its `tag(text, cls, tip)` helper (~line 4335).
  - `$("#mn-up-btn").onclick` (~line 4674): the file import's state-line style (`✓` / `✗`).
  - `hubFetch`: the fetch wrapper that sends the launch token.
- `docs/specs/h1-1-hub-site-import.md`: `/api/site/import`'s request, answer and error sentences.
- `docs/specs/h1-2-session-import.md`: the `theme.imported` message the editor posts.
- `tools/orb-manager/ui_test.go`, `tools/orb-manager/webui_test.go`: how the hub's tests read
  `index.html` and `app.js`. Add the new checks in the same style.

## Contract

### Markup (`index.html`), in the Themes toolbar right after `#mn-up-btn`

```html
<button id="mn-site-btn" class="ghost" type="button"
        title="Paste the link of a theme on orb-os.net. The hub fetches its design, this orb's editor for that app bakes it under its shared name (“name by @creator”), and it lands in the hub library. No sign-in is needed.">Import from orb-os.net</button>
```

and, directly under the toolbar `div.row.toolbar`, a row that starts hidden:

```html
<div class="row" id="mn-site-row" hidden>
  <input type="url" id="mn-site-link" placeholder="https://orb-os.net/t/…" spellcheck="false" autocomplete="off" size="44">
  <button id="mn-site-go" type="button">Import</button>
  <span id="mn-site-state" class="hint"></span>
</div>
```

### Behaviour (`app.js`)

1. `#mn-site-btn` toggles `#mn-site-row` and focuses `#mn-site-link` when it opens.
   Enter in the field does what `#mn-site-go` does.
2. `#mn-site-go`, with the field's trimmed value:
   - Empty → state `paste a theme’s link from orb-os.net first`. Nothing is fetched.
   - Disable the button, state `fetching the theme…`, then
     `hubFetch("/api/site/import", {method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify({link})})`.
   - A transport failure → `✗ the hub did not answer: <message>`. `ok:false` → `✗ ` + `error`.
     Re-enable the button in both cases.
   - Success (`data = {ticket, site, id, version, app, name, sharedName, handle}`): find
     `row = themeAppsRows.find((a) => a.slug === data.app)`.
     - No row → `✗ “<sharedName>” is a theme for the <themeAppTitle(app)> app, which <orb name> doesn’t run`.
     - A row without `has_editor && editor_url` → `✗ “<sharedName>” needs the <title> editor, and <orb name>’s <title> app has none here`.
     - Otherwise: if the Themes tab's chosen app isn't `data.app`, switch the app switch to it
       first (what clicking it in `#mn-themes-tabs` does). Then state
       `baking “<sharedName>” in the <title> editor…` and
       `openThemeEditor(row, { import: data.ticket })`.
     - `<orb name>` is the name the tab already uses for the orb (`current.name`, falling back to `themeIP`).
3. `openThemeEditor`: a `theme.import` value appends `&import=<encodeURIComponent(ticket)>` to the
   frame URL, beside the existing `&rebake=1` / `&entry=` handling. Nothing else changes there.
4. `themeBridge`: handle `m.type === "theme.imported"` (after the existing source and origin
   checks). Accept it only when `m.app === themeEditorApp`. Then:
   - `#mn-site-state` → `✓ “<m.name>” is in the hub library. Install puts it on <orb name>; Close the editor to see it in the list.`
   - clear `#mn-site-link`, re-enable `#mn-site-go`;
   - the editor pane's corner note (`say`) → `imported <m.name>`.
5. Closing the editor without a `theme.imported` (a refusal, or the user closed it) re-enables
   `#mn-site-go` and sets `#mn-site-state` to
   `the editor closed without importing: its status line said why`, unless the state already starts with `✓`.
6. **Library rows** (`themeLibraryRows`): when `e.theme && e.theme.origin`, add
   `tag("from " + <the URL host of origin.site>, "mut", "Imported from " + origin.site + "/t/" + origin.designId + " (version " + origin.version + ")")`
   right after the theme or draft tag. A `site` that `new URL()` can't parse shows `from a site`.

### README

`tools/orb-manager/README.md`: add a short section **Import from orb-os.net** (5–10 lines). It
covers: the button in the Themes tab; that the hub fetches `GET <site>/api/hub/theme/<id>`
with no sign-in; that the orb's own editor for the app bakes the design under
`<name> by @<handle>` and the hub keeps it in its library with a
`<theme file>.origin.json` sidecar `{site, designId, version, importedAt}`; and that only
`https://` links are accepted (plain http only for 127.0.0.1/localhost while testing). Match
the README's existing tone and heading level.

## Behaviour and edge cases

- The Themes tab is per orb, so the import uses the orb's editor for that app (the hub's own
  copy when it holds the orb's release, as Edit does). An older editor on the orb that can't
  read the design's schema refuses in its own status line (h1-2). The tab only reports that
  the editor closed without importing.
- Two quick Imports: the second press is disabled until the first one answers or the editor closes.
- No orb selected or the tab not loaded: `#mn-site-btn` behaves like `#mn-up-btn` does in
  that state. Whatever guard that button uses, use the same one.

## Acceptance criteria

From `tools/orb-manager`, Go at `/usr/local/go/bin/go`:

1. `/usr/local/go/bin/go test -run 'SiteImport|AppJS|ThemeEditor|ThemeRows' -v .` → passes, including a new
   `TestSiteImportDoorInThemesTab` (in `ui_test.go`) that checks:
   `index.html` has `#mn-site-btn`, `#mn-site-row` (hidden), `#mn-site-link` and `#mn-site-go`;
   `app.js` posts to `/api/site/import`, appends `&import=` in `openThemeEditor`, and handles
   `theme.imported` only when its app matches `themeEditorApp`.
2. `node --check web/app.js && node --check web/tabs.js` → no output.
3. `/usr/local/go/bin/go test ./...` → all pass.
4. From the repo root: `cd tools/orb-manager/web/runtime/_selftest && PY=python3 node headless-test.js && node headless-gorbhost.test.js` → pass (nothing in the runtime should have moved).

## Out of scope

- The hub's Go doors (h1-1) and `theme@1/session.js` (h1-2): use them as they are. If one of
  them is wrong for the UI, report `blocked` and say what.
- A gallery or browse tab, a "My orb-os.net" list, update checks, sending to the orb
  automatically after an import. The row's existing Install does that.
- `app.css` restyling beyond what the new row needs to sit like the toolbar row above it.
