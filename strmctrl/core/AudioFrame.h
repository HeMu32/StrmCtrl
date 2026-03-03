#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

namespace strmctrl {

/**
 * @brief 解码后的音频帧，对 AVFrame 进行 RAII 封装。
 *
 * AudioFrame 拥有其内部 AVFrame 的所有权。通过移动语义传递；
 * 若需在 callback 之外保留帧数据，请调用 clone()。
 */
class AudioFrame {
public:
    // -----------------------------------------------------------------------
    // 构造 / 析构
    // -----------------------------------------------------------------------

    /**
     * @brief 默认构造空帧（frame_ == nullptr）。
     */
    AudioFrame() = default;

    /**
     * @brief 接管一个已分配的 AVFrame 的所有权。
     * @param frame  由 av_frame_alloc() 分配、已填充数据的帧指针；
     *               构造后调用方不得再释放该指针。
     */
    explicit AudioFrame(AVFrame* frame) noexcept
        : frame_(frame, &AudioFrame::freeFrame)
    {}

    // 禁止拷贝，允许移动
    AudioFrame(const AudioFrame&)            = delete;
    AudioFrame& operator=(const AudioFrame&) = delete;
    AudioFrame(AudioFrame&&)                 = default;
    AudioFrame& operator=(AudioFrame&&)      = default;

    ~AudioFrame() = default;

    // -----------------------------------------------------------------------
    // 属性访问
    // -----------------------------------------------------------------------

    /** @brief 采样率（Hz），无效帧返回 0。 */
    int sampleRate() const noexcept { return frame_ ? frame_->sample_rate : 0; }

    /** @brief 单通道采样点数量，无效帧返回 0。 */
    int nbSamples() const noexcept { return frame_ ? frame_->nb_samples : 0; }

    /** @brief 声道数，无效帧返回 0。 */
    int channels() const noexcept
    {
        if (!frame_) return 0;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        return frame_->ch_layout.nb_channels;
#else
        return frame_->channels;
#endif
    }

    /** @brief 采样格式（AVSampleFormat），无效帧返回 AV_SAMPLE_FMT_NONE。 */
    AVSampleFormat format() const noexcept
    {
        return frame_ ? static_cast<AVSampleFormat>(frame_->format)
                      : AV_SAMPLE_FMT_NONE;
    }

    /**
     * @brief 帧的显示时间戳（Presentation Timestamp）。
     *
     * 单位取决于所在流的 time_base；若未设置则为 AV_NOPTS_VALUE。
     */
    int64_t pts() const noexcept { return frame_ ? frame_->pts : AV_NOPTS_VALUE; }

    /** @brief 返回底层 AVFrame 指针（不转移所有权）。 */
    const AVFrame* avFrame() const noexcept { return frame_.get(); }

    /** @brief 检查帧是否有效（持有非空 AVFrame）。 */
    bool valid() const noexcept { return frame_ != nullptr; }

    // -----------------------------------------------------------------------
    // 深拷贝
    // -----------------------------------------------------------------------

    /**
     * @brief 深拷贝当前帧，返回独立的 AudioFrame 实例。
     *
     * 用于在 callback 中将帧移出接收线程生命周期。
     * @throws std::runtime_error 若内存分配失败。
     */
    AudioFrame clone() const
    {
        if (!frame_) return AudioFrame{};

        AVFrame* copy = av_frame_alloc();
        if (!copy) {
            throw std::runtime_error("AudioFrame::clone: av_frame_alloc failed");
        }

        copy->format = frame_->format;
        copy->sample_rate = frame_->sample_rate;
        copy->nb_samples = frame_->nb_samples;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        av_channel_layout_copy(&copy->ch_layout, &frame_->ch_layout);
#else
        copy->channel_layout = frame_->channel_layout;
        copy->channels = frame_->channels;
#endif

        if (av_frame_get_buffer(copy, 0) < 0) {
            av_frame_free(&copy);
            throw std::runtime_error("AudioFrame::clone: av_frame_get_buffer failed");
        }

        if (av_frame_copy(copy, frame_.get()) < 0) {
            av_frame_free(&copy);
            throw std::runtime_error("AudioFrame::clone: av_frame_copy failed");
        }

        if (av_frame_copy_props(copy, frame_.get()) < 0) {
            av_frame_free(&copy);
            throw std::runtime_error("AudioFrame::clone: av_frame_copy_props failed");
        }

        return AudioFrame(copy);
    }

private:
    static void freeFrame(AVFrame* f) noexcept
    {
        if (f) av_frame_free(&f);
    }

    std::unique_ptr<AVFrame, decltype(&AudioFrame::freeFrame)> frame_{nullptr, &AudioFrame::freeFrame};
};

} // namespace strmctrl
