#pragma once

#include <functional>
#include <memory>
#include <string>

#include "AudioConfig.h"
#include "../core/AudioFrame.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
}

namespace strmctrl {

/**
 * @brief 音频编码器，将原始 AudioFrame 编码为压缩的 AVPacket。
 *
 * 内部使用 FFmpeg avcodec，编码器由 AudioConfig::codec_name 动态选择。
 * 包含 AVAudioFifo 以处理编码器对固定帧大小（如 AAC 的 1024 采样点）的要求。
 * 包含 SwrContext 以处理采样率、声道数、采样格式的重采样。
 */
class AudioEncoder {
public:
    using PacketCallback = std::function<void(AVPacket* pkt)>;

    explicit AudioEncoder(AudioConfig cfg);
    ~AudioEncoder();

    AudioEncoder(const AudioEncoder&)            = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    void setPacketCallback(PacketCallback cb);

    bool open();
    void flush();
    void close();

    bool isOpen() const noexcept { return codec_ctx_ != nullptr; }

    /**
     * @brief 编码一帧音频。
     *
     * 输入帧会被重采样并放入 FIFO。当 FIFO 数据量达到编码器要求的 frame_size 时，
     * 会触发一次或多次编码，并通过 PacketCallback 输出。
     *
     * @param frame 原始音频帧
     * @return true 表示成功；false 表示失败
     */
    bool encode(const AudioFrame& frame);

    AVCodecContext* codecContext() const noexcept { return codec_ctx_; }
    std::string lastError() const noexcept { return last_error_; }

private:
    bool initResampler(const AudioFrame& frame);
    bool encodeFifo(bool flush);

    AudioConfig cfg_;
    PacketCallback packet_cb_;

    AVCodecContext* codec_ctx_ = nullptr;
    AVPacket* pkt_ = nullptr;

    SwrContext* swr_ctx_ = nullptr;
    AVAudioFifo* fifo_ = nullptr;

    // 记录输入音频的格式，用于检测是否需要重新初始化重采样器
    int in_sample_rate_ = 0;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    AVChannelLayout in_ch_layout_ = {};
#else
    uint64_t in_channel_layout_ = 0;
#endif
    AVSampleFormat in_sample_fmt_ = AV_SAMPLE_FMT_NONE;

    int64_t next_pts_ = 0; // 用于生成连续的 PTS

    std::string last_error_;
};

} // namespace strmctrl
