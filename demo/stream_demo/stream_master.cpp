/**
 * @file stream_master.cpp
 * @brief stream_demo 主端示例程序。
 *
 * 功能演示：
 *   1. 从本地视频文件中解码视频帧。
 *   2. 通过 strmctrl::Master 将帧编码为 H.264（libopenh264）并通过 RTP 推流给从端。
 *   3. 同时接收来自从端的文本消息，并在控制台发送文本消息给从端。
 *
 * 用法：
 *   stream_master.exe <视频文件路径> [信令端口=11451] [RTP端口=11452] [--loop]
 *
 * 示例：
 *   stream_master.exe C:\Videos\sample.mp4
 *   stream_master.exe C:\Videos\sample.mp4 11451 11452 --loop
 *
 * 默认编码设置：
 *   分辨率 1280x720, 帧率 30, 码率 2000kbps (可在源码顶部常量中修改)
 *
 * 选项：
 *   --loop   视频播放完毕后从头循环推流，直到用户输入 'quit'
 */

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

#include <cerrno>
const int FFMPEG_EAGAIN = EAGAIN;

#include <ixwebsocket/IXNetSystem.h>

#include "strmctrl/Master.h"

// ---------------------------------------------------------------------------
// Demo defaults for encoder configuration.  These are not part of the library
// but are defined here as constants so that the demo behaviour is easy to
// change or audit. Using constexpr (C++ style) rather than raw #defines.
// ---------------------------------------------------------------------------
constexpr int kDefaultEncoderWidth       = 1280;
constexpr int kDefaultEncoderHeight      = 720;
constexpr int kDefaultEncoderFps         = 30;
constexpr int kDefaultEncoderBitrateKbps = 2000;

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/error.h>
}

// ---------------------------------------------------------------------------
// 工具：FFmpeg 错误转字符串
// ---------------------------------------------------------------------------
static std::string avErr(int e)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

// ---------------------------------------------------------------------------
// 视频文件读取器（独立于 strmctrl 库，属于 demo 层）
// ---------------------------------------------------------------------------
class FileVideoSource
{
public:
    /**
     * @brief 打开视频文件。
     * @param path  文件路径
     * @return      true 表示成功
     */
    bool open(const std::string &path)
    {
        path_ = path;
        video_idx_ = -1; // 重置，避免 reopen 时残留旧值
        int ret = avformat_open_input(&fmt_ctx_, path.c_str(), nullptr, nullptr);
        if (ret < 0)
        {
            err_ = "avformat_open_input: " + avErr(ret);
            return false;
        }

        ret = avformat_find_stream_info(fmt_ctx_, nullptr);
        if (ret < 0)
        {
            err_ = "avformat_find_stream_info: " + avErr(ret);
            return false;
        }

        // find_stream_info 会把文件读到末尾做探测，必须 seek 回起点
        if (av_seek_frame(fmt_ctx_, -1, 0, AVSEEK_FLAG_BACKWARD) < 0)
        {
            std::cerr << "[FileVideoSource] Warning: Seek to start failed. Loop playback may fail.\n";
        }

        // 用 av_find_best_stream 选视频流（比手动循环更可靠）
        ret = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (ret < 0)
        {
            err_ = "No video stream: " + avErr(ret);
            return false;
        }
        video_idx_ = ret;

        // 打印所有流信息（调试用）
        std::cout << "[FileVideoSource] Opened: " << path
                  << " | streams=" << fmt_ctx_->nb_streams
                  << " | video_idx=" << video_idx_ << "\n";
        for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i)
        {
            auto *par = fmt_ctx_->streams[i]->codecpar;
            std::cout << "  stream[" << i << "] type="
                      << av_get_media_type_string(par->codec_type)
                      << " codec=" << avcodec_get_name(par->codec_id) << "\n";
        }

        // 打开解码器
        const AVCodec *codec =
            avcodec_find_decoder(fmt_ctx_->streams[video_idx_]->codecpar->codec_id);
        if (!codec)
        {
            err_ = "Decoder not found";
            return false;
        }

        dec_ctx_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(dec_ctx_,
                                      fmt_ctx_->streams[video_idx_]->codecpar);
        ret = avcodec_open2(dec_ctx_, codec, nullptr);
        if (ret < 0)
        {
            err_ = "avcodec_open2: " + avErr(ret);
            return false;
        }

        return true;
    }

    /** @brief 关闭文件并释放资源。 */
    void close()
    {
        if (dec_ctx_)
            avcodec_free_context(&dec_ctx_);
        if (fmt_ctx_)
            avformat_close_input(&fmt_ctx_);
    }

    /**
     * @brief 重新打开文件用于循环播放（比 seek 更可靠）。
     * @return true 表示成功
     */
    bool reopen()
    {
        close();
        return open(path_);
    }

    /**
     * @brief 读取并解码下一帧。
     */
    AVFrame *nextFrame()
    {
        err_.clear();
        AVPacket *pkt = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        int ret = 0;

        while (true)
        {
            // 1. 尝试从解码器取帧
            ret = avcodec_receive_frame(dec_ctx_, frame);
            if (ret == 0)
            {
                // 成功取到一帧
                av_packet_free(&pkt);
                return frame;
            }
            if (ret == AVERROR_EOF)
            {
                // 解码器已排空
                break;
            }
            if (ret != AVERROR(FFMPEG_EAGAIN))
            {
                // 真正的解码错误
                err_ = "avcodec_receive_frame: " + avErr(ret);
                break;
            }

            // 2. 如果是 EAGAIN，说明解码器缺数据，需要读文件并送包
            ret = av_read_frame(fmt_ctx_, pkt);
            if (ret < 0)
            {
                if (ret == AVERROR_EOF)
                {
                    // 文件读完，发送 flush 包 (nullptr) 给解码器
                    avcodec_send_packet(dec_ctx_, nullptr);
                    // 继续循环，下次 receive_frame 会取走剩余缓冲帧或返回 EOF
                    continue;
                }
                else
                {
                    err_ = "av_read_frame: " + avErr(ret);
                    break;
                }
            }

            // 3. 只处理视频流
            if (pkt->stream_index == video_idx_)
            {
                ret = avcodec_send_packet(dec_ctx_, pkt);
                if (ret < 0 && ret != AVERROR(FFMPEG_EAGAIN) && ret != AVERROR_EOF)
                {
                    err_ = "avcodec_send_packet: " + avErr(ret);
                    av_packet_unref(pkt);
                    break;
                }
            }

            av_packet_unref(pkt);
        }

        av_packet_free(&pkt);
        av_frame_free(&frame);
        return nullptr;
    }

    /** @brief 视频帧率（fps），用于控制推流速度。 */
    double fps() const
    {
        if (!fmt_ctx_ || video_idx_ < 0)
            return 30.0;
        const AVRational r = fmt_ctx_->streams[video_idx_]->avg_frame_rate;
        return (r.den > 0) ? static_cast<double>(r.num) / r.den : 30.0;
    }

    const std::string &lastError() const { return err_; }

private:
    AVFormatContext *fmt_ctx_ = nullptr;
    AVCodecContext *dec_ctx_ = nullptr;
    int video_idx_ = -1;
    std::string err_;
    std::string path_;
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: stream_master.exe <video_file> "
                     "[signaling_port=11451] [rtp_port=11452] [--loop]\n";
        return 1;
    }

    const std::string video_path = argv[1];
    const int sig_port = (argc >= 3 && argv[2][0] != '-') ? std::stoi(argv[2]) : 11451;
    const int rtp_port = (argc >= 4 && argv[3][0] != '-') ? std::stoi(argv[3]) : 11452;

    // 检查 --loop 参数（任意位置）
    bool loop_mode = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--loop")
        {
            loop_mode = true;
            break;
        }
    }

    // 初始化 Windows 网络
    ix::initNetSystem();

    // -----------------------------------------------------------------------
    // 配置 Master
    // -----------------------------------------------------------------------
    strmctrl::Master master;
    master.setSignalingPort(sig_port);
    master.setRtpPort(rtp_port);
    // keep a local copy of the codec config so demo logging can show encoder target
    auto codec_cfg = strmctrl::CodecConfig::makeOpenH264(
        kDefaultEncoderWidth,
        kDefaultEncoderHeight,
        kDefaultEncoderFps,
        kDefaultEncoderBitrateKbps);
    master.setCodecConfig(codec_cfg);

    master.setMessageCallback([](const strmctrl::TextMessage &msg)
                              { std::cout << "[Slave " << msg.sender_id << "] " << msg.text << "\n"; });
    master.setConnectionCallback([](bool connected, const std::string &info)
                                 { std::cout << "[Master] Slave "
                                             << (connected ? "connected: " : "disconnected: ")
                                             << info << "\n"; });

    if (!master.start())
    {
        std::cerr << "[Master] Start failed: " << master.lastError() << "\n";
        ix::uninitNetSystem();
        return 1;
    }
    std::cout << "[Master] Listening on signaling port " << sig_port
              << ", RTP port " << rtp_port << "\n";

    // 等待从端连接（最多 30 秒）
    std::cout << "[Master] Waiting for slave to connect...\n";
    for (int i = 0; i < 300 && !master.hasSlaveConnected(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!master.hasSlaveConnected())
    {
        std::cerr << "[Master] Timeout: no slave connected\n";
        master.stop();
        ix::uninitNetSystem();
        return 1;
    }

    // 等待 READY 握手完成（最多 60 秒）
    // 从端须完成 SDP 协商、绑定 UDP 端口、并回传 READY 后，才能安全推流
    std::cout << "[Master] Slave connected, waiting for READY handshake...\n";
    for (int i = 0; i < 600 && !master.isRtpReady(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!master.isRtpReady())
    {
        std::cerr << "[Master] Timeout: slave never sent READY\n";
        master.stop();
        ix::uninitNetSystem();
        return 1;
    }

    // -----------------------------------------------------------------------
    // 打开源视频文件
    // -----------------------------------------------------------------------
    FileVideoSource src;
    if (!src.open(video_path))
    {
        std::cerr << "[Master] Cannot open video: " << src.lastError() << "\n";
        master.stop();
        ix::uninitNetSystem();
        return 1;
    }
    std::cout << "[Master] Streaming: " << video_path
              << " @ " << src.fps() << " fps"
              << " | encoder_out=" << codec_cfg.width << "x" << codec_cfg.height;

    // Also print source resolution (if available) to avoid ambiguity between source
    // frame size and encoder output size shown later when the first frame is pushed.
    if (src.fps() > 0.0)
        std::cout << " | source_fps=" << src.fps();
    std::cout << "\n"
              << "[Master] Type a message and press Enter to send; 'quit' to exit.\n";

    // -----------------------------------------------------------------------
    // 推流线程：读帧 → pushVideoFrame，按帧率限速
    // -----------------------------------------------------------------------
    std::atomic_bool streaming{true};
    const auto frame_interval =
        std::chrono::microseconds(static_cast<long long>(1e6 / src.fps()));

    std::thread stream_thread([&]()
                              {
        int64_t pts = 0;
        int64_t frame_count = 0;
        while (streaming.load()) {
            auto t0 = std::chrono::steady_clock::now();

            AVFrame* frame = src.nextFrame();
            if (!frame) {
                // 只打印真实错误（非正常 EOF）
                if (!src.lastError().empty()) {
                    std::cerr << "[Master] nextFrame error: "
                              << src.lastError() << "\n";
                }
                if (loop_mode && streaming.load()) {
                    std::cout << "[Master] Video file ended, looping...\n";
                    if (!src.reopen()) {
                        std::cerr << "[Master] Reopen failed: "
                                  << src.lastError() << "\n";
                        break;
                    }
                    std::cout << "[Master] Reopen OK, continuing.\n";
                    pts = 0;
                    frame_count = 0;
                    continue;
                }
                std::cout << "[Master] Video file ended.\n";
                break;
            }
            frame->pts = pts++;
            ++frame_count;

            // 首帧确认
            if (frame_count == 1) {
                std::cout << "[Master] First frame: source="
                          << frame->width << "x" << frame->height
                          << " fmt=" << frame->format
                          << " | encoder_out=" << codec_cfg.width << "x" << codec_cfg.height
                          << "\n";
            }

            if (!master.pushVideoFrame(frame)) {
                std::cerr << "[Master] pushVideoFrame error: "
                          << master.lastError() << "\n";
            }
            av_frame_free(&frame);

            // 限速到源视频帧率
            auto elapsed = std::chrono::steady_clock::now() - t0;
            if (elapsed < frame_interval)
                std::this_thread::sleep_for(frame_interval - elapsed);
        }
        streaming.store(false); });

    // -----------------------------------------------------------------------
    // 控制台输入线程：仅负责发送文本，不影响推流生命周期
    // -----------------------------------------------------------------------
    std::mutex send_mutex;
    std::atomic_bool stop_input{false};
    std::thread input_thread([&]()
                             {
                                 std::string line;
                                 while (!stop_input.load() && std::getline(std::cin, line))
                                 {
                                     if (line == "quit")
                                     {
                                         streaming.store(false);
                                         break;
                                     }
                                     std::lock_guard<std::mutex> lk(send_mutex);
                                     master.sendMessage(line);
                                 }
                                 // 读到 EOF/输入关闭时，不修改 streaming，避免主端被动退出
                             });

    if (stream_thread.joinable())
        stream_thread.join();
    stop_input.store(true);
    if (input_thread.joinable())
        input_thread.detach();

    // -----------------------------------------------------------------------
    // 清理
    // -----------------------------------------------------------------------
    src.close();
    master.stop();
    ix::uninitNetSystem();

    std::cout << "[Master] Exited cleanly.\n";
    return 0;
}
