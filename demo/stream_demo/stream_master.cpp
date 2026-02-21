/**
 * @file stream_master.cpp
 * @brief stream_demo 主端示例程序。
 *
 * 功能演示：
 *   1. 从本地视频文件中解码视频和音频帧。
 *   2. 通过 strmctrl::Master 将帧编码为 H.264 和 AAC，并通过 RTP 推流给从端。
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
 *   视频：分辨率 1280x720, 帧率 30, 码率 2000kbps
 *   音频：采样率 48000, 声道 2, 码率 128kbps
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
// Demo defaults for encoder configuration.
// ---------------------------------------------------------------------------
constexpr int kDefaultEncoderWidth       = 1280;
constexpr int kDefaultEncoderHeight      = 720;
constexpr int kDefaultEncoderFps         = 30;
constexpr int kDefaultEncoderBitrateKbps = 2000;

constexpr int kDefaultAudioSampleRate    = 48000;
constexpr int kDefaultAudioChannels      = 2;
constexpr int kDefaultAudioBitrate       = 128000;

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
// 媒体文件读取器（独立于 strmctrl 库，属于 demo 层）
// ---------------------------------------------------------------------------
class FileMediaSource
{
public:
    bool open(const std::string &path)
    {
        path_ = path;
        video_idx_ = -1;
        audio_idx_ = -1;

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

        if (av_seek_frame(fmt_ctx_, -1, 0, AVSEEK_FLAG_BACKWARD) < 0)
        {
            std::cerr << "[FileMediaSource] Warning: Seek to start failed.\n";
        }

        // 找视频流
        ret = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (ret >= 0) {
            video_idx_ = ret;
            if (!openDecoder(video_idx_, &video_dec_ctx_)) return false;
        }

        // 找音频流
        ret = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (ret >= 0) {
            audio_idx_ = ret;
            if (!openDecoder(audio_idx_, &audio_dec_ctx_)) return false;
        }

        if (video_idx_ == -1 && audio_idx_ == -1) {
            err_ = "No video or audio stream found";
            return false;
        }

        std::cout << "[FileMediaSource] Opened: " << path
                  << " | video_idx=" << video_idx_
                  << " | audio_idx=" << audio_idx_ << "\n";

        return true;
    }

    void close()
    {
        if (video_dec_ctx_) avcodec_free_context(&video_dec_ctx_);
        if (audio_dec_ctx_) avcodec_free_context(&audio_dec_ctx_);
        if (fmt_ctx_) avformat_close_input(&fmt_ctx_);
    }

    bool reopen()
    {
        close();
        return open(path_);
    }

    AVPacket* readPacket()
    {
        AVPacket* pkt = av_packet_alloc();
        int ret = av_read_frame(fmt_ctx_, pkt);
        if (ret < 0) {
            av_packet_free(&pkt);
            if (ret == AVERROR_EOF) {
                err_ = "EOF";
            } else {
                err_ = "av_read_frame: " + avErr(ret);
            }
            return nullptr;
        }
        return pkt;
    }

    AVFrame* decodePacket(AVPacket* pkt)
    {
        AVCodecContext* dec_ctx = nullptr;
        if (pkt->stream_index == video_idx_) dec_ctx = video_dec_ctx_;
        else if (pkt->stream_index == audio_idx_) dec_ctx = audio_dec_ctx_;
        else return nullptr;

        int ret = avcodec_send_packet(dec_ctx, pkt);
        if (ret < 0) return nullptr;

        AVFrame* frame = av_frame_alloc();
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == 0) {
            return frame;
        }
        av_frame_free(&frame);
        return nullptr;
    }

    int videoStreamIndex() const { return video_idx_; }
    int audioStreamIndex() const { return audio_idx_; }
    AVRational streamTimeBase(int stream_idx) const {
        if (stream_idx >= 0 && stream_idx < static_cast<int>(fmt_ctx_->nb_streams)) {
            return fmt_ctx_->streams[stream_idx]->time_base;
        }
        return {1, 1};
    }

    const std::string &lastError() const { return err_; }

private:
    bool openDecoder(int stream_idx, AVCodecContext** dec_ctx) {
        const AVCodec *codec = avcodec_find_decoder(fmt_ctx_->streams[stream_idx]->codecpar->codec_id);
        if (!codec) {
            err_ = "Decoder not found for stream " + std::to_string(stream_idx);
            return false;
        }
        *dec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(*dec_ctx, fmt_ctx_->streams[stream_idx]->codecpar);
        int ret = avcodec_open2(*dec_ctx, codec, nullptr);
        if (ret < 0) {
            err_ = "avcodec_open2: " + avErr(ret);
            return false;
        }
        return true;
    }

    std::string path_;
    AVFormatContext *fmt_ctx_ = nullptr;
    AVCodecContext *video_dec_ctx_ = nullptr;
    AVCodecContext *audio_dec_ctx_ = nullptr;
    int video_idx_ = -1;
    int audio_idx_ = -1;
    std::string err_;
};

// ---------------------------------------------------------------------------
// 主程序
// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <video_file> [signaling_port] [rtp_port] [--loop]\n";
        return 1;
    }

    std::string file_path = argv[1];
    int sig_port = 11451;
    int rtp_port = 11452;
    bool loop = false;

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--loop")
        {
            loop = true;
        }
        else if (i == 2)
        {
            sig_port = std::stoi(arg);
        }
        else if (i == 3)
        {
            rtp_port = std::stoi(arg);
        }
    }

    ix::initNetSystem();

    FileMediaSource source;
    if (!source.open(file_path))
    {
        std::cerr << "Failed to open source: " << source.lastError() << "\n";
        return 1;
    }

    strmctrl::Master master;

    // 配置视频
    auto v_cfg = strmctrl::CodecConfig::makeOpenH264(
        kDefaultEncoderWidth, kDefaultEncoderHeight, kDefaultEncoderFps, kDefaultEncoderBitrateKbps);
    master.setCodecConfig(v_cfg);

    // 配置音频
    if (source.audioStreamIndex() != -1) {
        auto a_cfg = strmctrl::AudioConfig::makeAAC(
            kDefaultAudioSampleRate, kDefaultAudioChannels, kDefaultAudioBitrate);
        master.setAudioConfig(a_cfg);
    }

    master.setSignalingPort(sig_port);
    master.setRtpPort(rtp_port);

    master.setMessageCallback([](const strmctrl::TextMessage &msg)
                              {
                                  std::cout << "[Slave MSG] " << msg.text << "\n";
                              });

    master.setConnectionCallback([](bool connected, const std::string &info)
                                 {
                                     std::cout << "[Connection] "
                                               << (connected ? "UP: " : "DOWN: ") << info << "\n";
                                 });

    if (!master.start())
    {
        std::cerr << "Master start failed.\n";
        return 1;
    }

    std::cout << "Master started. Listening on port " << sig_port << "...\n";
    std::cout << "Type 'quit' to exit, or type a message to send to slave.";

    // no prompt mechanism, nothing to flush

    std::atomic<bool> running{true};

    // 推流线程
    std::thread stream_thread([&]()
    {
        auto start_time = std::chrono::steady_clock::now();
        int64_t loop_offset_pts_v = 0;
        int64_t loop_offset_pts_a = 0;
        int64_t last_pts_v = 0;
        int64_t last_pts_a = 0;

        while (running)
        {
            AVPacket* pkt = source.readPacket();
            if (!pkt)
            {
                if (source.lastError() == "EOF" && loop)
                {
                    std::cout << "[Stream] EOF reached, looping...\n";
                    source.reopen();
                    loop_offset_pts_v += last_pts_v;
                    loop_offset_pts_a += last_pts_a;
                    continue;
                }
                else
                {
                    std::cerr << "[Stream] Read error or EOF: " << source.lastError() << "\n";
                    break;
                }
            }

            // 计算 PTS
            int64_t pts = pkt->pts;
            if (pts == AV_NOPTS_VALUE) pts = pkt->dts;
            if (pts == AV_NOPTS_VALUE) pts = 0;

            AVRational tb = source.streamTimeBase(pkt->stream_index);
            
            if (pkt->stream_index == source.videoStreamIndex()) {
                last_pts_v = pts;
                pts += loop_offset_pts_v;
            } else if (pkt->stream_index == source.audioStreamIndex()) {
                last_pts_a = pts;
                pts += loop_offset_pts_a;
            }

            double pts_sec = pts * av_q2d(tb);

            // 同步等待
            while (running) {
                auto now = std::chrono::steady_clock::now();
                std::chrono::duration<double> elapsed = now - start_time;
                if (elapsed.count() >= pts_sec) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            if (!running) {
                av_packet_free(&pkt);
                break;
            }

            AVFrame* frame = source.decodePacket(pkt);
            if (frame) {
                frame->pts = pts; // 更新为连续的 PTS
                if (pkt->stream_index == source.videoStreamIndex()) {
                    master.pushVideoFrame(strmctrl::VideoFrame(frame));
                } else if (pkt->stream_index == source.audioStreamIndex()) {
                    master.pushAudioFrame(strmctrl::AudioFrame(frame));
                } else {
                    av_frame_free(&frame);
                }
            }

            av_packet_free(&pkt);
        }
    });

    // 主线程：读取控制台输入
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line == "quit" || line == "exit")
        {
            break;
        }
        if (!line.empty())
        {
            master.sendMessage(line);
        }
        std::cout << "> ";
    }

    running = false;
    if (stream_thread.joinable())
    {
        stream_thread.join();
    }

    master.stop();
    source.close();
    ix::uninitNetSystem();

    return 0;
}
