cmake_minimum_required(VERSION 3.14)
project(StrmCtrl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Export compile_commands.json for IDEs (helps clangd / VS Code C/C++ tools)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Configure IXWebSocket options for minimal build (no SSL/ZLIB requirement)
option(USE_TLS "Build with TLS support" OFF)
option(USE_ZLIB "Build with ZLIB support" OFF)

# ---------------------------------------------------------------------------
# FFmpeg — LGPL shared build (headers + import libs under 3rdparty/ffmpeg)
# ---------------------------------------------------------------------------
set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg")
set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
set(FFMPEG_LIB_DIR     "${FFMPEG_ROOT}/lib")

# Helper macro: create one IMPORTED shared library target per FFmpeg component
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

# Convenience interface target that bundles all FFmpeg components
add_library(ffmpeg::ffmpeg INTERFACE IMPORTED GLOBAL)
target_link_libraries(ffmpeg::ffmpeg INTERFACE
    ffmpeg::avcodec
    ffmpeg::avformat
    ffmpeg::avutil
    ffmpeg::swresample
    ffmpeg::swscale
)

# ---------------------------------------------------------------------------
# Third-party & project subdirectories
# ---------------------------------------------------------------------------

# Add IXWebSocket submodule
add_subdirectory(3rdparty/IXWebSocket)

# Core StrmCtrl library
add_subdirectory(strmctrl)

# Demo programs
add_subdirectory(demo)

# Make compile_commands.json available at project root for editors that
# look for it there (this target copies from build/ when you run a build).
add_custom_target(export_compile_commands ALL
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_BINARY_DIR}/compile_commands.json
		${CMAKE_SOURCE_DIR}/compile_commands.json
	COMMENT "Copying compile_commands.json to project root for IDEs"
)
