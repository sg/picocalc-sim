# Writes OUT defining picocalc_sim_version from `git describe` of SRC, or from VERSION when given;
# run at every build so a new commit or tag shows without reconfiguring.
#   cmake -DSRC=dir -DOUT=file.c [-DVERSION=v1.2.3] -P Version.cmake
if(NOT VERSION)
    execute_process(COMMAND git describe --tags --match "v[0-9]*" --dirty --always
        WORKING_DIRECTORY ${SRC}
        OUTPUT_VARIABLE VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE failed ERROR_QUIET)
    if(failed OR NOT VERSION)
        set(VERSION "unknown")
    endif()
endif()
set(content "const char picocalc_sim_version[] = \"${VERSION}\";\n")
if(EXISTS ${OUT})
    file(READ ${OUT} old)
endif()
# Rewriting an unchanged file would recompile it and relink on every build.
if(NOT old STREQUAL content)
    file(WRITE ${OUT} "${content}")
endif()
