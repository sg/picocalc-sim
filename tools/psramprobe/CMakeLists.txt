# Firmware for a real PicoCalc, built with the Pico SDK on its own: cmake -S . -B build -DPICO_BOARD=pico2
cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(psramprobe C CXX ASM)
pico_sdk_init()

add_executable(psramprobe
    main.c
    drivers/audio.c
    drivers/clib.c
    drivers/display.c
    drivers/fat32.c
    drivers/font-5x10.c
    drivers/font-8x10.c
    drivers/keyboard.c
    drivers/lcd.c
    drivers/picocalc.c
    drivers/sdcard.c
    drivers/southbridge.c
)
target_include_directories(psramprobe PRIVATE ${CMAKE_CURRENT_LIST_DIR})
pico_generate_pio_header(psramprobe ${CMAKE_CURRENT_LIST_DIR}/probe.pio)
pico_generate_pio_header(psramprobe ${CMAKE_CURRENT_LIST_DIR}/psram_spi.pio)
pico_generate_pio_header(psramprobe ${CMAKE_CURRENT_LIST_DIR}/drivers/audio.pio)
# Runs from SRAM, so the flash never sees the higher system clocks.
pico_set_binary_type(psramprobe copy_to_ram)
pico_set_program_name(psramprobe "psramprobe")
pico_set_program_version(psramprobe "1")
pico_enable_stdio_uart(psramprobe 0)
pico_enable_stdio_usb(psramprobe 0)
target_compile_options(psramprobe PRIVATE -Wall)
target_link_libraries(psramprobe pico_stdlib pico_rand hardware_clocks hardware_dma hardware_i2c
    hardware_pio hardware_spi hardware_vreg)
pico_add_extra_outputs(psramprobe)
