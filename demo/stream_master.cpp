#include "strmctrl/StrmCtrl.h"

#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cout << "Usage: stream_master <peer_ip> <input_file>" << std::endl;
        return 1;
    }

    strmctrl::EndpointConfig config;
    config.peer_host = argv[1];

    strmctrl::FileStreamConfig file_cfg;
    file_cfg.input_path = argv[2];
    file_cfg.loop = true;

    strmctrl::CodecConfig codec;
    codec.codec = strmctrl::VideoCodec::H264;
    // Prefer automatic selection so CMake/FFmpeg-provided encoders (eg. libx264)
    // are used when available. Explicitly selecting OpenH264 can force the
    // use of libopenh264 which may not accept the chosen params.
    codec.backend = strmctrl::EncoderBackend::Auto;
    codec.width = 1920;  // Match input resolution
    codec.height = 1080;
    // Match source frame rate more closely (input is 59.94). Using 60
    // avoids excessive resampling and keeps encoder timing stable.
    codec.fps = 60;
    codec.bitrate = 2'000'000;

    strmctrl::MessageCallbacks callbacks;
    callbacks.on_text = [](const std::string& msg) {
        std::cout << "[slave] " << msg << std::endl;
    };
    callbacks.on_error = [](const std::string& err) {
        std::cerr << "[error] " << err << std::endl;
    };

    strmctrl::MasterEndpoint master;
    if (!master.start(config, callbacks)) {
        std::cerr << "Failed to start master." << std::endl;
        return 1;
    }
    if (!master.start_file_stream(file_cfg, codec)) {
        std::cerr << "Failed to start file streaming." << std::endl;
        return 1;
    }

    std::cout << "Master started. Type text to send. 'quit' to exit." << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit") break;
        master.send_text(line);
    }

    master.stop();
    return 0;
}
