/* The weather model — contract in wx.h. */
#include "wx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool wx_where_parse(const char *s, wx_where_t *out)
{
    memset(out, 0, sizeof *out);
    if (!s) return false;
    const char *bar = strchr(s, '|');
    if (!bar || bar == s || (size_t)(bar - s) >= sizeof out->name) return false;
    memcpy(out->name, s, (size_t)(bar - s));
    char *end = NULL;
    out->lat = strtod(bar + 1, &end);
    if (end == bar + 1 || *end != ',') return false;
    const char *lon = end + 1;
    out->lon = strtod(lon, &end);
    if (end == lon || *end) return false;
    return out->lat >= -90 && out->lat <= 90 && out->lon >= -180 && out->lon <= 180;
}

/* Two decimals is about a kilometre, finer than the models' grid, and keeps four places
 * inside the OS's 480 B path (orb_app_net.c). */
static size_t coords(char *b, size_t cap, const wx_where_t *w, int n, bool lat)
{
    size_t o = 0;
    for (int i = 0; i < n && o < cap; i++) {
        const int k = snprintf(b + o, cap - o, "%s%.2f", i ? "," : "", lat ? w[i].lat : w[i].lon);
        if (k < 0 || (size_t)k >= cap - o) return cap;
        o += (size_t)k;
    }
    return o;
}

size_t wx_url(char *buf, size_t cap, const wx_where_t *w, int n)
{
    if (n < 1 || n > WX_PLACES) return 0;
    char la[96], lo[96];
    if (coords(la, sizeof la, w, n, true) >= sizeof la || coords(lo, sizeof lo, w, n, false) >= sizeof lo)
        return 0;
    const int k = snprintf(buf, cap,
        "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
        "&current=temperature_2m,apparent_temperature,weather_code,is_day,wind_speed_10m"
        "&hourly=temperature_2m,weather_code,precipitation_probability,pressure_msl"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,"
        "precipitation_sum,wind_speed_10m_max,uv_index_max"
        "&forecast_hours=24&forecast_days=6&timezone=auto&timeformat=unixtime", la, lo);
    return k > 0 && (size_t)k < cap ? (size_t)k : 0;
}

void wx_begin(wx_reader_t *r, wx_place_t *out, int n_out)
{
    memset(r, 0, sizeof *r);
    r->out = out;
    r->n_out = n_out;
    r->place = -1;
    memset(out, 0, sizeof *out * (size_t)n_out);
    for (int p = 0; p < n_out; p++) {
        for (int i = 0; i < WX_HOURS; i++) out[p].hour[i].pop = -1;
        for (int i = 0; i < WX_DAYS; i++) out[p].day[i].rain = out[p].day[i].wind = out[p].day[i].uv = -1;
    }
}

static wx_place_t *cur(wx_reader_t *r)
{
    return r->place >= 0 && r->place < r->n_out ? &r->out[r->place] : NULL;
}

static void take_current(wx_place_t *p, const char *k, double v)
{
    if (!strcmp(k, "temperature_2m")) { p->now.t = (float)v; p->have_now = true; }
    else if (!strcmp(k, "apparent_temperature")) p->now.feels = (float)v;
    else if (!strcmp(k, "weather_code")) p->now.code = (int16_t)v;
    else if (!strcmp(k, "is_day")) p->now.day = v != 0;
    else if (!strcmp(k, "wind_speed_10m")) p->now.wind = (float)v;
}

/* One element of an hourly or daily array; `null` (a value the model has not got) is
 * NAN, which reads as unknown below. */
static void take_element(wx_reader_t *r, wx_place_t *p, double v)
{
    const char *a = r->arr;
    const int i = r->idx;
    if (!strcmp(r->sect, "hourly")) {
        if (i >= WX_HOURS) return;
        wx_hour_t *h = &p->hour[i];
        if (!strcmp(a, "time")) h->time = (int64_t)v;
        else if (!strcmp(a, "temperature_2m")) h->t = (float)v;
        else if (!strcmp(a, "weather_code")) h->code = isnan(v) ? 3 : (int16_t)v;
        else if (!strcmp(a, "precipitation_probability")) h->pop = isnan(v) ? -1 : (int8_t)v;
        else if (!strcmp(a, "pressure_msl")) h->pressure = isnan(v) ? 0 : (float)v;
        else return;
        if (i + 1 > p->n_hours) p->n_hours = (uint8_t)(i + 1);
    } else if (!strcmp(r->sect, "daily")) {
        if (i >= WX_DAYS) return;
        wx_day_t *d = &p->day[i];
        if (!strcmp(a, "time")) d->time = (int64_t)v;
        else if (!strcmp(a, "weather_code")) d->code = isnan(v) ? 3 : (int16_t)v;
        else if (!strcmp(a, "temperature_2m_max")) d->hi = (float)v;
        else if (!strcmp(a, "temperature_2m_min")) d->lo = (float)v;
        else if (!strcmp(a, "precipitation_probability_max")) d->pop = isnan(v) ? -1 : (int8_t)v;
        else if (!strcmp(a, "sunrise")) d->sunrise = isnan(v) ? 0 : (int64_t)v;
        else if (!strcmp(a, "sunset")) d->sunset = isnan(v) ? 0 : (int64_t)v;
        else if (!strcmp(a, "precipitation_sum")) d->rain = isnan(v) ? -1 : (float)v;
        else if (!strcmp(a, "wind_speed_10m_max")) d->wind = isnan(v) ? -1 : (float)v;
        else if (!strcmp(a, "uv_index_max")) d->uv = isnan(v) ? -1 : (float)v;
        else return;
        if (i + 1 > p->n_days) p->n_days = (uint8_t)(i + 1);
    }
}

bool wx_token(wx_reader_t *r, const orb_json_tok_t *t)
{
    if (r->error) return false;
    if (!r->started) {
        r->started = true;
        if (t->type == ORB_JSON_ARR && t->depth == 0) r->base = 2;          /* many places */
        else if (t->type == ORB_JSON_OBJ && t->depth == 0) { r->base = 1; r->place = 0; }
        else { r->error = true; return false; }
        return true;
    }
    const int d = t->depth, b = r->base;
    wx_place_t *p = cur(r);
    switch (t->type) {
    case ORB_JSON_ERROR:
        r->error = true;
        return false;
    case ORB_JSON_OBJ:
        if (b == 2 && d == 1) { r->place++; r->sect[0] = 0; }
        else if (d == b) snprintf(r->sect, sizeof r->sect, "%s", t->key_trunc ? "" : t->key);
        break;
    case ORB_JSON_OBJ_END:
        if (d == b) r->sect[0] = 0;
        else if (b == 2 && d == 1) r->places++;
        else if (b == 1 && d == 0) { r->places = 1; r->closed = true; }
        break;
    case ORB_JSON_ARR:
        if (d == b + 1 && r->sect[0]) { snprintf(r->arr, sizeof r->arr, "%s", t->key_trunc ? "" : t->key); r->idx = 0; }
        break;
    case ORB_JSON_ARR_END:
        if (d == b + 1) r->arr[0] = 0;
        else if (b == 2 && d == 0) r->closed = true;
        break;
    case ORB_JSON_END:
        break;
    default:                                                    /* a scalar */
        if (!p) break;
        if (d == b && t->type == ORB_JSON_NUM && !strcmp(t->key, "utc_offset_seconds"))
            p->utc_offset = (int32_t)t->num;
        else if (d == b + 1 && !strcmp(r->sect, "current") && t->type == ORB_JSON_NUM)
            take_current(p, t->key, t->num);
        else if (d == b + 2 && r->arr[0]) {
            take_element(r, p, t->type == ORB_JSON_NUM ? t->num : NAN);
            r->idx++;
        }
        break;
    }
    return true;
}

bool wx_done(const wx_reader_t *r)
{
    return !r->error && r->closed && r->places >= 1;
}

int wx_kind(int code, bool day)
{
    switch (code) {
    case 0: case 1:                       return day ? WX_CLEAR : WX_CLEAR_N;
    case 2:                               return day ? WX_PARTLY : WX_PARTLY_N;
    case 3:                               return WX_CLOUDY;
    case 45: case 48:                     return WX_FOG;
    case 51: case 53: case 55: case 56: case 57: return WX_DRIZZLE;
    case 61: case 63: case 65: case 66: case 67:
    case 80: case 81: case 82:            return WX_RAIN;
    case 71: case 73: case 75: case 77: case 85: case 86: return WX_SNOW;
    case 95: case 96: case 99:            return WX_THUNDER;
    default:                              return WX_CLOUDY;
    }
}

bool wx_is_day(const wx_place_t *p, int64_t t)
{
    for (int i = 0; i < p->n_days; i++) {
        const wx_day_t *d = &p->day[i];
        if (t >= d->time && t < d->time + 86400) {
            if (!d->sunrise || !d->sunset) return true;
            return t >= d->sunrise && t < d->sunset;
        }
    }
    return true;
}

static int64_t fmod64(int64_t a, int64_t m) { const int64_t r = a % m; return r < 0 ? r + m : r; }

int wx_local_hour(const wx_place_t *p, int64_t t)
{
    return (int)(fmod64(t + p->utc_offset, 86400) / 3600);
}

int wx_local_min(const wx_place_t *p, int64_t t)
{
    return (int)(fmod64(t + p->utc_offset, 3600) / 60);
}

int64_t wx_next_sun(const wx_place_t *p, int64_t t, bool *rise)
{
    for (int i = 0; i < p->n_days; i++) {
        const wx_day_t *d = &p->day[i];
        if (d->sunrise > t) { *rise = true; return d->sunrise; }     /* a day's dawn comes first */
        if (d->sunset > t) { *rise = false; return d->sunset; }
    }
    return 0;
}

float wx_pressure(const wx_place_t *p, int h, int *trend)
{
    *trend = 0;
    if (h < 0 || h >= p->n_hours || p->hour[h].pressure <= 0) return 0;
    const float now = p->hour[h].pressure;
    const int l = h + 3 < p->n_hours ? h + 3 : p->n_hours - 1;
    if (l > h && p->hour[l].pressure > 0) {
        const float d = p->hour[l].pressure - now;
        *trend = d >= 1.0f ? 1 : d <= -1.0f ? -1 : 0;
    }
    return now;
}

int wx_local_wday(const wx_place_t *p, int64_t t)
{
    const int64_t s = t + p->utc_offset;
    const int64_t days = (s - fmod64(s, 86400)) / 86400;
    return (int)fmod64(days + 4, 7);                            /* 1970-01-01 was a Thursday */
}

static int level(double v, double lo, double hi, int steps)
{
    if (!(hi > lo)) return steps / 2;
    const int k = (int)floor((v - lo) / (hi - lo) * (steps - 1) + 0.5);
    return k < 0 ? 0 : k >= steps ? steps - 1 : k;
}

int wx_bar_frame(float t, float lo, float hi, int pop)
{
    const int rain = pop < 30 ? 0 : pop < 70 ? 1 : 2;
    return level(t, lo, hi, WX_BAR_LEN) * WX_BAR_RAIN + rain;
}

int wx_day_frame(float lo, float hi, float wk_lo, float wk_hi)
{
    int i = level(lo, wk_lo, wk_hi, WX_DAY_POS), j = level(hi, wk_lo, wk_hi, WX_DAY_POS);
    if (j < i) { const int k = i; i = j; j = k; }
    return WX_DAY_POS * i - i * (i - 1) / 2 + (j - i);
}

int wx_dot_frame(int n, int sel)
{
    if (n < 1) n = 1;
    if (n > WX_PLACES) n = WX_PLACES;
    if (sel < 0) sel = 0;
    if (sel >= n) sel = n - 1;
    return n * (n - 1) / 2 + sel;
}

int wx_round(double v)
{
    return v < 0 ? -(int)floor(-v + 0.5) : (int)floor(v + 0.5);
}
