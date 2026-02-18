cmake_minimum_required(VERSION 3.14)
project(StrmCtrl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Export compile_commands.json for IDEs (helps clangd / VS Code C/C++ tools)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Configure IXWebSocket options for minimal build (no SSL/ZLIB requirement)
option(USE_TLS "Build with TLS support" OFF)
option(USE_ZLIB "Build with ZLIB support" OFF)

# Add IXWebSocket submodule
add_subdirectory(3rdparty/IXWebSocket)

# FFmpeg (shared) discovery
set(FFMPEG_ROOT "" CACHE PATH "Root directory of FFmpeg (optional)")
set(FFMPEG_INCLUDE_DIR "" CACHE PATH "FFmpeg include directory (optional)")
set(FFMPEG_LIB_DIR "" CACHE PATH "FFmpeg library directory (optional)")

# If the repo contains a bundled ffmpeg under 3rdparty/ffmpeg, prefer it by default
if (EXISTS "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg")
    # If the packaged FFmpeg tree exists, set it as the default FFMPEG_ROOT
    # in the CMake cache only when the user hasn't provided one. This makes
    # passing -DFFMPEG_ROOT on the command line unnecessary for the common
    # case where the repo already contains a usable ffmpeg/ include+lib tree.
    if (NOT DEFINED FFMPEG_ROOT OR FFMPEG_ROOT STREQUAL "")
        set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg" CACHE PATH "Root directory of FFmpeg (optional)")
    endif()
endif()

if (FFMPEG_ROOT)
    if (NOT FFMPEG_INCLUDE_DIR)
        set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
    endif()
    if (NOT FFMPEG_LIB_DIR)
        set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib")
    endif()
endif()

find_path(FFMPEG_INCLUDE_DIR
    NAMES libavcodec/avcodec.h
    PATHS ${FFMPEG_INCLUDE_DIR}
    HINTS ${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg
)

# If the repository ships FFmpeg sources/headers under 3rdparty/ffmpeg (common
# for development), accept that layout as an include root so editors and IDEs
# can resolve headers even when a prebuilt "include/" tree is not present.
if (NOT FFMPEG_INCLUDE_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/libavcodec/avcodec.h")
    set(FFMPEG_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg")
    message(STATUS "Found bundled FFmpeg headers at ${FFMPEG_INCLUDE_DIR}")
endif()
# try locate FFmpeg libraries in configured lib dirs (or bundled lib/)
find_library(AVCODEC_LIB avcodec PATHS ${FFMPEG_LIB_DIR} ${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/lib)
find_library(AVFORMAT_LIB avformat PATHS ${FFMPEG_LIB_DIR} ${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/lib)
find_library(AVUTIL_LIB avutil PATHS ${FFMPEG_LIB_DIR} ${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/lib)
find_library(SWSCALE_LIB swscale PATHS ${FFMPEG_LIB_DIR} ${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/lib)

# Compose list of found FFmpeg libs
set(FFMPEG_LIBS "")
if (AVCODEC_LIB)
    list(APPEND FFMPEG_LIBS ${AVCODEC_LIB})
else()
    message(WARNING "avcodec not found; FFmpeg features will be unavailable — 'strmctrl' and related demos will be skipped")
endif()
if (AVFORMAT_LIB)
    list(APPEND FFMPEG_LIBS ${AVFORMAT_LIB})
endif()
if (AVUTIL_LIB)
    list(APPEND FFMPEG_LIBS ${AVUTIL_LIB})
endif()
if (SWSCALE_LIB)
    list(APPEND FFMPEG_LIBS ${SWSCALE_LIB})
endif()

if (AVCODEC_LIB AND AVFORMAT_LIB AND AVUTIL_LIB)
    add_library(strmctrl STATIC
        src/StrmCtrl.cpp
    )

    target_include_directories(strmctrl PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    if (FFMPEG_INCLUDE_DIR)
        target_include_directories(strmctrl PUBLIC ${FFMPEG_INCLUDE_DIR})
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/include")
        target_include_directories(strmctrl PUBLIC "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/include")
    endif()

    if (EXISTS "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/lib")
        # Prefer bundled ffmpeg lib dir for linking
        link_directories("${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/lib")
    endif()

    target_link_libraries(strmctrl PUBLIC ixwebsocket ${FFMPEG_LIBS})
    if (WIN32)
        target_link_libraries(strmctrl PUBLIC ws2_32)
    endif()
else()
    message(STATUS "Skipping 'strmctrl' library: required FFmpeg libraries not found.")
endif()

# Add demo directory
add_subdirectory(demo)

# Make compile_commands.json available at project root for editors that
# look for it there (this target copies from build/ when you run a build).
add_custom_target(export_compile_commands ALL
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_BINARY_DIR}/compile_commands.json
		${CMAKE_SOURCE_DIR}/compile_commands.json
	COMMENT "Copying compile_commands.json to project root for IDEs"
)
