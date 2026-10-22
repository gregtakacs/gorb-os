# wasm3 provenance

Vendored WebAssembly interpreter for the `.orbapp` code tier.

- **Upstream:** https://github.com/wasm3/wasm3 (canonical home; `google/wasm3` is dead)
- **Pinned at:** commit `28ecb9af6d2040e474a70f7cb7f43666740141fb` ("Implement Atomics", v28ecb9a)
- **License:** MIT (see `LICENSE`, © 2019 Steven Massey, Volodymyr Shymanskyy)
- **Vendored:** 2026-10-21, for P7.2 (docs/app-platform-plan.md §8 P7, D3/D20/D27)

## What was removed and why

Copied from `source/` verbatim, then deleted the translation units that a guest can
never reach, because an `.orbapp`'s only OS surface is the frozen `orb_svc_t` broker
table (D15) — not libc, not WASI, not the snapshot/restore debugger:

- `m3_api_wasi.[ch]`, `m3_api_uvwasi.c`, `m3_api_meta_wasi.c`, `m3_wasi.h`,
  `m3_api_wasi_{host,posix,win32}.h` — the WASI libc import set. A guest gets no
  filesystem, sockets, clock or env; every OS call goes through the shim, so WASI
  would be dead bytes and an unpoliced door.
- `m3_api_libc.[ch]` — abort/exit/setjmp/memcpy imports, same reason.
- `m3_api_tracer.[ch]` — a tracing hook, off by config anyway.
- `m3_snapshot.c` — snapshot/restore. It is `#if d_m3HasSnapshots`-guarded, and that
  resolves to `d_m3HasStackSwitching → d_m3HasTypedRefs → 0` on this target
  (`m3_config_platforms.h:320-328`), so it compiles to nothing; the file is deleted to
  make the absence load-bearing rather than config-dependent.

What stays: `m3_deterministic.c` compiles to nothing (`d_m3DeterministicProfile` = 0)
but is kept so a future profile flip is a define, not a re-vendor.

The remaining 13 `.c` files are the interpreter core and are byte-for-byte upstream at
the pinned commit. Any edit to them (there are none) must be re-recorded here.

## Build flags (see CMakeLists.txt)

`ESP32` + `M3_IN_IRAM`, `-Os`, `-fno-strict-aliasing` — all PRIVATE. The reasoning,
with the measured IRAM figure that rules out the upstream example's placement, is in
`CMakeLists.txt`. Do not move `-fno-strict-aliasing` to a global scope.
