#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../codec/CodecConfig.h"

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace strmctrl
{

/**
 * @brief RTP 推流器，将编码后的 AVPacket 通过 RTP/UDP 推送给从端。
 *
 * RtpSender 支持多流（如视频+音频）。由于 FFmpeg 的 RTP muxer 限制，
 * 每个流对应一个独立的 AVFormatContext 和目标端口。
 *
 * ### 典型用法
 * @code
 * strmctrl::RtpSender sender;
 * int v_idx = sender.addStream("192.168.1.101", 11452, v_encoder.codecContext());
 * int a_idx = sender.addStream("192.168.1.101", 11454, a_encoder.codecContext());
 * sender.open();
 * std::string sdp = sender.generateSdp();
 * signaling->sendSdp(sdp);
 *
 * v_encoder.setPacketCallback([&](AVPacket* pkt) { sender.sendPacket(pkt, v_idx); });
 * a_encoder.setPacketCallback([&](AVPacket* pkt) { sender.sendPacket(pkt, a_idx); });
 * @endcode
 */
class RtpSender
{
public:
    // -----------------------------------------------------------------------
    // 构造 / 析构
    // -----------------------------------------------------------------------

    RtpSender() = default;
    ~RtpSender();

    RtpSender(const RtpSender &) = delete;
    RtpSender &operator=(const RtpSender &) = delete;

    // -----------------------------------------------------------------------
    // 生命周期
    // -----------------------------------------------------------------------

    /**
     * @brief 添加一个 RTP 流。
     *
     * @param dest_host    目标主机 IP 或主机名（从端地址）
     * @param dest_port    目标 RTP 端口
     * @param codec_ctx    已打开的编码器上下文，用于初始化流参数
     * @return             流的索引（stream_index），用于 sendPacket；失败返回 -1
     */
    int addStream(const std::string &dest_host,
                    int dest_port,
                    const AVCodecContext *codec_ctx);

    /**
     * @brief 设置发送端本地 RTP 端口基值（用于同机调试避让端口冲突）。
     *
     * 若设置为 >0，open() 时会为每个流追加 URL 参数：
     * localrtpport / localrtcpport。
     * 第 i 个流使用：
     *   localrtpport  = base + i * 2
     *   localrtcpport = base + i * 2 + 1
     *
     * 设为 <=0 表示禁用，交由系统自动分配。
     */
    void setLocalPortBase(int base) noexcept { local_port_base_ = base; }

    /**
     * @brief 打开所有已添加的 RTP 输出上下文。
     * @return true 表示全部成功；false 表示有失败（见 lastError()）
     */
    bool open();

    /**
     * @brief 关闭所有 RTP 输出上下文并释放资源。
     */
    void close();

    /** @brief 输出是否已打开。 */
    bool isOpen() const noexcept { return is_open_; }

    // -----------------------------------------------------------------------
    // 推流
    // -----------------------------------------------------------------------

    /**
     * @brief 将一个已编码的 AVPacket 写入指定的 RTP 输出。
     *
     * @param pkt          编码包（调用方保留所有权）
     * @param stream_index addStream 返回的流索引
     * @return             true 表示成功写入；false 表示写入错误（见 lastError()）
     */
    bool sendPacket(AVPacket *pkt, int stream_index);

    // -----------------------------------------------------------------------
    // SDP
    // -----------------------------------------------------------------------

    /**
     * @brief 生成并返回包含所有流的 SDP 描述字符串。
     *
     * 必须在 open() 成功之后调用。
     * @return SDP 字符串；若未打开则返回空字符串
     */
    std::string generateSdp() const;

    // -----------------------------------------------------------------------
    // 访问器
    // -----------------------------------------------------------------------

    /** @brief 返回最近一次错误的描述字符串。 */
    const std::string &lastError() const noexcept { return last_error_; }

private:
    struct StreamContext
    {
        AVFormatContext *fmt_ctx = nullptr;
        AVStream *stream = nullptr;
        AVRational enc_time_base = {1, 90000};
        int64_t last_dts = AV_NOPTS_VALUE;
        int64_t pkt_index = 0;
        std::string url;
    };

    std::vector<StreamContext> streams_;
    bool is_open_ = false;
    int local_port_base_ = -1;
    std::string last_error_;
};

} // namespace strmctrl
