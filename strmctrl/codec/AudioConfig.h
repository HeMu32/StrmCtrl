#pragma once

#include <string>

namespace strmctrl {

/**
 * @brief 音频编码器配置参数。
 *
 * 包含编码器名称、采样率、声道数、码率等。
 */
struct AudioConfig {
    std::string codec_name; ///< 编码器名称，如 "aac", "opus"
    int sample_rate;        ///< 采样率，如 48000
    int channels;           ///< 声道数，如 2
    int bit_rate;           ///< 目标码率（bps），如 128000

    /**
     * @brief 创建 AAC 编码器配置。
     * @param sample_rate 采样率（默认 48000）
     * @param channels 声道数（默认 2）
     * @param bit_rate 码率（默认 128kbps）
     * @return AudioConfig 实例
     */
    static AudioConfig makeAAC(int sample_rate = 48000, int channels = 2, int bit_rate = 128000) {
        return AudioConfig{
            "aac",
            sample_rate,
            channels,
            bit_rate
        };
    }

    /**
     * @brief 创建 Opus 编码器配置。
     * @param sample_rate 采样率（默认 48000）
     * @param channels 声道数（默认 2）
     * @param bit_rate 码率（默认 64kbps）
     * @return AudioConfig 实例
     */
    static AudioConfig makeOpus(int sample_rate = 48000, int channels = 2, int bit_rate = 64000) {
        return AudioConfig{
            "opus",
            sample_rate,
            channels,
            bit_rate
        };
    }
};

} // namespace strmctrl
