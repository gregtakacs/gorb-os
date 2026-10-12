# Theme identity: the name lives in the file, never in the filename

> **STALE COPY — read the live one instead.** This design was implemented and
> hardware-verified on orb-A in the sibling clone
> `/home/greg/docker/Projects/gorb-os-2` on 2026-10-12. That clone holds the
> authoritative copy of this file, and `tools/orbtheme.py` there is at `FORMAT = 2`
> / `NAME_MAX = 56`. The text below is the approval snapshot taken before that work
> and still describes the pre-change tree. Do not implement from it. Expect this
> file to be replaced by the next clone merge.
>
> **One gap the live copy does not record: the orb-manager hub.** Its theme client
> and its embedded Theme Editor are still FORMAT 1, and the §8 work order never
> listed `tools/orb-manager/`. That dependency is documented in
> `docs/orb-manager-plan.md`, section "Open — the hub and its Theme Editor are
> FORMAT 1". It is open, not implemented.

**Status: APPROVED — design finalized with owner 2026-10-12, not started.**
Self-contained implementation plan; pick it up cold. This document supersedes
`docs/theme-naming-todo.md`, which is deleted at the end of this work (its §1–§3
describe a defect class this design removes outright).

Repo: `/home/greg/docker/Projects/gorb-os-2`. Build env: ESP-IDF **v5.4** at
`~/esp/esp-idf` — `. $HOME/esp/esp-idf/export.sh && idf.py build`. Flash with
`tools/serflash.sh` (it verifies the `esp_app_desc_t` stamp itself; see
`.clinerules`). Anything past 30 s runs detached and polled — `.clinerules`
records why.

Ground was verified at HEAD `ea5043c` on 2026-10-12. All `file:line` below are
current as of that commit. Re-read a file before editing it if HEAD has moved.

## 1. The owner's rule

Theme naming is not a feature. There is no filename identity at all.

- The theme's one true name is the string the owner set **when they built the
  file**. It lives in the container header.
- The card filename is derived: `sha256(name)` truncated to 12 hex chars, then
  `.orbtheme`. It is opaque hex. No human types it, reads it, or cares about it.
- The original upload filename is meaningless after the bytes land. It carries no
  identity and is not stored.
- Theme management happens in exactly three places: the dial (load one, delete
  one), the device's `/themes` page, and the orb-manager tool over the API.
  Nothing else gets to name, rename, or file a theme.

This kills four defects at once rather than fixing one instance of each: the
24–31-char dead zone, the `filed_as_its_own_slug` refusal class, the FATFS
case-insensitivity ambiguity (todo §3), and the `?name=../` traversal surface.
No user string reaches a path builder, so none of those code paths exist.

## 2. Decisions the owner already made — do not re-open

1. **Hard FORMAT 2, single name field, no legacy read path.** Rejected
   alternatives: a FORMAT 1 fallback reader, and keeping `slug` + `title` as two
   fields with `title` as identity. The owner chose the cleanest end state over
   preserving the four themes on orb-A's card and over preserving the archived
   bytes of the D10 default container. Consequences are in §7.
2. **Duplicate upload offers "Overwrite now".** A name collision with different
   bytes is not a dead end. The default stays a refusal, and one deliberate
   action replaces the existing theme. Full policy in §5.
3. **Upload does not install.** Overwriting the card file of the theme that is on
   the wall must never be blocked, and must never reboot the board. Moving bytes
   to the wall stays one explicit Load tap. §5 states why.

## 3. Verified ground — what keys off the filename today

The three caps that disagree (the todo's headline defect, still live at HEAD):

| Cap | Where | Usable chars |
|---|---|---|
| `ORB_SD_NAME_MAX 31` | `main/sd/orb_sd.h:39` | 31 — what a card filename may be |
| `ORB_THEME_SLUG_MAX 31` | `main/art/orb_theme.h:52`, comment says "== ORB_SD_NAME_MAX: one path segment" | 31 — sizes `orb_theme_entry_t.slug` (`:298`), `orb_theme_geo_t.slug` (`:215`), `s_active_slug`/`s_damage_slug` (`orb_theme.c:620,625`), `s_try_slug` (`:1471`) |
| `NAME_MAX = 24` | `tools/orbtheme.py:46`, written by `_setword` (`:312`) which stores `nbytes-1` | **23** — what the on-wire slug field can hold |

So a card name of 24–31 chars passes `orb_sd_name_ok()` and can never equal the
23-char-max header slug. Such a theme uploads, lists, and cannot install. Under
this design the whole table above stops participating in identity.

The identity rule and its three consumers:

- `filed_as_its_own_slug()` — `main/art/orb_theme.c:342` (doc comment `:336–341`
  already names the cap disagreement as the reason it is the only line that can
  say so). **Delete it and all three calls.**
- `scan_entry()` — `:1591`. Takes `e->slug` from `rec->name`, the FILENAME, at
  `:1598`. Labels the row from `h.title` falling back to `rec->name` at `:1630`.
  Calls the identity rule at `:1632`, and marks `err` non-OK when it fails
  (`:1639`).
- `orb_theme_install()` — `:1347`. Validates the name with `orb_sd_name_ok()` at
  `:1350`, opens by name, applies the identity rule at `:1370`, and folds a
  failure into the same `vv` path as the checksums (`:1377`).
- `orb_theme_upload_end()` — `:1762`. Applies the rule at `:1803`, deliberately
  before `verify_container()` so a mistyped name costs one `strcmp` and not
  seconds of card reads.
- `theme_file()` — `:721`. Builds `/sd/themes/<slug>` via `orb_sd_theme_path()`
  and appends `ORB_THEME_EXT` here, not at the caller, so the client cannot spell
  the suffix.
- The dial already addresses installs by the entry's slug, which is the filename:
  `main/clock/clock_themes.c:159` posts `e->slug`, labels with
  `e->title[0] ? e->title : e->slug` at `:150` and `:245`, and compares
  `e->slug` against `orb_theme_try_slug()`/`orb_theme_damage_slug()` at
  `:286,288`.
- `orb_theme_active_slug()` is set from the **header** slug at boot, `:1068`, and
  the list marks a row active by comparing the header slug to it at `:1624`.
  This is the load-bearing subtlety — see §6.

The upload path (`orb_theme_upload_begin()` `:1700`): builds `u->final` from the
client's `?name=` through `theme_file()` at `:1715`, copies the same argument into
`u->slug` at `:1718`, then hand-builds `u->tmp` by appending `.part` with an
explicit length check (`:1726–1728`). `s_up` is a single global, so one upload
runs at a time (`:1702`).

The SD layer (`main/sd/orb_sd.c`): `orb_sd_name_ok()` at `:311` (card-on) and the
card-off stub at `:351` — two copies, both byte-identical, both lifted verbatim by
`tools/host_tz_test.py`. `orb_sd_theme_path()` at `:327`/`:370`. The header
declares them at `main/sd/orb_sd.h:90,96`. Their doc comments state the reason
they exist: `name` arrives from a browser query parameter and this guard is the
only thing between `?name=../../` and the volume.

The API surface (`main/net/orb_net.c`): routes at `:3228–3234` (`/themes` page,
then `GET /api/v1/sd`, `POST /api/v1/sd/theme|install|default|delete|rescan`).
`h_sd()` at `:2423` carries `static char json[2400]` at `:2438` and sizes the four
name/why escape buffers as `ORB_SD_NAME_MAX * 2 + 3` at `:2446,2452`.
`h_theme_install` `:2500`, `h_theme_default` `:2535`, `h_theme_del` `:2555`,
`h_sd_rescan` `:2582`, `h_theme_upload` `:2600`. Query params are read through the
two-pass helper at `:2279–2290`. The `/themes` page is built from `:2978`, its JS
posts `?name=` at `:3057,3066` and streams the file at `:3072`.

Tools: `tools/orbtheme.py` owns the wire format — `MAGIC`/`FORMAT`/`GEOM_ABI` at
`:41–43`, `HDR_WORDS = 32` at `:45`, `W_SLUG = 14` / `W_TITLE = 20` /
`W_HDR_CRC = 28` at `:82–83` with the field-order comment at `:60–63` that records
a past off-by-one, `pack_header()` `:324`, `write_container()` `:350`,
`read_container()` `:396`. `tools/bake_clock.py` sets both names at `:2375–2376`
(`--theme-slug`/`--theme-title` flags, `:1503`), writes at `:2391`, and stamps
`baked_ms` from `time.time()` at `:2393` — so its output was never
byte-reproducible.

`main/art/orb_theme.c` also writes a header itself: `build_slot_hdr()` `:1177`,
which hand-unpacks the name words at `:1190–1191` and takes the serial from
whatever slot B last held (`:1380–1386`).

## 4. FORMAT 2 — the container changes

The 128-byte header keeps its size. The two name fields collapse into one that
occupies exactly the bytes the pair used, so `HDR_WORDS = 32` and the word at
`W_HDR_CRC = 28` both stay where the tests already pin them.

| | FORMAT 1 (today) | FORMAT 2 |
|---|---|---|
| name fields | `slug` at `W_SLUG = 14`, 24 B (`NAME_MAX`) + `title` at `W_TITLE = 20`, 32 B (`TITLE_MAX`) | one `name` at `W_NAME = 14`, `NAME_MAX = 56` (words 14..27) |
| usable chars | 23 and 31, disagreeing with each other and with the filename cap | 55. One number. No filename budget competes with it |
| `W_HDR_CRC` | 28 | 28 — unchanged: `14 + ((56+3)//4) == 28` |
| `FORMAT` | 1 | 2, mirrored in `tools/orbtheme.py:42` and `orb_theme.c:75` |
| reader gate | `orb_theme.c:260` | same line, same sentence: "theme format this firmware cannot read" |

`GEOM_ABI` stays 1. The geometry blob and the pixel bank do not change, so
`host_sd_test.py`'s blob vectors and the `geom_arena_lint()` from `ea5043c` are
unaffected by the format change itself.

**The hash input rule.** `theme_id(name) = hex(sha256(bytes))[:12]`, where `bytes`
is the name up to its NUL — never the 56-byte padded array. `"Neon"` must hash the
same as `hashlib.sha256(b"Neon")`, so the Python and C sides agree and a name never
picks up trailing zero bytes as identity. An **empty name is refused at parse**:
otherwise every nameless theme hashes to one constant and collides with each other.

**Why 12 hex chars.** 48 bits. The card holds a handful of themes, and a birthday
collision needs ~16M themes. The 12 chars plus `.orbtheme` plus `/sd/themes/` is
well inside `ORB_SD_PATH_MAX 128` and `CONFIG_FATFS_MAX_LFN 64`, so no path or LFN
limit is approached — which is the whole reason the name length can go to 55.

**Device-side hashing.** `main/CMakeLists.txt` `REQUIRES` starts at `:39` and has
no `mbedtls` entry today; nothing in `main/` calls SHA-256 (`grep -rn
mbedtls_sha256 main` is empty). Add `mbedtls` and use
`mbedtls_sha256(..., is224=0)` over the name bytes, then render 6 bytes as 12
lowercase hex chars.

One canonical implementation of `theme_id()` lives in `tools/orbtheme.py`. The C
version is **lifted verbatim** into `tools/host_sd_test.py` (the pattern the arena
work established) and pinned against the Python version, so "the two sides agree
on the hash input" is a green line and not a hope.

## 5. Management paths

**Upload carries no name.** `POST /api/v1/sd/theme` loses `?name=`. Bytes stream
into one fixed temp path — safe because `s_up` already serialises uploads — then
the header is parsed, the name yields the filename, and `rename()` puts it in
place. The final path is never derived from a client string, so
`orb_sd_name_ok()`/`orb_sd_theme_path()` stop being browser-facing. Keep both
functions and their traversal pins as internal defence; `host_tz_test.py` lifts
them and the pins must stay green.

**Install and delete address a device-issued id, resolved by directory walk.**
`GET /api/v1/sd` returns `{"name": <the owner's name>, "id": <12 hex>, ...}` per
row. The buttons post `?id=`. The handler:

1. Validates `id` as exactly 12 chars of `[0-9a-f]` — nothing else. `../`,
   uppercase, 11 or 13 chars, and `/` are all refused before the card is touched.
2. Walks `/sd/themes` (`orb_files_walk`, as `orb_theme_scan` does at `:1672`)
   looking for the file whose name equals that id. It does **not** build a path
   from the id.
3. Re-parses the header at that path and requires `theme_id(header.name)` to equal
   the requested id.

Step 3 replaces the identity rule being deleted and is strictly stronger: a file
swapped under an id cannot install, and no browser string ever reaches a path
builder. `orb_theme_delete()` (`:1555`) and `orb_theme_install()` (`:1347`) take
the id, not a name.

**Duplicate-name policy is three-way**, decided by `geom_crc` + `art_crc` +
`serial`, all fields the header already carries (`orb_thdr_t` at `orb_theme.c:141`,
`serial` read at `:275`):

| Case | Result |
|---|---|
| Same name, all three equal | accepted, idempotent. Re-uploading unchanged bytes never needs confirming |
| Same name, bytes differ, `?overwrite=1` | replace |
| Same name, bytes differ, no flag | refused. The sentence names the conflict and the action: "a theme called X is already on the card and this is different artwork — choose Overwrite to replace it" |

The default refusal is deliberate: an accidental re-post of a stale build must not
silently destroy a working theme. One deliberate click replaces it. This satisfies
D11's "failure stays honest" while giving the forgiving path the owner asked for.

**Overwrite reuses the existing write code and needs no new guarantees.**
`upload_end()` already unlinks then renames (`:1823–1831`), and the comment there
records why: `f_rename()` returns `FR_EXIST` on a live destination, so the pair is
not atomic together, and that fraction-of-a-millisecond window beats renaming over
a live theme and only then discovering a checksum mismatch — which would destroy
the theme that was working. Overwrite inherits that unchanged, because the bytes
fully verify **before** the replace decision is reached. A rejected upload still
`unlink(up->tmp)` (`:1815`).

On conflict the page re-sends the same `File` handle with `?overwrite=1` — one more
WiFi pass, no file re-pick for the owner. Keeping the `.part` alive across the
confirm was rejected: it needs a pending-upload identity plus GC of abandoned
`.part` files, and is not worth the state for one re-send.

**Upload does not install.** D13's install reboots the board. A reboot triggered by
a file upload is not wired without an owner ruling. If the owner later asks for
one action, add `?install=1` as a separate, explicit step.

## 6. The trap this design sets — `active` by name becomes a lie

`scan_entry()` marks a row active with a name comparison:
`e->active = s_active_slug[0] && strcmp(h.slug, s_active_slug) == 0` (`:1624`).
Under FORMAT 2 that comparison is by theme **name**. Overwrite leaves the name
unchanged, so immediately after an overwrite the row reports **active** while the
dial still draws the *old* bytes in flash slot B. Same name, different theme, and
the page would claim they match.

Fix: capture the installed slot's `geom_crc` and `art_crc` beside `s_active_slug`
where boot sets it (`:1068`), and add `bool changed` to `orb_theme_entry_t`
(`orb_theme.h:298`) that is true when the card file's CRCs differ from the
installed copy's. The row then reads "on the dial — this file has changed since;
press Load to update". That is the same honest-state idiom
`orb_theme_delete()` already uses for deleting the installed theme
("(still installed on the dial)", `:1573`). Without it, overwrite reintroduces
exactly the silent no-op D11 exists to prevent.

`orb_theme_entry_t` keeps its `title` field renamed to `name`, drops the
filename-sourced `slug`, and gains `id[12+1]` and `bool changed` — a padding
absorption, so no layout pin breaks. `s_active_slug`, `s_try_slug` and
`s_damage_slug` (all `ORB_THEME_SLUG_MAX + 1` = 32 today) must be re-sized for
55-char names, and the `json[2400]` escape buffers that derive from
`ORB_SD_NAME_MAX * 2 + 3` (`orb_net.c:2446,2452`) must be re-derived from the name
cap. Then `h_sd()`'s `json[2400]` needs a measured re-check — rows grow by `id`
and a longer name. Its own comment (`:2431–2437`) says a truncated body is worse
than an error, so measure it against the live device, do not estimate.

## 7. Consequences the owner accepted — do not treat as bugs

1. **First boot after this upgrade refuses the installed slot.** Slot B holds a
   FORMAT-1 copy that `bank_copy` stamped itself. Boot rejects it at the format
   gate and falls back to the compiled-in pair — D13's ladder working as designed,
   and `damage_*` (`orb_theme.c:625,1311`) puts the reason on the row, so it is not
   a silent reversion. Re-installing any re-baked theme clears it.
2. **The four themes on orb-A's card become unloadable until re-baked** —
   `skeleton`, `diver`, `perpetual-calendar`, `neon` are all FORMAT 1. No
   migration logic is written; the owner re-bakes.
3. **`assets/sources/clock-app-default/clock_app_default.orbtheme` (2,658,351 B)
   is regenerated** and its bytes change. They were already not reproducible —
   `bake_clock.py:2393` stamps `baked_ms` from `time.time()`. Rewrite the
   "byte-identical to the owner's originals" wording in
   `docs/app-platform-plan.md:875` (§12.6) to what is actually true: `DESIGN_ID`
   and `skeleton_theme.zip` are the untouched source of truth, the container is a
   regenerated build product. Nothing in `tools/` or `.github/` reads that
   container today (verified by grep), so no test breaks.
4. **`docs/theme-naming-todo.md` is deleted**, not updated. §1–§2 (the cap
   disagreement) and §3 (FATFS case ambiguity) describe a class that cannot exist
   once filenames carry no identity. §4 cites `orb_theme_stage_load()`/
   `h_theme_load()`, deleted by the D13 squash. §5 cites a `json[2300]` that is
   now `json[2400]` (`orb_net.c:2438`), and its `/tmp/serflash.sh B=` worry is
   superseded by `tools/serflash.sh` + `.orb-target`. Record the outcome in
   `docs/app-platform-plan.md` §11 instead, next to rulings D11–D13.

## 8. Work order

Each step leaves the tree buildable and the 11 host suites green. Do not batch
them into one commit — the battery is the safety net between steps.

1. `tools/orbtheme.py` — `FORMAT = 2`, `NAME_MAX = 56`, `W_NAME = 14`, single
   `name` in `pack_header()`/`write_container()`/`read_container()` (`:324`,
   `:350`, `:396`), plus the canonical `theme_id()`. Update the `:60–63`
   field-order comment and the file's top doc block. `main/art/orb_theme.c:7`
   records that `tools/orbtheme.py` DEFINES the wire format and the C side
   mirrors it, so the Python edit is normative.
2. `main/art/orb_theme.{h,c}` — `orb_thdr_t` single `name[56+1]`;
   `THEME_FORMAT 2`; `hdr_parse` reads one field; empty-name refusal;
   `theme_id()`; id resolution by directory walk; delete
   `filed_as_its_own_slug()` and its three calls; duplicate policy in
   `upload_end()`; `s_active_crc[2]` and `changed`; `build_slot_hdr()` (`:1177`,
   `:1190–1191`) writes the one field.
3. `main/sd/orb_sd.{c,h}` — add an id-shaped path helper for the theme layer;
   keep `orb_sd_name_ok()`/`orb_sd_theme_path()` and both stub copies (`:351`,
   `:370`) in sync, since `host_tz_test.py` lifts the first occurrence and
   `run_mutations()` documents that duplication (`:1396–1401`).
4. `main/net/orb_net.c` — drop `?name=` from `h_theme_upload` (`:2600`), add
   `?overwrite=1`, switch install/delete (`:2500`, `:2555`) to `?id=`, emit
   `{name,id,changed}` in `h_sd()`, re-measure `json[2400]`, and update the page
   JS (`:3041–3072`) with the Overwrite banner and the changed hint.
5. `main/clock/clock_themes.c` — label rows from `name` (`:150`, `:245`), post the
   `id` (`:159`), match `try`/`damage` state on id (`:286`, `:288`), show the
   changed hint.
6. `tools/bake_clock.py` — one name from `--theme-name`/`theme.get("name")`
   (`:2375–2376`), drop `--theme-slug`/`--theme-title`, pass through
   `write_container()` at `:2391`.
7. Tests: vectors and mutants (§9).
8. Build, then the LAN headroom measure.
9. Docs (§7 items 3–4).
10. Re-bake the four themes, regenerate the D10 container.

## 9. Verification ladder

- `tools/host_sd_test.py` stays the home for this: it already compiles
  `orb_thdr_t`, `hdr_parse` and the parser, and owns the `"th"` battery (40 of the
  51 mutation vectors). It is green at HEAD `ea5043c`: 114 `ok` lines, rc=0, no
  survivor reported.
- New vectors: C `theme_id()` lifted and pinned against Python
  `orbtheme.theme_id()` on known-answer rows — ASCII, UTF-8, empty (refused), the
  55-char boundary, and one row that discriminates padded-array from
  NUL-terminated input. Then id-form refusals (`../`, uppercase, 11/13 chars,
  `/`), duplicate accept/refuse/overwrite in both directions, rename target
  derived from the header not the request, and the CRC-drift `changed` case.
- The 4 mutants that pinned `filed_as_its_own_slug()` are **replaced, not
  dropped**. New `"th"` mutants pin the hash input, the hex conversion, the id
  charset check, the post-lookup re-verify, and the duplicate tuple. Target: at
  least 51 vectors, all killed. `run_mutations()` fails on a survivor (`:1413`)
  and on a stale anchor (`:1402`), so a dropped anchor cannot hide.
- Then: all 11 suites, `idf.py build` clean, and the `json[2400]` headroom read
  against the live device.

## 10. Bench state at hand-off

- HEAD `ea5043c`; working tree clean; all 11 host suites green; `idf.py build`
  rc=0.
- Board orb-A is alive on the network at **192.168.2.32** — usable for reads
  (`/api/v1/status`, `GET /api/v1/sd` for the headroom measure) without USB.
- Physical reflash is **blocked** on a Windows-side USB re-attach
  (`usbipd attach --wsl`). The board reports `fw":"fc74aa9"`, a cosmetic stamp
  mismatch only; the flashed image is bit-identical to HEAD content.
- Tag `pre-squash-backup` is retained as the history-rewrite safety net. Delete it
  once the owner is satisfied — do not delete it as part of this work.
- Four pre-existing LVGL deprecation warnings in
  `main/app/orb_carousel.c:83,92,125,126` (`lv_obj_add_flag`/`lv_obj_remove_flag`)
  are not this plan's business. Separate commit if touched at all.
- M3 is blocked on P4 (mechanism B unimplemented, no banked apps, no second
  partition in `partitions.csv`). Unrelated to this plan.

## Appendix: the 11 host suites

`host_app_registry_test.py`, `host_art_residency_test.py`, `host_bake_comp_test.py`,
`host_carousel_test.py`, `host_comp_math_test.py`, `host_files_test.py`,
`host_idle_dim_test.py`, `host_knob_test.py`, `host_menu_test.py`,
`host_sd_test.py`, `host_tz_test.py`.



