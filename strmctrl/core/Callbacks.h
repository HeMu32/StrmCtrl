#pragma once

#include <functional>
#include <string>

#include "Message.h"
#include "VideoFrame.h"

namespace strmctrl {

/**
 * @brief 文本消息到达时的回调类型。
 *
 * 在 SignalingChannel 的内部线程中被调用，实现应尽快返回。
 * @param msg  收到的文本消息（含 sender_id 与 timestamp_ms）
 */
using MessageCallback = std::function<void(const TextMessage& msg)>;

/**
 * @brief 解码视频帧到达时的回调类型。
 *
 * 在 RtpReceiver / VideoDecoder 的内部线程中被调用，实现应尽快返回。
 * 若需在 callback 之外保留帧，应调用 frame.clone()。
 * @param frame  当前解码帧（生命周期仅覆盖 callback 执行期间）
 */
using VideoFrameCallback = std::function<void(const VideoFrame& frame)>;

/**
 * @brief 连接状态变化时的回调类型。
 *
 * @param connected  true = 连接建立；false = 连接断开
 * @param info       附加信息字符串（如远端地址或错误原因）
 */
using ConnectionCallback = std::function<void(bool connected,
                                              const std::string& info)>;

} // namespace strmctrl
