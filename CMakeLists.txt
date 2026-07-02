# Top-level CMakeLists for the ESP-IDF source-rebuild path (triggered by
# custom_sdkconfig in platformio.ini, used to move mbedTLS memory into PSRAM).
#
# This mirrors the template the pioarduino toolchain would otherwise auto-generate,
# but adds EXCLUDE_COMPONENTS to drop esp_https_server: it is an unused core component
# whose certificate-embed build step is broken in this toolchain and blocks the source
# rebuild (missing https_server.crt.S). We use the Arduino WebServer, not the IDF HTTPS
# server, so excluding it is safe. Must be set before project().
cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(EXCLUDE_COMPONENTS esp_https_server)
project(capsule-radar)
