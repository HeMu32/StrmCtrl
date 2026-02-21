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
void Slave::setVideoFrameCallback(VideoFrameCallback cb){ video_cb_ = std::move(cb); }
void Slave::setAudioFrameCallback(AudioFrameCallback cb){ audio_cb_ = std::move(cb); }
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
                onConnected();
            } else {
                onDisconnected();
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
        std::cerr << "SignalingChannel::start failed\n";
        return false;
    }

    return true;
}

void Slave::disconnect()
{
    if (rtp_receiver_) {
        rtp_receiver_->stop();
        rtp_receiver_.reset();
    }
    if (signaling_) {
        signaling_->stop();
        signaling_.reset();
    }
    connected_ = false;
}

// ---------------------------------------------------------------------------
// 发送
// ---------------------------------------------------------------------------

void Slave::sendMessage(const std::string& text)
{
    if (!signaling_) return;
    signaling_->sendMessage(TextMessage::make(text, "slave"));
}

// ---------------------------------------------------------------------------
// 内部回调处理
// ---------------------------------------------------------------------------

void Slave::onConnected()
{
    connected_ = true;
    // 连接建立后立即向主端请求 SDP
    signaling_->sendSdp("REQUEST");
    std::cout << "[Slave] Connected to master, SDP request sent.\n";
}

void Slave::onDisconnected()
{
    connected_ = false;
    if (rtp_receiver_) {
        rtp_receiver_->stop();
        rtp_receiver_.reset();
    }
    std::cout << "[Slave] Disconnected from master.\n";
}

void Slave::onSdpReceived(const std::string& sdp)
{
    std::cout << "[Slave] Received SDP offer, initializing RTP receiver...\n";

    rtp_receiver_ = std::make_unique<RtpReceiver>();

    if (video_cb_) rtp_receiver_->setVideoFrameCallback(video_cb_);
    if (audio_cb_) rtp_receiver_->setAudioFrameCallback(audio_cb_);

    rtp_receiver_->setErrorCallback([](const std::string& err) {
        std::cerr << "[Slave] RtpReceiver error: " << err << "\n";
    });

    if (!rtp_receiver_->openWithSdp(sdp)) {
        std::cerr << "[Slave] RtpReceiver::openWithSdp failed: " << rtp_receiver_->lastError() << "\n";
        return;
    }

    rtp_receiver_->start();
    std::cout << "[Slave] RTP receiver started.\n";

    // 通知主端已就绪
    signaling_->sendReady();
}

} // namespace strmctrl
