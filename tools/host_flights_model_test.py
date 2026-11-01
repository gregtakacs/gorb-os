#!/usr/bin/env python3
"""Host check for main/flights/flights_model.c, the flight tracker's model.

    python3 tools/host_flights_model_test.py             # all checks
    python3 tools/host_flights_model_test.py --mutations # do the checks actually bite?

The model is compiled as is (no LVGL, no IDF) with a C driver that checks each
behaviour the view and the feed lean on:

  merge     a poll upserts by hex; a contact the next poll misses stays, not
            live; past 60 s it drops; a full table replaces its stalest row
  reckon    position advances along track at ground speed, capped at 90 s;
            ground traffic and contacts without a track stay put
  project   range filter, nearest-first order, north up and east right
  select    by hex, so a re-sort does not move the highlight; cycles to none
  labels    outward side first, then the other side, above, below; refused
            off the glass and when every spot collides
  boost     emergency squawk, within 5 nm, low and closing; not low and
            opening, not far, not on the ground

The screenshots in docs/flight-tracker-design.md §6.5 show the view once; this
is what keeps it right after the next edit.
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "main", "flights", "flights_model.c")
INC = [os.path.join(ROOT, "main", "flights"), os.path.join(ROOT, "main", "net")]

DRIVER = r"""
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "flights_model.h"

static int fails, checks;
#define CHECK(cond, what) do { checks++; if (!(cond)) { fails++; printf("FAIL %s\n", what); } } while (0)
#define NEAR(a, b, tol) (fabsf((float)(a) - (float)(b)) <= (tol))

static orb_ac_t ac(const char *hex, float track, float gs, int alt, int ground)
{
    orb_ac_t a;
    memset(&a, 0, sizeof a);
    strcpy(a.hex, hex);
    a.track = track; a.gs = gs; a.baro_rate = NAN;
    a.alt_ft = alt; a.has_alt = 1; a.on_ground = ground; a.squawk = -1;
    return a;
}

static fm_t m;
#define S 1000000LL

int main(void)
{
    /* ---- merge ---- */
    orb_ac_t a1 = ac("aaa111", 0, 0, 5000, 0), a2 = ac("bbb222", 0, 0, 5000, 0);
    fm_merge_begin(&m); fm_upsert(&m, &a1, 10, 0, 0); fm_upsert(&m, &a2, 20, 90, 0); fm_merge_end(&m, 0);
    CHECK(m.n == 2, "merge: two contacts after the first poll");
    fm_merge_begin(&m); fm_upsert(&m, &a1, 11, 0, 10 * S); fm_merge_end(&m, 10 * S);
    CHECK(m.n == 2, "merge: a missed contact stays");
    int i1 = !strcmp(m.t[0].ac.hex, "aaa111") ? 0 : 1;
    CHECK(m.t[i1].live && !m.t[!i1].live, "merge: the missed one is not live, the carried one is");
    CHECK(NEAR(m.t[i1].n_km, 11, 1e-3), "merge: an upsert replaces the position, not a second row");
    fm_merge_begin(&m); fm_upsert(&m, &a1, 11, 0, 59 * S); fm_merge_end(&m, 59 * S);
    CHECK(m.n == 2, "merge: missing for 59 s is not yet stale");
    fm_merge_begin(&m); fm_upsert(&m, &a1, 11, 0, 61 * S); fm_merge_end(&m, 61 * S);
    CHECK(m.n == 1 && !strcmp(m.t[0].ac.hex, "aaa111"), "merge: missing for 61 s drops");
    memset(&m, 0, sizeof m);
    char hx[8];
    fm_merge_begin(&m);
    for (int i = 0; i < FM_MAX_TRK; i++) {
        snprintf(hx, sizeof hx, "h%05d", i);
        orb_ac_t a = ac(hx, 0, 0, 5000, 0);
        fm_upsert(&m, &a, 5, 0, (int64_t)(i + 1) * S);
    }
    orb_ac_t extra = ac("zzzzzz", 0, 0, 5000, 0);
    fm_upsert(&m, &extra, 5, 0, 100 * S);
    int has0 = 0, hasz = 0;
    for (int i = 0; i < m.n; i++) { has0 |= !strcmp(m.t[i].ac.hex, "h00000"); hasz |= !strcmp(m.t[i].ac.hex, "zzzzzz"); }
    CHECK(m.n == FM_MAX_TRK && hasz && !has0, "merge: a full table replaces its stalest contact");

    /* ---- reckon + project ---- */
    memset(&m, 0, sizeof m);
    orb_ac_t east = ac("e00001", 90, 360, 5000, 0);       /* 360 kt = 0.1852 km/s */
    orb_ac_t gnd  = ac("g00001", 90, 360, 0, 1);
    orb_ac_t notr = ac("n00001", NAN, 360, 5000, 0);
    fm_merge_begin(&m);
    fm_upsert(&m, &east, 10, 0, 0);                         /* 10 km north */
    fm_upsert(&m, &gnd, 10, 180, 0);                        /* 10 km south */
    fm_upsert(&m, &notr, 30, 270, 0);                       /* 30 km west  */
    fm_merge_end(&m, 0);
    fm_project(&m, 40, 10 * S, 233, 233, 184);
    const fm_trk_t *te = NULL, *tg = NULL, *tn = NULL;
    for (int i = 0; i < m.n; i++) {
        if (!strcmp(m.t[i].ac.hex, "e00001")) te = &m.t[i];
        if (!strcmp(m.t[i].ac.hex, "g00001")) tg = &m.t[i];
        if (!strcmp(m.t[i].ac.hex, "n00001")) tn = &m.t[i];
    }
    const float ex = 233 + 1.852f / 40 * 184, ey = 233 - 10.0f / 40 * 184;
    CHECK(te && NEAR(te->x, ex, 0.2f) && NEAR(te->y, ey, 0.2f), "reckon: 10 s east at 360 kt is 1.852 km east");
    CHECK(tg && NEAR(tg->x, 233, 0.01f) && NEAR(tg->y, 233 + 46, 0.01f), "reckon: ground traffic stays put (and south is down)");
    CHECK(tn && NEAR(tn->x, 233 - 30.0f / 40 * 184, 0.01f), "reckon: no track, no motion (and west is left)");
    fm_project(&m, 40, 1000 * S, 233, 233, 184);
    CHECK(te && NEAR(te->d_km, sqrtf(100 + (0.1852f * 90) * (0.1852f * 90)), 0.05f), "reckon: capped at 90 s");
    fm_project(&m, 20, 10 * S, 233, 233, 184);
    CHECK(m.norder == 2, "project: the 30 km contact is outside a 20 km range");
    CHECK(m.norder == 2 && m.t[m.order[0]].d_km <= m.t[m.order[1]].d_km, "project: nearest first");

    /* ---- select ---- */
    memset(&m, 0, sizeof m);
    orb_ac_t s1 = ac("s00001", NAN, NAN, 5000, 0), s2 = ac("s00002", NAN, NAN, 5000, 0);
    fm_merge_begin(&m); fm_upsert(&m, &s1, 5, 0, 0); fm_upsert(&m, &s2, 10, 0, 0); fm_merge_end(&m, 0);
    fm_project(&m, 40, 0, 233, 233, 184);
    char sel[8] = "";
    fm_sel_next(&m, sel, sel);
    CHECK(!strcmp(sel, "s00001"), "select: the first push takes the nearest");
    fm_merge_begin(&m); fm_upsert(&m, &s1, 20, 0, S); fm_upsert(&m, &s2, 10, 0, S); fm_merge_end(&m, S);
    fm_project(&m, 40, S, 233, 233, 184);
    CHECK(fm_find(&m, sel) == 1, "select: the highlight follows the hex through a re-sort");
    fm_sel_next(&m, sel, sel);
    CHECK(sel[0] == 0, "select: past the last one is none");

    /* ---- labels ---- */
    fm_rect_t placed[4], r;
    int np = 0;
    CHECK(fm_place_label(placed, np, 300, 233, 80, 16, 233, 233, 222, &r) && r.x1 == 308,
          "labels: a dot right of centre labels to its right");
    placed[np++] = r;
    CHECK(fm_place_label(placed, np, 300, 233, 80, 16, 233, 233, 222, &r) && r.x2 == 292,
          "labels: taken on the right, it goes left");
    placed[np++] = r;
    CHECK(fm_place_label(placed, np, 300, 233, 80, 16, 233, 233, 222, &r) && r.y2 == 223,
          "labels: both sides taken, it goes above");
    placed[np++] = r;
    CHECK(fm_place_label(placed, np, 300, 233, 80, 16, 233, 233, 222, &r) && r.y1 == 243,
          "labels: three taken, it goes below");
    placed[np++] = r;
    CHECK(!fm_place_label(placed, np, 300, 233, 80, 16, 233, 233, 222, &r),
          "labels: all four taken, no label");
    CHECK(fm_place_label(NULL, 0, 150, 233, 80, 16, 233, 233, 222, &r) && r.x2 == 142,
          "labels: a dot left of centre labels to its left");
    CHECK(!fm_place_label(NULL, 0, 233, 15, 200, 16, 233, 233, 222, &r),
          "labels: nothing that would leave the round glass");

    /* ---- boost ---- */
    orb_ac_t b = ac("b00001", 0, 200, 20000, 0);
    CHECK(fm_interesting(&b, 9.0f, 45), "boost: airborne within 5 nm");
    CHECK(!fm_interesting(&b, 9.5f, 45), "boost: not just past 5 nm at altitude");
    b.squawk = 7700;
    CHECK(fm_interesting(&b, 100, 45), "boost: an emergency squawk at any distance");
    b = ac("b00002", 180, 150, 2000, 0);                    /* north of home, heading south */
    CHECK(fm_interesting(&b, 15, 0), "boost: low and closing on home");
    b.track = 0;
    CHECK(!fm_interesting(&b, 15, 0), "boost: low and opening is not");
    b = ac("b00003", 0, 150, 2000, 0);                      /* south of home, heading north */
    CHECK(fm_interesting(&b, 15, 180), "boost: closing across the 360 wrap");
    b = ac("b00006", 200, 150, 2000, 0);                    /* NNW of home, heading SSW */
    CHECK(fm_interesting(&b, 15, 350), "boost: closing where the raw difference is 330 deg");
    b = ac("b00004", 180, 150, 2000, 0);
    CHECK(!fm_interesting(&b, 25, 0), "boost: low and closing but past 10 nm is not");
    b = ac("b00005", 0, 0, 0, 1);
    CHECK(!fm_interesting(&b, 1, 0), "boost: ground traffic is not");

    printf("END %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
"""


def build(src, out):
    d = tempfile.mkdtemp()
    drv = os.path.join(d, "drv.c")
    open(drv, "w").write(DRIVER)
    args = ["gcc", "-std=c11", "-O1", "-Wall", "-Werror", "-Wno-unused-function", "-D_GNU_SOURCE"]
    for i in INC:
        args += ["-I", i]
    r = subprocess.run(args + [drv, src, "-lm", "-o", out], capture_output=True, text=True)
    if r.returncode:
        sys.exit("build failed:\n" + r.stderr)


def run(binary):
    r = subprocess.run([binary], capture_output=True, text=True)
    return r.returncode, r.stdout


def main():
    b = os.path.join(tempfile.mkdtemp(), "fm")
    build(SRC, b)
    rc, out = run(b)
    print(out.strip())
    if rc:
        sys.exit(1)
    if "--mutations" in sys.argv:
        src = open(SRC).read()
        mutants = [
            ("missed contact stays live", "m->t[i].live = false;", "(void)0;"),
            ("staleness never drops", "> FM_STALE_US", "> FM_STALE_US * 1000"),
            ("full table drops the newest", "if (m->t[j].t_seen_us < m->t[k].t_seen_us) k = j;", "if (m->t[j].t_seen_us > m->t[k].t_seen_us) k = j;"),
            ("no reckoning cap", "if (dt > FM_DR_CAP_S) dt = FM_DR_CAP_S;", ""),
            ("ground traffic reckoned", "&& !t->ac.on_ground) {", ") {"),
            ("north is down", "t->y = cy - n / range_km * ring_r;", "t->y = cy + n / range_km * ring_r;"),
            ("order unsorted", "m->t[m->order[j]].d_km < m->t[m->order[j - 1]].d_km", "0"),
            ("select by index, not hex", "if (!strcmp(m->t[m->order[o]].ac.hex, hex)) return o;", "if (o == 0) return o;"),
            ("labels ignore collisions", "if (on_glass(&a, cx, cy, glass_r) && clear_of(&a, placed, np))", "if (on_glass(&a, cx, cy, glass_r))"),
            ("labels may leave the glass", "if (on_glass(&a, cx, cy, glass_r) && clear_of", "if (clear_of"),
            ("boost ignores emergencies", "a->squawk == 7700) return true;", "a->squawk == 7777) return true;"),
            ("boost ignores heading", "if (d < 60.0f) return true;", "return true;"),
            ("boost forgets the wrap", "if (d > 180.0f) d = 360.0f - d;", ""),
        ]
        survived = []
        for label, a, z in mutants:
            if a not in src:
                sys.exit("mutation anchor gone: " + label)
            p = os.path.join(tempfile.mkdtemp(), "flights_model.c")
            open(p, "w").write(src.replace(a, z, 1))
            mb = p[:-2]
            build(p, mb)
            if run(mb)[0] == 0:
                survived.append(label)
        if survived:
            sys.exit("mutants survived: " + ", ".join(survived))
        print("MUTATIONS %d/%d killed" % (len(mutants), len(mutants)))


main()
