cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gw32_nrf54l15 VERSION 0.3.0 LANGUAGES C)

target_sources(app PRIVATE
  src/main.c src/gw_app.c src/gw_core.c src/gw_filter_v2.c src/gw_radio.c src/gw_gatt.c
  src/gw_peripheral.c src/gw_dfu.c)
target_include_directories(app PRIVATE src)
target_compile_options(app PRIVATE -Wall -Wextra)
