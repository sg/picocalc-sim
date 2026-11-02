# SDL2 for the window, FatFs for building card images.

include(FetchContent)

set(PCSIM_HAVE_SDL OFF)
if(PCSIM_SDL)
    find_package(SDL2 CONFIG QUIET)
    if(TARGET SDL2::SDL2)
        set(PCSIM_SDL_TARGET SDL2::SDL2)
        set(PCSIM_HAVE_SDL ON)
        message(STATUS "pcsim: using installed SDL2 ${SDL2_VERSION}")
    elseif(TARGET SDL2::SDL2-static)
        set(PCSIM_SDL_TARGET SDL2::SDL2-static)
        set(PCSIM_HAVE_SDL ON)
        message(STATUS "pcsim: using installed static SDL2 ${SDL2_VERSION}")
    elseif(PCSIM_FETCH_SDL)
        message(STATUS "pcsim: SDL2 not installed; building SDL 2.30.9 from source")
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
        set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
        set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL2
            URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
            URL_HASH SHA256=${PCSIM_SDL_SHA256}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(SDL2)
        set(PCSIM_SDL_TARGET SDL2::SDL2-static)
        set(PCSIM_HAVE_SDL ON)
    else()
        message(STATUS "pcsim: SDL2 not found; building headless only")
    endif()
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
message(STATUS "pcsim: FatFs from ${FATFS_DIR}")
