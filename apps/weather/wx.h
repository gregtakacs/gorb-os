#pragma once
/* The weather model: Open-Meteo's forecast for home and up to three places, read one
 * JSON token at a time (docs/weather-plan.md §2.3).
 *
 * D51: the OS tokenizes the body; this file is only the schema — which keys are a
 * place's and what they mean. Pure C: the app builds it against apps/sdk/orb.h, and
 * tools/host_weather_test.py builds it against main/app/orb_json.h, whose token is the
 * same type, and feeds it real bodies. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __wasm__
#include "orb.h"          /* orb_json_tok_t, ORB_JSON_* */
#else
#include "orb_json.h"
#endif

#define WX_PLACES   4      /* home and the three places (§0 ruling 1) */
#define WX_HOURS   24
#define WX_DAYS     6      /* today, for Now's high and low, and the five after it */
#define WX_DAY_ROWS 5      /* §0 ruling 2: the days view, tomorrow first (§2.4) */
#define WX_NAME_MAX 24

/* The ten icon conditions, in the order of the theme's icon frames (plan §2.4). */
enum {
    WX_CLEAR = 0, WX_CLEAR_N, WX_PARTLY, WX_PARTLY_N, WX_CLOUDY, WX_FOG,
    WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_THUNDER, WX_KINDS
};

typedef struct {
    float   t, feels, wind;     /* °C, °C, km/h: always metric on the wire (§2.3) */
    int16_t code;               /* WMO */
    bool    day;
} wx_now_t;

typedef struct {
    int64_t time;               /* unix s */
    float   t;
    int16_t code;
    int8_t  pop;                /* rain chance %, -1 unknown */
    float   pressure;           /* hPa at sea level, 0 unknown */
} wx_hour_t;

typedef struct {
    int64_t time;               /* unix s, the place's local midnight */
    float   hi, lo;
    int16_t code;
    int8_t  pop;
    int64_t sunrise, sunset;
    float   rain, wind;         /* mm, km/h: the day's total and its strongest (-1 unknown) */
    float   uv;                 /* the day's peak UV index (-1 unknown) */
} wx_day_t;

typedef struct {
    int32_t   utc_offset;       /* s, the place's own (timezone=auto) */
    wx_now_t  now;
    wx_hour_t hour[WX_HOURS];
    wx_day_t  day[WX_DAYS];
    uint8_t   n_hours, n_days;
    bool      have_now;
} wx_place_t;

/* One place the app asks about. */
typedef struct {
    char   name[WX_NAME_MAX];
    double lat, lon;
} wx_where_t;

/* "Lisbon|38.7223,-9.1393": the app setting's form (§2.3). False for anything else. */
bool wx_where_parse(const char *s, wx_where_t *out);

/* The one request for n places, into buf. Answers its length, or 0 when it would not fit. */
size_t wx_url(char *buf, size_t cap, const wx_where_t *w, int n);

/* The reader. Feed every token in order; wx_done says the body was whole. */
typedef struct {
    wx_place_t *out;            /* the caller's, n_out of them */
    int         n_out;
    int         base;           /* depth of a place's own scalars: 1 one place, 2 many */
    int         place;          /* the place being read, -1 before the first */
    char        sect[32];       /* "current", "hourly", "daily", or "" */
    char        arr[32];        /* the array being read */
    int         idx;            /* its next element */
    int         places;         /* places seen */
    bool        started, error, closed;
} wx_reader_t;

void wx_begin(wx_reader_t *r, wx_place_t *out, int n_out);
bool wx_token(wx_reader_t *r, const orb_json_tok_t *t);
bool wx_done(const wx_reader_t *r);

/* WMO code to icon condition, night forms when it is dark. */
int wx_kind(int code, bool day);

/* Is `t` between that day's sunrise and sunset? Days unknown read as day. */
bool wx_is_day(const wx_place_t *p, int64_t t);

/* The place's local hour of day and weekday (0 = Sunday) for unix time t. */
int wx_local_hour(const wx_place_t *p, int64_t t);
int wx_local_wday(const wx_place_t *p, int64_t t);
/* The place's local minute of the hour for unix time t. */
int wx_local_min(const wx_place_t *p, int64_t t);

/* The next sunrise or sunset after `t` (Now's dawn/dusk line): its unix time, with *rise
 * true for a sunrise; 0 when the days hold none (a polar day or night, or no days yet). */
int64_t wx_next_sun(const wx_place_t *p, int64_t t, bool *rise);

/* The pressure at hour h and its trend over the three hours after it: *trend 1 rising,
 * -1 falling (a hectopascal or more either way), 0 steady. 0 when the hour has none. */
float wx_pressure(const wx_place_t *p, int h, int *trend);

/* Frame pickers for the theme's rows (plan §2.4), shared with the editor's bake:
 *   a bar: length level 0..7 (by where t sits in [lo, hi]) x 3 + rain level 0..2;
 *   a day bar: lo and hi positions 0..6 on the week's scale, i <= j, triangular;
 *   the place dots: n places 1..4, the selected one, triangular. */
#define WX_BAR_LEN   8
#define WX_BAR_RAIN  3
#define WX_BAR_FRAMES (WX_BAR_LEN * WX_BAR_RAIN)
#define WX_DAY_POS   7
#define WX_DAY_FRAMES (WX_DAY_POS * (WX_DAY_POS + 1) / 2)
#define WX_DOT_FRAMES 10
int wx_bar_frame(float t, float lo, float hi, int pop);
int wx_day_frame(float lo, float hi, float wk_lo, float wk_hi);
int wx_dot_frame(int n, int sel);

/* Whole degrees in the owner's unit, rounded half away from zero. */
int wx_round(double v);
