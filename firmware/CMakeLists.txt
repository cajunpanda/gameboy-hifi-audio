cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Project name is embedded in the app descriptor (esp_app_desc.project_name) and
# used as the OTA identity gate: ble_config.c refuses to boot an OTA image whose
# project_name doesn't match the running firmware's, so a stray/other ESP32 image
# pushed to the BLE OTA characteristic is rejected. Keep it distinctive.
project(gameboy-hifi-audio)
