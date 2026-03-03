#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

namespace strmctrl {

/**
 * @brief 解码后的视频帧，对 AVFrame 进行 RAII 封装。
 *
 * VideoFrame 拥有其内部 AVFrame 的所有权。通过移动语义传递；
 * 若需在 callback 之外保留帧数据，请调用 clone()。
 *
 * 典型用法（在 VideoFrameCallback 中）：
 * @code
 * slave.setVideoFrameCallback([&](const strmctrl::VideoFrame& frame) {
 *     // 快速拷贝后入队，不要在此处做耗时操作
 *     queue.push(frame.clone());
 * });
 * @endcode
 */
class VideoFrame {
public:
    // -----------------------------------------------------------------------
    // 构造 / 析构
    // -----------------------------------------------------------------------

    /**
     * @brief 默认构造空帧（frame_ == nullptr）。
     */
    VideoFrame() = default;

    /**
     * @brief 接管一个已分配的 AVFrame 的所有权。
     * @param frame  由 av_frame_alloc() 分配、已填充数据的帧指针；
     *               构造后调用方不得再释放该指针。
     */
    explicit VideoFrame(AVFrame* frame) noexcept
        : frame_(frame, &VideoFrame::freeFrame)
    {}

    // 禁止拷贝，允许移动
    VideoFrame(const VideoFrame&)            = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    VideoFrame(VideoFrame&&)                 = default;
    VideoFrame& operator=(VideoFrame&&)      = default;

    ~VideoFrame() = default;

    // -----------------------------------------------------------------------
    // 属性访问
    // -----------------------------------------------------------------------

    /** @brief 帧宽度（像素），无效帧返回 0。 */
    int width()  const noexcept { return frame_ ? frame_->width  : 0; }

    /** @brief 帧高度（像素），无效帧返回 0。 */
    int height() const noexcept { return frame_ ? frame_->height : 0; }

    /** @brief 像素格式（AVPixelFormat），无效帧返回 AV_PIX_FMT_NONE。 */
    AVPixelFormat format() const noexcept
    {
        return frame_ ? static_cast<AVPixelFormat>(frame_->format)
                      : AV_PIX_FMT_NONE;
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
     * @brief 深拷贝当前帧，返回独立的 VideoFrame 实例。
     *
     * 用于在 callback 中将帧移出接收线程生命周期。
     * @throws std::runtime_error 若内存分配失败。
     */
    VideoFrame clone() const
    {
        if (!frame_) return VideoFrame{};

        AVFrame* copy = av_frame_alloc();
        if (!copy) throw std::runtime_error("VideoFrame::clone: av_frame_alloc failed");

        if (av_frame_ref(copy, frame_.get()) < 0) 
        {
            av_frame_free(&copy);
            throw std::runtime_error("VideoFrame::clone: av_frame_ref failed");
        }
        return VideoFrame{copy};
    }

private:
    static void freeFrame(AVFrame* f)
    {
        if (f) av_frame_free(&f);
    }

    std::unique_ptr<AVFrame, decltype(&VideoFrame::freeFrame)>
        frame_{nullptr, &VideoFrame::freeFrame};
};

} // namespace strmctrl
