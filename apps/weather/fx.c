/* fx.c — the weather's particles (fx.h). */
#include "fx.h"
#include "wx.h"

/* xorshift32: deterministic, so a host suite can replay a pool. */
static uint32_t rnd(fx_t *fx)
{
    uint32_t x = fx->seed ? fx->seed : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return fx->seed = x;
}
static float rndf(fx_t *fx, float lo, float hi)
{
    return lo + (hi - lo) * (float)(rnd(fx) & 0xFFFFu) / 65535.0f;
}

/* sin of a phase in turns, a 5th-order fit held to [-1, 1]: within 0.5%, plenty for a sway. */
static float sin_turns(float t)
{
    t -= (float)(int)t;
    if (t < 0) t += 1.0f;
    float x = (t < 0.5f ? t : t - 1.0f) * 6.2831853f;            /* -pi .. pi */
    const float s = x < 0 ? -1.0f : 1.0f;
    x *= s;
    if (x > 1.5707963f) x = 3.1415927f - x;                         /* fold to 0..pi/2 */
    const float x2 = x * x;
    const float v = x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f));   /* 1.0045 at the top */
    return s * (v > 1.0f ? 1.0f : v);
}

/* Half the glass's chord at height y: a particle stands inside when |x - cx| < this. */
static float half_chord(float y)
{
    const float dy = y - FX_R;
    const float h2 = FX_R * FX_R - dy * dy;
    if (h2 <= 0) return 0;
    float r = h2 / FX_R;                                             /* sqrt, by Newton */
    for (int i = 0; i < 6; i++) r = 0.5f * (r + h2 / r);
    return r;
}

static bool inside(float x, float y, float pad)
{
    const float dx = x - FX_R, dy = y - FX_R, r = FX_R + pad;
    return dx * dx + dy * dy <= r * r;
}

/* A point inside the glass, y in [y0, y1]. */
static void place(fx_t *fx, fx_part_t *p, float y0, float y1)
{
    p->y = rndf(fx, y0, y1);
    const float hc = half_chord(p->y);
    p->x = FX_R + rndf(fx, -hc, hc);
}

/* One particle new: where it starts and how it moves, for the pool's kind. `fresh` places
 * it anywhere on its course (the pool appearing); otherwise at the course's start (a
 * particle that left the glass coming round again). */
static void spawn(fx_t *fx, fx_part_t *p, bool fresh)
{
    switch (fx->kind) {
    case WX_RAIN: case WX_THUNDER: case WX_DRIZZLE: {
        const bool light = fx->kind == WX_DRIZZLE;
        p->f = light ? FX_F_DROP_SMALL : FX_F_DROP;
        p->vy = light ? rndf(fx, 150, 210) : rndf(fx, 260, 360);
        p->vx = light ? 0 : rndf(fx, -18, -8);                       /* a little slant */
        p->opa = light ? 170 : 210;
        if (fresh) place(fx, p, 20, FX_SIDE - 20);
        else { p->y = rndf(fx, -10, 30); p->x = FX_R + rndf(fx, -half_chord(40), half_chord(40)); }
        break;
    }
    case WX_SNOW:
        p->f = (rnd(fx) & 1) ? FX_F_FLAKE : FX_F_FLAKE_SMALL;
        p->vy = rndf(fx, 28, 55);
        p->vx = 0;
        p->ph = rndf(fx, 0, 1);
        p->rate = rndf(fx, 0.3f, 0.7f);
        p->opa = 225;
        if (fresh) place(fx, p, 20, FX_SIDE - 20);
        else { p->y = rndf(fx, -10, 20); p->x = FX_R + rndf(fx, -half_chord(30), half_chord(30)); }
        break;
    case WX_FOG: case WX_CLOUDY: case WX_PARTLY: case WX_PARTLY_N: {
        const bool fog = fx->kind == WX_FOG;
        p->f = fog ? FX_F_MIST : FX_F_WISP;
        p->vx = fog ? rndf(fx, 8, 18) : rndf(fx, 10, 22);
        p->vy = 0;
        p->opa = fog ? 150 : 120;
        const float y0 = fog ? 150 : 90, y1 = fog ? 370 : 330;
        if (fresh) place(fx, p, y0, y1);
        else { p->y = rndf(fx, y0, y1); p->x = FX_R - half_chord(p->y) - 20; }
        break;
    }
    case WX_CLEAR_N:
        p->f = FX_F_STAR;
        p->vx = p->vy = 0;
        p->ph = rndf(fx, 0, 1);
        p->rate = rndf(fx, 0.15f, 0.45f);
        p->opa = 200;
        place(fx, p, 40, 200);                                      /* the sky, over the icon */
        break;
    default:
        p->opa = 0;
        break;
    }
}

/* How many a condition wants, at most. */
static int want(int kind)
{
    switch (kind) {
    case WX_RAIN:    return 8;
    case WX_THUNDER: return 10;
    case WX_DRIZZLE: return 6;
    case WX_SNOW:    return 10;
    case WX_FOG:     return 6;
    case WX_CLOUDY:  return 5;
    case WX_PARTLY: case WX_PARTLY_N: return 3;
    case WX_CLEAR_N: return 8;
    default:         return 0;                                       /* a clear day */
    }
}

void fx_set(fx_t *fx, int kind, int rows)
{
    int n = want(kind);
    if (n > rows) n = rows;
    if (n > FX_MAX) n = FX_MAX;
    if (n < 0) n = 0;
    if (kind == fx->kind && n == fx->n) return;
    fx->kind = kind;
    fx->n = n;
    for (int i = 0; i < n; i++) spawn(fx, &fx->p[i], true);
    fx->bolt.t = -1.0f;
    fx->bolt.wait = kind == WX_THUNDER ? rndf(fx, 1.0f, 3.0f) : 0;
}

void fx_step(fx_t *fx, float dt)
{
    if (dt <= 0) return;
    if (dt > 0.1f) dt = 0.1f;
    for (int i = 0; i < fx->n; i++) {
        fx_part_t *p = &fx->p[i];
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->ph += p->rate * dt;
        if (p->ph >= 1.0f) p->ph -= (float)(int)p->ph;
        /* past the glass on its course (below it, or off its right): round again */
        const bool gone = (p->vy > 0 && p->y > FX_R && !inside(p->x, p->y, 12)) ||
                          (p->vx > 0 && p->x > FX_R && !inside(p->x, p->y, 30));
        if (gone) spawn(fx, p, false);
    }
    if (fx->kind != WX_THUNDER) return;
    fx_bolt_t *b = &fx->bolt;
    if (b->t >= 0) {                                     /* a strike runs its 0.3 s */
        b->t += dt;
        if (b->t > 0.3f) { b->t = -1.0f; b->wait = rndf(fx, 2.5f, 7.0f); }
    } else if ((b->wait -= dt) <= 0) {                   /* the next, over the icon's sky */
        b->t = 0;
        b->x = FX_R + rndf(fx, -90, 90);
        b->y = rndf(fx, 110, 170);
    }
}

int fx_bolt(const fx_t *fx, float *x, float *y)
{
    const float t = fx->bolt.t;
    *x = fx->bolt.x;
    *y = fx->bolt.y;
    if (fx->kind != WX_THUNDER || t < 0) return 0;
    return t < 0.07f ? 255 : t < 0.12f ? 0 : t < 0.22f ? 190 : t < 0.3f ? 90 : 0;
}

int fx_opa(const fx_t *fx, int i)
{
    if (i < 0 || i >= fx->n) return 0;
    const fx_part_t *p = &fx->p[i];
    if (p->f == FX_F_STAR) {                                         /* a twinkle */
        const float s = 0.5f + 0.5f * sin_turns(p->ph);
        return (int)(60 + (p->opa - 60) * s);
    }
    return p->opa;
}

/* The snow's sway rides the drawing, not the course: main.c adds fx_sway to x. */
float fx_sway(const fx_t *fx, int i)
{
    if (i < 0 || i >= fx->n || fx->kind != WX_SNOW) return 0;
    return 10.0f * sin_turns(fx->p[i].ph);
}
