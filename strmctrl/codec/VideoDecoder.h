#pragma once

#include <functional>
#include <memory>
#include <string>

#include "../core/Callbacks.h"
#include "../core/VideoFrame.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace strmctrl {

/**
 * @brief 视频解码器，将压缩的 AVPacket 解码为原始 VideoFrame。
 *
 * 通过 AVCodecID 或编解码器名称选择解码器，典型场景下由
 * RtpReceiver 在收到流信息后自动创建并传入 codec context 参数。
 *
 * 解码结果通过 VideoFrameCallback 同步回调（在调用 decode() 的线程中）。
 *
 * ### 典型用法
 * @code
 * strmctrl::VideoDecoder dec;
 * dec.setFrameCallback([](const strmctrl::VideoFrame& frame) {
 *     // 处理解码帧...
 * });
 * dec.openWithParameters(codec_par);   // 由 RtpReceiver 调用
 *
 * // 循环中调用：
 * dec.decode(pkt);
 *
 * dec.flush();   // 流结束
 * dec.close();
 * @endcode
 */
class VideoDecoder {
public:
    // -----------------------------------------------------------------------
    // 构造 / 析构
    // -----------------------------------------------------------------------

    VideoDecoder() = default;
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&)            = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // -----------------------------------------------------------------------
    // 配置
    // -----------------------------------------------------------------------

    /**
     * @brief 注册解码帧输出 callback。
     * @param cb  每产出一个解码帧时调用（在 decode() 调用线程中同步触发）
     */
    void setFrameCallback(VideoFrameCallback cb);

    // -----------------------------------------------------------------------
    // 生命周期
    // -----------------------------------------------------------------------

    /**
     * @brief 使用 AVCodecParameters 打开解码器（由 RtpReceiver 调用）。
     *
     * 这是最常用的打开方式：从 AVFormatContext 的流信息中直接获取参数。
     * @param par   流的编解码器参数（不转移所有权）
     * @return      true 表示成功；false 表示解码器不存在或参数有误（见 lastError()）
     */
    bool openWithParameters(const AVCodecParameters* par);

    /**
     * @brief 使用编解码器 ID 打开解码器。
     * @param codec_id  AVCodecID，例如 AV_CODEC_ID_H264
     * @return          true 表示成功
     */
    bool openWithCodecId(AVCodecID codec_id);

    /**
     * @brief 冲出解码器缓存中的剩余帧（流结束时调用）。
     */
    void flush();

    /**
     * @brief 关闭解码器并释放 FFmpeg 资源。
     */
    void close();

    /** @brief 解码器是否已成功打开。 */
    bool isOpen() const noexcept { return codec_ctx_ != nullptr; }

    // -----------------------------------------------------------------------
    // 解码
    // -----------------------------------------------------------------------

    /**
     * @brief 送入一个压缩包进行解码。
     *
     * 产出的 VideoFrame 通过 VideoFrameCallback 同步返回。
     * @param pkt  压缩包（调用方保留所有权；传 nullptr 等效于 flush）
     * @return     true 表示成功；false 表示内部错误（见 lastError()）
     */
    bool decode(AVPacket* pkt);

    // -----------------------------------------------------------------------
    // 访问器
    // -----------------------------------------------------------------------

    /** @brief 返回最近一次错误的描述字符串。 */
    const std::string& lastError() const noexcept { return last_error_; }

    /** @brief 返回解码器上下文（只读）。 */
    const AVCodecContext* codecContext() const noexcept { return codec_ctx_; }

private:
    // 内部：从 codec_ctx_ 循环接收已解码的帧
    bool receiveFrames();

    // 内部：打开已分配好的 codec_ctx_
    bool openContext(const AVCodec* codec);

    AVCodecContext*  codec_ctx_  = nullptr;
    VideoFrameCallback frame_cb_;
    std::string        last_error_;
};

} // namespace strmctrl
