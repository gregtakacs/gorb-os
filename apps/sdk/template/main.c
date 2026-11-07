/**
 * @SLUG@ — a gorb-os app. The knob is the app's own dial (apps/README.md, "The knob"):
 * turning moves along it from the first click, 0 is home, a fast spin stops on home and
 * 20 s without a turn returns there. A press saves the value as the "saved" setting, so
 * it survives a reboot; app.json declares it, so the OS also shows it on the dial's menu
 * page and in Orb Manager, and the app re-reads it when orb_cfg_gen moves. A hold leaves
 * the app; the OS does that.
 * The clock line shows the beat working.
 *
 * Everything the OS offers is in orb.h; build with `orbsdk.py pack .`.
 */
#include "orb.h"
#include "orb_dial.h"
#include <stdio.h>

static int32_t s_title, s_value_lbl, s_clock, s_hint;
static int32_t s_saved;
static uint32_t s_cfg_gen;               /* the settings generation s_saved was read at */
static orb_dial_t s_dial = { .lo = -50, .hi = 50, .idle_ms = ORB_DIAL_IDLE_MS };

static int64_t up_ms(void) { return orb_uptime_us() / 1000; }

static int32_t label(int32_t font, uint32_t rgb, const char *text)
{
    const int32_t h = orb_ui_create(ORB_UI_LABEL, 0);
    orb_ui_set(h, ORB_P_FONT, font);
    orb_ui_set(h, ORB_P_TEXT_COLOR, (int32_t)rgb);
    orb_ui_text(h, text);
    return h;
}

static void show_value(void)
{
    char buf[40];
    snprintf(buf, sizeof buf, "%d", (int)s_dial.pos);
    orb_ui_text(s_value_lbl, buf);
    orb_ui_set(s_value_lbl, ORB_P_TEXT_COLOR, s_dial.pos == s_saved ? 0x40C0FF : 0xFFFFFF);
    snprintf(buf, sizeof buf, "saved %d   press: save", (int)s_saved);
    orb_ui_text(s_hint, buf);
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
    orb_knob_dial();                     /* every turn and press is ours while we show */
    s_cfg_gen = orb_cfg_gen();
    orb_cfg_get("saved", &s_saved);
    s_title = label(ORB_FONT_16, 0x909090, "@TITLE@");
    orb_ui_align(s_title, ORB_AL_TOP_MID, 0, 90);
    s_value_lbl = label(ORB_FONT_48, 0xFFFFFF, "0");
    orb_ui_align(s_value_lbl, ORB_AL_CENTER, 0, -10);
    s_clock = label(ORB_FONT_16, 0x606060, "");
    orb_ui_align(s_clock, ORB_AL_CENTER, 0, 50);
    s_hint = label(ORB_FONT_14, 0x909090, "");
    orb_ui_align(s_hint, ORB_AL_BOTTOM_MID, 0, -70);
    show_value();
    show_clock();
    orb_logf("ready, saved=%d", (int)s_saved);
    return ORB_OK;
}

/* Back on the glass: home first, so a glance always shows the same view. */
ORB_EXPORT("orb_activate") void orb_activate(void) { orb_dial_home(&s_dial); }

ORB_EXPORT("orb_show") void orb_show(void)
{
    orb_set_beat(1000);
    show_value();
    show_clock();
    orb_ui_refresh();
}

ORB_EXPORT("orb_tick") void orb_tick(uint32_t now_ms)
{
    (void)now_ms;
    bool redraw = orb_dial_idle(&s_dial, up_ms());
    if (orb_cfg_gen() != s_cfg_gen) {    /* changed from the menu or the hub */
        s_cfg_gen = orb_cfg_gen();
        orb_cfg_get("saved", &s_saved);
        redraw = true;
    }
    if (redraw) show_value();
    show_clock();
}

/* delta > 0 is clockwise; |delta| > 1 only on a fast spin (the OS accelerates). */
ORB_EXPORT("orb_input") void orb_input(int32_t delta, int32_t pressed)
{
    if (pressed) {
        s_saved = s_dial.pos;
        orb_cfg_set("saved", s_saved);   /* a press is a deliberate edge: save on it */
    }
    if (delta) orb_dial_turn(&s_dial, delta, up_ms());
    show_value();
}
