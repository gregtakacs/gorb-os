#!/usr/bin/env python3
"""Host check for apps/weather/wx.c, the weather app's model (docs/weather-plan.md).

    python3 tools/host_weather_test.py             # all checks
    python3 tools/host_weather_test.py --mutations # do the checks actually bite?

wx.c is compiled as is, beside the OS's own tokenizer (main/app/orb_json.c, the one
the app's orb_json_next runs), and fed bodies shaped exactly as Open-Meteo answers:
an object for one place, an array of them for several, `null` where a model has no
value. What it pins:

  url      the one request for four places fits the OS's 480 B path
           (orb_app_net.c) with room, and names every field the screens read
  where    "Name|lat,lon", the app setting's form, and nothing else
  read     one place and four; current, 24 hours, 5 days, the place's own offset;
           nulls read as unknown; a torn body is not whole
  kinds    WMO codes to the ten icons, the night forms after dark
  time     the place's local hour and weekday from its own offset
  frames   the bar, day bar and dot pickers the theme's banks are laid out by
"""
import json, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "apps", "weather", "wx.c")
INC = os.path.join(ROOT, "apps", "weather")
JSON_C = os.path.join(ROOT, "main", "app", "orb_json.c")
JSON_INC = os.path.join(ROOT, "main", "app")

T0 = 1793966400            # 2026-11-06 12:00 UTC, a Friday


def place(off, t_now, code, day, hours, days):
    return {
        "latitude": 51.5, "longitude": -0.12, "generationtime_ms": 0.1,
        "utc_offset_seconds": off, "timezone": "GMT", "timezone_abbreviation": "GMT",
        "elevation": 10.0,
        "current_units": {"time": "unixtime", "temperature_2m": "°C"},
        "current": {"time": T0, "interval": 900, "temperature_2m": t_now,
                    "apparent_temperature": t_now - 2, "weather_code": code,
                    "is_day": day, "wind_speed_10m": 14.4},
        "hourly_units": {"time": "unixtime"},
        "hourly": {"time": [T0 + 3600 * i for i in range(hours)],
                   "temperature_2m": [10 + i % 9 for i in range(hours)],
                   "weather_code": [61 if i == 3 else 2 for i in range(hours)],
                   "precipitation_probability": [None if i == 5 else 5 * i for i in range(hours)]},
        "daily_units": {"time": "unixtime"},
        "daily": {"time": [T0 - 12 * 3600 - off + 86400 * i for i in range(days)],
                  "weather_code": [95, 3, 0, 71, 45][:days],
                  "temperature_2m_max": [18.4, 17, 16, 15, 14][:days],
                  "temperature_2m_min": [9.6, 8, 7, -2.5, 5][:days],
                  "precipitation_probability_max": [80, None, 0, 40, 10][:days],
                  "sunrise": [T0 - 5 * 3600 + 86400 * i for i in range(days)],
                  "sunset": [T0 + 4 * 3600 + 86400 * i for i in range(days)],
                  "precipitation_sum": [6.2, None, 0, 1.5, 0.2][:days],
                  "wind_speed_10m_max": [24.1, 18, None, 30, 12][:days],
                  "uv_index_max": [4.6, 3, 5, None, 2][:days]},
    }


ONE = json.dumps(place(0, 17.2, 2, 1, 24, 5))
MANY = json.dumps([place(0, 17.2, 2, 1, 24, 5), place(3600, 22.0, 0, 1, 24, 5),
                   place(-25200, -1.0, 71, 0, 24, 5), place(32400, 13.0, 61, 0, 24, 5)])
TORN = MANY[: len(MANY) // 2]


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


DRIVER = r"""
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "wx.h"

static int fails;
#define CK(c, label) do { if (c) printf("ok    %s\n", label); else { printf("FAIL  %s\n", label); fails++; } } while (0)

static wx_place_t P[WX_PLACES];

static bool read_body(const char *body, int n_out, wx_reader_t *r)
{
    orb_json_t j;
    orb_json_begin(&j, body, strlen(body));
    wx_begin(r, P, n_out);
    orb_json_tok_t t;
    for (int guard = 0; guard < 100000; guard++) {
        const int32_t k = orb_json_next(&j, &t);
        if (!wx_token(r, &t) || k == ORB_JSON_END || k == ORB_JSON_ERROR) break;
    }
    return wx_done(r);
}

int main(void)
{
    /* url */
    wx_where_t w[4] = { { "Home", 51.4700, -0.4543 }, { "Lisbon", 38.7223, -9.1393 },
                        { "Denver", 39.7392, -104.9903 }, { "Tokyo", 35.6764, 139.6503 } };
    char url[600];
    const size_t n = wx_url(url, sizeof url, w, 4);
    const char *path = strstr(url, ".com") ? strstr(url, ".com") + 4 : url;
    CK(n > 0 && strlen(path) < 450, "four places fit the OS's 480 B path");
    { const wx_where_t far[4] = { { "A", -89.99, -179.99 }, { "B", -89.99, -179.99 },
                                  { "C", -89.99, -179.99 }, { "D", -89.99, -179.99 } };
      char u2[600];
      const size_t n2 = wx_url(u2, sizeof u2, far, 4);
      CK(n2 > 0 && strlen(strstr(u2, ".com") + 4) < 470,
         "four places at the longest coordinates still fit it"); }
    CK(strstr(url, "latitude=51.47,38.72,39.74,35.68&") && strstr(url, "longitude=-0.45,-9.14,-104.99,139.65&"),
       "the places go as two comma lists, two decimals each");
    CK(strstr(url, "current=temperature_2m,apparent_temperature,weather_code,is_day,wind_speed_10m") &&
       strstr(url, "hourly=temperature_2m,weather_code,precipitation_probability") &&
       strstr(url, "precipitation_probability_max,sunrise,sunset,precipitation_sum,wind_speed_10m_max,uv_index_max") &&
       strstr(url, "forecast_hours=24&forecast_days=6&timezone=auto&timeformat=unixtime"),
       "the request names every field the screens read");
    CK(wx_url(url, 64, w, 4) == 0 && wx_url(url, sizeof url, w, 0) == 0 && wx_url(url, sizeof url, w, 5) == 0,
       "no request that would not fit, and none for 0 or 5 places");

    /* where */
    wx_where_t x;
    CK(wx_where_parse("Lisbon|38.72,-9.14", &x) && !strcmp(x.name, "Lisbon") &&
       fabs(x.lat - 38.72) < 1e-9 && fabs(x.lon + 9.14) < 1e-9, "a place setting parses");
    CK(!wx_where_parse("", &x) && !wx_where_parse("Lisbon", &x) && !wx_where_parse("|1,2", &x) &&
       !wx_where_parse("A|1", &x) && !wx_where_parse("A|1,2x", &x) && !wx_where_parse("A|91,2", &x) &&
       !wx_where_parse("A|1,181", &x) && !wx_where_parse(NULL, &x),
       "anything else is no place");

    /* read: one place */
    wx_reader_t r;
    CK(read_body(ONE_BODY, 1, &r) && r.places == 1, "one place: an object, read whole");
    CK(P[0].have_now && fabsf(P[0].now.t - 17.2f) < 1e-4 && P[0].now.code == 2 && P[0].now.day &&
       fabsf(P[0].now.wind - 14.4f) < 1e-4 && fabsf(P[0].now.feels - 15.2f) < 1e-4,
       "current: temperature, feels-like, code, day, wind");
    CK(P[0].n_hours == 24 && P[0].hour[0].time == T0 && P[0].hour[3].code == 61 &&
       P[0].hour[8].t == 18 && P[0].hour[4].pop == 20 && P[0].hour[5].pop == -1,
       "24 hours, a null rain chance read as unknown");
    CK(P[0].n_days == 5 && fabsf(P[0].day[0].hi - 18.4f) < 1e-4 && fabsf(P[0].day[3].lo + 2.5f) < 1e-4 &&
       P[0].day[0].code == 95 && P[0].day[0].pop == 80 && P[0].day[1].pop == -1 && P[0].day[0].sunset == T0 + 4 * 3600,
       "5 days: highs, lows, codes, rain, the sun");
    CK(fabsf(P[0].day[0].rain - 6.2f) < 1e-4 && P[0].day[1].rain == -1 && P[0].day[2].rain == 0 &&
       fabsf(P[0].day[0].wind - 24.1f) < 1e-4 && P[0].day[2].wind == -1 &&
       fabsf(P[0].day[0].uv - 4.6f) < 1e-4 && P[0].day[3].uv == -1 && P[0].day[5].uv == -1,
       "the days' rain total, strongest wind and UV, a null or a missing day read as unknown");

    /* read: many */
    CK(read_body(MANY_BODY, 4, &r) && r.places == 4, "four places: an array, read whole");
    CK(P[1].utc_offset == 3600 && P[2].utc_offset == -25200 && P[3].utc_offset == 32400 &&
       fabsf(P[1].now.t - 22.0f) < 1e-4 && P[2].now.code == 71 && !P[2].now.day && P[3].n_hours == 24,
       "each place keeps its own offset and its own answer");
    CK(!read_body(TORN_BODY, 4, &r), "a torn body is not whole");
    CK(read_body(MANY_BODY, 2, &r) && P[1].now.code == 0, "more places than kept: the first ones are kept");

    /* kinds */
    CK(wx_kind(0, true) == WX_CLEAR && wx_kind(1, false) == WX_CLEAR_N && wx_kind(2, true) == WX_PARTLY &&
       wx_kind(2, false) == WX_PARTLY_N && wx_kind(3, false) == WX_CLOUDY && wx_kind(48, true) == WX_FOG &&
       wx_kind(55, true) == WX_DRIZZLE && wx_kind(81, true) == WX_RAIN && wx_kind(65, false) == WX_RAIN &&
       wx_kind(86, true) == WX_SNOW && wx_kind(99, true) == WX_THUNDER && wx_kind(4, true) == WX_CLOUDY,
       "WMO codes name the ten icons, night forms after dark");

    /* time (one place, offset 0: sunrise 07:00, sunset 16:00 UTC) */
    read_body(ONE_BODY, 1, &r);
    CK(wx_is_day(&P[0], T0) && !wx_is_day(&P[0], T0 + 5 * 3600) && !wx_is_day(&P[0], T0 - 6 * 3600) &&
       wx_is_day(&P[0], T0 + 86400 + 3600), "day between sunrise and sunset, by the day it falls in");
    { bool rise = false;
      const int64_t a = wx_next_sun(&P[0], T0, &rise);
      CK(a == T0 + 4 * 3600 && !rise, "by day, the next sun event is the sunset");
      const int64_t b = wx_next_sun(&P[0], T0 + 5 * 3600, &rise);
      CK(b == T0 - 5 * 3600 + 86400 && rise, "after it, the next morning's sunrise");
      const int64_t c = wx_next_sun(&P[0], T0 - 6 * 3600, &rise);
      CK(c == T0 - 5 * 3600 && rise, "before dawn, today's sunrise");
      CK(wx_next_sun(&P[0], T0 + 86400 * 9, &rise) == 0, "past the last day, none");
      CK(wx_local_min(&P[0], T0 + 7 * 60 + 59) == 7, "the local minute"); }
    read_body(MANY_BODY, 4, &r);
    CK(wx_local_hour(&P[0], T0) == 12 && wx_local_hour(&P[3], T0) == 21 && wx_local_hour(&P[2], T0) == 5,
       "the local hour is the place's own");
    CK(wx_local_wday(&P[0], T0) == 5 && wx_local_wday(&P[3], T0 + 3 * 3600) == 6 &&
       wx_local_wday(&P[2], T0 - 6 * 3600) == 4, "the weekday too, across midnight both ways");

    /* frames */
    CK(wx_bar_frame(10, 10, 18, 0) == 0 && wx_bar_frame(18, 10, 18, 0) == (WX_BAR_LEN - 1) * WX_BAR_RAIN &&
       wx_bar_frame(14, 10, 18, 85) == 4 * WX_BAR_RAIN + 2 && wx_bar_frame(12, 12, 12, 30) == 4 * WX_BAR_RAIN + 1 &&
       wx_bar_frame(12, 12, 12, 29) == 4 * WX_BAR_RAIN && WX_BAR_FRAMES == 24 && WX_DAY_FRAMES == 28 &&
       wx_bar_frame(30, 10, 18, -1) < WX_BAR_FRAMES, "a bar's length by temperature, its dot by rain, in range");
    int seen[WX_DAY_FRAMES] = { 0 }, all = 1;
    for (int i = 0; i < WX_DAY_POS; i++)
        for (int j = i; j < WX_DAY_POS; j++) {
            const float a = 7.0f + i * (11.0f / (WX_DAY_POS - 1)), b = 7.0f + j * (11.0f / (WX_DAY_POS - 1));
            const int f = wx_day_frame(a, b, 7, 18);
            if (f < 0 || f >= WX_DAY_FRAMES || seen[f]) all = 0; else seen[f] = 1;
        }
    CK(all && wx_day_frame(18, 7, 7, 18) == wx_day_frame(7, 18, 7, 18),
       "every lo/hi pair has its own day-bar frame, in range");
    CK(wx_dot_frame(1, 0) == 0 && wx_dot_frame(2, 1) == 2 && wx_dot_frame(4, 0) == 6 && wx_dot_frame(4, 3) == 9 &&
       wx_dot_frame(9, 9) == 9, "the dots: n places and the selected one, clamped");
    CK(wx_round(2.5) == 3 && wx_round(-2.5) == -3 && wx_round(-0.4) == 0 && wx_round(17.49) == 17,
       "whole degrees, half away from zero");
    return fails ? 1 : 0;
}
"""

MUTATIONS = [
    ('if (b == 2 && d == 1) { r->place++;', 'if (b == 2 && d == 2) { r->place++;'),
    ('else if (d == b + 2 && r->arr[0]) {', 'else if (d == b + 1 && r->arr[0]) {'),
    ('case 2:                               return day ? WX_PARTLY : WX_PARTLY_N;',
     'case 2:                               return WX_PARTLY;'),
    ('return t >= d->sunrise && t < d->sunset;', 'return t >= d->sunrise;'),
    ('if (d->sunrise > t) { *rise = true; return d->sunrise; }', ';'),
    ('d->wind = isnan(v) ? -1 : (float)v;', 'd->wind = (float)v;'),
    ('return (int)fmod64(days + 4, 7);', 'return (int)fmod64(days + 3, 7);'),
    ('h->pop = isnan(v) ? -1 : (int8_t)v;', 'h->pop = isnan(v) ? 0 : (int8_t)v;'),
    ('return WX_DAY_POS * i - i * (i - 1) / 2 + (j - i);', 'return WX_DAY_POS * i + (j - i);'),
    ('"%s%.2f"', '"%s%.4f"'),
    ('if (end == lon || *end) return false;', 'if (end == lon) return false;'),
    ('else if (b == 1 && d == 0) { r->places = 1; r->closed = true; }', 'else if (b == 1 && d == 0) { r->places = 1; }'),
]


def build(src_text, exe):
    with tempfile.TemporaryDirectory() as d:
        drv = os.path.join(d, "drv.c")
        wx = os.path.join(d, "wx.c")
        open(wx, "w").write(src_text)
        open(drv, "w").write(
            "#define T0 %dLL\n#define ONE_BODY %s\n#define MANY_BODY %s\n#define TORN_BODY %s\n%s"
            % (T0, c_str(ONE), c_str(MANY), c_str(TORN), DRIVER))
        r = subprocess.run(["gcc", "-std=c11", "-O1", "-Wall", "-Werror", "-D_GNU_SOURCE",
                            "-I", INC, "-I", JSON_INC, "-o", exe, drv, wx, JSON_C, "-lm"],
                           capture_output=True, text=True)
        if r.returncode:
            return r.stderr
    return None


def run(src_text):
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "t")
        err = build(src_text, exe)
        if err:
            return 2, "COMPILE FAILED:\n" + err
        p = subprocess.run([exe], capture_output=True, text=True)
        return p.returncode, p.stdout


def main():
    src = open(SRC).read()
    if "--mutations" in sys.argv:
        caught = 0
        for old, new in MUTATIONS:
            if old not in src:
                print("mutation %r does not apply -- rewrite the bench" % old)
                return 1
            rc, _ = run(src.replace(old, new))
            print(("bites " if rc else "MISSED") + "  " + old[:64])
            caught += bool(rc)
        print("%d of %d mutants caught" % (caught, len(MUTATIONS)))
        return 0 if caught == len(MUTATIONS) else 1
    rc, out = run(src)
    print(out, end="")
    return rc


if __name__ == "__main__":
    sys.exit(main())
