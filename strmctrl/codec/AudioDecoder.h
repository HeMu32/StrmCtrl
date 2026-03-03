#pragma once

#include <string>

#include "../core/AudioFrame.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace strmctrl {

/**
 * @brief 音频解码器，将 AVPacket 解码为 AudioFrame。
 */
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&)            = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    /**
     * @brief 使用指定的编解码器参数打开解码器。
     * @param par 包含解码器信息的参数（通常来自 AVStream）
     * @return true 表示成功；false 表示失败
     */
    bool openWithParameters(const AVCodecParameters* par);

    /**
     * @brief 关闭解码器并释放资源。
     */
    void close();

    bool isOpen() const noexcept { return codec_ctx_ != nullptr; }

    /**
     * @brief 解码一个数据包。
     * @param pkt 待解码的数据包
     * @param out_frame 解码成功后输出的音频帧
     * @return true 表示成功解码出一帧；false 表示需要更多数据或发生错误
     */
    bool decode(const AVPacket* pkt, AudioFrame& out_frame);

    std::string lastError() const noexcept { return last_error_; }

private:
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    std::string last_error_;
};

} // namespace strmctrl
