#!/usr/bin/env python3
"""host_publish_test.py — orbsdk.py publish, against a stand-in site (D1, SDK step 1).

Run:  python3 tools/host_publish_test.py
Exit 0 = every case ok. The suite stops at the first FAIL.

publish() is the CI door's client: it posts the raw bytes of a built .orbapp to
https://<site>/api/dev/release with the developer token from the environment, and says
either the site's sentence or nothing else. Nothing about that is testable against the
real site from a host suite, and everything about it is worth testing: the token is a
credential, so the whole contract is "quote the site, never yourself" — one line out,
one exit code, and no byte of the token in argv, in the printed line or on an http: URL.

So this suite stands a site in. An http.server bound to 127.0.0.1:0 answers from a
scripted table and records what it received, and the cases drive the shipping command in
a subprocess (not an import: the exit codes and the child's environment are the thing
under test, and an import would let the SDK see the token by another route than the one
it is allowed). Cases 1-6 are the contract's: accepted, a 403 quoted verbatim, a 409
quoted verbatim, the missing token, --dry-run with no token and no connection, and the
https rule seen from both ends — a real host that answers nothing must not exit 0, and a
non-localhost http: target must be refused before the first socket is opened, which the
second stub proves by counting zero hits. Alongside them: the duplicate and the
not-the-named-JSON answers, the 0-byte file the site refuses (ruling 15: the SDK does not
second-guess the site's parser), the header the stub actually got, and that adding the
command left pack and check-migrations as they were.
"""
import importlib.util
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SDK = os.path.join(ROOT, "apps", "sdk", "orbsdk.py")
ORBAPP = os.path.join(ROOT, "tools", "orbapp.py")
CLOCK = os.path.join(ROOT, "apps", "clock")

TOKEN = "host-publish-test-token-9d41"
D = tempfile.mkdtemp(prefix="publish-")


def load_orbapp():
    spec = importlib.util.spec_from_file_location("orbapp_t", ORBAPP)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


OA = load_orbapp()


# ---- the stand-in site --------------------------------------------------------------

def start_stub(bind):
    """A site in one class: it answers POSTs from state["script"] and records every hit."""
    state = {"script": [], "hits": [], "lock": threading.Lock()}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            n = int(self.headers.get("content-length") or 0)
            body = self.rfile.read(n)
            with state["lock"]:
                state["hits"].append({"method": self.command, "path": self.path,
                                      "auth": self.headers.get("authorization"),
                                      "ctype": self.headers.get("content-type"),
                                      "bytes": n, "body": body})
                answer = state["script"].pop(0) if state["script"] else \
                    (500, {"error": "stub site: nothing scripted"})
                st, payload = answer[:2]
                extra = answer[2] if len(answer) > 2 else {}
            raw = payload if isinstance(payload, (bytes, bytearray)) else \
                json.dumps(payload).encode()
            self.send_response(st)
            for k, v in extra.items():
                self.send_header(k, v)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def log_message(self, *_a):
            pass

    class Quiet(http.server.ThreadingHTTPServer):
        # a TLS ClientHello handed to a clear-text handler dies in the handler's own
        # thread: the case below reads that as the answer it is, not as a traceback.
        def handle_error(self, *_a):
            pass

    srv = Quiet((bind, 0), Handler)
    threading.Thread(target=srv.serve_forever, kwargs={"poll_interval": 0.05},
                     daemon=True).start()
    return srv, state


def script(state, *answers):
    with state["lock"]:
        state["script"].extend(answers)


def hits(state):
    with state["lock"]:
        return len(state["hits"])


# ---- driving the command ------------------------------------------------------------

def child_env(token=TOKEN):
    """The CI environment: the token in the env only. No proxies — a case that names a
    public host must fail at DNS, not tunnel out through an egress proxy."""
    e = dict(os.environ)
    for k in ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY",
              "all_proxy", "ALL_PROXY"):
        e.pop(k, None)
    e["no_proxy"] = e["NO_PROXY"] = "localhost,127.0.0.1,127.0.0.2,::1"
    e.pop("ORB_SITE_TOKEN", None)
    if token is not None:
        e["ORB_SITE_TOKEN"] = token
    return e


def orbsdk(args, token=TOKEN, timeout=180):
    """Run the shipping command; return (rc, every byte it printed)."""
    r = subprocess.run([sys.executable, SDK] + list(args), cwd=ROOT, env=child_env(token),
                       capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def lines(text):
    return [ln.strip() for ln in text.splitlines() if ln.strip()]


def one(text, prefix):
    got = [ln for ln in lines(text) if ln.startswith(prefix)]
    return got[0] if len(got) == 1 else None


class Suite:
    """One ok line per case, and it dies on the first FAIL."""

    def __init__(self):
        self.n = 0

    def ok(self, case, cond, detail=""):
        if not cond:
            print("FAIL  %s%s" % (case, (" — " + detail) if detail else ""))
            print("      (%d cases passed before it)" % self.n)
            sys.exit(1)
        self.n += 1
        print("ok    %s" % case)

    def exits(self, case, rc, want):
        self.ok("%s exits %s" % (case, want), rc == want, "got rc %d" % rc)

    def line(self, case, out, want):
        self.ok("%s prints %r" % (case, want), one(out, want) is not None,
                "output was: %r" % out[:400])


def sentinel_container(path, slug="clock", stamp="v1.4.0"):
    """A real .orbapp from the one writer: publish reads its slug out of the header."""
    OA.write_container(path, slug, "Clock", version=stamp)
    return path


def main():
    s = Suite()
    srv, site = start_stub("127.0.0.1")
    srv2, site2 = start_stub("127.0.0.2")
    port, port2 = srv.server_address[1], srv2.server_address[1]
    orb = sentinel_container(os.path.join(D, "clock.orbapp"))
    size = os.path.getsize(orb)
    try:
        cases(s, site, site2, port, port2, orb, size)
    finally:
        srv.shutdown()
        srv2.shutdown()
    print("%d cases ok" % s.n)
    print("ALL PASS")
    return 0


def cases(s, site, site2, port, port2, orb, size):
    host = "127.0.0.1:%d" % port

    # 1. the site takes it: the slug from the container header, the version from the site
    script(site, (200, {"status": "pending", "version": "v1.4.0"}))
    rc, out = orbsdk(["publish", "--site", host, orb])
    s.exits("200 pending", rc, 0)
    s.line("200 pending", out, "publish: clock v1.4.0 -> pending")
    s.ok("200 pending reached the CI door", hits(site) == 1, "%d hits" % hits(site))
    s.ok("the token is never printed", TOKEN not in out and "ORB_SITE_TOKEN" not in out,
         out[:200])

    # the header the site got: one bearer token, raw bytes, the path the contract names
    h = site["hits"][0]
    s.ok("header", h["auth"] == "Bearer " + TOKEN, repr(h["auth"]))
    s.ok("the upload is raw, not multipart", h["ctype"] == "application/octet-stream",
         repr(h["ctype"]))
    s.ok("the body is the file, once", h["path"] == "/api/dev/release"
         and h["bytes"] == size and h["body"] == open(orb, "rb").read(),
         "%s, %d B of %d" % (h["path"], h["bytes"], size))
    s.ok("the token is never an argument",
         "--token" not in orbsdk(["publish", "--help"], token=None)[1])

    # 2. a 403: the site's sentence, verbatim, and nothing else
    script(site, (403, {"error": "this token is for clock"}))
    rc, out = orbsdk(["publish", "--site", host, orb])
    s.exits("403", rc, 1)
    s.line("403", out, "publish: this token is for clock")

    # 3. a 409 not-newer: still just the site's words
    script(site, (409, {"error": "v1.3.0 is not newer than v1.3.2"}))
    rc, out = orbsdk(["publish", "--site", host, orb])
    s.exits("409 not newer", rc, 1)
    s.line("409 not newer", out, "publish: v1.3.0 is not newer than v1.3.2")

    # 4. no token in the environment: the refusal names it and no request goes out
    n = hits(site)
    rc, out = orbsdk(["publish", "--site", host, orb], token=None)
    s.exits("no token", rc, 2)
    s.line("no token", out, "publish: ORB_SITE_TOKEN is not set")
    s.ok("no token opens no connection", hits(site) == n, "%d new hits" % (hits(site) - n))

    # 5. --dry-run: CI linting. No token, no connection, the URL it would have used
    rc, out = orbsdk(["publish", "--dry-run", "--site", host, orb], token=None)
    s.exits("--dry-run", rc, 0)
    s.line("--dry-run", out, "publish: would POST %s (%d B) to http://%s/api/dev/release"
           % (orb, size, host))
    s.ok("--dry-run opens no connection", hits(site) == n, "%d new hits" % (hits(site) - n))
    rc, out = orbsdk(["publish", "--dry-run", "--site", "orb-os.net", orb], token=None)
    s.line("--dry-run on a real host", out,
           "publish: would POST %s (%d B) to https://orb-os.net/api/dev/release" % (orb, size))

    # 6a. the https rule, from the refusing end: a non-localhost http: target is refused
    #     before any socket, and the stub bound for exactly that traffic counts zero hits
    rc, out = orbsdk(["publish", "--site", "http://127.0.0.2:%d" % port2, orb])
    s.exits("http to a non-localhost host", rc, 2)
    s.line("http to a non-localhost host", out, "publish: --site must be https")
    s.ok("the refused http target was never dialed", hits(site2) == 0, "%d hits" % hits(site2))

    # 6b. a real host with nothing behind it: any exit but 0, and no sentence invented
    rc, out = orbsdk(["publish", "--site", "example.com", orb])
    s.ok("an unreachable site does not exit 0", rc != 0, "rc %d, out %r" % (rc, out[:200]))
    s.ok("an unreachable site prints one publish: line",
         len([ln for ln in lines(out) if ln.startswith("publish:")]) == 1
         and "Traceback" not in out, out[:300])
    s.ok("a failed run prints no token", TOKEN not in out, out[:200])

    # 6c. and from the other end: https to a clear-text stub never gets through in clear
    rc, out = orbsdk(["publish", "--site", "https://127.0.0.2:%d" % port2, orb])
    s.ok("https to a clear-text stub does not exit 0", rc != 0, "rc %d" % rc)
    s.ok("no clear-text request ever left", hits(site2) == 0, "%d hits" % hits(site2))

    # a 3xx answer moves the request, never the credential: the token rides as an
    # unredirected header, so the host it points at gets no upload and no bearer
    script(site, (302, {}, {"location": "http://127.0.0.2:%d/elsewhere" % port2}))
    rc, out = orbsdk(["publish", "--site", host, orb])
    s.ok("a 3xx answer does not exit 0", rc != 0, "rc %d, out %r" % (rc, out[:200]))
    s.ok("the redirect never reached the other host's door", hits(site2) == 0,
         "%d hits" % hits(site2))
    s.ok("a 3xx answer prints one publish: line",
         len([ln for ln in lines(out) if ln.startswith("publish:")]) == 1
         and "Traceback" not in out, out[:300])

    # the status words the site answers with, and the two answers that are not answers
    for status, want in (("live", "-> live"), ("skipped", "-> skipped"),
                         ("duplicate", "-> already published")):
        script(site, (200, {"status": status, "version": "v1.4.0"}))
        rc, out = orbsdk(["publish", "--site", host, orb])
        s.exits("200 %s" % status, rc, 0)
        s.ok("200 %s says %r" % (status, want), want in out, out[:200])
    script(site, (200, {"status": "duplicate", "accepted": True}))
    rc, out = orbsdk(["publish", "--site", host, orb])
    s.exits("200 duplicate names no version", rc, 0)
    s.line("200 duplicate names no version, so the container's own stamp speaks", out,
           "publish: clock v1.4.0 -> already published")

    script(site, (200, b"<html>a load balancer, not the site</html>"))
    rc, out = orbsdk(["publish", "--site", host, orb])
    s.exits("200 that is not the named JSON", rc, 1)
    s.line("200 that is not the named JSON", out, "publish: HTTP 200")
    script(site, (500, b"boom"))
    rc, out = orbsdk(["publish", "--site", host, orb])
    s.exits("500 with no JSON", rc, 1)
    s.line("500 with no JSON", out, "publish: HTTP 500")

    # ruling 15: a file the container reader refuses is still sent, and the site's
    # sentence is the whole of what prints
    junk = os.path.join(D, "junk.orbapp")
    open(junk, "wb").write(b"")
    script(site, (400, {"error": "not a readable .orbapp"}))
    rc, out = orbsdk(["publish", "--site", host, junk])
    s.exits("a 0-byte file", rc, 1)
    s.line("a 0-byte file", out, "publish: not a readable .orbapp")
    s.ok("a 0-byte file went as-is", site["hits"][-1]["bytes"] == 0,
         "%d B" % site["hits"][-1]["bytes"])
    script(site, (200, {"status": "pending", "version": "v9.9.9"}))
    rc, out = orbsdk(["publish", "--site", host, junk])
    s.exits("no header to read a slug from", rc, 0)
    s.line("no header to read a slug from falls back to the file's stem", out,
           "publish: junk v9.9.9 -> pending")

    # a file that is not there is the command's own refusal, not the site's
    missing = os.path.join(D, "nope.orbapp")
    rc, out = orbsdk(["publish", "--site", host, missing])
    s.exits("a missing file", rc, 2)
    s.line("a missing file", out, "publish: cannot read %s" % missing)

    # the rest of the SDK is as it was: the reader still reads, the other subcommands
    # still parse, and a pack still packs (when this host has the compiler)
    rc, out = orbsdk(["inspect", orb], token=None)
    s.ok("inspect still reads a container back", rc == 0 and "clock" in out, out[:200])
    rc, out = orbsdk(["check-migrations", "--help"], token=None)
    s.ok("check-migrations still parses", rc == 0 and "--save-current" in out, out[:200])
    cmds = re.findall(r'add_parser\("([^"]+)"', open(SDK).read())
    s.ok("publish joined the parser block and nothing else moved",
         cmds == ["new", "build", "pack", "install", "publish", "inspect", "binds",
                  "dist", "check-migrations"], str(cmds))
    cc = os.environ.get("ORB_CLANG") or shutil.which("clang")
    if cc and os.path.isfile(cc):
        rc, out = orbsdk(["pack", CLOCK], token=None)
        s.ok("pack still writes the shelf's container", rc == 0 and "wrote" in out, out[:200])
        shelf = os.path.join(CLOCK, "build", "clock.orbapp")
        script(site, (200, {"status": "pending", "version": "v1.6.2"}))
        rc, out = orbsdk(["publish", "--site", host, shelf])
        s.exits("a shelf container publishes", rc, 0)
        s.ok("a shelf container's slug comes from its own header",
             "publish: clock v1.6.2 -> pending" in out, out[:200])
    else:
        print("ok    pack leg skipped: no clang here (ORB_CLANG unset, none on PATH)")
    s.exits("publish with no --site is a usage error",
            orbsdk(["publish", orb], token=None)[0], 2)


if __name__ == "__main__":
    sys.exit(main())
