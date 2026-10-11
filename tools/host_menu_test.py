#!/usr/bin/env python3
"""host_menu_test.py — the on-dial menu's state machine, tested on the host.

Why this file exists
--------------------
The menu is the one part of the UI whose behaviour the bench cannot reach: it is
driven by a rotary knob nobody can turn here, and its failure modes are all
about WHICH ROW the cursor is on and WHAT the two-level (roaming vs armed) model
does about that. A wrong clamp is not visible on a serial console; it is a dial
where the owner cannot reach "<- back".

So, exactly as .clinerules requires for anything the bench cannot observe, the
shipped functions are lifted VERBATIM out of main/menu/menu_view.c and compiled
here — not re-typed in Python. An empty lift dies loudly, because a suite that
silently tests a stale copy is worse than no suite.

What is under test
------------------
  page_rows()          the single authority for "how many rows exist"; armed and
                       busy gating
  go_back()            which row the cursor lands on when a sub-page is left
  row_text()           the labels, including the damaged-theme row that must be
                       shown rather than hidden, and the current-zone marker
  menu_view_apply_input  the cursor clamp: one detent = one row, no wrapping
  menu_view_service()  a worker result landing while the screen is disarmed must
                       NOT paint a list on the title card
  the threading rule   no blocking orb_theme/orb_sd call may appear in a function
                       that touches widgets (a regex lint over _code_only())

Run:  python3 tools/host_menu_test.py
Mutations:  python3 tools/host_menu_test.py --mutations   (each must fail the suite)
"""

import os, re, subprocess, sys

ROOT = (sys.argv[sys.argv.index("--root") + 1] if "--root" in sys.argv
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
P = lambda *a: os.path.join(ROOT, *a)
MENU_C = P("main", "menu", "menu_view.c")
MENU_H = P("main", "menu", "menu_view.h")
ROUTER_C = P("main", "input", "input_router.c")
ROUTER_H = P("main", "input", "input_router.h")
for p in (MENU_C, MENU_H, ROUTER_C, ROUTER_H):
    if not os.path.exists(p):
        sys.exit("missing " + p + " — the lifts below have no fallback")

menu_src = open(MENU_C).read()
router_src = open(ROUTER_C).read()
router_hdr = open(ROUTER_H).read()


FAILS = []
def check(name, ok, detail=""):
    print(("ok    " if ok else "FAIL  ") + name + (("  " + detail) if detail else ""))
    if not ok:
        FAILS.append(name)
    return ok

def lift(text, pattern, where):
    m = re.search(pattern, text, re.S)
    if not m:
        sys.exit("could not lift %r from %s — did the code move? Dying here is the "
                 "point: an empty lift would keep this suite green while testing a "
                 "copy that no longer ships." % (pattern, where))
    return m.group(0)

# ---------------------------------------------------------------------------
# The lifts, verbatim from what compiles into the device.
#
# Lifts are computed by a function rather than at import, and take the source as
# an argument: --mutations replaces text in the source, and lifts that had already
# been extracted at import time would let the battery mutate a file while
# compiling an unmutated copy. Eleven of the twelve below "survived" until this
# was fixed, which is precisely the false-green a battery must never be able to do.
# ---------------------------------------------------------------------------
def lifts(src):
    """Return the (types, functions) spliced out of the given menu_view.c text."""
    f_row = lift(src, r"static void row_text\(int row,[\s\S]*?\n\}\n\nstatic const char \*page_title", MENU_C)
    f_row = f_row[:f_row.index("static const char *page_title")]
    funcs = [
        lift(src, r"static int page_rows\(void\)\n\{[\s\S]*?\n\}", MENU_C),
        lift(src, r"static void go_back\(void\)\n\{[\s\S]*?\n\}", MENU_C),
        f_row,
        lift(src, r"static const char \*page_hint\(void\)\n\{[\s\S]*?\n\}", MENU_C),
        lift(src, r"void menu_view_apply_input\(int32_t delta, bool pressed\)\n\{[\s\S]*?\n\}", MENU_C),
        lift(src, r"void menu_view_service\(void\)\n\{[\s\S]*?\n    repaint\(\);\n\}", MENU_C),
    ]
    consts = [
        lift(src, r"#define ROW_MAX\s+\d+[^\n]*", MENU_C),
        lift(src, r"#define THEME_MAX\s+\d+[^\n]*", MENU_C),
        lift(src, r"typedef enum \{\n    PG_LANDING[\s\S]*?\} page_t;", MENU_C),
        lift(src, r"typedef enum \{ IT_AP[\s\S]*?\} top_item_t;", MENU_C),
        lift(src, r"typedef struct \{\n    int\s+n;[\s\S]*?\} ui_t;", MENU_C),
    ]
    return consts, funcs

# The router's screen order and its two-level gate, both pinned below: rotate
# cycling screens and long-press going home are the contract the menu assumes.
ENUM_SCREEN = lift(router_hdr, r"typedef enum \{\s*INPUT_SCREEN_MENU\s*=\s*0[\s\S]*?\} input_screen_t;",
                   ROUTER_H)
ARM_STEP = lift(router_src, r"const int step = \(knob_col_step\(delta\) > 0\) \? 1\n\s*: \(INPUT_SCREEN_COUNT - 1\);",
                ROUTER_C)
# The carousel must keep asking the knob layer what "right" means. A screen that
# negates the delta itself is the bug host_knob_test.py exists for, and this lift
# dies loudly if the router ever goes back to reading a raw sign.
check("the screen carousel routes direction through the knob layer",
      "knob_col_step" in ARM_STEP, ARM_STEP.strip())

# orb_theme_entry_t, from the shipped header — row_text() indexes its fields.
KNOB_C = P("main", "knob", "knob.c")
KNOB_H = P("main", "knob", "knob.h")
for _p in (KNOB_C, KNOB_H):
    if not os.path.exists(_p):
        sys.exit("missing " + _p + " — menu_view.c now gets its cursor direction from"
                 " the knob layer, so the lift below has no fallback and must not"
                 " invent one")
knob_src = open(KNOB_C).read()
KNOB_HELPERS = (
    lift(open(KNOB_H).read(), r"#define KNOB_DIR_DOWN_IS_CW\s+\d+[^\n]*", KNOB_H) + "\n"
    + lift(knob_src, r"int knob_row_step\(int32_t delta\)\n\{[\s\S]*?\n\}", KNOB_C) + "\n"
    + lift(knob_src, r"int knob_col_step\(int32_t delta\)\n\{[\s\S]*?\n\}", KNOB_C))

TH_H = P("main", "art", "orb_theme.h")
th_hdr = open(TH_H).read()
ENTRY_T = lift(th_hdr, r"typedef struct \{\n    char      slug\[[\s\S]*?\} orb_theme_entry_t;", TH_H)

# ---------------------------------------------------------------------------
# The C harness: the lifted functions, compiled, with the outside world stubbed.
#
# Only the pure state machine is lifted. repaint() and activate() are NOT, because
# they touch LVGL and the card — the checks below pin the model that drives them,
# and the threading rule that keeps them off the LVGL task is linted separately.
# ---------------------------------------------------------------------------
HARNESS = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
struct lv_obj_t; typedef struct lv_obj_t lv_obj_t;

#define ESP_OK 0
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
static const char *TAG __attribute__((unused)) = "menu";
__SLUGDEF__
__UNUSED_OK__
static int g_tz_count   = 62;
static int g_tz_current = 1;
static const char *g_tz_labels[8] = {"UTC","New York","Chicago","Denver","Los Angeles",
                                     "Phoenix (no clock change)","London","Berlin"};
int  orb_net_tz_count(void) { return g_tz_count; }
int  orb_net_tz_current_index(void) { return g_tz_current; }
const char *orb_net_tz_key(int i) { static char b[8]; snprintf(b,sizeof b,"k%d",i); return b; }
const char *orb_net_tz_label(int i) { return g_tz_labels[i < 8 ? i : 7]; }

static void repaint(void) { }
static void input_router_home(void) { }

/* FreeRTOS semaphore calls inside menu_view_service(): the lifted code must keep
 * its publish/take pair, so these are recorded rather than dropped — deleting the
 * mutex from the lift would silently test a different function. */
static int g_takes, g_gives;
typedef void *SemaphoreHandle_t;
static SemaphoreHandle_t s_res_mx;
#define portMAX_DELAY 0xFFFFFFFFu
static int xSemaphoreTake(SemaphoreHandle_t m, unsigned t) { (void)m; (void)t; g_takes++; return 1; }
static int xSemaphoreGive(SemaphoreHandle_t m) { (void)m; g_gives++; return 1; }

/* activate() touches LVGL and the card, so it is not lifted; apply_input()'s
 * cursor half is, and this records that a press was routed to the action path. */
static int g_activations;
static void activate(void) { g_activations++; }

/* job_post() is stubbed: the queue is FreeRTOS, and what matters here is that the
 * cursor moved (or did not), not that a packet was queued. */
typedef enum { JOB_SCAN = 0, JOB_STAGE, JOB_TZ } job_kind_t;
static void job_post(job_kind_t k, const char *slug, int idx) { (void)k;(void)slug;(void)idx; }

__CONSTS__

static page_t s_page = PG_LANDING;
static bool   s_armed;
static int    s_cur;
static ui_t   s_ui;
static bool   s_busy;
static bool   s_dirty;
static ui_t   s_shared;

__FUNCS__

/* ---- intent-based turns ----
 * These tests are about the CLAMP, not about which way the owner's knob points.
 * Asking for +1 literally hardcoded the old sense, so the day the mounting decided
 * that clockwise means up, seven of them went red for no reason — and a suite that
 * cries wolf gets ignored, which is worse than no suite. The direction itself is
 * pinned where it belongs, in tools/host_knob_test.py; here the turn is derived from
 * the shipped helper, so these checks say what they mean and survive a remount. */
static int g_delta_down;                 /* set from knob_row_step() before use */
static void turn_down(int detents) { menu_view_apply_input(g_delta_down * detents, false); }
static void turn_up(int detents)   { menu_view_apply_input(-g_delta_down * detents, false); }

static int fails;
static void ck(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) fails++;
}
static const char *rowline(int r) {
    static char b[96];
    bool hi = false; row_text(r, b, sizeof b, &hi);
    return b;
}
static void one_theme(const char *slug, const char *title, esp_err_t err,
                      int active, int pending, const char *why) {
    memset(&s_ui, 0, sizeof s_ui);
    s_ui.n = 1;
    snprintf(s_ui.e[0].slug, sizeof s_ui.e[0].slug, "%s", slug);
    snprintf(s_ui.e[0].title, sizeof s_ui.e[0].title, "%s", title);
    s_ui.e[0].err = err; s_ui.e[0].active = (bool)active; s_ui.e[0].pending = (bool)pending;
    snprintf(s_ui.e[0].why, sizeof s_ui.e[0].why, "%s", why);
}

int main(void)
{
    /* ---- unarmed is a title card, not a list ---- */
    s_armed = false; s_page = PG_LANDING; s_cur = 0; s_busy = false;
    g_delta_down = knob_row_step(1);
    ck("unarmed has no rows", page_rows() == 0);
    turn_down(1);
    ck("unarmed ignores rotate", s_cur == 0);
    g_activations = 0;
    menu_view_apply_input(0, true);
    /* The press is counted, not merely observed via s_cur: activate() is stubbed,
     * so a check on s_cur alone would pass even if the armed guard were gone and
     * the press had reached the action path. It is g_activations that makes
     * "unarmed ignores press" actually pin that guard. */
    ck("unarmed ignores press", g_activations == 0 && s_cur == 0 && s_page == PG_LANDING);

    /* ---- armed top page ---- */
    s_armed = true; s_page = PG_TOP; s_cur = 0; s_busy = false;
    ck("top page has 4 rows", page_rows() == 4);
    ck("row order AP/TZ/THEME/exit",
       !strcmp(rowline(0),"AP") && !strcmp(rowline(1),"TZ") &&
       !strcmp(rowline(2),"THEME") && !strcmp(rowline(3),"exit"));

    /* ---- the clamp, both ends, no wrap ---- */
    g_delta_down = knob_row_step(1);      /* derived, never assumed */
    ck("a list turn exists to ask for", g_delta_down != 0);
    s_cur = 0;
    turn_up(1);
    ck("rotate past the top clamps to 0", s_cur == 0);
    s_cur = 3;
    turn_down(1);
    ck("rotate past the bottom clamps to last", s_cur == 3);
    s_cur = 1;
    turn_down(99);
    ck("one detent per event, no flying", s_cur == 2);
    s_cur = 2;
    turn_up(99);
    ck("and not backwards either", s_cur == 1);
    menu_view_apply_input(0, false);
    ck("zero delta is not a move", s_cur == 1);

    /* ---- the 62-row zone list ---- */
    s_page = PG_TZ; g_tz_count = 62; s_cur = 0;
    ck("tz list is zones + back row", page_rows() == 63);
    s_cur = 61;
    turn_down(1);
    ck("last zone reaches the back row", s_cur == 62);
    ck("back row says back",
       (s_page = PG_TZ, !strcmp(rowline(62), "<- back")));
    ck("back from tz returns to TZ row",
       (s_page = PG_TZ, go_back(), s_page == PG_TOP && s_cur == 1));
    ck("current zone is marked",
       (s_page = PG_TZ, !strcmp(rowline(1), "New York  *")));
    s_page = PG_TZ;
    ck("a non-current zone is not marked", !strcmp(rowline(2), "Chicago"));

    /* ---- the card theme list ---- */
    s_page = PG_THEME;
    one_theme("kit3", "Arrow 3", ESP_OK, 1, 0, "");
    s_cur = 0;
    ck("active theme is labelled", strstr(rowline(0), "on the dial") != NULL);
    ck("theme title preferred over slug", strstr(rowline(0), "Arrow 3") != NULL);
    one_theme("kit3", "Arrow 3", ESP_OK, 0, 1, "");
    ck("pending theme is labelled", strstr(rowline(0), "waiting to install") != NULL);
    one_theme("kit3", "Broken", 0x102, 0, 0, "checksum mismatch");
    ck("damaged theme is SHOWN with its reason",
       strstr(rowline(0), "checksum mismatch") != NULL);
    ck("damaged theme still names itself", strstr(rowline(0), "Broken") != NULL);
    one_theme("kit3", "", 0x102, 0, 0, "");
    ck("blank title falls back to the slug", strstr(rowline(0), "kit3") != NULL);
    ck("blank why falls back to a default", strstr(rowline(0), "not loadable") != NULL);
    ck("theme list has a back row", !strcmp(rowline(1), "<- back"));
    ck("back from theme returns to THEME row",
       (s_page = PG_THEME, go_back(), s_page == PG_TOP && s_cur == 2));

    /* ---- a running job freezes the list ---- */
    /* Tested on PG_TZ, not PG_THEME: page_rows() already returns 0 while busy on
     * the theme page, so a cursor check there would pass even with apply_input's
     * own s_busy guard deleted. The zone page ignores s_busy, which leaves that
     * guard as the only thing holding the cursor — the only place it can be seen. */
    s_page = PG_TZ; s_cur = 0; s_busy = true; g_tz_count = 62;
    turn_down(1);
    ck("cursor frozen on the zone page while a job runs", s_cur == 0);
    g_activations = 0;
    menu_view_apply_input(0, true);
    ck("press frozen too", g_activations == 0);
    s_busy = false;
    turn_down(1);
    ck("the cursor moves again once the job lands", s_cur == 1);

    /* ---- the theme page while scanning shows nothing ---- */
    s_page = PG_THEME; one_theme("a","A",ESP_OK,0,0,""); s_ui.n = 3; s_cur = 0; s_busy = true;
    ck("scanning shows no rows", page_rows() == 0);
    ck("scanning shows working...", !strcmp(page_hint(), "working..."));
    s_busy = false;
    ck("rows return when the job lands", page_rows() == 4);

    /* ---- the confirm page ---- */
    s_page = PG_ASK; s_cur = 0;
    ck("confirm offers reboot", !strcmp(rowline(0), "reboot now"));
    ck("confirm offers to decline", !strcmp(rowline(1), "keep this dial"));
    ck("confirm has exactly 2 rows", page_rows() == 2);

    /* ---- a worker result landing while disarmed must NOT paint a list ---- */
    s_armed = true; s_page = PG_TOP; s_cur = 0;
    memset(&s_shared, 0, sizeof s_shared);
    s_shared.staged = true; s_dirty = true;
    s_armed = false;                       /* the user rotated away mid-stage */
    menu_view_service();
    ck("staged result still recorded", s_ui.staged);
    ck("title card stays a title card", page_rows() == 0);

    s_armed = true; s_page = PG_TOP; s_cur = 0;
    memset(&s_shared, 0, sizeof s_shared);
    s_shared.staged = true; s_dirty = true;
    menu_view_service();
    ck("armed, a staged result opens the confirm page", s_page == PG_ASK);

    printf(fails ? "HARNESS FAILURES %d\n" : "HARNESS OK\n", fails);
    return fails ? 1 : 0;
}
"""


SLUG_DEF = lift(th_hdr, r"#define ORB_THEME_SLUG_MAX\s+\d+[^\n]*", TH_H)

def assembled(src):
    """The harness source: the shipped types and functions, spliced verbatim."""
    consts, funcs = lifts(src)
    body = "\n".join(consts[:2] + [SLUG_DEF, ENTRY_T] + consts[2:])
    return (HARNESS
            .replace("__CONSTS__", body + "\n" + KNOB_HELPERS)
            .replace("__FUNCS__", "\n".join(funcs))
            .replace("__SLUGDEF__", "")
            .replace("__UNUSED_OK__", ""))


def build_and_run(src):
    import tempfile
    d = tempfile.mkdtemp(prefix="hostmenu_")
    exe = os.path.join(d, "h")
    cf = os.path.join(d, "h.c")
    with open(cf, "w") as f:
        f.write(src)
    r = subprocess.run(["cc", "-O0", "-std=gnu11", "-Wall", "-Wextra",
                        "-Wno-unused-parameter", "-Wno-unused-function",
                        "-Werror", "-o", exe, cf],
                       capture_output=True, text=True)
    if r.returncode:
        return False, "COMPILE FAILED:\n" + r.stderr[-2500:]
    r = subprocess.run([exe], capture_output=True, text=True, timeout=30)
    return r.returncode == 0, r.stdout + r.stderr



# ---------------------------------------------------------------------------
# Source lints over the shipped file. These are not lifts: they read the text,
# the way lock_lint() does over the card code, and they exist for the same reason
# — the property they check is invisible on a bench with nobody turning the knob.
# ---------------------------------------------------------------------------
def _code_only(src):
    """Source with comments and string/char literals blanked, line count preserved.

    Not cosmetic: the threading lint below reads call sites, and a doc comment that
    names orb_theme_scan() would otherwise look like a call to orb_theme_scan().
    host_sd_test.py learned that the hard way (three phantom deadlocks).
    """
    out, i, n = [], 0, len(src)
    state = None
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if state is None:
            if c == "/" and nxt == "/": state = "//"; out.append("  "); i += 2; continue
            if c == "/" and nxt == "*": state = "/*"; out.append("  "); i += 2; continue
            if c in "\"'": state = c; out.append(c); i += 1; continue
            out.append(c); i += 1; continue
        if state == "//":
            if c == "\n": state = None; out.append("\n")
            else: out.append(" ")
            i += 1; continue
        if state == "/*":
            if c == "*" and nxt == "/": state = None; out.append("  "); i += 2; continue
            out.append("\n" if c == "\n" else " "); i += 1; continue
        if c == "\\": out.append(c + (nxt or "")); i += 2; continue
        if c == state: state = None
        out.append("\n" if c == "\n" else " "); i += 1
    return "".join(out)


BLOCKING = ("orb_theme_scan(", "orb_theme_stage_load(")
WIDGET = r"lv_label_set_text\(|lv_obj_set_style_|lv_obj_invalidate\("


def threading_lint(src):
    """No blocking card call may sit in a function that also touches a widget.

    input_router's poll is an lv_timer on the LVGL task and the only reader of the
    knob, so a card call there freezes the dial and swallows input for as long as
    the card mutex is held elsewhere — 38-48 s behind a browser upload, measured.
    The shipped design confines both calls to job_worker().
    """
    code = _code_only(src)
    defs = [(m.group(1), m.start())
            for m in re.finditer(r"^(?:static\s+)?[A-Za-z_][\w \*]*?\b(\w+)\s*\([^;]*?\)\s*\{",
                                 code, re.M)]
    defs.sort(key=lambda d: d[1])
    bad = []
    for i, (name, start) in enumerate(defs):
        end = defs[i + 1][1] if i + 1 < len(defs) else len(code)
        body = code[start:end]
        hit = next((b for b in BLOCKING if b in body), None)
        if hit and re.search(WIDGET, body):
            bad.append("%s() calls %s and also a widget" % (name, hit))
    return bad


def worker_lint(src):
    """The card calls must live in the worker task, and only there."""
    code = _code_only(src)
    m = re.search(r"static void job_worker\(void \*arg\)\n\{([\s\S]*?)\n\}", code)
    if not m:
        return ["job_worker() not found — the card work has nowhere to run"]
    body = m.group(1)
    return ["job_worker() no longer calls %s" % b for b in BLOCKING if b not in body]


MUTATIONS = [
    # (name, old text, new text). Each must make SOME check go red; a pin you can
    # delete without a failure is not a pin.
    ("clamp removed at the bottom",
     "if (next >= rows) next = rows - 1;", "if (0) next = rows - 1;"),
    ("clamp removed at the top",
     "if (next < 0) next = 0;", "if (0) next = 0;"),
    ("unarmed menu starts accepting input",
     "if (!s_armed) return;                    /* the router owns the knob here */", ""),
    ("a running job no longer freezes the cursor",
     "if (s_busy) return;\n\n    if (pressed)", "if (0) return;\n\n    if (pressed)"),
    ("page_rows forgets that unarmed means no list",
     "if (!s_armed) return 0;", "if (0) return 0;"),
    ("the back row is dropped from the zone list",
     "case PG_TZ:     return orb_net_tz_count() + 1;", "case PG_TZ:   return orb_net_tz_count();"),
    ("the back row is dropped from the theme list",
     "case PG_THEME:  return s_busy ? 0 : (s_ui.n + 1);", "case PG_THEME: return s_busy ? 0 : s_ui.n;"),
    ("a damaged theme is hidden instead of shown",
     "snprintf(out, outsz, \"%s  (%s)\", e->title[0] ? e->title : e->slug,\n                         e->why[0] ? e->why : \"not loadable\");",
     "out[0] = '\\0';"),
    ("the current zone stops being marked",
     "const bool cur = (row == orb_net_tz_current_index());", "const bool cur = false;"),
    ("back from the zone list loses its cursor position",
     "s_cur  = (from == PG_TZ) ? IT_TZ : (from == PG_THEME ? IT_THEME : 0);", "s_cur = 0;"),
    ("a staged result paints a list onto the title card",
     "if (s_ui.staged) {", "if (s_ui.staged && !s_armed) {"),
    ("the card scan moves back onto the LVGL task",
     "const int n = orb_theme_scan(out.e, THEME_MAX);",
     "const int n = orb_theme_scan(out.e, THEME_MAX); lv_label_set_text(s_title, \"x\");"),
]


def run_mutations():
    rc = 0
    shipped = open(MENU_C).read()
    for name, old, new in MUTATIONS:
        if shipped.count(old) != 1:
            print("FAIL  mutation %-54r target appears %d times — the pin is stale"
                  % (name, shipped.count(old)))
            rc = 1
            continue
        mutated = shipped.replace(old, new, 1)
        ok_c, out_c = build_and_run(assembled(mutated))
        caught = ((not ok_c) or ("FAIL" in out_c)
                  or bool(threading_lint(mutated)) or bool(worker_lint(mutated)))
        if caught:
            first = next((l for l in out_c.splitlines() if l.startswith("FAIL")),
                         (out_c.splitlines() or ["lint"])[0])
            print("ok    mutation %-54r caught: %s" % (name, first[:64]))
        else:
            print("FAIL  mutation %-54r SURVIVED — this check cannot fail" % (name,))
            rc = 1
    return rc


def main():
    if "--mutations" in sys.argv:
        return run_mutations()

    ok, out = build_and_run(assembled(menu_src))
    lines = [l for l in out.splitlines() if l.startswith(("PASS", "FAIL"))]
    for l in lines:
        check(l[5:], l.startswith("PASS"))
    if not lines:
        check("harness compiled and ran", ok, out.strip()[:1500])

    bad = threading_lint(menu_src)
    check("no blocking card call shares a function with a widget call",
          not bad, "; ".join(bad))
    wbad = worker_lint(menu_src)
    check("the worker task still owns both card calls", not wbad, "; ".join(wbad))
    check("rotate still owns the screen swap (router)",
          "if (!screen_armed(s_current))" in _code_only(router_src))
    check("long-press goes home to the clock (router)",
          re.search(r"if \(lpress\) \{\s*disarm_all\(\);\s*set_screen\(INPUT_SCREEN_CLOCK, true\);\s*return;",
                    router_src) is not None)
    check("menu screen registered with the router",
          "menu_view_screen()" in router_src and "menu_view_service()" in router_src)
    check("picker gained the same two-level gate",
          "bool color_picker_armed(void)" in open(P("main", "color", "color_picker.c")).read()
          and "if (!s_armed) return;" in open(P("main", "color", "color_picker.c")).read())

    print("all host checks pass" if not FAILS else "%d FAILED" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

