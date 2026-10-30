#!/usr/bin/env python3
"""host_clock_region_test.py — D37 §S4: the clock's editor region contract, host lane.

S4 moves the clock's editor into the clock's container. The bytes are derived
from tracked composer sources (the tracked files themselves are pinned elsewhere
and must not move), and this suite is what proves the DERIVED bytes are the
right ones. Text pins of the D35 idiom: exact tag rewrites, exact member set,
and the shipped bridge vocabulary — every one a sentence that says what silently
wrong bake looks like.

The suite reads the builder's module directly, so it works whether the baked
container lives in the hub's seed tree (pre-S5) or in a starter artifact dir
(post-S5): the region's contract does not depend on where its container waits.
"""
import importlib.util
import io
import json
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
FAILS = []


def check(label, cond, detail=""):
    if cond:
        print("ok   %s" % label)
    else:
        print("FAIL %s%s" % (label, (" — " + detail) if detail else ""))
        FAILS.append(label)


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


mcr = load_module("make_clock_region", os.path.join(HERE, "make_clock_region.py"))
region = mcr.make_region()
zf = zipfile.ZipFile(io.BytesIO(region))
names = [i.filename for i in zf.infolist()]
entry = zf.read("index.html").decode("utf-8")

# 1. The member set is exactly the four files, manifest first. A fifth member
#    would be an editor file the region does not declare; a missing engine or
#    renderer is a dead iframe that loads no dial.
check("members are exactly the four, editor.json first",
      names == ["editor.json", "index.html", "watch-engine.js", "dial-renderer.js"], str(names))
check("no member is DEFLATEd (the hub streams without unpacking)",
      all(i.compress_type == zipfile.ZIP_STORED for i in zf.infolist()))
check("region fits the firmware's 2 MiB cap", len(region) <= 2 * 1024 * 1024,
      "%d B" % len(region))

# 2. The manifest names what the hub's loader reads (editor.go's rawEntryName
#    and nothing else): entry must be a plain relative html file.
man = json.loads(zf.read("editor.json").decode("utf-8"))
check("editor.json: dialect 1, entry index.html",
      man.get("editor") == 1 and man.get("entry") == "index.html")
check("editor.json: baker 3 (the hub's writer is FORMAT 3 only)", man.get("baker") == 3)
# The names the hub serves today: react/babel from the vendor tree, support.js
# and orbtheme-baker.js as composer siblings. An unlisted name is a runtime the
# hub does not have — and §5.1's promise is that the hub refuses it, so the
# list here must stay inside what the hub actually serves.

# 3. The two hub-runtime rewrites landed and the app's own stay relative. These
#    are the exact strings the bake anchors on; a source drift is already a
#    SystemExit inside the builder, so a FAIL here means the rewrite itself
#    changed shape, not that the source moved.
check("support.js resolves to the hub's shared runtime",
      '<script src="/composer/support.js"></script>' in entry
      and 'src="./support.js"' not in entry)
check("orbtheme-baker.js resolves to the hub's shared writer",
      '<script src="/composer/orbtheme-baker.js"></script>' in entry
      and '<script src="orbtheme-baker.js">' not in entry)
check("the region's own members stay relative",
      '<script src="watch-engine.js"></script>' in entry
      and '<script src="dial-renderer.js"></script>' in entry)

# 4. The fonts links stay the page's own CDN references: serve-time offline
#    rewrite is the hub's job (injectOffline), and baking them in would freeze
#    one hub's vendor state into a card file.
check("google font links untouched (serve-time is the hub's)",
      entry.count("fonts.googleapis.com") == 11, str(entry.count("fonts.googleapis.com")))

# 5. The bridge shim: present once, speaks the shipped vocabulary (the one
#    app.js's themeBridge implements — NOT §7.1's stale table), frames-only,
#    and answers the awaited fetch with the pane's verdict.
def s_in_body(text):
    """The shim sits inside <body>: after the <body> tag and before </body>."""
    b0, b1 = text.find("<body>"), text.rfind("</body>")
    at = text.find("<script>\n(function ()")
    return b0 != -1 and b1 != -1 and b0 < at < b1


shim = re.search(r"<script>\n\(function \(\)[\s\S]*?</script>", entry)
check("shim is one script block before </body>",
      shim is not None and entry.count("theme.save") == 1
      and entry.rstrip().endswith("</html>") and s_in_body(entry),
      "shim found: %s, theme.save count: %d" % (shim is not None, entry.count("theme.save")))
if shim:
    s = shim.group(0)
    for tok in ('type: "theme.ready"', 'type: "theme.save"', 'm.type !== "theme.status"',
                'window.parent === window', '!Q.has("app")',
                r"/\/api\/theme\/upload/"):
        check("shim speaks %s" % tok, tok in s)
    check("shim targets the hub origin, not '*'",
          "location.origin)" in s and "'*'" not in s and '"*"' not in s)
    check("shim checks ev.source === window.parent", "ev.source !== window.parent" in s)
    check("shim leaves the composer's other fetches alone",
          'init.method !== "POST"' in s and "real(input, init)" in s)
    # The two legs of the upload door, pinned separately because they are different
    # asks. ?precheck=1 is a question about the card and the pane's bridge answers only
    # one thing (upload with overwrite=1, report "stored"), so a shim that intercepted
    # the question would file a truncated container as a theme; it goes to the hub's own
    # door, which the frame may dial. On 2026-10-29 the shim intercepted everything and
    # every FIRST Send press inside the Themes pane died on the head slice.
    check("shim passes the precheck leg to the hub, not the pane",
          "precheck=1" in s and "init.method !== \"POST\"\n        || /precheck=1/.test(url)" in s)
    # A save body is a BodyInit. The composer's own legs hand fetch a Uint8Array, and
    # ArrayBuffer.isView is what catches it: an ArrayBuffer has no .byteOffset, so the
    # old two-case chain refused the byte array a real save arrives as.
    check("shim reads a byte-array save body",
          "ArrayBuffer.isView(body)" in s and "body.byteOffset" in s)
    check("shim still refuses a body with no bytes in it",
          'new Error("bridge: unexpected upload body")' in s)
    # Injected JS that does not parse is a composer with no bridge, and the author finds
    # out by pressing Send. esprima is this lane's parser — host_sd_test.py parses the
    # firmware's /themes script with it — and a missing parser falls back to the hub's
    # jscheck posture (strip comments and strings, require the grouping to balance) while
    # saying out loud that the real parse did not run.
    def js_code(text):
        out, i, n = [], 0, len(text)
        while i < n:
            ch = text[i]
            if ch == "/" and text[i + 1:i + 2] == "/":
                while i < n and text[i] != "\n":
                    i += 1
                continue
            if ch == '"':
                i += 1
                while i < n and text[i] != '"':
                    if text[i] == "\\":
                        i += 1
                    i += 1
                i += 1
                continue
            out.append(ch)
            i += 1
        return "".join(out)

    def balanced(code):
        return all(code.count(a) == code.count(b) for a, b in ("{}", "()", "[]"))

    code = js_code(s)
    check("the shim balance check can fail",
          not balanced(js_code('function a(x) { if (x) { return 1; }'))
          and balanced(js_code('function a(x) { return (x || "}{"); }')),
          "a checker that cannot fail proves nothing")
    js_body = s.split("<script>", 1)[1].rsplit("</script>", 1)[0]
    try:
        import esprima
    except ImportError:
        esprima = None
    if esprima is None:
        check("shim's grouping balances (esprima absent, jscheck posture)",
              balanced(code) and code.count("function") >= 4,
              " ".join("%s:%d/%d" % (p, code.count(p[0]), code.count(p[1]))
                       for p in ("{}", "()", "[]")))
        print("SKIP esprima is not installed — the shim was balanced, not parsed. "
              "Run: pip install esprima")
    else:
        try:
            esprima.parseScript(js_body)
            check("shim parses as one script (esprima)", True)
        except Exception as e:
            check("shim parses as one script (esprima)", False, str(e))
        # The parse check has to be able to fail, or it reports a syntax the browser
        # would refuse. Two wounds in the legs this shim reads a save body through: the
        # ArrayBuffer branch loses its opening brace, and its call loses a paren.
        for wound, whole, hurt in (
                ("dropped brace", "} else if (body instanceof ArrayBuffer) {",
                 "} else if (body instanceof ArrayBuffer)"),
                ("stray paren", "done(body);", "done(body;")):
            assert whole in js_body, wound
            broken = js_body.replace(whole, hurt)
            try:
                esprima.parseScript(broken)
                refused = False
            except Exception:
                refused = True
            check("the esprima check can fail on a %s" % wound, refused,
                  "%s still parsed, so the parse check proves nothing" % wound)
        check("shim's grouping balances",
              balanced(code) and code.count("function") >= 4,
              " ".join("%s:%d/%d" % (p, code.count(p[0]), code.count(p[1]))
                       for p in ("{}", "()", "[]")))

# 6. The tracked composer is NOT what got baked — the region's entry is derived.
#    If this ever matches byte-exact, the bake stopped doing its one job.
with open(os.path.join(mcr.COMPOSER_DIR, mcr.COMPOSER_ENTRY), encoding="utf-8") as f:
    tracked = f.read()
check("baked entry differs from the tracked source", entry != tracked,
      "the bake became a passthrough")

# 7. Determinism, the lane's standing rule: two builds equal, and a CLI --verify
#    run out-of-process re-says it.
check("two in-process builds are identical", region == mcr.make_region())
r = subprocess.run([sys.executable, os.path.join(HERE, "make_clock_region.py"), "--verify"],
                   capture_output=True, text=True)
check("--verify exits 0 out of process", r.returncode == 0, r.stderr.strip()[-120:])

print("\n== CLOCK REGION SUITE: %d failure(s)" % len(FAILS))
sys.exit(1 if FAILS else 0)

