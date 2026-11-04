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
    orbsdk.py inspect <file.orbapp>              read a container back
    orbsdk.py binds [--write]                    in a gorb-os clone: check (or rewrite)
                                                 binds.json against the firmware
    orbsdk.py dist [-o DIR]                      in a gorb-os clone: write the SDK tarball

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
                 "editor", "shelf"}


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
            "-o", tmp] + sources(appdir)
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


def container(appdir, editor=b""):
    """The app's .orbapp bytes, through the one container writer and its checks."""
    m = manifest(appdir)
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


# What an SDK tarball carries, relative to this directory (plus orbapp.py, VERSION,
# README.md and LICENSE from the clone). Everything else here is the clone's own.
DIST_FILES = ("orbsdk.py", "binds.json", "orb.h", "orb_stage.h", "clk_types.h", "orb_rt.c")
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
    print("wrote %s (%d files)" % (path, len(members) + 1))


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
    s = sub.add_parser("inspect", help="read a container back")
    s.add_argument("file"); s.set_defaults(fn=cmd_inspect)
    s = sub.add_parser("binds", help="check binds.json against the firmware (clone only)")
    s.add_argument("--write", action="store_true"); s.set_defaults(fn=cmd_binds)
    s = sub.add_parser("dist", help="write orb-sdk-<version>.tar.gz (clone only)")
    s.add_argument("-o", "--out"); s.set_defaults(fn=cmd_dist)
    a = p.parse_args(argv)
    a.fn(a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
