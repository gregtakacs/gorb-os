/* orb_sound_menu.c — the OS's Sound page: one service row in the shell menu.
 *
 * Level and quiet hours are the owner's, and the OS applies them to every app's
 * sound (orb_sound.c's mixer and play gate), so they live on an OS page beside USB
 * Files rather than on any app's. The volume and the quiet hours' ends are edited with
 * the knob (docs/knob-edit-plan.md): a press enters the shell's edit mode, a turn
 * adjusts, a press saves. Quiet hours' own row is a press that turns them on or off,
 * and on restores the last hours set. Every save goes through orb_sound_cfg_set(), the
 * one door /api/v1/sound writes too. What an app does with sound (the clock's tick and
 * chimes) stays that app's. */
#include <stdio.h>

#include "orb_app.h"
#include "orb_sound.h"

enum { ROW_VOLUME, ROW_QUIET, ROW_FROM, ROW_UNTIL, ROW_TEST, ROW_N };

static char s_val[24];

static int snd_opt_count(orb_app_t *app) { (void)app; return ROW_N; }

static const char *snd_opt_label(orb_app_t *app, int idx)
{
    (void)app;
    switch (idx) {
    case ROW_VOLUME: return "Volume";
    case ROW_QUIET:  return "Quiet hours";
    case ROW_FROM:   return "Quiet from";
    case ROW_UNTIL:  return "Quiet until";
    case ROW_TEST:   return "Play a test sound";
    default:         return "";
    }
}

/* An end of quiet hours in half-hour steps (0..47): the times in force, or the last set
 * while they are off. */
static int snd_end(const orb_sound_cfg_t *c, bool from)
{
    const bool on = c->quiet_from >= 0;
    const int h = from ? (on ? c->quiet_from : c->quiet_last_from) : (on ? c->quiet_to : c->quiet_last_to);
    const int m = from ? (on ? c->quiet_from_m : c->quiet_last_from_m) : (on ? c->quiet_to_m : c->quiet_last_to_m);
    return h * 2 + (m >= 30 ? 1 : 0);
}

/* A value as its row shows it, saved or not. */
static const char *snd_opt_fmt(orb_app_t *app, int idx, int32_t v)
{
    (void)app;
    switch (idx) {
    case ROW_VOLUME:
        if (!v) return "off";
        snprintf(s_val, sizeof s_val, "%ld", (long)v);
        return s_val;
    case ROW_FROM:
    case ROW_UNTIL:                                   /* half-hour steps: v is 0..47 */
        snprintf(s_val, sizeof s_val, "%02ld:%02ld", (long)(v / 2), (long)(v % 2 * 30));
        return s_val;
    default:
        return "";
    }
}

static const char *snd_opt_value(orb_app_t *app, int idx)
{
    orb_sound_cfg_t c;
    orb_sound_cfg_get(&c);
    if (!orb_sound_ready()) return idx == ROW_TEST ? "no speaker" : "";
    switch (idx) {
    case ROW_VOLUME: return snd_opt_fmt(app, idx, c.volume);
    case ROW_QUIET:  return c.quiet_from < 0 ? "off" : "on";
    /* The ends read the times in force, or the last set while quiet hours are off. */
    case ROW_FROM:   return snd_opt_fmt(app, idx, snd_end(&c, true));
    case ROW_UNTIL:  return snd_opt_fmt(app, idx, snd_end(&c, false));
    default:         return "";
    }
}

static bool snd_opt_edit(orb_app_t *app, int idx, orb_opt_edit_t *e)
{
    (void)app;
    if (!orb_sound_ready()) return false;
    orb_sound_cfg_t c;
    orb_sound_cfg_get(&c);
    switch (idx) {
    case ROW_VOLUME:
        *e = (orb_opt_edit_t){ .min = 0, .max = 100, .value = c.volume, .step_max = 5 };
        return true;
    case ROW_FROM:
    case ROW_UNTIL: {
        *e = (orb_opt_edit_t){ .min = 0, .max = 47, .value = snd_end(&c, idx == ROW_FROM), .step_max = 4, .wrap = true };
        return true; }
    default:
        return false;
    }
}

/* Saving an end of quiet hours while they are off keeps them off and sets the hours
 * they come back with. */
static void snd_opt_set(orb_app_t *app, int idx, int32_t v)
{
    (void)app;
    orb_sound_cfg_t c;
    orb_sound_cfg_get(&c);
    if (idx == ROW_VOLUME) {
        c.volume = (uint8_t)v;
    } else if (idx == ROW_FROM || idx == ROW_UNTIL) {
        const bool on = c.quiet_from >= 0, from = idx == ROW_FROM;
        int8_t *hr = on ? (from ? &c.quiet_from : &c.quiet_to) : (from ? &c.quiet_last_from : &c.quiet_last_to);
        int8_t *mn = on ? (from ? &c.quiet_from_m : &c.quiet_to_m) : (from ? &c.quiet_last_from_m : &c.quiet_last_to_m);
        *hr = (int8_t)(v / 2);
        *mn = (int8_t)(v % 2 * 30);
    } else {
        return;
    }
    (void)orb_sound_cfg_set(&c);
}

static void snd_opt_sel(orb_app_t *app, int idx)
{
    (void)app;
    orb_sound_cfg_t c;
    orb_sound_cfg_get(&c);
    if (idx == ROW_QUIET) {
        if (c.quiet_from >= 0) { c.quiet_from = -1; c.quiet_to = -1; }
        else {
            c.quiet_from = c.quiet_last_from; c.quiet_to = c.quiet_last_to;
            c.quiet_from_m = c.quiet_last_from_m; c.quiet_to_m = c.quiet_last_to_m;
        }
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
    .opt_edit  = snd_opt_edit,
    .opt_fmt   = snd_opt_fmt,
    .opt_set   = snd_opt_set,
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
