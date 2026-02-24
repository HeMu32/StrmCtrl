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
 *   stream_slave.exe <主端IP> [信令端口=11451] [RTP接收端口段起点=11452]
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
#include "strmctrl/core/AudioFrame.h"

// ---------------------------------------------------------------------------
// 帧信息打印辅助
// ---------------------------------------------------------------------------
static void printVideoFrameInfo(const strmctrl::VideoFrame &frame)
{
    std::cout << "[Slave] Video Frame: "
              << frame.width() << "x" << frame.height()
              << " pts=" << frame.pts()
              << " fmt=" << static_cast<int>(frame.format())
              << "\n";
}

static void printAudioFrameInfo(const strmctrl::AudioFrame &frame)
{
    std::cout << "[Slave] Audio Frame: "
              << frame.nbSamples() << " samples"
              << " pts=" << frame.pts()
              << " fmt=" << static_cast<int>(frame.format())
              << " channels=" << frame.channels()
              << " sample_rate=" << frame.sampleRate()
              << "\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: stream_slave.exe <master_host> "
                     "[signaling_port=11451] [rtp_port=11452]\n";
        return 1;
    }

    const std::string master_host = argv[1];
    const int sig_port = (argc >= 3) ? std::stoi(argv[2]) : 11451;
    const int rtp_port = (argc >= 4) ? std::stoi(argv[3]) : 11452;

    // 初始化 Windows 网络
    ix::initNetSystem();

    // -----------------------------------------------------------------------
    // 帧队列（生产者-消费者）
    // -----------------------------------------------------------------------
    std::queue<strmctrl::VideoFrame> video_queue;
    std::queue<strmctrl::AudioFrame> audio_queue;
    std::mutex                       queue_mutex;
    std::condition_variable          queue_cv;
    std::atomic_bool                 running{true};

    // -----------------------------------------------------------------------
    // 配置 Slave
    // -----------------------------------------------------------------------
    strmctrl::Slave slave;

    // Request 1920x1080@30 from the master; this is a "suggested" config and
    // the master will reply with its chosen parameters (in our demo master it
    // is preconfigured to 1280x720@30, so the request will be ignored).
    strmctrl::VideoConfigRequest req;
    req.width  = 1920;
    req.height = 1080;
    req.fps    = 30;
    slave.setVideoConfigRequest(req);

    slave.setMessageCallback([](const strmctrl::TextMessage &msg)
                             {
                                 std::cout << "[Master] " << msg.text << "\n";
                             });

    slave.setConnectionCallback([&](bool connected, const std::string &info)
                                {
    std::cout << "[Slave] "
          << (connected ? "Connected to master: " : "Disconnected: ")
          << info << "\n";
        if (connected) {
            // no prompt
        }
        if (!connected) {
            running.store(false);
            queue_cv.notify_all();
        } });

    // 视频帧 callback
    slave.setVideoFrameCallback(
        [&](const strmctrl::VideoFrame &frame)
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            video_queue.push(frame.clone());
            queue_cv.notify_one();
        });

    // 音频帧 callback
    slave.setAudioFrameCallback(
        [&](const strmctrl::AudioFrame &frame)
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            audio_queue.push(frame.clone());
            queue_cv.notify_one();
        });

    // -----------------------------------------------------------------------
    // 连接主端
    // -----------------------------------------------------------------------
    if (!slave.connect(master_host, sig_port, rtp_port))
    {
        std::cerr << "[Slave] connect failed.\n";
        ix::uninitNetSystem();
        return 1;
    }
    std::cout << "[Slave] Connecting to master at "
              << master_host << ":" << sig_port << " ...\n"
              << "[Slave] Type a message and press Enter to send; 'quit' to exit.\n";

    // -----------------------------------------------------------------------
    // 帧消费线程：模拟音视频同步播放
    // -----------------------------------------------------------------------
    std::thread consumer_thread([&]()
    {
        int64_t v_count = 0;
        int64_t a_count = 0;
        while (running.load()) 
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait_for(lock, std::chrono::milliseconds(50),
                              [&]{ return !video_queue.empty() || !audio_queue.empty() || !running.load(); });

            while (!video_queue.empty()) 
            {
                strmctrl::VideoFrame f = std::move(video_queue.front());
                video_queue.pop();
                lock.unlock();

                ++v_count;
                if (v_count % 30 == 1) printVideoFrameInfo(f);

                lock.lock();
            }

            while (!audio_queue.empty()) 
            {
                strmctrl::AudioFrame f = std::move(audio_queue.front());
                audio_queue.pop();
                lock.unlock();

                ++a_count;
                if (a_count % 50 == 1) printAudioFrameInfo(f);

                lock.lock();
            }
        } 
    });

    // -----------------------------------------------------------------------
    // 控制台输入线程
    // -----------------------------------------------------------------------
    std::atomic_bool stop_input{false};
    std::thread input_thread([&]()
    {
        std::string line;
        while (!stop_input.load() && std::getline(std::cin, line))
        {
            if (line == "quit")
            {
                running.store(false);
                queue_cv.notify_all();
                break;
            }
            if (!line.empty()) {
                slave.sendMessage(line);
            }
        }
    });

    // -----------------------------------------------------------------------
    // 主线程：等待帧消费线程结束
    // -----------------------------------------------------------------------
    if (consumer_thread.joinable())
        consumer_thread.join();

    // -----------------------------------------------------------------------
    // 清理
    // -----------------------------------------------------------------------
    stop_input.store(true);
    if (input_thread.joinable())
        input_thread.detach();

    slave.disconnect();
    ix::uninitNetSystem();

    std::cout << "[Slave] Exited cleanly.\n";
    return 0;
}
