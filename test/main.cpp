#include <iostream>

extern "C" {
#include <libavutil/log.h>
}

#ifdef _WIN32
#include <winsock2.h>
#endif

extern void run_all_tests();

int main(int argc, char **argv) {
    std::cout << "Entering main" << std::endl;
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    // Disable FFmpeg spam during tests
    av_log_set_level(AV_LOG_QUIET);

    std::cout << "Calling run_all_tests()" << std::endl;
    run_all_tests();

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
