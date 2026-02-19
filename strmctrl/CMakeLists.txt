# ---------------------------------------------------------------------------
# strmctrl — core library
# ---------------------------------------------------------------------------
# Builds a static library that bundles all transport, codec and facade layers.
# Link against: strmctrl (which transitively pulls in ixwebsocket + FFmpeg).
# ---------------------------------------------------------------------------

add_library(strmctrl STATIC
    # Core data structures (header-only, listed for IDE visibility)
    core/Message.h
    core/VideoFrame.h
    core/Callbacks.h

    # Codec layer
    codec/CodecConfig.h
    codec/VideoEncoder.h
    codec/VideoEncoder.cpp
    codec/VideoDecoder.h
    codec/VideoDecoder.cpp

    # Transport layer
    transport/SignalingChannel.h
    transport/SignalingChannel.cpp
    transport/RtpSender.h
    transport/RtpSender.cpp
    transport/RtpReceiver.h
    transport/RtpReceiver.cpp

    # Facade layer
    Master.h
    Master.cpp
    Slave.h
    Slave.cpp
)

# Public include root: consumers can use #include <strmctrl/Master.h> etc.
target_include_directories(strmctrl PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Link dependencies
target_link_libraries(strmctrl PUBLIC
    ixwebsocket
    ffmpeg::ffmpeg
)

# Require C++17
target_compile_features(strmctrl PUBLIC cxx_std_17)

# Windows: suppress min/max macro conflicts from <windows.h>
if (WIN32)
    target_compile_definitions(strmctrl PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
