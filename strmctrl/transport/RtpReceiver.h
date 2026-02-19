#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "../codec/VideoDecoder.h"
#include "../core/Callbacks.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace strmctrl {

/**
 * @brief RTP 接收器，在独立线程中持续接收并解码来自主端的 RTP 视频流。
 *
 * RtpReceiver 通过 FFmpeg 的 avformat 层打开 "sdp://" 或 "rtp://" 输入，
 * 并在内部工作线程中循环调用 av_read_frame() + VideoDecoder::decode()。
 * 解码结果通过 VideoFrameCallback 在工作线程中同步回调。
 *
 * ### SDP 初始化流程
 * 从端收到 SDP 字符串（经 SignalingChannel）后，调用 openWithSdp()；
 * 该方法将 SDP 写入临时文件，再由 FFmpeg 以 "sdp" 格式打开。
 *
 * ### 典型用法
 * @code
 * strmctrl::RtpReceiver receiver;
 * receiver.setFrameCallback([](const strmctrl::VideoFrame& frame) {
 *     // 在此处理解码帧（尽快返回！）
 * });
 *
 * signaling->setSdpCallback([&](const std::string& sdp) {
 *     receiver.openWithSdp(sdp);
 *     receiver.start();
 * });
 * @endcode
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
     * @brief 注册解码帧到达 callback（在工作线程中调用，应尽快返回）。
     * @param cb  VideoFrameCallback
     */
    void setFrameCallback(VideoFrameCallback cb);

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
     * 将 SDP 写入临时内存 IO，由 FFmpeg 以 sdp 格式解析流参数并初始化解码器。
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
     * @brief 请求停止工作线程，并等待其退出。
     */
    void stop();

    /** @brief 工作线程是否正在运行。 */
    bool isRunning() const noexcept { return running_.load(); }

    // -----------------------------------------------------------------------
    // 访问器
    // -----------------------------------------------------------------------

    /** @brief 返回最近一次错误的描述字符串。 */
    const std::string& lastError() const noexcept { return last_error_; }

private:
    // 工作线程主函数
    void receiveLoop();

    // 内部：打开已分配好的 fmt_ctx_，初始化解码器
    bool initDecoder();

    AVFormatContext*  fmt_ctx_    = nullptr;
    int               video_idx_  = -1;      ///< 视频流索引

    VideoDecoder      decoder_;

    VideoFrameCallback                      frame_cb_;
    std::function<void(const std::string&)> error_cb_;

    std::thread      worker_;
    std::atomic_bool running_{false};
    std::atomic_bool stop_requested_{false};

    std::string last_error_;
};

} // namespace strmctrl
