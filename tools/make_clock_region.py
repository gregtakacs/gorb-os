#!/usr/bin/env python3
"""make_clock_region.py — the clock's editor region, built for its container (D37 §S4).

S4 moves the clock's editor from a hub-hosted alias into the clock's own
`.orbapp`. The bytes that move are the ones the plan sized (10): the three
files the clock owns — the composer document, the watch engine, the dial
renderer — about 652 KB STOREd, inside the 2 MiB cap the firmware enforces.

The entry document is a DERIVED copy of the tracked composer
(apps/clock/editor/), never an edit of it. It is pointed at the hub's OS theme
runtime (D43 §9), whose ids the region's editor.json declares and the hub checks:

    src="./support.js"          ->  src="/runtime/support@1/support.js"
    src="orbtheme-baker.js"     ->  src="/runtime/baker@1/orbtheme-baker.js"
    the CDN React/ReactDOM/Babel ->  window.__resources, set ahead of support.js,
                                     onto /runtime/react@18.3.1/ and babel@7.29.0/
    the Google Fonts sheets      ->  /runtime/fonts@1/fonts.css

so the dc-runtime, the theme builder and the libraries ride once, in the hub, for
every app, and the region never carries them. `dial-renderer.js` and `watch-engine.js`
stay relative: they are members of this region beside this document, and the
hub serves them from the ZIP at the same URL depth.

Then one app-owned script lands before </body>: the bridge shim. The composer
inside the Themes pane talks to the pane over postMessage (theme.ready /
theme.save / theme.status — the vocabulary app.js's themeBridge actually
implements), and the shim translates the composer's own POST to
/api/theme/upload into that conversation, answering the awaited fetch with the
pane's verdict. Outside an editor frame the shim defines nothing and the
document behaves exactly as the standalone page does.

editor.json names the entry and the resources; `runtime` declares what the hub
must serve for this editor to load (the hub injects its offline map today
without validating the list — the follow-up is pinned in the plan).

Determinism: STORE, the fixed 1980 date, crc32 of content, members in the fixed
order below, composer sources read verbatim — a rebuild of unchanged sources
re-emits identical bytes, which is what `--verify` demands and what
make_factory_apps.py's --check inherits. The ZIP layout is shared with the
format fixtures by importing make_app_fixture's writer, so one STOREd layout
spans both lanes.
"""
import importlib.util
import io
import json
import re
import os
import sys
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

# The clock app's editor sources (D43 §9.3): the app owns its document and engines;
# the runtime they build against is the hub's (tools/orb-manager/web/runtime/).
COMPOSER_DIR = os.path.join(HERE, "..", "apps", "clock", "editor")
COMPOSER_ENTRY = "Watch Composer.dc.html"

# Members in region order. editor.json first: it is the one file §5.1 requires,
# and a reader should learn the entry before it streams 600 KB. Names are plain
# (the container spec forbids '/'), so the region lives flat.
MEMBER_MANIFEST = "editor.json"
MEMBER_ENTRY = "index.html"
MEMBER_ENGINE = "watch-engine.js"
MEMBER_RENDERER = "dial-renderer.js"
# D39: the clock's default sounds ship as this generator, never as WAVs
# (docs/history/sound-plan.md §6). App-owned like the engine, so it is a member, relative.
MEMBER_SOUND = "sound-engine.js"
# D43 P4: the layer stack and its rules, app-owned like the engine.
MEMBER_STACK = "stack.js"

# The OS runtime ids this editor resolves against on the hub (D43 §9.1). The hub
# serves each at /runtime/<id>/ and refuses an editor naming one it does not carry.
RUNTIME = ["support@1", "baker@1", "ui@1", "theme@1", "react@18.3.1", "babel@7.29.0", "fonts@1"]
# The exact CDN URLs support.js compares against (its cdnScriptFor), and where the
# hub's runtime serves each. support.js consults window.__resources first.
CDN_LOCAL = (
    ("https://unpkg.com/react@18.3.1/umd/react.production.min.js",
     "/runtime/react@18.3.1/react.production.min.js"),
    ("https://unpkg.com/react-dom@18.3.1/umd/react-dom.production.min.js",
     "/runtime/react@18.3.1/react-dom.production.min.js"),
    ("https://unpkg.com/@babel/standalone@7.29.0/babel.min.js",
     "/runtime/babel@7.29.0/babel.min.js"),
)
FONTS_LOCAL = "/runtime/fonts@1/fonts.css"


# The pane bridge, app-owned and bake-injected. It is active only when this
# document runs inside the hub's Themes editor frame; standalone, the page
# behaves exactly as the file:// composer does. The composer already dials the
# hub's /api/theme/upload door from inside the frame (same origin, cookie held
# — §7.1's proof), and the pane's bridge is the contract the pane's status line
# and theme refresh answer, so this translates one door into the other. The
# composer's own send path is untouched: it lives inside the DC class, and the
# upload door is the seam the composer reaches the hub over anyway. A hub
# refusal (duplicate name, torn store) rides back as the rejection the
# composer already renders in its own status line.
#
# Two legs reach that door and only one of them is a save. ?precheck=1 asks
# whether the card already holds this identity, and the pane's half of the
# bridge (app.js's themeBridge) can answer exactly one thing: upload the bytes
# with overwrite=1 and report "stored". Forwarding a question as an answer would
# store a truncated container as a theme, so the question goes to the hub's own
# door — the frame holds the cookie it needs — and the shim intercepts saves.
# On 2026-10-29 this distinction did not exist, and the interception ate the
# precheck on every first Send press: see the body cases below.
BRIDGE_SHIM = r"""
<script>
(function () {
  var Q = new URLSearchParams(location.search);
  if (window.parent === window || !Q.has("app")) return;
  var app = Q.get("app"), pending = null;
  function toB64(buf) {
    var u8 = new Uint8Array(buf), s = "", CH = 0x8000;
    for (var i = 0; i < u8.length; i += CH)
      s += String.fromCharCode.apply(null, u8.subarray(i, i + CH));
    return btoa(s);
  }
  var real = window.fetch ? window.fetch.bind(window) : null;
  window.fetch = function (input, init) {
    var url = typeof input === "string" ? input : (input && input.url) || "";
    // Three requests go straight to the hub's own door, which this frame may dial:
    // anything that is not the upload door, anything that is not a save (a ?precheck=1
    // leg asks a question the pane cannot answer — see the header comment), and a POST
    // the shim cannot read. Intercepting the question is what made every first Send
    // press fail: the pane answered the head slice with a stored theme id and the
    // composer's next leg never happened.
    if (!real || !/\/api\/theme\/upload/.test(url) || !init || init.method !== "POST"
        || /precheck=1/.test(url))
      return real ? real(input, init) : Promise.reject(new Error("no fetch"));
    var body = init.body;
    return new Promise(function (resolve, reject) {
      pending = { resolve: resolve, reject: reject };
      var fail = function (e) { pending = null; reject(e); };
      var done = function (buf) {
        try {
          window.parent.postMessage({ type: "theme.save", app: app,
            container: toB64(buf) }, location.origin);
        } catch (e) { fail(e); }
      };
      // A save body is any BodyInit, and the pane's contract wants the container's
      // bytes whichever shape they arrived in. Blob and ArrayBuffer are the two this
      // shim was written for; the view and the Response cases are what an editor that
      // hands fetch a Uint8Array — the ordinary shape for raw bytes, and the one the
      // composer's own legs use — reaches on its first save. Before 2026-10-29 fell
      // through to the refusal below, which is why a byte array looked like a body the
      // bridge could not read.
      if (typeof Blob !== "undefined" && body instanceof Blob) {
        var r = new FileReader();
        r.onload = function () { done(r.result); };
        r.onerror = function () { fail(r.error || new Error("bridge read failed")); };
        r.readAsArrayBuffer(body);
      } else if (body instanceof ArrayBuffer) {
        done(body);
      } else if (typeof ArrayBuffer.isView === "function" && ArrayBuffer.isView(body)) {
        done(body.buffer.slice(body.byteOffset, body.byteOffset + body.byteLength));
      } else if (typeof Response !== "undefined" && body != null) {
        new Response(body).arrayBuffer().then(done, fail);
      } else {
        fail(new Error("bridge: unexpected upload body"));
      }
    });
  };
  window.addEventListener("message", function (ev) {
    var m = ev.data;
    if (!m || m.type !== "theme.status" || ev.source !== window.parent) return;
    var p = pending; pending = null;
    if (!p) return;
    if (m.state === "error") { p.reject(new Error(m.message || "the hub refused the theme")); return; }
    p.resolve(new Response(JSON.stringify({ ok: true, data: { id: m.themeId || "" } }),
      { status: 200, headers: { "Content-Type": "application/json" } }));
  });
  window.parent.postMessage({ type: "theme.ready", app: app }, location.origin);
})();
</script>
"""


def _app_version():
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir,
                           "apps", "clock", "version.txt")) as f:
        return f.read().strip()


def _manifest():
    return json.dumps({"editor": 1, "entry": MEMBER_ENTRY, "runtime": RUNTIME,
                       "baker": 3},
                      separators=(",", ":"), sort_keys=True).encode("utf-8")


def _rewrite(text):
    """The bake-time transform of the tracked composer into the region entry.

    Every rewrite is anchored on the exact shipped tag text, so a source drift
    raises here instead of baking a half-moved editor: the runtime tags go to their
    versioned /runtime/ paths with the CDN map ahead of support.js, the Google Fonts
    sheets collapse onto the vendored one, and the shim lands before the document's
    last closing tag. The engine/renderer/sound tags stay relative, or the ZIP's own
    members stop resolving.
    """
    resources = "<script>window.__resources=window.__resources||{};%s</script>" % "".join(
        "window.__resources[%s]=%s;" % (json.dumps(u), json.dumps(l)) for u, l in CDN_LOCAL)
    # D43 P7: the editor names the app and the release it belongs to in every design it
    # writes into a theme, so a design knows which editor made it (apps/clock/version.txt)
    resources += "<script>window.__ORB_APP=%s;</script>" % json.dumps(
        {"slug": "clock", "version": _app_version()}, sort_keys=True)
    pairs = (
        ('<script src="./support.js"></script>',
         resources + '\n<script src="/runtime/support@1/support.js"></script>'),
        ('<script src="orbtheme-baker.js"></script>',
         '<script src="/runtime/baker@1/orbtheme-baker.js"></script>'),
        # theme-editor-kit-plan K1: the send is an OS service
        ('<script src="send.js"></script>',
         '<script src="/runtime/theme@1/send.js"></script>'),
        ('<script src="session.js"></script>',
         '<script src="/runtime/theme@1/session.js"></script>'),
        # D43 §4.2: the hub's look, the runtime's ui@1 sheet
        ('<link href="ui.css" rel="stylesheet">',
         '<link href="/runtime/ui@1/ui.css" rel="stylesheet">'),
    )
    for old, new in pairs:
        n = text.count(old)
        if n != 1:
            raise SystemExit("make_clock_region: the composer source no longer carries "
                             "%r exactly once (found %d) — re-point the bake" % (old, n))
        text = text.replace(old, new)
    # The fonts: the helmet's Google Fonts sheets become the one vendored sheet, and the
    # preconnect hints go — with the faces local they would only open sockets to Google.
    sheets = re.findall(r'<link href="https://fonts\.googleapis\.com/css2[^"]*" rel="stylesheet">\n?', text)
    if not sheets:
        raise SystemExit("make_clock_region: the composer source no longer links its fonts "
                         "the way this bake rewrites them — re-point the bake")
    text = text.replace(sheets[0], '<link href="%s" rel="stylesheet">\n' % FONTS_LOCAL, 1)
    for sh in sheets[1:]:
        text = text.replace(sh, "", 1)
    text = re.sub(r'<link rel="preconnect" href="https://fonts\.(googleapis|gstatic)\.com"[^>]*>\n?', "", text)
    if "fonts.googleapis.com" in text:
        raise SystemExit("make_clock_region: a Google Fonts reference survived the rewrite")
    if text.count("</body>") != 1:
        raise SystemExit("make_clock_region: the composer entry lost its single </body>")
    return text.replace("</body>", BRIDGE_SHIM + "</body>")


def members():
    """(name, bytes) in region order — the exact archive make_region writes."""
    with open(os.path.join(COMPOSER_DIR, COMPOSER_ENTRY), "rb") as f:
        entry = _rewrite(f.read().decode("utf-8")).encode("utf-8")
    out = [(MEMBER_MANIFEST, _manifest()), (MEMBER_ENTRY, entry)]
    for name in (MEMBER_ENGINE, MEMBER_RENDERER, MEMBER_SOUND, MEMBER_STACK):
        with open(os.path.join(COMPOSER_DIR, name), "rb") as f:
            out.append((name, f.read()))
    return out


def make_region():
    """The region bytes: a STORE-only ZIP, layout shared with the fixtures."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        for name, data in members():
            # The identical construction make_app_fixture's region writer uses:
            # fixed date, 0644, STORE — one determinism recipe across the lane.
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.external_attr = 0o644 << 16
            zf.writestr(info, data, compress_type=zipfile.ZIP_STORED)
    region = buf.getvalue()
    # Self-check on the writer's own read-back, the posture of every sibling
    # tool: re-open what was just built and prove each member's stored CRC
    # against the bytes it claims, so a corrupt region cannot ship silently.
    zf = zipfile.ZipFile(io.BytesIO(region))
    for info in zf.infolist():
        raw = zf.read(info.filename)
        if zlib.crc32(raw) & 0xFFFFFFFF != info.CRC:
            raise SystemExit("make_clock_region: region member %s fails its own CRC"
                             % info.filename)
        if info.compress_type != zipfile.ZIP_STORED:
            raise SystemExit("make_clock_region: region member %s is not STORE"
                             % info.filename)
    if zf.read(MEMBER_MANIFEST) != _manifest():
        raise SystemExit("make_clock_region: editor.json does not read back")
    return region


def main():
    argv = sys.argv[1:]
    region = make_region()
    if "--verify" in argv:
        if region != make_region():
            print("FAIL: the clock region is not deterministic")
            return 1
        zf = zipfile.ZipFile(io.BytesIO(region))
        print("== CLOCK REGION: %d B over %d members %s" %
              (len(region), len(zf.infolist()),
               [i.filename for i in zf.infolist()]))
        return 0
    if "--out" in argv:
        with open(argv[argv.index("--out") + 1], "wb") as f:
            f.write(region)
        print("wrote region: %d B" % len(region))
    return 0


if __name__ == "__main__":
    sys.exit(main())
