/*
 * main/app/orb_app_manifest.h — the .orbapp container reader (P4).
 *
 * The validation half of the app install path: header decode, the semantic
 * refusals, and the settings-descriptor unpack. NO I/O lives here — the
 * installer reads the bytes (from a card file during scan, from an `apps`
 * partition slot after install) and hands them over; every function takes a
 * finished buffer. That split is what lets tools/orbapp.py face the same
 * rules from the host side: the suite feeds the shipping code below the
 * writer's own output and both must agree without a partition or an SD slot
 * in sight.
 *
 * THE THREE-WAY MIRROR (tools/theme_schema.py's rule, inherited verbatim by
 * this container — see tools/orbapp.py's header): this file, tools/orbapp.py,
 * and tools/host_orbapp_reader_test.py move together. A change to a cap, a
 * range, or a refusal on one side that does not appear on all three is a
 * silent contract break between author and device.
 *
 * Refusal strings are the writer's own sentences, character for character.
 * They are not debug output: each one is what the page shows next to the
 * file the owner just copied onto the card (the orb_theme.c convention).
 */
#ifndef ORB_APP_MANIFEST_H
#define ORB_APP_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/orb_svc.h"   /* orb_setting_desc_t — the loader's table, unpack target */

#ifdef __cplusplus
extern "C" {
#endif

/* The container's own numbers, mirrored from tools/orbapp.py. The sentinel
 * suite re-derives every one from the writer's source text and fails on any
 * disagreement — the lint rule of tools/host_art_residency_test.py. */
#define ORBAPP_MAGIC      0x00415247u    /* 'G','R','A' — unlike the theme magic on purpose */
#define ORBAPP_THEME_MAGIC 0x474F5254u   /* so a theme handed here gets named, not miscalled */
#define ORBAPP_FORMAT     3u    /* 2 -> 3 is S6 (§4.1): a 32-byte version field
                                 * after the editor triple, inside the hdr_crc span.
                                 * The tail's free word could not take it — a word
                                 * outside the CRC is rewritable on the card without
                                 * disturbing any checksum, which is the failure
                                 * D19's consent rule exists to forbid, and a 31-byte
                                 * stamp is not one word anyway. GEOM_ABI stays 3:
                                 * the class vocabulary did not move, only the
                                 * header's shape did, and the two numbers answer
                                 * two different questions. */
#define ORBAPP_GEOM_ABI   3u    /* The dialect this build speaks. 1 -> 2 is P5b's
                                 * asset table (D23): the geometry records did not
                                 * move, the vocabulary did. D23 names THIS word as
                                 * the class enum's version, so a second number that
                                 * could contradict it is not added — and an app
                                 * that needs a class this dial cannot name dies on
                                 * the same abi_min/abi_max ladder as everything
                                 * else, with the same sentence.
                                 * 2 -> 3 is D37's editor region: three new header
                                 * words inside the hdr_crc span. A container that
                                 * carries one is meaningless to an ABI-2 reader,
                                 * which would install the app and never see the
                                 * bytes it shipped for — so it is refused at the
                                 * door, naming this number, rather than half-run.
                                 * The bump costs the two factory containers their
                                 * install until re-baked; that is the ruling, and
                                 * Migration says it out loud (§4.1). */

#define ORBAPP_SLUG_MAX    24u   /* header bytes, NUL included */
#define ORBAPP_TITLE_MAX   32u
#define ORBAPP_VERSION_MAX 32u   /* S6: header bytes, NUL included — 31 usable chars */
#define ORBAPP_ID_MAX      16u   /* settings id bytes; NVS key limit is 15+NUL */
#define ORBAPP_DEFSTR_MAX  32u

#define ORBAPP_SETTINGS_MAX   16u
#define ORBAPP_EDITOR_MAX     0x200000u  /* D37: 2 MiB of editor, and no more. The
                                          * cap is the owner's ruling, and it is a
                                          * real one: the region NEVER installs, so
                                          * it prices nothing in flash — its job is
                                          * to stop one container filling the card.
                                          * The slot's 256 KB budget (GEOM_BYTES_MAX)
                                          * is the bill for everything that DOES
                                          * install, and this region is not on that
                                          * bill: that is the whole design (§4.2). */
#define ORBAPP_ASSETS_MAX     16u   /* P5b (D23): the asset table's cap, twin of
                                     * ORB_ASSET_MAX. 16 rows of 6 words = 384 B, and
                                     * the worst-case blob (settings + strings +
                                     * assets) fits the slot header's 4 KB slack. That
                                     * arithmetic is the only reason the cap exists: a
                                     * table that did not fit the region its own slot
                                     * CRC reaches would be checked by nobody. */
#define ORBAPP_ASSET_NAME_MAX 12u   /* bytes incl NUL, and NO prefix in them: the row
                                     * carries a namespace byte, so a global slot is
                                     * stored as `plate` and not `global/plate`. Nobody
                                     * reconstructs a prefix to strip it back off, and
                                     * `global/` + 12 still fits ORB_ASSET_REF_MAX. */
#define ORBAPP_GEOM_BYTES_MAX 0x00040000u  /* 256 KB — the apps-partition sizing depends on it */
#define ORBAPP_CODE_BYTES_MAX 0x00040000u  /* 256 KB — P7 (D20): a wasm module rides the
 * slot's payload region the geometry shares, so one slot budget bounds both and
 * install_decide prices the whole copied prefix against it */
#define ORBAPP_WASM_MEM_MAX   0x00020000u  /* 128 KB — D27: the linear-memory ceiling an
 * app may declare. Gated at install in the same shape as caps and period_ms, and it
 * sits INSIDE the header CRC so a card cannot raise it unseen */

#define ORBAPP_ART_BYTES_MAX  0x003D0000u  /* 3.81 MB — plausibility bound on the
                                            * art_req DECLARATION only: D21 (§0.9)
                                            * says no container carries art bytes */

/* D16: the manifest's period_ms is the FASTEST beat the app may run. 0 means
 * never ticks; the floor is the commit pass measured at 185 ms, priced at
 * 3000 by the writer. The reader refuses under it so a card cannot smuggle
 * in a beat the shared scheduler cannot honour. */
#define ORBAPP_BEAT_MIN_MS 3000u
#define ORBAPP_BEAT_MAX_MS 86400000u


/* The header word map, derived from the field widths exactly as the writer
 * derives its own (orbapp.py:137-168): a width change moves both sides'
 * layout together or the sentinel lint goes red. Never spell a word
 * position twice. */
enum {
    ORBAPP_W_MAGIC = 0, ORBAPP_W_FORMAT = 1, ORBAPP_W_GEOM_ABI = 2,
    ORBAPP_W_HDR_SIZE = 3, ORBAPP_W_SERIAL = 4, ORBAPP_W_BAKED_MS = 5,
    ORBAPP_W_ABI_MIN = 6, ORBAPP_W_ABI_MAX = 7, ORBAPP_W_SIG = 8,
    ORBAPP_W_APP_ID = 9,
    ORBAPP_W_SLUG = 10,
    ORBAPP_W_TITLE      = ORBAPP_W_SLUG      + ((ORBAPP_SLUG_MAX   + 3) / 4),
    ORBAPP_W_BEHAVIOUR  = ORBAPP_W_TITLE     + ((ORBAPP_TITLE_MAX  + 3) / 4),
    ORBAPP_W_ART_REQ    = ORBAPP_W_BEHAVIOUR + 1,
    ORBAPP_W_N_LAYER    = ORBAPP_W_ART_REQ   + 1,
    ORBAPP_W_N_COMP     = ORBAPP_W_N_LAYER   + 1,
    ORBAPP_W_N_AUX      = ORBAPP_W_N_COMP    + 1,
    ORBAPP_W_N_BANK     = ORBAPP_W_N_AUX     + 1,
    ORBAPP_W_PERIOD_MS  = ORBAPP_W_N_BANK    + 1,
    ORBAPP_W_CAPS       = ORBAPP_W_PERIOD_MS + 1,
    ORBAPP_W_N_SETTINGS = ORBAPP_W_CAPS      + 1,
    ORBAPP_W_SET_OFF    = ORBAPP_W_N_SETTINGS + 1,
    ORBAPP_W_GEOM_OFF   = ORBAPP_W_SET_OFF   + 1,
    ORBAPP_W_GEOM_SIZE  = ORBAPP_W_GEOM_OFF  + 1,
    ORBAPP_W_GEOM_CRC   = ORBAPP_W_GEOM_SIZE + 1,
    ORBAPP_W_ART_OFF    = ORBAPP_W_GEOM_CRC  + 1,
    ORBAPP_W_ART_SIZE   = ORBAPP_W_ART_OFF   + 1,
    ORBAPP_W_ART_CRC    = ORBAPP_W_ART_SIZE  + 1,
    /* P7 (D20/D27): the code region's three words and the gate's linear-memory
     * ceiling, inserted INSIDE the header CRC's span so a card cannot rewrite them
     * without disturbing the checksum — D27 gates wasm_mem_max "in the same shape"
     * as caps and period_ms. They sit contiguously after the art triple, so the
     * asset table and the one free word keep deriving from the moved HDR_CRC. */
    ORBAPP_W_CODE_OFF   = ORBAPP_W_ART_CRC   + 1,
    ORBAPP_W_CODE_SIZE  = ORBAPP_W_CODE_OFF  + 1,
    ORBAPP_W_CODE_CRC   = ORBAPP_W_CODE_SIZE + 1,
    ORBAPP_W_WASM_MEM   = ORBAPP_W_CODE_CRC  + 1,
    /* D37 (§4.1): the editor region's three words, inserted INSIDE the header
     * CRC's span for the same reason P7 put the code triple there — a placement
     * or checksum word that lived outside hdr_crc could be rewritten on the card
     * without disturbing any checksum, and the editor is the one region in this
     * container a stranger is invited to author. They sit contiguously after the
     * code quadruple, so the asset table and the one free word keep deriving from
     * the (moved) HDR_CRC. All three read zero on a container that ships no
     * editor, which is "ships nothing" and not a refusal — every app that ever
     * shipped before D37 answers that way. */
    ORBAPP_W_EDITOR_OFF  = ORBAPP_W_WASM_MEM  + 1,
    ORBAPP_W_EDITOR_SIZE = ORBAPP_W_EDITOR_OFF  + 1,
    ORBAPP_W_EDITOR_CRC  = ORBAPP_W_EDITOR_SIZE + 1,
    /* S6 (§4.1): the app's own release stamp — eight words, NUL-padded text, the
     * same 32-byte field esp_app_desc_t.version gives a firmware image. It sits
     * BEFORE ORBAPP_W_HDR_CRC for the reason the code triple and the editor triple
     * sit there: a word after the checksum can be rewritten on the card without
     * disturbing any checksum, and a version an operator can silently edit is not
     * an identity. An ALL-ZERO field is legal and reads as "unstamped", the way a
     * zero editor_size reads as "ships nothing" — a zero claims no ordering and
     * revokes no permission, so it needs no refusal. A NON-zero field must parse,
     * and orbapp_parse_header refuses one that does not. */
    ORBAPP_W_VERSION     = ORBAPP_W_EDITOR_CRC + 1,
    ORBAPP_W_HDR_CRC     = ORBAPP_W_VERSION    + 8,
    /* P5b's two fields take words the header left reserved for exactly this, so the
     * tail keeps its shape and the header CRC still covers up to W_HDR_CRC. One word
     * at the end stays free, and the reader checks it zero: a word that reads as
     * nothing today is a word that reads as something wrong tomorrow. The table
     * itself lives in the blob region, where the slot CRC already reaches it. A
     * container written before any app declared a slot leaves both zero, which reads
     * as "declares nothing": the truth for every app that ever shipped (D25), and the
     * one answer in this table that is not a refusal. */
    ORBAPP_W_ASSET_OFF  = ORBAPP_W_HDR_CRC   + 1,
    ORBAPP_W_N_ASSETS   = ORBAPP_W_ASSET_OFF + 1,
    ORBAPP_HDR_WORDS    = ORBAPP_W_HDR_CRC   + 4,   /* asset_off, n_assets, 1 free */
    ORBAPP_HDR_BYTES    = ORBAPP_HDR_WORDS * 4,
    /* One settings descriptor on the wire, mirroring orb_setting_desc_t
     * field for field — the writer's S_* numbers (orbapp.py:172). */
    ORBAPP_S_ID = 0, ORBAPP_S_TYPE = 4, ORBAPP_S_MIN = 5, ORBAPP_S_MAX = 6,
    ORBAPP_S_DEF = 7, ORBAPP_S_STR = 8,
    ORBAPP_SETTINGS_WORDS = ORBAPP_S_STR + ((ORBAPP_DEFSTR_MAX + 3) / 4),  /* 16 */
    /* One asset row (D23): a class byte, D31's selector byte, a namespace byte, and
     * a name carrying no prefix. Then the box — a slot the OS composites still has
     * to say where it is, and the box is the only geometry a container may state
     * about a slot it ships no pixels for. Words 4 and 5 are x|y and w|h, so a row
     * the dial cannot place is a refusal at read and not a slot parked at (0,0). */
    ORBAPP_A_CLASS = 0,   /* byte 0: an index into orb_asset_class_t             */
    ORBAPP_A_REQ   = 1,   /* byte 1: D31's selector                              */
    ORBAPP_A_NS    = 2,   /* byte 2: 0 owner-scoped, 1 the global/ namespace     */
    ORBAPP_A_NAME  = 1,   /* words 1..3: ORBAPP_ASSET_NAME_MAX bytes             */
    ORBAPP_A_XY    = 4,   /* x int16 | y int16                                   */
    ORBAPP_A_WH    = 5,   /* w u16   | h u16                                     */
    ORBAPP_ASSET_WORDS = 6            /* the row is exactly six words. Byte 3 of word 0
                                       * is the row's reserved byte and the reader
                                       * checks it zero, which is where a seventh word
                                       * would start */
};

/* The decoded header — the writer's parse_header dict, flattened. The scan
 * keeps these on the stack; the installer copies the decided fields into the
 * registry record and, under D19, into orb_app_t.caps. */
typedef struct {
    uint32_t fmt, abi, abi_min, abi_max, hdr_size;
    uint32_t serial, baked_ms, sig;
    uint32_t app_id;
    char     slug[ORBAPP_SLUG_MAX];
    char     title[ORBAPP_TITLE_MAX];
    uint32_t behaviour_kind, art_req;
    uint32_t n_layer, n_comp, n_aux, n_bank;
    uint32_t period_ms, caps, n_settings;
    uint32_t setting_off, geom_off, geom_size, geom_crc;
    uint32_t art_off, art_size, art_crc;
    uint32_t code_off, code_size, code_crc, wasm_mem_max;  /* P7 (D20/D27): the code
                                                            * region + its D27 bound */
    uint32_t editor_off, editor_size, editor_crc;  /* D37: the author-facing payload,
                                                   * card-resident and never installed
                                                   * (§4.2). Three zeros: ships none. */
    uint32_t asset_off, n_assets;   /* P5b (D23): the table, in the blob region    */
    /* S6 (§4.1): the app's own release stamp, NUL-terminated and empty when the
     * container names no release. The one field in this header that is prose and
     * not a gate word, and the reason it still sits inside hdr_crc: the hub prints
     * it as the app's version, so a value an operator could edit on the card
     * unnoticed is a lie on a page. */
    char     version[ORBAPP_VERSION_MAX];
} orbapp_hdr_t;

/* The class vocabulary the table's rows index into is main/assets/orb_assets.h's
 * frozen enum, included and not copied: the wire carries an index and a namespace
 * byte, never a string, so there is exactly one list of class names in the tree.
 * The include sits here because the manifest reader is what turns those bytes into
 * an orb_asset_decl_t, and that translation is where a drift would live. */
#include "orb_assets.h"

/* Decode + the fixed-layout refusals: not a container (a theme is NAMED,
 * per .clinerules:453), a newer FORMAT, a header size that is not this
 * format's, a torn checksum, a non-zero sig (C3 verifies nothing). `w` points
 * at ORBAPP_HDR_WORDS little-endian words. NULL on success; otherwise the
 * reason, shown verbatim. */
const char *orbapp_parse_header(const uint32_t *w, orbapp_hdr_t *h,
                                char *reason, size_t cap);

/* The semantic refusals on a CRC-passing header — the writer's check_header:
 * the C4 ABI range, the identity triple (slug charset, app_id =
 * sha256(slug)[:4] big-endian, the optional expected id and filename stem),
 * the caps declaration, the count sum, and the payload/beat bounds. The
 * caller decides file layout; these rules hold in a slot too. `stem` may be
 * NULL (a slot read has no filename to cross-check). */
const char *orbapp_check_header(const orbapp_hdr_t *h, uint32_t expect_app_id,
                                bool have_expect, const char *stem,
                                char *reason, size_t reason_cap);

/* The container FILE's own placement rule, which a slot read does not share —
 * the writer's check_file_layout. */
const char *orbapp_check_file_layout(const orbapp_hdr_t *h, char *reason,
                                     size_t reason_cap);

/* Unpack n wire descriptors into the loader's own table — the writer's
 * unpack_settings. Refuses a type word outside C9's frozen four, an
 * unterminated id or def_str, and a table short of what the count claims. */
const char *orbapp_unpack_settings(const uint32_t *words, size_t nwords,
                                   uint32_t n, orb_setting_desc_t out[],
                                   char *reason, size_t reason_cap);

/* Unpack n asset rows into the loader's own table — the D23/D31 half of the
 * manifest, and the sibling of orbapp_unpack_settings for exactly the reason that
 * one exists: the caller must not hand the registry a reinterpretation of the
 * wire. Refuses a class byte the frozen enum does not carry, a namespace byte
 * outside {0,1}, a reserved word that is not zero, an unterminated or too-long
 * name, a box the panel cannot hold, and a table short of what the count claims.
 * An empty name on a row is a refusal and not a blank slot: a slot nobody can
 * name cannot be resolved, cannot be refused by name, and would be filed under
 * the empty string.
 *
 * `slug` is the app's OWN slug, cross-checked against every owner-scoped row the
 * same way check_header cross-checks the filename stem — the reference's owner is
 * the identity triple's third member, so a container that names another app's
 * slot is refused here, before the first erase (D26), and not discovered by the
 * resolver on a tick. NULL skips the cross-check, which is what the scan does: it
 * has no card row's identity to compare against, and it must not invent one. */
const char *orbapp_unpack_assets(const uint32_t *words, size_t nwords,
                                 uint32_t n, const char *slug,
                                 orb_asset_decl_t out[],
                                 char *reason, size_t reason_cap);

/* D26's answer over a whole table: resolve every declaration in the one order the
 * registry owns and report the FIRST refusal. Thin over orb_asset_resolve_all on
 * purpose — the install path must not grow a second precedence, and a second
 * precedence is a second truth about what the owner's theme says. Returns the
 * answer; `why` carries the sentence naming the reference that failed and the
 * class it was declared as. */
orb_asset_ans_t orbapp_check_assets(const orb_asset_decl_t *decls, int n,
                                   const char *slug, char *why, size_t cap);

/* The declared owner caps in orb_cap_t order, e.g. "power, net" — the
 * disclosure's may-use line, from the same bits the gate consults. Returns
 * the length written, or -1 if dst is too small: a truncated disclosure is
 * refused by the caller, never shown. caps here must have passed
 * orbapp_check_header, so free bits cannot appear. */
int orbapp_caps_names(uint32_t caps, char *dst, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* ORB_APP_MANIFEST_H */
