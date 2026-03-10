#include "SignalingChannel.h"

#include <chrono>
#include <iostream>

namespace strmctrl
{

// ---------------------------------------------------------------------------
// 工厂方法
// ---------------------------------------------------------------------------

std::unique_ptr<SignalingChannel>
SignalingChannel::createServer(int port, const std::string &bind_addr)
{
    auto ch = std::unique_ptr<SignalingChannel>(new SignalingChannel(true));
    ch->server_ = std::make_unique<ix::WebSocketServer>(port, bind_addr);
    return ch;
}

std::unique_ptr<SignalingChannel>
SignalingChannel::createClient(const std::string &host, int port)
{
    auto ch = std::unique_ptr<SignalingChannel>(new SignalingChannel(false));
    ch->client_ = std::make_unique<ix::WebSocket>();
    ch->client_->setUrl("ws://" + host + ":" + std::to_string(port));
    return ch;
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

SignalingChannel::SignalingChannel(bool is_server)
    : is_server_(is_server)
{
}

SignalingChannel::~SignalingChannel()
{
    stop();
}

// ---------------------------------------------------------------------------
// Callback 注册
// ---------------------------------------------------------------------------

void SignalingChannel::setMessageCallback(MessageCallback cb)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    msg_cb_ = std::move(cb);
}

void SignalingChannel::setConnectionCallback(ConnectionCallback cb)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    conn_cb_ = std::move(cb);
}

void SignalingChannel::setSdpCallback(std::function<void(const std::string &)> cb)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    sdp_cb_ = std::move(cb);
}

void SignalingChannel::setVideoConfigCallback(std::function<void(const std::string &)> cb)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    video_cfg_cb_ = std::move(cb);
}

bool SignalingChannel::registerPrefixCallback(const std::string &prefix,
                                              PrefixCallback cb)
{
    if (prefix.empty() || !cb)
        return false;

    if (prefix == kReadyPrefix ||
        prefix == kMsgPrefix ||
        prefix == kSdpPrefix ||
        prefix == kVideoCfgPrefix)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(callback_mutex_);
    custom_prefix_cbs_[prefix] = std::move(cb);
    return true;
}

bool SignalingChannel::unregisterPrefixCallback(const std::string &prefix)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    return custom_prefix_cbs_.erase(prefix) > 0;
}

void SignalingChannel::clearPrefixCallbacks()
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    custom_prefix_cbs_.clear();
}

void SignalingChannel::setReadyCallback(std::function<void()> cb)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    ready_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool SignalingChannel::start()
{
    if (is_server_)
    {
        // 安装连接回调：每有新连接就为该 WebSocket 安装消息回调
        server_->setOnConnectionCallback(
            [this](std::weak_ptr<ix::WebSocket> weakWs,
                   std::shared_ptr<ix::ConnectionState> state)
            {
                attachServerCallbacks(std::move(weakWs), std::move(state));
            });

        auto [ok, err] = server_->listen();
        if (!ok)
        {
            std::cerr << "[SignalingChannel] Server listen failed: " << err << "\n";
            return false;
        }
        server_->start();
        return true;
    }

    // 客户端：安装消息回调后启动
    client_->setOnMessageCallback(
        [this](const ix::WebSocketMessagePtr &msg)
        {
            using T = ix::WebSocketMessageType;
            if (msg->type == T::Message)
            {
                dispatchRawMessage(msg->str, client_->getUrl());
            }
            else if (msg->type == T::Open)
            {
                auto cb = connectionCallbackCopy();
                if (cb)
                    cb(true, client_->getUrl());
            }
            else if (msg->type == T::Close)
            {
                auto cb = connectionCallbackCopy();
                if (cb)
                    cb(false, msg->closeInfo.reason);
            }
            else if (msg->type == T::Error)
            {
                auto cb = connectionCallbackCopy();
                if (cb)
                    cb(false, msg->errorInfo.reason);
            }
        });

    client_->start();
    return true;
}

void SignalingChannel::stop()
{
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        active_client_id_.clear();
        active_client_ws_.reset();
    }

    if (is_server_ && server_)
    {
        server_->stop();
    }
    else if (!is_server_ && client_)
    {
        client_->stop();
    }
}

// ---------------------------------------------------------------------------
// 消息发送
// ---------------------------------------------------------------------------

bool SignalingChannel::sendMessage(const TextMessage &msg)
{
    // 序列化为 "MSG:<text>" 形式；sender_id 与 timestamp 由接收方填充
    return sendRaw(std::string(kMsgPrefix) + msg.text);
}

bool SignalingChannel::sendSdp(const std::string &sdp)
{
    return sendRaw(std::string(kSdpPrefix) + sdp);
}

bool SignalingChannel::sendVideoConfig(const std::string &json)
{
    return sendRaw(std::string(kVideoCfgPrefix) + " " + json);
}

bool SignalingChannel::sendReady()
{
    return sendRaw(kReadyPrefix);
}

bool SignalingChannel::sendPrefixed(const std::string &prefix, const std::string &payload)
{
    if (prefix.empty())
        return false;

    return sendRaw(prefix + payload);
}

// ---------------------------------------------------------------------------
// 状态查询
// ---------------------------------------------------------------------------

bool SignalingChannel::isConnected() const
{
    if (is_server_)
        return static_cast<bool>(activeServerClient());

    return client_ && client_->getReadyState() == ix::ReadyState::Open;
}

std::string SignalingChannel::mode() const
{
    return is_server_ ? "server" : "client";
}

// ---------------------------------------------------------------------------
// 私有辅助
// ---------------------------------------------------------------------------

void SignalingChannel::dispatchRawMessage(const std::string &raw,
                                          const std::string &sender_id)
{
    using namespace std::chrono;

    MessageCallback msg_cb;
    std::function<void(const std::string &)> sdp_cb;
    std::function<void(const std::string &)> video_cfg_cb;
    std::function<void()> ready_cb;
    PrefixCallback matched_cb;
    std::string matched_prefix;

    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (raw.rfind(kMsgPrefix, 0) == 0)
        {
            msg_cb = msg_cb_;
        }
        else if (raw.rfind(kSdpPrefix, 0) == 0)
        {
            sdp_cb = sdp_cb_;
        }
        else if (raw.rfind(kVideoCfgPrefix, 0) == 0)
        {
            video_cfg_cb = video_cfg_cb_;
        }
        else if (raw == kReadyPrefix)
        {
            ready_cb = ready_cb_;
        }
        else
        {
            for (const auto &entry : custom_prefix_cbs_)
            {
                const std::string &prefix = entry.first;
                if (raw.rfind(prefix, 0) == 0 && prefix.size() > matched_prefix.size())
                {
                    matched_prefix = prefix;
                    matched_cb = entry.second;
                }
            }
        }
    }

    if (msg_cb)
    {
        // 普通文本消息
        TextMessage tm;
        tm.text = raw.substr(std::string(kMsgPrefix).size());
        tm.sender_id = sender_id;
        tm.timestamp_ms = duration_cast<milliseconds>(
                              system_clock::now().time_since_epoch())
                              .count();
        msg_cb(tm);
        return;
    }

    if (sdp_cb)
    {
        // 内部 SDP 帧
        sdp_cb(raw.substr(std::string(kSdpPrefix).size()));
        return;
    }

    if (video_cfg_cb)
    {
        video_cfg_cb(raw.substr(std::string(kVideoCfgPrefix).size()));
        return;
    }

    if (ready_cb)
    {
        // 从端 RTP 接收端就绪通知
        ready_cb();
        return;
    }

    if (matched_cb)
    {
        matched_cb(raw.substr(matched_prefix.size()), sender_id);
        return;
    }

    // 未知格式，忽略并打印警告
    std::cerr << "[SignalingChannel] Unknown message prefix, ignoring. "
              << "raw=" << raw.substr(0, 64) << "\n";
}

void SignalingChannel::attachServerCallbacks(
    std::weak_ptr<ix::WebSocket> weakWs,
    std::shared_ptr<ix::ConnectionState> state)
{
    auto ws = weakWs.lock();
    if (!ws || !state)
        return;

    // 保存远端地址供 callback 使用
    const std::string remote_addr =
        state->getRemoteIp() + ":" + std::to_string(state->getRemotePort());
    const std::string client_id = state->getId();

    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        auto active_ws = active_client_ws_.lock();
        if (active_client_id_.empty() || !active_ws || active_client_id_ == client_id)
        {
            active_client_id_ = client_id;
            active_client_ws_ = ws;
            accepted = true;
        }
    }

    if (!accepted)
    {
        ws->setOnMessageCallback(
            [weakWs](const ix::WebSocketMessagePtr &msg)
            {
                if (msg->type == ix::WebSocketMessageType::Open)
                {
                    if (auto ws = weakWs.lock())
                    {
                        ws->close(ix::WebSocketCloseConstants::kNormalClosureCode,
                                  kSingleSlaveReason);
                    }
                }
            });
        ws->close(ix::WebSocketCloseConstants::kNormalClosureCode, kSingleSlaveReason);
        return;
    }

    ws->setOnMessageCallback(
        [this, weakWs, remote_addr, client_id](const ix::WebSocketMessagePtr &msg)
        {
            using T = ix::WebSocketMessageType;
            if (msg->type == T::Message)
            {
                bool is_active = false;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex_);
                    is_active = (active_client_id_ == client_id);
                }
                if (is_active)
                    dispatchRawMessage(msg->str, remote_addr);
                return;
            }
            else if (msg->type == T::Open)
            {
                bool is_active = false;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex_);
                    if (active_client_id_ == client_id)
                    {
                        active_client_ws_ = weakWs;
                        is_active = true;
                    }
                }

                if (!is_active)
                {
                    if (auto ws = weakWs.lock())
                    {
                        ws->close(ix::WebSocketCloseConstants::kNormalClosureCode,
                                  kSingleSlaveReason);
                    }
                    return;
                }

                auto cb = connectionCallbackCopy();
                if (cb)
                    cb(true, remote_addr);
            }
            else if (msg->type == T::Close)
            {
                bool was_active = false;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex_);
                    if (active_client_id_ == client_id)
                    {
                        active_client_id_.clear();
                        active_client_ws_.reset();
                        was_active = true;
                    }
                }
                if (was_active)
                {
                    auto cb = connectionCallbackCopy();
                    if (cb)
                        cb(false, remote_addr + " disconnected");
                }
            }
            else if (msg->type == T::Error)
            {
                bool was_active = false;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex_);
                    if (active_client_id_ == client_id)
                    {
                        active_client_id_.clear();
                        active_client_ws_.reset();
                        was_active = true;
                    }
                }
                if (was_active)
                {
                    auto cb = connectionCallbackCopy();
                    if (cb)
                        cb(false, msg->errorInfo.reason);
                }
            }
        });
}

bool SignalingChannel::sendRaw(const std::string &raw)
{
    return is_server_ ? sendRawServer(raw) : sendRawClient(raw);
}

bool SignalingChannel::sendRawServer(const std::string &raw)
{
    auto ws = activeServerClient();
    if (!ws)
        return false;

    return ws->send(raw).success;
}

bool SignalingChannel::sendRawClient(const std::string &raw)
{
    if (!client_)
        return false;

    return client_->send(raw).success;
}

ConnectionCallback SignalingChannel::connectionCallbackCopy() const
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    return conn_cb_;
}

std::shared_ptr<ix::WebSocket> SignalingChannel::activeServerClient() const
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (active_client_id_.empty())
        return nullptr;

    auto ws = active_client_ws_.lock();
    if (!ws)
        return nullptr;

    return ws;
}

} // namespace strmctrl
