#!/usr/bin/env python3
"""Host bench for the art-residency arithmetic in main/art/art_bank.c.

docs/app-platform-plan.md section 12.5 flags this arithmetic as having NO host
suite: page rounding, the 234-page budget, reservation consumption, refusal
counting. It is exercised only by the M2 probe's boot log. art_bank.c cannot be
lifted and compiled the way idle_dim.c is -- the reserve/map path calls
esp_partition_mmap(), ESP_LOGE, and SOC_MMU_PAGE_SIZE -- so this is a mirror, as
host_app_registry_test.py and host_files_test.py mirror their sources.

What makes a mirror honest is the lint below: it reads the SHIPPING art_bank.c and
asserts every constant and guard the mirror depends on is still present verbatim.
If someone edits the budget, page size, alignment reject, or the refusal-counter
placement, the lint fails and forces a re-read. The mirror passes only while it
still matches the code.

Run:  python3 tools/host_art_residency_test.py              (rc 0 = pass)
      python3 tools/host_art_residency_test.py --mutations  (each must fail)
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "main", "art", "art_bank.c")
HDR = os.path.join(ROOT, "main", "art", "art_bank.h")

# The S3 default MMU page, and the DROM budget derived in art_bank.c's own comment
# (256 total, 22 spent by .text/.rodata). Cross-checked by the lint and by the real
# bank sizes below, whose page counts match the firmware's own art_pages report.
PAGE_SIZE = 64 * 1024
DROM_PAGES = 256
IMAGE_PAGES = 22
PAGE_BUDGET = DROM_PAGES - IMAGE_PAGES        # 234
RESERVE_MAX = 8                               # ORB_ART_RESERVE_MAX (ruling B5)
EXTRA_MAX = 8                                 # ART_BANK_EXTRA_MAX

FAILS = []
QUIET = [False]                 # the per-mutation replay silences the ok chatter
def ok(cond, label):
    if not QUIET[0]:
        print(("ok    " if cond else "FAIL  ") + label)
    if not cond:
        FAILS.append(label)


def lint_shipped_source():
    """Bind the mirror to the shipping code: every rule asserted here must still
    appear verbatim in main/art/art_bank.{c,h}, or the mirror describes dead code.
    The defines live in the header and the logic in the source, so both are read."""
    src = open(SRC).read() + open(HDR).read()
    must = [
        "#define ORB_ART_DROM_PAGES       256u",
        "#define ORB_ART_IMAGE_PAGES      22u",
        "#define ORB_ART_PAGE_BUDGET      (ORB_ART_DROM_PAGES - ORB_ART_IMAGE_PAGES)",
        "#define ORB_ART_RESERVE_MAX 8u",
        "#define ART_BANK_EXTRA_MAX 8",
        "return (bytes + ORB_ART_PAGE_SIZE - 1) / ORB_ART_PAGE_SIZE;",
        "return (bytes + SOC_MMU_PAGE_SIZE - 1) / SOC_MMU_PAGE_SIZE * SOC_MMU_PAGE_SIZE;",
        "if (bytes == 0) return ESP_ERR_INVALID_SIZE;",
        "if (bytes % ORB_ART_PAGE_ALIGN) return ESP_ERR_INVALID_SIZE;",
        "if (need_bytes == 0 || offset % ORB_ART_PAGE_ALIGN)",
        "if (want > ORB_ART_PAGE_BUDGET) {",
        "want -= art_bank_pages_for(s_extra[slot].bytes);",
        "if (s_extra[i].bytes >= need_bytes) {",
        "art_consume_reservation(art_round_pages(need_bytes));",
        "if (s_res_bytes[i] == bytes) {",
        "return (s_map != NULL ? 1u : 0u) + art_bank_extra_count() + art_reservation_count();",
    ]
    for tok in must:
        ok(tok in src, "lint: shipping art_bank.c contains %r" % tok[:52])
    # The refusal-counter placement is subtle: the two budget rejects and the
    # reservation-slot-full reject bump it; the secondary-slot-full map reject does
    # NOT. Pin the exact count so a wrong placement fails the lint, not the mirror.
    ok(src.count("s_reserve_refusals++;") == 3,
       "lint: exactly 3 s_reserve_refusals++ sites; got %d" % src.count("s_reserve_refusals++;"))
    ok("all %u reservation slots are booked" in src and "all %u secondary slots hold banks" in src,
       "lint: both exhaustion reasons present (reservation-full counts, secondary-full does not)")


# ---- the mirror: art_pages_accounted / orb_art_reserve / art_bank_map_extra ----
# Kept as source text so --mutations edits THIS and re-execs, giving the checks a
# real bite test rather than a tautology (host_files_test.py uses the same trick).
MODEL_SRC = r'''
def art_bank_pages_for(b):
    return (b + PAGE_SIZE - 1) // PAGE_SIZE

def art_round_pages(b):
    return (b + PAGE_SIZE - 1) // PAGE_SIZE * PAGE_SIZE

class ArtResidency:
    def __init__(self):
        self.primary = 0                 # s_bytes (the dial's own bank)
        self.extra = [None] * EXTRA_MAX  # each entry: (bytes, offset) once mapped
        self.res = [0] * RESERVE_MAX     # s_res_bytes (booked, not yet mapped)
        self.refusals = 0
        self.part_size = 0x800000        # 8 MB 'assets' partition

    def pages_accounted(self):
        # Each region rounds up on its own -- never a rounded sum of the bytes.
        pages = art_bank_pages_for(self.primary) if self.primary else 0
        for e in self.extra:
            if e is not None:
                pages += art_bank_pages_for(e[0])
        for r in self.res:
            if r:
                pages += art_bank_pages_for(r)
        return pages

    def mmu_pages(self):
        return self.pages_accounted()

    def resident_count(self):
        n = 1 if self.primary else 0
        n += sum(1 for e in self.extra if e is not None)
        n += sum(1 for r in self.res if r)
        return n

    def reserved_bytes(self):
        return sum(self.res)

    def _consume(self, rounded):
        for i, r in enumerate(self.res):
            if r == rounded:
                self.res[i] = 0
                return

    def reserve(self, nbytes):
        if nbytes == 0:
            return ERR_INVALID_SIZE
        if nbytes % PAGE_SIZE:
            return ERR_INVALID_SIZE
        want = self.pages_accounted() + art_bank_pages_for(nbytes)
        if want > PAGE_BUDGET:
            self.refusals += 1
            return ERR_NO_MEM
        free = next((i for i, r in enumerate(self.res) if r == 0), None)
        if free is None:
            self.refusals += 1
            return ERR_NO_MEM
        self.res[free] = nbytes
        return ESP_OK

    def map_extra(self, offset, need_bytes):
        if need_bytes == 0 or offset % PAGE_SIZE:
            return (ERR_INVALID_SIZE, -1)
        if offset + need_bytes > self.part_size:
            return (ERR_INVALID_SIZE, -1)
        slot = -1
        for i, e in enumerate(self.extra):
            if e is not None and e[1] == offset:
                if e[0] >= need_bytes:
                    return (ESP_OK, i)          # already covered: a no-op
                slot = i                        # re-grow in place
                break
        if slot < 0:
            slot = next((i for i, e in enumerate(self.extra) if e is None), -1)
        if slot < 0:
            return (ERR_NOT_FOUND, -1)          # secondary table full: NOT a refusal
        want = self.pages_accounted() + art_bank_pages_for(need_bytes)
        if self.extra[slot] is not None:
            want -= art_bank_pages_for(self.extra[slot][0])   # credit what it already pays
        if want > PAGE_BUDGET:
            self.refusals += 1
            return (ERR_NO_MEM, -1)
        self.extra[slot] = (need_bytes, offset)
        self._consume(art_round_pages(need_bytes))
        return (ESP_OK, slot)

    def unmap_extra(self, slot):
        if slot < 0 or slot >= EXTRA_MAX:
            return ERR_INVALID_SIZE
        self.extra[slot] = None
        return ESP_OK
'''


def model(mutations=()):
    text = MODEL_SRC
    for old, new in mutations:
        if text.count(old) < 1:
            raise AssertionError("mutation anchor missing: %r" % old)
        text = text.replace(old, new, 1)
    # The model reads these names from its exec globals: the mirror's constants and
    # the esp_err codes it returns, seeded rather than interpolated (a % -format would
    # collide with the `% PAGE_SIZE` modulo operators inside the model body).
    ns = dict(PAGE_SIZE=PAGE_SIZE, PAGE_BUDGET=PAGE_BUDGET,
              RESERVE_MAX=RESERVE_MAX, EXTRA_MAX=EXTRA_MAX,
              ESP_OK=0, ERR_INVALID_SIZE=1, ERR_NOT_FOUND=2, ERR_NO_MEM=3)
    exec(compile(text, "art_residency_mirror", "exec"), ns)
    return ns


def new_model(mutations=()):
    return model(mutations)["ArtResidency"]()


# ---- the checks, run against a new_model() instance (mutations may replace it) ----
def run_checks(mutations=()):
    E_OK, E_ISZ, E_NF, E_NM = 0, 1, 2, 3

    # 1. page rounding matches the real bank sizes the firmware reports as art_pages.
    p = model(mutations)["art_bank_pages_for"]
    ok(p(1978973) == 31, "pages_for(skeleton 1,978,973 B) == 31 (firmware art_pages)")
    ok(p(1855055) == 29, "pages_for(diver 1,855,055 B) == 29")
    ok(p(3858874) == 59, "pages_for(kit3 3,858,874 B) == 59 (M2's flagship)")
    ok(p(0) == 0 and p(64 * 1024) == 1, "pages_for: 0->0, one page->1")
    ok(p(64 * 1024 + 1) == 2, "pages_for rounds a ragged byte UP, not to")

    # 2. primary alone accounts for its own pages.
    m = new_model(mutations)
    m.primary = 31 * PAGE_SIZE
    ok(m.mmu_pages() == 31, "primary of 31 pages accounts as 31")

    # 3. a reservation is booked as whole pages and shows in reserved_bytes + total.
    ok(m.reserve(29 * PAGE_SIZE) == E_OK, "reserve a diver-sized bank")
    ok(m.reserved_bytes() == 29 * PAGE_SIZE, "reserve books the exact bytes")
    ok(m.mmu_pages() == 31 + 29, "a reservation bills its pages alongside the primary")

    # 4. size rejects are refused but NOT counted as budget refusals.
    before = m.refusals
    ok(m.reserve(1234) == E_ISZ, "reserve of a non-page-multiple -> INVALID_SIZE")
    ok(m.reserve(0) == E_ISZ, "reserve of 0 -> INVALID_SIZE")
    ok(m.refusals == before, "a size-reject is NOT a budget refusal")

    # 5. reserve-then-map bills its pages ONCE (the consume path).
    m2 = new_model(mutations)
    m2.primary = 31 * PAGE_SIZE
    m2.reserve(29 * PAGE_SIZE)
    e, slot = m2.map_extra(40 * PAGE_SIZE, 29 * PAGE_SIZE)
    ok(e == E_OK and slot >= 0, "map_extra of the booked size succeeds")
    ok(m2.reserved_bytes() == 0, "the map consumed the booking (no double bill)")
    ok(m2.mmu_pages() == 31 + 29, "reserve+map of one bank counts its pages once")

    # 6. the budget is on the RUNNING total, not the single request: two flagships
    # that each fit alone are exactly what overflows the window. Sizes stay inside
    # the model's 8 MB (128-page) partition footprint so the second one fails on the
    # budget, not on the partition-size gate -- each map is < 128 pages on its own.
    m3 = new_model(mutations)
    m3.primary = 100 * PAGE_SIZE
    flagship = 100 * PAGE_SIZE                    # 100 pages each, both < 128 footprint
    ok(m3.map_extra(2 * PAGE_SIZE, flagship)[0] == E_OK, "first flagship-sized map ok")
    ok(m3.map_extra(4 * PAGE_SIZE, flagship)[0] == E_NM,
       "second flagship-sized map refused: the running total overflows the window")
    ok(m3.refusals == 1, "the window-overflow counted exactly one refusal")

    # 7. re-growing a slot in place credits the pages it already pays. A filler bank
    # pushes the running total high enough that WITHOUT the credit the re-grow's raw
    # (accounted + new) sum would breach the 234 budget; WITH the credit (accounted
    # + new - old) it fits. Each bank's own footprint stays within the 128-page
    # partition gate, so this exercises the budget rule, not the size gate.
    m4 = new_model(mutations)
    m4.map_extra(40 * PAGE_SIZE, 15 * PAGE_SIZE)         # filler: 15 pages
    m4.map_extra(4 * PAGE_SIZE, 100 * PAGE_SIZE)         # target: 100 pages (footprint 104)
    e4, _ = m4.map_extra(4 * PAGE_SIZE, 120 * PAGE_SIZE)  # re-grow same offset to 120 (fp 124)
    ok(e4 == E_OK, "re-growing a slot in place credits what it already pays")
    ok(m4.mmu_pages() == 135, "after the credited grow the window bills filler+new = 135 pages")

    # A re-map at a size the entry already covers must be a NO-OP, not a re-grow:
    # re-grow consumes a matching booking and a no-op must not. That is the only
    # way the "already covered" branch is distinguishable from the re-grow branch
    # on host state -- both return ESP_OK and leave bytes/offset identical.
    m4b = new_model(mutations)
    m4b.map_extra(2 * PAGE_SIZE, 50 * PAGE_SIZE)          # existing 50 pages
    m4b.reserve(50 * PAGE_SIZE)                            # an identical booking held
    assert m4b.reserved_bytes() == 50 * PAGE_SIZE
    e5, _ = m4b.map_extra(2 * PAGE_SIZE, 50 * PAGE_SIZE)   # equal size -> covered no-op
    ok(e5 == E_OK and m4b.reserved_bytes() == 50 * PAGE_SIZE,
       "a covered re-map is a no-op: it must NOT consume the identical booking")

    # 8. the secondary-slot table full is NOT a budget refusal.
    m5 = new_model(mutations)
    m5.primary = 31 * PAGE_SIZE
    for i in range(EXTRA_MAX):
        assert m5.map_extra((100 + i) * PAGE_SIZE, PAGE_SIZE)[0] == E_OK, i
    before5 = m5.refusals
    efull, sfull = m5.map_extra((100 + EXTRA_MAX) * PAGE_SIZE, PAGE_SIZE)
    ok(efull == E_NF, "extra-slot table full -> NOT_FOUND")
    ok(m5.refusals == before5, "a full secondary table is NOT counted as a budget refusal")

    # 9. the reservation-slot table full IS a refusal (an installer bug still shows).
    m6 = new_model(mutations)
    for i in range(RESERVE_MAX):
        assert m6.reserve(PAGE_SIZE) == E_OK, i
    before6 = m6.refusals
    ok(m6.reserve(PAGE_SIZE) == E_NM, "reserve table full -> NO_MEM")
    ok(m6.refusals == before6 + 1, "a full reservation table IS counted as a refusal")

    # 10. unmap returns the pages to the window; a ragged offset is rejected.
    m7 = new_model(mutations)
    m7.primary = 31 * PAGE_SIZE
    e7, s7 = m7.map_extra(40 * PAGE_SIZE, 20 * PAGE_SIZE)
    ok(m7.mmu_pages() == 31 + 20, "a mapped extra is billed")
    ok(m7.unmap_extra(s7) == E_OK, "unmap_extra ok")
    ok(m7.mmu_pages() == 31, "unmap returns the extra's pages to the window")
    ok(m7.map_extra(PAGE_SIZE // 2, PAGE_SIZE)[0] == E_ISZ, "map_extra rejects a ragged offset")


MUTATIONS = [
    ("def art_bank_pages_for(b):\n    return (b + PAGE_SIZE - 1) // PAGE_SIZE",
     "def art_bank_pages_for(b):\n    return b // PAGE_SIZE"),              # truncates rounding
    ("pages += art_bank_pages_for(r)", "pages += 0"),                       # reservations unaccounted
    ("if want > PAGE_BUDGET:\n            self.refusals += 1\n            return (ERR_NO_MEM, -1)",
     "if want > PAGE_BUDGET:\n            return (ERR_NO_MEM, -1)"),        # map overflow not counted
    ("want -= art_bank_pages_for(self.extra[slot][0])", "want -= 0"),       # re-grow not credited
    ("if e[0] >= need_bytes:\n                    return (ESP_OK, i)",
     "if e[0] > need_bytes:\n                    return (ESP_OK, i)"),       # covered no-op lost
    ("if nbytes % PAGE_SIZE:\n            return ERR_INVALID_SIZE",
     "if False:\n            return ERR_INVALID_SIZE"),                     # ragged reserve allowed
    ("                self.res[i] = 0\n                return",
     "                pass\n                return"),                        # consume dropped -> double-bill
]


def run_mutations():
    print("\n-- mutations (each must break at least one check) --")
    QUIET[0] = True                       # silence the per-replay ok chatter
    bad = 0
    for old, new in MUTATIONS:
        if MODEL_SRC.count(old) != 1:
            print("FAIL  mutation anchor stale (%dx): %r" % (MODEL_SRC.count(old), old[:44]))
            bad += 1
            continue
        FAILS.clear()
        run_checks(mutations=[(old, new)])
        if FAILS:
            print("ok    mutation caught: %r -> %d check(s) failed" % (old[:44], len(FAILS)))
        else:
            print("FAIL  mutation SURVIVED: %r (a real regression would pass too)" % old[:44])
            bad += 1
    FAILS.clear()
    return bad


def main():
    if "--mutations" in sys.argv:
        run_checks()
        base = list(FAILS); FAILS.clear()
        if base:
            print("baseline checks failing, fix before mutating:", base)
            return 1
        return 1 if run_mutations() else 0
    lint_shipped_source()
    run_checks()
    print("\n== ART RESIDENCY SUITE:", "PASS" if not FAILS else "FAIL x%d" % len(FAILS))
    for f in FAILS:
        print("   -", f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())


