/**
 * knob.c — Rotary encoder (KY-040) input driver.
 *
 * Faithful ESP-IDF port of orb-os/src/knob.cpp's proven model, translated from
 * Arduino (attachInterrupt / digitalRead / millis) to native ESP-IDF
 * (gpio ISR service / gpio_get_level / esp_timer_get_time).
 *
 * Two design points carried over verbatim because they were hard-won on real
 * hardware (see the reference file's comments):
 *
 *  1. ONE committed detent stream, committed with hysteresis. A detent is only
 *     counted after a FULL KNOB_STEPS_PER_DETENT of net travel from the last
 *     committed anchor. A knob resting ON a boundary dithers by a step or two;
 *     without this it commits spurious detents and looks like a direction flip.
 *
 *  2. Press is edge-debounced in the ISR, on BOTH edges. The release edge must
 *     stamp the debounce clock too, or the switch's release bounce looks like a
 *     second press and one click fires twice.
 *
 * This port adds one improvement over the reference for the color-picker use
 * case: press and long-press are decided together at the RELEASE edge from the
 * measured hold time, so they are mutually exclusive (a deliberate hold does
 * not also count as a tap).
 */

#include "knob.h"
#include "board_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

/* Quadrature edges per physical click ("detent"). KY-040 emits 4. If turns feel
 * doubled/halved on a different encoder, tune to 2 or 1. */
#define KNOB_STEPS_PER_DETENT  4

/* Min time between accepted press edges. Wide on purpose: this switch bounces
 * heavily, and 200 ms is still far faster than anyone deliberately taps it, so
 * one physical click can't become two. (Mirrors orb-os SW_DEBOUNCE_MS.) */
#define SW_DEBOUNCE_MS 200

/* Hold duration that promotes a hold to a long press. The reference uses 8000 ms
 * (a recovery-reboot guard nobody hits by accident); for an interactive control
 * that threshold is unusably long, so this MVP validation uses a feelable hold. */
#define LONG_PRESS_MS  700

/* ---- Standard quadrature decode table (Ben Buxton style). Index is
 * (prev AB << 2) | (cur AB); value is the signed step -1/0/+1. ---- */
static const int8_t kQuadTable[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0,
};

/* ---- ISR-written state. 32-bit reads are atomic on the S3 core, so the
 * consumer can read s_detent without a critical section; the boolean flags are
 * byte-wide and single-producer/single-consumer, so plain volatile is enough. ---- */
static volatile int32_t s_rawPos  = 0;   /* raw quadrature count               */
static volatile int32_t s_anchor  = 0;   /* rawPos at the last committed detent*/
static volatile int32_t s_detent  = 0;   /* committed detents — the true stream*/
static volatile bool    s_pressPend = false;  /* set at release if held < LONG  */
static volatile bool    s_longPend  = false;  /* set at release if held >= LONG */

/* Switch edge bookkeeping, live inside the SW ISR (microseconds from esp_timer
 * so both edges share one IRAM-safe clock). */
static volatile uint8_t s_swDown        = 0;  /* 1 while the switch reads active-low */
static volatile int64_t s_swLastEdgeUs  = 0;  /* us of the last accepted edge        */
static volatile int64_t s_swPressStartUs = 0; /* us the current hold began           */

/* Consumer read-cursor (only ever touched by the single take_delta caller). */
static int32_t s_lastTakenDetent = 0;

/* ---- Rotation ISR: fires on any edge of A or B; both pins share it. ---- */
static void IRAM_ATTR knob_rot_isr(void *arg)
{
    (void)arg;
    const uint8_t a  = (uint8_t)gpio_get_level(PIN_KNOB_A);
    const uint8_t b  = (uint8_t)gpio_get_level(PIN_KNOB_B);
    const uint8_t ab = (uint8_t)((a << 1) | b);

    /* State persists across edges on BOTH pins in one shared static. Seeded to
     * 0xFF so the first edge after init only records state, never decodes. */
    static uint8_t s_prevAB = 0xFF;
    const uint8_t prev = s_prevAB;
    if (prev == 0xFF || ab == prev) {   /* unseeded, or the other pin's no-op edge */
        s_prevAB = ab;
        return;
    }

    const uint8_t idx = (uint8_t)(((prev << 2) | ab) & 0x0F);
    s_rawPos += kQuadTable[idx];
    s_prevAB = ab;

    /* Commit whole detents only (hysteresis). The two whiles absorb a fast burst
     * that skipped several detent widths within a single ISR run. */
    while (s_rawPos - s_anchor >= KNOB_STEPS_PER_DETENT) {
        s_anchor += KNOB_STEPS_PER_DETENT;
        s_detent += 1;                 /* right / CW */
    }
    while (s_anchor - s_rawPos >= KNOB_STEPS_PER_DETENT) {
        s_anchor -= KNOB_STEPS_PER_DETENT;
        s_detent -= 1;                 /* left / CCW */
    }
}

/* ---- Switch ISR: fires on any edge of SW. Debounces both edges, then decides
 * press vs. long-press at the release edge from the measured hold time. ---- */
static void IRAM_ATTR knob_sw_isr(void *arg)
{
    (void)arg;
    const int64_t now = esp_timer_get_time();
    const uint8_t lvl = (uint8_t)gpio_get_level(PIN_KNOB_SW);   /* active low */

    if (lvl == 1) {
        /* RELEASE (rising). Accept instantly so s_swDown can never get stuck low,
         * but stamp the clock so release-side bounce is rejected too. */
        if (s_swDown) {
            s_swDown = 0;
            if ((now - s_swPressStartUs) >= (int64_t)LONG_PRESS_MS * 1000) {
                s_longPend = true;
            } else {
                s_pressPend = true;
            }
        }
        s_swLastEdgeUs = now;
        return;
    }

    /* PRESS (falling): accept only SW_DEBOUNCE_MS after any prior edge, which
     * rejects both make-bounce and the tail of a just-released switch. */
    if ((now - s_swLastEdgeUs) < (int64_t)SW_DEBOUNCE_MS * 1000) {
        return;
    }
    s_swLastEdgeUs   = now;
    s_swDown         = 1;
    s_swPressStartUs = now;
}

/* ---- Public API ---- */
esp_err_t knob_init(void)
{
    /* A/B/SW as inputs with the internal pull-up (KY-040 idles HIGH; the module's
     * own 10k pull-ups make this belt-and-braces). Any-edge interrupt on all three. */
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_KNOB_A) | (1ULL << PIN_KNOB_B) | (1ULL << PIN_KNOB_SW),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) return err;

    /* Seed the switch debounce clock in the past so the very first press is not
     * swallowed by the min-interval rule against an unset zero. */
    s_swLastEdgeUs = esp_timer_get_time() - ((int64_t)SW_DEBOUNCE_MS * 2000);

    err = gpio_install_isr_service(0);   /* ESP_ERR_INVALID_STATE if already up */
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    gpio_isr_handler_add(PIN_KNOB_A, knob_rot_isr, NULL);
    gpio_isr_handler_add(PIN_KNOB_B, knob_rot_isr, NULL);
    gpio_isr_handler_add(PIN_KNOB_SW, knob_sw_isr, NULL);

    ESP_LOGI("knob", "ready on GPIO%d(A)/%d(B)/%d(SW) — turn + push",
             PIN_KNOB_A, PIN_KNOB_B, PIN_KNOB_SW);
    return ESP_OK;
}

void knob_poll(void)
{
    /* Rotation and the press edge are handled entirely in the ISRs. Nothing to
     * sample here for now; kept as the hook orb-os's poll() occupies, so a future
     * held-timeout (e.g. auto-repeat while turning) has a home on the LVGL task. */
}

int32_t knob_take_delta(void)
{
    const int32_t now = s_detent;                 /* atomic 32-bit read of ISR counter */
    const int32_t d   = now - s_lastTakenDetent;
    s_lastTakenDetent = now;
    return d;
}

bool knob_take_press(void)
{
    const bool p = s_pressPend;
    s_pressPend  = false;
    return p;
}

bool knob_take_long_press(void)
{
    const bool l = s_longPend;
    s_longPend   = false;
    return l;
}

uint32_t knob_held_ms(void)
{
    if (!s_swDown) return 0;
    return (uint32_t)((esp_timer_get_time() - s_swPressStartUs) / 1000);
}

uint32_t knob_long_press_ms(void)
{
    return LONG_PRESS_MS;
}
