# Hub: fetch a site design bundle, hold it as an import ticket, keep the bake with its origin

## Goal

Slice H1 of the orb-os-net design (`~/docker/Projects/orb-os-net/docs/design.md` §7.1, §8.4):
Orb Manager's **Import from orb-os.net**. The user pastes `https://<site>/t/<id>`. The hub
(Go) fetches that design's **design bundle** from `GET <site>/api/hub/theme/<id>`, checks it,
and holds it under a one-time **import ticket**. The app's own editor, opened later by the
Themes tab with `?import=<ticket>` (step h1-2), reads the bundle back from the hub, bakes it
under its shared name, and posts the bake to the hub. The hub files it in the library and
writes an **origin record** `{site, designId, version}` next to it.

This step is the Go side only: three doors, the ticket store, the origin sidecar, the
`origin` field on library entries, and their tests. No JavaScript changes.

## Read first

- `~/docker/Projects/orb-os-net/docs/design.md` §7.1 (import by link) and §8.4 ("The design
  bundle": the JSON shape, the `orb-asset:<sha256>` refs, the `orbAssets` sidecar, 404/410).
- `tools/orb-manager/libentries.go`: `LibTheme`, `LibEntry`, `entries()`, `handleLibKeep`.
  The new keep door copies `handleLibKeep`'s reading and checks.
- `tools/orb-manager/shelfkeep.go`: `keepTheme(body, fold)`. Reuse it; don't write a second filer.
- `tools/orb-manager/library.go` (lines ~570–660): `Drop`, `writableThemesDir`, `writeFileAtomic`.
- `tools/orb-manager/firmware.go` lines 255–286: `jsonOK` / `jsonErr` (the `{ok, data}` /
  `{ok:false, error}` envelope, and jsonErr's status 400).
- `tools/orb-manager/security.go` lines 77–120: `requireControl`. Every new door goes through it.
- `tools/orb-manager/main.go` line ~81 (`type Server struct`) and ~1219 (`s.routesLibraryEntries(mux)`):
  where the routes are registered.
- `tools/orb-manager/designs_test.go`: `designCall`, `rawCall`, `entriesOf`, `withDesign`,
  `designDoc`, `orbfake.Container(name)`. Imitate these in the new test file.
- `tools/orb-manager/internal/orbnet/themes.go`: `ContainerName`, `MaxThemeUpload`, `AppStemOK`.

## Contract

### Files

- New `tools/orb-manager/siteimport.go`: link parsing, the fetch, bundle checks, the ticket store,
  the three handlers, `routesSiteImport(mux)`, `ThemeOrigin` and its sidecar read/write.
- New `tools/orb-manager/siteimport_test.go`.
- `tools/orb-manager/main.go`: one `s.routesSiteImport(mux)` call beside `s.routesLibraryEntries(mux)`,
  plus whatever `Server` fields the store and client need.
- `tools/orb-manager/libentries.go`: `LibTheme.Origin`, filled in `entries()`.
- `tools/orb-manager/library.go`: `Drop("theme", …)` also removes the theme's origin sidecar.

### The link

`parseSiteLink(raw string) (site, id string, err error)`:

- Trim surrounding whitespace. If the text has no `://`, prefix `https://`.
- `url.Parse`. Refuse userinfo. The scheme must be `https`. Plain `http` is allowed only
  when the hostname is `127.0.0.1`, `localhost` or `::1` (local testing), with any port.
- The path must match `^/t/([A-Za-z0-9_-]{22})/?$`. The query and fragment are ignored.
- `site` = `scheme + "://" + host`, with the host lowercased and the port kept if present.
  `id` is the 22-character id.
- Every refusal is the one sentence:
  `paste a theme’s link from orb-os.net, like https://orb-os.net/t/AbCdEfGhIjKlMnOpQrStUv`
  except a remote `http://` link, which gets `the link must start with https://`.

### The fetch

- `GET <site>/api/hub/theme/<id>`, header `Accept: application/json`,
  header `User-Agent: orb-manager/<version>` (the existing `version` var).
- The client is a `Server` field (`siteClient *http.Client`). When it's nil, use a default
  with a 60 s timeout and `CheckRedirect` returning `http.ErrUseLastResponse`, so **no
  redirect is followed**. Tests set their own client or use the default against `httptest`.
- Read at most `siteBundleMax = 32 << 20` bytes. Read one byte more to detect an
  oversized body.

The error sentences are verbatim. `<host>` is the URL's host, with the port if any.

| Case | Sentence |
|---|---|
| transport error | `could not reach <host>: <err>` |
| any 3xx | `<host> sent a sign-in page instead of the theme: the site isn’t open to Orb Manager yet` |
| 404 | `that theme is private, or <host> has no such theme` |
| 410 | `that theme was deleted by its owner` |
| 409 | `<host> can’t share that theme right now: <message>`, where `<message>` is the JSON body's `message` (fall back to `error`, then to `HTTP 409`) |
| any other non-200 | `<host> answered HTTP <code>` |
| body over the cap | `that theme is too big to import (over 32 MB)` |
| 200, not JSON or not a bundle | `<host> did not answer with a theme` |
| a bundle check fails | `the theme from <host> is damaged: <why>` |

### The bundle checks

`checkBundle(raw []byte, id string) (*siteBundle, error)`. `<why>` is in brackets below.

- A JSON object with `format == "orb-design-bundle"` and `bundle == 1`. Anything else is
  "did not answer with a theme" (above), not "damaged".
- `id` equals the link's id [`it names another design`].
- `version` is an integer ≥ 1 [`it has no version`].
- `app` passes `orbnet.AppStemOK` [`it names no app`].
- `sharedName` is a non-empty string of at most 55 bytes of UTF-8 [`it has no shared name`],
  [`its shared name is over 55 bytes`].
- `design` is a JSON object [`it carries no design`]. `assets` is an object of string values,
  possibly empty [`its art is not readable`].
- For each element of `design.orbAssets`, when that array is present: `sha256` matches
  `^[0-9a-f]{64}$`, `assets[sha256]` exists [`an image is missing (<first 12 hex>)`], it
  decodes as standard base64 [`its art is not readable`], and the sha256 of the decoded
  bytes equals the key [`an image came back changed (<first 12 hex>)`].
- `name` and `handle` are kept when they are strings, and are `""` otherwise. They are not
  required.
- The bundle is stored **as received** (the raw bytes). The hub never rewrites it.

### Tickets

- A ticket is 32 lowercase hex characters from `crypto/rand`.
- The store keeps `{site, id, version, app, name, sharedName, handle, raw, at}` per ticket,
  guarded by a mutex. A ticket lives 15 minutes. At most 8 live at once; adding a ninth
  drops the oldest.
- `/api/site/bundle` can read a ticket any number of times while it lives, so an editor
  reload works. A successful `/api/site/keep` **consumes** it.
- An unknown or expired ticket, on either door: `that import has expired: paste the link again`.

### Doors (all `requireControl`)

1. `POST /api/site/import`, JSON body `{"link": "<pasted text>"}`, at most 4 KiB.
   - A method other than POST gets 405 with the body `POST only` (as `handleLibKeep` does).
   - A missing link or a body that isn't JSON gets the link-refusal sentence.
   - Success: `jsonOK` with
     `{"ticket","site","id","version","app","name","sharedName","handle"}`.
   - Log `site import: <site> <id> v<version> → "<sharedName>"`.
2. `GET /api/site/bundle?ticket=<t>` → 200 with the raw bundle bytes,
   `Content-Type: application/json`, `Cache-Control: no-store`.
3. `POST /api/site/keep?ticket=<t>`, body = the baked `.orbtheme` bytes.
   - Body over `orbnet.MaxThemeUpload` → `that theme is over what an orb will accept`.
   - `orbnet.ContainerName` fails → its error with the `orbnet: ` prefix trimmed (as handleLibKeep).
   - `c.Sections[0] != ticket.app` → `that theme is not one of the <app> app’s`.
   - `c.Name != ticket.sharedName` → `the bake is named “<c.Name>”, not “<sharedName>”`.
   - Then `dest, err := s.library().keepTheme(body, false)`. **fold is false**: a draft of
     the same name stays where it is. On error: `the theme could not be kept: <err>`.
   - Then write the origin sidecar (below), consume the ticket, and answer
     `jsonOK {"name": c.Name, "file": filepath.Base(dest), "origin": <ThemeOrigin>}`.
   - A sidecar write failure is `the theme was kept, but where it came from could not be recorded: <err>`
     (still a jsonErr). Leave the kept theme in place.

### The origin sidecar

```go
type ThemeOrigin struct {
	Site       string `json:"site"`       // "https://orb.takacsfamily.com"
	DesignID   string `json:"designId"`   // the 22-char design id
	Version    int    `json:"version"`    // the design version that was baked
	ImportedAt int64  `json:"importedAt"` // Unix milliseconds
}
```

- The sidecar path is `<theme file path> + ".origin.json"`, for example
  `themes/clock-Skeleton by @greg.orbtheme.origin.json`. It is written with
  `writeFileAtomic`, as indented JSON.
- `readThemeOrigin(themePath string) *ThemeOrigin` returns nil when the file is missing,
  isn't JSON, or has an empty `site` or `designId`, or a `version` below 1.
- `LibTheme` gains `Origin *ThemeOrigin \`json:"origin,omitempty"\``. `entries()` fills it from
  each shelf file's sidecar.
- `Library.Drop("theme", stem)` also removes `<the dropped file>.origin.json`, ignoring
  not-exist. That covers forgetting the whole entry (`/api/library/forget?what=all`).
- The other keep paths (the editor's Send, Rebake all's `/api/library/keep`, file import)
  **leave an existing sidecar alone**. A rebake or a Send of an imported theme still came from
  that design. Deciding when an edited copy stops tracking its design belongs to G3 (Keep my version).
- Sidecars are never listed as themes. The listings already filter on `.orbtheme`; a test
  proves it.

## Behaviour and edge cases

- Importing the same design twice gives two tickets; whichever keep lands last wins the
  shelf file and the sidecar. Importing a newer version overwrites the theme and rewrites
  the sidecar with the new version.
- A theme with no sidecar has no `origin` in `/api/library/entries`. The JSON for every
  existing case must be byte-for-byte what it was: `tools/orb-manager/contract/editor-api.json`
  **must not change**.
- The hub doesn't check that it has the app or its editor; the Themes tab does that
  (step h1-3). The hub only checks the slug's grammar.
- Nothing here talks to an orb.

## Acceptance criteria

Run from `tools/orb-manager` with Go at `/usr/local/go/bin/go`:

1. `/usr/local/go/bin/go vet ./...` → clean.
2. `/usr/local/go/bin/go test -run 'SiteImport|SiteLink|SiteBundle|SiteKeep|ThemeOrigin' -v .` → passes, and covers at least:
   - `parseSiteLink`: a good https link (with a trailing slash, a query and a fragment);
     `orb.takacsfamily.com/t/<id>` with no scheme; `http://127.0.0.1:8080/t/<id>` accepted;
     `http://orb-os.net/t/<id>` refused with the https sentence; a 21-character id, `/t/<id>/x`,
     userinfo and `ftp://` all refused.
   - Against an `httptest` site: 200 bundle → ticket; 302 → the sign-in sentence, and the
     redirect target is never requested (count hits on it); 404; 410; 409 with a message;
     500; an HTML 200; an oversized body; a bundle whose art hash is wrong; one with a
     missing asset; `id` mismatch; a shared name of 56 bytes.
   - `GET /api/site/bundle` returns the exact bytes the site served; an unknown ticket is refused.
   - `POST /api/site/keep` with `orbfake.Container("Skeleton by @greg")` for app `clock`: the
     shelf has the theme and its `.origin.json`; `/api/library/entries?app=clock` shows
     `theme.origin` with the site, id and version; the ticket is consumed (a second keep is
     refused). A bake named otherwise, or for another app, is refused and leaves nothing on the shelf.
   - Forgetting the entry (`/api/library/forget?app=clock&name=…&what=all`) removes the sidecar too.
   - A plain `/api/library/keep` of a theme that has a sidecar leaves the sidecar in place.
   - The ticket store: expiry (inject the clock), the cap of 8, and a ninth that drops the oldest.
   - Every new door refuses a request without the launch token (403).
3. `/usr/local/go/bin/go test ./...` → all pass (the whole hub suite, including the contract test).
4. From the repo root: `git diff --stat a2ec9a0 -- tools/orb-manager/contract/` → empty.

## Out of scope

- Any JavaScript: the session (`theme@1`), the Themes tab, `index.html`. Steps h1-2 and h1-3 do those.
- Linking, tokens, inbox, resolve and update checks (H2, H3, G3). Don't add a "check for
  updates" door.
- Don't modify `tools/orb-manager/contract/`, `web/`, `apps/`, or firmware code.

## Notes

- Use the curly apostrophe `’` and curly quotes `“ ”` exactly as written in the sentences.
  The hub's existing copy uses them.
- The hub runs on Windows and macOS too: build the sidecar path with `filepath`, not string concatenation with `/`.
- The 55-byte limit is `THEME_NAME_MAX` (the orb's 56-byte name field less its NUL). If a Go
  constant for it already exists in `internal/orbnet`, use it.
