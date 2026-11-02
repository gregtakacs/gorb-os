/* orb_sound_menu.c — the OS's Sound page: one service row in the shell menu.
 *
 * Level and quiet hours are the owner's, and the OS applies them to every app's
 * sound (orb_sound.c's mixer and play gate), so they live on an OS page beside USB
 * Files rather than on any app's. The menu's rows are press-only (the shell never
 * calls opt_inc), so each setting is a short ring a press steps around; the choice
 * is saved at once, the way /api/v1/sound saves it — both doors write the one
 * orb_sound_cfg_set(). What an app does with sound (the clock's tick and chimes)
 * stays that app's. */
#include <stdio.h>

#include "orb_app.h"
#include "orb_sound.h"

enum { ROW_VOLUME, ROW_QUIET, ROW_TEST, ROW_N };

/* The level ring: off, then quiet to full. 60 is the default the owner settled on. */
static const uint8_t VOLS[] = { 0, 20, 40, 60, 80, 100 };
/* The quiet-hours ring, as (from, to): off first, then the nights most people keep. */
static const int8_t QUIET[][2] = { { -1, -1 }, { 22, 7 }, { 23, 7 }, { 21, 7 }, { 22, 6 } };
#define N_VOLS  (sizeof VOLS / sizeof VOLS[0])
#define N_QUIET (sizeof QUIET / sizeof QUIET[0])

static char s_val[24];

static int snd_opt_count(orb_app_t *app) { (void)app; return ROW_N; }

static const char *snd_opt_label(orb_app_t *app, int idx)
{
    (void)app;
    switch (idx) {
    case ROW_VOLUME: return "Volume";
    case ROW_QUIET:  return "Quiet hours";
    case ROW_TEST:   return "Play a test sound";
    default:         return "";
    }
}

static const char *snd_opt_value(orb_app_t *app, int idx)
{
    (void)app;
    orb_sound_cfg_t c;
    orb_sound_cfg_get(&c);
    if (!orb_sound_ready()) return idx == ROW_TEST ? "no speaker" : "";
    switch (idx) {
    case ROW_VOLUME:
        if (!c.volume) return "off";
        snprintf(s_val, sizeof s_val, "%u", (unsigned)c.volume);
        return s_val;
    case ROW_QUIET:
        if (c.quiet_from < 0) return "off";
        snprintf(s_val, sizeof s_val, "%02d:00-%02d:00", c.quiet_from, c.quiet_to);
        return s_val;
    default:
        return "";
    }
}

/* The next stop on a ring after the current setting; a setting that is not on the
 * ring (set over /api/v1/sound) steps to the ring's first stop above it, or wraps. */
static void snd_opt_sel(orb_app_t *app, int idx)
{
    (void)app;
    orb_sound_cfg_t c;
    orb_sound_cfg_get(&c);
    if (idx == ROW_VOLUME) {
        size_t i = 0;
        while (i < N_VOLS && VOLS[i] <= c.volume) i++;
        c.volume = VOLS[i < N_VOLS ? i : 0];
        (void)orb_sound_cfg_set(&c);
    } else if (idx == ROW_QUIET) {
        size_t i = 0;
        for (size_t k = 0; k < N_QUIET; k++)
            if (QUIET[k][0] == c.quiet_from && QUIET[k][1] == c.quiet_to) { i = k + 1; break; }
        if (i >= N_QUIET) i = 0;
        c.quiet_from = QUIET[i][0];
        c.quiet_to   = QUIET[i][1];
        (void)orb_sound_cfg_set(&c);
    } else if (idx == ROW_TEST) {
        (void)orb_sound_test("ding");
    }
}

static void snd_opt_arm(orb_app_t *app, bool armed) { (void)app; (void)armed; }

/* No face and nothing to build: the engine came up on the boot ladder, and a board
 * without the codec still lists the page, saying so on the test row. */
static esp_err_t snd_init(orb_app_t *app) { (void)app; return ESP_OK; }

static const orb_app_iface_t s_sound_iface = {
    .init      = snd_init,
    .opt_count = snd_opt_count,
    .opt_label = snd_opt_label,
    .opt_value = snd_opt_value,
    .opt_sel   = snd_opt_sel,
    .opt_arm   = snd_opt_arm,
};

orb_app_t orb_app_sound_row = {
    .slug      = "sound",
    .alias     = NULL,
    .title     = "Sound",
    .kind      = ORB_APP_KIND_APP,
    .is_home   = false,
    .behaviour_kind = ORB_BEHAVIOUR_NONE,
    .art_offset = 0,
    .art_bytes  = 0,
    .art_slot   = -1,
    .flash_slot = -1,          /* an OS row: no card slot, outside the ledger span */
    .iface     = &s_sound_iface,
    .installed = true,
};
