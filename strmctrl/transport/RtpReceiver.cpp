#include "RtpReceiver.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>

extern "C"
{
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

namespace strmctrl
{

namespace
{

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------

std::atomic<std::uint64_t> g_temp_sdp_counter{0};

std::string avErrStrRecv(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

#if defined(_DEBUG)
#  define RTP_RECEIVER_DEBUG_MSG(x) do { std::cout << (x) << std::flush; } while (0)
#  define RTP_RECEIVER_DEBUG_ERR(x) do { std::cerr << (x) << std::endl; } while (0)
#else
#  define RTP_RECEIVER_DEBUG_MSG(x) do { (void)(x); } while (0)
#  define RTP_RECEIVER_DEBUG_ERR(x) do { (void)(x); } while (0)
#endif

std::string makeUniqueTempSdpPath()
{
    std::error_code ec;
    auto temp_dir = std::filesystem::temp_directory_path(ec);
    if (ec)
        temp_dir = std::filesystem::current_path(ec);
    if (ec)
        temp_dir = ".";

    const auto unique_id =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "_" + std::to_string(g_temp_sdp_counter.fetch_add(1));
    return (temp_dir / ("strmctrl_recv_" + unique_id + ".sdp")).string();
}

} // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

RtpReceiver::RtpReceiver()
{
    video_decoder_.setFrameCallback(
        [this](const VideoFrame &frame)
        {
            auto cb = videoFrameCallbackCopy();
            if (cb)
                cb(frame);
        });
}

RtpReceiver::~RtpReceiver()
{
    stop();

    if (fmt_ctx_)
    {
        fmt_ctx_->interrupt_callback.callback = nullptr;
        fmt_ctx_->interrupt_callback.opaque = nullptr;
        avformat_close_input(&fmt_ctx_);
    }

    if (timeout_ctx_)
    {
        delete timeout_ctx_;
        timeout_ctx_ = nullptr;
    }

    cleanupTempSdpFile();
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

void RtpReceiver::setVideoFrameCallback(VideoFrameCallback cb)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    video_cb_ = std::move(cb);
}

void RtpReceiver::setAudioFrameCallback(AudioFrameCallback cb)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    audio_cb_ = std::move(cb);
}

void RtpReceiver::setErrorCallback(std::function<void(const std::string &)> cb)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    error_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool RtpReceiver::openWithSdp(const std::string &sdp)
{
    cleanupTempSdpFile();

    const std::string sdp_path = makeUniqueTempSdpPath();
    {
        // SDP 规范（RFC 4566）要求每行以 \r\n 结尾。
        // WebSocket 传输可能把 \r\n 变成 \n，写文件前统一规范化。
        std::string normalized_sdp;
        normalized_sdp.reserve(sdp.size() + 32);
        for (std::size_t i = 0; i < sdp.size(); ++i)
        {
            if (sdp[i] == '\n' && (i == 0 || sdp[i - 1] != '\r'))
                normalized_sdp += '\r';

            normalized_sdp += sdp[i];
        }

        std::ofstream file(sdp_path, std::ios::trunc | std::ios::binary);
        if (!file)
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_error_ = "Cannot write temporary SDP file: " + sdp_path;
            return false;
        }

        file << normalized_sdp;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        temp_sdp_path_ = sdp_path;
    }

    RTP_RECEIVER_DEBUG_MSG("[RtpReceiver] SDP written to: " + sdp_path + "\n"
                  "--- SDP BEGIN ---\n" + sdp + "\n--- SDP END ---\n");

    return openWithUrl(sdp_path, -1);
}

bool RtpReceiver::openWithUrl(const std::string &host, int port)
{
    if (port != -1)
        cleanupTempSdpFile();

    if (fmt_ctx_)
    {
        fmt_ctx_->interrupt_callback.callback = nullptr;
        fmt_ctx_->interrupt_callback.opaque = nullptr;
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }

    if (timeout_ctx_)
    {
        delete timeout_ctx_;
        timeout_ctx_ = nullptr;
    }

    video_decoder_.close();
    audio_decoder_.close();
    video_stream_idx_ = -1;
    audio_stream_idx_ = -1;

    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_)
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = "avformat_alloc_context failed";
        return false;
    }

    timeout_ctx_ = new TimeoutContext;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    timeout_ctx_->last_activity_ts.store(now_ms);
    timeout_ctx_->timeout_ms = 5000;

    fmt_ctx_->interrupt_callback.callback = [](void *opaque) -> int
    {
        auto *ctx = static_cast<TimeoutContext *>(opaque);
        if (!ctx)
            return 0;

        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
        const auto diff = now - ctx->last_activity_ts.load();
        return diff > ctx->timeout_ms ? 1 : 0;
    };
    fmt_ctx_->interrupt_callback.opaque = timeout_ctx_;

    // 必须设置 protocol_whitelist，否则 sdp 无法打开 rtp/udp
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", "file,crypto,data,rtp,udp", 0);
    // 探测时间：500ms 足以接收首帧 SPS/PPS 并让解码器自配置。
    av_dict_set(&opts, "analyzeduration", "500000", 0);
    av_dict_set(&opts, "probesize", "5000000", 0);
    // UDP/RTP 接收缓冲与过载容忍
    av_dict_set(&opts, "buffer_size", "5000000", 0);
    av_dict_set(&opts, "fifo_size", "5000000", 0);
    av_dict_set(&opts, "overrun_nonfatal", "1", 0);
    // 设置长时间 socket 超时作为第二道防线
    av_dict_set(&opts, "timeout", "5000000", 0);
    av_dict_set(&opts, "rw_timeout", "5000000", 0);

    std::string url;
    const AVInputFormat *input_format = nullptr;
    if (port == -1)
    {
        // SDP 模式：host 实际上是 sdp 文件路径
        url = host;
        input_format = av_find_input_format("sdp");
        if (!input_format)
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_error_ = "av_find_input_format('sdp') failed";
            av_dict_free(&opts);
            return false;
        }
    }
    else
    {
        // 直接 RTP 模式
        url = "rtp://" + host + ":" + std::to_string(port);
    }

    const int open_ret = avformat_open_input(&fmt_ctx_, url.c_str(), input_format, &opts);
    av_dict_free(&opts);

    if (open_ret < 0)
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = "avformat_open_input(" + url + "): " + avErrStrRecv(open_ret);
        if (fmt_ctx_)
        {
            if (fmt_ctx_->interrupt_callback.opaque)
            {
                delete static_cast<TimeoutContext *>(fmt_ctx_->interrupt_callback.opaque);
                fmt_ctx_->interrupt_callback.opaque = nullptr;
            }
            avformat_free_context(fmt_ctx_);
            fmt_ctx_ = nullptr;
        }
        return false;
    }

    const int stream_info_ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (stream_info_ret < 0)
    {
RTP_RECEIVER_DEBUG_ERR("[RtpReceiver] Warning: avformat_find_stream_info returned " +
                  std::to_string(stream_info_ret) + " (" + avErrStrRecv(stream_info_ret) + "). Continuing anyway.");
    }

    // 更新 interrupt callback 的超时。
    // 这里我们不删除它，而是更新时间戳，这样后续 workerThread 可以持续使用它
    if (timeout_ctx_)
    {
        const auto refreshed_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count();
        timeout_ctx_->last_activity_ts.store(refreshed_now_ms);
        // 设置较长的超时（如 10 秒），避免短暂网络抖动误杀
        timeout_ctx_->timeout_ms = 10000;
    }

    // 遍历流，初始化解码器
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i)
    {
        AVStream *stream = fmt_ctx_->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx_ == -1)
        {
            video_stream_idx_ = static_cast<int>(i);
            if (!video_decoder_.openWithParameters(stream->codecpar))
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                last_error_ = "VideoDecoder open failed: " + video_decoder_.lastError();
                return false;
            }
            RTP_RECEIVER_DEBUG_MSG("[RtpReceiver] Found Video Stream: index=" + std::to_string(i) + "\n");
        }
        else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_idx_ == -1)
        {
            audio_stream_idx_ = static_cast<int>(i);
            if (!audio_decoder_.openWithParameters(stream->codecpar))
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                last_error_ = "AudioDecoder open failed: " + audio_decoder_.lastError();
                return false;
            }
            RTP_RECEIVER_DEBUG_MSG("[RtpReceiver] Found Audio Stream: index=" + std::to_string(i) + "\n");
        }
    }

    if (video_stream_idx_ == -1 && audio_stream_idx_ == -1)
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = "No video or audio stream found in input";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_.clear();
    }

    return true;
}

void RtpReceiver::start()
{
    if (running_.load() || !fmt_ctx_)
        return;

    running_.store(true);
    thread_ = std::thread(&RtpReceiver::workerThread, this);
}

void RtpReceiver::stop()
{
    if (!running_.load())
        return;

    running_.store(false);
    if (thread_.joinable())
        thread_.join();
}

std::string RtpReceiver::lastError() const noexcept
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_error_;
}

// ---------------------------------------------------------------------------
// 工作线程
// ---------------------------------------------------------------------------

void RtpReceiver::workerThread()
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
    {
        reportError("av_packet_alloc failed");
        return;
    }

    int64_t video_packets = 0;
    int64_t audio_packets = 0;
    int64_t error_count = 0;
    int64_t consecutive_errors = 0;
    auto last_packet_time = std::chrono::steady_clock::now();

    while (running_.load())
    {
        const int ret = av_read_frame(fmt_ctx_, pkt);

        // 更新 watch dog
        if (timeout_ctx_)
        {
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
            timeout_ctx_->last_activity_ts.store(now_ms);
        }

        if (ret < 0)
        {
            const bool is_eagain = (ret == AVERROR(EAGAIN));
            const bool is_eintr = (ret == AVERROR(EINTR));
            const bool is_timeout = (ret == AVERROR(ETIMEDOUT));
            const bool is_eof = (ret == AVERROR_EOF);
            const bool is_transient = (is_eagain || is_eintr || is_timeout || is_eof);

            const auto now = std::chrono::steady_clock::now();
            if (now - last_packet_time > std::chrono::seconds(2))
            {
                RTP_RECEIVER_DEBUG_MSG("[RtpReceiver] idle: no packets for 2000ms. last_error=" + avErrStrRecv(ret) + "\n");
                last_packet_time = now;
            }

            if (is_transient)
            {
                if ((is_timeout || is_eof) && (++error_count % 50 == 1))
                {
                    RTP_RECEIVER_DEBUG_MSG("[RtpReceiver] transient read error count=" + std::to_string(error_count) + " last=" + avErrStrRecv(ret) + "\n");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            consecutive_errors++;
            reportError("av_read_frame error: " + avErrStrRecv(ret));
            if (consecutive_errors % 10 == 1)
            {
                RTP_RECEIVER_DEBUG_ERR("[RtpReceiver] av_read_frame error count=" +
                      std::to_string(consecutive_errors) + " last=" + avErrStrRecv(ret));
            }
            if (consecutive_errors > 50)
            {
                RTP_RECEIVER_DEBUG_ERR("[RtpReceiver] too many consecutive errors, stopping worker thread.");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        consecutive_errors = 0;
        last_packet_time = std::chrono::steady_clock::now();

        if (pkt->stream_index == video_stream_idx_)
        {
            ++video_packets;
            if (!video_decoder_.decode(pkt))
            {
                // decode 内部会调用 callback，这里只处理致命错误
                // 忽略 EAGAIN
                // Decoder reports errors through lastError(); keep streaming on soft failures.
            }
        }
        else if (pkt->stream_index == audio_stream_idx_)
        {
            ++audio_packets;
            AudioFrame audio_frame;
            if (audio_decoder_.decode(pkt, audio_frame))
            {
                auto cb = audioFrameCallbackCopy();
                if (cb)
                    cb(audio_frame);
            }
        }

        if ((video_packets + audio_packets) % 100 == 1)
        {
            RTP_RECEIVER_DEBUG_MSG("[RtpReceiver] packets v=" + std::to_string(video_packets) + " a=" + std::to_string(audio_packets) + "\n");
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
}

// ---------------------------------------------------------------------------
// 内部辅助
// ---------------------------------------------------------------------------

void RtpReceiver::cleanupTempSdpFile()
{
    std::string temp_sdp_path;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        temp_sdp_path.swap(temp_sdp_path_);
    }

    if (temp_sdp_path.empty())
        return;

    std::error_code ec;
    std::filesystem::remove(temp_sdp_path, ec);
}

VideoFrameCallback RtpReceiver::videoFrameCallbackCopy() const
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    return video_cb_;
}

AudioFrameCallback RtpReceiver::audioFrameCallbackCopy() const
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    return audio_cb_;
}

std::function<void(const std::string &)> RtpReceiver::errorCallbackCopy() const
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    return error_cb_;
}

void RtpReceiver::reportError(const std::string &error)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = error;
    }

    auto cb = errorCallbackCopy();
    if (cb)
        cb(error);
}

} // namespace strmctrl
