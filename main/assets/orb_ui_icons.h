/*
 * main/assets/orb_ui_icons.h — the drawn cards the firmware carries (app-platform-plan §61).
 *
 * The carousel's Settings and the theme carousel's Back, drawn by tools/ui_icons/draw.py
 * and generated into orb_ui_icons.c by tools/ui_icons/make.py. The owner lifted D22's "no
 * shipped pixels" for these two (2026-11-08): they are the OS's own controls, not an app's
 * art. Each is an RGB565 disc (orb_disc.h) of `side` px, deflated for orb_inflate.
 */
#pragma once
#include <stdint.h>

extern const uint8_t  orb_ui_icon_settings_z[];
extern const uint32_t orb_ui_icon_settings_n;
extern const uint16_t orb_ui_icon_settings_side;
extern const uint8_t  orb_ui_icon_back_z[];
extern const uint32_t orb_ui_icon_back_n;
extern const uint16_t orb_ui_icon_back_side;
