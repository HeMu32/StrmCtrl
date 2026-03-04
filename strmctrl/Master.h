// WebSocket (IXWebSocket) and RTP (ffmpeg) implementation for Master class.

#pragma once

#include <memory>
#include <string>
#include <mutex>

#include "IMaster.h"
#include "core/Callbacks.h"
#include "codec/CodecConfig.h"
#include "codec/AudioConfig.h"
#include "transport/SignalingChannel.h"
#include "transport/RtpSender.h"
#include "codec/VideoEncoder.h"
#include "codec/AudioEncoder.h"

#include "core/VideoConfig.h"

extern "C"
{
#include <libavcodec/avcodec.h>
}

namespace strmctrl
{

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
 * master.setAudioConfig(strmctrl::AudioConfig::makeAAC(48000, 2, 128000));
 * master.setSignalingPort(11451);
 * master.setRtpPort(11452);
 *
 * // 启动（开始监听）
 * master.start();
 *
 * // 推流循环（caller 负责解码源视频并送帧）
 * while (有帧) {
 *     master.pushVideoFrame(v_frame);
 *     master.pushAudioFrame(a_frame);
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
class Master : public IMaster
{
public:
    // -----------------------------------------------------------------------
    // 构造 / 析构
    // -----------------------------------------------------------------------

    Master();
    ~Master();

    Master(const Master &) = delete;
    Master &operator=(const Master &) = delete;

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
    void setCodecConfig(const CodecConfig &cfg) override;

    /**
     * @brief 设置音频编码配置。
     * @param cfg  AudioConfig 实例
     */
    void setAudioConfig(const AudioConfig &cfg) override;

    /**
     * @brief 注册来自从端的文本消息 callback。
     * @param cb  消息到达时调用（在 IXWebSocket 内部线程中）
     */
    void setMessageCallback(MessageCallback cb) override;

    /**
     * @brief 注册连接状态变化 callback。
     * @param cb  从端连接/断开时调用
     */
    void setConnectionCallback(ConnectionCallback cb) override;

    // -----------------------------------------------------------------------
    // 生命周期
    // -----------------------------------------------------------------------

    /**
     * @brief 启动 Master 服务。
     *
     * 内部启动 WebSocket 服务器监听，并准备好编码器。
     * @return true 表示成功；false 表示端口被占用或编码器初始化失败
     */
    bool start() override;

    /**
     * @brief 停止 Master 服务。
     *
     * 断开所有从端连接，关闭编码器和 RTP 推流器。
     */
    void stop() override;

    /** @brief Master 是否正在运行。 */
    bool isRunning() const noexcept override { return running_; }

    // -----------------------------------------------------------------------
    // 推流
    // -----------------------------------------------------------------------

    /**
     * @brief 推送一帧原始视频数据。
     *
     * 内部将帧送入 VideoEncoder，编码后通过 RtpSender 发送。
     * 若当前无从端连接或未完成 SDP 协商，帧将被丢弃。
     * @param frame  解码后的视频帧（调用方保留所有权）
     */
    void pushVideoFrame(const VideoFrame &frame) override;

    /**
     * @brief 推送一帧原始音频数据。
     *
     * 内部将帧送入 AudioEncoder，编码后通过 RtpSender 发送。
     * 若当前无从端连接或未完成 SDP 协商，帧将被丢弃。
     * @param frame  解码后的音频帧（调用方保留所有权）
     */
    void pushAudioFrame(const AudioFrame &frame) override;

    // -----------------------------------------------------------------------
    // 消息
    // -----------------------------------------------------------------------

    /**
     * @brief 向所有已连接的从端广播文本消息。
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
    void onSlaveConnected(const std::string &ip);
    void onSlaveDisconnected();
    void onSdpRequest(const std::string &payload);
    void onReady();

    // 绑定编码器输出到 RTP 发送器
    void bindEncodersToSender();

    int signaling_port_ = 11451;
    int rtp_port_ = 11452;
    bool running_ = false;

    CodecConfig video_cfg_;
    CodecConfig active_video_cfg_;
    bool has_audio_cfg_ = false;
    AudioConfig audio_cfg_;

    std::unique_ptr<SignalingChannel> signaling_;
    std::unique_ptr<VideoEncoder> video_encoder_;
    std::unique_ptr<AudioEncoder> audio_encoder_;
    std::mutex rtp_sender_mutex_;
    std::unique_ptr<RtpSender> rtp_sender_;

    std::string current_slave_ip_;
    bool rtp_ready_ = false;

    MessageCallback msg_cb_;
    ConnectionCallback conn_cb_;
};

} // namespace strmctrl
