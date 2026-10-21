cmake_minimum_required(VERSION 3.20.0)

# Reuse the "halloween,gpio" binding from the stations app.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../stations)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(halloween_keypad_tester)

target_sources(app PRIVATE src/main.c)
