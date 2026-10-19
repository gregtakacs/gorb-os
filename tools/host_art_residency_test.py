#!/usr/bin/env python3
"""Host bench for the art-residency arithmetic in main/art/art_bank.c.

docs/app-platform-plan.md section 12.5 flags this arithmetic as having NO host
suite: page rounding, the 234-page budget, reservation consumption, refusal
counting. It is exercised only by the M2 probe's boot log. art_bank.c cannot be
lifted and compiled the way idle_dim.c is -- the reserve/map path calls
esp_partition_mmap(), ESP_LOGE, and SOC_MMU_PAGE_SIZE -- so this is a mirror, as
host_app_registry_test.py and host_files_test.py mirror their sources.

P6.3 (D30, §20.4/§20.5) added the third arm the mirror now carries: the primary
bank's arena refill (REFILL: checks). Mechanism B lit for shipping in that commit,
and the "#define ORB_ART_MECH_B" pin below moved with it -- §20.4's rule that the
light and the residency suite change in one commit, so a dark-by-default file
cannot come back quietly.

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
ARENA_MARGIN = 2 * 1024 * 1024                # ORB_ART_ARENA_MARGIN: PSRAM the arena gate keeps

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
        "return (art_bank_ready() ? 1u : 0u) + art_bank_extra_count() + art_reservation_count();",
        # ---- mechanism B (docs/app-platform-plan.md §3.3/§6 M3): the arena rules.
        # Same contract as above — if any of these stops matching the shipping
        # source, the mirror below describes dead code and must be re-read.
        # D30 lit the define (§20.4, P6.3): B is the runtime mechanism for an
        # installed theme's pixels. This pin moved in the same commit that flipped
        # the byte, which is the §20.4 rule written down so it repeats.
        "#define ORB_ART_MECH_B 1",          # lit for shipping: D30, §20.4
        # ---- D30's boot refill of the PRIMARY bank (§20.2's boot rule).
        "return s_map != NULL || s_psram;",                     # primary liveness: both kinds
        "return s_psram ? s_plane : s_map;",                    # one primary ptr, two sources
        "size_t pages = s_map ? art_bank_pages_for(s_bytes) : 0;",  # arena primary bills ZERO
        "if (free_p < need_bytes + ORB_ART_ARENA_MARGIN) {",   # the refill's own PSRAM gate
        "if (verify) crc = esp_rom_crc32_le(crc, arena + done, chunk);",  # fused, one pass
        "if (verify && crc != expect_crc) {",                  # corrupt slot never reaches paint
        "art_bank_primary_release();",                         # release is by mechanism
        "esp_err_t art_bank_map_at_0(size_t need_bytes) { return art_bank_map(0, need_bytes); }",
        "#define ORB_ART_M3_PROBE 0",        # ditto the probe that prices it
        "#define ORB_ART_ARENA_MARGIN (2u * 1024u * 1024u)",
        "return e->map != NULL || e->psram;",                       # liveness: both kinds
        "if (art_extra_live(&s_extra[i]) && !s_extra[i].psram)",    # arena bills ZERO pages
        "return s_extra[slot].psram ? s_extra[slot].plane : s_extra[slot].map;",  # one ptr, two sources
        "if (free_p < need_bytes + ORB_ART_ARENA_MARGIN)",          # the PSRAM gate
        "if (s_extra[slot].psram) heap_caps_free(s_extra[slot].plane);",  # arena release
        "art_bank_map_extra(M3_BANK_OFF, M3_BANK_BYTES, &slot)",    # the probe uses the REAL API
        # 2026-10-12 M3 incident: the first bench offset (0x100000) sat under the
        # baked bank's end (0x11E83B) and the bench write clobbered its tail.
        # Pin the offset to slot B's art and the gate that catches a stolen or
        # stale bench region — the old offset must never come back.
        "#define M3_BANK_OFF   0x00410000u",
        "#define M3_BANK_BYTES 3858874u",
        "!= 0x59801b57u",                                           # plane == the flashed kit3
    ]
    assert "#define M3_BANK_OFF   0x00100000u" not in src, \
        "lint: the retired M3 bench offset (inside the live bank) is back — §14's incident"
    for tok in must:
        ok(tok in src, "lint: shipping art_bank.c contains %r" % tok[:52])
    # The refusal-counter placement is subtle: the two budget rejects, the
    # reservation-slot-full reject, the TWO arena capacity gates on a secondary map
    # (free-PSRAM margin and the arena malloc itself) and the SAME two gates on
    # D30's primary refill bump it; the secondary-slot-full map reject does NOT.
    # Pin the exact count so a wrong placement fails the lint, not the mirror.
    ok(src.count("s_reserve_refusals++;") == 7,
       "lint: exactly 7 s_reserve_refusals++ sites; got %d" % src.count("s_reserve_refusals++;"))
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
    def __init__(self, mech_b=False, psram_free=16 * 1024 * 1024):
        self.primary = 0                 # s_bytes (the dial's own bank)
        self.primary_kind = "map"        # "map" = mechanism A, "arena" = D30's refill
        self.fill_ms = 0                 # the last fused pass, timed

        self.extra = [None] * EXTRA_MAX  # each entry: (bytes, offset, is_psram) once mapped
        self.res = [0] * RESERVE_MAX     # s_res_bytes (booked, not yet mapped)
        self.refusals = 0
        self.part_size = 0x800000        # 8 MB 'assets' partition
        self.mech_b = mech_b             # ORB_ART_MECH_B: map_extra fills arenas, not maps
        self.psram_free = psram_free     # heap_caps_get_free_size(MALLOC_CAP_SPIRAM)

    def pages_accounted(self):
        # Each region rounds up on its own -- never a rounded sum of the bytes.
        # An arena bank is skipped: mechanism B never occupies the MMU window,
        # so it bills it nothing (the mirror of art_extra_live(...) && !psram).
        # D30 made the same rule true of the PRIMARY bank: a refilled dial sits in
        # PSRAM and costs the window nothing, which is the mirror's line below.
        pages = (art_bank_pages_for(self.primary)
                 if self.primary and self.primary_kind == "map" else 0)

        for e in self.extra:
            if e is not None and not e[2]:
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
        if self.mech_b:
            # Mechanism B: no window check at all — an arena bank takes no
            # pages, so billing it against them would rig the A-vs-B measure.
            # Its gate is free PSRAM minus the margin, counted as a refusal.
            if self.psram_free < need_bytes + ARENA_MARGIN:
                self.refusals += 1
                return (ERR_NO_MEM, -1)
            self.extra[slot] = (need_bytes, offset, True)
            self._consume(art_round_pages(need_bytes))
            return (ESP_OK, slot)
        want = self.pages_accounted() + art_bank_pages_for(need_bytes)
        if self.extra[slot] is not None and not self.extra[slot][2]:
            want -= art_bank_pages_for(self.extra[slot][0])   # credit what it already pays
        if want > PAGE_BUDGET:
            self.refusals += 1
            return (ERR_NO_MEM, -1)
        self.extra[slot] = (need_bytes, offset, False)
        self._consume(art_round_pages(need_bytes))
        return (ESP_OK, slot)

    def unmap_extra(self, slot):
        if slot < 0 or slot >= EXTRA_MAX:
            return ERR_INVALID_SIZE
        self.extra[slot] = None
        return ESP_OK

    def primary_fill(self, offset, need_bytes, verify, expect_crc, actual_crc, ms=0):
        # art_bank_fill(): D30's boot refill of the dial's own bank. The gate order
        # is the C order -- zero size, free PSRAM, allocation, partition bounds,
        # then the one fused read+crc pass -- and every reject from the allocation
        # on frees the arena it just took (the C does that with heap_caps_free on
        # each early return; a leaked 1.17 MB at boot is a bug no check catches).
        # A checksum mismatch is NOT a capacity refusal, so it does not count.
        if need_bytes == 0:
            return ERR_INVALID_ARG
        if self.psram_free < need_bytes + ARENA_MARGIN:
            self.refusals += 1
            return ERR_NO_MEM
        if offset + need_bytes > self.part_size:
            return ERR_INVALID_SIZE
        if verify and actual_crc != expect_crc:
            return ERR_INVALID_CRC
        self.primary = need_bytes
        self.primary_kind = "arena"
        self.fill_ms = ms
        self._consume(art_round_pages(need_bytes))
        return ESP_OK

    def primary_map(self, offset, need_bytes):
        # art_bank_map(): the form the card-less default and Arm A always take.
        if need_bytes == 0:
            return ERR_INVALID_ARG
        self.primary = need_bytes
        self.primary_kind = "map"
        self._consume(art_round_pages(need_bytes))
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
              ARENA_MARGIN=ARENA_MARGIN,
              ESP_OK=0, ERR_INVALID_SIZE=1, ERR_NOT_FOUND=2, ERR_NO_MEM=3,
              ERR_INVALID_ARG=4, ERR_INVALID_CRC=5)
    exec(compile(text, "art_residency_mirror", "exec"), ns)
    return ns


def new_model(mutations=(), **kw):
    """**kw reaches the ArtResidency ctor (mech_b=, psram_free=) so the
    mechanism-B checks share the mutation pipeline with the rest."""
    return model(mutations)["ArtResidency"](**kw)


# ---- the checks, run against a new_model() instance (mutations may replace it) ----
def run_checks(mutations=()):
    E_OK, E_ISZ, E_NF, E_NM = 0, 1, 2, 3
    E_ARG, E_CRC = 4, 5

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

    # ---- mechanism B: the arena rules (§3.3/§6 M3), same mutation pipeline ----
    # 11. an arena bank bills ZERO MMU pages but is resident art.
    mB = new_model(mutations, mech_b=True)
    mB.primary = 31 * PAGE_SIZE
    eB, sB = mB.map_extra(16 * PAGE_SIZE, 59 * PAGE_SIZE)
    ok(eB == E_OK, "MECH_B: flagship-sized (59-page) arena map is granted")
    ok(mB.mmu_pages() == 31, "MECH_B: the arena bills the window NOTHING — only the primary counts")
    ok(mB.resident_count() == 2, "MECH_B: the arena bank still counts as resident art")

    # 12. the window can never refuse mechanism B: the exact map check 6 refused
    # under A is granted under B, and the refusal counter stays clean.
    mB2 = new_model(mutations, mech_b=True)
    mB2.primary = 100 * PAGE_SIZE
    ok(mB2.map_extra(2 * PAGE_SIZE, 100 * PAGE_SIZE)[0] == E_OK,
       "MECH_B: a map the A window refused for want of pages is GRANTED by B")
    ok(mB2.refusals == 0, "MECH_B: bypassing the window is not a refusal")

    # 13. the ONLY capacity gate is free PSRAM, and it counts as a refusal —
    # exactly one byte below the gate refuses, exactly on it passes.
    mB3 = new_model(mutations, mech_b=True, psram_free=59 * PAGE_SIZE + ARENA_MARGIN - 1)
    e3, s3 = mB3.map_extra(16 * PAGE_SIZE, 59 * PAGE_SIZE)
    ok(e3 == E_NM and mB3.refusals == 1, "MECH_B: bank+margin-1 free PSRAM -> NO_MEM, counted")
    ok(all(x is None for x in mB3.extra), "MECH_B: the refused map took no secondary slot")
    mB4 = new_model(mutations, mech_b=True, psram_free=59 * PAGE_SIZE + ARENA_MARGIN)
    ok(mB4.map_extra(16 * PAGE_SIZE, 59 * PAGE_SIZE)[0] == E_OK,
       "MECH_B: exactly bank+margin free PSRAM is enough (gate is >=, not >)")

    # 14. reserve-then-arena-map bills the booking once (symmetric with A).
    mB5 = new_model(mutations, mech_b=True)
    mB5.primary = 31 * PAGE_SIZE
    mB5.reserve(29 * PAGE_SIZE)
    e5, s5 = mB5.map_extra(40 * PAGE_SIZE, 29 * PAGE_SIZE)
    ok(e5 == E_OK and mB5.reserved_bytes() == 0, "MECH_B: the arena map consumes the booking")
    ok(mB5.mmu_pages() == 31, "MECH_B: booking consumed AND the arena unbilled — primary alone")

    # 15/16. a covered re-request of a live arena is the NO-OP the covered
    # branch promises — no re-gate (PSRAM could have gone), no refusal, same
    # slot; and unmap frees it back so the slot can be filled again.
    mB6 = new_model(mutations, mech_b=True, psram_free=59 * PAGE_SIZE + ARENA_MARGIN)
    e6, s6 = mB6.map_extra(16 * PAGE_SIZE, 59 * PAGE_SIZE)
    mB6.psram_free = 0                       # a re-request must not consult the gate
    e6b, s6b = mB6.map_extra(16 * PAGE_SIZE, 59 * PAGE_SIZE)
    ok(e6b == E_OK and s6b == s6 and mB6.refusals == 0,
       "MECH_B: covered arena re-map is a no-op — no re-gate, no second copy, same slot")
    ok(mB6.unmap_extra(s6) == E_OK, "MECH_B: unmap_extra releases the arena slot")
    mB6.psram_free = 59 * PAGE_SIZE + ARENA_MARGIN
    e6c, s6c = mB6.map_extra(16 * PAGE_SIZE, 59 * PAGE_SIZE)
    ok(e6c == E_OK and s6c == s6, "MECH_B: an emptied arena slot refills at the same index")

    # ---- D30's boot refill of the PRIMARY bank (§20.2, §20.4, §20.5(2)/(4)) ----
    # av-kit's real bank size, because D30 sized its gate against exactly this
    # bundle, and the board's live free-PSRAM figure from /api/v1/status on
    # 2026-10-19 (7,498,456 B) is what makes the gate checks below real rather
    # than symbolic: KIT + MARGIN leaves 5.1 MB of the 8 MB bank still standing.
    KIT = 1173563

    m7 = new_model(mutations, mech_b=True, psram_free=8 * 1024 * 1024)
    ok(m7.primary_fill(0x410000, 0, False, 0, 0) == E_ARG,
       "REFILL: a zero-length ask is refused as an argument, not a capacity miss")
    ok(m7.refusals == 0, "REFILL: the argument reject is NOT a capacity refusal")

    m8 = new_model(mutations, mech_b=True, psram_free=KIT + ARENA_MARGIN - 1)
    b8 = m8.refusals
    ok(m8.primary_fill(0x410000, KIT, True, 0, 0) == E_NM,
       "REFILL: bank+margin-1 free PSRAM is refused (the §20.5(4) refusal arm)")
    ok(m8.refusals == b8 + 1, "REFILL: the arena refusal is counted, like a page-budget miss")
    ok(m8.primary == 0 and m8.primary_kind == "map",
       "REFILL: a refused refill leaves the primary untouched -- nothing half-landed")

    m9 = new_model(mutations, mech_b=True, psram_free=KIT + ARENA_MARGIN)
    ok(m9.primary_fill(0x410000, KIT, True, 0xABCD, 0xABCD, ms=143) == E_OK,
       "REFILL: exactly bank+margin free PSRAM is granted (the gate is >=, not >)")
    ok(m9.primary == KIT and m9.primary_kind == "arena",
       "REFILL: the granted bank is recorded as the arena, not as a map")
    ok(m9.mmu_pages() == 0,
       "REFILL: a refilled primary bills the DROM window NOTHING (D30's whole point)")
    ok(m9.resident_count() == 1,
       "REFILL: unbilled is not unresident -- an arena primary is resident art")
    ok(m9.fill_ms == 143, "REFILL: the timed pass reaches the caller (§20.5(2)'s figure)")

    m10 = new_model(mutations, mech_b=True, psram_free=8 * 1024 * 1024)
    m10.primary = KIT
    b10 = m10.refusals
    ok(m10.primary_fill(0x410000, KIT, True, 0x1111, 0x2222) == E_CRC,
       "REFILL: a checksummed slot that does not match is refused")
    ok(m10.refusals == b10, "REFILL: a checksum miss is NOT a capacity refusal")
    ok(m10.primary == KIT, "REFILL: the corrupt bank was freed, not installed")

    m11 = new_model(mutations, mech_b=True, psram_free=8 * 1024 * 1024)
    m11.primary_map(0, 59 * PAGE_SIZE)
    ok(m11.mmu_pages() == 59, "REFILL: the card-less default still maps, and still bills (§20.4)")
    ok(m11.primary_fill(0x410000, KIT, True, 7, 7) == E_OK and m11.mmu_pages() == 0,
       "REFILL: taking over a mapped primary releases the map -- no page left billed")
    ok(m11.primary_kind == "arena", "REFILL: the handoff records the mechanism it used")

    m12 = new_model(mutations, mech_b=True, psram_free=8 * 1024 * 1024)
    ok(m12.primary_fill(0x7FF000, 64 * 1024, False, 0, 0) == E_ISZ,
       "REFILL: a bank past the end of \"assets\" is refused on size, after the allocation")
    ok(m12.primary == 0, "REFILL: the out-of-range ask installed nothing")


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
    # ---- mechanism B mutations: the arena rules must have bite too ----
    ("if e is not None and not e[2]:", "if e is not None:"),         # arena billed as a map
    ("if self.psram_free < need_bytes + ARENA_MARGIN:\n                self.refusals += 1\n                return (ERR_NO_MEM, -1)",
     "if False:\n                self.refusals += 1\n                return (ERR_NO_MEM, -1)"),  # arena gate gone
    ("self.extra[slot] = (need_bytes, offset, True)",
     "self.extra[slot] = (need_bytes, offset, False)"),              # 'arena' silently an MMU map
    ("self._consume(art_round_pages(need_bytes))\n            return (ESP_OK, slot)",
     "pass\n            return (ESP_OK, slot)"),                     # arena fill drops the booking
    # ---- D30's primary refill: the arena rules applied to the dial's own bank ----
    ("if self.primary and self.primary_kind == \"map\" else 0",
     "if self.primary else 0"),                                       # arena primary billed
    ("self.primary_kind = \"arena\"",
     "self.primary_kind = \"map\""),                                  # refill logged as a map
    ("            return ERR_INVALID_ARG\n        if self.psram_free < need_bytes + ARENA_MARGIN:",
     "            return ERR_INVALID_ARG\n        if False:"),        # refill gate gone
    ("        if verify and actual_crc != expect_crc:\n            return ERR_INVALID_CRC",
     "        if False:\n            return ERR_INVALID_CRC"),        # corrupt bank installed
    ("        if offset + need_bytes > self.part_size:\n            return ERR_INVALID_SIZE",
     "        if False:\n            return ERR_INVALID_SIZE"),       # unbounded refill accepted
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


