// Shared signaling base for Master and Slave.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "core/Callbacks.h"
#include "transport/SignalingChannel.h"

namespace strmctrl
{

/**
 * @brief 主从节点共享基类。
 *
 * 封装信令通道（SignalingChannel）的持有权以及与消息收发相关的
 * 所有公共逻辑，包括：
 *   - setMessageCallback / setConnectionCallback
 *   - sendMessage（通过 node_id 区分发送方标识）
 *   - registerPrefixCallback / sendPrefixedMessage
 *   - applyStoredCallbacksToSignaling()（启动时同步持久化回调）
 *
 * Master 和 Slave 继承此类，并各自实现与媒体流相关的特有逻辑。
 */
class HostBase
{
public:
    explicit HostBase(std::string node_id);
    virtual ~HostBase() = default;

    HostBase(const HostBase &) = delete;
    HostBase &operator=(const HostBase &) = delete;

    // -----------------------------------------------------------------------
    // 消息（满足 IMaster / ISlave 纯虚函数）
    // -----------------------------------------------------------------------

    /**
     * @brief 注册文本消息 callback。
     * @param cb  消息到达时调用（在 IXWebSocket 内部线程中）
     */
    virtual void setMessageCallback(MessageCallback cb);

    /**
     * @brief 注册连接状态变化 callback。
     * @param cb  连接建立 / 断开时调用
     */
    virtual void setConnectionCallback(ConnectionCallback cb);

    /**
     * @brief 向对端发送文本消息。
     * @param text  消息内容
     */
    virtual void sendMessage(const std::string &text);

    /**
     * @brief 注册自定义前缀消息回调。
     * @param prefix  前缀（不得与内部协议前缀冲突）
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

protected:
    /**
     * @brief 将持久化的回调注册到已创建的 signaling_ 上。
     *
     * 子类在创建 signaling_ 后、调用 signaling_->start() 之前调用此方法，
     * 以确保用户在 start()/connect() 之前注册的回调不丢失。
     */
    void applyStoredCallbacksToSignaling();

    std::string node_id_;
    std::unique_ptr<SignalingChannel> signaling_;
    MessageCallback msg_cb_;
    ConnectionCallback conn_cb_;
    std::unordered_map<std::string, SignalingChannel::PrefixCallback> prefix_cbs_;
};

} // namespace strmctrl
