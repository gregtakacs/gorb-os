# ring-3: the `color` setting type (§64.2) — declared, palette-stepped on the dial, picked on the hub

> **Read first: `docs/specs/ring-design.md` "Orchestrator amendments (2026-11-11)".** They override this spec where they differ (no per-pixel trig in the stamp; the hub accepts `ring`; mutation verifies by exit status; the whole hub gate; the colour setting turns like an enum).

## Goal

Give app settings a `color` type: an app declares it, the OS stores the value as `0xRRGGBB` in
the frozen C9 **int** store, `orbsdk.py` validates it (default required, `0..0xFFFFFF`), the
dial's settings page steps a fixed palette of named colours (an off-palette stored value shows
"custom"), and the hub's App settings page offers a colour picker. Slice **ring** of
`docs/specs/ring-design.md` §9 (design §9.1 for the 12-colour palette). It is the ring's colour
supply line (the pomodoro will give each state a `color` setting), but nothing here needs the
ring to exist — it is a settings type.

## Read first

- `docs/specs/ring-design.md` §9 (the `color` type) and §9.1 (the palette, its 12 names+values).
- `docs/app-platform-plan.md` §57.2 (the schema table and the "stored as C9 int" gate rule) and
  §57.4 (the dial's knob edit), §64.2.
- `tools/orbapp.py` — `UI_TYPES`/`UI_WIRE`/`SET_TYPES` (lines 201-204), `normalize_settings`
  (the `int`/`number` branch at 562, the choices branches), and `pack_schema`.
- `apps/sdk/orbsdk.py` — `check_manifest`/the settings label loop (128-137) and
  `caps_word`; it calls `orbapp.normalize_settings` for the rest.
- `main/app/orb_setui.c` / `.h` — the `TYPES[]` table (10), the `orb_setui_value` switch (138+),
  the range walk (114-125), and the enum/multi choices. `main/app/orb_svc.h` `orb_set_type_t`
  stays frozen (this is `int` on the wire).
- `tools/orb-manager/web/tabs.js` — `valueText` (1239), `valueRow` (1249) and the `s.type`
  branches; `tools/orb-manager/security.go` already allowlists `/api/v1/apps/settings` GET.
- `tools/host_setui_test.py` (SETTINGS, checks, `MUTATIONS`) and a hub web test to imitate:
  `tools/orb-manager/ui_test.go` / `webui_test.go` (they grep `app.js`/`tabs.js` for what must be
  present) and `jscheck_test.go` (brace balance).

## Contract

- **`tools/orbapp.py`.** Add `"color"` to `UI_TYPES` (line 202, appended after `"secret"` so the
  other indices hold). Set `UI_WIRE["color"] = "int"` (line 203). In `normalize_settings` add a
  branch: a colour is **required** to carry a `default`; the default must be an integer `0 ..
  0xFFFFFF` (a bool or non-integer, or out of range, raises with a sentence naming the id); the
  wire entry is `type="int", min=0, max=0xFFFFFF, default=<that int>`; `e["type"]="color"`, and
  like the other dial-editable types it keeps `show` defaulting to `both`. It MUST NOT appear in
  the frozen `SET_TYPES` (the C9 set stays `(int, enum, toggle, string, secret)`).
- **`apps/sdk/orbsdk.py`.** No new check of its own beyond the existing label check (which already
  runs for every setting): a missing `default` and the range are enforced by
  `orbapp.normalize_settings` (called at 135). Confirm a colour with no `default` and one with a
  `default: "red"` or `0x1_00_00_00` are refused.
- **`main/app/orb_setui.c`/`.h`.** Add `ORB_SETUI_COLOR` to the enum **immediately before**
  `ORB_SETUI_TYPES` and `"color"` to `TYPES[]` at the same index (`ORB_SETUI_TYPES` becomes 9);
  the reader's `for (i…) TYPES[i]` loop then maps it. Add the palette as one table,
  `static const struct { uint32_t rgb; const char *name; } s_palette[12]` exactly as design §9.1.
  In `orb_setui_value`, for a colour print the palette **name** if the stored value matches an
  entry, else the literal `"custom"`. The dial's edit (the same knob path the enum/toggle use in
  `menu_view.c`/`knob_edit`) steps the palette index, and pressing stores that entry's rgb into
  the int setting — never raw hex (design §9, §57.4). Add a `host`-side range so an off-palette
  (hub-set) value reads back as "custom" and is preserved until a dial press moves it onto the
  palette.
- **The hub picker, `tools/orb-manager/web/tabs.js`.** In `valueText` add
  `if (s.type === "color") return "#"+pad6(v)`; in `valueRow` add an `else if (s.type ===
  "color")` branch: an `<input type="color">` seeded from the stored int (as `#rrggbb`) that on
  change writes the int via the existing `write()`/`cfgURL` path (same POST `/api/v1/apps/cfg`),
  and shows a small name-or-"custom" chip using the same 12-colour names. The hub may store any
  `0..0xFFFFFF` (the picker is not palette-restricted). Keep `esc()`/the file brace-balanced.
- **`apps/README.md`.** Add `color` to the settings `type` list, and a line: stored `0xRRGGBB`,
  the dial steps the palette, the hub picks freely, opacity is an `int` `0..100` with `unit:"%"`.

## Behaviour and edge cases

- A `color` setting with no `default` → the build fails ("setting 'x' needs a default").
- `default: 16777216` (`0x1000000`, out of range) or `default: "red"` (not an int) → refused.
- A stored value on the palette (e.g. `0x0FA36B`) shows as `Emerald` on the dial; one off it
  (e.g. `0x123456`, set on the hub) shows as `custom` and is preserved on the dial until pressed.
- `show` defaults to `both` so the colour appears on the dial and the hub; a `show:"hub"` colour
  is dial-hidden like any other hub-only setting (the "More on Orb Manager" row).
- The C9 gate range-checks it as an int `0..0xFFFFFF` — a value outside the range never reaches
  NVS (`orb_svc.c` range path).
- `orbapp.pack_schema` writes the `"color"` type string into the §57 region; the reader maps it
  by `TYPES[i]`, so the id/type/count recheck in `check_schema` still matches the wire int.
## Acceptance criteria (sandbox half; the dial firmware half is `idf.py`-only)

Run all in the sandbox; the dial's on-glass drawing and the knob stepper are `idf.py`-only (the
orchestrator builds/flashes; the `orb_setui.c` value logic is host-covered by extending
`host_setui_test.py`).

1. `python3 tools/host_orbapp_test.py` and `python3 tools/host_setui_test.py --mutations` → pass
   (the writer emits `color` and the reader/format round-trips it; a colour mutant bites).
2. A temporary app.json with `{"id":"ring","type":"color","label":"Ring","default":984407}` packs
   (`python3 apps/sdk/orbsdk.py build <app>` exit 0), and the same file with the `default` removed,
   or `default` `16777216`, or `"red"`, is refused by `orbsdk.py build` (nonzero). (Use a scratch
   app dir under `/tmp`; do not add a shelf app.)
3. `grep -q 'color' tools/orbapp.py && grep -q '"color"' tools/orb-manager/web/tabs.js` → the type
   is wired in the writer and the hub picker is present.
4. `cd tools/orb-manager && go vet ./... && go test -run 'TestWeb|TestUI|TestJsCheck|TestAppSettings' -v .`
   → pass, and the run's log contains a `--- PASS` for a colour-picker assertion (add a small
   `ui_test.go`/`webui_test.go` grep asserting `type === "color"` and `#` + a hex appear in
   `tabs.js`); `jscheck_test.go` still balances the file (no `--- FAIL`).
5. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` and `make check-hub` → exit 0.
6. Firmware-only, orchestrator: `idf.py build` clean; a colour setting shows its palette name (or
   "custom") on the dial and steps the palette by knob.

## Out of scope

The ring itself (ring-1/ring-2) — `color` must not depend on the ring. A new C9 type (`color`
stays an int on the wire). The factory apps' own settings (do not add colour settings to weather/
flights/radar — the pomodoro will). `version.txt`/`os_min`. Do not touch `security.go`
(`/api/v1/apps/settings` GET is already allowlisted) or `SET_TYPES`. Do not add an `int` opacity
setting — that is just an `int` `0..100 unit:"%"` (§57.2), no code change.

## Notes

- Storing a colour as a plain C9 int is the whole point (§64.2, §57.2): it means `color` adds no
  storage type, no wire type, no NVS quota change (an int costs 4 bytes), and the C9 gate's
  `min/max/default` path already range-checks it. Only `UI_TYPES`/`UI_WIRE`, `orb_setui.c`,
  `tabs.js`, the README and the docs change.
- The dial palette and the hub chip must name the 12 colours identically (design §9.1); keep them
  as the single source of truth in `orb_setui.c`, and the hub copies the names as strings.
- `0xFFFFFF` and `0x000000` are valid defaults (near-white, black); the check is inclusive.
- Add the colour-picker assertion to the hub test in the **existing** table-grep style (the
  `ui_test.go`/`webui_test.go` `for _, want := range []string{…}` idiom) — do not invent a new
  test harness.
