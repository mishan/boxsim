# The ESP32-S3-BOX's board support: box_hal.h and the debug console, on the
# BOX itself, or on the desktop in a window when the target is linux.
idf_build_get_property(target IDF_TARGET)

if(target STREQUAL "linux")
  idf_component_register(SRCS "sim/box_hal_sim.c" "sim/esp_timer_linux.c" "sim/freertos_linux.c"
                              "debugcon.c"
                         INCLUDE_DIRS "include"
                         REQUIRES freertos esp_timer heap)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  target_link_libraries(${COMPONENT_LIB} PRIVATE PkgConfig::SDL2 m)
  target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=xTaskCreatePinnedToCore")
else()
  idf_component_register(SRCS "box_hal_esp32.c" "debugcon.c"
                         INCLUDE_DIRS "include"
                         REQUIRES freertos esp_timer heap
                         PRIV_REQUIRES esp_driver_i2s esp_driver_i2c esp_driver_gpio
                                       esp_driver_spi esp_lcd esp_driver_usb_serial_jtag
                                       esp_driver_uart vfs)
endif()
# QEMU has no USB-Serial/JTAG: the debug console on UART0 instead.
if(DEFINED ENV{DEBUGCON_UART})
  target_compile_definitions(${COMPONENT_LIB} PRIVATE DEBUGCON_UART)
endif()
