#include "strmctrl/StrmCtrl.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cout << "Usage: stream_slave <master_ip>" << std::endl;
        return 1;
    }

    strmctrl::EndpointConfig config;
    config.peer_host = argv[1];

    std::atomic<int> frame_count{0};
    std::atomic<bool> got_first_frame{false};

    strmctrl::MessageCallbacks callbacks;
    callbacks.on_text = [](const std::string& msg) {
        std::cout << "[master] " << msg << std::endl;
    };
    callbacks.on_frame = [&frame_count, &got_first_frame](const strmctrl::VideoFrame& frame) {
        int count = ++frame_count;
        if (!got_first_frame.exchange(true)) {
            std::cout << "[frame] First frame received: " << frame.width << "x" << frame.height
                      << " pts=" << frame.pts << std::endl;
        }
        // Periodic log every 30 frames for quieter output
        if (count % 30 == 0) {
            std::cout << "[frame] " << frame.width << "x" << frame.height
                      << " pts=" << frame.pts << " total=" << count << std::endl;
        }
    };
    callbacks.on_error = [](const std::string& err) {
        std::cerr << "[error] " << err << std::endl;
    };

    strmctrl::SlaveEndpoint slave;
    if (!slave.start(config, callbacks)) {
        std::cerr << "Failed to start slave." << std::endl;
        return 1;
    }

    std::cout << "Slave started. Type text to send. 'quit' to exit." << std::endl;

    // Monitor thread: prints FPS/health every second
    std::atomic<bool> stop_monitor{false};
    std::thread monitor([&]() {
        int last_count = 0;
        int zero_count_secs = 0;
        while (!stop_monitor.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            int total = frame_count.load();
            int fps = total - last_count;
            last_count = total;
            if (fps == 0) {
                ++zero_count_secs;
            } else {
                zero_count_secs = 0;
            }
            std::cout << "[stats] total_frames=" << total << " fps=" << fps;
            if (got_first_frame.load()) std::cout << " first_frame=true";
            std::cout << std::endl;

            if (zero_count_secs >= 3 && !got_first_frame.load()) {
                std::cerr << "[warning] no frames received for 3+ seconds" << std::endl;
            }
        }
    });

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit") break;
        slave.send_text(line);
    }

    stop_monitor = true;
    if (monitor.joinable()) monitor.join();

    slave.stop();
    return 0;
}
