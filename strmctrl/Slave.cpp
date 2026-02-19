#include "Slave.h"

#include <iostream>
#include <thread>
#include <chrono>

namespace strmctrl {

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

Slave::Slave() = default;

Slave::~Slave()
{
    disconnect();
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

void Slave::setMessageCallback(MessageCallback cb)     { msg_cb_   = std::move(cb); }
void Slave::setVideoFrameCallback(VideoFrameCallback cb){ frame_cb_ = std::move(cb); }
void Slave::setConnectionCallback(ConnectionCallback cb){ conn_cb_  = std::move(cb); }

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool Slave::connect(const std::string& master_host,
                    int                signaling_port,
                    int                rtp_port)
{
    rtp_port_  = rtp_port;

    // 创建信令通道（客户端模式）
    signaling_ = SignalingChannel::createClient(master_host, signaling_port);

    // 转发用户消息 callback
    if (msg_cb_) signaling_->setMessageCallback(msg_cb_);

    // 连接状态 callback
    signaling_->setConnectionCallback(
        [this](bool connected, const std::string& info) {
            if (connected) {
                // 连接建立后立即向主端请求 SDP
                // 使用内部 SDP 请求消息（以 SDP: 前缀但内容为 "REQUEST"）
                signaling_->sendSdp("REQUEST");
                std::cout << "[Slave] Connected to master, SDP request sent.\n";
            }
            if (conn_cb_) conn_cb_(connected, info);
        }
    );

    // SDP 接收 callback
    signaling_->setSdpCallback(
        [this](const std::string& sdp) {
            if (sdp == "REQUEST") {
                // 这是主端收到的请求，从端不应收到；忽略
                return;
            }
            onSdpReceived(sdp);
        }
    );

    if (!signaling_->start()) {
        last_error_ = "SignalingChannel::start failed";
        return false;
    }

    return true;
}

void Slave::disconnect()
{
    if (receiver_) {
        receiver_->stop();
        receiver_.reset();
    }
    if (signaling_) {
        signaling_->stop();
        signaling_.reset();
    }
}

bool Slave::isConnected() const
{
    return signaling_ && signaling_->isConnected();
}

// ---------------------------------------------------------------------------
// 发送
// ---------------------------------------------------------------------------

bool Slave::sendMessage(const std::string& text)
{
    if (!signaling_) return false;
    return signaling_->sendMessage(TextMessage::make(text, "slave"));
}

// ---------------------------------------------------------------------------
// 私有
// ---------------------------------------------------------------------------

void Slave::onSdpReceived(const std::string& sdp)
{
    std::cout << "[Slave] SDP received, initializing RTP receiver...\n";

    receiver_ = std::make_unique<RtpReceiver>();

    if (frame_cb_) receiver_->setFrameCallback(frame_cb_);

    receiver_->setErrorCallback([](const std::string& err) {
        std::cerr << "[Slave] RtpReceiver error: " << err << "\n";
    });

    if (!receiver_->openWithSdp(sdp)) {
        last_error_ = "RtpReceiver::openWithSdp failed: " + receiver_->lastError();
        std::cerr << "[Slave] " << last_error_ << "\n";
        receiver_.reset();
        return;
    }

    receiver_->start();
    std::cout << "[Slave] RTP receiver started on port " << rtp_port_ << "\n";

    // 通知主端：接收端已就绪，可以开始推流
    signaling_->sendReady();
    std::cout << "[Slave] READY sent to master.\n";
}

} // namespace strmctrl
