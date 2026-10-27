cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# The clock's GIF delta mode invalidates ~16 background rects plus the hands'
# swept wedges in one tick. LVGL's default invalid-area table is 32 entries;
# when a tick fills it, lv_refr.c degrades to a WHOLE-SCREEN invalidate
# (~145 ms/flush — defeating the delta packing entirely). Grow the table so
# partial-render stays partial. (LVGL 9.6 has no Kconfig entry for this and
# guards it with #ifndef, so a global -D is the supported override.)
add_compile_definitions(LV_INV_BUF_SIZE=96)

# ---------------------------------------------------------------------------
# Firmware version stamp: docs/firmware-versioning-plan.md.
#
# version.txt declares v<MAJOR>.<MINOR>.<PATCH>. The metadata appended here lets a
# board name the exact bytes it runs, and the whole string lands in
# esp_app_desc_t.version: char[32], so 31 usable bytes once strlcpy's NUL is counted.
# PROJECT_VER set here wins: IDF takes a
# pre-defined PROJECT_VER first and only then tries version.txt, a VERSION
# argument, and git describe (project.cmake:660-712).
#
# Bump version.txt, then reconfigure. CMake caches the stamp at CONFIGURE time, so
# `idf.py build` alone rebuilds sources around the old stamp. serflash.sh reads the
# stamp out of the image and refuses the mismatch, which is the point.
# ---------------------------------------------------------------------------
find_program(ORB_BASH NAMES bash sh)
if(ORB_BASH)
    execute_process(
        COMMAND "${ORB_BASH}" "${CMAKE_CURRENT_LIST_DIR}/tools/orb_version.sh"
                "${CMAKE_CURRENT_LIST_DIR}"
        OUTPUT_VARIABLE ORB_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE  ORB_VERSION_ERR RESULT_VARIABLE ORB_VERSION_RC)
    if(NOT ORB_VERSION_RC EQUAL 0)
        message(FATAL_ERROR "orb_version.sh refused to name this tree: ${ORB_VERSION_ERR}")
    endif()
else()
    # No shell on PATH, which is a bare Windows configure: the declared triple
    # alone is still ordered, and order is the half of the scheme that matters most.
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" ORB_VERSION)
    string(STRIP "${ORB_VERSION}" ORB_VERSION)
endif()
if(NOT ORB_VERSION MATCHES "^v[0-9][0-9]?\\.[0-9][0-9]?\\.[0-9][0-9]?")
    message(FATAL_ERROR "orb firmware version \"${ORB_VERSION}\" does not parse")
endif()
string(LENGTH "${ORB_VERSION}" ORB_VERSION_LEN)
if(ORB_VERSION_LEN GREATER 31)
    message(FATAL_ERROR "orb firmware version \"${ORB_VERSION}\" is ${ORB_VERSION_LEN} bytes; the descriptor field holds 31")
endif()
set(PROJECT_VER "${ORB_VERSION}")
# Register the dependency ourselves. Bypassing IDF's version.txt reader bypasses
# its own CMAKE_CONFIGURE_DEPENDS, so a bump would otherwise sit unnoticed in the
# cache and print a stamp that disagrees with the file.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/version.txt")
message(STATUS "orb firmware version: ${ORB_VERSION}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gorb-os)

# ---------------------------------------------------------------------------
# D36 (§0.11): the firmware flashes bare. No art bank in the manifest.
#
# The `assets` partition stays in the table — it is where the store door installs
# a theme (D9/D30) — but nothing is written there at flash time. A fresh or wiped
# board boots the shell menu with slot A erased: art_bank/adopt_default_bundle
# refuse the blank slot exactly the way they refuse a damaged one, and the clock
# falls to the §26 base render. The dial's first face arrives when the hub seeds
# the clock app and the built-in theme through the ordinary theme doors
# (tools/orb-manager/seed/, POST /api/v1/sd/theme + /api/v1/sd/bundle). The
# default's baked bytes live on as the hub's seed source, `assets/orb_default.img`
# (regenerate the seed with tools/make_seed_theme.py).
# ---------------------------------------------------------------------------
