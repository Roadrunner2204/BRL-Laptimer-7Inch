# BRL Laptimer - ESP32-P4 Waveshare 7" Touch LCD
# Top-level ESP-IDF CMakeLists.txt

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(brl-laptimer)

# Suppress harmless IRAM section attribute warning in LVGL
# (GCC 15 in ESP-IDF v6.0 is stricter about .iram1.N subsection conflicts)
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-error=attributes)
