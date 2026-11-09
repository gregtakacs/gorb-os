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
  s_pages[]            the page table (P5.2): the single inventory of what each
                       page IS - its count, labels, title, hint, press and back
                       target - plus the structural lints over its text below
  page_rows()          one line of that table plus the armed gate: the single
                       authority for "how many rows exist"
  activate()           the press, liftable for the first time: the back row and
                       the top page's exit are answered before the page's act()
                       hears about the press, and the press checks below run
                       the shipped dispatcher instead of a counter
  go_back()            which row the cursor lands on when a sub-page is left,
                       read from the table: the parent's page row, or the
                       ordinal function where the parent's list moves under it
  label_*()            the labels, including the D19
                       counts, and the DISPLAY submenu's two rows (the dim
                       switch reads the real enable, the timeout row reads the
                       real seconds)
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
    # The model block, whole: the page ids, the navigation contract, both
    # structs, the table's linkage and the page cursor. One lift - the suite
    # reads the same inventory the dispatcher does, so a rename that splits
    # the two dies here at compile time instead of passing quietly against a
    # restated copy.
    model = lift(src, r"typedef enum \{\n    PG_LANDING[\s\S]*?"
                      r"#define TOP_EXIT [^\n]*", MENU_C)
    # The prototypes the table's rows name. Lifted, not restated: a hook the
    # shipped code renamed would otherwise compile against a harness-only
    # declaration and test a function no longer wired to anything.
    protos = lift(src, r"static int  rows_top\(void\);[\s\S]*?"
                       r"static void activate\(void\);", MENU_C)
    # The DISPLAY submenu's timeout ceiling, lifted rather than restated: the range
    # the knob edits IS the setting the owner chooses (docs/knob-edit-plan.md).
    # timeout_label is the same story one level up.
    presets = lift(src, r"#define TO_MIN_MAX[^\n]*", MENU_C)
    f_timeout = lift(src, r"static void timeout_label\(char \*out, size_t outsz\)\n\{[\s\S]*?\n\}", MENU_C)
    funcs = [
        lift(src, r"static int app_rows\(void\)\n\{[\s\S]*?\n\}", MENU_C),
        lift(src, r"static const orb_app_t \*app_row\(int i\)\n\{[\s\S]*?\n\}", MENU_C),
        f_timeout,
        # D12's shape predicates, lifted as a block: the app page's count and
        # label lean on them, and a harness that restated any one of them
        # would keep passing while the shipped shell hid the wrong rows.
        lift(src, r"static int owner_group\(int rows, int idx\)\n\{[\s\S]*?"
                   r"static void shape_check\(int rows\)\n\{[\s\S]*?\n\}", MENU_C),
        # D12's press half retired 2026-10-26: the header is inert, the shell
        # keeps no press of its own in act_app, and the lifted dispatcher block
        # carries the whole answer.
        # D19's pack and worker, pinned on their own: cap_pack exists because
        # the contract between a press and the NVS word it writes is the whole
        # shift arithmetic, and it stays checked even now that the press
        # itself lifts. It precedes the page block below because act_caps
        # names it, and C reads top to bottom.
        lift(src, r"static int cap_pack\(const orb_app_t \*a, int row\)\n\{[\s\S]*?\n\}", MENU_C),
        lift(src, r"static void job_grant\(void \*arg\)\n\{[\s\S]*?\n\}", MENU_C),
        # The share card's page functions (PG_SHARE), one lift: they are
        # defined ahead of the shipped page block, and the lifted table names
        # them, so the lift keeps that order.
        lift(src, r"static int rows_share\(void\)[\s\S]*?"
                  r"static const char \*hint_share\(void\)\n\{[\s\S]*?\n\}", MENU_C),
        # The page block as one lift: the top sub-table, every count, label,
        # title, hint, press, ordinal, enter and leave the table names, and
        # the sub-table's helpers between them. They ship as one unit, and a
        # lift that took half of them would compile a half-moved shell.
        lift(src, r"static const menu_row_t s_top\[\][\s\S]*?"
                   r"static const char \*hint_about\(void\)[^\n]*\n", MENU_C),
        # The app page's entry, the generic move, the inventory itself, and
        # the press: navigation is the shell and is now testable as such.
        lift(src, r"static void open_app\(const orb_app_t \*a\)\n\{[\s\S]*?"
                   r"static void activate\(void\)\n\{[\s\S]*?\n\}", MENU_C),
        # The four accessors the view calls, one lift: page_rows keeps the
        # armed gate, row_text keeps the label clear, and the suite must see
        # the shipped versions of both decisions.
        lift(src, r"static int page_rows\(void\)[\s\S]*?"
                   r"static const char \*page_hint\(void\)\n\{[\s\S]*?\n\}", MENU_C),
        # The menu's "where was I" block (item 3): the RAM mirror, the restorable
        # switch, and the two NVS helpers set_armed now calls. Lifted as one unit
        # ahead of set_armed, which names them; a fake nvs_* below makes the
        # round-trip run through the SHIPPED pos_save/pos_load, not a restated copy.
        lift(src, r"#define MENU_POS_NS[\s\S]*?nvs_close\(h\);\n\}", MENU_C),
        lift(src, r"void menu_view_set_armed\(bool armed\)\n\{[\s\S]*?\n\}", MENU_C),
        lift(src, r"void menu_view_apply_input\(int32_t delta, bool pressed\)\n\{[\s\S]*?\n\}", MENU_C),
        # Step 4: the plane answer for the row under the cursor, lifted so the
        # service() re-check compiles against the shipped translation and not a
        # restated one: it is the only path from the app's reaped plane to the
        # repaint decision this suite can observe. Ahead of the service lift —
        # C reads the spliced file top to bottom, and the shipped service()
        # calls it.
        lift(src, r"static const void \*app_preview\(uint16_t \*w, uint16_t \*h,"
                   r" uint32_t \*stamp\)\n\{[\s\S]*?\n\}", MENU_C),
        lift(src, r"void menu_view_service\(void\)\n\{[\s\S]*?\n    repaint\(\);\n\}", MENU_C),
        # D12: the long press that closes an open group instead of going home.
        # It touches no widget directly (repaint() is stubbed above), so it
        # lifts; the router's half of the bargain is pinned in input_router.c.
        lift(src, r"bool menu_view_opt_long\(void\)\n\{[\s\S]*?\n\}", MENU_C),
    ]
    consts = [
        lift(src, r"#define ROW_MAX\s+\d+[^\n]*", MENU_C),
        # The glyph floor, lifted whole: the table IS the answer to "what can
        # this dial draw", so a restated copy would keep the suite green while
        # the shipped face lost a mapping. dial_text() is pure, which is what
        # lets it lift at all — repaint() is a stub here.
        lift(src, r"typedef struct \{ unsigned cp; const char \*sub; \} glyph_fold_t;"
                  r"[\s\S]*?static void dial_text\(char \*dst, size_t dstsz, const char \*src\)"
                  r"\n\{[\s\S]*?\n\}", MENU_C),
        presets,
        model,
        protos,
        # D19: the revocation ladder's state, the four caps, and the row
        # helpers - one block, lifted ahead of the page functions that call
        # into it. The orb_svc doors it reaches are declared by the real
        # orb_svc.h the harness includes and stubbed below with the registry's.
        lift(src, r"static int      s_perm_slot[\s\S]*?static bool perms_denied\(void\)\n\{[\s\S]*?\n\}", MENU_C),
        "static const orb_app_t *s_app;   /* the app PG_APP is showing */",
        # The one piece of D12 state the shell owns: which group is open, -1 = none.
        "static int s_group_open = -1;",
        "static bool s_busy;",
        "static bool orb_jobs_busy(void) { return s_busy; }",
        lift(src, r"typedef struct \{\n    char\s+status\[72\];[\s\S]*?\} ui_t;", MENU_C),
        # The worker's answer half and the recorder it publishes through -
        # after the ui_t lift, which its signature reads.
        "static uint32_t g_decl[4], g_grnt[4], g_dmask[4];",
        "static int g_setg_calls, g_publish_calls, g_setg_slot, g_setg_on;",
        "static orb_cap_t g_setg_cap;",
        "static char g_pub_line[72];",
        "static const char *esp_err_to_name(esp_err_t e)"
        " { static char b[16]; snprintf(b, sizeof b, \"0x%x\", (unsigned)e); return b; }",
        "static void worker_publish(const ui_t *src)"
        " { g_publish_calls++; snprintf(g_pub_line, sizeof g_pub_line, \"%s\", src->status); }",
        # The three jobs the shipped act_* functions post. job_post() itself is
        # stubbed below, so these exist to make the pointers the lifted presses
        # hand it real symbols; what they would do on a worker is the subject
        # of the dedicated suites, not this one.
        "static void job_dim_en(void *a) { (void)a; }",
        "static void job_dim_to(void *a) { (void)a; }",
        "static void job_units(void *a) { (void)a; }",
        # §59: the rotation's choices, its row text and its two jobs, lifted whole and run
        # against the shipped orb_rotation.c (compiled beside, NVS left out).
        '#include "orb_rotation.h"',
        lift(src, r"static const uint32_t ROT_S\[\][\s\S]*?static void job_rot_member\(void \*arg\)\n\{[\s\S]*?\n\}", MENU_C),
        # go_to's exit answer reaches through the public door; the harness
        # declares it before the lifted dispatcher and defines the real one
        # among the funcs, so a press on the exit row is testable. g_setg_rc
        # rode the original consts block: the D19 toggle tests script the
        # broker's answer with it and read the status lines back.
        "void menu_view_set_armed(bool armed);",
        "static esp_err_t g_setg_rc;",
        'static int g_act_slot, g_act_calls, g_act_rc;',
        'static int g_en_calls, g_en_slot, g_en_on, g_en_rc;',
        'static const char *g_en_why;',
        'static int g_un_calls, g_un_slot, g_un_rc;',
        'static const char *g_un_why;',
        'static const char *g_fault_why;',
        'typedef struct { const char *version; } host_app_desc_t;',
        'static host_app_desc_t g_desc;',
        'static int g_sd_mounted;',
        'const orb_app_t *orb_app_current(void);',
        'static const char *g_sd_id;',
        'esp_err_t orb_app_activate(const orb_app_t *a);',
        'esp_err_t orb_app_clear_faults(const char *slug, char *why, size_t whysz);',
        'esp_err_t orb_app_uninstall(const char *slug, char *why, size_t whysz);',
        'const char *orb_svc_fault_reason(const orb_app_t *a);',
        'static const host_app_desc_t *esp_app_get_description(void);',
        'static const char *orb_net_ip_str(void);',
        'static const char *orb_net_tz_current(void);',
        # ABOUT names the zone through orb_home (the home's IANA name while the
        # zone follows it); the stub answers the zone in force.
        '#include "orb_home.h"',
        'void orb_home_zone_label(char *o, size_t n) { snprintf(o, n, "%s", orb_net_tz_current()); }',
        # UNITS reads the choice in force and names it; the stub holds one record
        # and the real label table's spelling for the rows the checks read.
        '#include "orb_units.h"',
        'static orb_units_t g_units;',
        'void orb_units_get(orb_units_t *o) { *o = g_units; }',
        'int orb_units_count(int k) { return k == ORB_UNITS_TEMP || k == ORB_UNITS_TIME ? 2 : k < ORB_UNITS_KINDS ? 3 : 0; }',
        'const char *orb_units_label(int k, int v) { static const char *const L[5][3] = '
        '{ { "\\xC2\\xB0" "C", "\\xC2\\xB0" "F", NULL }, { "km/h", "mph", "kn" }, { "km", "mi", "nm" },'
        ' { "24-hour", "12-hour", NULL }, { "hPa", "inHg", "mmHg" } };'
        ' return v >= 0 && v < orb_units_count(k) ? L[k][v] : NULL; }',
        'static bool orb_sd_mounted(void);',
        # §58: an app's Themes sub-menu is the provider row's page, aimed at the app;
        # the fakes answer a settable count and active name and count the walks.
        '#include "themes_page.h"',
        'orb_app_t orb_app_themes_row = { .slug = "themes", .title = "Themes", .installed = true };',
        'static int g_th_n, g_th_walks; static const char *g_th_active; static char g_th_for[24];',
        'void themes_page_for(const char *s) { snprintf(g_th_for, sizeof g_th_for, "%s", s ? s : ""); }',
        'int themes_page_count_for(const char *s) { (void)s; return g_th_n; }',
        'const char *themes_page_active_for(const char *s) { (void)s; return g_th_active; }',
        'void themes_page_walk(void) { g_th_walks++; }',
        # theme-cache-plan (2026-11-09): why an app's theme is missing; none by default.
        'static bool g_th_absent;',
        'bool orb_theme_absent(const char *s, const char **b, char *w, size_t n) '
        '{ (void)s; (void)w; (void)n; if (b) *b = g_th_absent ? "no card" : NULL; return g_th_absent; }',
        # §61: the theme carousel, answered by a switch: off, the text list stands in.
        '#include "theme_strip.h"',
        'static bool g_ts_ok; static const orb_app_t *g_ts_app; static int g_ts_opens;',
        'bool theme_strip_open(const orb_app_t *a) { g_ts_opens++; g_ts_app = a; return g_ts_ok; }',
        'static const char *orb_sd_card_id(void);',
        # Item 3: a fake NVS with a real round-trip, so the lifted pos_save/
        # pos_load store and reload through this harness rather than a restated
        # copy. A two-key table (pg, cur) in the orb_menu namespace is all the
        # menu's memory uses; the read reports "not found" until a write lands,
        # which is the first-boot case the restore is checked to survive.
        'typedef int nvs_handle_t;',
        '#define NVS_READONLY  0',
        '#define NVS_READWRITE 1',
        '#define ESP_ERR_NVS_NOT_FOUND (-2)',
        'static int32_t g_nvs_pg, g_nvs_cur; static bool g_nvs_written;',
        'static int g_nvs_opens;',
        'static esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)',
        '{ (void)ns; (void)mode; g_nvs_opens++; *h = 1; return ESP_OK; }',
        'static esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *out)',
        '{ (void)h; if (!g_nvs_written) return ESP_ERR_NVS_NOT_FOUND;',
        '  if (!strcmp(k, "pg2")) { *out = g_nvs_pg; return ESP_OK; }',
        '  if (!strcmp(k, "cur")) { *out = g_nvs_cur; return ESP_OK; }',
        '  return ESP_ERR_NVS_NOT_FOUND; }',
        'static esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v)',
        '{ (void)h; if (!strcmp(k, "pg2")) g_nvs_pg = v;',
        '  else if (!strcmp(k, "cur")) g_nvs_cur = v;',
        '  g_nvs_written = true; return ESP_OK; }',
        'static esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }',
        'static esp_err_t nvs_close(nvs_handle_t h) { (void)h; return ESP_OK; }',
        'static void nvs_reset(void) { g_nvs_pg = 0; g_nvs_cur = 0;',
        '  g_nvs_written = false; }',
    ]
    return consts, funcs
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
# Lifted from the roaming branch of the router's poll. Plan §51 (2026-11-06) retired
# the carousel walk: a turn on a face with no dial of its own opens the app list, the
# cursor moved one row in the list's own sense, and a press still arms. The list is
# reached through the registry, so the router still names no view.
ROAM = lift(router_src,
            r"if \(!screen_armed\(s_current\)\) \{[\s\S]*?\n    \}", ROUTER_C)
check("a roaming turn opens the app list in the list's own direction",
      "open_switcher(knob_row_step(delta)" in ROAM and "set_screen(" not in ROAM, ROAM.strip())
check("a roaming press still arms the view on the glass",
      "if (pressed) { arm_current(); return; }" in ROAM, ROAM.strip())
OPEN_SW = lift(router_src, r"static void open_switcher\(int step\)\n\{[\s\S]*?\n\}", ROUTER_C)
check("the app list opens on the home entry, through the registry's switch path",
      re.search(r"set_screen\(orb_app_home\(\), true\) != ESP_OK\) return;\s*"
                r"if \(!orb_app_open_switcher\(from, step\)\) return;", OPEN_SW) is not None,
      OPEN_SW.strip())
# The menu must still be a walkable carousel entry, tagged as shell chrome
# rather than pretending to be an app it is not.
check("the menu is a registered walkable entry",
      '"menu"' in APP_TABLE and "ORB_APP_KIND_SHELL" in APP_TABLE)
# And the long-press's target must still be the chrome, spelled as a table flag
# rather than as an enum value the router hardcodes. D35 (§35) moved homing off
# the dial: the clock is a factory card row now, uninstallable like any app, and
# the shell menu is the row that can never go away — so the flag lives there.
check("the shell menu is the registered home entry",
      re.search(r'\"menu\"[\s\S]{0,500}\.is_home\s*=\s*true', APP_TABLE) is not None)

# orb_theme_entry_t, from the shipped header — row_text() indexes its fields.
KNOB_C = P("main", "knob", "knob.c")
KNOB_H = P("main", "knob", "knob.h")
for _p in (KNOB_C, KNOB_H):
    if not os.path.exists(_p):
        sys.exit("missing " + _p + " — menu_view.c now gets its cursor direction from"
                 " the knob layer, so the lift below has no fallback and must not"
                 " invent one")
knob_src = open(KNOB_C).read()
# docs/knob-edit-plan.md: the edit mode's step maths, compiled in whole (the header's
# types and the shipped .c), ahead of the model that names knob_range_t.
KE_H = P("main", "menu", "knob_edit.h")
KE_C = P("main", "menu", "knob_edit.c")
KNOB_EDIT = (open(KE_H).read().replace("#pragma once", "")
             + open(KE_C).read().replace('#include "knob_edit.h"', "")
             + "\nstatic int64_t g_now_us = 1, g_tick_us = 500000;"
             + "\nstatic int64_t esp_timer_get_time(void) { return g_now_us += g_tick_us; }\n")
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
#define ESP_ERR_INVALID_STATE_ 263   /* the doors' refusal code, named apart from
                                 * ESP_FAIL so a check cannot pass on the wrong one */
#define ESP_ERR_NOT_FOUND_ 259       /* orb_app_activate's no-screen refusal (P8 gap) */
#define ESP_ERR_NOT_FOUND ESP_ERR_NOT_FOUND_   /* the lifted act_appact reads the real name */
#define ESP_FAIL (-1)          /* the refused-write answer the D19 toggle's
                                * status line is checked against, spelled as
                                * esp_err.h spells it rather than as a bare -1 */
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define EXT_RAM_BSS_ATTR                /* PSRAM placement: nothing on a host */
static const char *TAG __attribute__((unused)) = "menu";
__SLUGDEF__
__UNUSED_OK__

/* The lifted repaint is a no-op in the pure-state harness; the real one's only
 * observable effect is that it stamps s_painted_rows, so the stub mirrors that
 * alone. Without it the app-grew reap check would never fire (s_painted_rows
 * would sit at 0 forever), and deleting that check from the shipped
 * menu_view_service() would leave the whole suite green — the exact
 * unfalsifiable-fix repaint_lint was later added to catch. */
static int  s_painted_rows;      /* stamped by the stub below, read by the lifted service() */
static int  page_rows(void);     /* the lifted definition follows */
static const void *app_preview(uint16_t *w, uint16_t *h, uint32_t *stamp);
                                 /* the lifted definition follows; the stub below
                                  * reads it the way the shipped repaint does */
/* Step 4: the plane the last repaint drew, the shipped paint_preview stamps.
 * The lifted service() reads these directly (the pv_landed re-check), and the
 * shipped repaint() is the writer, so the stub mirrors that stamping exactly —
 * otherwise the check below would pass against a service() that never repaints
 * for a plane, which is the "previews never show" bug the layout names. */
static uint32_t s_pv_stamp;
static uint16_t s_pv_w, s_pv_h;
static int  g_focus = -1;
/* The app's preview answer, settable per check: a fake plane the shell can be
 * made to see. The dispatcher itself is the real one (compiled from orb_app.c);
 * this fake stands for the clock's opt_preview hook. */
static uint8_t g_fake_plane[8];
static const void *g_pv_px;
static uint16_t g_pv_pw, g_pv_ph;
static uint32_t g_pv_pstamp;
static void pv_set(const void *px, uint16_t w, uint16_t h, uint32_t stamp)
{ g_pv_px = px; g_pv_pw = w; g_pv_ph = h; g_pv_pstamp = stamp; }
void orb_app_opt_focus(const orb_app_t *a, int i) { (void)a; g_focus = i; }
const void *orb_app_opt_preview(const orb_app_t *a, int i, uint16_t *w,
                                uint16_t *h, uint32_t *stamp)
{
    (void)a; (void)i;                     /* the fake answers every row the same */
    if (w) *w = g_pv_pw;
    if (h) *h = g_pv_ph;
    if (stamp) *stamp = g_pv_pstamp;
    return g_pv_px;
}
static int  g_repaints;
static void repaint(void)
{
    g_repaints++;
    s_painted_rows = page_rows();
    uint16_t w = 0, h = 0; uint32_t st = 0;
    const void *px = app_preview(&w, &h, &st);
    if (px) { s_pv_stamp = st; s_pv_w = w; s_pv_h = h; }
    else { s_pv_stamp = 0; }
}
static int  g_homes;               /* the share card's park lands here */
static void input_router_home(void) { g_homes++; }
/* Item 3's exit door: the lifted go_to/opt_long reach the save only through
 * this, exactly as the shipped router does (menu_exit -> disarm_all -> the
 * menu's own set_armed(false)). Calling it is what keeps the store-on-exit
 * checks honest: a menu_exit stub that skipped the disarm would let the save
 * rot while every check stayed green. */
void menu_view_set_armed(bool armed);   /* shipped definition spliced in later */
static int  g_menu_exits;
static void input_router_menu_exit(void) { menu_view_set_armed(false); g_menu_exits++; }
/* §51: the app list's press opens the app through the router; recorded, not run. */
static const orb_app_t *g_shown;
static int input_router_show(const orb_app_t *a) { g_shown = a; return 0; }

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

/* The press lifts now — it reaches the card only through the stubs — and
 * g_activations survives as the opt_sel recorder: the lifted app dispatcher
 * below counts every press the shell hands its own app. */
static int g_activations;

/* job_post() is stubbed below, after the shell's own state: the queue is
 * FreeRTOS, and what matters here is that the cursor moved (or did not), not
 * that a packet was queued. The kind enum is lifted, not restated, so a job
 * kind the menu starts posting but this file never learned about fails to
 * compile instead of passing quietly. */
static void (*g_posted_fn)(void *);
static int g_posted_pack, g_posts;

__CONSTS__

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
/* The service tier (orb_app.h's ORB_BEHAVIOUR_MENU_ONLY), off the same settable
 * count the face apps use: g_menu_n is 0 for every case that predates it, so the
 * rows checks below keep the geometry they were written against, and the service
 * cases set it to 1 to see where the row lands and what it prints. One service is
 * enough to test a tier: two would only test the harness's array. */
static orb_app_t g_fake_service = { .title = (char *)("USB Files") };
#define FAKE_SERVICE (&g_fake_service)
static size_t g_menu_n = 0;
size_t orb_app_menu_count(void) { return g_menu_n; }
const orb_app_t *orb_app_menu_at(size_t i) { return (g_menu_n && i == 0) ? FAKE_SERVICE : NULL; }
bool orb_app_is_menu_only(const orb_app_t *a) { return a == FAKE_SERVICE || a == &orb_app_themes_row; }
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
/* §58: row g_note_row answers a sentence instead of an action; -1 = none does. */
static int g_note_row = -1;
const char *orb_app_opt_note(const orb_app_t *a, int i)
{ (void)a; return i == g_note_row ? "Place 1 is set in Orb Manager." : NULL; }
/* docs/knob-edit-plan.md: row 0 of the fake app is knob-edited when g_opt_editable is
 * set (0..100 from 40, fast in fives), row 1 never is; the save is recorded. */
static bool g_opt_editable;
static int g_opt_set_i = -1;
static int32_t g_opt_set_v = -999;
bool orb_app_opt_edit(const orb_app_t *a, int i, orb_opt_edit_t *e)
{
    (void)a;
    if (!g_opt_editable || i != 0) return false;
    *e = (orb_opt_edit_t){ .min = 0, .max = 100, .value = 40, .step_max = 5 };
    return true;
}
const char *orb_app_opt_fmt(const orb_app_t *a, int i, int32_t v)
{ static char b[16]; (void)a; (void)i; snprintf(b, sizeof b, "%ld", (long)v); return b; }
void orb_app_opt_set(const orb_app_t *a, int i, int32_t v) { (void)a; g_opt_set_i = i; g_opt_set_v = v; }
void orb_app_opt_arm(const orb_app_t *a, bool on) { (void)a; g_arms += on ? 1 : -1; }
/* The page's own first refusal, pinned closed: every lifted long-press test here
 * exercises the shell's group rule, which is the fall-through path the real
 * dispatcher hands the shell when no app claims the hold. */
bool orb_app_opt_long(const orb_app_t *a) { (void)a; return false; }
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
    static const char *const N[10] = { "time", "config", "ui", "art", "power",
                                       "net", "sd_read", "themes", "sound", "location" };
    return (unsigned)c < 10u ? N[c] : "?";
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

/* ---- P5.3: the APPS page's doors and ABOUT's facts -------------------------
 * The catalog reaches the machine through calls the older pages never made.
 * Each is recorded rather than performed: this suite owns which row posts what
 * and which sentence lands on the status line, not the flash write behind it
 * (that is host_orbapp_install_test.py's subject). The refusal texts are
 * scripted per check because the row's job is to pass the door's own words
 * through verbatim.
 *
 * The slot a call names is resolved from the SLUG rather than an index: the
 * pack a press builds carries an index, so slug-out/slot-back is exactly the
 * round trip a wrong shift would corrupt without a sound. */
esp_err_t orb_app_activate(const orb_app_t *a)
{
    g_act_calls++; g_act_slot = orb_app_index_of(a);
    return (esp_err_t)g_act_rc;
}
esp_err_t orb_app_clear_faults(const char *slug, char *why, size_t whysz)
{
    g_en_calls++; g_en_on = 1;
    g_en_slot = (slug && !strcmp(slug, g_fake_app.slug))  ? 0 :
                (slug && !strcmp(slug, g_fake_app2.slug)) ? 1 :
                (slug && !strcmp(slug, g_fake_shell.slug))? 2 : -1;
    if (g_en_rc != ESP_OK && why && whysz) snprintf(why, whysz, "%s", g_en_why);
    return (esp_err_t)g_en_rc;
}
esp_err_t orb_app_uninstall(const char *slug, char *why, size_t whysz)
{
    g_un_calls++;
    g_un_slot = (slug && !strcmp(slug, g_fake_app.slug))  ? 0 :
                (slug && !strcmp(slug, g_fake_app2.slug)) ? 1 :
                (slug && !strcmp(slug, g_fake_shell.slug))? 2 : -1;
    if (g_un_rc != ESP_OK && why && whysz) snprintf(why, whysz, "%s", g_un_why);
    return (esp_err_t)g_un_rc;
}
/* Which entry the glass is showing decides both "on the dial" rows. Settable so
 * a check can put one of the fakes on the glass and watch the label follow. */
static const orb_app_t *g_current;
const orb_app_t *orb_app_current(void) { return g_current; }
const char *orb_svc_fault_reason(const orb_app_t *a) { (void)a; return g_fault_why; }
static const host_app_desc_t *esp_app_get_description(void) { return &g_desc; }
static const char *g_ip = "192.168.2.32";       /* "" = not on a network */
static const char *orb_net_ip_str(void) { return g_ip; }
static const char *orb_net_tz_current(void) { return "Europe/Bucharest"; }
static bool orb_sd_mounted(void) { return g_sd_mounted != 0; }
static const char *orb_sd_card_id(void) { return g_sd_id; }

/* ---- D38: the card-walk tier the catalog now lists, and its doors ---------
 * orb_app_find_slug answers the registry's installed view so slug_on_dial can
 * filter the snapshot; the scan hands back a settable table; install and rm
 * record their call and answer from a settable code, exactly as the enable and
 * uninstall fakes above do. The checks set g_scan_n to decide how many card
 * rows exist and g_scan_slot_* to name them. */
static orbapp_scan_entry_t g_scan[ORB_APP_SCAN_MAX];
static int g_scan_n;
const orb_app_t *orb_app_find_slug(const char *slug)
{
    if (!slug || !slug[0]) return NULL;
    if (!strcmp(slug, g_fake_app.slug))  return g_fake_app.installed  ? FAKE_APP  : NULL;
    if (!strcmp(slug, g_fake_app2.slug)) return g_fake_app2.installed ? FAKE_APP2 : NULL;
    return NULL;
}
int orb_app_scan(orbapp_scan_entry_t *out, int cap)
{
    int n = g_scan_n < cap ? g_scan_n : cap;
    for (int i = 0; i < n; i++) out[i] = g_scan[i];
    return n;
}
static int g_install_calls, g_install_rc, g_rm_calls, g_rm_rc;
static char g_install_stem[64], g_rm_rel[96];
esp_err_t orb_app_install(const char *stem, char *why, size_t whysz)
{
    g_install_calls++;
    if (stem) snprintf(g_install_stem, sizeof g_install_stem, "%s", stem);
    if (g_install_rc != ESP_OK && why && whysz) snprintf(why, whysz, "install refused");
    return (esp_err_t)g_install_rc;
}
esp_err_t orb_sd_fm_rm(const char *rel, bool recursive, int *removed_out)
{
    (void)recursive;
    g_rm_calls++;
    if (rel) snprintf(g_rm_rel, sizeof g_rm_rel, "%s", rel);
    if (g_rm_rc != ESP_OK) return (esp_err_t)g_rm_rc;
    if (removed_out) *removed_out = 1;
    return ESP_OK;
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
    g_desc.version = "abc1234"; g_sd_mounted = 1; g_sd_id = "SDSC"; g_current = NULL;
    g_fault_why = ""; g_en_why = ""; g_un_why = "";

    /* ---- unarmed is a title card, not a list ---- */
    s_armed = false; s_pg = &s_pages[PG_LANDING]; s_cur = 0; s_busy = false;
    g_delta_down = knob_row_step(1);
    ck("unarmed has no rows", page_rows() == 0);
    turn_down(1);
    ck("unarmed ignores rotate", s_cur == 0);
    g_activations = 0;
    menu_view_apply_input(0, true);
    /* The press is observed, not merely counted: activate() lifts, so a press
     * that leaked past the armed guard would reach the landing page's act() —
     * which is NULL, and the two g_ counters make that visible instead of a
     * counter that only existed to be incremented. */
    g_posts = 0;
    s_pg = &s_pages[PG_DISPLAY];  /* a list left set behind a disarm: only the
                                   * armed guard in apply_input answers this, not
                                   * the landing's missing rows — activate() would
                                   * post the dim write straight through it. */
    menu_view_apply_input(0, true);
    ck("unarmed ignores press",
       g_activations == 0 && g_posts == 0 && s_cur == 0 && s_pg == &s_pages[PG_DISPLAY]);
    s_pg = &s_pages[PG_LANDING];

    /* ---- armed top page ---- */
    s_armed = true; s_pg = &s_pages[PG_TOP]; s_cur = 0; s_busy = false;
    /* One fake app declaring two option rows, which is what earns it a line on
     * the top page. g_opt_n starts at 0 for the earlier unarmed checks. */
    g_opt_app_count = 1; g_opt_n = 2;
    snprintf(g_opt_label0, sizeof g_opt_label0, "diver");
    snprintf(g_opt_val0, sizeof g_opt_val0, "on the dial");
    snprintf(g_opt_label1, sizeof g_opt_label1, "neon"); g_opt_val1[0] = '\0';
    /* Four OS rows — ABOUT, DISPLAY, UNITS, APPS (2026-11-01: TZ left because
     * the hub sets the home and the home the zone; 2026-11-07: PERMISSIONS and
     * the per-app rows went into APPS, one page per app) — then the service
     * pages, then exit. Every index below is derived from top_os_rows() rather
     * than written out, because a check that hardcodes "exit is row 5" goes red
     * for the wrong reason the day the OS grows a row, and a suite that cries
     * wolf at a legitimate new row is a suite someone turns off. */
    const int OSN = top_os_rows(), EXIT = OSN;
    ck("top page is the OS's four rows, then exit — an app with options adds none",
       OSN == 4 && page_rows() == 5);
    ck("row order ABOUT/DISPLAY/UNITS/APPS/exit",
       !strcmp(rowline(0),"ABOUT") && !strcmp(rowline(1),"DISPLAY") &&
       !strcmp(rowline(2),"UNITS") && !strcmp(rowline(3),"APPS") &&
       !strcmp(rowline(EXIT),"exit"));
    /* UNITS (weather-plan §2.1): four knob-edited rows and back, each naming the
     * choice in force, and an edit's range is that quantity's choices. */
    { const page_t *spg = NULL; (void)spg;
      const menu_page_t *keep = s_pg; const int keepc = s_cur;
      s_pg = &s_pages[PG_UNITS];
      g_units = (orb_units_t){ ORB_UNIT_F, ORB_UNIT_KN, ORB_UNIT_NM, ORB_UNIT_12H, ORB_UNIT_MMHG };
      ck("UNITS lists temperature, speed, distance, time and pressure, then back",
         page_rows() == 6 && !strcmp(rowline(0), "temperature:  \xC2\xB0" "F") &&
         !strcmp(rowline(1), "speed:  kn") && !strcmp(rowline(2), "distance:  nm") &&
         !strcmp(rowline(3), "time:  12-hour") && !strcmp(rowline(4), "pressure:  mmHg") &&
         !strcmp(rowline(5), "<- back"));
      menu_edit_t e; memset(&e, 0, sizeof e);
      ck("a unit row edits through its own choices, wrapping",
         edit_units(0, &e) && e.r.min == 0 && e.r.max == 1 && e.r.wrap && e.v == ORB_UNIT_F &&
         edit_units(2, &e) && e.r.max == 2 && e.v == ORB_UNIT_NM &&
         edit_units(3, &e) && e.r.max == 1 && e.v == ORB_UNIT_12H &&
         edit_units(4, &e) && e.r.max == 2 && e.v == ORB_UNIT_MMHG && !edit_units(5, &e));
      g_units = (orb_units_t){ 0, 0, 0, 0, 0 };
      s_pg = keep; s_cur = keepc; }
    /* An app's settings are on its own APPS page now, so a second app that
     * declares options still adds nothing here. */
    g_opt_app_count = 2;
    ck("a second app declaring options adds no top row either", page_rows() == OSN + 1);
    g_opt_app_count = 1;

    /* ---- D36: the service tier -------------------------------------------
     * A row with options and no face. The registry owns which services exist;
     * the menu's job is to place the row, print its state on it, and hand the
     * press to the same option hooks a face page uses. */
    g_menu_n = 1;
    snprintf(g_opt_label0, sizeof g_opt_label0, "Start as USB drive");
    g_opt_val0[0] = '\0';
    const int SVC = OSN, EXIT2 = OSN + 1;
    ck("a service row is the top page's dynamic run, before exit",
       page_rows() == OSN + 2 && !strcmp(rowline(SVC), "USB Files") &&
       !strcmp(rowline(EXIT2), "exit"));
    snprintf(g_opt_val0, sizeof g_opt_val0, "armed");
    ck("a service row prints its own state", !strcmp(rowline(SVC), "USB Files  (armed)"));
    /* Turning it off makes the row vanish rather than leave a dead line. */
    g_menu_n = 0;
    ck("a service with no registry entry has no row, and exit moves back up",
       page_rows() == OSN + 1 && !strcmp(rowline(OSN), "exit"));
    g_menu_n = 1;
    /* Pressing it goes to the option page and arms the knob, exactly as a face
     * app row does: the shell has no second code path for a service. */
    g_opt_n = 1;
    const int act0 = g_activations;
    s_cur = SVC;
    menu_view_apply_input(0, true);
    ck("a press on a service row opens its page and arms the knob",
       s_pg == &s_pages[PG_APP] && s_cur == 0 && s_app == FAKE_SERVICE &&
       g_arms == 1 && page_rows() == 2 &&
       !strcmp(rowline(0), "Start as USB drive  (armed)") &&
       !strcmp(rowline(1), "<- back"));
    menu_view_apply_input(0, true);
    ck("the service answers the press through the same option hooks a face uses",
       g_activations == act0 + 1 && g_last_opt_sel == 0);
    s_cur = page_rows() - 1;
    menu_view_apply_input(0, true);
    ck("back from a service lands on its own top row and disarms",
       s_pg == &s_pages[PG_TOP] && s_cur == SVC && g_arms == 0);
    g_menu_n = 0;
    g_opt_n = 2;

    /* ---- the clamp, both ends, no wrap ---- */
    g_delta_down = knob_row_step(1);      /* derived, never assumed */
    ck("a list turn exists to ask for", g_delta_down != 0);
    s_cur = 0;
    turn_up(1);
    ck("rotate past the top clamps to 0", s_cur == 0);
    s_cur = EXIT;
    turn_down(1);
    ck("rotate past the bottom clamps to last", s_cur == EXIT);
    s_cur = 1;
    turn_down(99);
    ck("one detent per event, no flying", s_cur == 2);
    s_cur = 2;
    turn_up(99);
    ck("and not backwards either", s_cur == 1);
    menu_view_apply_input(0, false);
    ck("zero delta is not a move", s_cur == 1);

    /* ---- presses, through the table ----
     * The dispatcher lifted with the table, so the shell's navigation half is
     * testable here for the first time: which page a row opens, which cursor
     * it opens with, what the back row and the exit row do. Each answer used
     * to be a switch arm only the bench could reach. */
    s_armed = true; s_pg = &s_pages[PG_TOP]; s_cur = 0; s_busy = false;
    g_homes = 0; g_menu_exits = 0; g_posts = 0; g_arms = 0; g_activations = 0;
    nvs_reset(); g_nvs_opens = 0;               /* the store-on-exit check below reads a clean slot */
    g_opt_n = 2; g_opt_app_count = 1; g_kind_hook = false;
    s_cur = 1;
    menu_view_apply_input(0, true);
    ck("DISPLAY opens at its first row",
       s_pg == &s_pages[PG_DISPLAY] && s_cur == 0);
    s_cur = 1; g_dim_to = 600;
    menu_view_apply_input(0, true);
    ck("the timeout row enters the knob edit on the value in force, on its own page",
       s_edit.on && s_pg == &s_pages[PG_DISPLAY] && s_cur == 1 && s_edit.e.v == 10);
    menu_view_apply_input(0, true);
    ck("a second press saves the value through the job and ends the edit",
       !s_edit.on && g_posts == 1 && g_posted_fn == job_dim_to && g_posted_pack == 10 &&
       s_pg == &s_pages[PG_DISPLAY] && s_cur == 1);
    g_posts = 0;
    s_pg = &s_pages[PG_TOP]; s_cur = 3;
    menu_view_apply_input(0, true);
    ck("APPS opens the app list",
       s_pg == &s_pages[PG_APPS] && s_cur == 0);
    s_pg = &s_pages[PG_TOP]; s_cur = 0;
    menu_view_apply_input(0, true);
    ck("the first row opens ABOUT on the address, where the QR shows",
       s_pg == &s_pages[PG_ABOUT] && s_cur == 0 && about_qr_on());
    s_pg = &s_pages[PG_APPS]; s_cur = 0; g_scan_n = 0; s_card_n = 0;
    menu_view_apply_input(0, true);
    ck("an app row in APPS opens the app flat on its first row, knob armed",
       s_pg == &s_pages[PG_APP] && s_cur == 0 && s_group_open == -1 && g_arms == 1);
    menu_view_apply_input(0, true);
    ck("a press on an option row reaches the app at its own row number",
       g_activations == 1 && g_last_opt_sel == 0);
    s_cur = page_rows() - 1;
    menu_view_apply_input(0, true);
    ck("back from the app lands on that app's own row in APPS and disarms it",
       s_pg == &s_pages[PG_APPS] && s_cur == 0 && g_arms == 0);
    go_back();
    s_cur = page_rows() - 1;
    menu_view_apply_input(0, true);
    ck("the last row of the top page now exits through the new door",
       !s_armed && s_pg == &s_pages[PG_LANDING] && g_menu_exits == 1 && g_homes == 0);
    /* The ruling rides on the sub-pages too: a hold from anywhere in the menu,
     * with nothing open to collapse, is the door out — and the disarm inside
     * it is where item 3 stores the place the owner was working. */
    nvs_reset();
    s_armed = true; s_pg = &s_pages[PG_DISPLAY]; s_cur = 1;
    menu_view_opt_long();
    ck("the hold on DISPLAY takes the press and exits the menu",
       g_menu_exits == 2 && !s_armed && s_pg == &s_pages[PG_LANDING]);
    ck("the exit from DISPLAY stores the page and the row",
       g_nvs_written && g_nvs_pg == PG_DISPLAY && g_nvs_cur == 1);
    s_armed = true; s_pg = &s_pages[PG_TOP]; s_cur = 0;

    /* ---- D19 and the themes, as an app's two sub-menus (2026-11-07) ----
     * Declare-at-install is the grant, so nothing here approves anything: the
     * Permissions sub-menu shows what an app declared, what still runs after the
     * owner's toggles, and where it was refused for calling past its declaration.
     * The owner asked for one place per app, then for its themes and permissions
     * to be sub-menus there rather than rows on a long page: the app's page names
     * each with its state, and the refusal mark rides APPS and the app's row. */
    s_armed = true; s_pg = &s_pages[PG_TOP]; s_cur = 0; s_busy = false;
    g_opt_app_count = 1; g_opt_n = 2; g_scan_n = 0; s_card_n = 0;
    g_th_n = 3; g_th_active = "Scope";
    snprintf(g_opt_label0, sizeof g_opt_label0, "diver");
    snprintf(g_opt_val0, sizeof g_opt_val0, "on the dial");
    g_decl[0] = (1u << CAP_POWER) | (1u << CAP_NET);
    g_decl[1] = 1u << CAP_THEMES;
    g_grnt[0] = g_decl[0]; g_grnt[1] = g_decl[1];
    ck("no refusal, no mark on the APPS row", !strcmp(rowline(3), "APPS"));
    g_dmask[0] = 1u << CAP_POWER;
    ck("an app refused past its declaration marks the APPS row",
       (s_pg = &s_pages[PG_TOP], !strcmp(rowline(3), "APPS  *")));
    s_pg = &s_pages[PG_APPS]; s_cur = 0;
    ck("the refused app carries the mark into its own row of the list, chrome filtered",
       !strcmp(rowline(0), "* Clock  installed") && !strcmp(rowline(1), "HSB  installed"));
    g_arms = 0;
    menu_view_apply_input(0, true);
    ck("an installed app's row opens its one page on the first row, knob armed",
       s_pg == &s_pages[PG_APP] && s_app == FAKE_APP && s_cur == 0 && g_arms == 1);
    ck("the page is its settings, a Themes and a Permissions row, its doors, then back",
       page_rows() == 7 && !strcmp(rowline(0), "diver  (on the dial)") &&
       !strcmp(rowline(2), "Themes  (Scope)") &&
       !strcmp(rowline(3), "Permissions  (2 of 2)  *") &&
       !strcmp(rowline(4), "uninstall") && !strcmp(rowline(5), "remove from card") &&
       !strcmp(rowline(6), "<- back"));
    g_dmask[0] = 0; g_grnt[0] = 1u << CAP_NET;
    ck("the Permissions row counts what still runs against what was declared",
       !strcmp(rowline(3), "Permissions  (1 of 2)"));
    g_grnt[0] = (1u << CAP_NET) | (1u << CAP_THEMES); g_dmask[0] = 1u << CAP_THEMES;
    ck("a grant on a cap it never declared is not counted as one it runs",
       !strcmp(rowline(3), "Permissions  (1 of 2)  *"));
    g_grnt[0] = 1u << CAP_NET; g_dmask[0] = 0;
    g_th_active = NULL;
    ck("with none of its themes on the dial, the Themes row says how many",
       !strcmp(rowline(2), "Themes  (3)"));
    g_th_n = 0;
    ck("an app with no theme on the card has no Themes row",
       page_rows() == 6 && !strcmp(rowline(2), "Permissions  (1 of 2)"));
    g_th_n = 3; g_th_active = "Scope";
    /* The Permissions sub-menu: one row per cap, a press toggles it, back returns
     * to the app's page on the row that opened it. */
    s_cur = 3;
    menu_view_apply_input(0, true);
    ck("the Permissions row opens that app's caps, headed by its name",
       s_pg == &s_pages[PG_CAPS] && s_perm_slot == 0 && s_cur == 0 &&
       !strcmp(page_title(), "Clock PERMISSIONS"));
    ck("a declared cap the owner has taken back reads revoked, not denied",
       page_rows() == 3 && !strcmp(rowline(0), "power:  revoked") &&
       !strcmp(rowline(1), "net:  allowed") && !strcmp(rowline(2), "<- back"));
    g_dmask[0] = 1u << CAP_POWER;
    ck("the cap actually refused this boot is the one that carries the mark",
       !strcmp(rowline(0), "power:  revoked *"));
    g_dmask[0] = 1u << CAP_THEMES;      /* refused on a cap the clock never
                                         * declared: the row that says so is the
                                         * undeclared one, and the mark belongs
                                         * there most of all */
    ck("an undeclared cap it reached for gets a row, marked",
       page_rows() == 4 && !strcmp(rowline(2), "themes:  not declared *"));
    g_dmask[0] = 0; g_grnt[0] = g_decl[0];
    s_cur = 1; g_posts = 0;
    menu_view_apply_input(0, true);
    ck("a cap row's press posts its grant toggle and stays on the page",
       g_posts == 1 && g_posted_fn == job_grant &&
       g_posted_pack == cap_pack(FAKE_APP, 1) &&
       s_pg == &s_pages[PG_CAPS] && s_cur == 1);
    s_cur = page_rows() - 1;
    menu_view_apply_input(0, true);
    ck("back from Permissions lands on the app's page at its Permissions row",
       s_pg == &s_pages[PG_APP] && s_app == FAKE_APP && s_cur == 3);
    /* §61: the Themes row opens the carousel when it can, and the page stays under it. */
    s_cur = 2; g_ts_ok = true; g_ts_opens = 0;
    menu_view_apply_input(0, true);
    ck("the Themes row opens the theme carousel on this app, the page left as it was",
       g_ts_opens == 1 && g_ts_app == FAKE_APP &&
       s_pg == &s_pages[PG_APP] && s_app == FAKE_APP && s_cur == 2);
    g_ts_ok = false;
    /* The Themes sub-menu: the provider row's page, aimed at this app. */
    s_cur = 2; g_th_for[0] = '\0';
    menu_view_apply_input(0, true);
    ck("the Themes row opens the provider's page aimed at this app, headed by its name",
       s_pg == &s_pages[PG_APP] && s_app == &orb_app_themes_row &&
       s_theme_of == FAKE_APP && !strcmp(g_th_for, "clock") &&
       !strcmp(page_title(), "Clock THEMES") && own_rows() == 0);
    s_cur = page_rows() - 1;
    menu_view_apply_input(0, true);
    ck("back from Themes lands on the app's page at its Themes row",
       s_pg == &s_pages[PG_APP] && s_app == FAKE_APP && s_cur == 2 && s_theme_of == NULL);
    /* §59: while the rotation is on, the page carries an In rotation row after
     * Permissions; a press posts the membership flip. */
    orb_rotation_set(true, 60, "");
    ck("with the rotation on, the page grows an In rotation row after Permissions",
       page_rows() == 8 && !strcmp(rowline(4), "In rotation  (no)") && !strcmp(rowline(5), "uninstall"));
    s_cur = 4; g_posts = 0;
    menu_view_apply_input(0, true);
    ck("its press posts the flip into the rotation for this app",
       g_posts == 1 && g_posted_fn == job_rot_member && g_posted_pack == ((0 << 1) | 1));
    orb_rotation_set(false, 60, "");
    ck("off, the row goes", page_rows() == 7 && !strcmp(rowline(4), "uninstall"));
    s_cur = page_rows() - 1;
    menu_view_apply_input(0, true);
    ck("back from an app's page lands on its own row in APPS and disarms it",
       s_pg == &s_pages[PG_APPS] && s_cur == 0 && g_arms == 0);
    s_cur = 1;
    menu_view_apply_input(0, true);
    ck("a second app's page is its own, and an app that declared nothing has no Permissions row",
       (g_decl[1] = 0, s_app == FAKE_APP2 && page_rows() == 2 + 1 + 2 + 1 &&
        !strcmp(rowline(2), "Themes  (Scope)") && !strcmp(rowline(3), "uninstall")));
    g_decl[1] = 1u << CAP_LOCATION; g_grnt[1] = g_decl[1];
    s_perm_slot = 1; s_pg = &s_pages[PG_CAPS]; s_cur = 0;
    ck("the last owner cap, location, gets its row too",
       page_rows() == 2 && !strcmp(rowline(0), "location:  allowed"));
    g_decl[1] = 1u << CAP_THEMES; g_grnt[1] = g_decl[1];
    reopen_app(FAKE_APP2, OWN_PERMS);
    s_cur = page_rows() - 1;
    menu_view_apply_input(0, true);
    ck("and its back lands on its own row",
       s_pg == &s_pages[PG_APPS] && s_cur == 1);
    ck("back from the list lands on the APPS row",
       (go_back(), s_pg == &s_pages[PG_TOP] && s_cur == 3 /* s_top[3] */));
    g_th_n = 0; g_th_active = NULL;
    ck("entering APPS walks the themes beside the apps, so a page has its row at once",
       (g_th_walks = 0, job_scan(&(int){0}), g_th_walks == 1));
    {   /* The pack is the contract between a press and the NVS word it writes. */
        g_grnt[0] = g_decl[0];
        ck("a revoked cap's press asks for the grant, naming this app and cap",
           cap_pack(FAKE_APP, 3) == ((0 << 9) | ((int)CAP_THEMES << 1) | 1));
        g_grnt[0] = (1u << CAP_NET);
        ck("an allowed cap's same press asks for the revoke",
           cap_pack(FAKE_APP, 1) == ((0 << 9) | ((int)CAP_NET << 1) | 0));
        ck("and the second app's pack carries its own slot, not the page's",
           (g_grnt[1] = 0, cap_pack(FAKE_APP2, 0) ==
            ((1 << 9) | ((int)CAP_POWER << 1) | 1)));
        ck("the five rows pack exactly the owner's five caps, in that order",
           ({ int bad = 0; const int w[5] = { CAP_POWER, CAP_NET, CAP_SD_READ,
                                              CAP_THEMES, CAP_SOUND };
              for (int r = 0; r < 5; r++)
                  if (((cap_pack(FAKE_APP, r) >> 1) & 0xFF) != w[r]) bad++;
              bad == 0; }));
        g_grnt[1] = g_decl[1]; g_grnt[0] = g_decl[0];
        /* The busy rule the whole menu shares: no press acts while the worker
         * is down, so a double-tap cannot queue two writes. DISPLAY's first row
         * would post its job, so the post count is what a leak would move. */
        s_pg = &s_pages[PG_DISPLAY]; s_cur = 0;
        s_busy = true; g_posts = 0;
        menu_view_apply_input(1, false);
        menu_view_apply_input(0, true);
        ck("a press while the worker is busy moves nothing and posts nothing",
           g_posts == 0 && s_cur == 0);
        s_busy = false;
        s_pg = &s_pages[PG_TOP]; s_cur = 0;
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


    /* ---- the app options page, rendered from the registry ---- *
     * The theme rows themselves are answered by the clock app (B2), so what is
     * checked here is the SHELL's half: that it renders an app's label and value
     * through one line, clamps on its own cursor, and puts "<- back" after the
     * app's rows whatever the app says. The answers are pinned against the
     * shipped clock_themes.c in tools/host_clock_opts_test.py. */
    /* The shell renders an app's rows without knowing what they mean (B1/B2).
     * g_opt_n/g_opt_v are the fake app's answers; the clock's real ones are
     * pinned against the shipped clock_themes.c in host_clock_opts_test.py. */
    /* Run on the service page (2026-11-07): it is PG_APP like any app, but an
     * installed app's page also carries its caps and doors (checked in the D19
     * block above), and these checks are about the option rows alone. The fake
     * hooks answer the same for every entry. */
    g_menu_n = 1;
    s_pg = &s_pages[PG_APP]; s_app = FAKE_SERVICE; s_cur = 0; g_opt_n = 2;
    snprintf(g_opt_label0, sizeof g_opt_label0, "diver"); snprintf(g_opt_val0, sizeof g_opt_val0, "on the dial");
    snprintf(g_opt_label1, sizeof g_opt_label1, "neon");   g_opt_val1[0] = '\0';
    ck("an app row reads the app's label and value",
       !strcmp(rowline(0), "diver  (on the dial)"));
    ck("a row with no value reads the label alone", !strcmp(rowline(1), "neon"));
    /* §58: a note row opens the message box instead of pressing, the box holds the
     * knob, and press, turn and hold each close it without touching the page. */
    g_note_row = 1; s_cur = 1; g_activations = 0; s_armed = true; g_menu_exits = 0;
    menu_view_apply_input(0, true);
    ck("a note row opens the box instead of reaching the app",
       g_activations == 0 && !strcmp(s_note, "Place 1 is set in Orb Manager.") &&
       !strcmp(page_hint(), "push: OK"));
    turn_down(1);
    ck("a turn with the box up closes it and does not move the cursor",
       s_note[0] == '\0' && s_cur == 1);
    menu_view_apply_input(0, true);           /* opens it again */
    menu_view_apply_input(0, true);           /* closes it */
    ck("a press closes it too, and never reaches the app",
       s_note[0] == '\0' && g_activations == 0);
    menu_view_apply_input(0, true);           /* open, for the hold */
    ck("a hold closes it and stays in the menu",
       menu_view_opt_long() && s_note[0] == '\0' && s_armed && g_menu_exits == 0);
    g_note_row = -1; s_cur = 0;
    ck("the app page ends with the shell's back row", !strcmp(rowline(2), "<- back"));
    ck("the shell counts the app's rows plus its own back row", page_rows() == 3);
    ck("the header is the page's own title",
       (s_app->title && !strcmp(page_title(), s_app->title)));
    ck("back from an app page returns to that app's row",
       (s_pg = &s_pages[PG_APP], go_back(), s_pg == &s_pages[PG_TOP] && s_cur == top_os_rows()));

    /* ---- D12: one level of app-owned nesting, validated by the shell ----
     * The page is the clock's shape: a plain Factory default row, one GROUP
     * header, and the card's themes as its ITEMs. What is checked is the shell's
     * half — what gets drawn, where the cursor may go, and what the two
     * malformed shapes become — because the app that owns the answer must never
     * also own the owner's ability to reach a row. */
    s_armed = true; s_pg = &s_pages[PG_APP]; s_app = FAKE_SERVICE; s_cur = 0;
    g_opt_n = 5; g_nkind = 5; g_kind_hook = true;
    g_kind[0] = OPT_ROW; g_kind[1] = OPT_GROUP;
    g_kind[2] = g_kind[3] = g_kind[4] = OPT_ITEM;
    snprintf(g_opt_label1, sizeof g_opt_label1, "meadow");
    snprintf(g_opt_val1, sizeof g_opt_val1, "1 of 3: meadow");
    s_group_open = -1;
    ck("a shut group hides its items and leaves the plain row, header and back",
       page_rows() == 3 && vis_to_row(5, 2) == -1 && row_hidden(5, 2)
       && row_hidden(5, 4) && !row_hidden(5, 1));
    ck("a shut header answers with the app's own summary",
       !strcmp(rowline(1), "meadow  (1 of 3: meadow)"));
    /* Entering the app through the real door opens its one group with the page
     * (owner, 2026-10-26): the list is on the glass at once, the cursor rests
     * on the header, and nothing about the shape was reached by hand. */
    s_pg = &s_pages[PG_TOP]; s_cur = top_os_rows(); g_arms = 0;
    menu_view_apply_input(0, true);
    ck("entering the app opens its group, cursor on the header, knob armed",
       s_pg == &s_pages[PG_APP] && s_cur == 0 && s_group_open == 1 && g_arms == 1);
    ck("open, the page counts every row and the back",
       page_rows() == 6 && vis_to_row(5, 4) == 4 && !row_hidden(5, 4));
    ck("the open header still answers with the app's own summary, not the shell's state",
       !strcmp(rowline(1), "meadow  (1 of 3: meadow)"));
    /* The inert header: the press reaches the app at the header's own number —
     * vis_to_row(5,1)==1 — and the shell owns no answer that hides the list
     * behind a row that cannot bring it back. */
    s_cur = 1; g_activations = 0;
    menu_view_apply_input(0, true);
    ck("a press on the header reaches the app and toggles nothing",
       g_activations == 1 && g_last_opt_sel == 1
       && s_group_open == 1 && s_pg == &s_pages[PG_APP]);
    ck("an item is indented, so nesting is on the glass and not only in the ABI",
       !strncmp(rowline(2), "  ", 2));
    g_opt_val1[0] = '\0';                     /* an item with no value: the other branch */
    ck("a valueless item is indented too", !strncmp(rowline(2), "  ", 2));
    snprintf(g_opt_val1, sizeof g_opt_val1, "1 of 3: meadow");
    ck("the hint names the plain gestures and no escape hatch",
       (s_pg = &s_pages[PG_APP], !strcmp(page_hint(), "rotate: choose   push: change")));
    s_cur = 2;
    for (int t = 0; t < 6; t++) turn_down(1);      /* one row per event, by design */
    ck("inside an open group the walk steps down onto the page's back row",
       s_cur == 5);
    for (int t = 0; t < 6; t++) turn_up(1);
    ck("and up it clamps at the group's header, then walks free above it",
       s_cur == 0);
    ck("the group's own position maps back to the app's row number",
       row_to_vis(5, 2) == 2 && row_to_vis(5, 4) == 4 && row_to_vis(5, 1) == 1);
    /* The back row answers one level up, no intermediate: activate() routes the
     * last row to go_back(), and PG_APP's ordinal is the app's own top-page row. */
    s_cur = 5;
    menu_view_apply_input(0, true);
    ck("the back row leaves the app for its own top-page row",
       s_pg == &s_pages[PG_TOP] && s_cur == top_os_rows());
    s_pg = &s_pages[PG_APP]; s_app = FAKE_SERVICE; s_cur = 3; s_armed = true; g_menu_exits = 0;
    /* Owner ruling, 2026-10-26: with the header inert, the hold on the app's
     * page exits the menu — collapsing a list under a row that cannot reopen
     * it would be a trap. The app's own long hook still takes it first. The
     * list rides out whole: the next open_app() raises it from the shape. */
    ck("the hold on the open group's page exits the menu, list and all",
       menu_view_opt_long() && g_menu_exits == 1 && !s_armed);
    s_armed = true; s_pg = &s_pages[PG_APP]; s_app = FAKE_SERVICE; s_cur = 0;
    ck("a shut group gives the page its three rows back",
       (s_group_open = -1, page_rows() == 3));
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
    g_kind_hook = true; g_nkind = 0; g_opt_n = 2; g_opt_app_count = 1;
    s_group_open = -1; s_cur = 0;

    /* ---- the app's own scan landing must repaint the shell (B2 reachability) --
     * Entering the theme page posts the clock's card walk; that walk reaps in
     * orb_app_service_all() before menu_service runs, so the app already has its
     * rows and the menu's OWN s_dirty is still false. If menu_view_service()
     * repainted only on s_dirty, the page would open empty and only fill once
     * the owner turned the knob. g_repaints is the witness: the count going up
     * with s_dirty clear is exactly the fixed behaviour, and deleting the
     * app_grew re-check from the shipped file drops it back to no repaint. */
    g_opt_n = 0; s_pg = &s_pages[PG_APP]; s_app = FAKE_SERVICE; s_cur = 0; s_armed = true;
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
    /* §58: a Themes group that lands with the walk opens as entry would have. */
    g_opt_n = 1; g_kind_hook = true; g_nkind = 0; s_group_open = -1; s_dirty = false;
    repaint();
    g_opt_n = 4; g_nkind = 4;
    g_kind[0] = OPT_ROW; g_kind[1] = OPT_GROUP; g_kind[2] = g_kind[3] = OPT_ITEM;
    menu_view_service();
    ck("a group that lands after entry opens with its items on the glass",
       s_group_open == 1 && page_rows() == 5);
    g_kind_hook = false; g_nkind = 0; s_group_open = -1;
    g_opt_n = 2;

    /* ---- the preview plane landing repaints the shell (step 4) -------------
     * The plane arrives on the app's own worker, and it changes no row count:
     * a menu_view_service() that repainted only on s_dirty and app_grew would
     * sit showing text under a plane the app already has — the report "I still
     * don't see any previews". The fake answers with one plane after the
     * settle, and g_repaints is the witness again; the mutation list pins that
     * dropping the pv_landed re-check reddens this check. */
    g_opt_n = 3; g_kind_hook = true; g_nkind = 3;
    g_kind[0] = OPT_GROUP; g_kind[1] = OPT_ITEM; g_kind[2] = OPT_ITEM;
    g_pv_px = NULL;
    s_pg = &s_pages[PG_APP]; s_app = FAKE_SERVICE; s_cur = 0; s_armed = true;
    s_group_open = 0; s_cur = 1;                    /* cursor on an item row  */
    s_dirty = false;
    repaint();                                      /* settled: no plane yet  */
    const int pv_base = g_repaints;
    s_dirty = false;
    menu_view_service();
    ck("no plane, no repaint", g_repaints == pv_base && s_pv_stamp == 0);
    pv_set(g_fake_plane, 232, 232, 7);               /* the read landed        */
    s_dirty = false;
    menu_view_service();
    ck("the plane landing repaints with every menu flag clear",
       g_repaints > pv_base && s_pv_stamp == 7);
    menu_view_service();
    ck("and settles once it has caught up, plane stamped",
       g_repaints == pv_base + 1);
    pv_set(g_fake_plane, 232, 232, 9);               /* a re-read, new answer  */
    s_dirty = false;
    menu_view_service();
    ck("a stamp change repaints even on the same buffer", g_repaints > pv_base + 1);
    pv_set(NULL, 0, 0, 0);                           /* the plane withdrawn    */
    s_dirty = false;
    const int off_paints = g_repaints;
    menu_view_service();
    ck("a plane taken back repaints the list back",
       g_repaints > off_paints && s_pv_stamp == 0);
    g_kind_hook = false; g_opt_n = 2; g_app_rows_n = 3;
    s_group_open = -1; s_cur = 0;
    /* Entered properly: the go_back() above left PG_TOP with s_app cleared, and
     * an app page that was never entered proves nothing about leaving one. */
    g_opt_n = 0; s_pg = &s_pages[PG_APP]; s_app = FAKE_SERVICE; s_cur = 0;
    ck("an app page whose app answers nothing still lets you out", page_rows() == 1);
    ck("that one row is the back row", !strcmp(rowline(0), "<- back"));
    g_opt_n = 2;
    g_menu_n = 0; s_app = NULL;

    /* ---- a running job freezes the list ---- */
    /* page_rows() ignores s_busy, so apply_input's own guard is the only thing
     * holding the cursor here — the only place it can be seen. (The zone list,
     * the timeout presets, then the caps page carried this check until each
     * left the dial.) */
    s_pg = &s_pages[PG_UNITS]; s_cur = 0; s_busy = true;
    turn_down(1);
    ck("cursor frozen on a list while a job runs", s_cur == 0);
    g_posts = 0;
    menu_view_apply_input(0, true);
    ck("press frozen too", g_posts == 0);
    s_busy = false;
    turn_down(1);
    ck("the cursor moves again once the job lands", s_cur == 1);

    /* ---- the DISPLAY submenu: the dim switch and the timeout picker ----
     * These two rows are the only way to reach the idle dim on the dial, and the
     * failure modes are invisible on a bench: a picker that marks the wrong preset
     * tells the owner a 10 minute timer is in force while the panel actually dims
     * after 45 seconds. */
    s_pg = &s_pages[PG_DISPLAY]; s_cur = 0; g_dim_on = true; g_dim_to = 600;
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
       (s_pg = &s_pages[PG_DISPLAY], !strcmp(page_hint(), "rotate: choose   push: change")));
    ck("back from display returns to the DISPLAY row",
       (s_pg = &s_pages[PG_DISPLAY], go_back(), s_pg == &s_pages[PG_TOP] && s_cur == 1 /* s_top[1] */));

    /* ---- docs/knob-edit-plan.md: the knob edit mode ----
     * The value's sense is the knob layer's: "more" is the column step's +1, the
     * same physical turn as "up" (knob.h), derived here and never assumed. The
     * clock advances 500 ms a call unless a check makes the turns fast. */
    const int32_t vup = knob_col_step(1) == 1 ? 1 : -1;
    s_pg = &s_pages[PG_DISPLAY]; s_cur = 1; g_dim_to = 600; g_posts = 0; s_busy = false;
    g_tick_us = 500000;
    menu_view_apply_input(0, true);
    ck("a press on the timeout enters the edit at its minutes", s_edit.on && s_edit.e.v == 10);
    ck("the edit's hint says how to save and how to leave",
       !strcmp(page_hint(), "rotate: adjust   push: save   hold: cancel"));
    menu_view_apply_input(vup, false);
    ck("a slow turn up moves the value by one, and the row reads it",
       s_edit.e.v == 11 && !strcmp(rowline(1), "timeout:  11 min"));
    ck("a turn in the edit leaves the cursor where it is", s_cur == 1);
    menu_view_apply_input(-vup, false);
    menu_view_apply_input(-vup, false);
    ck("a slow turn down moves it by one each", s_edit.e.v == 9);
    g_tick_us = 10000;                       /* a hundred detents a second */
    for (int k = 0; k < 12; k++) menu_view_apply_input(vup, false);
    ck("a fast run takes the large step and rests on its multiples",
       s_edit.e.v > 30 && s_edit.e.v % 5 == 0);
    g_tick_us = 500000;
    ck("a hold cancels the edit, and nothing is saved",
       menu_view_opt_long() && !s_edit.on && g_posts == 0);
    ck("after a cancel the row reads the value in force", !strcmp(rowline(1), "timeout:  10 min"));
    g_dim_to = 120;
    menu_view_apply_input(0, true);
    for (int k = 0; k < 4; k++) menu_view_apply_input(-vup, false);
    ck("the edit clamps at its floor of one minute", s_edit.e.v == 1);
    menu_view_apply_input(0, true);
    ck("and saves it", g_posts == 1 && g_posted_pack == 1);
    g_posts = 0; g_dim_to = 600;
    s_cur = 0;
    menu_view_apply_input(0, true);
    ck("the dim switch is still a press, never an edit",
       !s_edit.on && g_posts == 1 && g_posted_fn == job_dim_en);
    g_posts = 0; s_cur = 1;
    menu_view_apply_input(0, true);
    go_back();
    ck("leaving the page ends the edit", !s_edit.on);
    s_pg = &s_pages[PG_DISPLAY]; s_cur = 1;
    menu_view_apply_input(0, true);
    menu_view_set_armed(false);
    ck("disarming ends the edit", !s_edit.on);
    s_armed = true;
    /* An app's row: the app says whether the knob edits it. */
    s_pg = &s_pages[PG_APP]; s_app = FAKE_APP; g_opt_n = 2; g_kind_hook = false;
    g_opt_editable = true; g_activations = 0; g_opt_set_v = -999; s_cur = 0;
    snprintf(g_opt_label0, sizeof g_opt_label0, "Level");
    menu_view_apply_input(0, true);
    ck("an app row the app edits enters the edit at its value", s_edit.on && s_edit.e.v == 40);
    menu_view_apply_input(vup, false);
    ck("the app row reads the value being set", !strcmp(rowline(0), "Level  (41)"));
    menu_view_apply_input(0, true);
    ck("the press saves through opt_set, never opt_sel",
       !s_edit.on && g_opt_set_i == 0 && g_opt_set_v == 41 && g_activations == 0);
    s_cur = 1;
    menu_view_apply_input(0, true);
    ck("an app row the app does not edit is still its press", !s_edit.on && g_activations == 1);
    g_opt_editable = false;
    s_cur = 0;
    menu_view_apply_input(0, true);
    ck("an app that edits nothing keeps every row a press", !s_edit.on && g_activations == 2);
    s_app = NULL;

    /* ---- a worker result landing while disarmed must NOT paint a list ---- */
    s_armed = true; s_pg = &s_pages[PG_TOP]; s_cur = 0;
    memset(&s_shared, 0, sizeof s_shared);
    snprintf(s_shared.status, sizeof s_shared.status, "zone set: Europe/Budapest");
    s_dirty = true;
    s_armed = false;                       /* the user rotated away mid-job */
    menu_view_service();
    ck("a landing never becomes a list because a result landed", page_rows() == 0);
    ck("the landing keeps its title", s_pg == &s_pages[PG_TOP]);


    s_armed = true; s_pg = &s_pages[PG_TOP]; s_cur = 0;
    memset(&s_shared, 0, sizeof s_shared);
    snprintf(s_shared.status, sizeof s_shared.status, "zone set: Europe/Budapest");
    s_dirty = true;
    menu_view_service();
    /* Armed, the same result must NOT move the page: menu_view_service() used to
     * jump to the theme confirm screen whenever a stage landed, which yanked a
     * page out from under an owner who had already scrolled elsewhere. Whether a
     * staged theme deserves a question is the clock's decision now; the shell's
     * job is to repaint and stay put. */
    ck("armed, a landing result does not change the page", s_pg == &s_pages[PG_TOP]);
    ck("armed, the result line is taken from the worker",
       !strcmp(s_ui.status, "zone set: Europe/Budapest"));

    /* ---- P5.3: APPS, its doors, and ABOUT ---------------------------------
     * The catalog is the registry's installed tier and nothing else: chrome
     * stays out, a faulted app stays IN with its reason (§43.3: no other state).
     * The two doors are the shipped orb_app_clear_faults()/orb_app_uninstall(), so
     * what is checked here is which press posts which pack and what the line
     * says when the door refuses — not the flash write behind it. */
    s_armed = true; s_pg = &s_pages[PG_TOP]; s_cur = 3; s_busy = false;
    g_posts = 0; g_act_calls = 0; g_en_calls = 0; g_un_calls = 0;
    g_fake_app.failed = false; g_fake_app.active = false;
    g_fake_app2.failed = false; g_fake_app2.active = false;
    g_current = NULL; g_fault_why = "";
    menu_view_apply_input(0, true);
    ck("the APPS row opens the catalog on its first app",
       s_pg == &s_pages[PG_APPS] && s_cur == 0);
    ck("the catalog is every installed app, the B5 count, the rotation, then back",
       page_rows() == 5 && !strcmp(rowline(0), "Clock  installed") &&
       !strcmp(rowline(1), "HSB  installed") &&
       !strcmp(rowline(2), "2 of 8 slots used") &&
       !strcmp(rowline(3), "Rotate apps  (off)") &&
       !strcmp(rowline(4), "<- back"));
    /* §59: the Rotate apps row edits by knob through its interval choices, off first,
     * and saves through its job, which writes the rotation the owner chose. */
    {
        menu_edit_t e; memset(&e, 0, sizeof e);
        ck("the Rotate apps row is the one row the knob edits here, off to an hour",
           edit_apps(3, &e) && e.r.min == 0 && e.r.max == ROT_N - 1 && e.v == 0 && !e.r.wrap &&
           !edit_apps(2, &e) && !edit_apps(0, &e));
        char t[48];
        fmt_apps(3, 3, t, sizeof t);
        ck("each choice reads as the interval", !strcmp(t, "Rotate apps  (every 1 min)"));
        int c = 2; g_publish_calls = 0;
        job_rot_every(&c);
        orb_rotation_t rr; orb_rotation_get(&rr);
        ck("saving a choice turns the rotation on at that interval and says so",
           rr.on && rr.every_s == 30 && g_publish_calls == 1 && !strcmp(g_pub_line, "rotate apps: every 30 s") &&
           !strcmp(rowline(3), "Rotate apps  (every 30 s)"));
        int pk = (0 << 1) | 1;
        job_rot_member(&pk);
        ck("an app's In rotation press puts it in, by its own slug",
           orb_rotation_takes_part("clock") && strstr(g_pub_line, "in the rotation"));
        pk = 0 << 1;
        job_rot_member(&pk);
        ck("and the next takes it out again", !orb_rotation_takes_part("clock") && strstr(g_pub_line, "out of"));
        c = 0;
        job_rot_every(&c);
        orb_rotation_get(&rr);
        ck("choosing off turns it off and keeps the interval", !rr.on && rr.every_s == 30);
    }
    ck("an installed app reads installed — there is no off (§43.3)",
       strstr(rowline(0), "off") == NULL && strstr(rowline(0), "enabled") == NULL);
    ck("chrome keeps out of the catalog", strstr(rowline(0), "Menu") == NULL &&
       strstr(rowline(1), "Menu") == NULL);
    g_current = FAKE_APP2;
    ck("the entry the glass is showing says so",
       !strcmp(rowline(1), "HSB  on the dial"));
    g_current = NULL;
    g_fake_app.failed = true; g_fault_why = "two faults in a row";
    ck("a failed app shows its reason rather than a bare switch",
       !strcmp(rowline(0), "Clock  failed: two faults in a row"));
    g_fake_app.failed = false; g_fault_why = "";
    g_app_rows_n = 0;
    ck("no installed app is a count row, the rotation and a back row, not an empty page",
       page_rows() == 3 && !strcmp(rowline(0), "0 of 8 slots used") &&
       !strcmp(rowline(2), "<- back"));
    g_app_rows_n = 3;
    s_cur = page_rows() - 1; menu_view_apply_input(0, true);
    ck("the catalog's back row lands on its own top row",
       s_pg == &s_pages[PG_TOP] && s_cur == 3);

    /* the doors — D38: show, uninstall, remove from card. No enable switch and
     * no off state (§43.3): a faulted app's first row is retry, and the press
     * posts C2's fault clear. They close the app's own page (2026-11-07); with
     * no settings and no caps declared they are its first rows, which keeps
     * every index below what it was when they had a page of their own. */
    g_opt_n = 0; g_decl[0] = g_decl[1] = 0; g_dmask[0] = g_dmask[1] = 0;
    s_pg = &s_pages[PG_APPS]; s_cur = 0; s_door_slot = -1; s_uninst_arm = -1;
    s_doormode = 0; s_card_sub = -1; s_cardrm_arm = -1;
    menu_view_apply_input(0, true);
    ck("an app row opens that app's own page, whose doors are its last rows",
       s_pg == &s_pages[PG_APP] && s_app == FAKE_APP && s_cur == 0 && s_door_slot == 0);
    ck("the page carries the app's name, not the shell's",
       !strcmp(page_title(), "Clock"));
    /* "show on the dial" is gone (owner, 2026-11-07): the app list and the carousel
     * open apps, and the door swapped the glass behind the router's back. */
    ck("a running app's doors are the two removals and back, nothing that opens it",
       page_rows() == 3 && !strcmp(rowline(0), "uninstall") &&
       !strcmp(rowline(1), "remove from card") && !strcmp(rowline(2), "<- back"));
    ck("the app page's hint stays the one-line plain answer, clear of its last row",
       !strcmp(page_hint(), "rotate: choose   push: change"));
    g_act_calls = 0; s_cur = 0;
    for (int r = 0; r < page_rows(); r++) { char b[64]; bool h; row_text(r, b, sizeof b, &h);
        if (strstr(b, "dial")) g_act_calls++; }
    ck("no row of the page offers to put the app on the dial", g_act_calls == 0);

    /* §43.3: a faulted app leads with retry; the press is C2's fault clear. */
    g_fake_app.failed = true;
    ck("the faulted app's doors lead with retry, not a switch",
       !strcmp(rowline(0), "retry") && !strcmp(rowline(1), "uninstall") &&
       strstr(rowline(0), "switch") == NULL && strstr(rowline(1), "switch") == NULL);
    g_posts = 0; s_cur = 0; menu_view_apply_input(0, true);
    ck("the retry press posts the fault clear with the slot",
       g_posts == 1 && g_posted_fn == job_clear && g_posted_pack == ((0 << 2) | 1));
    ck("the pack is still the slot-high, one-bit-low contract",
       door_pack(FAKE_APP, 1) == ((0 << 2) | 1) &&
       door_pack(FAKE_APP2, 0) == ((1 << 2) | 0));
    g_en_rc = 0; g_publish_calls = 0; g_posted_fn(&g_posted_pack);
    ck("the retry answer names the app and clears that app's faults",
       g_publish_calls == 1 && !strcmp(g_pub_line, "\"Clock\": fault cleared - it runs after the reboot") &&
       g_en_calls == 1 && g_en_slot == 0);
    g_en_rc = ESP_ERR_INVALID_STATE_; g_en_why = "a card job is running";
    g_posts = 0; g_publish_calls = 0; s_cur = 0; menu_view_apply_input(0, true);
    g_posted_fn(&g_posted_pack);
    ck("a refused retry prints the door's sentence, not the menu's",
       g_publish_calls == 1 && !strcmp(g_pub_line, "\"Clock\": a card job is running"));
    g_en_rc = 0; g_en_why = "";

    /* the two-press uninstall (row 1 of the off app's doors), and the
     * detent that cancels it — arm, prompt, cancel, and the worker's
     * refusal that must survive a cancel (bench, orb-B 2026-10-24). */
    g_posts = 0; s_cur = 1; menu_view_apply_input(0, true);
    ck("the first uninstall press arms and posts nothing",
       g_posts == 0 && s_uninst_arm == 0 &&
       strstr(s_ui.status, "press uninstall again") != NULL);
    ck("the armed row says so on the glass",
       !strcmp(rowline(1), "uninstall - press again"));
    turn_up(1);
    ck("a detent ends the arm", s_uninst_arm == -1);
    ck("and takes the confirmation's line off the status row",
       s_ui.status[0] == '\0');
    g_posts = 0; s_cur = 1; menu_view_apply_input(0, true);
    ck("the next press arms again rather than erasing",
       g_posts == 0 && g_un_calls == 0 && s_uninst_arm == 0);
    memset(&s_shared, 0, sizeof s_shared);
    snprintf(s_shared.status, sizeof s_shared.status, "\"Clock\": it is on the dial");
    s_dirty = true;
    menu_view_service();
    ck("a refusal landing while the row stands armed reaches the line",
       !strcmp(s_ui.status, "\"Clock\": it is on the dial") && s_uninst_arm == 0);
    turn_up(-1);
    ck("cancelling the arm leaves that answer where it is",
       s_uninst_arm == -1 && !strcmp(s_ui.status, "\"Clock\": it is on the dial"));
    g_posts = 0; s_cur = 1; menu_view_apply_input(0, true);
    s_cur = 1; menu_view_apply_input(0, true);
    ck("the second press on the same row posts the removal",
       g_posts == 1 && g_posted_fn == job_uninstall && g_posted_pack == (0 << 2) &&
       s_uninst_arm == -1);
    g_un_rc = 0; g_publish_calls = 0; g_posted_fn(&g_posted_pack);
    ck("a removal says the card copy survives",
       g_un_calls == 1 && g_un_slot == 0 &&
       !strcmp(g_pub_line, "\"Clock\": uninstalled, the card copy stays"));
    g_un_rc = ESP_ERR_INVALID_STATE_; g_un_why = "the dial would not leave the app";
    g_posts = 0; g_publish_calls = 0; s_cur = 1; menu_view_apply_input(0, true);
    s_cur = 1; menu_view_apply_input(0, true);
    g_posted_fn(&g_posted_pack);
    ck("a refused removal prints the door's reason",
       g_publish_calls == 1 && !strcmp(g_pub_line, "\"Clock\": the dial would not leave the app"));
    g_un_rc = 0; g_un_why = "";

    /* D38: remove from card — the row's own two arms, uninstall first,
     * and the file's own path to the file-manager door. */
    g_posts = 0; g_rm_calls = 0; g_un_calls = 0;
    s_cur = 2; menu_view_apply_input(0, true);
    ck("the first card-removal press arms its own row, not the uninstall",
       g_posts == 0 && s_cardrm_arm == 0 && s_uninst_arm == -1 &&
       strstr(s_ui.status, "press again to erase the card copy") != NULL);
    ck("the armed card row says so on the glass",
       !strcmp(rowline(2), "remove from card - press again"));
    turn_up(1);
    ck("a detent ends the card arm too", s_cardrm_arm == -1);
    ck("and its line off the glass", s_ui.status[0] == '\0');
    g_scan_n = 1;
    strcpy(g_scan[0].stem, "clock"); strcpy(g_scan[0].slug, "clock");
    strcpy(g_scan[0].title, "Clock"); g_scan[0].err = ESP_OK;
    g_rm_rc = 0; g_publish_calls = 0;
    g_posts = 0; s_cur = 2; menu_view_apply_input(0, true);
    s_cur = 2; menu_view_apply_input(0, true);
    ck("the second press posts the card removal",
       g_posts == 1 && g_posted_fn == job_cardrm && s_cardrm_arm == -1);
    g_posted_fn(&g_posted_pack);
    ck("the removal uninstalls first, then erases the card's own path",
       g_un_calls == 1 && g_rm_calls == 1 && !strcmp(g_rm_rel, "apps/clock.orbapp") &&
       !strcmp(g_pub_line, "\"Clock\": gone from the dial and the card"));
    g_scan_n = 0; g_un_calls = 0; g_rm_calls = 0; g_publish_calls = 0;
    g_posts = 0; s_cur = 2; menu_view_apply_input(0, true);
    s_cur = 2; menu_view_apply_input(0, true);
    g_posted_fn(&g_posted_pack);
    ck("a card with no file says so once rather than lying twice",
       g_un_calls == 1 && g_rm_calls == 0 &&
       !strcmp(g_pub_line, "\"Clock\": uninstalled; the card held no copy"));
    g_scan_n = 1;
    strcpy(g_scan[0].stem, "clock"); strcpy(g_scan[0].slug, "clock");
    strcpy(g_scan[0].title, "Clock"); g_scan[0].err = ESP_OK;
    g_rm_rc = ESP_FAIL; g_un_calls = 0; g_rm_calls = 0; g_publish_calls = 0;
    g_posts = 0; s_cur = 2; menu_view_apply_input(0, true);
    s_cur = 2; menu_view_apply_input(0, true);
    g_posted_fn(&g_posted_pack);
    ck("a card that refuses the delete says so",
       g_rm_calls == 1 && !strcmp(g_pub_line, "\"Clock\": the card refused the delete"));
    g_rm_rc = 0; g_scan_n = 0;
    g_opt_n = 2;
    s_pg = &s_pages[PG_APPS];                 /* leave the app page the way back does */

    /* D38: the catalog's card tier and the card file's doors. The
     * snapshot lands through the shipped path: entering APPS posts the walk,
     * and the worker runs it — here, by hand, on the fake card. */
    g_scan_n = 1;
    strcpy(g_scan[0].stem, "thermo"); strcpy(g_scan[0].slug, "thermo");
    strcpy(g_scan[0].title, "Thermo"); g_scan[0].err = ESP_OK;
    s_pg = &s_pages[PG_APPS];
    enter_apps();
    { int zero = 0; job_scan(&zero); }
    ck("the uninstalled card file rides under the registry rows",
       !strcmp(rowline(2), "Thermo  on the card") &&
       !strcmp(rowline(3), "2 of 8 slots used"));
    s_cur = 2; g_posts = 0;
    menu_view_apply_input(0, true);
    ck("a card file's doors: install, remove from card, back",
       s_pg == &s_pages[PG_APPACT] && s_doormode == 1 && page_rows() == 3 &&
       !strcmp(rowline(0), "install") &&
       !strcmp(rowline(1), "remove from card") &&
       !strcmp(rowline(2), "<- back"));
    ck("a card file's title heads its doors page",
       !strcmp(page_title(), "Thermo APP"));
    g_install_rc = 0; g_install_calls = 0; g_en_calls = 0; g_posts = 0; g_publish_calls = 0;
    s_cur = 0; menu_view_apply_input(0, true);
    ck("the card install posts its own job, not the enable job",
       g_posts == 1 && g_posted_fn == job_install_card);
    g_posted_fn(&g_posted_pack);
    ck("the install runs the sync door on the file's own stem",
       g_install_calls == 1 && !strcmp(g_install_stem, "thermo") &&
       !strcmp(g_pub_line, "\"Thermo\": installed"));
    g_install_rc = ESP_FAIL; g_install_calls = 0; g_posts = 0; g_publish_calls = 0;
    s_cur = 0; menu_view_apply_input(0, true);
    g_posted_fn(&g_posted_pack);
    ck("a refused install prints the install door's sentence",
       g_install_calls == 1 && !strcmp(g_pub_line, "\"Thermo\": install refused"));
    g_install_rc = 0;
    g_scan[0].err = (esp_err_t)-2; strcpy(g_scan[0].why, "bad header");
    g_scan[0].slug[0] = '\0'; g_scan[0].title[0] = '\0';   /* pre-CRC blanking */
    { int zz = 0; job_scan(&zz); }
    s_doormode = 1;
    ck("a file the walk refuses offers no install, only its erasure",
       page_rows() == 2 && !strcmp(rowline(0), "remove from card") &&
       !strcmp(rowline(1), "<- back"));
    s_pg = &s_pages[PG_APPS];
    ck("the catalog says the refused file cannot install",
       !strcmp(rowline(2), "thermo  cannot install"));
    g_scan_n = 0;
    { int zz = 0; job_scan(&zz); }
    s_pg = &s_pages[PG_APPACT]; s_doormode = 0; s_door_slot = 0;
    s_cur = page_rows() - 1; menu_view_apply_input(0, true);
    ck("the doors page back lands on its own catalog row",
       s_pg == &s_pages[PG_APPS] && s_cur == 0);
    /* The two follow-throughs (Gold, 2026-11-07): a page whose subject changed
     * tier under it follows the app instead of drawing "? APP" over a lone back
     * row, which read as a hung dial. Install from the card lands on the app's
     * own page; uninstall from that page lands on its card file's doors. */
    g_scan_n = 1;
    strcpy(g_scan[0].stem, "hue"); strcpy(g_scan[0].slug, "hue");
    strcpy(g_scan[0].title, "HSB"); g_scan[0].err = ESP_OK;
    { int zz = 0; job_scan(&zz); }
    s_armed = true; s_pg = &s_pages[PG_APPACT]; s_doormode = 1; s_card_sub = 0; s_door_slot = -1;
    strcpy(s_pend_slug, "hue"); g_fake_app2.installed = true;
    memset(&s_shared, 0, sizeof s_shared);
    snprintf(s_shared.status, sizeof s_shared.status, "\"HSB\": installed");
    s_dirty = true;
    menu_view_service();
    ck("an install from the card follows the app onto its own page",
       s_pg == &s_pages[PG_APP] && s_app == FAKE_APP2 && s_cur == 0 && s_door_slot == 1);
    g_fake_app2.installed = false;
    snprintf(s_shared.status, sizeof s_shared.status, "\"HSB\": uninstalled, the card copy stays");
    s_dirty = true;
    menu_view_service();
    ck("an uninstall from its page follows the app to its card file's doors",
       s_pg == &s_pages[PG_APPACT] && s_doormode == 1 && s_card_sub == 0 && s_app == NULL &&
       !strcmp(rowline(0), "install"));
    g_fake_app2.installed = true; g_scan_n = 0; s_pend_slug[0] = '\0';
    { int zz = 0; job_scan(&zz); }
    s_armed = true; s_pg = &s_pages[PG_TOP]; s_cur = 3; menu_view_apply_input(0, true);
    ck("back into the catalog clears the subject and both arms with it",
       s_pg == &s_pages[PG_APPS] && s_door_slot == -1 && s_doormode == 0 &&
       s_card_sub == -1 && s_uninst_arm == -1 && s_cardrm_arm == -1);

    /* ABOUT: the address first (the QR's row), five facts, the five credits
     * (radar-plan §6.7), and no press that does anything */
    s_pg = &s_pages[PG_TOP]; s_cur = 0; g_posts = 0;
    menu_view_apply_input(0, true);
    ck("ABOUT opens on its address row, and the QR is what shows",
       s_pg == &s_pages[PG_ABOUT] && page_rows() == 11 && s_cur == 0 && about_qr_on());
    ck("on the QR the hint says who scans it",
       !strcmp(page_hint(), "scan with a phone on this Wi-Fi"));
    ck("the facts are the address, the image, the card, the count and the zone",
       !strcmp(rowline(0), "address  192.168.2.32") &&
       !strcmp(rowline(1), "firmware  abc1234") &&
       !strcmp(rowline(2), "card  SDSC") &&
       !strcmp(rowline(3), "installed  2 of 8") &&
       !strcmp(rowline(4), "zone  Europe/Bucharest") &&
       !strcmp(rowline(10), "<- back"));
    ck("then the credits: OpenStreetMap and its licence's address, the tiles, the radar and the weather",
       !strcmp(rowline(5), "maps  OpenStreetMap contributors") &&
       !strcmp(rowline(6), "openstreetmap.org/copyright") &&
       !strcmp(rowline(7), "tiles  OpenMapTiles, OpenFreeMap") &&
       !strcmp(rowline(8), "radar  LibreWXR (CC BY 4.0)") &&
       !strcmp(rowline(9), "weather  Open-Meteo (CC BY 4.0)"));
    turn_down(1);
    ck("a turn off the address puts the list back",
       s_cur == 1 && !about_qr_on() && strstr(page_hint(), "nothing here") != NULL);
    menu_view_apply_input(0, true);
    ck("a press on a fact row does nothing at all",
       s_pg == &s_pages[PG_ABOUT] && s_cur == 1 && g_posts == 0);
    s_cur = 0; g_ip = "";
    ck("with no address there is no code to show, and the row says why",
       !about_qr_on() && !strcmp(rowline(0), "address  none (not on Wi-Fi)")
       && strstr(page_hint(), "nothing here") != NULL);
    g_ip = "192.168.2.32";
    s_armed = false;
    ck("unarmed, no QR whatever the page was", !about_qr_on());
    s_armed = true;
    g_sd_mounted = 0;
    ck("no card is stated, not left blank", !strcmp(rowline(2), "card  none"));
    g_sd_mounted = 1;
    s_cur = page_rows() - 1; menu_view_apply_input(0, true);
    ck("ABOUT back lands on its own top row",
       s_pg == &s_pages[PG_TOP] && s_cur == 0);
    s_armed = true; s_pg = &s_pages[PG_TOP]; s_cur = 0; s_busy = false;

    /* ---- P5.4: the position readout on a list that scrolls -----------------
     * The header is the only place a long picker can say where the cursor is:
     * five physical rows show a window, and the highlight answers "which of
     * these" and never "how far from the end". The gate is the window's own, so
     * these checks pin the boundary in both directions and the count's
     * dependence on the cursor rather than on the page id. */
    static char hb[64];
    /* UNITS: five quantities and back, six rows on a five-row window. */
    s_armed = true; s_busy = false; s_pg = &s_pages[PG_UNITS]; s_cur = 0;
    title_line(hb, sizeof hb);
    ck("a scrolled page names the row and the total", !strcmp(hb, "UNITS  1/6"));
    s_cur = 5; title_line(hb, sizeof hb);
    ck("the readout reaches the last row, the back row included", !strcmp(hb, "UNITS  6/6"));
    s_cur = 3; turn_down(1); title_line(hb, sizeof hb);
    ck("the count moves with the cursor", !strcmp(hb, "UNITS  5/6"));
    s_cur = 0; turn_up(1); title_line(hb, sizeof hb);
    ck("and does not move past the clamp", !strcmp(hb, "UNITS  1/6"));
    s_pg = &s_pages[PG_DISPLAY]; title_line(hb, sizeof hb);
    ck("a page that draws every row says no digits", !strcmp(hb, "DISPLAY"));
    s_pg = &s_pages[PG_UNITS]; s_cur = 0; title_line(hb, sizeof hb);
    ck("six rows overflow the five-row window, so the page counts",
       page_rows() == 6 && !strcmp(hb, "UNITS  1/6"));
    /* The gate is the count crossing the glass, not a per-page flag: the same
     * page function gains the readout at 6 rows and has none at 5, which is the
     * only shape of this rule that survives a page nobody thought to annotate.
     * A service page's rows are its options plus back (an installed app's page
     * adds its caps and doors), so its option count walks the boundary. */
    g_menu_n = 1; s_ui.status[0] = '\0';
    s_pg = &s_pages[PG_APP]; s_app = FAKE_SERVICE; g_kind_hook = false; s_cur = 0; g_opt_n = 4;
    title_line(hb, sizeof hb);
    ck("five rows fill the window exactly and say no digits",
       page_rows() == 5 && !strcmp(hb, "USB Files"));
    /* A status line takes the fifth row's place (Gold, 2026-11-07: "installed"
     * was drawn over the uninstall row), so the same five rows now scroll. */
    snprintf(s_ui.status, sizeof s_ui.status, "\"HSB\": installed");
    title_line(hb, sizeof hb);
    ck("with a status line up, four rows show and five rows count",
       rows_on_glass() == 4 && !strcmp(hb, "USB Files  1/5"));
    s_ui.status[0] = '\0';
    g_opt_n = 5; title_line(hb, sizeof hb);
    ck("one row more than the glass and it counts",
       page_rows() == 6 && !strcmp(hb, "USB Files  1/6"));
    s_armed = false; title_line(hb, sizeof hb);
    ck("unarmed, the count is gone whatever the page was",
       !strcmp(hb, "USB Files"));
    g_opt_n = 2; s_app = NULL; g_menu_n = 0;
    s_armed = true; s_pg = &s_pages[PG_TOP]; s_cur = 0; s_busy = false;

    /* ---- the glyph floor: what the dial can actually draw ------------------
     * The face is generated with `-r 0x20-0x7F,0xB0,0x2022` and there is no
     * fallback, so an unmapped codepoint paints LVGL's missing-glyph box — the
     * report this suite exists to make impossible. dial_text() is lifted, not
     * restated, so these vectors run against the shipped table. */
    {
        char gb[64];
        dial_text(gb, sizeof gb, "APPS");
        ck("ascii row text comes back untouched", !strcmp(gb, "APPS"));
        dial_text(gb, sizeof gb, "uninstall \xe2\x80\x94 press again");
        ck("the armed uninstall row draws a hyphen, not a box",
           !strcmp(gb, "uninstall - press again"));
        dial_text(gb, sizeof gb, "\xe2\x80\xa6loading");
        ck("an ellipsis takes three dots", !strcmp(gb, "...loading"));
        dial_text(gb, sizeof gb, "don\xe2\x80\x99t");
        ck("a curly apostrophe takes the straight one", !strcmp(gb, "don't"));
        dial_text(gb, sizeof gb, "21\xc2\xb0" "C");
        ck("U+00B0 survives — the face was generated with it",
           !strcmp(gb, "21\xc2\xb0" "C"));
        dial_text(gb, sizeof gb, "\xe2\x80\xa2one");
        ck("U+2022 survives, same reason", !strcmp(gb, "\xe2\x80\xa2one"));
        dial_text(gb, sizeof gb, "\xe4\xb8\x96");
        ck("a codepoint with no ASCII twin asks the question mark",
           !strcmp(gb, "?"));
        dial_text(gb, sizeof gb, "\xe2");                      /* lead, no tail */
        ck("a lone lead byte asks the question mark", !strcmp(gb, "?"));
        dial_text(gb, sizeof gb, "\xe2\x80");          /* lead + one of two tails */
        ck("torn UTF-8 marks each byte it cannot read, and stops at the string",
           !strcmp(gb, "??"));
        dial_text(gb, sizeof gb, "a\xe2\x80" "b");
        ck("so a torn byte in the middle costs two marks, not the rest of the row",
           !strcmp(gb, "a??b"));
        memset(gb, 0x5a, sizeof gb);                     /* canary the bound */
        dial_text(gb, 12, "uninstall \xe2\x80\x94 press again");
        ck("a tight bound truncates at a whole substitution, never half",
           !strcmp(gb, "uninstall -"));
        ck("and the fold never writes past the bound it was given",
           gb[12] == (char)0x5a);
        dial_text(gb, sizeof gb, "");
        ck("the empty line stays the empty line", gb[0] == '\0');
    }

    /* ---- item 3: the menu's own "where was I", across a disarm and a reboot --
     * The dial used to open every visit on the top page at row 0. These run the
     * SHIPPED pos_save/pos_load (lifted, not restated) through the fake NVS, so
     * the round-trip is the code that ships and the refusal paths are real. */
    {
        /* A first exit, on a restorable page, writes the record once. */
        nvs_reset(); s_pos_loaded = false; s_pos_pg = PG_TOP; s_pos_cur = 0;
        s_armed = true; s_pg = &s_pages[PG_TOP]; s_cur = 3; s_busy = false;
        menu_view_set_armed(false);
        ck("disarming stores the page and row the owner left",
           g_nvs_written && g_nvs_pg == PG_TOP && g_nvs_cur == 3);
        /* Re-arming reads it back through pos_load: DISPLAY has no enter() hook,
         * so the stored row is the cursor the dial opens on. */
        s_pos_loaded = false;
        s_pg = &s_pages[PG_DISPLAY]; s_cur = 0;
        menu_view_set_armed(false);
        s_pos_loaded = false;
        menu_view_set_armed(true);
        ck("re-arming opens on the page and row the last visit left",
           s_pg == &s_pages[PG_TOP] && s_cur == 3);

        /* The retired preset page (docs/knob-edit-plan.md): a store an older build
         * made of it opens the top page, never a page with no rows. */
        nvs_reset(); s_pos_loaded = false; s_pos_pg = PG_TOP; s_pos_cur = 0;
        g_nvs_written = true; g_nvs_pg = PG_TO_RETIRED; g_nvs_cur = 6;
        s_armed = false;
        menu_view_set_armed(true);
        ck("a remembered retired page opens the top page",
           s_pg == &s_pages[PG_TOP] && s_cur == 0);

        /* The stale-row clamp: a stored row past a page that shrank (an app
         * uninstalled since) is pulled back onto the glass, never left off it. */
        g_nvs_written = true; g_nvs_pg = PG_UNITS; g_nvs_cur = 40;
        s_pos_loaded = false; s_armed = false;
        menu_view_set_armed(true);           /* UNITS has no enter(); s_cur=40 */
        ck("a restored row past the count is clamped into the list by repaint",
           s_pg == &s_pages[PG_UNITS] && s_cur < page_rows());
        /* The retired PERMISSIONS page (2026-11-07): an older build's store of it
         * opens the top page, never a page with no rows. */
        nvs_reset(); s_pos_pg = PG_TOP; s_pos_cur = 0;
        g_nvs_written = true; g_nvs_pg = PG_PERMS; g_nvs_cur = 1;
        s_pos_loaded = false; s_armed = false;
        menu_view_set_armed(true);
        ck("a remembered PERMISSIONS page opens the top page",
           s_pg == &s_pages[PG_TOP] && s_cur == 0);

        /* A non-restorable page is never stored, so leaving from an app page
         * cannot leave a pointer the next boot has no way to resolve. */
        nvs_reset(); s_pos_loaded = false; s_pos_pg = PG_TOP; s_pos_cur = 0;
        s_armed = true; s_pg = &s_pages[PG_APP]; s_app = FAKE_APP; s_cur = 2;
        menu_view_set_armed(false);
        ck("leaving from an app page stores nothing (the app may be gone)",
           !g_nvs_written && s_pos_pg == PG_TOP);
        s_app = NULL;
    }

    /* ---- plan §51: the app list a hold opens ---- */
    {
        g_opt_app_count = 2;                  /* Clock, then HSB, in the walk's order */
        nvs_reset(); g_nvs_written = false;
        menu_view_switcher(FAKE_APP2, 0);
        ck("the list opens armed, on the app it was opened from",
           s_armed && s_pg == &s_pages[PG_GO] && s_cur == 1);
        ck("one row per app, then settings, and no back row",
           page_rows() == 3 && s_pages[PG_GO].back_to == MENU_GO_NONE);
        char b[40]; bool hi;
        row_text(0, b, sizeof b, &hi); const bool r0 = strcmp(b, "Clock") == 0;
        row_text(2, b, sizeof b, &hi);
        ck("rows read the apps' titles, and the last is settings", r0 && strcmp(b, "settings") == 0);
        menu_view_switcher(FAKE_APP, 1);
        ck("a turn's step moves the cursor off the app it came from", s_cur == 1);
        menu_view_switcher(FAKE_APP2, 5);
        ck("the step clamps at the last row", s_cur == 2);
        menu_view_switcher(FAKE_APP, -3);
        ck("and at the first", s_cur == 0);
        menu_view_switcher(&g_fake_shell, 0);
        ck("from a face that is not a row, the list starts at the top", s_cur == 0);
        g_shown = NULL;
        menu_view_switcher(FAKE_APP, 1);
        menu_view_apply_input(0, true);
        ck("a press on an app's row opens that app", g_shown == FAKE_APP2);
        menu_view_switcher(FAKE_APP, 2);
        menu_view_apply_input(0, true);
        ck("a press on settings opens the top page", s_armed && s_pg == &s_pages[PG_TOP] && s_cur == 0);
        menu_view_switcher(FAKE_APP, 0);
        menu_view_set_armed(false);
        ck("leaving the list stores no position (it always opens on an app)", !g_nvs_written);
        g_menu_exits = 0;
        menu_view_switcher(FAKE_APP, 0);
        ck("a hold on the list leaves the menu", menu_view_opt_long() && g_menu_exits == 1);
        g_opt_app_count = 1;
    }

    printf(fails ? "HARNESS FAILURES %d\n" : "HARNESS OK\n", fails);
    return fails ? 1 : 0;
}
"""


# The caps the shipped orb_theme_entry_t is sized with, lifted from the header
# alongside the struct itself: the row prints the name and addresses the theme by its
# id, and a harness that hardcoded either length would keep passing while the device's
# struct grew past it. The stem pair arrived with the 2026-10-24 card-naming rule.
ENTRY_DEFS = "\n".join(lift(th_hdr, r"#define " + k + r"\s+\d+[^\n]*", TH_H)
                      for k in ("ORB_THEME_NAME_MAX", "ORB_THEME_ID_HEX",
                                "ORB_THEME_HANDLE_MAX", "ORB_THEME_APP_MAX"))

# D38: the lifted APPS/APPACT block reaches for the card walk, the sync install,
# the file-manager rm, and strlcpy — none of which the pre-D38 harness knew.
# The scan row's shape is restated here at the shipped caps (asserted against
# the shipped orb_app.h's words), and the doors are fakes the checks set and
# read like the enable/uninstall fakes beside main().
SLUGDEF = r"""
static size_t host_strlcpy(char *d, const char *s, size_t n)
{
    const size_t l = strlen(s);
    if (n) { snprintf(d, n, "%s", s); d[n - 1] = '\0'; }
    return l;
}
#define strlcpy host_strlcpy
#define ORB_FILES_MAX_NAME   55   /* == orb_files.h's cap */
#define ORBAPP_SLUG_MAX      24u  /* == orb_app_manifest.h */
#define ORBAPP_TITLE_MAX     32u
#define ORBAPP_VERSION_MAX   32u
#define ORB_APP_SCAN_MAX     16
_Static_assert(ORBAPP_SLUG_MAX == ORB_APP_SLUG_MAX, "stub slug cap left the shipped one");
_Static_assert(ORBAPP_TITLE_MAX == ORB_APP_TITLE_MAX, "stub title cap left the shipped one");
typedef struct {
    char      stem[ORB_FILES_MAX_NAME + 1];
    char      slug[ORBAPP_SLUG_MAX];
    char      title[ORBAPP_TITLE_MAX];
    char      version[ORBAPP_VERSION_MAX];
    uint32_t  app_id, caps, period_ms, serial, n_settings, art_size, file_size;
    esp_err_t err;
    char      why[256];
} orbapp_scan_entry_t;
int orb_app_scan(orbapp_scan_entry_t *out, int cap);
esp_err_t orb_app_install(const char *stem, char *why, size_t whysz);
esp_err_t orb_sd_fm_rm(const char *rel, bool recursive, int *removed_out);
"""

def assembled(src):
    """The harness source: the shipped types and functions, spliced verbatim."""
    consts, funcs = lifts(src)
    body = "\n".join(consts[:2] + [ENTRY_DEFS, ENTRY_T] + consts[2:])
    return (HARNESS
            .replace("__CONSTS__", KNOB_EDIT + "\n" + body + "\n" + KNOB_HELPERS)
            .replace("__FUNCS__", "\n".join(funcs))
            .replace("__SLUGDEF__", SLUGDEF)
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
                        "-Werror", "-I", d, "-I", P("main","app"), "-I", P("main","net"), "-I", P("main","menu"), "-DORB_ROTATION_HOST",
                        "-o", exe, cf, P("main", "app", "orb_rotation.c")],
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


BLOCKING = ("orb_theme_scan(", "orb_theme_install_bundle(")
THEME_C = P("main","art","orb_theme.c")
CT_C = P("main","menu","themes_page.c")   # §43: the Themes page is an OS row now
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
    # P5.4: the header the armed branch paints must be the composed one. Painting
    # page_title() here instead is invisible to every pure check — title_line()
    # would still pass its own checks while nothing called it, which is the same
    # unfalsifiable shape the guard above was added for.
    if "title_line(" not in armed:
        bad.append("the armed header is painted from page_title() directly, so the "
                   "position readout exists but nothing draws it")
    return bad


def glyph_lint(src):
    """No dial-facing literal may hold a codepoint the shipped face lacks.

    lv_font_montserrat_14.c:4 records how the face was generated:
    `-r 0x20-0x7F,0xB0,0x2022` plus the FontAwesome symbol codepoints, and
    CONFIG_LV_FONT_MONTSERRAT_14 is the only face built in with no fallback set.
    Anything outside that prints as LVGL's missing-glyph box — which is the
    report this file's row "uninstall [box] press again" came in as on the bench
    (orb-B, 2026-10-24). dial_text() folds the common cases at the paint
    boundary so a door's refusal sentence keeps its wording, but a menu-owned
    row that needs a fold is a row written in the wrong characters: the fold
    costs bytes on a 64 B row buffer every repaint, and '-' is what the
    house style asks for on the glass anyway.

    ESP_LOG* statements are exempt, and deliberately: their text goes to the
    serial console and the HTTP log ring, both UTF-8, where the dash renders.
    That is also why this reads the literals rather than _code_only(), which
    blanks them out entirely (see the note in the mutation battery).
    """
    t = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    t = re.sub(r"//[^\n]*", "", t)
    t = re.sub(r"ESP_LOG[A-Z]+\s*\(.*?;", "", t, flags=re.S)
    bad = []
    for m in re.finditer(r'"((?:[^"\\\n]|\\.)*)"', t):
        lit = m.group(1)
        line = t[:m.start()].count("\n") + 1
        for esc in re.finditer(r"\\(?:u([0-9a-fA-F]{4})|U([0-9a-fA-F]{8}))", lit):
            cp = int(esc.group(1) or esc.group(2), 16)
            if not (0x20 <= cp <= 0x7E or cp in (0x00B0, 0x2022)):
                bad.append("line %d: escape U+%04X has no glyph in the face"
                           % (line, cp))
        for ch in re.sub(r"\\.", "", lit):
            cp = ord(ch)
            if cp > 127 and not (cp in (0x00B0, 0x2022)):
                bad.append("line %d: %r (U+%04X) has no glyph in the face"
                           % (line, ch, cp))
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
    if re.search(r"\borb_theme_install(_bundle)?\s*\(", code):
        bad.append("clock_themes.c calls the install door directly instead of "
                   "posting it — the copy would run on the task that paints the dial")
    if "orb_theme_post_bundle(" not in code:
        bad.append("clock_themes.c does not post the one store entry the page uses either")
    # A widget call inside a job would paint from the worker task.
    if re.search(WIDGET, body):
        bad.append("a card job in clock_themes.c touches a widget")
    # The install job itself, in the layer that owns the flash. Posted, not called.
    th = _code_only(open(THEME_C).read())
    tj = re.findall(r"static void (\w*job\w*)\(void \*arg\)\s*\{([\s\S]*?)\n\}", th)
    if not tj:
        bad.append("orb_theme.c has no job function — nothing runs the install off the caller")
    tjb = "".join(b for _, b in tj)
    if "orb_theme_install_bundle(" not in tjb:
        bad.append("orb_theme.c's job does not call orb_theme_install_bundle() — the store has no worker")
    if "orb_jobs_reboot_soon(" not in tjb:
        bad.append("orb_theme.c's default job never asks for the reboot that draws the factory dial (D13)")
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
     "    if (orb_jobs_busy()) return;\n\n    if (s_note[0]) {", "    if (0) return;\n\n    if (s_note[0]) {"),
    # The bug that made a theme page unreachable-but-not-obvious: the app's scan
    # lands with the menu's own worker idle, so a service() that repaints only on
    # s_dirty opens the page empty. Forcing that back must red the new check.
    # Step 4 added pv_landed to the same guard; each re-check is mutated out of
    # the shipped text separately, and each mutation is written against the
    # three-reason guard as it now ships.
    ("the app's scan landing no longer repaints the shell",
     "    if (!own_result && !app_grew && !pv_landed) return;",
     "    (void)app_grew;\n    if (!own_result && !pv_landed) return;"),
    # Step 4: the plane landing is the third reason. The shipped guard now reads
    # all three, so this mutation is written against the shipped text: forcing
    # pv_landed out must redden the plane-landing checks, exactly as the
    # app_grew mutation reddens the scan-landing check.
    ("the plane landing no longer repaints the shell",
     "    if (!own_result && !app_grew && !pv_landed) return;",
     "    (void)pv_landed;\n    if (!own_result && !app_grew) return;"),
    ("page_rows forgets that unarmed means no list",
     "static int page_rows(void) { return s_armed && s_pg->rows ? s_pg->rows() : 0; }",
     "static int page_rows(void) { return s_pg->rows ? s_pg->rows() : 0; }"),
    # The theme list's own rows left for the clock app (B2), so the two pins that
    # watched them now watch the shell's rendering of ANY app's answer: drop the
    # back row and the owner is trapped; drop the value and the page stops saying
    # which theme is on the dial. host_clock_opts_test.py pins the answers.
    # Same red seen through the shipped rows_app(), not a restatement of it:
    # the mutation drops the page's one back row out of the count.
    ("the back row is dropped from the app options page",
     "    return visible_rows(orb_app_opt_count(s_app)) + own_rows() + 1;",
     "    return visible_rows(orb_app_opt_count(s_app)) + own_rows();"),
    # 2026-11-07: an app's one page. Each pin is one way the merge can quietly
    # leave an app in pieces again.
    ("an app's page loses its caps and doors",
     "    return visible_rows(orb_app_opt_count(s_app)) + own_rows() + 1;",
     "    return visible_rows(orb_app_opt_count(s_app)) + 1;"),
    ("a cap the app reached for without declaring gets no row",
     "    const uint32_t m = orb_svc_declared(a) | orb_svc_denied_mask(a);",
     "    const uint32_t m = orb_svc_declared(a);"),
    ("back from an app's page goes to the top page, not its APPS row",
     "    if (s_pg == P(PG_APP) && s_app && !orb_app_is_menu_only(s_app)) {\n        go_to(PG_APPS, perm_row_of(s_app));",
     "    if (0) {\n        go_to(PG_APPS, perm_row_of(s_app));"),
    ("the refusal mark leaves the APPS row",
     "    if (r->to == PG_APPS && perms_denied())",
     "    if (r->to == PG_UNITS && perms_denied())"),
    ("the refusal mark leaves the app's own list row",
     'snprintf(out, outsz, "%s%s  %s", orb_svc_denied_mask(a) ? "* " : "",',
     'snprintf(out, outsz, "%s%s  %s", 0 ? "* " : "",'),
    ("an install from the card no longer follows the app",
     "            if (a && a->installed) s_pages[PG_APP].open(a);",
     "            (void)a;"),
    ("an uninstall leaves the page on a zeroed app",
     "            !s_app->installed && s_pend_slug[0]) {",
     "            0) {"),
    # P5.3's own pins. Each is one way the catalog or its doors can go wrong
    # while every older page still passes, which is what a new page needs red
    # before it reaches a dial.
    ("a status line draws over the fifth row again",
     "    return (s_ui.status[0] && !about_qr_on()) ? ROW_MAX - 1 : ROW_MAX;",
     "    return ROW_MAX;"),
    ("a note row presses the app instead of showing its sentence",
     "    if (note) { strlcpy(s_note, note, sizeof s_note); repaint(); return; }",
     "    (void)note;"),
    ("the message box lets a turn through to the page under it",
     "        if (pressed || delta) { s_note[0] = '\\0'; repaint(); }\n        return;",
     "        if (pressed || delta) { s_note[0] = '\\0'; repaint(); }\n        if (pressed) return;"),
    ("a hold with the box up leaves the menu",
     "    if (s_note[0]) { s_note[0] = '\\0'; repaint(); return true; }",
     "    if (0) { s_note[0] = '\\0'; repaint(); return true; }"),
    ("a group that lands after entry stays shut behind its inert header",
     "    if (app_grew && s_group_open < 0) {",
     "    if (0) {"),
    ("the Rotate apps row is not a knob edit",
     "    if (row != perm_rows() + card_rows_n() + 1) return false;",
     "    return false;"),
    ("the In rotation row stands with the rotation off",
     "static bool own_rotating(void) { orb_rotation_t r; orb_rotation_get(&r); return r.on; }",
     "static bool own_rotating(void) { return true; }"),
    ("the In rotation press asks to stay where it is",
     "                                 (orb_rotation_takes_part(s_app->slug) ? 0 : 1));",
     "                                 (orb_rotation_takes_part(s_app->slug) ? 1 : 0));"),
    ("choosing off leaves the rotation on",
     "    const char *why = orb_rotation_set(c > 0, c > 0 && c < ROT_N ? ROT_S[c] : r.every_s, NULL);",
     "    const char *why = orb_rotation_set(true, c > 0 && c < ROT_N ? ROT_S[c] : r.every_s, NULL);"),
    ("the catalog stops counting the back row", "static int rows_apps(void)   { return perm_rows() + card_rows_n() + 3; }",
     "static int rows_apps(void)   { return perm_rows() + card_rows_n() + 2; }"),
    ("a failed app loses its reason",
     'snprintf(out, outsz, "%s  failed: %s", a->title ? a->title : a->slug,\n                 orb_svc_fault_reason(a));',
     'snprintf(out, outsz, "%s  failed", a->title ? a->title : a->slug);'),
    ("the entry on the glass stops winning over its enable bit",
     '    return a == orb_app_current() || a->active;',
     '    return false;'),
    ("the B5 count row stops naming the cap",
     'snprintf(out, outsz, "%d of %d slots used", n, ORB_APP_CAP);',
     'snprintf(out, outsz, "%d slots used", n);'),
    ("the doors page drops uninstall",
     "        ids[n++] = ACT_UNINSTALL;\n",
     ""),
    ("an app's page forgets which app its doors are about",
     "    s_door_slot  = app_owned(a) ? orb_app_index_of(a) : -1;",
     "    s_door_slot  = -1;"),
    ("back from the doors page lands on row 0 rather than its own app",
     "    return a ? perm_row_of(a) : 0;",
     "    return 0;"),
    # §58: the app's two sub-menus and the doors that are left.
    ("a running app grows a door that is not a removal",
     "        if (a->failed) ids[n++] = ACT_INSTALL;",
     "        ids[n++] = ACT_INSTALL;"),
    ("the Themes row skips the theme carousel (§61)",
     "        if (theme_strip_open(a)) return;",
     "        (void)theme_strip_open;"),
    ("the Themes row opens the provider for the wrong app",
     "        themes_page_for(a->slug);",
     "        themes_page_for(\"\");"),
    ("back from Themes forgets whose they were",
     "    if (s_pg == P(PG_APP) && s_app == &orb_app_themes_row && s_theme_of) {",
     "    if (0) {"),
    ("back from Permissions goes to the list, not the app's row",
     "    if (s_pg == P(PG_CAPS)) {                  /* the Permissions sub-menu likewise */\n        reopen_app(orb_app_row((size_t)s_perm_slot), OWN_PERMS);",
     "    if (s_pg == P(PG_CAPS)) {                  /* the Permissions sub-menu likewise */\n        go_to(PG_APPS, 0);"),
    ("an app with no themes keeps an empty Themes row",
     "    return themes_page_count_for(a->slug) > 0 || orb_theme_absent(a->slug, NULL, NULL, 0);",
     "    (void)a; return true;"),
    ("the Permissions row counts grants it never asked for",
     "            if ((orb_svc_declared(s_app) & bit) && (orb_svc_grants(s_app) & bit)) ng++;",
     "            if (orb_svc_grants(s_app) & bit) ng++;"),
    ("the APPS walk leaves the themes stale",
     "    themes_page_walk();      /* §58: each app's Themes row is ready when its page opens */",
     ""),
    ("the switch pack drops the slot, so one press writes another app",
     "    return (orb_app_index_of(a) << 2) | (on ? 1 : 0);",
     "    return (on ? 1 : 0);"),
    ("the retry clears a different app's faults",
     "const esp_err_t e = orb_app_clear_faults(a->slug, why, sizeof why);",
     "const esp_err_t e = orb_app_clear_faults(\"menu\", why, sizeof why);"),
    ("the first uninstall press fires it",
     "        if (s_uninst_arm != door_subject()) {",
     "        if (0) {"),
    ("a detent leaves the uninstall armed",
     "    unarm_arms();\n    s_cur = next;",
     "    s_cur = next;"),
    ("leaving the catalog keeps the previous subject and its arms",
     "static void enter_apps(void)\n{\n    s_theme_of   = NULL;\n    s_door_slot  = -1;\n    s_doormode   = 0;\n    s_card_sub   = -1;\n    unarm_arms();\n    job_post(job_scan, 0);\n}",
     "static void enter_apps(void)\n{\n}"),
    ("the refused retry gets the menu's words instead of the door's",
     'e == ESP_OK ? "fault cleared - it runs after the reboot" : why);',
     'e == ESP_OK ? "fault cleared - it runs after the reboot" : "not changed");'),
    ("a removal stops saying the card copy survives",
     'e == ESP_OK ? "uninstalled, the card copy stays" : why);',
     'e == ESP_OK ? "uninstalled" : why);'),
    ("ABOUT stops promising it is read-only",
     ': "rotate: read   push: nothing here"; }',
     ': "rotate: choose   push: change"; }'),
    ("the QR shows with no address to point at",
     "s_cur == ABOUT_ROW_ADDR && orb_net_ip_str()[0];",
     "s_cur == ABOUT_ROW_ADDR;"),
    ("the QR stays up on every ABOUT row",
     "s_cur == ABOUT_ROW_ADDR && orb_net_ip_str()[0];",
     "orb_net_ip_str()[0];"),
    ("the QR shows over an unarmed menu",
     "return s_armed && s_pg == P(PG_ABOUT) && s_cur == ABOUT_ROW_ADDR",
     "return s_pg == P(PG_ABOUT) && s_cur == ABOUT_ROW_ADDR"),
    ("back from ABOUT lands on the wrong top row",
     ".hint = hint_about, .back_to = PG_TOP, .back_cur = 0 },",
     ".hint = hint_about, .back_to = PG_TOP, .back_cur = 3 },"),
    ("ABOUT shows a blank where the card answer belongs",
     'orb_sd_mounted() ? (orb_sd_card_id()[0] ? orb_sd_card_id() : "mounted")\n                                  : "none");',
     'orb_sd_mounted() ? "mounted" : "");'),
    ("the back row of ABOUT disappears",
     "static int rows_about(void)  { return 5 + 1; }",
     "static int rows_about(void)  { return 5; }"),
    ("ABOUT leaves the top of the menu",
     '    { "ABOUT",       PG_ABOUT,       NULL },\n',
     ''),
    ("an app's value stops being rendered",
     'const char *v = orb_app_opt_value(s_app, ar);',
     'const char *v = NULL;'),
    # D12's own pins. Each is one way the shell can get its one level of nesting
    # wrong, and each must turn one of the shape checks above red.
    ("a closed group stops hiding its items",
     "    if (drawn_kind(rows, idx) != OPT_ITEM) return false;\n"
     "    return owner_group(rows, idx) != s_group_open;",
     "    return false;"),
    ("an item with no header above it disappears instead of being drawn",
     "    return (owner_group(rows, idx) < 0) ? OPT_ROW : OPT_ITEM;",
     "    return OPT_ITEM;"),
    # "the cursor escapes an open group" retired 2026-11-01: input walks one row per
    # event, so a step off an item lands on the header or the back row before the
    # clamp in menu_view_apply_input can bind. The walk vectors above pin the
    # behaviour; no input reaches the clamp, so no mutant of it can be caught.
    ("the shell steals the header press back from the app",
     '    if (note) { strlcpy(s_note, note, sizeof s_note); repaint(); return; }\n    orb_app_opt_sel(s_app, ar);',
     '    if (note) { strlcpy(s_note, note, sizeof s_note); repaint(); return; }\n    if (drawn_kind(rows, ar) == OPT_GROUP) { s_group_open = ar; repaint(); return; }\n    orb_app_opt_sel(s_app, ar);'),
    ("items lose the indent that says they are inside a header",
     'snprintf(out, outsz, "%s%s", k == OPT_ITEM ? "  " : "",',
     'snprintf(out, outsz, "%s%s", "",'),
    # The table's own agreements, new to P5.2: where the old switch could drift
    # from the row that used it, these are one initializer each — and each of
    # these mutations says the old switch was right to fear.
    ("back from DISPLAY opens the top page on the wrong row",
     ".back_to = PG_TOP, .back_cur = 1, .edit = edit_display },",
     ".back_to = PG_TOP, .back_cur = 0, .edit = edit_display },"),
    ("back from the app list opens the top page on the wrong row",
     ".act = act_apps, .enter = enter_apps,\n                     .back_to = PG_TOP, .back_cur = 3, .edit = edit_apps },",
     ".act = act_apps, .enter = enter_apps,\n                     .back_to = PG_TOP, .back_cur = 0, .edit = edit_apps },"),
    ("back from the app page loses its row in the dynamic run",
     ".act = act_app, .open = open_app, .ordinal = ord_app,",
     ".act = act_app, .open = open_app,"),
    ("the app page is left with its app armed",
     ".leave = leave_app, .back_to = PG_TOP, .back_cur = 0, .edit = edit_app },",
     ".back_to = PG_TOP, .back_cur = 0, .edit = edit_app },"),
    # Item 3: the menu's "where was I". Each is one way the restore can rot into a
    # lie while every pre-restore check stays green.
    ("item 3: disarming stops storing where the owner was",
     "    if (!armed && s_armed && pos_restorable((int)(s_pg - s_pages)))\n"
     "        pos_save((int)(s_pg - s_pages), s_cur);",
     "    if (0 && s_armed && pos_restorable((int)(s_pg - s_pages)))\n"
     "        pos_save((int)(s_pg - s_pages), s_cur);"),
    ("item 3: a stored row is restored with no clamp onto the live list",
     "        cursor_fit(page_rows());",
     "        ;"),
    ("item 3: the app page becomes restorable onto a pointer that may be gone",
     "                      | (1u << PG_ABOUT) | (1u << PG_UNITS);",
     "                      | (1u << PG_ABOUT) | (1u << PG_UNITS)\n"
     "                      | (1u << PG_APP);"),
    ("item 3: re-arming ignores the stored page and always opens at the top",
     "        s_pg  = P(s_pos_pg);",
     "        s_pg  = P(PG_TOP);"),
    ("the top page's last row stops being exit",
     ".act = act_top, .back_to = MENU_GO_EXIT },",
     ".act = act_top, .back_to = MENU_GO_NONE },"),
    # docs/knob-edit-plan.md: the knob edit mode, one guard each.
    ("the timeout edit opens on a value other than the one in force",
     "    e->v = (int32_t)((s + 30u) / 60u);",
     "    e->v = (int32_t)((s + 30u) / 6u);"),
    ("a press on a knob-edited row runs its press instead",
     "    if (edit_begin(s_cur)) return;      /* a knob-edited row: the press enters the edit */",
     "    (void)edit_begin;"),
    ("the edit's save forgets the value",
     "        s_edit.e.commit(s_edit.row, s_edit.e.v);",
     "        s_edit.e.commit(s_edit.row, 0);"),
    ("a hold during an edit leaves the menu instead of cancelling",
     "    if (s_edit.on) { edit_end(); repaint(); return true; }",
     "    if (0) { edit_end(); repaint(); return true; }"),
    ("an edit outlives the page it was made on",
     "    edit_end();                          /* an edit never outlives its page */",
     "    (void)0;"),
    ("an edit outlives the disarm",
     "    if (!armed) edit_end();",
     "    (void)0;"),
    ("a turn in the edit moves the cursor too",
     "    if (s_edit.on) { edit_input(delta, pressed); return; }",
     "    if (s_edit.on && pressed) { edit_input(delta, pressed); return; }"),
    ("the edit turns the value the wrong way",
     "knob_col_step(delta) * n, step, &s_edit.e.r);",
     "-knob_col_step(delta) * n, step, &s_edit.e.r);"),
    ("the edit ignores the turn's speed",
     "knob_speed_step(&s_edit.sp, n, esp_timer_get_time(), s_edit.e.r.step_max);",
     "1; (void)esp_timer_get_time;"),
    ("the edited row reads the saved value, not the one being set",
     "    if (s_edit.on && s_edit.pg == s_pg && row == s_edit.row) {   /* the value being edited */",
     "    if (0) {"),
    ("an app row the app edits is saved through its press",
     "    orb_app_opt_set(s_app, vis_to_row(orb_app_opt_count(s_app), row), v);",
     "    orb_app_opt_sel(s_app, vis_to_row(orb_app_opt_count(s_app), row));"),
    ("an app's page loses LOCATION, the last owner cap",
     "    for (int c = 0; c < N_OWNER_CAPS; c++)\n        if ((m & (1u << OWNER_CAPS[c])) && i-- == 0) return c;",
     "    for (int c = 0; c < 5; c++)\n        if ((m & (1u << OWNER_CAPS[c])) && i-- == 0) return c;"),
    ("the top page stops counting its dynamic rows",
     "static int rows_top(void)     { return TOP_OS_N + app_rows() + 1; }",
     "static int rows_top(void)     { return TOP_OS_N + 1; }"),
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
    # setting they claim to show, which is exactly the failure nobody notices
    # until the panel dims at the wrong time.
    ("the DISPLAY row drops off the top menu",
     '    { "DISPLAY",     PG_DISPLAY,     NULL },\n', ''),
    ("the DISPLAY page loses a row",
     "static int rows_display(void) { return 3; }",
     "static int rows_display(void) { return 2; }"),
    ("the dim row stops reading the switch it is showing",
     'idle_dim_enabled() ? "on" : "off"', 'true ? "on" : "off"'),
    ("an off-preset timeout reads the nearest preset instead of the truth",
     'snprintf(out, outsz, "%lu s", (unsigned long)cur);',
     'snprintf(out, outsz, "%lu min", (unsigned long)(cur / 60u));'),
    ("D19: the chrome filter flips and lists the shell as an app",
     "if (a && a->kind == ORB_APP_KIND_APP && a->installed) n++;",
     "if (a && a->installed) n++;"),
    # The two facts the ladder exists to keep apart: what the app declared at
    # install, and what the owner has left running. Each pin breaks one of them
    # and only one, so the suite's (n of m) line is what catches it.
    ("D19: a cap the app never declared stops saying so",
     '!dcl ? "not declared" : g ? "allowed" : "revoked"',
     '0 ? "not declared" : g ? "allowed" : "revoked"'),
    ("D19: the refusal mark stops reading the mask it is meant to show",
     "if (a && a->kind == ORB_APP_KIND_APP && orb_svc_denied_mask(a)) return true;",
     "if (a && a->kind == ORB_APP_KIND_APP && false) return true;"),
    ("D19: the refusal mark is detached from the mask it reads",
     "    const bool ref = (orb_svc_denied_mask(a) & bit) != 0;\n    if (sel_hi) *sel_hi = g;",
     "    const bool ref = false;\n    if (sel_hi) *sel_hi = g;"),
    # The press builds the pack through cap_pack(), so the two shifts that
    # decide which app and which cap the worker writes are pinned in the helper
    # the suite lifts and calls directly — and the dispatcher that reaches it
    # is pinned by the press checks above.
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
    # P5.4's position readout. The rule is a comparison against the window, so
    # each of these is one way it can be wrong at the boundary or wrong in the
    # digits, and each lands on a check that pins the exact header text.
    ("P5.4: the readout counts from zero",
     'snprintf(out, outsz, "%s  %d/%d", page_title(), s_cur + 1, rows);',
     'snprintf(out, outsz, "%s  %d/%d", page_title(), s_cur, rows);'),
    ("P5.4: the total drops the row the cursor stands on",
     'snprintf(out, outsz, "%s  %d/%d", page_title(), s_cur + 1, rows);',
     'snprintf(out, outsz, "%s  %d/%d", page_title(), s_cur + 1, rows - 1);'),
    ("P5.4: a page one row short of scrolling starts counting",
     "    if (rows > rows_on_glass())\n        snprintf(out, outsz, \"%s  %d/%d\"",
     "    if (rows >= rows_on_glass())\n        snprintf(out, outsz, \"%s  %d/%d\""),
    ("P5.4: the scrolled page stops naming its total",
     'snprintf(out, outsz, "%s  %d/%d", page_title(), s_cur + 1, rows);',
     'snprintf(out, outsz, "%s  %d", page_title(), s_cur + 1);'),
    ("P5.4: the header loses the page name and keeps only the count",
     'snprintf(out, outsz, "%s  %d/%d", page_title(), s_cur + 1, rows);',
     'snprintf(out, outsz, "%d/%d", s_cur + 1, rows);'),
    ("P5.4: the painted header bypasses the readout",
     "        title_line(head, sizeof head);\n        dial_text(gl, sizeof gl, head);\n        lv_label_set_text(s_title, gl);",
     "        lv_label_set_text(s_title, page_title());"),
    # The glyph floor, added after the bench read a missing-glyph box in the
    # armed uninstall row. Both halves are pinned: the literal, and the fold
    # that saves a borrowed sentence this file cannot rewrite.
    ("P5.4: an unrenderable codepoint goes back into a dial row",
     'armed_u ? " - press again" : "");',
     'armed_u ? " \u2014 press again" : "");'),
    ("P5.4: the fold loses its em-dash mapping",
     '{ 0x2014, "-"  }, { 0x2015, "-"  }, { 0x2212, "-"  },',
     '{ 0x2015, "-"  }, { 0x2212, "-"  },'),
    # A cancel is two halves of one fact. Each mutant keeps the arm correct and
    # breaks only what the owner can see — the shape the bug shipped in.
    ("P5.3: a detent ends the arm and leaves its prompt on the glass",
     "    unarm_arms();\n    s_cur = next;",
     "    s_uninst_arm = -1;\n    s_cur = next;"),
    ("P5.3: a cancel wipes the status line whatever is on it",
     'if (strstr(s_ui.status, "press uninstall again to confirm") ||\n'
     '        strstr(s_ui.status, "press again to erase the card copy"))\n'
     "        s_ui.status[0] = '\\0';",
     "s_ui.status[0] = '\\0';"),
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
                  or (target == MENU_C and bool(glyph_lint(mutated)))
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
    gbad = glyph_lint(menu_src)
    check("every dial-facing literal is a glyph the shipped face carries",
          not gbad, "; ".join(gbad))
    wbad = worker_lint(menu_src)
    check("the worker task still owns both card calls", not wbad, "; ".join(wbad))
    check("rotate still owns the screen swap (router)",
          "if (!screen_armed(s_current))" in _code_only(router_src))
    check("long-press leaves for the app list (router, §51)",
          re.search(r"if \(lpress\) \{\s*open_switcher\(0\);", router_src) is not None)
    # Item 3 (owner ruling, 2026-10-25) armed the shell on a settled flick; §51 retired
    # the flick, and the list it opens is armed by the switcher itself.
    check("the switcher opens the menu armed on its app list (menu, §51)",
          re.search(r"void menu_view_switcher\(const orb_app_t \*from, int step\)\n\{[\s\S]*?"
                    r"s_armed = true;\s*s_pg = P\(PG_GO\);", menu_src) is not None)
    # And the exit door walks back to the entry the shell was entered from, so a
    # menu exit is not a trip to the landing card. A slug is resolved (a card row
    # can uninstall from inside the menu, and a stale pointer lands on a recycled
    # slot), and an empty memory falls back to the home entry (a boot that landed
    # on the menu has nowhere to walk back to). §61: an empty memory walks to the
    # carousel's first app, and Settings' landing only when no app will start.
    check("the menu exit resolves the remembered slug, then the first app, then home (router)",
          re.search(r"void input_router_menu_exit\(void\)[\s\S]*?"
                    r"back = s_before_shell\[0\] \? orb_app_find_slug\(s_before_shell\) : NULL;"
                    r"\s*if \(!back\) back = first_app\(\);[^\n]*"
                    r"\s*set_screen\(back \? back : orb_app_home\(\), true\);",
                    router_src) is not None)
    check("a boot lands on the carousel's first app that starts, not where the glass stood (router, §61)",
          re.search(r"const esp_err_t err0 = boot_first\(home\);", router_src) is not None and
          re.search(r"static esp_err_t boot_first\(const orb_app_t \*home\)[\s\S]*?"
                    r"a->kind == ORB_APP_KIND_SHELL\) continue;[\s\S]*?set_screen\(home", router_src) is not None)
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
    # §43: the only view the registry still names is the menu's; every app's page is
    # the wasm shim's (orb_ui), reached through its one iface.
    check("the registry, not the router, names the views",
          all(n in app_src for n in ("menu_view_screen()", "menu_view_service()",
                                     "orb_wasm_iface"))
          and not any(n in _code_only(router_src)
                      for n in ("menu_view_", "color_picker_", "clock_view_")))
    check("main.c builds the apps through the registry, not by name",
          "orb_app_init_all()" in open(P("main", "main.c")).read()
          and "stage_init()" not in _code_only(open(P("main", "main.c")).read()))
    check("the picker app keeps the two-level gate (apps/color_picker)",
          "if (!s_armed) return;" in open(P("apps", "color_picker", "main.c")).read())

    # ---- P5.2: the table's shape, read as text --------------------------------
    # The harness above proves the table DRIVES the real dispatcher; these prove
    # it is still one table of the shape the dispatcher trusts, and that the
    # switch model it replaced stays dead. They exist because the whole point of
    # the refactor is that a new page is one entry — these fail the day someone
    # starts a second way to say what a page is.
    tcode = _code_only(menu_src)
    check("the pages are exactly one table",
          len(re.findall(r"s_pages\[PG_N\]\s*=\s*\{", tcode)) == 1)
    entry_ids = re.findall(r"(?m)^\s*\[PG_(\w+)\]\s*=", tcode)
    # P5.3 adds APPS (the catalog), APPACT (a card file's doors) and ABOUT;
    # 2026-11-07 retired PERMS and made CAPS one app's Permissions sub-menu. The
    # list is exact rather than "at least": a page that ships without a table row
    # is a page the dispatcher cannot reach.
    check("the table rows name every page id exactly once",
          sorted(entry_ids) == sorted(["LANDING", "TOP", "APP", "DISPLAY", "CAPS",
                                       "APPS", "APPACT",
                                       "ABOUT", "SHARE", "UNITS", "GO"])
          and len(entry_ids) == 11, ",".join(entry_ids))
    # The retired page keeps its number, so a stored menu position keeps its
    # meaning, and is neither a page nor restorable; CAPS needs a selected app,
    # which a stored page id cannot re-derive.
    check("the retired permission list keeps its number and nothing else",
          re.search(r"PG_TO_RETIRED,[^\n]*\n[^\n]*\n[^\n]*\n\s*PG_PERMS,[^\n]*\n\s*PG_CAPS,", tcode)
          is not None
          and "PG_PERMS" not in tcode[tcode.index("s_pages[PG_N] = {"):]
          and "1u << PG_PERMS" not in tcode and "1u << PG_CAPS" not in tcode)
    # docs/knob-edit-plan.md: the timeout presets' id stays in the enum, so a stored
    # menu position keeps its page numbers, and is neither a page nor restorable.
    check("the retired timeout page keeps its number and nothing else",
          re.search(r"PG_DISPLAY,[^\n]*\n\s*PG_TO_RETIRED,", tcode) is not None
          and "PG_TO_RETIRED" not in tcode[tcode.index("s_pages[PG_N] = {"):]
          and "1u << PG_TO_RETIRED" not in tcode)
    _tparts = re.split(r"\[PG_(\w+)\]\s*=\s*\{",
                       tcode[tcode.index("s_pages[PG_N] = {") + len("s_pages[PG_N] = {"):])
    entries = dict(zip(_tparts[1::2], _tparts[2::2]))
    def entry(pg):
        body = entries.get(pg, "")
        return body[:body.find("},")]
    check("every page that can show rows says its count, its labels and its header",
          all(".rows" in entry(p) and ".label" in entry(p) and ".title" in entry(p)
              for p in entry_ids if p != "LANDING")
          and ".rows" not in entry("LANDING"))
    # APPACT joins the two that needed one in P5.2: its parent is APPS, whose
    # rows move when an app is uninstalled, so back has to be computed too.
    check("the ordinal answers only where the parent's list moves",
          all((".ordinal" in entry(p)) == (p in ("APP", "APPACT"))
              for p in entry_ids))
    check("a leaving app page says how to leave it",
          ".leave = leave_app" in entry("APP"))
    check("back lands on the sub-table row the page was opened from",
          ".back_cur = 1" in entry("DISPLAY") and ".back_cur = 2" in entry("UNITS"))
    check("the top page's last row is exit", ".back_to = MENU_GO_EXIT" in entry("TOP"))
    top_rows = re.search(r"menu_row_t s_top\[\] = \{([\s\S]*?)\n\};", menu_src)
    labels = re.findall(r'\{\s*"([^"]+)"', top_rows.group(1)) if top_rows else []
    # The order IS the glass: the owner walks this sequence, and an inserted
    # row silently retargets every back_cur literal in the table. PERMISSIONS
    # keeps row 3, so PG_PERMS's back_cur stayed valid when APPS/ABOUT landed.
    # TZ left (owner ruling, 2026-11-01): the zone follows the home, which the
    # hub sets. AP left the same day (a stub that never worked) and ABOUT took
    # its row: it opens on a QR code for the orb's own page.
    # 2026-11-07: PERMISSIONS went into APPS (one page per app), and UNITS moved
    # above it, so every back_cur below was re-read against this order.
    check("the OS's top rows are ABOUT, DISPLAY, UNITS, APPS",
          labels == ["ABOUT", "DISPLAY", "UNITS", "APPS"],
          ",".join(labels))
    # P5.3's two new sub-pages say their own parent row, and ABOUT promises the
    # inertness its hint claims: no act hook means no press can do anything.
    check("APPS and ABOUT land back on their own top rows",
          ".back_cur = 3" in entry("APPS") and ".back_cur = 0" in entry("ABOUT"))
    check("ABOUT has no press hook at all", ".act" not in entry("ABOUT"))
    check("UNITS lands back on its own top row and edits by knob",
          ".back_cur = 2" in entry("UNITS") and ".edit = edit_units" in entry("UNITS"))
    check("the door page reaches the machine through the two owner doors",
          ".open" not in entry("APPACT") and ".open = open_app" in entry("APP")
          and "orb_app_clear_faults" in tcode and "orb_app_uninstall" in tcode
          and "orb_app_activate" not in tcode)
    # Structural half of the two-press rule; the behavioural half is the check
    # in the harness that presses the uninstall row twice, and once either side
    # of a detent. The arm used to be cleared by four hand-written assignments,
    # and the cursor move's among them cleared ONLY the arm — which is how a
    # cancel left its own confirmation on the glass (bench, orb-B 2026-10-24).
    # One helper now clears the arm and its line together, so a cancel that
    # forgets the second half cannot be written by accident.
    check("both removal arms are owned by the one helper (D38)",
          # the declaration's initializer and the helper's own clear: two each,
          # or a site went back to disarming by hand and left its line behind.
          # D38 grew the uninstall's single arm into a pair — uninstall and
          # remove-from-card — and the helper takes both and both prompts.
          tcode.count("s_uninst_arm = -1") == 2
          and tcode.count("s_cardrm_arm = -1") == 2
          # three cancels call the helper: leaving the catalog, the firing press,
          # and the cursor move. Miss the one in apply_input and a detent leaves
          # an arm live; miss its line and the glass still says press again.
          # four: leaving the catalog, the two firing presses (D38's pair), and
          # the cursor move. Miss the one in apply_input and a detent leaves an
          # arm live; miss its line and the glass still says press again. The
          # fifth (2026-11-07) is leaving an app's own page, which carries the
          # doors now.
          and tcode.count("unarm_arms();") == 5
          and tcode.count("s_uninst_arm != door_subject()") == 1
          and tcode.count("s_uninst_arm == door_subject()") == 1
          and tcode.count("s_cardrm_arm != door_subject()") == 1
          and tcode.count("s_cardrm_arm == door_subject()") == 1)
    check("the switch model stays dead",
          "top_item_t" not in menu_src and re.search(r"\bs_page\b", menu_src) is None
          and not re.search(r"\bcase PG_", tcode))


    print("all host checks pass" if not FAILS else "%d FAILED" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

