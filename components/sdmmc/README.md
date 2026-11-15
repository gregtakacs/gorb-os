# SDMMC Protocol Layer Driver (vendored, patched)

> **This is a project-local override of ESP-IDF's built-in `sdmmc` component,
> copied from the v5.4.0 release and patched for gorb-os.** A component in the
> project `components/` directory shadows the built-in one of the same name, so
> these sources are what the firmware links against, not the IDF checkout's.
>
> Why it exists: cheap and counterfeit microSD cards mount fine under the
> Arduino SD stack (`sd_diskio.cpp`, the driver orb-os uses) but are reported
> absent under stock IDF. Two init steps are fatal upstream and merely
> tolerated by Arduino, and neither failure is something the 20/10/4 MHz
> ladder can fix, because both happen at the 400 kHz probe clock:
>
> 1. `sdmmc_init_spi_crc` (**sdmmc_sd.c**) — upstream aborts when the card
>    refuses CMD59 (data-CRC enable). Arduino switches its own checking off
>    and continues ("old card maybe"). Patched to do the same. This is the
>    patch the bench card (an "asdfg"-CID 3.8 GB SDHC) actually needed: it
>    refused CMD59 with ESP_ERR_INVALID_RESPONSE and now mounts at 20 MHz.
> 2. `sdmmc_init_sd_ssr` (**sdmmc_init.c**) — upstream lets a failed SD Status
>    read (ACMD13, 512-byte data block used only for card-info printing) abort
>    the mount. Arduino never reads SSR. Patched to log and continue; the same
>    card class is known to garble exactly this transfer, though the bench
>    card did not trip it.
>
> **When bumping ESP-IDF:** diff this copy against the new release's
> `components/sdmmc` first; if upstream changed, refresh the copy and re-apply
> the two `[gorb-os patch]` blocks (search that marker). The patches are
> deliberately small and marked so they survive the diff.

Everything below this notice is upstream's README, unmodified.

---

# SDMMC Protocol Layer Driver

## SD Host Side Related Component Architecture

### Architecture Graph

                            ┌─────────────────────────────────────────────┐
                            │                                             │
                            │  SD Protocol Layer Driver: components/sdmmc │
                            │                                             │
                            └──────────▲────────────────────▲─────────────┘
                                       │                    │
                                       │                    │
                            Implements │                    │ Implements
                                       │                    │
    ┌──────────────────────────────────┴─────┐         ┌────┴───────────────────────────────────┐
    │                                        │         │                                        │
    │Host Driver: components/esp_driver_sdmmc│         │Host Driver: components/esp_driver_sdspi│
    │                                        │         │                                        │
    └────────────────────────────────────────┘         └────────────────────────────────────────┘

### Components

- `sdmmc`: SD protocol layer driver, it provides SD protocol related definitions and interfaces. With corresponding implementation drivers, `sdmmc` APIs can help you:
  - send commands to slave devices
  - send and receive data
  - handle error conditions within the bus
- `esp_driver_sdmmc`: SDMMC Host driver for ESP SDMMC hardware, it implements the `sdmmc` protocol interfaces.
- `esp_driver_sdspi`: SDSPI Host driver for ESP GPSPI hardware, it implements the `sdmmc` protocol interfaces.

### Dependency

- `esp_driver_sdmmc` is in driver layer (G2), it relies on `sdmmc`
- `esp_driver_sdspi` is in driver layer (G2), it relies on `sdmmc`
- `sdmmc` does not and should not rely on `esp_driver_sdmmc` or `esp_driver_sdspi`. Though `sdmmc` is independent, it still stays in G2.
