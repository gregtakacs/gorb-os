/* orb_sound.c — the sound engine: I2S + ES8311 + NS4150B, a compiled mixer, and the
 * owner's settings. See orb_sound.h for the contract and docs/sound-plan.md §4. */
#include "orb_sound.h"

#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "orb_es8311.h"
#include "orb_svc.h"      /* orb_time_now(): the wall second the bench loop anchors to */

static const char *TAG = "sound";

/* One mixer block is 256 frames, 11.6 ms; the DMA holds four of them. Four buffers is
 * 46 ms of queue in 2 KB of internal RAM, which is the pool this board runs out of
 * (status heap_internal); the queue's only job is to ride out the LVGL task's
 * scheduling jitter, and the mixer runs above it. */
#define BLOCK      256u
#define DESC_N     4u
#define VOICES     16
#define IDLE_US    2000000     /* silence this long turns the amp and I2S off */
#define SNAP_SAMP  (ORB_SOUND_RATE / 20u)   /* 50 ms: past this a loop re-enters */
#define QUIET_PEAK 600         /* about -35 dBFS: a block this quiet hides a nudge */
#define MIXER_PRIO 6           /* above lvgl (5): a late block is an audible gap */
#define LEASE_US   1500000     /* a leased voice outlives its owner's last tick by this */

typedef struct {
    const void    *owner;      /* who started it: orb_sound_stop_owner's key */
    const int16_t *pcm;
    uint32_t n, pos;
    int64_t  at_us, anchor_us;
    int      id;
    uint16_t gain;
    uint8_t  flags;
    uint8_t  state;            /* 0 free, 1 queued, 2 sounding */
} voice_t;

static i2s_chan_handle_t s_tx;
static SemaphoreHandle_t s_mx;
static TaskHandle_t      s_task;
/* The voice table and the mixer's two block buffers are PSRAM, allocated at init:
 * this board's internal heap has no static headroom at all (v1.1.42's commit records
 * 320 B of .bss crash-looping it, and the first flash of this engine with ~3 KB of
 * statics here starved esp_wifi_init). */
static voice_t          *s_v;
static int32_t          *s_acc;
static int16_t          *s_buf;
static int               s_next_id = 1;
static bool              s_ready;
static orb_sound_cfg_t   s_cfg = {
    .volume = 60, .tick = true, .chimes = true, .strikes = true, .jumps = true,
    .quiet_from = 22, .quiet_to = 7, .offset_ms = 90, .pattern = -1,
};

/* The hardware's playback clock: the instant the last DMA buffer finished, and how
 * many have. Written by the I2S interrupt, read by the mixer as a pair. */
static portMUX_TYPE s_sent_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t  s_sent_us;
static uint32_t s_sent;

static volatile bool s_running;
static volatile bool s_pa;
static uint32_t s_blocks, s_started, s_nudges, s_snaps;
static volatile int64_t s_lease_us;
static int32_t  s_loop_err;

static bool on_sent(i2s_chan_handle_t h, i2s_event_data_t *ev, void *ctx)
{
    (void)h; (void)ev; (void)ctx;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_sent_mux);
    s_sent_us = now;
    s_sent++;
    portEXIT_CRITICAL_ISR(&s_sent_mux);
    return false;
}

static void sent_pair(int64_t *us, uint32_t *n)
{
    portENTER_CRITICAL(&s_sent_mux);
    *us = s_sent_us;
    *n  = s_sent;
    portEXIT_CRITICAL(&s_sent_mux);
}

static void pa_set(bool on)
{
    gpio_set_level(PIN_AUDIO_PA, on ? 1 : 0);
    s_pa = on;
}

static void apply_volume(void)
{
    if (!orb_es8311_present()) return;
    if (s_cfg.volume == 0) {
        orb_es8311_mute(true);
        return;
    }
    /* The register runs in 0.5 dB steps with 191 at 0 dB: each owner step below 100
     * is half a decibel, so 60 sits at -20 dB and 1 at -49.5 dB. */
    orb_es8311_set_volume_reg((uint8_t)(191 - (100 - s_cfg.volume)));
    orb_es8311_mute(false);
}

/* ---- mixing ------------------------------------------------------------------ */

static int64_t samples_to_us(int64_t s) { return s * 1000000 / ORB_SOUND_RATE; }
static int64_t us_to_samples(int64_t u)
{
    /* floor, for negative spans too: a loop's phase must not jump by one sample as
     * the anchor difference crosses zero */
    const int64_t num = u * ORB_SOUND_RATE;
    return num >= 0 ? num / 1000000 : -((-num + 999999) / 1000000);
}

static uint32_t loop_phase(const voice_t *v, int64_t t_us, int64_t offset_us)
{
    int64_t s = us_to_samples(t_us - (v->anchor_us + offset_us)) % (int64_t)v->n;
    if (s < 0) s += v->n;
    return (uint32_t)s;
}

static bool quiet_ahead(const voice_t *v)
{
    uint32_t p = v->pos;
    for (uint32_t i = 0; i < BLOCK; i++) {
        const int s = v->pcm[p];
        if (s > QUIET_PEAK || s < -QUIET_PEAK) return false;
        if (++p >= v->n) p = 0;
    }
    return true;
}

/* Mix every voice into one block whose first sample sounds at `t_us`. Returns the
 * number of voices still alive after it. */
static int mix_block(int16_t *out, int64_t t_us)
{
    int32_t *acc = s_acc;
    memset(acc, 0, BLOCK * sizeof acc[0]);
    const int64_t off_us = (int64_t)s_cfg.offset_ms * 1000;
    const bool lapsed = esp_timer_get_time() - s_lease_us > LEASE_US;
    const bool quiet = orb_sound_quiet_now();
    int alive = 0;

    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int k = 0; k < VOICES; k++) {
        voice_t *v = &s_v[k];
        if (!v->state) continue;
        if ((v->flags & ORB_SOUND_LEASED) && lapsed) { v->state = 0; continue; }
        if (quiet && !(v->flags & ORB_SOUND_ASKED)) { v->state = 0; continue; }   /* the OS's quiet hours */
        uint32_t i0 = 0;
        const bool anchored = (v->flags & ORB_SOUND_ANCHORED) != 0;
        if (v->state == 1) {
            if (anchored) {
                v->pos = loop_phase(v, t_us, off_us);
            } else if (v->at_us) {
                const int64_t d = us_to_samples(v->at_us + off_us - t_us);
                if (d >= (int64_t)BLOCK) { alive++; continue; }   /* not yet */
                i0 = d > 0 ? (uint32_t)d : 0;                    /* late: start now */
            }
            v->state = 2;
            s_started++;
        } else if (anchored) {
            const uint32_t want = loop_phase(v, t_us, off_us);
            int32_t err = (int32_t)want - (int32_t)v->pos;
            if (err >  (int32_t)(v->n / 2)) err -= (int32_t)v->n;
            if (err < -(int32_t)(v->n / 2)) err += (int32_t)v->n;
            s_loop_err = err;
            if (err > (int32_t)SNAP_SAMP || err < -(int32_t)SNAP_SAMP) {
                v->pos = want;                  /* a stall or a re-anchor: re-enter */
                s_snaps++;
            } else if (err != 0 && quiet_ahead(v)) {
                /* one sample, inside silence: a skip or a repeat nobody can hear */
                v->pos = err > 0 ? (v->pos + 1) % v->n : (v->pos + v->n - 1) % v->n;
                s_nudges++;
            }
        }
        const int32_t g = v->gain;
        bool done = false;
        for (uint32_t i = i0; i < BLOCK; i++) {
            acc[i] += ((int32_t)v->pcm[v->pos] * g) >> 12;
            if (++v->pos >= v->n) {
                if (v->flags & ORB_SOUND_LOOP) v->pos = 0;
                else { done = true; break; }
            }
        }
        if (done) v->state = 0;
        else alive++;
    }
    xSemaphoreGive(s_mx);

    for (uint32_t i = 0; i < BLOCK; i++) {
        const int32_t s = acc[i];
        out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    return alive;
}

static int voices_alive(void)
{
    int n = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int k = 0; k < VOICES; k++) n += s_v[k].state != 0;
    xSemaphoreGive(s_mx);
    return n;
}

static void mixer_task(void *arg)
{
    (void)arg;
    int16_t *buf = s_buf;
    const size_t buf_bytes = BLOCK * sizeof buf[0];
    const int64_t block_us = samples_to_us(BLOCK);
    for (;;) {
        /* Idle: wait for a voice with the amp and the I2S clocks off. */
        if (!voices_alive()) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!voices_alive()) continue;

        i2s_channel_enable(s_tx);
        s_running = true;
        /* Fill the queue with silence first, so every block after it is written into a
         * full queue and its sounding time is the measured one, then lift the amp:
         * the NS4150B wakes onto zeros rather than onto the first transient. */
        memset(buf, 0, buf_bytes);
        size_t w = 0;
        for (uint32_t i = 0; i < DESC_N + 1; i++)
            i2s_channel_write(s_tx, buf, buf_bytes, &w, portMAX_DELAY);
        pa_set(true);

        int64_t t_next = 0;
        int64_t last_live = esp_timer_get_time();
        for (;;) {
            int64_t sent_us; uint32_t sent0;
            sent_pair(&sent_us, &sent0);
            if (!t_next) t_next = sent_us + (int64_t)DESC_N * block_us;
            const int alive = mix_block(buf, t_next);
            i2s_channel_write(s_tx, buf, buf_bytes, &w, portMAX_DELAY);
            s_blocks++;
            /* The write returned because a buffer finished: the queue is full again and
             * the block just written sounds DESC_N-1 buffers after that instant, so the
             * next one sounds one block later still. When the write did not wait (the
             * counter did not move) the running estimate stands. */
            uint32_t sent1;
            sent_pair(&sent_us, &sent1);
            t_next = (sent1 != sent0) ? sent_us + (int64_t)DESC_N * block_us
                                      : t_next + block_us;
            const int64_t now = esp_timer_get_time();
            if (alive) last_live = now;
            else if (now - last_live > IDLE_US) break;
        }
        pa_set(false);
        vTaskDelay(pdMS_TO_TICKS(20));          /* let the amp settle before the clocks stop */
        i2s_channel_disable(s_tx);
        s_running = false;
    }
}

/* ---- public ------------------------------------------------------------------ */

int orb_sound_play(const int16_t *pcm, uint32_t n, uint16_t gain_q12,
                   uint32_t flags, int64_t at_us, int64_t anchor_us)
{
    return orb_sound_play_owned(NULL, pcm, n, gain_q12, flags, at_us, anchor_us);
}

int orb_sound_play_owned(const void *owner, const int16_t *pcm, uint32_t n,
                         uint16_t gain_q12, uint32_t flags, int64_t at_us,
                         int64_t anchor_us)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!pcm || n < 2 || ((uintptr_t)pcm & 1u)) return ESP_ERR_INVALID_ARG;
    if ((flags & ORB_SOUND_ANCHORED) && !(flags & ORB_SOUND_LOOP)) return ESP_ERR_INVALID_ARG;
    if (!(flags & ORB_SOUND_ASKED) && orb_sound_quiet_now()) return ESP_ERR_INVALID_STATE;
    int id = ESP_ERR_NO_MEM;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int k = 0; k < VOICES; k++) {
        if (s_v[k].state) continue;
        voice_t *v = &s_v[k];
        v->owner = owner;
        v->pcm = pcm; v->n = n; v->pos = 0;
        v->at_us = at_us; v->anchor_us = anchor_us;
        v->gain = gain_q12; v->flags = (uint8_t)flags; v->state = 1;
        v->id = id = s_next_id++;
        if (s_next_id <= 0) s_next_id = 1;
        break;
    }
    xSemaphoreGive(s_mx);
    if (id > 0) xTaskNotifyGive(s_task);
    return id;
}

void orb_sound_stop(int handle)
{
    if (!s_ready || handle <= 0) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int k = 0; k < VOICES; k++)
        if (s_v[k].state && s_v[k].id == handle) s_v[k].state = 0;
    xSemaphoreGive(s_mx);
}

void orb_sound_stop_owner(const void *owner)
{
    if (!s_ready || !owner) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int k = 0; k < VOICES; k++)
        if (s_v[k].state && s_v[k].owner == owner) s_v[k].state = 0;
    xSemaphoreGive(s_mx);
}

/* Act on `handle` only when `owner` started it: an app's handle is a number, and a
 * number an app made up must not stop another app's sound. */
static bool owned(const void *owner, int handle)
{
    bool yes = false;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int k = 0; k < VOICES; k++)
        if (s_v[k].state && s_v[k].id == handle && s_v[k].owner == owner) yes = true;
    xSemaphoreGive(s_mx);
    return yes;
}

void orb_sound_app_stop(const void *owner, int handle)
{
    if (!s_ready || handle <= 0 || !owner) return;
    if (owned(owner, handle)) orb_sound_stop(handle);
}

bool orb_sound_app_active(const void *owner, int handle)
{
    if (!s_ready || handle <= 0 || !owner) return false;
    return owned(owner, handle);
}

void orb_sound_stop_all(void)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int k = 0; k < VOICES; k++) s_v[k].state = 0;
    xSemaphoreGive(s_mx);
}

bool orb_sound_active(int handle)
{
    if (!s_ready || handle <= 0) return false;
    bool on = false;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int k = 0; k < VOICES; k++)
        if (s_v[k].state && s_v[k].id == handle) on = true;
    xSemaphoreGive(s_mx);
    return on;
}

void orb_sound_reanchor(int handle, int64_t anchor_us)
{
    if (!s_ready || handle <= 0) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int k = 0; k < VOICES; k++)
        if (s_v[k].state && s_v[k].id == handle) s_v[k].anchor_us = anchor_us;
    xSemaphoreGive(s_mx);
}

void orb_sound_lease_renew(void) { s_lease_us = esp_timer_get_time(); }

bool orb_sound_ready(void) { return s_ready; }

/* ---- settings ------------------------------------------------------------------ */

#define NVS_NS "sound"

static void cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;   /* never saved: defaults */
    uint8_t u8; int8_t i8; int16_t i16;
    if (nvs_get_u8(h, "vol", &u8) == ESP_OK && u8 <= 100) s_cfg.volume = u8;
    if (nvs_get_u8(h, "tick", &u8) == ESP_OK)   s_cfg.tick = u8 != 0;
    if (nvs_get_u8(h, "chime", &u8) == ESP_OK)  s_cfg.chimes = u8 != 0;
    if (nvs_get_u8(h, "strike", &u8) == ESP_OK) s_cfg.strikes = u8 != 0;
    if (nvs_get_u8(h, "jump", &u8) == ESP_OK)   s_cfg.jumps = u8 != 0;
    if (nvs_get_i8(h, "qfrom", &i8) == ESP_OK)  s_cfg.quiet_from = i8;
    if (nvs_get_i8(h, "qto", &i8) == ESP_OK)    s_cfg.quiet_to = i8;
    if (nvs_get_i16(h, "offms", &i16) == ESP_OK) s_cfg.offset_ms = i16;
    if (nvs_get_i8(h, "pat", &i8) == ESP_OK)    s_cfg.pattern = i8;
    nvs_close(h);
}

void orb_sound_cfg_get(orb_sound_cfg_t *out) { *out = s_cfg; }

const char *orb_sound_cfg_set(const orb_sound_cfg_t *in)
{
    if (in->volume > 100) return "volume runs 0..100";
    if (in->quiet_from < -1 || in->quiet_from > 23 || in->quiet_to < -1 || in->quiet_to > 23)
        return "quiet hours are an hour 0..23, or -1 for none";
    if ((in->quiet_from < 0) != (in->quiet_to < 0))
        return "quiet hours need both ends, or neither";
    if (in->offset_ms < 0 || in->offset_ms > 500) return "offset runs 0..500 ms";
    if (in->pattern < -1 || in->pattern > 4) return "pattern is -1 (the theme's) or 0..4";
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return "settings store unavailable";
    esp_err_t e = ESP_OK;
    e |= nvs_set_u8(h, "vol", in->volume);
    e |= nvs_set_u8(h, "tick", in->tick);
    e |= nvs_set_u8(h, "chime", in->chimes);
    e |= nvs_set_u8(h, "strike", in->strikes);
    e |= nvs_set_u8(h, "jump", in->jumps);
    e |= nvs_set_i8(h, "qfrom", in->quiet_from);
    e |= nvs_set_i8(h, "qto", in->quiet_to);
    e |= nvs_set_i16(h, "offms", in->offset_ms);
    e |= nvs_set_i8(h, "pat", in->pattern);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return "settings could not be saved";
    s_cfg = *in;
    apply_volume();
    return NULL;
}

/* Read once a second at most: the mixer asks every block, and localtime_r is not
 * free. TZ is the OS's (orb_net applies it with setenv+tzset). */
bool orb_sound_quiet_now(void)
{
    static time_t s_at;
    static bool   s_q;
    const time_t t = time(NULL);
    if (t != s_at) {
        struct tm ti;
        localtime_r(&t, &ti);
        s_q = orb_sound_quiet_at(ti.tm_hour);
        s_at = t;
    }
    return s_q;
}

bool orb_sound_quiet_at(int hour)
{
    const int a = s_cfg.quiet_from, b = s_cfg.quiet_to;
    if (a < 0 || b < 0 || a == b) return false;
    return a < b ? (hour >= a && hour < b) : (hour >= a || hour < b);
}

void orb_sound_stats(orb_sound_stats_t *out)
{
    memset(out, 0, sizeof *out);
    out->blocks  = s_blocks;
    out->sent    = s_sent;
    out->voices  = s_ready ? (uint32_t)voices_alive() : 0;
    out->started = s_started;
    out->nudges  = s_nudges;
    out->snaps   = s_snaps;
    out->loop_err = s_loop_err;
    out->pa_on   = s_pa;
    out->running = s_running;
}

/* ---- bench sounds: made once, in PSRAM, with no theme ---------------------------- */

static int16_t *s_ding, *s_tick, *s_loop1;
static uint32_t s_ding_n, s_tick_n;
static int s_loop_id;

static void make_bench_sounds(void)
{
    if (s_ding) return;
    s_ding_n = ORB_SOUND_RATE * 8 / 10;                        /* 0.8 s */
    s_tick_n = ORB_SOUND_RATE * 3 / 100;                       /* 30 ms */
    s_ding  = heap_caps_malloc(s_ding_n * 2, MALLOC_CAP_SPIRAM);
    s_tick  = heap_caps_malloc(s_tick_n * 2, MALLOC_CAP_SPIRAM);
    s_loop1 = heap_caps_calloc(ORB_SOUND_RATE, 2, MALLOC_CAP_SPIRAM);   /* 1 s */
    if (!s_ding || !s_tick || !s_loop1) return;
    /* A small bell on E6 with the free-bar partials the prototype uses, and a click
     * of two damped resonances: both well above the speaker's ~300 Hz floor. */
    static const float part[][3] = { {1.0f, 1.0f, 0.45f}, {2.76f, 0.5f, 0.18f}, {5.40f, 0.25f, 0.07f} };
    for (uint32_t i = 0; i < s_ding_n; i++) {
        const float t = (float)i / ORB_SOUND_RATE;
        float s = 0;
        for (int p = 0; p < 3; p++)
            s += part[p][1] * expf(-t / part[p][2]) * sinf(2 * (float)M_PI * 1318.5f * part[p][0] * t);
        s *= fminf(1.0f, t / 0.002f);
        s_ding[i] = (int16_t)(s * 16000);
    }
    for (uint32_t i = 0; i < s_tick_n; i++) {
        const float t = (float)i / ORB_SOUND_RATE;
        const float s = expf(-t / 0.004f) * (sinf(2 * (float)M_PI * 3200 * t) + 0.6f * sinf(2 * (float)M_PI * 5100 * t));
        s_tick[i] = (int16_t)(s * 18000);
    }
    memcpy(s_loop1, s_tick, s_tick_n * 2);
}

const char *orb_sound_test(const char *which)
{
    if (!s_ready) return "no codec: sound is off on this board";
    make_bench_sounds();
    if (!s_ding || !s_tick || !s_loop1) return "no PSRAM for the bench sounds";
    if (!strcmp(which, "ding")) {
        return orb_sound_play(s_ding, s_ding_n, ORB_SOUND_GAIN_1, ORB_SOUND_ASKED, 0, 0) > 0 ? NULL : "no free voice";
    }
    if (!strcmp(which, "tick")) {
        return orb_sound_play(s_tick, s_tick_n, ORB_SOUND_GAIN_1, ORB_SOUND_ASKED, 0, 0) > 0 ? NULL : "no free voice";
    }
    if (!strcmp(which, "loop")) {
        if (s_loop_id && orb_sound_active(s_loop_id)) {
            orb_sound_stop(s_loop_id);
            s_loop_id = 0;
            return NULL;
        }
        /* Sample 0 on every wall second: the instant this second began is now less
         * the fraction orb_time_now reports, sampled back to back with esp_timer. */
        time_t sec; double frac; bool synced;
        orb_time_now(&sec, &frac, &synced);
        const int64_t anchor = esp_timer_get_time() - (int64_t)(frac * 1e6);
        s_loop_id = orb_sound_play(s_loop1, ORB_SOUND_RATE, ORB_SOUND_GAIN_1,
                                   ORB_SOUND_LOOP | ORB_SOUND_ANCHORED | ORB_SOUND_ASKED, 0, anchor);
        return s_loop_id > 0 ? NULL : "no free voice";
    }
    return "unknown bench sound (ding, tick, loop)";
}

/* ---- bring-up --------------------------------------------------------------------- */

esp_err_t orb_sound_init(void)
{
    const gpio_config_t pa = {
        .pin_bit_mask = 1ULL << PIN_AUDIO_PA,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pa);
    pa_set(false);
    cfg_load();

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num  = DESC_N;
    cc.dma_frame_num = BLOCK;
    cc.auto_clear_after_cb = true;      /* a stalled mixer plays zeros, not a repeat */
    esp_err_t err = i2s_new_channel(&cc, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s channel: %s — sound is off", esp_err_to_name(err));
        return ESP_OK;
    }
    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(ORB_SOUND_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_LRCK,
            .dout = PIN_I2S_DOUT, .din = I2S_GPIO_UNUSED,
        },
    };
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    sc.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;      /* the one channel on both slots */
    err = i2s_channel_init_std_mode(s_tx, &sc);
    if (err == ESP_OK) {
        const i2s_event_callbacks_t cb = { .on_sent = on_sent };
        err = i2s_channel_register_event_callback(s_tx, &cb, NULL);
    }
    /* The codec's clock manager is configured against a running MCLK, so the channel
     * runs once for the bring-up and then idles like any silent stretch. */
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s std mode: %s — sound is off", esp_err_to_name(err));
        return ESP_OK;
    }
    err = orb_es8311_init(ORB_SOUND_RATE);
    i2s_channel_disable(s_tx);
    if (err != ESP_OK) return ESP_OK;   /* orb_es8311 logged why */
    apply_volume();

    s_v   = heap_caps_calloc(VOICES, sizeof *s_v, MALLOC_CAP_SPIRAM);
    s_acc = heap_caps_malloc(BLOCK * sizeof *s_acc, MALLOC_CAP_SPIRAM);
    s_buf = heap_caps_malloc(BLOCK * sizeof *s_buf, MALLOC_CAP_SPIRAM);
    s_mx  = xSemaphoreCreateMutex();
    if (!s_v || !s_acc || !s_buf) {
        ESP_LOGE(TAG, "mixer buffers: out of PSRAM — sound is off");
        return ESP_OK;
    }
    /* The stack is PSRAM's, for the reason orb_net's serial reader gives: the internal
     * heap is the pool this board runs out of. Nothing on this task touches flash. */
#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    const bool up = s_mx && xTaskCreateWithCaps(mixer_task, "orb_sound", 3072, NULL, MIXER_PRIO,
                                                &s_task, MALLOC_CAP_SPIRAM) == pdPASS;
#else
    const bool up = s_mx && xTaskCreate(mixer_task, "orb_sound", 3072, NULL, MIXER_PRIO, &s_task) == pdPASS;
#endif
    if (!up) {
        ESP_LOGE(TAG, "mixer task: out of memory — sound is off");
        return ESP_OK;
    }
    s_ready = true;
    ESP_LOGI(TAG, "engine up: %u Hz, %u x %u-frame DMA, %d voices, volume %u",
             (unsigned)ORB_SOUND_RATE, (unsigned)DESC_N, (unsigned)BLOCK, VOICES,
             (unsigned)s_cfg.volume);
    return ESP_OK;
}
