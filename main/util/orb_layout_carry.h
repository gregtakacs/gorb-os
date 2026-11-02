/* orb_layout_carry.h — the `storage` NVS carried across the Layout B → C table change.
 *
 * §43 moved `storage` (Wi-Fi credentials, settings, grants, secrets) from 0xD60000 to
 * 0xF20000. A flash writes the new table and the new image and nothing else, so on the
 * first Layout C boot the owner's NVS still sits at the old offset, now inside `assets`,
 * and the new `storage` holds whatever Layout B kept there (the old `apps` ledger).
 * Without a carry the board boots with no Wi-Fi and is unreachable over the LAN.
 */
#pragma once

/* Copy the old NVS into `storage` when `storage` holds no NVS and the old offset still
 * does, then erase the old copy so the carry runs exactly once. When `storage` already
 * holds NVS, a surviving old copy is only erased. Call before anything opens `storage`.
 * A failed carry leaves the old copy standing for the next boot. */
void orb_layout_carry_storage(void);
