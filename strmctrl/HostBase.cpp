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
    msg_cb_ = cb;
    if (signaling_)
        signaling_->setMessageCallback(cb);
}

void HostBase::setConnectionCallback(ConnectionCallback cb)
{
    conn_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// 发送
// ---------------------------------------------------------------------------

void HostBase::sendMessage(const std::string &text)
{
    if (signaling_)
        signaling_->sendMessage(TextMessage::make(text, node_id_));
}

bool HostBase::registerPrefixCallback(const std::string &prefix,
                                      SignalingChannel::PrefixCallback cb)
{
    // 镜像 SignalingChannel 的参数校验，确保 signaling_ 不存在时也能提前拦截无效输入
    if (prefix.empty() || !cb)
        return false;
    static const char *kReserved[] = {"READY", "MSG:", "SDP:", "CFG:VIDEO"};
    for (auto r : kReserved)
        if (prefix == r)
            return false;

    // 持久化：无论 signaling_ 是否存在，注册表始终保持最新
    prefix_cbs_[prefix] = cb;

    // 若信令通道已就绪，立即同步（同一 prefix 重复调用仅最后一次生效）
    if (signaling_)
        signaling_->registerPrefixCallback(prefix, std::move(cb));

    return true;
}

bool HostBase::sendPrefixedMessage(const std::string &prefix,
                                   const std::string &payload)
{
    if (!signaling_)
        return false;
    return signaling_->sendPrefixed(prefix, payload);
}

// ---------------------------------------------------------------------------
// 内部辅助
// ---------------------------------------------------------------------------

void HostBase::applyStoredCallbacksToSignaling()
{
    for (auto &[pfx, fn] : prefix_cbs_)
        signaling_->registerPrefixCallback(pfx, fn);

    if (msg_cb_)
        signaling_->setMessageCallback(msg_cb_);
}

} // namespace strmctrl
