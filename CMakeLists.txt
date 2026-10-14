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
# Clock art bank: flash assets/clock_assets.bin into the `assets` partition.
#
# The bank is no longer embedded in the app image (there is no
# target_add_binary_data in main/CMakeLists.txt any more), so it needs its own
# flash rule — the same pattern IDF uses for a custom-named partition in
# examples/protocols/mqtt/ssl_ds. `idf.py flash` now writes bootloader +
# partition table + ota_0 + this bank; OTA never touches the bank, which is what
# keeps an OTA payload down to code. Mapped at boot by main/art/art_bank.c
# (esp_partition_mmap, DROM window).
#
# Must live in this file, after project(): esptool_py_flash_target_image() and
# partition_table_get_partition_info() are only defined once the build system
# has been included.
# ---------------------------------------------------------------------------
set(_art_partition assets)
set(_art_image "${CMAKE_CURRENT_LIST_DIR}/assets/clock_assets.bin")

if(EXISTS "${_art_image}")
    # Fail at configure time rather than at boot: the partition size lives in
    # partitions.csv and the blob's true length is baked into
    # assets/clock_assets.h as CLK_BLOB_BYTES, so an oversized re-bake would
    # otherwise only surface as an art_bank_init() error on the serial log.
    # (partition_table's own check_sizes.py can't be used here: it measures
    # against the SMALLEST partition of a type, and `assets` shares the `data`
    # type with the 32 KB `nvs`.)
    partition_table_get_partition_info(_art_offset "--partition-name ${_art_partition}" "offset")
    partition_table_get_partition_info(_art_size   "--partition-name ${_art_partition}" "size")
    file(SIZE "${_art_image}" _art_bytes)
    if(_art_bytes GREATER _art_size)
        message(FATAL_ERROR
            "assets/clock_assets.bin is ${_art_bytes} bytes but the `${_art_partition}` "
            "partition in partitions.csv is only ${_art_size} bytes. Re-bake a smaller "
            "theme (fewer frames) or enlarge the partition.")
    endif()

    # esptool_py_flash_target_image() wants an ABSOLUTE path: it runs its own
    # file(RELATIVE_PATH ... ${BUILD_DIR}) on the argument
    # (esptool_py/project_include.cmake:343) so the path stored in
    # flash_args is build-dir-relative for `idf.py flash`.
    esptool_py_flash_target_image(flash "${_art_partition}" "${_art_offset}" "${_art_image}")
    message(STATUS "art bank: ${_art_bytes} B -> `${_art_partition}` @ ${_art_offset} "
                   "(${_art_size} B partition)")
else()
    message(STATUS "assets/clock_assets.bin absent — bake a theme with "
                   "tools/bake_clock.py before flashing the art bank")
endif()
