#include "RtpSender.h"

#include <cstring>
#include <sstream>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

namespace strmctrl {

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------
static std::string avErrStrSend(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// 析构
// ---------------------------------------------------------------------------

RtpSender::~RtpSender()
{
    close();
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool RtpSender::open(const std::string&    dest_host,
                     int                   dest_port,
                     const AVCodecContext* codec_ctx)
{
    if (!codec_ctx) {
        last_error_ = "RtpSender::open: null codec_ctx";
        return false;
    }

    const std::string url = "rtp://" + dest_host + ":" + std::to_string(dest_port);

    // 分配输出格式上下文（rtp 格式）
    int ret = avformat_alloc_output_context2(&fmt_ctx_, nullptr, "rtp", url.c_str());
    if (ret < 0 || !fmt_ctx_) {
        last_error_ = "avformat_alloc_output_context2: " + avErrStrSend(ret);
        return false;
    }

    // 添加视频流
    stream_ = avformat_new_stream(fmt_ctx_, nullptr);
    if (!stream_) {
        last_error_ = "avformat_new_stream failed";
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
        return false;
    }

    // 从编码器上下文复制参数到流
    ret = avcodec_parameters_from_context(stream_->codecpar, codec_ctx);
    if (ret < 0) {
        last_error_ = "avcodec_parameters_from_context: " + avErrStrSend(ret);
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
        return false;
    }

    stream_->time_base = codec_ctx->time_base;

    // 打开 UDP 输出 IO
    ret = avio_open(&fmt_ctx_->pb, url.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        last_error_ = "avio_open(" + url + "): " + avErrStrSend(ret);
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
        return false;
    }

    // 写文件头（对 RTP 来说会触发 SDP 生成）
    ret = avformat_write_header(fmt_ctx_, nullptr);
    if (ret < 0) {
        last_error_ = "avformat_write_header: " + avErrStrSend(ret);
        avio_closep(&fmt_ctx_->pb);
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
        return false;
    }

    pkt_index_ = 0;
    return true;
}

void RtpSender::close()
{
    if (fmt_ctx_) {
        av_write_trailer(fmt_ctx_);
        if (fmt_ctx_->pb) avio_closep(&fmt_ctx_->pb);
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
        stream_  = nullptr;
    }
}

// ---------------------------------------------------------------------------
// 推流
// ---------------------------------------------------------------------------

bool RtpSender::sendPacket(AVPacket* pkt)
{
    if (!fmt_ctx_ || !stream_) {
        last_error_ = "RtpSender not open";
        return false;
    }

    // 克隆包以便调整时间戳，不破坏调用方的包
    AVPacket* out = av_packet_clone(pkt);
    if (!out) {
        last_error_ = "av_packet_clone failed";
        return false;
    }

    out->stream_index = stream_->index;

    // 如果包没有有效 pts/dts，用单调递增的包索引推算
    if (out->pts == AV_NOPTS_VALUE) {
        out->pts = pkt_index_;
        out->dts = pkt_index_;
    }
    ++pkt_index_;

    // 将 pts/dts 从编码器时间基转换到流时间基
    av_packet_rescale_ts(out, { 1, stream_->time_base.den }, stream_->time_base);

    int ret = av_interleaved_write_frame(fmt_ctx_, out);
    av_packet_free(&out);

    if (ret < 0) {
        last_error_ = "av_interleaved_write_frame: " + avErrStrSend(ret);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// SDP
// ---------------------------------------------------------------------------

std::string RtpSender::generateSdp() const
{
    if (!fmt_ctx_) return {};

    // FFmpeg 在写 header 后将 SDP 存入 AVFormatContext::sdp（仅 rtp/rtsp 格式）
    // 通过 av_sdp_create 生成标准 SDP
    char sdp_buf[4096] = {};
    AVFormatContext* ctx_arr[1] = { fmt_ctx_ };
    int ret = av_sdp_create(ctx_arr, 1, sdp_buf, sizeof(sdp_buf));
    if (ret < 0) return {};
    return std::string(sdp_buf);
}

} // namespace strmctrl
