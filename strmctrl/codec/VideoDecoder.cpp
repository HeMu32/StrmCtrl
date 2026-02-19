#include "VideoDecoder.h"

extern "C" {
#include <libavutil/error.h>
}

namespace strmctrl {

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------
static std::string avErrStrDec(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// 析构
// ---------------------------------------------------------------------------

VideoDecoder::~VideoDecoder()
{
    close();
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

void VideoDecoder::setFrameCallback(VideoFrameCallback cb)
{
    frame_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool VideoDecoder::openWithParameters(const AVCodecParameters* par)
{
    if (!par) {
        last_error_ = "openWithParameters: null AVCodecParameters";
        return false;
    }

    const AVCodec* codec = nullptr;
    // Prefer libopenh264 for H.264 if available (fall back to default decoder otherwise).
    if (par->codec_id == AV_CODEC_ID_H264) {
        codec = avcodec_find_decoder_by_name("libopenh264");
        if (!codec) {
            codec = avcodec_find_decoder(par->codec_id);
        }
    } else {
        codec = avcodec_find_decoder(par->codec_id);
    }

    if (!codec) {
        last_error_ = "Decoder not found for codec_id=" +
                      std::to_string(static_cast<int>(par->codec_id));
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        last_error_ = "avcodec_alloc_context3 failed";
        return false;
    }

    int ret = avcodec_parameters_to_context(codec_ctx_, par);
    if (ret < 0) {
        last_error_ = "avcodec_parameters_to_context: " + avErrStrDec(ret);
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
        return false;
    }

    return openContext(codec);
}

bool VideoDecoder::openWithCodecId(AVCodecID codec_id)
{
    const AVCodec* codec = avcodec_find_decoder(codec_id);
    if (!codec) {
        last_error_ = "Decoder not found for codec_id=" +
                      std::to_string(static_cast<int>(codec_id));
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        last_error_ = "avcodec_alloc_context3 failed";
        return false;
    }

    return openContext(codec);
}

void VideoDecoder::flush()
{
    if (!codec_ctx_) return;

    int ret = avcodec_send_packet(codec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        last_error_ = "flush avcodec_send_packet: " + avErrStrDec(ret);
        return;
    }
    receiveFrames();
}

void VideoDecoder::close()
{
    flush();
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
    }
}

// ---------------------------------------------------------------------------
// 解码
// ---------------------------------------------------------------------------

bool VideoDecoder::decode(AVPacket* pkt)
{
    if (!codec_ctx_) {
        last_error_ = "Decoder not open";
        return false;
    }

    int ret = avcodec_send_packet(codec_ctx_, pkt);
    if (ret < 0) {
        // EAGAIN 表示当前不接受更多输入，需先 receiveFrames
        if (ret == AVERROR(EAGAIN)) {
            if (!receiveFrames()) return false;
            ret = avcodec_send_packet(codec_ctx_, pkt);
        }
        if (ret < 0 && ret != AVERROR_EOF) {
            last_error_ = "avcodec_send_packet: " + avErrStrDec(ret);
            return false;
        }
    }

    return receiveFrames();
}

// ---------------------------------------------------------------------------
// 私有辅助
// ---------------------------------------------------------------------------

bool VideoDecoder::receiveFrames()
{
    int ret = 0;
    while (true) {
        AVFrame* raw = av_frame_alloc();
        if (!raw) {
            last_error_ = "av_frame_alloc failed";
            return false;
        }

        ret = avcodec_receive_frame(codec_ctx_, raw);
        if (ret == 0) {
            // 成功解码一帧
            if (frame_cb_) {
                VideoFrame vf(raw);   // VideoFrame 接管 raw 的所有权
                frame_cb_(vf);
                // vf 析构时自动释放 raw
            } else {
                av_frame_free(&raw);
            }
        } else {
            av_frame_free(&raw);
            break;
        }
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;

    last_error_ = "avcodec_receive_frame: " + avErrStrDec(ret);
    return false;
}

bool VideoDecoder::openContext(const AVCodec* codec)
{
    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        last_error_ = "avcodec_open2: " + avErrStrDec(ret);
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
        return false;
    }
    return true;
}

} // namespace strmctrl
