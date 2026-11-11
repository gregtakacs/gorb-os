#!/usr/bin/env python3
"""orbsdk.py — build, pack and install a gorb-os app, inside this tree or out of it.

An app is a directory holding app.json (its manifest), version.txt (its release stamp)
and its C sources. It links against this SDK directory and nothing else: no libc, no OS
header, no LVGL. The OS's whole surface is the "orb" import module declared in orb.h, so
a module that imports anything else, or anything the OS does not bind (binds.json), is
refused here rather than on the dial.

    orbsdk.py new <dir> [--slug S] [--title T]   scaffold an app from template/
    orbsdk.py build [<appdir>...]                compile to <appdir>/build/<slug>.wasm
    orbsdk.py pack [<appdir>] [-o DIR]           build, then write DIR/<slug>.orbapp
    orbsdk.py install <appdir|file.orbapp> --orb HOST
                                                 pack, upload to the orb's card, install
    orbsdk.py publish --site HOST <file.orbapp>  post a built container to an orb-os.net site
    orbsdk.py inspect <file.orbapp>              read a container back
    orbsdk.py binds [--write]                    in a gorb-os clone: check (or rewrite)
                                                 binds.json against the firmware
    orbsdk.py dist [-o DIR]                      in a gorb-os clone: write the SDK tarball
    orbsdk.py check-migrations [<appdir>...] [--runtime DIR] [--chrome PATH] [--save-current]
                                                 open each app's schema fixtures in its packed
                                                 editor (needs Node and Chromium, or save one)

The same file runs in two places. In a gorb-os clone it is apps/sdk/orbsdk.py, it finds
the container writer at tools/orbapp.py, and every build also checks binds.json and
clk_types.h against main/, so the SDK cannot drift from the firmware it describes. In
an SDK tarball (orb-sdk-<version>.tar.gz, attached to every gorb-os release) orbapp.py
sits beside it and there is no main/ to check against: binds.json is the OS's surface as
of that release.

The compiler is clang targeting wasm32 -mcpu=mvp (no proposals), -O2 -Werror,
freestanding; override it with ORB_CLANG. Builds are deterministic: no timestamps, no
paths in the module, so unchanged sources rebuild byte-identical.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import zlib
import tempfile
import urllib.error
import urllib.parse
import urllib.request

SDK = os.path.dirname(os.path.abspath(__file__))
# In a clone the SDK is apps/sdk, two levels under the root; in a tarball there is no root.
ROOT = os.path.normpath(os.path.join(SDK, os.pardir, os.pardir))
IN_TREE = os.path.isfile(os.path.join(ROOT, "main", "app", "orb_app_wasm.c"))

CLANG = os.environ.get("ORB_CLANG", "clang")

# The page size is wasm's; the D27 ceiling is the firmware's ORB_WASM_MEM_MAX.
WASM_PAGE = 65536
WASM_MEM_MAX = 128 * 1024
STACK_BYTES = 8192

KIND_WASM = 2           # main/app/orb_app.h; kinds 1, 3 and 4 are retired
BAKED_MS = 0            # pinned: a container is content, not a build event

CFLAGS = [
    "--target=wasm32", "-mcpu=mvp",      # the feature set wasm3 runs, no proposals
    "-O2", "-ffreestanding", "-nostdlib", "-fno-exceptions",
    "-fvisibility=hidden", "-ffunction-sections", "-fdata-sections",
    "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
    "-I", SDK, "-isystem", os.path.join(SDK, "libc"),
]


def fail(msg):
    raise SystemExit("orbsdk: " + msg)


def _orbapp():
    """The container writer: beside this file in a tarball, tools/orbapp.py in a clone."""
    for p in (os.path.join(SDK, "orbapp.py"), os.path.join(ROOT, "tools", "orbapp.py")):
        if os.path.isfile(p):
            spec = importlib.util.spec_from_file_location("orbapp", p)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            return mod
    fail("no orbapp.py beside the SDK or in tools/")


orbapp = _orbapp()


# ---- the app directory ------------------------------------------------------------

MANIFEST_KEYS = {"slug", "title", "caps", "period_ms", "settings", "memory_kb",
                 "editor", "shelf", "os_min", "icon"}


def manifest(appdir):
    """app.json, checked. Unknown keys are refused so a typo is not a silent default."""
    path = os.path.join(appdir, "app.json")
    try:
        with open(path) as f:
            m = json.load(f)
    except OSError:
        fail("%s has no app.json" % appdir)
    except ValueError as e:
        fail("%s: %s" % (path, e))
    extra = set(m) - MANIFEST_KEYS
    if extra:
        fail("%s: unknown key(s) %s" % (path, ", ".join(sorted(extra))))
    for k in ("slug", "title"):
        if not isinstance(m.get(k), str) or not m[k]:
            fail("%s: %r is required" % (path, k))
    orbapp.check_slug(m["slug"])
    m.setdefault("caps", [])
    m.setdefault("settings", [])
    m.setdefault("period_ms", 0)
    m.setdefault("memory_kb", 64)
    # os_min: the oldest OS release the app runs on (owner, 2026-11-04: flights needs the
    # theme pool). It rides the editor region's stamp, where the hub reads it to refuse an
    # install on older firmware; the container has no field for it, and older readers
    # check the header's spare word zero.
    if "os_min" in m:
        if not isinstance(m["os_min"], str) or not re.match(r"^v\d{1,2}\.\d{1,2}\.\d{1,2}$", m["os_min"]):
            fail("%s: os_min is a release like v1.2.8, not %r" % (path, m["os_min"]))
        if not m.get("editor"):
            fail("%s: os_min rides the editor region, and this app ships no editor" % path)
    # §57: the settings are the app's own schema, which the dial and the hub both draw, so
    # each one names what they call it; orbapp.normalize_settings checks the rest
    for st in m["settings"]:
        if not isinstance(st, dict) or not isinstance(st.get("label"), str) or not st["label"]:
            fail("%s: setting %r has no label: the dial and the hub show it by that name"
                 % (path, st.get("id") if isinstance(st, dict) else st))
    try:
        orbapp.normalize_settings(m["settings"])
    except ValueError as e:
        fail("%s: %s" % (path, e))
    for c in m["caps"]:
        if c not in orbapp.CAP_OWNER:
            fail("%s: cap %r is not one of %s" % (path, c, ", ".join(orbapp.CAP_OWNER)))
    if m["memory_kb"] % 64 or not 64 <= m["memory_kb"] * 1024 <= WASM_MEM_MAX:
        fail("%s: memory_kb must be 64 or 128 (wasm pages are 64 KB; the OS caps an app "
             "at %d KB)" % (path, WASM_MEM_MAX // 1024))
    return m


def caps_word(m):
    w = 0
    for c in m["caps"]:
        w |= orbapp.cap_bit(c)
    return w


def version(appdir):
    """version.txt's stamp: v<MAJOR>.<MINOR>.<PATCH>, each part 1 or 2 digits."""
    path = os.path.join(appdir, "version.txt")
    try:
        v = open(path).read().strip()
    except OSError:
        fail("%s has no version.txt — every app carries its own release stamp" % appdir)
    if not orbapp.check_version(v):
        fail("%s holds %r, which is not a release stamp the format accepts "
             "(v<MAJOR>.<MINOR>.<PATCH>, each part 1 or 2 digits, no leading zero)" % (path, v))
    return v


def sources(appdir):
    own = sorted(os.path.join(appdir, f) for f in os.listdir(appdir) if f.endswith(".c"))
    if not own:
        fail("%s has no .c sources" % appdir)
    return own + [os.path.join(SDK, "orb_rt.c")]


# ---- the app's icon (§61) ------------------------------------------------------------
# app.json's "icon" names a square PNG in the app's directory. It is compiled into the
# module as a deflated RGB565 disc ICON_SIDE px across, the shape the OS draws (orb_disc.h:
# a row keeps the pixels whose centres lie within side/2 + 1 of the centre), and the app
# hands it over with ORB_ICON(). The PNG is read here, without Pillow: 8-bit RGB or RGBA,
# not interlaced, exactly ICON_SIDE square (tools/ui_icons/make.py writes HSB's).
ICON_SIDE = 232


def _png_rgb(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        fail("%s is not a PNG" % path)
    i, idat, w = 8, b"", None
    while i < len(data):
        n = int.from_bytes(data[i:i + 4], "big")
        kind, body = data[i + 4:i + 8], data[i + 8:i + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, inter = (int.from_bytes(body[0:4], "big"),
                                               int.from_bytes(body[4:8], "big"),
                                               body[8], body[9], body[10], body[11], body[12])
            if depth != 8 or ctype not in (2, 6) or inter:
                fail("%s: the icon must be 8-bit RGB or RGBA, not interlaced" % path)
        elif kind == b"IDAT":
            idat += body
        i += 12 + n
    if w is None:
        fail("%s has no header" % path)
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    out, prev, j = bytearray(), bytearray(stride), 0
    for _ in range(h):
        f, line = raw[j], bytearray(raw[j + 1:j + 1 + stride])
        j += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b, c = prev[x], prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return w, h, bpp, bytes(out)


def _disc_span(y, side):
    r = side // 2 + 1
    room = 4 * r * r - (2 * y + 1 - side) ** 2
    x = 0
    while x < side // 2 and (2 * x + 1 - side) ** 2 > room:
        x += 1
    return x, side - 1 - x


def icon_source(appdir, m, out_dir):
    """The generated C source carrying the app's icon, or None when app.json names none."""
    name = m.get("icon")
    if name is None:
        return None
    if not isinstance(name, str) or "/" in name or not name.endswith(".png"):
        fail("%s: icon names a .png in the app's own directory, not %r" % (appdir, name))
    w, h, bpp, px = _png_rgb(os.path.join(appdir, name))
    if w != ICON_SIDE or h != ICON_SIDE:
        fail("%s: the icon is %dx%d; it must be %d px square" % (name, w, h, ICON_SIDE))
    disc = bytearray()
    for y in range(h):
        x0, x1 = _disc_span(y, w)
        for x in range(x0, x1 + 1):
            o = (y * w + x) * bpp
            v = ((px[o] >> 3) << 11) | ((px[o + 1] >> 2) << 5) | (px[o + 2] >> 3)
            disc += bytes((v & 255, v >> 8))
    z = zlib.compress(bytes(disc), 9)
    rows = ",\n    ".join(", ".join("0x%02x" % b for b in z[i:i + 16]) for i in range(0, len(z), 16))
    path = os.path.join(out_dir, "orb_icon_gen.c")
    with open(path, "w") as f:
        f.write("/* generated by orbsdk from %s */\n"
                "const unsigned char orb_app_icon[%d] __attribute__((aligned(4))) = {\n    %s\n};\n"
                "const unsigned int orb_app_icon_n = %d, orb_app_icon_side = %d;\n"
                % (name, len(z), rows, len(z), w))
    return path


# ---- the OS surface ---------------------------------------------------------------

def binds():
    """name -> m3 signature: every orb.* import the OS answers (binds.json)."""
    with open(os.path.join(SDK, "binds.json")) as f:
        return json.load(f)["binds"]


def firmware_binds():
    """The same map read out of the shim's own s_binds table, in a clone only."""
    src = open(os.path.join(ROOT, "main", "app", "orb_app_wasm.c")).read()
    rows = re.findall(r'\{\s*"orb",\s*"(\w+)",\s*"([^"]+)"', src)
    if not rows:
        fail("no bind table found in main/app/orb_app_wasm.c")
    return dict(rows)


def binds_json(b):
    return json.dumps({"_": "the orb.* imports the OS binds; generated from "
                            "main/app/orb_app_wasm.c s_binds by orbsdk.py binds --write",
                       "binds": b}, indent=1, sort_keys=True) + "\n"


def check_tree():
    """In a clone: binds.json and clk_types.h must be the firmware's, byte for byte."""
    if not IN_TREE:
        return
    want = binds_json(firmware_binds())
    have = open(os.path.join(SDK, "binds.json")).read() if os.path.exists(
        os.path.join(SDK, "binds.json")) else ""
    if have != want:
        fail("apps/sdk/binds.json disagrees with main/app/orb_app_wasm.c s_binds — "
             "run apps/sdk/orbsdk.py binds --write")
    a = open(os.path.join(ROOT, "main", "art", "clk_types.h"), "rb").read()
    b = open(os.path.join(SDK, "clk_types.h"), "rb").read()
    if a != b:
        fail("apps/sdk/clk_types.h drifted from main/art/clk_types.h — copy it again "
             "(the dial stage memcpy's these records)")


# ---- wasm ---------------------------------------------------------------------------

def _leb(b, i):
    v = s = 0
    while True:
        c = b[i]; i += 1
        v |= (c & 0x7F) << s
        s += 7
        if not c & 0x80:
            return v, i


_VT = {0x7F: "i", 0x7E: "I", 0x7D: "f", 0x7C: "F"}


def imports(wasm):
    """(module, name, m3-signature) of every import the module declares, and its
    memory pages. The signature is spelled the way wasm3's m3_LinkRawFunction wants it
    and main/app/orb_app_wasm.c's bind table writes it: results, then params in parens."""
    if wasm[:8] != b"\0asm\1\0\0\0":
        fail("not a wasm module")
    i, out, pages, types = 8, [], None, []
    while i < len(wasm):
        sid = wasm[i]; i += 1
        size, i = _leb(wasm, i)
        body = wasm[i:i + size]
        if sid == 1:
            n, j = _leb(body, 0)
            for _ in range(n):
                if body[j] != 0x60:
                    fail("unexpected type form")
                j += 1
                np_, j = _leb(body, j); ps = body[j:j + np_]; j += np_
                nr, j = _leb(body, j); rs = body[j:j + nr]; j += nr
                res = "".join(_VT[b] for b in rs) or "v"
                types.append("%s(%s)" % (res, "".join(_VT[b] for b in ps)))
        if sid == 2:
            n, j = _leb(body, 0)
            for _ in range(n):
                ml, j = _leb(body, j); mod = body[j:j + ml].decode(); j += ml
                nl, j = _leb(body, j); name = body[j:j + nl].decode(); j += nl
                kind = body[j]; j += 1
                if kind == 0:
                    ti, j = _leb(body, j)
                else:
                    fail("import %s.%s is not a function" % (mod, name))
                out.append((mod, name, types[ti]))
        elif sid == 5:
            n, j = _leb(body, 0)
            j += 1                                   # limits flags
            pages, j = _leb(body, j)
        i += size
    return out, pages


def build(appdir, out_dir=None):
    """Compile one app. Answers (wasm bytes, import names, memory bytes)."""
    check_tree()
    m = manifest(appdir)
    pages = m["memory_kb"] * 1024 // WASM_PAGE
    out_dir = out_dir or os.path.join(appdir, "build")
    os.makedirs(out_dir, exist_ok=True)
    with tempfile.TemporaryDirectory() as td:
        tmp = os.path.join(td, m["slug"] + ".wasm")
        cmd = [CLANG] + CFLAGS + [
            "-Wl,--no-entry", "-Wl,--strip-all", "-Wl,--gc-sections",
            "-Wl,-z,stack-size=%d" % STACK_BYTES,
            "-Wl,--initial-memory=%d" % (pages * WASM_PAGE),
            "-Wl,--max-memory=%d" % (pages * WASM_PAGE),
            "-o", tmp] + sources(appdir) + [p for p in (icon_source(appdir, m, td),) if p]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True)
        except FileNotFoundError:
            fail("no %s on PATH — install clang and lld (wasm-ld), or set ORB_CLANG" % CLANG)
        if r.returncode:
            sys.stderr.write(r.stderr)
            fail("%s failed to compile" % m["slug"])
        wasm = open(tmp, "rb").read()
    imp, have = imports(wasm)
    foreign = ["%s.%s" % i[:2] for i in imp if i[0] != "orb"]
    if foreign:
        fail("%s imports outside the OS surface: %s — an app links against the SDK and "
             "nothing else" % (m["slug"], ", ".join(foreign)))
    b = binds()
    for _, name, sig in imp:
        if name not in b:
            fail("%s imports orb.%s, which the OS does not bind (binds.json)" % (m["slug"], name))
        if b[name] != sig:
            fail("%s imports orb.%s as %s; the OS binds it as %s — orb.h and binds.json "
                 "disagree" % (m["slug"], name, sig, b[name]))
    with open(os.path.join(out_dir, m["slug"] + ".wasm"), "wb") as f:
        f.write(wasm)
    return wasm, [n for _, n, _ in imp], have * WASM_PAGE


# ---- the editor region (flight-tracker-design §12.2) ---------------------------------
# An app whose app.json names a directory of its own as "editor" ships that directory as
# its editor region: the hub opens it from the Themes page. The recipe was the clock's
# (make_clock_region.py, retired with its dc page in E7 2d): a flat STORE ZIP, editor.json first and canonical, then
# the directory's top-level files and its schema fixtures in name order, the fixed date and
# mode, read back before it ships. Ruling 36 grew the list to carry
# `_fixtures/schema-<n>.orbdesign` (see fixture_members); every other subdirectory (an
# editor's _selftest/) and every dotfile stays home. One member
# is written here: orb-app.js, `window.__ORB_APP = {slug, version}`, the release the
# theme@1 session stamps into every design (the clock's region sets the same), plus its
# `schema` when editor.json names the design schema the editor reads (design §6.5 point 1,
# so the session can refuse to start against an editor.json that disagrees with it).
EDITOR_KEYS = {"baker", "editor", "entry", "host", "module", "runtime", "schema"}
RUNTIME_ID = re.compile(r"^[a-z][a-z0-9_-]*@[0-9][0-9.]*$")
ZIP_DATE = (1980, 1, 1, 0, 0, 0)


def editor_dir(appdir, m):
    """The app's editor directory when app.json names one, else None."""
    e = m.get("editor")
    if not isinstance(e, str) or not e or "/" in e or e.startswith("."):
        return None
    d = os.path.join(appdir, e)
    return d if os.path.isdir(d) else None


def editor_schema(ej, slug):
    """editor.json's `schema`: the design schema the editor reads (design §6.5 point 1).
    The int, or None when the editor names none and that is allowed — an editor with no
    design document may leave it out. A standalone (entry) editor listing theme@1 drives a
    session itself and must not; a hosted editor's page and session are Orb Manager's, its
    schema is checked against the running editor at start (theme@1 §6.5 point 1), so it may
    carry no design document and name none (radar)."""
    if "schema" in ej:
        schema = ej["schema"]
        if isinstance(schema, bool) or not isinstance(schema, int) or schema < 1:
            fail("editor.json's schema is the design schema the editor reads, a whole number from 1, not %r" % (schema,))
        return schema
    if "host" not in ej and "theme@1" in (ej.get("runtime") or []):
        fail("%s's editor lists theme@1, so its editor.json must name the design schema it reads (\"schema\": 1 for a first release)" % slug)
    return None


# The app keeps one sample design per past schema (design §6.5 point 4), in a folder of
# its editor's like the _selftest/ beside it. Ruling 36 makes them travel: every sample at
# or below the editor's own schema is packed into the editor region as
# `_fixtures/schema-<n>.orbdesign`, so a hub holding only the container can run the check,
# and the build refuses what check-migrations would refuse — one scan (fixture_schemas)
# answers both, so neither can drift from the other.
FIXTURES = "_fixtures"
FIXTURE_RE = re.compile(r"^schema-([1-9][0-9]{0,5})\.orbdesign$")   # the site's check-plan.ts grammar: no 0, no leading zero


def fixture_name(n):
    return "schema-%d.orbdesign" % n


def fixture_region_name(n):
    """The editor region member name a fixture rides as: its path under the editor directory,
    so an unpacked region holds the folder check-migrations reads (ruling 36)."""
    return "%s/%s" % (FIXTURES, fixture_name(n))


def fixture_schemas(slug, schema, fdir):
    """The schemas the app's _fixtures folder holds, ascending. A name that is not
    schema-<n>.orbdesign, and a fixture newer than the editor's own schema, are refused here:
    the folder is the app's promise of one design per past schema, so the build that packs it
    and the check that runs it refuse the same things (design §6.5 point 4, ruling 36)."""
    names = sorted(n for n in os.listdir(fdir) if os.path.isfile(os.path.join(fdir, n))) \
        if os.path.isdir(fdir) else []
    have = []
    for n in names:
        found = FIXTURE_RE.match(n)
        if not found:
            fail("%s: %s in _fixtures is not named schema-<n>.orbdesign" % (slug, n))
        if int(found.group(1)) > schema:
            fail("%s: %s is newer than the schema this editor reads (%d)" % (slug, n, schema))
        have.append(int(found.group(1)))
    return sorted(have)


def fixture_members(slug, schema, fdir):
    """The fixture members of an editor region: every sample the migration check would open,
    which is every schema the folder holds at or below the editor's own (ruling 36) — the
    past schemas the check migrates and an own-schema sample it opens as it is. An editor
    with no design schema drives no design and packs none; no folder is nothing."""
    if schema is None:
        return []
    return [fixture_region_name(k) for k in fixture_schemas(slug, schema, fdir)]


def tree_region_builder(key):
    """The clone's region-builder module an in-tree editor key names (the clock's), or None
    when the key names no builder (a tarball has no tools/, and answers None for all)."""
    if not isinstance(key, str) or not re.match(r"^[a-z0-9_]+$", key):
        return None
    p = os.path.join(ROOT, "tools", "make_%s_region.py" % key)
    if not os.path.isfile(p):
        return None
    spec = importlib.util.spec_from_file_location("make_%s_region" % key, p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def fixtures_dir(appdir, m):
    """Where the app keeps its schema fixtures: its editor's _fixtures, for an app whose
    app.json names an editor, else None."""
    d = editor_dir(appdir, m)
    if d:
        return os.path.join(d, FIXTURES)
    if tree_region_builder(m.get("editor")):
        return os.path.join(appdir, "editor", FIXTURES)      # a tree-built editor (the clock's)
    return None


def schema_fixture_rule(slug, schema, fdir):
    """An editor reading schema N >= 2 must hold a fixture for every past schema 1 … N-1
    (ruling 36): a design migrates from the schema before its own, so a two-hop-old design is
    proven to open only if its schema has a sample too. The first gap is named, and the same
    scan refuses a stray name or a fixture newer than the editor — what check-migrations
    would refuse, refused at build instead (design §6.5 point 4)."""
    if not schema or schema <= 1 or fdir is None:
        return
    have = fixture_schemas(slug, schema, fdir)
    for k in range(1, schema):
        if k in have:
            continue
        rel = os.path.relpath(os.path.join(fdir, fixture_name(k)), os.path.dirname(os.path.dirname(fdir)))
        fail("%s's editor reads design schema %d, so %s must keep a design of schema %d: the "
             "previous release's designs migrate from it, and check-migrations proves they still "
             "open. Save one with that release (orbsdk.py check-migrations --save-current, run "
             "before raising schema) and commit it" % (slug, schema, rel, k))


def editor_region(appdir, m, release):
    """The region bytes for the editor directory app.json names, for `release` (version.txt)."""
    import io
    import zipfile
    import zlib
    d = editor_dir(appdir, m)
    if not d:
        fail("%s names editor %r, which is no directory of the app" % (m["slug"], m.get("editor")))
    try:
        with open(os.path.join(d, "editor.json"), encoding="utf-8") as f:
            ej = json.load(f)
    except (OSError, ValueError) as e:
        fail("%s's editor has no readable editor.json (%s)" % (m["slug"], e))
    if not isinstance(ej, dict) or set(ej) - EDITOR_KEYS:
        fail("editor.json carries %s; it names entry or host and module, runtime, schema, editor and baker"
             % ", ".join(sorted(set(ej) - EDITOR_KEYS)) if isinstance(ej, dict) else "editor.json is not an object")
    runtime = ej.get("runtime", [])
    if not isinstance(runtime, list) or not all(isinstance(r, str) and RUNTIME_ID.match(r) for r in runtime):
        fail("editor.json's runtime lists ids like theme@1, not %r" % (runtime,))
    schema = editor_schema(ej, m["slug"])
    schema_fixture_rule(m["slug"], schema, fixtures_dir(appdir, m))
    files = sorted(n for n in os.listdir(d)
                   if not n.startswith(".") and n != "editor.json" and os.path.isfile(os.path.join(d, n)))
    if "host" in ej:
        # the hosted editor (theme-editor-kit-plan §8): Orb Manager's page, the app's module
        if ej["host"] != 1:
            fail("editor.json's host must be 1, the contract this SDK packs, not %r" % (ej["host"],))
        if "entry" in ej:
            fail("a hosted editor names no entry: Orb Manager serves its page")
        if "host@1" not in runtime:
            fail("a hosted editor lists host@1 in its runtime")
        mods = ej.get("module")
        if not isinstance(mods, list) or not mods or not all(
                isinstance(f, str) and "/" not in f and not f.startswith(".") and f.endswith(".js") for f in mods):
            fail("editor.json's module lists the editor's .js files, not %r" % (mods,))
        for f in mods:
            if f not in files:
                fail("editor.json's module %s is not in %s" % (f, d))
        if any(n.endswith((".html", ".htm")) for n in files):
            fail("a hosted editor ships no page of its own: %s" % ", ".join(n for n in files if n.endswith((".html", ".htm"))))
    else:
        if "module" in ej:
            fail("editor.json names a module only for a hosted editor (host: 1)")
        entry = ej.get("entry")
        if not isinstance(entry, str) or "/" in entry or not entry.endswith((".html", ".htm")):
            fail("editor.json's entry must be a plain .html file of the editor, not %r" % (entry,))
        if entry not in files:
            fail("editor.json's entry %s is not in %s" % (entry, d))
    if "orb-app.js" in files:
        fail("%s's editor carries orb-app.js, which the SDK writes" % m["slug"])
    # ruling 36: the fixtures ride too, keyed as `_fixtures/schema-<n>.orbdesign`. They join
    # the one sorted list below, where `_` (0x5F) sorts before the top-level files' letters.
    fixtures = fixture_members(m["slug"], schema, fixtures_dir(appdir, m))
    members = [("editor.json", json.dumps(ej, sort_keys=True, separators=(",", ":")).encode())]
    st = {"slug": m["slug"], "version": release}
    if schema is not None:
        st["schema"] = schema
    if m.get("os_min"):
        st["os_min"] = m["os_min"]
    stamp = ("window.__ORB_APP = %s;\n" % json.dumps(st, sort_keys=True)).encode()
    for n in sorted(files + ["orb-app.js"] + fixtures):
        if n == "orb-app.js":
            members.append((n, stamp))
            continue
        with open(os.path.join(d, n), "rb") as f:
            members.append((n, f.read()))
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        for name, data in members:
            info = zipfile.ZipInfo(name, date_time=ZIP_DATE)
            info.external_attr = 0o644 << 16
            zf.writestr(info, data, compress_type=zipfile.ZIP_STORED)
    region = buf.getvalue()
    zf = zipfile.ZipFile(io.BytesIO(region))
    for info, (name, data) in zip(zf.infolist(), members):
        if info.filename != name or info.compress_type != zipfile.ZIP_STORED or \
                zf.read(name) != data or zlib.crc32(data) & 0xFFFFFFFF != info.CRC:
            fail("%s's editor region does not read back at %s" % (m["slug"], name))
    if len(region) > orbapp.EDITOR_MAX:
        fail("%s's editor is %d B; a region holds %d" % (m["slug"], len(region), orbapp.EDITOR_MAX))
    return region


def container(appdir, editor=b""):
    """The app's .orbapp bytes, through the one container writer and its checks."""
    m = manifest(appdir)
    if not editor and editor_dir(appdir, m):
        editor = editor_region(appdir, m, version(appdir))
    if m.get("editor") and not editor:
        fail("%s declares an editor, which only tools/make_factory_apps.py builds "
             "(inside a gorb-os clone)" % m["slug"])
    code, _imp, mem = build(appdir)
    v = version(appdir)

    def write(stamp):
        with tempfile.TemporaryDirectory() as td:
            # write_container files a container under its own slug and checks the stem
            path = os.path.join(td, m["slug"] + ".orbapp")
            orbapp.write_container(
                path, m["slug"], m["title"], caps=caps_word(m),
                settings=tuple(m["settings"]), behaviour_kind=KIND_WASM,
                serial=orbapp.serial_of_version(v), period_ms=m["period_ms"],
                baked_ms=BAKED_MS, editor=editor, version=stamp, code=code, wasm_mem_max=mem)
            with open(path, "rb") as f:
                return f.read()
    # The stamp is version.txt plus the container's own content hash (v1.2.16+3f9a1c2),
    # the build tail the firmware's +<sha> fills: every set of bytes names itself, so a
    # rebuild between releases never wears another build's stamp, and version.txt moves
    # only for a release. The hash is of the container under the bare stamp, since it
    # cannot include itself; the serial stays version.txt's, so install order is unchanged.
    return write(v + "+" + hashlib.sha256(write(v)).hexdigest()[:7])


# ---- the orb ------------------------------------------------------------------------

def _post(host, path, body):
    url = "http://%s%s" % (host, path)
    req = urllib.request.Request(url, data=body, method="POST")
    if body:
        req.add_header("Content-Type", "application/octet-stream")
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except (urllib.error.URLError, OSError) as e:
        fail("could not reach %s: %s" % (host, e))


def install(blob, host):
    """Upload to the orb's card, then queue the card-to-slot install (it reboots)."""
    slug = orbapp.parse_header(blob)["slug"]
    st, txt = _post(host, "/api/v1/sd/app?overwrite=1", blob)
    if st != 200:
        fail("%s refused the upload (%d): %s" % (host, st, txt.strip()))
    st, txt = _post(host, "/api/v1/apps/install?id=" + urllib.parse.quote(slug), b"")
    if st != 200:
        fail("%s refused the install (%d): %s" % (host, st, txt.strip()))
    return slug


# ---- the site -----------------------------------------------------------------------

RELEASE_PATH = "/api/dev/release"       # the site's CI door (orb-os-net design.md §6.2)
RELEASE_STATUSES = ("pending", "live", "skipped", "duplicate")
LOCAL_HOSTS = ("localhost", "127.0.0.1")    # the only hosts a plain --site may speak http to


def site_url(site):
    """The release URL for a --site value: https://<host>/api/dev/release.

    The value is a host or host:port; a literal scheme in front of it is the caller
    repeating what the command already decided, so it is stripped and honoured, with one
    exception: http on anything but a localhost name is refused the way a missing token
    is, because the token rides in the request and an http POST would hand a developer
    token to whoever is listening. A bare localhost name may be http — that is how the
    host suite stands a stub server in for the site."""
    v = (site or "").strip()
    low = v.lower()
    if low.startswith("http://"):
        v, given = v[7:], "http"
    elif low.startswith("https://"):
        v, given = v[8:], "https"
    else:
        given = None
    host = v.split("/")[0].strip()
    if given == "http" and host.split(":")[0].lower() not in LOCAL_HOSTS:
        print("publish: --site must be https")
        raise SystemExit(2)
    scheme = given or ("http" if host.split(":")[0].lower() in LOCAL_HOSTS else "https")
    return "%s://%s%s" % (scheme, host, RELEASE_PATH)


def site_post(url, token, blob):
    """One POST of the raw container to the site, the token from the environment.

    The token is never an argument — argv lands in CI logs and in `ps` — and is never
    printed here; a refusal quotes the site's own sentence or a bare HTTP status. It goes
    as an unredirected header, so a 3xx answer can move the request but not the token."""
    req = urllib.request.Request(url, data=blob, method="POST")
    req.add_header("Content-Type", "application/octet-stream")
    req.add_unredirected_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:      # the site answered: its body decides the line
        return e.code, e.read()
    except (urllib.error.URLError, OSError, ValueError) as e:
        print("publish: could not reach %s: %s" % (url, getattr(e, "reason", None) or e))
        raise SystemExit(1)


# ---- commands -----------------------------------------------------------------------

def sdk_version():
    """The OS release this SDK describes: VERSION in a tarball, the clone's version.txt."""
    for p in (os.path.join(SDK, "VERSION"), os.path.join(ROOT, "version.txt")):
        if os.path.isfile(p):
            return open(p).read().strip()
    return "unknown"


def cmd_new(a):
    dst = os.path.abspath(a.dir)
    if os.path.exists(dst) and os.listdir(dst):
        fail("%s exists and is not empty" % dst)
    slug = a.slug or re.sub(r"[^a-z0-9_]", "_", os.path.basename(dst).lower()).strip("_")
    orbapp.check_slug(slug)
    shutil.copytree(os.path.join(SDK, "template"), dst, dirs_exist_ok=True)
    for root, _dirs, files in os.walk(dst):
        for f in files:
            p = os.path.join(root, f)
            with open(p) as fh:
                t = fh.read()
            t = (t.replace("@SLUG@", slug).replace("@TITLE@", a.title or slug.title())
                  .replace("@SDK_VERSION@", sdk_version()))
            with open(p, "w") as fh:
                fh.write(t)
    print("new app %s in %s — next: orbsdk.py pack %s" % (slug, dst, a.dir))


def cmd_build(a):
    for d in a.appdir or ["."]:
        m = manifest(d)
        # the region first: a schema/fixture refusal needs no clang, and the editor
        # checks outrank the compile (container keeps the same order for pack and install)
        if editor_dir(d, m):
            editor_region(d, m, version(d))
        wasm, imp, mem = build(d)
        print("%-14s %7d B code, %3d KB memory, %2d imports: %s"
              % (manifest(d)["slug"], len(wasm), mem // 1024, len(imp), " ".join(imp)))


def cmd_pack(a):
    d = a.appdir or "."
    blob = container(d)
    out = a.out or os.path.join(d, "build")
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, manifest(d)["slug"] + ".orbapp")
    with open(path, "wb") as f:
        f.write(blob)
    print("wrote %s (%d B, %s)" % (path, len(blob), version(d)))


def cmd_install(a):
    if os.path.isdir(a.target):
        blob = container(a.target)
    else:
        blob = open(a.target, "rb").read()
    slug = install(blob, a.orb)
    print("installed %s on %s; the orb reboots to load it" % (slug, a.orb))


def cmd_publish(a):
    """Post a built .orbapp to an orb-os.net site's CI door with the developer token."""
    url = site_url(a.site)
    token = os.environ.get("ORB_SITE_TOKEN")
    if not token and not a.dry_run:
        print("publish: ORB_SITE_TOKEN is not set")
        raise SystemExit(2)
    try:
        with open(a.file, "rb") as f:
            blob = f.read()
    except OSError:
        print("publish: cannot read %s" % a.file)
        raise SystemExit(2)
    if a.dry_run:
        print("publish: would POST %s (%d B) to %s" % (a.file, len(blob), url))
        return
    # the reader is only here to name the app on the line: a file the reader refuses is
    # still the site's to judge, so the site's 400 sentence is what prints (ruling 15)
    try:
        hdr = orbapp.parse_header(blob)
    except ValueError:
        hdr = {}
    st, body = site_post(url, token, blob)
    try:
        got = json.loads(body.decode("utf-8", "replace"))
    except ValueError:
        got = None
    if 200 <= st < 300:
        status = got.get("status") if isinstance(got, dict) else None
        if status not in RELEASE_STATUSES:
            print("publish: HTTP %d" % st)
            raise SystemExit(1)
        ver = got.get("version")
        if not isinstance(ver, str) or not ver:
            ver = hdr.get("version") or ""      # a duplicate answer names no version
        slug = hdr.get("slug") or os.path.splitext(os.path.basename(a.file))[0]
        label = "already published" if status == "duplicate" else status
        print("publish: %s -> %s" % (" ".join(w for w in (slug, ver) if w), label))
        return
    err = got.get("error") if isinstance(got, dict) else None
    print("publish: " + (err if isinstance(err, str) and err else "HTTP %d" % st))
    raise SystemExit(1)


def cmd_inspect(a):
    sys.exit(subprocess.call([sys.executable, orbapp.__file__, "--inspect", a.file]))


def cmd_binds(a):
    if not IN_TREE:
        fail("binds runs inside a gorb-os clone, where main/ holds the bind table")
    text = binds_json(firmware_binds())
    path = os.path.join(SDK, "binds.json")
    if a.write:
        with open(path, "w") as f:
            f.write(text)
        print("wrote %s" % path)
        return
    have = open(path).read() if os.path.exists(path) else ""
    if have != text:
        fail("binds.json is stale — run orbsdk.py binds --write")
    print("binds.json matches the firmware (%d imports)" % len(firmware_binds()))


# ---- check-migrations (design §6.5 point 4) ------------------------------------------

def _fixtures_to_run(slug, schema, fdir):
    """The fixture names to run, in schema order. A stray file, a fixture newer than the
    editor, or a past schema without one of its own is refused here, before anything
    reaches node: the folder is the app's promise of one design per past schema. The scan
    of what the folder holds is fixture_schemas, which the build shares (ruling 36)."""
    have = fixture_schemas(slug, schema, fdir)
    for k in range(1, schema):
        if k not in have:
            fail("%s: no fixture for schema %d: check-migrations keeps one design per past schema"
                 % (slug, k))
    return [fixture_name(k) for k in have]


def _run_migrate(a, node, runtime, editor_tmp, extra):
    cmd = [node, os.path.join(runtime, "_runner", "migrate.js"),
           "--runtime", runtime, "--editor", editor_tmp]
    if a.chrome:
        cmd += ["--chrome", a.chrome]
    cmd += list(extra)
    return subprocess.run(cmd).returncode


def cmd_check_migrations(a):
    """Open each app's schema fixtures in its packed editor, through the migration runner.
    Answers 0 only when every app passed; an app with no editor, or an editor with no
    design schema, has nothing to migrate. The SDK's own directory is skipped: it is the
    generic SDK for building new apps, not an app, and `apps/*/` catches it. Every other
    directory without app.json still fails, so a typo is never silently skipped."""
    node = os.environ.get("ORB_NODE") or "node"
    runtime = a.runtime or (os.path.join(ROOT, "tools", "orb-manager", "web", "runtime")
                            if IN_TREE else os.path.join(SDK, "runtime"))
    for d in a.appdir or ["."]:
        if os.path.realpath(d) == os.path.realpath(SDK):
            print("%s: the SDK itself, not an app, skipped" % d)
            continue
        m = manifest(d)
        slug = m["slug"]
        if editor_dir(d, m):
            region = editor_region(d, m, version(d))
        else:
            builder = tree_region_builder(m.get("editor"))
            if builder:
                region = builder.make_region()
            elif m.get("editor"):
                fail("%s declares an editor, which only tools/make_factory_apps.py builds "
                     "(inside a gorb-os clone)" % slug)
            else:
                print("%s: no editor, nothing to migrate" % slug)
                continue
        import io
        import zipfile
        with tempfile.TemporaryDirectory() as td:
            ed = os.path.join(td, "editor")
            os.makedirs(ed)
            zipfile.ZipFile(io.BytesIO(region)).extractall(ed)
            with open(os.path.join(ed, "editor.json"), encoding="utf-8") as f:
                ej = json.load(f)
            schema = editor_schema(ej, slug)
            if schema is None:
                print("%s: its editor names no design schema, nothing to migrate" % slug)
                continue
            fdir = fixtures_dir(d, m)
            if a.save_current:
                target = os.path.join(fdir, fixture_name(schema))
                if os.path.exists(target):
                    fail("%s: %s exists: a fixture is the design that release wrote, keep it"
                         % (slug, os.path.relpath(target, d)))
                os.makedirs(fdir, exist_ok=True)
                if _run_migrate(a, node, runtime, ed, ["--save-current", target]):
                    fail("%s: the packed editor gave up no current design: see the runner "
                         "output above" % slug)
                continue
            fixtures = _fixtures_to_run(slug, schema, fdir)
            if not fixtures:
                print("%s: schema 1 and no fixtures, nothing to migrate yet" % slug)
                continue
            if not shutil.which(node):
                fail("check-migrations runs the editor in Node and Chromium: install Node 18+ "
                     "and Chromium, or set ORB_NODE")
            if not os.path.isfile(os.path.join(runtime, "_runner", "migrate.js")):
                fail("no migration runner at %s: pass --runtime an editor runtime" % runtime)
            if _run_migrate(a, node, runtime, ed, [os.path.join(fdir, n) for n in fixtures]):
                fail("%s: a past design no longer opens in this editor" % slug)
    return 0


# What an SDK tarball carries, relative to this directory (plus orbapp.py, VERSION,
# README.md, LICENSE and the editor runtime as runtime/, from the clone). Everything
# else here is the clone's own.
DIST_FILES = ("orbsdk.py", "binds.json", "orb.h", "orb_stage.h", "orb_dial.h", "clk_types.h", "orb_rt.c")
DIST_DIRS = ("libc", "template")


def cmd_dist(a):
    import gzip
    import io
    import tarfile
    if not IN_TREE:
        fail("dist runs inside a gorb-os clone")
    check_tree()
    v = sdk_version()
    top = "orb-sdk-" + v
    members = [(f, os.path.join(SDK, f)) for f in DIST_FILES]
    for d in DIST_DIRS:
        for root, dirs, files in os.walk(os.path.join(SDK, d)):
            dirs[:] = sorted(x for x in dirs if x not in ("build", "__pycache__"))
            for f in sorted(files):
                full = os.path.join(root, f)
                members.append((os.path.relpath(full, SDK), full))
    members += [("orbapp.py", os.path.join(ROOT, "tools", "orbapp.py")),
                ("README.md", os.path.join(ROOT, "apps", "README.md")),
                ("LICENSE", os.path.join(ROOT, "LICENSE"))]
    # check-migrations runs its runner from the tarball (design §6.5 point 4), so the editor
    # runtime rides as runtime/. The _selftest lane stays home, as it does from the hub's
    # embed: it is the clone's own, and the runner is not a runtime id.
    rt = os.path.join(ROOT, "tools", "orb-manager", "web", "runtime")
    if not os.path.isdir(rt):
        fail("dist needs tools/orb-manager/web/runtime to carry the migration runner")
    for root_, dirs, files_ in os.walk(rt):
        dirs[:] = sorted(x for x in dirs if x not in ("_selftest", "__pycache__"))
        for f_ in sorted(files_):
            full = os.path.join(root_, f_)
            members.append((os.path.join("runtime", os.path.relpath(full, rt)), full))
    # Deterministic: sorted names, fixed owner and mtime, no gzip timestamp.
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        def add(name, data, mode):
            ti = tarfile.TarInfo(top + "/" + name)
            ti.size, ti.mode, ti.mtime, ti.uname, ti.gname = len(data), mode, 0, "", ""
            tf.addfile(ti, io.BytesIO(data))
        add("VERSION", (v + "\n").encode(), 0o644)
        for name, full in sorted(members):
            with open(full, "rb") as f:
                add(name.replace(os.sep, "/"), f.read(),
                    0o755 if name in ("orbsdk.py", "orbapp.py") else 0o644)
    out = a.out or os.path.join(ROOT, "build")
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, top + ".tar.gz")
    with open(path, "wb") as f:
        with gzip.GzipFile(fileobj=f, mode="wb", mtime=0, filename="") as gz:
            gz.write(raw.getvalue())
    print("wrote %s (%d files, %d KB)"
          % (path, len(members) + 1, os.path.getsize(path) // 1024))


def main(argv=None):
    p = argparse.ArgumentParser(prog="orbsdk.py", description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("new", help="scaffold an app")
    s.add_argument("dir"); s.add_argument("--slug"); s.add_argument("--title")
    s.set_defaults(fn=cmd_new)
    s = sub.add_parser("build", help="compile to <appdir>/build/<slug>.wasm")
    s.add_argument("appdir", nargs="*"); s.set_defaults(fn=cmd_build)
    s = sub.add_parser("pack", help="build and write <slug>.orbapp")
    s.add_argument("appdir", nargs="?"); s.add_argument("-o", "--out")
    s.set_defaults(fn=cmd_pack)
    s = sub.add_parser("install", help="pack, upload to an orb, install")
    s.add_argument("target"); s.add_argument("--orb", required=True, help="the orb's IP or name")
    s.set_defaults(fn=cmd_install)
    s = sub.add_parser("publish", help="publish a built .orbapp to an orb-os.net site")
    s.add_argument("file"); s.add_argument("--site", required=True,
                                           help="the site's host (or host:port); https, "
                                                "except a localhost test site")
    s.add_argument("--dry-run", action="store_true",
                   help="print what would be sent and stop: no token, no request")
    s.set_defaults(fn=cmd_publish)
    s = sub.add_parser("inspect", help="read a container back")
    s.add_argument("file"); s.set_defaults(fn=cmd_inspect)
    s = sub.add_parser("binds", help="check binds.json against the firmware (clone only)")
    s.add_argument("--write", action="store_true"); s.set_defaults(fn=cmd_binds)
    s = sub.add_parser("dist", help="write orb-sdk-<version>.tar.gz (clone only)")
    s.add_argument("-o", "--out"); s.set_defaults(fn=cmd_dist)
    s = sub.add_parser("check-migrations",
                       help="open each app's schema fixtures in its packed editor")
    s.add_argument("appdir", nargs="*")
    s.add_argument("--runtime", help="the editor runtime holding _runner/ (default: this "
                                     "clone's, or the tarball's own runtime/)")
    s.add_argument("--chrome", help="the Chromium to open the editors with (else $CHROME, "
                                    "else a name on PATH)")
    s.add_argument("--save-current", action="store_true",
                   help="write the packed editor's own design as this schema's fixture "
                        "instead of checking")
    s.set_defaults(fn=cmd_check_migrations)
    a = p.parse_args(argv)
    a.fn(a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
