#include "AudioEncoder.h"

#include <iostream>
#include <stdexcept>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

namespace strmctrl {

AudioEncoder::AudioEncoder(AudioConfig cfg)
    : cfg_(std::move(cfg))
{
    pkt_ = av_packet_alloc();
    if (!pkt_) {
        throw std::runtime_error("AudioEncoder: av_packet_alloc failed");
    }
}

AudioEncoder::~AudioEncoder()
{
    close();
    if (pkt_) {
        av_packet_free(&pkt_);
    }
}

void AudioEncoder::setPacketCallback(PacketCallback cb)
{
    packet_cb_ = std::move(cb);
}

bool AudioEncoder::open()
{
    if (isOpen()) {
        last_error_ = "Encoder already open";
        return false;
    }

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

    codec_ctx_->sample_rate = cfg_.sample_rate;
    codec_ctx_->bit_rate = cfg_.bit_rate;

    // 选择编码器支持的采样格式
    if (codec->sample_fmts) {
        codec_ctx_->sample_fmt = codec->sample_fmts[0];
    } else {
        codec_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    }

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    av_channel_layout_default(&codec_ctx_->ch_layout, cfg_.channels);
#else
    codec_ctx_->channels = cfg_.channels;
    codec_ctx_->channel_layout = av_get_default_channel_layout(cfg_.channels);
#endif

    // 允许实验性编码器（如某些 AAC 实现）
    codec_ctx_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        last_error_ = "avcodec_open2 failed";
        avcodec_free_context(&codec_ctx_);
        return false;
    }

    // 初始化 FIFO
    fifo_ = av_audio_fifo_alloc(codec_ctx_->sample_fmt, cfg_.channels, 1);
    if (!fifo_) {
        last_error_ = "av_audio_fifo_alloc failed";
        avcodec_free_context(&codec_ctx_);
        return false;
    }

    next_pts_ = 0;
    return true;
}

void AudioEncoder::flush()
{
    if (!isOpen()) return;

    // 冲出 FIFO 中剩余的数据
    encodeFifo(true);

    // 冲出编码器内部缓存
    avcodec_send_frame(codec_ctx_, nullptr);
    while (true) {
        int ret = avcodec_receive_packet(codec_ctx_, pkt_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            break;
        }

        if (packet_cb_) {
            packet_cb_(pkt_);
        }
        av_packet_unref(pkt_);
    }
}

void AudioEncoder::close()
{
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
    }
    if (swr_ctx_) {
        swr_free(&swr_ctx_);
    }
    if (fifo_) {
        av_audio_fifo_free(fifo_);
        fifo_ = nullptr;
    }
}

bool AudioEncoder::initResampler(const AudioFrame& frame)
{
    if (swr_ctx_) {
        swr_free(&swr_ctx_);
    }

    swr_ctx_ = swr_alloc();
    if (!swr_ctx_) {
        last_error_ = "swr_alloc failed";
        return false;
    }

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    av_opt_set_chlayout(swr_ctx_, "in_chlayout", &frame.avFrame()->ch_layout, 0);
    av_opt_set_int(swr_ctx_, "in_sample_rate", frame.sampleRate(), 0);
    av_opt_set_sample_fmt(swr_ctx_, "in_sample_fmt", frame.format(), 0);

    av_opt_set_chlayout(swr_ctx_, "out_chlayout", &codec_ctx_->ch_layout, 0);
    av_opt_set_int(swr_ctx_, "out_sample_rate", codec_ctx_->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx_, "out_sample_fmt", codec_ctx_->sample_fmt, 0);
#else
    av_opt_set_int(swr_ctx_, "in_channel_layout", frame.avFrame()->channel_layout, 0);
    av_opt_set_int(swr_ctx_, "in_sample_rate", frame.sampleRate(), 0);
    av_opt_set_sample_fmt(swr_ctx_, "in_sample_fmt", frame.format(), 0);

    av_opt_set_int(swr_ctx_, "out_channel_layout", codec_ctx_->channel_layout, 0);
    av_opt_set_int(swr_ctx_, "out_sample_rate", codec_ctx_->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx_, "out_sample_fmt", codec_ctx_->sample_fmt, 0);
#endif

    if (swr_init(swr_ctx_) < 0) {
        last_error_ = "swr_init failed";
        swr_free(&swr_ctx_);
        return false;
    }

    in_sample_rate_ = frame.sampleRate();
    in_sample_fmt_ = frame.format();
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    av_channel_layout_copy(&in_ch_layout_, &frame.avFrame()->ch_layout);
#else
    in_channel_layout_ = frame.avFrame()->channel_layout;
#endif

    return true;
}

bool AudioEncoder::encode(const AudioFrame& frame)
{
    if (!isOpen() || !frame.valid()) return false;

    // 检查是否需要初始化或重新初始化重采样器
    bool need_init = !swr_ctx_ ||
                     in_sample_rate_ != frame.sampleRate() ||
                     in_sample_fmt_ != frame.format();
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    if (!need_init && av_channel_layout_compare(&in_ch_layout_, &frame.avFrame()->ch_layout) != 0) {
        need_init = true;
    }
#else
    if (!need_init && in_channel_layout_ != frame.avFrame()->channel_layout) {
        need_init = true;
    }
#endif

    if (need_init) {
        if (!initResampler(frame)) {
            return false;
        }
    }

    // 分配重采样后的临时缓冲区
    int out_samples = av_rescale_rnd(swr_get_delay(swr_ctx_, frame.sampleRate()) + frame.nbSamples(),
                                     codec_ctx_->sample_rate, frame.sampleRate(), AV_ROUND_UP);
    
    uint8_t** resampled_data = nullptr;
    int resampled_linesize = 0;
    if (av_samples_alloc_array_and_samples(&resampled_data, &resampled_linesize, cfg_.channels,
                                           out_samples, codec_ctx_->sample_fmt, 0) < 0) {
        last_error_ = "av_samples_alloc_array_and_samples failed";
        return false;
    }

    // 执行重采样
    int converted_samples = swr_convert(swr_ctx_, resampled_data, out_samples,
                                        (const uint8_t**)frame.avFrame()->extended_data, frame.nbSamples());
    if (converted_samples < 0) {
        last_error_ = "swr_convert failed";
        av_freep(&resampled_data[0]);
        av_freep(&resampled_data);
        return false;
    }

    // 写入 FIFO
    if (av_audio_fifo_space(fifo_) < converted_samples) {
        if (av_audio_fifo_realloc(fifo_, av_audio_fifo_size(fifo_) + converted_samples) < 0) {
            last_error_ = "av_audio_fifo_realloc failed";
            av_freep(&resampled_data[0]);
            av_freep(&resampled_data);
            return false;
        }
    }

    if (av_audio_fifo_write(fifo_, (void**)resampled_data, converted_samples) < converted_samples) {
        last_error_ = "av_audio_fifo_write failed";
        av_freep(&resampled_data[0]);
        av_freep(&resampled_data);
        return false;
    }

    av_freep(&resampled_data[0]);
    av_freep(&resampled_data);

    // 从 FIFO 中读取数据并编码
    return encodeFifo(false);
}

bool AudioEncoder::encodeFifo(bool flush)
{
    int frame_size = codec_ctx_->frame_size;
    if (frame_size == 0) {
        frame_size = 1024; // 默认值，某些编码器可能为 0
    }

    while (av_audio_fifo_size(fifo_) >= frame_size || (flush && av_audio_fifo_size(fifo_) > 0)) {
        int current_frame_size = std::min(av_audio_fifo_size(fifo_), frame_size);

        AVFrame* enc_frame = av_frame_alloc();
        enc_frame->nb_samples = current_frame_size;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        av_channel_layout_copy(&enc_frame->ch_layout, &codec_ctx_->ch_layout);
#else
        enc_frame->channel_layout = codec_ctx_->channel_layout;
        enc_frame->channels = codec_ctx_->channels;
#endif
        enc_frame->format = codec_ctx_->sample_fmt;
        enc_frame->sample_rate = codec_ctx_->sample_rate;

        if (av_frame_get_buffer(enc_frame, 0) < 0) {
            last_error_ = "av_frame_get_buffer failed";
            av_frame_free(&enc_frame);
            return false;
        }

        if (av_audio_fifo_read(fifo_, (void**)enc_frame->data, current_frame_size) < current_frame_size) {
            last_error_ = "av_audio_fifo_read failed";
            av_frame_free(&enc_frame);
            return false;
        }

        enc_frame->pts = next_pts_;
        next_pts_ += current_frame_size;

        int ret = avcodec_send_frame(codec_ctx_, enc_frame);
        av_frame_free(&enc_frame);

        if (ret < 0) {
            last_error_ = "avcodec_send_frame failed";
            return false;
        }

        while (true) {
            ret = avcodec_receive_packet(codec_ctx_, pkt_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                last_error_ = "avcodec_receive_packet failed";
                return false;
            }

            if (packet_cb_) {
                packet_cb_(pkt_);
            }
            av_packet_unref(pkt_);
        }
    }

    return true;
}

} // namespace strmctrl
