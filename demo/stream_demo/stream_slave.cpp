/**
 * @file stream_slave.cpp
 * @brief stream_demo 从端示例程序。
 *
 * 功能演示：
 *   1. 连接主端的信令通道（WebSocket）。
 *   2. 自动完成 SDP 协商并启动 RTP 视频流接收与解码。
 *   3. 通过 callback 接收解码后的视频帧，打印帧信息。
 *   4. 接收来自主端的文本消息，并可从控制台向主端发送消息。
 *
 * 用法：
 *   stream_slave.exe <主端IP> [信令端口=11451] [RTP接收端口=11452]
 *
 * 示例：
 *   stream_slave.exe 192.168.1.100
 *   stream_slave.exe 192.168.1.100 11451 11452
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>

#include "strmctrl/Slave.h"
#include "strmctrl/core/VideoFrame.h"

// ---------------------------------------------------------------------------
// 帧信息打印辅助
// ---------------------------------------------------------------------------
static void printFrameInfo(const strmctrl::VideoFrame& frame)
{
    std::cout << "[Slave] Frame: "
              << frame.width() << "x" << frame.height()
              << " pts=" << frame.pts()
              << " fmt=" << static_cast<int>(frame.format())
              << "\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: stream_slave.exe <master_host> "
                     "[signaling_port=11451] [rtp_port=11452]\n";
        return 1;
    }

    const std::string master_host = argv[1];
    const int sig_port  = (argc >= 3) ? std::stoi(argv[2]) : 11451;
    const int rtp_port  = (argc >= 4) ? std::stoi(argv[3]) : 11452;

    // 初始化 Windows 网络
    ix::initNetSystem();

    // -----------------------------------------------------------------------
    // 帧队列（生产者-消费者）
    // 视频帧 callback 尽快返回，将帧 clone 后入队；
    // 消费线程负责处理（此 demo 中仅打印信息）。
    // -----------------------------------------------------------------------
    std::queue<strmctrl::VideoFrame> frame_queue;
    std::mutex                       queue_mutex;
    std::condition_variable          queue_cv;
    std::atomic_bool                 running{true};

    // -----------------------------------------------------------------------
    // 配置 Slave
    // -----------------------------------------------------------------------
    strmctrl::Slave slave;

    slave.setMessageCallback([](const strmctrl::TextMessage& msg) {
        std::cout << "[Master " << msg.sender_id << "] " << msg.text << "\n";
    });

    slave.setConnectionCallback([&](bool connected, const std::string& info) {
        std::cout << "[Slave] "
                  << (connected ? "Connected to master: " : "Disconnected: ")
                  << info << "\n";
        if (!connected) {
            // master 断开后停止消费循环，让主线程正常退出
            running.store(false);
            queue_cv.notify_all();
        }
    });

    // 视频帧 callback：快速 clone 后入队
    slave.setVideoFrameCallback(
        [&](const strmctrl::VideoFrame& frame) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            frame_queue.push(frame.clone());
            queue_cv.notify_one();
        }
    );

    // -----------------------------------------------------------------------
    // 连接主端
    // -----------------------------------------------------------------------
    if (!slave.connect(master_host, sig_port, rtp_port)) {
        std::cerr << "[Slave] connect failed: " << slave.lastError() << "\n";
        ix::uninitNetSystem();
        return 1;
    }
    std::cout << "[Slave] Connecting to master at "
              << master_host << ":" << sig_port << " ...\n"
              << "[Slave] Type a message and press Enter to send; 'quit' to exit.\n";

    // -----------------------------------------------------------------------
    // 帧消费线程：从队列取出帧并处理
    // -----------------------------------------------------------------------
    std::thread consumer_thread([&]() {
        int64_t frame_count = 0;
        while (running.load()) {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait_for(lock, std::chrono::milliseconds(200),
                              [&]{ return !frame_queue.empty() || !running.load(); });

            while (!frame_queue.empty()) {
                strmctrl::VideoFrame f = std::move(frame_queue.front());
                frame_queue.pop();
                lock.unlock();

                ++frame_count;
                // 每 30 帧打印一次，避免刷屏
                if (frame_count % 30 == 1) printFrameInfo(f);

                lock.lock();
            }
        }
    });

    // -----------------------------------------------------------------------
    // 控制台输入线程：仅负责发消息，EOF 不影响 slave 生命周期
    // -----------------------------------------------------------------------
    std::atomic_bool stop_input{false};
    std::thread input_thread([&]() {
        std::string line;
        while (!stop_input.load() && std::getline(std::cin, line)) {
            if (line == "quit") {
                running.store(false);
                queue_cv.notify_all();
                break;
            }
            if (!slave.sendMessage(line)) {
                std::cerr << "[Slave] sendMessage failed: " << slave.lastError() << "\n";
            }
        }
        // stdin EOF 时什么都不做，slave 继续运行
    });

    // -----------------------------------------------------------------------
    // 主线程：等待帧消费线程结束（视频流关闭 / quit）
    // -----------------------------------------------------------------------
    if (consumer_thread.joinable()) consumer_thread.join();

    // -----------------------------------------------------------------------
    // 清理
    // -----------------------------------------------------------------------
    stop_input.store(true);
    if (input_thread.joinable()) input_thread.detach();

    slave.disconnect();
    ix::uninitNetSystem();

    std::cout << "[Slave] Exited cleanly.\n";
    return 0;
}
