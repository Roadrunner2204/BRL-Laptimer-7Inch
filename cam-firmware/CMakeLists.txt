# BRL Laptimer Camera Module — ESP-IDF top-level project
#
# Target hardware: DFRobot FireBeetle 2 ESP32-P4 (DFR1172)
#   + Raspberry Pi Camera v1.3 (OV5647) on the MIPI-CSI port
#   + microSD on the onboard SDIO slot
#   + onboard ESP32-C6 (WiFi 6 / BT) used as a STA on the laptimer's AP
#
# Build:  idf.py set-target esp32p4 && idf.py build
#
# Note: this is a SEPARATE ESP-IDF project from the main laptimer firmware
# in ../main/. Switch IDF_TARGET / build dir before flashing the cam.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(brl_laptimer_cam)
