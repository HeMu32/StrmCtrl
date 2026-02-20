#pragma once

#include <memory>
#include <string>

#include "../codec/CodecConfig.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace strmctrl {

/**
 * @brief RTP 推流器，将编码后的 AVPacket 通过 RTP/UDP 推送给从端。
 *
 * RtpSender 使用 FFmpeg 的 avformat 层打开 "rtp://<host>:<port>" 输出，
 * 并将 VideoEncoder 产出的 AVPacket 写入该输出上下文。
 *
 * ### SDP 获取
 * 打开输出后，可通过 generateSdp() 获取 SDP 字符串，
 * 再经由 SignalingChannel 发送给从端，以便从端知晓 RTP 流参数。
 *
 * ### 典型用法
 * @code
 * strmctrl::RtpSender sender;
 * sender.open("192.168.1.101", 11452, encoder.codecContext());
 * std::string sdp = sender.generateSdp();
 * signaling->sendSdp(sdp);
 *
 * encoder.setPacketCallback([&](AVPacket* pkt) {
 *     sender.sendPacket(pkt);
 * });
 * @endcode
 */
class RtpSender {
public:
    // -----------------------------------------------------------------------
    // 构造 / 析构
    // -----------------------------------------------------------------------

    RtpSender() = default;
    ~RtpSender();

    RtpSender(const RtpSender&)            = delete;
    RtpSender& operator=(const RtpSender&) = delete;

    // -----------------------------------------------------------------------
    // 生命周期
    // -----------------------------------------------------------------------

    /**
     * @brief 打开 RTP 输出上下文。
     *
     * @param dest_host    目标主机 IP 或主机名（从端地址）
     * @param dest_port    目标 RTP 端口
     * @param codec_ctx    已打开的编码器上下文，用于初始化流参数
     * @return             true 表示成功；false 表示输出打开失败（见 lastError()）
     */
    bool open(const std::string& dest_host,
              int                dest_port,
              const AVCodecContext* codec_ctx);

    /**
     * @brief 关闭 RTP 输出上下文并释放资源。
     */
    void close();

    /** @brief 输出是否已打开。 */
    bool isOpen() const noexcept { return fmt_ctx_ != nullptr; }

    // -----------------------------------------------------------------------
    // 推流
    // -----------------------------------------------------------------------

    /**
     * @brief 将一个已编码的 AVPacket 写入 RTP 输出。
     *
     * 通常由 VideoEncoder 的 PacketCallback 调用。
     * @param pkt  编码包（调用方保留所有权）
     * @return     true 表示成功写入；false 表示写入错误（见 lastError()）
     */
    bool sendPacket(AVPacket* pkt);

    // -----------------------------------------------------------------------
    // SDP
    // -----------------------------------------------------------------------

    /**
     * @brief 生成并返回当前流的 SDP 描述字符串。
     *
     * 必须在 open() 成功之后调用。返回的 SDP 应通过 SignalingChannel
     * 发送给从端，从端据此打开 RTP 接收端。
     * @return SDP 字符串；若未打开则返回空字符串
     */
    std::string generateSdp() const;

    // -----------------------------------------------------------------------
    // 访问器
    // -----------------------------------------------------------------------

    /** @brief 返回最近一次错误的描述字符串。 */
    const std::string& lastError() const noexcept { return last_error_; }

private:
    AVFormatContext* fmt_ctx_   = nullptr;
    AVStream*        stream_    = nullptr;
    int64_t          pkt_index_ = 0;      ///< 单调递增的包序号（用于 dts/pts 推算）
    AVRational       enc_time_base_ = {1, 90000}; ///< 编码器时间基
    int64_t          last_dts_  = AV_NOPTS_VALUE; ///< 记录上一个发送的 dts，确保单调递增

    std::string last_error_;
};

} // namespace strmctrl
