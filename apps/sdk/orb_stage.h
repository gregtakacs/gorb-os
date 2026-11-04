/**
 * orb_stage.h — the stage: the OS's themed-element renderer (D50, plan §43), for any
 * app that ships a theme. It draws the live theme for the app that theme belongs to
 * (a theme is one app's, D48); any other app that opens it is told base = 1.
 *
 * The OS draws the app's themed face: the plate and its delta bank, every hand, pointer and
 * ring as a rotated sprite, the complication windows and strips, the glass, at the
 * frame rate the scheduler picks (D32), with every pixel and every damage mark its
 * own. What the face SHOWS is the app's: where each row points, what each
 * complication reads, and which sound plays when. Each frame the stage calls the
 * app's orb_tick; the app reads the frame's time with orb_stage_now, poses every row
 * with orb_stage_pose, sets every complication with orb_stage_comp_set, and returns.
 *
 * The records the theme carries are handed over verbatim, in the layouts of
 * clk_types.h (a byte copy of main/art/clk_types.h, which apps/sdk/orbsdk.py keeps
 * identical) and the clk_sound_t below, so the app reads its theme exactly as the
 * theme format defines it.
 */
#pragma once

#include "orb.h"
#include "clk_types.h"

/* A theme's sounds are numbered slots whose meaning is the app's own (the clock names
 * slot 5 its hour stroke); PCM is 16-bit mono at this rate. */
#define ORB_STAGE_SND_RATE 22050u

typedef struct {
    uint32_t pcm_off;
    uint32_t n;              /* samples */
    uint32_t first_beat;
    uint16_t beat_chz;
    uint8_t  slot;           /* CLK_SND_* */
    uint8_t  flags;
} clk_sound_t;

/* What the stage opened. base = 1: no theme is resident, nothing is drawn, and the
 * app shows its own face on its page (D31: the app supplies the default). */
typedef struct {
    uint16_t n_rows, n_comps;
    uint16_t n_sounds;
    uint8_t  pattern;        /* the theme's sound-pattern word (the app gives it meaning) */
    uint8_t  base;
    uint8_t  car_row;        /* the carriage's row, 0xFF none */
    uint8_t  tick_rate;      /* the dial's beat, beats per second */
    uint8_t  second_sweep;
    uint8_t  bg_mode;
    uint16_t tourbillon_n;
    uint16_t pad;
} orb_stage_info_t;

/* The owner's sound settings (the OS's Sound page), as the engine applies them. */
typedef struct {
    uint8_t volume;
    uint8_t tick, chimes, strikes, jumps;
    int8_t  quiet_from, quiet_to;
    int8_t  pattern;         /* -1 = the theme's own */
} orb_stage_sound_cfg_t;

#define ORB_STAGE_HIDDEN  ((int32_t)0x80000000)   /* a pose that draws nothing */

/** Open the stage on this app's page. Refused unless the app owns the section. */
ORB_IMPORT("stage_open")     int32_t orb_stage_open(orb_stage_info_t *out);
ORB_IMPORT("stage_row")      int32_t orb_stage_row(int32_t i, clk_row_t *out);
ORB_IMPORT("stage_comp")     int32_t orb_stage_comp(int32_t i, clk_comp_t *out);
ORB_IMPORT("stage_sound")    int32_t orb_stage_sound(int32_t i, clk_sound_t *out);
/** This frame's time: the wall second, the fraction the rows should read (held
 *  between beats on a delta-bank dial), and the true wall fraction. Nanoseconds. */
ORB_IMPORT("stage_now")      void    orb_stage_now(int64_t *t, int32_t *frac_ns, int32_t *wall_ns);
ORB_IMPORT("stage_pose")     void    orb_stage_pose(int32_t i, int32_t ang_10, int32_t ox, int32_t oy);
/** Pick the frame a row shows, for a row whose frames the app picks (a moving row
 *  with frames and no frame time). Answers the row's frame count: 1 for a row of one
 *  image, 0 for no such row. f in range shows frame f from the next frame on; f < 0
 *  only asks. */
ORB_IMPORT("stage_frame")    int32_t orb_stage_frame(int32_t i, int32_t f);
ORB_IMPORT("stage_comp_set") void    orb_stage_comp_set(int32_t i, int32_t value, int32_t live);

/* Sound (cap "sound"): the app's own theme sounds, by index into its sound table,
 * at an instant on orb_uptime_us()'s clock. Every voice dies when the dial parks. */
#define ORB_STAGE_SND_LOOP     0x01u
#define ORB_STAGE_SND_ANCHORED 0x02u    /* with LOOP: sample 0 rides anchor_us's phase */
#define ORB_STAGE_SND_ASKED    0x08u    /* the owner asked for it this moment */
ORB_IMPORT("stage_snd_ready")    int32_t orb_stage_snd_ready(void);
ORB_IMPORT("stage_snd_cfg")      void    orb_stage_snd_cfg(orb_stage_sound_cfg_t *out);
ORB_IMPORT("stage_snd_quiet")    int32_t orb_stage_snd_quiet(int32_t hour);
ORB_IMPORT("stage_snd_play")     int32_t orb_stage_snd_play(int32_t i, uint32_t flags, int64_t at_us,
                                                          int64_t anchor_us);
ORB_IMPORT("stage_snd_stop")     void    orb_stage_snd_stop(int32_t h);
ORB_IMPORT("stage_snd_active")   int32_t orb_stage_snd_active(int32_t h);
ORB_IMPORT("stage_snd_reanchor") void    orb_stage_snd_reanchor(int32_t h, int64_t anchor_us);
