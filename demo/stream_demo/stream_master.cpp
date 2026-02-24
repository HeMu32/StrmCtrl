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
#include <vector>
#include <cmath>

#include <cerrno>
const int FFMPEG_EAGAIN = EAGAIN;

#include <ixwebsocket/IXNetSystem.h>

#include "strmctrl/Master.h"

// ---------------------------------------------------------------------------
// Demo defaults for encoder configuration.  Master uses a modest
// 1280x720@30 profile with AAC audio at 44.1 kHz.  Slave will later request
// 1920x1080@30 to demonstrate negotiation.
// ---------------------------------------------------------------------------
constexpr int kDefaultEncoderWidth       = 1280;
constexpr int kDefaultEncoderHeight      = 720;
constexpr int kDefaultEncoderFps         = 30;    // master prefers 30fps to match slave request
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

    std::vector<AVFrame*> decodePacket(AVPacket* pkt)
    {
        std::vector<AVFrame*> frames;
        AVCodecContext* dec_ctx = nullptr;
        if (pkt->stream_index == video_idx_) dec_ctx = video_dec_ctx_;
        else if (pkt->stream_index == audio_idx_) dec_ctx = audio_dec_ctx_;
        else return frames;

        int ret = avcodec_send_packet(dec_ctx, pkt);
        if (ret == AVERROR(EAGAIN)) {
            drainDecoder(dec_ctx, frames);
            ret = avcodec_send_packet(dec_ctx, pkt);
        }

        if (ret < 0) return frames;

        drainDecoder(dec_ctx, frames);
        return frames;
    }

    std::vector<AVFrame*> flushStream(int stream_idx)
    {
        std::vector<AVFrame*> frames;
        AVCodecContext* dec_ctx = nullptr;
        if (stream_idx == video_idx_) dec_ctx = video_dec_ctx_;
        else if (stream_idx == audio_idx_) dec_ctx = audio_dec_ctx_;
        else return frames;

        int ret = avcodec_send_packet(dec_ctx, nullptr);
        if (ret < 0 && ret != AVERROR_EOF) return frames;
        drainDecoder(dec_ctx, frames);
        return frames;
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
    void drainDecoder(AVCodecContext* dec_ctx, std::vector<AVFrame*>& frames)
    {
        while (true) {
            AVFrame* frame = av_frame_alloc();
            int r = avcodec_receive_frame(dec_ctx, frame);
            if (r == 0) {
                frames.push_back(frame);
            } else {
                av_frame_free(&frame);
                break;
            }
        }
    }

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
        // 记录流开始的物理时间
        auto stream_clock_start = std::chrono::steady_clock::now();
        
        // 用于生成输出 PTS (单调递增)
        int64_t out_video_pts = 0;
        int64_t out_audio_pts = 0;

        // 记录上一帧被**接受并发送**的源视频时间戳(秒)，用于计算是否需要跳帧
        double last_sent_video_src_sec = -1.0;
        
        // 最小帧间隔 (略小于 1/30s 以容忍抖动)
        const double kMinFrameInterval = 1.0 / (kDefaultEncoderFps + 5.0); 

        // 辅助函数：等到物理时间到达 target_abs_sec
        auto waitDeletedUntil = [&](double target_rel_sec) {
            if (!running) return;
            auto target_time = stream_clock_start + std::chrono::duration<double>(target_rel_sec);
            std::this_thread::sleep_until(target_time);
        };

        // 记录当前 Loop 的时间偏移量（当文件 loop 时，pts 会归零，我们需要加上 offset 保持单调递增的逻辑时间轴）
        double loop_time_offset = 0.0;
        
        // 记录上一次读取到的 Packet 的解码时间戳，用于检测 Loop 回绕
        double last_pkt_sec = -1.0;

        while (running)
        {
            AVPacket* pkt = source.readPacket();
            if (!pkt)
            {
                if (source.lastError() == "EOF" && loop)
                {
                    std::cout << "[Stream] EOF reached, looping...\n";
                    
                    // 刷新剩下的帧
                    // 注意：这里为了简单，Flush 出来的帧我们就不做精细的时间控制了，或者直接忽略
                    // 更好的做法是封装一个 processFrames 函数复用逻辑。
                    // 鉴于 demo 简单性，这里直接重置
                    
                    // 累计时间偏移量：加上这一个文件的时长（近似用 last_pkt_sec）
                    // 加上一点余量防止重叠，比如 0.05s
                    if (last_pkt_sec > 0) {
                        loop_time_offset += (last_pkt_sec + 0.1);
                    }

                    source.reopen();
                    last_pkt_sec = -1.0;
                    last_sent_video_src_sec = -1.0; // 重置跳帧逻辑的参照点 (相对于新文件开头)
                    continue;
                }
                else
                {
                    std::cerr << "[Stream] Read error or EOF: " << source.lastError() << "\n";
                    break;
                }
            }

            if (!running) {
                av_packet_free(&pkt);
                break;
            }

            // 计算当前 Packet 大致的秒数，用于判断是否 Loop 回绕以及更新 last_pkt_sec
            AVRational tb_pkt = source.streamTimeBase(pkt->stream_index);
            double pkt_sec = (pkt->pts == AV_NOPTS_VALUE) ? 0.0 : (pkt->pts * av_q2d(tb_pkt));
            if (pkt_sec > last_pkt_sec) last_pkt_sec = pkt_sec;

            std::vector<AVFrame*> frames = source.decodePacket(pkt);
            
            // 区分流索引处理
            bool is_video = (pkt->stream_index == source.videoStreamIndex());
            bool is_audio = (pkt->stream_index == source.audioStreamIndex());
            
            // 用完了 pkt 就可以释放了
            av_packet_free(&pkt); 

            for (AVFrame* frame : frames) {
                if (!running) { av_frame_free(&frame); continue; }

                // 1. 获取该帧在源文件中的“绝对时间” (秒)
                AVRational tb = source.streamTimeBase(is_video ? source.videoStreamIndex() : source.audioStreamIndex());
                double frame_src_sec = 0.0;
                if (frame->pts != AV_NOPTS_VALUE) {
                    frame_src_sec = frame->pts * av_q2d(tb);
                }
                
                // 2. 视频跳帧逻辑：
                //    如果我们刚刚发送了一帧(last_sent)，且当前帧(frame_src_sec)距离它太近
                //    说明源帧率高于目标帧率，需要把中间的“过渡帧”丢掉。
                //    音频通常不需要跳帧，除非你要做倍速。
                if (is_video) {
                    // 如果不是第一帧，且距离上一帧太近，则丢弃
                    if (last_sent_video_src_sec >= 0.0) {
                        double delta = frame_src_sec - last_sent_video_src_sec;
                        // 如果 delta 为负（乱序）或极小，也丢弃
                        if (delta < kMinFrameInterval) {
                            // Drop
                            av_frame_free(&frame);
                            continue;
                        }
                    }
                }

                // 3. 实时等待 (Pacing)
                //    我们要等到 "物理时间" 追上 "视频/音频的播放时间"。
                //    播放时间 = loop_offset + frame_src_sec
                double play_time_sec = loop_time_offset + frame_src_sec;
                
                // 等待
                waitDeletedUntil(play_time_sec);

                // 4. 发送
                // 为了确保 RTP 传输的稳定性，我们手动按标准时钟计算 PTS
                // H.264 RTP 标准时钟频率通常为 90000
                // Audio 标准时钟频率通常为采样率 (e.g. 48000)
                
                if (is_video) {
                    last_sent_video_src_sec = frame_src_sec;
                    
                    // VideoEncoder 内部配置了 time_base = {1, fps}
                    // 我们这里给它传入 "帧序号" 作为 PTS，它会按 1/fps 编码
                    // 或者我们应该做更精确的映射。
                    // 为了避免混乱，我们按 "Packet" 数量递增，让 Encoder 认为这是第 N 帧
                    
                    frame->pts = out_video_pts;
                    out_video_pts++; 
                    
                    master.pushVideoFrame(strmctrl::VideoFrame(frame));

                } else if (is_audio) {
                    // AudioEncoder 内部使用 time_base = {1, sample_rate}
                    // 它是根据 sample count 来计算的。
                    // 我们传递 采样点总数 作为 PTS 给它。
                    
                    frame->pts = out_audio_pts;

                    // 更新计数器
                    if (frame->nb_samples > 0) out_audio_pts += frame->nb_samples;
                    else out_audio_pts += 1024; // fallback

                    master.pushAudioFrame(strmctrl::AudioFrame(frame));
                } else {
                    av_frame_free(&frame);
                }
            }
            
            // 打印一次进度日志 (每秒)
            static int last_print_sec = -1;
            auto now = std::chrono::steady_clock::now();
            int cur_sec = (int)std::chrono::duration_cast<std::chrono::seconds>(now - stream_clock_start).count();
            if (cur_sec > last_print_sec) {
                last_print_sec = cur_sec;
                std::cout << "[Stream] Time: " << cur_sec << "s | LoopOff=" << loop_time_offset << "\n";
            }
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
