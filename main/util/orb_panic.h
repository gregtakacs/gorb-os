/* orb_panic.h — the last panic, kept across the reset it causes.
 *
 * A panic's own report goes to the serial console and nowhere else, so a crash on
 * an orb nobody is capturing leaves only rst_name "panic" behind (2026-10-31: a
 * theme store on v1.1.41 panicked mid-slot-write, and five captured attempts never
 * reproduced it). This keeps the panic's kind, faulting PC, EXCCAUSE/EXCVADDR and
 * backtrace in RTC memory, which a software reset does not clear, and /api/v1/status
 * reports it, so the next one explains itself over the LAN. The PCs decode against
 * the image's own ELF: xtensa-esp32s3-elf-addr2line -pfiaC -e build/gorb-os.elf <bt>.
 */
#pragma once

#include <stddef.h>

/* Count this boot and validate the record. Call once, early in app_main. */
void orb_panic_boot(void);

/* Write `"panic":{...},` describing the last recorded panic into out, or nothing
 * when there is none. Returns the bytes written, or -1 when cap is too small. */
int orb_panic_json(char *out, size_t cap);
