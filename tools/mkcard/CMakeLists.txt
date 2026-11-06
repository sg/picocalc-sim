add_executable(picocalc-sim-mkcard
    mkcard.c
    walk.c
    fatfs/ff.c
    fatfs/ffunicode.c)
# This directory comes first so ff.c picks up the local ffconf.h.
target_include_directories(picocalc-sim-mkcard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/fatfs)
set_source_files_properties(fatfs/ff.c fatfs/ffunicode.c PROPERTIES COMPILE_OPTIONS "-w")
