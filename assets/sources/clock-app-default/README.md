# Clock app bundled default — archival record, FORMAT 1, not installable

Three files, and only two of them are the source of truth.

| File | What it is |
|---|---|
| `DESIGN_ID` | the Watch Composer Design ID the art came from |
| `skeleton_theme.zip` | the owner's layer kit, byte-faithful, sha256 `95aa5d3a…c5614d38`. **This is the source of truth.** |
| `clock_app_default.orbtheme` | a FORMAT **1** container, archived. Not a theme anyone can install today. |

## Why the container is stale

FORMAT 1 identified a theme by its filename slug plus a separate title, and the
filename did the filing. FORMAT 2 (2026-10-12, `docs/theme-identity-plan.md`) collapsed
that pair into one `name` field inside the header, and made the card filename a pure
function of it: `theme_id(name)` = `sha256(name)[:12]`. There is no migration path and
the readers have no legacy branch:

- `tools/orbtheme.py read_container` refuses the file — `theme format 1; this build reads
  format 2`;
- `main/art/orb_theme.c:262` refuses it at boot and on upload, so it never reaches the
  wall;
- the orb-manager hub refuses it before uploading, naming the format
  (`tools/orb-manager/internal/orbnet/themecontainer.go`).

Its 56-byte name field still holds FORMAT 1's slug, `skeleton`. Reading that as a FORMAT 2
name is a leftover: it is bytes the old writer put there, not an identity this format
would honour.

## Why it was not simply re-baked

Re-baking from `skeleton_theme.zip` produces a different dial. The kit bake renders
`show=[1,1,0,0,0]`; the archived container holds `show=[1,1,0,1,0]` — the c2 enamel
subdial's seconds hand. The flags that produced the archived bytes are not recorded
anywhere, so re-baking would silently ship a dial nobody approved. It stays untouched
until those flags are known.

## What to do with it

Nothing. It is a record of what the clock's default art was. Ruling D10 in
`docs/app-platform-plan.md` §0.2 keeps it **recorded, not wired**: boot still falls back
to the compiled-in `assets/clock_assets.{bin,h}` pair, and nothing in `tools/`, `main/`
or CI reads this container — verified by grep, 2026-10-12.

To get an installable default, re-bake the kit with the intended `show` flags and give
it a name of your own — a FORMAT 2 theme needs one, and the header holds 55 UTF-8 bytes
of it. Do not name it `skeleton` if that theme is already on the card: under FORMAT 2
two containers with the same name hash to the same card file, and the device makes the
replacement an explicit `?overwrite=1` decision rather than an accident.
