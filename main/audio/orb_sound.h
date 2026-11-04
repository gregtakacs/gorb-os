#pragma once
/**
 * orb_sound — the OS's clock-agnostic sound engine (docs/history/sound-plan.md §4, D39).
 *
 * It plays 16-bit mono PCM at 22,050 Hz that the caller owns: the clock hands it
 * pointers into its mapped theme, and the engine never copies, decodes or keeps a
 * sound. What it owns is the hardware (I2S, the ES8311, the NS4150B enable on
 * GPIO 46), a compiled mixer, the owner's level and quiet hours, and time:
 *
 *   - a one-shot can be queued for an exact esp_timer instant, so a strike pattern
 *     (ship's bells, Morse, a repeater) is laid down once and lands on time
 *     whatever the dial's tick is doing;
 *   - a loop can be anchored to a wall instant: its sample 0 sounds at the anchor
 *     plus every multiple of its length, and the engine keeps it there by moving
 *     at most one sample per mixer block, only inside a quiet block (§4.2).
 *
 * The playback clock is the I2S DMA's own "buffer sent" interrupt, so where a
 * block will sound is measured, not inferred from how long a write blocked.
 *
 * Flash writes suspend the cache, and the mixer's stack is in PSRAM, so the mixer
 * simply stops for the length of an erase; the DMA was configured to clear what it
 * has sent, so the speaker goes quiet instead of repeating a buffer (M-S1's policy:
 * silence, not a bigger ring in a full internal heap). A loop re-anchors on the
 * block after.
 *
 * Every pointer handed to orb_sound_play() must stay mapped until the voice ends
 * or is stopped. The clock stops its voices before it lets go of a theme.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ORB_SOUND_RATE     22050u
#define ORB_SOUND_GAIN_1   4096u      /* unity, in the Q12 voice gain */

#define ORB_SOUND_LOOP     0x01u      /* repeat until stopped */
#define ORB_SOUND_ANCHORED 0x02u      /* loop phase follows `anchor_us` (needs LOOP) */
#define ORB_SOUND_LEASED   0x04u      /* lives only while orb_sound_lease_renew() is called */
#define ORB_SOUND_ASKED    0x08u      /* the owner asked for it this moment (a bench test,
                                       * a repeater press): quiet hours do not apply */

/** Bring up I2S, the codec and the mixer task, and load the owner's settings.
 *  Never fatal: a board without the codec logs why and every call below becomes a
 *  no-op that reports ESP_ERR_INVALID_STATE. Call after imu_init() (the I2C bus). */
esp_err_t orb_sound_init(void);

/** True when the codec answered and the mixer is running. */
bool orb_sound_ready(void);

/** Start a voice. Returns a handle > 0, or a negative esp_err_t.
 *
 *  `at_us` is the esp_timer instant its first sample should sound (0 = as soon as
 *  possible). For an ANCHORED loop, `anchor_us` is an esp_timer instant at which
 *  sample 0 sounds; the loop enters at whatever phase that implies. The glass
 *  offset (orb_sound_cfg_t.offset_ms) is added to both, so a caller names the
 *  instant the dial SHOWS the event and the sound lands with the pixels. */
int orb_sound_play(const int16_t *pcm, uint32_t n, uint16_t gain_q12,
                   uint32_t flags, int64_t at_us, int64_t anchor_us);

/** The same, owned: `owner` (an app row, or any stable pointer) tags the voice, so
 *  orb_sound_stop_owner() can silence everything one app started — the registry
 *  does that when it suspends the app (C7, read for audio). Quiet hours are the
 *  OS's (orb_sound_cfg_t), applied here to every voice of every app: during them
 *  a voice without ORB_SOUND_ASKED is refused, and one already sounding is dropped
 *  at the boundary. */
int orb_sound_play_owned(const void *owner, const int16_t *pcm, uint32_t n,
                         uint16_t gain_q12, uint32_t flags, int64_t at_us,
                         int64_t anchor_us);
void orb_sound_stop_owner(const void *owner);

/* ---- the app surface (orb_svc_t's sound entries, cap SOUND) -------------------
 * An app plays ITS OWN theme's sounds by slot: the (ORB_ASSET_SOUND, slot) row of
 * its section in the adopted theme, resolved here, never a pointer it hands over.
 * flags: ORB_SOUND_LOOP repeats; ORB_SOUND_ANCHORED (with LOOP) puts sample 0 on a
 * wall second. delay_ms schedules it. Returns a handle > 0 or a negative esp_err_t:
 * ESP_ERR_NOT_FOUND for a slot the theme does not fill. stop/active act only on
 * the caller's own voices. */
int  orb_sound_app_play(const void *owner, const char *slug, uint16_t slot,
                        uint32_t flags, uint32_t delay_ms);
void orb_sound_app_stop(const void *owner, int handle);
bool orb_sound_app_active(const void *owner, int handle);

/** Stop one voice (a handle from orb_sound_play), or every voice. Safe on a handle
 *  that already ended. */
void orb_sound_stop(int handle);
void orb_sound_stop_all(void);

/** True while the handle's voice is queued or sounding. */
bool orb_sound_active(int handle);

/** Move an anchored loop's anchor (the wall clock re-anchored under it). */
void orb_sound_reanchor(int handle, int64_t anchor_us);

/** Keep LEASED voices alive. An app renews on every tick it runs; when its ticks
 *  stop — the dial left the glass, the app was suspended — the lease lapses after
 *  1.5 s and its voices fall silent without the app having to be asked (C7, read
 *  for audio). Queued strokes that would sound after the lapse never do. */
void orb_sound_lease_renew(void);

/* ---- owner settings: device config, like the tz (§5) ------------------------ */
typedef struct {
    uint8_t volume;        /* 0..100; 0 = silent (amp off) */
    bool    tick;          /* the tick loop */
    bool    chimes;        /* quarters and the hour chime */
    bool    strikes;       /* the hour pattern */
    bool    jumps;         /* step and jump events (D41) */
    int8_t  quiet_from;    /* hour 0..23 quiet hours start, -1 = none */
    int8_t  quiet_to;      /* hour 0..23 quiet hours end (exclusive) */
    int16_t offset_ms;     /* audio delay so a sound lands with the glass (M-S2) */
    int8_t  pattern;       /* hour pattern override, -1 = the theme's own */
    int8_t  quiet_last_from;   /* the last quiet hours set, kept while they are off, so */
    int8_t  quiet_last_to;     /* turning them on restores them (docs/knob-edit-plan.md
                                * §1.3). Written by orb_sound_cfg_set, never refused. */
} orb_sound_cfg_t;

void      orb_sound_cfg_get(orb_sound_cfg_t *out);
/** Validate, apply and persist. Returns a sentence on refusal, NULL on success. */
const char *orb_sound_cfg_set(const orb_sound_cfg_t *in);

/** True when `hour` (0..23, local) falls inside the owner's quiet hours. */
bool orb_sound_quiet_at(int hour);
/** True now, by the local wall clock. */
bool orb_sound_quiet_now(void);

/* ---- bench ------------------------------------------------------------------ */
typedef struct {
    uint32_t blocks;        /* mixer blocks written since boot */
    uint32_t sent;          /* DMA buffers the hardware reported sent */
    uint32_t voices;        /* voices queued or sounding now */
    uint32_t started;       /* voices started since boot */
    uint32_t nudges;        /* one-sample loop corrections */
    uint32_t snaps;         /* loop re-entries after a large error (stall, start) */
    int32_t  loop_err;      /* last measured anchored-loop error, samples */
    bool     pa_on;
    bool     running;       /* I2S enabled (false while idle) */
} orb_sound_stats_t;

void orb_sound_stats(orb_sound_stats_t *out);

/** Play a built-in bench sound with no theme: "ding" (a small bell), "tick" (one
 *  click) or "loop" (a 1 Hz click loop anchored to the wall second; play again to
 *  stop). Returns NULL, or a sentence naming the refusal. */
const char *orb_sound_test(const char *which);

#ifdef __cplusplus
}
#endif
