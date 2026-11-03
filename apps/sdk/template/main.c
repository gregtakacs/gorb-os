/**
 * @SLUG@ — a gorb-os app. Turn the knob while it is armed (press to arm, hold to
 * disarm) to change the count; the count is a declared setting, so it survives a
 * reboot and the hub can edit it. The clock line shows the beat working.
 *
 * Everything the OS offers is in orb.h; build with `orbsdk.py pack .`.
 */
#include "orb.h"
#include <stdio.h>

static int32_t s_title, s_count_lbl, s_clock, s_hint;
static int32_t s_count;
static int     s_armed;

static int32_t label(int32_t font, uint32_t rgb, const char *text)
{
    const int32_t h = orb_ui_create(ORB_UI_LABEL, 0);
    orb_ui_set(h, ORB_P_FONT, font);
    orb_ui_set(h, ORB_P_TEXT_COLOR, (int32_t)rgb);
    orb_ui_text(h, text);
    return h;
}

static void show_count(void)
{
    char buf[24];
    snprintf(buf, sizeof buf, "%d", (int)s_count);
    orb_ui_text(s_count_lbl, buf);
    orb_ui_set(s_count_lbl, ORB_P_TEXT_COLOR, s_armed ? 0x40C0FF : 0xFFFFFF);
    orb_ui_text(s_hint, s_armed ? "turn: count   hold: done" : "press to edit");
}

static void show_clock(void)
{
    int64_t sec; int32_t ns;
    orb_tm_t tm;
    char buf[16];
    if (!orb_now(&sec, &ns) || orb_localtime(sec, &tm) != ORB_OK) {
        orb_ui_text(s_clock, "--:--:--");
        return;
    }
    snprintf(buf, sizeof buf, "%02d:%02d:%02d", (int)tm.hour, (int)tm.min, (int)tm.sec);
    orb_ui_text(s_clock, buf);
}

ORB_EXPORT("orb_init") int32_t orb_init(void)
{
    orb_cfg_get("count", &s_count);
    s_title = label(ORB_FONT_16, 0x909090, "@TITLE@");
    orb_ui_align(s_title, ORB_AL_TOP_MID, 0, 90);
    s_count_lbl = label(ORB_FONT_48, 0xFFFFFF, "0");
    orb_ui_align(s_count_lbl, ORB_AL_CENTER, 0, -10);
    s_clock = label(ORB_FONT_16, 0x606060, "");
    orb_ui_align(s_clock, ORB_AL_CENTER, 0, 50);
    s_hint = label(ORB_FONT_14, 0x909090, "");
    orb_ui_align(s_hint, ORB_AL_BOTTOM_MID, 0, -70);
    show_count();
    show_clock();
    orb_logf("ready, count=%d", (int)s_count);
    return ORB_OK;
}

ORB_EXPORT("orb_show") void orb_show(void)
{
    orb_set_beat(1000);
    show_count();
    show_clock();
    orb_ui_refresh();
}

ORB_EXPORT("orb_tick") void orb_tick(uint32_t now_ms)
{
    show_clock();
}

ORB_EXPORT("orb_input") void orb_input(int32_t delta, int32_t pressed)
{
    if (!s_armed || !delta) return;
    s_count += delta;
    if (s_count < 0) s_count = 0;
    if (s_count > 9999) s_count = 9999;
    show_count();
}

ORB_EXPORT("orb_set_armed") int32_t orb_set_armed(int32_t armed)
{
    if (s_armed && !armed) orb_cfg_set("count", s_count);    /* commit on the edge */
    s_armed = armed != 0;
    show_count();
    orb_ui_refresh();
    return s_armed;
}
