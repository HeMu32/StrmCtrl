#include "Master.h"

#include <iostream>
#include <sstream>
#include <thread>

namespace strmctrl
{

namespace
{

void LogStrmCtrlMasterLifecycleText(const std::string& sMsg)
{
#if defined(_DEBUG) && defined(_DEBUG_LIFECYCLE)
    std::cerr << sMsg << std::endl;
#else
    (void)sMsg;
#endif
}

std::shared_ptr<SignalingChannel> toShared(std::unique_ptr<SignalingChannel> signaling)
{
    return std::shared_ptr<SignalingChannel>(signaling.release());
}

std::string parseRemoteIp(const std::string &info)
{
    // info 格式为 "ip:port"（e.g. "127.0.0.1:52390" 或 "[::1]:52390"）
    // addStream 只需要纯 IP，需要把 :port 部分去掉。
    if (!info.empty() && info.front() == '[')
    {
        // IPv6: "[::1]:52390" -> "::1"
        const auto bracket = info.find(']');
        return (bracket != std::string::npos) ? info.substr(1, bracket - 1) : info;
    }

    // IPv4: "127.0.0.1:52390" -> "127.0.0.1"
    const auto colon = info.rfind(':');
    return (colon != std::string::npos) ? info.substr(0, colon) : info;
}

void DisposeMasterSignalingAsync(std::shared_ptr<SignalingChannel> signaling)
{
    if (!signaling)
        return;

    std::thread(
        [signaling = std::move(signaling)]() mutable
        {
#if defined(_DEBUG) && defined(_DEBUG_LIFECYCLE)
            std::ostringstream oss;
            oss << "[Lifecycle][strmctrl::Master] async signaling dispose"
                << " signaling=" << signaling.get()
                << " thread=" << std::this_thread::get_id();
            LogStrmCtrlMasterLifecycleText(oss.str());
#endif
            signaling->stop();
        }).detach();
}

} // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

Master::Master()
    : HostBase("master"), video_cfg_(CodecConfig::makeOpenH264()), active_video_cfg_(video_cfg_)
{
}

Master::~Master()
{
    stop();
}

void Master::closeSenderState(const std::shared_ptr<SenderState> &sender_state)
{
    if (!sender_state)
        return;

    std::lock_guard<std::mutex> lock(sender_state->mutex);
    if (sender_state->sender)
    {
        sender_state->sender->close();
        sender_state->sender.reset();
    }
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

void Master::setSignalingPort(int port)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    signaling_port_ = port;
}

void Master::setRtpPort(int port)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    rtp_port_ = port;
}

void Master::setCodecConfig(const CodecConfig &cfg)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    video_cfg_ = cfg;
    if (!running_)
    {
        active_video_cfg_ = cfg;
    }
}

void Master::setAudioConfig(const AudioConfig &cfg)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    audio_cfg_ = cfg;
    has_audio_cfg_ = true;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool Master::start()
{
    CodecConfig video_cfg;
    AudioConfig audio_cfg;
    bool has_audio_cfg = false;
    int signaling_port = 0;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (running_)
            return false;

        video_cfg = video_cfg_;
        audio_cfg = audio_cfg_;
        has_audio_cfg = has_audio_cfg_;
        signaling_port = signaling_port_;
    }

    // 初始化视频编码器
    auto video_encoder = std::make_shared<VideoEncoder>(video_cfg);
    if (!video_encoder->open())
    {
        std::cerr << "VideoEncoder::open failed: " << video_encoder->lastError() << "\n";
        return false;
    }

    // 初始化音频编码器（如果配置了）
    std::shared_ptr<AudioEncoder> audio_encoder;
    if (has_audio_cfg)
    {
        audio_encoder = std::make_shared<AudioEncoder>(audio_cfg);
        if (!audio_encoder->open())
        {
            std::cerr << "AudioEncoder::open failed: " << audio_encoder->lastError() << "\n";
            return false;
        }
    }

    // 创建信令通道（服务端模式）
    auto signaling = toShared(SignalingChannel::createServer(signaling_port));
    setSignalingChannel(signaling);
    applyStoredCallbacksToSignaling();

    // 内部控制消息 callback
    signaling->setSdpCallback(
        [this](const std::string &sdp)
        {
            if (sdp.rfind("REQUEST", 0) == 0)
            {
                onSdpRequest(sdp);
            }
        });

    signaling->setReadyCallback([this]() { onReady(); });

    // 连接状态 callback
    signaling->setConnectionCallback(
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
            auto cb = storedConnectionCallback();
            if (cb)
                cb(connected, info);
        });

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        active_video_cfg_ = video_cfg;
        video_encoder_ = std::move(video_encoder);
        audio_encoder_ = std::move(audio_encoder);
        sender_state_.reset();
        current_slave_ip_.clear();
        rtp_ready_ = false;
        running_ = true;
    }

    if (!signaling->start())
    {
        std::cerr << "SignalingChannel::start failed\n";
        stop();
        return false;
    }

    return true;
}

void Master::stop()
{
    auto signaling = resetSignalingChannel();
    std::shared_ptr<VideoEncoder> video_encoder;
    std::shared_ptr<AudioEncoder> audio_encoder;
    std::shared_ptr<SenderState> sender_state;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!running_ && !video_encoder_ && !audio_encoder_ && !sender_state_ && !signaling)
            return;

        running_ = false;
        rtp_ready_ = false;
        current_slave_ip_.clear();
        active_video_cfg_ = video_cfg_;
        video_encoder = std::move(video_encoder_);
        audio_encoder = std::move(audio_encoder_);
        sender_state = std::move(sender_state_);
    }

    closeSenderState(sender_state);

    if (signaling)
    {
        // 确保 stop() 后注册的回调走持久化路径，不丢失
        signaling->setConnectionCallback(nullptr);
        signaling->setSdpCallback(nullptr);
        signaling->setReadyCallback(nullptr);
        DisposeMasterSignalingAsync(std::move(signaling));
    }
}

bool Master::isRunning() const noexcept
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return running_;
}

// ---------------------------------------------------------------------------
// 推流 / 发送
// ---------------------------------------------------------------------------

void Master::pushVideoFrame(const VideoFrame &frame)
{
    std::shared_ptr<VideoEncoder> video_encoder;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!video_encoder_ || !running_ || !rtp_ready_)
            return;
        video_encoder = video_encoder_;
    }
    video_encoder->encode(frame.avFrame());
}

void Master::pushAudioFrame(const AudioFrame &frame)
{
    std::shared_ptr<AudioEncoder> audio_encoder;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!audio_encoder_ || !running_ || !rtp_ready_)
            return;
        audio_encoder = audio_encoder_;
    }
    if (!audio_encoder->encode(frame))
    {
        std::cerr << "[Master] audio encode failed: " << audio_encoder->lastError() << "\n";
    }
}

// ---------------------------------------------------------------------------
// 内部回调处理
// ---------------------------------------------------------------------------

void Master::onSlaveConnected(const std::string &info)
{
#if defined(_DEBUG) && defined(_DEBUG_LIFECYCLE)
    std::ostringstream oss;
    oss << "[Lifecycle][strmctrl::Master] onSlaveConnected"
        << " this=" << this
        << " thread=" << std::this_thread::get_id()
        << " info=" << info;
    LogStrmCtrlMasterLifecycleText(oss.str());
#endif
    auto sender_state = std::shared_ptr<SenderState>();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!running_)
            return;

        current_slave_ip_ = parseRemoteIp(info);
        rtp_ready_ = false;
        sender_state = std::move(sender_state_);
    }

    closeSenderState(sender_state);

    std::cout << "[Master] Slave connected from " << info
              << " (RTP target: " << parseRemoteIp(info) << ")\n";
}

void Master::onSlaveDisconnected()
{
#if defined(_DEBUG) && defined(_DEBUG_LIFECYCLE)
    std::ostringstream oss;
    oss << "[Lifecycle][strmctrl::Master] onSlaveDisconnected"
        << " this=" << this
        << " thread=" << std::this_thread::get_id();
    LogStrmCtrlMasterLifecycleText(oss.str());
#endif
    auto sender_state = std::shared_ptr<SenderState>();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_slave_ip_.clear();
        rtp_ready_ = false;
        sender_state = std::move(sender_state_);
    }

    closeSenderState(sender_state);
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

    CodecConfig base_video_cfg;
    AudioConfig audio_cfg;
    bool has_audio_cfg = false;
    int rtp_port = 0;
    std::string slave_ip;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!running_ || current_slave_ip_.empty())
            return;

        base_video_cfg = video_cfg_;
        audio_cfg = audio_cfg_;
        has_audio_cfg = has_audio_cfg_;
        rtp_port = rtp_port_;
        slave_ip = current_slave_ip_;
        rtp_ready_ = false;
    }

    VideoConfigRequest req = parseVideoConfigRequest(request_payload);
    auto active_video_cfg = applyVideoRequestWithCaps(base_video_cfg, req);

    auto video_encoder = std::make_shared<VideoEncoder>(active_video_cfg);
    if (!video_encoder->open())
    {
        std::cerr << "VideoEncoder::open failed: " << video_encoder->lastError() << "\n";
        return;
    }

    std::shared_ptr<AudioEncoder> audio_encoder;
    if (has_audio_cfg)
    {
        audio_encoder = std::make_shared<AudioEncoder>(audio_cfg);
        if (!audio_encoder->open())
        {
            std::cerr << "AudioEncoder::open failed: " << audio_encoder->lastError() << "\n";
            return;
        }
    }

    auto sender_state = std::make_shared<SenderState>();
    sender_state->sender = std::make_shared<RtpSender>();
    // 同机调试时，显式给发送端分配远离接收端口的本地端口，避免 bind 冲突。
    // 例如 rtp_port_=11452 时，本地发送端从 11552 开始分配：
    //   流0: 11552/11553, 流1: 11554/11555 ...
    sender_state->sender->setLocalPortBase(rtp_port + 100);

    int v_idx = sender_state->sender->addStream(slave_ip, rtp_port, video_encoder->codecContext());
    if (v_idx < 0)
    {
        std::cerr << "[Master] Failed to add video stream to RtpSender\n";
        return;
    }

    int a_idx = -1;
    if (audio_encoder)
    {
        // 音频 RTP 用 rtp_port_ + 2，为视频 RTP/RTCP(+0/+1) 和音频 RTCP(+3) 留出空间
        a_idx = sender_state->sender->addStream(slave_ip, rtp_port + 2, audio_encoder->codecContext());
        if (a_idx < 0)
        {
            std::cerr << "[Master] Failed to add audio stream to RtpSender\n";
            return;
        }
    }

    if (!sender_state->sender->open())
    {
        std::cerr << "[Master] RtpSender::open failed: " << sender_state->sender->lastError() << "\n";
        return;
    }

    // 绑定编码器输出到发送器，但不捕获 this，避免 stop()/重协商竞态
    std::weak_ptr<SenderState> weak_sender_state = sender_state;
    video_encoder->setPacketCallback(
        [weak_sender_state, v_idx](AVPacket *pkt)
        {
            if (auto sender_state = weak_sender_state.lock())
            {
                std::lock_guard<std::mutex> lock(sender_state->mutex);
                if (sender_state->sender)
                    sender_state->sender->sendPacket(pkt, v_idx);
            }
        });

    if (audio_encoder)
    {
        audio_encoder->setPacketCallback(
            [weak_sender_state, a_idx](AVPacket *pkt)
            {
                if (auto sender_state = weak_sender_state.lock())
                {
                    std::lock_guard<std::mutex> lock(sender_state->mutex);
                    if (sender_state->sender)
                        sender_state->sender->sendPacket(pkt, a_idx);
                }
            });
    }

    auto signaling = signalingChannel();
    if (!signaling)
    {
        closeSenderState(sender_state);
        return;
    }

    if (!signaling->sendVideoConfig(serializeVideoConfig(active_video_cfg)))
    {
        std::cerr << "[Master] Failed to send negotiated video config\n";
        closeSenderState(sender_state);
        return;
    }

    std::string sdp = sender_state->sender->generateSdp();
    if (sdp.empty())
    {
        std::cerr << "[Master] Failed to generate SDP\n";
        closeSenderState(sender_state);
        return;
    }

    if (!signaling->sendSdp(sdp))
    {
        std::cerr << "[Master] Failed to send SDP offer\n";
        closeSenderState(sender_state);
        return;
    }

    std::shared_ptr<VideoEncoder> old_video_encoder;
    std::shared_ptr<AudioEncoder> old_audio_encoder;
    std::shared_ptr<SenderState> old_sender_state;
    bool stale_request = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!running_ || current_slave_ip_ != slave_ip)
        {
            stale_request = true;
        }
        else
        {
            old_video_encoder = std::move(video_encoder_);
            old_audio_encoder = std::move(audio_encoder_);
            old_sender_state = std::move(sender_state_);
            video_encoder_ = std::move(video_encoder);
            audio_encoder_ = std::move(audio_encoder);
            sender_state_ = std::move(sender_state);
            active_video_cfg_ = active_video_cfg;

            // We can start streaming immediately so that the slave's avformat_find_stream_info
            // can receive packets and not block.
            rtp_ready_ = true;
        }
    }

    if (stale_request)
    {
        closeSenderState(sender_state);
        return;
    }

    closeSenderState(old_sender_state);

    std::cout << "[Master] Sent SDP offer to slave\n";
}

void Master::onReady()
{
    std::cout << "[Master] Received READY, starting RTP stream...\n";
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (running_ && sender_state_)
        rtp_ready_ = true;
}

} // namespace strmctrl
