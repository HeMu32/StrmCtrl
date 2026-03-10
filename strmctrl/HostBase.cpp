#include "HostBase.h"

namespace strmctrl
{

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------

HostBase::HostBase(std::string node_id)
    : node_id_(std::move(node_id))
{
}

// ---------------------------------------------------------------------------
// 消息 callback 配置
// ---------------------------------------------------------------------------

void HostBase::setMessageCallback(MessageCallback cb)
{
    std::shared_ptr<SignalingChannel> signaling;
    MessageCallback cb_copy;
    {
        std::lock_guard<std::mutex> lock(host_mutex_);
        msg_cb_ = std::move(cb);
        cb_copy = msg_cb_;
        signaling = signaling_;
    }

    if (signaling)
        signaling->setMessageCallback(std::move(cb_copy));
}

void HostBase::setConnectionCallback(ConnectionCallback cb)
{
    std::lock_guard<std::mutex> lock(host_mutex_);
    conn_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// 发送
// ---------------------------------------------------------------------------

void HostBase::sendMessage(const std::string &text)
{
    auto signaling = signalingChannel();
    if (signaling)
        signaling->sendMessage(TextMessage::make(text, node_id_));
}

bool HostBase::registerPrefixCallback(const std::string &prefix,
                                      SignalingChannel::PrefixCallback cb)
{
    // 镜像 SignalingChannel 的参数校验，确保 signaling_ 不存在时也能提前拦截无效输入
    if (prefix.empty() || !cb)
        return false;

    static const char *kReserved[] = {"READY", "MSG:", "SDP:", "CFG:VIDEO"};
    for (auto reserved : kReserved)
    {
        if (prefix == reserved)
            return false;
    }

    std::shared_ptr<SignalingChannel> signaling;
    {
        // 持久化：无论 signaling_ 是否存在，注册表始终保持最新
        std::lock_guard<std::mutex> lock(host_mutex_);
        prefix_cbs_[prefix] = cb;
        signaling = signaling_;
    }

    // 若信令通道已就绪，立即同步（同一 prefix 重复调用仅最后一次生效）
    if (signaling)
        return signaling->registerPrefixCallback(prefix, std::move(cb));

    return true;
}

bool HostBase::sendPrefixedMessage(const std::string &prefix,
                                   const std::string &payload)
{
    auto signaling = signalingChannel();
    if (!signaling)
        return false;

    return signaling->sendPrefixed(prefix, payload);
}

// ---------------------------------------------------------------------------
// 内部辅助
// ---------------------------------------------------------------------------

void HostBase::applyStoredCallbacksToSignaling()
{
    std::shared_ptr<SignalingChannel> signaling;
    std::unordered_map<std::string, SignalingChannel::PrefixCallback> prefix_cbs;
    MessageCallback msg_cb;
    {
        std::lock_guard<std::mutex> lock(host_mutex_);
        signaling = signaling_;
        prefix_cbs = prefix_cbs_;
        msg_cb = msg_cb_;
    }

    if (!signaling)
        return;

    for (const auto &entry : prefix_cbs)
        signaling->registerPrefixCallback(entry.first, entry.second);

    if (msg_cb)
        signaling->setMessageCallback(std::move(msg_cb));
}

void HostBase::setSignalingChannel(std::shared_ptr<SignalingChannel> signaling)
{
    std::lock_guard<std::mutex> lock(host_mutex_);
    signaling_ = std::move(signaling);
}

std::shared_ptr<SignalingChannel> HostBase::signalingChannel() const
{
    std::lock_guard<std::mutex> lock(host_mutex_);
    return signaling_;
}

std::shared_ptr<SignalingChannel> HostBase::resetSignalingChannel()
{
    std::lock_guard<std::mutex> lock(host_mutex_);
    auto signaling = std::move(signaling_);
    signaling_.reset();
    return signaling;
}

ConnectionCallback HostBase::storedConnectionCallback() const
{
    std::lock_guard<std::mutex> lock(host_mutex_);
    return conn_cb_;
}

} // namespace strmctrl
