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

int RtpSender::addStream(const std::string&    dest_host,
                         int                   dest_port,
                         const AVCodecContext* codec_ctx)
{
    if (!codec_ctx) {
        last_error_ = "RtpSender::addStream: null codec_ctx";
        return -1;
    }

    if (is_open_) {
        last_error_ = "RtpSender::addStream: already open";
        return -1;
    }

    StreamContext ctx;
    ctx.url = "rtp://" + dest_host + ":" + std::to_string(dest_port);

    // 分配输出格式上下文（rtp 格式）
    int ret = avformat_alloc_output_context2(&ctx.fmt_ctx, nullptr, "rtp", ctx.url.c_str());
    if (ret < 0 || !ctx.fmt_ctx) {
        last_error_ = "avformat_alloc_output_context2: " + avErrStrSend(ret);
        return -1;
    }

    // 添加流
    ctx.stream = avformat_new_stream(ctx.fmt_ctx, nullptr);
    if (!ctx.stream) {
        last_error_ = "avformat_new_stream failed";
        avformat_free_context(ctx.fmt_ctx);
        return -1;
    }

    // 从编码器上下文复制参数到流
    ret = avcodec_parameters_from_context(ctx.stream->codecpar, codec_ctx);
    if (ret < 0) {
        last_error_ = "avcodec_parameters_from_context: " + avErrStrSend(ret);
        avformat_free_context(ctx.fmt_ctx);
        return -1;
    }

    ctx.stream->time_base = codec_ctx->time_base;
    ctx.enc_time_base = codec_ctx->time_base;
    ctx.last_dts = AV_NOPTS_VALUE;
    ctx.pkt_index = 0;

    streams_.push_back(std::move(ctx));
    return static_cast<int>(streams_.size() - 1);
}

bool RtpSender::open()
{
    if (is_open_) {
        last_error_ = "RtpSender::open: already open";
        return false;
    }

    for (std::size_t i = 0; i < streams_.size(); ++i) {
        auto& ctx = streams_[i];
        std::string open_url = ctx.url;

        // 可选：为发送端显式指定本地 RTP/RTCP 端口，避免同机时与接收端冲突。
        if (local_port_base_ > 0) {
            const int local_rtp_port  = local_port_base_ + static_cast<int>(i) * 2;
            const int local_rtcp_port = local_rtp_port + 1;
            open_url += "?localrtpport=" + std::to_string(local_rtp_port)
                     +  "&localrtcpport=" + std::to_string(local_rtcp_port);
        }

        // 打开 UDP 输出 IO
        int ret = avio_open(&ctx.fmt_ctx->pb, open_url.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            last_error_ = "avio_open(" + open_url + "): " + avErrStrSend(ret);
            close();
            return false;
        }

        // 写文件头（对 RTP 来说会触发 SDP 生成）
        ret = avformat_write_header(ctx.fmt_ctx, nullptr);
        if (ret < 0) {
            last_error_ = "avformat_write_header: " + avErrStrSend(ret);
            close();
            return false;
        }
    }

    is_open_ = true;
    return true;
}

void RtpSender::close()
{
    for (auto& ctx : streams_) {
        if (ctx.fmt_ctx) {
            if (ctx.fmt_ctx->pb) {
                av_write_trailer(ctx.fmt_ctx);
                avio_closep(&ctx.fmt_ctx->pb);
            }
            avformat_free_context(ctx.fmt_ctx);
            ctx.fmt_ctx = nullptr;
        }
    }
    streams_.clear();
    is_open_ = false;
}

// ---------------------------------------------------------------------------
// 推流
// ---------------------------------------------------------------------------

bool RtpSender::sendPacket(AVPacket* pkt, int stream_index)
{
    if (!is_open_ || !pkt) return false;
    if (stream_index < 0 || stream_index >= static_cast<int>(streams_.size())) {
        last_error_ = "RtpSender::sendPacket: invalid stream_index";
        return false;
    }

    auto& ctx = streams_[stream_index];

    // 复制包，避免修改原始包
    AVPacket* out_pkt = av_packet_clone(pkt);
    if (!out_pkt) {
        last_error_ = "av_packet_clone failed";
        return false;
    }

    // 转换时间基
    av_packet_rescale_ts(out_pkt, ctx.enc_time_base, ctx.stream->time_base);
    out_pkt->stream_index = ctx.stream->index;

    // 强制单调递增 DTS
    if (ctx.last_dts != AV_NOPTS_VALUE && out_pkt->dts <= ctx.last_dts) {
        int64_t diff = ctx.last_dts - out_pkt->dts + 1;
        out_pkt->dts += diff;
        out_pkt->pts += diff;
    }
    ctx.last_dts = out_pkt->dts;

    // 写入
    int ret = av_interleaved_write_frame(ctx.fmt_ctx, out_pkt);
    av_packet_free(&out_pkt);

    if (ret < 0) {
        last_error_ = "av_interleaved_write_frame: " + avErrStrSend(ret);
        return false;
    }

    ctx.pkt_index++;
    return true;
}

// ---------------------------------------------------------------------------
// SDP
// ---------------------------------------------------------------------------

std::string RtpSender::generateSdp() const
{
    if (!is_open_ || streams_.empty()) return "";

    std::vector<AVFormatContext*> ctxs;
    for (const auto& ctx : streams_) {
        ctxs.push_back(ctx.fmt_ctx);
    }

    char sdp_buf[4096] = {};
    int ret = av_sdp_create(ctxs.data(), static_cast<int>(ctxs.size()), sdp_buf, sizeof(sdp_buf));
    if (ret < 0) {
        return "";
    }

    return std::string(sdp_buf);
}

} // namespace strmctrl
