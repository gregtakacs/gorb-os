#!/usr/bin/env python3
"""Host check for main/net/settings.html, the connected orb's own page
(app-platform-plan §67).

    python3 tools/host_settings_page_test.py

  parse     the page's script parses (one syntax error blanks the whole page on a
            phone, and nothing on the orb can tell)
  calls     every function the script calls is defined, or is a browser built-in
            (esprima cannot see a dropped definition; the phone's dead button can)
  doors     the page reaches every setting through the orb's own /api/v1 doors, and
            each of those doors is registered in orb_net.c's URI table
  served    h_index serves it on a connected orb and the provisioning page in a
            setup boot, and the build embeds it
  gone      no route or link to the pages §67 retired (/timezone, /themes, /apps),
            and no plus-code entry anywhere on it
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(ROOT, "main", "net", "settings.html")
NET = os.path.join(ROOT, "main", "net", "orb_net.c")
CMAKE = os.path.join(ROOT, "main", "CMakeLists.txt")

BUILTINS = {"fetch", "if", "else", "for", "while", "switch", "return", "typeof", "function", "throw",
            "catch", "parseInt", "isFinite", "isNaN", "confirm", "setTimeout",
            "encodeURIComponent", "String", "Error", "Math", "JSON"}


def main():
    page = open(PAGE, encoding="utf-8").read()
    net = open(NET, encoding="utf-8").read()
    cmake = open(CMAKE, encoding="utf-8").read()
    fails = []
    checks = 0

    def check(ok, why):
        nonlocal checks
        checks += 1
        if not ok:
            fails.append(why)

    m = re.search(r"<script>(.*?)</script>", page, re.S)
    check(m is not None, "the page has no <script>")
    js = m.group(1) if m else ""
    try:
        import esprima
    except ImportError:
        print("SKIP  JS parse (no esprima: pip install esprima)")
    else:
        try:
            esprima.parseScript(js)
            check(True, "")
        except Exception as e:
            check(False, "script does not parse: %s" % e)

    # calls: a bare name followed by "(" that is neither defined here nor a built-in
    code = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', js)          # strings say nothing about calls
    code = re.sub(r"/(?:[^/\\\n]|\\.)+/[gimsuy]*(?=\.)", "/x/", code)   # nor do regex literals
    code = re.sub(r"(?m)^\s*//.*$", "", code)                             # nor do comments
    defs = set(re.findall(r"function ([A-Za-z_$][\w$]*)\s*\(", code))
    for params in re.findall(r"function\s*[\w$]*\s*\(([^)]*)\)", code):
        defs |= {p.strip() for p in params.split(",") if p.strip()}
    defs |= set(re.findall(r"var ([A-Za-z_$][\w$]*)\s*=", code))
    code = re.sub(r"\bnew\s+[A-Za-z_$][\w$]*\s*\(", "(", code)          # constructors are the browser's
    used = set(re.findall(r"(?<![.\w$])([A-Za-z_$][\w$]*)\s*\(", code))
    undef = sorted(used - defs - BUILTINS)
    check(not undef, "the script calls undefined functions: %s" % undef)

    # doors: what the page talks to, and that each is a registered route
    doors = sorted(set(re.findall(r'"(/api/v1/[a-z/]+)', js)))
    want = ["/api/v1/apps", "/api/v1/apps/cfg", "/api/v1/apps/settings", "/api/v1/display",
            "/api/v1/home", "/api/v1/name", "/api/v1/places", "/api/v1/reprov", "/api/v1/sd",
            "/api/v1/sd/bundle", "/api/v1/sd/preview", "/api/v1/sound", "/api/v1/status",
            "/api/v1/units"]
    for d in ("/manifest.webmanifest", "/icon.png"):
        check(d in page and '.uri = "%s"' % d in net, "the page's %s is not registered" % d)
    for d in want:
        check(d in doors, "the page no longer uses %s" % d)
    for d in doors:
        check('.uri = "%s"' % d in net, "the page calls %s, which orb_net.c does not register" % d)

    # the unit keys are orb_units.c's, so a select can never post a key the orb refuses
    units = open(os.path.join(ROOT, "main", "net", "orb_units.c"), encoding="utf-8").read()
    for key in re.findall(r'\["([a-z0-9]+)", "[^"]*"\]', js.split("var UNITS")[1].split("];")[0]):
        check('"%s"' % key in units, "unit key %r is not one of orb_units.c's" % key)

    # served
    check('EMBED_TXTFILES' in cmake and '"net/settings.html"' in cmake,
          "the build does not embed main/net/settings.html")
    check("s_setup_boot ? HTML_LANDING : settings_html_start" in net,
          "h_index must serve the provisioning page in a setup boot and this page otherwise")

    # gone
    for gone in ('"/timezone"', '"/themes"', '"/apps",'):
        check(gone not in net.split("static const httpd_uri_t")[1] if "static const httpd_uri_t" in net else True,
              "the retired route %s is registered again" % gone)
    # Leaflet's UMD build assigns window.L and window.leaflet: a page global by either name
    # is overwritten when the map script lands, and every later call to it throws
    check(not re.search(r"(?m)^(?:function|var)\s+(?:leaflet|L)\b", code),
          "the page defines a global named leaflet or L, which Leaflet overwrites")
    # the preview cache is keyed by the theme's serial and bake stamp, and pruned to the
    # card's list, so a changed or removed theme never draws a stale picture
    check('t.serial + ":" + t.baked' in js and "prevPrune(themes)" in js,
          "the preview cache must key on serial:baked and prune to the card's themes")
    for s in ("/timezone", "plus code", "?code=", "8559CWXG"):
        check(s not in page, "the page mentions %r, which §67 removed" % s)
    check("/timezone" not in net.split("static const char HTML_LANDING")[1].split(";\n")[0],
          "the provisioning page still links the retired clock page")

    if fails:
        for f in fails:
            print("FAIL  " + f)
        print("FAIL %d/%d settings-page checks" % (checks - len(fails), checks))
        return 1
    print("ok %d/%d settings-page checks" % (checks, checks))
    return 0


sys.exit(main())
