# The ESP32-S3-BOX's board support: box_hal.h and the debug console.
idf_component_register(SRCS "box_hal_esp32.c" "debugcon.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos esp_timer heap
                       PRIV_REQUIRES esp_driver_i2s esp_driver_i2c esp_driver_gpio
                                     esp_driver_spi esp_lcd esp_driver_usb_serial_jtag
                                     esp_driver_uart vfs)
# QEMU has no USB-Serial/JTAG: the debug console on UART0 instead.
if(DEFINED ENV{DEBUGCON_UART})
  target_compile_definitions(${COMPONENT_LIB} PRIVATE DEBUGCON_UART)
endif()
