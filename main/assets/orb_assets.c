/*
 * main/assets/orb_assets.c — the asset registry; see the header for the model
 * and for why this file compiles without LVGL.
 *
 * The whole of the policy is the ORDER: theme, then app default, then board;
 * owner scope before global; and a `required` declaration that finds no tier-1
 * content producing a refusal rather than a fallback. Everything else here is
 * bookkeeping over those three rules. Keeping them in one file is the point: the
 * moment the precedence lives in a caller's `if`, two callers can disagree about
 * what the owner's theme says, and D31's "theme content wins wherever a bundle
 * carries it" becomes a per-app opinion.
 *
 * Resolution reads no storage and takes no lock. Tier 1 comes from a bound source
 * (RAM the theme already holds), and the source table is written only from the
 * boot ladder and the theme-swap path, both off the LVGL task. A tick that
 * resolves while a swap is in flight sees either the old table or the new one.
 */
#include "orb_assets.h"

#include <stdio.h>
#include <string.h>

/* The class vocabulary's version IS the container's dialect word, and the two
 * headers cannot include each other (orb_app_manifest.h includes this one), so the
 * equality is pinned here where both are visible. This is the assertion that makes
 * ORB_ASSET_CLASS_ABI's literal honest: a bump on one side and not the other is a
 * build that fails at compile time rather than a dial that installs a manifest whose
 * class numbers it reads as somebody else's. */
#include "app/orb_app_manifest.h"
_Static_assert(ORB_ASSET_CLASS_ABI == ORBAPP_GEOM_ABI,
               "orb_assets.h's ORB_ASSET_CLASS_ABI must equal the container's "
               "ORBAPP_GEOM_ABI: the class vocabulary speaks the container's "
               "dialect and has no version of its own");
_Static_assert(ORB_ASSET_MAX == ORBAPP_ASSETS_MAX,
               "the registry's slot table and the loader's must hold the same count");
_Static_assert(ORBAPP_ASSET_WORDS == 6,
               "the wire row is six words; the test mirrors the layout in numbers");
_Static_assert(ORB_ASSET_NAME_MAX == ORBAPP_ASSET_NAME_MAX,
               "the name field's bound is one number in one place");


/* The strings are the ladder's shape, and the sentinel lifts this table to pin
 * it: reordering the enum without reordering these would keep every check green
 * while the ledger started naming the wrong failure. */
const char *const orb_asset_class_name[ORB_ASSET_CLASS_N] = {
    "text", "sprite", "hand", "plate", "ring", "bank", "layer", "status", "aux",
    "preview"
};

static const char *const WHY[10] = {
    "theme content",                 /* ORB_ASSET_OK                            */
    "app default",                   /* ORB_ASSET_APP                           */
    "board fallback",                /* ORB_ASSET_BOARD                         */
    "no slot",                       /* ORB_ASSET_NO_SLOT                       */
    "no content",                    /* ORB_ASSET_NO_CONTENT                    */
    "required and not carried",      /* ORB_ASSET_REQUIRED                      */
    "not declared",                  /* ORB_ASSET_UNDECLARED                    */
    "wrong class",                   /* ORB_ASSET_CLASS                         */
    "bad reference",                 /* ORB_ASSET_NAME                          */
    "table shorter than its count"   /* ORB_ASSET_ROOM                          */
};

const char *orb_asset_why(orb_asset_ans_t a)
{
    /* The bound is the answer's own width, not a literal 10 that survives adding
     * an enumerator by accident. An out-of-range answer is a caller bug, and a
     * caller bug printed as a nonsense string is a log line nobody can act on. */
    if ((unsigned)a > ORB_ASSET_ROOM) return "unknown answer";
    return WHY[a];
}

int orb_asset_class_of(const char *name)
{
    if (!name) return -1;
    for (unsigned i = 0; i < ORB_ASSET_CLASS_N; i++)
        if (!strcmp(name, orb_asset_class_name[i])) return (int)i;
    return -1;
}

bool orb_asset_class_known(uint32_t cls)
{
    return cls < (uint32_t)ORB_ASSET_CLASS_N;
}

/* `global/` is the only PREFIX that exists (D23: two namespaces, nothing else), and
 * the test reads the macro rather than a literal length so the header and this
 * predicate cannot disagree about where the name starts — the class of bug orbtheme's
 * hand-written FORMAT-1 name ranges produced, one map against another.
 *
 * An owner-scoped reference carries its owner: `<slug>/<name>`, because
 * orb_asset_resolve_one() checks the prefix against the caller's own slug and a
 * reference that arrived bare could not be checked at all. So there are exactly two
 * shapes and never a third — no slash at all, a trailing slash, a doubled slash and
 * an empty half are all refused rather than resolved against an empty slot, which
 * would read as "the theme is silent" and send the owner to re-bake a bundle that
 * was never asked to carry that name.
 *
 * `name` comes back WITHOUT its prefix for both shapes, because the source that
 * answers is bound to one namespace and indexes its own table by the slot name
 * alone: a theme bundle keyed `weather/dial` would be a bundle that stores who it
 * belongs to, and D23 put that fact in the binding instead. */
bool orb_asset_ref_split(const char *ref, bool *ns_global, const char **name,
                         size_t *nlen)
{
    if (!ref || !*ref) return false;

    const bool g = !strncmp(ref, ORB_ASSET_NS_GLOBAL, ORB_ASSET_NS_GLOBAL_N);
    const char *n    = g ? ref + ORB_ASSET_NS_GLOBAL_N : ref;
    const size_t len = strlen(n);

    if (!len || len > ORB_ASSET_REF_MAX - 1) return false;

    /* The global half is one flat namespace: everything after the prefix is the
     * name, and a second slash in it is somebody else's scope reached by accident.
     * The owner half splits once, and both halves are checked. */
    size_t slash = len;
    if (!g) {
        const char *s = memchr(n, '/', len);
        if (!s || s == n || (size_t)(s - n) > ORB_ASSET_OWNER_MAX) return false;
        slash = (size_t)(s - n);
    } else if (memchr(n, '/', len)) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        const char c = n[i];
        if (i == slash) {
            if (!g && (i + 1 >= len || n[i + 1] == '/')) return false;
            continue;                       /* the one separator, owner scope only */
        }
        if (c == '/') return false;         /* a second one, anywhere: not a name  */
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }

    /* The name half is what a source is asked for, so the bound that matters is
     * this one and not the whole reference's: an owner half of the wrong length is
     * the caller's slug and the caller's problem, while a name over the field is a
     * row this build cannot resolve and must refuse. */
    const size_t namelen = g ? len : len - slash - 1;
    if (namelen > ORB_ASSET_NAME_MAX - 1) return false;

    if (ns_global) *ns_global = g;
    if (name)      *name = g ? n : n + slash + 1;
    if (nlen)      *nlen = namelen;
    return true;
}

bool orb_asset_ref_ok(const char *ref)
{
    return orb_asset_ref_split(ref, NULL, NULL, NULL);
}

/* The answer to "how big is this slot", which orb_theme.c answered for itself
 * until now: generalized from the one clock bank per theme to a named slot per
 * class. Two classes are not pixels at all — a LAYER is an order position and a
 * mark, a STATUS line is text the compositor draws. Both are declarable (an app
 * says "I want a layer") and both cost zero, and that zero is the fact the
 * theme's extent checks need rather than a case to paper over with a 1.
 *
 * PREVIEW is the one class that is pixels and does not pay the RGB565A8 rule.
 * Its alpha is always 255: a preview is a whole picture, and the compositor
 * draws it opaque, so the third byte per pixel would be a plane of 255s that
 * no reader consults. This function is the one place that answers the size, and
 * orb_theme.c's extent check reads it, so a preview's extents validate against
 * the same rule the baker wrote them with — a `w*h*3` here against the baker's
 * `w*h*2` miscounts by exactly the alpha plane's worth of bytes. */
uint32_t orb_asset_slot_bytes(orb_asset_class_t cls, uint32_t w, uint32_t h,
                              uint32_t count)
{
    if (cls == ORB_ASSET_LAYER || cls == ORB_ASSET_STATUS) return 0;
    if (!count) return 0;
    if (cls == ORB_ASSET_PREVIEW) return w * h * 2u * count;   /* RGB565, opaque */
    return w * h * 3u * count;                 /* colour w*h*2 + alpha w*h      */
}

/* ---- the bound sources --------------------------------------------------
 * Written only by orb_asset_bind(): the boot ladder before the first app runs,
 * and the theme-swap path. Read on the LVGL task, so the table is a static array
 * and one count — a copy, not a lock. */
static orb_asset_src_t s_src[ORB_ASSET_SOURCES];
static int             s_n;

esp_err_t orb_asset_bind(const orb_asset_src_t *srcs, int n)
{
    if (n < 0 || n > ORB_ASSET_SOURCES) return ESP_ERR_INVALID_SIZE;
    if (n && !srcs) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < n; i++)
        if (!srcs[i].find) return ESP_ERR_INVALID_ARG;
    if (n) memcpy(s_src, srcs, (size_t)n * sizeof s_src[0]);
    s_n = n;
    return ESP_OK;
}

int orb_asset_sources(void) { return s_n; }

/* Ask the bound sources in the one order that is legal: theme tier before board
 * tier, and inside each the namespace the reference names. Sources of the same
 * tier and namespace answer in bind order, which is the order the boot ladder
 * registered them in and therefore the order the owner sees on the page.
 *
 * A source that answers 0 ("filed here, nothing carried") does not end the walk:
 * the next source may well carry the bytes, and NO_CONTENT is only the verdict
 * once every source in both tiers had the chance and none of them produced
 * pixels. Recording it and keeping going is what keeps a theme that ships a slot
 * empty from hiding a board fallback that would have drawn it. */
static orb_asset_ans_t ask(orb_asset_class_t cls, const char *name, bool want_global,
                           orb_asset_t *out)
{
    bool saw_empty = false;
    for (int pass = 0; pass < 2; pass++) {      /* 0 = theme, 1 = the board      */
        const uint8_t want_tier = pass ? ORB_ASSET_BOARD : ORB_ASSET_OK;
        for (int i = 0; i < s_n; i++) {
            const bool is_global = !s_src[i].slug;
            if (s_src[i].tier != want_tier) continue;
            if (is_global != want_global) continue;
            const int r = s_src[i].find(s_src[i].user, cls, name, out);
            if (r == 0) { saw_empty = true; continue; }
            if (r < 0) continue;
            out->tier   = s_src[i].tier;
            out->global = (uint8_t)is_global;
            return (s_src[i].tier == ORB_ASSET_BOARD) ? ORB_ASSET_BOARD
                                                      : ORB_ASSET_OK;
        }
    }
    return saw_empty ? ORB_ASSET_NO_CONTENT : ORB_ASSET_NO_SLOT;
}

/* The ladder, one declaration deep. Kept as the single-row form so
 * orb_asset_resolve_all() and the install path share one precedence instead of
 * restating it — the reason orb_app_manifest.c's validators are one ladder called
 * in order rather than four copies of its rules. */
orb_asset_ans_t orb_asset_resolve_one(const orb_asset_decl_t *d, const char *slug,
                                      orb_asset_t *out)
{
    if (!d || !out) return ORB_ASSET_ROOM;
    if (!orb_asset_class_known(d->cls)) return ORB_ASSET_CLASS;

    bool        global_ns;
    const char *name;
    size_t      nlen;
    if (!orb_asset_ref_split(d->ref, &global_ns, &name, &nlen))
        return ORB_ASSET_NAME;

    /* An owner-scoped reference is only ever the CALLER's own scope. A row naming
     * somebody else is not resolved and not refused quietly: UNDECLARED says "this
     * is not your slot", which a ledger row can show, where REQUIRED would read as
     * a missing asset and send the owner to the wrong card. A caller with no slug
     * — the OS resolving on an app's behalf — carries no scope to violate. */
    if (!global_ns && slug && *slug) {
        const size_t sl = strlen(slug);
        if (strncmp(d->ref, slug, sl) != 0 || d->ref[sl] != '/')
            return ORB_ASSET_UNDECLARED;
    }

    const orb_asset_ans_t a = ask((orb_asset_class_t)d->cls, name, global_ns, out);
    if (a == ORB_ASSET_OK || a == ORB_ASSET_BOARD) return a;

    /* Nothing carried it. D31's selector is read HERE and nowhere else: a required
     * slot refuses, an optional one is the app's own problem, and the two answers
     * stay distinct because the caller's branch differs — refuse the activation
     * and say why, versus draw the default and log which generator produced it. */
    if (d->required) return ORB_ASSET_REQUIRED;

    /* An optional TEXT slot with no font wired up has no default to draw. The
     * honest answer is NO_CONTENT and not APP: a caller that trusted APP would
     * bake a blank plate and the owner would see a dial with nothing where the
     * reading should be, which is the silent failure D31 exists to close. */
    const orb_prim_t prim = orb_asset_class_prim((orb_asset_class_t)d->cls);
    if (prim == ORB_PRIM_TEXT && !orb_asset_text_ready())
        return ORB_ASSET_NO_CONTENT;

    memset(out, 0, sizeof *out);
    out->cls    = d->cls;
    out->tier   = ORB_ASSET_APP;
    out->global = (uint8_t)global_ns;
    out->prim   = (uint16_t)prim;
    return ORB_ASSET_APP;
}

orb_asset_ans_t orb_asset_resolve(const orb_asset_decl_t *decls, int n,
                                  const char *slug, const char *ref,
                                  orb_asset_t *out)
{
    if (!orb_asset_ref_ok(ref)) return ORB_ASSET_NAME;
    if (n < 0 || !decls) return ORB_ASSET_ROOM;
    if (n == 0) return ORB_ASSET_UNDECLARED;

    /* Looked up by reference and not by index: an index into a table two callers
     * built in different orders is how a status line lands on a ring's
     * coordinates. */
    for (int i = 0; i < n; i++)
        if (!strcmp(decls[i].ref, ref))
            return orb_asset_resolve_one(&decls[i], slug, out);
    return ORB_ASSET_UNDECLARED;
}

orb_asset_ans_t orb_asset_resolve_all(const orb_asset_decl_t *decls, int n,
                                      const char *slug, char *why, size_t cap,
                                      int *at)
{
    if (at) *at = -1;
    if (n < 0 || (n > 0 && !decls)) {
        if (why) snprintf(why, cap, "%s", orb_asset_why(ORB_ASSET_ROOM));
        return ORB_ASSET_ROOM;
    }
    for (int i = 0; i < n; i++) {
        orb_asset_t r;
        const orb_asset_ans_t a = orb_asset_resolve_one(&decls[i], slug, &r);
        /* Only a refusal stops the walk. OK, APP and BOARD are all answers an app
         * can draw, and stopping on BOARD would turn the card-less board — the
         * state every shipped dial is in today — into a failed install. */
        if (a == ORB_ASSET_OK || a == ORB_ASSET_APP || a == ORB_ASSET_BOARD)
            continue;
        if (at) *at = i;
        if (why && cap)
            snprintf(why, cap, "%s: %s", orb_asset_why(a), decls[i].ref);
        return a;
    }
    return ORB_ASSET_OK;
}

/* ---- the five generated primitives (D22) --------------------------------
 * Every one of them ends in the same two loops: write the requested RGB565 into a
 * colour plane, write the coverage into the alpha plane after it. That shape is
 * what makes a fallback indistinguishable from banked art downstream — the
 * compositor never learns which kind of sprite it blends, so a fallback costs
 * what a sprite costs and gfx_role_snapshot() charges it to the role the caller
 * named. A rectangle the compositor draws is not artwork; a shipped fallback file
 * is, and D22 forbids that. */
#define PRIM_LEAD 4u

uint32_t orb_asset_prim_bytes(uint32_t w, uint32_t h)
{
    return PRIM_LEAD + w * h * 3u;
}

orb_prim_t orb_asset_class_prim(orb_asset_class_t cls)
{
    /* A HAND falls back to BAR: it is the only generator that rotates, and a hand
     * that cannot turn is a static mark on a dial — a wrong answer, not merely an
     * ugly one. LAYER gets RECT because a layer is a box until it has art. */
    switch (cls) {
    case ORB_ASSET_HAND:   /* fall through */
    case ORB_ASSET_AUX:    return ORB_PRIM_BAR;
    case ORB_ASSET_TEXT:   return ORB_PRIM_TEXT;
    case ORB_ASSET_STATUS: return ORB_PRIM_TEXT;
    case ORB_ASSET_RING:   return ORB_PRIM_RING;
    default:               return ORB_PRIM_RECT;
    }
}

/* The bar's rotation is the COMPOSITOR's, not the bake's. That is why D22's
 * fallback for a hand is a bar the caller rotates and not a pre-rotated plate:
 * gfx_paint_ctx_init() already maps the plate back through
 * gfx_rot_sin_cos(ang_10) with the same SWEPT_PAD-clipped box it uses for every
 * theme hand (orb_gfx.c:511-560). Baking the angle in would rotate twice and put
 * the ink outside the rows the job marked — the ghost the swept-box contract
 * (orb_gfx.h:92-97) exists to prevent, and a cost no counter would show. */
static uint8_t cov_bar(int32_t px, int32_t py, int32_t len, int32_t thick)
{
    const int32_t y0 = (len - thick) / 2;      /* centred across the plate      */
    return (px >= 0 && px < len && py >= y0 && py < y0 + thick) ? 255 : 0;
}

/* Squared-integer distance tests throughout: an exact comparison costs less than
 * a table lookup on the F0 and keeps a bake from pulling libm onto the LVGL
 * task, where the §6 cadence budget has no line item for it. */
static uint8_t cov_ring(int32_t px, int32_t py, int32_t cx, int32_t cy,
                        int32_t r, int32_t thick)
{
    const int64_t dx = px - cx, dy = py - cy;
    const int64_t d2 = dx * dx + dy * dy;
    const int64_t ri = (r > (int32_t)thick) ? (r - (int32_t)thick) : 0;
    return (d2 <= (int64_t)r * r && d2 >= ri * ri) ? 255 : 0;
}

/* A rounded corner is four quarter-discs and a cross. The cross answers most of
 * the plate's pixels before the distance test runs, so the arc costs only the
 * corner squares it has to. */
static uint8_t cov_rrect(int32_t px, int32_t py, int32_t w, int32_t h, int32_t r)
{
    if (r <= 0) return 255;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (px >= r && px < w - r) return 255;
    if (py >= r && py < h - r) return 255;
    int32_t dx = (px < r) ? (r - px) : (px - (w - r - 1));
    int32_t dy = (py < r) ? (r - py) : (py - (h - r - 1));
    return ((int64_t)dx * dx + (int64_t)dy * dy <= (int64_t)r * r) ? 255 : 0;
}

/* The one font entry, installed by orb_gfx.c at boot. NULL until then, which the
 * resolver reports as NO_CONTENT rather than baking a blank plate: a fallback
 * that draws nothing and answers OK is exactly the silent-failure shape D31 was
 * written to close, and an answer is cheaper to debug than a lie. */
static orb_asset_text_fn s_text;

void orb_asset_set_text(orb_asset_text_fn fn) { s_text = fn; }

bool orb_asset_text_ready(void) { return s_text != NULL; }

bool orb_asset_prim_bake(const orb_prim_arg_t *a, uint8_t *plane,
                         clk_sprite_t *spr)
{
    if (spr) memset(spr, 0, sizeof *spr);
    if (!a || !plane || !spr) return false;
    if ((int)a->w <= 0 || (int)a->h <= 0) return false;
    if (a->prim >= ORB_PRIM_N) return false;

    const uint32_t w = a->w, h = a->h;
    uint16_t *col = (uint16_t *)(plane + PRIM_LEAD);
    uint8_t  *alp = plane + PRIM_LEAD + w * h * 2u;

    if (a->prim == ORB_PRIM_TEXT) {
        if (!s_text || !a->text || !*a->text) return false;
        uint8_t  *tp = NULL;
        uint16_t  tw = 0, th = 0;
        if (!s_text(a->text, &tp, &tw, &th, a->color, a->alpha) || !tp)
            return false;
        /* The font chose the box. It is clipped to the slot's own rather than
         * allowed past it: the box is what the geometry said the slot IS, and ink
         * over the neighbour is the failure a theme bake would be refused for. */
        const uint32_t cw = (tw < w) ? tw : w, ch = (th < h) ? th : h;
        memset(alp, 0, w * h);
        for (uint32_t i = 0; i < w * h; i++) col[i] = (uint16_t)a->color;
        for (uint32_t y = 0; y < ch; y++)
            for (uint32_t x = 0; x < cw; x++)
                alp[y * w + x] = tp[(y * tw + x) * 4 + 3];   /* ARGB8888 alpha  */
    } else {
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                uint8_t c;
                switch (a->prim) {
                case ORB_PRIM_RRECT: c = cov_rrect((int32_t)x, (int32_t)y,
                                                   (int32_t)w, (int32_t)h,
                                                   (int32_t)a->r);              break;
                case ORB_PRIM_RING:  c = cov_ring((int32_t)x, (int32_t)y,
                                                  (int32_t)a->cx, (int32_t)a->cy,
                                                  (int32_t)a->r,
                                                  (int32_t)a->thick);            break;
                default:             c = cov_bar((int32_t)x, (int32_t)y,
                                                 (int32_t)w,
                                                 (int32_t)(a->thick ? a->thick : h));
                                     break;
                }
                col[y * w + x] = (uint16_t)a->color;
                alp[y * w + x] = (uint8_t)((uint32_t)c * a->alpha / 255u);
            }
        }
    }

    /* A plate with no covered pixel is not a drawable layer, and saying so here is
     * what stops a zero-radius ring or a bar thicker than its box reaching the
     * commit as an invisible job that still pays for the restore pass. */
    bool any = false;
    for (uint32_t i = 0; i < w * h; i++) if (alp[i]) { any = true; break; }
    if (!any) { memset(spr, 0, sizeof *spr); return false; }

    spr->w         = (uint16_t)w;
    spr->h         = (uint16_t)h;
    spr->offx      = a->x;
    spr->offy      = a->y;
    spr->pivotx    = (int16_t)(w / 2u);
    spr->pivoty    = (int16_t)(h / 2u);
    spr->centerx   = (int16_t)(a->x + spr->pivotx);
    spr->centery   = (int16_t)(a->y + spr->pivoty);
    spr->blend     = 0;                    /* NORMAL: a fallback shadows nothing */
    spr->show      = 1;
    /* Non-zero on purpose: gfx_sprite_drawable() refuses color_off == 0, because a
     * zero offset in a bank record is the signature of an unset field. A generated
     * plate that claimed 0 would be invisible to the dial and would read as a
     * broken fallback rather than a deliberate one. */
    spr->color_off = PRIM_LEAD;
    return true;
}
