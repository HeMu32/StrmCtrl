#pragma once

#include <memory>
#include <string>

#include "core/Callbacks.h"
#include "codec/CodecConfig.h"
#include "transport/SignalingChannel.h"
#include "transport/RtpSender.h"
#include "codec/VideoEncoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace strmctrl {

/**
 * @brief 主端门面类（Facade）。
 *
 * Master 整合 SignalingChannel（WebSocket 服务端）与 RtpSender，
 * 向调用方提供简洁的 API，隐藏内部 IXWebSocket / FFmpeg 细节。
 *
 * ### 典型使用流程
 * @code
 * strmctrl::Master master;
 *
 * // 配置
 * master.setMessageCallback([](const strmctrl::TextMessage& msg) {
 *     std::cout << "[" << msg.sender_id << "] " << msg.text << "\n";
 * });
 * master.setCodecConfig(strmctrl::CodecConfig::makeOpenH264(1280, 720, 30, 2000));
 * master.setSignalingPort(11451);
 * master.setRtpPort(11452);
 *
 * // 启动（开始监听）
 * master.start();
 *
 * // 推流循环（caller 负责解码源视频并送帧）
 * while (有帧) {
 *     master.pushVideoFrame(avframe);
 * }
 *
 * // 发送文本消息
 * master.sendMessage("Hello, slave!");
 *
 * // 停止
 * master.stop();
 * @endcode
 *
 * ### SDP 协商
 * 当有从端连接时，Master 会等待从端发送 "SDP_REQUEST" 内部控制消息，
 * 然后自动回复 SDP 字符串；RtpSender 随后开始向从端推流。
 * 这一过程对调用方透明。
 */
class Master {
public:
    // -----------------------------------------------------------------------
    // 构造 / 析构
    // -----------------------------------------------------------------------

    Master();
    ~Master();

    Master(const Master&)            = delete;
    Master& operator=(const Master&) = delete;

    // -----------------------------------------------------------------------
    // 配置（必须在 start() 之前设置）
    // -----------------------------------------------------------------------

    /**
    * @brief 设置 WebSocket 信令通道监听端口（默认 11451）。
     * @param port  端口号
     */
    void setSignalingPort(int port);

    /**
    * @brief 设置 RTP 推流的目标端口（从端接收端口，默认 11452）。
     *
     * 主端推流时使用此端口作为目标端口，从端在同一端口上监听。
     * @param port  端口号
     */
    void setRtpPort(int port);

    /**
     * @brief 设置视频编码配置。
     * @param cfg  CodecConfig 实例（默认为 libopenh264 1280x720@30fps 2Mbps）
     */
    void setCodecConfig(const CodecConfig& cfg);

    /**
     * @brief 注册来自从端的文本消息 callback。
     * @param cb  消息到达时调用（在 IXWebSocket 内部线程中）
     */
    void setMessageCallback(MessageCallback cb);

    /**
     * @brief 注册连接状态变化 callback。
     * @param cb  从端连接/断开时调用
     */
    void setConnectionCallback(ConnectionCallback cb);

    // -----------------------------------------------------------------------
    // 生命周期
    // -----------------------------------------------------------------------

    /**
     * @brief 启动主端：初始化编码器、启动信令通道监听。
     * @return true 表示成功；false 表示编码器初始化失败（见 lastError()）
     */
    bool start();

    /**
     * @brief 停止主端，断开所有连接，释放资源。
     */
    void stop();

    /** @brief 是否已启动。 */
    bool isRunning() const noexcept { return running_; }

    // -----------------------------------------------------------------------
    // 推流 / 发送
    // -----------------------------------------------------------------------

    /**
     * @brief 向从端推送一帧原始视频（编码并通过 RTP 发送）。
     *
     * 若从端尚未连接（未完成 SDP 协商），帧会被丢弃。
     * @param frame  原始 AVFrame（调用方保留所有权）
     * @return       true 表示帧成功送入编码器；false 表示编码器未就绪或内部错误
     */
    bool pushVideoFrame(AVFrame* frame);

    /**
     * @brief 向所有已连接的从端发送文本消息。
     * @param text  消息内容（UTF-8）
     * @return      true 表示至少有一个从端收到；false 表示无活跃连接
     */
    bool sendMessage(const std::string& text);

    // -----------------------------------------------------------------------
    // 访问器
    // -----------------------------------------------------------------------

    /** @brief 返回最近一次错误描述字符串。 */
    const std::string& lastError() const noexcept { return last_error_; }

    /** @brief 是否有从端连接。 */
    bool hasSlaveConnected() const;

    /**
     * @brief 是否已完成 READY 握手（从端 RTP 接收端口已就绪）。
     *
     * 推流线程应在此返回 true 后再调用 pushVideoFrame()，
     * 否则帧会被 pushVideoFrame() 内部丢弃。
     */
    bool isRtpReady() const noexcept { return rtp_ready_; }

private:
    // 内部：从端连接后触发 SDP 协商
    void onSlaveConnected(const std::string& slave_addr);

    // 内部：将 encoder 的 PacketCallback 与 sender 绑定
    void bindEncoderToSender();

    int         signaling_port_ = 11451;
    int         rtp_port_       = 11452;
    CodecConfig codec_cfg_;
    bool        running_        = false;

    std::string last_error_;

    std::unique_ptr<SignalingChannel> signaling_;
    std::unique_ptr<VideoEncoder>     encoder_;
    std::unique_ptr<RtpSender>        sender_;

    // 从端地址（SDP 协商完成后已知，用于 RtpSender::open）
    std::string slave_host_;
    bool        rtp_ready_ = false;

    MessageCallback    msg_cb_;
    ConnectionCallback conn_cb_;
};

} // namespace strmctrl
