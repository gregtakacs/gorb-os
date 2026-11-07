/* fx.h — the weather's life: particles drawn behind the weather now (docs/weather-plan.md §2.7).
 *
 * Rain falls, drizzle falls lighter, snow drifts down and sways, fog's mist and cloud's
 * wisps pass across, thunder pours, and a clear night has stars that twinkle. A clear day
 * has none. Each particle is one of a pool of theme rows naming one bank of frames (the
 * bake's PART_FRAMES, in FX_F_* order); this file decides where each one is and how bright,
 * main.c poses the rows. Pure: no orb import, so tools/host_weather_fx_test.py compiles it.
 * Motion is in px per second against the time that passed, so the look does not depend on
 * the frame rate. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define FX_MAX   16          /* the bake's N_PART: the most rows the pool offers */
#define FX_SIDE  466
#define FX_R     233.0f      /* the glass: a particle leaving it respawns */

/* The frames, in the bake's PART_FRAMES order. */
enum { FX_F_DROP, FX_F_DROP_SMALL, FX_F_FLAKE, FX_F_FLAKE_SMALL, FX_F_MIST, FX_F_WISP,
       FX_F_STAR, FX_F_N };

typedef struct {
    float   x, y;            /* centre, glass px */
    float   vx, vy;          /* px / s */
    float   ph, rate;        /* a sway's or a twinkle's phase (turns) and its turns / s */
    uint8_t f;               /* FX_F_* */
    uint8_t opa;             /* at rest, before a twinkle */
} fx_part_t;

/* Thunder's lightning (owner, 2026-11-07: "I don't see any difference between that and
 * regular rain"): one bolt sprite, struck every few seconds somewhere over the icon, in a
 * strike's rhythm: bright, dark, bright again, fading. */
typedef struct {
    float x, y;              /* the bolt's centre, glass px */
    float t;                 /* seconds into the strike; < 0 between strikes */
    float wait;              /* seconds to the next strike */
} fx_bolt_t;

typedef struct {
    int       kind;          /* the wx.h WX_* the pool is set for; -1 none */
    int       n;             /* particles live */
    uint32_t  seed;
    fx_part_t p[FX_MAX];
    fx_bolt_t bolt;
} fx_t;

/** Set the pool for a condition (wx.h WX_*), with `rows` rows to draw on (the theme's
 *  pool, at most FX_MAX). The same kind again keeps the particles where they are. */
void fx_set(fx_t *fx, int kind, int rows);

/** Advance by dt seconds (clamped, so a stalled tick does not teleport the rain). */
void fx_step(fx_t *fx, float dt);

/** Particle i's opacity now (a star's twinkle included); 0 = not drawn. */
int fx_opa(const fx_t *fx, int i);

/** Particle i's sideways sway now, px (snow's; 0 for every other kind). */
float fx_sway(const fx_t *fx, int i);

/** The bolt's opacity now (0 between strikes, and for every kind but thunder); its
 *  centre in *x, *y. */
int fx_bolt(const fx_t *fx, float *x, float *y);

/** True while the pool has anything to draw, so the caller keeps the frames coming. */
static inline bool fx_live(const fx_t *fx) { return fx->n > 0; }
