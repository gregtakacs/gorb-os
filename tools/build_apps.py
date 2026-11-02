#!/usr/bin/env python3
"""build_apps.py — compile every app under apps/ into the wasm module its .orbapp carries.

An app is a directory apps/<slug>/ of C sources. It links against apps/sdk/ and nothing
else: no libc, no OS header, no LVGL. The OS's whole surface is the "orb" import module
declared in apps/sdk/orb.h, so this tool refuses a module that imports anything else —
an app that compiled against a symbol the OS does not bind would fail at load on the
dial, and the build is the cheaper place to learn it.

    python3 tools/build_apps.py            # build all, print sizes
    python3 tools/build_apps.py clock      # build one

make_factory_apps.py calls build() and packs the bytes into the container's code region.
The output is deterministic: no timestamps, no paths in the module (--strip-all), so a
rebuild of unchanged sources is byte-identical and the freshness check stays honest.
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, os.pardir))
APPS = os.path.join(ROOT, "apps")
SDK = os.path.join(APPS, "sdk")
OUT = os.path.join(ROOT, "build", "apps")

CLANG = os.environ.get("ORB_CLANG", "clang")

# The page size is wasm's; the D27 ceiling is the firmware's ORB_WASM_MEM_MAX.
WASM_PAGE = 65536
STACK_BYTES = 8192

CFLAGS = [
    "--target=wasm32", "-mcpu=mvp",      # the feature set wasm3 runs, no proposals
    "-O2", "-ffreestanding", "-nostdlib", "-fno-exceptions",
    "-fvisibility=hidden", "-ffunction-sections", "-fdata-sections",
    "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
    "-I", SDK, "-isystem", os.path.join(SDK, "libc"),
]


def ldflags(pages):
    return [
        "-Wl,--no-entry", "-Wl,--strip-all", "-Wl,--gc-sections",
        "-Wl,-z,stack-size=%d" % STACK_BYTES,
        "-Wl,--initial-memory=%d" % (pages * WASM_PAGE),
        "-Wl,--max-memory=%d" % (pages * WASM_PAGE),
    ]


def app_slugs():
    return sorted(d for d in os.listdir(APPS)
                  if d != "sdk" and os.path.isdir(os.path.join(APPS, d)))


def sources(slug):
    d = os.path.join(APPS, slug)
    own = sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".c"))
    if not own:
        raise SystemExit("build_apps: apps/%s has no .c sources" % slug)
    return own + [os.path.join(SDK, "orb_rt.c")]


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
        raise SystemExit("build_apps: not a wasm module")
    i, out, pages, types = 8, [], None, []
    while i < len(wasm):
        sid = wasm[i]; i += 1
        size, i = _leb(wasm, i)
        body = wasm[i:i + size]
        if sid == 1:
            n, j = _leb(body, 0)
            for _ in range(n):
                if body[j] != 0x60:
                    raise SystemExit("build_apps: unexpected type form")
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
                    raise SystemExit("build_apps: import %s.%s is not a function" % (mod, name))
                out.append((mod, name, types[ti]))
        elif sid == 5:
            n, j = _leb(body, 0)
            flags = body[j]; j += 1
            pages, j = _leb(body, j)
        i += size
    return out, pages


def os_binds():
    """name -> signature, read out of the shim's own s_binds table: the one list of
    what the OS answers. An app that imports past it would fail to load on the dial."""
    import re
    src = open(os.path.join(ROOT, "main", "app", "orb_app_wasm.c")).read()
    rows = re.findall(r'\{\s*"orb",\s*"(\w+)",\s*"([^"]+)"', src)
    if not rows:
        raise SystemExit("build_apps: no bind table found in main/app/orb_app_wasm.c")
    return dict(rows)


def check_sdk_mirrors():
    """apps/sdk/clk_types.h is a byte copy of main/art/clk_types.h: the dial stage hands
    an app its theme's records verbatim, so the two layouts must be one text."""
    a = open(os.path.join(ROOT, "main", "art", "clk_types.h"), "rb").read()
    b = open(os.path.join(SDK, "clk_types.h"), "rb").read()
    if a != b:
        raise SystemExit("build_apps: apps/sdk/clk_types.h drifted from main/art/clk_types.h — "
                         "copy it again (the dial stage memcpy's these records)")


def build(slug, pages=1):
    """Compile apps/<slug> and answer the module bytes. Refuses a foreign import."""
    check_sdk_mirrors()
    os.makedirs(OUT, exist_ok=True)
    dst = os.path.join(OUT, slug + ".wasm")
    with tempfile.TemporaryDirectory() as td:
        tmp = os.path.join(td, slug + ".wasm")
        cmd = [CLANG] + CFLAGS + ldflags(pages) + ["-o", tmp] + sources(slug)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            sys.stderr.write(r.stderr)
            raise SystemExit("build_apps: %s failed to compile" % slug)
        wasm = open(tmp, "rb").read()
    imp, have = imports(wasm)
    foreign = ["%s.%s" % m[:2] for m in imp if m[0] != "orb"]
    if foreign:
        raise SystemExit("build_apps: %s imports outside the OS surface: %s — an app links "
                         "against apps/sdk and nothing else" % (slug, ", ".join(foreign)))
    binds = os_binds()
    for _, name, sig in imp:
        if name not in binds:
            raise SystemExit("build_apps: %s imports orb.%s, which the OS does not bind "
                             "(main/app/orb_app_wasm.c s_binds)" % (slug, name))
        if binds[name] != sig:
            raise SystemExit("build_apps: %s imports orb.%s as %s; the OS binds it as %s — "
                             "apps/sdk/orb.h and the bind table disagree" % (slug, name, sig, binds[name]))
    with open(dst, "wb") as f:
        f.write(wasm)
    return wasm, [n for _, n, _ in imp], have * WASM_PAGE


def main():
    want = sys.argv[1:] or app_slugs()
    for slug in want:
        wasm, imp, mem = build(slug)
        print("%-14s %7d B code, %3d KB memory, %2d imports: %s"
              % (slug, len(wasm), mem // 1024, len(imp), " ".join(imp)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
