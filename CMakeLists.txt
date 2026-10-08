cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# The clock's GIF delta mode invalidates ~16 background rects plus the hands'
# swept wedges in one tick. LVGL's default invalid-area table is 32 entries;
# when a tick fills it, lv_refr.c degrades to a WHOLE-SCREEN invalidate
# (~145 ms/flush — defeating the delta packing entirely). Grow the table so
# partial-render stays partial. (LVGL 9.6 has no Kconfig entry for this and
# guards it with #ifndef, so a global -D is the supported override.)
add_compile_definitions(LV_INV_BUF_SIZE=96)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(orb-os-esp-idf)
