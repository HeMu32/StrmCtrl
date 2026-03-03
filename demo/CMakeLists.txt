# ---------------------------------------------------------------------------
# Legacy WebSocket-only demos (IXWebSocket smoke-test)
# ---------------------------------------------------------------------------
add_executable(demo_master master.cpp)
target_link_libraries(demo_master PRIVATE ixwebsocket)

add_executable(demo_slave slave.cpp)
target_link_libraries(demo_slave PRIVATE ixwebsocket)

# ---------------------------------------------------------------------------
# stream_demo — full feature demo (strmctrl: WebSocket + RTP + FFmpeg)
# ---------------------------------------------------------------------------
add_executable(stream_master stream_demo/stream_master.cpp)
target_link_libraries(stream_master PRIVATE strmctrl)

add_executable(stream_slave stream_demo/stream_slave.cpp)
target_link_libraries(stream_slave PRIVATE strmctrl)

# ---------------------------------------------------------------------------
# decode_test — local-only FFmpeg decode sanity check
# ---------------------------------------------------------------------------
add_executable(decode_test decode_test.cpp)
target_link_libraries(decode_test PRIVATE ffmpeg::ffmpeg)

# ---------------------------------------------------------------------------
# transcode_h264 — local H.264 -> H.264 (libopenh264) transcode demo
# ---------------------------------------------------------------------------
add_executable(transcode_h264 transcode_h264.cpp)
target_link_libraries(transcode_h264 PRIVATE ffmpeg::ffmpeg)
