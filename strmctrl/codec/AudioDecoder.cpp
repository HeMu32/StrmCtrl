#include "AudioDecoder.h"

#include <stdexcept>

namespace strmctrl {

AudioDecoder::AudioDecoder()
{
    frame_ = av_frame_alloc();
    if (!frame_) {
        throw std::runtime_error("AudioDecoder: av_frame_alloc failed");
    }
}

AudioDecoder::~AudioDecoder()
{
    close();
    if (frame_) {
        av_frame_free(&frame_);
    }
}

bool AudioDecoder::openWithParameters(const AVCodecParameters* par)
{
    if (isOpen()) {
        last_error_ = "Decoder already open";
        return false;
    }

    if (!par) {
        last_error_ = "Invalid AVCodecParameters";
        return false;
    }

    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        last_error_ = "Decoder not found for codec_id: " + std::to_string(par->codec_id);
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        last_error_ = "avcodec_alloc_context3 failed";
        return false;
    }

    if (avcodec_parameters_to_context(codec_ctx_, par) < 0) {
        last_error_ = "avcodec_parameters_to_context failed";
        avcodec_free_context(&codec_ctx_);
        return false;
    }

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        last_error_ = "avcodec_open2 failed";
        avcodec_free_context(&codec_ctx_);
        return false;
    }

    return true;
}

void AudioDecoder::close()
{
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
    }
}

bool AudioDecoder::decode(const AVPacket* pkt, AudioFrame& out_frame)
{
    if (!isOpen()) return false;

    int ret = avcodec_send_packet(codec_ctx_, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        last_error_ = "avcodec_send_packet failed";
        return false;
    }

    ret = avcodec_receive_frame(codec_ctx_, frame_);
    if (ret == 0) {
        // 成功解码出一帧
        AVFrame* copy = av_frame_alloc();
        if (!copy) {
            last_error_ = "av_frame_alloc failed";
            return false;
        }

        av_frame_move_ref(copy, frame_);
        out_frame = AudioFrame(copy);
        return true;
    } else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        // 需要更多数据或已结束
        return false;
    } else {
        last_error_ = "avcodec_receive_frame failed";
        return false;
    }
}

} // namespace strmctrl
