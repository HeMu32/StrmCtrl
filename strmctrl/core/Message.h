#pragma once

#include <string>
#include <cstdint>
#include <chrono>

namespace strmctrl {

/**
 * @brief 文本消息，在主端与从端之间通过 WebSocket 信令通道传输。
 *
 * 该结构体同时用于发送和接收场景。发送时可仅填充 text；
 * 接收时 sender_id 与 timestamp_ms 由 SignalingChannel 自动填充。
 */
struct TextMessage {
    /** 消息内容（UTF-8 编码） */
    std::string text;

    /**
     * @brief 发送方标识符。
     *
     * 主端广播时为 "master"；从端发送时为连接时协商的 ID
     * （当前实现中为远端 IP:Port 字符串）。
     */
    std::string sender_id;

    /**
     * @brief 消息发出时的 Unix 时间戳（毫秒）。
     * 由发送方在 SignalingChannel 打包时写入。
     */
    int64_t timestamp_ms = 0;

    // -----------------------------------------------------------------------
    // 工厂方法
    // -----------------------------------------------------------------------

    /**
     * @brief 用当前系统时间创建一条文本消息。
     * @param text     消息内容
     * @param sender   发送方 ID（默认为空，由 SignalingChannel 覆写）
     * @return         填充好时间戳的 TextMessage 实例
     */
    static TextMessage make(const std::string& text,
                            const std::string& sender = "")
    {
        using namespace std::chrono;
        TextMessage msg;
        msg.text        = text;
        msg.sender_id   = sender;
        msg.timestamp_ms = duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
        return msg;
    }
};

} // namespace strmctrl
