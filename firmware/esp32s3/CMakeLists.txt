cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.3.0")
# The application opens its encrypted NVS partitions (including NimBLE bond
# storage when enabled) with explicit,
# manufacturing-provisioned keys. Do not link the default provider constructor:
# its HMAC fallback can generate an eFuse key when default nvs_flash_init is
# called. No application component uses that automatic initialization API.
set(EXCLUDE_COMPONENTS nvs_sec_provider)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gw32_esp32s3)
