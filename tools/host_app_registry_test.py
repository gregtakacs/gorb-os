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
compiled here against the REAL orb_app.h, art_bank.h, clock_view.h,
color_picker.h and menu_view.h, with only the leaves stubbed — not re-typed in
Python. An empty lift dies loudly, because a suite that silently tests a stale
copy is worse than no suite. Compiling against the shipped headers is the point:
if the registry's calls ever drift from a leaf API's real signature, this suite
fails to build rather than passing against a fiction.

What is under test
------------------
  orb_app_find_slug()   exact slug before alias, the "hue" alias still landing on
                        color_picker, and unknown/empty/NULL/case-wrong refused
  orb_app_init_all()    table order IS boot order (the clock's worker first), and
                        an init that fails drops that entry from the walk without
                        taking the dial down with it
  orb_app_neighbour()   the P2 pass line — clock and color_picker adjacent, the
                        shell's page last — plus both wraparounds
  orb_app_activate()    the call ORDER the two-level model depends on: outgoing
                        suspends before the incoming's tick restarts, and that
                        restart happens before lv_screen_load(), with the model
                        re-push after it
  art residency         a banked app maps on activate and unmaps on suspend; a
                        refused map keeps the dial on the app it was on and puts
                        the outgoing app's visibility back; a double activate
                        does not bill the same bank twice; and the NULL-reclaim
                        branch (suspend released the bank, the rollback's re-claim
                        also refused — the release-on-suspend ruling) is pinned by
                        asserting the on-glass app is left with art_slot == -1
  C7 / inertness        service() runs for a parked app, and the dial has no armed
                        view for a press to disappear into

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
#include "clock_view.h"
#include "color_picker.h"
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
static int tok_clock, tok_picker, tok_menu;
static int g_bounce, g_picker_armed, g_menu_armed;
/* Rows the clock's opt_count() reports; 0 makes the clock offer no options page. */
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

/* P7.2: the wasm behaviour module's iface lives in orb_app_wasm.c, not in the lifted
 * orb_app.c body, so the harness supplies the symbol the registry's behaviour table
 * references. An all-NULL iface is the honest stub: this suite proves the registry
 * DISPATCHES ORB_BEHAVIOUR_WASM to this module (orb_app_behaviour_iface answers the
 * pointer); the hooks' own behaviour is host_orbapp_wasm_test.py's and the device's. */
const orb_app_iface_t orb_wasm_iface = { 0 };

esp_err_t clock_view_init(orb_app_t *self)  { (void)self; T("clock_init");  return g_rc_clock; }
/* The clock's options surface (B1/B2). Stubbed rather than lifted: what this
 * suite proves is that the registry DISPATCHES them, and the answers themselves
 * are host_menu_test.py's and the device's to check. */
esp_err_t clock_themes_init(void)     { return ESP_OK; }
void      clock_themes_service(void)  { T("clock_service"); }
void      clock_themes_request_scan(void) { T("clock_scan"); }
void      clock_themes_reset(void)    { }
void      clock_themes_pick(int i)    { T("clock_pick(%d)", i); }
int       clock_opt_count(orb_app_t *a)     { (void)a; return g_clock_opts; }
const char *clock_opt_label(orb_app_t *a, int i) { (void)a; return (i == 0) ? "diver" : "neon"; }
const char *clock_opt_value(orb_app_t *a, int i) { (void)a; return (i == 0) ? "on the dial" : ""; }
void      clock_opt_inc(orb_app_t *a, int i, int32_t d) { (void)a; T("clock_inc(%d,%d)", i, (int)d); }
void      clock_opt_sel(orb_app_t *a, int i) { (void)a; T("clock_sel(%d)", i); }
void      clock_opt_arm(orb_app_t *a, bool on) { (void)a; T(on ? "clock_arm(1)" : "clock_arm(0)"); }
/* D12's two new answers, stubbed the same way: this suite proves the registry
 * DISPATCHES them and defaults them sanely, and the answers' content is
 * host_menu_test.py's shape table to check. The clock says GROUP at row 1 and
 * ITEM at 2, so a dispatcher that lost the value would answer OPT_ROW there. */
static orb_opt_kind_t g_clock_kind = OPT_ROW;
orb_opt_kind_t clock_opt_kind(orb_app_t *a, int i)
{ (void)a; T("clock_kind(%d)", i); return (i == 1) ? OPT_GROUP :
                                                      (i == 2 ? OPT_ITEM : g_clock_kind); }
static bool g_menu_long_taken;
bool menu_view_opt_long(void) { T("menu_long"); return g_menu_long_taken; }
esp_err_t color_picker_init(void){ T("picker_init"); return g_rc_picker; }
void color_picker_bind_app(orb_app_t *a) { (void)a; T("picker_bind"); }
esp_err_t menu_view_init(void)   { T("menu_init");   return g_rc_menu; }

lv_obj_t *clock_view_screen(void)       { return (lv_obj_t *)&tok_clock; }
lv_obj_t *color_picker_screen(void)     { return (lv_obj_t *)&tok_picker; }
lv_obj_t *menu_view_screen(void)        { return (lv_obj_t *)&tok_menu; }
void clock_view_set_bounce(bool on)     { g_bounce = on ? 1 : 0; T(on ? "bounce(1)" : "bounce(0)"); }
bool clock_view_bounce(void)            { return g_bounce != 0; }
void color_picker_show(void)            { T("picker_show"); }
void color_picker_apply_input(int32_t d, bool p) { T("picker_input(%d,%d)", (int)d, p ? 1 : 0); }
bool color_picker_armed(void)           { return g_picker_armed != 0; }
void color_picker_set_armed(bool a)     { g_picker_armed = a ? 1 : 0; T(a ? "picker_arm(1)" : "picker_arm(0)"); }
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

/* ---- the cases ---------------------------------------------------------- */
static int g_fail;
static void ck(const char *name, int ok)
{
    printf("%s %s%s\n", ok ? "PASS" : "FAIL", name, "");
    if (!ok) g_fail++;
}
static const orb_app_t *A(const char *slug) { return orb_app_find_slug(slug); }
static int idx(const char *slug) { return orb_app_index_of(A(slug)); }
static void seed_factory(void);   /* defined under restore_table (D35) */
static void restore_table(void)
{
    for (size_t i = 0; i < ORB_APP_N; i++) {
        s_apps[i].enabled = true; s_apps[i].failed = false;
        s_apps[i].installed = true; s_apps[i].art_slot = -1;
        s_apps[i].art_bytes = 0; s_apps[i].art_offset = 0;
    }
    s_apps[0].is_home = true;          /* D35: the one compiled row is the shell */
    s_current = NULL; g_bounce = g_picker_armed = g_menu_armed = 0; g_clock_opts = 2;
    g_map_calls = g_unmap_calls = g_service_calls = 0;
    g_map_rc = g_unmap_rc = ESP_OK; g_next_slot = 3; g_refuse_bytes = 0;
    g_rc_clock = g_rc_picker = g_rc_menu = ESP_OK;
    orb_app_card_reset();
    seed_factory();
}

/* D35 (plan §35): the factory rows, upserted the way orb_app_install's memory
 * source does it — behaviour kinds 3 and 1, slots 0 and 1, enabled — without a
 * partition, a CRC or an NVS word. The registry's view of the shipped dial. */
static void seed_factory(void)
{
    orb_app_t r;
    memset(&r, 0, sizeof r);
    r.slug = (char *)"clock"; r.title = (char *)"Clock";
    r.kind = ORB_APP_KIND_APP; r.behaviour_kind = ORB_BEHAVIOUR_CLOCK;
    r.flash_slot = 0; r.art_slot = -1;
    orb_app_card_upsert(&r);
    orb_app_set_enabled(orb_app_find_slug("clock"), true);
    memset(&r, 0, sizeof r);
    r.slug = (char *)"color_picker"; r.title = (char *)"HSB";
    r.kind = ORB_APP_KIND_APP; r.behaviour_kind = ORB_BEHAVIOUR_COLOR_PICKER;
    r.flash_slot = 1; r.art_slot = -1;
    orb_app_card_upsert(&r);
    orb_app_set_enabled(orb_app_find_slug("color_picker"), true);
}

/* The seeded rows, addressed where the old compiled entries used to be. */
#define CLK ((orb_app_t *)orb_app_find_slug("clock"))
#define PCK ((orb_app_t *)orb_app_find_slug("color_picker"))

int main(void)
{
    /* -- boot: the compiled shell builds first; the card tier follows in slot
     *    order, so the factory dial leads every installed app (D35 §35). The
     *    shared orb_jobs worker was started by main.c before this ladder, so
     *    no row's init can post to a queue that does not exist. -- */
    restore_table(); TR();
    orb_app_init_all();
    ck("boot inits menu, then the factory clock, then the factory picker",
       strncmp(g_trace, "menu_init|clock_init|picker_init|picker_bind", 43) == 0);

    /* -- a failing init drops that entry, not the dial -- */
    restore_table(); TR(); g_rc_picker = ESP_FAIL;
    orb_app_init_all();
    ck("a picker whose init fails leaves the dial up and running",
       orb_app_count() == 2 && A("color_picker")->enabled == false);
    ck("the failed app drops out of the carousel rather than showing a blank",
       orb_app_neighbour(A("clock"), 1) == A("menu")
       && orb_app_neighbour(A("menu"), -1) == A("clock"));

    /* -- slug resolution -- */
    restore_table();
    ck("the three entries resolve by slug", A("clock") && A("color_picker") && A("menu"));
    /* D35: card rows carry no alias field, and the registry no longer answers
     * "hue" — the ?to= route translates it in the layer that owns routes. */
    ck("the retired hue alias resolves nowhere in the registry", A("hue") == NULL);
    ck("an unknown slug resolves to nothing", A("nope") == NULL);
    ck("an empty slug resolves to nothing", A("") == NULL);
    ck("a NULL slug resolves to nothing", A(NULL) == NULL);
    ck("slug matching is case-sensitive", A("Clock") == NULL);
    ck("the shell is the home entry (D35 amended C8)", strcmp(orb_app_home()->slug, "menu") == 0);

    /* -- the carousel: the ring the factory ships — shell, dial, editor. The
     *    P2 pass line survives as a property of the seeded slots, not of the
     *    compiled table: turning from the dial lands on the picker. -- */
    restore_table();
    ck("clock -> color_picker is one step (P2 pass line, card-tier order)",
       orb_app_neighbour(A("clock"), 1) == A("color_picker"));
    ck("color_picker -> menu is one step",
       orb_app_neighbour(A("color_picker"), 1) == A("menu"));
    ck("menu wraps forward to the clock", orb_app_neighbour(A("menu"), 1) == A("clock"));
    ck("the clock wraps back to the menu", orb_app_neighbour(A("clock"), -1) == A("menu"));
    ck("NULL start means the first walkable entry, the shell",
       orb_app_neighbour(NULL, 1) == A("menu"));
    ck("long press homes to the shell (C8 as amended)", orb_app_home() == A("menu"));
    restore_table(); orb_app_set_enabled(A("color_picker"), false);
    ck("a disabled entry is skipped in both directions",
       orb_app_neighbour(A("clock"), 1) == A("menu")
       && orb_app_neighbour(A("menu"), -1) == A("clock"));
    restore_table();
    s_apps[0].enabled = false;
    orb_app_set_enabled(A("clock"), false);
    orb_app_set_enabled(A("color_picker"), false);
    ck("a table with nothing walkable answers NULL rather than spinning",
       orb_app_neighbour(A("clock"), 1) == NULL);

    /* -- the switch ORDER the two-level model depends on -- */
    restore_table(); orb_app_init_all(); TR();
    orb_app_activate(A("clock"));
    ck("activating the dial starts its beat", strstr(g_trace, "bounce(1)") != NULL);

    restore_table(); orb_app_init_all(); orb_app_activate(A("clock")); TR();
    orb_app_activate(A("color_picker"));
    /* The dial's beat must be parked BEFORE the picker's screen is loaded: a
     * beat left running across the swap burns flush work on rows nobody can
     * see, which is the cadence collapse the fps findings measured. */
    ck("leaving the dial parks its beat before the new screen loads",
       strstr(g_trace, "bounce(0)") != NULL
       && strstr(g_trace, "bounce(0)") < strstr(g_trace, "load")
       && strstr(g_trace, "load") != NULL);
    ck("the picker re-pushes its model after the load, not before",
       strstr(g_trace, "load") != NULL && strstr(g_trace, "picker_show") != NULL
       && strstr(g_trace, "load") < strstr(g_trace, "picker_show"));
    ck("the picker is the screen that got loaded",
       g_last_loaded == (struct _lv_obj_t *)&tok_picker);
    ck("returning to the dial restarts the beat before the load",
       (TR(), orb_app_activate(A("clock")), strstr(g_trace, "bounce(1)") < strstr(g_trace, "load"))
       && g_last_loaded == (struct _lv_obj_t *)&tok_clock);

    /* -- input goes to the visible entry only -- */
    restore_table(); orb_app_init_all(); orb_app_activate(A("color_picker")); TR();
    orb_app_input(3, false);
    ck("a turn reaches the armed app's own input hook",
       strstr(g_trace, "picker_input(3,0)") != NULL && strstr(g_trace, "menu_input") == NULL);
    restore_table(); orb_app_init_all(); orb_app_activate(A("clock")); TR();
    orb_app_input(1, true);
    ck("the dial has no input hook, so a turn there does nothing", g_trace[0] == '\0');

    /* -- arming, and the dial's deliberate inertness -- */
    restore_table(); orb_app_init_all(); orb_app_activate(A("clock"));
    ck("the dial never reports armed, so a press cannot disappear into it",
       orb_app_current_armed() == false && orb_app_arm_current() == false);
    orb_app_activate(A("menu"));
    ck("the menu arms and reports it", orb_app_arm_current() == true && orb_app_current_armed() == true);
    TR(); orb_app_disarm_all();
    ck("disarm reaches every entry, not only the one leaving",
       strstr(g_trace, "menu_arm(0)") != NULL && strstr(g_trace, "picker_arm(0)") != NULL
       && g_menu_armed == 0 && g_picker_armed == 0);

    /* -- C7: a parked app still reaps its own jobs -- */
    restore_table(); orb_app_init_all(); orb_app_activate(A("menu"));
    g_service_calls = 0; orb_app_activate(A("clock")); TR(); g_service_calls = 0;
    orb_app_service_all();
    ck("a parked app's worker jobs are still reaped (C7)",
       g_service_calls == 1 && strstr(g_trace, "menu_service") != NULL);
    ck("reaping does not load a screen: the dial stays on the glass",
       orb_app_current() == A("clock"));

    /* -- art residency: claim on activate, release on suspend -- */
    restore_table(); orb_app_init_all();
    PCK->art_offset = 0x400000; PCK->art_bytes = 0x80000;
    TR(); g_map_calls = 0;
    orb_app_activate(A("color_picker"));
    ck("a banked app maps its secondary bank on activate",
       g_map_calls == 1 && PCK->art_slot >= 0 && strstr(g_trace, "map(4194304,524288)") != NULL);
    ck("the slot the registry booked is the slot handed to the compositor",
       PCK->art_slot >= 3 && art_bank_extra_ptr(PCK->art_slot) == NULL);
    TR(); g_unmap_calls = 0; g_svc_revoke_calls = 0;
    orb_app_activate(A("clock"));
    ck("suspending releases the bank, so a parked app costs no MMU pages",
       g_unmap_calls == 1 && PCK->art_slot == -1);
    ck("and the same swap revokes the parked app's hold on the dial",
       g_svc_revoke_calls == 1 && strstr(g_trace, "svc_revoke:color_picker") != NULL);

    /* -- the overflow path: a refused claim must not blank the dial -- */
    restore_table(); orb_app_init_all();
    /* Both entries banked, the incoming one bigger: this is §3.3's mechanism-B
     * question in miniature — the app that cannot fit must be refused, and the
     * app already on the glass must come back exactly as it was. */
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
    ck("the dial's beat is running again after the refused switch", g_bounce == 1);
    g_refuse_bytes = 0;
    ck("the same switch succeeds once the budget allows it",
       orb_app_activate(A("color_picker")) == ESP_OK && orb_app_current() == A("color_picker"));

    /* The harder branch: the budget is so tight that even putting the outgoing
     * bank back fails. The switch is still refused and the dial still points at
     * the app it did — that case logs an emergency rather than pretending.
     * This is the NULL-reclaim branch (orb_app.c app_restore): the release-vs-
     * carry ruling (release-on-suspend, owner ruling 2026-10-12) makes it
     * reachable, because a suspend released the on-glass app's bank and the
     * rollback cannot re-claim it. The invariant that distinguishes it is pinned
     * here, not just the return code: the app left ON the glass ends with
     * art_slot == -1 — the compositor would read art_bank_extra_ptr(-1) and paint
     * the garbage art_bank.h fences off — and s_current was never reassigned. */
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
    /* Two map attempts past the reset: the refused incoming claim, and the
     * failed rollback re-claim of the outgoing bank. One unmap: releasing the
     * outgoing bank before the incoming claim. */
    ck("NULL-reclaim touched the window once released, twice refused to map",
       g_unmap_calls == 1 && g_map_calls == 2);
    g_map_rc = ESP_OK;

    /* -- no double-billing: re-activating a resident app maps it once -- */
    restore_table(); orb_app_init_all();
    PCK->art_offset = 0x400000; PCK->art_bytes = 0x80000;
    g_map_calls = 0;
    orb_app_activate(A("color_picker")); orb_app_activate(A("menu"));
    orb_app_activate(A("color_picker"));
    ck("a bank is never mapped twice while it is still resident",
       g_map_calls == 2);   /* out->clock in, clock->out ... exactly two claims */

    /* -- the options surface (B1/B2): the registry is the only door -- */
    restore_table();
    const orb_app_t *clk = A("clock"), *pick = A("color_picker");
    ck("the clock declares options and the picker declares none",
       orb_app_has_options(clk) && !orb_app_has_options(pick));
    ck("the count reaches the shell through the registry",
       orb_app_opt_count(clk) == 2 && orb_app_opt_count(pick) == 0);
    /* The reachability bug (B2): has_options() once asked opt_count() > 0, so a
     * page whose rows come from a card walk that only the row press starts was
     * invisible until it had been shown — a closed loop the dial can never
     * enter. Offering the page is what the entry DECLARES (the opt_* hooks are
     * present), not what it currently answers. g_clock_opts = 0 is the state
     * before the first walk lands; the row must still be there to press. */
    g_clock_opts = 0;
    ck("an app that declares hooks offers its row before its first scan lands",
       orb_app_has_options(clk) && orb_app_opt_count(clk) == 0);
    g_clock_opts = 2;
    ck("label and value answer in the app's words",
       strcmp(orb_app_opt_label(clk, 0), "diver") == 0 &&
       strcmp(orb_app_opt_value(clk, 0), "on the dial") == 0);
    /* Out of range is refused by the registry rather than handed to the app: a
     * shell that forwarded the index would let a cursor past the end press a
     * row the app never counted, which is how a theme gets staged that nobody
     * is looking at. */
    ck("a row outside the count is refused before the app sees it",
       orb_app_opt_label(clk, 9)[0] == '\0' && orb_app_opt_value(clk, -1)[0] == '\0');
    TR(); orb_app_opt_sel(clk, 0);
    ck("the press dispatches once, with the row index",
       strcmp(g_trace, "clock_sel(0)|") == 0);
    TR(); orb_app_opt_inc(clk, 1, 3);
    ck("a rotate dispatches with the row and the detents",
       strcmp(g_trace, "clock_inc(1,3)|") == 0);
    TR(); orb_app_opt_arm(clk, true);
    ck("arming the options page reaches the app", strcmp(g_trace, "clock_arm(1)|") == 0);
    TR(); orb_app_opt_arm(clk, false);
    ck("leaving it disarms, so nothing is armed off-glass",
       strcmp(g_trace, "clock_arm(0)|") == 0);
    /* D12's two new answers. The KIND one dispatches for the app that declares it
     * and answers OPT_ROW for the one that does not: color_picker's iface names no
     * opt_* hook at all, and the entire pre-D12 ABI depends on that meaning a flat
     * page rather than a crash or an invisible row. */
    g_clock_opts = 3;
    const int k_group = orb_app_opt_kind(clk, 1);
    const int k_item  = orb_app_opt_kind(clk, 2);
    const int k_row   = orb_app_opt_kind(clk, 0);
    g_clock_opts = 2;
    ck("the row kind reaches the shell as the app answered it",
       k_group == OPT_GROUP && k_item == OPT_ITEM && k_row == OPT_ROW);
    ck("a row outside the count is OPT_ROW without asking the app",
       (orb_app_opt_kind(clk, 9) == OPT_ROW) && (orb_app_opt_kind(clk, -1) == OPT_ROW));
    ck("a NULL opt_kind hook is the flat page it always was",
       orb_app_opt_kind(pick, 0) == OPT_ROW && orb_app_opt_count(pick) == 0);
    /* And the long press: an entry with nothing to collapse leaves it to the
     * router, which is C8's hold-to-home untouched. The menu entry is the one that
     * can spend it, because PG_APP's open group is the shell's own state. */
    const orb_app_t *menu = A("menu");
    ck("the clock declares no long-press hook, so it cannot swallow the escape hatch",
       orb_app_opt_long(clk) == false);
    ck("no entry at all takes nothing", orb_app_opt_long(NULL) == false);
    if (menu) {
        g_menu_long_taken = true;
        ck("the shell entry can take it, and the router must then not home",
           orb_app_opt_long(menu) == true);
        g_menu_long_taken = false;
        ck("and refuses it when no group is open", orb_app_opt_long(menu) == false);
    }
    /* A NULL hook is a skip, not a crash: the picker answers none of these and
     * the shell must be able to render its (empty) page regardless. */
    TR(); orb_app_opt_sel(pick, 0); orb_app_opt_arm(pick, true);
    ck("an app with no option hooks is skipped, not called", g_trace[0] == '\0');
    /* A disabled app cannot have its rows reached either: the count gates on
     * enabled/failed, which is what keeps P4's disable path from leaving a live
     * page on the glass. */
    orb_app_set_enabled(A("clock"), false);   /* D35: the clock is a card row now */
    ck("a disabled app offers no rows", orb_app_opt_count(clk) == 0 && !orb_app_has_options(clk));
    restore_table();

    /* -- C2: the failed app the broker names does not build -- */
    restore_table(); g_fault_slug = "color_picker"; orb_app_init_all();
    ck("an app faulted twice is marked failed and stays out of the carousel",
       A("color_picker")->failed && orb_app_count() == 2);
    ck("and it is the registry's failed flag the shell already reads",
       orb_app_at(orb_app_index_of(A("color_picker"))) == NULL);
    ck("while the owner's ledger accessor still sees the row the carousel hides",
       orb_app_row((size_t)orb_app_index_of(A("color_picker"))) == A("color_picker") &&
       /* P4a: the ledger span is POSITIONAL — compiled tier plus the whole
        * card table — so index == index_of == the broker's key even when a
        * slot is free (row() answers NULL for the hole). A rank-dense count
        * here would drift on every uninstall. */
       orb_app_rows() == ORB_APP_BASE + ORB_APP_CAP);   /* D35: the span is the key space */
    restore_table(); g_fault_slug = "clock"; orb_app_init_all();
    ck("a faulted factory clock quarantines like any card row (D35, no HOME exemption)",
       A("clock")->failed && orb_app_count() == 2 && orb_app_home() == A("menu"));
    restore_table(); g_fault_slug = "menu"; orb_app_init_all();
    ck("the faulting HOME shell still boots — long press must land somewhere",
       !A("menu")->failed && orb_app_count() == 3);
    restore_table(); g_fault_slug = NULL;
    TR(); orb_app_activate(A("color_picker"));
    ck("every activation stamps the reset record's slot",
       strstr(g_trace, "cur_note:color_picker") != NULL);
    restore_table();

    /* -- D1/C6: an installed card app resolves its behaviour_kind to a
     *    compiled-in module, then walks the carousel like any compiled entry.
     *    The card table is a PSRAM block on the device and a calloc here; the
     *    lift drives the SHIPPED orb_app_card_upsert()/card_fill(), so the
     *    iface attach, the walk gather, and the ledger-only refusal are the real
     *    shipping code, not a re-typed copy. -- */
    restore_table(); orb_app_card_reset();      /* D35: the card table starts empty,
                                                 * so this section sees the shell
                                                 * alone until it plants a row. */
    orb_app_t card; memset(&card, 0, sizeof card);
    card.slug = (char *)"hsb2";            /* distinct from the compiled picker */
    card.title = (char *)"HSB Card";
    card.kind = ORB_APP_KIND_APP;
    card.behaviour_kind = ORB_BEHAVIOUR_COLOR_PICKER;   /* resolves to s_picker_iface */
    int cidx = orb_app_card_upsert(&card);
    const orb_app_t *crow = orb_app_row((size_t)cidx);
    ck("a card app's behaviour_kind resolves to the registered module's iface",
       cidx == ORB_APP_BASE && crow && crow->iface == orb_app_behaviour_iface(ORB_BEHAVIOUR_COLOR_PICKER)
       && crow->iface != NULL);
    ck("a registered card app starts ledger-only: enabled is false from card_fill",
       crow && crow->enabled == false && orb_app_count() == 1
       && orb_app_index_of(crow) == ORB_APP_BASE);
    /* Enable the one card row and rebuild: init_all must run its iface hooks. */
    ((orb_app_t *)crow)->enabled = true;
    TR(); orb_app_init_all();
    ck("an enabled card app builds through its module iface on the boot ladder",
       strstr(g_trace, "picker_init") != NULL);
    ck("the qualified card row joins the walkable count", orb_app_count() == 2);
    /* D35: the compiled tier is the shell alone, and it leads; the card follows
     * and the two wrap. (The shipped dial additionally holds the factory pair,
     * seeded and pinned in the blocks above.) */
    ck("the card app sits in the carousel after the shell",
       orb_app_neighbour(A("menu"), 1) == crow
       && orb_app_neighbour(crow, 1) == A("menu")
       && orb_app_neighbour(A("menu"), -1) == crow);
    TR(); orb_app_activate(crow);
    ck("activating the installed card loads the picker screen and stamps its slot",
       strcmp(g_trace, "load|picker_show|cur_note:hsb2|") == 0);
    restore_table(); orb_app_card_reset();

    /* An unknown behaviour_kind names no module: the row is in the ledger but
     * never walkable and never activatable — the validated-not-drawn rule. */
    orb_app_t bad; memset(&bad, 0, sizeof bad);
    bad.slug = (char *)"future";
    bad.title = (char *)"Future";
    bad.kind = ORB_APP_KIND_APP;
    bad.behaviour_kind = 4242;             /* no firmware module registers this */
    int bidx = orb_app_card_upsert(&bad);
    const orb_app_t *brow = orb_app_row((size_t)bidx);
    ck("an unregistered behaviour_kind resolves to no iface",
       brow && brow->iface == NULL && orb_app_behaviour_known(4242) == false
       && orb_app_behaviour_iface(4242) == NULL);
    /* P7.2: the wasm tier is a registered behaviour module, so ORB_BEHAVIOUR_WASM
     * must resolve (not to NULL) and be known, exactly as the picker does. This is a
     * DISPATCH fact — that the registry maps the kind to the wasm module — and it is
     * what makes an installed code app walkable-instead-of-ledger-only rather than a
     * card row nobody can reach. The stub iface (all-NULL) is enough to prove it. */
    ck("the wasm behaviour_kind resolves to its module (registered, not ledger-only)",
       orb_app_behaviour_known(ORB_BEHAVIOUR_WASM) == true
       && orb_app_behaviour_iface(ORB_BEHAVIOUR_WASM) == &orb_wasm_iface
       && orb_app_behaviour_iface(ORB_BEHAVIOUR_WASM) != NULL);
    ((orb_app_t *)brow)->enabled = true;   /* force-enable: it must STILL stay out */
    /* D35: the card table here holds this one unregistered row and nothing else
     * (the card_reset above cleared the factory pair), so the ring is the shell
     * alone and the bad row stays out of it even enabled. */
    ck("a card app with no module never joins the walkable set",
       orb_app_count() == 1 && orb_app_neighbour(A("menu"), 1) == A("menu")
       && orb_app_neighbour(A("menu"), 1) != brow);
    ck("and it cannot be activated — no screen to load, the dial stays put",
       orb_app_activate(brow) != ESP_OK && orb_app_current() != brow);
    ck("but it IS in the owner's ledger, indexed at its card slot",
       orb_app_row((size_t)bidx) == brow && orb_app_index_of(brow) == ORB_APP_BASE + (bidx - ORB_APP_BASE));
    restore_table(); orb_app_card_reset();

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
           "-I", tmpdir,                       # the leaf stubs shadow the IDF ones
           "-I", P("main", "app"),
           "-I", P("main", "art"),
           "-I", P("main", "clock"),
           "-I", P("main", "color"),
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
    print("all host checks pass" if not FAILS else "%d FAILED" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
