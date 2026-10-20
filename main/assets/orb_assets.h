/*
 * main/assets/orb_assets.h — the asset registry (D23, D22, D26, D31; plan P5b).
 *
 * WHERE AN APP'S PIXELS COME FROM. A container never carries art (D21), so every
 * pixel an app draws arrives through this file: a named slot in the live theme,
 * the app's own generated default, or the board's fallback. This is the one place
 * that decides among the three, and it is why main/art/orb_theme.c no longer
 * answers "how many bytes is this slot" by itself (§1.4:253).
 *
 * TWO HALVES, DIFFERENT GROWTH RULES (D23). The class vocabulary is a frozen C
 * enum: a class enters by PR and never by bytes, and the append-only rule that
 * governs orb_cap_t and ORB_BEHAVIOUR_* governs it — never renumbered, never
 * reused. Its version IS the container's dialect word: an app that needs a class
 * this build does not speak dies on the abi_min/abi_max ladder the geometry blob
 * already rides (orb_app_manifest.c:245-249), so the vocabulary carries no version
 * of its own. ORB_ASSET_CLASS_ABI is pinned equal to ORBAPP_GEOM_ABI by
 * tools/host_orbapp_test.py; moving one and not the other fails there.
 *
 * The named slots grow per app, in the manifest's asset table (§4.1), and they
 * are owner-scoped: `<slug>/<name>`, with `global/<name>` for the frozen set.
 * Nothing else parses, so one app's `bg` cannot shadow another's.
 *
 * THREE TIERS, THREE DISTINCT ANSWERS. Theme content (tier 1) wins wherever a
 * bundle carries it — that precedence is what makes an owner's theme swap change
 * the dial. Where it does not, a `required` declaration gets a refusal (D26) and
 * an `optional` one gets the app's default (tier 2, D31); never a substitute
 * chosen silently. Tier 3, the board's own pair, was retired with §26 slice 2
 * and §27: the clock no longer carries compiled pixels, and a board with no
 * resident bank draws the base face instead. What survives of the tiering is
 * the caller's duty — one that cannot tell resident art from its absence
 * cannot say which face it is showing.
 *
 * NO SHIPPED PIXELS (D22, D10). A tier-2 default is one of five generated
 * primitives, baked into the plane layout the art bank uses, so the caller hands
 * the result to gfx_paint_ctx_init() exactly as it hands those functions a banked
 * sprite: the same commit path, and the same per-role blend counts
 * (orb_gfx.h:36-43, gfx_role_snapshot). A rectangle the compositor draws is not
 * artwork; a shipped fallback file is.
 *
 * THIS FILE STAYS OUT OF LVGL. It includes only the generated assets/clock_assets.h
 * — plain structs — so the registry compiles on the host and drives the shipped
 * resolution code from a suite rather than from a console. Only the TEXT primitive
 * needs the compiled-in font, and it reaches the font through one function pointer
 * the gfx TU installs, which is also what lets that suite stub it.
 *
 * THE THREE-WAY MIRROR. The class enum, the declaration record and the wire field
 * order are mirrored by main/app/orb_app_manifest.{h,c}, tools/orbapp.py and the
 * sentinels (host_orbapp_test.py, host_orbapp_reader_test.py, host_svc_test.py).
 * Writer, reader and test move in one commit, or the suite fails rather than
 * shipping a dial that resolves one app's `dial` slot as somebody else's.
 */
#ifndef ORB_ASSETS_H
#define ORB_ASSETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "clk_types.h"    /* clk_sprite_t: plain structs, generated, no LVGL */

#ifdef __cplusplus
extern "C" {
#endif


/* ---- the frozen class vocabulary (D23) ----------------------------------
 * Append-only. LAYER and STATUS are slots the OS composites rather than art a
 * theme ships, and they are in the enum because a manifest declares against one
 * vocabulary: a declaration asks for a KIND of slot, not for a file. */
typedef enum {
    ORB_ASSET_TEXT = 0,     /* a run of glyphs, in the compiled-in font           */
    ORB_ASSET_SPRITE,       /* one flat RGB565A8 plate, unrotated                 */
    ORB_ASSET_HAND,         /* a plate with a pivot: it turns                     */
    ORB_ASSET_PLATE,        /* the full-panel background                          */
    ORB_ASSET_RING,         /* a count of plates laid on a circle                 */
    ORB_ASSET_BANK,         /* an indexed run of plates, one per value            */
    ORB_ASSET_LAYER,        /* a compositor slot: order and mark, not pixels      */
    ORB_ASSET_STATUS,       /* the app's one status line                          */
    ORB_ASSET_CLASS_N       /* 8: the vocabulary's width, and the refusal bound   */
} orb_asset_class_t;

/* The dialect this vocabulary speaks — the container's geometry ABI, and not a
 * number this file knows. The manifest header is the owner; orb_assets.h cannot
 * include it (that header includes THIS one), so the literal sits here and
 * orb_assets.c pins it with a compile-time assert against the real word. A test
 * that checks two constants agree is a check that runs when someone remembers to
 * run it; an assert is a check that runs when someone builds. */
#define ORB_ASSET_CLASS_ABI   2u

/* The names the classes answer to, indexed by orb_asset_class_t. Wire text: the
 * strings a manifest's asset table carries. Lowercase, fixed. */
extern const char *const orb_asset_class_name[ORB_ASSET_CLASS_N];

/** The class a name denotes, or -1. Case-sensitive. */
int orb_asset_class_of(const char *name);

/** True when a class id is one this build can draw. The reader refuses an
 *  out-of-range declaration outright (D26: a slot the OS cannot name will never
 *  resolve, and an unresolved slot is not a thing to install); the resolver
 *  answers ORB_ASSET_CLASS, which the clock's own path and the host suite drive. */
bool orb_asset_class_known(uint32_t cls);

/* ---- the two namespaces -------------------------------------------------
 * A reference is `<name>` (owner-scoped: the caller's own slug is the namespace)
 * or `global/<name>`. Any OTHER prefix is refused rather than guessed at, because
 * reading `weather/bg` while resolving for `clock` is the exact cross-app shadow
 * D23 exists to forbid. */
#define ORB_ASSET_NS_GLOBAL     "global/"
#define ORB_ASSET_NS_GLOBAL_N   7                              /* strlen, above   */
/* The `<name>` half, NUL included: 12 bytes and 11 usable characters, because the
 * manifest row reserves words 1..3 and nothing else. This is the wire's bound and
 * not a preference — orb_assets.c asserts the equality against
 * ORBAPP_ASSET_NAME_MAX, and the first draft of this header carried 24 on the
 * reasoning that a slot name should be as long as a slug, which the row cannot
 * hold. `minute_hand` at 11 chars is the longest name that fits, and it fits. */
#define ORB_ASSET_NAME_MAX      12u  /* == ORBAPP_ASSET_NAME_MAX                  */
#define ORB_ASSET_OWNER_MAX     23u  /* the `<slug>` half; == ORBAPP_SLUG_MAX - 1 */
/* 36 and not 19: the widest reference this build can be handed is an OWNER-scoped
 * one, and an owner is a full slug. `global/<name>` tops out at 19, which is the
 * number a reader that thought about only one shape would have written, and a
 * ten-char slot name on a twenty-char slug would then overflow the field at the
 * worst possible moment — after install, inside the resolver, on the LVGL task. */
#define ORB_ASSET_REF_MAX       (ORB_ASSET_OWNER_MAX + 1 + ORB_ASSET_NAME_MAX)

/** True when `ref` is `<slug>/<name>` or `global/<name>`: both halves are
 *  `[a-z0-9_]`, the name is 1..ORB_ASSET_NAME_MAX-1 chars, the owner is at most
 *  ORB_ASSET_OWNER_MAX, and no other prefix or second slash appears. */
bool orb_asset_ref_ok(const char *ref);

/** Split a reference. False when it is not one. `*ns_global` says which
 *  namespace; `*name`/`*nlen` describe the name half INSIDE `ref` — no copy,
 *  because a resolver hands these to a source that compares them against a table
 *  it already owns. */
bool orb_asset_ref_split(const char *ref, bool *ns_global, const char **name,
                         size_t *nlen);

/* ---- the answers (D23's two fixed orders, D26, D31) --------------------
 * The order is not a caller argument and no flag changes it: theme content, then
 * the app's own default, then the board's. Inside the theme tier the caller's own
 * `<slug>/<name>` is consulted before `global/<name>`, so a bundle carrying both
 * an app's own dial and a global dial draws the app's. */
typedef enum {
    ORB_ASSET_OK = 0,       /* tier 1. out->off/bytes are the theme's payload     */
    ORB_ASSET_APP,          /* tier 2. declared optional; the app draws a default */
    ORB_ASSET_BOARD,        /* tier 3. the caller's board fallback owns this one  */
    ORB_ASSET_NO_SLOT,      /* nothing is filed under that reference, anywhere    */
    ORB_ASSET_NO_CONTENT,   /* a slot exists; the bundle carries no bytes for it  */
    ORB_ASSET_REQUIRED,     /* D26: declared required, and nothing carries it     */
    ORB_ASSET_UNDECLARED,   /* the caller asked about a reference it never made   */
    ORB_ASSET_CLASS,        /* declared, but as a class other than the one asked  */
    ORB_ASSET_NAME,         /* the reference is neither `<name>` nor `global/...` */
    ORB_ASSET_ROOM          /* the caller's table is shorter than its own count   */
} orb_asset_ans_t;

/** The word for one answer, for a log line or a ledger row. Never NULL. These
 *  stay separate words on purpose: NO_SLOT ("nobody filed that name") and
 *  NO_CONTENT ("the theme carried it empty") are different facts, and only the
 *  second is a reason to re-bake a bundle. */
const char *orb_asset_why(orb_asset_ans_t a);

/** One named slot, resolved. This is the handle P5.5's orb_layer_desc_t embeds as
 *  its art reference, and nothing else is legal there: an offset into a bank the
 *  caller mapped for itself is precisely how art creeps back out of a registry.
 *  The field order is frozen for that reason — a descriptor that grows appends
 *  after `bytes`, it never reorders these. */
typedef struct {
    uint16_t cls;        /* orb_asset_class_t, echoed from the declaration        */
    uint16_t tier;       /* which answer above produced this one                  */
    int16_t  x, y;       /* where it draws on the panel                           */
    int16_t  w, h;       /* its box                                              */
    int16_t  pivotx, pivoty;      /* HAND/LAYER: the rotation pivot               */
    uint16_t prim;       /* orb_prim_t — the generator, for tiers 2 and 3         */
    uint8_t  global;     /* 1 when it resolved in the global/<name> namespace     */
    uint8_t  pad;
    uint32_t off;        /* tier 1: offset into the theme's art payload           */
    uint32_t bytes;      /* tier 1: orb_asset_slot_bytes() for this box           */
} orb_asset_t;

/** The bytes a slot of one class carries — the answer orb_theme.c gives today for
 *  the one clock bank per theme, generalized to a named slot per class: colour
 *  w*h*2 plus alpha w*h for every class that IS pixels, and 0 for the two the
 *  compositor draws (LAYER, STATUS) or for count 0. orb_theme.c checks every
 *  extent it reads against this one call, which is what stops the bake's `w*h*3`
 *  and the reader's `w*h*3` being two rules. */
uint32_t orb_asset_slot_bytes(orb_asset_class_t cls, uint32_t w, uint32_t h,
                              uint32_t count);
/* ---- the declared table (D23's named slots, D31's selector) --------------
 * One unpacked manifest declaration. `ref` is the reference as the container
 * carries it. The flag is D31's selector: with it set the app has declared NO
 * default for the slot, which is what turns a tier-1 miss into D26's install
 * refusal instead of a fallback. */
typedef struct {
    uint8_t      cls;                                     /* orb_asset_class_t  */
    uint8_t      required;                                          /* D31      */
    char         ref[ORB_ASSET_REF_MAX];        /* "<name>" or "global/<name>"  */
    orb_asset_t  at;                    /* where it draws; the app's own layout */
} orb_asset_decl_t;

#define ORB_ASSET_MAX  16                               /* == ORBAPP_ASSETS_MAX */

/* ---- the content sources: who answers tier 1 -----------------------------
 * The registry owns the ORDER and the ANSWERS; the CONTENT belongs to whoever
 * bound. A source answers exactly one namespace, which is what lets orb_theme.c
 * answer for the live bundle and for the board's baked-in pair without this file
 * knowing either exists. A dial with nothing bound answers NO_SLOT everywhere,
 * and that is not an error: it is the card-less board, described.
 *
 * find() runs on the LVGL task — a tick resolves its own slots — so it may read
 * RAM it owns and may not touch the card, NVS or a partition. */
typedef struct {
    /* 1 = this source carries the slot; 0 = the slot is filed here and the bundle
     * carries no bytes for it, which is NO_CONTENT — the one answer that IS a
     * reason to re-bake; -1 = not mine. A source that cannot tell the two apart
     * returns -1, and the reference then answers NO_SLOT. The distinction is the
     * whole reason the enum has two misses: "nobody filed that name" is a manifest
     * fault and "the theme carried it empty" is a bake fault, and the owner can
     * only act on the second one if the OS can tell them apart. */
    int         (*find)(void *user, orb_asset_class_t cls, const char *name,
                        orb_asset_t *out);
    void        *user;
    const char  *slug;   /* the owner this source answers for, or NULL for the
                          * global namespace. A source lives in exactly one half
                          * of the registry, so D23's "one app's bg can never
                          * shadow another's" is structure and not a rule. */
    uint8_t      tier;   /* ORB_ASSET_OK for a theme, ORB_ASSET_BOARD for the
                          * board's pair. The answer a caller gets IS the
                          * source's, so a board fallback can never be reported
                          * on the ledger as a theme's art. */
} orb_asset_src_t;

#define ORB_ASSET_SOURCES 4      /* the live theme, the board's pair, two spare */

/** Install the source table, copied by value; the callbacks and their `user`
 *  pointers must be module statics. Every call REPLACES the whole table — a
 *  partial rebind would let a half-swapped dial draw this bundle's plate under
 *  last bundle's hands, which is D13's bug in a new place. n == 0 unbinds all. */
esp_err_t orb_asset_bind(const orb_asset_src_t *srcs, int n);

/** How many sources are bound. The status endpoint prints it: 0 is the evidence
 *  that the dial is drawing the compiled-in pair and not a card theme. */
int orb_asset_sources(void);

/* ---- the resolution ladder ----------------------------------------------
 * `decls`/`n` are the app's declared table — the manifest's asset table, or a
 * compiled tier app's own static one. `slug` is the caller's owner scope; NULL
 * means the caller has none, which is how the OS resolves a global reference on
 * an app's behalf. `out` is filled on OK, APP and BOARD and untouched on every
 * other answer, so a caller that ignores the return value draws nothing rather
 * than a half-resolved slot. */
orb_asset_ans_t orb_asset_resolve(const orb_asset_decl_t *decls, int n,
                                  const char *slug, const char *ref,
                                  orb_asset_t *out);

/** Resolve every declaration in table order and report the first refusal — the
 *  shape D26 needs, because the install ladder calls this BEFORE the first erase.
 *  Returns ORB_ASSET_OK when the whole table resolves; otherwise the failing
 *  answer, with the offending reference written into `why` and, when `at` is
 *  non-NULL, the row that failed. */
orb_asset_ans_t orb_asset_resolve_all(const orb_asset_decl_t *decls, int n,
                                      const char *slug, char *why, size_t cap,
                                      int *at);

/* ---- the five generated primitives (D22) --------------------------------
 * The whole of the OS's shipped art is these five routines. Each bakes a plate
 * into the layout the art bank uses — colour RGB565 (w*h*2), then alpha A8 (w*h)
 * — so a fallback is indistinguishable from a banked sprite at commit time and
 * rides the same path. orb_layers.c's gfx_prim_ctx_init() is the compositor-side
 * entry: it bakes into the caller's plane and fills a gfx_paint_ctx_t, which is
 * what puts a fallback INSIDE the role counters rather than beside them. */
typedef enum {
    ORB_PRIM_RECT = 0,    /* solid rectangle                                  */
    ORB_PRIM_RRECT,       /* rounded rectangle; r is the corner radius        */
    ORB_PRIM_RING,        /* stroked circle; r the radius, thick the stroke   */
    ORB_PRIM_BAR,         /* a rectangle rotated about cx,cy — D22's hand     */
    ORB_PRIM_TEXT,        /* glyphs, in the compiled-in font                  */
    ORB_PRIM_N
} orb_prim_t;

typedef struct {
    orb_prim_t prim;
    uint16_t   w, h;                            /* the plate, in px            */
    int16_t    x, y;                            /* where it lands              */
    int16_t    cx, cy;                          /* BAR/RING: centre of rotation*/
    uint16_t   r;                               /* RRECT corner, RING radius   */
    uint16_t   thick;                           /* RING stroke, BAR width      */
    int32_t    ang_10;                          /* BAR: tenths of a degree     */
    uint32_t   color;                                    /* RGB565             */
    uint8_t    alpha;                    /* 0..255 coverage over the whole plate*/
    const char *text;                                    /* TEXT only          */
} orb_prim_arg_t;

/** Plane bytes a primitive needs: a 4-byte lead (clk_sprite_t's color_off must be
 *  non-zero to be drawable — the same test the theme reader applies to a baked
 *  sprite), then colour w*h*2, then alpha w*h. */
uint32_t orb_asset_prim_bytes(uint32_t w, uint32_t h);

/** Bake one primitive. False when the box is empty, the primitive is not one this
 *  build generates, or TEXT has nothing the font can draw. On false `spr` is
 *  zeroed, so a caller that forgot to check draws nothing at all rather than the
 *  previous caller's plate — the failure that would otherwise read as stale art. */
bool orb_asset_prim_bake(const orb_prim_arg_t *a, uint8_t *plane,
                         clk_sprite_t *spr);

/** The generator a class falls back to when the app declared none of its own:
 *  HAND → BAR, because a rotating bar is the only fallback that reads as a hand;
 *  TEXT/STATUS → TEXT; the rest → their own generator. */
orb_prim_t orb_asset_class_prim(orb_asset_class_t cls);

/** The TEXT primitive's font, installed by the gfx TU at boot. This file must not
 *  include lvgl.h — that include is exactly what would make host compilation of
 *  the registry impossible — and the compiled-in font is LVGL's, so the font
 *  reaches the registry through one function pointer, and the host suite reaches
 *  it through a stub. With no backend installed a TEXT slot answers
 *  ORB_ASSET_NO_CONTENT rather than baking an empty plate: a fallback that
 *  silently draws nothing is the failure D31 exists to close. */
typedef bool (*orb_asset_text_fn)(const char *text, uint8_t **plane, uint16_t *w,
                                  uint16_t *h, uint32_t color, uint8_t alpha);
void orb_asset_set_text(orb_asset_text_fn fn);

/** True when a TEXT slot can be baked, i.e. a font backend is installed. The
 *  resolver reports a TEXT tier-2 answer as ORB_ASSET_NO_CONTENT while this is
 *  false, and the status page prints it, because "the dial has no font wired up
 *  yet" and "the app has no default" look identical on the glass and are not the
 *  same fault. */
bool orb_asset_text_ready(void);

#ifdef __cplusplus
}
#endif
#endif /* ORB_ASSETS_H */


/** One declaration on its own — the single-row form of orb_asset_resolve, exposed
 *  because the install path has the unpacked row in hand and a host vector wants
 *  to drive exactly one step of the ladder at a time. */
orb_asset_ans_t orb_asset_resolve_one(const orb_asset_decl_t *d, const char *slug,
                                      orb_asset_t *out);


