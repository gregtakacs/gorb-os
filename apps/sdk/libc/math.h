/* apps/sdk/libc/math.h — the maths an app may use.
 *
 * Square roots, rounding and min/max compile to wasm instructions. The transcendental
 * functions are the OS's libm, reached through the orb.* imports apps/sdk/orb.h
 * declares, so an app carries no polynomial tables of its own and gets the same
 * answers the firmware's own C does. */
#pragma once

double orb_sin(double)  __attribute__((import_module("orb"), import_name("sin")));
double orb_cos(double)  __attribute__((import_module("orb"), import_name("cos")));
double orb_asin(double) __attribute__((import_module("orb"), import_name("asin")));
double orb_acos(double) __attribute__((import_module("orb"), import_name("acos")));
double orb_atan2(double, double) __attribute__((import_module("orb"), import_name("atan2")));
double orb_fmod(double, double)  __attribute__((import_module("orb"), import_name("fmod")));

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define NAN         __builtin_nanf("")
#define INFINITY    __builtin_inff()
#define isnan(x)    __builtin_isnan(x)
#define isfinite(x) __builtin_isfinite(x)

static inline double sqrt(double x)  { return __builtin_sqrt(x); }
static inline float  sqrtf(float x)  { return __builtin_sqrtf(x); }
static inline double fabs(double x)  { return __builtin_fabs(x); }
static inline float  fabsf(float x)  { return __builtin_fabsf(x); }
static inline double floor(double x) { return __builtin_floor(x); }
static inline float  floorf(float x) { return __builtin_floorf(x); }
static inline double ceil(double x)  { return __builtin_ceil(x); }

/* wasm has no round-half-away or IEEE max/min instruction, so these are functions in
 * apps/sdk/orb_rt.c rather than builtins (a builtin would lower to a call anyway). */
double round(double x);
float  roundf(float x);
long   lround(double x);
long   lroundf(float x);
float  fmaxf(float a, float b);
float  fminf(float a, float b);
double fmax(double a, double b);
double fmin(double a, double b);

static inline double sin(double x)  { return orb_sin(x); }
static inline double cos(double x)  { return orb_cos(x); }
static inline double asin(double x) { return orb_asin(x); }
static inline double acos(double x) { return orb_acos(x); }
static inline double atan2(double y, double x) { return orb_atan2(y, x); }
static inline double fmod(double x, double y)  { return orb_fmod(x, y); }
static inline float  sinf(float x)  { return (float)orb_sin(x); }
static inline float  cosf(float x)  { return (float)orb_cos(x); }
static inline float  asinf(float x) { return (float)orb_asin(x); }
static inline float  acosf(float x) { return (float)orb_acos(x); }
static inline float  atan2f(float y, float x) { return (float)orb_atan2(y, x); }
static inline float  fmodf(float x, float y)  { return (float)orb_fmod(x, y); }
