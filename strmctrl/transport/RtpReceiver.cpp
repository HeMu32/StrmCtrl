#include "RtpReceiver.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

extern "C"
{
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

namespace strmctrl
{

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------
static std::string avErrStrRecv(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

RtpReceiver::RtpReceiver() = default;

RtpReceiver::~RtpReceiver()
{
    stop();
    if (fmt_ctx_)
    {
        // 清理 interrupt callback
        fmt_ctx_->interrupt_callback.callback = nullptr;
        fmt_ctx_->interrupt_callback.opaque = nullptr;
        avformat_close_input(&fmt_ctx_);
    }
    // Delete TimeoutContext if allocated
    if (timeout_ctx_)
    {
        delete timeout_ctx_;
        timeout_ctx_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

void RtpReceiver::setVideoFrameCallback(VideoFrameCallback cb)
{
    video_cb_ = std::move(cb);
    video_decoder_.setFrameCallback(video_cb_);
}

void RtpReceiver::setAudioFrameCallback(AudioFrameCallback cb)
{
    audio_cb_ = std::move(cb);
}

void RtpReceiver::setErrorCallback(std::function<void(const std::string &)> cb)
{
    error_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool RtpReceiver::openWithSdp(const std::string &sdp)
{
    // FFmpeg 通过 "data URI" 方式或临时 .sdp 文件加载 SDP。
    // 这里将 SDP 写入临时文件，再以 "sdp" 格式打开。
    // Windows 下使用 %TEMP% 目录。
    const char *tmp_dir = std::getenv("TEMP");
    if (!tmp_dir)
        tmp_dir = ".";
    const std::string sdp_path = std::string(tmp_dir) + "\\strmctrl_recv.sdp";

    {
        // SDP 规范（RFC 4566）要求每行以 \r\n 结尾。
        // WebSocket 传输可能把 \r\n 变成 \n，写文件前统一规范化。
        std::string sdp_normalized;
        sdp_normalized.reserve(sdp.size() + 32);
        for (std::size_t i = 0; i < sdp.size(); ++i)
        {
            if (sdp[i] == '\n' && (i == 0 || sdp[i - 1] != '\r'))
            {
                sdp_normalized += '\r';
            }
            sdp_normalized += sdp[i];
        }

        // 以二进制模式写入，防止 Windows 再次转换换行符
        std::ofstream f(sdp_path, std::ios::trunc | std::ios::binary);
        if (!f)
        {
            last_error_ = "Cannot write temporary SDP file: " + sdp_path;
            return false;
        }
        f << sdp_normalized;
    }

    std::cout << "[RtpReceiver] SDP written to: " << sdp_path << "\n"
                << "--- SDP BEGIN ---\n"
                << sdp << "\n--- SDP END ---\n"
                << std::flush;

    return openWithUrl(sdp_path, -1 /* 由 SDP 指定端口，传 -1 表示 URL 模式 */);
}

bool RtpReceiver::openWithUrl(const std::string &host, int port)
{
    if (fmt_ctx_)
    {
        // Clear interrupt first
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
        last_error_ = "avformat_alloc_context failed";
        return false;
    }

    // Set interrupt callback
    timeout_ctx_ = new TimeoutContext;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    timeout_ctx_->last_activity_ts.store(now_ms);
    timeout_ctx_->timeout_ms = 5000; // 5s timeout

    fmt_ctx_->interrupt_callback.callback = [](void *opaque) -> int
    {
        auto *ctx = static_cast<TimeoutContext *>(opaque);
        if (!ctx)
            return 0;

        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t diff = now_ms - ctx->last_activity_ts.load();

        if (diff > ctx->timeout_ms)
        {
            // Uncomment to debug timeouts:
            // std::cerr << "[RtpReceiver] Interrupt callback timed out! diff=" << diff << "ms\n";
            return 1; // Interrupt
        }
        return 0; // Continue
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
    const AVInputFormat *in_fmt = nullptr;

    if (port == -1)
    {
        // SDP 模式：host 实际上是 sdp 文件路径
        url = host;
        in_fmt = av_find_input_format("sdp");
        if (!in_fmt)
        {
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

    // -------------------------------------------------------------
    // FFmpeg debug logging can be very helpful when avformat_open_input
    // fails (e.g. a UDP bind error).  By default these messages go to
    // stderr and are invisible to callers.  We temporarily install a
    // custom log callback that accumulates the text so we can append
    // it to last_error_ if open() returns failure.
    //
    // Note: av_log_set_callback is global, so we capture and restore
    // the previous callback to avoid disturbing other parts of the
    // application.  We use a thread-local string buffer since FFmpeg
    // doesn't provide a user context pointer.

    static thread_local std::string s_ffmpeg_log;
    // capture callback (no captures so convertible to function pointer)
    auto capture_cb = [](void * /*ptr*/, int level, const char *fmt, va_list vl)
    {
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, vl);
        s_ffmpeg_log += buf;
    };

    // install our capture callback and clear buffer
    s_ffmpeg_log.clear();
    av_log_set_callback(capture_cb);

    int ret = avformat_open_input(&fmt_ctx_, url.c_str(), in_fmt, &opts);

    // restore default logging behaviour (original callback can't be
    // retrieved on older FFmpeg, so just go back to the default)
    av_log_set_callback(av_log_default_callback);

    // if the open failed, add any captured log text to last_error_
    if (ret < 0 && !s_ffmpeg_log.empty())
    {
        if (!last_error_.empty())
            last_error_ += " | ";
        last_error_ += "ffmpeg log: ";
        last_error_ += s_ffmpeg_log;
    }

    av_dict_free(&opts);

    if (ret < 0)
    {
        // last_error_ may already contain the log appended above
        if (last_error_.empty())
            last_error_ = "avformat_open_input(" + url + "): " + avErrStrRecv(ret);

        if (fmt_ctx_->interrupt_callback.opaque)
        {
            delete static_cast<TimeoutContext *>(fmt_ctx_->interrupt_callback.opaque);
            fmt_ctx_->interrupt_callback.opaque = nullptr;
        }
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
        return false;
    }

    // 查找流信息
    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0)
    {
        std::cerr << "[RtpReceiver] Warning: avformat_find_stream_info returned "
                    << ret << " (" << avErrStrRecv(ret) << "). Continuing anyway.\n";
    }

    // 更新 interrupt callback 的超时
    // 这里我们不删除它，而是更新时间戳，这样后续 workerThread 可以持续使用它
    if (timeout_ctx_)
    {
        // 更新最后活动时间
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        timeout_ctx_->last_activity_ts.store(now_ms);
        // 设置较长的超时（如 10秒），避免短暂网络抖动误杀
        timeout_ctx_->timeout_ms = 10000;
    }

    // 遍历流，初始化解码器
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i)
    {
        AVStream *stream = fmt_ctx_->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx_ == -1)
        {
            video_stream_idx_ = i;
            if (!video_decoder_.openWithParameters(stream->codecpar))
            {
                last_error_ = "VideoDecoder open failed: " + video_decoder_.lastError();
                return false;
            }
            std::cout << "[RtpReceiver] Found Video Stream: index=" << i << "\n";
        }
        else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_idx_ == -1)
        {
            audio_stream_idx_ = i;
            if (!audio_decoder_.openWithParameters(stream->codecpar))
            {
                last_error_ = "AudioDecoder open failed: " + audio_decoder_.lastError();
                return false;
            }
            std::cout << "[RtpReceiver] Found Audio Stream: index=" << i << "\n";
        }
    }

    if (video_stream_idx_ == -1 && audio_stream_idx_ == -1)
    {
        last_error_ = "No video or audio stream found in input";
        return false;
    }

    return true;
}

void RtpReceiver::start()
{
    if (running_.load())
        return;
    if (!fmt_ctx_)
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
    {
        thread_.join();
    }
}

// ---------------------------------------------------------------------------
// 工作线程
// ---------------------------------------------------------------------------

void RtpReceiver::workerThread()
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
    {
        if (error_cb_)
            error_cb_("av_packet_alloc failed");
        return;
    }

    int64_t video_packets = 0;
    int64_t audio_packets = 0;
    int64_t error_count = 0;
    int64_t consecutive_errors = 0;
    auto last_packet_time = std::chrono::steady_clock::now();

    while (running_.load())
    {
        int ret = av_read_frame(fmt_ctx_, pkt);

        // 更新 watch dog
        if (timeout_ctx_)
        {
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            timeout_ctx_->last_activity_ts.store(now_ms);
        }

        if (ret < 0)
        {
            const bool is_eagain = (ret == AVERROR(EAGAIN));
            const bool is_eintr = (ret == AVERROR(EINTR));
            const bool is_timeout = (ret == AVERROR(ETIMEDOUT));
            const bool is_eof = (ret == AVERROR_EOF);
            const bool is_transient = (is_eagain || is_eintr || is_timeout || is_eof);

            auto now = std::chrono::steady_clock::now();
            if (now - last_packet_time > std::chrono::seconds(2))
            {
                std::cout << "[RtpReceiver] idle: no packets for 2000ms. last_error=" << avErrStrRecv(ret) << "\n";
                last_packet_time = now;
            }

            if (is_transient)
            {
                if ((is_timeout || is_eof) && (++error_count % 50 == 1))
                {
                    std::cout << "[RtpReceiver] transient read error count=" << error_count
                                << " last=" << avErrStrRecv(ret) << "\n";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            consecutive_errors++;
            if (error_cb_)
            {
                error_cb_("av_read_frame error: " + avErrStrRecv(ret));
            }
            if (consecutive_errors % 10 == 1)
            {
                std::cerr << "[RtpReceiver] av_read_frame error count=" << consecutive_errors
                            << " last=" << avErrStrRecv(ret) << "\n";
            }
            if (consecutive_errors > 50)
            {
                std::cerr << "[RtpReceiver] too many consecutive errors, stopping worker thread.\n";
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
            last_packet_time = std::chrono::steady_clock::now();
            if (!video_decoder_.decode(pkt))
            {
                // decode 内部会调用 callback，这里只处理致命错误
                // 忽略 EAGAIN
            }
        }
        else if (pkt->stream_index == audio_stream_idx_)
        {
            ++audio_packets;
            last_packet_time = std::chrono::steady_clock::now();
            AudioFrame a_frame;
            if (audio_decoder_.decode(pkt, a_frame))
            {
                if (audio_cb_)
                {
                    audio_cb_(a_frame);
                }
            }
        }

        if ((video_packets + audio_packets) % 100 == 1)
        {
            std::cout << "[RtpReceiver] packets v=" << video_packets
                        << " a=" << audio_packets << "\n";
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
}

} // namespace strmctrl
