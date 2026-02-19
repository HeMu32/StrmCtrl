#include "SignalingChannel.h"

#include <iostream>
#include <chrono>

namespace strmctrl {

// ---------------------------------------------------------------------------
// 工厂方法
// ---------------------------------------------------------------------------

std::unique_ptr<SignalingChannel>
SignalingChannel::createServer(int port, const std::string& bind_addr)
{
    auto ch = std::unique_ptr<SignalingChannel>(new SignalingChannel(true));
    ch->server_ = std::make_unique<ix::WebSocketServer>(port, bind_addr);
    return ch;
}

std::unique_ptr<SignalingChannel>
SignalingChannel::createClient(const std::string& host, int port)
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
{}

SignalingChannel::~SignalingChannel()
{
    stop();
}

// ---------------------------------------------------------------------------
// Callback 注册
// ---------------------------------------------------------------------------

void SignalingChannel::setMessageCallback(MessageCallback cb)
{
    msg_cb_ = std::move(cb);
}

void SignalingChannel::setConnectionCallback(ConnectionCallback cb)
{
    conn_cb_ = std::move(cb);
}

void SignalingChannel::setSdpCallback(std::function<void(const std::string&)> cb)
{
    sdp_cb_ = std::move(cb);
}

void SignalingChannel::setReadyCallback(std::function<void()> cb)
{
    ready_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool SignalingChannel::start()
{
    if (is_server_) {
        // 安装连接回调：每有新连接就为该 WebSocket 安装消息回调
        server_->setOnConnectionCallback(
            [this](std::weak_ptr<ix::WebSocket> weakWs,
                   std::shared_ptr<ix::ConnectionState> state)
            {
                attachServerCallbacks(std::move(weakWs), std::move(state));
            }
        );

        auto [ok, err] = server_->listen();
        if (!ok) {
            std::cerr << "[SignalingChannel] Server listen failed: " << err << "\n";
            return false;
        }
        server_->start();
        return true;
    } else {
        // 客户端：安装消息回调后启动
        client_->setOnMessageCallback(
            [this](const ix::WebSocketMessagePtr& msg)
            {
                using T = ix::WebSocketMessageType;
                if (msg->type == T::Message) {
                    dispatchRawMessage(msg->str, client_->getUrl());
                } else if (msg->type == T::Open) {
                    if (conn_cb_) conn_cb_(true, client_->getUrl());
                } else if (msg->type == T::Close) {
                    if (conn_cb_) conn_cb_(false, msg->closeInfo.reason);
                } else if (msg->type == T::Error) {
                    if (conn_cb_) conn_cb_(false, msg->errorInfo.reason);
                }
            }
        );
        client_->start();
        return true;
    }
}

void SignalingChannel::stop()
{
    if (is_server_ && server_) {
        server_->stop();
    } else if (!is_server_ && client_) {
        client_->stop();
    }
}

// ---------------------------------------------------------------------------
// 消息发送
// ---------------------------------------------------------------------------

bool SignalingChannel::sendMessage(const TextMessage& msg)
{
    // 序列化为 "MSG:<text>" 形式；sender_id 与 timestamp 由接收方填充
    const std::string raw = std::string(kMsgPrefix) + msg.text;

    if (is_server_) {
        auto clients = server_->getClients();
        if (clients.empty()) return false;
        for (auto& ws : clients) ws->send(raw);
        return true;
    } else {
        if (!client_) return false;
        client_->send(raw);
        return true;
    }
}

bool SignalingChannel::sendSdp(const std::string& sdp)
{
    const std::string raw = std::string(kSdpPrefix) + sdp;

    if (is_server_) {
        auto clients = server_->getClients();
        if (clients.empty()) return false;
        for (auto& ws : clients) ws->send(raw);
        return true;
    } else {
        if (!client_) return false;
        client_->send(raw);
        return true;
    }
}

bool SignalingChannel::sendReady()
{
    const std::string raw = std::string(kReadyPrefix);

    if (is_server_) {
        auto clients = server_->getClients();
        if (clients.empty()) return false;
        for (auto& ws : clients) ws->send(raw);
        return true;
    } else {
        if (!client_) return false;
        client_->send(raw);
        return true;
    }
}

// ---------------------------------------------------------------------------
// 状态查询
// ---------------------------------------------------------------------------

bool SignalingChannel::isConnected() const
{
    if (is_server_) {
        return server_ && !server_->getClients().empty();
    } else {
        return client_ &&
               client_->getReadyState() == ix::ReadyState::Open;
    }
}

std::string SignalingChannel::mode() const
{
    return is_server_ ? "server" : "client";
}

// ---------------------------------------------------------------------------
// 私有辅助
// ---------------------------------------------------------------------------

void SignalingChannel::dispatchRawMessage(const std::string& raw,
                                          const std::string& sender_id)
{
    using namespace std::chrono;

    if (raw.rfind(kMsgPrefix, 0) == 0) {
        // 普通文本消息
        if (msg_cb_) {
            TextMessage tm;
            tm.text        = raw.substr(std::string(kMsgPrefix).size());
            tm.sender_id   = sender_id;
            tm.timestamp_ms = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count();
            msg_cb_(tm);
        }
    } else if (raw.rfind(kSdpPrefix, 0) == 0) {
        // 内部 SDP 帧
        if (sdp_cb_) {
            sdp_cb_(raw.substr(std::string(kSdpPrefix).size()));
        }
    } else if (raw == kReadyPrefix) {
        // 从端 RTP 接收端就绪通知
        if (ready_cb_) {
            ready_cb_();
        }
    } else {
        // 未知格式，忽略并打印警告
        std::cerr << "[SignalingChannel] Unknown message prefix, ignoring. "
                  << "raw=" << raw.substr(0, 64) << "\n";
    }
}

void SignalingChannel::attachServerCallbacks(
    std::weak_ptr<ix::WebSocket>       weakWs,
    std::shared_ptr<ix::ConnectionState> state)
{
    auto ws = weakWs.lock();
    if (!ws) return;

    // 保存远端地址供 callback 使用
    const std::string remote_addr =
        state->getRemoteIp() + ":" + std::to_string(state->getRemotePort());

    ws->setOnMessageCallback(
        [this, remote_addr](const ix::WebSocketMessagePtr& msg)
        {
            using T = ix::WebSocketMessageType;
            if (msg->type == T::Message) {
                dispatchRawMessage(msg->str, remote_addr);
            } else if (msg->type == T::Open) {
                if (conn_cb_) conn_cb_(true, remote_addr);
            } else if (msg->type == T::Close) {
                if (conn_cb_) conn_cb_(false, remote_addr + " disconnected");
            } else if (msg->type == T::Error) {
                if (conn_cb_) conn_cb_(false, msg->errorInfo.reason);
            }
        }
    );
}

} // namespace strmctrl
