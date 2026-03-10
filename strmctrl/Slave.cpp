#include "Slave.h"

#include <iostream>

namespace strmctrl
{

namespace
{

std::shared_ptr<SignalingChannel> toShared(std::unique_ptr<SignalingChannel> signaling)
{
    return std::shared_ptr<SignalingChannel>(signaling.release());
}

} // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

Slave::Slave()
    : HostBase("slave")
{
}

Slave::~Slave()
{
    disconnect();
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

void Slave::setVideoFrameCallback(VideoFrameCallback cb)
{
    std::shared_ptr<RtpReceiver> receiver;
    {
        std::lock_guard<std::mutex> lock(frame_callback_mutex_);
        video_cb_ = cb;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        receiver = rtp_receiver_;
    }

    if (receiver)
        receiver->setVideoFrameCallback(std::move(cb));
}

void Slave::setAudioFrameCallback(AudioFrameCallback cb)
{
    std::shared_ptr<RtpReceiver> receiver;
    {
        std::lock_guard<std::mutex> lock(frame_callback_mutex_);
        audio_cb_ = cb;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        receiver = rtp_receiver_;
    }

    if (receiver)
        receiver->setAudioFrameCallback(std::move(cb));
}

void Slave::setVideoConfigRequest(const VideoConfigRequest &req)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    video_req_ = req;
    has_video_req_ = true;
}

bool Slave::connect(const std::string &master_host, int signaling_port, int rtp_port)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (connected_ || signalingChannel())
            return false;

        rtp_port_ = rtp_port;
        ++session_generation_;
    }

    // 创建信令通道（客户端模式）
    auto signaling = toShared(SignalingChannel::createClient(master_host, signaling_port));
    setSignalingChannel(signaling);
    applyStoredCallbacksToSignaling();

    // 连接状态 callback
    signaling->setConnectionCallback(
        [this](bool connected, const std::string &info)
        {
            if (connected)
                onConnected();
            else
                onDisconnected();

            auto cb = storedConnectionCallback();
            if (cb)
                cb(connected, info);
        });

    // SDP 接收 callback
    signaling->setSdpCallback(
        [this](const std::string &sdp)
        {
            if (sdp == "REQUEST")
            {
                // 这是主端收到的请求，从端不应收到；忽略
                return;
            }

            onSdpReceived(sdp);
        });

    signaling->setVideoConfigCallback(
        [this](const std::string &json)
        {
            const std::string payload = trimCopy(json);
            auto negotiated = parseVideoConfig(payload);
            if (!negotiated.has_value())
                return;

            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                negotiated_video_cfg_ = negotiated;
            }

            const auto &cfg = *negotiated;
            std::cout << "[Slave] Negotiated video config: "
                      << cfg.width << "x" << cfg.height
                      << "@" << cfg.fps << "fps"
                      << " " << cfg.bitrate_kbps << "kbps"
                      << " codec=" << cfg.codec_name << "\n";
        });

    if (!signaling->start())
    {
        std::cerr << "SignalingChannel::start failed\n";
        resetSignalingChannel();
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void Slave::disconnect()
{
    auto signaling = resetSignalingChannel();
    std::shared_ptr<RtpReceiver> receiver;
    std::thread init_thread;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        connected_ = false;
        ++session_generation_;
        receiver = std::move(rtp_receiver_);
        init_thread = std::move(init_thread_);
    }

    if (signaling)
    {
        signaling->setConnectionCallback(nullptr);
        signaling->setSdpCallback(nullptr);
        signaling->setVideoConfigCallback(nullptr);
    }

    if (init_thread.joinable())
        init_thread.join();

    if (receiver)
        receiver->stop();

    if (signaling)
        signaling->stop();
}

bool Slave::isConnected() const noexcept
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return connected_;
}

// ---------------------------------------------------------------------------
// 内部回调处理
// ---------------------------------------------------------------------------

void Slave::onConnected()
{
    bool has_video_req = false;
    VideoConfigRequest video_req;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        connected_ = true;
        has_video_req = has_video_req_;
        video_req = video_req_;
    }

    auto signaling = signalingChannel();
    if (!signaling)
        return;

    // 连接建立后立即向主端请求 SDP
    if (has_video_req)
        signaling->sendSdp("REQUEST " + serializeVideoConfigRequest(video_req));
    else
        signaling->sendSdp("REQUEST");

    std::cout << "[Slave] Connected to master, SDP request sent.\n";
}

void Slave::onDisconnected()
{
    std::shared_ptr<RtpReceiver> receiver;
    std::thread init_thread;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        connected_ = false;
        ++session_generation_;
        receiver = std::move(rtp_receiver_);
        init_thread = std::move(init_thread_);
    }

    if (init_thread.joinable())
        init_thread.join();

    if (receiver)
        receiver->stop();

    std::cout << "[Slave] Disconnected from master.\n";
}

void Slave::onSdpReceived(const std::string &sdp)
{
    std::uint64_t session_generation = 0;
    std::thread old_thread;
    VideoFrameCallback video_cb;
    AudioFrameCallback audio_cb;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!connected_)
            return;

        session_generation = session_generation_;
        old_thread = std::move(init_thread_);
    }

    if (old_thread.joinable())
        old_thread.join();

    {
        std::lock_guard<std::mutex> lock(frame_callback_mutex_);
        video_cb = video_cb_;
        audio_cb = audio_cb_;
    }

    std::thread init_thread(
        [this, sdp, session_generation, video_cb, audio_cb]()
        {
            std::cout << "[Slave] Received SDP offer, initializing RTP receiver in background thread...\n"
                      << std::flush;

            auto receiver = std::make_shared<RtpReceiver>();
            if (video_cb)
                receiver->setVideoFrameCallback(video_cb);
            if (audio_cb)
                receiver->setAudioFrameCallback(audio_cb);

            receiver->setErrorCallback(
                [](const std::string &err)
                {
                    std::cerr << "[Slave] RtpReceiver error: " << err << "\n";
                });

            if (!receiver->openWithSdp(sdp))
            {
                std::cerr << "[Slave] RtpReceiver::openWithSdp failed: "
                          << receiver->lastError() << "\n";
                return;
            }

            std::shared_ptr<RtpReceiver> old_receiver;
            bool committed = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (connected_ && session_generation == session_generation_)
                {
                    // 安全地将就绪的 receiver 赋值给类成员
                    old_receiver = std::move(rtp_receiver_);
                    rtp_receiver_ = receiver;
                    rtp_receiver_->start();
                    committed = true;
                }
            }

            if (!committed)
            {
                // 如果在初始化期间断开了连接，则停止并丢弃
                receiver->stop();
                return;
            }

            if (old_receiver)
                old_receiver->stop();

            std::cout << "[Slave] RTP receiver started.\n" << std::flush;

            bool should_send_ready = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                should_send_ready =
                    connected_ &&
                    session_generation == session_generation_ &&
                    rtp_receiver_.get() == receiver.get();
            }

            if (!should_send_ready)
                return;

            // 通知主端已就绪
            auto signaling = signalingChannel();
            if (signaling)
                signaling->sendReady();
        });

    bool adopted = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (connected_ && session_generation == session_generation_)
        {
            init_thread_ = std::move(init_thread);
            adopted = true;
        }
    }

    if (!adopted && init_thread.joinable())
        init_thread.join();
}

} // namespace strmctrl
