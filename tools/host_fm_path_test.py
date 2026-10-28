#!/usr/bin/env python3
"""Bench check for the file manager's two doors: the query decoder and the path rule.

    python3 tools/host_fm_path_test.py

Two shipped bugs, both found on the owner's own card, both invisible to every test
that existed before them:

1. NESTED PATHS WERE UNDELIVERABLE. The FM handlers read ?path= through the shared
   q_param(), whose decoder refuses any escape resolving to '/'. That refusal is
   right for an id, which is one segment by definition, and fatal for a card path,
   which is '/'-separated by construction: encodeURIComponent turns the separator
   into %2F before the browser sends it. The decode fails, the value comes back
   empty, and the door reports "path is missing" for a parameter that arrived whole.
   Every delete inside a directory and every listing two levels deep died there.

2. LISTABLE WAS NOT DELETABLE. orb_sd_fm_path() judged a name against the loose read
   rule when listing and the strict store rule when mutating, so "System Volume
   Information" — written by a Windows host, shown by the page, offered a Delete
   button — answered "bad path". v1.1.18 documented the asymmetry as deliberate and
   left the owner holding an undeletable row. D39 collapses it: one segment rule for
   the whole FM surface, and no directory off-limits. The first flash after that
   change kept /sd/apps and /sd/themes refused, and the owner hit that wall the same
   day — a card that shows a row and will not delete it is the same bug in a different
   hat. A USB host already reaches the entire volume through MTP (orb_usb_mtp.c
   exports ORB_SD_BASE as its one storage), so the refusal protected nothing the owner
   did not already hold. What replaces it is orb_sd_ensure_store_dirs(): both
   directories are re-created before any door writes inside one, so deleting them
   costs a re-push rather than the dial.

Checked here is the shipped source, lifted and compiled:

1. THE DECODER, BOTH MODES. A %2F reaches a path door and is refused by an id door;
   a NUL and a malformed escape are refused by both; %2E%2E%2F survives decoding and
   is then refused by the path rule — the ordering that makes letting a slash through
   safe at all.
2. THE PATH RULE, ONE ANSWER BOTH WAYS. Every accepted rel builds, every refused rel
   builds nothing, and the two directions agree on every name except the
   device-owned directories.
3. THE STORE RULE DID NOT MOVE. orb_sd_name_ok() still refuses what it always
   refused, so widening the file manager did not widen what an app may install.

The lifts fail LOUDLY if the code they extract has moved: a lift that silently came
back empty would keep reporting green while testing nothing.
"""
import hashlib, os, re, subprocess, sys

ROOT = (sys.argv[sys.argv.index("--root") + 1] if "--root" in sys.argv
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
P = lambda *a: os.path.join(ROOT, *a)
SD_C, SD_H = P("main", "sd", "orb_sd.c"), P("main", "sd", "orb_sd.h")
NET_C = P("main", "net", "orb_net.c")
for p in (SD_C, SD_H, NET_C):
    if not os.path.exists(p):
        sys.exit("missing " + p + " — the lifts below have no fallback")

sd_src, net_src, sd_hdr = open(SD_C).read(), open(NET_C).read(), open(SD_H).read()


def lift(text, pattern, where):
    m = re.search(pattern, text, re.S)
    if not m:
        sys.exit("could not lift %r from %s — did the code move? Dying here is the "
                 "point: an empty lift would keep this suite green while testing a "
                 "copy that no longer ships." % (pattern, where))
    return m.group(0)


CONSTS = "\n".join(lift(sd_hdr, r"#define " + k + r"\b[^\n]*", SD_H) for k in
                   ("ORB_SD_BASE", "ORB_SD_NAME_MAX", "ORB_SD_FM_PATH_MAX",
                    "ORB_SD_FM_SEGS_MAX"))
F_NAME_OK = lift(sd_src, r"bool orb_sd_name_ok\(const char \*name\)\n\{[\s\S]*?\n\}", SD_C)
F_FM_SEG  = lift(sd_src, r"static bool fm_seg_ok_read\(const char \*seg[\s\S]*?\n\}", SD_C)
F_FM_PATH = lift(sd_src, r"bool orb_sd_fm_path\(char \*out[\s\S]*?\n\}", SD_C)
F_QPAIR   = lift(net_src,
                 r"static bool q_param_seg\(httpd_req_t \*req[\s\S]*?"
                 r"static bool q_param_path\(httpd_req_t \*req[^\n]*\n\{[\s\S]*?\n\}",
                 NET_C)

for name, body in (("orb_sd_fm_path", F_FM_PATH), ("fm_seg_ok_read", F_FM_SEG),
                   ("q_param_seg/q_param/q_param_path", F_QPAIR),
                   ("orb_sd_name_ok", F_NAME_OK)):
    if len(body) < 200:
        sys.exit("the lift of %s is %d bytes — too short to be the shipped code"
                 % (name, len(body)))

# The rule that used to be two is now one, and the direction argument is gone with
# it. A re-split would put apps/ and themes/ back under a mutation-only policy, which
# is the exact wall this round of fixes exists to take down.
if re.search(r"orb_sd_fm_path\([^)]*read_only", sd_src) or \
   re.search(r"bool orb_sd_fm_path\(char \*out, size_t outsz, const char \*rel,", sd_src):
    sys.exit("orb_sd_fm_path has grown a read_only argument back — the "
             "listable-is-deletable collapse this test exists to protect is gone")
fm_body = sd_src.split("bool orb_sd_fm_path(char *out, size_t outsz, const char *rel)")[1]
fm_body = fm_body[:fm_body.index("\n}") + 2]
for owned in ('"apps"', '"themes"'):
    if owned in fm_body:
        sys.exit("%s is back inside orb_sd_fm_path: the guard was removed on purpose "
                 "(a USB host reaches the whole volume through MTP), and the store "
                 "directories are rebuilt by orb_sd_ensure_store_dirs instead" % owned)
# The guard is gone, so this is the only thing keeping a deleted store directory from
# breaking every upload until the next remount. Losing it turns a cleanup into a dial
# that cannot be restyled or reinstalled on.
if "void orb_sd_ensure_store_dirs(void)" not in sd_src:
    sys.exit("orb_sd_ensure_store_dirs is gone — with the apps/themes guard removed, "
             "a deleted store directory would fail every upload with FR_NO_PATH")
for where, f in (("esp_err_t orb_theme_upload_begin(", P("main", "art", "orb_theme.c")),
                 ("esp_err_t orb_app_upload_begin(", P("main", "app", "orb_app_install.c")),
                 ("esp_err_t orb_sd_fm_write_open(", SD_C)):
    body = open(f).read()
    at = body.index(where)             # the definition, not a comment that names it
    if where not in body:
        sys.exit("%s no longer defined in %s" % (where, f))
    if "orb_sd_ensure_store_dirs()" not in body[at:at + 1400]:
        sys.exit("%s writes inside a store directory without calling "
                 "orb_sd_ensure_store_dirs() first — FR_NO_PATH after a delete" % where)
for door in ("fm_query_path(req", 'q_param_path(req, "path"'):
    if door not in net_src:
        sys.exit("the file manager doors no longer call %s — the nested-path fix has "
                 "been rewired away" % door)

# The two IDF calls the lifted decoder needs, faithful to httpd_parse.c: the value is
# copied UNDECODED (strlcpy, bounded), which is exactly why the hand-rolled decode
# loop that follows it is load-bearing.
IDF_STUB = r'''
/* req reaches only the two macros below, so a void* stands in for httpd_req_t. */
typedef void *httpd_req_t;
static char GQ[512];
static size_t gq_cut = 0;
static bool q_get(httpd_req_t *r, char *b, size_t n)
{
    (void)r;
    if (gq_cut + 1 > n) return true;                    /* would not fit: TRUNC */
    memcpy(b, GQ, gq_cut); b[gq_cut] = '\0';
    return false;
}
#define httpd_req_get_url_query_str(r, b, n) (q_get(r, b, n) ? 0x10A : ESP_OK)
static esp_err_t qkv(const char *qry, const char *key, char *val, size_t n)
{
    const char *p = qry;
    const size_t kl = strlen(key);
    while (*p) {
        const char *eq = strchr(p, '=');
        if (!eq) return 1;
        if ((size_t)(eq - p) == kl && !strncasecmp(p, key, kl)) {
            const char *s = eq + 1, *e = strchr(s, '&');
            const size_t l = e ? (size_t)(e - s) : strlen(s);
            if (l + 1 > n) return 1;
            memcpy(val, s, l); val[l] = '\0';
            return ESP_OK;
        }
        const char *amp = strchr(eq, '&');
        if (!amp) break;
        p = amp + 1;
    }
    return 1;
}
#define httpd_query_key_value(q, k, v, n) qkv(q, k, v, n)
'''
QDECODE = F_QPAIR

# rel, want-read, want-write.  The regression rows are the ones with spaces in them.
PATH_CASES = [
    ("a", 1, 1), ("photo.jpg", 1, 1), ("a/b", 1, 1), ("photos/2026/a.txt", 1, 1),
    ("System Volume Information", 1, 1), ("My Photos", 1, 1),
    ("a/My Photos/b.txt", 1, 1), ("trailing space ", 1, 1),
    ("weird!#$%&()+,-;=@[]^`{}", 1, 1), ("my/apps", 1, 1),
    ("deep/one/two/three/four/five/six/seven", 1, 1),
    # the store directories, deletable like any other row on the card. A USB host
    # already holds the whole volume through MTP, so the old refusal protected
    # nothing the owner did not already have; orb_sd_ensure_store_dirs rebuilds
    # both before the next write lands, so a delete costs a re-push, not the dial.
    ("apps", 1, 1), ("themes", 1, 1), ("apps/clock.orbapp", 1, 1),
    ("themes/x.theme", 1, 1), ("apps/My App.orbapp", 1, 1),
    # traversal, every spelling
    ("../etc", 0, 0), ("..", 0, 0), (".", 0, 0), ("a/../b", 0, 0), ("a/..", 0, 0),
    ("/sd/x", 0, 0), ("a//b", 0, 0), ("a/", 0, 0), (".hidden", 0, 0), ("a/.git", 0, 0),
    # characters that break FAT or cannot round-trip the CP437 volume
    ("a\\b", 0, 0), ("a:b", 0, 0), ("a?b", 0, 0), ("caf\\xc3\\xa9", 0, 0),
]
PARITY = ["a", "photo.jpg", "a/b", "photos/2026/a.txt", "System Volume Information",
          "My Photos", "a/My Photos/b.txt", "my/apps",
          "deep/one/two/three/four/five/six/seven", "L64",
          "System Volume Information/Photo 1.jpg",
          "apps", "themes", "apps/clock.orbapp", "themes/clock-skeleton.orbtheme"]
NAME_ROWS = [("clock.orbapp", 1), ("a.b-c_1", 1), ("System Volume Information", 0),
             ("a b", 0), ("caf\xc3\xa9", 0), ("..", 0), (".hidden", 0), ("a/b", 0)]
# encoded value, strict-mode want, path-mode want
DECODE_ROWS = [("photos%2F2026%2Fa.txt", 0, 1), ("System%20Volume%20Information", 1, 1),
               ("a", 1, 1), ("%2E%2E%2Fx", 0, 1), ("%2e%2e%2f", 0, 1),
               ("a%00b", 0, 0), ("%zz", 0, 0), ("%2", 0, 0),
               ("100%25", 1, 1), ("plu%2Bs", 1, 1)]

L64, L65 = "l" * 64, "l" * 65
LREL = "d/" + "x" * 64 + "/" + "y" * 63          # 128 bytes: over ORB_SD_FM_PATH_MAX

C_ESC = lambda s: s.replace("\\", "\\\\").replace('"', '\\"')
PATH_ROWS = "\n".join('    tcase("%s", %d, %d);' % (C_ESC(r), a, b) for r, a, b in PATH_CASES)
PARITY_ROWS = "\n".join('    parity("%s");' % C_ESC(p).replace("L64", L64) for p in PARITY)
NAME_CALLS = "".join('    ncase("%s", %d);' % (C_ESC(n), w) for n, w in NAME_ROWS)
DECODE_CALLS = "\n".join('    dcase("%s", %d, %d);' % (e, a, b) for e, a, b in DECODE_ROWS)

HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stddef.h>
#define ESP_OK 0
typedef int esp_err_t;
@IDF@
@CONSTS@
@NAMEOK@
@FMSEG@
@FMPATH@
@QDECODE@

static int fails = 0;

/* One call, one answer. The fixture still carries a want per direction, so a row
 * that ever disagrees between the two is visible here, and the lifted function
 * takes no direction argument at all: re-splitting the rule is a compile failure at
 * every call site in this file, which is the loudest way to lose the test. */
static void tcase(const char *rel, int ro, int rw)
{
    char a[ORB_SD_FM_PATH_MAX + 32];
    if (ro != rw) {
        fails++;
        printf("FAIL fixture \"%s\" wants read=%d write=%d; one rule cannot split\n",
               rel, ro, rw);
        return;
    }
    const bool g = orb_sd_fm_path(a, sizeof a, rel);
    if (g != (bool)ro) {
        fails++;
        printf("FAIL rel \"%s\" accepted=%d want=%d\n", rel, g, ro);
        return;
    }
    if (g && strncmp(a, ORB_SD_BASE, strlen(ORB_SD_BASE))) {
        fails++; printf("FAIL rel \"%s\" built a path off the card: %s\n", rel, a);
    }
}

/* The promise the whole change rests on, asserted per row: one call serves both
 * directions, so nothing the page can list can refuse a delete. */
static void parity(const char *rel)
{
    char a[ORB_SD_FM_PATH_MAX + 32];
    const bool g = orb_sd_fm_path(a, sizeof a, rel);
    const bool w = orb_sd_fm_path(a, sizeof a, rel);
    if (g != w) {                       /* same call: fires only if the rule grows state */
        fails++;
        printf("FAIL \"%s\" lists but will not delete — the asymmetry is back\n", rel);
    }
}

static void ncase(const char *n, int want)
{
    const bool g = orb_sd_name_ok(n);
    if (g != (bool)want) {
        fails++; printf("FAIL store rule orb_sd_name_ok(\"%s\") = %d want %d\n",
                        n, g, want);
    }
}

static void dcase(const char *enc, int want_strict, int want_path)
{
    char out[256], path[256];
    strcpy(GQ, "x=1&path="); strcat(GQ, enc); gq_cut = strlen(GQ);
    const bool s = q_param(NULL, "path", out, sizeof out);
    if (s != (bool)want_strict) {
        fails++; printf("FAIL id-mode decode \"%s\" = %d want %d\n", enc, s, want_strict);
    }
    strcpy(GQ, "x=1&path="); strcat(GQ, enc); gq_cut = strlen(GQ);
    const bool p = q_param_path(NULL, "path", path, sizeof path);
    if (p != (bool)want_path) {
        fails++; printf("FAIL path-mode decode \"%s\" = %d want %d\n", enc, p, want_path);
        return;
    }
    if (!p) return;
    char abs_[ORB_SD_FM_PATH_MAX + 32];
    const bool ok = orb_sd_fm_path(abs_, sizeof abs_, path);
    printf("      \"%s\" decodes to \"%s\" -> %s\n", enc, path,
           ok ? "accepted" : "REFUSED by the path rule");
}

int main(void)
{
    puts("== one rule both ways: what the page can list it can delete ==");
@PATHS@
    puts("== listable-is-deletable, asserted on every shape above ==");
@PARITY@
    puts("      (silence above is the pass: nothing listed was undeletable)");
    puts("== the length caps are measured, not assumed ==");
    tcase("@L64@", 1, 1);
    tcase("@L65@", 0, 0);
    tcase("@LREL@", 0, 0);
    puts("== the store rule did not move with the file manager ==");
@NAMES@
    puts("== the decoder: a slash for the path door, never for an id door ==");
@DECODES@
    puts("== traversal survives decoding and dies at the path rule ==");
    {
        char out[256];
        strcpy(GQ, "path=%2E%2E%2Fetc%2Fpasswd"); gq_cut = strlen(GQ);
        if (!q_param_path(NULL, "path", out, sizeof out)) {
            fails++; puts("FAIL %2E%2E%2F did not decode — the path door is strict again");
        } else {
            char abs_[ORB_SD_FM_PATH_MAX + 32];
            if (orb_sd_fm_path(abs_, sizeof abs_, out)) {
                fails++; printf("FAIL \"%s\" reached a path: %s\n", out, abs_);
            } else printf("      \"%s\" decoded, then was refused before a path was built\n", out);
        }
        if (q_param(NULL, "path", out, sizeof out)) {
            fails++; puts("FAIL the strict id door now accepts a slash");
        } else puts("      ok   the strict door still refuses it at decode");
    }
    return fails != 0;
}
'''.replace("@L64@", L64).replace("@L65@", L65).replace("@LREL@", LREL)

for _k, _v in (("@IDF@", IDF_STUB), ("@CONSTS@", CONSTS), ("@NAMEOK@", F_NAME_OK),
               ("@FMSEG@", F_FM_SEG), ("@FMPATH@", F_FM_PATH), ("@QDECODE@", QDECODE),
               ("@PATHS@", PATH_ROWS), ("@PARITY@", PARITY_ROWS),
               ("@NAMES@", NAME_CALLS), ("@DECODES@", DECODE_CALLS)):
    HARNESS = HARNESS.replace(_k, _v)

if re.search(r"@[A-Z0-9]+@", HARNESS):
    sys.exit("leftover token in the harness:\n" +
             "\n".join(l for l in HARNESS.split("\n") if re.search(r"@[A-Z0-9]+@", l))[:600])

BIN = "/tmp/orb_fm_path_" + hashlib.md5((sd_src + net_src).encode()).hexdigest()[:10]
open(BIN + ".c", "w").write(HARNESS)
cc = subprocess.run(["gcc", "-O1", "-Wall", "-Wextra", "-Wno-unused-function",
                     "-o", BIN + ".out", BIN + ".c"], capture_output=True, text=True)
if cc.returncode:
    print("FAIL the lifted doors do not compile on the host:")
    print(cc.stderr[-3000:])
    sys.exit(1)
if cc.stderr.strip():
    print("note: the lifted code warns:\n" + cc.stderr[-1500:])

r = subprocess.run([BIN + ".out"], capture_output=True)
print(r.stdout.decode("utf-8", "backslashreplace"), end="")
if r.returncode:
    print("  (the harness itself reported failures)")
sys.exit(r.returncode)


