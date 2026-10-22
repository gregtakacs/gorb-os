/**
 * knob.c — Rotary encoder (KY-040) input driver.
 *
 * ESP-IDF port of orb-os/src/knob.cpp, translated from Arduino (attachInterrupt /
 * digitalRead / millis) to native ESP-IDF (gpio ISR service / gpio_get_level /
 * esp_timer_get_time). Two of the reference's ideas are kept unchanged, and one is
 * deliberately not.
 *
 *  1. KEPT — Press is edge-debounced in the ISR, on BOTH edges. The release edge must
 *     stamp the debounce clock too, or the switch's release bounce looks like a second
 *     press and one click fires twice.
 *
 *  2. KEPT — One committed detent stream, and the stream that decides a gesture is the
 *     same one that moves the UI, so they cannot disagree about what happened.
 *
 *  3. CHANGED — The reference commits a detent after a FULL detent of net raw travel.
 *     That is the right amount of hysteresis and the wrong place to measure it: it
 *     counts edges, and a mechanical click is not a number of edges. Edges get eaten
 *     by the click stop (so a notch that stops on the click under-counts and does
 *     nothing) and thrown back at it by make-bounce (so a bouncy notch over-counts and
 *     moves twice). Both were reported by the owner of this dial on 2026-10-10, in the
 *     same sentence: "mostly one, sometimes two, sometimes none".
 *
 *     So the commit now happens on ARRIVAL AT THE VALLEY — the quadrature state the
 *     detent spring parks the shaft in — crediting at most one detent per arrival, in
 *     the direction of the net travel since the last one. A rock out-and-back nets
 *     zero and commits nothing, which is the same noise immunity the reference wanted,
 *     expressed against the thing that actually makes a click a click. A valley that
 *     never arrives (edges lost between pin and ISR) is settled by knob_poll() once
 *     the stream has been idle, which is a recovery path and cannot double-count.
 *
 *     The valley is learned from the pins at init, not hardcoded: where the code wheel
 *     sits relative to the spring is a property of the unit. knob.spd10, knob.valley,
 *     knob.ab and the valley/settle hit counts on /api/v1/status are what made the
 *     difference between these two designs visible without a logic analyser.
 *
 * This port also decides press and long-press together at the RELEASE edge from the
 * measured hold time, so they are mutually exclusive (a deliberate hold does not also
 * count as a tap).
 */

#include "knob.h"
#include "board_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"   /* esp_rom_delay_us: sampling the pins at init */

#define TAG "knob"

/* Quadrature edges one physical click ("detent") emits on a healthy KY-040: four,
 * one per A/B transition, and the click settles back into the state both pins idle
 * at. Read knob.spd10 on /api/v1/status before changing this — the two failure
 * directions (too many, too few) feel identical from the hand. */
#define KNOB_STEPS_PER_DETENT  4

/* Net travel that makes an arrival at the idle state a real click rather than a
 * rock. Half a detent, rounded up: a notch whose fourth edge never arrives (the
 * hand stopped on the click stop, or one edge was eaten by noise) still nets three
 * and counts, and a half-and-half rock nets zero and does not.
 *
 * This replaced a raw-step threshold, and the reason is the owner's own report
 * after that first fix: "mostly one, sometimes two, sometimes none". Counting edges
 * cannot tell a click from a bouncy click — a make-burst that lands seven net steps
 * in one notch reads exactly like two notches of three-and-a-half. The mechanical
 * click can only be recognised by the thing that makes it a click: the shaft coming
 * to rest in the next valley, which is a quadrature state, not a step count. So the
 * commit now happens on arrival at that state, and it fires AT most once per
 * arrival, whatever the edges in between did. */
#define KNOB_DETENT_MIN_STEPS  ((KNOB_STEPS_PER_DETENT + 1) / 2)

/* No new edge for this long means the hand has stopped, and knob_poll() settles the
 * account: whatever net travel never reached a valley gets rounded into detents. It
 * covers the edges the decoder could not see (both pins moving inside one ISR entry
 * reads as an invalid transition and is dropped), and it can never double-count one,
 * because the net is zeroed by every valley arrival. A detent period of even a fast
 * turn is well over 30 ms, so this does not fire mid-flick. */
#define KNOB_SETTLE_MS          30

/* Minimum spacing between two committed detents. One valley arrival is one detent, and
 * the net-zeroing above already handles a bounce that returns to the same valley; this
 * covers the bounce that does not — a shaft kicking forward four steps, back four and
 * forward four inside a single notch would otherwise be read as two clicks, because
 * each arrival really does carry net travel.
 *
 * 20 ms is the ceiling on what that leaves exploitable (50 notches a second) against
 * what a hand can actually do: a 24-detent KY-040 flicked as fast as a wrist manages is
 * nearer 10 a second, and the mechanical detent keeps the contacts inside a notch
 * together for a couple of milliseconds at most. Anything under a real inter-click gap
 * and over a bounce burst does the job; this is the middle of that range, not a tuned
 * value — the knob's own timing is what would tune it, and there is no measurement of
 * that here yet. */
#define KNOB_CLICK_GUARD_MS     20

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
 * byte-wide and single-producer/single-consumer, so plain volatile is enough.
 *
 * s_rawPos, s_net and s_detent are written by knob_rot_isr and by the settle pass
 * in knob_poll(), which run on different contexts, so the pair is guarded by
 * s_rot_mux — held for the length of a few adds. Without it a settle landing between
 * the ISR's two updates commits a detent twice or loses one, which is the class of
 * bug this file has already been rewritten once for. ---- */
static uint8_t          s_prevAB  = 0xFF; /* last decoded AB, shared by A and B  */
static volatile int32_t s_rawPos  = 0;   /* raw quadrature count               */
static int32_t          s_net     = 0;   /* travel since the last valley arrival */
static volatile int32_t s_detent  = 0;   /* committed detents — the true stream*/
static volatile bool    s_pressPend = false;  /* set at release if held < LONG  */
static volatile bool    s_longPend  = false;  /* set at release if held >= LONG */

/* The quadrature state the shaft rests in when it is parked on a click — the valley
 * the code wheel's detent spring holds it in. Learned, never assumed: it is read off
 * the pins at init, and re-learned at the first idle if the boot reading was mid
 * click. Both the commit point and the "is it a click" test hang off this one byte,
 * and the wrong value makes the dial feel laggy rather than wrong, so it is published
 * on /api/v1/status as knob.valley next to knob.ab. */
static volatile uint8_t s_valleyAB = 0xFF;

/* When the last quadrature edge was seen, so knob_poll() can tell "paused mid
 * turn" from "the hand has stopped", and only settle the account for the latter. */
static volatile int64_t s_lastEdgeUs = 0;

/* When the last detent was committed, for the click guard. Written from both the
 * ISR and the settle pass, so it lives inside s_rot_mux like the counters. */
static int64_t s_lastCommitUs = 0;

/* Diagnostics, all of it for one question: is this unit really a 4-edge KY-040, and
 * is the commit firing where it should. s_spanSum/s_spanCount is the measured mean
 * raw travel per committed detent; s_valleyHits and s_settleHits say how the detents
 * were arrived at, which is what separates "the decoder is missing edges" from
 * "the rule is stingy". */
static volatile int32_t s_spanMax   = 0;
static volatile int32_t s_spanSum   = 0;
static volatile int32_t s_spanCount = 0;
static volatile int32_t s_valleyHits = 0;
static volatile int32_t s_settleHits = 0;
static volatile int32_t s_abNow      = 0xFF;   /* levels as last read by the ISR */
static int32_t          s_rawAtCommit = 0;     /* raw at the last commit, span datum */
static portMUX_TYPE     s_rot_mux   = portMUX_INITIALIZER_UNLOCKED;

/* Switch edge bookkeeping, live inside the SW ISR (microseconds from esp_timer
 * so both edges share one IRAM-safe clock). */
static volatile uint8_t s_swDown        = 0;  /* 1 while the switch reads active-low */
static volatile int64_t s_swLastEdgeUs  = 0;  /* us of the last accepted edge        */
static volatile int64_t s_swPressStartUs = 0; /* us the current hold began           */

/* Consumer read-cursor (only ever touched by the single take_delta caller). */
static int32_t s_lastTakenDetent = 0;

/* ---- Was this arrival at a valley a click? Pure, and lifted VERBATIM by
 * tools/host_knob_test.py, which drives it with synthetic AB sequences — the only
 * way to hold a bouncy notch up in front of the rule without a hand on the dial.
 *
 * The net travel since the last valley decides it. A rock out and back nets zero and
 * commits nothing, which is the hysteresis the original driver needed; a notch that
 * lost its fourth edge to the click stop still nets three and commits. One arrival
 * can credit ONE detent, never two: that is the whole of the "sometimes it moves
 * two rows" report, and an edge counter cannot express it. */
static int32_t knob_detent_from_net(int32_t net)
{
    if (net >=  KNOB_DETENT_MIN_STEPS) return  1;
    if (net <= -KNOB_DETENT_MIN_STEPS) return -1;
    return 0;
}

/* What the edges between two valleys rounded to, when the hand stopped without
 * reaching one. Below half a detent it was a rock and the answer is zero, so this is
 * also where the settle refuses to invent a row. */
static int32_t knob_detents_rounded(int32_t net)
{
    const int32_t mag = (net > 0) ? net : -net;
    if (mag < KNOB_DETENT_MIN_STEPS) return 0;
    const int32_t n = (mag + KNOB_STEPS_PER_DETENT / 2) / KNOB_STEPS_PER_DETENT;
    if (n < 1) return 0;
    return (net > 0) ? n : -n;
}

/* ---- One decoded edge. Also pure, so the host suite replays edges through the
 * real decode path and not a paraphrase of it: it takes the new AB level, the state
 * the last edge left behind, the valley, and the two clocks the guard needs — time
 * as an argument, never read from a clock inside the function, which is what makes
 * the timing rule testable instead of merely true on a bench.
 *
 * Returns 0 for the three things that are not a click — the no-op edge (the other
 * pin's interrupt arriving after both levels had already changed), an invalid
 * transition (both pins moved inside one ISR entry, so the direction is genuinely
 * unknowable), and a valley arrival inside the click guard. None of them is guessed
 * at: inventing a step there is how a knob ends up moving twice for one notch. */
static int32_t knob_edge(int32_t *raw, int32_t *net, uint8_t *prev_ab, uint8_t ab,
                         uint8_t valley_ab, int64_t now_us, int64_t last_commit_us)
{
    const uint8_t prev = *prev_ab;
    *prev_ab = ab;

    if (prev == 0xFF || ab == prev) return 0;             /* unseeded, or no-op edge */

    const int8_t step = kQuadTable[((prev << 2) | ab) & 0x0F];
    if (step == 0) return 0;                              /* invalid transition */

    *raw += step;
    *net += step;

    /* Arriving at the valley is the click. Whatever the edges in between cost, the
     * shaft came to rest in the next notch, so at most one detent is owed — and it is
     * owed now, at the click, not after the hand has been still for a poll period.
     * The net is closed either way: an arrival that was guarded, or that netted a
     * rock, starts the next notch from zero rather than carrying this one's residue. */
    if (ab == valley_ab) {
        /* 64-bit on purpose. Truncating this delta to int32 is what killed the dial:
         * esp_timer_get_time() counts microseconds since boot, so on a board left
         * idle past 2^31 us (~36 min) the gap wrapped negative, this guard read it as
         * "too soon since the last detent" forever, and every valley arrival was
         * suppressed while raw kept moving. A reboot cleared it by resetting the
         * clock. Pinned by the long-idle case in tools/host_knob_test.py. */
        const int64_t since = now_us - last_commit_us;
        const int32_t n = (since >= (int64_t)KNOB_CLICK_GUARD_MS * 1000)
                          ? knob_detent_from_net(*net) : 0;
        *net = 0;
        return n;
    }
    return 0;
}

/* ---- Rotation ISR: fires on any edge of A or B; both pins share it. ---- */
static void IRAM_ATTR knob_rot_isr(void *arg)
{
    (void)arg;
    const uint8_t a  = (uint8_t)gpio_get_level(PIN_KNOB_A);
    const uint8_t b  = (uint8_t)gpio_get_level(PIN_KNOB_B);
    const uint8_t ab = (uint8_t)((a << 1) | b);

    /* Any real edge restarts the idle clock, including the ones that decode to
     * nothing. Not stamping it on a no-op edge let the settle fire in the middle of
     * a bouncy click and book the leftovers as a detent of its own. */
    s_lastEdgeUs = esp_timer_get_time();
    s_abNow = ab;

    portENTER_CRITICAL(&s_rot_mux);
    /* Copied out rather than passed by pointer: the shared counters are volatile
     * (the consumer reads them lock-free) and knob_edge() is deliberately pure over
     * plain integers so the host suite can drive it. Still one atomic region, so the
     * ISR and the settle pass cannot interleave inside a single edge. */
    int32_t raw  = s_rawPos;
    int32_t net  = s_net;
    uint8_t prev = s_prevAB;
    const int64_t now = esp_timer_get_time();
    const int32_t n = knob_edge(&raw, &net, &prev, ab, (uint8_t)s_valleyAB,
                                now, s_lastCommitUs);
    s_rawPos = raw;
    s_net    = net;
    s_prevAB = prev;
    if (n) {
        s_detent += n;
        s_lastCommitUs = now;
        /* Measured, not assumed: the raw travel this commit actually consumed, taken
         * from the raw count rather than from KNOB_STEPS_PER_DETENT, so the number can
         * disagree with the constant it is checking. knob.spd10 on /api/v1/status. */
        const int32_t spent = (raw - s_rawAtCommit);
        s_rawAtCommit = raw;
        const int32_t mag = (spent > 0) ? spent : -spent;
        if (mag > s_spanMax) s_spanMax = mag;
        s_spanSum += mag;
        s_spanCount += (n > 0 ? n : -n);
        s_valleyHits++;
    }
    portEXIT_CRITICAL(&s_rot_mux);
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

    /* Learn the valley — the AB state the detent spring parks the shaft in — by
     * sampling rather than assuming. Nobody is holding the knob at app_main, so this
     * is the most trustworthy reading of the session, and it is the one number that
     * cannot be deduced from the datasheet: it depends on where the code wheel sits
     * relative to the spring on this unit. Requiring two identical readings rejects a
     * shaft left between notches; if the pins disagree, leave 0xFF and let knob_poll()
     * learn it the first time the knob has genuinely been still for 30 ms. */
    uint8_t valley = 0xFF;
    for (int tries = 0; tries < 8 && valley == 0xFF; tries++) {
        const uint8_t first = (uint8_t)(((gpio_get_level(PIN_KNOB_A) << 1) |
                                         gpio_get_level(PIN_KNOB_B)) & 0x03);
        esp_rom_delay_us(200);
        const uint8_t again = (uint8_t)(((gpio_get_level(PIN_KNOB_A) << 1) |
                                         gpio_get_level(PIN_KNOB_B)) & 0x03);
        if (first == again) valley = first;
    }
    s_valleyAB = valley;
    s_abNow    = valley;
    s_prevAB   = (valley == 0xFF) ? 0xFF : valley;
    s_rawPos = 0;
    s_net = 0;
    s_rawAtCommit = 0;
    s_detent = 0;
    s_spanMax = 0;
    s_spanSum = 0;
    s_spanCount = 0;
    s_valleyHits = 0;
    s_settleHits = 0;
    s_lastEdgeUs = esp_timer_get_time();
    s_lastCommitUs = esp_timer_get_time() - ((int64_t)KNOB_CLICK_GUARD_MS * 2000);
    ESP_LOGI(TAG, "valley %u (%c%c), seeded decoder, %d edges/detent",
             (unsigned)valley,
             (valley & 2) ? '1' : '0', (valley & 1) ? '1' : '0',
             KNOB_STEPS_PER_DETENT);

    err = gpio_install_isr_service(0);   /* ESP_ERR_INVALID_STATE if already up */
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    gpio_isr_handler_add(PIN_KNOB_A, knob_rot_isr, NULL);
    gpio_isr_handler_add(PIN_KNOB_B, knob_rot_isr, NULL);
    gpio_isr_handler_add(PIN_KNOB_SW, knob_sw_isr, NULL);

    ESP_LOGI(TAG, "ready on GPIO%d(A)/%d(B)/%d(SW) — turn + push",
             PIN_KNOB_A, PIN_KNOB_B, PIN_KNOB_SW);
    return ESP_OK;
}

void knob_poll(void)
{
    const int64_t now = esp_timer_get_time();
    int32_t settled = 0, spent = 0, net_seen = 0;
    bool learned = false;
    uint8_t valley = 0xFF;

    portENTER_CRITICAL(&s_rot_mux);
    const bool idle = (now - s_lastEdgeUs) >= (int64_t)KNOB_SETTLE_MS * 1000;
    if (idle) {
        /* The hand has stopped. If it stopped without reaching a valley, the travel
         * it made never got its click — usually because the decoder saw an invalid
         * transition (both pins moved inside one ISR entry) and refused to guess a
         * direction from it. Round what is left into whole detents rather than
         * dropping it: that is the "sometimes the notch does nothing at all" report,
         * and dropping here is what caused it. */
        const int32_t n = knob_detents_rounded(s_net);
        if (n) {
            s_detent += n;
            spent = (s_rawPos - s_rawAtCommit);
            s_rawAtCommit = s_rawPos;
            const int32_t mag = (spent > 0) ? spent : -spent;
            if (mag > s_spanMax) s_spanMax = mag;
            s_spanSum += mag;
            s_spanCount += (n > 0 ? n : -n);
            s_settleHits++;
            s_lastCommitUs = now;
            settled = n;
            net_seen = s_net;
        }
        s_net = 0;

        /* Learn the valley if init never got a reading (a bare header, or pins that
         * were still floating when knob_init ran). A shaft parked on a click is the
         * best reading available, and this is the only place idle is proven by a gap
         * rather than assumed. It deliberately does NOT overwrite an already-learned
         * valley: a hand resting the knob between notches would otherwise install a
         * wrong valley for the rest of the session. */
        if (s_valleyAB == 0xFF || s_abNow == 0xFF) {
            s_valleyAB = (uint8_t)(((gpio_get_level(PIN_KNOB_A) << 1) |
                                    gpio_get_level(PIN_KNOB_B)) & 0x03);
            s_abNow = s_valleyAB;
            s_prevAB = s_valleyAB;
            s_net = 0;
            learned = true;
            valley = (uint8_t)s_valleyAB;
        }
    }
    const uint8_t ab_seen = (uint8_t)s_abNow;
    portEXIT_CRITICAL(&s_rot_mux);

    /* Logged outside the lock: ESP_LOGI can allocate, and allocating inside a
     * critical section on the LVGL task is how a driver turns a 10 ms poll into an
     * unbounded one with interrupts masked. */
    if (settled) {
        ESP_LOGI(TAG, "settled in %+ld detent(s) without a valley arrival (%ld raw, "
                      "net %ld, valley %u/ab %u)",
                 (long)settled, (long)((spent > 0) ? spent : -spent),
                 (long)net_seen, (unsigned)s_valleyAB, (unsigned)ab_seen);
    }
    if (learned) {
        ESP_LOGI(TAG, "valley state learned at %u", (unsigned)valley);
    }
}

/* ---- The UI's direction convention. See the block comment in knob.h: this is
 * the ONLY place a turn's sign is turned into a screen direction, so a list and a
 * row of cards can never disagree about what one flick means. ---- */
int knob_row_step(int32_t delta)
{
    if (delta == 0) return 0;
    return KNOB_DIR_DOWN_IS_CW ? (delta > 0 ? 1 : -1)
                               : (delta > 0 ? -1 : 1);
}

int knob_col_step(int32_t delta)
{
    /* Left/right is the same physical flick read the other way round: documented
     * as the row axis rotated, so flipping KNOB_DIR_DOWN_IS_CW moves both axes
     * together and they cannot come apart. */
    const int row = knob_row_step(delta);
    return row == 0 ? 0 : -row;
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

int32_t knob_raw_position(void) { return s_rawPos; }

/* The committed stream, so raw and committed can be read side by side. The pair is
 * the diagnosis: raw moving while det stays is a decoder not seeing edges, det at
 * half the raw rate is a commit rule asking for too much travel. */
int32_t knob_detent_count(void) { return s_detent; }

int32_t knob_raw_steps_per_detent(void)
{
    /* Mean raw travel per committed detent over the session, in TENTHS. 40 says the
     * click really does emit four edges on this unit; 20 says it emits two and
     * KNOB_STEPS_PER_DETENT is wrong by half — a difference that feels identical from
     * the hand and is invisible in the code. 0 until the first detent commits. */
    if (s_spanCount <= 0) return 0;
    return (s_spanSum * 10) / s_spanCount;
}

int32_t knob_raw_span_max(void) { return s_spanMax; }

/** How detents arrived, and what the pins are saying right now. The split is the
 * whole point: valley hits are ordinary clicks, settle hits are clicks the decoder
 * could not see the end of, and a session that is mostly settle hits means the edges
 * are being lost between the pin and the ISR, not that the rule is stingy. */
int32_t knob_valley_hits(void)  { return s_valleyHits; }
int32_t knob_settle_hits(void)  { return s_settleHits; }
int32_t knob_ab(void)           { return s_abNow; }
int32_t knob_valley(void)       { return s_valleyAB; }

uint32_t knob_long_press_ms(void)
{
    return LONG_PRESS_MS;
}
