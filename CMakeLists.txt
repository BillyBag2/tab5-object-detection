cmake_minimum_required(VERSION 3.22)
set(IDF_TARGET "esp32p4" CACHE STRING "Tab5 uses ESP32-P4")
set(SDKCONFIG_DEFAULTS "sdkconfig.default")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_object_detection)

include(cmake/optimization.cmake)
# Run after component configuration, including deferred ESP-DL setup, so
# component-specific -Os/-O2 settings cannot silently override the project policy.
cmake_language(DEFER CALL tab5_optimize_sources "${CMAKE_CURRENT_SOURCE_DIR}")
