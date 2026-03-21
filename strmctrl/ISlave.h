#pragma once

#include <string>
#include <optional>
#include "core/Callbacks.h"
#include "core/VideoConfig.h"
#include "codec/CodecConfig.h"

namespace strmctrl
{

/**
 * @brief 从端接口类（抽象基类），用于提供可拓展的连接协议门面。
 */
class ISlave
{
public:
    virtual ~ISlave() = default;

    virtual void setMessageCallback(MessageCallback cb) = 0;
    virtual void setVideoFrameCallback(VideoFrameCallback cb) = 0;
    virtual void setAudioFrameCallback(AudioFrameCallback cb) = 0;
    virtual void setConnectionCallback(ConnectionCallback cb) = 0;
    virtual void setVideoConfigRequest(const VideoConfigRequest &req) = 0;

    virtual std::optional<CodecConfig> negotiatedVideoConfig() const noexcept = 0;

    virtual void disconnect() = 0;
    virtual bool isConnected() const noexcept = 0;

    virtual void sendMessage(const std::string &text) = 0;
};

} // namespace strmctrl
