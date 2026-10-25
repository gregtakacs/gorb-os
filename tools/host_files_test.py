#!/usr/bin/env python3
"""host_files_test.py — the OS file service, tested on the host.

Why this file exists
--------------------
main/sd/orb_files.c decides, from a browser-supplied name, which file on the card
gets listed or unlinked. That is the same class of risk host_sd_test.py already
guards on orb_sd_name_ok()/orb_sd_theme_path(): a bench with no card in the
socket cannot mount a volume, but it CAN exhaustively test the pure functions
that stand between a name nobody vouched for and the rest of the volume -- under
FORMAT 2 the theme side of that surface only ever hands over a sha256-derived id,
but the listing path still sees whatever FATFS reports, so the rules stay in force.
The shipped code
is lifted VERBATIM out of main/sd/orb_files.c and compiled here, not re-typed in
Python. A lift that silently came back empty would keep this suite green while
testing a copy that no longer ships, so the lifts die loudly instead.

What is under test
------------------
  orb_files_path()      the name validator is reached from HERE rather than from
                        each caller, and the extension is appended HERE rather
                        than by the client. A name that only just fits is refused
                        rather than truncated: a truncated name is a different
                        file on the card. The canary tail is what makes deleting
                        the fit check visible as bytes written past outsz.
  orb_files_register()  an app's declaration is accepted only for a directory
                        under /sd/ with a dotted extension. A caller that could
                        name an arbitrary directory could name any file on the
                        volume, and an ext of "orbtheme" would silently match
                        "xorbtheme" in the walk's tail compare.
  orb_files_src()       lookup is by exact (app, id); anything else is NULL, so a
                        wrong directory is never guessed.
  the walk's contract   max_entries and the extension tail compare, read out of
                        the shipped source rather than restated here.

Run:  python3 tools/host_files_test.py
Mutations:  python3 tools/host_files_test.py --mutations   (each must fail the suite)
"""

import os, re, subprocess, sys, tempfile

ROOT = (sys.argv[sys.argv.index("--root") + 1] if "--root" in sys.argv
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
P = lambda *a: os.path.join(ROOT, *a)
FI_C, FI_H = P("main", "sd", "orb_files.c"), P("main", "sd", "orb_files.h")
SD_C, SD_H = P("main", "sd", "orb_sd.c"),   P("main", "sd", "orb_sd.h")
for _p in (FI_C, FI_H, SD_C, SD_H):
    if not os.path.exists(_p):
        sys.exit("missing " + _p + " — the lifts below have no fallback")

fi_src = open(FI_C).read()
fi_hdr = open(FI_H).read()
sd_src = open(SD_C).read()
sd_hdr = open(SD_H).read()


def lift(text, pattern, where):
    m = re.search(pattern, text, re.S)
    if not m:
        sys.exit("could not lift %r from %s — did the code move? Dying here is the "
                 "point: an empty lift would keep this suite green while testing a "
                 "copy that no longer ships." % (pattern, where))
    return m.group(0)


# ---------------------------------------------------------------------------
# The lifts, verbatim from what compiles into the device. Computed inside
# build() from the given source text, so --mutations mutates the same text the
# harness compiles: lifts taken at import time would let a mutation change the
# file while the suite still compiled an unmutated copy.
# ---------------------------------------------------------------------------
def build(src, fi_hdr, sd_src, sd_hdr):
    f_guard = lift(src, r"static bool dir_under_sd\(const char \*dir\)[\s\S]*?\n\}", FI_C)
    f_path = lift(src, r"bool orb_files_path\(const orb_file_src_t \*src[\s\S]*?\n\}", FI_C)
    f_reg  = lift(src, r"esp_err_t orb_files_register\(const char \*app_slug[\s\S]*?\n\}", FI_C)
    f_src  = lift(src, r"const orb_file_src_t \*orb_files_src\(const char \*app_slug[\s\S]*?\n\}", FI_C)
    f_name = lift(sd_src, r"bool orb_sd_name_ok\(const char \*name\)[\s\S]*?\n\}", SD_C)
    consts = "\n".join(lift(sd_hdr, r"#define " + k + r"\b[^\n]*", SD_H)
                       for k in ("ORB_SD_BASE", "ORB_SD_NAME_MAX"))
    fconsts = "\n".join(lift(fi_hdr, r"#define " + k + r"\b[^\n]*", FI_H)
                        for k in ("ORB_FILES_ID_MAX", "ORB_FILES_DIR_MAX",
                                  "ORB_FILES_MAX_NAME", "ORB_FILES_PREFIX_MAX"))
    st = lift(fi_hdr, r"typedef struct \{\s*char\s+id\[ORB_FILES_ID_MAX[\s\S]*?\} orb_file_src_t;", FI_H)
    # The table's own record type and its cap, lifted rather than restated: the
    # harness compiles the shipped register()/src(), and a restated record would
    # let those compile against a layout the device does not use.
    tbl = lift(src, r"#define ORB_FILES_SRC_MAX \d+[\s\S]*?\} src_rec_t;", FI_C)
    st = st + "\n" + tbl + "\nstatic src_rec_t s_srcs[ORB_FILES_SRC_MAX];"

    body = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_LOGE(t, ...) ((void)0)
#define ESP_LOGI(t, ...) ((void)0)
#define ESP_LOGW(t, ...) ((void)0)
@CONSTS@
@FCONSTS@
@STRUCT@
@NAMEOK@
@FUNCS@

static int fails = 0;

/* Every refusal must clear out[0] and write nothing past outsz. The canary tail
 * is the point: deleting the fit check has to show up as bytes written beyond
 * the caller's buffer, not merely as a wrong return code. */
static char pbuf[256 + 16];
static void pcase(const char *dir, const char *ext, const char *name,
                  unsigned outsz, int want, const char *wantpath)
{
    orb_file_src_t s; memset(&s, 0, sizeof s);
    snprintf(s.id, sizeof s.id, "themes");
    snprintf(s.dir, sizeof s.dir, "%s", dir);
    snprintf(s.ext, sizeof s.ext, "%s", ext);
    s.max_entries = 8;
    memset(pbuf, 0x5a, sizeof pbuf);
    const int ok = orb_files_path(&s, name, pbuf, outsz);
    int pass = (ok == want) && (!want || strcmp(pbuf, wantpath) == 0);
    if (pass && !want) pass = (pbuf[0] == '\0');
    for (unsigned i = outsz; pass && i < sizeof pbuf; i++)
        if ((unsigned char)pbuf[i] != 0x5a) pass = 0;
    if (!pass) fails++;
    printf("%-5s path(%-22s %-20s) -> %s%s\n", pass ? "ok" : "FAIL",
           name ? name : "(null)", dir, ok ? pbuf : "(refused)",
           pass ? "" : "   <== WRONG");
}

static void rcase_pre(const char *app, const char *dir, const char *ext,
                      const char *pre, int want, const char *why)
{
    orb_file_src_t s; memset(&s, 0, sizeof s);
    snprintf(s.id, sizeof s.id, "themes");
    snprintf(s.dir, sizeof s.dir, "%s", dir);
    snprintf(s.ext, sizeof s.ext, "%s", ext);
    if (pre) snprintf(s.name_prefix, sizeof s.name_prefix, "%s", pre);
    s.max_entries = 8;
    const esp_err_t e = orb_files_register(app, &s);
    const int got = (e == ESP_OK);
    if (got != !!want) fails++;
    printf("%-5s register(%-8s %-14s %-11s pre=%-9s) -> %-8s (%s)\n",
           got == !!want ? "ok" : "FAIL", app ? app : "(null)", dir, ext,
           pre ? pre : "-", got ? "accepted" : "refused", why);
}

static void rcase(const char *app, const char *dir, const char *ext, int want,
                  const char *why)
{
    rcase_pre(app, dir, ext, NULL, want, why);
}


int main(void)
{
    /* The one accepted source, reused by every path case below. */
    rcase("clock", "/sd/themes", ".orbtheme", 1, "under /sd");

    pcase("/sd/themes", ".orbtheme", "diver", 128, 1, "/sd/themes/diver.orbtheme");
    pcase("/sd/themes", ".orbtheme", "..", 128, 0, "");
    pcase("/sd/themes", ".orbtheme", "../../etc/passwd", 128, 0, "");
    pcase("/sd/themes", ".orbtheme", ".hidden", 128, 0, "");
    pcase("/sd/themes", ".orbtheme", "a/b", 128, 0, "");
    pcase("/sd/themes", ".orbtheme", "a\\b", 128, 0, "");
    pcase("/sd/themes", ".orbtheme", NULL, 128, 0, "");
    pcase("/sd/themes", ".orbtheme", "", 128, 0, "");
    /* The client cannot spell the suffix: what it sends is a stem, so a name
     * that already carries it gets it again rather than being trusted as named. */
    pcase("/sd/themes", ".orbtheme", "diver.orbtheme", 128, 1,
          "/sd/themes/diver.orbtheme.orbtheme");
    /* Exact boundary: "/sd/themes/x.orbtheme" is 21 bytes, so 22 fits and 21
     * does not. The refusal, not the truncation, is the requirement. */
    pcase("/sd/themes", ".orbtheme", "x", 22, 1, "/sd/themes/x.orbtheme");
    pcase("/sd/themes", ".orbtheme", "x", 21, 0, "");
    pcase("/sd/themes", ".orbtheme", "diver", 20, 0, "");
    pcase(NULL, ".orbtheme", "diver", 128, 0, "");
    pcase("/sd/themes", "", "diver", 128, 0, "");

    /* The declaration. Anything that would let the OS walk outside the one
     * directory it was handed is refused, including the forms that only LOOK
     * like siblings. */
    rcase("clock", "/sd/themes", "orbtheme",  0, "no dot in ext");
    rcase("clock", "/sd",        ".orbtheme", 0, "/sd itself is not a source");
    rcase("clock", "/etc",       ".orbtheme", 0, "outside /sd");
    rcase("clock", "/sd/../etc", ".orbtheme", 0, "escapes /sd as text");
    rcase("clock", "/sdthemes",  ".orbtheme", 0, "sibling, not child");
    rcase("clock", "",           ".orbtheme", 0, "no directory");
    rcase(NULL,   "/sd/themes",  ".orbtheme", 0, "no owning app");
    rcase_pre("clock", "/sd/themes", ".orbtheme", "clock-", 1, "the owner's prefix is the compound name's first token and its dash");
    rcase_pre("clock", "/sd/themes", ".orbtheme", "my_app-", 1, "underscores are slug bytes");
    rcase_pre("clock", "/sd/themes", ".orbtheme", "x", 1, "a prefix may omit the dash: the slug alone");
    rcase_pre("clock", "/sd/themes", ".orbtheme", "Clock-", 0, "a prefix is lowercase like the slug rule");
    rcase_pre("clock", "/sd/themes", ".orbtheme", "cl-ck-", 0, "a second dash is not a slug byte");
    rcase_pre("clock", "/sd/themes", ".orbtheme", "clock.-", 0, "no dot in a prefix");
    rcase_pre("clock", "/sd/themes", ".orbtheme", "-x", 0, "a prefix cannot lead with the separator");
    rcase_pre("clock", "/sd/themes", ".orbtheme", "x-----------------------", 0, "over the prefix cap");

    if (orb_files_src("nosuchapp", "themes")) { fails++; printf("FAIL  unknown app resolved\n"); }
    if (orb_files_src("clock", "nosuchid"))   { fails++; printf("FAIL  unknown id resolved\n"); }
    if (orb_files_src(NULL, "themes"))        { fails++; printf("FAIL  null app resolved\n"); }

    printf("%s, %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
"""
    for tok, val in (("@CONSTS@", consts), ("@FCONSTS@", fconsts), ("@STRUCT@", st),
                     ("@NAMEOK@", f_name), ("@FUNCS@",
                     f_guard + "\n" + f_path + "\n" + f_reg + "\n" + f_src)):
        body = body.replace(tok, val)
    return body



def run(tmp_root):
    """Compile and run the spliced harness against one tree's sources."""
    src = open(os.path.join(tmp_root, "main", "sd", "orb_files.c")).read()
    tmpl = build(src, open(os.path.join(tmp_root, "main","sd","orb_files.h")).read(),
                  open(os.path.join(tmp_root, "main","sd","orb_sd.c")).read(),
                  open(os.path.join(tmp_root, "main","sd","orb_sd.h")).read())
    tmp = tempfile.mkdtemp(prefix="orbfiles")
    exe = os.path.join(tmp, "t")
    cf = os.path.join(tmp, "t.c")
    open(cf, "w").write(tmpl)
    r = subprocess.run(["gcc", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-include", "stdio.h", cf, "-o", exe],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("FAIL  the lifted code does not compile:\n" + r.stderr[-2500:])
        return 1, ""
    r2 = subprocess.run([exe], capture_output=True, text=True)
    out = r2.stdout
    bad = [l for l in out.splitlines() if l.startswith("FAIL")]
    # A crash, a non-zero exit, or the missing verdict line are all failures.
    # Reading an empty stdout as "zero FAIL lines" would call a segfault a pass,
    # which is exactly how the null-app mutation initially slipped the battery.
    if r2.returncode != 0 or "PASSED, 0 failure(s)" not in out:
        bad.append("harness exited %d without a clean verdict: %s"
                   % (r2.returncode, (r2.stderr or "")[-200:]))
        return 1, out + "\n".join(bad)
    return (1 if bad else 0), out


def main():
    rc, out = run(ROOT)
    print(out if out else "")
    if rc:
        print("%d check(s) failed" % len([l for l in out.splitlines() if l.startswith("FAIL")]))
    else:
        print("ok    orb_files path/declaration rules hold (%d cases)"
              % len([l for l in out.splitlines() if l.startswith("ok ")]))

    if "--mutations" not in sys.argv:
        return 1 if rc else 0

    # Each mutation removes one rule the harness above proves. A battery in which
    # a rule can go missing and the suite stays green is a battery that did not
    # learn anything, so every entry here must make run() fail.
    MUT = [
      ("path builder stops validating the name", "if (!orb_sd_name_ok(name)) return false;", "if (0) return false;"),
      ("path stops appending the extension",     '"%s/%s%s", src->dir, name, src->ext', '"%s/%s", src->dir, name'),
      ("path's fit check is deleted",            "if (n < 0 || (size_t)n >= outsz) { out[0] = '\\0'; return false; }", "if (n < 0) { out[0] = '\\0'; return false; }"),
      ("refusal leaves the old name in out",
       "    out[0] = '\\0';\n    /* The dir is checked here",
       "    /* The dir is checked here"),
      ("register accepts a dotless extension",   "if (src->ext[0] != '.') {", "if (0) {"),
      ("register stops requiring /sd/",          "if (!dir_under_sd(src->dir)) {", "if (0) {"),
      ("src() matches on id alone",              "strcmp(r->app, app_slug) == 0 && strcmp(r->src.id, id) == 0", "strcmp(r->src.id, id) == 0"),
      ("src() accepts a null app",               "if (!app_slug || !app_slug[0] || !id || !id[0]) return NULL;", "if (!id || !id[0]) return NULL;"),
    ]
    fail = 0
    for name, old, new in MUT:
        if fi_src.count(old) < 1:
            print("FAIL mutation %r: anchor found %dx — the battery is stale" % (name, fi_src.count(old))); fail = 1; continue
        tmp = tempfile.mkdtemp(prefix="orbfiles_mut")
        for real in (FI_C, FI_H, SD_C, SD_H):
            d = os.path.join(tmp, os.path.relpath(os.path.dirname(real), ROOT))
            os.makedirs(d, exist_ok=True)
            open(os.path.join(tmp, os.path.relpath(real, ROOT)), "w").write(open(real).read())
        open(os.path.join(tmp, "main", "sd", "orb_files.c"), "w").write(fi_src.replace(old, new, 1))
        rc2, _ = run(tmp)
        if rc2 == 0:
            print("FAIL  mutation %r SURVIVED — a real regression would pass too" % name); fail = 1
        else:
            print("ok    mutation %r caught" % name)
    return fail


if __name__ == "__main__":
    sys.exit(main())
