#include "RtpReceiver.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

namespace strmctrl {

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------
static std::string avErrStrRecv(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

struct TimeoutContext {
    std::chrono::steady_clock::time_point start;
    int timeout_ms;
};

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

RtpReceiver::RtpReceiver() = default;

RtpReceiver::~RtpReceiver()
{
    stop();
    if (fmt_ctx_) {
        if (fmt_ctx_->interrupt_callback.opaque) {
            delete static_cast<TimeoutContext*>(fmt_ctx_->interrupt_callback.opaque);
            fmt_ctx_->interrupt_callback.opaque = nullptr;
        }
        avformat_close_input(&fmt_ctx_);
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

void RtpReceiver::setErrorCallback(std::function<void(const std::string&)> cb)
{
    error_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool RtpReceiver::openWithSdp(const std::string& sdp)
{
    // FFmpeg 通过 "data URI" 方式或临时 .sdp 文件加载 SDP。
    // 这里将 SDP 写入临时文件，再以 "sdp" 格式打开。
    // Windows 下使用 %TEMP% 目录。
    const char* tmp_dir = std::getenv("TEMP");
    if (!tmp_dir) tmp_dir = ".";
    const std::string sdp_path = std::string(tmp_dir) + "\\strmctrl_recv.sdp";

    {
        // SDP 规范（RFC 4566）要求每行以 \r\n 结尾。
        // WebSocket 传输可能把 \r\n 变成 \n，写文件前统一规范化。
        std::string sdp_normalized;
        sdp_normalized.reserve(sdp.size() + 32);
        for (std::size_t i = 0; i < sdp.size(); ++i) {
            if (sdp[i] == '\n' && (i == 0 || sdp[i - 1] != '\r')) {
                sdp_normalized += '\r';
            }
            sdp_normalized += sdp[i];
        }

        // 以二进制模式写入，防止 Windows 再次转换换行符
        std::ofstream f(sdp_path, std::ios::trunc | std::ios::binary);
        if (!f) {
            last_error_ = "Cannot write temporary SDP file: " + sdp_path;
            return false;
        }
        f << sdp_normalized;
    }

    std::cout << "[RtpReceiver] SDP written to: " << sdp_path << "\n"
              << "--- SDP BEGIN ---\n" << sdp << "\n--- SDP END ---\n" << std::flush;

    return openWithUrl(sdp_path, -1 /* 由 SDP 指定端口，传 -1 表示 URL 模式 */);
}

bool RtpReceiver::openWithUrl(const std::string& host, int port)
{
    if (fmt_ctx_) {
        if (fmt_ctx_->interrupt_callback.opaque) {
            delete static_cast<TimeoutContext*>(fmt_ctx_->interrupt_callback.opaque);
            fmt_ctx_->interrupt_callback.opaque = nullptr;
        }
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }

    video_decoder_.close();
    audio_decoder_.close();
    video_stream_idx_ = -1;
    audio_stream_idx_ = -1;

    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) {
        last_error_ = "avformat_alloc_context failed";
        return false;
    }

    // Set interrupt callback to timeout if no data arrives
    TimeoutContext* tctx = new TimeoutContext{std::chrono::steady_clock::now(), 3000}; // 3s timeout
    fmt_ctx_->interrupt_callback.callback = [](void* opaque) -> int {
        auto* ctx = static_cast<TimeoutContext*>(opaque);
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx->start).count() > ctx->timeout_ms) {
            return 1; // 1 means interrupt
        }
        return 0; // 0 means continue
    };
    fmt_ctx_->interrupt_callback.opaque = tctx;

    // 必须设置 protocol_whitelist，否则 sdp 无法打开 rtp/udp
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", "file,crypto,data,rtp,udp", 0);
    // 探测时间：500ms 足以接收首帧 SPS/PPS 并让解码器自配置。
    // 注意：avformat_find_stream_info 对 RTP 流可能返回负值（unspecified size），
    // 这不影响后续解码。
    av_dict_set(&opts, "analyzeduration", "500000", 0);
    av_dict_set(&opts, "probesize", "1048576", 0);
    // 设置超时以防止挂起
    av_dict_set(&opts, "timeout", "1000000", 0); // 1秒
    av_dict_set(&opts, "rw_timeout", "1000000", 0); // 1秒

    std::string url;
    const AVInputFormat* in_fmt = nullptr;

    if (port == -1) {
        // SDP 模式：host 实际上是 sdp 文件路径
        url = host;
        in_fmt = av_find_input_format("sdp");
        if (!in_fmt) {
            last_error_ = "av_find_input_format('sdp') failed";
            av_dict_free(&opts);
            return false;
        }
    } else {
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
    auto capture_cb = [](void* /*ptr*/, int level, const char* fmt, va_list vl) {
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
    if (ret < 0 && !s_ffmpeg_log.empty()) {
        if (!last_error_.empty()) last_error_ += " | ";
        last_error_ += "ffmpeg log: ";
        last_error_ += s_ffmpeg_log;
    }
    
    av_dict_free(&opts);

    if (ret < 0) {
        // last_error_ may already contain the log appended above
        if (last_error_.empty())
            last_error_ = "avformat_open_input(" + url + "): " + avErrStrRecv(ret);
        
        if (fmt_ctx_->interrupt_callback.opaque) {
            delete static_cast<TimeoutContext*>(fmt_ctx_->interrupt_callback.opaque);
            fmt_ctx_->interrupt_callback.opaque = nullptr;
        }
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
        return false;
    }

    // 查找流信息
    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) {
        // 对于 RTP 流，如果发送端还没发数据，这里可能会返回负数（如 unspecified size）。
        // 但只要流被创建了，后续收到 SPS/PPS 依然可以解码。
        std::cerr << "[RtpReceiver] Warning: avformat_find_stream_info returned "
                  << ret << " (" << avErrStrRecv(ret) << "). Continuing anyway.\n";
    }

    // Disable the timeout callback now that stream info is probed
    if (fmt_ctx_->interrupt_callback.opaque) {
        delete static_cast<TimeoutContext*>(fmt_ctx_->interrupt_callback.opaque);
        fmt_ctx_->interrupt_callback.opaque = nullptr;
        fmt_ctx_->interrupt_callback.callback = nullptr;
    }

    // 遍历流，初始化解码器
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
        AVStream* stream = fmt_ctx_->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx_ == -1) {
            video_stream_idx_ = i;
            if (!video_decoder_.openWithParameters(stream->codecpar)) {
                last_error_ = "VideoDecoder open failed: " + video_decoder_.lastError();
                return false;
            }
            std::cout << "[RtpReceiver] Found Video Stream: index=" << i << "\n";
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_idx_ == -1) {
            audio_stream_idx_ = i;
            if (!audio_decoder_.openWithParameters(stream->codecpar)) {
                last_error_ = "AudioDecoder open failed: " + audio_decoder_.lastError();
                return false;
            }
            std::cout << "[RtpReceiver] Found Audio Stream: index=" << i << "\n";
        }
    }

    if (video_stream_idx_ == -1 && audio_stream_idx_ == -1) {
        last_error_ = "No video or audio stream found in input";
        return false;
    }

    return true;
}

void RtpReceiver::start()
{
    if (running_.load()) return;
    if (!fmt_ctx_) return;

    running_.store(true);
    thread_ = std::thread(&RtpReceiver::workerThread, this);
}

void RtpReceiver::stop()
{
    if (!running_.load()) return;

    running_.store(false);
    if (thread_.joinable()) {
        thread_.join();
    }
}

// ---------------------------------------------------------------------------
// 工作线程
// ---------------------------------------------------------------------------

void RtpReceiver::workerThread()
{
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        if (error_cb_) error_cb_("av_packet_alloc failed");
        return;
    }

    while (running_.load()) {
        int ret = av_read_frame(fmt_ctx_, pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (ret == AVERROR_EOF) {
                std::cout << "[RtpReceiver] EOF reached.\n";
                break;
            }
            // 其他错误，可能是网络断开
            if (error_cb_) {
                error_cb_("av_read_frame error: " + avErrStrRecv(ret));
            }
            break;
        }

        if (pkt->stream_index == video_stream_idx_) {
            if (!video_decoder_.decode(pkt)) {
                // decode 内部会调用 callback，这里只处理致命错误
                // 忽略 EAGAIN
            }
        } else if (pkt->stream_index == audio_stream_idx_) {
            AudioFrame a_frame;
            if (audio_decoder_.decode(pkt, a_frame)) {
                if (audio_cb_) {
                    audio_cb_(a_frame);
                }
            }
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
}

} // namespace strmctrl
