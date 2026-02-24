#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXConnectionState.h>

#include "../core/Callbacks.h"
#include "../core/Message.h"

namespace strmctrl {

/**
 * @brief WebSocket 信令通道，负责主端与从端之间的文本消息和内部控制帧传输。
 *
 * 该类有两种工作模式，通过不同的工厂方法创建：
 * - **服务端模式**（主端）：监听指定端口，接受来自从端的连接。
 * - **客户端模式**（从端）：主动连接主端的 WebSocket 地址。
 *
 * ### 内部协议约定
 * 普通文本消息以 `MSG:` 开头，后接消息内容。
 * 内部 SDP 协商帧以 `SDP:` 开头，后接 SDP 字符串。
 * 视频协商结果帧以 `CFG:VIDEO` 开头，后接 JSON 字符串。
 * 这些前缀由本类内部处理，外部 caller 感知不到。
 *
 * ### 线程安全
 * 所有 callback 均在 IXWebSocket 的内部线程中调用；
 * send() 是线程安全的，可从任意线程调用。
 */
class SignalingChannel {
public:
    // -----------------------------------------------------------------------
    // 工厂方法
    // -----------------------------------------------------------------------

    /**
     * @brief 创建服务端模式的信令通道（主端使用）。
     * @param port      监听端口
     * @param bind_addr 绑定地址（默认 "0.0.0.0"）
     * @return          SignalingChannel 唯一指针
     */
    static std::unique_ptr<SignalingChannel>
    createServer(int port, const std::string& bind_addr = "0.0.0.0");

    /**
     * @brief 创建客户端模式的信令通道（从端使用）。
     * @param host  主端 IP 或主机名
     * @param port  主端 WebSocket 端口
     * @return      SignalingChannel 唯一指针
     */
    static std::unique_ptr<SignalingChannel>
    createClient(const std::string& host, int port);

    // -----------------------------------------------------------------------
    // 析构
    // -----------------------------------------------------------------------
    ~SignalingChannel();

    // 禁止拷贝和移动（内部持有线程资源）
    SignalingChannel(const SignalingChannel&)            = delete;
    SignalingChannel& operator=(const SignalingChannel&) = delete;
    SignalingChannel(SignalingChannel&&)                 = delete;
    SignalingChannel& operator=(SignalingChannel&&)      = delete;

    // -----------------------------------------------------------------------
    // Callback 注册
    // -----------------------------------------------------------------------

    /**
     * @brief 注册用户文本消息回调。
     * @param cb  消息到达时调用（在 IXWebSocket 内部线程中）
     */
    void setMessageCallback(MessageCallback cb);

    /**
     * @brief 注册连接状态变化回调。
     * @param cb  连接建立/断开时调用
     */
    void setConnectionCallback(ConnectionCallback cb);

    /**
     * @brief 注册 SDP 内部帧回调（由 RtpSender/RtpReceiver 使用，普通 caller 无需关心）。
     * @param cb  收到 SDP 帧时调用，参数为 SDP 字符串本体
     */
    void setSdpCallback(std::function<void(const std::string& sdp)> cb);

    /**
     * @brief 注册视频协商结果回调。
     * @param cb  收到 CFG:VIDEO 帧时调用，参数为 JSON 字符串
     */
    void setVideoConfigCallback(std::function<void(const std::string& json)> cb);

    // -----------------------------------------------------------------------
    // 生命周期
    // -----------------------------------------------------------------------

    /**
     * @brief 启动信令通道（服务端开始监听 / 客户端发起连接）。
     * @return true 表示启动成功；false 表示端口绑定或网络错误。
     */
    bool start();

    /**
     * @brief 停止信令通道，断开所有连接并释放网络资源。
     */
    void stop();

    // -----------------------------------------------------------------------
    // 消息发送
    // -----------------------------------------------------------------------

    /**
     * @brief 向对端发送文本消息（线程安全）。
     *
     * 服务端模式下会广播给所有已连接的从端；
     * 客户端模式下发送给已连接的主端。
     * @param msg  待发送的文本消息
     * @return     true 表示至少向一个连接成功入队；false 表示无活跃连接
     */
    bool sendMessage(const TextMessage& msg);

    /**
     * @brief 发送内部 SDP 帧（由 RtpSender/RtpReceiver 使用，普通 caller 无需调用）。
     * @param sdp  SDP 字符串
     * @return     true 表示入队成功
     */
    bool sendSdp(const std::string& sdp);

    /**
     * @brief 发送视频协商结果帧（JSON 字符串）。
     * @param json  协商结果 JSON
     * @return      true 表示入队成功
     */
    bool sendVideoConfig(const std::string& json);

    /**
     * @brief 注册 READY 内部帧回调（从端 RtpReceiver 就绪后通知主端）。
     * @param cb  收到 READY 帧时调用
     */
    void setReadyCallback(std::function<void()> cb);

    /**
     * @brief 发送内部 READY 帧，通知对端 RTP 接收端已就绪（线程安全）。
     * @return true 表示入队成功
     */
    bool sendReady();

    // -----------------------------------------------------------------------
    // 状态查询
    // -----------------------------------------------------------------------

    /** @brief 是否有至少一个活跃连接。 */
    bool isConnected() const;

    /** @brief 返回当前模式描述字符串（"server" 或 "client"）。 */
    std::string mode() const;

private:
    // 私有构造（通过工厂方法创建）
    explicit SignalingChannel(bool is_server);

    // 统一消息分发：解析前缀后分发到对应 callback
    void dispatchRawMessage(const std::string& raw,
                            const std::string& sender_id);

    // 服务端内部连接回调安装
    void attachServerCallbacks(std::weak_ptr<ix::WebSocket> weakWs,
                               std::shared_ptr<ix::ConnectionState> state);

    // -----------------------------------------------------------------------
    // 成员变量
    // -----------------------------------------------------------------------

    bool is_server_;

    // 服务端模式
    std::unique_ptr<ix::WebSocketServer> server_;

    // 客户端模式
    std::unique_ptr<ix::WebSocket>       client_;

    // 用户注册的 callbacks
    MessageCallback                              msg_cb_;
    ConnectionCallback                           conn_cb_;
    std::function<void(const std::string& sdp)> sdp_cb_;
    std::function<void(const std::string& json)> video_cfg_cb_;
    std::function<void()>                        ready_cb_;

    // 消息前缀常量
    static constexpr const char* kMsgPrefix   = "MSG:";
    static constexpr const char* kSdpPrefix   = "SDP:";
    static constexpr const char* kVideoCfgPrefix = "CFG:VIDEO";
    static constexpr const char* kReadyPrefix = "READY";
};

} // namespace strmctrl
