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
