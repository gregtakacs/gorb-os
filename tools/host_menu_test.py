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
                       busy gating; the DISPLAY page and the preset picker, whose
                       row count comes from the lifted s_to_preset table
  go_back()            which row the cursor lands on when a sub-page is left, and
                       from the timeout picker back to its parent at its own row
  row_text()           the labels, including the damaged-theme row that must be
                       shown rather than hidden, the current-zone marker, and the
                       DISPLAY submenu's two rows (the dim switch reads the real
                       enable, the timeout row reads the real seconds)
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
    # The DISPLAY submenu's data, lifted rather than restated: which timeouts the
    # picker offers IS the setting the owner chooses, so a hand-typed copy of the
    # table would keep this suite green while the shipped menu offered something
    # else. timeout_label is the same story one level up -- it is the row's claim
    # about the value in force, and it must be the code that ships.
    presets = lift(src, r"typedef struct \{ uint32_t s; const char \*lbl; \} to_preset_t;\n"
                       r"static const to_preset_t s_to_preset\[\][\s\S]*?\n\};\n"
                       r"#define TO_PRESET_N[^\n]*", MENU_C)
    f_timeout = lift(src, r"static void timeout_label\(char \*out, size_t outsz\)\n\{[\s\S]*?\n\}", MENU_C)
    funcs = [
        lift(src, r"static int app_rows\(void\)\n\{[\s\S]*?\n\}", MENU_C),
        lift(src, r"static const orb_app_t \*app_row\(int i\)\n\{[\s\S]*?\n\}", MENU_C),
        f_timeout,
        # D12's shape predicates, lifted as a block: page_rows(), row_text() and
        # apply_input()'s clamp all lean on them, and a harness that restated any
        # one of them would keep passing while the shipped shell hid the wrong
        # rows. The block is one lift because the functions are written together
        # and die together.
        lift(src, r"static int owner_group\(int rows, int idx\)\n\{[\s\S]*?"
                   r"static void shape_check\(int rows\)\n\{[\s\S]*?\n\}", MENU_C),
        lift(src, r"static int page_rows\(void\)\n\{[\s\S]*?\n\}", MENU_C),
        lift(src, r"static void go_back\(void\)\n\{[\s\S]*?\n\}", MENU_C),
        # D12's press half: what a header does when it is pressed. Lifted on its own
        # because it sits by activate(), which is not liftable (it paints). cursor_fit
        # came along with the shape block above, and a second lift of it would be a
        # redefinition.
        lift(src, r"static bool group_press\(int rows, int ar\)\n\{[\s\S]*?\n\}", MENU_C),
        f_row,
        lift(src, r"static const char \*page_hint\(void\)\n\{[\s\S]*?\n\}", MENU_C),
        lift(src, r"void menu_view_apply_input\(int32_t delta, bool pressed\)\n\{[\s\S]*?\n\}", MENU_C),
        lift(src, r"void menu_view_service\(void\)\n\{[\s\S]*?\n    repaint\(\);\n\}", MENU_C),
        # D12: the long press that closes an open group instead of going home. It
        # touches no widget directly (repaint() is stubbed above), so it lifts.
        lift(src, r"bool menu_view_opt_long\(void\)\n\{[\s\S]*?\n\}", MENU_C),
        # D19: the worker's toggle and the pack it decodes, lifted whole — the
        # pack is built by cap_pack(), which exists on its own precisely because
        # activate() cannot be lifted and the shift arithmetic is the whole
        # contract between a press and the NVS word it writes.
        lift(src, r"static int cap_pack\(const orb_app_t \*a, int row\)\n\{[\s\S]*?\n\}", MENU_C),
        lift(src, r"static void job_grant\(void \*arg\)\n\{[\s\S]*?\n\}", MENU_C),
    ]
    consts = [
        lift(src, r"#define ROW_MAX\s+\d+[^\n]*", MENU_C),
        presets,
        lift(src, r"typedef enum \{\n    PG_LANDING[\s\S]*?\} page_t;", MENU_C),
        lift(src, r"typedef enum \{ IT_AP[\s\S]*?\} top_item_t;", MENU_C),
        # D19: the revocation ladder's state, the four caps, and the row
        # helpers — one block, lifted ahead of the page functions that call into
        # it. The orb_svc doors it reaches are declared by the real orb_svc.h
        # the harness includes and stubbed below with the registry's.
        lift(src, r"static int      s_perm_slot[\s\S]*?static bool perms_denied\(void\)\n\{[\s\S]*?\n\}", MENU_C),
        "static const orb_app_t *s_app;   /* the app PG_APP is showing */",
        # The one piece of D12 state the shell owns: which group is open, -1 = none.
        "static int s_group_open = -1;",
        "static bool s_busy;",
        "static bool orb_jobs_busy(void) { return s_busy; }",
        lift(src, r"typedef struct \{\n    char\s+status\[72\];[\s\S]*?\} ui_t;", MENU_C),
        # The worker's answer half and the recorder it publishes through —
        # after the ui_t lift, which its signature reads.
        "static uint32_t g_decl[4], g_grnt[4], g_dmask[4];",
        "static int g_setg_calls, g_publish_calls, g_setg_slot, g_setg_on;",
        "static orb_cap_t g_setg_cap;",
        "static esp_err_t g_setg_rc;",
        "static char g_pub_line[72];",
        "static const char *esp_err_to_name(esp_err_t e)"
        " { static char b[16]; snprintf(b, sizeof b, \"0x%x\", (unsigned)e); return b; }",
        "static void worker_publish(const ui_t *src)"
        " { g_publish_calls++; snprintf(g_pub_line, sizeof g_pub_line, \"%s\", src->status); }",
    ]
    return consts, funcs

# The app registry owns what the menu is registered IN: the carousel order, the
# home entry, and the fact the router no longer names a single view. These were
# pinned here against the router's own INPUT_SCREEN_* enum until the registry
# replaced it (docs/app-platform-plan.md §1), so the lifts below moved to
# main/app/orb_app.c rather than being dropped: the contract the menu assumes
# ("rotate cycles me, long-press goes home") is still checked, and it still dies
# loudly if the table ever loses the menu row or the home flag.
APP_C = P("main", "app", "orb_app.c")
APP_H = P("main", "app", "orb_app.h")
for _p in (APP_C, APP_H):
    if not os.path.exists(_p):
        sys.exit("missing " + _p + " — the registry is where the menu's carousel "
                 "entry lives now; the lifts below have no fallback")
app_src = open(APP_C).read()

APP_TABLE = lift(app_src, r"static orb_app_t s_apps\[\] = \{[\s\S]*?\n\};", APP_C)
# Lifted from the roaming branch of the router's poll. It changed shape when the
# carousel landed (docs/app-platform-plan.md ruling C5): a turn now NAMES the
# next stop through orb_carousel_step() and the activate happens on the settle,
# so the block that must survive is "direction comes from the knob layer" and
# "the only switch is still set_screen(), which is where the M1 probe lives".
ARM_STEP = lift(router_src,
                r"const int step = knob_col_step\(delta\)[\s\S]*?"
                r"if \(next\) set_screen\(next, true\);", ROUTER_C)
# The carousel must keep asking the knob layer what "right" means. A screen that
# negates the delta itself is the bug host_knob_test.py exists for, and this lift
# dies loudly if the router ever goes back to reading a raw sign.
check("the screen carousel routes direction through the knob layer",
      "knob_col_step" in ARM_STEP, ARM_STEP.strip())
# The turn must be deferred, not immediate: a roaming detent that called
# set_screen() itself would put the flick's cost back to one full-page commit per
# detent, which is the only reason the carousel exists. The fallback branch is
# allowed exactly one set_screen, behind orb_carousel_ready().
check("a turn peeks instead of switching, and only the settle switches",
      "orb_carousel_step(" in ARM_STEP
      and ARM_STEP.count("set_screen(") == 1
      and "orb_carousel_ready()" in ARM_STEP, ARM_STEP.strip())
# And the settle must commit through the registry's own switch path, so the M1
# frame window still wraps every screen change the knob causes.
check("the settle commits through set_screen, not around it",
      re.search(r"orb_carousel_due\(now\)[\s\S]{0,160}orb_carousel_commit\(\);[\s\S]{0,60}"
                r"set_screen\(named, true\);", router_src) is not None)
# The menu must still be a walkable carousel entry, tagged as shell chrome
# rather than pretending to be an app it is not.
check("the menu is a registered walkable entry",
      '"menu"' in APP_TABLE and "ORB_APP_KIND_SHELL" in APP_TABLE)
# And the long-press's target must still be the dial, spelled as a table flag
# rather than as an enum value the router hardcodes.
check("the dial is the registered home entry",
      re.search(r'"clock"[\s\S]{0,500}\.is_home\s*=\s*true', APP_TABLE) is not None)

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
ENTRY_T = lift(th_hdr, r"typedef struct \{\n    char      name\[[\s\S]*?\} orb_theme_entry_t;", TH_H)

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
#include "orb_app.h"   /* orb_app_t: the fake app the shell renders */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
struct lv_obj_t; typedef struct lv_obj_t lv_obj_t;

#define ESP_OK 0
#define ESP_FAIL (-1)          /* the refused-write answer the D19 toggle's
                                * status line is checked against, spelled as
                                * esp_err.h spells it rather than as a bare -1 */
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

/* The lifted repaint is a no-op in the pure-state harness; the real one's only
 * observable effect is that it stamps s_painted_rows, so the stub mirrors that
 * alone. Without it the app-grew reap check would never fire (s_painted_rows
 * would sit at 0 forever), and deleting that check from the shipped
 * menu_view_service() would leave the whole suite green — the exact
 * unfalsifiable-fix repaint_lint was later added to catch. */
static int  s_painted_rows;      /* stamped by the stub below, read by the lifted service() */
static int  page_rows(void);     /* the lifted definition follows */
static int  g_repaints;
static void repaint(void) { g_repaints++; s_painted_rows = page_rows(); }
static void input_router_home(void) { }

/* The two DISPLAY rows ask the idle-dim layer what is in force. Stubbed here with
 * settable globals, the way the zone list stubs orb_net: the menu must show the
 * dim's real state and the timeout's real seconds, and which value counts as
 * "current" is exactly what the checks below pin. The presets themselves are not
 * stubbed — they are lifted from menu_view.c, so the rows this harness reads are
 * the rows the dial draws. */
static bool     g_dim_on  = true;
static uint32_t g_dim_to  = 600;
static bool     idle_dim_enabled(void)    { return g_dim_on; }
static uint32_t idle_dim_timeout_s(void)  { return g_dim_to; }

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

/* job_post() is stubbed below, after the shell's own state: the queue is
 * FreeRTOS, and what matters here is that the cursor moved (or did not), not
 * that a packet was queued. The kind enum is lifted, not restated, so a job
 * kind the menu starts posting but this file never learned about fails to
 * compile instead of passing quietly. */
static void (*g_posted_fn)(void *);
static int g_posted_pack, g_posts;

__CONSTS__

static page_t s_page = PG_LANDING;
static bool   s_armed;
static int    s_cur;
static ui_t   s_ui;
static bool   s_busy;   /* the shell worker's gate, faked below */
static bool   s_dirty;
static ui_t   s_shared;

static void job_post(void (*fn)(void *), int idx)
{
    /* The worker's own busy gate, not the menu's: orb_jobs_post answers
     * INVALID_STATE while a job is outstanding and job_post() swallows it, so
     * the fake has to drop the request the same way or the suite cannot tell a
     * row that respects it from one that posts twice. */
    if (s_busy) return;
    g_posted_fn = fn; g_posted_pack = idx; g_posts++;
}

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
/* A ring, not one static buffer: with a single buffer, every rowline() call in
 * one expression overwrites the last, so a check like
 *   !strcmp(rowline(1), "2 min") && !strcmp(rowline(6), "1 hour")
 * compares row 6 against both strings and can only pass by luck — and it passed
 * while the row it named was wrong. Sixteen slots is more than any single check
 * asks for; the count is not a tuning knob, going lower would reintroduce the
 * aliasing. */
static const char *rowline(int r) {
    static char b[16][96];
    static int i;
    char *const out = b[i = (i + 1) % 16];
    bool hi = false; row_text(r, out, 96, &hi);
    return out;
}

/* ---- the registry, as the menu sees it ------------------------------------
 * The menu's only contact with an app is through the orb_app_opt_* dispatchers,
 * so those are what a fake has to answer. The dispatchers themselves are lifted
 * and tested against the real registry in host_app_registry_test.py; restating
 * them here would test a copy. */
static char g_opt_label0[40], g_opt_val0[40], g_opt_label1[40], g_opt_val1[40];
static int  g_opt_n;
static int  g_arms;
static int  g_last_opt_sel = -1;
static size_t g_opt_app_count = 1;
static orb_app_t g_fake_app, g_fake_app2;
#define FAKE_APP (&g_fake_app)
#define FAKE_APP2 (&g_fake_app2)
bool orb_app_has_options(const orb_app_t *a) { (void)a; return g_opt_n > 0; }
int  orb_app_opt_count(const orb_app_t *a) { (void)a; return g_opt_n; }
const char *orb_app_opt_label(const orb_app_t *a, int i)
{ (void)a; return (i == 0) ? g_opt_label0 : g_opt_label1; }
const char *orb_app_opt_value(const orb_app_t *a, int i)
{ (void)a; return (i == 0) ? g_opt_val0 : g_opt_val1; }
void orb_app_opt_sel(const orb_app_t *a, int i) { (void)a; g_activations++; g_last_opt_sel = i; }
void orb_app_opt_arm(const orb_app_t *a, bool on) { (void)a; g_arms += on ? 1 : -1; }
/* D12's answer, off a settable table: the shape checks below are the whole point
 * of lifting the shell's nesting, and they need to hand it a page that nests.
 * g_kind_hook == 0 is the NULL hook — every row OPT_ROW, the page color_picker
 * still declares — and that regression is checked with the flag clear. The bounds
 * here mirror the real dispatcher in main/app/orb_app.c on purpose: the shell must
 * never be handed an index it did not count. */
static orb_opt_kind_t g_kind[24];
static int  g_nkind;
static bool g_kind_hook = true;
orb_opt_kind_t orb_app_opt_kind(const orb_app_t *a, int i)
{
    (void)a;
    if (!g_kind_hook || i < 0 || i >= g_nkind) return OPT_ROW;
    return g_kind[i];
}
size_t orb_app_count(void) { return g_opt_app_count; }
const orb_app_t *orb_app_at(size_t i)
{
    /* Only index 0 is a real entry. Past that there is nothing, so the row
     * after the app stays exit: a fake that ignored i would answer app_row(1)
     * with the same app and print its title where the word "exit" belongs. */
    if (!g_opt_app_count) return NULL;
    if (i == 0) return FAKE_APP;
    return (i == 1 && g_opt_app_count > 1) ? FAKE_APP2 : NULL;
}

/* ---- D19's side of the registry and the broker: the PERMISSIONS ladder reads
 * the raw rows (chrome included, because a row that filtered the shell out by
 * accident would pass every count check and still show MENU as an app), so
 * the fake table carries the kinds the shipped filter sorts on. */
static orb_app_t g_fake_shell;
static int g_app_rows_n = 3;
size_t orb_app_rows(void) { return (size_t)g_app_rows_n; }
const orb_app_t *orb_app_row(size_t i)
{
    /* Honours g_app_rows_n, not just the index: the worker's toggle writes by
     * slot number out of a pack, and the D19 guard that refuses a slot the
     * registry no longer has can only be tested if the table can actually
     * shrink from under it. */
    if ((int)i >= g_app_rows_n) return NULL;
    if (i == 0) return FAKE_APP;
    if (i == 1) return FAKE_APP2;
    if (i == 2) return &g_fake_shell;
    return NULL;
}
int orb_app_index_of(const orb_app_t *a)
{
    if (a == FAKE_APP) return 0;
    if (a == FAKE_APP2) return 1;
    if (a == &g_fake_shell) return 2;
    return -1;
}
/* declared is what the manifest asked for at install, grants is what the gate
 * will answer — the suite scripts them apart, because the menu's whole job on
 * these pages is showing the difference rather than one of them. */
uint32_t orb_svc_declared(const orb_app_t *a)
{
    const int i = orb_app_index_of(a);
    return i < 0 ? 0 : g_decl[i];
}
uint32_t orb_svc_grants(const orb_app_t *a)
{
    const int i = orb_app_index_of(a);
    return i < 0 ? 0 : g_grnt[i];
}
uint32_t orb_svc_denied_mask(const orb_app_t *a)
{
    const int i = orb_app_index_of(a);
    return i < 0 ? 0 : g_dmask[i];
}
const char *orb_svc_cap_name(orb_cap_t c)
{
    static const char *const N[8] = { "time", "config", "ui", "art",
                                      "power", "net", "sd_read", "themes" };
    return (unsigned)c < 8u ? N[c] : "?";
}
esp_err_t orb_svc_set_grant(const orb_app_t *a, orb_cap_t c, bool on)
{
    g_setg_calls++;
    g_setg_slot = orb_app_index_of(a);
    g_setg_cap = c;
    g_setg_on = on ? 1 : 0;
    if (g_setg_rc == ESP_OK && g_setg_slot >= 0)
    {
        if (on) g_grnt[g_setg_slot] |= 1u << (unsigned)c;
        else    g_grnt[g_setg_slot] &= ~(1u << (unsigned)c);
    }
    return g_setg_rc;
}

int main(void)
{
    /* The fake app carries a title so the header check means something. */
    g_fake_app.slug = "clock"; g_fake_app.title = "Clock";
    /* A distinct second app, so "a second app adds a row" cannot pass by
     * reading the first one's title twice. */
    g_fake_app2.slug = "hue"; g_fake_app2.title = "HSB";
    /* B3's third row: chrome that perm_rows must filter out by kind, not by
     * position — installed like an app, invisible to the ladder like chrome. */
    g_fake_shell.slug = "menu"; g_fake_shell.title = "Menu";
    g_fake_shell.kind = ORB_APP_KIND_SHELL; g_fake_shell.installed = true;
    g_fake_app.installed = true; g_fake_app2.installed = true;

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
    /* One fake app declaring two option rows, which is what earns it a line on
     * the top page. g_opt_n starts at 0 for the earlier unarmed checks. */
    g_opt_app_count = 1; g_opt_n = 2;
    snprintf(g_opt_label0, sizeof g_opt_label0, "diver");
    snprintf(g_opt_val0, sizeof g_opt_val0, "on the dial");
    snprintf(g_opt_label1, sizeof g_opt_label1, "neon"); g_opt_val1[0] = '\0';
    /* The OS's own three rows, then one per app that declares options, then
     * exit — the plan's §7.2. THEME is gone from the list because the shell no
     * longer knows what a theme is: "Clock" is there because the clock ANSWERS
     * opt_*, and it disappears again below when the hook stops answering. A
     * hardcoded IT_TOP_N could not show that, which is exactly why it was
     * listed as the thing to remove. */
    ck("top page is the OS's four rows, one app row, then exit", page_rows() == 6);
    { const char *r0=rowline(0),*r1=rowline(1),*r2=rowline(2),*r3=rowline(3),*r4=rowline(4),*r5=rowline(5);
      ck("row order AP/TZ/DISPLAY/PERMISSIONS/<app>/exit",
         !strcmp(r0,"AP") && !strcmp(r1,"TZ") && !strcmp(r2,"DISPLAY") &&
         !strcmp(r3,"PERMISSIONS") && !strcmp(r4,"Clock") && !strcmp(r5,"exit")); }
    /* The row comes from the registry, not from this file's text: an app that
     * stops answering opt_* loses its row without the shell being edited. */
    g_opt_n = 0;                                    /* the app stops answering */
    ck("an app with no options has no row, and exit moves up",
       page_rows() == 5 && !strcmp(rowline(4),"exit"));
    g_opt_n = 2;
    /* And a second app that declares options adds a second row, which is the
     * half IT_TOP_N could never do without an edit to the shell. */
    g_opt_app_count = 2;
    { const char *r3=rowline(3), *r4=rowline(4), *r5=rowline(5);
      ck("a second app declaring options adds a row, in table order",
         page_rows() == 7 && !strcmp(r3,"PERMISSIONS") && !strcmp(r4,"Clock") &&
         !strcmp(r5,"HSB")); }
    g_opt_app_count = 1;

    /* ---- the clamp, both ends, no wrap ---- */
    g_delta_down = knob_row_step(1);      /* derived, never assumed */
    ck("a list turn exists to ask for", g_delta_down != 0);
    s_cur = 0;
    turn_up(1);
    ck("rotate past the top clamps to 0", s_cur == 0);
    s_cur = 5;
    turn_down(1);
    ck("rotate past the bottom clamps to last", s_cur == 5);
    s_cur = 1;
    turn_down(99);
    ck("one detent per event, no flying", s_cur == 2);
    s_cur = 2;
    turn_up(99);
    ck("and not backwards either", s_cur == 1);
    menu_view_apply_input(0, false);
    ck("zero delta is not a move", s_cur == 1);

    /* ---- D19: the permissions ladder ----
     * Declare-at-install is the grant, so these pages approve nothing: they
     * show what each app declared, what still runs after the owner's toggles,
     * and where an app has been refused for calling past its declaration. The
     * shapes, strings and routing are lifted and pinned here; the press half
     * that walks the dial is activate() and lives on the bench. */
    s_armed = true; s_page = PG_TOP; s_cur = 0; s_busy = false;
    g_opt_app_count = 1; g_opt_n = 2;
    /* alpha asks for two caps, beta for one — the count a row shows is the
     * DECLARED set, not the four caps the page happens to list. */
    g_decl[0] = (1u << CAP_POWER) | (1u << CAP_NET);
    g_decl[1] = 1u << CAP_THEMES;
    g_grnt[0] = g_decl[0]; g_grnt[1] = g_decl[1];
    ck("no refusal, no mark on the PERMISSIONS row",
       !strcmp(rowline(3), "PERMISSIONS"));
    g_dmask[0] = 1u << CAP_POWER;
    ck("an app refused past its declaration marks the top row",
       (s_page = PG_TOP, !strcmp(rowline(3), "PERMISSIONS  *")));
    g_dmask[0] = 0;
    s_page = PG_PERMS; s_cur = 0;
    ck("the app list counts against what each declared and filters out chrome",
       page_rows() == 3 && !strcmp(rowline(0), "Clock  (2 of 2)") &&
       !strcmp(rowline(1), "HSB  (1 of 1)") && !strcmp(rowline(2), "<- back"));
    g_grnt[0] = 0;                            /* the owner took both back */
    ck("revoked caps read as live-zero, not as never-declared",
       (s_page = PG_PERMS, !strcmp(rowline(0), "Clock  (0 of 2)")));
    g_grnt[0] = g_decl[0];
    g_decl[0] = 0;                            /* an app that asked for nothing */
    ck("an app that declared nothing says so in words, not as a zero count",
       (s_page = PG_PERMS, !strcmp(rowline(0), "Clock  (nothing declared)")));
    g_decl[0] = (1u << CAP_POWER) | (1u << CAP_NET);
    g_dmask[1] = 1u << CAP_THEMES;
    ck("the refused app carries the mark into its own row too",
       (s_page = PG_PERMS, !strcmp(rowline(1), "* HSB  (1 of 1)")));
    g_dmask[1] = 0;
    s_page = PG_CAPS; s_perm_slot = 1; s_cur = 0;
    ck("the cap page is the four owner caps plus back, never the free four",
       page_rows() == 5 && !strcmp(rowline(0), "power: not declared") &&
       !strcmp(rowline(1), "net: not declared") &&
       !strcmp(rowline(2), "sd_read: not declared") &&
       !strcmp(rowline(3), "themes: allowed") &&
       !strcmp(rowline(4), "<- back"));
    ck("a declared cap the owner has taken back reads revoked, not denied",
       (s_page = PG_CAPS, s_perm_slot = 0, g_grnt[0] = (1u << CAP_NET),
        !strcmp(rowline(0), "power: revoked") &&
        !strcmp(rowline(1), "net: allowed")));
    g_dmask[0] = 1u << CAP_POWER;
    ck("the cap actually refused this boot is the one that carries the mark",
       (s_page = PG_CAPS, s_perm_slot = 0,
        !strcmp(rowline(0), "power: revoked *") &&
        !strcmp(rowline(1), "net: allowed")));
    g_dmask[0] = 0; g_grnt[0] = g_decl[0];
    g_dmask[0] = 1u << CAP_THEMES;      /* refused on a cap the clock never
                                         * declared: the row that says so is the
                                         * undeclared one, and the mark belongs
                                         * there most of all */
    ck("the mark rides the undeclared row too, where the divergence actually shows",
       (s_page = PG_CAPS, s_perm_slot = 0,
        !strcmp(rowline(3), "themes: not declared *")));
    g_dmask[0] = 0;
    ck("back from the cap page lands on the app's own row in the list",
       (s_page = PG_CAPS, s_perm_slot = 1, go_back(),
        s_page == PG_PERMS && s_cur == 1));
    ck("back from the app list lands on the PERMISSIONS row",
       (s_page = PG_PERMS, go_back(), s_page == PG_TOP && s_cur == IT_PERMS));
    {   /* One press, one toggle. activate() is not liftable, so the pack the
         * press builds is checked through the helper that builds it — and the
         * routing is checked where it can be: the press must stay on the page
         * rather than walk anywhere, because there is no confirm to reach. */
        s_page = PG_CAPS; s_perm_slot = 0; s_cur = 3;
        g_grnt[0] = g_decl[0];
        ck("a revoked cap's press asks for the grant, naming this app and cap",
           cap_pack(FAKE_APP, 3) == ((0 << 9) | ((int)CAP_THEMES << 1) | 1));
        s_cur = 1; g_grnt[0] = (1u << CAP_NET);   /* row 1 is net, which the
                                                   * row still shows as allowed:
                                                   * the press must ask to revoke
                                                   * the very cap it names */
        ck("an allowed cap's same press asks for the revoke",
           cap_pack(FAKE_APP, 1) == ((0 << 9) | ((int)CAP_NET << 1) | 0));
        ck("and the second app's pack carries its own slot, not the page's",
           (g_grnt[1] = 0, cap_pack(FAKE_APP2, 0) ==
            ((1 << 9) | ((int)CAP_POWER << 1) | 1)));
        ck("the four rows pack exactly the owner's four caps, in that order",
           ({ int bad = 0; const int w[4] = { CAP_POWER, CAP_NET, CAP_SD_READ,
                                              CAP_THEMES };
              for (int r = 0; r < 4; r++)
                  if (((cap_pack(FAKE_APP, r) >> 1) & 0xFF) != w[r]) bad++;
              bad == 0; }));
        g_grnt[1] = g_decl[1]; g_grnt[0] = g_decl[0];
        g_activations = 0;
        menu_view_apply_input(0, true);
        ck("the press routes to the action path and stays on the cap page",
           g_activations == 1 && s_page == PG_CAPS && s_cur == 1);
        /* The busy rule the whole menu shares: no press acts while the worker
         * is down, so a double-tap cannot queue two writes. */
        s_busy = true; g_activations = 0; g_posts = 0;
        menu_view_apply_input(1, false);
        menu_view_apply_input(0, true);
        ck("a press while the worker is busy moves nothing and posts nothing",
           g_activations == 0 && g_posts == 0 && s_cur == 1);
        s_busy = false;
    }
    {   /* The worker's answer half — lifted whole from the shipped file. */
        int pack = (0 << 9) | ((int)CAP_THEMES << 1) | 1;
        g_setg_rc = ESP_OK; g_setg_calls = 0;
        g_publish_calls = 0;
        job_grant(&pack);
        ck("the toggle persists exactly the grant it was handed, in the slot it names",
           g_setg_calls == 1 && g_setg_slot == 0 && g_setg_cap == CAP_THEMES &&
           g_setg_on == 1 && (g_grnt[0] & (1u << CAP_THEMES)) != 0 &&
           g_publish_calls == 1);
        ck("and the line it publishes names the cap and the new state",
           strstr(g_pub_line, "themes") && strstr(g_pub_line, "ALLOWED"));
        int pack2 = (1 << 9) | ((int)CAP_POWER << 1) | 0;
        g_setg_calls = 0; g_publish_calls = 0;
        job_grant(&pack2);
        ck("the revoke half reaches the broker as a revoke",
           g_setg_calls == 1 && g_setg_slot == 1 && g_setg_cap == CAP_POWER &&
           g_setg_on == 0 && !(g_grnt[1] & (1u << CAP_POWER)) &&
           g_publish_calls == 1);
        ck("and says revoked, not allowed, on the status line",
           strstr(g_pub_line, "revoked") && !strstr(g_pub_line, "ALLOWED"));
        g_setg_rc = ESP_FAIL; g_publish_calls = 0;
        job_grant(&pack);
        ck("a refused NVS write says not saved instead of claiming the toggle",
           g_publish_calls == 1 && strstr(g_pub_line, "not saved"));
        g_setg_rc = ESP_OK;
        /* The pack carries a slot number, and by the time the worker runs the
         * app it named can be uninstalled. Writing then would land the toggle
         * on whichever row slid into the number — so the answer is a line, not
         * a write. */
        g_app_rows_n = 0; g_setg_calls = 0; g_publish_calls = 0;
        job_grant(&pack);
        ck("a slot the table no longer has writes nothing and says the app is gone",
           g_setg_calls == 0 && g_publish_calls == 1 &&
           strstr(g_pub_line, "gone"));
        g_app_rows_n = 3;
        g_grnt[0] = 0; g_grnt[1] = 0;
    }

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

    /* ---- the app options page, rendered from the registry ---- *
     * The theme rows themselves are answered by the clock app (B2), so what is
     * checked here is the SHELL's half: that it renders an app's label and value
     * through one line, clamps on its own cursor, and puts "<- back" after the
     * app's rows whatever the app says. The answers are pinned against the
     * shipped clock_themes.c in tools/host_clock_opts_test.py. */
    /* The shell renders an app's rows without knowing what they mean (B1/B2).
     * g_opt_n/g_opt_v are the fake app's answers; the clock's real ones are
     * pinned against the shipped clock_themes.c in host_clock_opts_test.py. */
    s_page = PG_APP; s_app = FAKE_APP; s_cur = 0; g_opt_n = 2;
    snprintf(g_opt_label0, sizeof g_opt_label0, "diver"); snprintf(g_opt_val0, sizeof g_opt_val0, "on the dial");
    snprintf(g_opt_label1, sizeof g_opt_label1, "neon");   g_opt_val1[0] = '\0';
    ck("an app row reads the app's label and value",
       !strcmp(rowline(0), "diver  (on the dial)"));
    ck("a row with no value reads the label alone", !strcmp(rowline(1), "neon"));
    ck("the app page ends with the shell's back row", !strcmp(rowline(2), "<- back"));
    ck("the shell counts the app's rows plus its own back row", page_rows() == 3);
    ck("the header is the app's own title", (s_app->title && !strcmp(s_app->title, "Clock")));
    ck("back from an app page returns to that app's row",
       (s_page = PG_APP, go_back(), s_page == PG_TOP && s_cur == IT_OS_N));

    /* ---- D12: one level of app-owned nesting, validated by the shell ----
     * The page is the clock's shape: a plain Factory default row, one GROUP
     * header, and the card's themes as its ITEMs. What is checked is the shell's
     * half — what gets drawn, where the cursor may go, and what the two
     * malformed shapes become — because the app that owns the answer must never
     * also own the owner's ability to reach a row. */
    s_armed = true; s_page = PG_APP; s_app = FAKE_APP; s_cur = 0;
    g_opt_n = 5; g_nkind = 5; g_kind_hook = true;
    g_kind[0] = OPT_ROW; g_kind[1] = OPT_GROUP;
    g_kind[2] = g_kind[3] = g_kind[4] = OPT_ITEM;
    snprintf(g_opt_label1, sizeof g_opt_label1, "meadow");
    snprintf(g_opt_val1, sizeof g_opt_val1, "1 of 3: meadow");
    s_group_open = -1;
    ck("a closed group leaves the plain row, its header and the back row",
       page_rows() == 3 && vis_to_row(5, 0) == 0 && vis_to_row(5, 1) == 1
       && vis_to_row(5, 2) == -1);
    ck("an item of a shut group is hidden, the header is not",
       row_hidden(5, 2) && row_hidden(5, 4) && !row_hidden(5, 1));
    ck("the shut header answers with the app's own summary",
       !strcmp(rowline(1), "meadow  (1 of 3: meadow)"));
    ck("pressing the header opens it", group_press(5, 1) && s_group_open == 1);
    ck("open, its items are on the glass and counted",
       page_rows() == 6 && vis_to_row(5, 4) == 4 && !row_hidden(5, 4));
    ck("an open header says so instead of the summary",
       (s_cur = 1, !strcmp(rowline(1), "meadow  (open)")));
    ck("an item is indented, so nesting is on the glass and not only in the ABI",
       !strncmp(rowline(2), "  ", 2));
    g_opt_val1[0] = '\0';                     /* an item with no value: the other branch */
    ck("a valueless item is indented too", !strncmp(rowline(2), "  ", 2));
    snprintf(g_opt_val1, sizeof g_opt_val1, "1 of 3: meadow");
    ck("the hint names the gesture that leaves an open group",
       (s_page = PG_APP, !strcmp(page_hint(), "rotate: choose   push: change   hold: close")));
    s_cur = 2;
    for (int t = 0; t < 6; t++) turn_down(1);      /* one row per event, by design */
    ck("inside an open group the cursor clamps at its last item", s_cur == 4);
    for (int t = 0; t < 6; t++) turn_up(1);
    ck("and at its first, so the header cannot be escaped by turning", s_cur == 2);
    ck("the group's own position maps back to the app's row number",
       row_to_vis(5, 2) == 2 && row_to_vis(5, 4) == 4 && row_to_vis(5, 1) == 1);
    s_cur = 3;
    ck("hold closes the group, stays on the page, and parks on the header",
       menu_view_opt_long() && s_group_open == -1 && s_page == PG_APP && s_cur == 1);
    s_group_open = -1; s_cur = 0;
    ck("with nothing open the hold is the router's to spend on going home",
       !menu_view_opt_long());
    ck("closing the group gives the page its three rows back", page_rows() == 3);
    /* The two malformed shapes, and the one answer that keeps them reachable. */
    g_kind[0] = OPT_ITEM;                       /* a leading ITEM, with nothing above it */
    s_group_open = -1;
    ck("a leading item with no header is drawn as a row, not swallowed",
       !row_hidden(5, 0) && vis_to_row(5, 0) == 0 && page_rows() == 3);
    /* The same row, judged while a group IS open: an orphan item and a hidden item
     * are only distinguishable here, because hidden means "owner's group is shut"
     * and an orphan has no owner to shut. Mutating drawn_kind's orphan branch to
     * OPT_ITEM hides the row in this state and nowhere else. */
    s_group_open = 1;
    ck("an orphan item stays drawn while another group is open",
       !row_hidden(5, 0) && visible_rows(5) == 5);
    s_group_open = -1;
    g_kind[0] = OPT_ROW; g_kind[2] = OPT_ROW; g_kind[3] = OPT_ROW;
    ck("an item with no group above it is a row too",
       !row_hidden(5, 2) && !row_hidden(5, 3));
    g_kind[2] = g_kind[3] = g_kind[4] = OPT_ITEM;
    /* The NULL hook is the whole pre-D12 ABI: color_picker declares no options and
     * every app written before nesting answered nothing. Nothing may disappear and
     * nothing may indent when there are no groups to ask about. */
    g_kind_hook = false; g_opt_n = 4; g_nkind = 4;
    s_group_open = -1; s_cur = 0;
    ck("a NULL-hook page is flat: every row counted, none hidden",
       page_rows() == 5 && !row_hidden(4, 0) && !row_hidden(4, 3));
    for (int t = 0; t < 3; t++) turn_down(1);
    ck("and its cursor reaches the last row and the back row", s_cur == 3);
    g_kind_hook = true; g_nkind = 0; g_opt_n = 2; s_group_open = -1; s_cur = 0;

    /* ---- the app's own scan landing must repaint the shell (B2 reachability) --
     * Entering the theme page posts the clock's card walk; that walk reaps in
     * orb_app_service_all() before menu_service runs, so the app already has its
     * rows and the menu's OWN s_dirty is still false. If menu_view_service()
     * repainted only on s_dirty, the page would open empty and only fill once
     * the owner turned the knob. g_repaints is the witness: the count going up
     * with s_dirty clear is exactly the fixed behaviour, and deleting the
     * app_grew re-check from the shipped file drops it back to no repaint. */
    g_opt_n = 0; s_page = PG_APP; s_app = FAKE_APP; s_cur = 0; s_armed = true;
    s_dirty = false;
    repaint();                                 /* stamp the settled baseline */
    const int base_paints = g_repaints;
    g_opt_n = 4;                               /* the walk landed: rows appeared */
    s_dirty = false;                           /* ... and the menu's worker is idle */
    menu_view_service();
    ck("the app's scan landing repaints with the menu's own flag clear",
       g_repaints > base_paints && page_rows() == 5);
    menu_view_service();
    ck("and settles once it has caught up", g_repaints == base_paints + 1);
    g_opt_n = 2;
    /* Entered properly: the go_back() above left PG_TOP with s_app cleared, and
     * an app page that was never entered proves nothing about leaving one. */
    g_opt_n = 0; s_page = PG_APP; s_app = FAKE_APP; s_cur = 0;
    ck("an app page whose app answers nothing still lets you out", page_rows() == 1);
    ck("that one row is the back row", !strcmp(rowline(0), "<- back"));
    g_opt_n = 2;

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

    /* ---- the DISPLAY submenu: the dim switch and the timeout picker ----
     * These two rows are the only way to reach the idle dim on the dial, and the
     * failure modes are invisible on a bench: a picker that marks the wrong preset
     * tells the owner a 10 minute timer is in force while the panel actually dims
     * after 45 seconds. */
    s_page = PG_DISPLAY; s_cur = 0; g_dim_on = true; g_dim_to = 600;
    ck("display page has dim, timeout and back", page_rows() == 3);
    ck("the dim row reads the switch", !strcmp(rowline(0), "dim after idle:  on"));
    ck("the dim row follows the switch off",
       (g_dim_on = false, !strcmp(rowline(0), "dim after idle:  off")));
    g_dim_on = true;
    ck("the timeout row reads the preset in force", !strcmp(rowline(1), "timeout:  10 min"));
    ck("a timeout no preset covers reads the truth, not the nearest preset",
       (g_dim_to = 45, !strcmp(rowline(1), "timeout:  45 s")));
    g_dim_to = 600;
    ck("the display page ends with back", !strcmp(rowline(2), "<- back"));
    ck("the display page offers push, not just rotate",
       (s_page = PG_DISPLAY, !strcmp(page_hint(), "rotate: choose   push: change")));
    ck("back from display returns to the DISPLAY row",
       (s_page = PG_DISPLAY, go_back(), s_page == PG_TOP && s_cur == IT_DISPLAY));

    s_page = PG_TO; s_cur = 0;
    ck("the picker offers every preset plus back", page_rows() == TO_PRESET_N + 1);
    /* Rows that are not the current one, so the label is compared bare: the row in
     * force carries the "  *" mark and is pinned on its own below. */
    ck("the picker rows carry the preset labels",
       !strcmp(rowline(1), "2 min") && !strcmp(rowline(6), "1 hour"));
    ck("the preset in force is marked",
       (g_dim_to = 600, !strcmp(rowline(3), "10 min  *")));
    ck("a preset that is not in force is not marked", !strcmp(rowline(0), "1 min"));
    ck("a timeout no preset covers marks nothing",
       (g_dim_to = 45, !strcmp(rowline(3), "10 min")));
    g_dim_to = 600;
    ck("the picker ends with back", !strcmp(rowline(TO_PRESET_N), "<- back"));
    ck("the picker page says what rotate and push do",
       (s_page = PG_TO, !strcmp(page_hint(), "rotate: choose   push: set")));
    s_cur = TO_PRESET_N;
    turn_down(1);
    ck("the picker clamps at its back row", s_cur == TO_PRESET_N);
    ck("back from the picker lands on the timeout row, not the top page",
       (s_page = PG_TO, go_back(), s_page == PG_DISPLAY && s_cur == 1));

    /* ---- a worker result landing while disarmed must NOT paint a list ---- */
    s_armed = true; s_page = PG_TOP; s_cur = 0;
    memset(&s_shared, 0, sizeof s_shared);
    snprintf(s_shared.status, sizeof s_shared.status, "zone set: Europe/Budapest");
    s_dirty = true;
    s_armed = false;                       /* the user rotated away mid-job */
    menu_view_service();
    ck("a landing never becomes a list because a result landed", page_rows() == 0);
    ck("the landing keeps its title", s_page == PG_TOP);


    s_armed = true; s_page = PG_TOP; s_cur = 0;
    memset(&s_shared, 0, sizeof s_shared);
    snprintf(s_shared.status, sizeof s_shared.status, "zone set: Europe/Budapest");
    s_dirty = true;
    menu_view_service();
    /* Armed, the same result must NOT move the page: menu_view_service() used to
     * jump to the theme confirm screen whenever a stage landed, which yanked a
     * page out from under an owner who had already scrolled elsewhere. Whether a
     * staged theme deserves a question is the clock's decision now; the shell's
     * job is to repaint and stay put. */
    ck("armed, a landing result does not change the page", s_page == PG_TOP);
    ck("armed, the result line is taken from the worker",
       !strcmp(s_ui.status, "zone set: Europe/Budapest"));

    printf(fails ? "HARNESS FAILURES %d\n" : "HARNESS OK\n", fails);
    return fails ? 1 : 0;
}
"""


# The two caps the shipped orb_theme_entry_t is sized with, lifted from the header
# alongside the struct itself: the row prints the name and addresses the theme by its
# id, and a harness that hardcoded either length would keep passing while the device's
# struct grew past it.
ENTRY_DEFS = "\n".join(lift(th_hdr, r"#define " + k + r"\s+\d+[^\n]*", TH_H)
                      for k in ("ORB_THEME_NAME_MAX", "ORB_THEME_ID_HEX"))

def assembled(src):
    """The harness source: the shipped types and functions, spliced verbatim."""
    consts, funcs = lifts(src)
    body = "\n".join(consts[:2] + [ENTRY_DEFS, ENTRY_T] + consts[2:])
    return (HARNESS
            .replace("__CONSTS__", body + "\n" + KNOB_HELPERS)
            .replace("__FUNCS__", "\n".join(funcs))
            .replace("__SLUGDEF__", "")
            .replace("__UNUSED_OK__", ""))


def build_and_run(src):
    import tempfile
    d = tempfile.mkdtemp(prefix="hostmenu_")
    # orb_app.h reaches for esp_err.h, which does not exist on a host. The two
    # stubs the registry header needs are written into the build dir so the
    # harness reads the SHIPPED orb_app.h rather than a restated struct — a
    # hand-copied orb_app_t would stay green while the device's field order
    # moved underneath it.
    for n, body in (("esp_err.h", "#pragma once\ntypedef int esp_err_t;\n"
                                   "#define ESP_OK 0\n"),
                    ("esp_log.h", "#pragma once\n#define ESP_LOGI(t,...)\n")):
        open(os.path.join(d, n), "w").write(body)
    exe = os.path.join(d, "h")
    cf = os.path.join(d, "h.c")
    with open(cf, "w") as f:
        f.write(src)
    r = subprocess.run(["cc", "-O0", "-std=gnu11", "-Wall", "-Wextra",
                        "-Wno-unused-parameter", "-Wno-unused-function",
                        "-Werror", "-I", d, "-I", P("main","app"), "-o", exe, cf],
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


BLOCKING = ("orb_theme_scan(", "orb_theme_install(")
THEME_C = P("main","art","orb_theme.c")
CT_C = P("main","clock","clock_themes.c")
JOBS_C = P("main","app","orb_jobs.c")
WIDGET = r"lv_label_set_text\(|lv_obj_set_style_|lv_obj_invalidate\("


def threading_lint(src):
    """No blocking card call may sit in a function that also touches a widget.

    input_router's poll is an lv_timer on the LVGL task and the only reader of the
    knob, so a card call there freezes the dial and swallows input for as long as
    the card mutex is held elsewhere — 38-48 s behind a browser upload, measured.
    The card work used to sit in this file's own job_worker(); B2 gave the theme
    picker to the clock app, so the same rule is now checked across the shipped
    clock_themes.c (which issues the calls) and orb_jobs.c (which owns the one
    worker they run on). A call that migrates back into a widget-touching function
    is the regression this catches.
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


def repaint_lint(src):
    """The landing must stay a landing when a worker result lands.

    The state machine above cannot see this: page_rows() already answers 0 while
    unarmed, so deleting the guard inside repaint() leaves every pure check green
    while the dial shows a title card wearing a menu's hint and status line — the
    screen looks interactive with nothing in it, which is the report "the knob
    stopped working" that this feature already had once. So the guard is read out
    of the shipped function rather than reimplemented here.
    """
    code = _code_only(src)
    m = re.search(r"static void repaint\(void\)\s*\{([\s\S]*?)\n\}", code)
    if not m:
        return ["repaint() is not a column-0 static any more; the landing rule cannot be checked"]
    body = m.group(1)
    bad = []
    if "if (!s_armed)" not in body:
        bad.append("repaint() no longer branches on s_armed, so a landed result can paint a list on the title card")
        return bad
    # The unarmed branch is the whole guard: it must restore the landing's own
    # title and hint, in that branch, before the else. Comparing the text cannot
    # work — _code_only() blanks string literals, so "MENU" is not findable and
    # never will be. What is findable is that the landing sets a big font and the
    # armed path sets the small one: swap them and the title card is gone.
    arm = body.index("if (!s_armed)")
    el = body.index("} else {", arm)
    unarmed, armed = body[arm:el], body[el:]
    if "lv_label_set_text(s_title" not in unarmed:
        bad.append("the unarmed branch no longer sets the landing's title")
    if "montserrat_48" not in unarmed:
        bad.append("the landing's title lost its large font, so a title card and a list header are now indistinguishable")
    if "montserrat_48" in armed:
        bad.append("the armed branch sets the large title, which is the landing's")
    if "s_ui.status" not in armed:
        bad.append("the result line is no longer written by the armed branch alone")
    return bad


def worker_lint(menu_src, ct=None):
    """The card calls run on the shell's one worker, whoever posted them.

    B2 moved the theme picker out of this file, and ruling D11 moved the install
    into orb_theme.c so the dial's picker and the browser's Load button post the
    identical job. So the pair that used to be checked inside menu_view.c's own
    job_worker() is now checked where it lives: the clock's job_*() walks the card,
    orb_theme.c's install job copies a container, and neither runs on the task that
    paints the dial. Two worker tasks would also be caught here, because the shell
    owns the only xTaskCreate below main.c's ladder and an app growing a private one
    is the duplication the shell worker exists to prevent.
    """
    bad = []
    ct = ct if ct is not None else open(CT_C).read()
    code = _code_only(ct)
    jobs = re.findall(r"static void (job_\w+)\(void \*arg\)\s*\{([\s\S]*?)\n\}", code)
    if not jobs:
        return ["clock_themes.c has no job_*() functions — the card work has no worker"]
    body = "".join(b for _, b in jobs)
    if "orb_theme_scan(" not in body:
        bad.append("no card job in clock_themes.c walks the card (orb_theme_scan)")
    if "orb_jobs_post(" not in code:
        bad.append("clock_themes.c never posts to the shell worker")
    # Ruling D11: the picker answers with a post, never with the install itself. A
    # direct call here would run a multi-second SD-to-flash copy on the LVGL task.
    if re.search(r"\borb_theme_install\s*\(", code):
        bad.append("clock_themes.c calls orb_theme_install() directly instead of "
                   "posting it — the copy would run on the task that paints the dial")
    if "orb_theme_post_install(" not in code:
        bad.append("clock_themes.c does not post the one install entry the page uses either")
    # A widget call inside a job would paint from the worker task.
    if re.search(WIDGET, body):
        bad.append("a card job in clock_themes.c touches a widget")
    # The install job itself, in the layer that owns the flash. Posted, not called.
    th = _code_only(open(THEME_C).read())
    tj = re.findall(r"static void (\w*job\w*)\(void \*arg\)\s*\{([\s\S]*?)\n\}", th)
    if not tj:
        bad.append("orb_theme.c has no job function — nothing runs the install off the caller")
    tjb = "".join(b for _, b in tj)
    if "orb_theme_install(" not in tjb:
        bad.append("orb_theme.c's job does not call orb_theme_install() — the copy has no worker")
    if "orb_jobs_reboot_soon(" not in tjb:
        bad.append("orb_theme.c's install job never asks for the reboot that draws it (D11)")
    if "orb_jobs_post(" not in th:
        bad.append("orb_theme.c never posts to the shell worker")
    # The menu must not have grown a private task back.
    if "xTaskCreate" in _code_only(menu_src):
        bad.append("menu_view.c created its own worker task again")
    if "xTaskCreate" not in _code_only(open(JOBS_C).read()):
        bad.append("orb_jobs.c owns no task — nothing runs the posted jobs")
    return bad


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
     "    if (orb_jobs_busy()) return;\n\n    if (pressed)", "    if (0) return;\n\n    if (pressed)"),
    # The bug that made a theme page unreachable-but-not-obvious: the app's scan
    # lands with the menu's own worker idle, so a service() that repaints only on
    # s_dirty opens the page empty. Forcing that back must red the new check.
    ("the app's scan landing no longer repaints the shell",
     "    const bool own_result = s_dirty;\n    if (!own_result && !app_grew) return;",
     "    const bool own_result = s_dirty;\n    (void)app_grew;\n    if (!own_result) return;"),
    ("page_rows forgets that unarmed means no list",
     "if (!s_armed) return 0;", "if (0) return 0;"),
    ("the back row is dropped from the zone list",
     "case PG_TZ:     return orb_net_tz_count() + 1;", "case PG_TZ:   return orb_net_tz_count();"),
    # The theme list's own rows left for the clock app (B2), so the two pins that
    # watched them now watch the shell's rendering of ANY app's answer: drop the
    # back row and the owner is trapped; drop the value and the page stops saying
    # which theme is on the dial. host_clock_opts_test.py pins the answers.
    ("the back row is dropped from the app options page",
     "case PG_APP:    return s_app ? visible_rows(orb_app_opt_count(s_app)) + 1 : 0;",
     "case PG_APP:    return s_app ? visible_rows(orb_app_opt_count(s_app)) : 0;"),
    ("an app's value stops being rendered",
     'const char *v = (k == OPT_GROUP && s_group_open == ar) ? "open"\n                      : orb_app_opt_value(s_app, ar);',
     'const char *v = (k == OPT_GROUP && s_group_open == ar) ? "open"\n                      : NULL;'),
    # D12's own pins. Each is one way the shell can get its one level of nesting
    # wrong, and each must turn one of the shape checks above red.
    ("a closed group stops hiding its items",
     "    if (drawn_kind(rows, idx) != OPT_ITEM) return false;\n"
     "    return owner_group(rows, idx) != s_group_open;",
     "    return false;"),
    ("an item with no header above it disappears instead of being drawn",
     "    return (owner_group(rows, idx) < 0) ? OPT_ROW : OPT_ITEM;",
     "    return OPT_ITEM;"),
    ("the cursor escapes an open group",
     "            if (lo >= 0 && hi >= lo) {\n                if (next < lo) next = lo;\n"
     "                if (next > hi) next = hi;\n            }",
     "            if (0) { }"),
    ("the long press goes home instead of closing the group",
     "    if (!s_armed || s_page != PG_APP || !s_app || s_group_open < 0) return false;",
     "    return false;"),
    ("an open header loses the fact that it is open",
     'const char *v = (k == OPT_GROUP && s_group_open == ar) ? "open"',
     'const char *v = (k == OPT_GROUP && s_group_open == ar) ? ""'),
    ("items lose the indent that says they are inside a header",
     'snprintf(out, outsz, "%s%s", k == OPT_ITEM ? "  " : "",',
     'snprintf(out, outsz, "%s%s", "",'),
    ("the current zone stops being marked",
     "const bool cur = (row == orb_net_tz_current_index());", "const bool cur = false;"),
    ("back from the zone list loses its cursor position",
     "s_cur  = (from == PG_TZ)     ? IT_TZ\n           : (from == PG_DISPLAY)? IT_DISPLAY\n           : (from == PG_PERMS)  ? IT_PERMS\n           : 0;",
     "s_cur  = (from == PG_TZ) ? 0 : (from == PG_DISPLAY) ? 0 : (from == PG_PERMS) ? 0 : 0;"),
    ("a worker result paints a list onto the title card",
     "if (!s_armed) {\n        /* Rows are already blank", "if (0) {\n        /* Rows are already blank"),
    # Cross-file: the scan now lives in the clock app. Painting from the posted
    # job is the same regression the menu's own worker used to guard, so it is
    # mutated where the call sits.
    ("the card scan moves back onto the LVGL task",
     "        const int n = orb_theme_scan(out.e, THEME_MAX);",
     "        const int n = orb_theme_scan(out.e, THEME_MAX); lv_label_set_text(NULL, \"x\");",
     CT_C),
    # The DISPLAY submenu. Each of these is a way the two rows can lie about the
    # setting they claim to show, which is exactly the failure nobody notices until
    # the panel dims at the wrong time.
    ("the DISPLAY row drops off the top menu",
     'case IT_DISPLAY: snprintf(out, outsz, "DISPLAY"); break;', ''),
    ("the DISPLAY page loses a row",
     "case PG_DISPLAY:return 3;", "case PG_DISPLAY:return 2;"),
    ("the dim row stops reading the switch it is showing",
     'idle_dim_enabled() ? "on" : "off"', 'true ? "on" : "off"'),
    ("the timeout preset in force stops being marked",
     "const bool cur = (s_to_preset[row].s == idle_dim_timeout_s());",
     "const bool cur = false;"),
    ("an off-preset timeout reads the nearest preset instead of the truth",
     'snprintf(out, outsz, "%lu s", (unsigned long)cur);',
     'snprintf(out, outsz, "%s", s_to_preset[0].lbl);'),
    ("the timeout picker loses its back row",
     "case PG_TO:     return TO_PRESET_N + 1;", "case PG_TO:     return TO_PRESET_N;"),
    ("back from the picker loses its place in the parent",
     "s_page = PG_DISPLAY;\n        s_cur  = 1;", "s_page = PG_DISPLAY;\n        s_cur  = 0;"),
    ("a preset label drifts from what ships",
     '{   600, "10 min" },', '{   600, "10 minutes" },'),
    ("D19: the app list loses its back row",
     "case PG_PERMS:  return perm_rows() + 1;", "case PG_PERMS:  return perm_rows();"),
    ("D19: the chrome filter flips and lists the shell as an app",
     "if (a && a->kind == ORB_APP_KIND_APP && a->installed) n++;",
     "if (a && a->installed) n++;"),
    # The two facts the ladder exists to keep apart: what the app declared at
    # install, and what the owner has left running. Each pin breaks one of them
    # and only one, so the suite's (n of m) line is what catches it.
    ("D19: the declared count stops being the declared count",
     "if (dm & (1u << OWNER_CAPS[c])) nd++;",
     "if (gm & (1u << OWNER_CAPS[c])) nd++;"),
    ("D19: an app that declared nothing reads as a zero instead of a sentence",
     "if (nd == 0)", "if (false)"),
    ("D19: a cap the app never declared stops saying so",
     "if (!dcl) {", "if (false) {"),
    ("D19: the refusal mark stops reading the mask it is meant to show",
     "if (a && a->kind == ORB_APP_KIND_APP && orb_svc_denied_mask(a)) return true;",
     "if (a && a->kind == ORB_APP_KIND_APP && false) return true;"),
    ("D19: the refusal mark is detached from the mask it reads",
     "const bool ref = (orb_svc_denied_mask(a) & bit) != 0;",
     "const bool ref = false;"),
    # The press builds the pack through cap_pack() now, so the two shifts that
    # decide which app and which cap the worker writes are pinned in the helper
    # the suite lifts and calls directly.
    ("D19: the toggle's slot field loses a shift and lands on another app",
     "return (orb_app_index_of(a) << 9) | ((int)cap << 1) | (on ? 1 : 0);",
     "return (orb_app_index_of(a) << 8) | ((int)cap << 1) | (on ? 1 : 0);"),
    ("D19: the toggle stops reading the state it is showing and always allows",
     "const bool on = !(orb_svc_grants(a) & (1u << cap));",
     "const bool on = true;"),
    ("D19: the worker decodes the cap from the wrong end of the pack",
     "const int slot = pack >> 9, cap = (pack >> 1) & 0xFF, on = pack & 1;",
     "const int slot = pack >> 9, cap = (pack >> 2) & 0xFF, on = pack & 1;"),
    ("D19: a refused NVS write lets the row claim the toggle anyway",
     '\\"%s\\": %s not saved (%s)', '\\"%s\\": %s %s (%s)'),
    ("D19: the worker writes a slot the registry no longer has",
     "if (!a) { snprintf(out.status, sizeof out.status, \"that app is gone\"); worker_publish(&out); return; }",
     "if (false) { snprintf(out.status, sizeof out.status, \"that app is gone\"); worker_publish(&out); return; }"),
]


def run_mutations():
    rc = 0
    for entry in MUTATIONS:
        name, old, new, target = (entry + (MENU_C,))[:4] if len(entry) == 4 else (*entry, MENU_C)
        shipped = open(target).read()
        if shipped.count(old) != 1:
            print("FAIL  mutation %-54r target appears %d times in %s — the pin is stale"
                  % (name, shipped.count(old), os.path.relpath(target, ROOT)))
            rc = 1
            continue
        mutated = shipped.replace(old, new, 1)
        if target == MENU_C:
            ok_c, out_c = build_and_run(assembled(mutated))
        else:
            # A cross-file pin cannot be spliced into the menu harness; it is
            # checked by the lints that read that file, which is exactly how the
            # "painting from the worker" class of bug is caught.
            ok_c, out_c = True, ""   # judged by the lints over the mutated file
        # Every check the suite runs must be in this disjunction, or a pin can
        # "survive" simply because the lint that would catch it is not consulted
        # here — which is exactly what repaint_lint did until it was added.
        caught = ((not ok_c) or ("FAIL" in out_c)
                  or bool(threading_lint(mutated))
                  or bool(repaint_lint(mutated))
                  or bool(worker_lint(shipped,
                                      mutated if target == CT_C else None)))
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
    rbad = repaint_lint(menu_src)
    check("a landed result cannot paint a list onto the title card",
          not rbad, "; ".join(rbad))
    wbad = worker_lint(menu_src)
    check("the worker task still owns both card calls", not wbad, "; ".join(wbad))
    check("rotate still owns the screen swap (router)",
          "if (!screen_armed(s_current))" in _code_only(router_src))
    check("long-press goes home to the clock (router)",
          re.search(r"if \(lpress\) \{\s*orb_carousel_cancel\(\);\s*disarm_all\(\);"
                    r"\s*set_screen\(orb_app_home\(\), true\);", router_src) is not None)
    # D12's one exception, in both halves: the entry gets first refusal, and the
    # home path survives underneath it. An entry with nothing open answers false and
    # the hold homes exactly as C8 rules, so a nesting feature that quietly swallowed
    # the escape hatch — or a router that never asks — is a red check here rather
    # than a dial somebody has to unplug.
    check("the router offers the long press to the entry before it homes",
          re.search(r"if \(lpress && orb_app_opt_long\(s_current\)\) \{\s*lpress = false;"
                    r"\s*\}\s*if \(lpress\) \{", router_src) is not None
          and "(*opt_long)(orb_app_t *app)" in open(APP_H).read())
    check("the shell entry wires that hook to the menu",
          re.search(r"\.opt_long\s*=\s*menu_long", app_src) is not None
          and re.search(r"static bool menu_long\(orb_app_t \*app\)\n\{\n[^}]*"
                        r"menu_view_opt_long\(\);", app_src) is not None)
    # Inverted since the registry landed: the router must NOT name a single view
    # any more, and the registry must name all three. The old form of this check
    # required menu_view_screen() to appear in input_router.c, which is precisely
    # the coupling docs/app-platform-plan.md §1 sets out to remove.
    check("the registry, not the router, names the views",
          all(n in app_src for n in ("menu_view_screen()", "menu_view_service()",
                                     "color_picker_screen()", "clock_view_screen()"))
          and not any(n in _code_only(router_src)
                      for n in ("menu_view_", "color_picker_", "clock_view_")))
    check("main.c builds the apps through the registry, not by name",
          "orb_app_init_all()" in open(P("main", "main.c")).read()
          and "clock_view_init()" not in _code_only(open(P("main", "main.c")).read()))
    check("picker gained the same two-level gate",
          "bool color_picker_armed(void)" in open(P("main", "color", "color_picker.c")).read()
          and "if (!s_armed) return;" in open(P("main", "color", "color_picker.c")).read())

    print("all host checks pass" if not FAILS else "%d FAILED" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

