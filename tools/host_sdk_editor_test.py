#!/usr/bin/env python3
"""Host suite for apps/sdk/orbsdk.py's editor region (flight-tracker-design §12.2, E1).

    python3 tools/host_sdk_editor_test.py [--mutations]

An app whose app.json names a directory as "editor" ships it as a STORE ZIP: editor.json
first and canonical, the directory's top-level files in name order, the fixed date, read
back. The checks build regions from scratch app directories and read them the way the hub
does (tools/orb-manager/editor.go: flat names, editor.json's entry and runtime ids).
--mutations breaks each guarded line in a private copy of orbsdk.py and expects a check
to fail.
"""
import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.path.join(ROOT, "apps", "sdk", "orbsdk.py")


def load(src_text):
    d = tempfile.mkdtemp(prefix="sdked.")
    p = os.path.join(d, "orbsdk.py")
    open(p, "w").write(src_text)
    # the SDK finds orbapp.py beside itself, else in the clone: hand it the clone's
    shutil.copy(os.path.join(ROOT, "tools", "orbapp.py"), os.path.join(d, "orbapp.py"))
    spec = importlib.util.spec_from_file_location("orbsdk_t", p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def app(files, editor="editor", ej=None):
    """A scratch app: an editor directory holding `files` and an editor.json."""
    d = tempfile.mkdtemp(prefix="sdkapp.")
    if editor:
        e = os.path.join(d, "editor")
        os.makedirs(os.path.join(e, "_selftest"))
        open(os.path.join(e, "_selftest", "t.js"), "w").write("// stays home")
        open(os.path.join(e, ".DS_Store"), "w").write("x")
        for n, b in files.items():
            open(os.path.join(e, n), "wb").write(b)
        open(os.path.join(e, "editor.json"), "w").write(json.dumps(
            ej if ej is not None else {"runtime": ["theme@1", "react@18.3.1"], "entry": "index.html", "editor": 1}))
    return d, {"slug": "radar", "editor": editor}


def run(sdk):
    fails = []

    def ck(ok, what):
        if not ok:
            fails.append(what)

    def refused(fn, needle):
        try:
            fn()
        except SystemExit as e:
            return needle in str(e)
        return False

    many = {"%s.js" % c: c.encode() for c in "qzsxwrvtuy"}       # scrambled: listdir's order is not name order
    many.update({"index.html": b"<!doctype html><p>hi</p>", "b.js": b"b()", "a.js": b"a()"})
    d, m = app(many)
    r = sdk.editor_region(d, m)
    zf = zipfile.ZipFile(io.BytesIO(r))
    names = [i.filename for i in zf.infolist()]
    ck(names == ["editor.json"] + sorted(many),
       "members: editor.json first, then the files in name order (%s)" % names)
    ck(all(i.compress_type == zipfile.ZIP_STORED for i in zf.infolist()), "every member is STORE")
    ck(all(i.date_time == (1980, 1, 1, 0, 0, 0) for i in zf.infolist()), "every member carries the fixed date")
    ck(all("/" not in n for n in names), "names are flat, as the hub serves them")
    ck(zf.read("editor.json") == b'{"editor":1,"entry":"index.html","runtime":["theme@1","react@18.3.1"]}',
       "editor.json is canonical: sorted keys, no spaces (%r)" % zf.read("editor.json"))
    ck(zf.read("a.js") == b"a()", "a member's bytes are the file's")
    ck(sdk.editor_region(d, m) == r, "a rebuild is byte-identical")
    ck("_selftest" not in "".join(names) and ".DS_Store" not in names, "subdirectories and dotfiles stay home")
    # the clock's key names no directory of the app: the SDK leaves it to make_factory_apps
    ck(sdk.editor_dir(d, {"slug": "clock", "editor": "clock"}) is None, "a key that names no directory is not a region")
    ck(sdk.editor_dir(d, {"slug": "radar"}) is None, "no editor key, no region")
    ck(sdk.editor_dir(d, {"slug": "radar", "editor": ".."}) is None, "an editor path out of the app is not one")
    # refusals
    d2, m2 = app({"index.html": b"x"}, ej={"entry": "index.html", "runtime": [], "colour": 1})
    ck(refused(lambda: sdk.editor_region(d2, m2), "carries colour"), "an unknown editor.json key is refused")
    d3, m3 = app({"index.html": b"x"}, ej={"entry": "sub/index.html", "runtime": []})
    ck(refused(lambda: sdk.editor_region(d3, m3), "plain .html"), "an entry in a subdirectory is refused")
    d4, m4 = app({"index.html": b"x"}, ej={"entry": "main.html", "runtime": []})
    ck(refused(lambda: sdk.editor_region(d4, m4), "is not in"), "an entry the directory lacks is refused")
    d5, m5 = app({"index.html": b"x"}, ej={"entry": "index.html", "runtime": ["theme"]})
    ck(refused(lambda: sdk.editor_region(d5, m5), "runtime lists"), "a runtime id with no version is refused")
    d6, m6 = app({"index.html": b"x", "big.bin": b"\0" * (sdk.orbapp.EDITOR_MAX + 1)})
    ck(refused(lambda: sdk.editor_region(d6, m6), "a region holds"), "a region past the cap is refused")
    return fails


MUTANTS = (
    ("editor.json not first", 'members = [("editor.json", json.dumps(ej, sort_keys=True, separators=(",", ":")).encode())]',
     'members = []'),
    ("files unsorted", "files = sorted(n for n in os.listdir(d)", "files = list(n for n in os.listdir(d)"),
    ("no canonical json", 'json.dumps(ej, sort_keys=True, separators=(",", ":"))', 'json.dumps(ej)'),
    ("deflated members", "zf.writestr(info, data, compress_type=zipfile.ZIP_STORED)", "zf.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)"),
    ("today's date", "info = zipfile.ZipInfo(name, date_time=ZIP_DATE)", "info = zipfile.ZipInfo(name, date_time=(2026, 11, 4, 12, 0, 0))"),
    ("subdirectories ride", "and os.path.isfile(os.path.join(d, n)))", ")"),
    ("unknown keys pass", "if not isinstance(ej, dict) or set(ej) - EDITOR_KEYS:", "if not isinstance(ej, dict):"),
    ("no entry check", "if entry not in files:", "if 0:"),
    ("no cap", "if len(region) > orbapp.EDITOR_MAX:", "if 0:"),
    ("an escaping editor path", 'if not isinstance(e, str) or not e or "/" in e or e.startswith("."):',
     'if not isinstance(e, str) or not e:'),
)


def main():
    text = open(SDK).read()
    fails = run(load(text))
    for f in fails:
        print("FAIL " + f)
    print("%s: %d failures" % ("sdk editor region", len(fails)))
    bad = bool(fails)
    if "--mutations" in sys.argv:
        caught = 0
        for name, a, b in MUTANTS:
            if text.count(a) != 1:
                print("FAIL mutant anchor %r found %d times" % (name, text.count(a)))
                bad = True
                continue
            try:
                got = run(load(text.replace(a, b)))
            except (SystemExit, Exception) as e:     # a crash is a check that failed
                got = ["crashed: %s" % e]
            if got:
                caught += 1
            else:
                print("FAIL uncaught mutant: " + name)
                bad = True
        print("mutants: %d of %d caught" % (caught, len(MUTANTS)))
    print("ALL PASS" if not bad else "FAILED")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
