#include "RtpReceiver.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

namespace strmctrl {

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------
static std::string avErrStrRecv(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

RtpReceiver::RtpReceiver() = default;

RtpReceiver::~RtpReceiver()
{
    stop();
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
    }
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

void RtpReceiver::setFrameCallback(VideoFrameCallback cb)
{
    frame_cb_ = std::move(cb);
    decoder_.setFrameCallback(frame_cb_);
}

void RtpReceiver::setErrorCallback(std::function<void(const std::string&)> cb)
{
    error_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool RtpReceiver::openWithSdp(const std::string& sdp)
{
    // FFmpeg 通过 "data URI" 方式或临时 .sdp 文件加载 SDP。
    // 这里将 SDP 写入临时文件，再以 "sdp" 格式打开。
    // Windows 下使用 %TEMP% 目录。
    const char* tmp_dir = std::getenv("TEMP");
    if (!tmp_dir) tmp_dir = ".";
    const std::string sdp_path = std::string(tmp_dir) + "\\strmctrl_recv.sdp";

    {
        // SDP 规范（RFC 4566）要求每行以 \r\n 结尾。
        // WebSocket 传输可能把 \r\n 变成 \n，写文件前统一规范化。
        std::string sdp_normalized;
        sdp_normalized.reserve(sdp.size() + 32);
        for (std::size_t i = 0; i < sdp.size(); ++i) {
            if (sdp[i] == '\n' && (i == 0 || sdp[i - 1] != '\r')) {
                sdp_normalized += '\r';
            }
            sdp_normalized += sdp[i];
        }

        // 以二进制模式写入，防止 Windows 再次转换换行符
        std::ofstream f(sdp_path, std::ios::trunc | std::ios::binary);
        if (!f) {
            last_error_ = "Cannot write temporary SDP file: " + sdp_path;
            return false;
        }
        f << sdp_normalized;
    }

    std::cout << "[RtpReceiver] SDP written to: " << sdp_path << "\n"
              << "--- SDP BEGIN ---\n" << sdp << "\n--- SDP END ---\n";

    return openWithUrl(sdp_path, -1 /* 由 SDP 指定端口，传 -1 表示 URL 模式 */);
}

bool RtpReceiver::openWithUrl(const std::string& host, int port)
{
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }

    // 构造 URL
    std::string url;
    bool is_sdp_file = (port < 0);
    if (is_sdp_file) {
        // 已是完整路径/URL（如 .sdp 文件路径）
        url = host;
    } else {
        url = "rtp://" + host + ":" + std::to_string(port);
    }

    // 打开选项：
    //   protocol_whitelist — sdp 格式必须显式允许所用协议
    //   analyzeduration    — find_stream_info 等待数据的时长（微秒）。
    //                        设为 0 完全跳过探测等待，H.264 解码器
    //                        通过 in-band SPS/PPS 自配置，无需预探测。
    //   probesize          — 探测缓冲区字节数
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", "file,crypto,data,rtp,udp", 0);
    av_dict_set(&opts, "analyzeduration",    "0",       0);   // 不等待，立即返回
    av_dict_set(&opts, "probesize",          "32",      0);   // 最小探测缓冲

    // 对 .sdp 文件必须显式指定 "sdp" 输入格式，
    // 否则 FFmpeg 在 Windows 路径下可能猜格式失败。
    const AVInputFormat* ifmt = is_sdp_file
        ? av_find_input_format("sdp")
        : nullptr;

    // 注意：avformat_open_input 会负责释放 opts（即使失败）
    int ret = avformat_open_input(&fmt_ctx_, url.c_str(),
                                  const_cast<AVInputFormat*>(ifmt), &opts);
    av_dict_free(&opts); // 若 avformat_open_input 未消费完则手动释放

    if (ret < 0) {
        last_error_ = "avformat_open_input(" + url + "): " + avErrStrRecv(ret);
        fmt_ctx_ = nullptr;
        return false;
    }

    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) {
        // 对于实时 RTP 流，find_stream_info 可能因无足够数据而返回负值，
        // 但流已绑定（codec_type 已知），此时继续尝试 initDecoder。
        // 若 codec 参数确实为空，initDecoder 内 openWithParameters 会失败并报错。
        std::cerr << "[RtpReceiver] avformat_find_stream_info warning: "
                  << avErrStrRecv(ret) << " — continuing anyway\n";
    }

    return initDecoder();
}

void RtpReceiver::start()
{
    if (running_.load()) return;
    stop_requested_.store(false);
    running_.store(true);
    worker_ = std::thread(&RtpReceiver::receiveLoop, this);
}

void RtpReceiver::stop()
{
    stop_requested_.store(true);
    if (worker_.joinable()) worker_.join();
    running_.store(false);
}

// ---------------------------------------------------------------------------
// 工作线程
// ---------------------------------------------------------------------------

void RtpReceiver::receiveLoop()
{
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        last_error_ = "receiveLoop: av_packet_alloc failed";
        if (error_cb_) error_cb_(last_error_);
        running_.store(false);
        return;
    }

    // 无数据超时计数：连续 N 次 EAGAIN/EOF 后报错退出
    // 单次 sleep=5ms，10000 次 = 50s 无数据则认为流已断开
    constexpr int kMaxNoDataRetries = 10000;
    int no_data_count = 0;

    while (!stop_requested_.load()) {
        int ret = av_read_frame(fmt_ctx_, pkt);
        if (ret == AVERROR(EAGAIN)) {
            // 暂时没有数据，稍等重试
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++no_data_count;
            if (no_data_count > kMaxNoDataRetries) {
                last_error_ = "receiveLoop: no data received for 10s, giving up";
                if (error_cb_) error_cb_(last_error_);
                break;
            }
            continue;
        }
        if (ret == AVERROR_EOF) {
            // 对实时 RTP 流而言，EOF 通常是 demuxer 探测超时造成的短暂状态，
            // 不代表流真正结束，稍等重试即可。
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ++no_data_count;
            if (no_data_count > kMaxNoDataRetries) {
                last_error_ = "receiveLoop: stream ended (EOF)";
                if (error_cb_) error_cb_(last_error_);
                break;
            }
            continue;
        }
        if (ret < 0) {
            last_error_ = "av_read_frame: " + avErrStrRecv(ret);
            if (error_cb_) error_cb_(last_error_);
            break;
        }

        // 收到真实数据，重置超时计数
        no_data_count = 0;

        if (pkt->stream_index == video_idx_) {
            if (!decoder_.decode(pkt)) {
                last_error_ = decoder_.lastError();
                if (error_cb_) error_cb_(last_error_);
            }
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    running_.store(false);
}

// ---------------------------------------------------------------------------
// 私有辅助
// ---------------------------------------------------------------------------

bool RtpReceiver::initDecoder()
{
    // 找到第一个视频流
    video_idx_ = -1;
    for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx_ = static_cast<int>(i);
            break;
        }
    }

    if (video_idx_ < 0) {
        last_error_ = "No video stream found in RTP input";
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    // 传递 frame callback（可能在 setFrameCallback 之后才调用 open，这里确保同步）
    if (frame_cb_) decoder_.setFrameCallback(frame_cb_);

    if (!decoder_.openWithParameters(fmt_ctx_->streams[video_idx_]->codecpar)) {
        last_error_ = "VideoDecoder::openWithParameters: " + decoder_.lastError();
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    return true;
}

} // namespace strmctrl
