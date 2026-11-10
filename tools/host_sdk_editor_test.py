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
            ej if ej is not None else {"runtime": ["theme@1", "react@18.3.1"], "entry": "index.html", "editor": 1, "schema": 1}))
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
    r = sdk.editor_region(d, m, "v9.9.9")
    zf = zipfile.ZipFile(io.BytesIO(r))
    names = [i.filename for i in zf.infolist()]
    ck(names == ["editor.json"] + sorted(list(many) + ["orb-app.js"]),
       "members: editor.json first, then the files in name order (%s)" % names)
    ck(all(i.compress_type == zipfile.ZIP_STORED for i in zf.infolist()), "every member is STORE")
    ck(all(i.date_time == (1980, 1, 1, 0, 0, 0) for i in zf.infolist()), "every member carries the fixed date")
    ck(all("/" not in n for n in names), "names are flat, as the hub serves them")
    ck(zf.read("editor.json") == b'{"editor":1,"entry":"index.html","runtime":["theme@1","react@18.3.1"],"schema":1}',
       "editor.json is canonical: sorted keys, no spaces (%r)" % zf.read("editor.json"))
    ck(zf.read("a.js") == b"a()", "a member's bytes are the file's")
    ck(zf.read("orb-app.js") == b'window.__ORB_APP = {"schema": 1, "slug": "radar", "version": "v9.9.9"};\n',
       "orb-app.js names the app, its release and the schema the editor reads (%r)" % zf.read("orb-app.js"))
    ck(sdk.editor_region(d, m, "v9.9.9") == r, "a rebuild is byte-identical")
    ck("_selftest" not in "".join(names) and ".DS_Store" not in names, "subdirectories and dotfiles stay home")
    # the clock's key names no directory of the app: the SDK leaves it to make_factory_apps
    ck(sdk.editor_dir(d, {"slug": "clock", "editor": "clock"}) is None, "a key that names no directory is not a region")
    ck(sdk.editor_dir(d, {"slug": "radar"}) is None, "no editor key, no region")
    ck(sdk.editor_dir(d, {"slug": "radar", "editor": ".."}) is None, "an editor path out of the app is not one")
    # refusals
    d2, m2 = app({"index.html": b"x"}, ej={"entry": "index.html", "runtime": [], "colour": 1})
    ck(refused(lambda: sdk.editor_region(d2, m2, "v9.9.9"), "carries colour"), "an unknown editor.json key is refused")
    d3, m3 = app({"index.html": b"x"}, ej={"entry": "sub/index.html", "runtime": []})
    ck(refused(lambda: sdk.editor_region(d3, m3, "v9.9.9"), "plain .html"), "an entry in a subdirectory is refused")
    d4, m4 = app({"index.html": b"x"}, ej={"entry": "main.html", "runtime": []})
    ck(refused(lambda: sdk.editor_region(d4, m4, "v9.9.9"), "is not in"), "an entry the directory lacks is refused")
    d5, m5 = app({"index.html": b"x"}, ej={"entry": "index.html", "runtime": ["theme"]})
    ck(refused(lambda: sdk.editor_region(d5, m5, "v9.9.9"), "runtime lists"), "a runtime id with no version is refused")
    d6, m6 = app({"index.html": b"x", "big.bin": b"\0" * (sdk.orbapp.EDITOR_MAX + 1)})
    ck(refused(lambda: sdk.editor_region(d6, m6, "v9.9.9"), "a region holds"), "a region past the cap is refused")
    d7, m7 = app({"index.html": b"x", "orb-app.js": b"mine"})
    ck(refused(lambda: sdk.editor_region(d7, m7, "v9.9.9"), "which the SDK writes"), "an editor's own orb-app.js is refused")
    # the hosted editor (theme-editor-kit-plan §8): Orb Manager's page, the app's module
    hj = {"editor": 1, "host": 1, "module": ["b.js", "a.js"], "runtime": ["host@1", "theme@1"]}
    dh, mh = app({"a.js": b"a()", "b.js": b"b()"}, ej=hj)
    zh = zipfile.ZipFile(io.BytesIO(sdk.editor_region(dh, mh, "v9.9.9")))
    ck([i.filename for i in zh.infolist()] == ["editor.json", "a.js", "b.js", "orb-app.js"], "a hosted editor packs its module and the stamp")
    ck(json.loads(zh.read("editor.json"))["module"] == ["b.js", "a.js"], "and keeps the module's load order")
    ck(b'"schema"' not in zh.read("orb-app.js"),
       "a hosted editor with no design document stamps no schema (radar)")
    for why, ej, files, needle in (
            ("a newer contract", dict(hj, host=2), None, "host must be 1"),
            ("an entry beside host", dict(hj, entry="index.html"), {"a.js": b"", "b.js": b"", "index.html": b""}, "names no entry"),
            ("no host@1", dict(hj, runtime=["theme@1"]), None, "lists host@1"),
            ("no module", {"host": 1, "runtime": ["host@1"]}, None, "module lists"),
            ("a module path", dict(hj, module=["sub/a.js"]), None, "module lists"),
            ("a missing module", dict(hj, module=["c.js"]), None, "module c.js is not in"),
            ("a page of its own", hj, {"a.js": b"", "b.js": b"", "index.html": b""}, "ships no page of its own"),
            ("a module without host", {"entry": "index.html", "module": ["a.js"], "runtime": []}, {"a.js": b"", "index.html": b""}, "only for a hosted editor")):
        dx, mx = app(files or {"a.js": b"", "b.js": b""}, ej=ej)
        ck(refused(lambda: sdk.editor_region(dx, mx, "v9.9.9"), needle), "a hosted editor with %s is refused" % why)
    # the design schema editor.json names (design §6.5 point 1): a whole number from 1
    for bad in (0, -1, 1.5, "2", True):
        db, mb = app({"index.html": b"x"}, ej={"entry": "index.html", "runtime": ["theme@1"], "schema": bad})
        ck(refused(lambda: sdk.editor_region(db, mb, "v9.9.9"), "design schema"),
           "a schema of %r is refused: the editor's schema is a whole number from 1" % (bad,))
    dn, mn = app({"index.html": b"x"}, ej={"entry": "index.html", "runtime": ["theme@1"]})
    ck(refused(lambda: sdk.editor_region(dn, mn, "v9.9.9"), "must name the design schema"),
       "a standalone editor listing theme@1 with no schema is refused: its session could not check itself")
    dr, mr = app({"index.html": b"x"}, ej={"entry": "index.html", "runtime": ["ui@1"]})
    rr = zipfile.ZipFile(io.BytesIO(sdk.editor_region(dr, mr, "v9.9.9")))
    ck(b'"schema"' not in rr.read("orb-app.js"),
       "an editor with no design document and no schema stamps none (%r)" % rr.read("orb-app.js"))
    d9, m9 = app({"index.html": b"x"}, ej={"entry": "index.html", "runtime": ["theme@1"], "schema": 3})
    os.makedirs(os.path.join(d9, "editor", "_fixtures"))  # the fixture rule: schema 3 keeps schema 2
    open(os.path.join(d9, "editor", "_fixtures", "schema-2.orbdesign"), "w").write("{}\n")
    r3 = zipfile.ZipFile(io.BytesIO(sdk.editor_region(d9, m9, "v9.9.9")))
    ck(r3.read("orb-app.js") == b'window.__ORB_APP = {"schema": 3, "slug": "radar", "version": "v9.9.9"};\n'
       and r3.read("editor.json") == b'{"entry":"index.html","runtime":["theme@1"],"schema":3}',
       "a schema rides both the canonical editor.json and the stamp (%r, %r)" % (r3.read("orb-app.js"), r3.read("editor.json")))
    ck(sdk.editor_schema({"runtime": ["theme@1"], "schema": 3}, "tint") == 3
       and sdk.editor_schema({"runtime": ["ui@1"]}, "tint") is None
       and sdk.editor_schema({"host": 1, "runtime": ["theme@1"]}, "radar") is None,
       "editor_schema answers the schema, or None when the editor may name none")

    # the fixture rule (design §6.5 point 4): schema N > 1 keeps a fixture of schema N-1
    def with_fixtures(d, names):
        fdir = os.path.join(d, "editor", sdk.FIXTURES)
        os.makedirs(fdir)
        for n in names:
            open(os.path.join(fdir, n), "w").write("{}\n")
        return d

    def schema_app(schema, fixture_names=()):
        d, m = app({"index.html": b"x"}, ej={"entry": "index.html", "runtime": ["theme@1"], "schema": schema})
        if fixture_names:
            with_fixtures(d, fixture_names)
        return d, m

    d10, m10 = schema_app(2)
    ck(refused(lambda: sdk.editor_region(d10, m10, "v9.9.9"), "must keep a design of schema 1"),
       "schema 2 with no _fixtures is refused: the previous release's designs would strand")
    ck(not os.path.exists(os.path.join(d10, "editor", sdk.FIXTURES)),
       "the refusal creates no folder")
    d11, m11 = schema_app(2, ["schema-1.orbdesign"])
    r11 = zipfile.ZipFile(io.BytesIO(sdk.editor_region(d11, m11, "v9.9.9")))
    ck(r11.namelist() and not any("_fixtures" in n for n in r11.namelist()),
       "schema 2 with the fixture of schema 1 packs, and _fixtures stays out of the region")
    d12, m12 = app({"index.html": b"x"})                        # the helper's ej is schema 1
    ck(sdk.editor_region(d12, m12, "v9.9.9") != b"", "schema 1 needs no fixture")
    d13, m13 = schema_app(3, ["schema-1.orbdesign"])
    ck(refused(lambda: sdk.editor_region(d13, m13, "v9.9.9"), "schema-2.orbdesign"),
       "schema 3 holding only schema-1 is refused naming schema-2, the schema it must keep")

    def disk(d, m, editor=True):                                # the same app, on disk for main()
        json.dump({"slug": m["slug"], "title": "Radar", "editor": "editor"} if editor
                  else {"slug": m["slug"], "title": "Radar"}, open(os.path.join(d, "app.json"), "w"))
        open(os.path.join(d, "version.txt"), "w").write("v1.0.0\n")
        return d

    sdk.CLANG = "/nonexistent/clang"                             # a refusal before this one needs no clang
    d14, m14 = schema_app(2)
    msg = ""
    try:
        sdk.main(["build", disk(d14, m14)])
    except SystemExit as e:
        msg = str(e)
    ck("must keep a design of schema 1" in msg and "clang" not in msg,
       "build checks the editor before compiling: the fixture refusal, not the clang one (%s)" % msg)

    import contextlib

    def say(argv):                                              # what check-migrations prints and refuses
        out = io.StringIO()
        why = ""
        with contextlib.redirect_stdout(out):
            try:
                rc = sdk.main(argv)
            except SystemExit as e:
                rc, why = 1, str(e)
        return out.getvalue(), rc, why

    dp = tempfile.mkdtemp(prefix="sdkapp.")
    json.dump({"slug": "plain", "title": "Plain"}, open(os.path.join(dp, "app.json"), "w"))
    txt, rc, why = say(["check-migrations", dp])
    ck(rc == 0 and "plain: no editor, nothing to migrate" in txt,
       "check-migrations on an app with no editor says so and passes (%r)" % txt)
    de, me = app({"index.html": b"x"}, ej={"entry": "index.html", "runtime": ["ui@1"]})
    txt, rc, why = say(["check-migrations", disk(de, me)])
    ck(rc == 0 and "names no design schema" in txt,
       "an editor with no theme@1 and no schema has nothing to migrate (%r)" % txt)
    d5, m5 = schema_app(2, ["schema-1.orbdesign", "schema-5.orbdesign"])
    txt, rc, why = say(["check-migrations", disk(d5, m5)])
    ck(rc == 1 and "newer than" in why, "a fixture newer than the schema is refused (%s)" % why)
    d3, m3 = schema_app(3, ["schema-2.orbdesign"])
    txt, rc, why = say(["check-migrations", disk(d3, m3)])
    ck(rc == 1 and "no fixture for schema 1" in why,
       "schema 3 missing schema-1 fails the promise of one design per past schema (%s)" % why)
    d1, m1 = app({"index.html": b"x"})
    txt, rc, why = say(["check-migrations", disk(with_fixtures(d1, ["notes.txt"]), m1)])
    ck(rc == 1 and "not named schema-<n>.orbdesign" in why, "a stray in _fixtures is refused (%s)" % why)
    return fails


MUTANTS = (
    ("editor.json not first", 'members = [("editor.json", json.dumps(ej, sort_keys=True, separators=(",", ":")).encode())]',
     'members = []'),
    ("files unsorted", 'for n in sorted(files + ["orb-app.js"]):', 'for n in ["orb-app.js"] + list(reversed(files)):'),
    ("no canonical json", 'json.dumps(ej, sort_keys=True, separators=(",", ":"))', 'json.dumps(ej)'),
    ("deflated members", "zf.writestr(info, data, compress_type=zipfile.ZIP_STORED)", "zf.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)"),
    ("today's date", "info = zipfile.ZipInfo(name, date_time=ZIP_DATE)", "info = zipfile.ZipInfo(name, date_time=(2026, 11, 4, 12, 0, 0))"),
    ("subdirectories ride", "and os.path.isfile(os.path.join(d, n)))", ")"),
    ("unknown keys pass", "if not isinstance(ej, dict) or set(ej) - EDITOR_KEYS:", "if not isinstance(ej, dict):"),
    ("no entry check", "if entry not in files:", "if 0:"),
    ("no cap", "if len(region) > orbapp.EDITOR_MAX:", "if 0:"),
    ("no release stamp", 'st = {"slug": m["slug"], "version": release}', 'st = {"slug": m["slug"]}'),
    ("hosted: any contract", 'if ej["host"] != 1:', 'if 0:'),
    ("hosted: no host@1 check", 'if "host@1" not in runtime:', 'if 0:'),
    ("hosted: module unchecked", '            if f not in files:', '            if 0:'),
    ("hosted: a page rides", 'if any(n.endswith((".html", ".htm")) for n in files):', 'if 0:'),
    ("schema type unchecked", "if isinstance(schema, bool) or not isinstance(schema, int) or schema < 1:", "if 0:"),
    ("theme@1 without schema passes", 'if "host" not in ej and "theme@1" in (ej.get("runtime") or []):', "if 0:"),
    ("an escaping editor path", 'if not isinstance(e, str) or not e or "/" in e or e.startswith("."):',
     'if not isinstance(e, str) or not e:'),
    ("no fixture rule", "schema_fixture_rule(m[\"slug\"], schema, fixtures_dir(appdir, m))", "pass"),
    ("build compiles first", "if editor_dir(d, m):\n            editor_region(d, m, version(d))",
     "if 0:\n            editor_region(d, m, version(d))"),
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
