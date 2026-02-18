add_executable(demo_master master.cpp)
target_link_libraries(demo_master PRIVATE ixwebsocket)

add_executable(demo_slave slave.cpp)
target_link_libraries(demo_slave PRIVATE ixwebsocket)

if (TARGET strmctrl)
    add_executable(stream_master stream_master.cpp)
    target_link_libraries(stream_master PRIVATE strmctrl)

    add_executable(stream_slave stream_slave.cpp)
    target_link_libraries(stream_slave PRIVATE strmctrl)
else()
    message(STATUS "Skipping stream_master/stream_slave demos because 'strmctrl' target is not available (missing FFmpeg).")
endif()

# Copy headers to build directory if needed, but not strictly required for running since we use direct path in subdirectories
# However, ensure that runtime DLLs (if any) are available. IXWebSocket is usually static by default but check.
