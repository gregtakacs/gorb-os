# gorb-os app platform — OS/app split, storage model, measurement protocol

Status: **P1–P3 landed 2026-10-12; P4 and P6 landed through 2026-10-20; P6b landed
2026-10-20 (§28) — the broker (`main/app/orb_svc.c/h`) is the app surface, the grant table
is live, the app platform is on card, and the theme platform is FORMAT 3-only end to end
(P6 CLOSED 2026-10-20, §27). The orb-manager hub speaks the FORMAT 3 store door (§28).
**§31 RULED and SLICED 2026-10-20: the animated-plate contract is the owner's ("sounds
good, start on the baker and the projection") and slices 1-2 are landed — rules 1-6 as
the plate-bank contract, `CLK_BG_MAX 16`, projection, pair mirror, baker, and suites
(§31.1 below); the split, the glass bench, and the composer export landed the same
day (§31.2): 9 of 15 presets bake animated, the bank measured 5–8 delivered fps on
orb-B. Open: the hub send-to-orb round, the comp-table bake, the abi check.** The
phase table below carries the evidence per row.** Written 2026-10-11 at commit
`a911468`; the owner ruled on
every open point the same day (§0.1). P4 may start. Two of the three bench
re-checks on the P3 row passed on orb-A 2026-10-12. The third — the PERMISSIONS
ladder under a real ask — was closed by ruling D19 (§0.8) rather than by the
bench: declare-at-install replaced the ask-prompt, so no shipped caller can
produce one.

Working copy: **`/home/greg/docker/Projects/gorb-os-2`, branch `main`** (owner's
call, 2026-10-11; reconciled onto `main` 2026-10-20, §28). The sibling `../gorb-os`
clone is on the same `main` at the same commit; the two divergent lines — this tree's
FORMAT 2 rip and the sibling's orb-manager hub work — merged clean (file-disjoint) and
`origin/main` was fast-forwarded over both, so every firmware and hub line citation in
this file resolves the same in both trees. The old `dev2` branch, whose five ahead-commits
were already in `origin/main` by patch-id, is deleted (§28). `/tmp/serflash.sh` now
builds from this tree's `build/`.

It answers the three questions the owner asked:

1. Which part of the current clock implementation is the OS, and which is the clock app?
2. Can every enabled app's art sit in one shared store so the knob carousel stays lively?
3. What does running apps "not natively" cost in frames?

On question 2 the ruling is sharper than the original framing. Rotating between
**installed apps whose art is already resident** must not reboot: that is a pointer
swap. **Loading new art** — a theme, an install — does reboot, and the owner accepts
that as the cost of a commit (C1, D9).

Every number is read off a file or a build artifact in this tree, and the source is named with it. Two numbers cannot be given yet; they are marked MEASURE, and producing them is what P1 exists for.

---

## 0. Decision record

| # | Decision | Choice | Why |
|---|---|---|---|
| D1 | App execution model | Declarative packages, interpreted by compiled-in behaviour modules | The pixel path is priced in hardware (section 5): an interpreter there is fatal, an interpreter over decisions is free |
| D2 | Apps ship code | No. The card carries data; a new behaviour kind is a firmware build | `theme_schema.py:23` states the rule already: adding a kind the device does not know is one skipped row in a log line, not a corrupt dial |
| D3 | Escape hatch for app-authored logic | Deferred wasm3 (about 60-100 KB, apps written in C) against the same `orb_svc_t` table — **committed rather than deferred by D20 (§0.9): `.orbapp` carries the wasm3 module, so P7 is required work, not optional** | ESP-IDF 5.4 ships no user-space ELF loader; MicroPython (about 1.5-2 MB) does not fit a 2 MB slot with the image at 1,476,128 B |
| D4 | Native loadable code | Rejected for now | No loader anywhere in `~/esp/esp-idf/components`; it needs relocatable objects, a hand-rolled PIC loader, a pinned ABI, and the S3 gives no isolation for PSRAM data. Its one unique payoff is app-authored per-pixel painting, and D1's painter registry covers that cheaper |
| D5 | Switching an app | No reboot to rotate between resident apps. Reboot stays on the commit path: loading new art, i.e. install | section 2 |
| D6 | Where packages live durably | New `apps` data partition for catalog, manifests and geometry; art banks in three re-sliced `assets` slots; sector-granular, written off the visible path — **the art half is superseded by D20/D21 (§0.9): the partition keeps catalog, manifests, settings and the code module, and the three-slot re-slice is retired** | An erase suspends the cache. `main.c:62-71` exists precisely so nothing erases under a live dial |
| D7 | Where art lives while running | **Mechanism A (map many)** is the default now the page maths works for two flagships; M2/M3 confirm on the board | Both flagships plus the image's own segments cost 142 of 256 DROM pages (`orb_theme.h:48-55`, `build/gorb-os.map`); PSRAM was never the only option, it was the unmapped one |
| D8 | Working copy | `gorb-os-2`, branch `dev2`, at `a911468` | Owner's call. `/tmp/serflash.sh` repointed at this tree so a flash here cannot silently re-send the sibling's image |
| D9 | App installs keep A/B? | **No.** One slot, write, read back and CRC, then write the header, then reboot — **D21 (§0.9) narrows this: the payload that verifies before the first erase is now code, never art, so the half-copy this rule priced is a half-written module** | Owner ruling C1. A/B survives a torn write by keeping the *other* side bootable; the owner accepts that a torn app install costs that app and a re-copy of the card file instead, and section 2 keeps the header-last rule that makes a torn bank read as absent rather than corrupt |

### 0.1 Owner's rulings, 2026-10-11

These are decided. An implementer who disagrees edits this table, not the code.

| Ref | Decision | Ruling |
|---|---|---|
| B1 | Faces per app | **One face per app.** Options are exposed through OS hooks into the app's options menu |
| B2 | Who owns the theme picker | The **clock app** supplies it via the same options-menu hooks; the OS renders, the app decides |
| B3 | Where grants are approved | **On the dial.** The PWA approval screen comes later. Superseded in part by D19 (§0.8): the dial is the revocation surface, and approval moved to install-time disclosure |
| B4 | Where uninstall lives | The **OS options menu**, not inside the app |
| B5 | How many apps | **Cap 8 installed**, hard, no repartition |
| C1 | Art residency, A/B, capacity | **No A/B for apps.** Reboot on load is expected and committed. Provision **two flagship apps**, revisit the rest once those two exist |
| C2 | Fault handling | NVS fault counter; boot into the clock; mark the app `failed` with a reason shown in APPS |
| C3 | Provenance | Trust the card. Reserve a `sig` word at a fixed header offset so signing later does not move `GEOM_ABI` |
| C4 | Compatibility gate | `abi_min`/`abi_max` instead of `min_os_build`; out-of-range refuses with a named reason |
| C5 | Carousel identity | **Text only.** No art tile |
| C6 | Behaviour-kind registry | Compiled into the OS, one table, append-only IDs, never renumbered |
| C7 | Suspended apps | `service()` yes, `tick()` no; refresh on reveal |
| C8 | Uninstall the clock | Not offered. The clock is the permanent homing screen the long-press returns to, replacing the hardcoded `INPUT_SCREEN_CLOCK` |
| C9 | Setting types | `int`, `enum`, `toggle` **and `string`**, frozen now |
| C10 | Card writes | No `sd_write`. Apps persist through NVS inside their quota |
| C11 | Tooling | `tools/orbapp.py` writes the container; upload lands `/sd/apps/<slug>.orbapp.part` then renames, exactly the theme path |
| C12 | Arena exhaustion | Evict least-recently-visible, log it |
| C13 | Vocabulary freeze | **Ratified 2026-10-11.** The whole vocabulary is OS-frozen, none of it is app-extensible: all 4 `KINDS` (window, strip, ring, flare), all 9 `FLAGS` bits, and the 17 `SOURCES` are OS-owned names resolved through one internal table fed by the `time` and `config` capabilities. An app declares which frozen names it uses; it cannot add one. Only `text` needs a config grant (it reads an app's own NVS keys). `battery` and `power_reserve` stay reserved and unimplemented — no power source is wired, so the bake tool must refuse them. The 12 calendar sources plus `none`/`value` are OS-internal to the clock's own theme schema and need no per-app grant at all |

### 0.2 Owner's ruling, 2026-10-12 — the clock app's bundled default art

| Ref | Decision | Ruling |
|---|---|---|
| D10 | Whose art boots the clock | **The OS ships no art.** The clock app carries its own bundled default artwork, ingested from the SD card. **The committed-source half of this ruling is revoked as of 2026-10-12:** the Design ID `WCD5-W1sic2…` (full string preserved in git history at `assets/sources/clock-app-default/DESIGN_ID`), its compiled container `clock_app_default.orbtheme`, and the Watch Composer layer kit `skeleton_theme.zip` the ID refers to are no longer in the tree. Restore them with `git log --diff-filter=D -- assets/sources/clock-app-default/` and verify against sha256 `95aa5d3a4b646d9e80c63e75a118778d548cb7a46d0fab7e90b482a6c5614d38` (kit) / `4559e0f525bf69f49acf7e4ea8cfb5df893e48ffd05897ee80063ef5f2e2bdc0` (container); the copies on the owner's Windows Downloads folder are *different files* and are not substitutes. Until P4/P6 ship the `.orbapp` container and the `apps`-partition ingest, this art is **recorded, not wired**: it does not touch the boot path, and the compiled-in `assets/clock_assets.{bin,h}` pair — source kit committed at `assets/sources/av-kit/` — stays the sole card-less boot fallback (section 1.1). |

This container was validated when it was written, and it is still byte-intact, but it is a FORMAT 1 file and no current reader will open it. `tools/orbtheme.py read_container` now refuses it at its format gate (`theme format 1; this build reads format 2`) before it reaches the checksums; recomputing them directly on 2026-10-12 still matches (header `86092E66`, art `DB5CC981`), so nothing is corrupt — the file simply predates FORMAT 2, which has no legacy path. The geometry below (`unpack_geom` on the blob it carries) was read while that call still accepted the container, and is recorded rather than re-verifiable today: hands `show=[1,1,0,1,0]`, 8 GIF frames at `tick_rate 4`, 128 delta rects, 0 tourbillon, and 2 rotating-second `aux` records (the c2 enamel subdial's seconds hand). Art is 2,650,159 B = 41 pages, inside the `SLOT_CAP` (0x400000) and the 234-page budget `tools/host_art_residency_test.py` pins. `skeleton_theme.zip` was byte-identical to the owner's source (sha256 `95aa5d3a4b646d9e80c63e75a118778d548cb7a46d0fab7e90b482a6c5614d38`) and its `manifest.json` carried the same `WCD5-…` ID; it is no longer committed. The Design ID holds a *reference* to the art, never the bytes (composer help, `Watch Composer.dc.html`); the layer kit holds the bytes, so the kit in history is the only remaining copy of those pixels.

Three facts for whoever wires this at P6. The container's slug and title are `skeleton`, which collides with the theme already installed under that slug on orb-A's card — it cannot go on a card as-is under that name. The committed container has never rendered on hardware: routine `serflash.sh` reflashes push only the app, never the `assets` partition, and the slot headers validate before the fallback is reached. And no tool converts a finished container back into the compiled-in `clock_assets.{bin,h}` pair — `bake_clock.py` regenerates that pair only from an Arduino-style `clock_style.json`/`theme.json` folder, and the composer's `orbtheme-baker.js` emits only the container. Building that reverse path, or the `.orbapp` packaging, is P4/P6 work, not this ruling.

### 0.3 Owner's rulings, 2026-10-12 (second round) — closing P2's item 1

The `opt_*` shell rendering is accepted as designed — the shell really does render
what the clock answers (`menu_view.c` `PG_TOP`/`PG_APP`, §7 item 2 done, `PG_THEME`
gone); §12.6's "NULL everywhere" row was stale and is corrected there. Three rulings
close the item; the work order is §13.

| Ref | Decision | Ruling |
|---|---|---|
| D11 | The theme picker's commit path | **A theme pick installs and reboots, unconditionally.** The stage-then-ask flow is rejected: no pending NVS key, no "reboot now" / "keep this dial" rows, no boot-time card install. Press on a theme copies the container into the alternate slot on the worker task, verifies it from flash, marks that slot live, and asks the OS for the reboot — the pick IS the confirmation. Failure stays honest: the row answers with its reason and the device does not reboot. |
| D12 | App option pages may nest | **Sub-menus, one level deep** (owner chose this ABI shape from the four offered). An app's option list can carry header rows that open one nested list; items live inside it, never inside each other. One ABI hook added (§1.4): `opt_kind` — `OPT_ROW` / `OPT_GROUP` / `OPT_ITEM`, contiguous-section convention, NULL hook = today's flat page. |
| D13 | The default theme's slot | **The factory default is never an install target, and stays pickable.** Today the flashed bank at `assets` partition offset 0 sits inside slot A's region and `target_slot()` installs into A whenever slot B holds an install: the second pick erases the default AND the `use_baked_in()` card-less fallback, which reads pixels from that same region. The model the owner ratified: **A = the default, B = the card install** — a swap flips which is live, it never copies over A. **D21/D24 (§0.9) keep the rule and change what fills it: the slot holds a whole multi-app FORMAT-3 bundle rather than one clock dial's bank, so A stays the shipped default bundle and the flip is the bundle swap.** **RETIRED 2026-10-19 by §26.** The premise it protects — that a board must boot to a theme it was flashed with — was dropped instead of amended: a fresh flash has **no theme**, and the dial renders each app's base form from OS primitives. No factory pixels means offset 0 stops being sacred, `assets` becomes two honest slots, and ping-pong unblocks without a home for the default being found at all. §24.1's four rejected homes are moot. The clause bound until then, and D30 shipped its refill, its `staged` word and its light on the single target slot under it.** |

### 0.4 Owner's ruling, 2026-10-12 (third round) — the M3 overflow-mechanism choice

The owner ratified the §14.1 ruling as proposed. Numbers, method and the
refusal-hunt finding stand there unchanged.

| Ref | Decision | Ruling |
|---|---|---|
| D14 | Which mechanism serves the overflow bank | **A (flash re-map) is the mechanism — for the overflow bank as for the primary.** The measured 3.6 ms of switch and 5% of read cost an arena saves do not pay its 471 ms boot fill and 3.86 MB of PSRAM (of 7.95 free), and steady rotation is identical at the reporting resolution. **B (PSRAM arena) ships dark** (`ORB_ART_MECH_B 0`) as P4's escape hatch — the only way a third flagship can boot when the page budget says no — with its gate measured and its ABI promise host-pinned. Flipping B on is one byte (`art_bank.h:98`); if it ever ships lit, its carried-bank rule applies (§14.1: the release-for-free variant re-fills at 471 ms, so an arena app carries its parked bank — the §6/M-carrier decision is mandatory for B, not optional). **D30 (2026-10-15, §20) lights B for the installed-theme runtime path only; A stays boot's mechanism and this row's overflow ruling stands. Implementation trap: `tools/host_art_residency_test.py:72` pins `#define ORB_ART_MECH_B 0`, so flipping the byte lands together with an update to that pin.** |

### 0.5 Owner's ruling, 2026-10-12 (fourth round) — the behaviour vocabulary, closing P0
The owner answered §9.2's question — which `CLK_KIND_*`/`CLK_SRC_*` become
OS-registered behaviours — with **"I didn't think we were going to expose
any of it, only time"**, and answered the paper's one open question (does
`now()` hand out the OS-converted `struct tm`) with **"raw time_t only —
apps convert themselves; the OS just guarantees the seconds."** The full
paper is `docs/behaviour-vocabulary.md`.

| Ref | Decision | Ruling |
|---|---|---|
| D15 | The behaviour vocabulary the OS registers | **None — the OS exposes only time.** `orb_svc_t` (§4.3) is the entire registered surface and its time entry freezes exactly as the plan already spelled it: `now(app, sec, frac, synced)` + `tz_label`, handing the **raw `time_t`** — no `struct tm` and no source name crosses the broker. The OS guarantees the wall second (the `compose_now` extraction: two time sources, `CLOCK_EPOCH`, the trust rule, the drift re-anchor), the scheduler's sub-second fraction (the sweep contract), and the synced flag. Conversion and every calendar derivation (`SOURCES` incl. `week`/`moon_age`/`quarter`) are app-side code — the clock's `comp_value` moves with the app per §1.2, and an app that disagrees about what Saturday is owns that bug. `SOURCES`/`KINDS`/`FLAGS`/`HOSTS`/aux-drive enums freeze as the **bake-to-app wire interface** under the append-only rule (`theme_schema.py:52-53`, static-pinned `orb_theme.h:122`); the OS never inspects them (§4.1 geometry is opaque). §4.1's `behaviour_kind` ships reserved 0, the `sig` discipline. **P0 is closed; P3's broker implements §4.3 verbatim.** |

### 0.6 Owner's ruling, 2026-10-12 (fifth round) — the app beat is set at runtime (D16)

Surfaced while presenting D15's full exposed surface: §4.3's prose promised
"a scheduler an app registers with (`period_ms`, `cb`, `budget_us`)", but
the frozen `orb_svc_t` carries no scheduler entry, and today's beat is
decided at activation inside the app (`clock_view.c:1935-1939` picks
`start_period` from GIF mode and `orb_bg_every_p()`; the shipped dial
disables the whole beat at zero — §13.7). The owner's ruling: the clock app
runs at 15/12/8/4 fps, so **the beat must be settable at runtime**.

| Ref | Decision | Ruling |
|---|---|---|
| D16 | How an app gets its frame beat | **The `tick()` hook stays the only callback; the beat is a runtime `set_beat` entry on `orb_svc_t`, bounded by the manifest.** The app never hands the OS a function pointer: delivery runs through the registry's `tick(app, now_ms)`, visible apps only (§1.4). The manifest's `period_ms` (§4.1) becomes the **ask** — what the app wants right now, adjustable at runtime through `set_beat` as before — and the manifest additionally carries an **explicit set** of acceptable rates (D32, R7). The OS picks the highest rung it can achieve that is no faster than the ask, and falls back to the set's floor when the ask is not achievable at all, so the owner sees the whole reachable range at install and an app can neither grab a rate the set forbids nor invent one. `BEAT_MIN_MS` applies to the floor. Budget policing stays as written: the OS measures each app's tick time against its declared budget and refuses/parks an overrun (§4.3, C2 discipline). **Since D32 there is no shared per-app beat:** the OS owns one flush cadence, and each live stack advances frames on top of it at its own picked rung. The flush still runs at the fastest live requirement, so a thin watch under a thick one costs the thick one's rate until the swap settles, and the §6 cadence-collapse guard measures that before it ships. *Amended by D32 (§23.4): the manifest table is an offer, the theme names what is present, and the beat is the OS's pick from a declared set.* |

### 0.7 Owner's rulings, 2026-10-12 (sixth round) — the tz offset and theme install join the surface (D17, D18)

Surfaced when the owner reviewed D15's full exposed surface: "I think I need
the TZ offset too. And I need to be able to copy my theme files into flash,
no?" Both are true gaps in the frozen paper. D15's raw-`time_t` ruling does
not work for the P6 clock app without the offset — the app converts by
itself, but the zone is owner config, not app config. And the theme picker
is app UI (B2) while install is the OS package manager (§1.1): today the
compiled-in clock calls `orb_theme_scan()`/`orb_theme_post_install()`
directly (`clock_themes.c:87,159`); on the card those calls must go through
the broker, and the invariant the incidents enforce is that **no app ever
opens the assets partition** — the OS copies, verifies, commits, reboots.

| Ref | Decision | Ruling |
|---|---|---|
| D17 | The tz offset on the broker | **Yes — `tz_offset(app, when, &offset_min)` joins the free `time` surface.** It returns the offset in minutes of the OWNER's configured zone (`orb_net`'s `s_tz` table, the `net_tz_set()` single door, `orb_net.h:77-97`) at any instant, DST included — the existing `tz_offset_min()` computation (`orb_net.c:369`, localtime-minus-gmtime) re-exported as the broker's fourth door. D15 stands untouched: still no `struct tm`, still no source name — the OS guarantees seconds AND the owner's zone, because a process-global `TZ` silently shared by twelve apps is exactly the split clock-authority D15 was drawn to avoid. `localtime_r` with `gmtime_r` plus this offset reproduces the dial's local time exactly. |
| D18 | An app installing a theme | **Yes — as requests, never as bytes: the `themes` capability (owner-granted).** `orb_svc_t` gains `theme_count` / `theme_id` / `theme_active` / `theme_changed` / `theme_note` / `theme_install(id)`. `theme_install` is D11 verbatim through the OS package manager — copy into the target slot, verify from flash, mark live, ask the reboot — gated by `orb_theme_id_ok()`, taking a scan-validated id and never a path. ~~An app's own art arrives in its `.orbapp` payload and is banked by the installer at install time (P4)~~ — **void under D20/D21 (§0.9): no container carries art, so the installer banks nothing and an app's pixels arrive only in a theme bundle. The six `theme_*` entries and the request-not-bytes rule all stand, and runtime flash writes still do not exist for apps, because §14.4 is the cheapest teacher of why.** The browser-upload trio (`/upload`) stays the owner's direct door, unchanged. |

---

### 0.8 Owner's ruling, 2026-10-13 (seventh round) — the permissions model (D19)

B3's prompt shipped (`da0ecd7`) and cost more than it bought. An ask could only
ever be exercised by an app the gate had already refused. Every app in the tree
today is compiled in and gate-free, so the ceremony had no subject, and the
PERMISSIONS bench item could not be closed by anyone holding the rotary. The
owner chose **Option B: declare-at-install is the grant**.

| Ref | Decision | Ruling |
|---|---|---|
| D19 | Where a capability is consented to | **At install, once, by the manifest's declared set — not by a prompt at first use.** `orb_app_t` carries `caps`, and the `.orbapp` manifest fills it (P4). A declared owner cap is live the moment the app installs. The dial's PERMISSIONS ladder stays, and its one power is **revocation**: a toggle closes a declared cap and reopens it. It can never open a cap the app never asked for. `orb_svc_set_grant()` refuses an ON outside the declared set (`ESP_ERR_INVALID_ARG`). `orb_svc_init()` masks every undeclared bit out of a stored word. A stale, restored or hand-rolled `orb.grants` can therefore only ever under-grant. **Absent is not zero.** A slot with no stored word was never decided, so its declared set stands. A slot whose stored word reads 0 is the owner's last word and stays closed. A partition wipe now costs the owner their revocations and never costs an app its consent — the direction B3's default-off had backwards. A refusal is evidence, not a request. The gate counts it, sets its bit in an ever-refused mask nothing but a reboot clears, and logs once per (app, cap) per boot. The ladder marks such a row `*`; the status ledger carries the names and the mask. `PG_CONFIRM` and the ask latch are gone: the cap row is the decision, one press. Compiled-in OS-tier apps carry `caps = 0` and are gate-free by tier, not by grant; their mask proves any call that walked past it. **Consequence for P4:** install-time disclosure is the UI that carries the consent — the manifest's capability list prints on the install confirm. A "real ask" can never again be owed on the bench, because nothing ships that can be asked for. The first P4 card app exercises a denial. |

### 0.9 Owner's rulings, 2026-10-13 (eighth round) — containers carry code, art is theme-only, and assets resolve through a registry

The upload bench proved the install ladder works on hardware. The design review that
followed changed what the ladder installs. The owner ruled on eight questions. The
decisions reverse D18's art transport, retire the three-slot art re-slice, and promote
wasm3 from an optional tier to a committed one. D3's "escape hatch" wording and §5's
"only when a real app demands it" recommendation are superseded by D20.

| Ref | Decision | Ruling |
|---|---|---|
| D20 | What `.orbapp` carries | **Manifest, settings, asset declarations, and executable code. Never art.** The code is a wasm3 module (60-100 KB runtime, apps stay C, existing toolchain survives) run over the same frozen `orb_svc_t`. P7 stops being optional. Raw native machine code loading stays refused: ESP-IDF 5.4 ships no user-space loader, and §5 prices what unaudited pixel code costs the 123 ms commit. |
| D21 | Where an app's art lives | **Nowhere in the app.** `.orbapp` never ships a byte of art. Art arrives only in a theme bundle. An app that cannot find an asset either substitutes an OS global or refuses to run, and the app decides which. D18's "an app's own art arrives in its `.orbapp` payload and is banked by the installer" is void. |
| D22 | The fallback tier | **The OS ships renderers, not pixels.** The tier is five generated primitives: solid rect, rounded rect, stroked ring, rotated bar, and text in the compiled-in font. No fallback bitmap ships, so D10 ("the OS ships no art") stands: a filled rectangle the compositor draws is not artwork, a shipped fallback file is. Where a fallback would look wrong the layer is simply absent, which is D21's "the app decides" arriving by another route. The global-asset tier and the fallback tier are the same five routines. |
| D23 | The asset registry | **Two halves with different growth rules.** The *class vocabulary* (text, sprite, hand, plate, ring, bank, layer, status) is a frozen C enum, versioned by the `abi_min`/`abi_max` the header already carries. The *named slots* live in the manifest's asset table, owner-scoped as `<slug>/<name>`, with `global/<name>` for the frozen set, and that table is the app's **offer**: what it *can* paint. Since D32 the theme's per-app element table is the **declaration**: what is present, in what order, with what pixels. Offer and declaration are checked against each other at install in both directions, and every slot the theme does not name stays silent on the app's own default (D31). Resolution picks between those two namespaces and nothing else, so one app's `bg` can never shadow another's. A PR promotes a **class**, never bytes: two apps wanting the same pixels is coincidence, two apps wanting the same kind of slot is a shared abstraction. *Amended by D32 (§23.4): the manifest table is an offer, the theme names what is present, and the beat is the OS's pick from a declared set.* |
| D24 | Format compatibility | **FORMAT 3 only, no backward path.** `orbtheme.py` refuses FORMAT 2 at the format gate exactly as it already refuses FORMAT 1. The shipped kits get re-baked. No dual reader. |
| D25 | Launch apps | **The two shipped apps run bare.** Clock and HSB use globals only and declare no app-specific art. The app-namespaced half of the registry ships dark. `global/` carries the first release. |
| D26 | A missing required asset | **Refuse at install, in both directions.** Content an app declares required and no theme supplies is a refusal, and so, since D32, is a theme whose element table names a class or element the installed app cannot paint: an app cannot promise to draw a thing it was given no instruction for. The owner learns both on the page that already lists refusal reasons, not after an activation that shows nothing. A theme section naming an app that is **not installed** is neither — it installs, and the intersection resolves (§23.8). *Amended by D32 (§23.4): the manifest table is an offer, the theme names what is present, and the beat is the OS's pick from a declared set.* |
| D27 | The wasm3 memory bound | **Declared in the manifest and gated at install**, in the same shape as `caps` and `period_ms`: the owner sees the cost before consenting, and a module that cannot fit never writes a byte of flash. A trapping module bills a C2 fault, so two strikes mark the app failed and the dial boots to the clock. |
| D28 | Who defines the format | **The firmware's resolver is the source of truth.** The theme editor adheres to it, never the reverse. |
| D29 | The theme budget | **`ORB_THEME_SLOT_CAP`, 0x3F0000 (4,128,768 B) per bundle**, policed by the editor at build time and again by the installer at verify time. It is a 63-page ceiling at the measured 64 KB MMU page. **The row read "(4,128,764 B)" from §0.9 until 2026-10-15; `orb_theme.h:372` says 0x3F0000 and 0x3F0000 is 4,128,768.** |
| D30 | Flipping among several card themes | **RULED 2026-10-15 (owner): the PSRAM hybrid, default-first — lazy, no reboot, no size gate.** Two worker stages per flip. (1) Select the baked-in default: a selection change, zero PSRAM, and it frees the previous theme's arena. (2) ONE fused card pass streams the bundle into both the PSRAM arena and the other flash slot (ping-pong) while P6's flip page paints a progress bar; verify the arena by streaming CRC, swap the live bank at a tick boundary, then write the slot header LAST (D9's order; the ledger reads `staged` until commit). One theme is resident at a time, so every bundle ≤ D29's ceiling flips lazy — the two-arena /2 arithmetic and the per-size split are retired. Boot refills the arena from the committed slot: no card on the boot path, +≈0.15 s at av-kit and ≈0.5 s at cap (D14's fill arithmetic, moved onto flash). A torn flip leaves glass and boot on the default, which the owner accepted; the previous theme stays one press away in its slot. Arm A (copy+reboot) survives only as the fallback when the arena cannot allocate at flip time. Measurements §19.2; the ruling's full text, its consequences, and its outstanding bench gate are §20. **P6.3 as shipped (2026-10-19, §24): the fused boot refill, the lit mechanism B and the `staged` word landed; the ping-pong half did not, because D13's amendment was declined and the default has no home that does not consume a slot (§24.1). The flip is single-slot-B until it does. **P6.4 (2026-10-19, §24.8) built the flip page's store half: /api/v1/sd/bundle is the ingest's first live caller, /api/v1/sd/theme stores a verified bundle, and the page's store button warns the owner that a cut mid-copy returns the factory clock — the sentence §24.5(2) assigned to this slice. The glass half (progress bar, arena swap at a tick boundary, ping-pong) waits on the default's new home and on P6.5's reader. **§26 settled the first half of that on 2026-10-19: the reader landed (§25.7) and the default's new home is that it has none, so the glass half is unblocked — with this ruling's "a torn flip lands on the default" restated as "lands on the base render, with the last good theme still in the other slot", which is the same promise and a version-matched floor under it.** |
| D31 | Content when a theme carries none | **The app supplies its own default.** Theme content wins wherever the bundle carries it. Where it does not, the app renders a default rather than showing nothing, and D21's two choices ("substitute an OS global or refuse to run") gain the third it left open. The default is code, not bytes: D22's five primitives draw it, so D21's "`.orbapp` never ships a byte of art" stands and D10's "the OS ships no art" stands. The selector is §4.1's `assets` flag, which that row already named "D21's fallback contract": `required` keeps D26's install refusal, `optional` means the app has a default to draw. App-carried *layout* for a silent theme reads from the container's own `geometry` region (§4.1: opaque to the OS, already size- and CRC-checked at `orb_app_manifest.c:319`) — the only reading those counts exist for. §9 item 3 is closed for the clock. |
| D32 | Who declares a displayed element, and who sets the pace | **Ruled by the owner 2026-10-15.** The **theme** declares, per app, the elements it displays — identity, class, pixels, animation frames, opacity mask, and the **depth order** of the stack — in one legible schema the OS can read. The **app** says how and where each element it was given is painted, and when it needs repainting. Every element is bound to its theme, so a different theme re-composes the same app. The **OS** composites in the declared order and picks, per live app stack, the **highest frame rate it can afford** out of a declared acceptable set (a watch ladder like 4/8/12/15 fps; another app 1 fps to as-fast-as-possible), asks that app to repaint at that pace, and adjusts the pick dynamically. Recorded as heard in §23. It re-opens **D23**, **D26** and **D16**, with those amendments applied to §0 on 2026-10-15 (§23.4, wording at §23.9), and voids §22.2's opaque per-section geometry (§23.3). Split as ruled: the element table is **P6**, the scheduler is **P8** — numbered P8 because P7 is the ratified wasm3 tier (§23.5). R6 and R7 are ruled (§23.6, consequences at §23.7); R2 is ruled with D26; R3, R4 and R5 are open (§23.8). |
| D33 | Where the product logic runs | **The browser holds it. Go is a thin same-origin shim.** RULED by the owner 2026-10-20, ahead of P6b's editor work. The next orb-manager release is served from a server and flashes over Web Serial, the browser ESP Writer flow ESPHome Web uses, so no Go process may hold product logic. Every FORMAT 3 rule (the writer, the two budget sums, the element table, the preview, the release pack) lives in browser JS and must run with no hub present. Go keeps three jobs: serve the page, forward device calls same origin, and the serial plus fleet machinery the desktop build still needs. `orbnet.ContainerName` stays a pre-gate and never becomes the authority on a bundle. No new Go format package enters the tree. The derivation, the three verified facts behind it, and the P6b decisions it changes are §29. |

**What this retires.** The three-slot art re-slice of §3.2 (`art0`/`art1`/`art2`) is
withdrawn: with no art in any container there is no per-app bank to place, and
`partitions.csv` stays exactly as pushed on 2026-10-12. `orb_art_reserve()`'s per-app
bank claim and D18's art-banking installer path are shelved rather than deleted,
D30's ruling (2026-10-15, §20) wants no per-theme reserve — one theme is resident at a time — so the shelf stands as dead code. `CAP_ART`'s meaning moves
from "its own bank" to "the theme slot the registry resolved for this app".
D13's A/B survives with changed content: A is the shipped default bundle, B is the
installed bundle, and a pick flips which is live. **§20 (D30, 2026-10-15) supersedes
that pairing for themes: both slots become ping-pong install targets, and the card-less
fallback moves from slot A's pixels to the baked-in `clock_assets` mmap, which is why
"A is never an install target" is no longer load-bearing. Verify the mmap fallback
covers the card-less boot before erasing A — bench item §20.5(2).**

**The capacity question D21 does not answer.** A theme covers every enabled app at
once, so a bundle carries N flagship banks against one 4 MB ceiling. The clock's
shipped av-kit bank is 1,173,563 B (18 pages) and the kit3 stress bank measured
3,858,874 B (59 pages), so one kit3-sized app nearly fills a slot by itself. The
owner's ruling is that this is a theme-editor budget decision rather than a firmware
one, and D29 polices it. Two launch apps drawing on globals fit with room to spare.


---

## 1. The line, drawn at today's files

Today the OS reaches into the clock by name. `input_router.c` hardcodes `INPUT_SCREEN_CLOCK` and calls `clock_view_screen()`; `orb_net.c:2261` hardcodes `to=clock|menu|hue`; `main.c:93-95` calls three `*_init()` in a fixed order. That is the coupling this split removes.

### 1.1 Stays in the OS (never ships on the card)

| Area | Code to keep | Why it cannot be app data |
|---|---|---|
| Boot ladder | `main.c` whole, incl. the pre-`display_init()` install order | an install must not race a paint |
| Panel and flush | `display/display.c`, `co5300.c`, `display_mark_rows*`, `display_commit_row`, `display_fb_mirror`, the bench counters | hardware, TE timing, DMA |
| Compositor | from `clock_view.c`: `fill_sprite_dsc`, `fill_plate_dsc`, `make_layer`, `layer_spr`, `sprite_bytes`, `sprite_drawable`, `paint_ctx_init`, `paint_row_ctx`, `paint_row`, `blend_px`, `mix_16_16`, `trigo_sin`, `rot_sin_cos`, `swept_rect_box`, `swept_box`, `area_union`, `mark_rows`, `l4_active`, `request_commit`, `mark_all_layers`, `clock_layer_commit`, `map_blend`, the `s_row_snap` flush gate | the measured 0.22 ms/row and 185-232 ns/px live here (section 5) |
| Delta-bank playback | `s_canvas`, `gif_apply_frame`, `build_bg_dscs`, `show_plate_frame`, `bg_frame_now`, `bg_rate`, `gif_tick_ms`, `gif_mode` | keyframe-plus-rect playback is not clock-specific |
| Frame scheduler | `tick_timer_cb`, `tick_base_ms`, `hand_beat_ticks`, the deadline ladder, `report_fps_if_due`, and today's `clock_view_set_bounce` (rename: suspend/resume) | the LVGL task is the one animation clock and the only widget context |
| Art bank and mmap | `art/art_bank.c` whole | the MMU budget is device-global (section 3) |
| Package manager | `orb_theme.c`: header parse, `slot_verify_pixels`, `copy_into`, `adopt`, `orb_theme_boot/stage_load/unstage/scan/delete`, the upload trio; plus `orb_sd.c` mount/lock/`orb_sd_name_ok`/path builders | verify-then-commit, the card lock, the A/B discipline **for themes** (apps drop A/B per D9 and P6 reconciles the paths) |
| Input | `knob/knob.c`, `input/input_router.c` rewritten as a registry | the one knob, the two-level gate, `knob_row_step` |
| Power and motion | `power/idle_dim.c`, `imu/imu.c` | policy owner; apps request, never write (section 4) |
| Time service | `orb_net.c` SNTP, the tz table, `orb_net_time_synced()` | one clock authority; exposed as `orb_time_now()` (section 4) |
| Radios, HTTP, OTA | `orb_net.c` STA/SoftAP/httpd/OTA/rollback | no app may own the radio or the boot slot |
| Menu shell | `menu/menu_view.c` row pool, cursor clamp, `orb_menu` worker, service/reap path | the OS's own UI (section 7) |
| Fallback | the compiled-in `assets/clock_assets.{bin,h}` pair, `orb_theme_geo.h` macro indirection | a board with a dead card still tells the time |

### 1.2 Becomes the clock app (`apps/clock/`)

| Area | Code to move |
|---|---|
| Dial art | the whole pixel bank: `assets/clock_assets.bin`, every `clk_sprite_t`. The clock app's own bundled default (ruling D10) was the Design-ID-sourced container committed at `assets/sources/clock-app-default/` (§0.2) — no longer committed, recoverable from git history — recorded now, wired by P6 once `.orbapp`/`apps`-partition ingest exists |
| Scene and geometry schema | `clk_theme_t`, `clk_bg_rect_t`, `clk_comp_t`, `clk_aux_t`; the `THEME_FIELDS`/`COMP_FIELDS`/`AUX_FIELDS` wire lists; `theme_schema.py` SOURCES/KINDS/FLAGS; `GEOM_ABI` |
| Time to hand math | `hand_angles` (hour/min/sec mapping, sweep phase, railway, `second_sweep`/`tick_rate` policy), `s_bt_ctr`, `s_bt_frac` |
| Tourbillon | `tourbillon_advance`, `tourb_chords`, `tourb_stamp_pose`, `tourbillon_ink_mark`, `tourb_mark`, frame-bank selection |
| Complications | `comp_value`, `comp_frame_index`, `comp_strip_positions`, `comp_strip_digits`, `comp_slot_show/hide`, `comps_update`, `comp_create_slots`, `comp_bucket`, `complications_create_bucket`, `comp_touch_box`, `comp_add_jobs`, and the calendar/astronomy constants (`MOON_SYNODIC_S`, `MOON_NEW_MOON_S`, `comp_iso_week`, `comp_year_leap`) |
| Sub-dial hands | `aux_create`, `aux_update`, `aux_value_1000`, `aux_angle_10`, `aux_insert_at`, `aux_add_jobs` |
| Layer ordering policy | the z-stack discipline of `clock_layer_commit` (982-1051), submitted as a layer list rather than coded into the compositor |
| Its own settings | tick rate and sweep, the `--matte` bake policy, `CLOCK_L4_MARK_STATIC_BOXES` |
| Its own UI | the THEME menu page and the `/themes` PWA: that is the clock app's installer |
| Toolchain | `bake_clock.py`, `prep_watch_kit.py`, `kit_check.py`, `theme_schema.py`, `orbtheme.py`, `tools/watch-composer/` |

### 1.2a The compiled renderers, named against the class enum (the P5b naming pass)

§1.2 lists the clock's derivations by function name. This pass puts each one against the frozen `orb_asset_class_t` (D25, `main/assets/orb_assets.h`) and the six primitives it maps to, and records which is OS-generic and which stays clock policy. It settles nothing new: it reads §1.1, §1.2, the D25 freeze and the per-commit cost table of §5 together. The test for each row is one question — *does a weather app need this exact code?*

| Area (§1.2 row) | Function | Feeds | Verdict |
|---|---|---|---|
| Time to hand math | `hand_angles` (275) | `hand` | **Clock policy.** The mapping is watchmaking; the payload is not. It emits an angle, which is the whole `hand` contract, and `ang[0..4]` never leaves the file. OS takes the number, never the formula. |
| Sub-dial hands | `aux_angle_10` (1584) | `hand` | **Generic.** `zero`/`span`/`value_max` to tenths of a degree is integer-only and mentions no watch. A fuel gauge needs it. Move it to `main/gfx/` with the `hand` job builder. |
| Sub-dial hands | `aux_value_1000` (1554) | `hand` | **Clock policy.** It knows what a power reserve is. |
| Complications | `comp_value` (1180), `comp_iso_week`, `comp_year_leap`, the moon constants | `bank` / `text` | **Clock policy, and it must stay.** D15: no `CLK_SRC_*` name crosses the broker. The app converts seconds to local time and owns the calendar bug. |
| Complications | `comp_frame_index` (1093) | `bank` | **Generic.** Signed round-to-nearest at `scale x16`, clamp below zero, *wrap* (never clamp) at `count`. It is `theme_schema.frame_index()` verbatim, so the wire contract already fixes it; the OS owns both ends. Move it to `main/gfx/` as the `bank` selector. |
| Complications | `comp_strip_positions` (1108), `comp_strip_digits` (1121) | `text` | **Split.** The glyph-index reduction is `theme_schema.strip_indices()` verbatim and generic; formatting an integer as the date drum's digits is clock policy. The OS draws glyph indices from a box and a `glyph_step`; the app chooses the digits. |
| Complications | `comp_slot_show/hide`, `comp_add_jobs` (1442) | `bank` / `layer` | **Clock policy.** It is a job list. §5 prices interpreting it as free; the app builds the list, the OS paints it. |
| Tourbillon | `tourbillon_advance`, frame-bank selection | `bank` | **Clock policy** — one rotating frame per beat is an escapement, not a library. |
| Tourbillon | `tourb_chords` (472), `tourb_stamp_pose` (516) | `bank` | **Generic.** The alpha-chord row extent and its rotated stamp are how *any* rotated art marks rows tightly instead of taking the swept box. Only the tourbillon calls it today. Move both to `main/gfx/` and keep the `/1024`-times-64 scale contract intact — breaking it puts stamped rows outside restored rows, and the artifact is invisible to the fps line. |
| Layer ordering | `clock_layer_commit` (982-1051) | `layer` | **Both, and that is the split.** The z-stack *discipline* is policy the app submits as a list; the commit that walks the list is `gfx_commit()`, already in `main/gfx/`. |
| Its own settings | `second_sweep`/`second_railway`/`tick_rate`, the `hand_beat_ticks` phase counter (`s_bt_ctr`/`s_bt_frac`) | — | **Clock policy.** Cadence is the app's own feel. The OS keeps the one timer (§1.1) and hands back beats. |
| Pixel path | `blend_px` (238) / `gfx_blend_px` (456), `gfx_paint_row_ctx` (321), `mix_16_16` (224), the in-place flush gate inside `gfx_commit` | all six | **Already in `main/gfx/`**, byte-identical maths since P1. Per-pixel, so interpreter cost lands per-pixel — the one row that can never move behind the broker. `s_row_snap` and `paint_layer` in the §1.1 list are the pre-P1 names: the snapshot buffer is gone (the gate compares the staged band against the PSRAM mirror in place, `orb_gfx.c:384-392`) and `paint_layer` is inlined into `gfx_paint_row_ctx`; `l4_active`/`request_commit` now live in `orb_sched.c`. §10 records the P1 renames. |

Two constraints the pass leaves standing, both already written down:

1. **The `text` class has two answers, and the clock uses the baked one.** The dial's only text render is the banked glyph strip (`comp_strip_digits` + the bake's glyph bank); its LVGL digital label is long gone, repurposed as the bounce gate at `clock_view.c:1967`. The OS's own answer to the same class is the *generated* fallback, gated by `orb_asset_text_ready()` (`orb_assets.c:260`) and taken when an app ships no font (`orb_assets.c:404`). So a `text` slot resolves two different ways depending on whether art is present, which the resolver already encodes and D25's one-name-per-thing freezes the naming of. An app that declares `text` and ships no art still gets readable glyphs — that is P5b's second pass line, and it is the cheap path an app like `hsb_color` takes.
2. **`orb_gfx.h` still includes `clock_assets.h`** for `clk_sprite_t` and `CLK_SCR_W/H`, and takes an art-bank `base` per call. The header names P6 as the owner of that debt. Until it is paid the rows above move into `main/gfx/` but cannot be *called* from outside the clock, which is why P5.5 sits between this pass and the first non-clock app.

### 1.2b P5.5 design constraints, read off the tree (2026-10-13)

A first implementation pass was written, found to rest on three wrong assumptions, and reverted. These are the corrections, each verified against a file rather than reasoned from the plan. They decide the shape of P5.5 and cost nothing to state now.

1. **`gfx_prim_ctx_init()` does not exist.** `orb_assets.h:266-268` describes it as the compositor-side entry that bakes a primitive into a caller's plane and fills a `gfx_paint_ctx_t`, so a fallback rides the same path and the same role counters. Nothing in `main/gfx/` defines it. Writing the promise and not the function is the exact drift the D25 freeze is meant to prevent, and building it is the first commit of P5.5.
2. **A fallback cannot bake into the app's art bank.** `art_bank_extra_ptr()` returns `const uint8_t *` (`art_bank.h:238`) — the bank is a mapped, read-only region. A generated plate has to be written somewhere, so a fallback's pixels are heap memory the row owns, freed on del and on wipe. The precedent is already in the tree: `tourb_chords()` allocates PSRAM per row (`clock_view.c:481-485`), and `orb_assets.h` already routes its one LVGL dependency through an installed function pointer rather than an include, for the identical host-suite reason.
3. **The registry already hands out compositor-compatible records.** `orb_asset_prim_bake()` writes a `clk_sprite_t` (`orb_assets.h:300`), the type `gfx_paint_ctx_init()` consumes, with `color_off = PRIM_LEAD` (4) — plane-relative, and non-zero precisely to satisfy `gfx_sprite_drawable()`. So the adapter P5.5 needs is small. The catch is that each baked row points into its *own* plane while each tier-1 row points into the theme payload, so the table stores a base per row and `orb_gfx.h`'s single-`base` assumption does not survive contact with two tiers.
4. **`orb_asset_t` carries geometry, not styling — by design.** It has no colour, alpha, corner radius, bar width or string, and adding them would put a theme's palette in a registry record. So the styling belongs on the layer descriptor, which is the app's own claim. `orb_layer_desc_t` must carry `color`/`alpha`/`r`/`thick`/`text` and a generator override, appended after the frozen `art` field and never reordered.
5. **The frozen ABI's signatures settle two questions.** `layer_add` takes `(app, desc)` and returns one int (`§4.3`): no plane argument, so the plane is the module's to own (point 2); no z argument, so paint order IS table order, and reordering a layer means del and re-add. Both readings are forced, not chosen, which is worth writing down before someone rediscovers it as a limitation.
6. **One trap the table must not reintroduce.** A `gen_prim` field left at 0 reads as `ORB_PRIM_RECT`. For a `HAND` slot that is a silent lie — the app asked for a hand and gets a rectangle. The descriptor's unset value must map to "whatever the class says", with `SPRITE`/`PLATE` the only classes that genuinely mean `RECT`.

**Landed 2026-10-13 against all six.** Two of the constraints moved shape under implementation, and both moves are worth recording before the code is read as if the notes described it. Point 1: `gfx_prim_ctx_init()` lives in `orb_layers.c`, not `orb_gfx.c` as `orb_assets.h` originally said (the header's comment now says so too). The seam it is — bake output into paint input — needs no LVGL at all, and living in the LVGL-free TU is what lets `host_orblayers_test.py` drive it with vectors instead of leaving the promise build-checked only. Point 5's forced readings stand; a sixth forced reading surfaced in `orb_svc.c`: `CAP_UI` is a free cap (`orb_svc.h:79-83`), so the "revoke wipes the glass" door this section sketched cannot exist — `orb_svc_set_grant` refuses a free cap's toggle above the layer code. The wipe lives at the two doors a free cap actually has: `orb_svc_app_suspended` and `orb_svc_forget_row`. The commit is the layer table itself (`main/gfx/orb_layers.{h,c}`, ~600 lines with the comments the design cost), the four wired entries plus the live/refused ledger pair in `orb_svc.c`, the forward typedefs in `orb_svc.h` (a complete type there would drag `clock_assets.h` and `esp_err.h` across the app boundary — the P5b lesson applied), and `tools/host_orblayers_test.py`: 73 host vectors over the shipped table and the shipped registry, 10 source-rewrite mutants all caught, plus mirror lints pinning the harness's two copies of compositor maths (`gfx_paint_ctx_init`'s gate, the swept-box formula) against `orb_gfx.c`'s own text. The job list it builds (`orb_layers_jobs`) has no caller yet — the clock routes its own layers and stays that way until P6 — which is exactly the pass line the table row promised: reachable through the gate, refused for the documented reason, counted either way, on the host, before a single pixel of it is benched. **D30's ruling (§20) lands on this seam for P6: the flip page owns the glass while one fused card pass fills both the PSRAM arena and the ping-pong slot, and boots refill that arena from flash — the card never re-enters the boot path.**

### 1.3 Also an app, and the cheaper proof the ABI works

`color/color_picker.c` is a screen with an armed state and knob input and no OS tether beyond the router. It is the second registered app and the first non-clock one. `orb_net.c:2261`'s `to=clock|menu|hue` becomes a slug lookup and `INPUT_SCREEN_COUNT` disappears.

### 1.4 New OS code that does not exist yet

| Module | Owns |
|---|---|
| `main/app/orb_app.c/h` | manifest reader, the `apps`-partition catalog, per-app state (installed / enabled / active / order / `failed`), install with reboot, uninstall from the OS page, the residency arena owner, the C2 fault counter |
| `main/app/orb_svc.c/h` | the `orb_svc_t` table, the grant check, the per-app call counter, the NVS quota check |
| `main/gfx/` | the compositor, delta bank and frame scheduler extracted from `clock_view.c`; P5 gives the `layer_*`/`bank`/`status` broker stubs their bodies |
| `main/assets/orb_assets.c/h` | **new (D23, P5b):** the asset registry — the frozen class enum, `<slug>/<name>` vs `global/<name>` resolution and its two fixed orders, and the five generated fallback primitives (D22). Owns the answer `orb_theme_slot_bytes()` returns today, generalized from one clock bank per theme to a named slot per class, and `main/art/orb_theme.c` shrinks by its offset arithmetic. `main/app/orb_app_manifest.c` and `tools/orbapp.py` gain the manifest's asset table together, so resolver and writer cannot drift. **Since D32 the manifest's asset table is the app's offer and the theme's per-app element table is the declaration; the compositor reads the declaration.** |
| `apps/clock/` | the clock app, built as one package plus a compiled-in fallback; under D25 it ships bare (globals only) and its art arrives in a FORMAT 3 theme bundle (D24, P6) |

The ABI is a small struct of function pointers, all called on the LVGL task, which is what lets the wasm3 tier arrive later without touching apps:

    typedef struct orb_app orb_app_t;
    struct orb_app_iface {                 /* every call runs on the LVGL task */
      esp_err_t (*activate)(orb_app_t *, const orb_svc_t *);
      void      (*suspend)(orb_app_t *);   /* park the tick; service() still runs (C7) */
      void      (*resume)(orb_app_t *);    /* clean repaint + restart the tick (C7) */
      void      (*tick)(orb_app_t *, uint32_t now_ms);      /* visible apps only */
      bool      (*armed)(orb_app_t *);
      void      (*set_armed)(orb_app_t *, bool);
      void      (*input)(orb_app_t *, int32_t delta, bool pressed);
      void      (*service)(orb_app_t *);   /* reap its own worker jobs, even parked */
      /* Options, one face per app (B1). The OS renders the page; the app answers
       * the rows. This is also how the clock app supplies its theme picker (B2):
       * the OS owns the list, the app owns what the list means. Single-level
       * nesting per D12 (§0.3): a GROUP row opens the run of ITEMs beneath it,
       * the shell hides those ITEMs until the group is open, and a NULL opt_kind
       * is the whole pre-D12 flat page. The shell validates the convention. */
      int       (*opt_count)(orb_app_t *);
      orb_opt_kind_t (*opt_kind)(orb_app_t *, int idx);  /* ROW | GROUP | ITEM (D12) */
      const char *(*opt_label)(orb_app_t *, int idx);
      const char *(*opt_value)(orb_app_t *, int idx);   /* rendered after the label */
      void      (*opt_inc)(orb_app_t *, int idx, int32_t steps);
      void      (*opt_sel)(orb_app_t *, int idx);       /* the press, if it acts */
      void      (*opt_arm)(orb_app_t *, bool armed);    /* gate the knob like a screen */
      const orb_menu_page_t *(*menu)(orb_app_t *);      /* NULL = no page, common */
    };

`suspend` keeps `service()` alive and stops `tick()` (C7), so a weather app parked
behind the dial can still finish an HTTP refresh it posted, but cannot repaint or be
counted against the frame budget. Nothing may repaint from `service()` — the OS drops
those marks and logs it, because a hidden app committing rows is precisely the
cadence collapse the fps findings measured.

Fault handling is the OS's, not the app's (C2): an abort inside any hook above is
caught at the panic handler, the fault counter for that app id goes up in NVS, and
the next boot comes up on the **clock** with the app marked `failed` and its reason
shown in the APPS page. Two consecutive faults mark it failed permanently until the
owner reinstalls. Because the S3 gives no MPU protection for PSRAM data, this is a
reboot boundary, not a recoverable error, and the ABI deliberately has no
`on_crash` hook an app could be trusted to run.

---

## 2. Reboot on commit, no reboot on rotate

Two operations were conflated until ruling C1. Loading **new** art is an erase-and-rewrite of the region the dial reads pixels from, and it ends in a reboot — `main.c:62-71`'s install-before-`display_init()` ordering is exactly that, and the owner accepts the dark screen it costs. Rotating between **already-resident** apps is a widget and descriptor rebuild, and must not reboot.

`art_bank.h:20-22` deliberately never unmaps, because the LVGL image descriptors built in `clock_view.c` cache raw pointers into the mapping. Those descriptors are already rebuilt on every activation (`build_bg_dscs`, `fill_sprite_dsc`, `comp_create_slots`, `aux_create`), so an app swap is a pointer and widget rebuild, not a flash operation. With both flagship banks resident at once (section 3.3) the swap needs no flash I/O at all.

| Operation | Touches flash? | Reboots? | Allowed on | Target |
|---|---|---|---|---|
| activate / switch (resident) | no | **no** | LVGL task, in the router's poll | one full-page commit, about 150 ms |
| activate / switch (evicted) | read only, re-mmap | no | worker, then swap | under 400 ms, never blocks the knob |
| install from card | yes, erase and write | **yes** | boot time, before `display_init()`, like a staged theme | refuse over budget, name the reason |
| uninstall, from the OS options menu | yes, erase | no, once nothing resident aliases it | worker task | confirm first, like `orb_theme_delete()` |
| firmware OTA | yes | yes | unchanged: boot slot + rollback | unchanged |

Why activation still cannot be allowed to write, priced: a 3.9 MB install is about 960 sectors, and an erase suspends the cache while the SPI bus is busy, which stalls every flash-resident read including LVGL's rendering code. Un-probed builds of the shipped dial measure 133.6-138.2 ms mean flush, and the accepted carriage recipe 123.3 ms at 7.41 median fps (`docs/fps-budget-findings-2026-10-10.md:23-24,107,114`); a 4 KB sector erase is in that same order. A live install turns a 7 fps dial into a stuttering one for seconds, which is the failure the pre-`display_init()` install order was written to prevent.

**What dropping A/B for apps gives up (D9), and what bounds the damage.** The theme path's two slots exist so an interrupted install leaves the *other* side intact and the dial still boots something recognisable (`orb_theme.h:24-28`; bench-proven at `.clinerules:444-458`, including the pending-theme ladder that gives up after 3 attempts and falls back to the baked-in bank). One slot has no such spare, so an install torn by a power cut or a pulled card leaves a bank with nothing to fall back to *inside that slot*. Three rules recover most of it without a second slot, all already written in `orb_theme.c`:

1. write payload and geometry first, read them back and CRC them **before** the header is written;
2. the header carries `serial` and is the only thing that makes a bank live, so a torn install has no header and its slot reads as **absent**, never as corrupt art;
3. a boot that resolves to no valid bank falls back to the compiled-in `assets/clock_assets.{bin,h}` pair, and the app is marked `failed` with its reason (C2) rather than retried forever.

So the accepted failure is bounded: a bad install costs the owner that one app and a re-copy of the card file, not the dial, and it can never show half of one theme and half of another because a half-written bank has no header and so never becomes the live one. Themes keep their A/B path as shipped; this ruling covers the app container, and P6 reconciles the two install paths rather than running both indefinitely.

## 3. Storage: the new partition, the theme bundle slots, and where art lives

### 3.1 Measured facts

| Item | Value | Source |
|---|---|---|
| MMU page size | 64 KB | `build/config/sdkconfig.h:854` (`CONFIG_MMU_PAGE_SIZE=0x10000`) |
| MMU pages per region / regions | 256 / 2 | `soc/esp32s3/include/soc/mmu.h:18` |
| DROM address window | `0x3C000000..0x3E000000` | `soc/esp32s3/include/soc/soc.h:168-169` |
| App `.flash.rodata` (the shared claim on that window) | 298 KB = 4a7c8 | `build/gorb-os.map` |
| App `.flash.text` | 1.00 MB = 10054c | `build/gorb-os.map` |
| App image in a 2 MB slot | 1,476,128 B | `build/gorb-os.bin` |
| Shipped compiled-in bank (av-kit) | 1,173,563 B = **18 pages** of 64 KB | `assets/clock_assets.bin`, `stat`; `CLK_BLOB_BYTES`, `LAT_BYTES_PRIMARY` in `main/art/art_bank.c:640`; source kit at `assets/sources/av-kit/` |
| Flagship bench theme (kit3) | 3,858,874 B = 59 pages of 64 KB | measured 2026-10-12 from `assets/sources/watch-orbital-kit3.zip` (sha256 `c910be26dde63eb4…`), which is no longer committed: restore it from git history to re-run this bake. **Not** `assets/clock_assets.bin`, which holds av-kit — an earlier revision of this row cited that file and so read as if the shipped bank were kit3 |
| `assets` partition | 8 MB at 0x420000, two A/B slots at 0x0/0x400000, cap 4,128,768 B (0x3F0000) each | `partitions.csv`, `orb_theme.h:48-55,296` |
| `www` partition | 1.25 MB at 0xC20000 | `partitions.csv` |
| `storage` (NVS) | 24 KB at 0xD60000, shared with the WiFi credentials | `partitions.csv` |
| Unallocated flash tail | 2.60 MB (0x29a000, ends 0xD66000 of 16 MB) | `partitions.csv` comment, verified by arithmetic |

| Per-app art footprint, flagship | 3,924,410 B (3,858,874 art + a 64 KB header block) = **60 pages** | arithmetic on the flagship row above |
| Two flagships mapped together | 120 pages + the image's own **22** (`.text` 17 + `.rodata`/`.appdesc` 5, per `idf.py size` 2026-10-11) = **142 of 256** | arithmetic; 114 pages of headroom. An earlier revision of this row counted only `.rodata`'s 5 and said 125 |
| Measured flagship, for calibration | `perpetual-calendar` = 2,503,747 B = **39 pages** on the board (§10); two of *these* plus `.rodata` = 83 of 256 | boot line `39 MMU pages`, `art_pages` in `/api/v1/status` |
| Two flagships in `assets` | 7,848,820 B of 8,388,608, **539,788 B spare** | arithmetic; the partition is the binding limit, not the MMU |
| Two *maximum*-size banks | 128+128+22 = **278 of 256 — overflows** | arithmetic; why `orb_art_reserve()` caps the running sum, not each request |

The DROM window is shared with the app's own `.rodata`, which is why `art_bank_init()` maps only `need_bytes` (`art_bank.h:37-41`). With `.rodata` at 298 KB the window is not the binding limit: the 8 MB `assets` partition is, at two flagship banks. That is exactly the capacity ruling C1 asks for, which is why mechanism A stops being a coin flip.

### 3.2 Agreed layout

```
/sd/apps/<slug>.orbapp              the drop zone (card; hand-copied or uploaded)
/sd/themes/<name>.orbtheme          FORMAT 3 theme bundles, several may coexist
assets 0x420000 + 8 MB              TWO A/B slots unchanged (D13 as re-scoped by
                                    D21/D24): 0x0 and 0x400000, cap 0x3F0000 each,
                                    each holding ONE whole theme bundle.
                                    The three-slot art re-slice of this section's
                                    earlier draft is RETIRED by D20/D21: no app
                                    carries art, so no per-app flagship bank
                                    exists to place. The `apps` partition holds
                                    containers (header + settings + code), not
                                    pixels.
apps, data, 0xD80000, 0x280000      2.5 MB: catalog + manifests + settings +
                                    the wasm3 module of each installed app
PSRAM arena (cap from M2)            overflow only: banks past the resident budget
NVS "orb.apps"                       installed / enabled / active / order /
                                    grants / settings / fault counters
```

**Correction to §3.2's original (2026-10-12), superseded by D20/D21 (2026-10-13).**
This section formerly sliced `assets` into `art0`/`art1`/`art2` for one flagship bank
per installed app. D20/D21 moved every pixel out of the container and into the theme
bundle, so the re-slice has no purpose and the partition stays as landed in
`e166897` (two 0x3F0000 A/B slots at 0x0/0x400000, cap `ORB_THEME_SLOT_CAP`).
The `apps` partition and every other row are unchanged from the table the owner
approved and the board pushed.

The new `apps` partition fits exactly in the unused tail: `0xD80000 + 0x280000 = 0x1000000`. It moves no existing row, so the `nvs` retention invariant at the top of `partitions.csv` holds and the WiFi credentials survive the change.

**Landed 2026-10-12** (owner approval, `e166897`, its own commit per the bench rule). The table went onto orb-A once at `0x8000` and reads back byte-identical to `build/partition_table/partition-table.bin`; the nine parsed rows end at exactly `0x1000000`. Lookups are label-only tree-wide, so no offset constant anywhere needed moving. Post-push: `ota_0` valid, art bank 18 pages / 1,173,563 B mmap'd, credentials and tz intact, D19 ladder 34 checks 0 failed.

**Capacity arithmetic against the cap of 8 (B5), re-run under D20/D21.** With no art in
any container, an installed app is header + settings + asset declarations + one wasm3
module. The slot stride is `APPS_SLOT_STRIDE` (0x41000, 260 KB) and the geometry ceiling
is `APPS_SLOT_GEOM_CAP` (256 KB), so the per-app budget is now the code budget, and a
real wasm app costs tens of KB. Every one of the 8 slots fits with orders of magnitude
to spare, and the flagship question moves off the `apps` partition entirely: it becomes
how much art one 0x3F0000 theme bundle can carry for all enabled apps at once (D29),
which is a theme-editor budget decision and not a firmware one. The former
*two flagship + several small* mix in this paragraph is obsolete: nothing is
flagship-sized on the app side any more.

Do not shrink the two app slots to buy room: 1,476,128 B used of 2 MB, and `partitions.csv` already warns that a slot smaller than the image turns every upgrade into `ESP_ERR_OTA_VALIDATE_FAILED`.

`storage` at 24 KB is the other real constraint, and C9's `string` settings make it tighter: an 8-app grant-and-config table with free-text values needs a per-app byte quota and a size check at install, before any app gets `config`.

### 3.3 Residency: A is the default, B is the overflow valve

| | A: map many (**default**) | B: copy to PSRAM (overflow) |
|---|---|---|
| Art lives | mmap'd flash, one slot per installed app | packed PSRAM arena, filled from the card at boot |
| Cost per switch | one full-page commit, no flash I/O | copy on the worker, then one full-page commit |
| Hard limit | 256 DROM pages, shared with the image's own 22: two flagships cost 142, so A is not what caps us | PSRAM free bytes after `s_canvas` (434 KB), `s_fb` (434 KB), the tile ring and the tourbillon chord buffers |
| Risk | none at two flagships; a third would be measured before it shipped | OPI bandwidth is already being paid for by the flush path and the canvas restore; sprite reads there can steal the very ms the fps model measures |
| Precedent in this tree | exactly what `art_bank.c` does today, one bank at a time (`art_bank.h:37-41`) | the tourbillon chord caches (`clock_view.c:864`) and the GIF canvas already do this |

A becomes the default because the page maths is no longer in doubt at two flagships: 142 of 256, using the image's measured 22 pages rather than `.rodata` alone. What ships unchanged is the *single-bank* mapping of today; M2 now proves the two-bank case on the board rather than discovering it, and M3 checks A against B for the overflow path only. The ABI hides the choice behind `orb_art_reserve()` either way, so an app never learns which mechanism served its pixels — which is what keeps B viable if a third flagship is ever wanted. **M3 measured that check 2026-10-12 (§14) and the owner ratified A for the overflow bank as D14 (§0.4); B ships dark as P4's escape hatch.**

## 4. Apps, capabilities, and gated configuration

### 4.1 The package

One file per app, mirroring the `.orbtheme` discipline in `tools/orbtheme.py`: all little-endian u32 words, every field widened so no padding rule differs between the bake and the reader, header CRC32 and payload CRC32, verified whole before a byte is committed.

    header    magic 'GRA', FORMAT, GEOM_ABI, hdr_size, serial, baked_ms,
              abi_min, abi_max,                       <- C4: refuse out of range,
              sig,                                    <- C3: reserved, 0 today,
                                                        signing later moves nothing
              slug[24], title[32], hdr_crc            <- whole words, disjoint ranges
    manifest  app_id, behaviour_kind,
              layer/comp/aux/bank counts,
              period_ms (the app's ASK under D32 — D16: set_beat may only move
              within the declared rate set, which P8 adds, D16),
              capability bitmask,
              wasm_mem_max,                           <- D27: refuse at install if the
                                                        module cannot fit the bound
              n_settings, setting_off,
              n_assets, asset_off,                    <- D23: the legible asset table
              geom_off/geom_size/geom_crc, code_off/code_size/code_crc
    settings  per descriptor: id[16], type, min, max, default, def_str[32]
              type in {INT, ENUM, TOGGLE, STRING}     <- C9, frozen now
    assets    per declaration: class id, owner-scoped name, required/optional flag
              <- D23's named slots and D21's fallback contract; the ONE legible
                 surface of an app's drawing, and the preview's only source (D28).
                 The flag is D31's selector: `required` keeps D26's install refusal,
                 `optional` says the app draws its own default for that slot.
    geometry  the app's own records, opaque to the OS  <- D31: this is also the
                 app's DEFAULT layout, the content it draws when a theme carries
                 none for it. The counts above decide its size (C3, item 3).
    code      the wasm3 module (D20). FORMAT 2's `payload`/art words retire.

`min_os_build` is gone (C4): a package carries `abi_min`/`abi_max` and the OS refuses one outside its `GEOM_ABI` with the numbers in the reason, the way `theme_schema.py` refuses an unknown kind. There is no icon or tile field anywhere in the container (C5) — the carousel is text, so a title is the app's whole visual identity. Under D21 the art rule stops being a validation and becomes an absence: **a FORMAT 3 container has no art field to misuse**, so a writer that emits one is refused by the layout itself, not by a check.

The container must keep the three-way mirror rule from `theme_schema.py`: the Python writer, the C reader, and the sentinel test move together, or the test fails rather than shipping a dial with the hands' pivots in the overlay's slots.

Two rules are load-bearing, both copied from the theme path:

- the header's `app_id` must equal the filename stem, checked through one helper reached by upload, scan, boot and stage (`filed_as_its_own_slug()`'s shape), or a mis-filed app reads as a broken installer;
- nothing is committed before the whole payload verifies, and a half-uploaded file stays invisible under a `.part` name that the scan does not glob;
- `tools/orbapp.py` is the only sanctioned writer of the container (C11), standing beside `tools/orbtheme.py`, and browser upload lands at `/sd/apps/<slug>.orbapp.part` then renames through the same `orb_sd` validators. The `/apps` PWA is **deferred** (B3): grants, uninstall and the catalog all have to work from the dial alone first.

### 4.2 Capabilities

The manifest declares what it needs, and the declaration is the consent: the
owner gave it when the app installed (D19, §0.8). Nothing is implicit, and
nothing is asked for at first use — the ask-prompt B3 shipped is gone. The dial
owns the other direction: the PERMISSIONS ladder lists what each app declared
against what still runs, and every row is a revocation the owner can take back.
A cap the manifest never declared cannot be granted at all, from the dial or
from flash. Install-time disclosure — printing the declared list on the install
confirm — is P4's job, and it is the only place consent is now given.

| Capability | Grants | Gate |
|---|---|---|
| `time` | `orb_time_now()`, tz label, tz offset (D17) | free, always on: no app is useful without the clock |
| `config` | its own NVS keys under `orb.apps`, within the manifest's declared settings | always on, bounded to its own namespace, its declared types and the per-app byte quota |
| `power` | a request to change the dim timeout or hold the dial awake | declared at install (D19), revocable on the dial; clamped by `idle_dim`, auto-revoked when the app is not visible |
| `net` | a bounded HTTP client through the OS, allowlisted hosts, rate-limited | declared at install (D19), revocable on the dial; an app that declared nothing never opens a socket |
| `sd_read` | read inside its own `/sd/apps/<id>/` only | declared at install (D19), revocable on the dial; path built by `orb_sd` validators |
| `ui` | the compositor primitives and an OS-rendered options page | free |
| `themes` | enumerate the card's theme entries and request an install by validated id (D18) — never a path, never bytes; the OS package manager owns the assets partition | declared at install (D19), revocable on the dial; the install itself ends in the D11 reboot |
| `art` | its own bank, plus reserve from the arena | free for its own bank |

There is no `sd_write` (C10): an app that needs to persist uses `config` inside its quota, and a card it cannot write to is the same boundary the theme path already enforces from the other side.

`power` and `net` are the two that answer the owner's config-override requirement. The mechanism is a **request, not a write**: the app calls `orb_power_request(timeout_s, hold)`, and `idle_dim` stays the policy owner, applies the Kconfig and NVS ceiling, and lifts the hold when the app is suspended or the owner turns the dim back on. That keeps `idle_dim.h:64-70`'s ownership rule intact: the Kconfig values are the boot defaults, the owner's choice is the truth, and an app is a third input that can only ask.

Enforcement is a single check per entry in `orb_svc_t`, taken from a per-app
word that is the stored revocations ANDed with the declared set, so an
undeclared cap is unreachable from every direction and a VM tier later cannot
bypass it (D19):

    #define SVC(a, cap)  do { if (!orb_grant_ok((a)->id, CAP_##cap)) \n                                  return ESP_ERR_NOT_ALLOWED; } while (0)

### 4.3 The OS service table apps see

    typedef struct {
      /* time */
      void      (*now)(orb_app_t *, time_t *sec, double *frac, bool *synced);
      const char *(*tz_label)(orb_app_t *);
      /* the OWNER's zone, DST-aware, at any instant (D17, §0.7) — minutes
       * east of UTC. The app converts; the zone stays owner config. */
      esp_err_t (*tz_offset)(orb_app_t *, time_t when, int32_t *offset_min);
      /* power: a request only, idle_dim owns the decision */
      esp_err_t (*power_request)(orb_app_t *, uint32_t timeout_s, bool hold);
      /* config: bounded to this app's declared settings, int32 and string (C9) */
      esp_err_t (*cfg_get)(orb_app_t *, const char *id, int32_t *out);
      esp_err_t (*cfg_set)(orb_app_t *, const char *id, int32_t v);
      esp_err_t (*cfg_get_str)(orb_app_t *, const char *id, char *buf, size_t cap);
      esp_err_t (*cfg_set_str)(orb_app_t *, const char *id, const char *v);
      /* net: OS-owned, allowlisted, rate-limited, off the LVGL task */
      esp_err_t (*http_get)(orb_app_t *, const char *url, orb_buf_t *out);
      /* art and compositor */
      const uint8_t *(*bank)(orb_app_t *);
      int  (*layer_add)(orb_app_t *, const orb_layer_desc_t *);
      void (*layer_set)(orb_app_t *, int h, const orb_layer_desc_t *);
      void (*layer_touch)(orb_app_t *, int h, const lv_area_t *);
      void (*layer_del)(orb_app_t *, int h);
      /* worker: never block the LVGL task, same rule as the menu's orb_menu */
      esp_err_t (*job_post)(orb_app_t *, orb_job_fn, void *arg, size_t arglen);
      /* beat: the only callback stays the iface's tick(); this SETS its rate
       * (D16, §0.6). Under D32 this is the ASK, not the ceiling: the OS picks
       * the rung. period_ms >= the declared floor, or 0 = stop ticking until
       * the next resume. A rate outside the declared set is refused
       * with the number in the reason — the install is the cadence budget. */
      esp_err_t (*set_beat)(orb_app_t *, uint32_t period_ms);
      /* themes: enumerate and request (D18, §0.7). id-keyed through
       * orb_theme_id_ok(), never a path; install = D11 through the OS
       * package manager, ending in the reboot ask. cap: themes. */
      int       (*theme_count)(orb_app_t *);
      esp_err_t (*theme_id)(orb_app_t *, int idx, char *buf, size_t cap);
      bool      (*theme_active)(orb_app_t *, int idx);
      bool      (*theme_changed)(orb_app_t *, int idx);
      const char *(*theme_note)(orb_app_t *, int idx); /* damage/trial reason, or NULL */
      esp_err_t (*theme_install)(orb_app_t *, const char *id);
      /* ui: the shell's list primitives for the app's settings page */
      void (*status)(orb_app_t *, const char *fmt, ...);
    } orb_svc_t;

`orb_time_now()` is an extraction, not new logic: `compose_now()` (`clock_view.c:1711-1769`) already resolves the two time sources, the `CLOCK_EPOCH` baseline, the `orb_net_time_synced()` trust rule and the drift re-anchor. That belongs to the OS, every app needs it, and the clock should read it like any other app.

The OS must also hand out the tick instead of letting each app own one. `tick_timer_cb`, `tick_base_ms`, `bg_rate` and the deadline ladder become one OS scheduler whose rate each app sets at runtime — **ruling D16 (§0.6): the iface's `tick()` stays the only callback (no app function pointer enters the scheduler), the manifest's `period_ms` is the app's fastest beat, and `orb_svc_t.set_beat()` may only slow from it or stop; the OS keeps the fastest active beat, measures each tick against the app's declared budget, and parks an app that overruns, logging it.** The shared beat is the fastest active `set_beat`, so two apps on the glass at once during a swap cost the faster one's cadence until the settle — measured under §6's cadence-collapse guard (`docs/fps-budget-findings-2026-10-10.md`) before it ships, because the alternative is two apps asking for 25 fps each reproducing that finding exactly.

---

## 5. What "not running natively" costs

The rule falls out of the measured model:

    flush_ms = 55 fixed + 218 us/row x full-width rows + (185..232 ns/px) x blended px
    (docs/fps-budget-findings-2026-10-10.md, single-variable contrasts; the shipped
     kit3 dial sits at 133..138 ms mean flush, or 123.3 ms on the carriage recipe)

**Interpreted decisions are free. Interpreted pixels are fatal.**

| Path | Frequency | Cost of interpretation | Verdict |
|---|---|---|---|
| Compositor pixel blending (`blend_px`, `paint_row_ctx`, `gif_apply_frame`) | per pixel, ~200k/tick | 20-100x the 185 ns/px native price, so a 450-row carriage tick goes from 123 ms to minutes | must stay compiled C, forever |
| Layer job list assembly (`clock_layer_commit`, `comp_add_jobs`, `aux_add_jobs`) | once per commit, `CLK_JOB_MAX` entries | fetch a record and branch: tens of cycles against a 123 ms commit | free. This is where the declarative interpreter belongs |
| Value derivation (`comp_value`, `aux_value_1000`, `hand_angles`) | once per moving element per tick, integer maths | a table lookup plus a switch instead of a switch: single-digit us on ~30 elements | free |
| App logic beyond the vocabulary (fetch, decide, animate a curve) | once per tick | Lua-class VM: a few hundred ns to a few us per op, so 1-10 ms for real work | acceptable **off** the LVGL task, on its own task with a budget and a deadline; fatal if it can delay a commit |
| App logic per frame on the LVGL task | every tick at 15 fps (67 ms) | 1-10 ms is 1.5-15 % of the beat | measure per app; the OS must refuse an app that cannot make its budget |
| App-authored pixel code | — | no sandbox, no bounds, the same 123 ms pipeline, one wild pointer away from a Guru meditation on a device in a sealed case | only via wasm3 (memory-bounded) or a registered native painter in the OS; never raw uploaded machine code |

Runtime footprints against the 1,476,128 B image in a 2 MB slot:

| Runtime | Code size | Fits? |
|---|---|---|
| Lua 5.4 | about 200-260 KB | yes, leaves about 300 KB of headroom |
| QuickJS | about 400-900 KB | yes, but eats most of the headroom |
| wasm3 | about 60-100 KB | yes, and apps stay C, so the existing toolchain survives |
| MicroPython | about 1.5-2 MB + heap | no, not without repartitioning the slots |

Recommendation: build the declarative interpreter and the broker now (no VM cost, no ABI risk, and it covers the clock plus every watch-style app expressible in the vocabulary). ~~Add wasm3 in P7 only when a real app demands logic the vocabulary cannot carry.~~ — **superseded by D20/D3 (§0.9): wasm3 is committed, P7 is required work, and the container's `code` range exists to carry it.** Do not add native code loading: ESP-IDF 5.4 has no user-space loader, so it means hand-rolling PIC plus a manifest verifier plus a crash-recovery story, and the owner's low-risk tolerance does not change what a bad app costs the dial's frame budget.

## 6. Measurement protocol (the answers this plan refuses to guess)

All three ran on the board with the kit3 art that was compiled in when they were
recorded (§10's `lat: kit3 59 pages` boot lines), before any app code existed,
because a change that regresses the shipped dial invalidates everything above it.
The shipped bank is av-kit (18 pages) since `fb15a96`, so re-run all three against
it before citing these numbers as the shipped dial's — the `ORB_ART_LAT_PROBE` bench
in `main/art/art_bank.c:739-741` measures avkit, d10 and kit3 side by side, which is
what keeps the two sets comparable. Only avkit is re-measurable today: the other two
rows take their byte counts from compile-time constants, and the kits behind them
(kit3, the d10 default) left the tree on 2026-10-12, so restoring them from git
history is the first step of any re-run.

### M1 — switch latency and carousel liveness

Instrument `orb_app_activate()` with `esp_timer_get_time()` at entry and after the first commit, plus `display_bench_snapshot()` deltas and `display_bench_phases_us()`.

    app: switch "steam" -> "orbital": build 4 ms, first commit 142 ms
         (mirror 41, hook 88, te 9, dma 4 ms) rows 452 blends 214k

Pass: full activate under 250 ms; a second switch under 60 ms (objects survive, only the model re-pushes); no new watchdog; the boot dial still reads its baked tempo (8 fps on the flagship bank measured in §10, whose art asks for 16 frames @ 125 ms).

### M2 — residency budget: prove two banks map at once

Mechanism A is the default on arithmetic (section 3.1: 125 of 256 pages), and arithmetic is not a boot. Map, read-back-CRC and keep resident **two** flagship banks, then a third, and record the first count that fails plus the free PSRAM at that instant (`heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`).

    art_bank: 2 banks resident = 125 pages of 256, ok; crc verified through the mapping
    art_bank: 3rd bank refused: need 60 more, have 131-free-but-reserved, psram 6.1 MB
    psram: free at that instant, printed next to it, as the M2 arena cap input

Two results, both wanted. The pass line is that both flagships stay resident with their CRCs verified through the mapping and the dial still hits 6-7 fps, which is what makes a no-reboot carousel real. The second is the PSRAM number, which is the arena cap for mechanism B and for C12's eviction headroom. Both become compile-time caps the installer refuses against with a named reason, the way `ORB_THEME_SLOT_CAP` already works.

### M3 — the overflow path: A versus B for a bank with no slot

With A the default inside the flash slots, M3's question narrows to the bank that has no slot — the third flagship, or an app whose art spilled to the arena. Same art, two builds: A re-maps on switch (one full-page commit and a fresh mmap); B copies into the PSRAM arena on the worker at boot. Compare M1's activate cost and the steady-state fps of the two most demanding apps while rotating. B steals PSRAM bandwidth from the flush path and A costs an mmap call per switch; only the board says which bill is cheaper, and the answer is allowed to differ per bank size. `art_bank_bytes()` plus `display_bench_phases_us()` print the difference.

### M5 — the theme flip: install-and-reboot (D11) versus card-resident lazy load

D30 leaves the multi-bundle flip undecided, and only the board can settle it. Two arms,
same bundles on the card. **Arm A (today's rule):** a pick copies the bundle into the
inactive slot, verifies from flash, marks it live, reboots. Cost is the copy plus a boot;
`orb_theme_post_install` already owns the path and D11 ratified the reboot. **Arm B
(lazy):** the bundle stays on the card and activation maps it in place, or arena-copies
it on the worker, with no reboot. Price what B actually costs: card-read throughput over
SD at 20 MHz, a copy of up to 0x3F0000 on the worker against the flush path, and the
`display_commit_row` stall while the LVGL task waits for pixels that are not resident.
`display_bench_phases_us()` and `art_bank_bytes()` print both sides, and M1's activation
cost is the comparison. The card is the suspect: `.clinerules:153` says the bench has no
WiFi radio and the antenna already cost this session its HTTP timeouts at rssi −62.
**The decision is owed before P6 ships the flip UI, and either answer is allowed to be
per-bundle-size.**

**CLOSED 2026-10-15: §20 rules a third arm, not A or B as drafted here — lazy flip into
the PSRAM arena via a default-first route, flash kept as the cross-reboot cache. The
per-bundle-size allowance goes unused: one arena resident covers every bundle ≤ D29's
cap.**

### M4 — the existing bench limits, from `.clinerules`

- The bench has no WiFi radio (`.clinerules:153`), so an install over the browser is proven only by `tools/host_sd_test.py`-style lifts of the shipped functions (`.clinerules:333-345`), and the card path itself only by the measured table at `.clinerules:444-458`. Any claim about a live install of an app must name which of the two it rests on.
- Long work runs detached (`setsid /tmp/foo.sh </dev/null >/dev/null 2>&1 & disown`, poll the log): `idf.py build` after a Kconfig change, serial flash, and any OTA.
- `/tmp/serflash.sh` pointed at `B=.../Projects/gorb-os/build`, the sibling clone, until this session repointed it at `.../gorb-os-2/build` (filed in `docs/theme-naming-todo.md:123` — a file deleted at §13.8, so the checkbox no longer exists; verify the line yourself). Verify that line before the first flash of an app build: a stale `B=` re-flashes the old image and every measurement becomes a lie.
- Read the IP from the serial log, never from memory (`.clinerules:461`).

## 7. Menu system work

The shell's discipline is right and stays: the armed/unarmed two-level gate, the clamping non-wrapping cursor, the one long-press escape, the worker task for anything that can block, and "show the damaged row with its reason".

| # | Change | Why |
|---|---|---|
| 1 | One registered `menu_page_t` table (title, hint, rows, label, select, back target) replaces the four coupled switches in `menu_view.c`: `page_rows`, `row_text`, `activate`, `go_back` — **landed 2026-10-13 as P5.2.** The row shape grew past the sketch: `rows/label/title/hint/act/open/ordinal/enter/leave/back_to/back_cur`, because two pages whose parent list moves need a function where the sketch wanted an int, and the app page needs an entry hook a generic dispatcher cannot fake. The top level is the sketch's dynamic list minus APPS/ABOUT (P5.3), spelled as a `menu_row_t` sub-table the count reads by `sizeof`. `activate()` turned out liftable once the table absorbed the switch arms — the press half the suite could never test now runs, and six new lints read the table's shape. Costs 720 B: `0x17a140` to `0x17a410`; rendered strings, row order and back cursors are what the suite re-pinned unchanged. The bench frame-diff closed the same day: `tools/bench_menu_walk.py` walked `e74d500` and `185af12` on orb-A, 221 cursor records and 49 checks on each side, 0 failed, and the only diff line between the two logs is the boot's own image stamp | they must agree today and nothing enforces it; `go_back` already hardcodes each page by hand (289-303) |
| 2 | Top level becomes dynamic: AP, TZ, DISPLAY, APPS, ABOUT, then one entry per active app that declares options through the `opt_*` hooks | `top_item_t`/`IT_TOP_N` cannot grow without editing the shell; this is B1/B2's rendering side, and the clock app's theme picker becomes one of these entries rather than a hardcoded PG_THEME. **Landed in full 2026-10-13 as P5.3:** APPS and ABOUT joined `s_top` after PERMISSIONS, so every existing `back_cur` literal stayed valid and the dynamic run plus exit shifted down two by arithmetic nobody edited |
| 3 | `job_t` carries a function pointer plus payload instead of `job_kind_t` | an app posts its own blocking work while the shell still owns the task, the queue, `s_busy`, the mutex and the stack high-water log (`menu_view.c:245-249`) |
| 4 | APPS page: catalog of installed apps, states (`on the dial`, `enabled`, `on card`, `failed: <why>`), actions activate / enable / **uninstall** | B4 puts uninstall in this OS page, not in the app; B5's cap of 8 and the slot arithmetic are enforced here, refusal reason on the row; reuses `orb_theme_scan`'s shown-with-reason rule verbatim. **Landed 2026-10-13 as P5.3.** `PG_APPS` lists the registry's installed tier in the plan's four words (`on the dial` / `enabled` / `on card` / `failed: <reason>` verbatim from `orb_svc_fault_reason`), with a `N of 8 slots used` row. One press cannot carry three doors, so an app row opens `PG_APPACT`: activate through `orb_app_activate()`, enable through `orb_app_enable()`, uninstall through `orb_app_uninstall()` — the same three entries the /apps page calls, reached from the dial for the first time, and each refusal prints the door's own sentence rather than the menu's paraphrase. Uninstall is the one irreversible row on the dial and takes two presses on the same row; any cursor move clears the arm. `PG_ABOUT` is the inert five-fact list. Costs 2,392 B: `0x17a410` to `0x17ad80`. Suite: 169 checks, 67 mutants all caught |
| 5 | ~~Grant prompt: a two-row confirm when an app asks for a capability, persisted per app id, re-answerable from the app's options page~~ — **replaced by D19 (§0.8) 2026-10-13.** The ladder is app → cap, one press per toggle, no confirm page: the consent was given at install, so the row only ever revokes. What remains of this item is the P4 half — the install confirm that prints the manifest's declared list, which is where the consent is actually given | The ask latch and `PG_CONFIRM` shipped as `da0ecd7` and are removed by D19; the persisted per-app word stays, and an absent word now means as-declared rather than all-off, so a wiped partition cannot silently un-consent an installed app |
| 6 | Position readout on long lists (`3/62`), not only the current-row `*` — **landed 2026-10-13 as P5.4.** One composer, `title_line()`, and the gate is the window's own condition (`rows > ROW_MAX`) rather than a flag a page can forget: a page that draws every row says no digits, a page that scrolls cannot avoid them. Costs 112 B: `0x17ad80` to `0x17adf0`. The denominator is `page_rows()`, so the zone picker reads `/63` and not the `/62` this row sketched — the page has 63 rows, the cursor fits on the back row, and a header that could print `63/62` is the worse answer. Suite: 179 checks, 73 mutants all caught, and `repaint_lint` grew a witness that the armed branch paints the composed header, because a `title_line()` nothing calls would pass every pure check. Bench: re-walked on the committed image (49 checks, 0 failed, 229 cursor records) and the log is byte-identical to P5.3's once the boot stamp is normalized — zero diff lines. It cannot see the digits: the log line carries the bare title by design, and its `row N/M` is the same `s_cur`/`page_rows()` pair the header composes. The bench has no framebuffer readback, so the ink itself is host-pinned | TZ is 62 rows and the picker cannot answer "how far am I from the end" |
| 7 | Fix the debt this touches: `h_sd()`'s 2,300 B JSON buffer whose overflow check runs **after** the loop (`docs/theme-naming-todo.md` 88-102), the 23/24/31 name-cap disagreement app ids will hit identically, and gate `/api/v1/screen` behind `CONFIG_ORB_DEBUG_HTTP` — **all three landed 2026-10-12 as P5.1.** The bound check moved inside the row loop with a `too_big` flag and the tail write guarded, so a long volume label can no longer wrap `sizeof json - off` into a huge `size_t`. `stem` is now `ORB_FILES_MAX_NAME + 1` with two `_Static_assert`s pinning the reader cap plus extension inside the volume's name cap and that against the walk buffer; the sentinel derives the width from the header instead of matching a literal. `/api/v1/screen` sits behind `ORB_DEBUG_HTTP` (`depends on ORB_NET_ENABLE`, `default y`) in the handler and the uri table together, and `sdkconfig.defaults` carries the symbol — a new `default y` does not materialize into an existing `sdkconfig` — and the build proved the gate by presence: `CONFIG_ORB_DEBUG_HTTP 1` in `build/config/sdkconfig.h` and `/api/v1/screen` in the image. The whole closure costs 16 B: `0x17a130` to `0x17a140`. | an `off >= sizeof json` break inside the loop closes a real buffer overrun; `host_tz_test.py` +2 lints +3 mutants and `host_orbapp_scan_test.py`'s header battery (3 mutants) are the witnesses |

## 8. Phases

| Phase | Deliverable | Passes when |
|---|---|---|
| P0 | this file agreed and **agreed 2026-10-11 (§0.1)**; freeze the ABI on paper (`orb_app.h`, `orb_svc.h`, `.orbapp` manifest words, `theme_schema.py`'s vocabulary split OS/app) and name the first registered behaviour kinds (section 9.2 — **closed by ruling D15 (§0.5), 2026-10-12: the OS registers none, "only time"; paper at `docs/behaviour-vocabulary.md`**) | the owner signs the vocabulary subset; no code before this line closes — **CLOSED 2026-10-12: D15 signed, the frozen subset is the §4.3 `orb_svc_t` surface alone** |
| P1 | extract `main/gfx/` compositor, delta bank and scheduler out of `clock_view.c`, zero behaviour change; add `orb_art_reserve()`; run M1 and M2 | boot dial byte-identical behaviour, 6-7 fps, `host_comp_math_test.py` and `host_menu_test.py` green, mutations still red — **DONE. Compositor + commit pass + delta bank + frame scheduler all landed and proven identical (`836a0c2`); `orb_art_reserve()` landed; fps bar met at 8 fps on the flagship bank and 15 fps on the aviator stress bank; M1 measured, M2's two numbers taken (§10). `clock_view.c` is down to 1,957 lines of dial policy.** |
| P2 | `orb_app` registry and a dynamic text carousel (C5); clock as app #0, `color_picker` as app #1; `?to=` becomes a slug; the `opt_*` hooks render through the shell (B1/B2); run M3 — **art loader landed 2026-10-11: `art_bank.c` maps and unmaps resident secondary banks behind the page budget, M2 measured (§11); registry, `color_picker` as app #1 and the slug route landed 2026-10-11 (§12); carousel landed 2026-10-11, hardware-verified 2026-10-12 (§12.8), and its settle window replaced by the hand-measured two-window rule 2026-10-12 (§12.9); M1 re-run on hardware (§12.7) — the `opt_*` shell rendering landed and was accepted 2026-10-12 under rulings D11-D13 (§0.3), executed as §13; M3 measured on hardware 2026-10-12 (§14) — **P2 complete** | knob walks both apps **(§12.8: 63-83 ms per activation)**; long-press still homes to the clock (C8) **(§12.8: lands on `clock`, 315 ms)**; ~~the carousel survives a disable/enable cycle~~ — moved to P4 with the `enabled`/`failed` state it needs (§12.6) |
| P3 | broker and grants: `time`, `config` (int/enum/toggle/string), `power` first; on-dial grant prompts (B3); `compose_now`'s anchoring becomes `orb_time_now()`; the dim-timeout request path; the C2 fault counter and boot-into-clock rule — **landed 2026-10-12 as seven commits (`f10b054` broker+gate+config+quota, `b26f1c7` `orb_time_now`+`tz_offset`, `a241463` power request, `3e8ddf2` C2, `d2839ae` status ledger, `bf1272e` D18 themes, `da0ecd7` B3 prompt): `main/app/orb_svc.c/h` beside `tools/host_svc_test.py` (all entries verbatim-compiled and pinned, gates and stubs separately observable); the anchor pair now lives in exactly one file and `clock_view.c` calls it like any app would; grants persist in `orb.grants`, config in per-slot `orb.app<slot>` namespaces under the chosen 2048 B quota (§9.4.4, decided); the D16 beat POLICY (manifest floor, slow-only, stop) ships in the broker — its DELIVERY (the shared tick owning cadence, budget parking) waits for P4's first card app, per D16's own measure-before-shipping rule; the C2 mechanism is a `.noinit` reset record, not a panic hook — IDF 5.4 ships none; the ARM/RUN/disarm protocol bills a live record only when `esp_reset_reason()` blames the code, because the bench's own RTS pulses leave a live record too** | `/api/v1/status` shows per-app grants, call counts and fault count — **landed, ledger `apps[]`**; a denied app degrades visibly — **pinned on host (denial counts, ever-refused mask, `*` mark on the ladder row and in the status ledger; D19 removed the pending latch and the `(ASKED)` badge the earlier flash of this row pinned)**; a faulting app cannot wedge the boot — **pinned on host (two strikes, HOME exempt)**; **bench 2026-10-12, orb-A on `28d9aea`: the dial after the `compose_now` move — booted, `synced true`, epoch advanced across two boots, 15 fps with the usual phase breakdown; the status body measured **1041 B** with three apps on the ledger, inside the 1536 B frame the P3.5 commit reserved (960 B would not have held it); the PERMISSIONS ladder under a real ask — **closed by design under D19 (§0.8), not by the bench: nothing that ships can be asked for, because the declaration at install is the grant. What the bench still owes narrows accordingly — the ladder's rows, strings and toggles are host-pinned and have never been walked on the board under a live owner toggle, and the denial half waits for P4's first card app, which will be the first caller that can be refused at all.** That flash also caught C2's first false accusation: esptool's RTS pulse ends a boot with the reset record live, and the record alone cannot say who ended it, so two flashes marked the OS's own clock FAILED. Fixed in `28d9aea`: the chip's `esp_reset_reason()` decides (PANIC, the watchdog family, lockup, and an unrouted SW bill; UNKNOWN/POWERON/EXT/USB and the supply-side causes do not), an unbilled reset breaks the consecutive chain and heals a mark the chain set from a reset alone, and a fault with a recorded reason keeps its ban. Proven both ways on the board: reason 11 (USB) billed nobody, and a scratch `__builtin_trap` build billed slot 0 every cycle while HOME kept booting.** |
| P4 | the `apps` partition, `/sd/apps` scan, verify-then-header install with reboot (D9), uninstall from the OS APPS page (B4), boot resolve; ~~the three `assets` art slots~~ (retired by D21 (§0.9): no container carries art, so P4b deletes the claim instead of filling a slot); the 8-app cap and slot arithmetic (B5); `net` with an allowlisted client; **and, new under D19 (§0.8): the manifest's `capability bitmask` fills `orb_app_t.caps` at install, the install confirm prints that declared list as the disclosure that carries the consent, and the first card app is the first caller the gate can actually refuse** | the `.clinerules:444-458` install ladder re-run with real apps; refusal reasons reach the dial, not only serial; a power cut mid-install leaves the dial bootable and the app marked failed *(superseded §19.3: it reads ABSENT — D9's ruling, and the mark half is retired)*; **an install that declared nothing shows a `*` on the ladder the first time it calls past its declaration, and the owner's toggle cannot clear it. **Partial, 2026-10-13:** the container is settled and host-pinned — `tools/orbapp.py` is the only sanctioned writer (C11) and `tools/host_orbapp_test.py` holds it with 143 checks and 24 mutants, all caught. That fixes the word map, the caps validation, the identity triple and the disclosure text. **Landed since, 2026-10-12:** the owner approved the §3.2 layout — the `apps` row (`data, 0xD80000, 0x280000`, ending at exactly 16 MB) is in `partitions.csv`, the board's table was pushed once at `0x8000` and reads back byte-identical to the built binary on orb-A, every pre-existing row kept its offset, and the D19 ladder re-ran 34 checks 0 failed on the repartitioned dial. The C reader shipped as `main/app/orb_app_manifest.c` with its sentinel `tools/host_orbapp_reader_test.py` (48 vectors, 7/7 mutants caught): device refusals and writer refusals are now pinned to each other in the same words. **The `/sd/apps` scan shipped the same day as `main/app/orb_app_scan.c` with its sentinel `tools/host_orbapp_scan_test.py` (16 vectors, 7/7 mutants caught): `row_decide()` is lifted verbatim and compiled against the shipping reader, the two card facts with no writer twin (header-block truncation, file short of claimed art) are pinned in both directions including the tolerate-longer and accept-boundary rulings, and the shown-with-reason rule is inherited from `orb_theme_scan` whole. It stopped being inert on 2026-10-13: `orb_app_scan_init()` registers the drop zone on the boot ladder beside `orb_sd_init`, and `orb_net.c` serves `GET /api/v1/apps` (rows streamed, the per-row bound recomputed by the sentinel from the shipped headers) and `GET /apps` (the card listing; the disclosure line is `orbapp_caps_names()`’s answer and nothing else — a second list is one of six caller-side mutants). Sentinel: 16 vectors, 13/13 mutants caught. The board caught what no host vector could: the row array as an internal static cost the httpd task its 8 kB stack (ESP_ERR_HTTPD_TASK on orb-A), so the array now heap_caps_mallocs from PSRAM and the sentinel pins the residency the same way the residency suite pins the art banks.**Landed 2026-10-13 (`aecc258`, `d2174f2`): the install POST with its card button, the boot-time resolve from the slot, and the browser→card upload route — `POST /api/v1/sd/app`, the theme upload's mirror (§15). D19's `orb_svc_grants` loading needed no new code: `orb_svc_init` already masked every stored word against whatever `caps` the table held, so filling `caps` from the header at both upsert sites made the gate card-aware on contact — the bench's installed row reports `declared: []`, and `orb_svc_declared()` answers the header's zero, not a stored word. **Landed 2026-10-13 (P4a): the enable path and uninstall (B4).** The enable state is one u32 `e<slot>` in the `orb.grants` namespace — same owner-door provenance, same slot keying, opposite default (absent means off, because the install confirm promises off). Card rows key on their FLASH slot by construction (`orb_app_t.flash_slot`; the card table places every row at its slot's index and `orb_app_rows()`/`row()` went POSITIONAL — a free slot answers a NULL hole), so uninstalling one app never renumbers another's grant, fault or enable words and no migration exists to get wrong. main.c grew a rung, `orb_app_install_apply_enable()`, between `orb_svc_init()` and `orb_app_init_all()`: resolve_boot stays partition-reads-only and the NVS partition is not up before the broker. `POST /api/v1/apps/enable` and `POST /api/v1/apps/uninstall` answer synchronously — one NVS key and one 4 KB header sector, no worker needed where the install's multi-second card copy was — and the `/apps` page grew its "On the dial" tier (Turn on/off, Uninstall, on-glass warning) streamed on every response path, card or no card. Enable persists the word BEFORE the RAM flip (a crash keeps the decision on disk), refuses the row that is on the glass, and turning ON a `failed` app is `orb_svc_clear_faults()`'s first device caller — C2's owner door, opened. Uninstall is erase-header → `orb_svc_forget_row()` → free the entry: D9 makes the erased slot an absent app at that byte, no reboot, blob bytes stay until the slot's next copy erases them (B5 counts live headers), and the owner's card file is untouched. A fresh install into a REUSED slot wipes its stale owner words; an upgrade of the same slug keeps them (D19). A card row whose behaviour module is already built reuses that screen instead of re-running `init()` (C6 modules are singletons; `color_picker_init` is not idempotent). Sentinel: six new lint rules and six new mutants (16/16 caught), and the svc suite gained the enable round-trip and the forget-the-row battery. Still open after P4a: the dial's own APPS-page mirror of the two doors (P5's table-driven menu), a standalone clear-failures door for reviving a failed app WITHOUT enabling it, and the bench pass lines below; P4b's art-claim deletion LANDED the same day (`e66c0f0` + `e74d500`, the P4b row); the three `assets` art slots are **withdrawn, not owed** (D21 (§0.9): installs carry no art, `art_size` stays 0 and `art_slot` stays -1 — deleted at P4b), and the D16 beat delivery (§9.5, unblocked the day the enable path makes a card app walkable). P4a's sentinel claim needs its correction noted here too: the six P4a mutants were authored OUTSIDE the runner's list and never executed — P4b folded them in (install now runs 25, all caught). **Two P4 pass lines stood unbenched; the power-cut half closed 2026-10-15 (§19.3): the cut landed, the app read ABSENT exactly as D9 promises, the dial booted, recovery re-installed in 40 ms — and the pass line's `failed`-mark wording is RETIRED, because D9's write order leaves no row to mark; only C2's counter and `init()` refusal can mark, both still blocked on a second behaviour kind.** The `*`-on-the-ladder line remains unbenched for that same blocker — the C6 picker module makes no `orb_svc` call, so a `caps=0` picker app can never accrue a denial. |
| P4b | the container sheds its art claim for good: `art_size`/`art_req`/`art_crc` stop being read by the installer, `art_req` with no payload inverts from refusal (`orb_app_manifest.c:317`) to a valid D21 declaration, and the gate-2 refusal text changes | no container can offer art to an installer that will not take it; the sentinel pins the inverted gate and the writer drops the field. **Landed 2026-10-13 (`e66c0f0`, fix `e74d500`). The refusal lives once, in the reader: `orbapp_check_header` refuses any container with `art_size` or `art_crc` non-zero ("carries a %u B art payload: no container ships art (D21) — re-package it"), mirrored in the same words in `orbapp.py`; `art_req` survives as D18's request-as-declaration, bounded only by the old slot cap as a mis-count guard, and `art_req>0, art_size==0` is now the ACCEPT vector (the inversion the pass line named). The installer lost gate 2 entirely (gates renumbered), the art CRC loop in `verify_payload`, the `h->art_size` fill into the registry row, the `art_crc` term from the upload dedup tuple, and `APPS_ART_SMALL_MAX` with the art term in `ORB_APP_UPLOAD_MAX` (now geometry + 128 KB slack: the largest legal file cannot carry pixels). Scan's truncation fact moved to the copied prefix (`geom_off+geom_size`) — the art claim can no longer extend it because no file the reader admits has one. Writer: `write_container`/`pack_header` have no art argument, `disclosure()` has no art line — the owner consents to no pixels because there are none. Sentinels: writer 147 checks / 24 mutants, reader 50 vectors / 9 mutants (two new: the D21 gate dropped, the bound doubled), install 25/25, scan 17 vectors / 13 mutants — all caught. The fold also righted two lies the P4a close had told itself: install's six "new" P4a mutants were dead tuples sitting OUTSIDE the `MUTATIONS` list (the count that read 16/16 never ran them — folded in, plus the stride mutant re-anchored to the uninstall site where the first-occurrence `_m` actually hits the install copy), and scan's caps mutant and terminator mutant lost their uniqueness to P4a's second caps emitter and the three shared handler closes — re-anchored, with the word-check and the terminator assert now scoped per-body, both green. Bench: §15 P4b record — the live D21 422 arrived verbatim, and it caught a third thing: both header validators had been returning their refusal from a frame-local buffer, so the door printed garbage where the sentence should be (the `#\x04\x06` the fix commit is named for) — `static char why[160]` in both, pinned by a new lint rule and its own mutant.** |
| P5 | table-driven menu pages, generic worker jobs, the position readout, the 3 debt fixes, **and the `layer_*`/`bank`/`status` broker stubs get bodies** (§1.4 reserved them and `orb_svc.h:115` named P5 as their blocker) — **decomposed 2026-10-12 into five separable commits, and P5.1 is in.** **P5.1 (landed, `df87abc`): the three debt fixes of §7 item 7.** **P5.2 (landed, and benched 2026-10-13): the `menu_page_t` table** replaces `page_rows`/`row_text`/`activate`/`go_back` (§7 item 1; item 2's AP/TZ/DISPLAY/PERMISSIONS/apps/exit half — APPS and ABOUT join as `s_top` entries in P5.3). `host_menu_test.py` was rebuilt around the table: 131 checks including a press block that drives the lifted dispatcher the stub used to fake, 48 mutants all caught, six structural lints over the table's text. The lifted labels/titles/hints are the shipped ones, so identical rendered text is pinned by the suite rather than by eye. **P5.3 (landed 2026-10-13): APPS + ABOUT rows** (§7 items 2 and 4). Three pages — `PG_APPS`, its one-app `PG_APPACT`, and the inert `PG_ABOUT` — reusing `orb_app_enable()`/`orb_app_uninstall()`/`orb_app_activate()` as a second caller rather than new business logic. Bench: walked on the flashed P5.3 build — 49 checks, 0 failed, and the top page's app row derived as 6 where it had been 4, the two new OS rows accounted for exactly. The re-walk landed afterwards: the P5.4 image carries P5.3's pages and its walk log matched this one, so the pending line is closed. APPS/ABOUT's own rows are host-pinned until the walk grows checks for them. **P5.4 (landed 2026-10-13): the position readout** (§7 item 6). `title_line()` composes the armed header and gates on the window's own `rows > ROW_MAX`, so a page that scrolls counts itself and a page that fits says nothing — no per-page flag to forget. 10 new checks pin the exact text at the boundary in both directions, 6 new mutants, 73 of 73 caught, suite 179 checks. Costs 112 B: `0x17ad80` to `0x17adf0`. Bench: re-walked on the committed image — 49 checks, 0 failed, 229 cursor records, and the log is byte-identical to P5.3's once the boot stamp is normalized. The digits are host-pinned: the cursor log line keeps the bare title so the walk's `row N/M` stays the pair the header composes, and the bench has no framebuffer readback. **P5.5 (landed 2026-10-13): the `layer_*` bodies** (§1.4, shaped by §1.2b). The per-app table lives in `main/gfx/orb_layers.{h,c}` with generation-tagged handles, row-owned PSRAM planes behind an installed allocator hook, and a job builder that keeps table order as paint order; `orb_svc.c` grew the four gated bodies, the suspend/uninstall wipes, and the live/refused ledger pair the status endpoint now prints per row. `gfx_prim_ctx_init()` — the entry `orb_assets.h` had promised since P5b — is real, and it lives in `orb_layers.c` so the suite can drive it (see §1.2b's closing note). `tools/host_orblayers_test.py`: 73 vectors over the shipped table and registry with the shipped bake, 10 source-rewrite mutants all caught, mirror lints pinning the two stubbed compositor maths against `orb_gfx.c`'s text. Firmware builds clean: +2,944 B flash. **Bench-corrected 2026-10-14 (§16): the embedded tables died on the board — 14,720 B of .bss answered `httpd_start: ESP_ERR_HTTPD_TASK` and the config server stopped. `fix(svc)` moved each row's table to a lazy PSRAM calloc (16 x 920 B returned to the heap, +256 B flash), freed at row-forget, kept across suspend, with a per-row refusal word so the ledger counts refusals made before a table exists; 5 new host vectors pin the paths the embedded shape did not have.** No commit-hook caller yet — that is P6, as recorded. Bench state on `fix(svc)`'s image: heap region back to 136 KiB, `config server listening`, boot-to-dial 1.98 s; the status-body read of the `layers` pair awaits the radio fix in §16's checklist. No commit-hook caller yet — that is P6, as recorded. Item 3 needed no P5 work: `orb_jobs_post(fn, arg, arglen)` is already the generic payload carrier, and `menu_view.c` has called it that way since before P5 opened | `host_menu_test.py` rewritten for the table, all mutations red; the compositor primitives reachable through the gate and counted by the ledger |
| P5b | **new (D22/D23):** the asset registry. The frozen class enum (`text`/`sprite`/`hand`/`plate`/`ring`/`bank`/`layer`/`status`) versioned through `abi_min`/`abi_max`; `<slug>/<name>` vs `global/<name>` resolution and its two fixed orders; the manifest's legible asset table; the five **generated** fallback primitives (no shipped pixels, D22); `orb_app_manifest.h/.c` and `tools/orbapp.py` gain the table together | a declared-optional asset resolves global-then-fallback and a declared-required one refuses at install (D26), all on the host; a card app draws a ring and a hand with zero theme bytes present. **D31 names the owner of that second clause:** the app supplies the default content and theme content wins wherever a bundle carries it. P5.5's `layer_*` bodies sit behind this row, because the descriptor's art reference is a registry handle and nothing else is legal |
| P6 | **rewritten (D21/D24/D29):** `.orbtheme` FORMAT 3, the theme as a multi-app bundle. The app-to-slot binding map, the 0x3F0000 budget gate at both bake and install, several bundles listed on the card, the flip-on-orb over them (D30 ruled §20: default-first lazy copy into the PSRAM arena and the ping-pong slot in one fused card pass, progress bar on the glass, no reboot, Arm A kept only as the arena-full fallback), and the retirement of the clock's FORMAT 2 dialect. The compiled-in `clock_assets` pair stays the card-less fallback, and `bake_clock.py` gains the reverse path §0.2 still owes. **Design draft in §22, rewritten in part by D32 (§23): the per-app section carries a legible ELEMENT TABLE, not an opaque blob. R1–R7 are all ruled or consumed (§23.6, §24). P6.1–P6.3 landed: the format and the gate at both doors, the refill with its timing figure, the `staged` word, and the streaming ingest under a parity family (§24.6). P6.4 landed and benched 2026-10-19 (§24.8, §24.9) — the flip door, its worker, the scan's bundle row, and the store half of the page, with M6(1)'s streaming rate (4398 ms at av-kit size) and M6(3)'s bundle-half cut proven on orb-A. **P6.5 CLOSED 2026-10-20: all four owed items are PAID — §25.5(1) the bundle→pair reverse path, §25.5(2) the FORMAT 3 device reader, §25.5(3) the FORMAT 2 retirement (`7c0fd4b`, §27), §25.5(4) `mut_rebake.py`. The FORMAT 2 dialect is out of the firmware reader and writer; the door is FORMAT 3-only. P6 is DONE.** The rate ladder and the picker sit in P8 (§23.5)** | the shipped av-kit source kit re-bakes to a FORMAT 3 bundle that boots, a FORMAT 2 file is refused at the format gate with no fallback reader, and a card-less board still tells the time |
| P6b | **new (D28):** orb-manager's theme and app manager. Bundle editor over the firmware resolver's format, preview rendered from the manifest's asset table at real scale, the budget meter, upload of app files and bundles, and release packaging of the cross-product | an editor-built bundle installs on the first try, and the preview's pixels match the dial's at the same coordinates |
| P7 | **promoted from optional (D20/D3):** the wasm3 tier over the same `orb_svc_t`, the module carried in the container's `code` range, and `wasm_mem_max` gated at install (D27) | a wasm app that traps twice is marked failed and the dial boots to the clock, exactly as C2 already specifies for a faulting compiled app |
| P8 | **new (D32, split ruled 2026-10-15; numbered P8 because P7 is the wasm3 tier):** the OS schedules the repaint. The manifest carries an explicit rate SET (R7) and the app names a runtime ask; the OS picks the highest achievable rung not faster than the ask, else the set's floor, re-picks dynamically under a hysteresis rule, and drives per-element repaints through `layer_touch`'s damage rects. R6's advance mode becomes a compositor behaviour. **Its first commit is D16's beat delivery** — the machinery §0.6 deferred for want of a consumer, which D32 finally supplies. | a two-app board where each stack holds its rung while the other is parked, a measured rung-vs-rows table replacing §13.4's single 8 fps ceiling, no flicker across a rung boundary over a 60 s sample, and the dial's own hands unaffected while a second app is scheduled |
| Shelve | the three-slot `assets` re-slice, the per-app bank claim, D18's art-banking installer path, `apps/clock/` directory split | void under D20/D21 — do not build these |

## 9. Resolved, and what is still genuinely open

Rulings B1–B5 and C1–C12 are in section 0.1 and are not open questions. What remains is only what the board can answer or what a second app will force:

1. **M2's two numbers. — answered 2026-10-11, §11.** Five banks (the live dial's,
   both installed payloads, two 59-page stress banks — 206 art pages) stayed resident
   with CRCs verified through the mapping while the dial held its 15 fps baseline, and
   free PSRAM at the refusal was 7.9 MB. The plan's pass line held; what stays open is
   the MMU's true refusal point (the probe never reached it — §11's third finding),
   which M3 measures with a second partition. **— answered 2026-10-12, §14.2: the hunt
   mapped all 212 distinct pages the table can offer (assets, www, both app slots) with
   ~99 pages already resident and refused zero times; below flash capacity the allocator
   does not refuse at all, so budget 234 stands as contract, not ceiling.**
2. **The behaviour vocabulary's first cut.** **— answered and signed by the owner 2026-10-12 as ruling D15 (§0.5): none. The OS exposes only time.** `orb_svc_t`'s §4.3 surface is the whole registered ABI — `now(app, sec, frac, synced)` handing the raw `time_t`, `tz_label`, config, the power request, the compositor primitives — and no `CLK_SRC_*` name crosses the broker: apps convert seconds to local time themselves and own any calendar bug. The SOURCES/KINDS/FLAGS/HOSTS/aux enums freeze as the bake-to-app wire interface. The signable paper, its reference-derivation table and findings G1-G5 are `docs/behaviour-vocabulary.md`.
3. **Whether the compiled-in fallback is one app or several.** **Answered for every app by D31 (2026-10-13):** the default is each app's own code, so a second flagship adds no baked-in pair and no image growth against the 1,476,128 B figure in section 3.1. Today `assets/clock_assets.h` guarantees a card-less board tells the time (section 1.1) and stays that board's fallback, which is what `use_baked_in()` reports as `from_container == false`. Ruling D10 (§0.2) settled the clock's own default art: the OS ships none, and the clock app's bundled default is the Design-ID-sourced container at `assets/sources/clock-app-default/`, recorded now and wired by P6.
4. **NVS quota per app** against the 24 KB `storage` partition with `string` settings enabled (C9) and 8 apps allowed (B5): the byte budget is a number to be chosen at P3 with the real key shapes in hand, then enforced at install. **— decided at P3, 2026-10-12: 2048 B per app (`ORB_CFG_QUOTA_BYTES`, `orb_svc.h`). The real key shapes came first: NVS's 15-char key limit ruled out slug-keyed anything, so config lives in one namespace per registry slot (`orb.app<slot>`, keys are the setting ids themselves) and the quota is charged on write with refund on overwrite, persisted beside the values at `"_used"`. Worst case 8 x 2 KB plus the OS namespaces' ~1 KB fits the partition's 6 sectors with NVS's sector-granularity waste absorbed. P4's installer enforces it at install against the manifest's declared strings, and refuses the package, not the write.**
5. **The D16 beat delivery, owed to P4's first card app.** The broker ships the beat POLICY today (`set_beat` may only slow from the manifest's `period_ms`, or stop; the floor is `orb_app_t.period_ms`), but the cadence DELIVERY is still the P1 scheduler driven by the clock's policy struct — one app, one tick. The shared-beat change (the OS scheduler taking the fastest active `set_beat`, measuring every tick against its declared budget and parking overrunners) lands when there is a second app to police, measured under the §6 cadence-collapse guard before it ships, exactly as ruling D16 (§0.6) itself demands. Until then the compiled-in clock keeps its ladder and no app can move the shared beat, which is the safe direction: the policy already refuses a fast ask that the delivery would not honour. **— still owed. P4a's enable path removed the walkability blocker: a card app can now be enabled, policed and cycled. What remains is a second BEAT — the bench `hsb_color` declares `period_ms 0`, so the shared-beat delivery waits for an installed app with a declared tick.**
6. **The theme-class promotion procedure (D23).** A class enters the frozen enum by PR, and `abi_min`/`abi_max` carry the version, but the review bar is unwritten. Open until a second app actually asks to share a class: who decides a use case is legitimate, and what a rejected class costs the requester.
7. **Which compiled renderers generalize (D22's real list).** §1.1 already puts the compositor, the delta bank and the layer commit on the OS side, and `orb_gfx.h` already exposes `gfx_trigo_sin`, `gfx_rot_sin_cos`, `gfx_fill_sprite_dsc`. What stays clock policy is the derivation side: `comp_value`, `hand_angles`, the tourbillon, strip composition. The open work is P5b's enum naming pass over that line, not a fight over whether the line exists. **— done at P5b, 2026-10-13, as §1.2a.** The line exists and holds; four functions move to `main/gfx/` (`aux_angle_10`, `comp_frame_index`, `tourb_chords`, `tourb_stamp_pose`) because each is a class *selector* the wire already fixes, and everything else named stays app policy. The pass also corrected two stale symbols the §1.1/§1.2 rows cited: `clock_layer_commit` is at 982-1051, not 1636-1700, and `s_row_snap`/`paint_layer` no longer exist (§10 records the P1 renames).
8. **The asset-ownership record for self-deletion.** D21 removes app-carried art, so "an app deletes its own themes" needs a persisted answer to which bundle an app installed, and `orb.app<slot>` has room for it. Deferred with the deletion ruling (prior round's item 5), and it does not block P4a, P5 or P5b.

---

## 10. P1 result — what landed 2026-10-11

**Landed.** `main/gfx/orb_gfx.h` (165 lines) and `main/gfx/orb_gfx.c` (568) are the OS
compositor, carved out of `clock_view.c`, which fell from 2,974 to 2,441 lines. gfx owns the
rotation and blend maths (`gfx_trigo_sin`, `gfx_rot_sin_cos`, the private `blend_px`/`mix_16_16`,
`gfx_paint_ctx_t`, `gfx_paint_ctx_init`, `gfx_paint_row_ctx`), the swept-box geometry
(`gfx_swept_box`, `gfx_swept_rect_box`, `gfx_area_union`, and `SWEPT_PAD` — which the header now
owns, because the tourbillon's orbit chords must pad by the same value the compositor's box
carries), the sprite-record helpers, the row mark set (`gfx_mark_rows` plus a `gfx_rows_t` read
view), the staging band, and the whole commit pass: `gfx_commit()` restores each marked row from
the canvas, paints the job list, byte-compares against the mirror, and DMAs only what came out
different. `clock_view.c` keeps the policy — which layers exist, at what angles, in what order,
plus the complication and sub-dial job lists — and ends `clock_layer_commit()` with one call into
gfx. That call is the whole app-side surface today.

`blend_px`/`mix_16_16` stayed file-static inside gfx on purpose: they are the measured
185-232 ns/px loop, and keeping `gfx_commit()` in the same TU preserves the inlining they had
when the hook lived in the clock. `gfx_blend_px` exists only so the DISPLAY_BENCH synthetic ink
can still reach the paint window, now through `gfx_set_band_hook()` instead of a call from the
hook that moved.

`art_bank` gained the residency accounting P1 promised: `orb_art_reserve()` (accounts, never
maps — mapping secondary banks is P2's job), `art_bank_pages_for()`, `art_bank_mmu_pages()`,
`orb_art_resident_count()` and `orb_art_reserved_bytes()`. A request that is not a multiple of
the 64 KB MMU page is refused, since a ragged span wastes a page at each end of the mapping.
`/api/v1/status` answers `art_pages` and `art_banks` beside the existing `art_ready`/`art_bytes`,
and the boot line now prints the page cost — measured on the bench:
`bank "assets" @0x830000, 1978973 B mapped of 8388608 B (theme blob 1978973 B), 31 MMU pages`.
Section 3.3's page arithmetic is now a number the board printed, not one the plan asserted. Free
PSRAM was already reported as `heap_psram`, so it was not duplicated under a second name.

**Landed same day, second commit (`836a0c2`).** `main/gfx/orb_sched.h` (149 lines) and
`main/gfx/orb_sched.c` (589) are the delta bank and the frame scheduler, carved out of
`clock_view.c`, which fell from 2,441 to 1,957 lines. The bank owns the GIF-mode PSRAM canvas
(`s_canvas`, `s_canvas_dsc`), the classic plate-frame bank (`s_bg_dsc`, `build_bg_dscs`,
`bg_frame_now`, `show_plate_frame`), the frame cycling and the push (`gif_apply_frame`), and the
three pacing predicates (`gif_mode`, `gif_tick_ms`, `bg_live`); the scheduler owns the `lv_timer`
with its deadline ladder (`tick_timer_cb`), `tick_base_ms`, `bg_rate`, the commit nudge
(`request_commit`), `l4_active`, and the per-second throughput report (`report_fps_if_due`).
`gfx_commit()` already took the canvas as a parameter, so the bank moved without touching the
paint path again.

The seam is `orb_sched_policy_t`, installed at `orb_sched_start()`: the tick calls the app's
`compose_now()`+`tourbillon_advance()` pair through `.tick`, and the L4 full-box re-mark and the
bench plan through `.mark_all`/`.bench`. This TU includes no clock header, so the dependency
stays app → gfx and never inverts — the same shape as `gfx_set_band_hook()`. Three things stayed
in the app deliberately. `s_bt_ctr/sec/frac` stayed with `hand_angles()`, its only reader: the
plan's original P1 line listed `hand_beat_ticks` under the scheduler, but the code graph shows
the tick never touches it — it is sampled where the angles are made, so moving it would have
meant inventing an accessor for one caller. `mark_all_layers()` stayed because it walks the
app's own layer and shadow arrays. `l4_active()`'s eleven mark-site callers are policy-side; the
predicate itself moved and is exported.

**Zero-behaviour-change proof, mechanical.** `tools/gfx_parity_test.py` lifts each moved
function out of the pre-extraction `clock_view.c` (the paint path pinned at `786efcd`, the last
revision whose clock still held it whole; the bank and the tick pinned at `3116942`, two
revisions later, because `orb_gfx.c`'s own move had already rewritten `request_commit` and the
fps report's per-role loop — comparing those against `786efcd` would read someone else's change
as this one's) and out of `orb_gfx.c` / `orb_sched.c`, canonicalises the renames, the row-state
consolidation and the policy callbacks, and compares token streams. 13 paint routines and 14
bank/tick routines, all identical. It fails on anything but the accepted delta, and it was
checked to fail four ways: shifting one bit in `gfx_rot_sin_cos` (`>> 5` to `>> 4`) returns rc 1
naming the function, and so does `s_gif_due += d;` becoming `s_gif_due = d;`, the deadline
ladder's `- 8` becoming `- 9`, and the `gif_tick_ms()` floor's `d < 20` becoming `d < 21`. The
last three are pure scheduler, invisible to any dial-pixel test.
Of the 28 comparisons, 26 are identical, including the entire commit band loop. The two that
differ are `gfx_fill_sprite_dsc` and `gfx_paint_ctx_init`, and both differ only by the injected
`const uint8_t *base` parameter: the bank base is threaded in per call so gfx never learns about
art_bank. That is the intended delta and the only one.

**Verified on the bench, not just on the host.** The aviator 15 fps bank was the stress case,
since it is the only theme whose tick is over-subscribed. Pre- and post-extraction builds were
flashed back to back over the same serial link, same theme, same 62 s capture: fps median 15
both, p10 12 both, mean 14.41 vs 14.33; flush median 60 ms and p90 77 ms both; px/s median
1,212,532 vs 1,231,172; chunks/s median 167 vs 165. The boot dial prints the identical lines:
`bg gif delta bank: 15 frames @ 67 ms (14.9 fps), tick 67 ms` and `composite: baked static
matte`. All eight host suites are green (`gfx_parity`, `host_tz`, `host_sd`, `host_comp_math`
at 5933 vectors, `host_bake_comp`, `host_idle_dim`, `host_knob`, `host_menu`), and the app builds
clean under `-Werror`.

**Gates.** `idf.py build` clean, zero warnings under `-Wall -Wextra -Werror=all`.
`host_comp_math_test.py` green at 5,933 vectors with all 14 mutants caught; `host_menu_test.py`
green with its 3 mutations caught. Both suites lift complication and sub-dial maths — app code
that did not move — so the extraction left the benches' lift anchors alone, which is what the
context note about `host_comp_math_test.py` breaking at P6 will have to revisit.

**On the bench.** Flashed app-only to ota_0 through `/tmp/serflash.sh` (EXIT=0), boot log captured
over an RTS reset with `/tmp/p1/cap.py`. Clean: no `E (`, no Guru, no abort. The dial runs the L4
path — `hook 435 (rest 23 paint 390 cmp 17) ms/s` and `blend/s by layer: hr 16054/31 min 17468/220
sec 4105/8 ... glass 198085/246` — so the moved code is doing the real compositing and the
per-role counters cross the TU boundary correctly.

**fps: the bar is met, measured on a flagship bank 2026-10-11.** An earlier version of this
section declined the claim because the bench held only the `skeleton` theme
(`8 frames @ 250 ms (4.0 fps)`, which it held exactly). The owner has since loaded
`perpetual-calendar`, and the same build now reads:

    art_bank: bank "assets" @0x430000, 2503747 B mapped of 8388608 B (theme blob 2503747 B), 39 MMU pages
    orb_theme: theme "perpetual-calendar" from slot-A, 2503747 B of pixels
    clock: 3 hand(s), shadow on(0,0), sweep on(sweep, every tick), bg gif delta bank: 16 frames @ 125 ms (8.0 fps)

Steady state over 13 one-second samples past the warm-up: **8 fps** (min 8, peak 9 — it
reaches the tempo the art asks for, so the plan's 6-7 fps bar is exceeded, not just met),
947,844 px/s, flush avg 78 ms of the 125 ms tick — 62% spent, 38% of the tick unspent.
The breakdown says where the remaining budget goes, and it is not the bus: of the
compositor's 467 ms/s, `paint` is 415 (89%), `cmp` 19, `rest` 25, while DMA is 108 ms/s
(13.5 ms per flush) and TE 61 ms/s (7.6 ms). Row-major paid for itself, and the next
percent of frames is in the blend loop, not in the SPI path. Per layer: `glass` 440,334 px/s
at 146 ns/px and `dial` 424,250 at 114 ns/px are the two costs; the three hands together are
133,000.

**M1: measured — 2026-10-11, three clock-to-menu-to-clock round trips.** Taken over
`GET /api/v1/screen?to=…` on the connected bench (`sta ip 192.168.2.32`, rssi -56). An
earlier note here said the route was unreachable because "the bench has no radio"; that
misread `.clinerules:136-166`, which says the *host* has no radio and so cannot play the
phone joining the AP. The orb's own station joins the LAN and its routes answer curl, and
`.clinerules:161` lists exactly that. A switch between two already-resident views costs:

| theme / direction | synchronous | request to glass | the commit it provoked |
|---|---|---|---|
| skeleton, clock -> menu | 0 ms | 75, 75, 91 ms | 1, avg 68-80 ms |
| skeleton, menu -> clock | 0-1 ms | 256, 262, 266, 273 ms | 1, avg 239-255 ms |
| perpetual-calendar, boot -> clock | 1 ms | 464 ms | 1, avg 446 ms |

No reboot is involved in either direction, so D5's rule ("no reboot to rotate between
resident apps") is satisfied by the only switch the firmware has today. Two things the
numbers say, which the first version of the probe hid by sampling its counters inside the
switch (a 1 ms window at a 250 ms tempo holds no commit, so it printed `frames 0` and
printed display's all-time flush maximum as if the switch had caused it):

1. **The app's half is free.** 0-1 ms for the pointer swap plus the widget rebuild. An
   app host can spend the whole budget on locating a view and none on drawing it.
2. **The asymmetry is the compositor's, and it is a full-damage commit.** Entering the
   clock repaints every marked row (~250 ms), while the menu's first commit is ~70 ms.
   The steady-state commit at this theme's 250 ms tick costs 74-84 ms, so the switch
   costs ~3x a normal tick of the same compositor work: event-only row marking keeps
   steady frames small, and a switch loses that advantage because nothing has a valid
   delta bank yet. The flagship row scales that effect with the art — a cold first commit
   is 446 ms at 8 fps against 256 ms at 4 — so M3's two-app rotation must be judged against
   the ~273 ms figure at 4 fps and the ~450 ms one at 8, and P2's bank swap must not add to
   either. Warming a delta bank instead of discarding it is the lever.

**M2: the two numbers are measured; the residency test itself is P2's.** With the flagship
resident and the dial running its own 8 fps tempo, `/api/v1/status` reads
`heap_psram 7,471,404` free and `heap_internal 26,111` (largest block 9,216). So the
mechanism-B arena cap and C12's eviction headroom have a real budget to be cut against
instead of an estimate. The page arithmetic is now checkable against a measured bank rather
than a guessed one: at 64 KB per page, this 2,503,747 B bank is 39 pages, so two flagships
cost 78 of the **234 pages art actually gets** — 256 less the 22 the image's own `.text`
(17) and `.rodata`/`.appdesc` (5) occupy, per `idf.py size`. §3.1's original "125 of 256"
understated the fixed cost by 17 pages by counting `.rodata` alone; corrected there, and the
conclusion (A is not what caps us) holds with more room than claimed. Where the cap bites is
the slot, not the theme: two *maximum* 8 MB banks cost 128+128+22 = 278 of 256, so
`orb_art_reserve()` refuses on the running total — implemented and its decisions replayed.
Whether two mapped banks *actually* hold 8 fps under animation is still unmeasured: the
loader cannot map a second bank until P2, and no number is claimed for it here. That the
page figures above are size-report arithmetic rather than a reading of the live MMU table is
exactly what M2 has to confirm.

---

## 11. P2's art loader, and the M2 residency result — 2026-10-11

**Landed.** `art_bank.c` is no longer single-bank. The static mapping is joined by a
resident table — `art_bank_map_extra(offset, bytes, &slot)`, `art_bank_extra_ptr/bytes`,
`art_bank_extra_count`, `art_bank_unmap_extra` — capped at `ART_BANK_EXTRA_MAX` = 8
(one bank per installed app, ruling B5; the page budget, not the table, is what
runs out for flagship art). A re-request of a live offset returns or grows that entry
rather than billing a second region — same-app-asking-again, not a second copy. The
reservations are a per-bank table now, and a map of the same page-rounded size
consumes its booking, so reserve-then-map charges the window once;
`art_bank_mmu_pages()` sums each live map and each outstanding booking separately,
because each is its own virtual region. The primary's contract is untouched: still
`art_bank_init()`, still never unmapped, so every cached LVGL descriptor in
`clock_view.c` keeps working and the dial's boot path is byte-identical to P1's.
An app hands its bank to the compositor through the seam P1 already cut: whatever
pointer the app passes to `orb_bg_init()` is just `art_bank_extra_ptr(slot)` instead
of `blob()`, and gfx still never learns `art_bank.h` exists. `/api/v1/status` needed
no change — its `art_pages`/`art_banks`/`art_refusals` read the same accessors, and
they now describe a real multi-bank window.

**M2: measured on the bench, probe compiled out afterwards.**
`ORB_ART_M2_PROBE` (art_bank.h, the `DISPLAY_BENCH` one-line switch) maps the primary
plus the two installed payloads still on flash — slot A's perpetual-calendar at
0x10000 (2,503,747 B, 39 pages) and slot B's at 0x410000 (1,978,973 B, 31 pages) —
books both sizes through `orb_art_reserve()` first to prove the consumption rule,
CRCs every bank through the mapping against chunked `esp_partition_read` ground
truth, then pushes 59-page stress banks until the budget itself refuses. Boot log,
verbatim (`/tmp/p2/boot.txt`, fw `6e91c47-dirty`):

    I (716) art_bank: bank "assets" @0x420000, 1173563 B mapped ... 18 MMU pages
    I (716) art_bank: m2: start — primary 1173563 B (18 pages, resident); budget 234 of 256 pages, image claims 22
    I (717) art_bank: m2: booked A 2555904 B: ESP_OK
    I (717) art_bank: m2: booked B 2031616 B: ESP_OK
    I (1177) art_bank: m2: bank A — 3 banks resident = 110 pages of 256, ok; crc verified through the mapping
    I (1541) art_bank: m2: bank B — 3 banks resident = 110 pages of 256, ok; crc verified through the mapping
    I (2249) art_bank: m2: stress bank — 4 banks resident = 169 pages of 256, ok; crc verified through the mapping
    I (2958) art_bank: m2: stress bank — 5 banks resident = 228 pages of 256, ok; crc verified through the mapping
    E (2958) art_bank: m2: stress bank 3 refused (ESP_ERR_NO_MEM): need 59 more, art holds 206 of 234 budgeted; psram 7.9 MB

Both of the plan's numbers, and both wanted results. (1) Two flagship-sized banks
plus the live dial's own — five banks, 206 art pages — stayed mapped, CRC-verified
through the mapping, and the dial ran at its requested tempo under them: the same
boot's `clock N fps` lines read 15 for nine consecutive seconds and 12-16 after
(60 lines sampled), matching the P1 aviator A/B baseline of median 15, with no
watchdog and no reboot. The no-reboot carousel is real on this board. (2) Free
PSRAM at the refusing instant: 7.9 MB — mechanism B's arena cap and C12's eviction
headroom input, measured before the canvas, `s_fb` and the tile ring claim theirs,
which is the same basis §10's `heap_psram` figure uses and the same caveat applies.
`/api/v1/status` after boot read `art_pages: 206, art_banks: 5, art_refusals: 1` —
the budget's decision, the residency it granted, and the refusal are all visible
from the LAN without the serial cable.

**And one finding the plan did not ask for.** The probe's second loop maps every
distinct 64 KB page of the 8 MB partition, unaccounted, looking for the flash MMU's
own refusal point — and never found one: all 128 requests succeeded, 127 of them to
fresh pointers, one folded (page 0 returned the primary's own base, which is the
mmap layer deduping an identical physical request; it is also why the first version
of this loop, hammering one offset 256 times, "succeeded" without measuring
anything). Together with the 22 pages the image claims and the 206 of art already
resident, the window accepted 356 region-pages' worth of mappings without a single
ESP_ERR_NO_MEM from the allocator itself. Those numbers are region sums and the
spans overlap, so this bounds requests-accepted, not entries-consumed — but it is
already past §3.1's 256-page figure, which is therefore a `soc/mmu.h` region-count
reading, not the capacity this allocator refuses at. It was never the binding limit;
the 8 MB partition was, and still is. `ORB_ART_PAGE_BUDGET` 234 stays as the enforced
contract — it is what the installer's refusal reasons are cut against, and a
conservative budget that refuses early is a bug report, not a liability. The true
refusal point is unmeasured: the partition ran out before the MMU did. M3 should
find it with a second mmap-able region (the www partition is at hand) — that number
decides whether the budget can rise, and it belongs there, not in the loader.

**Verified.** `idf.py build` clean under `-Werror` with the probe at 0 and at 1;
all eight host suites green after the change (the reserve semantics — per-bank
entries, consumption on map — are not host-liftable, so the boot log's page
arithmetic stands as their check: 57 → 88 → 88, no double-billing, across
book+map); shipping-image boot re-captured after the flip-back — no `m2:` lines,
same primary map line, same dial ramp. The aviator swap in `assets/` remains
uncommitted and unrelated, as filed under P1.

**Reclaim latency: measured on the bench, probe compiled out afterwards (2026-10-12, `0623239-dirty`).**
The Step-5 owner question — release an app's `art_bank_map_extra` reservation on
suspend (release) or carry it while parked (carry) — turned on an unmeasured number:
what does the re-claim actually cost? `ORB_ART_LAT_PROBE` (art_bank.h, the same
one-line switch as `ORB_ART_M2_PROBE`) times `art_bank_map_extra()` (the reclaim a
resume runs) and `art_bank_unmap_extra()` (the release a suspend runs) over 31
map/unmap pairs per bank, at page-aligned offsets clear of the primary, fully
unmapped between samples so every timing is a cold re-claim. Boot log, verbatim:

    I (955)  art_bank: lat: start — primary 1855055 B (29 pages, resident); budget 234 of 256 pages; 31 iters/bank
    I (974)  art_bank: lat: kit3    59 pages  3768 KB | RECLAIM map_extra   min    305 median    316 max    403 us  (~5 us/page median)
    I (975)  art_bank: lat: kit3    59 pages  3768 KB | RELEASE unmap_extra min    250 median    265 max    326 us  (mean 266 us)
    I (975)  art_bank: lat: kit3   one switch (reclaim+release pair): ~581 us median
    I (994)  art_bank: lat: d10     41 pages  2588 KB | RECLAIM map_extra   min    302 median    318 max    409 us  (~7 us/page median)
    I (995)  art_bank: lat: d10     41 pages  2588 KB | RELEASE unmap_extra min    249 median    269 max    335 us  (mean 270 us)
    I (995)  art_bank: lat: d10    one switch (reclaim+release pair): ~587 us median
    I (1019) art_bank: lat: avkit   18 pages  1146 KB | RECLAIM map_extra   min    458 median    508 max    555 us  (~28 us/page median)
    I (1019) art_bank: lat: avkit   18 pages  1146 KB | RELEASE unmap_extra min    213 median    241 max    268 us  (mean 237 us)
    I (1020) art_bank: lat: avkit  one switch (reclaim+release pair): ~749 us median
    I (1020) art_bank: lat: done — 1 banks accounted (29 art pages; every lat bank unmapped, primary intact)

The answer to the owner question is unambiguous on the latency axis: **every op is
sub-millisecond** — worst observed single op 555 µs, worst reclaim+release *pair*
(one suspend, one resume) ~749 µs. The cost is dominated by the fixed
cache-invalidate/MMU-walk, not the page count: `esp_partition_mmap` installs the
bank's page-table entries and copies nothing (no CRC in this path), so a 59-page
reclaim (316 µs) is *faster* than an 18-page one (508 µs) — the small bank mapped
last, into a more-churned MMU free-list, and absolute µs tracks allocator state, not
size. Read against §12.7's M1 figures this is noise: an app already activates in
63-87 ms and the dial's own return is 411-476 ms, so the ~0.3 ms the release choice
adds on suspend and ~0.3 ms on resume are ~0.4% of an app-switch and ~0.07% of a
dial-return. **Reclaim latency is not a reason to carry.** The real cost of release
is therefore not µs-on-the-glass; it is the budget-pressure failure mode (§12.4: a
re-claim that returns NULL under pressure paints garbage) and the headroom carry
would consume (§12, the 234-page budget that M2 watched refuse at 206). The
`lat: done` line proves the probe is self-cleaning — every bank it mapped it
unmapped, MMU back to primary-only (29 pages) — and the same boot's `clock 12 fps`
lines after it confirm the dial runs at tempo untouched. `idf.py build` clean under
`-Werror` at the probe both 0 and 1; shipping-image boot re-captured after the
flip-back — no `lat:` lines, same primary map, same dial ramp.

---

## 12. P2's first half — the app registry lands (2026-10-11)

`main/app/orb_app.{h,c}`, ~500 lines of registry plus a 43-check host suite. §1's
three hardcoded couplings are gone: `main.c` builds whatever the table lists, the
router walks the table, and `?to=` resolves through it.

### 12.1 What actually changed

| Was | Now |
|---|---|
| `main.c` called `clock_view_init()`, `color_picker_init()`, `menu_view_init()` in a fixed order | `orb_app_init_all()` walks `s_apps[]` in order; table order IS boot order, so the clock's worker still comes first and the invariant moved into the table instead of being deleted |
| `input_screen_t {MENU, COLOR, CLOCK}` + five switch statements in the router | `orb_app_neighbour()`, `orb_app_current_armed()`, `orb_app_input()`, `orb_app_disarm_all()`, `orb_app_home()` |
| `orb_net.c` tested three literals against an enum | `orb_app_find_slug()`, exact slug before alias |
| long-press hardcoded `INPUT_SCREEN_CLOCK` | `orb_app_home()` reads the `is_home` flag (C8, as §1.4 anticipated) |

An entry whose `init()` fails is dropped from the carousel with its reason
logged; the boot continues. `ESP_ERROR_CHECK` on an app init would turn a broken
app into a bricked device on a board in a sealed case.

The table's three entries: **clock** (`is_home`, primary bank), **color_picker**
(app #1, `alias="hue"`, vector-only), **menu** (`ORB_APP_KIND_SHELL` — OS chrome
that walks in the carousel, not an app). Tagging the shell page rather than
faking it into apphood is what keeps B5's cap and the uninstall page honest:
only `ORB_APP_KIND_APP` rows are uninstallable.

### 12.2 The one deliberate behaviour change

The knob cycle was menu→picker→dial. It is now dial→picker→menu, because the P2
pass line asks for "knob walks both apps (clock → color_picker → clock)".
Everything else — the two-level gate, one step per poll, the direction coming
from `knob_col_step`, long--home — is unchanged, and `tools/host_menu_test.py`
still pins each of those.

`?to=clock|menu|hue` all still work: two are slugs, `hue` is the picker's
registered alias, so `/tmp/p2/cap.sh`, `tools/idle_watch.py` and the bench table
in `.clinerules` did not need editing to keep functioning.

### 12.3 The ABI moved from §1.4's paper list, by three hooks

`activate`/`suspend`/`resume`/`input`/`armed`/`set_armed`/`service`/`opt_*` are
§1.4's, with the same meanings. Three additions, all because these entries are
compiled in rather than shipped on the card:

- **`init`** — §1.4 has `activate` build the widgets, which is right for a VM app
  handed a container. A compiled app's widgets are built by its own `*_init()` on
  the boot ladder, where `main.c` can order the clock's worker task before the
  LVGL task exists. `activate` is therefore the *pre-load* half: claim art,
  restart the tick.
- **`screen`** — §1.4's "activation returns its owning obj", spelled for the tier
  that already has one.
- **`show`** — the post-load model re-push. It cannot fold into `activate`: LVGL
  invalidates on `lv_screen_load()`, and the order that has always run is
  park-outgoing → restart-incoming tick → load → re-push. The picker's chosen
  colour surviving a trip to the dial depends on that order specifically.

Nothing in the scheduler, the arming gate or the residency path reads `init` or
`screen`, so a VM-tier app that never supplies them is still ABI-complete.



### 12.4 The refusal paths, found by writing the test

The registry's art-claim failure paths were where the real bugs were, and the
host suite caught one on first run: a refused activation suspended the outgoing
app and then restored it via `resume`, which the clock did not implement — so
the dial stayed on the glass with its tick parked. A refused switch would have
left a frozen clock, caused by an app that never appeared.

Three refusals now share one `app_restore()`: art claim refused, `activate`
refused, no screen to load. Each re-claims the outgoing bank, restarts what its
`suspend` parked, and returns the code. The re-claim can itself fail (the budget
that refused the incoming bank is the same budget), which is logged as the
emergency it is: an app on the glass with no bank reads `NULL` from
`art_bank_extra_ptr()` and paints the silent garbage the loader's header has
been fencing off since it was written. `activate` returning non-NULL is not
enough; an unimplemented `resume` had to be caught by something other than the
glass.

**The release-vs-carry question (the residual on this step), now closed with a
measured number and a test.** The suspend path releases a banked app's secondary
(`app_art_release`, `orb_app.c`); the alternative was to carry it while parked,
sidestepping the re-claim that can fail above. Measured on the bench (§6,
`ORB_ART_LAT_PROBE`), re-claiming a flagship is ~320 µs and a full suspend+resume
pair ~580-750 µs — sub-millisecond, ~0.4% of an app switch's 63-87 ms, so latency
is not a reason to carry. **Owner ruling 2026-10-12: keep release-on-suspend.** The
parked app holds no MMU pages (so a banked app is never refused because a parked
one is squatting the window), and the cost is the NULL-reclaim branch below, which
is now pinned on the host by `tools/host_app_registry_test.py` — the app left ON the
glass ends with `art_slot == -1` after a switch whose rollback also fails, the
`art_bank_extra_ptr(-1)` garbage paint `art_bank.h` fences off, asserted rather than
only logged. It stays dead code for the shipping path until P4's first banked app
(§12.5), but the registry's handling of it is now covered.

### 12.5 Two findings this step settled

**`art_bank_extra_ptr()` has no shipping consumer yet, and will not get one from
P2.** `color_picker` is vector-only (LVGL draws the whole editor; 327 lines, no
bank), and ruling C5 makes the carousel text-only, so nothing P2 introduces owns
a secondary bank. The dial's own bank is the PRIMARY mapping, taken by
`art_bank_init()` before `display_init()` and never unmapped, because
`clock_view.c`'s descriptors cache raw pointers into it. `orb_app_t.art_bytes`
is therefore zero for all three entries today: the claim/release pairing is
implemented and host-tested, but its first real caller is P4's first banked app,
not the carousel. An earlier reading of this step ("wire the dial's bank
through `art_bank_map_extra`") would have moved a mapping the compositor cannot
survive losing, so it is recorded here as not happening.

**The residency arithmetic has no host suite.** `orb_art_reserve`,
`art_bank_map_extra` and the page-budget checks in `art_bank.c` are exercised on
the bench by the M2 probe's log and nowhere else; no `tools/` file referenced
them before this step. My registry suite drives the *registry's* pairing through
stubs, which is not the same claim. The loader's own arithmetic — page rounding,
the 234-page budget, reservation consumption, refusal counting — is untested on
a host, and `art_bank.c`'s behaviour after a bulk flash is not a rebuildable
fact. Flagged, not fixed here: it belongs with P3/P4, where a second banked app
makes it observable.

### 12.6 What P2 still owes

| Item | Note |
|---|---|
| Text carousel (C5) | **DONE on hardware 2026-10-12 (§12.8), ratified and re-specified by the owner's hand the same day (§12.9)**: a burst inside the settle window costs ONE activation and ONE full-page commit, detents spaced beyond the READ window cost one each, and a press commits a named peek instantly. The single 400 ms window of §12.8 is gone — measured read-pauses (510-1936 ms) made it commit under the reader. Its first real caller of `art_bank_unmap_extra()`/`map_extra()` in the shipping path is still P4 — see 12.5 |
| `opt_*` through the shell (B1/B2) | **Shell rendering DONE and accepted 2026-10-12 (§0.3 D11-D13).** This row's earlier claim — "NULL everywhere; tiles painted by the clock's own view code" — was stale when written: the clock implements all six hooks (`clock_themes.c:184-263`), the shell renders them (`menu_view.c` `PG_TOP`/`PG_APP`, §7 item 2 done, `PG_THEME` gone from the enum, menu list and knob walk share one table order), and `host_menu_test.py` exercises the path. What closed the item were the owner's three follow-up rulings — unconditional install-and-reboot on pick (D11), one-level sub-menus (D12), default-theme slot protection (D13) — which shipped as §13 |
| APPS page (B3/B4) | Needs the manifest and catalog (P4) to have anything to list |
| Disable/enable CRC round-trip | Testable once an app owns a bank, since it is the remap path that must CRC-verify |
| **M1 re-run** | **DONE on hardware 2026-10-11 (§12.7)**: apps activate in 63-87 ms, second switch flat, zero watchdog, and an md5-matched A/B shows the dial's tempo unchanged at 7 fps median either side. The dial's own return costs 411-476 ms, which is its full-page commit, not the switch |
| **Clock app bundled default (P2 clock theme-options E2E)** | **Owner ruling D10 (§0.2), 2026-10-12; committed-source clause revoked 2026-10-12.** The OS ships no art; the clock app carries its own bundled default, ingested from the SD card. Its source of truth was Design ID `WCD5-W1sic2…`, the compiled `clock_app_default.orbtheme` (a FORMAT 1 file: `read_container` refuses it at its format gate, though its stored header and art CRCs still match when recomputed directly), and the `skeleton_theme.zip` layer kit the ID referred to; those three files sat in `assets/sources/clock-app-default/` and now exist only in git history, verified by the sha256 in §0.2. It is **recorded, not wired**: booting it as the clock's art needs the P4/P6 `.orbapp` container, the `apps`-partition ingest, and a container→`clock_assets.{bin,h}` reverse tool that do not exist yet. Its archived container is FORMAT 1, so no current orb can read it at all. Under FORMAT 2 it would also collide: the name field of that container holds `skeleton`, which is the name of a theme already on orb-A's card, and FORMAT 2 files both under the same `theme_id("skeleton")`, so the device refuses the second one as a duplicate name until the operator sends `?overwrite=1`. The compiled-in `assets/clock_assets.{bin,h}` pair (av-kit, source kit committed at `assets/sources/av-kit/`) stays the card-less boot fallback and has never itself rendered on hardware — slot headers validate first |

M1 is captured (§12.7) and the carousel is captured (§12.8), so the registry is
host-proven and hardware-proven, and C5's deferral is hardware-proven. The `opt_*`
item is **accepted** (D11-D13, §0.3; work order §13). What keeps
P2 open is M3. The pass line's third clause, "the
carousel survives a disable/enable cycle," is **not testable at P2 and is not a
blocker**: the registry exposes no enable/disable/set-failed entry point at all
(`orb_app.h`'s surface is init/activate/arm/service/opt), `enabled` is a plain
field written from the compiled table, and `orb_app.h:129` defers `enabled` and
`failed` to P4. The clause belongs to the P4 catalog, where the state exists.

### 12.7 M1 re-run against the registry — captured 2026-10-11, `8535154`

Kit3 (`3,858,874 B`, `8 frames @ 125 ms`), ota_0, serial flash, wall-powered,
`sta ip 192.168.2.32`, **rssi -73**. Screens driven over `GET /api/v1/screen`,
which is the same `set_screen()` the knob reaches.

| Switch | sync | on glass | commits |
|---|---|---|---|
| → color_picker (first ever) | 0 ms | +76 ms | 1, avg 65 ms |
| → menu | 0 ms | +87 ms | 1, avg 76 ms |
| → color_picker (2nd) | 0 ms | +70 ms | 1, avg 63 ms |
| → color_picker (3rd, via `hue`) | 0 ms | +63 ms | 1, avg 57 ms |
| → clock (return to the dial) | 1 ms | +411 / +476 ms | 1, avg 394-462 ms |

**Verdict against the four pass lines.**

1. **Full activate under 250 ms — passes for an app** (63-87 ms), and the clock's
   return is the one case that does not: 411-476 ms. That number is not switch
   cost. `clock_view_set_bounce(true)` forces a clean repaint, which is one
   full-page commit, and kit3's full commit is 403 ms in `.clinerules`' own
   measurement; §10 measured the same shape at 256-273 ms on the skeleton bank
   and 464 ms on perpetual-calendar. The switch adds roughly 8-70 ms on top of a
   commit the dial was always going to pay. The bar is met by apps and missed by
   the dial's own reveal, for a reason owned by the dial.
2. **Second switch under 60 ms — passes where it means anything.** Sync is 0 ms
   every time, and the to-glass cost is flat across first/second/third entry into
   the picker (76/70/63 ms), which is the proof that objects survive and only the
   model re-pushes. The glass figure cannot go below ~60 ms at all: it is one
   commit of a 466-wide band, and the average commit measured 57-65 ms.
3. **No new watchdog — passes.** Zero `guru`/`abort`/`watchdog`/`stack overflow`
   in both boot captures; clean `boot: Loaded app from partition at offset
   0x20000`.
4. **Boot dial at baked tempo — passes on the only comparison that is valid.**
   The A/B, same bank (md5-identical `aba381f3…`), same rig, same session:

   | image | stamp | one-second samples | fps median | flush avg median |
   |---|---|---|---|---|
   | pre-registry | `24fc3c2` | 44 | 7 | 133 ms |
   | registry | `8535154` | 45 | 7 | 132 ms |

   Zero delta, so the registry costs the dial nothing. The absolute 7 fps sits
   below kit3's documented 8.32 (`1999911`'s table), and the honest reading is
   rig state, not code: rssi is -73 tonight against -56/-61 in those runs, and
   this rig's established rssi sensitivity says the flush cost roughly doubles by
   -84. Named rather than explained away — the number to reproduce after the next
   antenna-side change is 8.32 with rssi above -60.

Also proven on hardware: `?to=color_picker` and the `hue` alias both switch, and
`?to=bogus` answers `{"error":"unknown app","known":[…]}` with a 400 without
echoing the caller's value. The registry costs **2,256 B** of app image
(1,478,240 against 1,475,984 for the same tree one commit earlier).

**Bench caveats worth repeating.** DHCP did not complete on the first retry boot
(no `sta ip` line at all), so the run needs a reachability poll rather than a
fixed sleep. The dial auto-dims at 60 s on this build, so the usable lit window
is t≈12-58 s; every figure above is from that window, and `frames` in each fps
line is a one-second count, not the cumulative total.

---

### 12.8 C5's carousel on hardware — captured 2026-10-12, `71947dd`

The ruling-C5 promise is that a flick costs one full-page commit instead of one
per detent. `host_carousel_test.py` proved it against a synthetic clock and
stubbed leaves; this proves it on the board, driving the shipping build through
`tools/bench/carousel_bench.py`. Detents are fired as separate
`GET /api/v1/screen?turn=+1` requests, which reach the router through
`input_router_inject()` — the same fold the encoder's own detent uses
(`input_router.c:202`) — so from the fold down the path is the shipping one: real
LVGL task, real registry, real display, and the commit count read from the
router's own M1 probe line (`input: switch X -> glass +N ms: N commit(s)`).

Run: orb-A (`44:bd:8d:60:80:88`, ARP-confirmed by the tool's own gate), `fw
71947dd`, slot ota_0, `art_banks 1`, `art_refusals 0`, rssi -56 to -69 across
three passes. The tool reads the two constants under test **from the device's
boot log** (`chip ready (settle 400 ms)`, `router running (poll 10 ms, 3 walkable
app(s))`), so if `ORB_CAROUSEL_SETTLE_MS` changes the control case moves with it
instead of silently testing a window the board does not use. Exit 0, all cases
held.

| Case | Detents | Device-side gap | Activations | Commits | Glass |
|---|---|---|---|---|---|
| coalesce-2 | 2 | 97-206 ms | **1** | **1** | 73-80 ms |
| coalesce-w+1 | 4 | 148-247 ms | **1** | 1 | 74-80 ms |
| wrap-w | 3 (= walkable) | 105-209 ms | **0** | — | — |
| control-2 | 2, spaced beyond window | 635-720 ms | **2** | 1 / 1 | 67-77 ms |
| control-3x600 | 3, spaced beyond window | 726 ms | **3** | 1 / 1 / 1 | 64 / 70 / 294 ms |
| hold-c8 | long-press from `color_picker` | — | lands on **`clock`** | 1 | 315 ms |

The differential is the whole result. Two detents inside the window: one
activation, one commit. The same two detents spaced past the window: two
activations, two commits. Without the control, an infinite window (never
switches) and a zero window (switches every detent, i.e. the pre-carousel
behaviour) would both pass the coalesce row. Five tight detents also cost one
activation, after one mid-burst wrap-cancel.

**Falsification checked, not assumed.** Forcing the control's spacing inside the
window (300 ms) must break the suite, and it did: `control-2 ... acts=1 want 2 |
FAIL`, exit 1. A second guard proved itself unprompted — one pass returned `VOID
(HTTP jitter pushed detents to 446 ms)` rather than claiming a coalesce it had not
tested. That matters because HTTP round-trip at bench rssi is the noise floor: a
requested 25 ms spacing arrives as 92-446 ms apart, so the x-axis is the device's
own `synthetic turn` timestamps and a drifted burst is voided, never passed.

**Bracketing the window.** Coalesced at gaps up to 326 ms observed; split at 510 ms
and above. Both straddle the coded 400 ms, so the constant behaves as written. The
bracket is ~180 ms wide because of the HTTP jitter above, which is why this
**cannot** ratify the number — see the limit below.

**Two findings the pass produced.**

1. **A full-circle flick does nothing, by design.** With three walkable entries
   (`clock → color_picker → menu`), a burst of exactly three names its way back
   onto the entry already on the glass, and `orb_carousel.c:164` cancels rather
   than commit a phantom switch — `wrap-w` prints 0 activations and one
   `cancelled on … — no switch`, and the hops counter resets. The host suite already
   pinned the no-phantom rule; what is new is the ergonomic consequence the table
   makes visible: on a three-app carousel, a three-detent flick returns the user
   exactly where they started. Correct, but worth knowing before a fourth app is
   not in the table.
2. **Switch cost reproduced, dial return still the outlier.** Picker and menu
   activate in 63-83 ms against §12.7's 63-87 ms, so the carousel added nothing to
   the switch. The clock's own return is the expensive one again: 294-315 ms here,
   411-476 ms in §12.7 — the dial's full-page commit, not the switch.

**What this run cannot say, stated by evidence.** Synthetic turns bypass the
encoder: `/api/v1/status` reads `raw 0, det 0, spd10 0, vhits 0, shits 0` after the
31 injected turns of the first pass, while the walk visibly happens. Injection
enters at `input_router_inject()`, downstream of `knob.c`'s valley/bounce logic, so
**the gap distribution of a real hand is not measurable this way**, and the 400 ms
window is therefore still un-ratified against fingers. It is derived from
`KNOB_SETTLE_MS` (30 ms, `knob.c:82`) and the picker's `VEL_IDLE_US` (220 ms), not
measured — `orb_carousel.c:40-47`'s own note, flagged "not owner-ratified" when the
module landed. Ratifying it needs a hand on the knob while the encoder telemetry is
read, which is a two-minute ask rather than an engineering task.

**That two-minute ask was made the same day, and it killed the single window.**
The hand measurement, its verdict, and the two-window replacement that stands in
the shipped code today are §12.9. Nothing in this section's bench record is
withdrawn — every case it lists still passes — but "still un-ratified" in the
paragraph above is now history: the number was ratified, found wrong at human
speed, and replaced.

Health: zero `Guru`/`abort()`/watchdog, zero `no display commit within` (the
parked beat was never leaked — the commit handover and the cancel-unpark both held
across 46 fired detents), and the dial resumed its 10-15 fps baseline after the
run. The device was re-flashed to `71947dd` first: it had come up on
`8eb4bf0-dirty` (branch `main` plus uncommitted edits), which the `serflash.sh`
audit caught as a stamp mismatch before anything was written.

### 12.9 The settle window, measured on a hand — 2026-10-12, ruling A+B

§12.8 closed with one debt: 400 ms was arithmetic, not a feeling. The owner turned
the knob on orb-A while `tools/orb_watch_ser.py` captured the carousel's own
`peek`/`settle`/`cancelled` lines — the timestamps are the device's, so the
numbers below carry no host-side clock — across two passes, with encoder
telemetry confirming a real hand each time (`det -16, spd10 42, shits 0`;
injection would show zeros, §12.8's proof). Logs: `/tmp/hand_flicks_20261012.log`,
`/tmp/hand_flicks_20261012b.log`.

**The measurement.** Pass 1 was free-style: 12 detents, every inter-detent gap
510-1266 ms, every one committed alone — coalescing never once engaged at human
speed, and the feature was a no-op for exactly the use it exists for. Pass 2
split the two regimes deliberately: 3 fast flicks of 3 detents each, and a slow
walk of single detents with a read-and-decide stop at each.

| Regime | Gaps observed (ms) | Against the old single 400 |
|---|---|---|
| Fast flick, detent-to-detent | 33-157 (n=9) | coalesces, 2.5x headroom |
| Slow read-stop, detent-to-detent | 510-1936, median ~1000 (n=24 both passes) | every one committed early, under the reader |
| peek -> commit latency | 400-408, all 19 commits | the timer itself is faithful |

The medians of the two regimes sit 15x apart, and the closest pair (157 vs 510)
still bracket 3x. **No single number can serve both**: any window above the
longest read-pause taxes every clean flick by ~2 s (past the ~1 s
feels-dropped line, and past the host suite's own `<= 800` ceiling); any window
below it dumps the reader onto an entry mid-decision — which is what the hand
experienced at 400. An earlier 800 ms compromise from this thread is withdrawn:
it coalesced the pass-1 gaps only by treating the reader's pauses as bursts, and
still committed early on the 1266-1936 ms tail.

**Owner ruling A+B.** (A) two windows, chosen per detent by the gap that
precedes it; (B) press-to-commit, so nobody waits on a timer they have already
beaten. B turned out to be already shipped — `input_router.c`'s "that one, now"
branch has committed a named peek on press since P2 — but no bench had ever
exercised it; it is now (press-1 below). The owner's same-day follow-up turned
the press from commit into **enter**: rotate to name, press once, and you are
inside the named app — the commit path calls `arm_current()` on the switched
entry, so the HSB editor or menu list opens with the same press (the state a
press on an already-showing screen produces; the dial stays inert because it
has no armed view). The auto-commit at window expiry deliberately does NOT
arm — an expiry that entered would make every settled switch a room the user
cannot turn the knob out of, so the bench asserts the absence of `armed` on
the coalesce/control cases and its presence, name-matched to the activation,
on press-1. Arming happens only when the switch landed: a refused
`set_screen` leaves the old view unarmed, because arming it would arm a
screen the user never named.

**The shipped rule** (`main/app/orb_carousel.c`):

| Constant | Value | Protects against |
|---|---|---|
| `ORB_CAROUSEL_SETTLE_MS` | 400 | in-burst commit delay; floors: encoder 30 ms recovery, picker 220 ms velocity-idle, 2.5x widest flick |
| `ORB_CAROUSEL_BURST_GAP_MS` | 300 | the classifier, cut between the two regimes (157 < 300 < 510), strictly-under comparison |
| `ORB_CAROUSEL_READ_MS` | 2000 | the hold for a detent that landed after a pause — or alone, a pause the shell never saw end; covers the 1936 ms observed max |

A lone first detent is deliberately a read, not a burst: the walk-away case pays
2 s for a switch nobody is waiting on, and the reader who has finished presses.
`cancel()`/`commit()` reset the classification, so the first detent of the next
gesture is a new gesture — pinned by a host case, because a predicate placed
after the pending assignment silently makes every detent look in-burst.

**Tests.** `host_carousel_test.py` gained the two-regime re-arm (fast window
counted from the in-burst detent itself), the read-window walk (1000 ms pause:
not due at 2999, due at 3000, walk continues from the named entry), the exact
`BURST_GAP` boundary direction, the post-cancel new-gesture rule, and bounds
tying all three constants to the measured regimes (`157 < GAP < 510`,
`READ >= 1936`, `READ <= 2500`). All 11 host suites green.

**Bench re-pin and the pass run.** The board now publishes all three windows in
one boot line (`chip ready (settle 400 ms, read 2000 ms, burst gap 300 ms)`),
and the bench takes its case spacing from the device, never from this file. The
control moved beyond the READ window (detents spaced between the two windows
coalesce legitimately now — a control that coalesces is not a control), the
tight-burst VOID re-pinned to the burst gap, the control gained its own VOID
for jitter that failed to clear READ, and `press-1` landed: one detent, a press
0.6 s later, the activation TIMED to prove the press pre-empted the auto-commit
(a count alone cannot tell them apart). 2026-10-12, `6188cd1-dirty`, orb-A,
rssi -58: coalesce-2 `seen 215, 1 act, hops 2, glass 87 ms`; coalesce-w+1
`seen 165, 1 act`; wrap-w `0 acts, 1 cancel`; control-2 `seen 2494, 2 acts,
71/83 ms`; hold-c8 lands clock; press-1 ok. All assertions held. With
rotate-press-enter the same seven pass on `e2926c2-dirty` (rssi -61): press-1
`1 act, armed color_picker`, control-2 `2 acts, no armed line` (the negative
arm pin), coalesce-w+1 `peeks+canc == 4 steps`. The tight-burst VOID earned
its keep in between: one run delivered only 3 of coalesce-w+1's 4 requests —
two HTTP arrivals clustered into one poll lose the extra step to the
one-step-per-poll rule — and was reported VOID, not passed and not failed.

**Precondition fixed on the way: the association wedge.** Three bench boots in
a row went network-dead for minutes at ~7.4 s, recovered only by pulsing EN.
The capture named the mechanism exactly: the self-inflicted reason-3 leave at
boot arms a 5 s retry; the association then reaches `run`; the uncancelled
timer fires `esp_wifi_connect()` into the live-but-unleased station, gets
`ESP_OK`, and destroys the association with no event left to re-arm the ladder
— both old guards (`s_sta_up`, `s_provisioning`) are false in that window by
definition. The fix (`main/net/orb_net.c`): a new `WIFI_EVENT_STA_CONNECTED`
handler cancels a pending retry at the association (not at the lease — the
unleased window is where the wedge lived) and arms a 15 s lease watchdog;
`NET_CMD_CONNECT` grew `s_associated` to its guard via the pure
`connect_attempt_is_moot()`; a stalled association is dropped the honest way
(`lease_watch_should_drop()` + `esp_wifi_disconnect()`) so the existing,
host-tested ladder retries from a real DISCONNECTED. Both predicates are
truth-tabled in `host_tz_test.py` alongside the ladder they complete. The pass
run above completed its own port-open boot with no wedge and no EN pulse.

**Caveats before this is called final.** n=1 person, one device, three walkable
entries — the 510 ms floor of the read regime especially needs a second hand
before sign-off (a fast reader could classify their own pauses as bursts and
get the 400; the press path caps the damage either way). The beat parks for up
to READ_MS after the last detent, so a deliberate slow walk keeps the clock
frozen ~2 s after the final stop; the idle-dim and every knob event bound it as
before. §12.8's ergonomic finding stands under the new windows: a three-detent
flick still wraps and cancels on a three-app table — correct, and now visibly
named three times before it does.

## 13. Work order — closing P2's item 1 under D11/D12/D13 (2026-10-12)

Owner-ratified scope; the rulings are §0.3, the ABI shape is §1.4. Three
deliverables, one or more commits, all before P2's `opt_*` item is called done.
Nothing here opens a question the owner has not already answered; the only
implementation latitude named below is flagged as such.

### 13.1 The defect being fixed (found by the owner's hand, confirmed in code)

`orb_theme.c` keeps two art slots, A at partition offset 0 and B at 0x400000,
each 4 MB (`SLOT_CAP`). `idf.py flash` writes the factory default bank at
offset 0 — inside slot A — and `use_baked_in()` draws the card-less fallback's
pixels from that same region. `target_slot()` protects the default only in the
never-installed state (both-invalid picks B): once slot B holds the first card
install, slot A reads invalid (a raw bank has no slot header), and the
`a->valid ? B : A` line aims the next install at A. The second pick therefore
erases the default and the fallback together; the "both live: replace the
older" branch that would honor the function's own comment never runs. The
pending NVS key makes it worse than one loss: a staged install survives into
whichever boot applies it, and the two ask-rows put the recovery decision on
the glass.

### 13.2 D13 — slots become what the owner said they were

**A is the default's permanent home; B is the only install target; the NVS
key records a choice, not a pending operation.** Concretely:

- `target_slot()` is deleted. `copy_into()` always receives `SLOT_B_HDR`.
  The erase-before-header discipline (the old header stays valid until the
  new one is ready) and the read-back verification stay exactly as they are —
  they are the reason a failed copy costs a row message and not a brick.
- Replace `THEME_NVS_PENDING` ("install this at next boot") with
  `THEME_NVS_LIVE` = `"default" | "card"`: which slot the dial draws. No
  install is ever deferred to boot — if the NVS says `card` but slot B fails
  verification at boot, boot falls back to the default with its damage row
  shown, because B's bytes were verified when written and anything else is
  corruption, not a pending state. The `THEME_MAX_FAILS` ladder and the
  boot-time card-open/copy path in `orb_theme_boot()` are deleted with it:
  the boot path resolves, it never installs from the card again.
- Picking the default is `live=default` + reboot: no card access, no copy.
  The picker must list a **Factory default** row (first row, title from the
  compiled pair, value `on the dial` when live) even when the card holds
  nothing — until D10's `.orbapp` wiring ships, its source is the flashed
  bank at offset 0, not the SD container.
- Pin in `host_tz_test.py` (next to the ladder's existing tables): the
  boot-resolve predicate, live-key x B-valid/B-corrupt x fallback, and
  `install_target() == SLOT_B_HDR` unconditionally.
- The debug HTTP surface must stop offering the old path: `h_theme_stage`
  (`POST /api/v1/sd/load?name=`) calls the same worker entry the picker
  posts, and `/api/v1/sd/unstage` disappears with the pending concept; the
  PWA's "staged … installs at the next reboot / Cancel" banner shows the
  live choice, not a pending one. Keep one code path; the endpoint and the
  knob must not be able to disagree about what a theme change is.

### 13.3 D11 — a pick is an install, an install ends in a reboot

`clock_themes_pick()` stops posting `J_STAGE`: it posts the install job (the
worker task — multi-second SD-to-flash copy, never the LVGL task). The job
runs the existing verify-first ladder (`card_check` → `verify_container` →
identity → `copy_into` → partition read-back → header commit); on success it
sets `live=card` and calls `orb_jobs_reboot_soon("theme <slug>")`. On any
failure it returns the reason to the row's value (the `not loadable`/`why`
rule is unchanged) and the device stays up. There is no second press to
have faith in: the rows "reboot now" and "keep this dial", the `staged`
field, and the `+2` in `clock_opt_count` are deleted — the count is the row
list again, plus the Factory default row from 13.2. While the job runs the
shell's existing busy treatment holds the page (no ghost press; the C8
lesson: a press the firmware swallows is a press the user must be told
about). Reboot timing stays the OS's — `orb_jobs_reboot_soon` is already
the only door, and the delay that lets the last frame reach the panel is
its whole point.

### 13.4 D12 — one level of app-owned nesting, validated by the shell

ABI: add `orb_opt_kind_t (*opt_kind)(orb_app_t *, int idx)` to the iface
(`orb_app.h`; §1.4's paper list is its frozen text: `OPT_ROW`, `OPT_GROUP`,
`OPT_ITEM`). A NULL hook means every row is `OPT_ROW` — the entire pre-D12
ABI, and every existing answer; the registry wrapper defaults it like
`orb_app_opt_label` guards. Contiguous-section convention: an `ITEM` belongs
to the nearest preceding `GROUP`; `GROUP`s are top-level only. The shell
(menu `PG_APP`) hides `ITEM`s while their group is closed, and validates the
shape at entry — a leading `ITEM`, or an `ITEM` with no group, renders as
`ROW` and logs once, because a silently invisible row is the "knob appears
not to work" failure in list form. Inside an open group the cursor clamps
over that group's items only; a press on a `GROUP` opens or closes it (its
value column carries the state: `open`, or `1 of 4` naming the chosen item);
long-press inside an open group closes it and stays on the page — homing to
the dial remains C8's, at the router. `color_picker` passes NULL (flat page
unchanged — the pin that the NULL default is real); the clock's rows become
`ROW "Factory default"`, then one `GROUP "From the card"` whose `ITEM`s are
the scanned themes: the dogfood that proves the feature.
`host_menu_test.py` carries the shape table: group open/close, hidden-row
renumbering across the whole page, cursor clamps on both sides, the two
malformed-shape rows, NULL-hook regression — and the suite's mutation rule
applies: break the validator, at least one test goes red.

### 13.5 What "done" means

Build clean; every host suite green; `carousel_bench.py` green (unrelated to
themes, but the iface literal changes every registered app's table); then
on orb-A, the owner's hand: MENU → the clock's options, enter the theme
sub-menu, pick a card theme — the board installs, reboots itself, and
returns on the new dial with no row ever asking; pick Factory default and it
returns without touching the card. After both trips, the flashed bytes at
slot A's region still hash to `assets/clock_assets.bin` (the D13 invariant
proven on hardware, not asserted). Record the run here as §13.6 and the
§0.3 rows are signed off. If the pick-to-reboot sequence should also ask the
PWA for a confirm, that is a new owner question, not an implementation
choice.

### 13.6 The sign-off run, 2026-10-12, orb-A

**Superseded on the theme routes by §13.8 below.** This run predates FORMAT 2, so the
`?name=` verbs, the `try_slug`/`damage_slug` fields and the `name is missing or not a
theme name` 400 are what the device said that day and are kept as the bench record it
is. A FORMAT 2 orb takes `?id=` with exactly 12 chars of `[0-9a-f]`, reports `try_id` and
`damage_id`, and refuses with `id is missing or not a theme id`. Copy routes from §13.8,
not from here.


Image: the tree these three commits record, stamped `c0a224b-dirty`, audited by
`tools/serflash.sh` (`AUDIT OK: the image's own stamp matches the working tree`)
before the write at `0x20000`. Card: `SD16G SDHC 29.4GB @20000kHz`, four containers:
`skeleton`, `diver`, `perpetual-calendar`, `neon`.

**The owner's walk, MENU → Clock → the card sub-menu → a theme.**
`menu: cursor MENU row 3/4: Clock`, press, then
`menu: cursor Clock row 1/2: From the card  (1 of 4: diver)`. The items are absent from
that page while the group is shut, and the header answers with what is on the wall. Press:
`menu: group row 1 open: From the card`, and the page becomes rows `2/6` to `5/6`, each
label indented, the live one carrying `(on the dial)`. Press again:
`menu: group row 1 closed: From the card`. Both toggles are the shell's, and the app never
heard about either.

**The cursor clamps inside the group.** Five downward detents from the header stop at
`5/6:   neon`, never at the back row. Eight upward detents stop at `2/6:   Skeleton`, never
onto the header. A hold gives `menu: hold closes group row 1: From the card`, and the hold
after it gives `menu: menu idle` plus `input: switch clock`: C8's escape hatch intact for
every screen with nothing to collapse.

**D11, the pick is the confirmation.** A press on `  Skeleton` and nothing else:
`orb_theme: installed "skeleton" into slot-B in 8850 ms — the dial reboots to draw it`,
`jobs: rebooting in 750 ms: theme install`. The board returned on the new dial:
`orb_theme: theme "skeleton" from slot-B (chosen), 1978973 B of pixels`,
`art_bank: bank "assets" @0x830000, 1978973 B mapped`,
`clock: art bank mapped: 1978973 B (theme needs 1978973 B) from slot-B`. No row asked
"reboot now" or "keep this dial" at any point, before or after.

**D13, the default is a choice rather than a file.** `POST /api/v1/sd/default` gives
`jobs: rebooting in 750 ms: theme: the factory default`, and the boot after it says
`orb_theme: slot-B holds "skeleton" but the dial is on the factory default` beside
`art_bank: bank "assets" @0x420000, 1173563 B mapped … from baked-in`. `/api/v1/sd` answers
`live:"default"`, `source:"baked-in"`. The card stayed shut for that trip.

**A refused install says why and changes nothing.** `POST /api/v1/sd/install?name=neon`
carries intact checksums and one complication record with no size. That install left the dial
where it was and answered on its own row: `/api/v1/sd` returned `try_slug:"neon"` with
`try_why:"comp is shown but has no size"`, while `live`, `source` and `active` kept their
previous values and no reboot was asked for. An unknown name logs
`orb_theme: install: no such theme on the card`. A missing or traversal `name` is a 400
before the worker hears about it: `name is missing or not a theme name`. The page confirms
first, on both paths: `if(!confirm('Install '+n+' and …` and
`if(!confirm('Go back to the facto…`.

**The interrupted-install invariant, forced by hand.** `erase_region 0x820000 0x1000` with
`live:"card"` (B's header gone): boot answers
`orb_theme: no installed theme — drawing the baked-in bank` and
`art_bank … @0x420000 … from baked-in`, and the watch still tells the time. No damage record
is set, because an absent slot is not a damaged one. Erasing the pixels beneath a live header
(`erase_region 0x830000 0x40000`) does set one: `source:"baked-in"`, `damage_slug:"diver"`,
`damage_why:"the installed copy did not verify — re-install it"`, `live` still `"card"`. One
press of Install repaired it: `source:"slot-B"`, `active:"skeleton"`, both reason fields empty.

**Slot A, measured rather than asserted.** After two installs, one headerless boot, one
corrupt-pixel boot and five reboots, the 1,173,563 B read from `0x420000` hashes to
`f6e21a315489851461d04930752b50151a826d0a0f20e13f82dc80d389f7d0d6`, byte-identical to
`assets/clock_assets.bin`.

**Endpoints.** `POST /api/v1/sd/load` and `POST /api/v1/sd/unstage` → 404 `Nothing matches
the given URI`. `POST /api/v1/sd/install?name=X` → 200 `{"state":"installing",…,"reboot":"automatic"}`.
`POST /api/v1/sd/default` → 200 `{"state":"default","live":"default","reboot":"automatic"}`.

**Host side.** `idf.py build` clean. `host_sd_test`, `host_tz_test`, `host_menu_test` and
`host_app_registry_test` green, each with its `--mutations` battery reporting zero survivors,
and `carousel_bench.py` reporting all assertions held.

**Two notes for the next person at the bench.** `/api/v1/screen?turn=N` moves exactly one row
per request, and its sign comes from `knob_row_step`, so a scripted walk sends one request per
detent and turns negative to go down the list. The first attempt here pressed `IT_AP` three
times believing it had reached the clock page, and the `menu: cursor …` line is what named the
mistake. The three commits land as menu, theme, net: checked out alone, the theme commit does
not link, because it deletes `orb_theme_stage_load()`, `orb_theme_unstage()` and
`orb_theme_pending_slug()` while the previous commit's page and picker still call them. The API
deletion and its consumers are one atomic change. Squash the three if a buildable `git bisect`
matters more than the three rulings read separately.

§0.3: D11, D12 and D13 signed off against this run.

### 13.7 The dial that would not wake, 2026-10-12, orb-A

**Report.** The owner: the dial no longer wakes from the dimmed state. Intermittent — both wake
sources answered when tested at the bench minutes later.

**Ruled out on the bench, with the log as the witness.** The dim lands: `idle_dim: armed: dim to
20% after 60 s (knob + motion)`, then `co5300_spi: set brightness to 20% (hardware value: 51)` at
61 s of quiet. A knob event that reaches the router restores the glass in 32 ms:
`touch (http): 2 stamps since boot` at 300641 ms, `set brightness to 100% (hardware value: 255)`
at 300680 ms. So the poll timer runs in the dark, the decision is right and the write lands when
the panel is answering. One bench trap cost an hour: the orb's DHCP lease moved to
`192.168.2.32`, and every request to the old `192.168.1.177` hung until its timeout and looked
like a wedged device. Resolve the port and IP through `tools/orb_bind`, never from memory.

**The defect.** `idle_task` committed `s_dimmed = want` *before* asking the panel. The brightness
level is one QSPI write to the CO5300's `0x51`, `apply_level()` returned `void`, and a refused
write only logged. One refusal and the flag says lit while the glass says 20%: no later tick
disagrees with the flag, so the transition is never attempted again, and `per` — chosen from that
same flag — drops to the 1 Hz lit tick, taking the 20 Hz dark accelerometer poll, and the second
wake source, with it. `apply_level` is the file's only brightness writer and `s_dimmed` its only
assignment, which is why this one latch is the whole story inside the module.

**Fix.** `apply_level()` reports whether the panel took the write. The commit goes through
`idle_dim_level_apply_decide(want, dimmed, wrote)` — `want` if it landed, `dimmed` if not — so the
next tick asks again at the rate of the state the panel is actually in: a lost wake retries every
50 ms, a lost dim every second. No new timer, no new task. The dim-only side effects (the `0x51`
stamp the wake log measures from, the motion re-seed and run reset) now happen on a dim that
landed. The failure line is rate-capped once per second the way the touch line is, because a dead
panel is refused at 20 Hz.

**Made measurable.** `/api/v1/status` gains `idle.writes_failed`, the refused writes since boot,
and `tools/idle_watch.py` prints it as `wfail` and names the diagnosis when `dimmed` is stuck true
under a climbing `stamps`. Before this, a dial that woke on the third try and a dial that never
woke produced the same evidence.

**Pins.** `tools/host_idle_dim_test.py` gains 7 commit vectors (both refused directions, both
accepted, both no-ops, and the refused-then-accepted retry — the pair no single call can
distinguish), 4 predicate mutants and 3 mutants of the call site, checked by a lint on
`idle_task`'s own text: the only assignment to `s_dimmed` may be the one that asks the predicate.
Without the lint, `s_dimmed = want;` beside a correct predicate passes every vector and returns the
dark dial. 29 mutants, 0 survivors. All 11 host suites green, `idf.py build` clean, and the
dim-compiled-out variant (`build_vdim`) links.

**On hardware.** Flashed to orb-A (`AUDIT OK`), dim at 61 s, wake to `lit_pct=100%`, a second dim
after it, `wfail=0` throughout — the panel is answering now, so the retry did not have to.

**Limit, stated plainly.** The owner's episode was not reproduced. The CO5300's `0x51` has no
readback, so a write the driver *reports* as taken and the panel never applies stays invisible to
this fix; it covers the refused write, which is the failure the module could turn permanent. The
next report reads `idle.writes_failed` before it reads the glass.

### 13.8 Theme identity FORMAT 2 — outcome, hardware-verified 2026-10-12 (closes `docs/theme-naming-todo.md`)

FORMAT 2 replaces the slug+title pair with one `name` field; the card filename is
`sha256(name)[:12] + ".orbtheme"` and carries no identity of its own. The full design and the
green/blocked inventory live in `docs/theme-identity-plan.md`; that file's §7.4 promised this
outcome record before `docs/theme-naming-todo.md` was deleted, and the deletion is in this commit.

**Verified on orb-A at `73a5acf`, over the wire and on the glass.** FORMAT 1 containers refuse at
the format gate with an honest reason string, on the card list and at boot, and the dial degrades
to the baked-in bank instead of drawing garbage. Upload lands the card copy under the header's own
id — the request carries no name. Identical re-send is a no-op; same name with different bytes
answers 409 plus `overwrite:false`, and one deliberate `?overwrite=1` replaces. Install by id
mirrors into slot B, reboots, and boots logging `theme "skeleton" from slot-B (chosen)`; the row's
`changed` hint lights when the card file drifts from the installed mirror. Delete resolves by id
only: `id=skeleton` and uppercase hex are 400s. Worst-case `GET /api/v1/sd` measures 1395 B against
the 6144 B buffer with the list at its eight-entry cap and three 55-byte UTF-8 names.

**One defect fell out of the verification.** bake_clock.py truncated an over-55-byte `--theme-name`
silently — a 55-CHARACTER slice before the id hash and a 55-BYTE slice in orbtheme.py's header
write, disagreeing for multibyte names: the printed card id hashed a different string than the one
the header stored. Both layers now refuse with the byte count, matching the studio page's nameOk
rule, before any file is written. The device reader stays tolerant of a ≤56-byte stored name by
design: its id is the hash of the stored bytes, so any such file is self-consistent.

**Still open.** The node-gated `_selftest` suites (headless-orbtheme, export-test) have never been
executed — no node or Chrome on this machine. `diver`, `perpetual-calendar` and `neon` need their
owner's layer kits to re-bake. The archived `clock_app_default.orbtheme` is no longer committed
(revoked D10 source, §0.2), and was never regeneratable in any case: the kit bake renders
`show=[1,1,0,0,0]` against the archived `[1,1,0,1,0]`, so the original bake flags are unknown.
Restore it from git history against its sha256 rather than re-baking it.



---

## 14. M3 — the overflow bank: re-map (A) vs PSRAM arena (B) — captured 2026-10-12, orb-A

The last open item of P2 (§8's M3, §6's M3 text, §11's handed-over refusal
question). Same art, two builds, one variable: `ORB_ART_MECH_B` at 0 and at 1.
Everything else was held identical — same tree (`d7de71f` + this session's
bench code, stamp `d7de71f-dirty`, audited by `tools/serflash.sh` on every
push), same board (orb-A, `44:bd:8d:60:80:88`, wall-powered, ota_0, rssi −55),
same bench bank, same 31 iterations, same capture (`tools/capf.py`).

**The bank is real kit3, not a size.** The M2/LAT probes bill address ranges;
M3 reads pixels, so it measures on flashed bytes: `/tmp/m3-kit3.bin`,
3,858,874 B, md5 `aba381f34903574babf25320fd83ae22`, crc32_le `0x59801b57`,
baked from the committed `assets/sources/watch-orbital-kit3.zip` (prep_watch_kit
→ bake_clock, kit_check gate green) and written behind the clone's MAC gate.
The first campaign placed it at `assets+0x100000` (flash `0x520000`) on the
strength of an old comment claiming the baked bank ended at `0x11E000`; it
actually ends at `0x11E83B`, so the bench write ate the live bank's last
~122 KB and orb-A ran a corrupt dial (§14.4). The bench home is now slot B's
art, `M3_BANK_OFF = assets+0x410000` (flash `0x830000`) — under an erased,
never-installed slot header and clear of both slots' live bytes. The probe
refuses erased flash, verifies map-vs-read consistency, and now also compares
the whole plane against the flashed file's crc32_le before printing a number;
`/tmp/m3_bankwrite.sh` keeps the write reproducible.

**The probe** (`ORB_ART_M3_PROBE`, ships 0, same family as M2/LAT) prices one
app switch per arm: release → reclaim → first full-page commit (60 blocks of
60×60 through the shipping `gfx_blend_px` at alpha 128, sources striped one
per page band so every one of the bank's 59 pages is touched each pass). The
bench ladder (`DISPLAY_BENCH 1`, `PX 72`, `ALPHA 128` — 255 prices the wrong
op) then runs the dial at tempo reading the same bank through the arm's own
plane, and the per-second `clock N fps | ink px/s` lines are the rotation
steady state. `ORB_ART_MECH_B` is compile-time on purpose: a runtime switch
would be a second residency table the host mirror cannot pin.

### 14.1 The numbers (medians of 31, µs; the two arms' distributions do not overlap)

| | A (re-map) | B (PSRAM arena) |
|---|---|---|
| release (suspend) | 280 | 0 (carrying host) |
| reclaim (resume) | 318 | 1 (covered-entry lookup) |
| first full-page commit | 63,019 | 59,986 |
| **switch total (matched samples)** | **63,067** | **59,473** |
| boot cost (once) | — | **471,117** (arena fill, ≈ 8.2 MB/s) |
| holds | 59 MMU pages | 3,858,874 B PSRAM (free 7,947,908 → 4,081,280) |
| steady rotation | 6 fps med (83 lines), ink 1,555,200 px/s, hook 924 ms/s | 6 fps med (79 lines), ink 1,555,200 px/s, hook 921 ms/s |
| status LAN corroboration | `art_pages 77, art_banks 2` | `art_pages 18, art_banks 2` — the arena bills the window NOTHING |

A's switch reproduced within 0.15% across four boots (63,018/63,021/63,067/63,085).
Read against §12.7: an app switch through a cold re-map costs what an app
switch already cost (M1's 63–87 ms) — the µs-scale MMU op is noise under the
first composite, and the 5% the arena saves on reads (3.0 ms) is inside that
figure's spread. The release-for-free variant of B — an arena app whose
parked bank is NOT carried — costs a 251 µs free plus a **471 ms re-fill**:
for the arena, the carry decision of §6/M-carrier is not optional, it is the
mechanism's survival condition.

**Ruling — ratified by the owner 2026-10-12 as D14 (§0.4).** A stays the
mechanism for the overflow bank too: 3.6 ms of switch and 5% of read cost do
not pay 471 ms of boot and half the PSRAM (3.86 of 7.95 MB free), and steady
rotation is identical at the reporting resolution. B ships dark
(`ORB_ART_MECH_B 0`) as P4's escape hatch — it is the only way a third
flagship can ever boot, its gate (free minus bank minus 2 MB margin, refusal
counted with the byte figure in the log) is measured, and the ABI promise of
§3.3 is now code, host-pinned (`host_art_residency_test.py`: 16 checks, 11
mutations). If B's 3.6 ms is ever wanted badly enough to pay the PSRAM, the
define is one byte (`art_bank.h:98`).

### 14.2 The refusal hunt — §11's third finding, answered

The hunt maps every distinct 64 KB-aligned page of every partition the table
exposes — assets (128), www (20), ota_0 (32), ota_1 (32) — counts distinct
virtual bases against fold-backs (M2's dedup lesson: an overlapping-stride
version of this loop "ran" 1,165 folded requests and measured nothing),
and unmaps everything before the dial starts. **Result: 212 attempts, 211
fresh, 1 folded, zero refusals, 170 KB internal free at the end** — with the
image's 22 pages, the primary's 18 and the bank's 59 already resident, the
allocator accepted ≥ 310 region-pages. The §3.1 "256-page window" is
confirmed a `soc/mmu.h` region-count reading, not the capacity anyone refuses
at; below the size of the flash itself the allocator does not refuse at all.
`ORB_ART_PAGE_BUDGET` 234 therefore stays exactly what §11 called it — an
enforced contract, deliberately conservative, refusing early with a named
reason — and there is no measured ceiling to raise it toward. Two flagships
plus the image cost 142 of it; that is the real constraint's whole shape.

### 14.3 One incident worth a standing rule

The first M3 builds died their network boot: `httpd_start:
ESP_ERR_HTTPD_TASK` at the same millisecond every time — the probe's two 4 KB
flash-staging statics and its hunt arrays had eaten the last contiguous
internal block the httpd task's 8 KB stack needed (this heap's largest block
measures 7,168 B with a healthy boot). Fixed by moving every probe buffer to
PSRAM (`m2_stage_buf`, heap-allocated handle arrays). Standing rule for this
probe family: **boot-probe scratch earns no internal `.bss`** — a bench
instrument must not change what the board does after the instrument finishes,
which is the same reason the probes ship 0 in the first place.

### 14.4 The corrupt-dial incident — a bench fixture is flash content too

The bench bank's first home (`assets+0x100000`) was chosen from the LAT
header's claim that the baked bank ends "near 0x11E000". It ends at `0x11E83B`
— the bake output answers that in one line, and the probe's gate now quotes
it. The bench write therefore replaced the live bank's last 124,987 bytes with
kit3's head, orb-A rendered a corrupt dial, and `art_ready` stayed `true` all
the way through: the ship path verifies header geometry at load and runs no
whole-bank CRC (§11.2's "no CRC runs in the shipping path"), and the probe's
erase/CRC gates compare the mapping against the read path over the same bytes
— both said "consistent" about content nothing had cross-checked against the
baked file. The A/B numbers are unaffected: esptool verified the kit3 hash
after the write, and the probe reads only the bench range, which held genuine
kit3 — every measured byte was a byte of the bank it claims to price.
Recovery: re-flash the committed `assets/clock_assets.bin` at `assets+0`
behind the MAC gate, then read back and compare head and full tail —
byte-exact, `art_refusals 0`, dial restored. Two rules come out of it:
**a bench fixture earns no flash offset without checking the live bank's
exact end in the bake output**, and a probe that measures content gates on the
content's own hash, not just on self-consistency — hence the `crc32_le`
ground-truth gate, the relocated `M3_BANK_OFF`, and the retired-offset lint
pin in `host_art_residency_test.py`.



---

## 15. The install ladder gets its bytes onto the card — P4's upload route, orb-A, 2026-10-13 (`d2174f2`)

The `.clinerules` install ladder had been benched only against a container placed
on the card by hand. `POST /api/v1/sd/app` closes that gap: it is
`h_theme_upload`'s mirror for `.orbapp`, raw body, mandatory `Content-Length`, one
upload under the one card lock, bytes into `/sd/apps/upload.part`, and a
`rename()` to `<slug>.orbapp` only once the container passes the reader's header
ladder. The destination is the header's own slug, never a client string — a
nameable upload would choose what the next walk lists. The payload CRCs stay the
install's D9 job: the card-facing tier reads only the header (`orb_app_scan.c:17`),
and running the payload check from the httpd task needs install.c's internal DMA
buffer, which is the same `.bss` squeeze that costs httpd its 8 kB stack. Sentinel:
16 vectors, **16/16 mutants** caught, the four new ones pinned to rename-before-
validate, the payload check out of the upload tier, the scratch's extension, and a
client-named destination.

**The board found two bugs no host vector could.** Both are the same class as the
`/api/v1/apps` row-array finding two commits earlier, and the plan's standing rule
about hardware holds again: the ladder's ORDER is provable on a host, its BYTES are
not.

1. **`/sd/apps` had never existed.** The mount made `/sd/themes` and nothing else,
   so the scratch `open()` failed `FR_NO_PATH` and every upload answered 500. The
   scan's own comment had reasoned correctly that the walk must not see a `.part`
   file and had registered the directory anyway — registering a reader of a
   directory nobody creates. Fixed by mirroring the theme dir-create at mount, with
   `ORB_SD_APPS_DIR` in `orb_sd.h` beside `ORB_SD_THEMES_DIR` so the two can no
   longer drift apart.
2. **`orb_app_install()` overflowed the worker's stack.** `orb_jobs` runs on a
   hard-measured 4096 B — `orb_jobs.c:65` says a task that needs more "is a crash
   rather than a log line" — and the installer carried `settings[16]` and `sw[]` as
   frame locals, ~1 KB each, on top of `h_store`, `raw`, the verdict's `why[128]`
   and the read/decide/copy chain. Serial said `***ERROR*** A stack overflow in
   task orb_jobs`. This is exactly the bug `boot_resolve()` already documents for
   the 3584 B main task, one function over; the install path had simply never run on
   hardware to find its own copy. Fixed the same way — one PSRAM block for both
   tables, a single `out:` label that closes the fd and frees it on every exit.
   Because no host test can see a stack, `host_orbapp_install_test.py` now pins the
   *shape* instead (lint rule 3c): the install's scratch must come from
   `heap_caps_malloc(MALLOC_CAP_SPIRAM)`, must be freed at the `out:`, and
   `settings[]`/`sw[]` must never reappear as frame locals. The mutant that puts
   them back is caught.

**The run, orb-A, `fw aecc258-dirty`, `hsb_color.orbapp` (4096 B, behaviour_kind 1,
art_size 0, caps 0):** upload → `{"state":"stored","bytes":4096}`; `GET
/api/v1/apps` → listed `ok:true`, `app_id 0x17ba1a0a`, filed under the header's
slug; install → `{"state":"installing"}`; `/api/v1/status` → the row is resident
with `failed:false`. The next boot's serial carries the two lines that make the
milestone: `appinstall: resolved 1 installed app from the "apps" partition` (the
header-last write held, and the table rebuilt from flash with the card irrelevant)
and `app: 3 app(s) built, 3 walkable` (the installed app is resident but
**disabled**, so the dial is exactly what it was — additive safety, proven on
silicon rather than asserted).

**The enable path landed at P4a, and the confirm it owed is paid.** `card_fill()`
still writes `enabled = false` — that IS the promise now, not a gap — and
`orb_app_install_apply_enable()` re-applies the owner's stored `e<slot>` word between
`orb_svc_init()` and `orb_app_init_all()`, so an installed app walks exactly the boots
the owner turned it on for. `POST /api/v1/apps/enable?id=<slug>&on=0|1` is the door
and the `/apps` page's "On the dial" tier is the handle; uninstall (`POST
/api/v1/apps/uninstall`, B4) erases the slot header, forgets every owner word through
`orb_svc_forget_row()`, and frees the table entry — no reboot, because D9 already
un-made the app at the erase. The D16 beat delivery's precondition, "a second app to
police", is met from this commit: an enabled card row is walkable and schedulable.
What it still lacks is a second BEAT — the bench `hsb_color` declares `period_ms 0`,
so the shared-beat delivery waits for an installed app that asks for a tick.

**The P4a bench, orb-A, `fw 36f21a4`, rssi -62 (marginal: two HTTP timeouts mid-run,
none lost a state transition — every refusal was re-probed before the ladder moved).**
The full lifecycle ran over the two new doors, card file untouched in `/sd/apps`
throughout. Enable → `{"state":"enabled"}`; a cold boot then carries `appinstall:
enabled "hsb_color" from its stored word`, `app: hsb_color: reuses its behaviour
module's built screen — walkable`, and `4 walkable` — the apply rung, the word's
durability across a reboot, and the C6 iface-reuse guard all proven on silicon. On
the glass, disable answers 409 with the device sentence (`that app is on the dial —
carousel away from it first`), and the refusal held even after injected detents
re-committed the row — the check runs at door time, not at page-draw time. Carousel
away, disable lands, and five detents never bring the row back: the disabled app is
out of the walkable set, P4's deferred disable/enable cycle pass line, closed. The
glass needs two settles after a `to=` before the row reads `on_glass: false` — a
one-second window is not enough, and a bench that does not wait sees a false refusal.
Uninstall → `{"state":"uninstalled"}`, the ledger drops to three rows, `installed:[]`
streams, and no reboot follows; the next cold boot reads `resolved 0 installed apps`
and `3 walkable` — the single-sector erase made the slot absent exactly as D9 promises.
Reinstall from the same card file lands the row back in slot 0 with `enabled:false`:
`orb_svc_forget_row` erased the `e0` word and the fresh install refused to resurrect
the owner's last ON through flash residue — absent means off, on hardware. The bench
also caught one stale string no host vector could: `h_screen`'s 400 body still listed
`["clock","color_picker","hue","menu"]` by hand while the registry had grown a card
slug the same endpoint happily resolves — fixed in the same commit to stream slugs and
aliases from the table (the array was also malformed JSON: a stray quote made `known`
a string, not a list).

**The P4b bench, orb-A, `fw e74d500`, rssi -62 (the radio kept its P4a temperament:
the decisive POST landed in the first live window; GETs stalled after it and the card
followed them down).** Three containers, all made by the post-P4b writer: the bench
`hsb_color.orbapp` (4096 B, behaviour_kind 1, `art_req 65536`, `art_size 0` — the
declaration on hardware for the first time), a FORGED `art_carrier.orbapp` (the same
bytes with the art words patched back to 4096 and the header CRC re-summed — the
writer refuses to emit it, which is the pass line, so a stranger's pre-D21 card is
exactly what this vector is), and the pre-existing hsb_color on the card. Upload of
the art-carrier answered 422 with the reader's sentence VERBATIM in `detail`:
`carries a 4096 B art payload: no container ships art (D21) — re-package it`, and
nothing was filed. The first attempt at that sentence arrived as `#\x04\x06` — both
header validators returned their refusal from a frame-local buffer, and the door
printed the corpse of a stack frame after close() and the unlock had reused it. P4b
is the first phase with a real reason to print, so P4b found it: `static char
why[160]` in both validators (`e74d500`), pinned by lint 10b and a mutant that puts
the frame-local back. The declaration upload landed (`overwrote the previous one` on
serial), and the cold boot re-resolved it: `resolved 1 installed app from the "apps"
partition`, `3 app(s) built, 3 walkable` (installed, off, as promised) — the inverted
gate holds from header to registry on silicon. The run then lost the card:
`sdmmc_write_blocks failed (0x101)` mid-ladder and the next boot `no usable card on
/sd (tried 20000/10/4 kHz)` — the same internal-DMA squeeze this project keeps
billing (the mount retry's `esp_dma_capable_malloc(125): Not enough heap memory`
under the marginal radio's buffers), not a P4b byte (P4b REMOVED code paths from the
card). Owed to the next healthy window, neither one P4b's pass line: the re-send of
unchanged bytes for the dedup tuple WITHOUT the art term, and the `/api/v1/apps`
listing read of the declared row (the scan refuses nothing here — the row is legal).


---

## 16. P5.5's first flash catches the embedded tables; M5 blocked at the card and the radio — orb-A, 2026-10-14

The session's mandate was two items in one bench visit: put P5.5 (`b1acae1`) on
orb-A and read the status ledger, and run M5's two arms so D30 can be settled.
The first half produced a real finding and its fix. The second half could not be
run at all, and the reason is recorded rather than worked around.

**The state before the flash was not the state the plan assumed.** The board ran
`20c7a38` (P5.4) — P4b, P5b and P5.5 had never been benched. The boot was already
sick on the bench's own terms: `esp-aes: Failed to allocate memory` during the first
WPA3-SAE attempt, one `auth-fail (reason 202)` retry, and — caught only by the
patient curl — HTTP answers the *headers* of every route and never the *body*
(`code=200 size=0` at 8 s, then at 20 s, on `/api/v1/status`, `/api/v1/sd`, and
`/`). The dial itself was fine at 15 fps. §15's tail had already billed "the same
internal-DMA squeeze this project keeps billing" when a card write died mid-ladder
with `esp_dma_capable_malloc(125)`; tonight that squeeze stopped being survivable.
The fw stamp read `20c7a38` truthfully, and the `fw`-is-the-hash audit could not
see any of this, because the audit checks bytes and the failure is a budget.

**The P5.5 embedded tables failed the boot exactly as the numbers predicted, and
only the boot could say so.** Pre-computed before the flash: `ORB_SVC_ROWS` 16 ×
sizeof `orb_layer_tab_t` 920 B = 14,720 B of `.bss`; the flashed image's heap region
1 began at `0x3FCC7508` with 136 KiB. Post-flash: `heap_init: At 3FCCA898 len
0001EE78 (123 KiB)` — the design's cost, to the byte. Then the verdict line:
`httpd_start: ESP_ERR_HTTPD_TASK (check LWIP_MAX_SOCKETS >= max_open_sockets+3)`,
`orb_net_start(4837): httpd`, `networking down — clock running without config
page`. The cause is not LWIP_MAX_SOCKETS (the hint lies); post-wifi_start internal
free was 14,675 with largest block 7,168, and httpd's worker stack does not fit.
The dial kept drawing at 15 fps and no watchdog fired — the failure shape §4.2's
clause describes for an app, demonstrated by the OS's own web server. Compile, link
and all 18 host suites had passed; `idf.py build` EXIT=0 had "passed"; only the
serial line saw it.

**The fix the finding forces.** The layer table moves out of `.bss` into PSRAM,
allocated lazily at the first `layer_add` and freed at row-forget, via
`heap_caps_calloc` beside the plane hook. A refusal now costs a layer, never a
config server. `set`/`touch`/`del` on a row that never added anything refuse
*above* the table and count in a new per-row `layer_refuses` word, so the ledger
pair never hides a refusal made before a table existed. The suspend wipe keeps the
table (generations must survive a sleep); forget wipes then frees. The four new
`host_svc_test.py` vectors pin exactly the paths that did not exist while the table
was embedded: post-forget refusal without the stub hearing anything, refusal with
live==0, PSRAM-OOM-at-add refusal, and a re-add that succeeds after the fail-once
arm. The orblayers suite needed no edit (the table module never moved): 73 vectors,
10/10 mutants, unchanged. The firmware grows **256 B of flash and returns 13 KB of
internal RAM**.

**Verified on the board, serial-captured (`/tmp/m5_boot_fix.txt`).** `heap_init: At
3FCC7598 len 00022178 (136 KiB)` — parity restored. `config server listening on
port 80` — httpd back. `post-httpd_start internal free=9,759 | dma_free=2,255`. Boot
to first dial frame 1.94 s (embedded build) vs 1.98 s (fixed build): the table's
home costs nothing in boot time. The dial held 13-16 fps through both. **The
status-body read of the new `layers` field did not happen**: headers-only survived
the fix unchanged — it is the radio-path defect below, not a P5.5 byte.

**M5 not run. Two physical blockers, both bench-side.**

1. **No card.** `sdmmc_init_cid: send_cid returned 0x101` at all three ladder rungs
   (20/10/4 MHz) — the slot is empty; the SD16G that produced §14's residency table
   and §15's upload ladder is out of the board. M5's same-bundles-on-the-card
   precondition cannot be met, and Arm A's copy cost has no source.
2. **HTTP payloads stall at every size** (headers 0.14-0.24 s, body 0 B at 12-25 s
   on three routes, both images). Every M5 trigger is a POST (`/api/v1/sd/install`,
   `/api/v1/sd/rescan`) and every M5 read is a GET body. Boot logs
   `esp-aes: Failed to allocate memory` and three BSSIDs in two boots
   (`3a:98:b5:f3:e2:2d`, `3a:98:b5:fc:e6:5e`) with rssi swinging −55..−70: band
   steering plus SAE re-authentication is eating the ~2 KB DMA pool the TX path
   needs. This predates tonight's fix — the same shape is in §15's tail from
   2026-10-13 — and is now named where the audit loop can see it.

**The D30 gate stands as written**: unrationalised, pending M5. Nothing here
licenses the lazy arm or the reboot arm; the session adds only the constant: an
install-reboot costs the owner ≈2.0 s of dial plus the boot's own ~3.3 s of
association before the page answers again.

**The re-run checklist, in order.** (1) Seat a FAT32 card with ≥2 bundles in
`/sd/themes` (the av-kit re-bake plus kit3, both FORMAT 2 as shipped; P6's FORMAT 3
will change the arms' payload, not the method). (2) Pin the orb to one AP BSSID
(`/api/v1/wifi/join`) or move it next to the router; require a *transferred body*
(`curl -o file -w %{size_download}` > 0) before trusting any HTTP measurement — a
200 header is not a response. (3) Arm A: time `POST /api/v1/sd/install?id=B` from
request to the first serial `slot-B` line after `reboot; app` (`/tmp/m5_armA.sh` idiom
below); read the serial for `install of "B" from the card: ok` and the post-reboot
`art bank mapped: N B`. (4) Arm B's two halves without shipping B: boot cost =
the same file's PSRAM-calloc timed against free-PSRAM before/after; flush
interference = an `orb_jobs_post` arena copy of the av-kit bank (1,173,563 B)
running while `display_bench_phases_us()` samples the hook/dma/te phases before,
during, after. (5) Only then does D30 get a ruling, and §1.2b's seam is where its
consequences land. *(Done 2026-10-15: M5 ran (§19), and §20 holds the ruling.)*

### 16.1 Correction, same night: the card is in; the wall is one 4 KB allocation

The owner's bench note — the orb has its SD card — is right, and §16's blocker 1
was an inference from boot lines that a later witness contradicts. The
`send_cid 0x101` triple is not an empty slot: it is the boot's DMA squeeze —
`post-httpd_start dma_free=1883` cannot give the sdmmc driver even its 125-byte
command buffer, and the retry throttle makes the boot attempt a single pass. The
same board at uptime 20 s answers `POST /api/v1/sd/rescan` with a *transferred*
53-byte body: `{"mounted":true,"card":"SD16G SDHC 29.4GB @20000kHz"}`. Seated,
FAT32, mounting at the top clock rate as soon as the boot squeeze has passed.

What actually stops M5 is one measured number, not two physical conditions:

- The upload route's `orb_theme_upload_begin` → `ensure_chunk` needs a 4,096 B
  **internal** buffer — SPI-SD is its source and sink, PSRAM cannot serve it — and
  the verify path reserves it again before any copy. Card-install and upload share
  the allocation, so every install door is gated on it.
- The board's steady state after association: `sta-connected internal free=8,139
  largest=3,328` (orb_net's own heap line). 3,328 < 4,096. The upload answered
  `507 out of memory for the card read buffer` — clean, logged, config server
  alive. The queue-200 family appeared a third time en route: an install for an
  absent id returned `200 {"state":"installing"}` and the serial said
  `install: no such theme on the card`, `job done, 864 B stack never used`.

§16's blocker 2 shrinks with the same evidence: the body stall has a size
threshold. Bodies of 50, 53 and 63 B transferred in 0.1–2.8 s all evening; the
~1 KB status body stalled at 25 s. "Every size" was wrong, and the arms care:
Arm A's trigger and response are readable today; only the list/status reads are
not, and the arm's timings can come off serial anyway. The card's own content
got one line of the truth on the way: the scan keeps **0 of 4** `*.orbtheme` —
four files present, all refused by the validator — and the per-file reasons ride
the `/api/v1/sd` body that stalls, so tonight they stay unknown.

**Checklist steps 1–2, superseded.** The wall is not seating or pinning. The
next session: (1) give `ensure_chunk` a granted-size fallback — 4,096, else
2,048, else 1,024, into a new `s_chunk_bytes` the eight clamp sites read instead
of the `CHUNK_BYTES` constant (measured: lines 745/820/832/1076/1182/1183/1205/
1334/1345 of `orb_theme.c`; the file alone changes, the copy loops already treat
the chunk size as a variable); (2) rebuild, re-flash, re-run the boot audit;
(3) upload `/tmp/m5_avkit.orbtheme` — built from the committed av-kit source
this session, FORMAT 2, 1,177,659 B, host-verified, id `5eb1ed3ae931` — and re-bake
a second bundle for the flip's two-file precondition; (4) require a transferred
status body before reading the `layers` field; (5) run `/tmp/m5_run.sh`, which
now does lease→rescan→upload→install with the serial epoch as the clock, so a
stalled response body cannot lie about any step. D30 stays unrationalised; its
blocker is now named in bytes, and the honest fix is one fallback allocation in
the theme module's own file.

## 17. The radio moved off the DMA pool; the config server paid for it — orb-A, 2026-10-14, `9fec1b9`

§16.1 named the wall in bytes. This session applied its prescribed relief: the four
knobs below, chosen so the radio stops holding the internal DMA pool the SD driver
asks 125 B at a time from. Committed as `9fec1b9`, flashed over serial, stamp
audited `want=9fec1b9 have=9fec1b9`.

    CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
    CONFIG_ESP_WIFI_TX_BUFFER_TYPE=0        (choice: ESP_WIFI_STATIC_TX_BUFFER=y)
    CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=8
    CONFIG_ESP_WIFI_AMPDU_RX_ENABLED=n

All four materialized. `build/config/sdkconfig.h` carries
`SPIRAM_TRY_ALLOCATE_WIFI_LWIP 1`, `ESP_WIFI_STATIC_TX_BUFFER 1`,
`ESP_WIFI_TX_BUFFER_TYPE 0`, `ESP_WIFI_STATIC_TX_BUFFER_NUM 8`, and no
`ESP_WIFI_AMPDU_RX_ENABLED` line at all. `ESP_WIFI_RX_BA_WIN` disappeared with it,
as the dependency intends. The materialization trap named in `.clinerules` did not
fire.

### 17.1 The relief worked, on the radio's own terms

Two boots captured (`/tmp/hd_boot.txt`, `/tmp/hd_boot2.txt`).

| Line | Before (`e3733a5`) | After (`9fec1b9`) |
|---|---|---|
| `heap_init` RAM region 1 | 136 KiB @ 3FCC7598 | 136 KiB @ 3FCC7698 |
| post-wifi_start internal / dma | — | 22,651 / 15,147 |
| post-httpd_start internal / dma | 9,759 / 2,255 | **no line — see 17.2** |
| sta-connected internal / largest / dma | 8,139 / 3,328 / — | 19,111 / 7,936 / 11,607 |
| `esp-aes: Failed to allocate memory` | present at first SAE attempt | **absent, both boots** |
| Association | band-steer retries, several BSSIDs | single attempt, lease in 2.3 s |
| Dial | 13–16 fps | 12–17 fps, mode 15 |
| Card at boot | `send_cid 0x101` triple | mounted clean, `SD16G SDHC 29.4GB @20000kHz` |

The squeeze §16.1 measured is gone. Largest internal block at sta-connected went
3,328 → 7,936, which clears `ensure_chunk`'s 4,096 B ask with 3.8 KB to spare — the
exact number that blocked the upload. The card mounted at boot with no `send_cid`
triple for the first time in three sessions. `esp-aes` stopped failing. The dial did
not regress.

### 17.2 The cost: httpd's worker task no longer spawns

Both boots ended with `httpd_start: ESP_ERR_HTTPD_TASK`, `orb_net_start(4837):
httpd`, `networking down — clock running without config page`. Identical, twice.
The `.clinerules` stop rule applies: report, do not improvise a third boot.

`httpd_start` reached `httpd_os_thread_create` (`httpd_main.c:532`) and lost. The
task wants `cfg.stack_size = 8192` with `task_caps = MALLOC_CAP_INTERNAL |
MALLOC_CAP_8BIT` (`orb_net.c:4545`, `esp_http_server.h:57`) — 8 KB of contiguous
internal DRAM. The `largest` column answers directly:

    boot 1  post-wifi_start  internal free=22,651  largest=14,848
    boot 2  post-wifi_start  internal free=18,683  largest= 8,704

Boot 2 is the plain failure: 8,192 B of stack does not fit an 8,704 B block once
its TCB and TLSF bookkeeping are charged. Boot 1 had 14,848 B and still failed —
httpd takes its 4 KB header scratch, 1 KB URI scratch and socket set from the same
block before the task is created. Total internal free is the highest this board has
reported. The failure is contiguous-block shape, and `ESP_ERR_HTTPD_TASK` reports
it as a task error rather than an allocation one.

**Why it got worse while memory got better.** `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`
allocates the radio's buffers PSRAM-first. Those buffers no longer consolidate into
one internal region, so what remains internal is many medium blocks instead of a few
large ones. The mechanism that freed 11 KB of dma_free also removed the single
contiguous 8 KB the config server needs. §16's fix (moving 14.7 KB of `.bss` out)
and this one fight the same budget from opposite ends: every session that enlarges
the total must re-check the shape.

**This is not the SPIRAM line alone.** The three wifi knobs are what reallocate the
driver's buffers. The handoff's fallback (drop `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, keep
the two buffer-shape knobs) was scoped to a dial regression. The dial did not
regress, so that clause does not authorize the run. It is recorded as the next
experiment, not performed here.

### 17.3 M5 Arm A: not run

Steps 3 and 4 are HTTP-dependent. With no config server there is no
`/api/v1/sd/rescan`, no upload route and no install route: `curl
http://192.168.2.32/api/v1/status` returns `code=000` (connection refused) while
ping answers at 7–22 ms. The bundle is intact and host-verified — `5eb1ed3ae931
'M5 AvKit' 466x466 geom 1888 B art 1173563 B`. No install timings, no upload
verdict and no flip numbers exist for this session, and none are invented here.

Rollback stays armed and named: `/tmp/orb-previous-orb-A.bin` is stamped `e3733a5`,
so `tools/serflash.sh --rollback` returns the board to a boot with a working config
server. The board is left on `9fec1b9` deliberately, so the failure is the state on
the bench rather than a claim about it.

### 17.4 What the next session owes

1. The shape, not the total, is the constraint. Print the largest internal block
   *after* httpd is configured, and give the config server a smaller `stack_size`
   retry before declaring the page dead. A server that will not spawn on a board
   with 22 KB free is the same failure shape §4.2 exists to survive.
2. Run the handoff's named fallback: revert only `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`
   and keep static TX plus AMPDU RX off. It separates the PSRAM-first line from the
   buffer-shape lines. Its risk is what this section measured — the two wifi knobs
   alone may not clear a 4 KB largest block.
3. The upload has never been proven on this board with a healthy DMA pool. §16.1's
   granted-size fallback in `ensure_chunk` stays unwritten, and the mis-map that
   reports `esp_dma_capable_malloc` failure as 413 is still open.
4. D30 stays unrationalised. Its price is still unpaid, and the owner rules it.
### 17.5 Correction, same night: the radio knobs were the wrong lever

§17 stopped at two identical refusals. The owner's reply reframed the requirement:
the upload is rare, the dial may stall for its duration, and the only hard rule is
that the radio and the card are both alive at the same moment. That makes the
config server a non-negotiable, and it makes §17's own conclusion — "the two wifi
knobs may not clear 4 KB of largest" — the wrong thing to chase.

Two more boots were run to settle which knob killed httpd, and both refused the
config server again:

| Image | Knobs | post-wifi_start largest | post-wifi_start dma_free | `config server listening` |
|---|---|---|---|---|
| `9fec1b9` boot 1 | static TX 8 + AMPDU RX off + PSRAM radio | 14,848 | 15,147 | absent |
| `9fec1b9` boot 2 | same | 8,704 | 11,179 | absent |
| `164d667` | static TX **8** + AMPDU RX off, PSRAM line gone | **7,168** | 5,583 | absent |
| `164d667`+TX4 | static TX **4** + AMPDU RX off | **11,264** | 12,507 | absent |
| `4367e7a` | all reverted (dynamic TX 32, AMPDU RX on) | — | — | **present** |

The last line is the point: `TX_BUFFER_NUM=4` restored `largest=11,264` — comfortably
over the 8,192 B worker stack — and `httpd_start` **still** refused. The stack-size
theory in §17.2 was wrong about the trigger, and so was the premise underneath it.

**Why the whole direction was wrong.** Between the `post-wifi_start` probe and
`httpd_start`, `orb_net.c:4835` runs `net_post_join()`, which starts the association.
Association is what allocates the WPA3/SAE and mgmt-frame state, and `esp-aes` takes
its buffer from that pool during the handshake — the very allocation that §16 saw
*failing* on a starved boot. Removing the radio's own DMA pressure simply moved
those allocations from "fails" to "succeeds", and they land in the same large block
httpd's stack wants. The radio knobs changed the total and the first-fit order;
they could not change the fact that association and httpd's worker stack are
allocated back to back and compete for the same remaining region. No combination of
these four symbols fixes that, because the shortage is not in the radio's own
buffers. It is in what sits above them: `CONFIG_LV_MEM_SIZE=24576` as a static
internal array, and the display's tile ring.

**The lever the owner's license actually points at.** `display.c:721` allocates the
tile ring with `heap_caps_malloc(18640, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)` —
2 tiles, **37,280 B**, the largest single internal-DMA consumer on the board, and
one of only two places the SD driver's asks can come from. It is already reclaimed
and re-acquired cleanly elsewhere in the same file (`s_scratch` is freed at
`display.c:718` precisely to fund the ring, and the ring itself degrades to
PSRAM-direct submit when it cannot get 2 tiles). "The clock may stop for the
duration of the upload" therefore converts directly into a code change: park the
### 17.6 Upload reproduction on the restored image — the answer to "can I upload?"

Run on `4367e7a` (radio config identical to `e3733a5`), via `tools/m5_armA.sh` with
the serial epoch as the clock, `/tmp/m5_armA2_ser.txt`:

| Step | Result |
|---|---|
| `POST /api/v1/sd/rescan` | `code=200` **body transferred, 53 B**: `{"mounted":true,"card":"SD16G SDHC 29.4GB @20000kHz"}` |
| `POST /api/v1/sd/theme` | `code=000 size=0 t=120.0` — client timed out with no response at all |
| Serial at 31.34 s | `dma_utils: esp_dma_capable_malloc(125): Not enough heap memory` |
| Serial, same instant | `orb_theme: upload: create failed (I/O error)` |
| Heap at 29.45 s (`sta-connected`) | internal free=7,719 largest=3,328 **dma_free=4,315** |
| `POST /api/v1/sd/install` | refused: `install: no card in the socket`, `job done` |

So: **no, the upload does not work today, on any image this session produced.** The
restored config-server build reproduces the handoff's original failure to the byte —
the card mounts, and the first write dies because the sdmmc driver cannot get **125
bytes** of contiguous internal DMA RAM out of a pool reporting 4,315 B free. The
pool is fragmented below a 125 B ask. That is the whole bug.

`dma_free=4,315` next to a refused 125 B allocation is the number to hold onto: this
is not a shortage of DMA memory, it is a shortage of DMA memory **in one piece**, and
the piece that is missing is smaller than anything the four Kconfig symbols move.

Two bench-hygiene notes, both caught mid-run and both capable of producing a false
green: `/tmp/m5_up.json` and `/tmp/m5_inst.json` are reused by the script and were
45 minutes stale — the "out of memory for the card read buffer" body that
`m5_armA.sh` printed at 01:49 is the **previous session's** refusal at 00:52, not this
boot's (the real result is `code=000 size=0`). And a `/tmp/st4.json` that appeared to
show orb-B's identity at orb-A's address was likewise an Oct 13 15:48 file. Read the
serial line and the file's mtime before believing any HTTP body on this bench.

**The fix this evidence selects.** Free the display's tile ring for the duration of a
card write. `display.c:721` holds `2 x 18,640 B` = 37,280 B of
`MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA` — the board's largest single internal-DMA
consumer, and it is already heap-allocated, already freed wholesale on the
`!s_tiles_ok` path, and already survived by a documented PSRAM-direct degradation.
Reclaiming it around the upload handler gives the sdmmc driver ~37 KB instead of a
fragmented 4,315 B, restores the config server's own 8 KB untouched, and costs the
dial exactly what the owner licensed: a stall for the length of the copy. It needs a
release/restore pair in `display.c` and a call in the upload/install paths — code, not
`sdkconfig`. **No Kconfig change is required, and §17's and §17.5's rejections of the
radio knobs stand.**

dial, free the ring, hand ~37 KB of internal DMA back to the sdmmc driver for the
length of the copy, restore the ring and redraw. That is a handler-scoped pause in
`orb_theme.c`/`orb_net.c` plus one release/restore pair in `display.c` — not a
boot-time budget renegotiation, and not a Kconfig search.

The board is flashed with `4367e7a` (both config commits reverted, tracked defaults
back at their `e3733a5` state, audit `want=4367e7a have=4367e7a`) so the orb is
usable and the named failure is on the bench rather than papered over.

**Open, unchanged by this section:** the upload has still never been proven on a
boot with a healthy DMA pool; the 413 mis-map on `esp_dma_capable_malloc` failure is
still open; and D30 stays unrationalised, still owed its two timings.

---

## 18. §17.4's two owed code fixes land; the upload proof runs against the owner's flash — 2026-10-15

§17.4 item 3 named the last two code debts of the upload path, and §17.5/§17.6
selected the fix (free the tile ring for the length of the copy) but left it
undocumented between `99f1fc3` and this section. The loan itself shipped the night
of Oct 14 (`829d8ca`, corrected by `f7481db` after the first boot proved the loan
and died on the two-buffer refund); what follows records the rest.

**The 413 mis-map is closed.** `h_app_upload` and `h_theme_upload` answered
`413 Payload Too Large` for ANY failure of `orb_*_upload_data()`, which made the
DMA starvation §17.6 measured — 125 B refused with 4,315 B free, a capacity
failure — report itself as a size verdict. Both handlers now branch on the
returned `esp_err_t`: `ESP_ERR_INVALID_SIZE` → 413 (the body outgrew the declared
cap, and that is the ONLY thing 413 now means on this device),
`ESP_ERR_INVALID_STATE` → 409, `ESP_ERR_NO_MEM` → 507, everything else → 500
"the card refused the write". Both `orb_*_upload_data()`s read `errno` at the
failed `write()` and map `ENOMEM` (FatFS's `FR_NOT_ENOUGH_CORE`) to
`ESP_ERR_NO_MEM` instead of the flat `ESP_FAIL`, and log the errno — §17.6's own
diagnosis was "the same internal-DMA squeeze this project keeps billing," and the
serial line that names it now always accompanies the refusal.

**The installer's chunk buffer got the ladder.** `orb_theme.c:751` has granted the
read buffer in three rungs (4096→2048→1024) since `e3733a5`; `orb_app_install.c`
kept asking a flat 4 KB on the same board §16.1 measured at `largest=3,328`. P4's
install copies card→flash through that buffer on the worker, so a post-association
boot with a fragmented pool could never install an app while it could install a
theme. The module now mirrors the theme ladder verbatim (`s_chunk_bytes`, every
clamp site reads the grant, `APPS_CHUNK_MIN 1024` stops where the command overhead
leads the data), and the theme file's remaining `CHUNK_BYTES` reads at
`slot_parse_geom`/`card_check_geom` were checked by hand: they clamp into their own
`geom_size` blob allocation, never into `s_chunk`, so they stay constants.

**Host suites:** `host_sd_test.py` green, `--mutations` all caught (`EXIT=0`
14:07); `host_orbapp_install_test.py` green. Neither pinned the old 413 block or
the flat ask. `idf.py reconfigure && idf.py build` clean; the bundle re-packed via
`make firmware` at `v1.1.0+11076ca-dirty` (5 images, 1,330,907 B), and the
`BuiltInBundle` disagreement test passes against `build/`.

**The bench run is owed to this session's flash — by the owner, through the hub.**
`tools/m5_upload_proof.sh` is the ladder: writer-regenerated `hsb_color` fixture,
open-reset serial capture, lease→rescan→upload→list→install→reboot→enable, with
the 413-still-alive check and the loan witnesses (`tile ring borrowed`,
`esp_dma_capable_malloc`) read off the capture. Run it detached (its own header
carries the command). The pass lines this leaves open after a green run: the
theme-side upload (same handlers, needs a re-baked FORMAT 2 file — §16.1's
`m5_avkit.orbtheme` is gone from /tmp), M5's two timings, D30's two numbers, and
the power-cut-mid-install pass line (§8's still-unbenched half).

### 18.1 The upload is proven — orb-A, 2026-10-15, `fw v1.1.0+11076ca-dirty`, UPLOAD_PROOF PASS

The owner flashed this session's bundle over the hub, and the ladder ran green on
`/tmp/m5p_ser.txt` (verdict line at 14:17:19). §17.4 item 3's first clause —
"the upload has never been proven on this board with a healthy DMA pool" — is
closed for the app path, and the honest-error fix stood in the only witness that
matters: a live upload answering `code=200` and no 413 anywhere.

| Step | Result |
|---|---|
| Boot with the loan build | lease at uptime 6.9 s; `tile ring: 2 x 18,640 B` normal fund |
| Heap at association (first boot of the session, same image) | `internal free=41,179 largest=14,336 dma_free=33,675` — the healthy pool §17.6 never had |
| `POST /api/v1/sd/rescan` | 200, 53 B transferred: `SD32G SDHC 29.1GB @20000kHz` |
| `POST /api/v1/sd/app` (4,096 B fixture, fresh from `orbapp.write_container`) | **`{"state":"stored","bytes":4096}` first try, t=2.3 s** |
| Serial at the write | `tile ring borrowed by the card: 37280 B internal DMA RAM free (largest block 21,504 B)`, then `uploading an app to the card`, `app "hsb_color" uploaded to the card as hsb_color.orbapp` — 102 ms between begin and rename |
| `esp_dma_capable_malloc` in the capture | absent, all 90 s |
| `GET /api/v1/apps` | card tier lists `hsb_color` `ok:true`; installed tier empty at that instant |
| `POST /api/v1/apps/install?id=hsb_color` | 200 `installing`; worker: `installed "HSB Colour" (hsb_color) into app slot 0 in 28 ms`, `job done, 540 B stack never used` |
| Ledger after install | `installed:[{slot:0, enabled:false, walkable:true}]` — the installer's chunk ladder took the copy on a live boot, D9's no-reboot install confirmed live by design |
| `POST /api/v1/apps/enable?id=hsb_color&on=1` | `{"state":"enabled"}` |
| Dial through the whole ladder | held 12–15 fps (the stall the owner licensed was under one frame) |

**Two script corrections found by the first run, both landed:** the lease window
went 60→150 s (the radio re-associated at 85 s on that boot and the §15 "two HTTP
timeouts" temperament is real — the retry booted in 6.9 s), and the install step's
witness changed from an `rst:0x` reboot boundary to the worker's
`into app slot` line plus the ledger read: **a card-app install is live by design**
(`orb_app_install.c` contains no `esp_restart`; the reboot belongs to the theme
path under D11). The first run burned its 120 s reboot wait against a promise the
design never made, and still PASSed.

**What item 1's closure does NOT cover:** the theme-side upload shares the now-honest
handlers but has not re-run since `e3733a5` (needs a re-baked FORMAT 2 file), M5's
flip timings are still unmeasured, D30 still owes its two numbers, and
power-cut-mid-install still needs a reachable `failed` mark. §8's P4 pass lines
keep their two unbenched halves.
*(Every item in that sentence is now closed by §19, on the same board state.)*

## 19 — Theme upload proven, M5's two timings paid, the power cut lands — orb-A, 2026-10-15, firmware unchanged since §18.1

One session, three owed lines, all at the board with the §18.1 image. The fixtures
were rebuilt first, so nothing here rests on a /tmp file that predates the session.
**Bench state after this section: the dial draws av-kit from slot-B (first real theme
install on this card), `hsb_color` is installed in app slot 0 and enabled, and the card
holds av-kit, av-kit-b, and the canonical 4 KB app fixture — the M5 precondition stays
met for a re-measure without any re-bake.**

### 19.1 The theme-side upload — `tools/m5_flip.sh`, M5_FLIP PASS

`assets/sources/av-kit/README.md` is the re-bake recipe; it ran clean on a rebuilt
`/tmp/bakevenv` (pillow 12.3.0, numpy 2.5.4 — unpinned, as that README says). The
container came back at exactly §16.1's byte counts — FORMAT 2, 1,177,659 B, 1,173,563 B
of pixels — named `av-kit` (card id `c38cd33b1c35`). The second bundle the flip needs
came from the sanctioned writer on the container's own bytes: `read_container` then
`write_container` under `av-kit-b` (id `7f41c37febbe`), pixel blob verified identical,
so the two bundles differ in name alone. The bake's default `--bin`/`--header` overwrite
of `assets/clock_assets.bin` (the 59-byte fringe the README predicts) was caught by
`git status` and reverted with `git checkout --`; pass `--bin`/`--header` out of tree
if a re-bake ever needs the pair too.

Both uploads ran on the associated boot: `POST /api/v1/sd/theme` answered
**`{"state":"stored","bytes":1177659}`, t=16.5 s (≈71 KB/s at rssi −62), first try**,
no 413, no `esp_dma_capable_malloc` anywhere in the capture, and `GET /api/v1/sd`
came back `card_count:2` with both ids. The theme path and the app path now share a

### 19.2 M5 Arm A timed both directions; Arm B's halves read off the same run

Same two bundles on the card, one live boot, every wait judged by a transferred body.
`tools/m5_flip.sh` does the pair and prints its own table:

| | to `av-kit-b` | back to `av-kit` |
|---|---|---|
| copy+verify, from the board's line | `into slot-B in 3835 ms` | `in 5423 ms` (slot re-erase) |
| POST → first dial after `rst:0xc` | **7.04 s** | **9.04 s** |
| POST → config page answers | 8.05 s | 10.05 s |
| min dial fps inside the copy window | 4 | 4 |
| implied copy throughput | ≈307 KB/s | ≈217 KB/s |

The boundary witness each time: `installed "X" (id) into slot-B — the dial reboots to
draw it`, `rst:0xc (RTC_SW_CPU_RST)`, then `art bank mapped: 1173563 B ... from slot-B`.
Both directions copied into slot-B — with only one theme slot ever populated, the
inactive-slot choice degenerates, and the second copy paid the re-erase (3835 → 5423 ms).
§16.1's constant (≈2.0 s dial + 3.3 s association) now has its full-ladder form: a
clicked flip is **7–9 s to a live dial**.

Arm B, without shipping it, §16's checklist step 4. PSRAM half: `heap_psram` read
7,463,488 before the two installs and 7,463,408 after — the whole flip ladder costs
80 B, so a lazy bundle's residency is the payload's own 1.17 MB of the 7.46 MB free,
exactly the bill D14 already refused at boot (471 ms to fill per 3.86 MB). Flush half:
**the dial fell to 4 fps during Arm A's own worker copy** — and Arm A hides that stall
behind a reboot. A lazy arm would show it on the glass, every flip. D30's two numbers
are paid: Arm A costs 7.0–9.0 s all-in (copy 3.8–5.4 s, boot-to-mapped 0.9 s, the rest
association); Arm B's honest floor is the copy alone with a visibly starving dial during
it. Per-bundle-size stays allowed: at 1.17 MB the arms differ by ≈2× and share the same
card-read bill — the reboot buys isolation, not speed. **The owner ruled the same day:
neither arm as drafted — see §20.**

### 19.3 The power cut mid-install — `tools/m5_powercut.sh`, M5_POWERCUT PASS, trial 1

The cut was real and remote: `POST /api/v1/reboot` arms a fixed 750 ms esp_timer that
does not drain `orb_jobs` (`orb_net.c:3688`) — the polite door is blunt enough. It
needs a window wide enough to die in, and the 4 KB fixture has none (its whole ladder
measured 28 ms in §18.1). The lever is `orbapp.write_container`'s geometry argument at
the DEVICE's slot ceiling: 0x3FE00 of opaque bytes puts the blob at exactly
`APPS_SLOT_GEOM_CAP` (the accepted boundary, `orb_app_install.c:61`), which stretches
erase→header to a 906–1346 ms measured window. Trial 1 — install queued at 14:40:06,
fuse lit 50 ms later — landed inside it.

The chain, all four claims:
1. **Torn:** ledger read `"installed":[]`, no `into app slot` line after the arm, and
   the boot after the `rst:0xc` logged `resolved 0 installed apps from the "apps" partition`.
2. **Dial boots:** the same boot mapped `art bank mapped: 1173563 B from slot-B` and
   the `clock:` fps lines resumed.
3. **Never half:** the card row stayed listed `ok:true` at 262,144 B; the app read as
   absent, exactly as D9 promises (orb_app_install.c:687-691).
4. **Recoverable:** re-upload of the canonical 4 KB fixture, re-install —
   `installed "HSB Colour" (hsb_color) into app slot 0 in 40 ms` (the slot was already
   erased) — and the ledger returned green with `enabled:true` from its stored NVS word.

**The `failed`-mark half of P4's pass line is therefore retired, not passed.** A torn
install leaves no row to mark: D9's written-order rule makes the app ABSENT, and the
board's only reachable `failed` marks stay C2's fault counter and `init()` refusal —
both blocked on a second behaviour kind, the same blocker as the `*`-on-the-ladder line.
The bench also caught a sentinel gap on the way: `orbapp.write_container` accepts
`geometry` at the full 0x40000 ceiling, but the device computes
`geom_off + geom_size` (the 176 B manifest header counts), so that boundary container
is refused pre-erase with an honest why. The writer should clamp geometry to
`CEILING − geom_off`; owed as a one-liner in `orbapp.write_container` with its
sentinel boundary updated in the same pass **(PAID 2026-10-15: §21)**. (§19.4's recovery
commands belong beside
the verbs in `docs/wsl-usb-reattach.md` when that doc is next touched — its
doc-test pins wording, so it was not edited blind).

### 19.4 Bench lessons, this session

- Five power-cut trials strand the usbip cdc-acm link (RTS ioctl: Errno 62, the device
  stays HTTP-alive). Recovery without touching the wire:
  `sudo modprobe -r cdc_acm && modprobe cdc_acm`, then toggle
  `/sys/bus/usb/devices/1-1/authorized` 0→1 if the by-id node vanished. The
  `m5_powercut` loop judges by the LEDGER over HTTP for this reason; serial is
  witness, not verdict.
- Never run a tracked script from its tracked path while editing it — bash re-reads by
  byte offset and a mid-run edit corrupts the live interpreter. Run a frozen `/tmp` copy.
- The bake's default output pair lands on the committed `assets/` files. Check
  `git status` after every re-bake.

## 20 — D30 is ruled: the PSRAM hybrid, default-first (owner, 2026-10-15)

### 20.1 The question, the corrected arithmetic, and how it resolved

Two of the owner's questions settled the arm choice.

*Must the old and new themes occupy PSRAM at once?* No — the default theme costs zero
PSRAM (the baked-in mmap is already resident), so a flip routes through the default and
every bundle fits with ONE arena resident. The /2 double-arena arithmetic and its
per-bundle-size split die with that answer.

*Does a card-resident theme run as fast as a flashed one during normal ticking?* No, on
this bus. A dial reads art every tick: av-kit's 1,173,563 B bank over 15 base frames
averages ≈78 KB per frame, which is ≈1.17 MB/s at 15 fps against a card ceiling of
1.31 MB/s measured on the bench (`.clinerules:730` — the whole 3,867,066 B kit3 container
read and CRC'd in 2952 ms, with the HTTP routes answering during the pass). That is 89 %
of the ceiling on a single SPI bus whose mutex the menu scan and any upload also hold;
worst case (a full 466×466 RGB565 frame, 434 KB) wants 6.5 MB/s, five times the ceiling.
So map-in-place streaming off the card is struck: it cannot tick, and it would make
every menu and upload wait behind the dial.

One correction to §19.2's reading of its own table: its ≈217–307 KB/s is the
read+erase+write+verify *ladder*, so the copy window is **flash-write-bound, not
card-bound**. That works for the hybrid — the fused dual-sink pass pays one card read
(≈0.9 s of the measured 3.8–5.4 s window) plus one slot write plus a nearly-free arena
memcpy, so writing both sinks costs the same window as writing one.

The ruled design is therefore Arm B's arena lit by D30 with Arm A demoted to fallback:
lazy flip, flash as the cross-reboot cache, card touched exactly once per flip and never
on the boot path.

### 20.2 The ruled rule

**Flip** (menu-driven; the clock does not paint during it, so the §19.2 flush
interference is moot by context — a status page owns the glass):
1. Select the baked-in default bank. Instant, free; it frees the live theme's arena.
   The page now shows the default dial plus a progress row.
2. Stream the new bundle once from the card, fanning each chunk to BOTH sinks: the PSRAM
   arena and the inactive flash slot. CRC-verify both while reading (the streaming
   verify already proven at §18). One window of ≈4 s at av-kit size — write-bound per
   §20.1, and uncontended now that the clock stopped competing for the bus (unproven;
   §20.5).
3. Swap the live bank pointer at a tick boundary. The new dial draws.
4. Mark `staged` in the ledger until the slot header lands; the header write is the
   commit, last, unchanged from D9's order.

**Boot:** read the slot header; if committed, refill the PSRAM arena from it, fused
with the CRC verify — one pass, the same code as the flip's flash sink in read-only
shape. Cost: D14's fill arithmetic on flash, 471 ms per 3.86 MB ⇒ ≈150 ms at av-kit
(1.17 MB), ≈470 ms at the 4,128,768 B cap. No card dependency. An empty or corrupt slot
follows today's ladder (other slot, baked-in fallback).

**Failure story:** power loss between arena-swap and header-commit ⇒ glass showed the
new theme; on reboot the flash holds the OLD commit and the boot draws it — flip
rolls back, `/themes` says so with the `staged` word. The owner accepted a coarser
version of this (rollback to default, not to the original, if the flip staged from
default); the implementation should still offer the exact committed theme's flip-back
when the arena allows, which costs nothing extra given step 2 already wrote the other
slot.

**Arm A fallback:** if the arena cannot allocate the new bundle at flip time
(fragmented PSRAM, or a bundle larger than free), the picker says so and installs with
the shipped copy+reboot path instead. No silent degrade.

### 20.3 What this retires

The /2 double-arena rule (only the default is live during a flip). The per-bundle-size
split D30 pre-authorized — lazy now covers every bundle ≤ D29's cap. Arm A as the
primary flip. Card-resident map-in-place (B2) — dead on arrival by §20.1's throughput
math. D30's `Unrationalised pending measurement` label and its two-arms-in-flight
wording, both superseded by this section and the amended row.

### 20.4 Consequences at the seam (§1.2b) and the D13 amendment owed

- **Mech B lit.** `ORB_ART_MECH_B` flips 0→1 (`art_bank.h:98`); D14's "ships dark"
  note gets the D30 rider — B is now the runtime mechanism for installed themes, and
  A is boot's mapping and Arm A's fallback. `tools/host_art_residency_test.py:72`
  pins the `0`, so the byte and that pin move in the same commit.
- **D13 amended, ping-pong.** The ratified "A = default, B = install" pairing is
  superseded for themes: both slots become install targets in ping-pong (same model
  apps already run), and the baked-in `assets/clock_assets` mmap becomes the sole
  card-less fallback — the "A is never an install target" clause existed to protect
  slot-A pixels the fallback read; the mmap fallback needs no such protection. The
  flip is the selection, and the arena makes it instant (§1.2b's own table already
  priced an mmap↔arena switch at 150–400 ms).
- **D14's rejection stands, its target moved.** What D14 refused was *card-at-boot*
  refill; D30 pays card reads once per flip instead and re-fills from flash at boot.
- **D29's cap is now the arena's bill too**: one resident bundle ≤ 4,128,768 B of
  ~7.4 MB PSRAM, permanent while installed.
- **Ledger word:** `staged` joins the status row between arena-swap and header-commit
  (§18's field-shape precedent).

### 20.5 The bench gate before P6 ships it (M6)

Four measurements, same rig as §19, before the flip UI relies on any of it:
1. The uncontended fused-copy rate — clock dark, menu page up, worker streaming
   av-kit to both sinks; the §19.2 copy numbers (3.8–5.4 s) are the contention-paid
   comparison.
2. Boot arena-refill from slot, at av-kit size and at cap — the predicted 150/470 ms
   must be measured, not assumed, and it lands as a line in the boot log.
3. A power cut inside the commit window (the `m5_powercut.sh` idiom, retargeted at the
   header write): must roll back to the committed theme with the `staged` word set and
   the dial alive. **PAID 2026-10-20 on orb-A — `tools/bench_storecut.sh`, BENCH_STORECUT
   PASS**; the torn copy also cannot theme the dial: the flip onto it fails safe at boot's
   adopt with one sentence. §26's tail carries the witnesses.
4. The fallback refusal path: pin PSRAM so the arena cannot allocate a cap-sized
   bundle, and confirm the picker names the reason and offers Arm A.
Arm B2 (map-in-place off the card) stays unbenched on purpose — §20.1 killed it on
arithmetic, and re-opening it requires new numbers, not hope.

## 21 — The writer clamp: the slot boundary is a sum — orb-A host, 2026-10-15

§19.3's owed one-liner is paid. `orbapp.write_container` now refuses a container whose
`geom_off + geom_size` passes `GEOM_BYTES_MAX` (0x40000), in the device's own words —
`its manifest and geometry are 262320 B: a slot holds 262144 B — re-bake the app
smaller` — the sentence `install_decide`'s gate 1 (`orb_app_install.c:62-68`) prints
pre-erase. A writer that emits what the dial refuses spends an upload and an erase to
teach the owner what the host already knew, which is the rule D26 states for refusals.
Host-only change: no firmware touched, no rebuild, no reflash.

**The correction to §19.3's wording:** the boundary is not `CEILING − 176`. `geom_off`
is derived from the header plus the settings table plus the asset table, so it moves —
measured on the writer: **0xB0 bare, 0xF0 with one setting, 0xC8 with one asset row**
(`SETTINGS_WORDS` is 16 words = 64 B; an asset row is 24 B). A hard-coded
`CEILING − HDR_BYTES` would repeat the bug one row larger, so the clamp reads
`hdr[W_GEOM_OFF] + hdr[W_GEOM_SIZE]` after both are derived. `== cap` stays ACCEPTED,
matching the device's "the last word before the header sector" rule.

**Sentinels** (`tools/host_orbapp_test.py`): four vectors — the sum refusal pinned by
`wants="a slot holds"` so the blob-only cap next door cannot pass for it; the exact
boundary accepted at `== cap` (file lands on 262,144 B); the same blob refused once one
setting moves `geom_off` 64 B; and the moved boundary accepted to the byte. One mutant
("the slot-sum gate is gone") is caught by the first. Suite: **178 checks, 0 failed;
25 mutants, all caught.** `tools/m5_powercut.sh`'s fixture comment is corrected the same
pass — its 0x3FE00 pad is still admissible (blob `0x3FEB0`), so the bench lever survives
the clamp.

**The same class of gap found on the theme side, and deliberately left:**
`orbtheme.write_container` imposes NO geometry bound — `pack_header` takes
`len(geom_bytes)` and writes it. The device refuses `geom_size > SLOT_GEOM_CAP`
(60 KB) at `orb_theme.c:287` and again pre-erase at `:1337`, so a bake with an oversized
geometry blob uploads and dies on the dial. It is not patched here: theme geometry sits
at a fixed `SLOT_GEOM_OFF` (no sum term), the FORMAT 2 dialect retires at P6, and D29's
budget gate already owns the bake-time bound for FORMAT 3. P6's budget-gate pass takes
this with it. **PAID at P6.2 (2026-10-15, §23.6): `write_container` now refuses both
device sums, in `copy_into`'s verbatim sentences.**

## 22 — P6 design draft: FORMAT 3, the gate, the flip, the reverse path (DRAFT)

Written 2026-10-15 off the ratified rows. **Nothing here is ruled except what §22.6 puts
in front of the owner.** Facts the draft rests on carry a file:line at §22.8, so the
proposal can be argued against the tree rather than against prose.

### 22.1 What P6 owes

From the rows: **D21** (a container ships no art; art arrives only in a theme bundle),
**D22** (the fallback tier is five generated primitives, no shipped bitmap), **D23**
(frozen class enum plus the owner-scoped named-slot table), **D24** (FORMAT 3 only, no
backward path, the shipped kits get re-baked), **D29** (4,128,768 B per bundle), **D31**
(`required` refuses at install, `optional` draws the app's own default, and an app's
default layout rides its own `geometry` region), **D30/§20** (the flip mechanism), and
the P6 pass line: av-kit re-bakes to a FORMAT 3 bundle that boots, a FORMAT 2 file dies
at the format gate with no fallback reader, and a card-less board still tells the time.
Two inherited debts ride with it: the theme writer's missing geometry bound (§21), and
retiring the clock's FORMAT 2 dialect.

### 22.2 FORMAT 3 layout (proposed)

FORMAT 2 holds ONE app's bank (the clock's) behind fixed offsets. FORMAT 3's whole job
is N of those in one file, addressed by app slug — the index shape the app manifest
already uses for asset rows (`n_assets`/`asset_off`).

    header   FORMAT=3; the word-3 hdr_size the format already carries, so a wider
             header needs no new rule; n_sections; index_off; the scr_w/scr_h
             scalars MOVE into the section (a bundle may re-layout one app and
             leave the others alone)
    index    n_sections ≤ 8 records (the app cap, B5), each:
                 slug[24], abi_min, abi_max,       <- per-section dialect, C4's
                                                      ladder, not the file's
                 geom_off/geom_size/geom_crc,
                 art_off/art_size/art_crc
    section  per app, in index order: that app's geometry blob (opaque, D15), pad
             to 4096, that app's art bank

In-slot placement keeps today's rule (geom at `+0x1000`, art at `+0x10000`,
`orb_theme.c:90-95`): the index plus the geometry blobs share the 60 KB
`SLOT_GEOM_CAP`, and the banks share everything above `SLOT_ART_OFF`. A section whose
bank is absent is a D31 question at resolve time, not a corrupt file.

> **Superseded in part (2026-10-15):** D32 (§23) rules the theme's per-app content a
> **legible element table** — identity, class, pixels, frames, mask, depth order — so the
> `geometry blob (opaque, D15)` line above does not stand for theme sections. The index
> shape, the ≤ 8 sections, the 4096 padding rule and the in-slot placement all survive.

### 22.3 The budget gate, both ends (D29, D26)

- **Bake end** (`bake_clock.py --orbtheme`, and the composer's `orbtheme-baker.js`):
  refuse over 4,128,768 B, over the 60 KB geometry region, over 8 sections, and over the
  per-slot art bound — each refusal in the sentence the device prints, which is D26's
  rule that the owner learns a cost where they already see refusals. This is also where
  §21's unbounded `orbtheme.write_container` gets its bound.
- **Install end**, before the erase and matching `orb_theme.c:1337-1338`'s existing
  shape: the sum of the sections against `ORB_THEME_SLOT_CAP`, `== cap` accepted, and
  the per-section streaming CRC §18 already proved.
- Both ends are pinned by one new host suite (`tools/host_theme_format3_test.py`) under
  §4.1's three-way mirror rule: writer, C reader and sentinel move in one commit.

### 22.4 The flip UI and worker, per §20

A THEMES page lists the card's readable bundles with each one's size and the slugs its
index names — a pick that changes three dials says so before it is pressed. The pick
posts one job: select default → fused card pass into the arena and the inactive slot,
with a byte-count progress row → per-section CRC verify → swap at the tick boundary →
header last. The status word runs `staged` → `live`, and the page keeps a `staged` row
honest. Every refusal prints a reason instead of spinning forever: the arena cannot
allocate (offer Arm A), the card vanished mid-copy (roll back, clear `staged`), a section
speaks a class ABI this build does not (the `abi_min`/`abi_max` ladder, D23). M6
(§20.5) gates the commit that ships it.

### 22.5 The reverse path

`bake_clock.py` gains the inverse of `--orbtheme`: read a FORMAT 3 bundle, take one
section by slug, and emit the `assets/clock_assets.{bin,h}` pair exactly as the kit path
does — so the shipped av-kit pair is reproducible from a bundle and not only from its
source kit, which is the clause §0.2 still owes. The pair rule stands (one bake, both
files, never hand-edited, `.clinerules:447-450`). The composer stays write-only: D28
makes the firmware resolver the truth, so a browser-side reader is not part of this path.

### 22.6 Rulings P6 needs before its first commit

- **R1 — RULED 2026-10-15, and reframed: see §23.** The theme carries arrangement, in one
  shared legible schema — not the app's private dialect. §22.2's per-section opaque
  geometry blob is superseded by §23.3's first bullet.
- **R2 — a section naming an app that is not installed.** Refuse the bundle at install
  (D26's shape), or install it and let it go live when that app appears later?
- **R3 — is a subset bundle legal?** D31 reads as yes (uncovered apps fall to their own
  defaults); the answer decides whether the installer checks coverage at all.
- **R4 — identity rule unchanged?** Keep `theme_id = hex(sha256(name))[:12]` as the card
  stem with the header name as the identity (the owner's rule,
  `theme-identity-plan.md` §1). The draft assumes keep.
- **R5 — the flip's entry point.** THEMES beside APPS on the top page, or under
  SETTINGS. Cosmetic, and the walk suite pins page order, so it is a commit-time choice.

### 22.7 Sequencing, on P5's decomposition pattern

1. **P6.1** FORMAT 3 writer, reader, sentinel suite. Host-only; refuses FORMAT 2 in the
   tools; no device ingest.
2. **P6.2** the budget gate at both ends, plus §21's deferred theme-writer bound.
3. **P6.3** device ingest: ping-pong slots (the D13 amendment), mech B lit for themes
   with `host_art_residency_test.py:72`'s pin moved in the same commit, the boot arena
   refill, the `staged` word. Bench item §20.5(2) lands here.
4. **P6.4** the flip page and worker, with M6's four measurements as the pass line.
   **LANDED and BENCHED 2026-10-19 (§24.8, §24.9):** the device's door owns the
   format check; `/api/v1/sd/theme` now stores a verified FORMAT 3 bundle; the scan lists it
   (`fmt`, `sects`); `POST /api/v1/sd/bundle` is the first live caller of
   `orb_theme_install_bundle()`; the page's Store button carries the power-cut sentence
   §24.5(2) assigned here. M6(1) over HTTP: 4398 ms at av-kit size. M6(3)'s bundle half:
   the cut landed, the witness and recovery are proven. The progress-bar-on-glass flip and
   cap-size refill remain behind P6.5 (they need the default's new home and the reader).
5. **P6.5** the reverse path, av-kit re-baked to FORMAT 3, and the FORMAT 2 dialect
   deleted from the firmware.

**Bench prerequisite ahead of M6:** fresh FORMAT 3 fixtures for av-kit and av-kit-b. The
§19 fixtures are FORMAT 2, and P6.5 refuses them, so M6 cannot be run against them.

### 22.8 Facts verified while writing this draft

- `ORB_THEME_SLOT_CAP` is `0x3F0000` = **4,128,768 B** (`orb_theme.h:372`). The plan said
  4,128,764 in four places until this commit.
- Slot placement and bounds: `orb_theme.c:90-95` (`SLOT_GEOM_OFF` 0x1000, `SLOT_ART_OFF`
  0x10000, `SLOT_GEOM_CAP` 60 KB, `SLOT_CAP` 4 MB), checked at `:287` (geometry size),
  `:289` (art size), `:1337-1338` (the pre-erase sums).
- `theme_id` is lowercase `hex(sha256(name))[:12]` over the name bytes
  (`orb_theme.c:329-342`), mirrored in `orbtheme.py`; the filename carries no identity.
- FORMAT 2's file layout is header (32 words), geometry, pad-4096, art
  (`orbtheme.py:333-337`), and its word 3 already carries `hdr_size`, so a wider FORMAT 3
  header breaks no existing assumption.
- The registry answers three tiers today (`orb_assets.h:23-29`); FORMAT 3 changes what
  tier 1 is made of and nothing about the ladder.
- Doc rot found while citing: line 668 cites `docs/theme-naming-todo.md:123`, a file
  deleted at §13.8.

## 23 — D32, as heard: the theme declares the elements, the OS schedules the rate

Ruled by the owner 2026-10-15, in answer to §22.6 R1, and bigger than it: the answer to
"does a section carry geometry" is that the theme carries a **legible element
declaration** instead of the app's private dialect. Recorded here as heard, with the fit
measured against the tree. The four ratified rows it re-opens are listed at §23.4 with
the amendment proposed, and **none of them is edited in §0 until the owner confirms the
wording** — this section is the mulling, not the amendment.

### 23.1 The ruling, as heard

1. The **theme** carries, per app: which elements (sprites, complications, backgrounds)
   the app displays, each one's pixels, its animation frames, its opacity mask, and the
   **depth order** of the stack.
2. The **app** tells the OS how and where to paint each element it was given, and when it
   needs repainting. Placement stays the app's: "the app tells the OS where and when".
3. Every element is **bound to the theme** — its identity is (theme, app, element), so a
   different theme re-composes the same app.
4. The **OS** composites in the declared depth order, and for each live app's stack picks
   the **highest frame rate it can afford** out of a declared acceptable set — a watch
   ladder like 4/8/12/15 fps, another app 1 fps up to as-fast-as-possible — then asks
   that app to repaint at that pace. The choice is adjusted dynamically.

### 23.2 The fit: most of it exists, in clock shape

| The ruling needs | The tree already has | What generalizing costs |
|---|---|---|
| A theme-carried named element set, joined to an app | `orb_asset_decl_t` (cls, required, `slug/name`) and the answer `orb_asset_t` (cls, tier, x/y/w/h, pivot, prim, off, bytes) — `orb_assets.h:165-196` | the table moves from the manifest into the theme's section. **The answer struct already carries placement, so it survives unchanged.** |
| Sprites, backgrounds, opacity masks | classes SPRITE/HAND/PLATE/RING/BANK/TEXT/LAYER/STATUS; `orb_asset_slot_bytes` prices pixel classes as colour `w*h*2` **plus alpha `w*h`** | nothing new: the mask already IS the alpha plane. Blend modes exist (`orb_layer_desc_t.blend`: normal/multiply/screen). |
| Animation frames carried by the theme | the `bank` class (indexed run of plates), `gif_delays`, `bg_frames`/`bg_fps`/`bg_loop`/`bg_every`, `tourbillon_fps` — all FORMAT 2 geometry fields | move them from clock scalars to per-element columns. No new byte class. |
| A depth map the OS paints in order | the theme's `order0..4`+`order_n` scalars, the LAYER class, and 8 layer rows per app (`orb_layers.h:48-74`, `ORB_LAYER_MAX 8`) | one order column per element row, replacing a 5-slot scalar. |
| The app saying where and when | `layer_add`/`layer_set`/`layer_touch`/`layer_del` (`orb_svc.h:146-149`), with `ang_10`, `show`, `blend` and fallback styling in the descriptor | **unchanged** — this half is already the app's, and is the half the ruling leaves alone. |
| Per-element invalidation, so a rate ladder can be cheap | `layer_touch(app, h, area)` takes a damage rect | already there. A ladder is mechanically feasible on day one. |
| The cost model the pick reads | the measured `flush_ms = -33.2 + 215.1 µs/row + 648 ns/real-layer-blend + 95 ns/synthetic-blend`, plus per-role blend counts (`CLK_PAINT_ROLES 8`) | reused as the pick's input. Rows are the driver — §13.4 proved it — so the pick must count rows, not elements. |

**Genuinely new, after that table:** the declared rate *set* (today's manifest carries one
`period_ms`, the fastest beat), and the OS re-picking it dynamically. Everything else the
ruling asks for is already in the tree wearing clock clothes.

### 23.3 What it retires, and what it forces

- **§22.2's per-section opaque geometry is void.** A theme's element table cannot be
  opaque: an OS that composites in declared order and schedules per-element repaints has
  to read it. So the dialect is **one schema for every app** — `tools/theme_schema.py`
  grows it — and D15's opacity survives only where it was always bound: the app's own
  default layout, inside the container, for a silent theme.
- **D16's deferred beat delivery dies of its own blocker.** §0.6 deferred the scheduler
  "for want of a second tick-declaring app". This ruling makes the OS the one who asks
  for repaints, so the delivery machinery is no longer optional. That is the first
  forcing function the deferral ever had.
- **A flip can now change what an app draws**, not only what it looks like. D26's
  two-way refusal has to run at flip time, not only at install, and a flip that would
  leave a live app unable to paint its given elements must fall back with a reason on
  the glass. §20.5's M6 gate grows a fifth measurement for it.
- **The dynamic pick needs a hysteresis rule.** A measured cost that drifts across a rung
  boundary would make the dial flicker between 8 and 12 fps; D9's "never starve the rest"
  already bans letting one app's ladder eat another's frames.

### 23.4 Ratified rows this re-opens — amendments APPLIED 2026-10-15

D32's row (§0.10) carries the pointer, so the table cannot be read without meeting it.
**The paste-ready wording for all four edits is at §23.9, pending the owner's blessing.**
The in-row pointer clauses on D23/D26/D16 ship **with** the confirmed amendment, not
before it, so a row is never half-edited.

- **D23** says "the *named slots* live in the manifest's asset table". Proposed: the
  manifest declares what the app **can** paint — its offer, names and classes only — and
  the theme declares which of them are present, with what pixels, frames, mask and depth.
  The join stays owner-scoped (`<slug>/<name>`), so D23's shadowing rule is untouched.
- **D26** refuses an app that requires an absent slot, at install. Proposed second
  direction: a theme that names an element the app cannot paint is refused **too**, not
  silently dropped — the same commit already forbids silent substitution.
- **D31** survives whole. `required`/`optional` stays the app's claim about its own
  defaults; a theme that omits an `optional` element still falls to D22's five
  primitives.
- **D16** is the row with real work attached. One `period_ms` (the fastest beat, floor
  3000 ms) becomes a declared **set** of acceptable rates. The 3000 ms floor was priced
  against a *whole-dial* commit pass measured at 185 ms; a ladder that repaints one
  element's damage rect is a different measurement, so the floor gets **re-measured, not
  re-argued** — D9's lesson that the model is not the measurement. §13.4's 8 fps ceiling
  for the current carriage is the honest starting number: 12 and 15 fps are only
  affordable on stacks thin enough to touch fewer rows.
- **§1.2b's asset-table seam** moves with D23. The sentence §23.4 first quoted as "the
  manifest's asset table is that selector" was my paraphrase, not a verbatim line; the
  sentence actually amended is §1.2b's `main/assets/orb_assets.c/h` row ("…gain the
  manifest's asset table together, so resolver and writer cannot drift"), and it now carries
  the offer/declaration clause. §4.1's `period_ms` listing and §5's `set_beat` comment gained
  the same pointer so they cannot be read as the old ceiling rule.

### 23.5 Where it goes in the sequence — RULED 2026-10-15: split

The owner ruled the split. **Numbering corrected on the same pass: "P7" in the first draft
of this section collided with the ratified wasm3 tier at §8's table, so the scheduler is
P8.**

- **P6 — FORMAT 3 carries the element table.** Multi-app bundle, the legible element
  schema (identity, class, pixels, frames + loop, opacity mask, depth order, advance mode),
  the D29 gate at both ends, the flip under §20. The app still drives its own tick on
  today's single `period_ms`, so nothing here waits on the scheduler. R1 is answered inside
  this: the theme carries arrangement, in one shared schema, never the app's private
  dialect.
- **P8 (new) — the OS schedules the repaint.** The rate set in the manifest, the app's
  runtime ask, the measured pick and its hysteresis, the `adv` behaviour on the tick, and
  D16's beat delivery as its first commit. P8 owns the parts of D32 that need the delivery
  machinery the plan has deferred for want of a second tick-declaring app.

**Why R6 and R7 forced the split rather than merely recommending it.** Both rulings landed
on the scheduler side of the line: R7's set is a `.orbapp` manifest field touching
`orbapp.py`, `orb_app_manifest.c`, `disclosure()`'s priced number and the `BEAT_MIN_MS`
floor, and R6's advance mode is a compositor behaviour on the tick. Neither is a bundle
format question, and P6 has enough of those. **Neither freezes a row shape either:** the
manifest rides `abi_min`/`abi_max`, and an element table gains columns the way the class
enum gains entries — append-only — so P8 can add what it needs without a format bump.

### 23.6 Where every open question landed, and what P6.1 still owes

- **R2 — RULED with D26's amendment** (§23.9): a bundle whose index names an uninstalled app
  installs, and the picker prints the intersection. **R3, R4, R5 — RULED 2026-10-15 with the standing recommendation** (§23.8): subset
  bundles are legal, the identity rule stands, and the flip lives in a sixth top-level row
  named THEMES. All eight questions D32 raised are answered.
- **P6.1 first commit LANDED 2026-10-15:** the element schema itself — `ELEMENT_FIELDS` with
  its class/flag/bound vocabulary now lives in `theme_schema.py`, checked by
  `tools/host_theme_elem_test.py` (46 checks, 14 mutants caught, class enum linted against
  `orb_assets.h`). 17 fixed-width columns, so a later schema appends past `scale16` without
  a format bump.
- **P6.1 second commit LANDED 2026-10-15 (`76bd1cd`):** the FORMAT 3 bundle writer and its
  host reader — `tools/orbbundle.py`. FORMAT 2's 128-byte header with FORMAT/ABI stamps
  swapped and nothing widened; a slug-sorted index of 13-field records and one
  self-describing legible blob per app in the geometry region; sector-aligned banks
  byte-identical to FORMAT 2 payloads, offsets derived by walk and never stored. D29's
  gate opens here (region under 60 KB, file under 4 MB, refusals in the sentences the
  installer will repeat). `tools/host_theme_format3_test.py`: 43 checks, 15 sentinels,
  the two inner guards pinned by re-stamped-region rows; mutation harness catches 12/12.
- **P6.1 third commit LANDED 2026-10-15:** the firmware's parse mirror. `main/art/orb_bundle.c`
  checks header, region, index, blobs, banks and sortedness in `read_bundle()`'s order, its
  refusal sentences byte-identical except the two labeled header-wording pairs the suite
  pins. The u16 count cap and the check_slug rule now bind at read time on both sides;
  equal neighbouring slugs die like a shuffle does. `orb_theme.c`'s format gate names a
  FORMAT 3 bundle for what it is — a valid header gets the bundle sentence, a broken one
  the bundle reader's own reason, and the 128-byte torn-header check stays one rule for
  both formats. `tools/host_orb_bundle_c_test.py` compiles the shipped C file with host
  gcc against the writer's own bytes: 147 checks, 29 vectors, 21/21 mutants caught, the
  host_orbapp_reader_test.py discipline. What P6.1 still owes is P6.3's streaming install
  (banks verified as they arrive, the same `bank_check` rule), and the door itself flips
  with THEMES (P6.4).
- **P6.2 LANDED 2026-10-15:** the budget gate at both ends, plus §21's deferred bound.
  `orbbundle.check_budget()` states the two INSTALL sums once — region against `GEOM_CAP`
  (61,440 B, the tables between slot `+0x1000` and `+0x10000`) and pixel payload against
  `SLOT_ART_CAP` (4,128,768 B, `ORB_THEME_SLOT_CAP`) — `== cap` accepted, refusals ending
  in `copy_into`'s own sentences. It replaces the writer's two file-shape checks: the old
  geometry line refused a legal `== cap` region by the 128-byte header's worth, and the
  old file-sum line accepted a bundle whose installed art overran — the §21 bug, caught on
  themes before it could ship. `parse_header3` and `orb_bundle3_hdr_parse` call the pair
  at the header, before the name or a streamed byte, so forged sizes meet the same words
  on both readers. `orbtheme.write_container` and the composer's `writeContainer` take the
  FORMAT 2 bound (§21, paid), and `host_sd_test.py` gained the bundle-reader link stub its
  lifted door has needed since P6.1. Suites: format3 **52 checks, 22 refusals, 0 failed**;
  parity **167 checks, 37 vectors, 24/24 mutants**; `mut_bundle.py` **16/16**; the C u16
  mutant retired — the gate speaks first now, and vector 5 pins that order. av-kit
  untouched; bench items owed by P6.3 unchanged.
- **R6 — RULED 2026-10-15.** An animation carries its frames and **no rate**: no fps, no
  per-frame delay. Playback rate IS the rate the OS picks for that stack, so one tick
  advances one frame. Consequences at §23.7.
- **R7 — RULED 2026-10-15.** The manifest carries an **explicit set** of acceptable rates.
  The OS attempts the highest rung it can achieve that is no greater than what the app
  asks for; if the ask cannot be fulfilled at all, it runs at the set's nominal minimum.
  Consequences at §23.7.
- **R8 — RULED 2026-10-15: split.** P6 is the bundle and element table; the ladder and the
  picker are **P8** (the draft's "P7" collided with the wasm3 tier). Recorded at §23.5.

### 23.7 The consequences of R6 and R7, as ruled

**R6 retires every rate field the kit carries today.** `gif_delays` (the per-frame delay
table), `bg_fps`, `bg_every`, `tourbillon_fps` and the sweep/`tick_rate` pair that
`orbtheme.py`'s `THEME_FIELDS` carries all stop meaning anything: frames are data, and the
stack's picked rate is the only clock. The element row keeps `n_frames` and the loop rule;
it loses the delays. av-kit re-baked to FORMAT 3 therefore loses its per-element timing,
which is the sharpest thing this ruling costs, and it shows up as a question at P6's
re-bake: a watch whose hands want 1 Hz and whose seconds carriage wants 8 fps cannot get
both, because one app stack gets one rate. A theme wanting both needs either two apps or a
rate exception, and the advance-mode field below is where an exception would be recorded if
the owner ever wants one.

**R7 needs two manifest numbers, not one.** The declared **set** bounds the rungs the OS
may pick (`{4,8,12,15}` is four u32s or one bitmask), and the app's **ask** — the runtime
request `set_beat` already carries — names what it wants right now. The pick rule is then
mechanical: highest rung that is achievable and not faster than the ask, else the set's
lowest rung. D16's install-time price reads off the set's top, so the ladder the owner
consents to is the whole reachable range and not one number.

**The set's floor and D16's floor disagree, and that is a measurement P8 takes.** D16's
3000 ms floor was priced against a whole-dial commit pass at 185 ms; a 12 fps rung is
83 ms, which the shipped carriage cannot flush at all (§13.4: mean flush 138 ms, and the
carriage alone force-marks ~255 rows). So 12 and 15 fps are reachable only on thin stacks
or on repaints that stay inside a damage rect — and the row-counting pick at §23.2 is what
decides, on the bench, which stacks qualify.

**`bank` is not an animation.** It is indexed "one plate per value" (`orb_assets.h:75`), so
its frame is app state, and R6's OS-advance rule would march a date wheel on a clock the
app wanted to own. The element row therefore carries an advance mode — OS-advanced or
app-chosen — and R6's ruling is what forces the field to exist.

### 23.8 A correction to R5, and the recommendation on R2-R4

**R5 as drafted named a page that does not exist.** It offered "THEMES beside APPS, or
under SETTINGS". `menu_view.c:512-525` gives the top page five fixed rows — TZ, DISPLAY,
PERMISSIONS, APPS, ABOUT — and no SETTINGS among them; `PG_DISPLAY` holds the two idle-dim
settings, and `PG_N` stops at `PG_ABOUT` (`menu_view.c:70-81`). The real choice is a
**sixth fixed row** or a sub-page hung off DISPLAY. The recommendation is the row: D30 makes
a flip a daily action, and a daily action two taps deep under display settings is a feature
that stops being used. A sixth row costs one knob detent and one walk case.

**R2/R3/R4, recommended together.** A bundle is a SET of per-app arrangements, each
independently absent. An index naming an app that is not installed is not damage — it is a
theme covering more than the board has, and `NO_SLOT` already describes that state without
an error. So install it, resolve the intersection, and print "covers 3 apps, 2 installed"
on the picker so the owner knows what the flip changes. Subset bundles are legal for the
same reason: D31 lets a silent app fall to its own default, so a clock-only theme over
three installed apps is a complete design, not a partial one. Keep the identity rule:
`theme_id = hex(sha256(name))[:12]` (`orb_theme.c:329-342`) addresses the bundle, and the
index addresses the app inside it, so no second identifier is needed.

**What all eight answers point at.** A theme is a set of per-app arrangements, each
independently absent, each with a declared advance mode, and the OS owns the one clock. That
retires the last of the clock's privileges from the theme format, and it makes
`theme_schema.py` the file that defines what a theme IS: one schema, every app, no dialect.

### 23.9 The amendment wording, as applied

The text below is what went into §0 on 2026-10-15, kept here so the diff is reviewable
against the reasoning. The owner approved all four edits as written, **including the D26
sentence that rules R2**: a theme section naming an uninstalled app installs, and the
intersection resolves. **R3, R4 and R5 remain recommendations awaiting a word** (§23.8).

**D23 — replace only the second sentence, "The *named slots* live in the manifest's asset
table, owner-scoped as `<slug>/<name>`, with `global/<name>` for the frozen set.", with:**

> The *named slots* live in the manifest's asset table, owner-scoped as `<slug>/<name>`,
> with `global/<name>` for the frozen set, and that table is the app's **offer**: what it
> *can* paint. Since D32 the theme's per-app element table is the **declaration**: what is
> present, in what order, with what pixels. Offer and declaration are checked against each
> other at install in both directions, and every slot the theme does not name stays silent
> on the app's own default (D31).

*D23's rest survives untouched, including "A PR promotes a class, never bytes" — and the
append-only rule now covers element columns as well as class enum entries.*

**D26 — replace the whole cell with:**

> **Refuse at install, in both directions.** Content an app declares required and no theme
> supplies is a refusal, and so, since D32, is a theme whose element table names a class or
> element the installed app cannot paint: an app cannot promise to draw a thing it was given
> no instruction for. The owner learns both on the page that already lists refusal reasons,
> not after an activation that shows nothing. A theme section naming an app that is **not
> installed** is neither — it installs, and the intersection resolves (§23.8, pending R2).

**D16 — replace the cell's third sentence, "The manifest's `period_ms` (§4.1) is the
**fastest** beat the app may ever run; `set_beat(period_ms)` may only slow from there (and
off: `0` = no ticks until the next `resume`), never below it, so the owner sees the real
cadence cost at install and an app cannot grab the shared beat the §6 cadence findings
price.", with:**

> The manifest's `period_ms` (§4.1) becomes the **ask** — what the app wants right now,
> adjustable at runtime through `set_beat` as before — and the manifest additionally carries
> an **explicit set** of acceptable rates (D32, R7). The OS picks the highest rung it can
> achieve that is no faster than the ask, and falls back to the set's floor when the ask is
> not achievable at all, so the owner sees the whole reachable range at install and an app
> can neither grab a rate the set forbids nor invent one. `BEAT_MIN_MS` applies to the floor.

**And D16's carousel paragraph is now wrong, and this is the one substantive correction.**
It reads "the shared beat runs at the fastest active `set_beat` — a 4 fps theme and a 15 fps
theme on the glass at once (carousel transition) cost 15 fps until the swap settles". Under
D32 there is no per-app beat to share: the OS owns one flush cadence, and each stack gets its
own frame *advance* on top of it. The cost the paragraph priced survives in a different form
— the flush still runs at the fastest live requirement, so the §6 cadence-collapse guard
still measures exactly what it was written to measure — but the mechanism it describes is
gone, and the row must say so or the next reader will look for a shared per-app beat that
will never exist.

**§1.2b, and the three in-row pointers.** The seam sentence gains "…the manifest's asset
table is the offer; since D32 the theme's element table is the declaration, and the
compositor reads the declaration." D16, D23 and D26 each gain a trailing clause: *"Amended by
D32 (§23.4): the manifest table is an offer, the theme names what is present, and the beat is
the OS's pick from a declared set."*

### 23.10 The element-table rulings, as given 2026-10-20 — A through H, all closed

D31's seam (`orb_elems.c/h`, 981a8ad) made the resolver real; the eight open questions about
what it answers were put to the owner and all eight came back. Recorded here as the authority
the following commits implement.

- **R-A (kind/src columns): ruled append.** The element row grows `kind` and `src` after
  `scale16` — 19 words, one u32 per column, exactly the append §23.5's header comment
  promises. Both belong to a BANK row; `check_element` refuses a non-zero one on any other
  class, in the same shape as the radius-on-a-text-row rule.
- **R-B (the frame-to-value map): ruled as the source's domain, not a carried figure.** No
  `value_min` column: `theme_schema.CLOCK_WINDOW_DOMAIN` names each fixed calendar source's
  (min, count), a window row driven by one must carry exactly that many plates, and the
  device re-checks the same table at adopt (`orb_clock3.c` mirrors it, suite-pinned). The
  §25.6 refusal to trust a carried minimum stands: a month window is twelve plates because
  a year has twelve months.
- **R-C (carriage and second hand): ruled two objects, both drawn.** The owner's model: the
  tourbillon is a second hand with a per-second rotational delta, a start orientation and a
  pivot; the escapement inside is the frame animation. A theme may carry `hand/2` and
  `hand/5` at once; the carriage's own row geometry (`x/y/pivot/degrees`) places it, and
  its frames animate at the carried `tourbillon_fps`. The capacity refusal stands: more
  frames than `CLK_TOURB_MAX` names a bank the dial holds no room for, and names it in words.
- **R-D (order for complications): ruled a z-bucket, the window's `first` semantics.**
  0–4, lower paints first — the row's `order` lands in the existing bucket, not a free sort.
- **R-E (scope): ruled everything in scope.** The dial draws every class it offers; the
  offered set is the app's manifest, so what a given app cannot draw is its own refusal,
  not a hole in the schema.
- **R-F (the failure strategy): ruled refuse-whole.** Any element that cannot be drawn —
  a missing asset, a refused class, a contradicted table — refuses the whole flip, and the
  app draws its default: for the clock, the digits. A half-drawn theme is wrong pixels,
  which is what D26's direction exists to refuse.
- **R-G (the tourbillon tempo): ruled to keep the wall sampler.** The frame swap rides the
  OS beat the carriage already had; the row's `degrees` is the start orientation. No new
  column, no new timer.
- **R-H (verification): ruled bench-verified.** A host suite proves the logic; the proof of
  the drawing is a boot log on orb-B (heap delta quoted, the §16 rule) and the owner's eye
  on the glass. The bench theme is hand-built with `tools/orbbundle.py` — the seam is not
  allowed to pass only on synthetic goldens.

The one consequence the rulings do not state but force: §23.5's append-with-no-format-bump
promise only holds if the reader learns the row stride from the blob itself, so `blob_check`
(orbbundle.py, orb_bundle.c, the JS baker's reader leg) now derives the element row width
from the blob's own length equality. A stride of 17 — every bundle baked before this commit —
reads with `kind` and `src` as the zeros those columns were written after; a stride below the
dialect's floor refuses as the torn file it is; the GEOM_ABI word stays 3, as §23.5 said it
should, because the length equality is the guard the format word no longer has to be.

### 23.11 Slice 2: the drawing projection — carriage, windows, and the two doors' shared words

The rulings A–H above are now code on every leg. `orb_clock3.c` stops refusing hand slot 5 and
class bank and projects both: a carriage row fans out into the theme's `tourbillon[]` (one
record per plane, the ROW's geometry shared, so any frame rotates about one pivot and dirties
one box), and a BANK row on a fixed calendar domain projects into a `clk_comp_t` whose count
and `value_min` come from the C mirror of `CLOCK_WINDOW_DOMAIN`, never from the row. R-D lands
as written: the row's `order` IS the window's z bucket, refused past the dial's five layers.
R-G keeps the wall sampler; the row's `degrees` rides a new final field on `clk_theme_t`,
`tourbillon_start`, appended last so every positional `.h` initializer ever printed still
lands field for field. `clk_comp_t`/`clk_aux_t` and the `CLK_*` vocabularies moved from
`orb_theme.h` to `clk_types.h` — the projection fills them and must compile on a host.
`main/CMakeLists.txt` registers the seam, `art/orb_elems.c`.

The ruling's runtime consequence: R-C's two drawn objects replaced the tourbillon's
pixel-swap of the second-hand layer. `clock_view.c` gives the bank its own image object
(`s_car`), created under the complications, turned by the seconds angle plus the start
orientation; the chord model keeps its own case (carriage alone) and degrades to the box
union whenever a needle or shadow shares the angle. A FORMAT 2 tourbillon kit would read
differently (its .h's comment said the swap) — no shipped or installed theme carries one:
the av-kit pair is `tourbillon_n 0`, and the kit3 carriage source was never flashed.

The two doors keep their shared sentences and gain one honest split: `--from-bundle` rebuilds
a carriage into the .h it can carry and refuses a non-zero `degrees` and any BANK row in words
that name the PAIR's limits, not the theme's; the device projects both. The suite battery for
it forges the pair bundle plus three carriage planes and twelve month plates and pins the
projected record field for field; the mutation battery grows three guards (slot bound,
capacity, scalar-pair) and the domain mirror is pinned against `theme_schema.py` itself.

Refusals this slice adds at the device door: hand slot 6 and beyond; carriage scalar and row
disagreeing; more frames than `CLK_TOURB_MAX`; a frame window running off the catalogue; kind
other than window ("the element row carries plates and a box"); src the dial cannot drive
(battery, reserve, text, value — R-F refuses the flip rather than ship a comp that draws
nothing); a static window naming more than one plate; a domain count lie; an order past the
buckets; a caller with no comp table; more comps than the table holds. `CLK_FLAG_UNDER_DIAL`
stays 0 by construction: the bank gate costs every plane past the plate at w*h*3, so a FORMAT
3 tile is always the runtime-composited kind. R-B's other edge: `comp_value` now gives
`CLK_SRC_NONE` the value zero — a static window is one plate at frame 0 — where it once refused
the source; `host_comp_math_test.py` pins both halves of that move. The loader prints the
projection it installed ("projected into a dial: N complication rows, tourbillon_n N @ N fps
start N"), the line the bench reads off the boot capture. `tools/bench_d32_bundle.py` builds
R-H's bench theme — av-kit's own art plus an eight-frame openwork carriage at 45 degrees and a
twelve-plate month window at bucket 0 — and the shipped projection parses it field for field on
the host before the radio carries it. `host_clock3_test.py` 239 checks + 3 new mutants,
`bake_clock.py` refusal vectors 18, `host_comp_math_test.py` 5933 vectors, orbelems 560,
orb_bundle parity 333, the schema's own selftest, and the firmware build all green on this
commit. The bench ran R-H on orb-B (`tools/bench_d32.sh`, 2026-10-20): the store door took
the bundle and answered `reboot:none`; the flip landed it — press to reboot boundary 1 s —
and the boot log printed the projection verbatim, `bundle "d32-bench" projected into a dial:
1 complication row, tourbillon_n 8 @ 12 fps start 4500`, over a 304 ms refill of the same
1,961,984 B the lazy arena had just checksummed. PSRAM free moved 7,915,084 → 5,501,436
across the flip: the refill the flip is supposed to make, plus the face's LVGL objects —
the store before it moved nothing. The dial ticked a minimum 6 fps for 30 s on the carriage
face (flush-bound, as the base av-kit face is: the 12 fps frame swaps drop frames, they do
not stall), and `sd/default` put the board back. The month window and the carriage turned on
the glass; `sd/flip` refused a never-stored id in its own words when the first bench draft
skipped the store press, which is the refusal the script now asserts.

## 24 — P6.3 as ruled option A: the refill, the light, the word, the stream — 2026-10-19

The owner's ruling closed §20.4's blocking question before a line of ping-pong was
written. **Option A: keep the default where the flash puts it, defer the ping-pong, and
land the rest of P6.3.** What follows is what that bought, what it cost, and what it left
owed.

### 24.1 Why the ruling was needed: the hole §20.4's own sentence papers over

§20.4 line 2569 asserts "the mmap fallback needs no such protection" and leaves the
mechanism out. `orb_theme.c`'s `use_baked_in()` names it: the card-less default reads
geometry from the compiled `clock_assets.h` and **pixels from partition offset 0**.
`CMakeLists.txt:76-101` flashes `clock_assets.bin` at that same offset 0, and
`SLOT_A_HDR` is 0x0 — offset 0 is inside slot A's region. Making both slots ping-pong
install targets erases the only copy of the factory pixels the board holds, and the
card-less fallback dies in the commit that enables the second install.
`tools/host_tz_test.py:1164` pins D13's sentence and `install_target()`'s hardcode today
precisely because the bench found the first version of this bug for real.

The four homes considered, with numbers measured on 2026-10-19 rather than argued:

| Home | Verdict | The number that decides it |
|---|---|---|
| App image DROM, compressed | Fits; declined | `gorb-os.bin` 1,565,728 B in a 2,097,152 B slot = 531,424 B free. The default bank is 1,173,563 B raw and **250,968 B at DEFLATE-9 (21.4%)**. Price: 280 KB of OTA headroom and a decoder IDF v5.4 does not ship (no `esp_zip`, `zlib` or `miniz` among its 106 components). |
| `assets`, as a permanent region | Does not fit | Default plus two 4 MB slots is 9,175,040 B against an 8,388,608 B partition. Shrinking the slots drags D29's cap through writer, reader, `orbbundle.py` and this ledger. |
| `apps`, as an app container | Refused twice | P4's format rejects art bytes (`orb_app_install.c:291`), and `APPS_SLOT_STRIDE` is 266,240 B — 4.4× too small. |
| The card | Circular | A card-resident default cannot guard a card-less boot. D14 refused card-at-boot; §20.1 measured the card path at 217–307 KB/s against the 6.5 MB/s a frame at tempo needs. |

So the default stays at offset 0, the ping-pong waits for a home for the default that
does not steal a slot, and §20.4's sentence is corrected rather than left standing.

### 24.2 What landed

- **Mechanism B lit.** `art_bank.h`'s `ORB_ART_MECH_B` is 1, and §20.4's rule that the
  define and `tools/host_art_residency_test.py`'s pin move together held: the pin moved
  in this commit, and the mirror gained a REFILL family that could not exist while the
  define was dark.
- **The boot refill, fused.** `art_bank_fill(offset, bytes, verify, expect_crc, &ms)`
  gates on free PSRAM, allocates, and reads, copies and CRCs in one loop. A corrupt slot
  returns `ESP_ERR_INVALID_CRC` with the arena freed, so damage cannot reach the paint
  path. `adopt()` calls it, and `slot_verify_pixels()` left the boot path: the pre-D30
  boot passed over the slot twice, once to prove it and once through the map to draw it.
- **The primary bank has two forms.** `art_bank_ptr()` hands out either base, which is
  §3.3's one-accessor promise extended to the dial's own pixels. An arena primary bills
  the DROM window nothing (`art_pages_accounted()` gates on `s_map`), and
  `orb_art_resident_count()` still counts it. The card-less default calls
  `art_bank_map_at_0()`, which ignores the define on purpose — that is D13 as ruled.
- **The `staged` word.** NVS key `theme_staged`, marked before the first erase and
  cleared when the header lands, on both install paths. Boot clears it the first time it
  finds the header the marker was waiting for, by hashing the committed slot's own name
  back to the id it holds. `/api/v1/sd` answers `staged_id`/`staged_why` beside the
  damage pair, with the refill figures as `refill_ms`/`refill_bytes`. It is a witness and
  never a pending operation — the distinction that lets it exist under D13's own
  reasoning about the key D13 deleted.
- **Streaming FORMAT 3 ingest.** `orb_theme_install_bundle(id, why, whysz)` reads the
  128-byte header, then the region alone (≤60 KB, PSRAM), validates it through
  `orb_bundle_parse_region()`, moves the banks with `orb_bundle_rebase()`, and streams
  each bank chunk-to-partition with its CRC accumulated on the wire and settled against
  `orb_bundle_bank_rule()` before the next section starts. Installed pixels are read back
  and checksummed from the partition, and that figure is the header's. Header last, slot A
  never named, `install_target()` untouched.
- **It stores, and does not choose.** The ingest never calls `orb_theme_set_live()`,
  because no compositor can draw a bundle yet. Boot answers `an app bundle this dial
  cannot draw yet` on the bundle's row instead of calling installed bytes missing (D26).
  `host_sd_test.py`'s `install_lint()` pins both halves.

### 24.3 The byte-layout ruling the ingest had to make

`tools/orbbundle.py` puts the region at file offset 128 and the banks at
`align4096(128 + geom_size)` — a base that moves with the table size, which is why the
writer's "+0x10000 + art_off" line is prose and not a formula. The slot puts both at
fixed offsets, so a record's `art_off` is an address that must be edited rather than a
length that can be re-based at read time. `orb_bundle_rebase()` shifts every record by
one delta, recomputes the region CRC over the edited bytes, and returns the file's old
base so the stream can seek by it. After the rebase the slot is self-describing: every
offset inside it reads slot-relative, which is what §22.1's third rule needs before
`--inspect` can answer with installed bytes.

### 24.4 Verification, and what it did not cover

`idf.py build` clean with no new warnings; `host_art_residency_test.py` green with the
moved pin and 16 of 16 mutations caught. `host_sd_test.py` green with its new lifecycle
pins, `host_tz_test.py` green (D13's table unchanged, as option A requires), and
`host_orb_bundle_c_test.py` green.

Corrected 2026-10-19 after §24.6's work: this paragraph said the P6.3 image was
1,569,936 B, 4,208 B over P6.2. That figure does not reproduce. `build/gorb-os.bin`
measures 1,568,912 B (0x17f090, 25% of the app slot free) on the P6.3 commit AND on this
tree with the two-pass rebase in it, so the restructure costs zero bytes and the earlier
number was taken off some other state of the tree. The claim worth keeping is the one
measured twice, not the one recorded once.

The gap that §24 first named is closed: `orb_bundle_parse_region()`,
`orb_bundle_bank_rule()` and `orb_bundle_rebase()` now have a parity family of their own
in `host_orb_bundle_c_test.py` — cases 70-85, 320 checks, 32 of 32 mutants caught — which
injects a container and asks the shipped reader for the installer's own calls in the
installer's own order. The suite no longer feeds it whole buffers only, so the streaming
door is pinned by shared refusal sentences and not by `install_lint()`'s call order alone.
Writing the family cost a rewrite of `orb_bundle_rebase()` into two passes: its comment
claimed a refused move leaves nothing half-moved, and the merged check-and-write loop did
not deliver that (§24.6).
`orb_theme_install_bundle()` had no route at this writing; P6.4 posted it (§24.8).

### 24.5 Owed, with the bench status that makes each one owed

The bench came up on **board 1 at 192.168.2.34** (`/dev/ttyACM0`, `44:BD:8D:78:26:18`);
board 2 was not reattached. The four `tools/m5_*.sh` runners read `ORB=${ORB:-
http://192.168.2.34}` as of this commit, where they had `192.168.2.32` hardcoded since
§19; the leases are static and the address moved anyway, so the override is the fix and
the new default is this bench.

1. **§20.5(2) refill timing — PAID 2026-10-19, on orb-A with the card in it: 182 ms.**
   `adopt()` printed `theme "av-kit" from slot-B (chosen): 1173563 B refilled to the
   arena in 182 ms`, and its neighbour line carries the same pass's own accounting:
   `1173563 B refilled to the PSRAM arena in 182 ms, checksum verified (7206536 B free
   after, 0 of 234 art pages)`. `/api/v1/sd` mirrors it exactly — `refill_ms: 182`,
   `refill_bytes: 1173563`, `source: "slot-B"`, `active: "av-kit"`. The figure lands
   inside §20.5(2)'s predicted 150–250 ms band rather than at its bottom edge, so the
   prediction is confirmed as a band and not retrofitted to the measurement. The fused
   pass is what the number is for: verify-and-draw in one read of the slot, and `0 of 234
   art pages` says the MMU path stayed untouched to get there.
2. **§20.5(3) power cut mid-flip — RUN 2026-10-19: the word is proven, and the pass
   line's second half is wrong about the design it is testing.** The cut was a real one:
   `/api/v1/reboot`'s 750 ms one-shot armed, and `POST /api/v1/sd/install?id=c38cd33b1c35`
   answered `{"state":"installing"}` 350 ms later, so rst landed inside the copy. The boot
   that followed answered `staged_id: "c38cd33b1c35"`,
   `staged_why: "pixels staged, never committed — the dial shows the previous theme"`,
   `damage_id: ""` — the witness is there, it is readable over the same route as the list,
   and it is not the damage word. **What it does not do is show the previous theme.**
   `source` read `"baked-in"`, `active` read `""`, `refill_ms` read 0, and the boot line
   was `no installed theme — drawing the baked-in bank`. The why's own sentence is
   therefore inaccurate on this firmware, and the reason is D13's single slot, not a bug in
   the staging word: `orb_theme_install()` erases slot-B to write the new commit into it,
   so an interrupted flip destroys the commit it was supposed to fall back to. Half of
   §20.4's amendment is now measured rather than argued — D30's flip is not a ping-pong, and
   the cost is that an interrupted flip is a fall-back-to-default rather than a
   keep-the-old-dial. P6.4's flip page owns that sentence: it must warn the owner that the
   dial can come back showing the factory clock, and `staged_why`'s text should say so.
   Recovery is proven in the same run: a clean re-install took 4406 ms, the automatic reboot
   crossed, and the next boot read `source: "slot-B"`, `active: "av-kit"`,
   `refill_ms: 182`, `staged_id: ""`.
   Scope note: this door is the FORMAT 2 `orb_theme_install()`, because it is the only theme
   install with a route. `orb_theme_install_bundle()` marks the same word at its own
   `staged_mark()` and remains uncut-able until P6.4 posts it, so the bundle half of this
   item stays owed. The FORMAT 2 dialect itself installed and drew fine on the P6.3 build —
   the strict bundle gate is at the bundle door, and P6.5 is still what retires the old one.
3. **§20.5(4) arena pin and refusal — the pin half needs board 2.** The refusal arm is
   host-covered (REFILL at bank+margin−1). Measured here against the live figure: av-kit
   at 1,173,563 B leaves 6,324,893 B free after the copy and the 2 MB margin.
4. **The ping-pong, and with it the amendment §20.4 asked for.** D13 stands as written.
   D30's flip is single-slot-B until the default has a home that does not consume one of
   the two slots, and §24.1's table says that home is not `assets`, not `apps`, and not
   the card. If it ever is the app image, the compressed bake is the shape and the OTA
   headroom is the price.
5. **The flashing that gates all four — PAID 2026-10-19, and it moved the blocker
   rather than clearing it.** The owner's call came: the attached board is the bench
   board, so `.orb-target` re-binds to it. The clone is now `orb-B`
   (`44:BD:8D:78:26:18`, `/dev/serial/by-id/…78:26:18-if00`, `192.168.2.34`); orb-A's old
   binding is saved at `/tmp/orb-target.orb-A.bak` for whoever drives that board. The
   gates ran in order and said so: `orb-B confirmed as orb-B (MAC 44:bd:8d:78:26:18)`,
   then `AUDIT OK`, after `--check` had first refused the stale image (`have=
   v1.1.0+bbaa036 want=v1.1.0+9e1fc27`) exactly as §18's note about configure-time stamps
   predicts. The image the audit accepted is 1,568,912 B, so §24.4's corrected figure is
   now the stamp-audited number and not merely a `build/` listing. `EXIT=0` at 16:50:07,
   and the boot log on the new firmware says `no installed theme — drawing the baked-in
   bank`, `art bank mapped: 1173563 B … from baked-in`, `art_refusals: 0`,
   `heap_psram 7,498,456`. Item 3's live half is paid by that pair: 1,173,563 resident
   inside 7,498,456 free is the sum §24.5(3) quoted.
   **Amended the same afternoon, twice.** First correction: this paragraph said the gate
   that remained was a card, not a MAC, and that held for about an hour. The owner attached
   the second board — Windows busid 1-8, COM4, `usbipd attach --wsl` — and it resolved to
   the bound MAC `44:BD:8D:60:80:88`: that is orb-A, and the SD card was already in its
   socket. The clone is bound to orb-A again (`/dev/ttyACM1`), and orb-A carries the audited
   image as `v1.1.0+3bb3799`. orb-B's stamp is `v1.1.0+9e1fc27`. The two app images are
   byte-identical — every commit between those stamps is docs-only — but the boards do not
   carry the same stamp, and a bench note must say which board said what. Second
   correction: orb-B's socket is no longer empty either. It now mounts the same SD32G part
   blank (`card_count: 0`, 29,807 of 29,808 MB free, rescan answers at the full 20 MHz), so
   the bench has two card sockets, and orb-B is the writable one for a bundle door that has
   no door yet.

### 24.6 The streaming door, under test — 2026-10-19

The door landed in §24.2 with no vector of its own. It has one now, and the first thing
the vectors found was a comment that overstated its function.

**What the family is.** `host_orb_bundle_c_test.py` gained `F_STREAM` and a probe
language, so a vector names the installer's calls in the installer's order: `region`
(the geometry region on its own, with the container's real length), `rebase[:<base>:<cap>]`
(the move to Slot B, with the capacity spelled when a vector is about the boundary),
`secdump` (what the records hold now), `recheck` (the edited buffer, through the door a
second time), and `bank:<n>:<mode>` (one bank across a wire, chunked, with the running
count and crc settled by `orb_bundle_bank_rule()`). Each mode breaks exactly one thing the
wire can get wrong: `FULL` honest, `SHORT` one byte fewer, `BAD` a corrupted running crc,
`LYE` a caller reporting a corrupt crc at a short count, `LYEC` the dangerous one — a
caller reporting the record's OWN crc over one byte short of it, which is what the count
clause in `bank_rule` exists for and nothing else watches. Expectations come from
`orbbundle.py`'s own parse of the same bytes, never from the C's output: a shared refusal
sentence is the claim, so the two sides may not grade each other.

**The two-pass fix.** `orb_bundle_rebase()` had one loop that bounds-checked a section and
then wrote its record. A bundle whose third section does not fit therefore left its first
two moved and its header untouched — the half-state the header-last rule exists to make
impossible, in the one buffer the installer still holds when it prints the refusal. The
function now checks every section, then writes every section, then recomputes the region
crc. Cases 84 and 85 pin it with a `secdump` after a refused move: the offsets on the
far side of a refusal are the offsets the container was baked with, unchanged.

**What the vectors say.** The claim is one rule and every case tests a corner of it:
everything the whole-buffer reader checks before a bank's bytes says the same sentence
through the region door, and the bank check itself moves onto the wire.

| Case | Bytes | The claim |
|---|---|---|
| 70-71 | the golden bundle; a single-section one | the region door accepts what the writer's reader accepts, and says what it says |
| 72 | clean, probes out of order | a bank settled with no region parsed is refused as out of order — `copy_bundle3()`'s sequence is law and not habit |
| 73-76 | clean, five wire faults | one broken parameter refuses a bank, in the writer's own sentence |
| 77-78 | one bank byte flipped (single-, then two-section) | the pairing: the region door ACCEPTS these bytes, the wire refuses, and 78 shows the first bank close clean before the second is refused |
| 79-81 | region crc, a lying art offset, a short file | checks that need no bank bytes still bite, in the same order, in the same words |
| 82 | the device's own move, `0x10000` | the rebased offsets, the recomputed region crc, the recheck that accepts, and a slot image the writer's own reader reads back |
| 83-84 | the cap at the byte and one under | the boundary is `art_off + art_size <= cap`, in both directions |
| 85 | a move that does not fit a two-section bundle | a refusal moves no record and re-checksums nothing |

**What it cost to get there.** The suite went from 167 checks and 24 mutants to 320 and 32.
Of the eight new mutants, six break a streaming guard alone — the forced `geom_off`, the
`banks_present` skip, the count clause, the whole bank rule, the slot bound, the region
crc — and two break the order of the rebase's work (the wrong record edited; the bank moved
before its bounds are known). The last one is the mutant the single-pass loop could not be
caught by, which is the plainest way to say why the loop changed.

**Still owed.** Real traffic on a board. `install_bundle` has no route until P6.4 posts
it, so the door has never run on flash: the 60 KB PSRAM region buffer's failure path, the
chunk loop against a real card's read speed, and the `staged` witness that a cut power is
supposed to leave behind are all §24.5's, unchanged.

### 24.7 The bundle door's refusal, on hardware — orb-B, 2026-10-19

The bench work §24.5(5) unblocked turned out to be smaller than the paragraph above
promised, and the ledger needs that said plainly. Three things I had offered as available
are not:

- **No route posts `orb_theme_install_bundle()`.** Grep is unambiguous: the function has no
  caller outside its own definition, and `orb_bundle3_hdr_parse()` has exactly four call
  sites, all inside `orb_theme.c` — `hdr_parse()` (the upload door's refusal, measured
  below), `card_check3()` and the serial read inside `orb_theme_install_bundle()` itself,
  and `orb_theme_boot()`'s "installed but not drawable" detection. The streaming ingest
  cannot run on flash until P6.4, card or no card.
- **No FORMAT 3 fixture exists, and inventing one is P6.5's job.** `write_bundle()` has no
  production caller — `bake_clock.py` calls `orbtheme.write_container()`, and the only
  Python that calls `write_bundle()` is the two host suites and `mut_bundle.py`. av-kit's
  facts are all present in the FORMAT 2 blob (3 hands with pivots and radii, 5 tourbillon
  entries, 15 bg frames, 2 complications, 185 bank rects, the sweep scalars), but the class
  assignment, the bank-span arithmetic and the tourbillon fan-out are the reverse path's
  design decisions, ruled as **P6.5**. A hand-written fixture would be an undocumented
  convention inside a ruled slice, so the right output here is the open decision, not a file.
- **No MSC path.** `docs/usb-sd-msc-plan.md` is a plan and `main/` has one comment
  mentioning it, so the card is not reachable from a host except through the upload door —
  which refuses FORMAT 3. The two blockers are the same door, which is why item 1 below
  replaces the streaming run rather than preceding it.

**What the door does answer, measured.** Four files at `POST /api/v1/sd/theme` on orb-B
(`v1.1.0+9e1fc27`, blank card), each refusal read twice for reproducibility:

| File | Body | HTTP | The sentence |
|---|---|---|---|
| `orbbundle.write_bundle()` output, untouched | 8,192 B | 422 | "that is a FORMAT 3 app bundle; this firmware installs single-app themes only — update the dial to load it" |
| same, one byte torn inside the geometry region | 8,192 B | 422 | the same named sentence — the header is what the door reads, so region damage is invisible to it |
| same, one byte torn inside the 128-byte header | 8,192 B | 422 | "theme format this firmware cannot read" |
| `orbtheme.write_container()` output, a real FORMAT 2 file | 1,177,659 B | 200 | stored — the dialect is not retired until P6.5 |
| `assets/clock_assets.bin`, a headerless bank blob | 1,173,563 B | 422 | "not an .orbtheme file" |

Three things worth keeping from that table. The card is **unchanged after every refusal**
(`card_count`, `free_mb`, `uploading` identical before and after; the scratch file is
deleted). The refusal is honest in D26's sense — the owner's next action is in the sentence,
in all three arms. And a refusal costs **the transfer and nothing else**: the door's own read
is 128 bytes, so the 8 KB probes came back in 0.09–0.22 s, and the 1,173,563 B headerless
bank took 8.5 s — the whole radio time, and not one byte of install work. httpd has received
the body before the door looks at it, so a page that could know the format first should say
so; that is P6.4's question in the open decision below.

**A comment that said the opposite, now corrected.** `orb_theme.c`'s gate carried "a damaged
one must hear the bundle reader's own reason, not the generic line". Row 3 above is the
refutation: `orb_bundle3_hdr_parse()`'s reason is computed and discarded, so a torn bundle
header gets the generic sentence. This is the second comment in three days that overstated
what its code does (§24.6's was `orb_bundle_rebase()`'s). The comment now states the measured
behavior and the reason it is the safe direction: the named sentence promises "update the
dial to load it", and that promise must not be made about bytes whose checksum failed. **No
code changed.** The rebuild is the same 1,568,912 B, and the only differing bytes are the two
places a rebuild always moves — the descriptor's build timestamp (`0xb0`–`0xcf`) and the
trailing image hash (`0x17f06f`–`0x17f08f`), 65 bytes in all. My first draft of this paragraph
said "byte-identical", which the `cmp` refuted; the claim that survives is the one measured.

**The open decision this leaves, for P6.4 to answer.** §24.6's claim is that the install door
refuses a bad bundle *before erasing slot B* — the `try_id`/`try_why` path, D11's way of
keeping a refusal honest — and that claim still has no vector on either side of the wire,
because there is no door to post it through and no FORMAT 3 file to send once there is. It
splits into two, and both belong to whoever builds P6.4's page: does the page read the format
word before uploading, and does the upload door's refusal path leave an install-shaped `try`
witness at all? The second half is already answerable on the host (the refusal path never
calls `try_note()` — that lives on the install side), and if a page relies on the sentence
appearing on the row it presses, that is wrong today. P6.4 must decide which door owns the
format check, not inherit this one by silence.

**Bench state after this run, so the next entry starts from facts.** orb-A: bound, flashed,
`v1.1.0+3bb3799`, card mounted with FORMAT 2 av-kit and av-kit-b chosen (`source: "slot-B"`,
2,976 pages). orb-B: `/dev/ttyACM0`, `v1.1.0+9e1fc27`, card mounted with one FORMAT 2 theme
(av-kit, stored by this run's accepted upload; **nothing chosen** — `live: default`, slot B
unwritten, so it stays the clean board for a first real flip). RSSI −78, and its WiFi OTA
reaches `final 100%` with no reboot while orb-A's reboots; orb-B is on the older stamp, which
is the one difference between them that could explain it. Note the asymmetry before anyone
blames the radio: the OTA that would fix orb-B's stamp cannot be trusted on orb-B, and the
board the radio can be trusted on already has the stamp. Serial is the way.

**Still owed by §24.5, restated.** Item 3's pin half is owed because it needs board 2 on
*internal* heap with no arena — orb-B now has a card, so it is no longer that board, and
neither attached board is uncarded any more. That leaves: the streaming ingest's real
traffic (P6.4), the cut through `install_bundle()` (P6.4), and the fixtures (P6.5).
Nothing left in §24.5 is bench-available. (P6.4 landed the same day, §24.8: the route and
the door exist now; the traffic and the cut became flash-available, not code-blocked.)

### 24.8 P6.4 lands: the flip door, its worker, and the sentence §24.5(2) owed — 2026-10-19

The open decision §24.7 left — which door owns the FORMAT 3 check — is answered in code,
not in prose: **the device's door owns it, and the page reads no container bytes.** The
rule lands on R4's own technique. `hdr_parse()`'s FORMAT 3 refusal is now one named
constant, `THEME3_REFUSAL`, and the doors that can store a bundle compare it by POINTER.
A caller that cannot install bundles (the picker, the boot slot reader, the FORMAT 2
install door) receives the sentence as text and behaves as it did. A caller that can —
the scan's list pass, the upload door — sees the sentinel pointer and dispatches to
`card_check3()`, the install door's own rule, so the same container meets the same
sentences through either door and no rule is written twice.

**What changed, per surface:**

- `GET /api/v1/sd` rows carry `fmt` (2/3, 0 unreadable) and `sects`, and the base carries
  `bundle_ms`/`bundle_bytes` — the last streaming store's figures, timed like `refill_ms`,
  so M6(1)'s copy rate is reachable without a serial cable. That closes the lesson
  §24.6(5) learned about what gates a bench. The handler's bound comment is recomputed:
  ~362 B a row, ~940 B base, worst case ~5.6 kB inside the 6,144 B static.
- `POST /api/v1/sd/theme` stores a well-formed FORMAT 3 bundle. Before the scratch is
  filed under the hash of its header name: the header through `card_check3()`, the
  region through the shipped `orb_bundle_parse_region()`, and the art streamed and
  checksummed against the header's figure — `verify_bundle3()`, the twin shape of
  `verify_container()`, `ensure_chunk()` reserved inside it (the measured bench bug the
  P6.3 comment records applies to any bare-open reader, FORMAT 3 included). The
  duplicate check compares the identity tuple with `fmt` included: a FORMAT 2 and a
  FORMAT 3 under one name are different themes, refused without `?overwrite`. Per-bank
  rules stay the install's debt: the door checks the region and the art sum, the same
  split `read_bundle()` writes against.
- A torn FORMAT 3 header keeps the generic line, exactly as §24.7 measured. The reader's
  reason propagates only after its self-checksum has settled, so the format word a
  refusal names is always a verified word. The new `host_sd_test.py` rows pin that order
  from both directions — §24.7's table row 3 is a pinned vector now, not prose.
- `POST /api/v1/sd/bundle?id=X` calls `orb_theme_post_bundle()`: the same queue as the
  FORMAT 2 install, the same `try_note` before the queue and on the refusal, and no
  reboot — a stored bundle changes nothing the dial draws, so rebooting would show the
  owner the same clock and bill them a boot. The response says `"reboot":"none"`. This is
  the first live caller of `orb_theme_install_bundle()`, five commits after it shipped.
- `scan_entry()` lists a verified bundle `ok:true` with its header name and hashed id,
  never `active`: nothing draws a bundle until P6.5's reader. The page marks such a row
  `app bundle of N apps`. A misnamed bundle keeps the filename stem as its address and is
  refused, the same rule FORMAT 2 rows follow.
- `orb_theme_delete()` falls through to `card_open_verified_id3()`: what can be listed
  and installed must be deletable from the page, or the owner needs a card reader — and
  this host has none (`lsblk`: four devices, none an SD reader; the card is device-only
  media, which is also why the upload door had to open for the bench to run at all).
- The `/themes` page gained the Store button and §24.5(2)'s owed sentence on both confirm
  doors, install and store alike — a power cut during the copy brings back the factory
  clock, and re-doing it recovers. `STAGED_WHY` itself changed from *shows the previous
  theme* to *shows the factory clock*: §24.5(2)'s cut measured the boot that followed, and
  the old text described a fall-back the single-slot design does not have. install_lint
  pins the new substring so it cannot rot back.
- `slot_read()` gained the explicit belt `hdr_parse(...) || s->h.fmt != THEME_FORMAT`:
  boot's bundle-detection branch depends on `b.valid` staying false for a FORMAT 3 slot,
  and that one read decides it. A mutant deleting the guard reddens the lint.

**The suite paid for the stub's retirement on its own terms.** `BUNDLE_STUB`'s comment
said to replace it if any lifted caller ever fed the reader FORMAT 3 headers; that caller
now exists, so the harness compiles the REAL `orb_bundle3_hdr_parse()` lifted from
`orb_bundle.c` (with `b32` and `reject` and the real `orb_bundle3_hdr_t`). The word-index
enum is not duplicated — `orb_bundle.c` declares the identical anonymous enum, and C
forbids one enumerator twice in a TU — so `caps_lint` compares the two enum blocks as
text every run: drift would silently read FORMAT from the wrong word and green through a
dead gate, and it cannot any more. New rows: the writer's own header returns the sentinel
and `r3ptr` pins the POINTER itself (a copied literal reddens it); torn-header, format-4,
wrong-abi and over-budget headers all answer the generic line at the FORMAT 2 gate while
`r3why` rows pin the reader's own refusals directly — the two-sided pin §24.7 asked for;
`r3fields` pins every shared tuple field against the values `pack_header3()` wrote,
because the duplicate check compares exactly those words. Suite: **157 ok rows, 0
failures; 12 new mutants, 76 of 76 caught.** Two of the new mutants survived their first
draft — bare words survive in comments and argument lists when the wiring is deleted —
and the checks now pin the route-table line and the JSON format string verbatim, so both
bite. The lift fix that took two tries is recorded here because both failures were mine:
a `+ ";"` on a typedef whose regex already ends in one, and a multi-line comment cut
mid-stream by a line-based lift regex, each landing as a compiler wall rather than a
silent hole, which is the point of lifting loudly.

**`tools/mk_bundle_fix.py` — a bench driver, labelled as one.** §24.7's blocker "no
FORMAT 3 fixture exists, and inventing one is P6.5's job" stays true for the PRODUCT
re-bake; the bench no longer waits on it. The tool turns the real FORMAT 2 av-kit into a
FORMAT 3 bundle whose art bank is the source's art bytes verbatim — measured run:
1,173,563 B source art, 1,173,755 B of bundle art across two sections, 1,181,755 B file —
so M6(1) and the M6(3) bundle cut move av-kit's own pixels at av-kit's size. The element
tables are synthetic (a plate, a hand, one tiny sprite); the class assignment and
bank-span arithmetic remain P6.5's ruled decisions, and the docstring says so where a
future reader who trusts the file as a product bake will find it. `--probe` regenerates
the §24.7 five-file sentence set, retargeted at the door that now opens: `valid` is
stored where it used to refuse; `region` is now REFUSED where §24.7 row 2 saw it through
("the door's own read is 128 bytes, so region damage is invisible to it" stopped being
true the day the door verifies the region — the answer is `geometry checksum mismatch —
the copy on the card is damaged`, the reader's own words, before the rename); `header`
still answers the generic line, and `format2` and `blob` are unchanged.

**Not on hardware yet — said plainly, as §24.7 insisted it be.** Both attached boards run
pre-P6.4 firmware (.34's `/api/v1/sd` carries no `fmt` field as of this writing), so the
route, the door, the rows and the timing pair are host-verified and build-clean only. The
M6 bundle-half measurements ride the next flash: swap `.orb-target` to the board that
answers serial (orb-B's real MAC-verified values sit in `/tmp/orb-target.orb-B.bak`; the
audit stays honest — no forged binding, no unswitched write), `tools/serflash.sh`, then
upload the fixture, read the bundle row's `fmt`/`sects`, press store, read
`bundle_ms`/`bundle_bytes` off `/api/v1/sd` over the radio, run §24.5(3) through the new
door with the armed-reboot idiom, confirm the boot's damage pair answers `an app bundle
this dial cannot draw yet`, and re-install FORMAT 2 av-kit to prove the dialects coexist.
Restore orb-A's binding and re-flash board 1 afterwards, or §24.6's `A061615644e5` stamp
claim goes stale without saying so. The page's upload label and the door's FORMAT 3
sentences are worth a second hardware table with this build's stamp; §24.7's table stays
the record of the build it measured, and row 1's answer changed on purpose with this
commit — the refusal that table pinned was true of the firmware that gave it.

### 24.9 The flip door benched on orb-A: the rate, the cut, and the words the doors speak — 2026-10-19

§24.8 closed saying the bench rode the next flash. The flash happened the same day,
twice: the first pass of the battery caught a real bug (`26fc3c2`'s message carries it —
`verify_bundle3` hashed the art region including the writer's inter-bank sector pads,
so a two-section bundle could never upload), and every number below is from the audited
clean build `v1.1.0+26fc3c2` on orb-A (`.32`, MAC 44:BD:8D:60:80:88, RSSI −52, the card
as §24.7 left it — one FORMAT 2 av-kit, which is also the correction to §24.7's bench
note: `av-kit-b` is not on this card and was not seen by any probe here).

**The upload door, §24.7's battery redone on the firmware that opens it:**

| File | Body | HTTP | The sentence |
|---|---|---|---|
| `b3_blob.orbtheme`, headerless | 8,192 B | 422 | "not an .orbtheme file" |
| `b3_header.orbtheme`, FORMAT 3 header torn | 16,384 B | 422 | "theme format this firmware cannot read" |
| `b3_region.orbtheme`, region torn past the header | 16,384 B | 422 | "geometry checksum mismatch — the copy on the card is damaged" |
| `b3_valid.orbtheme`, two real sections | 16,384 B | 200 | stored |
| `b3_valid` again, byte-identical | — | 200 | stored quietly (the §6 tuple, `fmt` included, held) |
| `b3_format2.orbtheme` (the real av-kit) | 1,177,659 B | 200 | stored — the dialect coexists |
| `m5_avkit.b3.orbtheme`, the real pixels as FORMAT 3 | 1,181,755 B | 200 | stored in 10.8 s of radio |

Row 3 is §24.7 row 2's premise dying: the door used to be blind past its 128 bytes, and
region damage is now named at the door in the reader's own words, before the rename.
Row 1's old answer is row 1's history. The scan then listed both bundles
(`13620ad69497` FORMAT 3, 2 sections, 1,177,659 B — the name `av-kit (bench)` hashing to
its id like every R4 file).

**M6(1)'s streaming-door figure, over HTTP with no serial cable:** `POST
/api/v1/sd/bundle?id=13620ad69497` → `{"state":"storing","reboot":"none"}` → 6 s later
`/api/v1/sd` carried `bundle_ms: 4398`, `bundle_bytes: 1177659`. Against §19.2's FORMAT 2
comparison (4406 ms installing the same 1,177,659 B container) the fused streaming pass
is **the same cost to the millisecond — 4398 vs 4406 — for a bundle carrying a second
section, a rebase, and the wire verification the FORMAT 2 door does not do**. The second
store of the same bundle measured 3099 ms; both figures are recorded because the
difference is cache warmth on a 29 GB card, not a rate, and the ledger quotes the first.

**The words the doors speak — the reason `26fc3c2` exists.** Pressed at the wrong door,
each dialect now names itself and the right button, through the walk resolver's carried
reason rather than R4's identity line (which is a lie about a file whose name matches
its id perfectly): the FORMAT 2 door on the bundle answered
`that is a FORMAT 3 app bundle — press Store, not Load` (53 bytes — sized to survive the
try witness's 56-byte buffer, `_Static_assert`-pinned), and the bundle door on av-kit
answered `theme format this firmware cannot read` — the reader's own format line, which
propagates only because `resolve3_entry` now carries it. The first cross-probe run
showed one stale witness pair (a 2 s read racing the worker, and a refusal inherited
from a file deleted under an old build); the clean re-press at 4 s is what the table
above records. Nothing invisible was allowed to stand as a pass.

**M6(3)'s bundle half — the cut, through the streaming door.** Armed with the same
idiom §24.5(2) used (`/api/v1/reboot`'s 750 ms one-shot, store posted 350 ms after the
arm): `staged_id: 13620ad69497`, `staged_why: "pixels staged, never committed — the dial
shows the factory clock"` (the corrected text, on hardware, in the boot that followed a
cut this firmware caused), `damage` empty, `source: baked-in`, `refill_ms: 0`, and the
dial alive. The slot held av-kit's commit before the press and the copy erases the
header sector first — so the cut destroyed the FORMAT 2 install exactly as §24.5(2)
proved for the FORMAT 2 door, and the new sentence is the honest one that says so.
Recovery, same run: a clean re-store landed (`bundle_ms: 3099`, staged cleared by the
install itself), and re-installing av-kit through the FORMAT 2 door returned the dial to
`source: slot-B, active: av-kit, refill_ms: 182` — the §24.5(1) figure to the millisecond
across the whole session's abuse.

**The boot promise §20.6 wrote, observed the first time it could be.** Before the cut
even ran, the re-flash's own reboot put the earlier small-bundle store in the slot and
the next boot answered the pair `damage_id: d3910a0e16eb` / `an app bundle this dial
cannot draw yet` — installed bytes named as installed bytes, the dial on the factory
clock, the choice key left written so P6.5's reader can honour it. That branch had
never run on any board; it ran twice here and cleared both times.

**Bench state after this session, and the owed list it settles.** orb-A runs
`v1.1.0+26fc3c2` — the code on the silicon is HEAD's code, and the tree has moved past
that hash by docs-only commits; the re-flash of this tree's image re-syncs the label,
and whatever hash the board then reports over `/api/v1/status`, the code under it is
`26fc3c2`'s. The trap hit for real and is worth naming precisely, because I first
mis-named it: `idf.py reconfigure` DOES re-bake the app descriptor for a docs-only
commit — what bit me was ORDER. A build whose configure ran before the commit carries
`-dirty` or the previous hash, `touch CMakeLists` and deleting the cached descriptor
object do not help (the version rides `compile_commands.json` definitions), and the
audit refuses every stale variant exactly as serflash.sh's header promises. The rule
the flasher enforces: commit FIRST, reconfigure SECOND, flash THIRD.
`serflash.sh --check` between two and three costs seconds and catches all of it.
§24.6's `A061615644e5` claim is superseded by this sentence. The card carries av-kit
(installed, chosen) plus the two bench bundles, deletable from the page. orb-B was not
touched (§24.7's state stands). Of §24.5: item 1 stands paid; item 2 is now paid **on
both doors**; item 3's pin half stays owed (no uncarded board exists); item 4
(ping-pong) unchanged. What M6 still owes is the two halves that are not the door's:
the progress-bar-on-glass flip (behind the default's new home and P6.5) and the
cap-size refill (P6.5's reader, or the bundle path once the swap exists). The streaming
door's M6 half is closed.




## 25 — P6.5a: the re-bake mapping ruled, and `rebake_bundle.py` — 2026-10-19

### 25.1 What this half decides

P6.5's pass line (§22.7 step 5) is av-kit re-baked to a FORMAT 3 bundle, the reverse
path proven, and the FORMAT 2 dialect retired. The blocking decision was inherited the
moment R1 made the theme's content an element table: every §19/§24 FORMAT 2 fixture and
every shipped `.orbtheme` speaks the geometry-blob dialect, and nothing could produce a
REAL bundle until someone ruled the mapping. This section rules it and ships the tool
that applies it — `tools/rebake_bundle.py` — the first product FORMAT 3 bundle:
av-kit's own art, verbatim, under a legible 8-row clock table. §24.8's owed product
re-bake is paid; `mk_bundle_fix.py`'s synthetic element rows retire with the FORMAT 2
suite once the reader lands.

### 25.2 The mapping, as ruled

Source: a FORMAT 2 container read by `orbtheme.read_container` (both checksums, or the
tool refuses at the door). The clock section it emits:

- **scalars** — all 26 clock-dialect fields ride verbatim (D31: the app's own dialect,
  opaque to every other app). `scr_w`/`scr_h` move into the index record, §22.2's
  "the scalars MOVE into the section" honored for the one shipped file that has them.
- **plate** (scalar `plate_off`, `scr_w×scr_h×2`, colour-only) → class `plate`, slot 0,
  `order 0`, frame 0.
- **hands 0..4** (`show`-filtered) → class `hand`, slot = the FORMAT 2 array index —
  the offer's own vocabulary, so (class, slot) identity needs no new namespace.
  `order = 2·order_N + 1`, `x, y = offx, offy` (the drawn top-left),
  `pivot_x, pivot_y = centerx − offx, centery − offy`. The rule that makes six columns
  out of FORMAT 2's placement five: **element pivot is the rotation center relative to
  the draw point**, and every shipped sprite satisfies
  `draw point = center − crop pivot`, so `center` and `pivot` both rebuild and the
  `.h`'s `pivotx/pivoty` (crop-relative) is recoverable as `center − off`. The hand's
  own crop pivot and the shadow's (which differ by the light offset the source baked)
  rebuild from the same pair — no column invented, nothing lost for the kits that exist.
- **shadows 0..4** → class `sprite`, slot = index, `order = 2·order_N`, same placement
  rule — each shadow lands exactly one rung under its hand.
- **overlay** → class `plate`, slot 1 (the crystal is a second plate),
  `order 63` — the table states what `clock_view.c` already draws: last, over all.
- **show=0 hands carry NO row** — the offer's slot exists and the theme says nothing
  draws there; D31's default is the honest answer. An `EF_HIDDEN` row would claim a
  bank range the art does not hold, which `check_element` rightly refuses. The flag
  belongs to a theme that HIDES an element the bank carries, not to one that never
  had it.

The sprite catalogue (the blob's `sprites`) carries the planes in byte order —
`plate, overlay, hand0, shadow0, hand1, shadow1, hand2, shadow2` for av-kit — and
every row's `frame0` indexes that list, which is FORMAT 3's frame window doing its
named job.

### 25.3 What refuses, and why each refusal is the honest one

The tool exits with the sentence, never a partial table:

- a **tourbillon** the source shows: the fan-out mapping is not ruled, and av-kit
  carries none — guessing what a fan would draw is exactly the wrongly-painted
  element D26's direction refuses.
- a **gif bank with real delta rects** (`rects[i].off` nonzero, or a plate-frame
  table): the extra frames ride rect planes whose byte sizes this cut cannot prove
  contiguous. A source that merely NAMES gif frames (av-kit: `gif_frames: 15` under
  `--no-bg`) is not a bank — the bank itself is the guard, and the scalars ride as
  the source truth they are.
- **complication/aux/bank tables**: the text and bank rows ride the complication
  mapping, which lands with the kit that carries them.
- **any plane the catalogue cannot account for byte-for-byte**: `sum(plane sizes)
  == art_size` and contiguity from offset 0 are the mapping's own checksum — a bank
  this catalogue does not explain whole is a bank this mapping does not understand.

### 25.4 Verification

`rebake_bundle.py` verifies its own output on every run (the mapping's contract is
round-trip, and a tool that only writes cannot tell you it lost something):
`read_bundle` the file, assert the art byte-identical, the catalogue against the byte
walk, every element field — including each row's `frame0` seat in the byte-ordered
rect list, both pivot identities, and the crop-pivot-equals-center-minus-off claim
itself — against the source geometry, all 26 scalars verbatim, the name identity, and
the screen pair in the record. av-kit's re-bake: **0 failures, 1,177,659 B, 8 elements,
art 1,173,563 B identical**. The shipped C reader — `main/art/orb_bundle.c`, compiled
by the P6.1 harness's own `compile_reader` — parsed the same bytes and dumped
`ok sec 0 clock … 8 8 4096 1173563 466 466`: the device's reader, not the writer's
mirror, accepted the product bundle. `tools/mut_rebake.py` runs 17 mutants — 13
mapping breaks (swapped placement, crop-pivot-as-pivot, lost rung, overlay demoted,
class/slot/frame0 broken, wrong plane sizes, unsorted planes, dropped scalars, screen
pair and name identity lost) and 4 §25.3 guards tripped by forged-geometry drivers
(tourbillon, real delta rects, comp table, moved plate): **17/17 caught**. The harness
earned its keep writing itself: the first draft's verify lacked frame0 checks, the
name check compared against its own mutated value, and the refusal-exit mapping read
inverted — every one a mutant exposed. Suites re-run green unchanged: format3 52/0,
C parity 320/0, element suite 46/0. Firmware untouched.

### 25.5 What the rest of P6.5 owes

1. **PAID 2026-10-19** — the bundle→pair half. `bake_clock.py --from-bundle`; the
   proof is §25.6.
2. **PAID 2026-10-19** — the device reader: `main/art/orb_clock3.{h,c}`, the boot adopt
   that draws it, and the `active` answer for bundle rows. §25.7.
3. **PAID 2026-10-20** — the retirement: FORMAT 2's dialect out of the firmware's reader.
   `orb_theme.c`'s FORMAT 2 reader island is deleted (`hdr_parse`, `card_check`,
   `geom_validate_blob`, `slot_read`, the install/upload FORMAT 2 branch); the door is
   `card_check3`/`card_open_verified_id3`/`verify_bundle3`, and `orb_theme_install` /
   `orb_theme_post_install` / `POST /api/v1/sd/install` are gone. The FORMAT 3-only picker
   offers Store, never Load. `host_sd_test.py` retargets to the surviving door and pins the
   FORMAT 2 refusal at the format gate with a real-av-kit-derived vector (the P6 pass line:
   a FORMAT 2 file refused, no fallback reader); `mk_bundle_fix.py`'s FORMAT 2 arm is now the
   door's refusal probe, not a store. The compiled-in FORMAT 2 default pair stays the
   card-less fallback (D21, §26 Option B) — the dialect is out of the *reader and writer*,
   the fallback pixels stay in flash. Firmware builds clean, all eight host suites green,
   every mutation battery caught with zero survivors. Commit `7c0fd4b`.
4. **PAID 2026-10-19** — `tools/mut_rebake.py`, 17/17 (see §25.4).

### 25.7 §25.5(2) PAID 2026-10-19 — the FORMAT 3 device reader

The M6 root blocker is gone: a bundle in a slot is now a picture, not a sentence about
a picture. `main/art/orb_clock3.c` is the projection, `orb_theme.c`'s `adopt_bundle3()`
is the door, and `tools/host_clock3_test.py` is the proof. §25.2 and §25.6 own the
mapping; what follows is only the reader's side of it.

**A new TU, on purpose.** `orb_bundle.c` is the format mirror of `orbbundle.py` and
carries no dialect knowledge. The clock's reading of a table *is* dialect, so it lives
beside it, and it stays pure the way `orb_theme_geom_parse` is pure: no allocator, no
card, no partition, caller-owned tables with caller-declared caps. That is also what
lets the suite compile the shipped file with host gcc — the same reason §22.6's harness
exists.

**Four rulings, none of which invents a column.**

- **Placement is §25.2's identity run backwards.** `centre = (x + pivot_x, y + pivot_y)`;
  for a hand and the overlay the crop pivot *is* `(pivot_x, pivot_y)`. A shadow's crop
  pivot is its **hand's centre minus its own draw point** — the light offset, recoverable
  and never carried — so a shadow row with no hand at its slot refuses rather than draws
  with the wrong pivot. `show = 0` when `EF_HIDDEN`; `blend = 0`, the dialect carrying no
  blend column.
- **An absent row is a blank layer at the plate's centre, `show = 0`** — the record a kit
  bake writes for an unused slot, so the pair and the picture stay one definition rather
  than two. The tourbillon array is all-blank because a non-zero `tourbillon_n` names a
  bank the art cannot hold: a contradiction between the scalars and the table.
- **The bank is proved before any offset is trusted**: contiguous from 0, each plane's
  size equal to its own extent (colour-only for the plate row, colour + alpha for the
  rest), the sum equal to the record's `art_size`. §25.3's last bullet from the device
  end. Every row's `frame0` must seat in the catalogue and match it on size, because a
  table and a catalogue describing different layers draw a torn sprite.
- **The blob base is the section's, not the region's.** `orb_theme_geo_t` gains
  `blob_off`: 0 for FORMAT 2 and the compiled pair, whose payload starts at the region's
  first byte; for a bundle, wherever its record's bank sits. `clock_view.c` adds one line
  and its 85 read sites do not move.

**Boot, and one fused pass.** `adopt_bundle3()` reads the region first — a bundle's art
lives at addresses only its records name — computes the installed span to the last bank's
sector-rounded end, and hands `art_bank_fill()` that span with the slot header's own art
figure. One pass verifies and lands the pixels, FORMAT 2's D30 rule untouched; it is not
§24.9's bug because the slot really does hold that span, pads and all, exactly as
`copy_bundle3` wrote and read it back. A refusal leaves the default drawn and puts its
sentence on the bundle's row, with one distinction: **a bundle with no `clock` section is
`ESP_ERR_NOT_FOUND`, not damage** — those bytes are whole and honest, they simply are not
a clock, and "re-install it" would send the owner to fix nothing.

**The row's identity needed a word.** `orb_bundle_rebase()` restamps the region checksum
when it moves banks, so a slot's `geom_crc` can never equal a card file's, and an
`active`/`changed` rule built on it would call every stored bundle `changed` forever.
`build_slot_hdr3` now keeps the **container's** checksum in header word 10 — FORMAT 2's
bake-duration word, always 0 for a bundle — and the scan answers a bundle row through the
same `theme_state` the FORMAT 2 rows use. §24.9's flag-lies-by-omission note is closed.

**Proof, third leg.** `tools/host_clock3_test.py` builds the shipped C with the shipped
bundle reader and drives it with the writer's own bytes. The oracle is the pair the
firmware compiles with: **231 checks, 0 failures** — every scalar, all five hands, all
five shadows, the overlay, sixteen blanks, the derived gif block, and the bank's bytes —
run against **two independently written tables**: one forged from the shipped pair (so
byte-identity is construction, not lineage) and §25.4's product bundle. Nine forgeries
draw the same sentence at both ends of the mapping, C and `--from-bundle` — the pair
§25.6's suite pinned from the other side. `--mutations` is **14/14**, each guard asked
about the lie only it refuses: a clean golden table satisfies every guard, so breaking
one and re-running the golden proves nothing, which is §25.4's lesson about a name check
that compared itself, applied here before it could bite. Two guards turned out to be
backstops rather than doors — `theme_schema.check_table` refuses an unknown class and a
repeated `(class, slot)` at the writer's door, so no bundle exists that carries either,
and they are driven as post-write byte tampers with all three checksums restamped.

**Two findings the battery cost, both worth the price.** The suite's first cut compared
the forged bundle against `clock_assets.bin` while reading `sec` from the *other*
fixture's loop binding — a green-looking run reporting a lineage failure that was really
a stale variable; and §25.4's product bundle is a re-bake of a FORMAT 2 container whose
plate differs from the shipped `.bin` by 59 bytes in its flat border, which is the kit
bake's own history and not the reader's claim. Hence the pair-forged golden: it makes the
byte check something the suite controls.

**Bench, orb-A, `v1.1.0+6b9f970` — and the board corrected the commit twice.** The
first flash reported the reader working; the boot log said otherwise (`source:
baked-in`, empty damage word), because `boot_resolve()` had one input and a chosen
bundle was not in its vocabulary. The second flash drew the bundle and the ledger
contradicted itself: `active: avkit-b31` beside a row saying `changed=True`, because
the slot header's art word is the INSTALLED span's checksum with the inter-bank pads
included and no card file has ever carried that number. Both are fixed, with the
board's wording in each commit message. What stands after them:

- `bundle "avkit-b31" from slot-B (chosen): 1175552 B refilled to the arena in 182 ms`
  — the §20.5(2) line through the FORMAT 3 door. The span is 1989 B larger than
  FORMAT 2's identical-theme figure because it is the sector-rounded installed region,
  and the refill costs the same **182 ms**: D30's budget holds on the new path, which
  is §24.5(2)'s answer, and the glass ran the clock at 12 fps off it.
- `source: slot-B`, `active: avkit-b31`, and that bundle's row alone
  `active=True changed=False`. §24.9's flag-lies-by-omission note is closed on
  hardware, not in a comment.
- **Clause 2 of the pass line, refused at the wrong door:** `POST /sd/install` with a
  bundle id answered `that is a FORMAT 3 app bundle — press Store, not Load`, left the
  drawn theme and its 182 ms refill untouched, and claimed no damage. No fallback read
  happened, so there was nothing for the retirement's guard to be there for yet.
- A stored-and-not-chosen bundle boots quietly (`slot-B holds bundle "avkit-b31" —
  stored, and not chosen`), which is D30's resting state holding for the new format.
- The device was returned to FORMAT 2 av-kit through its own door and reads
  `active: av-kit`, 182 ms, clean ledger.

**Step 2's artefacts, built — and two assumptions the bench corrected.**

`mk_bundle_fix.py --pad-to N` yields the cap fixture: av-kit's real bytes, one declared
plane on the end, **4,128,731 B of the 4,128,768 B cap — 37 B under**. Exact sizing is
impossible, not merely inconvenient: the reader requires the catalogue to account for the
bank byte-for-byte and every plane to equal its own `w*h*3`, and that product does not
divide an arbitrary remainder. So the flag names a ceiling and reports the largest legal
bank under it. The padded build carries ONE section, because the writer's budget gate sums
banks rather than spans — a cap clock plus a second section is over budget by construction,
and that refusal is the gate being right. The first fixture failed at both ends of the
mapping with the same sentence (`a plate at extent (0,0,464,464)`), which is the mirror
doing its job: the old synthetic table guessed a 464 panel, and the reader refuses a plate
that is not the panel.

`ORB_ART_ARENA_MARGIN_KB` is §20.5(4)'s pin, and it must exist because the gate as shipped
is not a lever: free PSRAM measured **7,206,536 B after** a 1,173,563 B refill, so a cap
bank needs 6,176,704 + 2,097,152 = 8,273,856 B against roughly 9.3 MB gross. **At cap the
arena cannot hold bank and margin on this board, so §20.5(2)'s predicted at-cap 470 ms is
not measurable here today** — the refusal happens first, honestly, naming both byte counts
and handing off to Arm A. Raised to the option's ceiling of 6144 KiB the pin left the
av-kit refill inside the gate by 21 KB, which is how close this board comes to a forced
refusal at the size that matters, and exactly the size-dependent flakiness that made a
compile-time knob necessary rather than optional. The `range 0 6144` clamped a 6400 ask
back to the default on reconfigure, so the bound holds and a typo cannot widen the
experiment by accident. Shipping stays 2048, now pinned from `main/Kconfig`'s default by
`host_art_residency_test.py`: a Kconfig default is the value a build that never mentions
the option gets, so pinning it keeps the guarantee the old literal gave instead of turning
the check into a tautology.

Two bench notes that will bite the next person. `serflash.sh`'s stamp guard compares the
COMMIT, so a 6144-KiB image and a shipping image carry the same `v1.1.0+<sha>` and only the
log line tells them apart — a pinned build is a different measurement wearing the same
label. And `pkill -f orb_watch_ser.py` matches the shell that runs it; the tools' own
refusal to double-open the port is the safer guard.

**Still owed to M6.** The glass's own visual comparison against the kit-rendered reference,
and the press that CHOOSES a stored bundle: Store deliberately does not choose and Load is
FORMAT 2's door, so a bundle reaches the dial today only via a reboot after a Store into a
slot already chosen. That lever is §24.5(4)'s flip press, blocked on the owner's ruling
about the default's home — not on this reader, which is now the thing the flip will press
onto. The at-cap refill needs a board with more free PSRAM than orb-A has, or §20.5(2)'s
cap figure is not merely unmeasured but unmeasurable on this rig.

### 25.6 §25.5(1) PAID 2026-10-19 — the bundle→pair reverse path

`bake_clock.py --from-bundle <bundle> [--bundle-section clock]` rebuilds
`clock_assets.{bin,h}` from a FORMAT 3 section with no kit, no PIL and no card in the
loop. §0.2's last clause is paid: a finished container now converts back into the
compiled-in pair.

**One format definition.** `_finish()` grew one parameter, `prov`, and prints the
provenance comment from it; every line below `#pragma once` is the same code both
paths run. A second `.h` writer would have been the drift, so there is none. Provenance
is the pair's only deliberate difference, and it names the container plus a hash of the
art — the rule `.clinerules` already uses to compare a kit re-bake.

**Every field is carried or refused.** Scalars ride verbatim (D31). Hands, shadows and
the overlay come from the element rows through §25.2's identity, and the shipped pair
reproduces exactly, including the three shadow crop pivots — a shadow's pivot is
`hand_centre − shadow_xy`, so the light offset is recoverable from the hand it hangs
under and the earlier worry about it was answered by measurement, not assertion. The
gif tables are rebuilt as one keyframe with zero delta rects, which is the only bank
shape FORMAT 3 can hold; `clk_gif_delays` is regenerated at
`round(1000 / tick_rate)` — R6's own ruling that the OS beat is the rate, and for
av-kit's 15 fps that is the shipped `67` byte-for-byte. What cannot be rebuilt refuses
by sentence: a tourbillon, a real delta bank, an extent the catalogue does not account
for, a plate that is not the panel, a shadow with no hand, and any bundle with no
`clock` section.

**Proof.** `tools/host_from_bundle_test.py` parses the shipped `.h`, forges the faithful
bundle through `orbbundle`/`orbtheme` with `assets/clock_assets.bin` as its art, and
requires the rebuilt pair to match: **76 checks, 18 refusal vectors, 0 failures.** The
rebuilt `.h` is byte-identical to `assets/clock_assets.h` from `#pragma once` down, and
the rebuilt `.bin` md5s to the shipped art. `tools/mut_from_bundle.py` is
**11/11 caught** — including the shadow-pivot recovery, the delay regeneration and the
`EF_HIDDEN` row, which was a real gap the first battery exposed. Against the same
bake's own container the `.bin` is byte-identical (`cmp` clean); against the shipped
`.bin` the delta is the 59-byte antialiasing fringe `.clinerules` documents for any
fresh bake of this kit, so the byte-exact claim is made against a container built from
the committed art and the fringe is measured, not assumed.

Firmware, the committed pair and every other suite are untouched: format3 52/0,
element 46/0, `mut_rebake.py` still 17/17, and the kit-bake path re-emits the shipped
`.h` and `.bin` unchanged after the refactor. §25.5(2) and (3) are still owed.




---

## 26 — No default theme: the fifth home for the default (RULED 2026-10-19, owner)

§24.1's dead end was real and it was not an arithmetic problem. It was a premise: that a
board must boot to a theme it was flashed with. Drop that and the knot unties, so the
owner dropped it. **A freshly-flashed orb has no theme at all.** Themes arrive from the
card, and only from the card. With none chosen and none installed, the dial renders the
**base version** of each app: what the OS can draw from its own primitives — LVGL's
compiled-in font, rectangles, lines, the STATUS line — with no art bank behind it.

**Partition arithmetic, checked before writing this.** `partitions.csv` puts `assets` at
**0x420000, 0x800000** (8 MB), and `CMakeLists.txt:62` flashes `assets/clock_assets.bin`
at that same 0x420000. `orb_theme.c`'s slot map is SLOT_A_HDR 0 / SLOT_B_HDR 0x400000
*within* that partition, so the flashed default lands exactly on slot A and runs through
its art region — D13's claim, verified rather than trusted. Stop flashing it and the whole
8 MB is two honest slots, which is what ping-pong needs: **the default's home is not found,
it is vacated.** `assets` keeps its offset and size, so the retention invariant at the head
of the CSV is untouched and the table does not move.

**What this retires, by name.** Not amends — retires, and the doc should say so rather than
keep clauses that no longer bind:
- **D13's clause.** It exists because the card-less fallback read pixels from offset 0 and
  offset 0 was inside slot A. No fallback pixels, no constraint. §24.1's four rejected homes
  are moot: this is option five, and it is the only one that costs a slot nothing.
- **`use_baked_in()` and `art_bank_map_at_0()`** (`orb_theme.c:744`, `art_bank.c:350`), and
  with them D13's "the default ALWAYS maps, whatever ORB_ART_MECH_B says".
- **The `s_source == "baked-in"` state**, the `built-in` row in `clock_themes.c`'s list, and
  the four sentences that promise `"— the default is drawn"` / `"the dial shows the factory
  clock"`. Each needs a true replacement, not a deletion: with no default, a refused refill
  leaves the base renderer, and the sentence must say that.
- **`target_slot()`'s asymmetry.** Slot A becomes a legal install target for the first time.
  The `host_tz_test.py` D13 table that pins "an install aimed at offset 0 erases the default
  and the fallback together" changes its meaning: it can now erase the *previous theme*, which
  is the point of ping-pong, and the table must be rewritten to test the new rule rather than
  deleted.
- **D30's safety property, restated not weakened.** "A torn flip leaves glass and boot on the
  default" becomes "a torn flip leaves glass and boot on the base render, and the last good
  theme in the other slot." That is at least as good a story: the base render is deterministic
  and version-matched to the firmware, where the old default was a snapshot of one kit baked
  at one date.

**M6's own pass line changes, and keeps its teeth.** Clause 3 read "a card-less board still
tells the time". Under this ruling it must read **"a card-less board still tells the time,
from the base render"** — the requirement survives, the mechanism is new. Do not let it be
closed by the sentence "there is no default, so nothing to check": that is how a board ships
that shows a black rectangle until someone finds a card.

**The cost, stated before the code, because it is the largest item in P6 and it is new.**
There is no renderer that draws an app from its declared elements today. `clock_view.c` is
85 hand-written read sites over one `clk_theme_t`, and `host_orblayers_test.py` pins the
fact in its own words: *the shipped analog clock is still a special view, not a table
renderer, and no element can reach the draw-list gate yet.* So the base render is not a
fallback flag — it is the §23 table-driven draw path, arriving early and with a harder
contract, because a special-cased clock cannot render an app that has no art. Two halves
that were planned separately turn out to be the same work:
1. **A primitive draw path** for at least TEXT, HAND, PLATE and STATUS: draw-list built from
   an element table, geometry from the rows, no `color_off` dereferenced when a section
   carries no bank. The §25.7 reader already hands back `n_spr` and a plane list, so "a clock
   section with no art" is expressible today and nothing draws it.
2. **`clock_view.c` made safe without a bank.** It currently returns an error when the bank
   is short (`clock_view.c:1766`), so with no default a fresh board would fail
   `display_init()` and the clock would never come up at all. That gate becomes a branch to
   the base renderer, and `orb_bg_classic_init()`'s plate read needs the same audit: a null
   blob today is a crash, not a fallback.

**Slices, in the order that keeps a working board at every step.** Each is flashable and
each leaves the dial telling the time.

1. **Base render, additive.** Draw the base clock when `orb_theme` reports no resident theme.
   The flashed default is still there and still preferred, so nothing regresses if this is
   abandoned halfway. Bench: run it by choosing nothing on a board that has a default, which
   needs a temporary override — say so in the commit rather than pretending the state occurred
   naturally.
2. **No-default boot.** Drop the `clock_assets.bin` flash line and the CMake guard beside it;
   retarget `use_baked_in()`'s two call sites at the base render; rewrite the four sentences
   and `s_source`; rewrite the D13 table in `host_tz_test.py`. A board that OTA'd from an older
   build keeps its slot-B theme and never sees the base render, so migration is free.
3. **Ping-pong.** Now unblocked with nothing left to protect: `target_slot()` alternates,
   §20.5(3)'s progress bar and arena swap land, `staged` keeps its meaning, and §24.5(4)'s press
   gets its two-stage form.
4. **FORMAT 2 retirement (§25.5(3)).** Ordering constraint GONE. There is no boot path left to
   brick — the reason the deletion had to wait was the default, and the default no longer
   exists. This can run any time after slice 2, including in parallel with 3.

**Slice 1 PAID 2026-10-19** (`6e1c6c9`), with the owner's face chosen: **digital** — large
`HH:MM` in montserrat_48, date beneath at 14, one status line reading `waiting to sync`
until the clock is synced and nothing after. Additive as planned: the flashed default is
still preferred, so shipping behaviour is byte-for-byte what it was and the new path is
reachable only through `ORB_BASE_RENDER_FORCE`, which slice 2 deletes. On orb-A the branch
ran once, boot completed, the clock came to the foreground, and **zero** sprite-commit or
blend lines appeared across 8 s — the themed path demonstrably did not build behind the
`return`. What was NOT verified then is the pixels, and the sentence under this one (added at
`ab13a13`) said no pixel path exists on this bench. It does now (`5e8655e`), because the gap
was worth closing rather than working around: `/api/v1/fb` serves the PSRAM panel mirror — the
same buffer the QSPI bands DMA out, and the only full-frame record there is, since the CO5300's
GRAM is write-only — copied on the LVGL task inside the commit window, after the clock has
composited and before the bands read. `tools/fbgrab.py` fetches it, checks received length
against the device's own `X-Orb-FB` header, and reports ink fraction, mean luma, a luminance
thumbnail and a `--diff` between two grabs. Measured on orb-A: PIL parses the BMP at the exact
geometry and agrees with the bench decoder within one luma unit at 25 points spread over the
frame; bmp and raw565 of one screen put ink at 97.48% and 97.45%; `--diff` reads 3.67% for two
frames of the same clock and 97.45% for clock against menu, so the metric separates drift from
content; a dark frame fails `--min-ink` with exit 1; and the capture costs the frame carrying it
19.5 ms (434,312 B staged, off the device log). Three of the four defects this first exercised —
a `?direct=1` path that could never run, a BMP header shifted four bytes, and rows 1402 bytes
wide where BMP wants 1400 — were invisible until something decoded the file, which is the
standing objection to log-line evidence in one measurement. What the capture has NOT yet seen is
**the base face**: orb-A boots av-kit, so slice 1's branch still has never produced a visible
frame. Slice 2's first act is to make it one and grab it.

Two facts slice 1 settled that §26's plan assumed rather than knew. The base face does
**not** want the dial's scheduler: `orb_sched`'s policy is a sprite view's contract (pose
hands, step a frame bank, mark layer boxes), so a text face owns a plain 1 s `lv_timer`
instead — installing the real policy here would tick at 12 fps to redraw a string that
changes once a minute. And the no-theme check is `!from_container` plus the override, not
`theme == NULL`: `use_baked_in()` today points `theme` at the compiled pair, so a NULL test
would never fire until slice 2 removes that function, and slice 1 would ship untestable.

**Slice 2 is next and it is the one with the teeth**: stop flashing the default, delete
`use_baked_in()`, `art_bank_map_at_0()` and the override, retarget `s_source` and the four
sentences that promise a default, rewrite the D13 rows in `host_tz_test.py`, and move the
partition-table write into the flash path so a board learns the two-slot layout. Slice 2 is
where M6's clause 3 becomes a real test rather than a sentence. **Working doc for whoever
picks this up: `docs/slice2-handoff.md`** — it re-verifies every anchor below, corrects three
in the outgoing summary, and records the one structural problem: the art gate at
`clock_view.c:1774` runs ahead of the base branch at 1811, so slice 2 is a reorder first and a
deletion second. Delete that file when the slice lands. Slice 2 no longer needs a
person holding the dial to check its face: it builds, flashes, and `tools/fbgrab.py` says
what the glass holds — ink fraction, mean luma and a thumbnail, plus `--diff` against a
capture taken before the change. That is what slice 1 could not do and why its status above
reads "runs, unseen".

**Slice 2 landed (owner decision 2026-10-20, Option B): the base face is the fallback, not
the default.** The plan's original teeth — "stop flashing the default, delete `use_baked_in()`
and `art_bank_map_at_0()`" — are NOT what shipped, because the owner kept the flashed default
preferred (a board with no theme installed should show the good baked-in dial, not a bare
digital face). What shipped instead, in three commits: (1) `refactor(clock)` decides art
residency and creates the screen **before** the art gate, so the gate and the themed leg read
one flag instead of recomputing it — behaviour-neutral, orb-A's dial moves only the digits
(6.82% pixels, mean delta 5.7 vs the pre-slice capture); (2) `feat(clock)` keys the base render
on `!art_resident` (Option B: default preferred, base is the no-resident-art fallback) and
deletes `ORB_BASE_RENDER_FORCE` and every reference to it; (3) `docs(clock_base)` restates the
header premise to match. The base face is now proven with pixels, not a log line. Grabbed on
orb-A, mean luma and ink: base face **0.99% ink / luma 1.9** (seq 2, commit-fresh), menu
reference **1.12% / 2.3**; the base face differs from the dial by **97.4%** (mean delta 55),
base-vs-menu by **2.1%** — two near-black faces that are each ~97% off the ~97%-ink dial, which
rules out a stale frame. All four host suites (tz, art-residency, sd, mdns) pass under
`--mutations`.

**One empirical premise this slice disproved.** The capture plan said to erase the assets
partition to force the base face. It does not work, and the reason is load-bearing:
`esp_partition_mmap` validates only that the requested range lies inside the partition, not the
bytes there, so it succeeds on erased `0xFF` flash; `art_bank_ready()` keys on that map pointer,
so residency stays true after an erase and the board takes the *themed* leg and draws garbage —
never `clock_base_show()`. A card-less board boots the flashed default by design (that is
Option B), so no reversible on-bench state (erase, `sd/default`, ledger fallback) reaches the
base face while an assets partition exists in the table. The capture above was therefore taken
with a throwaway forced-base build, reverted before commit; the shipped code has no bench
override. **Consequence, open:** under Option B the base face is a software fallback reachable
only when `esp_partition_mmap` genuinely fails — a board with no assets partition in its table,
a blob larger than the partition, or an exhausted DROM window. Whether a shipping SKU should
carry such a table is a §26 decision still on the table, not something this slice assumed.

**RULED 2026-10-20 (owner): ship WITH the default; the open item above is closed.** `idf.py
flash` keeps writing `assets/orb_default.img` (the FORMAT 3 av-kit image), so a fresh board
boots to the dial, and the base face stays exactly what Option B made it: the no-resident-art
fallback, never a factory state. The no-default-SKU option is rejected. This ruling also fixes
the reading of the same day's earlier close-out sentence, "displays the themeless default with
the digital clock": the shelf state is the slot-A dial; the digital face is what a REFUSED
slot A shows, and that sentence described the fallback leg, not the factory state. One
operational
consequence of the ruling: `serflash.sh` deliberately does not resend the assets partition, so
a bench board whose flashed blob predates FORMAT 3 lives in the refused-fallback state until
someone writes the bank back (`idf.py flash`, or `esptool write_flash 0x420000
assets/orb_default.img`).

**And the "erase never reaches the base face" premise above is FALSE for the shipped code —
re-measured 2026-10-20 on orb-B, no overrides.** The default leg now reads slot A's header
before it maps anything: `adopt_default_bundle()` (orb_theme.c:1140) flash-reads the header
and returns `ESP_ERR_NOT_FOUND` on erased or FORMAT 2 bytes, so `esp_partition_mmap` is never
reached, nothing maps, `art_bank_ready()` stays false, and the residency gate takes the base
leg. The always-map finding above belonged to the boot path it preceded. Measured on the
shipped build, all three no-default-adjacent states, each a `fbgrab` of the live glass:

| state | boot log | ink / mean luma |
|---|---|---|
| slot A stale (pre-FORMAT 3 blob) | `default slot-A refused: ESP_ERR_NOT_FOUND` | 0.87% / 1.7 |
| slot A erased (`erase_region 0x420000 0x800000`) | same refusal + `no resident art — base digital face (§26)` | 0.89% / 1.8 |
| default rewritten (restored) | `default bundle "av-kit" from slot-A: mapped` | 97.26% / 54.7 |

The two base captures differ by 0.65% of pixels (same face, digits moved); erased vs dial by
97.24%, which rules a stale frame in or out. With them M6's clause 3 — "a card-less board
still tells the time, from the base render" — is bench-testable on shipped code in a state a
board actually reaches: erase, grab, `--min-ink 0.3` passes at ~0.9% (the face is three
labels, not a black rectangle), write the bank back. No forced builds were needed; this closes
slice 1's "the base face has never produced a visible frame" gap from shipped firmware. In the
same commit: `art_bank_map_at_0()` deleted (no caller since the verified door landed; its
comment still asserted the retired always-map rule), and the two header comments that named
`use_baked_in()` as live corrected to the base-face truth.


**Two things this section will not guess.** Both are visible to a person holding the device,
and a wrong answer looks like a broken product rather than a wrong field. **The base clock's
face is ANSWERED** (owner, 2026-10-19): digital — large time, date beneath, one status line —
as the cheapest legible thing and the base form that asks least of a theme it has not been
given; slice 1 below is that answer. **What the glass and the flip page show before the first
theme ever arrives**, the state a board is in on a shelf — RULED 2026-10-20, twice: the first
close-out record below ruled the shelf state, and the same-day ruling above fixed its wording
(ship WITH the flashed default: shelf glass is the slot-A dial, the base face is the refusal
fallback, now measured in pixels on both paths). There is precedent
for the second — the unconfigured-network case already has an on-screen ask — and the answer
probably reuses it, but it is a decision and not an inference. Recorded here so whoever writes
slice 2 decides it deliberately rather than discovering it in the pixels.

**Slice 3 landed 2026-10-20 as the flip/choose door — two commits, ping-pong first.** The
plan's slice 3 read "Ping-pong… §20.5(3)'s progress bar and arena swap land"; what shipped
keeps the ping-pong and the choose halves and defers the progress bar and lazy arena swap to
their own slices (§24's bar, §29's ledger — the flip door is the seam that unblocks both).
(1) `target_slot()` derives the store target from which slot the dial draws, so the second
store overwrites slot A's flashed pixels — the owner's sanctioned cost, reflash or re-store
the recovery. (2) The flip: `boot_resolve` reads a slot designator (`"default"`, `"slot-A"`,
`"slot-B"`); the legacy `"card"` word is DELETED outright, not aliased — owner ruling
2026-10-20, new infra owes no backwards compatibility — so a board whose NVS still holds it
boots the default leg like any other unknown word until one press rewrites it (orb-A's
NVS is exactly such a board: the post-flash expectation there is the default leg plus
re-store, and that is the whole migration story), the boot block probes BOTH slots through one `slot_probe3` helper, and the `staged`
witness expires against either header so a store-then-flip order cannot strand the word. A
chosen slot that cannot honour the choice gets its own damage sentence naming the slot and
re-store as the recovery, asked before any unchosen-slot branch. `POST /api/v1/sd/flip?id=X`
is the door: the worker resolves the id against the slot headers (slot B preferred when both
hold it — B is the newer copy on a both-holds board), writes the choice, and arms the reboot;
`reboot:"automatic"` answers, and an id nowhere stored refuses on the row rather than booting
to a dial that would not load. The scan carries a live slot probe, so a row's
**"Put it on the dial"** appears for stored-not-live bundles with no card in the socket — the
slots outlive the card, and the door re-verifies the headers itself, so the scan only ever
over-suggests. The clock's `pick_default` and its active-row label treat slot A as the
default leg. `host_tz_test.py` (23 batteries) pins the slot vocabulary and the chosen-A
failure order; `host_sd_test.py` (22 batteries) pins the route, the flip worker's
choice-before-reboot order, and the JS anchors. **Bench PAID 2026-10-20 on orb-B
(`.34`, fw `7d57f2f`): M5_FLIP PASS — two stores answered reboot:none with no boundary
and a 4.48 s ingest into slot-B (the ping-pong target from the default leg), PSRAM
unchanged; the flip answered reboot:automatic, the worker logged its own sentence,
`rst:0xc` followed 1.1 s later, and the re-leased board reported live slot-B with the
av-kit row active; sd/default put the choice back.** The bench fix the run demanded is
itself a finding: with the base face printing no fps line (this board's flashed default
predates FORMAT 3, and the new reader correctly calls it `source none`), the m5 idiom of
anchoring windows on the last clock line reads the capture's own open-reset as a flip;
m5_flip.sh now anchors on the press's host wall time with a -2 s grace — a +3 s one
swallowed a real boundary that landed +0.86 s after the ack.

**§26 CLOSED 2026-10-20 — the first-boot ruling, and the flip benched on orb-A.** The
owner ruled the last open §26 question: an orb with no theme ever arriving displays the
themeless default with the digital clock — the shelf state stands; no code change, because
that is exactly the default leg slice 3 ships (draw slot A; the flashed bank carries the
av-kit re-bake as the fifth home; `clock_themes` draws the dial over it either way). The
`"card"` migration question died obsolescent with the alias deletion (`7f30bbb`): orb-A's
NVS was read on the bench and already says `"default"` — the §24.9 restore wrote it back —
so no board on this bench holds the word, and the host table's two dead-word vectors plus
the revived-word mutation are the whole remaining proof. **Bench PAID on orb-A (`.32`, fw
`v1.1.0+1dbad79`): `bench_flip.sh` M5_FLIP PASS** — boot ledger `live:"default"` drawing
`source:"slot-A"` (the ruled first-boot state observed live), two stores `reboot:none`
with no boundary (bundle_ms 4587/4787, PSRAM unchanged), the flip logged `flipped to
"av-kit" (slot-B)`, `rst:0xc` 1 s after the ack, re-lease reported `live:"slot-B"` with
the av-kit row active, `sd/default` restored `live:"default"`. The bench scripts that run
this gate are now clone-bound and renamed (`1dbad79`): `tools/m5_*.sh` → `tools/bench_*.sh`,
`ORB` defaults from `.orb-target`'s `ORB_IP` with a loud refuse on any override naming
another host, and `bench_store.sh` carries the same wall-time anchor fix `bench_flip.sh`
demanded — its clock-line anchor read empty on the base face and the empty anchor turned
the capture's own open-reset into a false flip FAIL.
**2026-10-20 — the §20.5(3) store-cut gate is paid, and §26's closure holds under a power
cut.** `tools/bench_storecut.sh` cuts the FORMAT 3 **store** window. The fuse is bench_powercut's
own idiom: POST /api/v1/reboot arms the board's 750 ms one-shot and does not drain `orb_jobs`,
so the tear lands where a pulled plug would. It is a new script, not a flag on
`bench_powercut.sh`, because the pass lines differ: that bench asserts a torn **app** is absent;
this one asserts a torn **copy** rolls back and that a flip onto it fails safe. The fuse fires
50 ms after the store press; trial 1 landed `rst:0x` inside the art copy (≈4.6 s, header not
yet written) on both runs. The ledger rolled back: `staged_id` names av-kit-b, `staged_why`
speaks, `live:"default"`, `source:"slot-A"`, the clock ticking at 16–17 fps. The torn copy
cannot theme the glass: slot B's OLD header still advertises av-kit and the flip door reads
headers only (by design), so the press took and boot's adopt refused — `damage_why:"the
installed copy did not verify — re-store it"`, serial `would not load (ESP_ERR_INVALID_CRC)`,
board back on slot-A. Failing safe with a sentence on the row is §20.5(3)'s gate, and it held.
Recovery re-stored clean (bundle_ms 4013, `staged` cleared), the flip onto the fresh copy
themed the dial (`source:"slot-B"`), `sd/default` put the board back where it started. The
first run's FAIL was the script, not the board: the recovery wait broke on stale stamps (a
press seconds after the flip's boot, with the `(e-2)` margin) and the `staged` guard tested an
inverted empty-expansion; both fixed in the same commit, and the re-run printed
**BENCH_STORECUT PASS**.


## 27 — P6.5(3) lands: the FORMAT 2 ingest door is gone, and P6 closes — 2026-10-20, `7c0fd4b`

**What shipped.** The FORMAT 2 write and read paths are deleted from the firmware. One
door admits one format. `orb_theme.c` loses its FORMAT 2 reader island (`hdr_parse`,
`card_check`, `geom_validate_blob`, `slot_read`, the FORMAT 2 install/upload branch);
`orb_theme_install`, `orb_theme_post_install`, and `POST /api/v1/sd/install` are gone; the
embedded `/themes` picker offers a single Store button and no Load, and the queue is
`orb_theme_post_bundle`. The FORMAT 3 store door is `card_check3` (scan),
`card_open_verified_id3` (identity), and `verify_bundle3` (install), each lifted verbatim
and host-pinned.

**What stays, and why it is not a contradiction.** The compiled-in `clock_assets` FORMAT 2
pair remains the card-less boot fallback (D21, §26 Option B). The retirement removes the
FORMAT 2 *dialect* — the container parser, the install door, the upload branch — not the
fallback pixels living in flash. A board with no theme installed still shows the baked dial.

**The pass line, proven this time.** §24.5 flagged that `_H3_FMT_SENT` had a mutation but no
positive vector, so "a FORMAT 2 file is refused at the format gate with no fallback reader"
was an unenforced claim. `host_sd_test.py` now builds a real FORMAT 2 container header
(`_H3_FMT2`: same magic, same shape, honestly checksummed — differing only in the format
word) and asserts the reader returns the format sentence. The format check runs before the
self-checksum, so a well-formed FORMAT 2 file hears "theme format this firmware cannot read"
and never reaches a fallback parse. `mk_bundle_fix.py`'s FORMAT 2 arm becomes the door's
refusal probe; `rebake_bundle.py` and `orbtheme.write_container`/`read_container` stay as the
re-bake *source* readers (§25.5(1)), not ingest paths.

**Verification.** Firmware builds clean (`EXIT=0`). All eight host suites pass.
`host_sd --mutations` catches 19/19; `host_tz` and `host_menu` mutation batteries report zero
survivors. The stale cross-references to the deleted theme reader in `orb_bundle.c`,
`orb_app_install.c` (×2), `orb_app_manifest.c`, `orbapp.py`, and `orbtheme.py` are retargeted
to `orb_bundle3_hdr_parse` / `verify_bundle3` / `card_open_verified_id3`.

**Consequence for the plan.** P6 is DONE. The theme platform is FORMAT 3-only end to end.
What remains on the roadmap is unrelated to the theme dialect: P6b (orb-manager's
editor/preview), P7 (the wasm3 code tier), P8 (OS-scheduled repaint), and the one open §26
decision — what the glass shows before a first theme ever lands.

## 28 — P6b lands: the hub speaks the FORMAT 3 store door; both clones trued to `origin/main` — 2026-10-20, `f3e31d7`

**Why this existed.** §27 retired the FORMAT 2 ingest door in the firmware. The
orb-manager hub lives in a sibling clone that branched ~20 firmware commits earlier,
so its device client still dialed the now-deleted `POST /api/v1/sd/install` and pinned
`h_theme_install`. The two `main` lines had diverged (firmware/this tree vs
`tools/orb-manager/**` + `.clinerules`/the sibling), and merging them made three
firmware-reading contract tests go red — the hub was describing a firmware it no longer
sat on top of.

**The merge.** The sibling's three orb-manager commits (hub auto-probe, the merged `.bin`
bench verdict, the esprom probe-doc fix) merged into this tree clean and file-disjoint
(`ee94eae`): no firmware, host-suite, or tool file outside `tools/orb-manager/` and
`.clinerules` moved, so every §27 host-suite result still holds byte-for-byte.

**The P6b hub change (`f3e31d7`).** The broken install client is gone; the hub now speaks
FORMAT 3:
- `security.go`: dropped the dead `/api/v1/sd/install` allowlist entry; added the missing
  `GET /api/v1/fb` (`h_fb`, the frame-capture read that reached firmware while the hub was
  behind — same gate as `/api/v1/screen`).
- `internal/orbnet/themes.go`: renamed the device verb `ThemeInstall` → `ThemeStore` so the
  method name matches FORMAT 3 truth — it dials `POST /api/v1/sd/bundle?id=`
  (`h_bundle_install` → `orb_theme_post_bundle`) and answers `reboot:none` with no dial flip
  until the P6.5 compositor.
- `theme.go`: the server route `/api/theme/install` and the flip-page button keep their
  names; only the device wire the handler dials moved to the store door.
- `internal/orbfake/`: dropped the FORMAT 2 `serveInstall`, added `serveFb`, corrected the
  `serveBundle` docs to the sole-store-door contract.
- Contract and behavior tests repointed to `h_bundle_install` / `ThemeStore`, and the
  live-double store test now asserts the FORMAT 3 promise (ack `storing`/`reboot:none`,
  dial unchanged) rather than the old install-flips-active claim.

**Verification.** `go test ./...` green in both clones. The three previously-failing
contract tests (`TestDeviceRouteAllowlistMatchesTheFirmwaresUriTable`,
`TestEveryFirmwareDeviceRouteIsAllowedAndFakeable`, `TestFirmwareAddressesThemeVerbsByID`)
pass. All eight firmware host suites still pass — firmware sources are identical to the
§27-closed tree. `go vet ./...` clean.

**The two clones, trued.** `origin/main` fast-forwarded `9c92162 → f3e31d7` (no force: this
tree was a linear descendant; the sibling's commits arrived via the merge). The sibling
`../gorb-os` fast-forwarded `--ff-only` onto the same `f3e31d7`. Both clones now sit at one
identical `main`, clean, `0/0` against `origin/main`. The stale `dev2` branch (its five
ahead-commits already in `origin/main` by patch-id) is deleted locally and on `origin`.

**Consequence for the plan.** P6b's client half is DONE. The roadmap's remaining P6b item —
the hub-side theme **editor/preview** §27 named — and the larger FORMAT 3 threads (P6.5's
glass/flip, P7 wasm3 code tier, P8 OS-scheduled repaint) are still open and unrelated to the
ingest dialect; the §26 first-boot decision named there closed the same day (RULED: ship WITH
the flashed default — see the §26 ruling records).

## 29 — D33: the web release decides P6b's shape (RULED 2026-10-20, owner)

**The requirement, as heard.** The next orb-manager release is served from a server and
flashes over the browser's ESP Writer plugin — the Web Serial flow the ESPHome website
uses. The Go module must stay lightweight, must exist only to replace the one thing a
browser cannot do, and the rest of the code must be portable without it. D33 drives the
P6b editor decisions rather than following them.

**Three facts measured on this tree, which set what "lightweight" can mean.**

1. **The device answers no CORS header.** `main/` sets `Cache-Control` and
   `Content-Type` and never `Access-Control-Allow-*`, and the hub strips that header on
   the way back (`proxy.go:182`, pinned by `proxy_test.go:213`). A page hosted on a
   server cannot read an orb's API cross-origin, so the same-origin forwarder at
   `/orb/<ip>/api/v1/...` is not a convenience — it is what lets a web release reach a
   dial at all. The firmware's own provisioning page escapes this only because the orb
   serves that page and its single write runs `mode:'no-cors'` (`orb_net.c:4981`).
2. **The forwarder already carries a full-size theme.** `proxyRequestCap` is `4 << 20`
   (`security.go:383`). The largest legal FORMAT 3 bundle is its `art_off` at cap plus
   `ORB_THEME_SLOT_CAP`: the 128 B header and a region at `ORB_BUNDLE_GEOM_CAP` (0xF000)
   pushed to the 4 KiB sector line make 65,536, plus 4,128,768 — **exactly 4,194,304 B**.
   A theme that fills a slot fits the generic forwarder with no headroom and no Go
   assembly step.
3. **`orbnet.ContainerName` refused FORMAT 3 until this slice.** `themecontainer.go` required
   `f == themeFormat2` and `internal/orbfake` agreed with it. Since `7c0fd4b` the
   firmware refuses FORMAT 2. The hub rejected every file its devices accepted and
   forwarded every file they rejected. **Fixed 2026-10-20:** the reader now admits FORMAT 3
   at `ORB_BUNDLE3_ABI`, verifies the header's own CRC and both D29 install sums, walks the
   40-byte index records for the slug list, and refuses FORMAT 1 and 2 in the firmware's
   words; the double writes and gates FORMAT 3 on its own copy of the layout. The four
   readers are pinned against one committed file —
   `tests/fixtures/format3-pair.orbtheme`, a 603,392-byte two-section bundle written by
   `tools/make_bundle_fixture.py` with an animated sprite and a four-plate bank, beside a
   `.json` of the authority's own read-back. `--verify-c` re-runs the shipped C reader over
   it; `internal/orbnet/themecontainer_test.go` re-runs the hub's. The JS writer matches the
   same file, which is what makes the D33 parity claim checkable instead of asserted. That
   file pins the **container** dialect only: its `clock` section carries colour-only sprite
   rows, which `orb_clock3.c` refuses ("catalogue row 1 claims 16000 B of a 40x200 plane;
   that extent is 24000 B"), because a hand's plane needs an alpha stride. The projection
   leg therefore runs on the browser's own bake (§29 next bullet), and the compiled clock's
   golden stays `host_clock3_test.py`'s av-kit pair forge.


**What D33 decides for P6b.**

- **The browser owns the FORMAT 3 writer AND the FORMAT 3 read-back check.** D28 makes
  the firmware resolver the truth; D33 makes the browser the place the product has to
  reach. A Go writer leaves the web build unable to bake, and a Go-only verifier leaves it
  unable to gate. `tools/watch-composer/orbtheme-baker.js` (its FORMAT 3 section, copied
  into the hub's `web/composer/` by `make composer`) mirrors `tools/orbbundle.py`
  (`pack_blob` → `packBlob3`, `pack_index` → `packIndex3`, `pack_header3` → `packHeader3`,
  `write_bundle` → `writeBundle3`, `check_budget` → `checkBudget3`, plus `theme_schema`'s
  `check_element`/`check_table` as `checkElement`/`checkTable`) and mirrors
  `orb_bundle3_hdr_parse`'s refusal order in `verifyBundle3` when it re-reads what it wrote.
  The proof is the mirror test `_selftest/headless-bundle3.test.js`:
  `python3 tools/make_bundle_fixture.py --vectors <dir>` emits the reference bundle and its
  input model with the Python that defines the format, and the suite requires the browser's
  bytes to match the committed `tests/fixtures/format3-pair.orbtheme` byte for byte, then
  hands those bytes back to `read_bundle` so the authority vouches for them rather than the
  fixture vouching for itself. It needs no pillow and no numpy for the writer leg: the
  writer takes encoded pixels.
- **The bake path is in the browser too, and its gate is the projection, not the door.**
  `bank3(planes)` encodes RGBA planes the way `theme_schema.bank_bytes` costs them (RGB565
  colour then an A8 stride, so a masked plane is `w*h*3` and a colour-only bank `w*h*2`),
  and `bakeClock3({plate, hands, shadows, plates})` assembles a `clock` section under
  `orb_clock3.c`'s own rules: plate slot 0 full-panel and one keyframe, hand slots 0-4, a
  sprite row only as a shadow beneath a hand of the same slot, overlay at plate slot 1, and
  `tourbillon_n`/`bg_frames` refused outright because the delta and carriage planes a FORMAT
  3 clock section cannot carry. The pass line is deliberately the harder one — `orb_bundle.c`
  opening a bundle proves nothing about whether the dial can draw it — so
  `tools/verify_js_bake.py` compiles the shipped `orb_clock3.c` with the shipped
  `orb_bundle.c` behind it and projects the browser's bytes: the plate seats at colour_off 0,
  the first hand at 434,312 (= 466·466·2), and the catalogue accounts for the bank to the
  byte. `budget3(bytes)` answers to that bullet's promise, reading both D29 install sums back
  off finished bytes rather than estimating them from the model.
- **A browser bake from real Chrome pixels projects, and zero of the fifteen shipped presets
  qualify for it today.** `_selftest/bake3-test.html` mounts the composer's own component,
  composes each preset's plate twice — frame 0 and the last frame of its loop — and calls the
  plate animated if any pixel moved. All fifteen fail. The least motion is `neon` at 40 px over
  a 15-frame loop and the most `kitetour` at 72,571 px over 8, and 14 of the 15 also carry
  complications a clock section has no field for — `aviator` alone has none, and still
  animates. This is not the bake failing; it is §25.3's
  rule measured against the product. `orb_clock3.c` gives a FORMAT 3 clock one static plate
  keyframe — it forces `bg_frames` to 0 and derives a one-frame gif block from the keyframe
  copy — so an animated geartrain has nowhere to ride until the complication mapping lands.
  The page therefore bakes a still of whichever preset qualifies first (today: none, so the
  skeleton still it uses is labelled a still), and the shipped projection accepts that bake:
  483,936 bytes, plate colour-only at `colour_off` 0, hands seated at (213,95) and (217,33),
  bank 479,840 B accounted to the byte. **The `.orbtheme` button must not be pointed at
  `bakeClock3` yet**: it would refuse every dial the gallery ships. What it owes instead is
  this sentence, in its own note, before an owner presses it.
- **No new Go format package enters the tree.** §22.3's install-end gate closes on the
  device, where it already landed (`orb_bundle.c:171-180`). Go's theme work shrinks to
  retargeting the identity read's dialect (fact 3) and forwarding bytes.
- **The budget meter is browser-side, and it is D29's authoring gate.** Both install sums
  shown before the bake and after, `== cap` accepted, refusal in the writer's own
  sentences.
- **The preview reads the element table, in JS.** D32's table is its only source. Pass
  line (b) is host-pinned: each preview row's rect and rotation centre equal the shipped
  pair's `clk_sprite_t` fields in `tests/fixtures/avkit-pair.h` — the same reference
  `tools/host_clock3_test.py` holds the C projection to. On-glass parity stays owed to
  P6.5's compositor, read back through `GET /api/v1/fb`.
- **The animated sprite is the animation carrier, and only four of the eight classes may
  carry one.** The sprite catalogue row is `{off, size, w, h}` and an element's `frame0`
  indexes it, so a bank of separately-stored keyframes is a FORMAT 3 fact today
  (`orb_bundle.h:184-193`, `ORB_BUNDLE_SPR_WORDS 4`, `ELEM_FRAMES_MAX 360` — av-kit's
  busiest bank uses 208). What the clock adapter refuses is the GIF **delta** mechanism
  specifically: `orb_clock3.c:238` rejects `plate.n_frames > 1` because FORMAT 2 stored
  the next frame's delta rects in the sprite row's `diffx/diffy/diffw/diffh`, four fields
  FORMAT 3's four-word catalogue row does not carry. `theme_schema.check_element` then
  narrows the choice further, and the editor has to honour it rather than discover it at
  bake time: **`sprite` and `hand` carry `n_frames` planes the OS advances** (`EF_OS_ADV`
  allowed); **`text` and `bank` carry `n_frames` planes the app advances** and refuse
  `EF_OS_ADV` outright ("holds app state, so the OS must not advance its frames"); **`layer`
  and `status` refuse `n_frames > 1`** ("is composited, not animated — it carries one
  frame, not N"), and `status` refuses the advance flag as well. `plate` and `ring` take
  both at the schema, and the compiled clock then refuses an animated plate in its own
  words. So an editor region that animates on the OS clock bakes to **one `sprite` row with
  `n_frames` planes**,
  size-gated by the 4,128,768 B cap — D29's measurement, not a guess. The plate behind it
  stays one keyframe. Animate anything on a compiled-dial clock section and the refusal is
  `orb_clock3.c`'s: that adapter maps `plate` slots 0-1, `hand` slots 0-4 and `sprite`
  slots 0-4 (its shadows) and names no field for a multi-frame sprite — it is the shipped
  compiled clock's reader, not the platform's compositor, and §25.3 rules that shape in
  "when a kit that needs them is on the bench". Pass line (a), installs and lists `OK:
  true`, is reachable today; pass line (b), it moves on glass, is P6.5's.
- **The app-offer reader moves to JS.** The bake-time intersection (Decision D) reads the
  `.orbapp` offer from the file the operator picks, so it needs a JS reader for
  `orbapp.py`'s manifest rather than a new Go client. D26's install-time refusal on the
  device stays the authority.
- **The cross-product release pack is a browser artifact.** `Watch Composer.dc.html:1584`
  already writes a STORE-method zip in JS. A theme×apps set is that zip with a manifest
  and the FORMAT 3 bundle inside, and installing one is the existing forwarder carrying
  the members in order through doors already allowlisted (`sd/app`, `apps/install`,
  `sd/theme`, `sd/bundle`). No `cmd/orbset`, and no firmware-adjacent Go.

**What stays in Go, and why each piece is irreducible:** the page host, the same-origin
forwarder with its route allowlist and address grant, the fleet store and serial console
the desktop build uses, and the embedded firmware bundle. Flashing moves to the browser
plugin. The desktop binary keeps its own flasher, because a person on a cable with no
server still needs one.

## 30 — The sibling fork is merged: the hub's FORMAT 2 gate was a real bug; both clones trued again — 2026-10-20, `8ecc8d3`

**Why this existed.** §28 trued both clones at `f3e31d7`. The sibling `../gorb-os`
then drifted again: 4 new commits, 7 behind. Two threads, different verdicts.

**The hub commit (`0aaaabc`, KEPT as `e891544`).** §28's `f3e31d7` renamed the device
verb and fixed the allowlist, but it left `themecontainer.go` gating `format == 2` —
this tree's hub rejected every FORMAT 3 bundle the firmware accepted since `7c0fd4b`.
The sibling found and fixed that real bug, not a duplicate of §28's work: FORMAT 3 at
ABI v3, the firmware's own refusal words, header-CRC re-check, both D29 install sums,
the 40-byte index walk, one fixture (`tests/fixtures/format3-pair.orbtheme`) pinning
all four readers, and `orbfake` gating FORMAT 3 from its own layout copy so a test
cannot read its fixture through the implementation under test. Dropping it would have
restored the broken gate.

**The composer thread (3 commits, KEPT as `e7d0c8e`/`568f018`/`8ecc8d3`).** The
browser FORMAT 3 baker: `orbtheme-baker.js` `writeBundle3`, the mirror suite
`headless-bundle3.test.js`, `bake3-test.html`, the fixture generator and
`verify_js_bake.py`, and the §29 ruling they carry. Genuinely new work on §25's
re-bake thread; nothing in this tree duplicated it.

**The merge.** `git rebase origin/main` onto `7f30bbb` replayed all four clean —
zero conflicts, because the two orbfake changes (the sibling's FORMAT 3 gate, our
`serveFlip` from `7d57f2f`) touch disjoint regions, and the docs additions land in
disjoint file sections. Pre-rebase state tagged `sibling-backup-56dffd3`.

**Verification.** `go vet` clean and 10/10 hub packages pass on BOTH clones — the
contract battery runs against this tree's flip-door firmware. `host_sd_test` and
`host_tz_test` green. `make_bundle_fixture.py --verify-c` reads the fixture as the
authority wrote it (2 sections, 832 B region, 599,296 B art). The node mirror suite:
RESULT PASS, 72 checks, including the shipped `orb_clock3.c` projecting the browser
bake — the byte-identical-to-authority claim §25 ruled holds across the merge.
`main/` is untouched by all four commits (`git diff origin/main..HEAD -- main/`
empty), so every firmware result on this tree still stands.

**The two clones, trued.** `origin/main` fast-forwarded `7f30bbb → 8ecc8d3` (no
force). `../gorb-os` pushed from; this tree `--ff-only` onto the same `8ecc8d3`.
Both clean, `0/0` against `origin/main` — §28's invariant restored.

## 31 — Animated plate: the FORMAT 3 contract for keyframe background banks — RULED and SLICED 2026-10-20

**Why now.** The owner asked the editor for a FORMAT 3 export and a send-to-orb, and
found neither (§29's measurement explains the first: all fifteen presets animate their
plate, and the compiled clock refuses a plate that names more than one keyframe). Ruled
direction: design the plate-multi-frame extension first, so animated presets ship whole
rather than stilled. This section is that design.

**The finding that sets the shape: no format bump is needed.** Every byte an animated
background would carry is already legal FORMAT 3. The clock blob's 26-word dialect
(`orb_bundle.h:47`, gate `orb_bundle.c:249`) already names `bg_frames` (w9), `bg_fps`
(w10), `bg_loop` (w11), `bg_every` (w12), `bg_mode` (w21) and `gif_frames` (w23); the
element row already carries `frame0`/`n_frames` with `ELEM_FRAMES_MAX 360`; the schema
already lets a `plate` take frames and `EF_OS_ADV` (§29's own words: "plate and ring take
both at the schema, and the compiled clock then refuses an animated plate in its own
words"). The two refusals that stand between a preset and the glass are projection
behaviour, not format: `orb_clock3.c:277` rejects `bg_frames != 0` and `:303` rejects
`plate.n_frames != 1`. The planes need no new machinery — this is the carriage mechanism
(D32): contiguous keyframe planes in the catalogue, OS-advanced at a named fps. FORMAT
2's delta-GIF needs the rects/delays subtables a bundle blob has no room for
(`BLOB_COUNT_WORDS` is n_elem/n_spr/n_scalars/slug_len — there is no rects count);
keyframes do not.

**The contract, stated as acceptance rules.** A clock section MAY carry an animated
background iff, and the projection refuses the whole dial otherwise (refuse-whole, D32's
policy):

1. The animation rides **the plate row itself**: class `plate`, slot 0, full-panel
   (existing rule), `n_frames = N`, `frame0 = k`. Slot 1 (glass) and every other class
   keep `n_frames == 1`.
2. `scalars.bg_mode == 0` (keyframes). Mode 1 (delta bank) keeps §25.3's refusal, in
   those words — a delta bank is still unruled, not merely unimplemented.
3. `scalars.bg_frames == N − 1` — FORMAT 2's own semantics ("distinct extra plate
   frames"), mirroring the carriage's scalar/row cross-check (`:264`): the scalars and
   the row must name one animation.
4. The N plate planes are contiguous in catalogue order at `frame0`, each exactly
   `scr_w·scr_h·2` B (colour-only, `PLATE_BYTES_T`). The §25.3 byte-for-byte accounting
   already proves the bank inside the art region; this pins its shape.
5. `2 ≤ N ≤ CLK_BG_MAX 16` when `bg_frames > 0`; `bg_fps` obeys the same free-u16 rule
   tourbillon_fps got (D32 ruling C): projection validates the shape, runtime picks the
   rung. `bg_loop`/`bg_every` ride as authored.
6. `gif_frames == 0` (no delta vocabulary is honoured by this shape; av-kit's
   source-truth `gif_frames 15` under a still plate stays legal exactly as §25.3 ruled,
   because nothing is claimed of it).

**Cost is the real constraint, and it is arithmetic, not opinion.** A full-panel keyframe
is 434,312 B (466², RGB565, colour-only). Against D29's 4,128,768 B bundle cap: a body the
size of av-kit's art (1,173,563 B) leaves **6 plate frames**; a bare plate-plus-hands kit
reaches 9. `ELEM_FRAMES_MAX` never binds here; the cap does. `theme_schema` gains the
authoring check — `N·scr_w·scr_h·2 + Σ other planes ≤ slot cap` — so the editor refuses in
the writer's own sentence at design time, not the installer at store time.

**Consumer and the rate question.** The bank lands in `clk_theme_t` as plane records: a
new `clk_sprite_t bg[CLK_BG_MAX]` plus a count. The FORMAT 2 fields at `clk_types.h:45-58`
describe a rects world this shape does not use; the record carries planes, not offsets.
`clock_view` grows a third standalone object beside the carriage (`s_car`, §23.11): one
LVGL image, full-panel, behind everything, `src` swapped per advance, loop per
`bg_loop`/`bg_every`. Its flush cost is the panel's worst case — every advance
invalidates all 466 rows. The nearest measured figure is the D32 bench: a carriage face
min'd at **6 fps flush-bound**, and that animated a 146² region plus the hands. Until P8
puts a real rung ladder on the dial, the honest policy is: request `bg_fps`, expect the
flush floor, and let P8's measured table set the ceiling. §23.11's bench idiom repeats:
the shipped bake carries the fps the bench measured, not the fps the preset loops at.

**Which presets survive is a measurement, not a claim.** §29's motion numbers were
first-vs-last frame; a loop's true motion is the union of consecutive-frame diffs (the
bake3-test idiom, widened). A preset whose motion is local bakes better as a **static
plate plus a masked sprite row** at the motion's bounding box — §29's ruled animation
carrier, and 15 frames of a 120² region costs 648 KB, not 6.5 MB. A preset with
whole-panel motion (kitetour's 72,571 px per loop is the signature) must fit N keyframes
under the cap or reduce frames. The bake tells the truth per preset; no preset is
promised an export before its motion union has been measured.

**The editor slice, in order.** (1) `orbtheme-baker.js`: `bakeClock3` stops forcing
`bg_frames` to 0 and accepts plate keyframes under rules 1-6; the composer's export
computes each preset's motion union and picks plate-bank or sprite-region by the cap
arithmetic; the `.orbtheme` note (line 364) is rewritten from FORMAT 2's
`/orbtheme/<sha256>` world to the FORMAT 3 store/flip story, carrying §29's refusal
sentence for whatever the split cannot. (2) Firmware: `orb_clock3.c` replaces the two
refusals with the cross-checks above; `clk_types.h` grows `CLK_BG_MAX` and the plane
array; `clock_view.c` grows the bg consumer beside the carriage's. (3) Proof:
`host_clock3_test.py` goldens the animated projection; `verify_js_bake.py` projects a
browser-baked animated bundle through the shipped C; `mut_rebake.py` gains mutants
(bg_frames/row disagreement, non-contiguous plate bank, animated glass, over-cap N);
`headless-bundle3.test.js` re-pins the mirror; the bench runs the `bench_d32.sh` idiom on
an animated bundle and `fbgrab` settles glass-versus-requested-fps.

**One hardening asked alongside.** The index's per-section `abi_min`/`abi_max`
(`orb_bundle.c:391-392`) are parsed and enforced by nobody. This extension deliberately
needs no bump — which makes it exactly the day those fields were promised for. Wiring one
check (`abi_min > ORB_BUNDLE3_ABI` refuses the section with the firmware-too-old sentence)
costs four lines and makes the next dialect change negotiable per section instead of
bundle-wide.

**What this deliberately does not buy.** Send-to-orb from the composer page: the boards
serve no CORS headers, so the push rides the hub's `/api/theme/upload` — already FORMAT
3-correct and demonstrated live on orb-B with store, flip and delete — and the composer's
export stays a download the owner picks in that button until a hub round adds a push
action on the composer frame. Delta-GIF backgrounds: unruled, per rule 2. Glass
animation: refused by rule 1 — the crystal composites, it does not loop.

**Ruling asked of the owner.** Accept rules 1-6 as the plate-bank contract (keyframes
only, delta still refused); accept `CLK_BG_MAX 16` with the cap arithmetic as the binding
limit; accept motion-split bake policy (plate bank vs sprite region, chosen by the
authoring check); accept or defer the per-section abi enforcement.

### 31.1 — The ruling and the landed slices (2026-10-20)

**Ruled.** The owner accepted the contract and set the first cut: rules 1-6 stand as the
plate-bank contract, `CLK_BG_MAX 16` is the bound with the D29 cap as the real limit, and
"start on the baker and the projection" opened slices 1-2 the same day. The per-section
abi check stays on the shelf — deferred, not refused; four lines when the next dialect
change needs it.

**Slice: projection + pair mirror.** `orb_clock3.c` replaced its two refusals with the
cross-checks: the delta-vocabulary and capacity guards sit with the scalars, the
row/scalar one-length, gif-disagreement and one-shot-tempo guards sit with the plate, and
the plate-off seating guard sits after the row bounds are proved. The catalogue's
colour-only rule generalized from "row 0" to "the plate's plane range" — keyframes cost
`w*h*2` like the plate, and the walk accounts them at that. On success the projection now
fills the plate-frame table with one offset per extra plane (`io->bg_frames[0..N-2]`);
a still plate keeps the single keyframe row it always wrote. The glass rule (no animated
overlay) fires in the table walk itself, before any offset is trusted. `bake_clock.py
--from-bundle` says the same sentences in the same order, and its rebuilt `.h` carries
the bank in the `clk_bg_frames` array the pair has always had — `baked_frames` now fills
from the catalogue, so a rebuilt pair and a kit pair remain one definition. `CLK_BG_MAX`
moved from `orb_sched.c`'s private 8 into `clk_types.h` as the shared 16; `orb_theme.c`'s
dead `BG_FRAMES_MAX` comment-pointer died with the move.

**The consumer needed no new object.** §31's step (2) had `clock_view.c` growing a bg
consumer "beside the carriage's"; the walk of the shipped code found the consumer already
live: `orb_sched.c`'s classic bgAnim path (`orb_bg_bg_live`, `build_bg_dscs`,
`orb_bg_frame_now`, `orb_bg_show_plate_frame`) is FORMAT 2's whole-plane flipper, it
reads exactly the theme scalars and the `clk_bg_frames` table the projection now fills,
and the install path already wires them (`s_geo.bg_frames = s_b3_bgf`). What FORMAT 3
never did was feed it. An installed animated theme therefore flips at `orb_sched_bg_rate`
— the dialect's own ladder, the same one the kit path drove — with no new runtime code.
The flush floor (§23.11's ~6 fps for full-panel invalidations) still binds until P8's
rung table; the ladder is requested tempo, not delivered glass.

**Slice: baker.** `bakeClock3` takes `plate.frames`, an array of two or more whole-panel
planes: they enter contiguous and colour-only under one row, the scalars cross-set
(`bg_frames = N-1`, `bg_mode` 0, `gif_frames` 0), a bank with no stated tempo means a
loop at `every: 1` (what every FORMAT 2 bgAnim kit was), and an explicit one-shot without
an fps is refused rather than frozen. The D29 sum is checked against the assembled bank
before the write, in a sentence that names the keyframe cost so an owner can trade
frames. `rowFrames()` is the one new writer helper; every existing refusal keeps its
sentence.

**Proof.** `host_clock3_test.py` grew four mirrored REFUSALS vectors and a §31 battery: a
three-keyframe bank forged from the shipped pair projects at both ends, its plate-frame
table lands at the keyframes' own offsets, its rebuilt `.h` carries `clk_bg_frames[2]`,
and five bank-only lies (scalar disagreement, delta-mode claim, off-plate start, masked
keyframe, shuffled bank) draw the projection's own sentence at both ends. The mutant
anchors moved with the guards they pin — the zero-frame backstop now drives the plate
floor, and the catalogue-seat mutant moved to a hand row because the generalized walk
catches a plate-row lie first. 303 checks, 0 failures, with `--mutations` across 17
guards. `headless-bundle3.test.js` grew §31's accept side (row, catalogue costs, scalar
cross-set, art sum) and five authoring refusals, and its projection-parity step now runs
both bakes through the shipped C — `plate bank: 2 extra keyframes at 434312, 868624`
printed from `verify_js_bake.py`, which learned to report the table. 82 JS checks, all
six suites green, all 24 host suites green, `idf.py build` clean.

### 31.2 — the split landed, the glass said yes (2026-10-20, same day as the ruling)

**The bench passed on orb-B (`v1.1.0+acc917e`).** `tools/bench_d31.sh` with its fixture
`tools/bench_d31_bundle.py`: the shipped av-kit dial with its still plate replaced by a
four-frame keyframe bank of a turning medallion (colour-only planes at the catalogue
head, `bg_frames 3, bg_fps 8, bg_loop 1`), host-projected through the shipped C and
mirrored by `--from-bundle` before a board is touched. The device agreed with the host
to the byte: the boot line — a line §31 added to the loader for exactly this grep —
reads `plate bank: 4 keyframes @ 8 fps, loop`; the flip landed it live (`live=slot-B`,
`active=d31-anim`, press-to-boundary 1 s, refill 384 ms / 2,478,080 B). Two `fbgrab`s
0.16 s apart across one bank stride diffed **80.41%** and **4.65%** of pixels (loop
phase decides which frame pair lands; hands alone account for under 1% — the plate is
moving on the glass). The heap question the ruling reserved for the bench: `heap_psram`
7,472,728 → 4,982,352 across the flip, the bundle's own 2.48 MB footprint, no surprise.

**The tempo question §31.1 left open, answered (§31.1 step 5):** requested `bg_fps 8`
lands the ladder at `every=2` per ≈ 62 ms, and the steady clock on the animated face
measured **min 5 / median 7 / max 8** over the bench's 30 s window, first settling
second excluded. No ladder clamp is needed at 8: the flush floor for a whole-panel bank
is ~5 fps on this board, every shipped tourbillon loop runs at 4, and 4 sits inside the
floor with margin. The first bench run's FAIL was the bench's own gate counting the
flip's settle second as the floor — the window now starts 3 s past the boundary, which
is the difference between measuring the flush floor and measuring the reboot.

**The motion table, measured as §31 ordered it (step 3).**
`tools/watch-composer/_selftest/motion-test.html` mounts every shipped preset through
the component (with its real train, cfg and case — the live pick's state, which
bake3-test's design-only mount left classic: the caged presets' unions differ by half
between the two mounts), composes the plate at every frame including the wrap, and
feeds the consecutive-frame union and its bbox to **`GorbBake.splitPolicy3`** — one
policy function for sweep, composer, and suite alike (still under a 0.1%-panel shimmer
band; carriage bank first when the box is local AND the train is caged; plate keyframe
bank next, thinned by frame step before it ever refuses; the refusal sentence names the
byte it tripped on). Fifteen presets, fifteen honest verdicts, zero refusals:

| carrier | presets | figures |
|---|---|---|
| bank at native tempo | skeleton, orbital, opentour, kitetour | 8 keyframes @ 4 fps, 3,474,496 B |
| bank, thinned | calendar, regulator (÷2), modern (÷3), sector, diver (÷2) | 7–8 keyframes, 3.0–3.5 MB |
| still (shimmer) | aviator, station, luna, neon, phase, perpetual | union 47–65 px — AA jitter RGB565 swallows |
| carriage (sprite) | none today | every local box measured un-caged; every caged train measured whole-panel |

§29's "0 of 15 qualify" is superseded: 9 of 15 bake animated today, 6 bake as the
stills they already are. The carriage carrier (`bakeClock3`'s `carriage` row — hand
slot 5, masked contiguous planes, `tourbillon_n/fps` cross-set, four refusals mirroring
the projection, C-parity proved) ships idle on this table and ready for the first
custom dial whose motion measures local on a caged train.

**The composer slice (step 4).** The Device-theme button runs `exportTheme3`: guard,
measure, split, bake, download — the walk in the click path, the status line naming
the carrier and the art meter, the note stating the comp-table refusal BEFORE the press
for the six comp presets (the clock's comp mapping remains the open §31-adjacent slice).
The note div now tells the FORMAT 3 store/flip contract the watch actually implements.
`headless-ui.test.js` follows the new note text; `headless-bundle3.test.js` grew the
carriage section and nine `splitPolicy3` checks (96 JS checks, all six suites green,
all 24 host suites green, `idf.py build` clean).

**Correction, same evening — the guard lied and the note disagreed with it.** The
export refused any design with any comp row and quoted the raw count: Skeleton, the
composer's default dial, pressed to "shows 2 live complications" while its own button
note had promised the bake — an opening is a hole in the baked plate and a decorative
flare is engraving, both shipped untouched by FORMAT 3. The refusal and the note now
count through one predicate the baker exports (`GorbBake.liveComps3`): live is a window
showing calendar text, a moon, any subdial (its hand is not a baked slot), and a
date-printing flare; an unknown type counts live, so the safe direction stays a refusal
the author can see. Four bundle3 checks pin it. The shipped bake set narrows from the
sweep table's motion verdict to what the button actually carries today: **sector,
orbital, kitetour bake as banks; aviator, station bake as stills; the other ten
presets refuse** with an honest live count, until the comp-table bake lands.

**Still open on this contract:** the hub's send-to-orb round (the button downloads;
the hub's upload door is the receiver), the comp-table bake, and P8 consolidating the
delivered-fps ladder across bank sizes.





