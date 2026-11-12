#!/usr/bin/env python3
"""Host bench for the settings schema the OS draws (main/app/orb_setui.c, app-platform-plan §57).

The writer (tools/orbapp.py normalize_settings + pack_schema) makes the region; the shipped
orb_setui.c, compiled here with host gcc beside orb_json.c, parses it and formats values. The
checks hold the two to each other:

- every row the writer wrote comes back with its id, type, label, show, range, step,
  decimals, unit and choices, in the manifest's order;
- a choice given per distance unit answers the unit asked for;
- values print as the dial shows them: a number's decimals, a unit's spacing, an enum's
  and a toggle's choice, a multi's list or its count when the list does not fit;
- what is not the writer's shape is refused: not an array, an unknown type, a row with
  no id, more rows or choices than the tables hold, an enum of one choice.

Run:  python3 tools/host_ui_test.py              (rc 0 = pass)
      python3 tools/host_ui_test.py --mutations  (proves the checks bite)
"""
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
APP = os.path.join(ROOT, "main", "app")
sys.path.insert(0, HERE)
import orbapp  # noqa: E402

ORB_SETUI_PALETTE = 12                       # orb_setui.h: the dial's twelve colours
HUB_TABS = os.path.join(ROOT, "tools", "orb-manager", "web", "tabs.js")
PALETTE_RE = re.compile(r'\["([A-Za-z]+)", 0x([0-9a-fA-F]{6})\]')


def hub_palette():
    """The hub's copy of the dial's palette, read out of the shipped tabs.js."""
    with open(HUB_TABS, encoding="utf-8") as f:
        src = f.read()
    m = re.search(r"const APS_PALETTE = (\[[\s\S]*?\]);", src)
    if not m:
        raise SystemExit("tabs.js carries no APS_PALETTE to mirror: the hub stopped naming the "
                         "dial's colours, or the table was renamed and this lint must move")
    return [(name, int(hexs, 16)) for name, hexs in PALETTE_RE.findall(m.group(1))]

SETTINGS = [
    {"id": "range", "type": "enum", "label": "Range",
     "choices": [{"km": "20 km", "mi": "10 mi", "nm": "10 nm"}, {"km": "40 km", "mi": "25 mi", "nm": "20 nm"}],
     "default": 1},
    {"id": "source", "type": "enum", "label": "Source", "choices": ["adsb.lol", "adsb.fi"], "default": 1},
    {"id": "vol", "type": "number", "label": "Volume", "decimals": 1, "min": -1, "max": 2.5, "step": 0.5,
     "default": 1.5, "unit": "x"},
    {"id": "hue", "type": "int", "label": "Hue", "min": 0, "max": 359, "default": 210, "step": 5, "unit": "°"},
    {"id": "bri", "type": "int", "label": "Brightness", "min": 0, "max": 100, "default": 90, "unit": "%"},
    {"id": "days", "type": "multi", "label": "Days", "choices": ["Mon", "Tue", "Wed", "Thu", "Fri"], "default": [0, 2]},
    {"id": "clock", "type": "toggle", "label": "Clock", "choices": ["hidden", "shown"], "default": 1},
    {"id": "force", "type": "int", "label": "Force", "min": -1, "max": 9, "default": -1, "show": "none"},
    {"id": "place1", "type": "place", "label": "Place 1", "help": "A place on the map."},
    {"id": "key", "type": "secret", "label": "API key", "host": "api.example.com",
     "links": [{"href": "https://example.com/key", "text": "Get one"}]},
    # §64.2: three colours, the shapes amendment 8 names — a colour default with specials,
    # an off-palette default the hub set, and a special as the default.
    {"id": "ring", "type": "color", "label": "Ring", "default": 0x0FA36B,
     "specials": ["From the look", "Match the app"]},
    {"id": "edge", "type": "color", "label": "Edge", "default": 0x123456},
    {"id": "look", "type": "color", "label": "Look", "default": -1, "specials": ["As themed"],
     "show": "hub"},
]

MUTATIONS = [
    ("r->show = !strcmp(t.str, \"both\") ? (ORB_SETUI_DIAL | ORB_SETUI_HUB)", "r->show = !strcmp(t.str, \"both\") ? ORB_SETUI_DIAL"),
    ("if (!strcmp(t.key, DIST[u]) && !(slot[u] = pool_put(ui, t.str))) return false;",
     "if (!strcmp(t.key, DIST[0]) && !(slot[0] = pool_put(ui, t.str))) return false;"),
    ("if (ui->n >= ORB_SETUI_MAX) return false;", "if (ui->n > ORB_SETUI_MAX) return false;"),
    ("if (!fits) snprintf(out, n, \"%d of %d\", on, (int)r->n_choices);", ";"),
    ("if (q->n_choices < 2) return false;", "if (q->n_choices < 1) return false;"),
    ("else if (!strcmp(t.key, \"step\")) r->step = v > 0 ? v : 1;", "else if (0) ;"),
    ("const char *sp = r->unit[0] && r->unit[0] != '%' &&", "const char *sp = r->unit[0] &&"),
    # §64.2: the colour row's four rules the checks above read back
    ('{ 0x0FA36B, "Emerald" }', '{ 0x0FA36B, "Teal" }'),
    ('snprintf(out, n, "%s", nm ? nm : "custom");', 'snprintf(out, n, "%s", nm ? nm : "");'),
    ("q->max = q->n_choices + ORB_SETUI_PALETTE;", "q->max = ORB_SETUI_PALETTE;"),
    ("return p >= 0 ? r->n_choices + p : r->max;", "return p >= 0 ? r->n_choices + p : r->min;"),
    ("if (pos < r->n_choices) return pos < 0 ? cur : -(pos + 1);",
     "if (pos <= r->n_choices) return pos < 0 ? cur : -(pos + 1);"),
    ("if (q->n_choices > ORB_SETUI_SPECIALS) return false;", "if (0) return false;"),
]

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_setui.h"
static char *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(*n + 1); *n = fread(b, 1, *n, f); fclose(f); b[*n] = 0; return b;
}
int main(int argc, char **argv)
{
    size_t n = 0; char *j = slurp(argv[1], &n);
    orb_setui_t *ui = malloc(orb_setui_bytes(n));
    if (!orb_setui_parse(j, n, ui)) { printf("REFUSED\n"); return 0; }
    printf("ROWS %d\n", ui->n);
    for (int i = 0; i < ui->n; i++) {
        const orb_setui_row_t *r = &ui->row[i];
        printf("ROW %s|%d|%s|%d|%ld|%ld|%ld|%d|%s|%d", r->id, r->type, r->label, r->show,
               (long)r->min, (long)r->max, (long)r->step, r->decimals, r->unit, r->n_choices);
        for (int c = 0; c < r->n_choices; c++)
            printf("|%s/%s/%s", orb_setui_choice(ui, r, c, 0), orb_setui_choice(ui, r, c, 1), orb_setui_choice(ui, r, c, 2));
        printf("\n");
    }
    for (int a = 2; a + 2 < argc; a += 3) {           /* row value dist */
        char out[40];
        orb_setui_value(ui, &ui->row[atoi(argv[a])], atoi(argv[a + 1]), atoi(argv[a + 2]), out, sizeof out);
        printf("VAL %s\n", out);
    }
    for (int i = 0; i < ui->n; i++) {                 /* §64.2: the colour row's walk */
        const orb_setui_row_t *r = &ui->row[i];
        if (r->type != ORB_SETUI_COLOR) continue;
        const int32_t probe[4] = { -1, 0, 0x0FA36B, 0x123456 };
        for (int p = 0; p < 4; p++) {
            const int pos = orb_setui_color_pos(r, probe[p]);
            printf("COLOR %s|%ld|%d|%ld\n", r->id, (long)probe[p], pos,
                   (long)(pos < 0 ? probe[p] : orb_setui_color_val(r, pos, probe[p])));
        }
        for (int pos = r->min; pos <= r->max; pos++) {
            const int32_t v = orb_setui_color_val(r, pos, 0x123456);
            char nm[32];
            orb_setui_value(ui, r, v, 0, nm, sizeof nm);
            printf("STEP %s|%d|%ld|%s|%d\n", r->id, pos, (long)v, nm,
                   orb_setui_color_pos(r, v) == pos);
        }
    }
    char small[12];
    for (int i = 0; i < ui->n; i++)
        if (ui->row[i].type == ORB_SETUI_MULTI) {
            orb_setui_value(ui, &ui->row[i], 31, 0, small, sizeof small);
            printf("SMALL %s\n", small);
        }
    return 0;
}
"""


def build(src_ui, d):
    open(os.path.join(d, "orb_setui.c"), "w").write(src_ui)
    open(os.path.join(d, "h.c"), "w").write(HARNESS)
    exe = os.path.join(d, "h")
    r = subprocess.run(["gcc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", APP, "-o", exe,
                        os.path.join(d, "orb_setui.c"), os.path.join(APP, "orb_json.c"), os.path.join(d, "h.c")],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("compile failed:\n" + r.stderr)
    return exe


def run(exe, d, schema, args=()):
    p = os.path.join(d, "s.json")
    open(p, "wb").write(schema if isinstance(schema, bytes) else schema.encode())
    return subprocess.run([exe, p] + [str(a) for a in args], capture_output=True, text=True).stdout


def checks(exe, d, quiet=False):
    fails = 0

    def ck(c, what):
        nonlocal fails
        if not quiet:
            print(("ok    " if c else "FAIL  ") + what)
        fails += not c

    _rows, sch = orbapp.normalize_settings(SETTINGS)
    region = orbapp.pack_schema(sch)
    out = run(exe, d, region, [0, 1, 1, 2, 25, 0, 1, 0, 0, 3, 210, 0, 4, 90, 0, 5, 5, 0, 6, 0, 0, 2, -5, 0, 5, 0, 0,
                               10, 0x0FA36B, 0, 10, 0x123456, 0, 10, -1, 0, 10, -2, 0, 10, -3, 0,
                               10, 0, 0, 11, 0x123456, 0, 11, 0, 0, 12, -1, 0])
    rows = [ln[4:].split("|") for ln in out.splitlines() if ln.startswith("ROW ")]
    vals = [ln[4:] for ln in out.splitlines() if ln.startswith("VAL ")]
    cols = [ln[6:].split("|") for ln in out.splitlines() if ln.startswith("COLOR ")]
    steps = [ln[5:].split("|") for ln in out.splitlines() if ln.startswith("STEP ")]
    small = [ln[6:] for ln in out.splitlines() if ln.startswith("SMALL ")]
    ck(len(rows) == len(SETTINGS) and [r[0] for r in rows] == [s["id"] for s in SETTINGS],
       "every setting comes back, in the manifest's order")
    want_types = [orbapp.UI_TYPES.index(s["type"]) for s in SETTINGS]
    ck([int(r[1]) for r in rows] == want_types and [r[2] for r in rows] == [s["label"] for s in SETTINGS],
       "each row's type and label are the writer's")
    shows = {r[0]: int(r[3]) for r in rows if len(r) > 3}
    ck(shows.get("range") == 3 and shows.get("force") == 0 and shows.get("key") == 2 and shows.get("place1") == 2,
       "show: both is dial and hub, none is neither, text kinds the hub's")
    vol = next((r for r in rows if r[0] == "vol"), [""] * 9)
    ck(vol[4:9] == ["-10", "25", "5", "1", "x"], "a number's stored range, step and decimals: %s" % vol[4:9])
    rng = next((r for r in rows if r[0] == "range"), [""] * 12)
    ck(len(rng) > 11 and rng[9] == "2" and rng[10] == "20 km/10 mi/10 nm" and rng[11] == "40 km/25 mi/20 nm",
       "a choice per distance unit answers each unit")
    days = next((r for r in rows if r[0] == "days"), [""] * 6)
    ck(days[4:6] == ["0", "31"], "a multi walks its bits: 0..31 for five choices")
    ck(vals == ["25 mi", "2.5 x", "adsb.lol", "210°", "90%", "Mon, Wed", "hidden", "-0.5 x", "none",
                "Emerald", "custom", "From the look", "Match the app", "custom", "custom",
                "custom", "custom", "As themed"],
       "values print as the dial shows them: %s" % vals)
    ck(shows.get("ring") == 3 and shows.get("look") == 2,
       "a colour shows on the dial and the hub by default; show:\"hub\" is the hub's alone")
    ck(small == ["5 of 5"], "a multi's list that does not fit is its count: %s" % small)

    # ---- §64.2: the colour row ------------------------------------------------
    ck([c[0] for c in cols] == ["ring"] * 4 + ["edge"] * 4 + ["look"] * 4,
       "every colour row reports its knob positions for the four probes: %s" % (cols[:1],))
    want_cols = [
        ["ring", "-1", "0", "-1"], ["ring", "0", "14", "0"],
        ["ring", "1024875", "6", "1024875"], ["ring", "1193046", "14", "1193046"],
        ["edge", "-1", "-1", "-1"], ["edge", "0", "12", "0"],
        ["edge", "1024875", "4", "1024875"], ["edge", "1193046", "12", "1193046"],
        ["look", "-1", "0", "-1"], ["look", "0", "13", "0"],
        ["look", "1024875", "5", "1024875"], ["look", "1193046", "13", "1193046"],
    ]
    ck(len(cols) == 12 and cols == want_cols,
       "a colour's stored value finds its knob position and back: a special it declared, "
       "black off the palette, Emerald on it, and a hub colour held as custom: %s" % (cols,))
    ck(all(s[-1] == "1" for s in steps) and len(steps) == 15 + 13 + 14,
       "every position the dial steps round-trips to itself, on all three rows (%d steps)" % len(steps))
    ring_steps = [s for s in steps if s[0] == "ring"]
    ck(len(ring_steps) == 15 and [s[3] for s in ring_steps[:2]] == ["From the look", "Match the app"],
       "the specials are offered first, by the name the app gave them: %s" % (ring_steps[:3],))
    ck(len(ring_steps) == 15 and ring_steps[-1][3] == "custom" and ring_steps[-1][1] == "14"
       and ring_steps[-1][2] == "1193046",
       "the last position is custom, and pressing it leaves the hub's colour alone: %s" % (ring_steps[-1:],))
    edge_pal = [(s[3], int(s[2])) for s in steps if s[0] == "edge" and int(s[1]) < ORB_SETUI_PALETTE]
    ck(len(edge_pal) == ORB_SETUI_PALETTE and edge_pal[0][0] == "Ruby" and edge_pal[-1][0] == "Pearl",
       "a colour with no specials steps the twelve palette colours in order: %s" % (edge_pal[:2],))
    hub = hub_palette()
    ck(hub == edge_pal, "the hub's copy of the palette names the dial's twelve, in its order: "
                        "%s vs %s" % (hub[:2], edge_pal[:2]))

    def refused(doc, what):
        ck(run(exe, d, doc).strip() == "REFUSED", "refused: " + what)
    refused('{"id":"x"}', "not an array")
    refused('[{"id":"x","type":"slider","label":"X"}]', "a type it does not know")
    refused('[{"type":"int","label":"X"}]', "a row with no id")
    refused(json.dumps([{"id": "s%d" % i, "type": "int", "label": "S"} for i in range(19)]), "nineteen rows")
    refused(json.dumps([{"id": "m", "type": "multi", "label": "M", "choices": ["c%d" % i for i in range(17)]}]),
            "seventeen choices")
    refused('[{"id":"e","type":"enum","label":"E","choices":["one"]}]', "an enum of one choice")
    refused('[{"id":"e","type":"enum","label":"E","choices":[{"km":"a","mi":"b"}]}]', "a unit choice missing nm")
    refused('[{"id":"c","type":"color","label":"C","specials":["a","b","c","d","e"],"default":0}]',
            "a colour with five specials")
    return fails


def main():
    src = open(os.path.join(APP, "orb_setui.c")).read()
    with tempfile.TemporaryDirectory() as d:
        if "--mutations" in sys.argv:
            caught = 0
            for a, b in MUTATIONS:
                if a not in src:
                    raise SystemExit("mutation %r does not apply -- rewrite the bench" % a)
                exe = build(src.replace(a, b, 1), d)
                bit = checks(exe, d, quiet=True) > 0
                print(("bites " if bit else "MISSED") + "  " + a[:70])
                caught += bit
            print("%d of %d mutants caught" % (caught, len(MUTATIONS)))
            return 0 if caught == len(MUTATIONS) else 1
        fails = checks(build(src, d), d)
        print("RESULT: pass" if not fails else "RESULT: %d failed" % fails)
        return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
