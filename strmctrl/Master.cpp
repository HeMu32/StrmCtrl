#include "Master.h"

#include <iostream>

namespace strmctrl
{

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

Master::Master()
    : video_cfg_(CodecConfig::makeOpenH264()), active_video_cfg_(video_cfg_)
{
}

Master::~Master()
{
    stop();
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

void Master::setSignalingPort(int port) { signaling_port_ = port; }
void Master::setRtpPort(int port) { rtp_port_ = port; }
void Master::setCodecConfig(const CodecConfig &cfg)
{
    video_cfg_ = cfg;
    if (!running_)
    {
        active_video_cfg_ = cfg;
    }
}
void Master::setAudioConfig(const AudioConfig &cfg)
{
    audio_cfg_ = cfg;
    has_audio_cfg_ = true;
}

void Master::setMessageCallback(MessageCallback cb)
{
    msg_cb_ = cb;
    if (signaling_)
        signaling_->setMessageCallback(cb);
}

void Master::setConnectionCallback(ConnectionCallback cb)
{
    conn_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool Master::start()
{
    if (running_)
        return false;

    // 初始化视频编码器
    active_video_cfg_ = video_cfg_;
    video_encoder_ = std::make_unique<VideoEncoder>(active_video_cfg_);
    if (!video_encoder_->open())
    {
        std::cerr << "VideoEncoder::open failed: " << video_encoder_->lastError() << "\n";
        return false;
    }

    // 初始化音频编码器（如果配置了）
    if (has_audio_cfg_)
    {
        audio_encoder_ = std::make_unique<AudioEncoder>(audio_cfg_);
        if (!audio_encoder_->open())
        {
            std::cerr << "AudioEncoder::open failed: " << audio_encoder_->lastError() << "\n";
            return false;
        }
    }

    // 创建信令通道（服务端模式）
    signaling_ = SignalingChannel::createServer(signaling_port_);

    // 转发用户消息 callback
    if (msg_cb_)
        signaling_->setMessageCallback(msg_cb_);

    // 内部控制消息 callback
    signaling_->setSdpCallback([this](const std::string &sdp)
                                {
    if (sdp.rfind("REQUEST", 0) == 0) {
        onSdpRequest(sdp);
    } });

    signaling_->setReadyCallback([this]()
                                    { onReady(); });

    // 连接状态 callback
    signaling_->setConnectionCallback(
        [this](bool connected, const std::string &info)
        {
            if (connected)
            {
                onSlaveConnected(info);
            }
            else
            {
                onSlaveDisconnected();
            }
            if (conn_cb_)
                conn_cb_(connected, info);
        });

    if (!signaling_->start())
    {
        std::cerr << "SignalingChannel::start failed\n";
        return false;
    }

    running_ = true;
    return true;
}

void Master::stop()
{
    if (!running_)
        return;
    running_ = false;

    if (video_encoder_)
    {
        video_encoder_->flush();
        video_encoder_->close();
    }
    if (audio_encoder_)
    {
        audio_encoder_->flush();
        audio_encoder_->close();
    }

    {
        std::lock_guard<std::mutex> lock(rtp_sender_mutex_);
        if (rtp_sender_)
            rtp_sender_->close();
    }

    if (signaling_)
        signaling_->stop();

    rtp_ready_ = false;
}

// ---------------------------------------------------------------------------
// 推流 / 发送
// ---------------------------------------------------------------------------

void Master::pushVideoFrame(const VideoFrame &frame)
{
    if (!video_encoder_ || !running_ || !rtp_ready_)
        return;
    video_encoder_->encode(frame.avFrame());
}

void Master::pushAudioFrame(const AudioFrame &frame)
{
    if (!audio_encoder_ || !running_ || !rtp_ready_)
        return;
    if (!audio_encoder_->encode(frame))
    {
        std::cerr << "[Master] audio encode failed: " << audio_encoder_->lastError() << "\n";
    }
}

void Master::sendMessage(const std::string &text)
{
    if (signaling_)
        signaling_->sendMessage(TextMessage::make(text, "master"));
}

bool Master::registerPrefixCallback(const std::string &prefix,
                                    SignalingChannel::PrefixCallback cb)
{
    if (!signaling_)
    {
        return false;
    }
    return signaling_->registerPrefixCallback(prefix, std::move(cb));
}

bool Master::sendPrefixedMessage(const std::string &prefix, const std::string &payload)
{
    if (!signaling_)
    {
        return false;
    }
    return signaling_->sendPrefixed(prefix, payload);
}

// ---------------------------------------------------------------------------
// 内部回调处理
// ---------------------------------------------------------------------------

void Master::onSlaveConnected(const std::string &info)
{
    // info 格式为 "ip:port"（e.g. "127.0.0.1:52390" 或 "[::1]:52390"）
    // addStream 只需要纯 IP，需要把 :port 部分去掉。
    if (!info.empty() && info.front() == '[')
    {
        // IPv6: "[::1]:52390" → "::1"
        const auto bracket = info.find(']');
        current_slave_ip_ = (bracket != std::string::npos)
                                ? info.substr(1, bracket - 1)
                                : info;
    }
    else
    {
        // IPv4: "127.0.0.1:52390" → "127.0.0.1"
        const auto colon = info.rfind(':');
        current_slave_ip_ = (colon != std::string::npos)
                                ? info.substr(0, colon)
                                : info;
    }

    rtp_ready_ = false;
    std::cout << "[Master] Slave connected from " << info
                << " (RTP target: " << current_slave_ip_ << ")\n";
}

void Master::onSlaveDisconnected()
{
    current_slave_ip_.clear();
    rtp_ready_ = false;
    {
        std::lock_guard<std::mutex> lock(rtp_sender_mutex_);
        if (rtp_sender_)
        {
            rtp_sender_->close();
            rtp_sender_.reset();
        }
    }
    std::cout << "[Master] Slave disconnected\n";
}

void Master::onSdpRequest(const std::string &payload)
{
    std::cout << "[Master] Received SDP:REQUEST, preparing RTP sender...\n";

    std::string request_payload = payload;
    const std::string request_prefix = "REQUEST";
    if (request_payload.rfind(request_prefix, 0) == 0)
    {
        request_payload = request_payload.substr(request_prefix.size());
    }

    VideoConfigRequest req = parseVideoConfigRequest(request_payload);
    // apply request but never change fps: master dictates the frame rate, to avoid complex resampling. 
    active_video_cfg_ = applyVideoRequestWithCaps(video_cfg_, req);
    // enforce assertion - fps must remain equal to configured value
    // (nothing the slave asks for should change this)
    active_video_cfg_.fps = video_cfg_.fps;
    // DEBUG CHECK (non-fatal):
    if (active_video_cfg_.fps != video_cfg_.fps) {
        std::cerr << "[Master] warning: fps override ignored, "
                  << "configured=" << video_cfg_.fps
                  << " requested=" << req.fps.value_or(-1) << "\n";
        active_video_cfg_.fps = video_cfg_.fps;
    }
        // !!! ASSERTION !!!
        // The negotiated video configuration sent back to the slave must
        // *always* advertise the master's own fps value.  Downstream code
        // (RtpSender, eventual slave) relies on this invariant to compute
        // RTP timestamps.  Violating it triggered subtle A/V sync bugs during
        // testing, so we keep this comment as a reminder and a sanity check.

    if (video_encoder_)
    {
        video_encoder_->flush();
        video_encoder_->close();
    }
    video_encoder_ = std::make_unique<VideoEncoder>(active_video_cfg_);
    if (!video_encoder_->open())
    {
        std::cerr << "VideoEncoder::open failed: " << video_encoder_->lastError() << "\n";
        return;
    }

    std::unique_ptr<RtpSender> sender = std::make_unique<RtpSender>();
    // 同机调试时，显式给发送端分配远离接收端口的本地端口，避免 bind 冲突。
    // 例如 rtp_port_=11452 时，本地发送端从 11552 开始分配：
    //   流0: 11552/11553, 流1: 11554/11555 ...
    sender->setLocalPortBase(rtp_port_ + 100);

    int v_idx = sender->addStream(current_slave_ip_, rtp_port_, video_encoder_->codecContext());
    if (v_idx < 0)
    {
        std::cerr << "[Master] Failed to add video stream to RtpSender\n";
        return;
    }

    int a_idx = -1;
    if (audio_encoder_)
    {
        // 音频 RTP 用 rtp_port_ + 2，为视频 RTP/RTCP(+0/+1) 和音频 RTCP(+3) 留出空间
        a_idx = sender->addStream(current_slave_ip_, rtp_port_ + 2, audio_encoder_->codecContext());
        if (a_idx < 0)
        {
            std::cerr << "[Master] Failed to add audio stream to RtpSender\n";
            return;
        }
    }

    if (!sender->open())
    {
        std::cerr << "[Master] RtpSender::open failed: " << sender->lastError() << "\n";
        return;
    }

    signaling_->sendVideoConfig(serializeVideoConfig(active_video_cfg_));

    // 绑定编码器输出到发送器
    video_encoder_->setPacketCallback([this, v_idx](AVPacket *pkt)
                                        {
    std::lock_guard<std::mutex> lock(rtp_sender_mutex_);
    if (rtp_sender_) {
        rtp_sender_->sendPacket(pkt, v_idx);
    } });

    if (audio_encoder_)
    {
        audio_encoder_->setPacketCallback([this, a_idx](AVPacket *pkt)
                                            {
        std::lock_guard<std::mutex> lock(rtp_sender_mutex_);
        if (rtp_sender_) {
            rtp_sender_->sendPacket(pkt, a_idx);
        } });
    }

    std::string sdp = sender->generateSdp();
    if (sdp.empty())
    {
        std::cerr << "[Master] Failed to generate SDP\n";
        return;
    }

    {
        std::lock_guard<std::mutex> lock(rtp_sender_mutex_);
        rtp_sender_ = std::move(sender);
    }

    signaling_->sendSdp(sdp);
    std::cout << "[Master] Sent SDP offer to slave\n";

    // We can start streaming immediately so that the slave's avformat_find_stream_info
    // can receive packets and not block.
    rtp_ready_ = true;
}

void Master::onReady()
{
    std::cout << "[Master] Received READY, starting RTP stream...\n";
    rtp_ready_ = true;
}

} // namespace strmctrl
