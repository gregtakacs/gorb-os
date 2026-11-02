#!/usr/bin/env python3
"""host_layout_carry_test.py — main/util/orb_layout_carry.c, tested on the host.

    python3 tools/host_layout_carry_test.py              # vectors
    python3 tools/host_layout_carry_test.py --mutations  # do the vectors bite?

The carry moves the owner's Wi-Fi credentials across the Layout B -> C table change,
and a wrong answer either strands a board off the LAN or resurrects credentials a
RESET ALL wiped. The shipped file is compiled as is against a 16 MB in-memory flash
with Layout C's `assets` and `storage` rows; the esp_partition_* leaves read and
write that array, and the NVS it must recognise is made by IDF's own
nvs_partition_gen.py, so "is this NVS" is measured against the real format rather
than against bytes this suite typed.
"""
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, os.pardir))
SRC = os.path.join(ROOT, "main", "util", "orb_layout_carry.c")
IDF = os.environ.get("IDF_PATH", os.path.expanduser("~/esp/esp-idf"))
GEN = os.path.join(IDF, "components", "nvs_flash", "nvs_partition_generator", "nvs_partition_gen.py")


def idf_python():
    """The IDF venv's interpreter: the generator imports its pip package from there."""
    import glob
    for p in sorted(glob.glob(os.path.expanduser("~/.espressif/python_env/*/bin/python"))):
        if subprocess.run([p, "-c", "import esp_idf_nvs_partition_gen"],
                          capture_output=True).returncode == 0:
            return p
    return sys.executable

OLD, NEW, ASSETS, ASSETS_SZ, SZ = 0xD60000, 0xF20000, 0x720000, 0x800000, 0x6000

STUBS = {
    "esp_heap_caps.h": "#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n"
                       "static inline void *heap_caps_malloc(size_t n, int c) { (void)c; return malloc(n); }\n"
                       "static inline void heap_caps_free(void *p) { free(p); }\n",
    "esp_log.h": "#include <stdio.h>\n#define ESP_LOGW(t, ...) (printf(\"W %s: \", t), printf(__VA_ARGS__), printf(\"\\n\"))\n"
                 "#define ESP_LOGE(t, ...) (printf(\"E %s: \", t), printf(__VA_ARGS__), printf(\"\\n\"))\n",
    "esp_rom_crc.h": "#include <stdint.h>\n#include <stddef.h>\n"
                     "static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *b, uint32_t n)\n"
                     "{ crc = ~crc; while (n--) { crc ^= *b++; for (int k = 0; k < 8; k++)\n"
                     "  crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u))); } return ~crc; }\n",
    "esp_partition.h": r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_CRC 0x109
static inline const char *esp_err_to_name(esp_err_t e) { return e ? "fail" : "ESP_OK"; }
typedef enum { ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
enum { ESP_PARTITION_SUBTYPE_DATA_NVS = 2, ESP_PARTITION_SUBTYPE_ANY = 0xff };
typedef struct { uint32_t address, size; const char *label; } esp_partition_t;
extern uint8_t g_flash[];
extern const esp_partition_t g_assets, g_storage;
extern int g_fail_write, g_erases;
static inline const esp_partition_t *esp_partition_find_first(int t, int st, const char *label)
{ (void)t; (void)st; return !strcmp(label, "storage") ? &g_storage : !strcmp(label, "assets") ? &g_assets : NULL; }
static inline esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *d, size_t n)
{ if (off + n > p->size) return ESP_FAIL; memcpy(d, g_flash + p->address + off, n); return ESP_OK; }
static inline esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t n)
{ if (off + n > p->size || off % 4096 || n % 4096) return ESP_FAIL; g_erases++;
  memset(g_flash + p->address + off, 0xFF, n); return ESP_OK; }
static inline esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *s, size_t n)
{ if (off + n > p->size) return ESP_FAIL; if (g_fail_write) return ESP_FAIL;
  const uint8_t *b = s; for (size_t i = 0; i < n; i++) g_flash[p->address + off + i] &= b[i]; return ESP_OK; }
''',
}

DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
#include "esp_partition.h"
#include "orb_layout_carry.h"
uint8_t g_flash[0x1000000];
const esp_partition_t g_assets  = { 0x720000, 0x800000, "assets" };
const esp_partition_t g_storage = { 0xF20000, 0x6000, "storage" };
int g_fail_write, g_erases;
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); if (fread(g_flash, 1, sizeof g_flash, f) != sizeof g_flash) return 2; fclose(f);
    g_fail_write = argc > 3 && argv[3][0] == '1';
    orb_layout_carry_storage();
    f = fopen(argv[2], "wb"); fwrite(g_flash, 1, sizeof g_flash, f); fclose(f);
    printf("ERASES %d\n", g_erases);
    return 0;
}
'''


def nvs_image(tmp, ssid):
    csv = os.path.join(tmp, "n.csv")
    out = os.path.join(tmp, "n.bin")
    open(csv, "w").write("key,type,encoding,value\norb.net,namespace,,\nssid,data,string,%s\n"
                         "pass,data,string,hunter2-%s\n" % (ssid, ssid))
    r = subprocess.run([idf_python(), GEN, "generate", csv, out, hex(SZ)],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("nvs_partition_gen failed:\n" + r.stdout + r.stderr)
    return open(out, "rb").read()


def build(tmp, src_text):
    inc = os.path.join(tmp, "inc")
    os.makedirs(inc, exist_ok=True)
    for name, body in STUBS.items():
        open(os.path.join(inc, name), "w").write(body)
    open(os.path.join(tmp, "orb_layout_carry.c"), "w").write(src_text)
    open(os.path.join(tmp, "drv.c"), "w").write(DRIVER)
    exe = os.path.join(tmp, "drv")
    r = subprocess.run(["gcc", "-O1", "-Wall", "-Wextra", "-Werror", "-I", inc,
                        "-I", os.path.dirname(SRC), "-o", exe,
                        os.path.join(tmp, "drv.c"), os.path.join(tmp, "orb_layout_carry.c")],
                       capture_output=True, text=True)
    return exe if r.returncode == 0 else None


def run(exe, tmp, flash, fail_write=False):
    a, b = os.path.join(tmp, "in.bin"), os.path.join(tmp, "out.bin")
    open(a, "wb").write(bytes(flash))
    subprocess.run([exe, a, b, "1" if fail_write else "0"], capture_output=True, text=True, timeout=30)
    return bytearray(open(b, "rb").read())


def vectors(exe, tmp, nvs_a, nvs_b):
    rnd = random.Random(7)
    junk = bytes(rnd.getrandbits(8) for _ in range(SZ))
    blank = b"\xff" * SZ
    fails = []

    def case(name, old, new, want_new, want_old, fail_write=False):
        fl = bytearray(b"\xff" * 0x1000000)
        fl[OLD:OLD + SZ] = old
        fl[NEW:NEW + SZ] = new
        out = run(exe, tmp, fl, fail_write)
        if bytes(out[NEW:NEW + SZ]) != want_new:
            fails.append(name + ": storage is not what it should be")
        if bytes(out[OLD:OLD + SZ]) != want_old:
            fails.append(name + ": the old copy is not what it should be")
        rest = out[:OLD] + out[OLD + SZ:NEW] + out[NEW + SZ:]
        if rest != fl[:OLD] + fl[OLD + SZ:NEW] + fl[NEW + SZ:]:
            fails.append(name + ": a byte outside the two regions moved")

    case("crossing board, old app slot bytes at the new offset", nvs_a, junk, nvs_a, blank)
    case("crossing board, erased flash at the new offset", nvs_a, blank, nvs_a, blank)
    case("carried by hand, stale old copy still standing", nvs_a, nvs_b, nvs_b, blank)
    case("already carried, old copy gone", blank, nvs_b, nvs_b, blank)
    case("theme pixels at the old offset are not NVS", junk, blank, blank, junk)
    case("fresh Layout C board", blank, blank, blank, blank)
    case("RESET ALL after the carry wiped storage: nothing comes back", blank, blank, blank, blank)
    bad = bytearray(nvs_a)
    bad[8] ^= 0x01                                  # one header byte under the CRC
    case("an old copy whose page header fails its CRC is not carried", bytes(bad), junk, junk, bytes(bad))
    case("a failed write keeps the old copy for the next boot", nvs_a, junk, blank, nvs_a, fail_write=True)
    odd = bytearray(nvs_a)
    odd[0x1000:0x1004] = (0x12345678).to_bytes(4, "little")   # a page state NVS never writes
    case("a page in a state NVS never writes is not NVS", bytes(odd), junk, junk, bytes(odd))
    dead = bytearray(nvs_a)
    dead[0x1000:0x2000] = junk[:0x1000]
    dead[0x1000:0x1004] = (0xFFFFFFF0).to_bytes(4, "little")  # CORRUPT: IDF's dead page
    case("a store with a dead page is still carried", bytes(dead), junk, bytes(dead), blank)
    return fails


MUTATIONS = [
    ("header CRC unchecked", "if (esp_rom_crc32_le(0xFFFFFFFFu, h + 4, 24) != rd32(h + 28)) return false;", ";"),
    ("CRC over the wrong span", "esp_rom_crc32_le(0xFFFFFFFFu, h + 4, 24)", "esp_rom_crc32_le(0xFFFFFFFFu, h, 28)"),
    ("unknown page states tolerated", "            return false;\n        if (esp_rom", "            continue;\n        if (esp_rom"),
    ("an all-blank region counts as NVS", "return live > 0;", "return true;"),
    ("the old copy outlives the carry", "if (e == ESP_OK) e = esp_partition_erase_range(src, from, STORAGE_BYTES);", ";"),
    ("the old copy goes before the new one is verified",
     "if (e == ESP_OK && !holds_nvs(dst, 0)) e = ESP_ERR_INVALID_CRC;",
     "esp_partition_erase_range(src, from, STORAGE_BYTES);"),
    ("a hand-carried board keeps its stale copy",
     "        const esp_err_t e = esp_partition_erase_range(src, from, STORAGE_BYTES);\n        ESP_LOGW",
     "        const esp_err_t e = ESP_OK;\n        ESP_LOGW"),
    ("storage already holding NVS is overwritten", "    if (holds_nvs(dst, 0)) {", "    if (0) {"),
]


def main():
    if not os.path.exists(GEN):
        print("FAIL no nvs_partition_gen.py at %s (set IDF_PATH)" % GEN)
        return 1
    src = open(SRC).read()
    with tempfile.TemporaryDirectory() as tmp:
        nvs_a, nvs_b = nvs_image(tmp, "home-a"), nvs_image(tmp, "home-b")
        exe = build(tmp, src)
        if not exe:
            print("FAIL the shipped carry does not compile on the host")
            return 1
        fails = vectors(exe, tmp, nvs_a, nvs_b)
        for f in fails:
            print("FAIL " + f)
        print("11 cases, %d failed" % len(fails))
        if fails:
            return 1
        if "--mutations" in sys.argv:
            caught = 0
            for name, old, new in MUTATIONS:
                if src.count(old) != 1:
                    print("FAIL mutation anchor lost: " + name)
                    return 1
                m = build(tmp, src.replace(old, new, 1))
                if m is None or vectors(m, tmp, nvs_a, nvs_b):
                    caught += 1
                else:
                    print("MISSED " + name)
            print("MUTATIONS %d/%d caught" % (caught, len(MUTATIONS)))
            return 0 if caught == len(MUTATIONS) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
