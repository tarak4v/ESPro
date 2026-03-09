# minimp3 — public-domain header-only MP3 decoder by lieff
# Downloaded automatically from GitHub at CMake configure time.

set(MINIMP3_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(MINIMP3_HEADER ${MINIMP3_INCLUDE_DIR}/minimp3.h)

file(MAKE_DIRECTORY ${MINIMP3_INCLUDE_DIR})

if(NOT EXISTS ${MINIMP3_HEADER})
    message(STATUS "Downloading minimp3.h from GitHub ...")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h"
        ${MINIMP3_HEADER}
        TIMEOUT 30
        STATUS dl_status
    )
    list(GET dl_status 0 dl_code)
    if(NOT dl_code EQUAL 0)
        message(FATAL_ERROR "Failed to download minimp3.h: ${dl_status}")
    endif()
    message(STATUS "minimp3.h downloaded successfully")
endif()

idf_component_register(
    SRCS "minimp3_impl.c"
    INCLUDE_DIRS "include"
)

# Suppress warnings in third-party decoder code
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-all -Wno-extra -Wno-maybe-uninitialized)
