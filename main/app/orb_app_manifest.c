/*
 * main/app/orb_app_manifest.c — the .orbapp reader; see the header for the
 * three-way mirror rule and why refusals are the writer's own sentences.
 *
 * Structured like hdr_parse() in main/art/orb_theme.c: cheap fixed-layout
 * refusals first, checksum before anything streamed, semantics last — a
 * header whose numbers do not add up is refused before one payload byte is
 * looked at. The one place this file is STRICTER than the writer's reader
 * (tools/orbapp.py's parse_header/check_header/unpack_settings) is marked
 * STRICTER below and pinned as such in tools/host_orbapp_reader_test.py's
 * vector table; nowhere is it looser.
 */
#include "orb_app_manifest.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_rom_crc.h"
#include "mbedtls/sha256.h"

/* Fill the caller's reason buffer printf-style and hand it back; every
 * refusal path ends in `return refuse(...)`. */
static const char *refuse(char *dst, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    return dst;
}

/* ^[a-z0-9_]{1,23}$ — the writer's SLUG_RE, and the same charset its
 * check_slug names in the refusal. One charset, three consumers (NVS
 * namespace, ?to= route, filename stem), so this predicate is the whole
 * identity rule and not an ad-hoc copy of it. */
static bool slug_ok(const char *s)
{
    size_t n = 0;
    for (; s[n]; n++) {
        const char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
        if (n + 1 >= ORBAPP_SLUG_MAX)   /* over 23 chars */
            return false;
    }
    return n >= 1;
}

/* app_id = first four bytes of sha256(UTF-8 slug), big-endian — the writer's
 * app_id_of byte for byte, and the same derivation the theme path runs at
 * orb_theme.c:342. The host suite pins the hash INPUT (bytes, length, is224
 * must stay 0) exactly as tools/host_sd_test.py's hook pins theme_id's. */
static uint32_t orbapp_app_id_of(const char *slug)
{
    unsigned char d[32];
    mbedtls_sha256((const unsigned char *)slug, strlen(slug), d, /*is224=*/0);
    return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
           ((uint32_t)d[2] << 8)  | (uint32_t)d[3];
}

/* Copy a word-packed string, requiring its NUL INSIDE the field. The writer
 * zero-pads every field and its validators cap the text one byte short, so a
 * field with no NUL is header damage, not overflow — and it is damage inside
 * the hdr_crc-covered block, which is why "damaged" is the true word for it
 * and the same sentence the checksum refusal uses. */
static const char *get_field(const uint32_t *w, unsigned base, unsigned nbytes,
                             char *dst, size_t dst_cap, const char *damaged)
{
    for (unsigned i = 0; i < nbytes; i++) {
        const char c = (char)((w[base + i / 4] >> (8 * (i % 4))) & 0xFF);
        if (!c) {
            if (i >= dst_cap) return damaged;   /* no room for the NUL at all */
            dst[i] = '\0';
            return NULL;
        }
        if (i + 1 >= dst_cap) return damaged;   /* no room left to terminate */
        dst[i] = c;
    }
    return damaged;   /* ran the whole field with no NUL */
}

/* ---- the fixed-layout refusals (the writer's parse_header) ----------------- */

const char *orbapp_parse_header(const uint32_t *w, orbapp_hdr_t *h,
                                char *reason, size_t cap)
{
    static const char DAMAGED[] =
        "container header checksum mismatch — re-copy the file onto the card";
    if (w[ORBAPP_W_MAGIC] != ORBAPP_MAGIC) {
        /* Named separately on purpose (the .clinerules:453 confusion the two
         * extensions exist to prevent): "that is a theme" is a truer reason
         * than "that is not a container" when the owner handed over an
         * .orbtheme. */
        if (w[ORBAPP_W_MAGIC] == ORBAPP_THEME_MAGIC)
            return refuse(reason, cap,
                          "not an .orbapp file (that is an .orbtheme: themes live"
                          " in /sd/themes and the OS package manager installs"
                          " them alone)");
        return refuse(reason, cap, "not an .orbapp file");
    }
    if (w[ORBAPP_W_FORMAT] != ORBAPP_FORMAT)
        return refuse(reason, cap, "app format %u; this build reads format %u",
                      w[ORBAPP_W_FORMAT], (unsigned)ORBAPP_FORMAT);
    if (w[ORBAPP_W_HDR_SIZE] != ORBAPP_HDR_BYTES)
        return refuse(reason, cap, "header claims %u B; this format is %u B",
                      w[ORBAPP_W_HDR_SIZE], (unsigned)ORBAPP_HDR_BYTES);

    /* Self-checksum before any size field downstream is trusted: it covers
     * HDR_BYTES, so a torn header that claims a 4 MB payload dies here and
     * not after the payload is streamed (orb_theme.c's rule, followed to the
     * letter). */
    uint32_t zeroed[ORBAPP_HDR_WORDS];
    memcpy(zeroed, w, sizeof(zeroed));
    zeroed[ORBAPP_W_HDR_CRC] = 0;
    if (w[ORBAPP_W_HDR_CRC] !=
        esp_rom_crc32_le(0, (const uint8_t *)zeroed, sizeof(zeroed)))
        return refuse(reason, cap, "%s", DAMAGED);

    if (w[ORBAPP_W_SIG] != 0)
        return refuse(reason, cap,
                      "sig is non-zero and this build verifies no signatures"
                      " (C3): refusing to install bytes nobody vouched for");

    h->fmt            = w[ORBAPP_W_FORMAT];
    h->abi            = w[ORBAPP_W_GEOM_ABI];
    h->hdr_size       = w[ORBAPP_W_HDR_SIZE];
    h->serial         = w[ORBAPP_W_SERIAL];
    h->baked_ms       = w[ORBAPP_W_BAKED_MS];
    h->abi_min        = w[ORBAPP_W_ABI_MIN];
    h->abi_max        = w[ORBAPP_W_ABI_MAX];
    h->sig            = w[ORBAPP_W_SIG];
    h->app_id         = w[ORBAPP_W_APP_ID];
    h->behaviour_kind = w[ORBAPP_W_BEHAVIOUR];
    h->art_req        = w[ORBAPP_W_ART_REQ];
    h->n_layer        = w[ORBAPP_W_N_LAYER];
    h->n_comp         = w[ORBAPP_W_N_COMP];
    h->n_aux          = w[ORBAPP_W_N_AUX];
    h->n_bank         = w[ORBAPP_W_N_BANK];
    h->period_ms      = w[ORBAPP_W_PERIOD_MS];
    h->caps           = w[ORBAPP_W_CAPS];
    h->n_settings     = w[ORBAPP_W_N_SETTINGS];
    h->setting_off    = w[ORBAPP_W_SET_OFF];
    h->geom_off       = w[ORBAPP_W_GEOM_OFF];
    h->geom_size      = w[ORBAPP_W_GEOM_SIZE];
    h->geom_crc       = w[ORBAPP_W_GEOM_CRC];
    h->art_off        = w[ORBAPP_W_ART_OFF];
    h->art_size       = w[ORBAPP_W_ART_SIZE];
    h->art_crc        = w[ORBAPP_W_ART_CRC];

    /* The writer always NUL-pads inside the field, so an unterminated field
     * is the same class of damage the CRC above answers for — and unlike a
     * raw _getstr, stopping at the buffer edge is the one bug a device reader
     * may not have: these two fields are the NVS namespace and the label
     * under the carousel row. */
    const char *e = get_field(w, ORBAPP_W_SLUG, ORBAPP_SLUG_MAX,
                              h->slug, sizeof h->slug, DAMAGED);
    if (!e)
        e = get_field(w, ORBAPP_W_TITLE, ORBAPP_TITLE_MAX,
                      h->title, sizeof h->title, DAMAGED);
    if (e)
        return refuse(reason, cap, "%s", e);
    return NULL;
}

/* ---- the semantic refusals (the writer's check_header) --------------------- */

/* The filename's side of the identity triple — the writer's
 * filed_as_its_own_slug. Case-insensitive on the extension, exact on the
 * stem, and refusing a .orbtheme or .orbapp.part by NAME and not by magic
 * alone: the two containers share a card, a page and an owner, and "that is
 * a theme" is a truer reason than "that is not a container". */
static bool ends_ic(const char *s, size_t n, const char *suf)
{
    size_t m = strlen(suf);
    if (n < m)
        return false;
    for (size_t i = 0; i < m; i++) {
        char a = s[n - m + i];
        char b = suf[i];
        if (a >= 'A' && a <= 'Z') a += (char)('a' - 'A');
        if (a != b)
            return false;
    }
    return true;
}

static const char *check_stem(const char *name, const orbapp_hdr_t *h,
                              char *reason, size_t cap)
{
    const size_t n = strlen(name);
    const char *ext = NULL;
    if (ends_ic(name, n, ".orbtheme"))
        return refuse(reason, cap, "%s is a theme, not an app: themes live in"
                      " /sd/themes and the OS package manager alone installs"
                      " them", name);
    if (ends_ic(name, n, ".orbapp.part"))
        return refuse(reason, cap, "%s is a staging name: a .part file is"
                      " invisible to the scan by design and nothing is ever"
                      " committed under it", name);
    if (ends_ic(name, n, ".orbapp"))
        ext = ".orbapp";
    else
        return refuse(reason, cap, "%s does not end in .orbapp", name);

    char stem[ORBAPP_SLUG_MAX];
    const size_t k = n - strlen(ext);
    if (k >= sizeof stem)
        return refuse(reason, cap,
                      "app id '%s' is not lowercase a-z 0-9 _ of 1..%u chars:"
                      " it is the NVS namespace, the ?to= route and the"
                      " filename stem at once, and a character any of those"
                      " three treats specially installs an app that cannot be"
                      " found again", name, (unsigned)ORBAPP_SLUG_MAX - 1);
    memcpy(stem, name, k);
    stem[k] = '\0';
    if (!slug_ok(stem))
        return refuse(reason, cap,
                      "app id '%s' is not lowercase a-z 0-9 _ of 1..%u chars:"
                      " it is the NVS namespace, the ?to= route and the"
                      " filename stem at once, and a character any of those"
                      " three treats specially installs an app that cannot be"
                      " found again", stem, (unsigned)ORBAPP_SLUG_MAX - 1);
    if (strcmp(stem, h->slug) != 0)
        return refuse(reason, cap, "filename stem '%s' is not the header's"
                      " slug '%s': the app would install where nothing looks"
                      " for it", stem, h->slug);
    /* The writer's last leg — app_id_of(stem) against the header's app_id —
     * is unreachable here: slug already equals the stem and the caller has
     * checked app_id against slug. */
    return NULL;
}

const char *orbapp_check_header(const orbapp_hdr_t *h, uint32_t expect_app_id,
                                bool have_expect, const char *stem,
                                char *reason, size_t cap)
{
    /* The C4 ABI range, checked against THIS build's dialect. The writer's
     * reader checks the header's own abi against its own declared range —
     * sound for a file the writer just stamped — and a reader that only
     * copied that would install an app whose geometry words it reads wrong.
     * So: the declared range must cover this build's number, and the stamped
     * abi must be it. STRICTER side of the mirror, pinned as such in
     * tools/host_orbapp_reader_test.py. */
    if (h->abi != ORBAPP_GEOM_ABI ||
        !(h->abi_min <= ORBAPP_GEOM_ABI && ORBAPP_GEOM_ABI <= h->abi_max))
        return refuse(reason, cap,
                      "app needs geometry ABI %u..%u; this build speaks %u —"
                      " re-package it (C4)", h->abi_min, h->abi_max,
                      (unsigned)ORBAPP_GEOM_ABI);

    if (!h->slug[0])
        return refuse(reason, cap,
                      "app declares no slug in its header — re-package it");
    if (!slug_ok(h->slug))
        return refuse(reason, cap,
                      "app id '%s' is not lowercase a-z 0-9 _ of 1..%u chars:"
                      " it is the NVS namespace, the ?to= route and the"
                      " filename stem at once, and a character any of those"
                      " three treats specially installs an app that cannot be"
                      " found again", h->slug, (unsigned)ORBAPP_SLUG_MAX - 1);
    if (!h->title[0])
        return refuse(reason, cap,
                      "app declares no title: the install confirm and the"
                      " carousel row both read this field, and an empty one"
                      " is an unlabeled permission request");

    /* The D19 consent check, not a range check: an over-declared cap is a
     * permission the owner never read. Bits come from orb_svc.h's enum —
     * CAP_N and CAP_FREE_MASK — so the word map has one source per side and
     * the sentinel lint is the pin that the two sides still agree. */
    const uint32_t owner_mask = ((1u << CAP_N) - 1u) & ~CAP_FREE_MASK;
    if ((h->caps >> CAP_N) || (h->caps & ~owner_mask))
        return refuse(reason, cap,
                      "header caps 0x%X are not a legal declaration — owner"
                      " caps only (power, net, sd_read, themes), and nothing"
                      " at or above CAP_N", h->caps);

    const uint32_t want = orbapp_app_id_of(h->slug);
    if (h->app_id != want)
        return refuse(reason, cap,
                      "header app_id 0x%08x is not the slug '%s' it claims"
                      " (that slug is 0x%08x)", h->app_id, h->slug, want);
    if (have_expect && h->app_id != expect_app_id)
        return refuse(reason, cap,
                      "header app_id 0x%08x is not the 0x%08x its caller"
                      " expects", h->app_id, expect_app_id);
    if (stem) {
        const char *e = check_stem(stem, h, reason, cap);
        if (e)
            return e;
    }

    if (h->n_settings > ORBAPP_SETTINGS_MAX)
        return refuse(reason, cap, "%u settings declared; the loader's table"
                      " holds %u", h->n_settings, (unsigned)ORBAPP_SETTINGS_MAX);
    /* D21 (§0.9): no container carries art. The payload words are a shape to
     * refuse, not a size to price — a file shipping bytes at art_off predates
     * the ruling and must be re-baked. art_req alone survives: it is D18's
     * request-as-declaration, answered by the theme and asset registries, and
     * no container byte depends on it, so the old slot cap bounds it for
     * plausibility only. The writer's check_header mirrors these two refusals
     * in the same words. */
    if (h->art_size != 0 || h->art_crc != 0)
        return refuse(reason, cap, "carries a %u B art payload: no container "
                      "ships art (D21) — re-package it", h->art_size);
    if (h->art_req > ORBAPP_ART_BYTES_MAX)
        return refuse(reason, cap, "declares an art need of %u B past the %u B "
                      "ever budgeted — a demand that size is a miscount",
                      h->art_req, (unsigned)ORBAPP_ART_BYTES_MAX);
    if (h->geom_size > ORBAPP_GEOM_BYTES_MAX || (h->geom_size % 4))
        return refuse(reason, cap, "geometry blob is %u B: past the cap, or"
                      " not a whole number of words", h->geom_size);

    /* The counts decide the blob's length, so a header whose numbers do not
     * add up is refused before one byte of payload is streamed — the sums
     * run in 64 bits because four u32s can overflow where Python's ints
     * cannot; the refusal below then reads as the same rule. */
    const uint64_t need = ((uint64_t)h->n_layer + h->n_comp + h->n_aux +
                           h->n_bank) * 4;
    if ((uint64_t)h->geom_size < need)
        return refuse(reason, cap, "geometry blob is %u B but its counts need"
                      " %llu", h->geom_size,
                      (unsigned long long)(need > 0xFFFFFFFFull
                                           ? 0xFFFFFFFFull : need));

    /* D21 inverted the pair that lived here: art_req with no payload USED to be
     * the refusal (a reservation nothing mapped starved the bank budget). With
     * no payload to map the word can only be a declaration, and a declaration
     * needs no backing store — this is now the container's expected shape, and
     * any art_size is already gone refused above. */

    /* D16's floor, and only the floor: the writer refuses a too-low beat at
     * pack time and its own reader re-checks it on cards; the too-high end
     * dies at the writer alone, so a crafted period above a day is the
     * scheduler's clamp to answer, not a card-reading refusal. */
    if (h->period_ms != 0 && h->period_ms < ORBAPP_BEAT_MIN_MS)
        return refuse(reason, cap, "period_ms %u is below the %u ms the OS can"
                      " commit (D16)", h->period_ms,
                      (unsigned)ORBAPP_BEAT_MIN_MS);
    return NULL;
}

/* ---- the file-layout rule and the settings table --------------------------- */

const char *orbapp_check_file_layout(const orbapp_hdr_t *h, char *reason,
                                     size_t cap)
{
    if (h->setting_off != ORBAPP_HDR_BYTES)
        return refuse(reason, cap,
                      "the settings table is not where this format puts it");
    if (h->geom_off != (uint64_t)ORBAPP_HDR_BYTES +
                       (uint64_t)h->n_settings * ORBAPP_SETTINGS_WORDS * 4)
        return refuse(reason, cap,
                      "the geometry blob does not follow the settings table");
    const uint64_t art = ((uint64_t)h->geom_off + h->geom_size + 4095) &
                         ~(uint64_t)4095;      /* SECTOR, orbapp.py:499 */
    if ((uint64_t)h->art_off != art)
        return refuse(reason, cap,
                      "the payload is not sector-aligned off the geometry blob");
    return NULL;
}

const char *orbapp_unpack_settings(const uint32_t *words, size_t nwords,
                                   uint32_t n, orb_setting_desc_t out[],
                                   char *reason, size_t cap)
{
    if (n > ORBAPP_SETTINGS_MAX)
        return refuse(reason, cap, "%u settings; the loader's table holds %u",
                      n, (unsigned)ORBAPP_SETTINGS_MAX);
    const size_t need = (size_t)n * ORBAPP_SETTINGS_WORDS;
    if (nwords < need)
        return refuse(reason, cap, "settings blob is %u B, short of the %u B"
                      " its count claims", (unsigned)(nwords * 4),
                      (unsigned)(need * 4));
    static const char DAMAGED[] =
        "container header checksum mismatch — re-copy the file onto the card";
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t *b = words + (size_t)i * ORBAPP_SETTINGS_WORDS;
        memset(&out[i], 0, sizeof out[i]);
        const uint32_t t = b[ORBAPP_S_TYPE];
        if (t > (uint32_t)ORB_SET_STRING)      /* C9's four, frozen */
            return refuse(reason, cap, "setting %u has type word %u, outside"
                          " C9's frozen 4", i, t);
        out[i].type = (orb_set_type_t)t;
        const char *e = get_field(b, ORBAPP_S_ID, ORBAPP_ID_MAX, out[i].id,
                                  sizeof out[i].id, DAMAGED);
        if (!e)
            e = get_field(b, ORBAPP_S_STR, ORBAPP_DEFSTR_MAX, out[i].def_str,
                          sizeof out[i].def_str, DAMAGED);
        if (e)
            return refuse(reason, cap, "%s", e);
        out[i].min = (int32_t)b[ORBAPP_S_MIN]; /* the wire's u32, the struct's */
        out[i].max = (int32_t)b[ORBAPP_S_MAX]; /* int32_t — the writer's _s32 */
        out[i].def = (int32_t)b[ORBAPP_S_DEF]; /* agrees by being a bit-copy  */
    }
    return NULL;
}

int orbapp_caps_names(uint32_t caps, char *dst, size_t cap)
{
    static const char *const NAMES[4] = { "power", "net", "sd_read", "themes" };
    size_t len = 0;
    if (cap == 0)
        return -1;
    dst[0] = '\0';
    for (unsigned i = 0; i < 4; i++) {
        if (!(caps & (1u << (CAP_POWER + i))))
            continue;
        const size_t n = strlen(NAMES[i]);
        if (len + (len ? 2 : 0) + n + 1 > cap)
            return -1;                          /* caller refuses, never truncates */
        if (len) { dst[len++] = ','; dst[len++] = ' '; }
        memcpy(dst + len, NAMES[i], n);
        len += n;
    }
    dst[len] = '\0';
    return (int)len;
}
