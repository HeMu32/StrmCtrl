add_executable(demo_master master.cpp)
target_link_libraries(demo_master PRIVATE ixwebsocket)

add_executable(demo_slave slave.cpp)
target_link_libraries(demo_slave PRIVATE ixwebsocket)

# Copy headers to build directory if needed, but not strictly required for running since we use direct path in subdirectories
# However, ensure that runtime DLLs (if any) are available. IXWebSocket is usually static by default but check.
