#pragma once

#include <string>
#include "core/Callbacks.h"
#include "codec/CodecConfig.h"
#include "codec/AudioConfig.h"

namespace strmctrl
{

/**
 * @brief 主端接口类（抽象基类），用于提供可拓展的连接协议门面。
 */
class IMaster
{
public:
    virtual ~IMaster() = default;

    virtual void setCodecConfig(const CodecConfig &cfg) = 0;
    virtual void setAudioConfig(const AudioConfig &cfg) = 0;
    virtual void setMessageCallback(MessageCallback cb) = 0;
    virtual void setConnectionCallback(ConnectionCallback cb) = 0;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const noexcept = 0;

    virtual void pushVideoFrame(const VideoFrame &frame) = 0;
    virtual void pushAudioFrame(const AudioFrame &frame) = 0;

    virtual void sendMessage(const std::string &text) = 0;
};

} // namespace strmctrl
