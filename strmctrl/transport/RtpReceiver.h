#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "../codec/VideoDecoder.h"
#include "../codec/AudioDecoder.h"
#include "../core/Callbacks.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace strmctrl {

/**
 * @brief RTP 接收器，在独立线程中持续接收并解码来自主端的 RTP 音视频流。
 *
 * RtpReceiver 通过 FFmpeg 的 avformat 层打开 "sdp://" 或 "rtp://" 输入，
 * 并在内部工作线程中循环调用 av_read_frame()。
 * 根据 stream_index 将包分发给 VideoDecoder 或 AudioDecoder。
 * 解码结果通过对应的 Callback 在工作线程中同步回调。
 */
class RtpReceiver {
public:
    // -----------------------------------------------------------------------
    // 构造 / 析构
    // -----------------------------------------------------------------------

    RtpReceiver();
    ~RtpReceiver();

    RtpReceiver(const RtpReceiver&)            = delete;
    RtpReceiver& operator=(const RtpReceiver&) = delete;

    // -----------------------------------------------------------------------
    // 配置
    // -----------------------------------------------------------------------

    /**
     * @brief 注册视频解码帧到达 callback（在工作线程中调用，应尽快返回）。
     */
    void setVideoFrameCallback(VideoFrameCallback cb);

    /**
     * @brief 注册音频解码帧到达 callback（在工作线程中调用，应尽快返回）。
     */
    void setAudioFrameCallback(AudioFrameCallback cb);

    /**
     * @brief 注册错误 callback（工作线程遇到致命错误时调用）。
     * @param cb  参数为错误描述字符串
     */
    void setErrorCallback(std::function<void(const std::string&)> cb);

    // -----------------------------------------------------------------------
    // 生命周期
    // -----------------------------------------------------------------------

    /**
     * @brief 通过 SDP 字符串初始化 RTP 输入上下文。
     *
     * 将 SDP 写入临时文件，由 FFmpeg 以 sdp 格式解析流参数并初始化解码器。
     * 该方法是阻断的，返回后即可调用 start()。
     *
     * @param sdp  SDP 字符串（由 RtpSender::generateSdp() 生成并经信令通道传输）
     * @return     true 表示成功；false 表示 FFmpeg 无法解析 SDP（见 lastError()）
     */
    bool openWithSdp(const std::string& sdp);

    /**
     * @brief 直接通过 RTP URL 打开输入（调试或无 SDP 协商时使用）。
     * @param host  发送方 IP
     * @param port  RTP 端口
     * @return      true 表示成功
     */
    bool openWithUrl(const std::string& host, int port);

    /**
     * @brief 启动内部工作线程，开始接收并解码 RTP 包。
     *
     * 必须在 openWithSdp() 或 openWithUrl() 成功后调用。
     */
    void start();

    /**
     * @brief 停止工作线程并关闭输入上下文。
     *
     * 阻塞直到工作线程退出。
     */
    void stop();

    /** @brief 接收器是否正在运行。 */
    bool isRunning() const noexcept { return running_.load(); }

    // -----------------------------------------------------------------------
    // 访问器
    // -----------------------------------------------------------------------

    std::string lastError() const noexcept { return last_error_; }

private:
    void workerThread();

    AVFormatContext* fmt_ctx_ = nullptr;
    int video_stream_idx_ = -1;
    int audio_stream_idx_ = -1;

    VideoDecoder video_decoder_;
    AudioDecoder audio_decoder_;

    VideoFrameCallback video_cb_;
    AudioFrameCallback audio_cb_;
    std::function<void(const std::string&)> error_cb_;

    std::atomic<bool> running_{false};
    std::thread       thread_;
    std::string       last_error_;

    // TimeoutContext management
    struct TimeoutContext {
        std::atomic<int64_t> last_activity_ts; // steady_clock::now().time_since_epoch().count() (ms)
        int                  timeout_ms;
    };
    TimeoutContext* timeout_ctx_ = nullptr;
};

} // namespace strmctrl
