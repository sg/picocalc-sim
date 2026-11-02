cmake_minimum_required(VERSION 3.16)
project(picocalc_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(FATFS_DIR "" CACHE PATH "FatFs sources (ff.c); found or fetched when empty")
option(PCSIM_SDL "Build the SDL2 window frontend" ON)
option(PCSIM_FETCH_SDL "Download and build SDL2 when it is not installed" ON)
set(PCSIM_SDL_SHA256 "24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4" CACHE STRING "SHA-256 of the SDL2 release tarball")

find_package(Threads REQUIRED)
include(cmake/Dependencies.cmake)

add_executable(pcsim
    emu/chip.c
    emu/cpu.c
    emu/dcp.c
    emu/devices.c
    emu/dma.c
    emu/dw_i2c.c
    emu/gpio.c
    emu/machine.c
    emu/pio.c
    emu/pl011.c
    emu/pl022.c
    emu/pwm.c
    emu/qmi.c
    emu/rom.c
    emu/sio.c
    emu/sys.c
    emu/timer.c
    emu/uf2.c
    sim/src/runtime.c
    sim/devices/picocalc_board.c
    sim/devices/sdcard.c
    sim/devices/southbridge.c
    sim/devices/st7365p.c
    frontend/keys.c
    frontend/main.c
    frontend/png.c
    frontend/script.c
    frontend/sdl.c)
target_include_directories(pcsim PRIVATE emu sim/include)
target_compile_options(pcsim PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(pcsim PRIVATE Threads::Threads m)
if(PCSIM_HAVE_SDL)
    target_compile_definitions(pcsim PRIVATE PCSIM_HAVE_SDL=1)
    target_link_libraries(pcsim PRIVATE ${PCSIM_SDL_TARGET})
endif()

add_subdirectory(tools/mkcard)
