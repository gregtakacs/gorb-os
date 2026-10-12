#!/usr/bin/env python3
"""host_carousel_test.py — the knob's text carousel, tested on the host.

Why this file exists
--------------------
main/app/orb_carousel.c decides, on every detent, whether a screen changes at
all. Get it wrong in the quiet direction and the knob stops walking; get it wrong
in the loud direction and a three-detent flick pays for three full-page commits,
which is the cost this module was written to avoid (the dial's own reveal
measured 411-476 ms, docs/app-platform-plan.md §12.7). Neither failure is
comfortably visible from a serial console: one looks like a dead knob, the other
only shows up as a tempo number on a bench run.

So the shipped code is lifted VERBATIM out of main/app/orb_carousel.c and
compiled here against the REAL orb_app.h, with only the leaves stubbed — not
re-typed in Python. Compiling against the shipped header is the point: if the
carousel's calls ever drift from orb_app_neighbour()'s or orb_sched_pause()'s
real signature, this suite fails to build rather than passing against a fiction.

What is under test
------------------
  at rest          nothing is pending and due() is false, so an idle poll can
                   never invent a switch
  the peek         a detent NAMES the next stop and parks the beat instead of
                   loading a screen — the deferral the module exists for
  burst walk       three detents name three different entries, because the walk
                   continues from the entry already named, not from the one on
                   the glass. Restarting from the glass every call is the bug
                   this case exists for: the burst would never travel further
                   than one screen and the flick would look broken.
  two windows      an in-burst detent (arrived within ORB_CAROUSEL_BURST_GAP_MS
                   of the last) commits ORB_CAROUSEL_SETTLE_MS after itself; a
                   detent that lands after a pause is a stop to read and holds
                   the preview for ORB_CAROUSEL_READ_MS instead. The single
                   400 ms window was killed by the owner's own hand (§12.9):
                   11 of 11 hand gaps exceeded it and nothing coalesced.
  park handover    commit() clears its claim on the beat WITHOUT resuming it:
                   the activation the caller runs next owns the beat
  park honesty     cancel() resumes only a beat this module parked — on the
                   picker and the menu there was none to park
  degenerate table a single walkable entry, and a burst that wraps the whole way
                   round, must leave nothing pending, so the router can never
                   commit a phantom switch
  no chip          if the chip was never built, step() refuses and the router
                   falls back to the immediate switch
  the numbers      the fast window clears the encoder's own recovery window
                   (30 ms); the burst gap sits between the two measured hand
                   regimes; the read window covers the longest pause taken

Run:  python3 tools/host_carousel_test.py
"""

import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
def P(*a): return os.path.join(ROOT, *a)

CAR_C = P("main", "app", "orb_carousel.c")
if not os.path.exists(CAR_C):
    sys.exit("missing " + CAR_C + " — the lifts below have no fallback")
car_src = open(CAR_C).read()

FAILS = []
def check(name, ok, extra=""):
    print(("PASS " if ok else "FAIL ") + name + (("  <- " + extra) if (extra and not ok) else ""))
    if not ok: FAILS.append(name)

def lift(text, pattern, where):
    m = re.search(pattern, text, re.S)
    if not m:
        sys.exit("could not lift %r from %s — did the code move? Dying here is the "
                 "point: an empty lift would keep this suite green while testing a "
                 "copy that no longer ships." % (pattern, where))
    return m.group(0)

# Everything from the chip geometry down: the statics, the widget build and the
# whole peek, as ONE contiguous span. Lifting the tail rather than eleven named
# functions means a block added above the peek comes along whether or not
# anybody remembered a lift for it.
BODY = lift(car_src, r"#define CHIP_W[\s\S]*$", CAR_C)
SETTLE = int(lift(car_src, r"#define ORB_CAROUSEL_SETTLE_MS\s+\d+", CAR_C).split()[-1])
READ_MS = int(lift(car_src, r"#define ORB_CAROUSEL_READ_MS\s+\d+", CAR_C).split()[-1])
BGAP_MS = int(lift(car_src, r"#define ORB_CAROUSEL_BURST_GAP_MS\s+\d+", CAR_C).split()[-1])

# ---------------------------------------------------------------------------
# The leaf stubs. Each records the fact the cases below read: the calls, in
# order. The LVGL leaves are no-ops with identity pointers — what matters here
# is whether the chip was BUILT and whether it was told to show, not how LVGL
# rasterises it. That division is the reason this module is testable at all:
# the compositor interaction it exists to survive is a hardware question, and
# the state machine around it is not.
# ---------------------------------------------------------------------------
STUBS = r'''
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "orb_sched.h"
#include "orb_app.h"
#include "orb_carousel.h"

char g_trace[8192];
int  g_tn;
static void T(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    if (g_tn < (int)sizeof(g_trace) - 96)
        g_tn += vsnprintf(g_trace + g_tn, sizeof(g_trace) - g_tn, fmt, ap);
    va_end(ap);
    g_trace[g_tn++] = '|'; g_trace[g_tn] = '\0';
}
static void TR(void) { g_trace[0] = '\0'; g_tn = 0; }
static int has(const char *needle) { return strstr(g_trace, needle) != NULL; }

const char *esp_err_to_name(esp_err_t c) { return c == ESP_OK ? "ESP_OK" : "ERR"; }

/* The scheduler's gates as the carousel sees them. g_running stands in for "the
 * dial's beat is on the glass": true on the clock, false on the picker and the
 * menu. g_pause/g_resume count the calls, because a leaked park is invisible
 * until somebody's clock stops animating in a sealed case, and a stolen park is
 * a beat resumed that its real owner had just parked. */
static int g_started = 1, g_running = 1, g_pause, g_resume;
int  orb_sched_started(void) { return g_started; }
int  orb_sched_running(void) { return g_running; }
void orb_sched_pause(void)   { g_pause++;  T("pause"); }
void orb_sched_resume(void)  { g_resume++; T("resume"); }

/* Two distinct non-NULL tokens, so orb_carousel_ready() and step()'s no-chip
 * refusal are both observable, and g_top == NULL is how init() signals that it
 * consumed the only top-layer handle. */
static int chip_tok, lbl_tok;
static lv_obj_t *g_top;
static int g_shown, g_hidden;
lv_obj_t *lv_layer_top(void)            { return g_top; }
lv_obj_t *lv_obj_create(lv_obj_t *p)    { (void)p; return (lv_obj_t *)&chip_tok; }
lv_obj_t *lv_label_create(lv_obj_t *p)  { (void)p; return (lv_obj_t *)&lbl_tok; }
void lv_label_set_text(lv_obj_t *o, const char *t) { (void)o; T("text(%s)", t ? t : "(null)"); }
void lv_obj_add_flag(lv_obj_t *o, lv_obj_flag_t f)    { (void)o; (void)f; g_hidden++; T("hide"); }
void lv_obj_remove_flag(lv_obj_t *o, lv_obj_flag_t f) { (void)o; if (f == LV_OBJ_FLAG_HIDDEN) { g_shown++; T("show"); } }
void lv_obj_move_foreground(lv_obj_t *o) { (void)o; }
void lv_obj_set_size(lv_obj_t *o, int32_t w, int32_t h) { (void)o; (void)w; (void)h; }
void lv_obj_set_width(lv_obj_t *o, int32_t w) { (void)o; (void)w; }
void lv_obj_align(lv_obj_t *o, int a, int32_t x, int32_t y) { (void)o; (void)a; (void)x; (void)y; }
void lv_obj_center(lv_obj_t *o) { (void)o; }
void lv_obj_set_scrollable(lv_obj_t *o, bool b) { (void)o; (void)b; }
void lv_obj_set_scrollbar_mode(lv_obj_t *o, int m) { (void)o; (void)m; }
void lv_obj_set_style_bg_color(lv_obj_t *o, lv_color_t c, int s) { (void)o; (void)c; (void)s; }
void lv_obj_set_style_bg_opa(lv_obj_t *o, int a, int s) { (void)o; (void)a; (void)s; }
void lv_obj_set_style_radius(lv_obj_t *o, int r, int s) { (void)o; (void)r; (void)s; }
void lv_obj_set_style_border_width(lv_obj_t *o, int w, int s) { (void)o; (void)w; (void)s; }
void lv_obj_set_style_pad_all(lv_obj_t *o, int p, int s) { (void)o; (void)p; (void)s; }
void lv_obj_set_style_text_color(lv_obj_t *o, lv_color_t c, int s) { (void)o; (void)c; (void)s; }
void lv_obj_set_style_text_font(lv_obj_t *o, const lv_font_t *f, int s) { (void)o; (void)f; (void)s; }
void lv_label_set_long_mode(lv_obj_t *o, lv_label_long_mode_t m) { (void)o; (void)m; }
lv_color_t lv_color_black(void) { lv_color_t c = {0,0,0}; return c; }
lv_color_t lv_color_white(void) { lv_color_t c = {0,0,0}; return c; }
lv_font_t lv_font_montserrat_48;

/* ---- the module under test, lifted verbatim from the device build ------- */
__BODY__

/* ---- the fake registry ---------------------------------------------------
 * orb_app_neighbour() has its own 43-check suite in host_app_registry_test.py.
 * Stubbing it here is the right seam: everything under test in THIS module is
 * what the carousel does with the answer, not the answer. */
static orb_app_t t_clock, t_picker, t_menu;
static orb_app_t *g_ring[3];
static int g_ring_n;
static int g_neighbour_calls;

static void mk(orb_app_t *a, const char *slug, const char *title)
{
    memset(a, 0, sizeof *a);
    a->slug = slug; a->title = title;
    a->kind = ORB_APP_KIND_APP;
    a->art_slot = -1; a->installed = true; a->enabled = true;
}
/* The table the device ships today (clock, color_picker, menu — the menu's title
 * deliberately absent, so the slug fallback is exercised), with the module's own
 * state scrubbed: a case that inherits a pending peek from the case before it
 * proves nothing about the quiet direction. */
static void reset(void)
{
    mk(&t_clock, "clock", "Clock");
    mk(&t_picker, "color_picker", "HSB");
    mk(&t_menu, "menu", "Menu");
    g_ring[0] = &t_clock; g_ring[1] = &t_picker; g_ring[2] = &t_menu;
    g_ring_n = 3; g_neighbour_calls = 0;
    s_pending = NULL; s_hops = 0; s_parked = false; s_last_us = 0; s_due_us = 0;
    g_pause = g_resume = 0; g_running = 1; g_started = 1;
    g_top = (lv_obj_t *)&chip_tok; g_shown = g_hidden = 0;
    /* Left NULL on purpose: only orb_carousel_init() builds the chip, so a case
     * that forgot to call it fails the same way the board would if the heap had
     * said no at boot, instead of quietly borrowing the previous case's widget. */
    s_chip = NULL; s_lbl = NULL;
    TR();
}
const orb_app_t *orb_app_neighbour(const orb_app_t *from, int step)
{
    g_neighbour_calls++;
    int i = -1;
    for (int k = 0; k < g_ring_n; k++) if ((const orb_app_t *)g_ring[k] == from) i = k;
    if (i < 0) return g_ring_n ? g_ring[0] : NULL;
    for (int hops = 0; hops < g_ring_n; hops++) {
        i = (i + step) % g_ring_n; if (i < 0) i += g_ring_n;
        if (g_ring[i]->enabled && !g_ring[i]->failed) return g_ring[i];
    }
    return NULL;
}

/* ---- the cases ---------------------------------------------------------- */
static int g_fail;
#define CK(name, cond) do { \
    if (!(cond)) { g_fail++; printf("FAIL %s\n   trace: %s\n", name, g_trace); } \
    else          { printf("PASS %s\n", name); } \
    } while (0)

#define T0        1000000        /* an arbitrary session origin, in us */
#define AFTER(ms) ((int64_t)(T0 + (long)(ms) * 1000))
#define SETTLE    ORB_CAROUSEL_SETTLE_MS
#define READ      ORB_CAROUSEL_READ_MS
#define BGAP      ORB_CAROUSEL_BURST_GAP_MS

int main(void)
{
    /* Boot: the chip is built once, and hidden. */
    reset();
    CK("init builds the chip and reports ready",
       orb_carousel_init() == ESP_OK && orb_carousel_ready());
    reset(); orb_carousel_init();
    CK("init hides the chip, so no boot screen ever carries a name",
       has("hide") && !has("show"));

    /* At rest: the quiet direction, which is the dangerous one. */
    reset(); orb_carousel_init();
    CK("nothing is pending before the first turn", orb_carousel_pending() == NULL);
    CK("an idle poll can never invent a switch", !orb_carousel_due(AFTER(60000)));
    CK("commit with nothing pending does nothing at all",
       (orb_carousel_commit(), orb_carousel_pending() == NULL && g_resume == 0));

    /* The peek: name it, park the beat, do NOT switch. */
    reset(); orb_carousel_init(); TR();
    const orb_app_t *named = orb_carousel_step(1, &t_clock, AFTER(0));
    CK("one detent names the next app",
       named == &t_picker && orb_carousel_pending() == &t_picker);
    CK("the peek parks the beat, which is what keeps the chip on the glass",
       g_pause == 1);
    CK("the named title reaches the label", has("text(HSB)"));
    CK("a peek shows the chip", has("show"));
    CK("a lone detent is not due on the fast window — it is a stop to read",
       !orb_carousel_due(AFTER(SETTLE + 1)));
    CK("and it is due only when the read window closes",
       !orb_carousel_due(AFTER(READ - 1)) && orb_carousel_due(AFTER(READ)));

    /* The burst: three detents, three DIFFERENT names. The walk has to continue
     * from the entry already named; restarting each call from the glass would
     * name the same neighbour three times and a flick could never travel past
     * one screen — invisible on a board, because the chip just stops changing. */
    reset(); orb_carousel_init(); TR();
    orb_carousel_step(1, &t_clock, AFTER(0));
    const orb_app_t *b = orb_carousel_step(1, &t_clock, AFTER(60));
    const orb_app_t *c = orb_carousel_step(1, &t_clock, AFTER(120));
    CK("a burst walks from the entry already named, not from the glass",
       b == &t_menu && c == &t_clock);
    CK("every stop that is not the one already showing is named on the chip",
       has("text(HSB)") && has("text(Menu)"));
    /* The third detent landed back on the clock, so it cancelled rather than
     * painting the dial's own name over the dial. Asserting text(Clock) here
     * would have demanded the bug. */
    CK("the detent that comes back home names nothing",
       !has("text(Clock)") && orb_carousel_pending() == NULL);
    CK("the whole burst parks the beat exactly once", g_pause == 1);
    CK("one burst asks the registry for one step per detent",
       g_neighbour_calls == 3);

    /* The burst regime: a further detent inside the burst gap is the same
     * gesture, and its deadline is the fast window counted from ITSELF. */
    reset(); orb_carousel_init();
    const int64_t last = AFTER(BGAP - 1);           /* in-burst: one gesture */
    orb_carousel_step(1, &t_clock, AFTER(0));
    orb_carousel_step(1, &t_clock, last);
    CK("a detent inside the burst gap re-arms the fast window from itself",
       !orb_carousel_due(last + (int64_t)SETTLE * 1000 - 1) &&
       orb_carousel_due(last + (int64_t)SETTLE * 1000 + 1));
    /* The read regime: a detent landing at/after the burst gap is a pause, and
     * the entry it names must survive the reader's whole decision. This is
     * the case the single 400 ms window failed on hardware: 11 of 11 hand gaps
     * measured 510-1936 ms and every one committed underneath the reader
     * (§12.9). The walk must continue from the named entry, not restart. */
    reset(); orb_carousel_init();
    orb_carousel_step(1, &t_clock, AFTER(0));
    const orb_app_t *after_pause = orb_carousel_step(1, &t_clock, AFTER(1000));
    CK("a detent after a pause continues the walk rather than restarting it",
       after_pause == &t_menu && orb_carousel_pending() == &t_menu);
    CK("it is not due while the reader is deciding",
       !orb_carousel_due(AFTER(1000 + READ - 1)));
    CK("and it settles once the read window closes",
       orb_carousel_due(AFTER(1000 + READ)));
    /* Boundary pin: the classifier is strictly-under the burst gap. */
    reset(); orb_carousel_init();
    orb_carousel_step(1, &t_clock, AFTER(0));
    orb_carousel_step(1, &t_clock, AFTER(BGAP));
    CK("at exactly the burst gap it is already a pause: the fast window cannot apply",
       !orb_carousel_due(AFTER(BGAP + SETTLE + 1)));
    /* A detent right after a cancel is a new gesture: the window the cancelled
     * peek was holding must not leak into the next flick's classification. */
    reset(); orb_carousel_init();
    orb_carousel_step(1, &t_clock, AFTER(0));
    orb_carousel_cancel();
    orb_carousel_step(1, &t_clock, AFTER(60));
    CK("the first detent of a new gesture gets the read window, not the burst one",
       !orb_carousel_due(AFTER(60 + SETTLE + 1)) &&
       orb_carousel_due(AFTER(60 + READ)));
    /* Park handover. commit() must release its CLAIM without resuming the beat:
     * the activation the caller runs next starts it against the incoming view,
     * and resuming here would put a frame of the outgoing view on the glass
     * between two halves of one switch. */
    reset(); orb_carousel_init();
    orb_carousel_step(1, &t_clock, AFTER(0));
    orb_carousel_commit();
    CK("commit puts the chip away and clears the pending entry",
       g_hidden >= 1 && orb_carousel_pending() == NULL);
    CK("commit does NOT resume the beat — the activation owns it", g_resume == 0);
    orb_carousel_step(1, &t_picker, AFTER(SETTLE + 100));
    CK("the next peek parks again, so the handover did not leak the claim",
       g_pause == 2);

    /* The other exit: cancel, which does take the beat back. */
    reset(); orb_carousel_init();
    orb_carousel_step(1, &t_clock, AFTER(0));
    orb_carousel_cancel();
    CK("cancel puts the chip away and takes the beat back",
       orb_carousel_pending() == NULL && g_resume == 1);

    /* Park honesty on a view that never had a beat. */
    reset(); orb_carousel_init();
    g_running = 0;                          /* the picker: nothing ticking */
    orb_carousel_step(1, &t_picker, AFTER(0));
    CK("a peek from a view with no beat parks nothing", g_pause == 0);
    orb_carousel_cancel();
    CK("and cancel therefore resumes nothing, stealing nobody's beat",
       g_resume == 0);

    /* Two ways to end up naming what is already showing: a one-app table, and a
     * burst that wraps the whole way round. Both must leave nothing pending, or
     * the quiet poll commits a switch nobody asked for onto the screen it is on. */
    reset(); orb_carousel_init();
    g_ring_n = 1;
    const orb_app_t *solo = orb_carousel_step(1, &t_clock, AFTER(0));
    CK("a table of one returns the entry already on the glass", solo == &t_clock);
    CK("and leaves nothing pending, so the quiet poll switches nothing",
       orb_carousel_pending() == NULL && !orb_carousel_due(AFTER(5000)));
    CK("and never even shows the chip", g_shown == 0 && g_pause == 0);

    reset(); orb_carousel_init();
    for (int i = 0; i < 3; i++) orb_carousel_step(1, &t_clock, AFTER(i * 40));
    CK("a burst that wraps the whole table ends at home, quietly",
       orb_carousel_pending() == NULL && !orb_carousel_due(AFTER(5000)));

    /* No chip: the router's immediate-switch fallback depends on refusal. */
    reset(); orb_carousel_init();
    s_chip = NULL;
    CK("with no chip, step() refuses so the router can switch immediately",
       orb_carousel_step(1, &t_clock, AFTER(0)) == NULL && !orb_carousel_ready());
    CK("a NULL view is refused rather than walked",
       (s_chip = (lv_obj_t *)&chip_tok, orb_carousel_step(1, NULL, AFTER(0)) == NULL));
    reset();
    CK("init without a top layer fails rather than half-building a chip",
       (g_top = NULL, orb_carousel_init() != ESP_OK));

    /* Identity falls back to the slug, never to silence — which is all ruling
     * C5 actually asks of a text-only carousel. A P4 app off a card can ship an
     * empty title; the walk must still say who it is. */
    reset(); orb_carousel_init();
    t_menu.title = NULL; TR();
    orb_carousel_step(1, &t_picker, AFTER(0));
    CK("an app with no title names itself by slug", has("text(menu)"));
    t_menu.title = "Menu";

    /* The numbers, ratified by the owner's hand rather than by arithmetic
     * (§12.9). The bounds are assertions so a future edit cannot drift the
     * two regimes back onto each other: the fast window must clear the
     * encoder's own recovery and stay short enough not to feel dropped; the
     * burst gap must sit strictly between the widest flick the hand produced
     * (157 ms) and the shortest pause it produced (510 ms); the read window
     * must cover the longest pause observed (1936 ms) and still end. */
    CK("the fast window clears the encoder's 30 ms recovery window with room",
       SETTLE >= 4 * 30);
    CK("the fast window is short enough not to feel dropped", SETTLE <= 800);
    CK("the burst gap separates the two hand regimes",
       BGAP > 157 && BGAP < 510);
    CK("the read window covers the longest pause the hand took", READ >= 1936);
    CK("the read window still ends — nobody reads forever", READ <= 2500);
    CK("the shipped numbers are the ones §12.9 records",
       SETTLE == 400 && READ == 2000 && BGAP == 300);
    printf("%s\n", g_fail ? "SOME FAILED" : "ALL PASS");
    return g_fail ? 1 : 0;
}
'''



def build_and_run(tmpdir):
    """Compile the lifted carousel against the shipped orb_app.h and run it."""
    src = STUBS.replace("__BODY__", BODY)
    cpath = os.path.join(tmpdir, "carousel_test.c")
    with open(cpath, "w") as f:
        f.write(src)
    cmd = ["cc", "-std=gnu17", "-g", "-O0", "-Wall", "-Wno-unused-parameter",
           "-I", tmpdir,                # the leaf stubs shadow the IDF and LVGL ones
           "-I", P("main", "app"),
           "-I", P("main", "gfx"),
           cpath, "-o", os.path.join(tmpdir, "carousel_test")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None, "COMPILE FAILED\n" + r.stdout + r.stderr
    run = subprocess.run([os.path.join(tmpdir, "carousel_test")],
                         capture_output=True, text=True, timeout=30)
    return run.returncode, run.stdout + run.stderr


def main():
    with tempfile.TemporaryDirectory() as td:
        # The stub headers, written first so they shadow the real ones for the
        # types a host does not have — and NOT for orb_app.h, which is included
        # for real so the struct the carousel reads is the struct that ships.
        hdrs = {
            "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n"
                         "#define ESP_OK 0\n#define ESP_FAIL -1\n"
                         "#define ESP_ERR_NO_MEM 0x101\n"
                         "const char *esp_err_to_name(esp_err_t);\n",
            "esp_log.h": "#pragma once\n#define ESP_LOGI(t,...)\n#define ESP_LOGE(t,...)\n"
                         "#define ESP_LOGW(t,...)\n#define ESP_LOGD(t,...)\n",
            "lvgl.h": "#pragma once\n#include <stdbool.h>\n#include <stdint.h>\n"
                      "typedef struct _lv_obj_t lv_obj_t;\n"
                      "typedef struct { uint8_t r,g,b; } lv_color_t;\n"
                      "typedef struct { int dummy; } lv_font_t;\n"
                      "typedef unsigned int lv_obj_flag_t;\n"
                      "typedef enum { LV_LABEL_LONG_MODE_WRAP = 0,"
                      " LV_LABEL_LONG_MODE_DOTS } lv_label_long_mode_t;\n"
                      "#define LV_OBJ_FLAG_HIDDEN    (1u<<4)\n"
                      "#define LV_OBJ_FLAG_CLICKABLE (1u<<1)\n"
                      "#define LV_OBJ_FLAG_FLOATING  (1u<<18)\n"
                      "#define LV_ALIGN_BOTTOM_MID 3\n#define LV_OPA_COVER 255\n"
                      "#define LV_SCROLLBAR_MODE_OFF 1\n"
                      "extern lv_font_t lv_font_montserrat_48;\n"
                      "lv_obj_t *lv_layer_top(void);\nlv_obj_t *lv_obj_create(lv_obj_t*);\n"
                      "lv_obj_t *lv_label_create(lv_obj_t*);\n"
                      "void lv_label_set_text(lv_obj_t*, const char*);\n"
                      "void lv_obj_add_flag(lv_obj_t*, lv_obj_flag_t);\n"
                      "void lv_obj_remove_flag(lv_obj_t*, lv_obj_flag_t);\n"
                      "void lv_obj_move_foreground(lv_obj_t*);\n"
                      "void lv_obj_set_size(lv_obj_t*, int32_t, int32_t);\n"
                      "void lv_obj_set_width(lv_obj_t*, int32_t);\n"
                      "void lv_obj_align(lv_obj_t*, int, int32_t, int32_t);\n"
                      "void lv_obj_center(lv_obj_t*);\n"
                      "void lv_obj_set_scrollable(lv_obj_t*, bool);\n"
                      "void lv_obj_set_scrollbar_mode(lv_obj_t*, int);\n"
                      "void lv_obj_set_style_bg_color(lv_obj_t*, lv_color_t, int);\n"
                      "void lv_obj_set_style_bg_opa(lv_obj_t*, int, int);\n"
                      "void lv_obj_set_style_radius(lv_obj_t*, int, int);\n"
                      "void lv_obj_set_style_border_width(lv_obj_t*, int, int);\n"
                      "void lv_obj_set_style_pad_all(lv_obj_t*, int, int);\n"
                      "void lv_obj_set_style_text_color(lv_obj_t*, lv_color_t, int);\n"
                      "void lv_obj_set_style_text_font(lv_obj_t*, const lv_font_t*, int);\n"
                      "void lv_label_set_long_mode(lv_obj_t*, lv_label_long_mode_t);\n"
                      "lv_color_t lv_color_black(void);\nlv_color_t lv_color_white(void);\n",
            # Only the four gates the carousel is allowed to touch. The rest of
            # orb_sched.h is dial policy it must never reach for, and a stub
            # header that declared more than that would let it.
            "orb_sched.h": "#pragma once\n#include <stdbool.h>\n"
                           "int orb_sched_started(void);\nint orb_sched_running(void);\n"
                           "void orb_sched_pause(void);\nvoid orb_sched_resume(void);\n",
        }
        for n, body in hdrs.items():
            with open(os.path.join(td, n), "w") as f:
                f.write(body)
        rc, out = build_and_run(td)
        if rc is None:
            check("the lifted carousel compiles against the shipped orb_app.h",
                  False, out[:2000])
        else:
            lines = [l for l in out.strip().split("\n") if l.startswith(("PASS ", "FAIL "))]
            for l in lines:
                check(l[5:].split("\n")[0], l.startswith("PASS"))
            if not lines:
                check("harness compiled and ran", False, out[:2000])
            else:
                check("every case reported", rc == 0 and "ALL PASS" in out)
    print("all host checks pass" if not FAILS else "%d FAILED" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

