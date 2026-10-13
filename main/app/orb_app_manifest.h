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
#define ORBAPP_FORMAT     1u
#define ORBAPP_GEOM_ABI   1u             /* the dialect this build speaks */

#define ORBAPP_SLUG_MAX    24u   /* header bytes, NUL included */
#define ORBAPP_TITLE_MAX   32u
#define ORBAPP_ID_MAX      16u   /* settings id bytes; NVS key limit is 15+NUL */
#define ORBAPP_DEFSTR_MAX  32u

#define ORBAPP_SETTINGS_MAX   16u
#define ORBAPP_GEOM_BYTES_MAX 0x00040000u  /* 256 KB — the apps-partition sizing depends on it */
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
    ORBAPP_W_HDR_CRC    = ORBAPP_W_ART_CRC   + 1,
    ORBAPP_HDR_WORDS    = ORBAPP_W_HDR_CRC   + 4,   /* +4 words reserved for the next format */
    ORBAPP_HDR_BYTES    = ORBAPP_HDR_WORDS * 4,
    /* One settings descriptor on the wire, mirroring orb_setting_desc_t
     * field for field — the writer's S_* numbers (orbapp.py:172). */
    ORBAPP_S_ID = 0, ORBAPP_S_TYPE = 4, ORBAPP_S_MIN = 5, ORBAPP_S_MAX = 6,
    ORBAPP_S_DEF = 7, ORBAPP_S_STR = 8,
    ORBAPP_SETTINGS_WORDS = ORBAPP_S_STR + ((ORBAPP_DEFSTR_MAX + 3) / 4)  /* 16 */
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
} orbapp_hdr_t;

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
