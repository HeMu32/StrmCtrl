#include "Master.h"

#include <iostream>

namespace strmctrl {

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

Master::Master()
    : codec_cfg_(CodecConfig::makeOpenH264())
{}

Master::~Master()
{
    stop();
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

void Master::setSignalingPort(int port)   { signaling_port_ = port; }
void Master::setRtpPort(int port)         { rtp_port_       = port; }
void Master::setCodecConfig(const CodecConfig& cfg) { codec_cfg_ = cfg; }

void Master::setMessageCallback(MessageCallback cb)    { msg_cb_  = std::move(cb); }
void Master::setConnectionCallback(ConnectionCallback cb) { conn_cb_ = std::move(cb); }

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool Master::start()
{
    // 初始化编码器
    encoder_ = std::make_unique<VideoEncoder>(codec_cfg_);
    if (!encoder_->open()) {
        last_error_ = "VideoEncoder::open failed: " + encoder_->lastError();
        return false;
    }

    // 创建信令通道（服务端模式）
    signaling_ = SignalingChannel::createServer(signaling_port_);

    // 转发用户消息 callback
    if (msg_cb_) signaling_->setMessageCallback(msg_cb_);

    // 连接状态 callback：从端连接时触发 SDP 协商
    signaling_->setConnectionCallback(
        [this](bool connected, const std::string& info) {
            if (connected) {
                onSlaveConnected(info);
            } else {
                rtp_ready_ = false;
                slave_host_.clear();
            }
            if (conn_cb_) conn_cb_(connected, info);
        }
    );

    if (!signaling_->start()) {
        last_error_ = "SignalingChannel::start failed";
        return false;
    }

    running_ = true;
    return true;
}

void Master::stop()
{
    if (!running_) return;
    running_ = false;

    if (encoder_) { encoder_->flush(); encoder_->close(); }
    if (sender_)  sender_->close();
    if (signaling_) signaling_->stop();

    rtp_ready_ = false;
}

// ---------------------------------------------------------------------------
// 推流 / 发送
// ---------------------------------------------------------------------------

bool Master::pushVideoFrame(AVFrame* frame)
{
    if (!encoder_ || !running_) {
        last_error_ = "Master not running";
        return false;
    }
    if (!rtp_ready_) {
        // 从端尚未就绪，丢弃该帧（正常情况，无需报错）
        return true;
    }
    bool ok = encoder_->encode(frame);
    if (!ok) {
        last_error_ = "encode: " + encoder_->lastError();
    }
    return ok;
}

bool Master::sendMessage(const std::string& text)
{
    if (!signaling_) return false;
    return signaling_->sendMessage(TextMessage::make(text, "master"));
}

// ---------------------------------------------------------------------------
// 访问器
// ---------------------------------------------------------------------------

bool Master::hasSlaveConnected() const
{
    return signaling_ && signaling_->isConnected();
}

// ---------------------------------------------------------------------------
// 私有
// ---------------------------------------------------------------------------

void Master::onSlaveConnected(const std::string& slave_addr)
{
    // 从 "IP:Port" 中解析出 IP
    const auto colon = slave_addr.rfind(':');
    slave_host_ = (colon != std::string::npos)
                ? slave_addr.substr(0, colon)
                : slave_addr;

    // 初始化 RtpSender，绑定到从端的 RTP 端口
    sender_ = std::make_unique<RtpSender>();
    if (!sender_->open(slave_host_, rtp_port_, encoder_->codecContext())) {
        last_error_ = "RtpSender::open failed: " + sender_->lastError();
        std::cerr << "[Master] " << last_error_ << "\n";
        return;
    }

    // 将编码器输出绑定到 sender
    bindEncoderToSender();

    // 生成 SDP 并通过信令通道发送给从端
    const std::string sdp = sender_->generateSdp();
    if (!signaling_->sendSdp(sdp)) {
        std::cerr << "[Master] Failed to send SDP to slave\n";
        return;
    }
    std::cout << "[Master] SDP sent to slave at " << slave_host_ << "\n";

    // 注册 READY 回调：从端就绪后才开放推流
    signaling_->setReadyCallback([this]() {
        rtp_ready_ = true;
        std::cout << "[Master] Slave READY - starting RTP stream.\n";
    });
}

void Master::bindEncoderToSender()
{
    encoder_->setPacketCallback(
        [this](AVPacket* pkt) {
            if (sender_ && rtp_ready_) {
                if (!sender_->sendPacket(pkt)) {
                    std::cerr << "[Master] RtpSender error: "
                              << sender_->lastError() << "\n";
                }
            }
        }
    );
}

} // namespace strmctrl
