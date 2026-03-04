// WebSocket (IXWebSocket) and RTP (ffmpeg) implementation for Slave class.

#pragma once

#include <memory>
#include <string>
#include <mutex>
#include <optional>

#include "ISlave.h"
#include "core/Callbacks.h"
#include "core/VideoConfig.h"
#include "transport/SignalingChannel.h"
#include "transport/RtpReceiver.h"

#include <thread>

namespace strmctrl
{

/**
 * @brief 从端门面类（Facade）。
 *
 * Slave 整合 SignalingChannel（WebSocket 客户端）与 RtpReceiver，
 * 向调用方提供简洁的 API，隐藏内部 IXWebSocket / FFmpeg 细节。
 *
 * ### 典型使用流程
 * @code
 * strmctrl::Slave slave;
 *
 * // 注册 callbacks
 * slave.setMessageCallback([](const strmctrl::TextMessage& msg) {
 *     std::cout << "[Master] " << msg.text << "\n";
 * });
 * slave.setVideoFrameCallback([](const strmctrl::VideoFrame& frame) {
 *     // 尽快返回！如需处理，先 clone 后入队
 *     std::cout << "Frame: " << frame.width() << "x" << frame.height() << "\n";
 * });
 * slave.setAudioFrameCallback([](const strmctrl::AudioFrame& frame) {
 *     // 尽快返回！如需处理，先 clone 后入队
 * });
 *
 * // 连接主端（阻断直到信令通道就绪）
 * slave.connect("192.168.1.100", 11451, 11452);
 *
 * // 等待运行（主端推流期间 callback 持续被调用）
 * std::this_thread::sleep_for(std::chrono::seconds(30));
 *
 * // 发送文本消息
 * slave.sendMessage("Hello, master!");
 *
 * // 断开
 * slave.disconnect();
 * @endcode
 *
 * ### RTP 接收启动时机
 * 从端连接到主端的信令通道后，会自动向主端请求 SDP（发送内部控制消息）。
 * 主端回复 SDP 后，Slave 内部自动初始化 RtpReceiver 并启动接收线程。
 * 这一过程对调用方完全透明。
 */
class Slave : public ISlave
{
public:
    // -----------------------------------------------------------------------
    // 构造 / 析构
    // -----------------------------------------------------------------------

    Slave();
    ~Slave();

    Slave(const Slave &) = delete;
    Slave &operator=(const Slave &) = delete;

    // -----------------------------------------------------------------------
    // 配置（在 connect() 之前设置）
    // -----------------------------------------------------------------------

    /**
     * @brief 注册来自主端的文本消息 callback。
     * @param cb  消息到达时调用（在 IXWebSocket 内部线程中）
     */
    void setMessageCallback(MessageCallback cb) override;

    /**
     * @brief 注册解码视频帧 callback。
     * @param cb  帧到达时调用（在 RtpReceiver 工作线程中，应尽快返回）
     */
    void setVideoFrameCallback(VideoFrameCallback cb) override;

    /**
     * @brief 注册解码音频帧 callback。
     * @param cb  帧到达时调用（在 RtpReceiver 工作线程中，应尽快返回）
     */
    void setAudioFrameCallback(AudioFrameCallback cb) override;

    /**
     * @brief 注册连接状态变化 callback。
     * @param cb  连接建立/断开时调用
     */
    void setConnectionCallback(ConnectionCallback cb) override;

    /**
     * @brief 设置视频参数请求（建议值）。
     * @param req  请求参数（可缺省字段）
     */
    void setVideoConfigRequest(const VideoConfigRequest &req) override;

    /** @brief 获取 Master 返回的最终视频参数（如有）。 */
    const std::optional<CodecConfig> &negotiatedVideoConfig() const noexcept override
    {
        return negotiated_video_cfg_;
    }

    // -----------------------------------------------------------------------
    // 生命周期
    // -----------------------------------------------------------------------

    /**
     * @brief 连接到主端。
     *
     * 启动信令通道（WebSocket 客户端）连接主端，连接建立后自动请求 SDP
     * 并初始化 RTP 接收。该方法立即返回（非阻断），连接建立通过
     * ConnectionCallback 通知。
     *
     * @param master_host       主端 IP 或主机名
     * @param signaling_port    主端 WebSocket 端口（默认 11451）
     * @param rtp_port          本地 RTP 接收端口（默认 11452）
     * @return                  true 表示信令通道启动成功（连接可能尚未建立）
     */
    bool connect(const std::string &master_host,
                    int signaling_port = 11451,
                    int rtp_port = 11452);

    /**
     * @brief 断开与主端的连接。
     *
     * 停止信令通道和 RTP 接收器。
     */
    void disconnect() override;

    /** @brief 是否已连接到主端。 */
    bool isConnected() const noexcept override { return connected_; }

    // -----------------------------------------------------------------------
    // 消息
    // -----------------------------------------------------------------------

    /**
     * @brief 向主端发送文本消息。
     * @param text  消息内容
     */
    void sendMessage(const std::string &text) override;

    /**
     * @brief 注册自定义前缀消息回调。
     * @param prefix  前缀
     * @param cb      回调（payload, sender_id）
     * @return true 表示注册成功
     */
    bool registerPrefixCallback(const std::string &prefix,
                                SignalingChannel::PrefixCallback cb);

    /**
     * @brief 发送自定义前缀消息。
     * @param prefix   前缀
     * @param payload  内容
     * @return true 表示入队成功
     */
    bool sendPrefixedMessage(const std::string &prefix, const std::string &payload);

private:
    // 内部回调处理
    void onConnected();
    void onDisconnected();
    void onSdpReceived(const std::string &sdp);

    bool connected_ = false;
    int rtp_port_ = 11452;
    bool has_video_req_ = false;
    VideoConfigRequest video_req_;
    std::optional<CodecConfig> negotiated_video_cfg_;

    std::unique_ptr<SignalingChannel> signaling_;
    std::mutex rtp_mutex_;
    std::unique_ptr<RtpReceiver> rtp_receiver_;
    std::mutex init_mutex_;
    std::thread init_thread_;

    MessageCallback msg_cb_;
    VideoFrameCallback video_cb_;
    AudioFrameCallback audio_cb_;
    ConnectionCallback conn_cb_;
};

} // namespace strmctrl
