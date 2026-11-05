add_executable(picocalc-sim-mkcard
    mkcard.c
    walk.c
    ${FATFS_DIR}/ff.c
    ${FATFS_DIR}/ffunicode.c)
# This directory comes first so ff.c picks up the local ffconf.h.
target_include_directories(picocalc-sim-mkcard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FATFS_DIR})
set_source_files_properties(${FATFS_DIR}/ff.c ${FATFS_DIR}/ffunicode.c PROPERTIES COMPILE_OPTIONS "-w")
