# SDL3 and Dear ImGui for the window, FatFs for building card images.

include(FetchContent)

set(PICOCALC_SIM_HAVE_SDL OFF)
if(PICOCALC_SIM_SDL)
    find_package(SDL3 CONFIG QUIET)
    if(TARGET SDL3::SDL3)
        set(PICOCALC_SIM_SDL_TARGET SDL3::SDL3)
        set(PICOCALC_SIM_HAVE_SDL ON)
        message(STATUS "picocalc-sim: using installed SDL3 ${SDL3_VERSION}")
    elseif(TARGET SDL3::SDL3-static)
        set(PICOCALC_SIM_SDL_TARGET SDL3::SDL3-static)
        set(PICOCALC_SIM_HAVE_SDL ON)
        message(STATUS "picocalc-sim: using installed static SDL3 ${SDL3_VERSION}")
    elseif(PICOCALC_SIM_FETCH_SDL)
        message(STATUS "picocalc-sim: SDL3 not installed; building SDL 3.4.18 from source")
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
        # nothing in picocalc-sim uses these, and leaving them out saves building them
        foreach(subsystem CAMERA HAPTIC HIDAPI JOYSTICK SENSOR)
            set(SDL_${subsystem} OFF CACHE BOOL "" FORCE)
        endforeach()
        FetchContent_Declare(SDL3
            URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
            URL_HASH SHA256=${PICOCALC_SIM_SDL3_SHA256}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(SDL3)
        set(PICOCALC_SIM_SDL_TARGET SDL3::SDL3-static)
        set(PICOCALC_SIM_HAVE_SDL ON)
    else()
        message(STATUS "picocalc-sim: SDL3 not found; building headless only")
    endif()
endif()

if(PICOCALC_SIM_HAVE_SDL)
    enable_language(CXX)
    if(NOT IMGUI_DIR)
        FetchContent_Declare(imgui
            URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
            URL_HASH SHA256=${PICOCALC_SIM_IMGUI_SHA256}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(imgui)
        set(IMGUI_DIR ${imgui_SOURCE_DIR})
    endif()
    message(STATUS "picocalc-sim: Dear ImGui from ${IMGUI_DIR}")
    add_library(picocalc_sim_imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(picocalc_sim_imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_compile_definitions(picocalc_sim_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_compile_features(picocalc_sim_imgui PUBLIC cxx_std_17)
    target_link_libraries(picocalc_sim_imgui PUBLIC ${PICOCALC_SIM_SDL_TARGET})
endif()

# Prefer a FatFs already checked out next to the firmware projects; fetch it otherwise.
if(NOT FATFS_DIR)
    foreach(candidate
            ${CMAKE_SOURCE_DIR}/../ftpserver/submodules/pico_fatfs/fatfs
            ${CMAKE_SOURCE_DIR}/../picocalc_lua/submodules/pico_fatfs/fatfs
            ${CMAKE_SOURCE_DIR}/../picocalc-z80/submodules/pico_fatfs/fatfs)
        if(EXISTS ${candidate}/ff.c)
            get_filename_component(FATFS_DIR ${candidate} ABSOLUTE)
            break()
        endif()
    endforeach()
endif()
if(NOT FATFS_DIR)
    FetchContent_Declare(pico_fatfs
        GIT_REPOSITORY https://github.com/elehobica/pico_fatfs.git
        GIT_TAG e0e512bcd8f0ddebafb6708c9dd1bc49b3f1e484)
    FetchContent_Populate(pico_fatfs)
    set(FATFS_DIR ${pico_fatfs_SOURCE_DIR}/fatfs)
endif()
message(STATUS "picocalc-sim: FatFs from ${FATFS_DIR}")

# lwIP terminates the emulated WiFi's TCP connections. The Pico SDK carries the release the
# firmware itself uses; a checkout next to this one (or PICO_SDK_PATH) saves fetching it.
if(NOT LWIP_DIR)
    foreach(candidate
            ${CMAKE_SOURCE_DIR}/../pico-sdk/lib/lwip
            $ENV{PICO_SDK_PATH}/lib/lwip)
        if(EXISTS ${candidate}/src/core/tcp.c)
            get_filename_component(LWIP_DIR ${candidate} ABSOLUTE)
            break()
        endif()
    endforeach()
endif()
if(NOT LWIP_DIR)
    FetchContent_Declare(lwip
        GIT_REPOSITORY https://github.com/lwip-tcpip/lwip.git
        GIT_TAG 77dcd25a72509eb83f72b033d219b1d40cd8eb95)
    FetchContent_Populate(lwip)
    set(LWIP_DIR ${lwip_SOURCE_DIR})
endif()
message(STATUS "picocalc-sim: lwIP from ${LWIP_DIR}")
