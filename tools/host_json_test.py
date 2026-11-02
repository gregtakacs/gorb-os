#!/usr/bin/env python3
"""Host check for main/app/orb_json.c, the OS's streaming JSON tokenizer (D46).

    python3 tools/host_json_test.py              # vectors
    python3 tools/host_json_test.py --mutations  # do the vectors actually bite?

The shipped file is compiled as is with host gcc behind a small driver that prints one
line per event. Every valid document's event stream is checked against the stream
Python's own json module implies for it (same order, keys, depths, values), so the
tokenizer is measured against a reference rather than against hand-typed answers.
Malformed documents must answer an error, and keep answering it.
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, os.pardir))
SRC = os.path.join(ROOT, "main", "app", "orb_json.c")
INC = os.path.join(ROOT, "main", "app")

DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
#include "orb_json.h"
static void hex(const char *s) { if (!*s) putchar('-'); for (; *s; s++) printf("%02x", (unsigned char)*s); }
int main(int argc, char **argv)
{
    (void)argc;
    FILE *f = fopen(argv[1], "rb");
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    orb_json_t j;
    orb_json_begin(&j, buf, n);
    orb_json_tok_t t;
    for (int guard = 0; guard < 100000; guard++) {
        int r = orb_json_next(&j, &t);
        if (r == ORB_JSON_END) { printf("END\n"); return 0; }
        if (r == ORB_JSON_ERROR) {
            int again = orb_json_next(&j, &t);
            printf("ERROR %d\n", again);
            return 0;
        }
        printf("%d %d ", r, t.depth);
        hex(t.key); putchar(' '); hex(t.str);
        printf(" %.17g %d%d\n", r == ORB_JSON_NUM ? t.num : 0.0, t.key_trunc, t.str_trunc);
    }
    printf("LOOP\n");
    return 0;
}
'''

OBJ, OBJ_END, ARR, ARR_END, STR, NUM, TRUE, FALSE, NULL = range(1, 10)
KEY_MAX, STR_MAX = 31, 95


def cut(s, n):
    b = s.encode()
    if len(b) <= n:
        return s, 0
    # The tokenizer cuts whole escape-produced units; a cut never splits a byte it
    # copied, so compare on the byte prefix it can have kept.
    return b[:n].decode("utf-8", "ignore"), 1


def expect(doc):
    out = []

    def walk(v, key, depth):
        k, kt = cut(key, KEY_MAX)
        if isinstance(v, dict):
            out.append((OBJ, depth, k, "", 0.0, kt, 0))
            for kk, vv in v.items():
                walk(vv, kk, depth + 1)
            out.append((OBJ_END, depth, "", "", 0.0, 0, 0))
        elif isinstance(v, list):
            out.append((ARR, depth, k, "", 0.0, kt, 0))
            for vv in v:
                walk(vv, "", depth + 1)
            out.append((ARR_END, depth, "", "", 0.0, 0, 0))
        elif isinstance(v, bool):
            out.append((TRUE if v else FALSE, depth, k, "", 0.0, kt, 0))
        elif v is None:
            out.append((NULL, depth, k, "", 0.0, kt, 0))
        elif isinstance(v, str):
            s, st = cut(v, STR_MAX)
            out.append((STR, depth, k, s, 0.0, kt, st))
        else:
            out.append((NUM, depth, k, "", float(v), kt, 0))

    walk(json.loads(doc), "", 0)
    return out


def parse_out(text):
    def unhex(h):
        return "" if h == "-" else bytes.fromhex(h).decode("utf-8", "replace")
    ev = []
    for line in text.splitlines():
        if line == "END":
            return ev, "END"
        if line.startswith("ERROR") or line == "LOOP":
            return ev, line
        typ, depth, key, sval, num, tr = line.split(" ")
        ev.append((int(typ), int(depth), unhex(key), unhex(sval), float(num), int(tr[0]), int(tr[1])))
    return ev, "EOF"


VALID = [
    '{}', '[]', '{"a":1}', '[1,2,3]', '"bare"', '42', 'true', 'null',
    '{"a":{"b":{"c":[1,{"d":null}]}}, "e":[], "f":{}}',
    '  {\n\t"sp" :  [ true , false , null ] \r\n}  ',
    '{"esc":"q\\"b\\\\s\\/n\\nt\\tu\\u00e9\\u20ac"}',
    '{"num":[-0, 0.5, -12.25e3, 1E-2, 123456789012]}',
    '{"k%s":"%s"}' % ("x" * 40, "y" * 120),
    '{"ac":[{"hex":"a1b2c3","flight":"AAL1270 ","lat":32.9,"lon":-97.1,'
    '"alt_baro":"ground","gs":141.2,"squawk":"7700","dbFlags":1}],"now":1.7e9}',
]
INVALID = [
    '{', '[1,', '{"a"}', '{"a":}', '{"a" 1}', '[1 2]', '{,"a":1}', '[,1]',
    '{"a":1,}x', '"unterminated', '{"a":tru}', '[01x]', '{"a":"\x01"}',
    '{"a":"\\x"}', '[' * 20 + ']' * 20, '{"a":1 "b":2}',
]


def build(tmp, src_text):
    c = os.path.join(tmp, "orb_json.c")
    open(c, "w").write(src_text)
    d = os.path.join(tmp, "drv.c")
    open(d, "w").write(DRIVER)
    exe = os.path.join(tmp, "drv")
    r = subprocess.run(["gcc", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-Wall", "-Wextra", "-Werror", "-I", INC, "-o", exe, d, c],
                       capture_output=True, text=True)
    return exe if r.returncode == 0 else None


def run(exe, tmp, doc):
    f = os.path.join(tmp, "doc.json")
    open(f, "w", encoding="utf-8").write(doc)
    return subprocess.run([exe, f], capture_output=True, text=True, timeout=10).stdout


def vectors(exe, tmp):
    fails = []
    for doc in VALID:
        got, end = parse_out(run(exe, tmp, doc))
        want = expect(doc)
        same = end == "END" and len(got) == len(want) and all(
            g[:4] == w[:4] and abs(g[4] - w[4]) <= 1e-9 * max(1.0, abs(w[4])) and g[5:] == w[5:]
            for g, w in zip(got, want))
        if not same:
            fails.append("valid %r:\n    got  %r %s\n    want %r" % (doc[:60], got[:6], end, want[:6]))
    for doc in INVALID:
        _, end = parse_out(run(exe, tmp, doc))
        if end != "ERROR -1":
            fails.append("malformed %r answered %s, not a sticky error" % (doc[:40], end))
    return fails


MUTATIONS = [
    ("missing-comma check dropped", "        } else if (!j->first[j->depth - 1]) {\n            return fail(j);",
     "        } else if (0) {\n            return fail(j);"),
    ("leading-comma check dropped", "if (j->first[j->depth - 1]) return fail(j);", ";"),
    ("control bytes accepted", "if (c < 0x20) return false;", ";"),
    ("depth limit gone", "if (j->depth >= ORB_JSON_DEPTH) return fail(j);", "if (0) return fail(j);"),
    ("escape \\n mistranslated", "case 'n': tmp[0] = '\\n'; break;", "case 'n': tmp[0] = 'n'; break;"),
    ("error not sticky", "    if (j->error) return ORB_JSON_ERROR;\n    if (j->done)",
     "    if (0) return ORB_JSON_ERROR;\n    if (j->done)"),
    ("truncation unflagged", "else *trunc = 1;", "else *trunc = 0;"),
    ("key colon not required", "if (j->p >= j->end || *j->p != ':') return fail(j);\n        j->p++;",
     "if (j->p < j->end && *j->p == ':') j->p++;"),
]


def main():
    src = open(SRC).read()
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, src)
        if not exe:
            print("FAIL the shipped tokenizer does not compile")
            return 1
        fails = vectors(exe, tmp)
        for f in fails:
            print("FAIL " + f)
        print("%d valid + %d malformed documents, %d failed" % (len(VALID), len(INVALID), len(fails)))
        if fails:
            return 1
        if "--mutations" in sys.argv:
            caught = 0
            for name, old, new in MUTATIONS:
                if old not in src:
                    print("FAIL mutation anchor lost: " + name)
                    return 1
                m = build(tmp, src.replace(old, new, 1))
                if m is None or vectors(m, tmp):
                    caught += 1
                else:
                    print("MISSED " + name)
            print("MUTATIONS %d/%d caught" % (caught, len(MUTATIONS)))
            return 0 if caught == len(MUTATIONS) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
