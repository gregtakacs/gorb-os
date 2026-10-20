/* orb_bundle.c — the FORMAT 3 bundle parse mirror (P6.1, §22.2/§22.6).
 *
 * Pure parsing: no allocator, no card, no partition — the discipline
 * orb_theme.c's header/geometry half follows, and the reason
 * tools/host_orb_bundle_c_test.py can link this file with host gcc and drive
 * it with the writer's own bytes. The three-way rule names the legs: the
 * Python writer (tools/orbbundle.py), this reader, and that sentinel suite
 * move in one commit, or the test fails rather than ship a dial that reads
 * its bundles differently from the tool that wrote them.
 *
 * The refusal sentences are copied from orbbundle.py WORD FOR WORD, em dash
 * and all, because §22.3 promises the owner reads on the device the sentence
 * the tool said. Two labeled divergences, both on the header's format lines
 * and pinned by the suite so they stay decisions rather than drift: a format
 * mismatch answers with the device gate's existing sentence (the host bake
 * names both numbers), and the header-checksum sentence keeps v2 firmware's
 * first clause ("container header is damaged") where the host says
 * "checksum mismatch" — the tail both sides show the owner is identical.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_rom_crc.h"
#include "orb_bundle.h"

/* The header's word indices, from orb_theme.c's own enum: FORMAT 3 changes
 * the VALUES of words 1/2/9 and nothing else, which is what lets the 128-byte
 * torn-header check stay one rule for both formats. */
enum {
    W_MAGIC = 0, W_FORMAT, W_GEOM_ABI, W_HDR_SIZE,
    W_GEOM_OFF, W_GEOM_SIZE, W_GEOM_CRC, W_ART_SIZE, W_ART_CRC,
    W_SCR_W, W_SCR_H, W_SERIAL, W_BAKED_MS, W_ART_OFF,
    W_NAME = 14, W_HDR_CRC = 28, W_WORDS = 32
};
#define B3_MAGIC  0x474F5254u     /* 'T','R','O','G' — THEME_MAGIC itself */
#define B3_SECTOR 4096u

/* The index record's layout mirrors orbbundle.IDX_PACK "<4H7I2H" with the
 * field order of IDX_ORDER: four u16s at bytes 0,2,4,6 (slug_off, slug_len,
 * abi_min, abi_max), seven u32s at 8,12,16,20,24,28,32 (n_elem, geom_off,
 * geom_size, geom_crc, art_off, art_size, art_crc), two u16s at 36,38
 * (scr_w, scr_h). geom_crc sits between art_off's neighbours: the suite's
 * sentinel record — every field a distinct value, read back at its own name
 * — is what pins this table, the same lesson host_orbapp_reader_test learned
 * the hard way when a field order drifted in the app manifest. */
#define IDX_BYTES        (ORB_BUNDLE_IDX_WORDS * 4u)      /* 40 */
#define IDX_HEAD_BYTES   8u
#define BLOB_COUNT_WORDS 4u
#define BLOB_HEAD_BYTES  ((BLOB_COUNT_WORDS + ORB_BUNDLE_SCALARS_N) * 4u)
#define SPR_BYTES        (4u * 4u) /* a sprite entry: off, size, w, h */

static uint16_t b16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t b32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Refusal strings fill one static buffer — every counted sentence has a
 * single call site, the callers return the pointer immediately, and the file
 * is called from one task at a time (the card lock serializes it on device,
 * the harness on host). Variadic so the compiler checks every printf site. */
static char s_msg[160];

static const char *reject(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
static const char *reject(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_msg, sizeof s_msg, fmt, ap);
    va_end(ap);
    return s_msg;
}

/* The sign extension the clock dialect's readers need, mirroring
 * theme_schema._sext through _SEXT_BITS: words 3 and 4 (shadow_dx/dy) are
 * i8, word 10 (bg_fps) is i16; every other word is unsigned as stored. The
 * sentinel suite pins this table against orbtheme.THEME_FIELDS, so a dialect
 * change that forgets this file fails there rather than on a dial. */
int32_t orb_bundle_scalar_sext(const orb_bundle_sec_t *s, uint32_t idx)
{
    const uint32_t v = b32(s->blob + BLOB_COUNT_WORDS * 4u + idx * 4u);
    if (idx == 3 || idx == 4)
        return (int32_t)(int8_t)(v & 0xFFu);
    if (idx == 10)
        return (int32_t)(int16_t)(v & 0xFFFFu);
    return (int32_t)v;
}

uint32_t orb_bundle_scalar(const orb_bundle_sec_t *s, uint32_t idx)
{
    return b32(s->blob + BLOB_COUNT_WORDS * 4u + idx * 4u);
}

/* unpack_record()'s device-side twin: one fixed 17-word row, narrow fields
 * masked from their word, the i16s sign-extended. blob_check() proved the
 * table's length against the blob before any row index reaches here, and the
 * compositor iterates exactly n_elem rows — the zero-copy rule, so a table
 * wider than any firmware array is still fully addressable and the bake, not
 * this file, owns the cap refusal (unpack_elements' own discipline). */
void orb_bundle_elem_at(const orb_bundle_sec_t *s, uint32_t i,
                        orb_bundle_elem_t *e)
{
    const uint8_t *r = s->blob + BLOB_HEAD_BYTES +
                       i * (ORB_BUNDLE_ELEM_WORDS * 4u);
    e->n        = (uint16_t)b32(r +  0);
    e->cls      = (uint8_t)b32(r +  4);
    e->flags    = (uint8_t)b32(r +  8);
    e->order    = (uint8_t)b32(r + 12);
    e->alpha    = (uint8_t)b32(r + 16);
    e->slot     = (uint16_t)b32(r + 20);
    e->frame0   = (uint16_t)b32(r + 24);
    e->n_frames = (uint16_t)b32(r + 28);
    e->x        = (int16_t)b32(r + 32);
    e->y        = (int16_t)b32(r + 36);
    e->w        = (uint16_t)b32(r + 40);
    e->h        = (uint16_t)b32(r + 44);
    e->pivot_x  = (int16_t)b32(r + 48);
    e->pivot_y  = (int16_t)b32(r + 52);
    e->radius   = (uint16_t)b32(r + 56);
    e->degrees  = (uint16_t)b32(r + 60);
    e->scale16  = (uint16_t)b32(r + 64);
}

/* ---- the FORMAT 3 header -------------------------------------------------- */

const char *orb_bundle3_hdr_parse(const uint32_t *w, orb_bundle3_hdr_t *o)
{
    if (w[W_MAGIC] != B3_MAGIC) return "not an .orbtheme file";
    if (w[W_FORMAT] != ORB_BUNDLE3_FORMAT)
        return "theme format this firmware cannot read";   /* WORDING-DIFF,
                                                            * pinned by the
                                                            * suite: the host
                                                            * bake names both
                                                            * numbers */
    if (w[W_GEOM_ABI] != ORB_BUNDLE3_ABI)
        return reject("bundle geometry v%lu; this build reads v%lu — re-bake it",
                      (unsigned long)w[W_GEOM_ABI], (unsigned long)ORB_BUNDLE3_ABI);

    /* The self-checksum first, on the header's SHAPE — one rule for every
     * .orbtheme the door admits: same 128 bytes, same zeroed word 28 — the
     * torn-header check does not vary with the dialect inside the header. */
    uint32_t zeroed[W_WORDS];
    memcpy(zeroed, w, sizeof zeroed);
    zeroed[W_HDR_CRC] = 0;
    if (w[W_HDR_CRC] != esp_rom_crc32_le(0, (const uint8_t *)zeroed, sizeof zeroed))
        return "container header is damaged — re-copy the file onto the card";

    o->geom_off = w[W_GEOM_OFF];   o->geom_size = w[W_GEOM_SIZE];
    o->geom_crc = w[W_GEOM_CRC];
    o->art_off  = w[W_ART_OFF];    o->art_size  = w[W_ART_SIZE];
    o->art_crc  = w[W_ART_CRC];
    o->serial   = w[W_SERIAL];
    o->n_sections = w[W_SCR_W];    /* the one repurposed word: FORMAT 3's count.
                                    * No range test HERE: a count outside 1..8
                                    * dies at the index's own sentence below or
                                    * at the header-vs-index match, on this side
                                    * and the writer's alike. */

    /* The D29 budget gate, install end (P6.2, §22.3), in copy_into's sums and
     * words (orb_theme.c:1349-1350): geometry installs at SLOT_GEOM_OFF and its
     * sector-aligned erase must not overrun the banks, and the payload must
     * live inside SLOT_CAP less SLOT_ART_OFF — ORB_THEME_SLOT_CAP on the header,
     * 4,128,768 B. == cap is ACCEPTED both ways: each cap is the aligned sum
     * landing exactly on the next boundary, the same rule the writer and
     * check_budget() state. A header over either is refused HERE, before the
     * name or any streamed byte, so the door never prices an install that
     * cannot happen. Sentences byte-identical to orbbundle.check_budget(). */
    if (o->geom_size > ORB_BUNDLE_GEOM_CAP)
        return reject("geometry region is %lu B; the dial's table area holds "
                      "%lu B — geometry would overrun the pixels",
                      (unsigned long)o->geom_size,
                      (unsigned long)ORB_BUNDLE_GEOM_CAP);
    if (o->art_size > ORB_BUNDLE_ART_CAP)
        return reject("pixel payload is %lu B; a theme slot holds %lu B — "
                      "theme is larger than a slot",
                      (unsigned long)o->art_size,
                      (unsigned long)ORB_BUNDLE_ART_CAP);

    int i = 0;
    for (; i < 56; i++) {
        const char c = (char)((w[W_NAME + i / 4] >> (8 * (i % 4))) & 0xFF);
        if (!c) break;
        o->name[i] = c;
    }
    o->name[i] = '\0';
    if (o->name[0] == '\0')
        return "theme has no name in its header — re-bake it";
    return NULL;
}

/* ---- the geometry region: index, walk, blobs ------------------------------ */

int orb_bundle_slug_rule(const uint8_t *p, uint32_t n)
{
    if (n < 1 || n > ORB_BUNDLE_SLUG_MAX) return 1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return 1;
    }
    return 0;
}

/* unpack_blob()'s sprite list, one row at a time: the four words pack_blob()
 * wrote per plane, in the order it wrote them (off, size, w, h). Same zero-copy
 * rule as orb_bundle_elem_at — no bounds re-check here, because blob_check()
 * proved the blob's length against n_elem and n_spr before any index landed. */
void orb_bundle_spr_at(const orb_bundle_sec_t *s, uint32_t i,
                       orb_bundle_spr_t *sp)
{
    const uint8_t *r = s->blob + BLOB_HEAD_BYTES +
                       (uint64_t)s->n_elem * (ORB_BUNDLE_ELEM_WORDS * 4u) +
                       (uint64_t)i * SPR_BYTES;
    sp->off  = b32(r +  0);
    sp->size = b32(r +  4);
    sp->w    = b32(r +  8);
    sp->h    = b32(r + 12);
}

/* unpack_blob()'s mirror, in its refusal order: head length, scalar dialect,
 * count length arithmetic — in 64-bit, because a u32 product can wrap where
 * Python's arbitrary precision cannot and the two readers must refuse the
 * same files. Reports the blob's own counts and the slug's place inside it;
 * the tables stay in place (the zero-copy rule) and every later read is
 * bounded by the length equality proven here. */
static const char *blob_check(const uint8_t *blob, uint32_t len,
                              uint32_t *n_elem, uint32_t *n_spr,
                              uint32_t *slug_off, uint32_t *slug_len)
{
    if (len < BLOB_HEAD_BYTES)
        return "section blob is shorter than its counts and scalars";
    const uint32_t ne = b32(blob + 0), ns = b32(blob + 4);
    const uint32_t nsc = b32(blob + 8), nl = b32(blob + 12);
    if (nsc != ORB_BUNDLE_SCALARS_N)
        return reject("blob carries %lu scalars; this dialect has %lu",
                      (unsigned long)nsc, (unsigned long)ORB_BUNDLE_SCALARS_N);
    const uint64_t body = (uint64_t)(BLOB_COUNT_WORDS + nsc) * 4u
                        + (uint64_t)ne * (ORB_BUNDLE_ELEM_WORDS * 4u)
                        + (uint64_t)ns * SPR_BYTES;
    const uint64_t want = body + nl + (4u - (nl % 4u)) % 4u;
    if (want != len) {
        snprintf(s_msg, sizeof s_msg,
                 "blob is %lu bytes; its counts say %llu — a torn file, not a "
                 "short one", (unsigned long)len, (unsigned long long)want);
        return s_msg;
    }
    if (ne > 0xFFFFu || ns > 0xFFFFu)
        return "blob outgrew its u16 counts";   /* the record's counts are u16
                                                 * and this reader stores them
                                                 * as such; orbbundle.py's
                                                 * unpack_blob refuses the same
                                                 * rule in the same words. Since
                                                 * P6.2 no file can reach this
                                                 * line as its first refusal —
                                                 * the budget gate above bounds
                                                 * the region at 60 KB and a
                                                 * u16-outgrown blob cannot fit
                                                 * one — the check stands as the
                                                 * backstop, and parity vector
                                                 * 5 pins the order */
    *n_elem = ne; *n_spr = ns;
    *slug_off = (uint32_t)body; *slug_len = nl;
    return NULL;
}

/* read_bundle()'s per-bank line, on one section: the crc of the bytes at the
 * record's stored range. A range past the file's end is CRCed over exactly
 * the bytes the host reader's slice would yield — short or empty — so the
 * same damaged copy meets the same sentence on both sides. */
static const char *bank_check(const orb_bundle_sec_t *s, const uint8_t *file,
                              uint32_t file_size)
{
    const uint32_t have = (s->art_off <= file_size)
        ? (s->art_size <= file_size - s->art_off ? s->art_size
                                                 : file_size - s->art_off)
        : 0u;
    if (esp_rom_crc32_le(0, have ? file + s->art_off : (const uint8_t *)"",
                         have) != s->art_crc) {
        snprintf(s_msg, sizeof s_msg,
                 "section '%s' bank checksum mismatch — the copy on the card is "
                 "damaged", s->slug);
        return s_msg;
    }
    return NULL;
}

/* The region half — the whole of read_bundle() below its header: bounds,
 * region checksum, index, walk, per-blob length/crc/slug/n_elem checks, the
 * art walk and per-bank checksums in section order, and the sortedness check
 * last. The device's streaming installer (§22.3) verifies each bank as its
 * bytes arrive instead of over the buffer; the rule it applies there is
 * bank_check()'s here, exported as orb_bundle_bank_rule(). NULL on success,
 * refusal sentence otherwise, with orbbundle.py's order of first refusal
 * preserved line for line.
 *
 * banks_present=false is the streaming shape: `file` points at the GEOMETRY
 * REGION (so h->geom_off is forced to 0 by the caller below) and `file_size` is
 * the whole container's size, which the caller knows from the card. Every check
 * that does not need a bank's bytes still runs and still bites in that order —
 * the art walk's bounds and the per-record art_off-vs-walk lie check included —
 * and only bank_check()'s dereference is skipped. A caller cannot use it to
 * install a bank it never checksummed: orb_bundle_bank_rule() is the other half
 * of the deal, and the installer that skips it has no way to say it is done. */
static const char *parse_core(const orb_bundle3_hdr_t *h, const uint8_t *file,
                              uint32_t file_size, orb_bundle_t *b, bool banks_present)
{
    if (h->geom_off > file_size || h->geom_size > file_size - h->geom_off)
        return "geometry region runs past the end of the file";
    const uint8_t *region = file + h->geom_off;
    const uint32_t region_size = h->geom_size;
    if (esp_rom_crc32_le(0, region, region_size) != h->geom_crc)
        return "geometry checksum mismatch — the copy on the card is damaged";
    if (region_size < IDX_HEAD_BYTES)
        return reject("index claims %lu sections in %lu region bytes — torn file",
                      (unsigned long)h->n_sections, (unsigned long)region_size);

    const uint32_t n = b32(region);
    if (n < 1 || n > ORB_BUNDLE_SECT_MAX)
        return reject("index claims %lu sections; a bundle holds 1..%lu",
                      (unsigned long)n, (unsigned long)ORB_BUNDLE_SECT_MAX);
    if (region_size < IDX_HEAD_BYTES + n * IDX_BYTES)
        return reject("index claims %lu sections in %lu region bytes — torn file",
                      (unsigned long)n, (unsigned long)region_size);
    if (n != h->n_sections)
        return reject("header names %lu sections, the index holds %lu",
                      (unsigned long)h->n_sections, (unsigned long)n);

    memset(b, 0, sizeof *b);
    b->region = region; b->region_size = region_size; b->n_sections = n;

    /* The art region, bounded the way the host reader bounds its slice, and
     * walked sector by sector like write_bundle() laid it: a stored bank
     * offset that disagrees with the walk is the same class of lie as a
     * geom_off that does, and gets the same sentence. */
    if (h->art_off > file_size || h->art_size > file_size - h->art_off)
        return "art region runs past the end of the file";
    uint32_t aoff = h->art_off;

    /* The walk is the sequential truth; the stored geom_off must answer to
     * it. A repack that slides a blob without re-stamping its record — or a
     * record edited to point somewhere else — is refused here, not quietly
     * read from the wrong address. */
    uint32_t walk = IDX_HEAD_BYTES + n * IDX_BYTES;

    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *r = region + IDX_HEAD_BYTES + i * IDX_BYTES;
        orb_bundle_sec_t *s = &b->sec[i];
        const uint32_t slug_off = b16(r + 0), rec_elem = b32(r + 8);
        const uint32_t rec_slen = b16(r + 2), geom_crc = b32(r + 20);
        s->slug_len = (uint8_t)rec_slen;
        s->abi_min  = (uint8_t)b16(r + 4);
        s->abi_max  = (uint8_t)b16(r + 6);
        s->n_elem   = (uint16_t)rec_elem;
        s->geom_off = b32(r + 12); s->geom_size = b32(r + 16);
        s->art_off  = b32(r + 24); s->art_size  = b32(r + 28);
        s->art_crc  = b32(r + 32);
        s->scr_w    = b16(r + 36); s->scr_h     = b16(r + 38);
        if (s->geom_off != walk)
            return reject("section %lu stores geom_off %lu where the walk says %lu "
                          "— the index and the bytes contradict",
                          (unsigned long)i, (unsigned long)s->geom_off, (unsigned long)walk);
        if (walk > region_size || s->geom_size > region_size - walk)
            return reject("section %lu's blob range is outside the region",
                          (unsigned long)i);
        s->blob = region + walk;
        if (esp_rom_crc32_le(0, s->blob, s->geom_size) != geom_crc)
            return reject("section %lu blob checksum mismatch", (unsigned long)i);

        uint32_t ne = 0, ns = 0, so = 0, sl = 0;   /* zeroed to answer gcc's
                                                   * uninit warning: blob_check
                                                   * fills all four on success */
        const char *why = blob_check(s->blob, s->geom_size, &ne, &ns, &so, &sl);
        if (why) return why;
        /* The record's window first — sized, not summed, so no u32 wrap can
         * walk memcmp out of the region — and the lengths before the bytes:
         * memcmp runs only when sl == rec_slen inside proven bounds, exactly
         * when Python's slice compare can differ. */
        if (slug_off > region_size || rec_slen > region_size - slug_off ||
            sl != rec_slen ||
            memcmp(s->blob + so, region + slug_off, sl) != 0)
            return reject("section %lu: the index's slug and the blob's disagree",
                          (unsigned long)i);
        if (orb_bundle_slug_rule(region + slug_off, sl)) {
            snprintf(s_msg, sizeof s_msg,
                     "bundle slug '%.*s' is not lowercase a-z 0-9 _ of 1..%lu chars",
                     (int)(sl < ORB_BUNDLE_SLUG_MAX ? sl : ORB_BUNDLE_SLUG_MAX),
                     region + slug_off, (unsigned long)ORB_BUNDLE_SLUG_MAX);
            return s_msg;                /* the name must be one the writer's
                                          * gate could have produced — and an
                                          * app could later claim; both sides
                                          * print the rule in the same words */
        }
        memcpy(s->slug, region + slug_off, sl);
        s->slug[sl] = '\0';
        if (rec_elem != ne) {
            snprintf(s_msg, sizeof s_msg,
                     "section '%s': the index counts %lu elements, the blob "
                     "carries %lu", s->slug, (unsigned long)rec_elem, (unsigned long)ne);
            return s_msg;
        }
        s->n_elem = (uint16_t)ne; s->n_spr = (uint16_t)ns;
        if (s->art_off != aoff)
            return reject("section %lu stores art_off %lu where the art walk "
                          "says %lu — the index and the bytes contradict",
                          (unsigned long)i, (unsigned long)s->art_off, (unsigned long)aoff);
        if (banks_present) {
            why = bank_check(s, file, file_size);
            if (why) return why;
        }
        aoff += (s->art_size + B3_SECTOR - 1) & ~(B3_SECTOR - 1);
        walk += s->geom_size;
    }


    /* Sortedness last, as read_bundle() checks it: by then every slug in the
     * comparison is one the blob itself agreed to, so an index sorted only by
     * its own storage order — the swap the §22.6 rules exist to catch — is
     * what this line refuses. Adjacent pairs only: >= on strcmp of ASCII
     * slugs is the codepoint order Python's str sort gives. */
    for (uint32_t i = 1; i < n; i++)
        if (strcmp(b->sec[i - 1].slug, b->sec[i].slug) >= 0)
            return "index is not slug-sorted; the resolver and --inspect both "
                   "walk it in order, and a shuffled one hides a lost section";
    return NULL;
}

const char *orb_bundle_parse(const orb_bundle3_hdr_t *h, const uint8_t *file,
                             uint32_t file_size, orb_bundle_t *b)
{
    return parse_core(h, file, file_size, b, true);
}

const char *orb_bundle_parse_region(const orb_bundle3_hdr_t *h, const uint8_t *region,
                                    uint32_t region_size, uint32_t file_size,
                                    orb_bundle_t *b)
{
    /* The streaming installer's first door (P6.3, §22.3): the region is a buffer of
     * its own, so the header's file-relative geom_off is not an offset into it. The
     * copy forces 0 and carries the buffer's real length, and the region checksum
     * inside parse_core is what proves that length against the header's claim — a
     * short read fails there, before a single index record is believed. */
    orb_bundle3_hdr_t local = *h;
    local.geom_off  = 0;
    local.geom_size = region_size;
    return parse_core(&local, region, file_size, b, false);
}

const char *orb_bundle_bank_rule(const orb_bundle_sec_t *s, uint32_t got_bytes,
                                 uint32_t got_crc)
{
    /* bank_check()'s sentence, for bytes that arrived across a wire instead of
     * sitting in a buffer: short is wrong first, and equal-but-not-matching is
     * wrong second, so a truncated copy can never land on the checksum clause. */
    if (got_bytes != s->art_size || got_crc != s->art_crc) {
        snprintf(s_msg, sizeof s_msg,
                 "section '%s' bank checksum mismatch — the copy on the card is "
                 "damaged", s->slug);
        return s_msg;
    }
    return NULL;
}

const char *orb_bundle_rebase(orb_bundle_t *b, orb_bundle3_hdr_t *h, uint8_t *region,
                              uint32_t new_art_off, uint32_t slot_cap,
                              uint32_t *out_file_art_off)
{
    /* The record's art_off is the 7th word of its 40-byte entry, at byte 24
     * (b32(r + 24) above): the field order theme_schema pins and the sentinel
     * suite dumps. Editing it in place is honest only because the caller holds a
     * buffer this module already parsed, and the region's own checksum is
     * recomputed here so what the header claims covers what is now in it.
     *
     * TWO PASSES. The bounds of every section are settled before a single byte is
     * written, because a refusal hands the caller back the region it already parsed
     * and the installer's error path reports the refusal while that buffer is still
     * live. A check-then-write loop would leave section 0 moved and section 3 not,
     * which is the half-state the header-last rule exists to make impossible. */
    const int32_t delta = (int32_t)new_art_off - (int32_t)h->art_off;
    if (out_file_art_off) *out_file_art_off = h->art_off;
    for (uint32_t i = 0; i < b->n_sections; i++) {
        const orb_bundle_sec_t *s = &b->sec[i];
        const uint32_t moved = (uint32_t)((int64_t)s->art_off + (int64_t)delta);
        if (delta < 0 && (uint32_t)-delta > s->art_off)
            return "section bank offset would run below the region's start";
        if (moved > slot_cap || s->art_size > slot_cap - moved)
            return "section bank would run past the end of the slot";
    }
    for (uint32_t i = 0; i < b->n_sections; i++) {
        orb_bundle_sec_t *s = &b->sec[i];
        const uint32_t moved = (uint32_t)((int64_t)s->art_off + (int64_t)delta);
        s->art_off = moved;
        uint8_t *rec = region + IDX_HEAD_BYTES + i * IDX_BYTES;
        rec[24] = (uint8_t)(moved & 0xffu);         rec[25] = (uint8_t)((moved >> 8) & 0xffu);
        rec[26] = (uint8_t)((moved >> 16) & 0xffu); rec[27] = (uint8_t)((moved >> 24) & 0xffu);
    }
    h->art_off  = new_art_off;
    h->geom_crc = esp_rom_crc32_le(0, region, h->geom_size);
    return NULL;
}

const orb_bundle_sec_t *orb_bundle_find(const orb_bundle_t *b,
                                        const char *slug)
{
    for (uint32_t i = 0; i < b->n_sections; i++)
        if (strcmp(b->sec[i].slug, slug) == 0)
            return &b->sec[i];
    return NULL;
}



