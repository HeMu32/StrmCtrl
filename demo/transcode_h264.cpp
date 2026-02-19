/**
 * @file transcode_h264.cpp
 * @brief H.264 -> H.264 (libopenh264) 本地转码 demo。
 *
 * 用法：
 *   transcode_h264 <input_video> <output.h264> [--max-frames N] [--bitrate kbps] [--fps N] [--gop N]
 */

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

static std::string avErr(int e)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

struct Options {
    std::string input_path;
    std::string output_path;
    int64_t max_frames = -1;
    int bitrate_kbps = 2000;
    int fps = 0; // 0 means use input fps or fallback to 30
    int gop = 60;
};

static void printUsage()
{
    std::cout
        << "Usage: transcode_h264 <input_video> <output.h264> [--max-frames N] [--bitrate kbps] [--fps N] [--gop N]\n";
}

static bool parseArgs(int argc, char* argv[], Options& opts)
{
    if (argc < 3) return false;
    opts.input_path = argv[1];
    opts.output_path = argv[2];

    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--max-frames") {
            if (i + 1 >= argc) return false;
            opts.max_frames = std::stoll(argv[++i]);
            continue;
        }
        if (arg == "--bitrate") {
            if (i + 1 >= argc) return false;
            opts.bitrate_kbps = std::stoi(argv[++i]);
            continue;
        }
        if (arg == "--fps") {
            if (i + 1 >= argc) return false;
            opts.fps = std::stoi(argv[++i]);
            continue;
        }
        if (arg == "--gop") {
            if (i + 1 >= argc) return false;
            opts.gop = std::stoi(argv[++i]);
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            return false;
        }
        return false;
    }
    return true;
}

static double streamFps(const AVStream* stream)
{
    if (!stream) return 0.0;
    const AVRational r = stream->avg_frame_rate;
    return (r.den > 0) ? static_cast<double>(r.num) / r.den : 0.0;
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

    // ------------------------------------------------------------
    // Open input
    // ------------------------------------------------------------
    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, opts.input_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::cerr << "[transcode] avformat_open_input: " << avErr(ret) << "\n";
        return 1;
    }

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        std::cerr << "[transcode] avformat_find_stream_info: " << avErr(ret) << "\n";
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (ret < 0) {
        std::cerr << "[transcode] No video stream: " << avErr(ret) << "\n";
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    const int video_idx = ret;
    AVStream* video_stream = fmt_ctx->streams[video_idx];

    // ------------------------------------------------------------
    // Open decoder (input codec)
    // ------------------------------------------------------------
    const AVCodec* dec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!dec) {
        std::cerr << "[transcode] Decoder not found\n";
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVCodecContext* dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dec_ctx, video_stream->codecpar);
    ret = avcodec_open2(dec_ctx, dec, nullptr);
    if (ret < 0) {
        std::cerr << "[transcode] avcodec_open2 (decoder): " << avErr(ret) << "\n";
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    const int width = dec_ctx->width;
    const int height = dec_ctx->height;
    const double input_fps = streamFps(video_stream);
    const int fps = (opts.fps > 0) ? opts.fps : (input_fps > 0.0 ? static_cast<int>(input_fps + 0.5) : 30);

    // ------------------------------------------------------------
    // Open encoder (libopenh264)
    // ------------------------------------------------------------
    const AVCodec* enc = avcodec_find_encoder_by_name("libopenh264");
    if (!enc) {
        std::cerr << "[transcode] Encoder not found: libopenh264\n";
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVCodecContext* enc_ctx = avcodec_alloc_context3(enc);
    enc_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    enc_ctx->width = width;
    enc_ctx->height = height;
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx->time_base = AVRational{1, fps};
    enc_ctx->framerate = AVRational{fps, 1};
    enc_ctx->gop_size = opts.gop;
    enc_ctx->max_b_frames = 0;
    enc_ctx->bit_rate = static_cast<int64_t>(opts.bitrate_kbps) * 1000;

    ret = avcodec_open2(enc_ctx, enc, nullptr);
    if (ret < 0) {
        std::cerr << "[transcode] avcodec_open2 (encoder): " << avErr(ret) << "\n";
        avcodec_free_context(&enc_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    std::ofstream ofs(opts.output_path, std::ios::binary);
    if (!ofs) {
        std::cerr << "[transcode] Cannot open output: " << opts.output_path << "\n";
        avcodec_free_context(&enc_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    std::cout << "[transcode] Input: " << opts.input_path << "\n"
              << "[transcode] Output: " << opts.output_path << "\n"
              << "[transcode] Size: " << width << "x" << height
              << " fps=" << fps
              << " bitrate=" << opts.bitrate_kbps << "kbps"
              << " gop=" << opts.gop << "\n";

    // ------------------------------------------------------------
    // Frame conversion setup
    // ------------------------------------------------------------
    SwsContext* sws = nullptr;
    if (dec_ctx->pix_fmt != AV_PIX_FMT_YUV420P) {
        sws = sws_getContext(width, height, dec_ctx->pix_fmt,
                             width, height, AV_PIX_FMT_YUV420P,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) {
            std::cerr << "[transcode] sws_getContext failed\n";
            avcodec_free_context(&enc_ctx);
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&fmt_ctx);
            return 1;
        }
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* yuv = av_frame_alloc();
    yuv->format = AV_PIX_FMT_YUV420P;
    yuv->width = width;
    yuv->height = height;
    ret = av_frame_get_buffer(yuv, 32);
    if (ret < 0) {
        std::cerr << "[transcode] av_frame_get_buffer: " << avErr(ret) << "\n";
        av_packet_free(&pkt);
        av_frame_free(&frame);
        av_frame_free(&yuv);
        if (sws) sws_freeContext(sws);
        avcodec_free_context(&enc_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    auto encodeFrame = [&](AVFrame* in) -> bool {
        int send_ret = avcodec_send_frame(enc_ctx, in);
        if (send_ret < 0 && send_ret != AVERROR_EOF) {
            std::cerr << "[transcode] avcodec_send_frame: " << avErr(send_ret) << "\n";
            return false;
        }
        while (true) {
            AVPacket* out_pkt = av_packet_alloc();
            if (!out_pkt) {
                std::cerr << "[transcode] av_packet_alloc failed\n";
                return false;
            }
            int recv_ret = avcodec_receive_packet(enc_ctx, out_pkt);
            if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
                av_packet_free(&out_pkt);
                break;
            }
            if (recv_ret < 0) {
                std::cerr << "[transcode] avcodec_receive_packet: " << avErr(recv_ret) << "\n";
                av_packet_free(&out_pkt);
                return false;
            }
            ofs.write(reinterpret_cast<const char*>(out_pkt->data), out_pkt->size);
            av_packet_free(&out_pkt);
        }
        return true;
    };

    int64_t frame_count = 0;
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt);
            continue;
        }

        ret = avcodec_send_packet(dec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
            std::cerr << "[transcode] avcodec_send_packet (decoder): " << avErr(ret) << "\n";
            break;
        }

        while (true) {
            ret = avcodec_receive_frame(dec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                std::cerr << "[transcode] avcodec_receive_frame: " << avErr(ret) << "\n";
                break;
            }

            AVFrame* enc_in = frame;
            if (sws) {
                av_frame_make_writable(yuv);
                sws_scale(sws, frame->data, frame->linesize, 0, height, yuv->data, yuv->linesize);
                enc_in = yuv;
            }

            enc_in->pts = frame_count;
            if (!encodeFrame(enc_in)) {
                break;
            }

            frame_count++;
            if (frame_count == 1 || frame_count % 30 == 0) {
                std::cout << "[transcode] encoded frame " << frame_count << "\n";
            }
            if (opts.max_frames >= 0 && frame_count >= opts.max_frames) {
                std::cout << "[transcode] Reached max frames: " << frame_count << "\n";
                goto flush;
            }
        }
    }

flush:
    // Flush decoder
    avcodec_send_packet(dec_ctx, nullptr);
    while (true) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
        if (ret < 0) {
            std::cerr << "[transcode] avcodec_receive_frame (flush): " << avErr(ret) << "\n";
            break;
        }

        AVFrame* enc_in = frame;
        if (sws) {
            av_frame_make_writable(yuv);
            sws_scale(sws, frame->data, frame->linesize, 0, height, yuv->data, yuv->linesize);
            enc_in = yuv;
        }

        enc_in->pts = frame_count++;
        if (!encodeFrame(enc_in)) {
            break;
        }
    }

    // Flush encoder
    encodeFrame(nullptr);

    std::cout << "[transcode] Done. Total frames: " << frame_count << "\n";

    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&yuv);
    if (sws) sws_freeContext(sws);
    avcodec_free_context(&enc_ctx);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    return 0;
 }
