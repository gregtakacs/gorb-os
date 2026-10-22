# wasm3 — the interpreter behind the .orbapp code tier (docs/app-platform-plan.md
# D3/D20/D27, §4.1). Vendored source, pinned; see PROVENANCE.md in this directory.
#
# Two choices here are load-bearing and were MEASURED against this tree, not copied
# from wasm3's platforms/embedded/esp32-idf example (that example targets a board with
# spare IRAM; this one has 1 byte free — `idf.py size` says IRAM 16383/16384):
#
#   ESP32        selects wasm3's microcontroller config branch (m3_config_platforms.h
#                :295), which turns OFF gas metering, multi-memory, memory64, typed
#                refs, atomics and stack-switching. That is what lets the snapshot and
#                api_wasi / api_libc translation units drop out of the link entirely —
#                they are gone from source/ here, and a guest never gets them: an
#                app's ONLY surface is the orb_svc_t table (D15), not libc.
#   M3_IN_IRAM   does NOT force the interpreter into IRAM. Its single effect
#                (m3_config_platforms.h:201) is to make `vectorcall` empty instead of
#                IRAM_ATTR. IRAM_ATTR on the op dispatch functions would place them in
#                the 16 KB IRAM that is already full — a link failure. Empty vectorcall
#                puts them in normal cached flash .text: the interpreter runs from
#                flash. We deliberately add NO linker fragment and NO IRAM_ATTR, so
#                this tier cannot take a single IRAM byte. The flag name is upstream's
#                for "I control placement myself"; the honest reading here is
#                "keep vectorcall off IRAM_ATTR."
#
# -fno-strict-aliasing is scoped PRIVATE to the engine only. wasm3 type-puns through
# unions throughout the compile/exec path (m3_env.h's operand unions), which the
# compiler's strict-aliasing model can mis-optimize at -O2. This does NOT touch the
# app pixel path or any other component — the -O2 image the frame budget was priced
# against is unchanged; only the interpreter's own objects relax the rule, which is
# wasm3's own documented stance (its top CMake adds -Wstrict-aliasing=2).
idf_component_register(
    SRC_DIRS "source"
    INCLUDE_DIRS "source"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP32
    M3_IN_IRAM
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -fno-strict-aliasing
    -Os
    -Wno-unused-parameter
    -Wno-unused-function
)
