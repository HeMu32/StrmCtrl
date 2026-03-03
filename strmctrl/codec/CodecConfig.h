#pragma once

#include <string>
#include <map>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace strmctrl {

/**
 * @brief 编解码器配置参数。
 *
 * 通过 codec_name 字段字符串选择编解码器，使得 VideoEncoder
 * 可以在不修改架构的前提下支持多种编码器。
 *
 * ### 预定义编码器名称
 * | codec_name       | 说明                          | 备注               |
 * |------------------|-------------------------------|--------------------|
 * | "libopenh264"    | H.264 软件编码（默认）        | 需 FFmpeg 含此库   |
 * | "libx264"        | H.264 软件编码（高质量）      | GPL，需额外授权    |
 * | "h264_qsv"       | Intel Quick Sync H.264 硬编   | 预留               |
 * | "h264_nvenc"     | NVIDIA NVENC H.264 硬编       | 预留               |
 * | "h264_amf"       | AMD AMF H.264 硬编            | 预留               |
 * | "hevc_qsv"       | Intel Quick Sync H.265 硬编   | 预留               |
 * | "hevc_nvenc"     | NVIDIA NVENC H.265 硬编       | 预留               |
 * | "hevc_amf"       | AMD AMF H.265 硬编            | 预留               |
 *
 * ### extra_opts 用法示例（libopenh264）
 * @code
 * CodecConfig cfg;
 * cfg.codec_name = "libopenh264";
 * cfg.extra_opts["profile"] = "baseline";
 * @endcode
 */
struct CodecConfig {
    // -----------------------------------------------------------------------
    // 基础参数
    // -----------------------------------------------------------------------

    /** @brief FFmpeg 编码器名称，驱动编码器选择（见上表）。默认为 "libopenh264"。 */
    std::string codec_name = "libopenh264";

    /** @brief 输出帧宽度（像素）。 */
    int width  = 1280;

    /** @brief 输出帧高度（像素）。 */
    int height = 720;

    /** @brief 目标帧率（fps）。 */
    int fps = 30;

    /** @brief 目标码率（kbps）。 */
    int bitrate_kbps = 2000;

    /**
     * @brief 编码器输入期望的像素格式。
     * libopenh264 通常期望 AV_PIX_FMT_YUV420P；
     * 若源帧格式不同，VideoEncoder 会通过 swscale 自动转换。
     */
    AVPixelFormat pixel_format = AV_PIX_FMT_YUV420P;

    // -----------------------------------------------------------------------
    // 扩展选项
    // -----------------------------------------------------------------------

    /**
     * @brief 透传给 avcodec_open2 的 codec-specific 选项。
     *
     * 键值均为字符串，对应 AVOption 名称。例如：
     * - libopenh264: { "profile", "baseline" }
     * - h264_qsv:    { "preset", "veryfast" }
     */
    std::map<std::string, std::string> extra_opts;

    // -----------------------------------------------------------------------
    // 便捷工厂方法
    // -----------------------------------------------------------------------

    /**
     * @brief 创建适合 RTP 推流的 libopenh264 基线配置。
     * @param w         宽度
     * @param h         高度
     * @param fps       帧率
     * @param kbps      码率
     */
    static CodecConfig makeOpenH264(int w = 1280, int h = 720,
                                    int fps = 30,  int kbps = 2000)
    {
        CodecConfig c;
        c.codec_name   = "libopenh264";
        c.width        = w;
        c.height       = h;
        c.fps          = fps;
        c.bitrate_kbps = kbps;
        // 注意：libopenh264 不接受字符串 profile 选项（如 "baseline"）
        // 编码器会自动使用适合 RTP 的 profile，无需手动设置
        return c;
    }
};

} // namespace strmctrl
