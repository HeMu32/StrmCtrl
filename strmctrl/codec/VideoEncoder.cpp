#include "VideoEncoder.h"

#include <cstring>
#include <stdexcept>
#include <iostream>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
}

namespace strmctrl {

// ---------------------------------------------------------------------------
// 辅助：将 FFmpeg 错误码转为字符串
// ---------------------------------------------------------------------------
static std::string avErrStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

VideoEncoder::VideoEncoder(CodecConfig cfg)
    : cfg_(std::move(cfg))
{}

VideoEncoder::~VideoEncoder()
{
    close();
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

void VideoEncoder::setPacketCallback(PacketCallback cb)
{
    pkt_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool VideoEncoder::open()
{
    // 查找编码器
    const AVCodec* codec = avcodec_find_encoder_by_name(cfg_.codec_name.c_str());
    if (!codec) {
        last_error_ = "Encoder not found: " + cfg_.codec_name;
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        last_error_ = "avcodec_alloc_context3 failed";
        return false;
    }

    // 填写基础编码参数
    codec_ctx_->width     = cfg_.width;
    codec_ctx_->height    = cfg_.height;
    codec_ctx_->time_base = { 1, cfg_.fps };
    codec_ctx_->framerate = { cfg_.fps, 1 };
    codec_ctx_->bit_rate  = static_cast<int64_t>(cfg_.bitrate_kbps) * 1000;
    codec_ctx_->pix_fmt   = cfg_.pixel_format;

    // 对于 RTP 推流，强制关键帧间隔合理
    codec_ctx_->gop_size = cfg_.fps * 2;  // 每 2 秒一个 IDR

    // 应用 extra_opts
    for (auto& [key, val] : cfg_.extra_opts) {
        if (av_opt_set(codec_ctx_->priv_data, key.c_str(), val.c_str(), 0) < 0) {
            // 非致命：某些选项在特定编码器下可能不存在，仅打印警告
            // （不中断初始化）
        }
    }

    // 打开编码器
    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        last_error_ = "avcodec_open2 failed: " + avErrStr(ret);
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
        return false;
    }

    return true;
}

void VideoEncoder::flush()
{
    if (!codec_ctx_) return;

    // 送入空帧以冲出缓存
    int ret = avcodec_send_frame(codec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        last_error_ = "flush avcodec_send_frame: " + avErrStr(ret);
        return;
    }
    receivePackets();
}

void VideoEncoder::close()
{
    flush();

    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (sws_frame_) {
        av_frame_free(&sws_frame_);
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
    }
}

// ---------------------------------------------------------------------------
// 编码
// ---------------------------------------------------------------------------

bool VideoEncoder::encode(const AVFrame* frame)
{
    if (!codec_ctx_) {
        last_error_ = "Encoder not open";
        return false;
    }

    const AVFrame* input_frame = frame;

    // 如果输入帧的格式或尺寸与配置不符，先做 swscale 转换
    if (frame &&
        (frame->format != cfg_.pixel_format ||
         frame->width  != cfg_.width        ||
         frame->height != cfg_.height))
    {
        if (!ensureSwsContext(frame->width, frame->height,
                              static_cast<AVPixelFormat>(frame->format)))
        {
            return false;
        }

        // 必须调用 av_frame_make_writable 确保 sws_frame_ 的 buffer 是可写的
        // 否则在多次调用 sws_scale 时，如果底层 buffer 被编码器引用，会导致崩溃或数据损坏
        int ret = av_frame_make_writable(sws_frame_);
        if (ret < 0) {
            last_error_ = "av_frame_make_writable failed: " + avErrStr(ret);
            return false;
        }

        sws_scale(sws_ctx_,
                  (const uint8_t * const *)frame->data, frame->linesize, 0, frame->height,
                  sws_frame_->data, sws_frame_->linesize);

        sws_frame_->pts = frame->pts;
        input_frame = sws_frame_;
    }

    int ret = avcodec_send_frame(codec_ctx_, input_frame);
    if (ret < 0) {
        last_error_ = "avcodec_send_frame: " + avErrStr(ret);
        return false;
    }

    return receivePackets();
}

// ---------------------------------------------------------------------------
// 私有辅助
// ---------------------------------------------------------------------------

bool VideoEncoder::receivePackets()
{
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        last_error_ = "av_packet_alloc failed";
        return false;
    }

    static int64_t s_packet_count = 0;

    int ret = 0;
    while ((ret = avcodec_receive_packet(codec_ctx_, pkt)) == 0) {
        if (pkt_cb_) pkt_cb_(pkt);
        ++s_packet_count;
        if (s_packet_count % 200 == 1) {
            std::cout << "[VideoEncoder] output packets=" << s_packet_count
                      << " pts=" << pkt->pts
                      << " dts=" << pkt->dts
                      << " size=" << pkt->size
                      << "\n";
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;

    last_error_ = "avcodec_receive_packet: " + avErrStr(ret);
    return false;
}

bool VideoEncoder::ensureSwsContext(int src_w, int src_h, AVPixelFormat src_fmt)
{
    // 如果已存在且参数一致，直接复用
    if (sws_ctx_ && sws_frame_ &&
        sws_frame_->width  == cfg_.width   &&
        sws_frame_->height == cfg_.height  &&
        src_fmt == static_cast<AVPixelFormat>(sws_frame_->format))
    {
        return true;
    }

    // 释放旧资源
    if (sws_ctx_)   { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (sws_frame_) { av_frame_free(&sws_frame_); }

    sws_ctx_ = sws_getContext(
        src_w, src_h, src_fmt,
        cfg_.width, cfg_.height, cfg_.pixel_format,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!sws_ctx_) {
        last_error_ = "sws_getContext failed";
        return false;
    }

    sws_frame_ = av_frame_alloc();
    if (!sws_frame_) {
        last_error_ = "av_frame_alloc (sws) failed";
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
        return false;
    }

    sws_frame_->format = cfg_.pixel_format;
    sws_frame_->width  = cfg_.width;
    sws_frame_->height = cfg_.height;

    int ret = av_frame_get_buffer(sws_frame_, 32);
    if (ret < 0) {
        last_error_ = "av_frame_get_buffer (sws) failed: " + avErrStr(ret);
        av_frame_free(&sws_frame_);
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
        return false;
    }

    return true;
}

} // namespace strmctrl
