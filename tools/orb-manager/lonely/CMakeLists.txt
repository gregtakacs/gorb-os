cmake_minimum_required(VERSION 3.16)

# The lonely image: what a wiped orb becomes instead of a boot-loop.
# Deliberately NOT a gorb-os build — no WiFi, no BLE, no NVS, no LVGL, no
# serial command door. It boots the panel, animates, and nothing else, so the
# hub can still see it as a blank-ish Espressif board and New Orb over it.

set(EXTRA_COMPONENT_DIRS components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lonely)