/* orb_sound_app.c — an app's own theme sounds, by slot (D39's service surface).
 *
 * The svc entries (orb_svc.c, cap SOUND) land here. An app never hands the engine a
 * pointer: it names a slot, and the sound is that slot's in THAT app's voice bank
 * (theme-cache-plan §2.6), which boot maps apart from the display entry and never remaps
 * (a store reboots, and stops every voice first), so the pointer handed to the engine
 * outlives the voice whatever the glass binds. See orb_sound.h for the contract. */
#include "orb_sound.h"
#include "orb_adpcm.h"

#include "art_bank.h"
#include "esp_timer.h"
#include "orb_assets.h"
#include "orb_elems.h"
#include "orb_svc.h"      /* orb_time_now(): an anchored loop's wall second */
#include "orb_theme.h"

int orb_sound_app_play(const void *owner, const char *slug, uint16_t slot,
                       uint32_t flags, uint32_t delay_ms)
{
    if (!owner || !slug) return -ESP_ERR_INVALID_ARG;
    /* §64.6: bits 8..15 carry a level as a share of the OS volume, stored +1 so 0 is "full"
     * (every app before this). It only ever lowers the voice gain: never louder than the master. */
    const uint32_t vol = (flags >> 8) & 0xFFu;
    flags &= ~0xFF00u;
    if (flags & ~(ORB_SOUND_LOOP | ORB_SOUND_ANCHORED)) return -ESP_ERR_INVALID_ARG;
    const uint32_t pct = vol ? (vol - 1u > 100u ? 100u : vol - 1u) : 100u;
    const uint16_t gain = (uint16_t)(ORB_SOUND_GAIN_1 * pct / 100u);
    if (gain == 0) return -ESP_ERR_INVALID_STATE;            /* 0 %: nothing to play */
    /* theme-cache-plan §2.6: from the app's voice bank, mapped from boot apart from the
     * display entry a bind replaces, so it plays whatever is on the glass. orb_voice_parse
     * held every sound to its bank's bounds and even offsets when boot listed it. */
    const uint8_t *bank = NULL;
    const orb_voice_t *vb = orb_theme_voice(slug, &bank);
    if (!vb || !bank) return -ESP_ERR_NOT_FOUND;
    const clk_sound_t *t = NULL;
    for (uint32_t k = 0; k < vb->n && !t; k++) if (vb->s[k].slot == slot) t = &vb->s[k];
    if (!t) return -ESP_ERR_NOT_FOUND;
    const void *clip = bank + t->pcm_off;
    if (flags & ORB_SOUND_LOOP) flags |= ORB_SOUND_LEASED;   /* a loop is its app's on the glass alone */
    const int64_t now = esp_timer_get_time();
    int64_t anchor = 0;
    if (flags & ORB_SOUND_ANCHORED) {
        time_t sec_now; double frac; bool synced;
        orb_time_now(&sec_now, &frac, &synced);
        anchor = now - (int64_t)(frac * 1e6);
    }
    return orb_sound_play_enc(owner, clip, t->n, t->enc, gain, flags,
                              delay_ms ? now + (int64_t)delay_ms * 1000 : 0, anchor);
}
