/**
 * @file decode_test.cpp
 * @brief 纯本地解码测试：仅验证 demux + decode 的稳定性。
 *
 * 用法：
 *   decode_test <video_path> [--loop] [--max-frames N]
 */

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

static std::string avErr(int e)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

static bool dumpFrameToFile(const AVFrame* frame, const std::string& path)
{
    const int buffer_size = av_image_get_buffer_size(
        static_cast<AVPixelFormat>(frame->format),
        frame->width,
        frame->height,
        1);
    if (buffer_size <= 0) {
        return false;
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(buffer_size));
    const int copied = av_image_copy_to_buffer(
        buffer.data(),
        buffer_size,
        frame->data,
        frame->linesize,
        static_cast<AVPixelFormat>(frame->format),
        frame->width,
        frame->height,
        1);
    if (copied < 0) {
        return false;
    }

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    ofs.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    return static_cast<bool>(ofs);
}

struct Options {
    std::string path;
    bool loop = false;
    int64_t max_frames = -1;
    bool strict = false;
    int64_t check_every = 1;
    std::string decoder_name; // force a specific libavcodec implementation (e.g. "libopenh264")
};

static void printUsage()
{
    std::cout
    << "Usage: decode_test <video_path> [--loop] [--max-frames N] [--strict] [--check-every N]\n";
}

static bool parseArgs(int argc, char* argv[], Options& opts)
{
    if (argc < 2) return false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--loop") {
            opts.loop = true;
            continue;
        }
        if (arg == "--decoder") {
            if (i + 1 >= argc) return false;
            opts.decoder_name = argv[++i];
            continue;
        }
        if (arg == "--strict") {
            opts.strict = true;
            continue;
        }
        if (arg == "--max-frames") {
            if (i + 1 >= argc) return false;
            opts.max_frames = std::stoll(argv[++i]);
            continue;
        }
        if (arg == "--check-every") {
            if (i + 1 >= argc) return false;
            opts.check_every = std::stoll(argv[++i]);
            if (opts.check_every < 1) return false;
            continue;
        }
        if (opts.path.empty()) {
            opts.path = arg;
            continue;
        }
        return false;
    }
    return !opts.path.empty();
}

class SimpleDecoder {
public:
    ~SimpleDecoder() { close(); }

    bool open(const std::string& path, const std::string& forced_decoder = "")
    {
        forced_decoder_name_ = forced_decoder;
        path_ = path;
        close();

        int ret = avformat_open_input(&fmt_ctx_, path.c_str(), nullptr, nullptr);
        if (ret < 0) {
            last_error_ = "avformat_open_input: " + avErr(ret);
            return false;
        }

        ret = avformat_find_stream_info(fmt_ctx_, nullptr);
        if (ret < 0) {
            last_error_ = "avformat_find_stream_info: " + avErr(ret);
            return false;
        }

        if (av_seek_frame(fmt_ctx_, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
            std::cerr << "[decode_test] Warning: seek-to-start failed.\n";
        }

        ret = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (ret < 0) {
            last_error_ = "No video stream: " + avErr(ret);
            return false;
        }
        video_idx_ = ret;

        const AVCodec* codec = nullptr;
        if (!forced_decoder_name_.empty()) {
            codec = avcodec_find_decoder_by_name(forced_decoder_name_.c_str());
            if (!codec) {
                last_error_ = "Requested decoder not found: " + forced_decoder_name_;
                return false;
            }
        } else {
            codec = avcodec_find_decoder(fmt_ctx_->streams[video_idx_]->codecpar->codec_id);
            if (!codec) {
                last_error_ = "Decoder not found";
                return false;
            }
        }

        dec_ctx_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(dec_ctx_, fmt_ctx_->streams[video_idx_]->codecpar);

        ret = avcodec_open2(dec_ctx_, codec, nullptr);
        if (ret < 0) {
            last_error_ = "avcodec_open2: " + avErr(ret);
            return false;
        }

        if (!pkt_) pkt_ = av_packet_alloc();
        if (!frame_) frame_ = av_frame_alloc();

        std::cout << "[decode_test] Opened: " << path << " (video stream "
                  << video_idx_ << ")";
        if (!forced_decoder_name_.empty()) std::cout << " using decoder=" << forced_decoder_name_;
        std::cout << "\n";
        return true;
    }

    void close()
    {
        if (dec_ctx_) avcodec_free_context(&dec_ctx_);
        if (fmt_ctx_) avformat_close_input(&fmt_ctx_);
        if (pkt_) av_packet_free(&pkt_);
        if (frame_) av_frame_free(&frame_);
        video_idx_ = -1;
        eof_ = false;
        last_error_.clear();
    }

    bool reopen()
    {
        std::cout << "[decode_test] Reopening...\n";
        return open(path_, forced_decoder_name_);
    }

    const AVFrame* readFrame()
    {
        last_error_.clear();
        eof_ = false;

        while (true) {
            int ret = avcodec_receive_frame(dec_ctx_, frame_);
            if (ret == 0) {
                return frame_;
            }
            if (ret == AVERROR_EOF) {
                eof_ = true;
                return nullptr;
            }
            if (ret != AVERROR(EAGAIN)) {
                last_error_ = "avcodec_receive_frame: " + avErr(ret);
                return nullptr;
            }

            ret = av_read_frame(fmt_ctx_, pkt_);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    avcodec_send_packet(dec_ctx_, nullptr);
                    continue;
                }
                last_error_ = "av_read_frame: " + avErr(ret);
                return nullptr;
            }

            if (pkt_->stream_index != video_idx_) {
                av_packet_unref(pkt_);
                continue;
            }

            ret = avcodec_send_packet(dec_ctx_, pkt_);
            av_packet_unref(pkt_);
            if (ret == AVERROR(EAGAIN)) {
                continue;
            }
            if (ret < 0 && ret != AVERROR_EOF) {
                last_error_ = "avcodec_send_packet: " + avErr(ret);
                return nullptr;
            }
        }
    }

    const std::string& lastError() const { return last_error_; }
    bool eof() const { return eof_; }

    double fps() const
    {
        if (!fmt_ctx_ || video_idx_ < 0) return 0.0;
        const AVRational r = fmt_ctx_->streams[video_idx_]->avg_frame_rate;
        return (r.den > 0) ? static_cast<double>(r.num) / r.den : 0.0;
    }

private:
    std::string path_;
    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* dec_ctx_ = nullptr;
    int video_idx_ = -1;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    bool eof_ = false;
    std::string last_error_;
    std::string forced_decoder_name_;
};

struct ValidationStats {
    int64_t total_frames = 0;
    int64_t bad_frames = 0;
    int64_t pts_non_monotonic = 0;
    int64_t invalid_layout = 0;
    int64_t unknown_pix_fmt = 0;
    int64_t zero_size = 0;
    int64_t missing_planes = 0;
    int64_t last_pts = AV_NOPTS_VALUE;
};

static bool validateFrame(const AVFrame* frame, ValidationStats& stats, std::string& reason)
{
    bool ok = true;
    reason.clear();

    if (frame->width <= 0 || frame->height <= 0) {
        stats.zero_size++;
        ok = false;
        reason += "size<=0;";
    }

    if (frame->format == AV_PIX_FMT_NONE) {
        stats.unknown_pix_fmt++;
        ok = false;
        reason += "pix_fmt=none;";
    }

    if (frame->format != AV_PIX_FMT_NONE) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
        if (!desc) {
            stats.unknown_pix_fmt++;
            ok = false;
            reason += "pix_fmt=unknown;";
        } else if (desc->nb_components > 0) {
            for (int i = 0; i < FFMIN(4, desc->nb_components); ++i) {
                if (!frame->data[i] || frame->linesize[i] <= 0) {
                    stats.missing_planes++;
                    ok = false;
                    reason += "missing_plane;";
                    break;
                }
            }
        }
    }

    if (stats.last_pts != AV_NOPTS_VALUE && frame->pts != AV_NOPTS_VALUE) {
        if (frame->pts < stats.last_pts) {
            stats.pts_non_monotonic++;
            ok = false;
            reason += "pts_non_monotonic;";
        }
    }
    if (frame->pts != AV_NOPTS_VALUE) {
        stats.last_pts = frame->pts;
    }

    if (!ok) {
        stats.bad_frames++;
    }
    return ok;
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        printUsage();
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
    }

    Options opts;
    if (!parseArgs(argc, argv, opts)) {
        printUsage();
        return 1;
    }

    SimpleDecoder decoder;
    if (!decoder.open(opts.path, opts.decoder_name)) {
        std::cerr << "[decode_test] " << decoder.lastError() << "\n";
        return 1;
    }

    std::cout << "[decode_test] fps: " << decoder.fps() << "\n";

    int64_t frame_count = 0;
    ValidationStats stats;
    while (true) {
        if (opts.max_frames >= 0 && frame_count >= opts.max_frames) {
            std::cout << "[decode_test] Reached max frames: " << frame_count << "\n";
            break;
        }

        const AVFrame* frame = decoder.readFrame();
        if (!frame) {
            if (!decoder.lastError().empty()) {
                std::cerr << "[decode_test] " << decoder.lastError() << "\n";
                break;
            }

            if (decoder.eof()) {
                std::cout << "[decode_test] EOF reached. Frames decoded: "
                          << frame_count << "\n";
                if (opts.loop) {
                    if (!decoder.reopen()) {
                        std::cerr << "[decode_test] " << decoder.lastError() << "\n";
                        break;
                    }
                    frame_count = 0;
                    continue;
                }
            }
            break;
        }

        ++frame_count;
        stats.total_frames++;

        if (opts.check_every > 0 && (frame_count % opts.check_every == 0)) {
            std::string reason;
            if (!validateFrame(frame, stats, reason)) {
                std::cerr << "[decode_test] validation failed at frame=" << frame_count
                          << " reason=" << reason << "\n";
                if (opts.strict) {
                    break;
                }
            }
        }
        if (frame_count == 1 || frame_count % 30 == 0) {
            const char* fmt_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
            std::cout << "[decode_test] frame=" << frame_count
                      << " pts=" << frame->pts
                      << " size=" << frame->width << "x" << frame->height
                      << " pix_fmt=" << (fmt_name ? fmt_name : "unknown")
                      << "\n";
        }

        if (frame_count % 30 == 0) {
            const char* fmt_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
            std::string safe_fmt = fmt_name ? fmt_name : "unknown";
            std::string out_path = "R:\\frame_" + std::to_string(frame_count)
                                   + "_" + std::to_string(frame->width)
                                   + "x" + std::to_string(frame->height)
                                   + "_" + safe_fmt + ".yuv";
            if (!dumpFrameToFile(frame, out_path)) {
                std::cerr << "[decode_test] Failed to dump frame to " << out_path << "\n";
            } else {
                std::cout << "[decode_test] Dumped frame to " << out_path << "\n";
            }
        }
    }

    std::cout << "[decode_test] summary: total=" << stats.total_frames
              << " bad=" << stats.bad_frames
              << " pts_non_monotonic=" << stats.pts_non_monotonic
              << " zero_size=" << stats.zero_size
              << " missing_planes=" << stats.missing_planes
              << " unknown_pix_fmt=" << stats.unknown_pix_fmt
              << "\n";

    return 0;
}
