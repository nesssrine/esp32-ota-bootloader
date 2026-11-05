cmake_minimum_required(VERSION 3.16)

# The application version is read from version.txt and embedded in the
# image (esp_app_desc_t), so the updater can identify every firmware.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_ota_bootloader)
