#!/usr/bin/env python3
"""host_app_registry_test.py — the app registry, tested on the host.

Why this file exists
--------------------
main/app/orb_app.c decides what boots, in what order, what the knob's carousel
walks, what the long-press returns to, and which app is holding MMU pages for
its art. None of that is visible on a serial console in the way it needs to be:
a wrong carousel order is a dial that steps to the wrong screen, a missing
suspend is an app that keeps burning flush work while it is off the glass, and a
bank that is never released is the next app getting ESP_ERR_NO_MEM at boot.

So the shipped functions are lifted VERBATIM out of main/app/orb_app.c and
compiled here against the REAL orb_app.h, art_bank.h, orb_stage.h and
menu_view.h, with only the leaves stubbed — not re-typed in
Python. An empty lift dies loudly, because a suite that silently tests a stale
copy is worse than no suite. Compiling against the shipped headers is the point:
if the registry's calls ever drift from a leaf API's real signature, this suite
fails to build rather than passing against a fiction.

What is under test
------------------
  orb_app_find_slug()   exact slug, the retired "hue" alias gone, and
                        unknown/empty/NULL/case-wrong refused
  orb_app_init_all()    the shell first, then each wasm row in slot order, each
                        through its own init (the shared wasm iface is exempt from
                        C6's singleton rule, §43); an init that fails marks that
                        row failed without taking the dial down with it
  orb_app_neighbour()   the shell, then the card rows in slot order, plus both
                        wraparounds; a failed row is skipped
  orb_app_activate()    the call ORDER the two-level model depends on: the outgoing
                        row suspends before lv_screen_load(), the incoming row's
                        activate runs before it and its model re-push after it
  art residency         a banked app maps on activate and unmaps on suspend; a
                        refused map keeps the dial on the app it was on and puts
                        the outgoing app's visibility back; a double activate
                        does not bill the same bank twice; and the NULL-reclaim
                        branch (suspend released the bank, the rollback's re-claim
                        also refused — the release-on-suspend ruling) is pinned by
                        asserting the on-glass app is left with art_slot == -1
  C7                    service() runs for a parked app, and reaping never loads a
                        screen

Run:  python3 tools/host_app_registry_test.py
"""

import os, re, subprocess, sys, tempfile

ROOT = (sys.argv[sys.argv.index("--root") + 1] if "--root" in sys.argv
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
P = lambda *a: os.path.join(ROOT, *a)

APP_C  = P("main", "app", "orb_app.c")
APP_H  = P("main", "app", "orb_app.h")
for _p in (APP_C, APP_H):
    if not os.path.exists(_p):
        sys.exit("missing " + _p + " — the lifts below have no fallback")

app_src = open(APP_C).read()


FAILS = []
def check(name, ok, detail=""):
    print(("ok    " if ok else "FAIL  ") + name + (("  " + detail) if detail else ""))
    if not ok:
        FAILS.append(name)


def lift(text, pattern, where):
    m = re.search(pattern, text, re.S)
    if not m:
        sys.exit("could not lift %r from %s — did the code move? Dying here is the "
                 "point: an empty lift would keep this suite green while testing a "
                 "copy that no longer ships." % (pattern, where))
    return m.group(0)


# Everything the registry implements, lifted whole, with the includes removed:
# the harness supplies the real headers itself, so that a signature drift is a
# compile error here rather than a mystery on the dial.
BODY = "\n".join(l for l in app_src.split("\n")
                 if not re.match(r"\s*#include\b", l))
for _fn in ("orb_app_init_all", "orb_app_find_slug", "orb_app_activate",
            "orb_app_neighbour", "orb_app_service_all", "orb_app_home",
            "orb_app_current_armed", "orb_app_disarm_all", "orb_app_arm_current"):
    if re.search(r"\b%s\s*\(" % _fn, BODY) is None:
        sys.exit("lifted body has no %s() — the lift is wrong" % _fn)

# ---------------------------------------------------------------------------
# The leaf stubs. Each records the fact the checks below read: the calls, the
# order they happened in, and the return code the registry has to react to.
# ---------------------------------------------------------------------------
STUBS = r'''
/* ---- recording leaf stubs (the real headers declare these) -------------- */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "orb_app.h"
#include "orb_stage.h"
#include "menu_view.h"
#include "art_bank.h"

char  g_trace[2048];
int   g_tn;
static void T(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    if (g_tn < (int)sizeof(g_trace) - 64)
        g_tn += vsnprintf(g_trace + g_tn, sizeof(g_trace) - g_tn, fmt, ap);
    va_end(ap);
    g_trace[g_tn++] = '|'; g_trace[g_tn] = '\0';
}
static void TR(void) { g_trace[0] = '\0'; g_tn = 0; }

/* Distinct opaque tokens, so "which screen got loaded" is observable. */
static int tok_clock, tok_picker, tok_menu, tok_other;
static int g_clock_armed, g_picker_armed, g_menu_armed;
/* Rows the clock row's opt_count() reports; 0 is a page whose rows have not landed. */
static int g_clock_opts = 2;
static int g_map_calls, g_unmap_calls, g_next_slot = 3, g_service_calls;
static esp_err_t g_map_rc = ESP_OK, g_unmap_rc = ESP_OK;
static size_t g_refuse_bytes;        /* refuse exactly this bank size, so the
                                      * outgoing app's smaller bank can still
                                      * come back after a refusal */
static esp_err_t g_rc_clock, g_rc_picker, g_rc_menu;

const char *esp_err_to_name(esp_err_t c)
{
    switch (c) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        default: return "ERR";
    }
}

/* The card table is one heap_caps_calloc block on the device; here calloc stands
 * in (no PSRAM on a host, block is a few KB). It backs the same card_table() the
 * shipped code runs, so the ledger paths are exercised for real. */
void *heap_caps_calloc(size_t n, size_t sz, uint32_t caps)
{
    (void)caps;
    return calloc(n, sz);
}

/* §43: every app is a wasm row, and every wasm row shares the one iface that
 * orb_app_wasm.c defines. The harness supplies that symbol with hooks that answer
 * per row, by slug, and record which hook ran for which row. What is under test is
 * the registry's dispatch — which row, which hook, in what order; the hooks' own
 * behaviour is host_orbapp_wasm_test.py's and the device's. The two seeded rows
 * trace under their short names, clock and picker. */
static const char *nm(const orb_app_t *a)
{
    if (!a || !a->slug) return "?";
    if (!strcmp(a->slug, "clock")) return "clock";
    if (!strcmp(a->slug, "color_picker")) return "picker";
    return a->slug;
}
static bool is_(const orb_app_t *a, const char *n) { return !strcmp(nm(a), n); }
static esp_err_t w_init(orb_app_t *a)
{
    T("%s_init", nm(a));
    return is_(a, "clock") ? g_rc_clock : is_(a, "picker") ? g_rc_picker : ESP_OK;
}
static esp_err_t w_activate(orb_app_t *a) { T("%s_act", nm(a)); return ESP_OK; }
static lv_obj_t *w_screen(orb_app_t *a)
{
    return is_(a, "clock")  ? (lv_obj_t *)&tok_clock :
           is_(a, "picker") ? (lv_obj_t *)&tok_picker : (lv_obj_t *)&tok_other;
}
static void w_suspend(orb_app_t *a) { T("%s_susp", nm(a)); }
static void w_resume(orb_app_t *a)  { T("%s_resume", nm(a)); }
static void w_show(orb_app_t *a)    { T("%s_show", nm(a)); }
static void w_input(orb_app_t *a, int32_t d, bool p)
{ T("%s_input(%d,%d)", nm(a), (int)d, p ? 1 : 0); }
static bool w_armed(const orb_app_t *a)
{ return is_(a, "clock") ? g_clock_armed : is_(a, "picker") ? g_picker_armed : false; }
static void w_set_armed(orb_app_t *a, bool on)
{
    if (is_(a, "clock")) g_clock_armed = on;
    else if (is_(a, "picker")) g_picker_armed = on;
    T("%s_arm(%d)", nm(a), on ? 1 : 0);
}
static void w_service(orb_app_t *a) { T("%s_service", nm(a)); }
/* The clock row answers rows; every other row answers none, which is what the real
 * shim answers for a module that exports no orb_opt_*. */
static int w_opt_count(orb_app_t *a) { return is_(a, "clock") ? g_clock_opts : 0; }
static orb_opt_kind_t w_opt_kind(orb_app_t *a, int i)
{ T("%s_kind(%d)", nm(a), i); return i == 1 ? OPT_GROUP : i == 2 ? OPT_ITEM : OPT_ROW; }
static const char *w_opt_label(orb_app_t *a, int i) { (void)a; return i == 0 ? "diver" : "neon"; }
static const char *w_opt_value(orb_app_t *a, int i) { (void)a; return i == 0 ? "on the dial" : ""; }
static void w_opt_inc(orb_app_t *a, int i, int32_t d) { T("%s_inc(%d,%d)", nm(a), i, (int)d); }
static void w_opt_sel(orb_app_t *a, int i) { T("%s_sel(%d)", nm(a), i); }
static void w_opt_arm(orb_app_t *a, bool on) { T("%s_optarm(%d)", nm(a), on ? 1 : 0); }
const orb_app_iface_t orb_wasm_iface = {
    .init = w_init, .activate = w_activate, .screen = w_screen, .suspend = w_suspend,
    .resume = w_resume, .show = w_show, .input = w_input, .armed = w_armed,
    .set_armed = w_set_armed, .service = w_service,
    .opt_count = w_opt_count, .opt_kind = w_opt_kind, .opt_label = w_opt_label,
    .opt_value = w_opt_value, .opt_inc = w_opt_inc, .opt_sel = w_opt_sel,
    .opt_arm = w_opt_arm,
};

static bool g_menu_long_taken;
bool menu_view_opt_long(void) { T("menu_long"); return g_menu_long_taken; }
void menu_view_switcher(const orb_app_t *from, int step) { (void)from; (void)step; T("menu_switcher"); }
/* orb_app_share_gate reaches the share window's claim through this; one recorder,
 * answering false, the same shape menu_long carries. */
static bool g_share_gate_claim;
bool menu_view_share_gate(bool press) { (void)press; T("share_gate"); return g_share_gate_claim; }
esp_err_t menu_view_init(void)   { T("menu_init");   return g_rc_menu; }
lv_obj_t *menu_view_screen(void)        { return (lv_obj_t *)&tok_menu; }
void menu_view_show(void)               { T("menu_show"); }
void menu_view_apply_input(int32_t d, bool p) { T("menu_input(%d,%d)", (int)d, p ? 1 : 0); }
bool menu_view_armed(void)              { return g_menu_armed != 0; }
void menu_view_set_armed(bool a)        { g_menu_armed = a ? 1 : 0; T(a ? "menu_arm(1)" : "menu_arm(0)"); }
void menu_view_service(void)            { g_service_calls++; T("menu_service"); }

/* The broker's revoke door — §4.2's auto-revoke, called by the registry when
 * an entry leaves the glass. Recorded, and asserted below: a swap that stops
 * calling it hands every parked app a free hold on the dial. */
static int g_svc_revoke_calls;
void orb_svc_app_suspended(const orb_app_t *a)
{
    g_svc_revoke_calls++;
    T("svc_revoke:%s", a && a->slug ? a->slug : "?");
}
/* The C2 doors the registry consults at build time: one knob, the slug the
 * broker has marked failed. */
static const char *g_fault_slug;
bool orb_svc_faulted(const orb_app_t *a)
{
    return g_fault_slug && a && a->slug && !strcmp(a->slug, g_fault_slug);
}
uint32_t orb_svc_fault_count(const orb_app_t *a) { (void)a; return g_fault_slug ? 2u : 0u; }
const char *orb_svc_fault_reason(const orb_app_t *a)
{
    (void)a;
    return "unexpected reset";
}
void orb_svc_current_note(const orb_app_t *a)
{
    T("cur_note:%s", a && a->slug ? a->slug : "?");
}

esp_err_t art_bank_map_extra(size_t off, size_t need, int *out_slot)
{
    g_map_calls++;
    T("map(%lu,%lu)", (unsigned long)off, (unsigned long)need);
    if (g_refuse_bytes && need == g_refuse_bytes) return ESP_ERR_NO_MEM;
    if (g_map_rc != ESP_OK) return g_map_rc;
    if (out_slot) *out_slot = g_next_slot++;
    return ESP_OK;
}
esp_err_t art_bank_unmap_extra(int slot)
{
    g_unmap_calls++;
    T("unmap(%d)", slot);
    return g_unmap_rc;
}
const uint8_t *art_bank_extra_ptr(int slot) { (void)slot; return NULL; }

static struct _lv_obj_t *g_last_loaded;
void lv_screen_load(lv_obj_t *scr) { g_last_loaded = (struct _lv_obj_t *)scr; T("load"); }

/* ---- the registry, lifted verbatim from main/app/orb_app.c -------------- */
__BODY__

/* The row orb_app.c's service array points at, which the firmware build finds in
 * main/usb/orb_usb_mtp.c when CONFIG_ORB_USB_MTP is on. Its title and opt_* hooks
 * are that module's business — what is under test here is the registry's
 * treatment of the tier: whether a row outside the ledger can reach the ledger's
 * words, the walk, or a face. */
static const orb_app_iface_t s_usb_stub_iface = {0};
orb_app_t orb_app_usb_files_row = {
    .slug = (char *)"usb-files", .title = (char *)"USB Files",
    .kind = ORB_APP_KIND_APP, .behaviour_kind = ORB_BEHAVIOUR_NONE,
    .iface = &s_usb_stub_iface, .installed = true,
    .art_slot = -1, .flash_slot = -1,
};
/* D39: the OS's Sound page, main/audio/orb_sound_menu.c in the firmware. */
orb_app_t orb_app_sound_row = {
    .slug = (char *)"sound", .title = (char *)"Sound",
    .kind = ORB_APP_KIND_APP, .behaviour_kind = ORB_BEHAVIOUR_NONE,
    .iface = &s_usb_stub_iface, .installed = true,
    .art_slot = -1, .flash_slot = -1,
};
/* §43: the OS's Themes page, main/menu/themes_page.c — the tier's first row, and the
 * one iface in the tree that names the cursor-land and plane hooks, so the dispatch
 * checks for those two doors run against this stand-in. */
static uint8_t g_th_plane[4];
static const void *g_th_pv_px;
static int th_count(orb_app_t *a) { (void)a; return 2; }
static const char *th_label(orb_app_t *a, int i) { (void)a; return i ? "neon" : "diver"; }
static void th_arm(orb_app_t *a, bool on) { (void)a; T("th_arm(%d)", on ? 1 : 0); }
static void th_focus(orb_app_t *a, int i) { (void)a; T("th_focus(%d)", i); }
static const void *th_preview(orb_app_t *a, int i, uint16_t *w, uint16_t *h, uint32_t *stamp)
{
    (void)a; (void)i;
    if (!g_th_pv_px) return NULL;
    *w = 232; *h = 232; *stamp = 1;
    return g_th_plane;
}
static const orb_app_iface_t s_themes_stub_iface = {
    .opt_count = th_count, .opt_label = th_label, .opt_value = th_label,
    .opt_arm = th_arm, .opt_focus = th_focus, .opt_preview = th_preview,
};
orb_app_t orb_app_themes_row = {
    .slug = (char *)"themes", .title = (char *)"Themes",
    .kind = ORB_APP_KIND_APP, .behaviour_kind = ORB_BEHAVIOUR_NONE,
    .iface = &s_themes_stub_iface, .installed = true,
    .art_slot = -1, .flash_slot = -1,
};

/* ---- the cases ---------------------------------------------------------- */
static int g_fail;
static void ck(const char *name, int ok)
{
    printf("%s %s%s\n", ok ? "PASS" : "FAIL", name, "");
    if (!ok) g_fail++;
}
static const orb_app_t *A(const char *slug) { return orb_app_find_slug(slug); }
static void seed_factory(void);
static void restore_table(void)
{
    for (size_t i = 0; i < ORB_APP_N; i++) {
        s_apps[i].failed = false;
        s_apps[i].installed = true; s_apps[i].art_slot = -1;
        s_apps[i].art_bytes = 0; s_apps[i].art_offset = 0;
    }
    s_apps[0].is_home = true;          /* D35: the one compiled row is the shell */
    s_current = NULL; g_clock_armed = g_picker_armed = g_menu_armed = 0; g_clock_opts = 2;
    g_map_calls = g_unmap_calls = g_service_calls = 0;
    g_map_rc = g_unmap_rc = ESP_OK; g_next_slot = 3; g_refuse_bytes = 0;
    g_rc_clock = g_rc_picker = g_rc_menu = ESP_OK;
    orb_app_card_reset();
    seed_factory();
}

/* §43: the starter rows, upserted the way the installer does — two wasm rows at
 * flash slots 0 and 1 — without a partition, a CRC or an NVS word. */
static void seed_factory(void)
{
    orb_app_t r;
    memset(&r, 0, sizeof r);
    r.slug = (char *)"clock"; r.title = (char *)"Clock";
    r.kind = ORB_APP_KIND_APP; r.behaviour_kind = ORB_BEHAVIOUR_WASM;
    r.flash_slot = 0; r.art_slot = -1;
    orb_app_card_upsert(&r);
    memset(&r, 0, sizeof r);
    r.slug = (char *)"color_picker"; r.title = (char *)"HSB";
    r.kind = ORB_APP_KIND_APP; r.behaviour_kind = ORB_BEHAVIOUR_WASM;
    r.flash_slot = 1; r.art_slot = -1;
    orb_app_card_upsert(&r);
}

#define CLK ((orb_app_t *)orb_app_find_slug("clock"))
#define PCK ((orb_app_t *)orb_app_find_slug("color_picker"))
#define AT(s) strstr(g_trace, s)

int main(void)
{
    /* -- boot: the compiled shell builds first; the card tier follows in slot
     *    order. Both rows name the one wasm iface and both run their own init:
     *    the C6 singleton rule exempts it (one iface, N instances, §43). -- */
    restore_table(); TR();
    orb_app_init_all();
    ck("boot inits the menu, then each wasm row in slot order, each its own init",
       strncmp(g_trace, "menu_init|clock_init|picker_init|", 33) == 0);

    /* -- a failing init drops that entry, not the dial -- */
    restore_table(); TR(); g_rc_picker = ESP_FAIL;
    orb_app_init_all();
    ck("a row whose init fails is marked failed and the dial stays up",
       orb_app_count() == 2 && A("color_picker")->failed);
    ck("the failed app drops out of the carousel rather than showing a blank",
       orb_app_neighbour(A("clock"), 1) == A("menu")
       && orb_app_neighbour(A("menu"), -1) == A("clock"));

    /* -- slug resolution -- */
    restore_table();
    ck("the three entries resolve by slug", A("clock") && A("color_picker") && A("menu"));
    ck("the retired hue alias resolves nowhere in the registry", A("hue") == NULL);
    ck("an unknown slug resolves to nothing", A("nope") == NULL);
    ck("an empty slug resolves to nothing", A("") == NULL);
    ck("a NULL slug resolves to nothing", A(NULL) == NULL);
    ck("slug matching is case-sensitive", A("Clock") == NULL);
    ck("the shell is the home entry (D35 amended C8)", strcmp(orb_app_home()->slug, "menu") == 0);

    /* -- the carousel: shell, dial, editor -- */
    restore_table();
    ck("clock -> color_picker is one step (card-tier order)",
       orb_app_neighbour(A("clock"), 1) == A("color_picker"));
    ck("color_picker -> menu is one step",
       orb_app_neighbour(A("color_picker"), 1) == A("menu"));
    ck("menu wraps forward to the clock", orb_app_neighbour(A("menu"), 1) == A("clock"));
    ck("the clock wraps back to the menu", orb_app_neighbour(A("clock"), -1) == A("menu"));
    ck("NULL start means the first walkable entry, the shell",
       orb_app_neighbour(NULL, 1) == A("menu"));
    ck("long press homes to the shell (C8 as amended)", orb_app_home() == A("menu"));
    restore_table(); PCK->failed = true;
    ck("a failed entry is skipped in both directions",
       orb_app_neighbour(A("clock"), 1) == A("menu")
       && orb_app_neighbour(A("menu"), -1) == A("clock"));
    restore_table();
    s_apps[0].failed = true; CLK->failed = true; PCK->failed = true;
    ck("a table with nothing walkable answers NULL rather than spinning",
       orb_app_neighbour(A("clock"), 1) == NULL);

    /* -- the switch ORDER the two-level model depends on -- */
    restore_table(); orb_app_init_all(); TR();
    orb_app_activate(A("clock"));
    ck("activating a row runs its activate, loads its screen, then re-pushes it",
       strcmp(g_trace, "clock_act|load|clock_show|cur_note:clock|") == 0);

    restore_table(); orb_app_init_all(); orb_app_activate(A("clock")); TR();
    orb_app_activate(A("color_picker"));
    /* The outgoing row must be parked BEFORE the new screen loads: a beat left
     * running across the swap burns flush work on rows nobody can see, which is
     * the cadence collapse the fps findings measured. */
    ck("leaving a row suspends it before the new screen loads",
       AT("clock_susp") && AT("load") && AT("clock_susp") < AT("load"));
    ck("and the suspended row loses its hold on the dial right after its suspend",
       strncmp(g_trace, "clock_susp|svc_revoke:clock|", 28) == 0);
    ck("the picker re-pushes its model after the load, not before",
       AT("load") && AT("picker_show") && AT("load") < AT("picker_show"));
    ck("the picker is the screen that got loaded",
       g_last_loaded == (struct _lv_obj_t *)&tok_picker);
    ck("returning to the dial parks the picker and loads the dial's screen",
       (TR(), orb_app_activate(A("clock")), AT("picker_susp") && AT("clock_act"))
       && g_last_loaded == (struct _lv_obj_t *)&tok_clock);

    /* -- input goes to the visible entry only -- */
    restore_table(); orb_app_init_all(); orb_app_activate(A("color_picker")); TR();
    orb_app_input(3, false);
    ck("a turn reaches the visible row's input hook and no other",
       strcmp(g_trace, "picker_input(3,0)|") == 0);

    /* -- arming -- */
    restore_table(); orb_app_init_all(); orb_app_activate(A("clock"));
    ck("a press arms the visible row through its own hook",
       orb_app_arm_current() == true && orb_app_current_armed() == true && g_clock_armed);
    orb_app_activate(A("menu"));
    ck("the menu arms and reports it", orb_app_arm_current() == true && orb_app_current_armed() == true);
    TR(); orb_app_disarm_all();
    ck("disarm reaches every entry, not only the one leaving",
       AT("menu_arm(0)") && AT("clock_arm(0)") && AT("picker_arm(0)")
       && g_menu_armed == 0 && g_clock_armed == 0 && g_picker_armed == 0);

    /* -- C7: a parked app still reaps its own jobs -- */
    restore_table(); orb_app_init_all(); orb_app_activate(A("menu"));
    orb_app_activate(A("clock")); TR(); g_service_calls = 0;
    orb_app_service_all();
    ck("a parked app's worker jobs are still reaped (C7)",
       g_service_calls == 1 && AT("menu_service") && AT("picker_service"));
    ck("reaping does not load a screen: the dial stays on the glass",
       orb_app_current() == A("clock") && !AT("load"));

    /* -- art residency: claim on activate, release on suspend -- */
    restore_table(); orb_app_init_all();
    PCK->art_offset = 0x400000; PCK->art_bytes = 0x80000;
    TR(); g_map_calls = 0;
    orb_app_activate(A("color_picker"));
    ck("a banked app maps its secondary bank on activate",
       g_map_calls == 1 && PCK->art_slot >= 0 && AT("map(4194304,524288)"));
    ck("the slot the registry booked is the slot handed to the compositor",
       PCK->art_slot >= 3 && art_bank_extra_ptr(PCK->art_slot) == NULL);
    TR(); g_unmap_calls = 0; g_svc_revoke_calls = 0;
    orb_app_activate(A("clock"));
    ck("suspending releases the bank, so a parked app costs no MMU pages",
       g_unmap_calls == 1 && PCK->art_slot == -1);
    ck("and the same swap revokes the parked app's hold on the dial",
       g_svc_revoke_calls == 1 && AT("svc_revoke:color_picker"));

    /* -- the overflow path: a refused claim must not blank the dial -- */
    restore_table(); orb_app_init_all();
    CLK->art_offset = 0x100000; CLK->art_bytes = 0x40000;
    PCK->art_offset = 0x400000; PCK->art_bytes = 0x80000;
    orb_app_activate(A("clock"));                     /* the dial maps its bank */
    g_refuse_bytes = 0x80000;                          /* the picker's bank will not fit */
    TR(); g_map_calls = 0; g_unmap_calls = 0;
    const esp_err_t refused = orb_app_activate(A("color_picker"));
    ck("a bank the page budget refuses is a refused switch", refused == ESP_ERR_NO_MEM);
    ck("the refused app never becomes the visible one",
       orb_app_current() == A("clock") && PCK->art_slot == -1);
    ck("the outgoing bank was released and then claimed back",
       g_map_calls == 2 && g_unmap_calls == 1 && CLK->art_slot >= 0);
    ck("the parked dial is resumed after the refused switch", AT("clock_resume") != NULL);
    g_refuse_bytes = 0;
    ck("the same switch succeeds once the budget allows it",
       orb_app_activate(A("color_picker")) == ESP_OK && orb_app_current() == A("color_picker"));

    /* The NULL-reclaim branch (orb_app.c app_restore): the budget is so tight
     * that even putting the outgoing bank back fails. The switch is still
     * refused, s_current is never reassigned, and the app left on the glass
     * ends with art_slot == -1 — the release-on-suspend ruling makes it
     * reachable, and the invariant is pinned, not just the return code. */
    restore_table(); orb_app_init_all();
    CLK->art_offset = 0x100000; CLK->art_bytes = 0x40000;
    PCK->art_offset = 0x400000; PCK->art_bytes = 0x80000;
    orb_app_activate(A("clock"));
    ck("the on-glass app holds its bank before the doomed switch",
       CLK->art_slot >= 0 && g_map_calls == 1);
    TR(); g_map_calls = 0; g_unmap_calls = 0;
    g_map_rc = ESP_ERR_NO_MEM;                          /* nothing fits at all */
    const orb_app_t *cur_before = orb_app_current();
    ck("a refusal the rollback cannot undo still keeps the dial on the old app",
       orb_app_activate(A("color_picker")) == ESP_ERR_NO_MEM
       && orb_app_current() == A("clock") && PCK->art_slot == -1);
    ck("the switch never repointed current — the failed rollback returned before the commit",
       orb_app_current() == cur_before);
    ck("NULL-reclaim: the app left ON the glass has NO bank — its suspend released it and the rollback could not re-claim",
       CLK->art_slot == -1);
    ck("NULL-reclaim touched the window once released, twice refused to map",
       g_unmap_calls == 1 && g_map_calls == 2);
    g_map_rc = ESP_OK;

    /* -- no double-billing: re-activating a resident app maps it once -- */
    restore_table(); orb_app_init_all();
    PCK->art_offset = 0x400000; PCK->art_bytes = 0x80000;
    g_map_calls = 0;
    orb_app_activate(A("color_picker")); orb_app_activate(A("menu"));
    orb_app_activate(A("color_picker"));
    ck("a bank is never mapped twice while it is still resident", g_map_calls == 2);

    /* -- the options surface (B1/B2): the registry is the only door -- */
    restore_table();
    const orb_app_t *clk = A("clock"), *pick = A("color_picker");
    const orb_app_t *th = &orb_app_themes_row;
    /* Every wasm row shares one iface, so every one declares the hooks; the rows
     * a page answers are the module's own (the shim answers 0 for a module with
     * no orb_opt_* export). */
    ck("every wasm row offers its page; the count is the app's own",
       orb_app_has_options(clk) && orb_app_has_options(pick)
       && orb_app_opt_count(clk) == 2 && orb_app_opt_count(pick) == 0);
    /* The reachability bug (B2): offering the page is what the entry DECLARES,
     * not what it currently answers, or a page whose rows land after the first
     * press is invisible until it has been shown. */
    g_clock_opts = 0;
    ck("an app that declares hooks offers its row before its first rows land",
       orb_app_has_options(clk) && orb_app_opt_count(clk) == 0);
    g_clock_opts = 2;
    ck("label and value answer in the app's words",
       strcmp(orb_app_opt_label(clk, 0), "diver") == 0 &&
       strcmp(orb_app_opt_value(clk, 0), "on the dial") == 0);
    ck("a row outside the count is refused before the app sees it",
       orb_app_opt_label(clk, 9)[0] == '\0' && orb_app_opt_value(clk, -1)[0] == '\0');
    TR(); orb_app_opt_sel(clk, 0);
    ck("the press dispatches once, with the row index", strcmp(g_trace, "clock_sel(0)|") == 0);
    TR(); orb_app_opt_inc(clk, 1, 3);
    ck("a rotate dispatches with the row and the detents", strcmp(g_trace, "clock_inc(1,3)|") == 0);
    TR(); orb_app_opt_arm(clk, true);
    ck("arming the options page reaches the app", strcmp(g_trace, "clock_optarm(1)|") == 0);
    TR(); orb_app_opt_arm(clk, false);
    ck("leaving it disarms, so nothing is armed off-glass", strcmp(g_trace, "clock_optarm(0)|") == 0);
    g_clock_opts = 3;
    const int k_group = orb_app_opt_kind(clk, 1);
    const int k_item  = orb_app_opt_kind(clk, 2);
    const int k_row   = orb_app_opt_kind(clk, 0);
    g_clock_opts = 2;
    ck("the row kind reaches the shell as the app answered it",
       k_group == OPT_GROUP && k_item == OPT_ITEM && k_row == OPT_ROW);
    ck("a row outside the count is OPT_ROW without asking the app",
       (TR(), orb_app_opt_kind(clk, 9) == OPT_ROW && orb_app_opt_kind(clk, -1) == OPT_ROW
        && orb_app_opt_kind(pick, 0) == OPT_ROW && g_trace[0] == '\0'));
    const orb_app_t *menu = A("menu");
    ck("a wasm row declares no long-press hook, so it cannot swallow the escape hatch",
       orb_app_opt_long(clk) == false);
    ck("no entry at all takes nothing", orb_app_opt_long(NULL) == false);
    /* The cursor-land and plane doors: they dispatch for the page that names the
     * hooks and bounds-gate like every other answer; NULL three ways (no hook,
     * no entry, out of range) is the text-only page, extents zeroed. */
    TR(); orb_app_opt_focus(th, 1);
    ck("a cursor land reaches the page that names the hook", strcmp(g_trace, "th_focus(1)|") == 0);
    TR(); orb_app_opt_focus(th, 9); orb_app_opt_focus(th, -1);
    ck("a focus outside the count never reaches the page", g_trace[0] == '\0');
    TR(); orb_app_opt_focus(clk, 0); orb_app_opt_focus(NULL, 0);
    ck("a NULL focus hook and a NULL entry are skipped, not called", g_trace[0] == '\0');
    uint16_t pw = 42, ph = 42; uint32_t pst = 42;
    ck("a row with no preview hook answers the text-only NULL",
       orb_app_opt_preview(clk, 0, &pw, &ph, &pst) == NULL && pw == 0 && ph == 0 && pst == 0);
    ck("no entry answers NULL too",
       orb_app_opt_preview(NULL, 0, &pw, &ph, &pst) == NULL && pst == 0);
    g_th_pv_px = g_th_plane;
    ck("and so does a row outside the count, without asking the page",
       orb_app_opt_preview(th, 9, &pw, &ph, &pst) == NULL && pw == 0 && pst == 0);
    pw = ph = 0; pst = 0;
    const void *pv = orb_app_opt_preview(th, 1, &pw, &ph, &pst);
    ck("with the hook armed, the plane and its extents answer",
       pv == g_th_plane && pw == 232 && ph == 232 && pst == 1);
    g_th_pv_px = NULL;
    if (menu) {
        g_menu_long_taken = true;
        ck("the shell entry can take it, and the router must then not home",
           orb_app_opt_long(menu) == true);
        g_menu_long_taken = false;
        ck("and refuses it when no group is open", orb_app_opt_long(menu) == false);
    }
    TR(); orb_app_opt_sel(&orb_app_usb_files_row, 0); orb_app_opt_arm(&orb_app_usb_files_row, true);
    ck("a row with no option hooks is skipped, not called", g_trace[0] == '\0');
    /* A failed app's rows cannot be reached: the count gates on failed. */
    CLK->failed = true;
    ck("a failed app offers no rows", orb_app_opt_count(clk) == 0 && !orb_app_has_options(clk));
    restore_table();

    /* -- C2: the failed app the broker names does not build -- */
    restore_table(); g_fault_slug = "color_picker"; orb_app_init_all();
    ck("an app faulted twice is marked failed and stays out of the carousel",
       A("color_picker")->failed && orb_app_count() == 2);
    ck("and it is the registry's failed flag the shell already reads",
       orb_app_at(orb_app_index_of(A("color_picker"))) == NULL);
    ck("while the owner's ledger accessor still sees the row the carousel hides",
       orb_app_row((size_t)orb_app_index_of(A("color_picker"))) == A("color_picker") &&
       orb_app_rows() == ORB_APP_BASE + ORB_APP_CAP);   /* the span is the key space */
    restore_table(); g_fault_slug = "clock"; orb_app_init_all();
    ck("a faulted clock quarantines like any card row (no HOME exemption)",
       A("clock")->failed && orb_app_count() == 2 && orb_app_home() == A("menu"));
    restore_table(); g_fault_slug = "menu"; orb_app_init_all();
    ck("the faulting HOME shell still boots — long press must land somewhere",
       !A("menu")->failed && orb_app_count() == 3);
    restore_table(); g_fault_slug = NULL;
    orb_app_init_all(); TR(); orb_app_activate(A("color_picker"));
    ck("every activation stamps the reset record's slot", AT("cur_note:color_picker") != NULL);
    restore_table();

    /* -- D1/C6: an installed card app resolves its behaviour_kind to a
     *    compiled-in module, then walks the carousel like any compiled entry.
     *    The lift drives the SHIPPED orb_app_card_upsert()/card_fill(). -- */
    restore_table(); orb_app_card_reset();
    orb_app_t card; memset(&card, 0, sizeof card);
    card.slug = (char *)"hsb2";
    card.title = (char *)"HSB Card";
    card.kind = ORB_APP_KIND_APP;
    card.behaviour_kind = ORB_BEHAVIOUR_WASM;
    int cidx = orb_app_card_upsert(&card);
    const orb_app_t *crow = orb_app_row((size_t)cidx);
    ck("a card app's behaviour_kind resolves to the registered module's iface",
       cidx == ORB_APP_BASE && crow && crow->iface == &orb_wasm_iface);
    ck("a card row lands installed from card_fill — installed is the whole state (§43.3)",
       crow && crow->installed && orb_app_count() == 2 && orb_app_index_of(crow) == ORB_APP_BASE);
    TR(); orb_app_init_all();
    ck("an installed card app builds through its module iface on the boot ladder",
       AT("hsb2_init") != NULL);
    ck("the qualified card row joins the walkable count", orb_app_count() == 2);
    /* The dense rank the count publishes is the rank the accessor answers: a
     * positional orb_app_at() once answered NULL there and the top page counted
     * zero app rows on a dial whose Clock was showing (orb-B 2026-10-25). */
    ck("the dense rank the count publishes is the rank the accessor answers",
       orb_app_at(0) == A("menu") && orb_app_at(1) == crow
       && orb_app_at(2) == NULL && orb_app_at(orb_app_count()) == NULL);
    ck("the card app sits in the carousel after the shell",
       orb_app_neighbour(A("menu"), 1) == crow
       && orb_app_neighbour(crow, 1) == A("menu")
       && orb_app_neighbour(A("menu"), -1) == crow);
    TR(); orb_app_activate(crow);
    ck("activating the installed card runs its hooks around the load and stamps its slot",
       strcmp(g_trace, "hsb2_act|load|hsb2_show|cur_note:hsb2|") == 0);
    restore_table(); orb_app_card_reset();

    /* An unknown behaviour_kind names no module: the row is in the ledger but
     * never walkable and never activatable — the validated-not-drawn rule. */
    orb_app_t bad; memset(&bad, 0, sizeof bad);
    bad.slug = (char *)"future";
    bad.title = (char *)"Future";
    bad.kind = ORB_APP_KIND_APP;
    bad.behaviour_kind = 4242;
    int bidx = orb_app_card_upsert(&bad);
    const orb_app_t *brow = orb_app_row((size_t)bidx);
    ck("an unregistered behaviour_kind resolves to no iface",
       brow && brow->iface == NULL && orb_app_behaviour_known(4242) == false
       && orb_app_behaviour_iface(4242) == NULL);
    ck("the native kinds §43 retired resolve to no module either",
       orb_app_behaviour_iface(ORB_BEHAVIOUR_COLOR_PICKER) == NULL
       && orb_app_behaviour_iface(ORB_BEHAVIOUR_CLOCK) == NULL
       && orb_app_behaviour_iface(ORB_BEHAVIOUR_FLIGHTS) == NULL);
    ck("the wasm behaviour_kind resolves to its module (registered, not ledger-only)",
       orb_app_behaviour_known(ORB_BEHAVIOUR_WASM) == true
       && orb_app_behaviour_iface(ORB_BEHAVIOUR_WASM) == &orb_wasm_iface);
    ck("a card app with no module never joins the walkable set, though installed",
       brow && brow->installed && orb_app_count() == 1
       && orb_app_neighbour(A("menu"), 1) == A("menu"));
    ck("and it cannot be activated — no screen to load, the dial stays put",
       orb_app_activate(brow) != ESP_OK && orb_app_current() != brow);
    ck("but it IS in the owner's ledger, indexed at its card slot",
       orb_app_row((size_t)bidx) == brow && orb_app_index_of(brow) == ORB_APP_BASE + (bidx - ORB_APP_BASE));
    restore_table(); orb_app_card_reset();

    /* -- D36: the service tier, populated -- */
    restore_table();
    const size_t nface = orb_app_count();
    const int    ncard = orb_app_card_count();
    ck("three service rows — Themes, USB Files, then Sound — and the index past them answers nothing",
       orb_app_menu_count() == 3 && orb_app_menu_at(0) == &orb_app_themes_row &&
       orb_app_menu_at(1) == &orb_app_usb_files_row &&
       orb_app_menu_at(2) == &orb_app_sound_row && orb_app_menu_at(3) == NULL);
    ck("the Sound page is a service row too: past the ledger, never walked, never a face",
       orb_app_is_menu_only(&orb_app_sound_row) &&
       orb_app_index_of(&orb_app_sound_row) == ORB_APP_MENU_BASE + 2 &&
       orb_app_activate(&orb_app_sound_row) == ESP_ERR_NOT_SUPPORTED &&
       orb_app_neighbour(A("clock"), 1) != &orb_app_sound_row);
    ck("a service is a menu row only — no slug lookup, so no route reaches it",
       A("usb-files") == NULL);
    ck("its index is an address past the ledger span, not a key into it",
       orb_app_index_of(&orb_app_themes_row) == ORB_APP_MENU_BASE &&
       ORB_APP_MENU_BASE >= ORB_APP_BASE + ORB_APP_CAP);
    ck("it is a service and no face row is",
       orb_app_is_menu_only(&orb_app_usb_files_row) &&
       orb_app_is_menu_only(A("menu")) == false &&
       orb_app_is_menu_only(A("clock")) == false);
    ck("activating it is refused with the reason the header promises",
       orb_app_activate(&orb_app_usb_files_row) == ESP_ERR_NOT_SUPPORTED &&
       orb_app_current() != &orb_app_usb_files_row);
    ck("a populated tier adds no face to the ledger",
       orb_app_count() == nface && orb_app_card_count() == ncard);
    ck("the carousel walk never lands on it",
       orb_app_neighbour(A("menu"), 1) != &orb_app_usb_files_row &&
       orb_app_neighbour(A("clock"), 1) != &orb_app_usb_files_row &&
       orb_app_neighbour(A("color_picker"), 1) != &orb_app_usb_files_row);
    /* A failed service must vanish from the menu rather than leave a row that
     * refuses its own press. */
    orb_app_usb_files_row.failed = true;
    ck("a failed service has no menu row",
       orb_app_menu_count() == 2 && orb_app_menu_at(0) == &orb_app_themes_row &&
       orb_app_menu_at(1) == &orb_app_sound_row);
    orb_app_usb_files_row.failed = false;
    ck("and it comes back when its module clears the fault",
       orb_app_menu_count() == 3 && orb_app_menu_at(1) == &orb_app_usb_files_row);

    /* -- the shell page is chrome, not an app -- */
    restore_table();
    ck("the options page is tagged shell chrome", A("menu")->kind == ORB_APP_KIND_SHELL);
    ck("the two real apps are tagged apps",
       A("clock")->kind == ORB_APP_KIND_APP && A("color_picker")->kind == ORB_APP_KIND_APP);
    ck("the table fits the installed-app cap B5 sets", ORB_APP_N <= ORB_APP_CAP);

    printf("%s\n", g_fail ? "SOME FAILED" : "ALL PASS");
    return g_fail ? 1 : 0;
}
'''


def build_and_run(tmpdir):
    """Compile the lifted registry against the shipped headers and run it."""
    src = STUBS.replace("__BODY__", BODY)
    cpath = os.path.join(tmpdir, "reg_test.c")
    with open(cpath, "w") as f:
        f.write(src)
    exe = os.path.join(tmpdir, "reg_test")
    cmd = ["cc", "-std=gnu17", "-g", "-O0", "-Wall", "-Wno-unused-parameter",
           # With the feature off the tier is one NULL and every case below reads
           # the ledger exactly as it always did. Defining it is what makes the
           # service-array code in orb_app.c the code under test.
           "-DCONFIG_ORB_USB_MTP=1",
           "-I", tmpdir,                       # the leaf stubs shadow the IDF ones
           "-I", P("main", "app"),
           "-I", P("main", "art"),
           "-I", P("main", "stage"),
           "-I", P("main", "menu"),
           cpath, "-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None, "COMPILE FAILED\n" + r.stdout + r.stderr
    run = subprocess.run([exe], capture_output=True, text=True, timeout=30)
    return run.returncode, run.stdout + run.stderr


def main():
    with tempfile.TemporaryDirectory() as td:
        # The stub headers, written first so the real ones are shadowed for the
        # ESP-IDF types that do not exist on a host, and NOT for the five
        # project headers the registry calls into — those are included for real.
        hdrs = {
            "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n"
                         "#define ESP_OK 0\n#define ESP_FAIL -1\n"
                         "#define ESP_ERR_NO_MEM 0x101\n"
                         "#define ESP_ERR_INVALID_ARG 0x102\n"
                         "#define ESP_ERR_INVALID_SIZE 0x103\n"
                         "#define ESP_ERR_NOT_FOUND 0x105\n"
                         "#define ESP_ERR_NOT_SUPPORTED 0x106\n"
                         "#define ESP_ERR_INVALID_STATE 0x103\n"
                         "const char *esp_err_to_name(esp_err_t);\n",
            "esp_log.h": "#pragma once\n#define ESP_LOGI(t,...)\n#define ESP_LOGE(t,...)\n"
                         "#define ESP_LOGW(t,...)\n#define ESP_LOGD(t,...)\n",
            "lvgl.h": "#pragma once\n#include <stddef.h>\n"
                      "typedef struct _lv_obj_t lv_obj_t;\n"
                      "typedef struct _lv_obj_t lv_group_t;\n"
                      "void lv_screen_load(lv_obj_t *new_screen);\n",
            "esp_partition.h": "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n"
                               "typedef struct esp_partition esp_partition_t;\n",
            "esp_heap_caps.h": "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n"
                               "#define MALLOC_CAP_SPIRAM 0\n"
                               "#define MALLOC_CAP_8BIT 0\n"
                               "void *heap_caps_calloc(size_t n, size_t sz, uint32_t caps);\n",
        }
        for n, body in hdrs.items():
            with open(os.path.join(td, n), "w") as f:
                f.write(body)
        rc, out = build_and_run(td)
        if rc is None:
            check("the lifted registry compiles against the shipped headers", False,
                  out[:1200])
        else:
            lines = [l for l in out.strip().split("\n") if l.startswith(("PASS ", "FAIL "))]
            for l in lines:
                ck_name = l[5:]
                check(ck_name, l.startswith("PASS"))
            if not lines:
                check("harness compiled and ran", False, out[:1500])
            else:
                check("every case reported", rc == 0 and "ALL PASS" in out)
    # The menu lists an app's options page only when its iface declares
    # opt_count, opt_label AND opt_arm (orb_app_has_options). The flight tracker
    # shipped the first two without the third, and its settings page was
    # unreachable on the dial with nothing red anywhere (2026-11-01). Every
    # compiled iface that offers rows must declare the hook that lists them.
    # §43: the ifaces that offer rows live in the wasm shim and the OS pages now.
    app_c = "".join(open(os.path.join(ROOT, *f)).read() for f in (
        ("main", "app", "orb_app.c"), ("main", "app", "orb_app_wasm.c"),
        ("main", "menu", "themes_page.c"), ("main", "audio", "orb_sound_menu.c")))
    ifaces = re.findall(r"static const orb_app_iface_t (\w+) = \{(.*?)\n\};", app_c, re.S)
    offering = [(n, b) for n, b in ifaces if ".opt_count" in b]
    check("every iface that offers option rows declares opt_arm, or the menu never lists it",
          bool(offering) and all(".opt_arm" in b for _, b in offering),
          ", ".join(n for n, b in offering if ".opt_arm" not in b))
    print("all host checks pass" if not FAILS else "%d FAILED" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
