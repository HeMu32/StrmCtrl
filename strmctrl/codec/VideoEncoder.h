#pragma once

#include <functional>
#include <memory>
#include <string>

#include "CodecConfig.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace strmctrl {

/**
 * @brief 视频编码器，将原始 AVFrame 编码为压缩的 AVPacket。
 *
 * 内部使用 FFmpeg avcodec，编码器由 CodecConfig::codec_name 动态选择，
 * 支持软件编码（libopenh264）及后续扩展的硬件编码器（QSV/NVENC/AMF）。
 *
 * 若输入帧的像素格式或尺寸与 CodecConfig 不匹配，VideoEncoder 会
 * 通过 libswscale 自动进行格式转换与缩放。
 *
 * ### 典型用法
 * @code
 * auto cfg = strmctrl::CodecConfig::makeOpenH264(1280, 720, 30, 2000);
 * strmctrl::VideoEncoder enc(cfg);
 *
 * enc.setPacketCallback([](AVPacket* pkt) {
 *     // pkt 生命周期仅在此 callback 内有效
 *     rtp_sender->sendPacket(pkt);
 * });
 *
 * enc.open();
 * enc.encode(raw_frame);  // 可多次调用
 * enc.flush();            // 编码结束时调用，冲出缓存帧
 * enc.close();
 * @endcode
 */
class VideoEncoder {
public:
    /**
     * @brief 编码包回调类型。
     * @note  callback 在 encode() / flush() 的调用线程中同步触发，不跨线程。
     *        pkt 的生命周期仅在 callback 执行期间有效；如需保留，应调用
     *        av_packet_ref() / av_packet_clone()。
     */
    using PacketCallback = std::function<void(AVPacket* pkt)>;

    // -----------------------------------------------------------------------
    // 构造 / 析构
    // -----------------------------------------------------------------------

    /**
     * @brief 构造编码器（不立即打开，需调用 open()）。
     * @param cfg  编解码器配置
     */
    explicit VideoEncoder(CodecConfig cfg);

    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&)            = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // -----------------------------------------------------------------------
    // 配置
    // -----------------------------------------------------------------------

    /**
     * @brief 注册编码包输出 callback。
     * @param cb  每产出一个 AVPacket 时调用一次
     */
    void setPacketCallback(PacketCallback cb);

    // -----------------------------------------------------------------------
    // 生命周期
    // -----------------------------------------------------------------------

    /**
     * @brief 打开编码器，根据 CodecConfig 初始化 AVCodecContext。
     * @return true 表示成功；false 表示编码器不存在或参数有误（见 lastError()）
     */
    bool open();

    /**
     * @brief 冲出编码器缓存中的剩余帧（编码结束时调用）。
     */
    void flush();

    /**
     * @brief 关闭编码器并释放 FFmpeg 资源。
     */
    void close();

    /** @brief 编码器是否已成功打开。 */
    bool isOpen() const noexcept { return codec_ctx_ != nullptr; }

    // -----------------------------------------------------------------------
    // 编码
    // -----------------------------------------------------------------------

    /**
     * @brief 送入一帧原始视频帧进行编码。
     *
     * 若帧的像素格式或尺寸与配置不匹配，会先经 swscale 转换。
     * 产出的 AVPacket 通过 PacketCallback 同步返回。
     *
     * @param frame  原始帧（调用方保留所有权）
     * @return       true 表示帧成功送入编码器；false 表示编码器未打开或内部错误
     */
    bool encode(AVFrame* frame);

    // -----------------------------------------------------------------------
    // 访问器
    // -----------------------------------------------------------------------

    /** @brief 返回最近一次错误的描述字符串。 */
    const std::string& lastError() const noexcept { return last_error_; }

    /** @brief 返回编解码器上下文（只读，供 RtpSender 读取 extradata 等信息）。 */
    const AVCodecContext* codecContext() const noexcept { return codec_ctx_; }

private:
    // 内部：将 avcodec_receive_packet 的结果分发给 callback
    bool receivePackets();

    // 内部：确保 sws_ctx_ 适配当前输入帧格式/尺寸
    bool ensureSwsContext(int src_w, int src_h, AVPixelFormat src_fmt);

    CodecConfig     cfg_;
    AVCodecContext* codec_ctx_  = nullptr;
    AVFrame*        sws_frame_  = nullptr;   ///< 格式转换后的中间帧
    SwsContext*     sws_ctx_    = nullptr;

    PacketCallback  pkt_cb_;
    std::string     last_error_;
};

} // namespace strmctrl
