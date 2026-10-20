/* orb_bundle.h — the FORMAT 3 theme bundle reader (P6.1, §22.2/§22.6).

 * One .orbtheme holding N per-app sections, addressed by slug, where FORMAT 2
 * held one clock bank. The 128-byte header is FORMAT 2's layout with two words
 * repurposed — the FORMAT stamp says 3 and the header's scr_w word carries the
 * section count instead of a panel width — so the existing torn-header check
 * transfers byte-identically. Everything new rides the geometry region: a
 * 2-word index head, n fixed 40-byte records, then one self-describing blob
 * per section. Art banks sit at the end of the file, sector-aligned, addressed
 * by the records alone; no section resolves to another's pixels.
 *
 * This file is the parse mirror of tools/orbbundle.py, and the third leg of
 * that module's three-way rule: writer, this reader, and
 * tools/host_orb_bundle_c_test.py move in one commit or the test fails. The
 * harness feeds the SHIPPED C reader the writer's own bytes (host gcc,
 * zlib-backed crc shim) and requires identical verdicts and identical refusal
 * sentences, without a board, a card, or a partition in the loop — the
 * host_orbapp_reader_test.py discipline.
 *
 * Zero-copy by design: nothing is allocated and nothing is clamped. The
 * compositor (P6.4+) reads rows through orb_bundle_elem_at() as needed, so a
 * table wider than any firmware array is still fully addressable — and the
 * refusal of a too-wide table belongs to the bake, which never writes one,
 * exactly as unpack_elements() on the host deliberately does not re-check.
 *
 * P6.1 scope: parse and refuse. The device install path does not ingest a
 * bundle through this reader until P6.3; orb_theme.c's format gate calls
 * orb_bundle3_hdr_parse() to recognise format 3 honestly — a real bundle
 * gets the bundle sentence, not the v2 reader's "cannot read" — and the
 * install path refuses it with its own single-sentence reason until the
 * THEMES flip (P6.4) gives the device a multi-app door.
 */
#ifndef ORB_BUNDLE_H
#define ORB_BUNDLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORB_BUNDLE3_FORMAT   3u        /* the header's FORMAT word */
#define ORB_BUNDLE3_ABI      3u        /* the header's GEOM_ABI word */
#define ORB_BUNDLE_SECT_MAX  8u        /* B5's app cap: the index's width */
#define ORB_BUNDLE_IDX_WORDS 10u       /* 4H + 7I + 2H = 40-byte records */
#define ORB_BUNDLE_SCALARS_N 26u       /* the clock dialect, carried opaque */
#define ORB_BUNDLE_ELEM_WORDS 19u      /* theme_schema.ELEMENT_FIELDS: the width a
                                        * write carries and this reader decodes.
                                        * A read row's STRIDE is the blob's own
                                        * number (§23.5): blob_check() derives it
                                        * from the length equality, an older row
                                        * stride decodes with the appended
                                        * columns zero, and D32's ruling A —
                                        * append-only, no format bump — holds. */
#define ORB_BUNDLE_ELEM_WORDS_MIN 17u  /* the narrowest row the shipped dialect
                                        * ever wrote, pre-D32: the floor the
                                        * derived stride is refused below */
#define ORB_BUNDLE_SLUG_MAX  23u       /* ORBAPP_SLUG_MAX - 1: the NUL's room */
#define ORB_BUNDLE_GEOM_CAP  0xF000u   /* orb_theme.c SLOT_GEOM_CAP: 60 KB of tables */
#define ORB_BUNDLE_ART_CAP   0x3F0000u /* SLOT_CAP less SLOT_ART_OFF: the
                                        * orb_theme.h ORB_THEME_SLOT_CAP,
                                        * 4,128,768 B of banks per slot */


/* One parsed index record, plus pointers INTO the caller's region for the
 * blob it locates. Offsets and sizes are exactly what the record stored; the
 * parse proved them against the walk and the blob before handing them back. */
typedef struct {
    char        slug[ORB_BUNDLE_SLUG_MAX + 1]; /* the blob's own, NUL-kept */
    const uint8_t *blob;                        /* region-relative blob    */
    uint32_t    geom_off, geom_size;            /* its range inside region  */
    uint32_t    art_off,  art_size;             /* its bank inside the file */
    uint32_t    art_crc;                        /* record's, cross-checked to blob */
    uint16_t    n_elem, n_spr;
    uint16_t    elem_words;    /* the blob's own row stride (§23.5): 19 from a
                                * current bake, 17 from an older one; 0 reads
                                * as the current width */
    uint16_t    scr_w, scr_h;
    uint8_t     abi_min, abi_max;
    uint8_t     slug_len;
} orb_bundle_sec_t;

typedef struct {
    char     name[57];                 /* the bundle name: R4's theme_id input */
    uint32_t n_sections;
    uint32_t geom_off, geom_size, geom_crc;
    uint32_t art_off,  art_size,  art_crc;
    uint32_t serial;
} orb_bundle3_hdr_t;

typedef struct {
    const uint8_t     *region;         /* caller-owned; parse keeps no copy  */
    uint32_t           region_size;
    uint32_t           n_sections;
    orb_bundle_sec_t   sec[ORB_BUNDLE_SECT_MAX];
} orb_bundle_t;

/* The header half of FORMAT 3: the same 128 bytes as FORMAT 2 with the format
 * and abi stamps swapped and the section count riding scr_w. Two D29 budget
 * sums ride it too (P6.2): geom_size against ORB_BUNDLE_GEOM_CAP and art_size
 * against ORB_BUNDLE_ART_CAP, == cap accepted — the same refusal check_budget()
 * makes at write time, so a header that names a slot it cannot fit is refused
 * before its name is read or a byte streams. NULL on success
 * (the file IS a format-3 bundle — whether this build may INSTALL one is the
 * caller's rule, stated at the install door), otherwise the refusal sentence,
 * in words, byte-identical to parse_header3() in tools/orbbundle.py except
 * where the suite pins a labeled wording pair: the format-line refusal (the
 * host names both numbers; the device keeps v2's one sentence for every
 * unknown format) and the header-checksum sentence's first clause. */
const char *orb_bundle3_hdr_parse(const uint32_t *w, orb_bundle3_hdr_t *o);

/* The shared slug rule (1..23 bytes, lowercase a-z 0-9 _): 0 when the name
 * passes, 1 when it does not. orbbundle.py applies check_slug's copy of the
 * rule to the same bytes; both print check_slug's sentence for a miss. */
int orb_bundle_slug_rule(const uint8_t *p, uint32_t n);

/* The region half, driven from the whole file like read_bundle(): bounds,
 * region checksum, index, walk, per-blob crc/length/slug/n_elem cross-checks,
 * the art walk, per-bank checksums, and the final slug-sortedness check, in
 * that order. `file` is the whole container, caller-owned. NULL on success.
 * Refusal sentences are byte-identical to read_bundle()/unpack_index()/
 * unpack_blob() in tools/orbbundle.py. The device's streaming install path
 * (P6.3) verifies the banks as they cross the wire instead of in one pass;
 * the rule they check there is the rule bank_check() applies here. */
const char *orb_bundle_parse(const orb_bundle3_hdr_t *h, const uint8_t *file,
                             uint32_t file_size, orb_bundle_t *b);


/* The streaming installer's door (P6.3, §22.3). `region` is the geometry region
 * ALONE — a caller-sized buffer, at most ORB_BUNDLE_GEOM_CAP, which the caller
 * read out of the container — and `file_size` is the whole container's size as
 * the card reports it. Every check orb_bundle_parse() makes still runs here in
 * the same order except each bank's content checksum, because a bank's bytes are
 * not in this buffer; the caller checksums each one as it crosses the wire and
 * settles the debt with orb_bundle_bank_rule() below, or the install is not done.
 * Refusal sentences are identical to orb_bundle_parse()'s, byte for byte, so the
 * same damaged container says the same thing through either door.
 *
 * This is the shape the device uses: an internal-RAM chunk of 1-4 KB, a PSRAM
 * region buffer of at most 60 KB, and no path in which 4 MB of a card file sits in
 * memory waiting to be hashed. */
const char *orb_bundle_parse_region(const orb_bundle3_hdr_t *h, const uint8_t *region,
                                    uint32_t region_size, uint32_t file_size,
                                    orb_bundle_t *b);

/* The rule bank_check() applies, for a bank whose bytes arrived incrementally:
 * feed it the byte count and running crc32 of exactly the section's art_size
 * bytes, and it returns NULL or the same mismatch sentence the in-buffer path
 * returns. A count short of art_size is refused even if the crc happens to
 * match, because a truncated copy of the right prefix is not the bank. */
const char *orb_bundle_bank_rule(const orb_bundle_sec_t *s, uint32_t got_bytes,
                                 uint32_t got_crc);


/* Move a parsed bundle's banks to a new art base, in place: each record's
 * art_off shifts by the same delta, the region's checksum is recomputed over the
 * edited records, and h->art_off becomes new_art_off. `out_file_art_off` receives
 * the base the records carried BEFORE the move, which is what a caller streaming
 * from the container needs to seek — after the move the records describe the slot,
 * not the file. Rejects any bank the move would push past `slot_cap` (or below
 * zero), in a sentence; a refused rebase leaves nothing half-moved because the
 * checks precede the first write. The caller stamps geom_off itself: this
 * function knows about banks. */
const char *orb_bundle_rebase(orb_bundle_t *b, orb_bundle3_hdr_t *h, uint8_t *region,
                              uint32_t new_art_off, uint32_t slot_cap,
                              uint32_t *out_file_art_off);


/* The resolver's one lookup (D31's question stays a question here): exact
 * slug, or NULL. Hitting one is the app's own default question to answer. */
const orb_bundle_sec_t *orb_bundle_find(const orb_bundle_t *b,
                                        const char *slug);

/* The unpacked element row — the device's side of unpack_record(): fixed
 * 19-word rows, narrow fields masked from their word, the four i16 fields
 * sign-extended exactly as the C compositor will read them. Field order is
 * theme_schema.ELEMENT_FIELDS and the sentinel suite pins the order by
 * dumping a table whose every field carries a distinct sentinel. `kind` and
 * `src` are D32's ruling A, appended past scale16: both belong to a BANK row
 * and read zero on every other class, where they name nothing. */
typedef struct {
    uint16_t n;
    uint8_t  cls;
    uint8_t  flags;
    uint8_t  order;
    uint8_t  alpha;
    uint16_t slot;
    uint16_t frame0;
    uint16_t n_frames;
    int16_t  x, y;
    uint16_t w, h;
    int16_t  pivot_x, pivot_y;
    uint16_t radius, degrees, scale16;
    uint8_t  kind;   /* BANK only: CLK_KIND_* — how the plates are drawn  */
    uint8_t  src;    /* BANK only: CLK_SRC_*  — what value indexes the plates */
} orb_bundle_elem_t;

void orb_bundle_elem_at(const orb_bundle_sec_t *s, uint32_t i,
                        orb_bundle_elem_t *e);

/* The element table's row stride, in words, as the section's blob carries it:
 * orb_bundle_elem_at() and orb_bundle_spr_at() both land on it, and a table
 * baked before an appended column still addresses whole. 0 — a section not
 * filled by blob_check() — reads as the current schema's width. */
uint32_t orb_bundle_elem_stride(const orb_bundle_sec_t *s);

/* One row of the section's sprite catalogue — the planes its bank holds, in byte
 * order, each named by the element that draws it. `orb_bundle_elem_at()` gives a
 * row's frame0, and frame0 indexes THIS list: the catalogue is why a compositor
 * never has to guess whether a bank is contiguous or what a plane costs. Read
 * through the same blob_check() bounds as the element table, so an index the
 * record's n_spr did not vouch for is the caller's bug, not the file's. */
typedef struct {
    uint32_t off, size, w, h;
} orb_bundle_spr_t;

void orb_bundle_spr_at(const orb_bundle_sec_t *s, uint32_t i,
                       orb_bundle_spr_t *sp);

/* The scalars, opaque here (D31): the raw word by index, and the sign
 * extension for the two i8 fields (shadow_dx/dy, words 3/4) and the one i16
 * (bg_fps, word 10) that the clock dialect's readers need. */
uint32_t orb_bundle_scalar(const orb_bundle_sec_t *s, uint32_t idx);
int32_t  orb_bundle_scalar_sext(const orb_bundle_sec_t *s, uint32_t idx);

#ifdef __cplusplus
}
#endif
#endif /* ORB_BUNDLE_H */
