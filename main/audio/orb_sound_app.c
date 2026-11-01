/* orb_sound_app.c — an app's own theme sounds, by slot (D39's service surface).
 *
 * The svc entries (orb_svc.c, cap SOUND) land here. An app never hands the engine a
 * pointer: it names a slot, and the sound is the (ORB_ASSET_SOUND, slot) row of THAT
 * app's section in the adopted theme, resolved through the same orb_elems join every
 * plane goes through. The bank stays mapped for the life of the adoption (a flip
 * reboots), and orb_theme stops every voice before it remaps, so the pointer handed
 * to the engine outlives the voice. See orb_sound.h for the contract. */
#include "orb_sound.h"

#include "art_bank.h"
#include "esp_timer.h"
#include "orb_assets.h"
#include "orb_elems.h"
#include "orb_svc.h"      /* orb_time_now(): an anchored loop's wall second */
#include "orb_theme.h"

int orb_sound_app_play(const void *owner, const char *slug, uint16_t slot,
                       uint32_t flags, uint32_t delay_ms)
{
    if (!owner || !slug) return ESP_ERR_INVALID_ARG;
    if (flags & ~(ORB_SOUND_LOOP | ORB_SOUND_ANCHORED)) return ESP_ERR_INVALID_ARG;
    const orb_bundle_sec_t *sec = NULL;
    uint32_t bank_off = 0;
    const uint8_t *base = art_bank_ptr();
    if (!base || !orb_theme_live_sec(slug, &sec, &bank_off)) return ESP_ERR_NOT_FOUND;
    orb_elem_t e;
    if (orb_elems_at(sec, ORB_ASSET_SOUND, slot, 0, &e, NULL, 0) != ORB_ELEMS_OK)
        return ESP_ERR_NOT_FOUND;
    /* The clock's projection refuses an odd plane at the door; another app's section
     * has no such reader, so the rule is held here, where the load would fault. */
    if (e.plane_h != 1 || e.size != e.plane_w * 2u || ((bank_off + e.off) & 1u))
        return ESP_ERR_INVALID_SIZE;
    const int16_t *pcm = (const int16_t *)(const void *)(base + bank_off + e.off);
    const int64_t now = esp_timer_get_time();
    int64_t anchor = 0;
    if (flags & ORB_SOUND_ANCHORED) {
        time_t sec_now; double frac; bool synced;
        orb_time_now(&sec_now, &frac, &synced);
        anchor = now - (int64_t)(frac * 1e6);
    }
    return orb_sound_play_owned(owner, pcm, e.plane_w, ORB_SOUND_GAIN_1, flags,
                                delay_ms ? now + (int64_t)delay_ms * 1000 : 0, anchor);
}
