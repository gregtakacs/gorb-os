# Orb Manager: a Theme library on the home view, and Import from orb-os.net moves there

## Goal

Slice H1b of the orb-os-net design (`~/docker/Projects/orb-os-net/docs/design.md`,
**ruling 32**: *Import from orb-os.net needs no orb. Orb Manager imports a site link by itself,
with no orb connected. It bakes with its own copy of the app's editor and saves the result into
its own theme library. From there the theme can go to an orb as any library entry does, or be
uploaded to the site.*)

H1 put the door in each orb's Themes tab and baked with that orb's editor. Step h1b-1 gave the
hub `/editor/hub/<slug>/` (Orb Manager's own copy of an app's editor, no orb) and put each
theme's `origin` on `GET /api/library`'s theme rows. Step h1b-2 made the editor runtime work on
that page, `?import=<ticket>` included.

This step is the UI. The home view (`#manage`, the Orbs list) has no orb-independent place
today, so it gets one: a **Theme library** section under the Orbs list. It lists the themes in
Orb Manager's library and holds the **only** Import from orb-os.net door. Import fetches the
bundle through the hub (`POST /api/site/import`, unchanged), opens `/editor/hub/<app>/` on the
ticket in the full-window editor pane, and lists the result. The per-orb Themes tab loses its
door; its library rows keep their `from <site>` tag and their Install, which is how a library
theme reaches an orb. Uploading to the site needs Orb Manager linked to an account (H2, not
built), so it appears only as a disabled, labelled button.

## Read first

- `tools/orb-manager/web/index.html`:
  - lines ~25–45: `<main id="manage" class="view">`, `#mn-findcard` (`section.fleet`), `#mn-empty`;
  - lines ~368–413: the Themes card `#mn-themecard`, its toolbar (`#mn-up-btn`, H1's
    `#mn-site-btn`, `#mn-up-state`), H1's `#mn-site-row`, and the editor pane
    `<section id="mn-theme-editor" class="themee" hidden>` with its comment.
- `tools/orb-manager/web/app.css`: lines ~105–130 (`#mn-theme-editor`, `body.editing`), ~178
  (`.fleet`), and H1's `#mn-site-row` rules (~387).
- `tools/orb-manager/web/app.js`:
  - `show(view)` (~line 81), `route()` (~101), `hubFetch` (~39);
  - `openThemeEditor(row, theme)` (~3831), `editorVeilSource` (~3868), the veil's load
    listener `edVeil` (~3895), `themeEditorApp`, `themeEditorBusy`, `closeThemeEditor` (~3925),
    `themeBridge` (~3955);
  - `themeLibraryRows` (~4290) and its `tag(text, cls, tip)` helper: the row markup to imitate;
  - H1's door, `// ---- Import from orb-os.net` (~4723 to ~4836): `siteOrbName`,
    `siteHostOf`, the `#mn-site-*` handlers, `siteImportPress`, `siteImportEnded`,
    `siteImportLanded`, `siteImportClosed`, and `siteImportPending` (~3186).
- `tools/orb-manager/library.go`: `LibraryPayload`, `LibraryFile` (`file`, `slug`, `title`,
  `has_editor`, `app`, and since h1b-1 `origin`).
- `tools/orb-manager/ui_test.go`: `TestSiteImportDoorInThemesTab` (~1717), `cardMarkup`,
  `blockOf`, `blockOfLine`, `readWebFile`, `readAppJS`; the id list near line 57.
- `docs/specs/h1-3-themes-tab-import.md`: the sentences H1 used. Keep the ones reused below.
- `tools/orb-manager/README.md`, the section `### Import from orb-os.net` (~828).

## Contract

### Markup (`index.html`)

1. **Remove** H1's door from the Themes card: the `#mn-site-btn` button and the whole
   `#mn-site-row` div. `#mn-up-btn` and `#mn-up-state` stay as they are.
2. **Move** the editor pane `<section id="mn-theme-editor" class="themee" hidden>…</section>`
   (with its comment) out of `#mn-themecard` to the top level of `<body>`, directly after the
   closing `</section>` of `<section id="orb" class="view">`. Its content is unchanged. It is
   `position: fixed` and fills the window, but a pane inside the hidden `#orb` view can't show
   on the home view; at the top level it serves both views. Add one sentence to its comment:
   it lives outside both views because the home view's Theme library opens it too.
3. **Add** to `<main id="manage">`, after `#mn-empty` and before `</main>`:

```html
  <section id="mn-libthemes" class="fleet">
    <h3>Theme library</h3>
    <div class="row toolbar">
      <button id="mn-site-btn" class="ghost" type="button"
              title="Paste the link of a theme on orb-os.net. Orb Manager fetches its design, bakes it with its own editor for that app under its shared name (“name by @creator”), and keeps it here. No orb and no sign-in are needed.">Import from orb-os.net</button>
      <button id="mn-lib-upload" class="ghost" type="button" disabled
              title="Not yet: uploading a theme to your orb-os.net account needs this Orb Manager connected to it (Connect to orb-os.net, coming in a later release).">Upload to orb-os.net</button>
      <span id="mn-lib-state" class="hint"></span>
    </div>
    <div class="row" id="mn-site-row" hidden>
      <input type="url" id="mn-site-link" placeholder="https://orb-os.net/t/…" spellcheck="false" autocomplete="off" size="44">
      <button id="mn-site-go" type="button">Import</button>
      <span id="mn-site-state" class="hint"></span>
    </div>
    <ul id="mn-lib-list" class="themelist"></ul>
    <p class="hint">The themes Orb Manager keeps. Open an orb and its Themes tab to put one on it.</p>
  </section>
```

### CSS (`app.css`)

H1's `#mn-site-row` rules stay (the ids are the same). Add only what the new section needs to
sit like the Themes card's rows, if anything. A disabled ghost button must look disabled (check
the existing `button:disabled` rule covers `.ghost`; add one rule if not).

### Behaviour (`app.js`)

**The library list.**

1. `async function loadLibThemes()`: `hubFetch("/api/library")`, read `{ok, data}`. Keep
   `data.apps` in a module variable `libApps` (for titles and editors). Render
   `#mn-lib-list` from `data.themes`:
   - sorted by app title, then by `title` (case-insensitive);
   - each row `li.trow` with `data-file="<file>"`: the theme's `title` (else `file`) as the row's
     name, then tags built the way `themeLibraryRows` builds them (through the DOM, `textContent`,
     never `innerHTML` with hub data):
     - the app: `tag(libAppTitle(t.app), "mut", "A theme for the " + libAppTitle(t.app) + " app")`;
     - when `t.origin`: `tag("from " + siteHostOf(t.origin.site), "mut", "Imported from " + t.origin.site + "/t/" + t.origin.designId + " (version " + t.origin.version + ")")`.
   - No buttons on the rows in this step.
   - No themes: one `li.hint` `No themes yet. Import one from orb-os.net, or keep one from an orb’s Themes tab.`
   - `#mn-lib-state`: `` `${n} theme${n === 1 ? "" : "s"}` ``; a failed read →
     `the library could not be read: <message>` and the list left as it was.
2. `libAppTitle(slug)`: the `title` of `libApps`' row with that `slug`, else the slug.
3. Call `loadLibThemes()` once at startup (wherever the page's first loads run), every time
   `show("manage")` makes the home view visible (add the call to `show()` when
   `view === "manage"`), after a landed import, and when the editor pane closes from a library
   open.

**The editor pane, opened from the library.**

4. `let themeEditorFrom = "orb";` beside `themeEditorApp`. `openThemeEditor` sets it to
   `"orb"`; nothing else in it changes (drop H1's `&import=` there: the per-orb pane no longer
   imports).
5. `function openLibraryEditor(slug, ticket)`: opens the same pane (`#mn-theme-editor`, its
   frame, title, state, veil) for Orb Manager's own editor:
   - `themeEditorFrom = "hub"`, `themeEditorApp = slug`;
   - the veil text `Opening Orb Manager’s <libAppTitle(slug)> editor…` (don't call
     `editorVeilSource`, which asks about an orb), `pane.classList.add("loading")`;
   - `frame.src = "/editor/hub/" + encodeURIComponent(slug) + "/?app=" + encodeURIComponent(slug) + "&import=" + encodeURIComponent(ticket)`;
   - the title `<libAppTitle(slug)> theme editor`, the state cleared, `pane.hidden = false`,
     `document.body.classList.add("editing")`.
   - No `/api/theme/warm`, no `?orb=`, no `&label=`.
6. `closeThemeEditor()`: where it re-reads the orb's list (`if (wasOpen && themeIP) loadThemes(themeIP)`),
   do that only when `themeEditorFrom === "orb"`; when it is `"hub"`, call `loadLibThemes()`
   instead. The rest (busy guard, `about:blank`, `siteImportPending`, `siteImportClosed`)
   stays.
7. `themeBridge`: after the existing source/origin checks, when `themeEditorFrom === "hub"`:
   - `theme.update` → ignore (return): there's no orb to update;
   - `theme.save` → `back({ state: "error", message: "this editor has no orb: open an orb’s Themes tab to send a theme" })` and return;
   - `theme.ready`, `theme.close`, `theme.busy`, `theme.imported` → as today.
   `theme.imported` keeps its `m.app !== themeEditorApp` guard.

**The import door, now orb-independent.** Rework H1's door code in place (same function names):

8. `#mn-site-btn` toggles `#mn-site-row` and focuses the field; Enter in the field and
   `#mn-site-go` both call `siteImportPress()`. Unchanged from H1.
9. `siteImportPress()`: **no orb is needed.** Remove the `themeIP` guard and every use of
   `themeAppsRows`, `themeFaceScope`, `themeChooseApp`, `siteOrbName` and `openThemeEditor` from
   it. Keep: the pending guard, the empty-field sentence `paste a theme’s link from orb-os.net first`,
   `go.disabled = true`, `fetching the theme…`, the `/api/site/import` POST with
   `{ link: text }`, the `✗ ` refusal and `✗ the hub did not answer: <message>` lines. Then:
   - if `libApps` is empty, `await loadLibThemes()` first;
   - `const app = libApps.find((a) => a.slug === data.app)`;
   - no row → `✗ “<sharedName>” is a theme for the <libAppTitle(data.app)> app, which Orb Manager doesn’t carry`;
   - a row without `has_editor` → `✗ “<sharedName>” needs the <title> editor, and Orb Manager’s copy of <title> has none`;
   - otherwise state `baking “<sharedName>” in Orb Manager’s <title> editor…` and
     `openLibraryEditor(data.app, data.ticket)`.
   In the two ✗ cases call `siteImportEnded()`.
10. `siteImportLanded(name)`: state
    `✓ “<name>” is in the theme library. Open an orb’s Themes tab to put it on the orb; Close the editor to see it in the list.`;
    clear the field, re-enable the button, and call `loadLibThemes()`.
11. `siteImportClosed()` and `siteImportEnded()`: unchanged.
12. Delete `siteOrbName()` if nothing uses it any more. Keep `siteHostOf`. Keep
    `themeChooseApp` (the app switch still uses it).
13. The Themes tab's library rows keep H1's `from <site>` tag (`themeLibraryRows`), unchanged.

### README

Rewrite `tools/orb-manager/README.md`'s `### Import from orb-os.net` section (same length,
same tone): the door is in the **Theme library** on the home view, under the Orbs list, and
needs no orb; the hub fetches `GET <site>/api/hub/theme/<id>` with no sign-in; Orb Manager's
own copy of the app's editor (`/editor/hub/<app>/`, from the `.orbapp` in its app library)
bakes it under `<name> by @<handle>`; the hub keeps it with the `.origin.json` sidecar; an
orb's Themes tab installs it; **Upload to orb-os.net** is shown disabled until Orb Manager can
be connected to an account; only `https://` links (plain http for 127.0.0.1/localhost).

## Behaviour and edge cases

- With **no orbs at all**, the home view shows the Theme library and the door works end to
  end: paste, Import, the editor fills the window, bakes, `✓`, Close, the row is in the list
  with `from <host>`.
- With orbs, nothing about an orb's Themes tab changes except that its toolbar no longer has
  Import from orb-os.net. Edit, Rebake all, Send, Install, Import a theme behave as before.
- Two quick presses: the second is ignored while `siteImportPending` stands.
- The pane is busy (`theme.busy`): Close is refused as today, for either open.
- The editor refuses (a newer schema, a wrong name): its own status line says why; closing it
  gives `the editor closed without importing: its status line said why`.

## Acceptance criteria

From `tools/orb-manager`, `GOFLAGS=-buildvcs=false`, Go at `/usr/local/go/bin/go`:

1. `node --check web/app.js && node --check web/tabs.js` → no output.
2. `go test -run 'SiteImport|AppJS|ThemeEditor|ThemeRows|LibraryDoor' -v .` passes. Replace
   H1's `TestSiteImportDoorInThemesTab` with `TestSiteImportDoorInTheLibrary` (same file, same
   style), which checks:
   - `#mn-libthemes` is inside `<main id="manage"`, and holds `#mn-site-btn`, `#mn-site-row`
     (hidden), `#mn-site-link`, `#mn-site-go`, `#mn-site-state`, `#mn-lib-list`, and
     `#mn-lib-upload` with `disabled`;
   - `#mn-themecard` contains none of `mn-site-btn`, `mn-site-row`;
   - `#mn-theme-editor` is not inside `#mn-themecard` and not inside `<section id="orb"`;
   - `siteImportPress` posts `{ link: text }` to `"/api/site/import"`, trims, disables its
     button, and contains neither `themeIP` nor `openThemeEditor(`; it calls `openLibraryEditor(`;
   - `openLibraryEditor` builds `"/editor/hub/"` with `"&import="` and doesn't contain `?orb=`;
   - `openThemeEditor` no longer contains `"&import="`;
   - `themeBridge` still gates `theme.imported` on `m.app !== themeEditorApp`, and refuses
     `theme.save` when `themeEditorFrom === "hub"`;
   - `closeThemeEditor` calls `loadLibThemes()` for a hub open and keeps `if (wasImport) siteImportClosed();`;
   - `themeLibraryRows` still has the `th.origin` / `tag("from "` tag, and `loadLibThemes`
     uses `t.origin` and `"from "` too.
   Any existing test that pinned the pane inside the Themes card, or the door in the Themes
   toolbar, is updated to the new places; say which in your report. No other existing test is
   edited.
3. `go vet ./...` and `go test ./...` pass.
4. From `tools/orb-manager/web/runtime/_selftest`: `PY=python3 node headless-test.js` and
   `node headless-gorbhost.test.js` pass.
5. From the repo root: `make check-web` passes.

## Out of scope

- Go code (h1b-1), `session.js`/`send.js`/`host.js` (h1b-2), any app's editor.
- Install, Forget, Edit or Download buttons on the home list's rows. Installing stays in an
  orb's Themes tab.
- Any request to orb-os.net other than the hub's existing `/api/site/import`. No linking, no
  account, no upload: the Upload button stays disabled and has no handler.
- Restyling beyond what the new section needs.
