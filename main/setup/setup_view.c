/* The setup screen — contract in setup_view.h (app-platform-plan §66).
 *
 * The QR is built the way menu_view.c's ABOUT builds its code: LVGL's qrcodegen into
 * a PSRAM plane of one pixel per module, drawn through one image scaled up nearest. No
 * lv_qrcode widget, whose draw buffer would come from the LVGL pool. Black on white
 * with the spec's four-module quiet zone, because the glass around it is black and a
 * scanner needs the light border to find the code.
 *
 * Two payloads. The default joins the hotspot: "WIFI:T:nopass;S:orb-XXXX;;" is the
 * ZXing Wi-Fi code every current phone camera reads (iOS offers a Join card, Android
 * joins on confirm); the hotspot is open by design, so nothing secret is in it. Once
 * joined, the captive page opens by itself. Holding the knob swaps in the page's
 * address, for the phone that does not read Wi-Fi codes and has joined by hand. */
#include "setup_view.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "libs/qrcode/qrcodegen.h"

#include "knob.h"
#include "orb_net.h"

static const char *TAG = "setup";

#define QR_VER_MAX   4                 /* "WIFI:T:nopass;S:orb-ABCD;;" is version 2 */
#define QR_QUIET     4
#define QR_SIDE_MAX  (QR_VER_MAX * 4 + 17 + 2 * QR_QUIET)
#define QR_GLASS_PX  224               /* the code's side on the glass, at most     */
#define BLANK_AFTER_MS (120 * 1000)    /* §66 (plan P3): an AMOLED left on a QR burns */
#define PAGE_URL     "http://192.168.4.1/"

EXT_RAM_BSS_ATTR static uint16_t s_qr_px[QR_SIDE_MAX * QR_SIDE_MAX];
static lv_image_dsc_t s_qr_dsc;

static lv_obj_t *s_scr, *s_box, *s_img, *s_title, *s_hint, *s_alt;
static char      s_ssid[33];
static bool      s_url_mode;           /* false: the join code; true: the address  */
static bool      s_blank;
static int64_t   s_last_touch_us;

/* Encode text into the plane and point the image at it. False only if the text does
 * not fit a version-4 code, which neither payload comes near. */
static bool qr_build(const char *text)
{
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VER_MAX)];
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VER_MAX)];
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, QR_VER_MAX,
                              qrcodegen_Mask_AUTO, true)) {
        ESP_LOGE(TAG, "\"%s\" does not fit a version-%d code", text, QR_VER_MAX);
        return false;
    }
    const int size = qrcodegen_getSize(qr), n = size + 2 * QR_QUIET;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            s_qr_px[y * n + x] =
                qrcodegen_getModule(qr, x - QR_QUIET, y - QR_QUIET) ? 0x0000 : 0xFFFF;
    memset(&s_qr_dsc, 0, sizeof s_qr_dsc);
    s_qr_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_qr_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_qr_dsc.header.w      = (uint32_t)n;
    s_qr_dsc.header.h      = (uint32_t)n;
    s_qr_dsc.header.stride = (uint32_t)n * 2u;
    s_qr_dsc.data_size     = (uint32_t)(n * n * 2);
    s_qr_dsc.data          = (const uint8_t *)s_qr_px;

    const int mod = QR_GLASS_PX / n;   /* whole pixels per module: edges stay sharp */
    lv_image_set_src(s_img, &s_qr_dsc);
    lv_obj_set_size(s_img, n, n);
    lv_image_set_pivot(s_img, n / 2, n / 2);
    lv_image_set_scale(s_img, (uint32_t)(mod * 256));
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 4);
    lv_obj_invalidate(s_scr);
    return true;
}

static void show_mode(void)
{
    if (s_url_mode) {
        qr_build(PAGE_URL);
        lv_label_set_text(s_title, "Open in a browser");
        lv_label_set_text_fmt(s_hint, "after joining %s", s_ssid);
        lv_label_set_text(s_alt, "hold for the join code");
    } else {
        char payload[64];
        snprintf(payload, sizeof payload, "WIFI:T:nopass;S:%s;;", s_ssid);
        qr_build(payload);
        lv_label_set_text(s_title, s_ssid);
        lv_label_set_text(s_hint, "Scan to set up this orb");
        lv_label_set_text(s_alt, "hold for " PAGE_URL);
    }
}

static void set_blank(bool blank)
{
    if (blank == s_blank) return;
    s_blank = blank;
    /* Black is off on an AMOLED: hiding the box leaves every pixel dark. */
    if (blank) lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_remove_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "glass %s", blank ? "blanked (2 min untouched; any knob event wakes it)"
                                    : "lit");
}

/* The knob, polled on the LVGL task the way the dial's screens poll it. A turn or a
 * press while blank only wakes the glass; a hold swaps the payload. */
static void poll_cb(lv_timer_t *t)
{
    (void)t;
    knob_poll();
    const int32_t turn  = knob_take_delta();
    const bool    press = knob_take_press();
    const bool    hold  = knob_take_long_press();
    (void)knob_take_push_delta();
    (void)knob_take_push_end();
    const int64_t now = esp_timer_get_time();
    if (turn || press || hold) {
        s_last_touch_us = now;
        if (s_blank) { set_blank(false); return; }
        if (hold) { s_url_mode = !s_url_mode; show_mode(); }
        return;
    }
    if (!s_blank && now - s_last_touch_us > (int64_t)BLANK_AFTER_MS * 1000) set_blank(true);
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t rgb, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    return l;
}

esp_err_t setup_view_start(void)
{
    orb_net_ap_ssid(s_ssid, sizeof s_ssid);

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Everything lives in one transparent box so blanking is one flag. */
    s_box = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_box);
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_SCROLLABLE);

    s_img = lv_image_create(s_box);
    lv_image_set_antialias(s_img, false);   /* nearest: a blurred module will not scan */

    /* The round glass: lines sit where the circle is wide enough for them. */
    s_title = label(s_box, &lv_font_montserrat_28, 0xEDEDED, -150);
    s_hint  = label(s_box, &lv_font_montserrat_18, 0xEDEDED, 142);
    s_alt   = label(s_box, &lv_font_montserrat_14, 0x8C8C8C, 170);   /* 14, 18, 28, 48: the fonts sdkconfig.defaults ships */

    show_mode();
    lv_screen_load(s_scr);
    s_last_touch_us = esp_timer_get_time();
    lv_timer_create(poll_cb, 20, NULL);
    ESP_LOGW(TAG, "setup screen up: join code for \"%s\" (hold the knob for %s)",
             s_ssid, PAGE_URL);
    return ESP_OK;
}
