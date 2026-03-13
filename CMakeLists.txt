cmake_minimum_required(VERSION 3.14)
project(StrmCtrl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(STRMCTRL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(STRMCTRL_BUILD_DEMOS_DEFAULT ON)
    set(STRMCTRL_BUILD_TESTS_DEFAULT ON)
else()
    set(STRMCTRL_BUILD_DEMOS_DEFAULT OFF)
    set(STRMCTRL_BUILD_TESTS_DEFAULT OFF)
endif()

option(STRMCTRL_BUILD_DEMOS "Build StrmCtrl demo programs" ${STRMCTRL_BUILD_DEMOS_DEFAULT})
option(STRMCTRL_BUILD_TESTS "Build StrmCtrl tests" ${STRMCTRL_BUILD_TESTS_DEFAULT})

# Export compile_commands.json for IDEs (helps clangd / VS Code C/C++ tools)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Configure IXWebSocket options for minimal build (no SSL/ZLIB requirement)
option(USE_TLS "Build with TLS support" OFF)
option(USE_ZLIB "Build with ZLIB support" OFF)

# ---------------------------------------------------------------------------
# FFmpeg — LGPL shared build (headers + import libs under 3rdparty/ffmpeg)
# ---------------------------------------------------------------------------
if(NOT DEFINED FFMPEG_ROOT)
    if(DEFINED RSCA_FFMPEG_DIR)
        set(FFMPEG_ROOT "${RSCA_FFMPEG_DIR}")
    else()
        set(FFMPEG_ROOT "${STRMCTRL_ROOT}/3rdparty/ffmpeg")
    endif()
endif()

if(NOT DEFINED RSCA_IXWEBSOCKET_DIR)
    if(DEFINED RSCA_3RDPARTY_DIR AND EXISTS "${RSCA_3RDPARTY_DIR}/IXWebSocket/CMakeLists.txt")
        set(RSCA_IXWEBSOCKET_DIR "${RSCA_3RDPARTY_DIR}/IXWebSocket")
    else()
        set(RSCA_IXWEBSOCKET_DIR "${STRMCTRL_ROOT}/3rdparty/IXWebSocket")
    endif()
endif()

set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
set(FFMPEG_LIB_DIR     "${FFMPEG_ROOT}/lib")

if(NOT EXISTS "${RSCA_IXWEBSOCKET_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "[StrmCtrl] Missing required dependency: ${RSCA_IXWEBSOCKET_DIR}/CMakeLists.txt")
endif()

if(NOT TARGET ffmpeg::ffmpeg)
    # Helper macro: create one IMPORTED shared library target per FFmpeg component.
    macro(add_ffmpeg_lib TARGET_NAME LIB_STEM)
        add_library(${TARGET_NAME} SHARED IMPORTED GLOBAL)
        set_target_properties(${TARGET_NAME} PROPERTIES
            IMPORTED_IMPLIB   "${FFMPEG_LIB_DIR}/lib${LIB_STEM}.dll.a"
            INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
        )
    endmacro()

    add_ffmpeg_lib(ffmpeg::avcodec    avcodec)
    add_ffmpeg_lib(ffmpeg::avformat   avformat)
    add_ffmpeg_lib(ffmpeg::avutil     avutil)
    add_ffmpeg_lib(ffmpeg::swresample swresample)
    add_ffmpeg_lib(ffmpeg::swscale    swscale)

    # Convenience interface target that bundles all FFmpeg components.
    add_library(ffmpeg::ffmpeg INTERFACE IMPORTED GLOBAL)
    target_link_libraries(ffmpeg::ffmpeg INTERFACE
        ffmpeg::avcodec
        ffmpeg::avformat
        ffmpeg::avutil
        ffmpeg::swresample
        ffmpeg::swscale
    )
endif()

# ---------------------------------------------------------------------------
# Third-party & project subdirectories
# ---------------------------------------------------------------------------

# Add IXWebSocket submodule
add_subdirectory("${RSCA_IXWEBSOCKET_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/IXWebSocket")

# Core StrmCtrl library
add_subdirectory(strmctrl)

if(STRMCTRL_BUILD_DEMOS)
    add_subdirectory(demo)
endif()

if(STRMCTRL_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(test)
endif()

# Make compile_commands.json available at project root for editors that
# look for it there (this target copies from build/ when you run a build).
add_custom_target(export_compile_commands ALL
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_BINARY_DIR}/compile_commands.json
        ${STRMCTRL_ROOT}/compile_commands.json
	COMMENT "Copying compile_commands.json to project root for IDEs"
)
