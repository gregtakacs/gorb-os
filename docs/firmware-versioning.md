# Firmware versioning

Every image this repo builds carries one stamp, in one field, composed by one script.

    v<MAJOR>.<MINOR>.<PATCH>[-<prerelease>]+<7-char SHA>[-dirty]
    v1.0.0+e1a2b3c        v1.2.0+e1a2b3c-dirty        v1.3.0-rc.1+e1a2b3c

## Why the metadata is mandatory

`version.txt` declares an intent. Anyone with write access can type any number into
it at any time, so a bare triple says nothing about the bytes on a board. The part
after the plus is a git fact: the SHA names the commit the tree was at when cmake
ran, and `-dirty` says the tree carried uncommitted edits at that moment. A board
that prints `v1.0.0+e1a2b3c` states which commit it runs. A board that prints
`v1.0.0+e1a2b3c-dirty` states that the question has no clean answer, which is the
honest result and the reason the flag exists.

The scheme replaces the hub's previous ordering rule. A git-describe stamp has no
version order, so `firmware.Compare` could only rank builds by the embedded compile
timestamp — a clock that a dev host with a skewed RTC can set backwards, which made
an upgrade decision depend on something that is not a fact about the code. The
declared triple now outranks the timestamp, and the timestamp survives only as the
fallback that lets a mixed fleet of legacy orbs keep working.

## Grammar

`tools/orb_version.sh` is the only composer. Both callers — root `CMakeLists.txt`
and `tools/serflash.sh` — read its output rather than computing anything.

    <stamp>      ::= <declared> [ "+" <sha> [ "-dirty" ] ]
    <declared>   ::= "v" <num> "." <num> "." <num> [ "-" <pre> ]
    <num>        ::= digit | digit digit            (0 through 99)
    <pre>        ::= <ident> ( "." <ident> )*
    <ident>      ::= [0-9A-Za-z-]+ , a numeric one without a leading zero
    <sha>        ::= 7 lowercase hex

Two rules are easy to get backwards and both are tested:

- A numeric prerelease identifier may not have a leading zero (`rc.01` is refused,
  `rc.1` and `rc.0` are not). Non-numeric identifiers may contain hyphens.
- `-dirty` follows the SHA, never precedes it. `v1.0.0-dirty+e1a2b3c` parses as the
  prerelease `dirty` and is a different, legal, meaningless thing.

`--self-test` runs 14 assertions, including the dirty predicate on its own, the
no-commit case, a nested checkout, the prerelease grammar and the width. It is the
only check in this repo that needs no ESP-IDF: ~2 s, no toolchain, no board.

## Width arithmetic

`esp_app_desc_t.version` is `char[32]`: 32 bytes of storage, 31 usable characters
plus a NUL. ESP-IDF copies with `strlcpy`, so an overlong stamp truncates silently
and the build reports the wrong version forever. cmake therefore enforces the whole
stamp against 31 before anything compiles.

    v99.99.99-rc.99+e1a2b3c-dirty     31 B   the two-digit grammar's worst case
    v1.0.0-rc.1+92424ff-dirty        23 B   this project's realistic worst case

The two-digit cap is arithmetic, not taste: the same worst case with one three-digit
component is 32 bytes and will not fit. The declared version is the only part a
human edits, and at two digits per component it tops out at `v99.99.99`.

## Bumping

    edit version.txt  ->  idf.py reconfigure  ->  idf.py build

The reconfigure is load-bearing. cmake runs the composer at configure time and
caches the result in `build/CMakeCache.txt`, so `idf.py build` alone recompiles your
edits and keeps the old stamp: an image carrying new code under an old version,
which is worse than no versioning at all because it looks clean. `idf.py
set-version` is not the fix — it re-runs IDF's own version finder, which ignores
`version.txt` entirely once `PROJECT_VER` is set.

`tools/serflash.sh` reads the stamp back out of `build/gorb-os.bin` at byte offset
48 and refuses the flash unless it equals the live composer output. `--check` runs
that audit without touching a board. The mismatch line prints the fix.

`orbpack` refuses to ship a stamp that is not a version, because the built-in bundle
is what an end user flashes with no choices made. `-force` overrides it for a bench
build; `firmware.Load` stays permissive so a person can still load an old `.zip` and
deliberately downgrade.

## Ship chain: from an edit to a pushable hub

The whole route one changed source file travels before an orb can be updated from
the hub's card. Every step is required; skipping the reconfigure or the bake is what
makes a hub offer — or refuse — the wrong update.

    edit version.txt (if the triple changed)
    idf.py reconfigure && idf.py build                     # repo root, ESP-IDF shell
    cd tools/orb-manager
    make firmware                                          # orbpack build/ -> embedded .zip, + guard test
    make manager                                           # bin/orb-manager, bundle compiled in
    # restart the running hub (or relaunch the desktop app) — a running hub serves
    # the bundle it embedded at build time, never the file on disk

`make firmware` runs the `BuiltInBundle` guard, but the guard only compares the
committed `.zip` against `$(BUILD_DIR)` when `ORB_BUILD_DIR` is set — the `make
check` chain. A bare `make firmware` sets it for its own test run, so the baked
bundle is verified at bake time; what nothing verifies is a hub built **without**
`make firmware` after a firmware build: it silently ships yesterday's code under
today's stamp, and `firmware.Compare` judges stamps, not contents. If the hub's
"update available" flag disagrees with the source tree, check the bake before the
comparator.

The hub's answer on `/api/orb/update` is the same triple comparison the flash gate
uses, so a rebuilt image under an unchanged `version.txt` refuses as `same-version`
with "already runs this firmware". That refusal carries no `overridable` — the only
overridable verdict on the route is `downgrade`, where the bootloader's rollback
makes the second press recoverable. The fix for a same-version refusal is the ship
chain above, not a force: bump the triple, reconfigure, rebuild, re-bake, and the
fleet sees a real upgrade. A scripted caller can still push through any refusal with
`{"ip": "...", "force": true}`; the gate guards the button, not the API.

## Watching an update go in

A Wi-Fi push has three surfaces, and all three now say what is happening:

| Surface | What it shows | Source |
|---|---|---|
| hub card / flash modal | `sending N of M KB — P%`, then `staged`, then `rebooting` | `GET /api/orb/update/progress?ip=`, fed by the byte count the hub's own HTTP writer takes (`orbnet.PostImageProgress`); polled every 700 ms while the POST is open |
| orb dial | the boot overlay parks the glass and draws `updating` with a KB count and ring, then the clock returns when the image is staged; a reboot request re-parks on `rebooting` | `h_ota` / `h_reboot` in `main/net/orb_net.c`, same park the card upload uses |
| anything HTTP after the fact | `slot` and `img:"pending"` on `/api/v1/status` while a staged slot awaits its reboot | the A/B state the boot itself carries |

The hub is the only witness with live bytes: `h_ota` holds the one httpd task for
the whole body, so the orb answers no request — not even a status one — while a
push is landing. That is also why the page polls the hub instead of the orb. When
the push commits, the hub folds the new stamp into the fleet row itself
(`NoteAnswer`), so the amber badge clears on the next frame instead of waiting for
the rebooted board to re-announce; a rollback rewrites the row back within one
sweep, because the board's own answer always outranks the hub's note.

## Same-triple rebuilds: what the gate does and why

`Compare` weighs the declared triple first, so
`v1.1.0+aac2a23-dirty` against `v1.1.0+91c69fe-dirty` is
`same-version` — two different builds of one release. The gate refuses it without
an override, on purpose: a badge and a button that fire on every rebuild would
re-push the fleet whenever the hub's tree moved, and a version that means nothing
stops gating anything. The refusal names the verdict so the page and a scripted
caller can see why it stopped; the ship chain above is the way through.

## The four surfaces that print it

| Surface | Where it is read |
|---|---|
| mDNS TXT `fw=` | `main/net/orb_mdns.c:342`, into `char fw[32]` at `orb_mdns.h:51` |
| HTTP status | `main/net/orb_net.c:2292`, printing `app->version` at `:2341` — no intermediate buffer |
| Serial boot | the bootloader's `app_init: app version:` line, from the same field |
| Bundle / hub API | `appDescriptor` in `tools/orb-manager/firmware.go`, `Release.Version` |

The hub answers the triple as a structured `semver` object on `/api/v1/firmware` and
in each `/api/v1/identity` device row, so no front end has to parse semver:
`{legacy, raw, declared, major, minor, patch, pre, commit, dirty}`. A pre-scheme stamp
reports `legacy: true`, and the UI prints the raw string for it.

## Reading a stamp without flashing anything

The descriptor lives at a fixed offset inside the app image, so a built `.bin` answers
the question on its own:

    python3 - <<'PY'
    import struct
    d = open('build/gorb-os.bin', 'rb').read()
    o = d.index(struct.pack('<I', 0xABCD5432))          # esp_app_desc_t magic
    print(d[o+16:o+48].split(b'\0')[0].decode())        # .version, 32 bytes
    PY

`cmake -LA | grep ORB_VERSION` shows what a configure resolved. If that disagrees with
`tools/orb_version.sh`, the cache is stale and the fix is `idf.py reconfigure`.
