#!/usr/bin/env python3
"""Host check for apps/flights/model.c, the flight tracker's model.

    python3 tools/host_flights_model_test.py             # all checks
    python3 tools/host_flights_model_test.py --mutations # do the checks actually bite?

The model is compiled as is (no LVGL, no IDF) with a C driver that checks each
behaviour the view and the feed lean on:

  merge     a poll upserts by hex; a contact the next poll misses stays, not
            live; past 60 s it drops; a full table replaces its stalest row
  reckon    position advances along track at ground speed, capped at 90 s;
            ground traffic and contacts without a track stay put
  project   range filter, nearest-first order, north up and east right
  select    by hex, so a re-sort does not move the highlight; wraps both ways
  boost     emergency squawk, within 5 nm, low and closing; not low and
            opening, not far, not on the ground
  route     airline callsigns only; adsbdb's answer parsed (IATA, else ICAO;
            unknown callsigns and null airports refused); shown only when the
            aircraft is plausibly on it (the bench's stale Envoy 3776 is not)

The screenshots in docs/flight-tracker-design.md §6.5 show the view once; this
is what keeps it right after the next edit.
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "apps", "flights", "model.c")
INC = [os.path.join(ROOT, "apps", "flights"), os.path.join(ROOT, "main", "app")]   # readsb.h takes orb_json.h (D51)

DRIVER = r"""
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "model.h"

/* D51: adsbdb's answer is read from the OS tokenizer's events; the vectors keep their
 * one-call shape through this harness-only helper over the host-compiled orb_json.c. */
static bool fm_route_parse(const char *json, fm_route_t *out)
{
    orb_json_t j;
    orb_json_begin(&j, json, strlen(json));
    fm_route_rd_t rd;
    fm_route_begin(&rd);
    orb_json_tok_t t;
    for (;;) {
        const int32_t k = orb_json_next(&j, &t);
        if (k == ORB_JSON_END || k == ORB_JSON_ERROR) break;
        fm_route_take(&rd, &t);
    }
    return fm_route_end(&rd, out);
}

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
    fm_sel_step(&m, sel, 1, sel);
    CHECK(!strcmp(sel, "s00001"), "select: a step forward from none takes the nearest");
    fm_merge_begin(&m); fm_upsert(&m, &s1, 20, 0, S); fm_upsert(&m, &s2, 10, 0, S); fm_merge_end(&m, S);
    fm_project(&m, 40, S, 233, 233, 184);
    CHECK(fm_find(&m, sel) == 1, "select: the highlight follows the hex through a re-sort");
    fm_sel_step(&m, sel, 1, sel);
    CHECK(!strcmp(sel, "s00002"), "select: past the farthest wraps to the nearest");
    fm_sel_step(&m, sel, -1, sel);
    CHECK(!strcmp(sel, "s00001"), "select: back from the nearest wraps to the farthest");
    fm_sel_step(&m, sel, -1, sel);
    CHECK(!strcmp(sel, "s00002"), "select: a step back walks toward the nearest");
    orb_ac_t s3 = ac("s00003", NAN, NAN, 5000, 0);
    fm_merge_begin(&m); fm_upsert(&m, &s1, 20, 0, S); fm_upsert(&m, &s2, 10, 0, S); fm_upsert(&m, &s3, 30, 0, S); fm_merge_end(&m, S);
    fm_project(&m, 40, S, 233, 233, 184);
    strcpy(sel, "s00001");
    fm_sel_step(&m, sel, -1, sel);
    CHECK(!strcmp(sel, "s00002"), "select: of three, back from the middle is the nearer one");
    strcpy(sel, "s00001");
    fm_sel_step(&m, sel, 1, sel);
    CHECK(!strcmp(sel, "s00003"), "select: of three, forward from the middle is the farther one");
    char none[8] = "";
    fm_sel_step(&m, none, -1, none);
    CHECK(!strcmp(none, "s00003"), "select: a step back from none takes the farthest");
    fm_t empty;
    memset(&empty, 0, sizeof empty);
    strcpy(none, "s00001");
    fm_sel_step(&empty, none, 1, none);
    CHECK(none[0] == 0, "select: nothing drawn, nothing selected");

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

    /* ---- route ---- */
    CHECK(fm_route_callsign_ok("AAL1393") && fm_route_callsign_ok("SWA12A") && fm_route_callsign_ok("CLX251"),
          "route: airline callsigns are worth asking about");
    CHECK(!fm_route_callsign_ok("N570AM") && !fm_route_callsign_ok("") && !fm_route_callsign_ok("AA1393")
          && !fm_route_callsign_ok("AAL") && !fm_route_callsign_ok("AALX93") && !fm_route_callsign_ok("aal1393")
          && !fm_route_callsign_ok("AAL12345"),
          "route: registrations, short, lower-case and over-long callsigns are not");
    /* adsbdb's answers, verbatim from the bench (2026-11-01) */
    static const char ENY3776[] =
        "{\"response\":{\"flightroute\":{\"callsign\":\"ENY3776\",\"callsign_icao\":\"ENY3776\",\"callsign_iata\":\"3776\","
        "\"airline\":{\"name\":\"Envoy Air\",\"icao\":\"ENY\",\"iata\":null,\"country\":\"United States\",\"country_iso\":\"US\",\"callsign\":\"Envoy\"},"
        "\"origin\":{\"country_iso_name\":\"US\",\"country_name\":\"United States\",\"elevation\":20,\"iata_code\":\"BOS\",\"icao_code\":\"KBOS\","
        "\"latitude\":42.3643,\"longitude\":-71.0052,\"municipality\":\"Boston\",\"name\":\"Logan International Airport\"},"
        "\"destination\":{\"country_iso_name\":\"US\",\"country_name\":\"United States\",\"elevation\":672,\"iata_code\":\"ORD\",\"icao_code\":\"KORD\","
        "\"latitude\":41.9786,\"longitude\":-87.9048,\"municipality\":\"Chicago\",\"name\":\"Chicago O'Hare International Airport\"}}}}";
    static const char AAL1393[] =
        "{\"response\":{\"flightroute\":{\"callsign\":\"AAL1393\",\"airline\":{\"name\":\"American Airlines\",\"icao\":\"AAL\",\"iata\":\"AA\"},"
        "\"origin\":{\"elevation\":36,\"iata_code\":\"PHL\",\"icao_code\":\"KPHL\",\"latitude\":39.8719,\"longitude\":-75.2411,\"municipality\":\"Philadelphia\"},"
        "\"destination\":{\"elevation\":19,\"iata_code\":\"PBI\",\"icao_code\":\"KPBI\",\"latitude\":26.6832,\"longitude\":-80.0956,\"municipality\":\"West Palm Beach\"}}}}";
    fm_route_t rt;
    CHECK(fm_route_parse(ENY3776, &rt) && !strcmp(rt.from, "BOS") && !strcmp(rt.to, "ORD")
          && NEAR(rt.from_lat, 42.3643f, 1e-4f) && NEAR(rt.to_lon, -87.9048f, 1e-4f),
          "route: the codes and ends come from origin and destination, not the airline");
    CHECK(!fm_route_parse("{\"response\":\"unknown callsign\"}", &rt), "route: an unknown callsign names none");
    static const char NOIATA[] =
        "{\"response\":{\"flightroute\":{\"origin\":{\"iata_code\":null,\"icao_code\":\"EGLL\",\"latitude\":51.47,\"longitude\":-0.4543},"
        "\"destination\":{\"iata_code\":\"JFK\",\"icao_code\":\"KJFK\",\"latitude\":40.6398,\"longitude\":-73.7789}}}}";
    CHECK(fm_route_parse(NOIATA, &rt) && !strcmp(rt.from, "EGLL") && !strcmp(rt.to, "JFK"),
          "route: an airport with no IATA code goes by its ICAO code");
    static const char NOPOS[] =
        "{\"response\":{\"flightroute\":{\"origin\":{\"iata_code\":\"BOS\",\"latitude\":42.36},"
        "\"destination\":{\"iata_code\":\"ORD\",\"latitude\":41.97,\"longitude\":-87.9}}}}";
    CHECK(!fm_route_parse(NOPOS, &rt), "route: an end with no position names no route");
    static const char ODD[] =
        "{\"response\":{\"flightroute\":{\"origin\":{\"iata_code\":\"B\\\"S\",\"latitude\":42.36,\"longitude\":-71.0},"
        "\"destination\":{\"iata_code\":\"ORD\",\"latitude\":41.97,\"longitude\":-87.9}}}}";
    CHECK(fm_route_parse(ODD, &rt) == false, "route: a code that is not letters and digits is refused, not drawn");

    fm_route_t eny, aal;
    fm_route_parse(ENY3776, &eny);
    fm_route_parse(AAL1393, &aal);
    CHECK(!fm_route_plausible(&eny, 32.89, -97.16, NAN, NAN), "route: Boston-Chicago is not a plane over Fort Worth");
    CHECK(fm_route_plausible(&aal, 33.3, -77.5, NAN, NAN), "route: a plane mid-route, on the line, is on it");
    CHECK(fm_route_plausible(&aal, 33.3, -79.0, NAN, NAN), "route: 140 km off the line still is");
    CHECK(!fm_route_plausible(&aal, 33.3, -73.0, NAN, NAN), "route: 400 km off the line is not");
    CHECK(fm_route_plausible(&aal, 39.9, -74.4, NAN, NAN), "route: climbing out near the origin is");
    CHECK(fm_route_plausible(&aal, 26.9, -80.6, NAN, NAN), "route: on approach near the destination is");
    CHECK(!fm_route_plausible(&aal, 22.0, -81.5, NAN, NAN), "route: well past the destination is not");
    CHECK(!fm_route_plausible(&aal, 43.0, -73.8, NAN, NAN), "route: well behind the origin is not");
    fm_route_t hop = { "DAL", "HOU", 32.847f, -96.852f, 29.645f, -95.279f };
    CHECK(fm_route_plausible(&hop, 30.9, -97.04, NAN, NAN), "route: a short hop, 100 km off its line, is (the 150 km floor)");

    /* Which way: the bench's AAL2336, "CLE > DFW" on a jet 25 km SE of DFW
     * heading east and climbing — the flight number flown the other way. */
    fm_route_t cle = { "CLE", "DFW", 41.4117f, -81.8498f, 32.8968f, -97.038f };
    CHECK(!fm_route_plausible(&cle, 32.77, -96.88, 87.0f, 2560.0f),
          "route: climbing away from the destination is the flight the other way");
    CHECK(fm_route_plausible(&cle, 32.77, -96.88, 300.0f, -1200.0f), "route: heading in and descending is arriving");
    CHECK(fm_route_plausible(&cle, 32.77, -96.88, 120.0f, -800.0f), "route: a downwind leg away from it, descending, is arriving");
    CHECK(fm_route_plausible(&cle, 32.77, -96.88, 300.0f, NAN), "route: heading in with no climb rate is arriving");
    CHECK(fm_route_plausible(&cle, 32.77, -96.88, NAN, NAN), "route: no track, the position alone decides");
    CHECK(fm_route_plausible(&aal, 39.9, -74.4, 160.0f, 1500.0f), "route: near the origin, heading out and climbing is leaving");
    CHECK(fm_route_plausible(&aal, 39.9, -74.4, 340.0f, 1500.0f), "route: near the origin, climbing through a turn is leaving");
    CHECK(!fm_route_plausible(&aal, 39.9, -74.4, 260.0f, -900.0f), "route: near the origin, heading in and descending is not leaving");
    CHECK(fm_route_plausible(&aal, 33.3, -77.5, 200.0f, NAN), "route: mid-route heading for the destination is on it");
    CHECK(!fm_route_plausible(&aal, 33.3, -77.5, 20.0f, NAN), "route: mid-route heading back the way it came is not");

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
    r = subprocess.run(args + [drv, src, os.path.join(ROOT, "main", "app", "orb_json.c"), "-lm",
                               "-o", out], capture_output=True, text=True)
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
            ("route asks about registrations", "if (cs[3] < '0' || cs[3] > '9') return false;", ""),
            ("route takes eight-character callsigns", "if (i >= 7 ||", "if (i >= 8 ||"),
            ("route codes come from the airline object", 'r->end = !strcmp(t->key, "origin") ? 1 : !strcmp(t->key, "destination") ? 2 : 0;',
             'r->end = !strcmp(t->key, "airline") ? 1 : !strcmp(t->key, "destination") ? 2 : 0;'),
            ("route never falls back to ICAO", "if (!r->iata_ok[e] && !r->icao_ok[e]) return false;", "if (!r->iata_ok[e]) return false;"),
            ("route takes any code text", "if (*c < '0' || (*c > '9' && *c < 'A') || *c > 'Z') return false;", ";"),
            ("route takes an end with no position", "if (!r->has_lat[e] || !r->has_lon[e]) return false;", ";"),
            ("route ignores the off-line distance", "if (fabsf(xt) * R > off) return false;", "if (fabsf(xt) * R > off * 100.0f) return false;"),
            ("route ignores which side of the origin", "    if (cosf(dth) <= 0.0f) return false;                                 /* behind the origin */\n", ""),
            ("route runs past the destination", "if (at > d12) return false;", "if (at > d12 * 10.0f) return false;"),
            ("route ignores the heading mid-route", "    return !dir || off_course(track, course(la3, lo3, la2, lo2)) < 90.0f;", "    return true || off_course(track, course(la3, lo3, la2, lo2)) < 90.0f;"),
            ("route ends ignore which way", "        return (at_from && leaving) || (at_to && arriving);", "        return (at_from || at_to) || leaving || arriving;"),
            ("route ignores the climb", "|| baro_rate > 0.0f;", ";"),
            ("route ignores the descent", "|| baro_rate < 0.0f;", ";"),
            ("route reads the track backwards", "off_course(track, course(la1, lo1, la3, lo3)) < 90.0f", "off_course(track, course(la3, lo3, la1, lo1)) < 90.0f"),
            ("route ends count from anywhere", "at_from = d13 * R < 100.0f, at_to = d23 * R < 100.0f;", "at_from = d13 * R < 2000.0f, at_to = d23 * R < 2000.0f;"),
            ("route short hops lose the floor", "fmaxf(150.0f, 0.15f * d12 * R)", "(0.15f * d12 * R)"),
            ("missed contact stays live", "m->t[i].live = false;", "(void)0;"),
            ("staleness never drops", "> FM_STALE_US", "> FM_STALE_US * 1000"),
            ("full table drops the newest", "if (m->t[j].t_seen_us < m->t[k].t_seen_us) k = j;", "if (m->t[j].t_seen_us > m->t[k].t_seen_us) k = j;"),
            ("no reckoning cap", "if (dt > FM_DR_CAP_S) dt = FM_DR_CAP_S;", ""),
            ("ground traffic reckoned", "&& !t->ac.on_ground) {", ") {"),
            ("north is down", "t->y = cy - n / range_km * ring_r;", "t->y = cy + n / range_km * ring_r;"),
            ("order unsorted", "m->t[m->order[j]].d_km < m->t[m->order[j - 1]].d_km", "0"),
            ("select by index, not hex", "if (!strcmp(m->t[m->order[o]].ac.hex, hex)) return o;", "if (o == 0) return o;"),
            ("select steps only forward", "(step < 0 ? -1 : 1)) % m->norder", "1) % m->norder"),
            ("select never wraps", "else        next = ((at + (step < 0 ? -1 : 1)) % m->norder + m->norder) % m->norder;",
             "else { next = at + (step < 0 ? -1 : 1); if (next < 0 || next >= m->norder) { hex_out[0] = 0; return; } }"),
            ("boost ignores emergencies", "a->squawk == 7700) return true;", "a->squawk == 7777) return true;"),
            ("boost ignores heading", "if (d < 60.0f) return true;", "return true;"),
            ("boost forgets the wrap", "if (d > 180.0f) d = 360.0f - d;", ""),
        ]
        survived = []
        for label, a, z in mutants:
            if a not in src:
                sys.exit("mutation anchor gone: " + label)
            p = os.path.join(tempfile.mkdtemp(), "model.c")
            open(p, "w").write(src.replace(a, z, 1))
            mb = p[:-2]
            build(p, mb)
            if run(mb)[0] == 0:
                survived.append(label)
        if survived:
            sys.exit("mutants survived: " + ", ".join(survived))
        print("MUTATIONS %d/%d killed" % (len(mutants), len(mutants)))


main()
