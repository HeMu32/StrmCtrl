#include "strmctrl/StrmCtrl.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <iostream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace strmctrl {

namespace {

/**
 * @brief RAII wrapper for IXWebSocket network init.
 */
class NetSystemGuard
{
public:
    NetSystemGuard()
    {
        if (ref_count_.fetch_add(1) == 0) {
            ix::initNetSystem();
        }
    }

    ~NetSystemGuard()
    {
        if (ref_count_.fetch_sub(1) == 1) {
            ix::uninitNetSystem();
        }
    }

private:
    static std::atomic<int> ref_count_;
};

std::atomic<int> NetSystemGuard::ref_count_{0};

std::string ffmpeg_error(int err)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buffer, sizeof(buffer));
    return std::string(buffer);
}

bool ffmpeg_has_gpl_or_nonfree()
{
    const char* cfg = avcodec_configuration();
    if (!cfg) return false;
    return std::strstr(cfg, "--enable-gpl") || std::strstr(cfg, "--enable-nonfree");
}

int guess_rtp_payload(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::H264:
        return 96;
    case VideoCodec::H265:
        return 98;
    default:
        return 96;
    }
}

const char* encoder_name(VideoCodec codec, EncoderBackend backend)
{
    if (codec == VideoCodec::H264) {
        switch (backend) {
        case EncoderBackend::OpenH264:
            return "libopenh264";
        case EncoderBackend::QSV:
            return "h264_qsv";
        case EncoderBackend::AMF:
            return "h264_amf";
        case EncoderBackend::NVENC:
            return "h264_nvenc";
        case EncoderBackend::Auto:
        default:
            // Prefer libx264 as the default encoder for H.264 when auto-selected
            // because it is more widely available and robust than libopenh264.
            return "libx264";
        }
    }

    if (codec == VideoCodec::H265) {
        switch (backend) {
        case EncoderBackend::QSV:
            return "hevc_qsv";
        case EncoderBackend::AMF:
            return "hevc_amf";
        case EncoderBackend::NVENC:
            return "hevc_nvenc";
        case EncoderBackend::Auto:
        default:
            return "libx265";
        }
    }

    return "libopenh264";
}

/**
 * @brief Minimal UDP socket wrapper for RTP transport.
 */
class UdpSocket
{
public:
    bool open_sender(const std::string& host, uint16_t port)
    {
#ifdef _WIN32
        if (!net_guard_) net_guard_ = std::make_unique<NetSystemGuard>();
#endif
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ < 0) return false;
        std::memset(&remote_, 0, sizeof(remote_));
        remote_.sin_family = AF_INET;
        remote_.sin_port = htons(port);
        remote_.sin_addr.s_addr = inet_addr(host.c_str());
        return true;
    }

    bool open_receiver(const std::string& bind_host, uint16_t port)
    {
#ifdef _WIN32
        if (!net_guard_) net_guard_ = std::make_unique<NetSystemGuard>();
#endif
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ < 0) return false;

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(bind_host.c_str());
        if (::bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close();
            return false;
        }
        return true;
    }

    int send_to(const uint8_t* data, size_t size)
    {
        if (socket_ < 0) return -1;
        return ::sendto(socket_, reinterpret_cast<const char*>(data),
                        static_cast<int>(size), 0,
                        reinterpret_cast<sockaddr*>(&remote_),
                        sizeof(remote_));
    }

    int recv_from(uint8_t* data, size_t size)
    {
        if (socket_ < 0) return -1;
        sockaddr_in from;
#ifdef _WIN32
        int from_len = sizeof(from);
#else
        socklen_t from_len = sizeof(from);
#endif
        return ::recvfrom(socket_, reinterpret_cast<char*>(data),
                          static_cast<int>(size), 0,
                          reinterpret_cast<sockaddr*>(&from), &from_len);
    }

    void close()
    {
        if (socket_ >= 0) {
#ifdef _WIN32
            ::closesocket(socket_);
#else
            ::close(socket_);
#endif
            socket_ = -1;
        }
    }

    ~UdpSocket() { close(); }

private:
    int socket_ = -1;
    sockaddr_in remote_{};
#ifdef _WIN32
    std::unique_ptr<NetSystemGuard> net_guard_;
#endif
};

/**
 * @brief Parsed RTP packet metadata and payload.
 */
struct RtpPacket
{
    uint16_t seq = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0x22334455;
    uint8_t payload_type = 96;
    bool marker = false;
    std::vector<uint8_t> payload;
};

/**
 * @brief Sends RTP packets over UDP.
 */
class RtpSender
{
public:
    bool open(const std::string& host, uint16_t port, int payload_type)
    {
        payload_type_ = static_cast<uint8_t>(payload_type);
        return socket_.open_sender(host, port);
    }

    bool send_packet(const uint8_t* payload, size_t size, uint32_t timestamp, bool marker)
    {
        const size_t header_size = 12;
        std::vector<uint8_t> packet(header_size + size);

        packet[0] = 0x80;
        packet[1] = static_cast<uint8_t>(payload_type_ | (marker ? 0x80 : 0x00));
        packet[2] = static_cast<uint8_t>((seq_ >> 8) & 0xFF);
        packet[3] = static_cast<uint8_t>(seq_ & 0xFF);
        packet[4] = static_cast<uint8_t>((timestamp >> 24) & 0xFF);
        packet[5] = static_cast<uint8_t>((timestamp >> 16) & 0xFF);
        packet[6] = static_cast<uint8_t>((timestamp >> 8) & 0xFF);
        packet[7] = static_cast<uint8_t>(timestamp & 0xFF);
        packet[8] = static_cast<uint8_t>((ssrc_ >> 24) & 0xFF);
        packet[9] = static_cast<uint8_t>((ssrc_ >> 16) & 0xFF);
        packet[10] = static_cast<uint8_t>((ssrc_ >> 8) & 0xFF);
        packet[11] = static_cast<uint8_t>(ssrc_ & 0xFF);

        std::memcpy(packet.data() + header_size, payload, size);
        ++seq_;
        return socket_.send_to(packet.data(), packet.size()) >= 0;
    }

private:
    UdpSocket socket_;
    uint16_t seq_ = 0;
    uint32_t ssrc_ = 0x22334455;
    uint8_t payload_type_ = 96;
};

/**
 * @brief Receives RTP packets over UDP.
 */
class RtpReceiver
{
public:
    bool open(const std::string& bind_host, uint16_t port)
    {
        return socket_.open_receiver(bind_host, port);
    }

    bool receive_packet(RtpPacket& out)
    {
        uint8_t buffer[1500];
        int ret = socket_.recv_from(buffer, sizeof(buffer));
        if (ret <= 12) return false;

        const uint8_t v = buffer[0] >> 6;
        if (v != 2) return false;

        out.payload_type = buffer[1] & 0x7F;
        out.seq = static_cast<uint16_t>((buffer[2] << 8) | buffer[3]);
        out.timestamp = (buffer[4] << 24) | (buffer[5] << 16) | (buffer[6] << 8) | buffer[7];
        out.ssrc = (buffer[8] << 24) | (buffer[9] << 16) | (buffer[10] << 8) | buffer[11];
        out.marker = (buffer[1] & 0x80) != 0;
        out.payload.assign(buffer + 12, buffer + ret);
        return true;
    }

private:
    UdpSocket socket_;
};

/**
 * @brief Video decoder wrapper.
 */
/**
 * @brief Video decoder wrapper.
 */
class Decoder
{
public:
    bool open(VideoCodec codec)
    {
        const char* name = (codec == VideoCodec::H265) ? "hevc" : "h264";
        const AVCodec* dec = avcodec_find_decoder_by_name(name);
        if (!dec) return false;
        ctx_ = avcodec_alloc_context3(dec);
        if (!ctx_) return false;
        if (avcodec_open2(ctx_, dec, nullptr) < 0) return false;
        return true;
    }

    bool decode(const uint8_t* data, size_t size, std::vector<VideoFrame>& frames)
    {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return false;
        int ret = av_new_packet(pkt, static_cast<int>(size));
        if (ret < 0) {
            av_packet_free(&pkt);
            return false;
        }
        std::memcpy(pkt->data, data, size);
        ret = avcodec_send_packet(ctx_, pkt);
        av_packet_free(&pkt);
        if (ret < 0) return false;

        AVFrame* frame = av_frame_alloc();
        if (!frame) return false;
        while ((ret = avcodec_receive_frame(ctx_, frame)) == 0) {
            frames.push_back(copy_frame(frame));
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
        return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF;
    }

    void close()
    {
        if (ctx_) {
            avcodec_free_context(&ctx_);
        }
    }

    ~Decoder() { close(); }

private:
    VideoFrame copy_frame(const AVFrame* frame)
    {
        VideoFrame out;
        out.width = frame->width;
        out.height = frame->height;
        out.pixel_format = static_cast<int>(frame->format);
        out.pts = frame->pts;
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(out.pixel_format));
        const int planes = desc ? desc->nb_components : 0;
        for (int i = 0; i < planes; ++i) {
            int plane_h = out.height;
            if (desc && desc->log2_chroma_h && i > 0) {
                plane_h = out.height >> desc->log2_chroma_h;
            }
            VideoFramePlane plane;
            int lines = frame->linesize[i];
            plane.width = out.width;
            plane.height = plane_h;
            plane.line_size = lines;
            plane.data.resize(lines * plane_h);
            std::memcpy(plane.data.data(), frame->data[i], lines * plane_h);
            out.planes.push_back(std::move(plane));
        }
        return out;
    }

    AVCodecContext* ctx_ = nullptr;
};

/**
 * @brief Video encoder wrapper.
 */
    class Encoder
    {
public:
    bool open(const CodecConfig& cfg)
    {
        const char* name = encoder_name(cfg.codec, cfg.backend);
        const AVCodec* enc = avcodec_find_encoder_by_name(name);
        if (!enc) return false;
        ctx_ = avcodec_alloc_context3(enc);
        if (!ctx_) return false;

        ctx_->width = cfg.width;
        ctx_->height = cfg.height;
        ctx_->time_base = AVRational{1, cfg.fps};
        ctx_->framerate = AVRational{cfg.fps, 1};
        ctx_->bit_rate = cfg.bitrate;
        // Force All-Intra (every frame is a keyframe) to simplify packetization
        // and avoid B-frame reordering issues during this debugging run.
        ctx_->gop_size = 1;
        ctx_->max_b_frames = 0;
        ctx_->pix_fmt = AV_PIX_FMT_YUV420P;

        if (!cfg.profile.empty()) {
            av_opt_set(ctx_->priv_data, "profile", cfg.profile.c_str(), 0);
        }
        if (!cfg.preset.empty()) {
            av_opt_set(ctx_->priv_data, "preset", cfg.preset.c_str(), 0);
        }

        if (avcodec_open2(ctx_, enc, nullptr) < 0) return false;
        std::cerr << "[encoder] opened '" << name << "' w=" << ctx_->width << " h=" << ctx_->height
                  << " fps=" << cfg.fps << " fmt=" << ctx_->pix_fmt << std::endl;
        return true;
    }

    bool encode(AVFrame* frame, std::vector<AVPacket*>& packets)
    {
        // Detailed logging to help debug encoder behavior.
        if (frame) {
            std::cerr << "[encoder] send_frame pts=" << frame->pts;
            if (ctx_ && ctx_->time_base.num) {
                std::cerr << " enc_timebase=" << ctx_->time_base.num << "/" << ctx_->time_base.den;
            }
            if (ctx_ && ctx_->framerate.num) {
                std::cerr << " framerate=" << ctx_->framerate.num << "/" << ctx_->framerate.den;
            }
            std::cerr << std::endl;
        }

        int ret = avcodec_send_frame(ctx_, frame);
        // Treat EAGAIN from avcodec_send_frame as non-fatal: it means the
        // encoder's internal buffer is full and we should drain packets via
        // avcodec_receive_packet. Previously we returned false for any
        // ret < 0 which incorrectly treated EAGAIN as a fatal error and
        // caused the pipeline to drop frames.
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            last_error_ = "send_frame failed: " + ffmpeg_error(ret) + " (" + std::to_string(ret) + ")";
            std::cerr << "[encoder] " << last_error_ << std::endl;
            return false;
        }
        if (ret == AVERROR(EAGAIN)) {
            std::cerr << "[encoder] send_frame returned EAGAIN -> will drain available packets" << std::endl;
        }

        // Receive all available packets
        while (true) {
            AVPacket* pkt = av_packet_alloc();
            if (!pkt) return false;
            ret = avcodec_receive_packet(ctx_, pkt);
            if (ret == 0) {
                packets.push_back(pkt);
                std::cerr << "[encoder] recv ok pkt size=" << pkt->size << " pts=" << pkt->pts << std::endl;
                continue;
            }
            // not zero -> free and handle
            av_packet_free(&pkt);
            if (ret == AVERROR(EAGAIN)) {
                if (!packets.empty()) {
                    std::cerr << "[encoder] produced " << packets.size();
                    for (size_t i = 0; i < packets.size(); ++i) std::cerr << " size=" << packets[i]->size;
                    std::cerr << std::endl;
                }
                std::cerr << "[encoder] receive_packet returned " << ret << " -> " << ffmpeg_error(ret) << std::endl;
                last_error_.clear();
                return true;
            }
            if (ret == AVERROR_EOF) {
                std::cerr << "[encoder] receive_packet EOF" << std::endl;
                last_error_.clear();
                return true;
            }
            last_error_ = ffmpeg_error(ret);
            std::cerr << "[encoder] receive_packet returned " << ret << " -> " << last_error_ << std::endl;
            return false;
        }
    }

    const std::string& last_error() const { return last_error_; }

    void close()
    {
        if (ctx_) {
            avcodec_free_context(&ctx_);
        }
    }

    ~Encoder() { close(); }

    AVCodecContext* context() const { return ctx_; }

private:
    AVCodecContext* ctx_ = nullptr;
    std::string last_error_;
};

/**
 * @brief Reads a video file, decodes, scales, and encodes frames.
 */
    class FileStreamPipeline
    {
public:
    /**
     * @brief Encoded frame payload group.
     */
    struct EncodedFrame
    {
        int64_t pts = 0;
        std::vector<AVPacket*> packets;
    };

    bool open(const FileStreamConfig& file, const CodecConfig& codec)
    {
        file_ = file;
        codec_ = codec;
        // For this quick test disable bitstream filter (BSF) so we can
        // isolate whether BSF is the source of packet drops. This will
        // be toggled back when BSF behavior is fixed.
        force_packetization_ = false;

        if (ffmpeg_has_gpl_or_nonfree()) {
            // don't fail startup when FFmpeg is built with GPL/nonfree options;
            // only warn the user and continue (caller should still validate
            // behavior if they require a LGPL-only FFmpeg build).
            std::cerr << "[warning] FFmpeg build includes GPL/nonfree options. " << std::endl;
        }

        int ret = avformat_open_input(&fmt_, file.input_path.c_str(), nullptr, nullptr);
        if (ret < 0) {
            last_error_ = "Failed to open input: " + ffmpeg_error(ret);
            return false;
        }
        ret = avformat_find_stream_info(fmt_, nullptr);
        if (ret < 0) {
            last_error_ = "Failed to read stream info: " + ffmpeg_error(ret);
            return false;
        }
        stream_index_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream_index_ < 0) {
            last_error_ = "No video stream found.";
            return false;
        }

        const AVCodec* dec = avcodec_find_decoder(fmt_->streams[stream_index_]->codecpar->codec_id);
        if (!dec) {
            last_error_ = "Failed to find decoder.";
            return false;
        }
        dec_ctx_ = avcodec_alloc_context3(dec);
        if (!dec_ctx_) return false;
        avcodec_parameters_to_context(dec_ctx_, fmt_->streams[stream_index_]->codecpar);
        if (avcodec_open2(dec_ctx_, dec, nullptr) < 0) {
            last_error_ = "Failed to open decoder.";
            return false;
        }

        if (!encoder_.open(codec_)) {
            last_error_ = "Failed to open encoder.";
            return false;
        }

        if (force_packetization_) {
        if (force_packetization_) {
            if (!bitstream_filter_.open(encoder_.context())) {
                last_error_ = "Failed to open H.264 bitstream filter.";
                return false;
            }
        }
        }

        sws_ = sws_getContext(dec_ctx_->width, dec_ctx_->height,
                              dec_ctx_->pix_fmt,
                              codec_.width, codec_.height,
                              AV_PIX_FMT_YUV420P,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) {
            last_error_ = "Failed to initialize scaling.";
            return false;
        }

        return true;
    }

    bool read_and_encode(std::vector<EncodedFrame>& frames)
    {
        static int debug_packet_count = 0;
        static int debug_frame_count = 0;
        AVPacket* packet = av_packet_alloc();
        if (!packet) return false;
        int ret = av_read_frame(fmt_, packet);
        if (ret < 0) {
            av_packet_free(&packet);
            if (file_.loop) {
                av_seek_frame(fmt_, stream_index_, 0, AVSEEK_FLAG_BACKWARD);
                return true;
            }
            eof_ = true;
            return false;
        }
        if (debug_packet_count < 20) {
            std::cerr << "[pipeline] packet stream=" << packet->stream_index
                      << " size=" << packet->size << std::endl;
            ++debug_packet_count;
        }
        if (packet->stream_index != stream_index_) {
            if (debug_packet_count < 20) {
                std::cerr << "[pipeline] skipping non-video packet" << std::endl;
            }
            av_packet_unref(packet);
            av_packet_free(&packet);
            return true;
        }

        ret = avcodec_send_packet(dec_ctx_, packet);
        av_packet_unref(packet);
        av_packet_free(&packet);
        if (ret < 0) {
            std::cerr << "[pipeline] decode send error: " << ffmpeg_error(ret) << std::endl;
            return false;
        }

        AVFrame* frame = av_frame_alloc();
        if (!frame) return false;
        while ((ret = avcodec_receive_frame(dec_ctx_, frame)) == 0) {
            if (debug_frame_count < 5) {
                std::cerr << "[pipeline] decoded frame " << frame->width << "x" << frame->height
                          << " pts=" << frame->pts << std::endl;
                ++debug_frame_count;
            }
            AVFrame* out = alloc_output_frame();
            if (!out) {
                av_frame_unref(frame);
                break;
            }
            sws_scale(sws_, frame->data, frame->linesize, 0, frame->height,
                      out->data, out->linesize);
            // For stability use a strictly monotonic PTS counter for the
            // encoder. This avoids libx264 "non-strictly-monotonic PTS"
            // warnings caused by decoder reordering or missing pts.
            out->pts = next_out_pts_++;
            std::vector<AVPacket*> raw_packets;
            bool enc_ok = encoder_.encode(out, raw_packets);
            std::cerr << "[pipeline] encoder.encode returned " << (enc_ok ? "true" : "false") << std::endl;
            if (!enc_ok) {
                std::cerr << "[pipeline] encode failed (encoder error)" << std::endl;
                av_frame_free(&out);
                av_frame_unref(frame);
                continue;
            }
            std::cerr << "[pipeline] raw_packets_count=" << raw_packets.size();
            for (size_t ri = 0; ri < raw_packets.size(); ++ri) {
                auto* p = raw_packets[ri];
                std::cerr << " pkt[" << ri << "]=" << (p ? p->size : 0) << " pts=" << (p ? p->pts : -1);
            }
            std::cerr << std::endl;
            if (raw_packets.empty()) {
                // Encoder did not produce packets for this frame yet (EAGAIN).
                // Not an error — continue processing following frames.
                std::cerr << "[pipeline] encoder produced 0 packets pts=" << out->pts << std::endl;
            }
            EncodedFrame encoded;
            encoded.pts = out->pts;
            for (auto* pkt : raw_packets) {
                if (force_packetization_) {
                    // Try BSF into a temporary vector. If BSF succeeds but
                    // produces no output (internal buffering), fall back to
                    // sending the original encoded packet to avoid dropping
                    // frames.
                    std::vector<AVPacket*> bsf_out;
                    bool ok = bitstream_filter_.filter(pkt, bsf_out);
                    if (!ok) {
                        std::cerr << "[pipeline] bitstream filter error for pkt size=" << pkt->size << 
                                     ", falling back to original packet" << std::endl;
                        encoded.packets.push_back(pkt);
                        pkt = nullptr;
                    } else {
                        if (bsf_out.empty()) {
                            std::cerr << "[pipeline] bitstream filter produced 0 packets for pkt size=" << pkt->size
                                      << ", falling back to original packet" << std::endl;
                            encoded.packets.push_back(pkt);
                            pkt = nullptr;
                        } else {
                            for (auto* bp : bsf_out) encoded.packets.push_back(bp);
                            // original packet no longer needed
                            av_packet_free(&pkt);
                        }
                    }
                } else {
                    encoded.packets.push_back(pkt);
                    pkt = nullptr;
                }
                if (pkt) av_packet_free(&pkt);
            }
            if (!encoded.packets.empty()) {
                std::cerr << "[pipeline] assembled " << encoded.packets.size() << " encoded packets pts=" << encoded.pts;
                for (size_t i = 0; i < encoded.packets.size(); ++i) std::cerr << " size=" << encoded.packets[i]->size;
                std::cerr << std::endl;
            } else {
                std::cerr << "[pipeline] assembled 0 encoded packets pts=" << encoded.pts << std::endl;
            }
            if (!encoded.packets.empty()) {
                frames.push_back(std::move(encoded));
            }
            av_frame_free(&out);
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
        return true;
    }

    const std::string& last_error() const { return last_error_; }
    bool eof() const { return eof_; }

    void close()
    {
        if (sws_) {
            sws_freeContext(sws_);
            sws_ = nullptr;
        }
        if (dec_ctx_) {
            avcodec_free_context(&dec_ctx_);
        }
        if (fmt_) {
            avformat_close_input(&fmt_);
        }
        encoder_.close();
        bitstream_filter_.close();
    }

    ~FileStreamPipeline() { close(); }

private:
    AVFrame* alloc_output_frame()
    {
        AVFrame* frame = av_frame_alloc();
        if (!frame) return nullptr;
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width = codec_.width;
        frame->height = codec_.height;
        if (av_frame_get_buffer(frame, 32) < 0) {
            av_frame_free(&frame);
            return nullptr;
        }
        return frame;
    }

    FileStreamConfig file_{};
    CodecConfig codec_{};
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* dec_ctx_ = nullptr;
    int stream_index_ = -1;
    SwsContext* sws_ = nullptr;
    Encoder encoder_{};
    class H264BitstreamFilter
    {
    public:
        bool open(AVCodecContext* encoder_ctx)
        {
            const AVBitStreamFilter* filter = av_bsf_get_by_name("h264_mp4toannexb");
            if (!filter) return false;
            if (av_bsf_alloc(filter, &ctx_) < 0) return false;

            AVCodecParameters* par = avcodec_parameters_alloc();
            if (!par) return false;
            avcodec_parameters_from_context(par, encoder_ctx);
            if (avcodec_parameters_copy(ctx_->par_in, par) < 0) {
                avcodec_parameters_free(&par);
                return false;
            }
            avcodec_parameters_free(&par);

            if (av_bsf_init(ctx_) < 0) return false;
            return true;
        }

        bool filter(AVPacket* input, std::vector<AVPacket*>& output)
        {
            if (!ctx_) return false;
            int ret = av_bsf_send_packet(ctx_, input);
            if (ret < 0) return false;
            AVPacket* pkt = av_packet_alloc();
            if (!pkt) return false;
            while ((ret = av_bsf_receive_packet(ctx_, pkt)) == 0) {
                output.push_back(pkt);
                pkt = av_packet_alloc();
                if (!pkt) return false;
            }
            av_packet_free(&pkt);
            return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF;
        }

        void close()
        {
            if (ctx_) {
                av_bsf_free(&ctx_);
            }
        }

        ~H264BitstreamFilter() { close(); }

    private:
        AVBSFContext* ctx_ = nullptr;
    } bitstream_filter_{};
    std::string last_error_;
    bool eof_ = false;
    bool force_packetization_ = true;
    // Monotonic output PTS counter used when source frames have no PTS.
    int64_t next_out_pts_ = 0;
};

class H264Packetizer
{
public:
    explicit H264Packetizer(size_t max_payload)
        : max_payload_(max_payload)
    {
    }

    void packetize(const uint8_t* data, size_t size,
                   std::vector<std::vector<uint8_t>>& packets)
    {
        std::vector<std::pair<const uint8_t*, size_t>> nals = split_annexb(data, size);
        for (const auto& nal : nals) {
            if (nal.second <= max_payload_) {
                packets.emplace_back(nal.first, nal.first + nal.second);
            } else {
                packetize_fu_a(nal.first, nal.second, packets);
            }
        }
    }

private:
    std::vector<std::pair<const uint8_t*, size_t>> split_annexb(const uint8_t* data, size_t size)
    {
        std::vector<std::pair<const uint8_t*, size_t>> out;
        size_t i = 0;
        auto is_start = [&](size_t pos) {
            if (pos + 3 >= size) return false;
            return (data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) ||
                   (pos + 4 < size && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 0 && data[pos + 3] == 1);
        };
        while (i + 3 < size) {
            while (i + 3 < size && !is_start(i)) ++i;
            if (i + 3 >= size) break;
            size_t start = (data[i + 2] == 1) ? i + 3 : i + 4;
            size_t next = start;
            while (next + 3 < size && !is_start(next)) ++next;
            size_t nal_size = next - start;
            if (nal_size > 0) out.emplace_back(data + start, nal_size);
            i = next;
        }
        return out;
    }

    void packetize_fu_a(const uint8_t* nal, size_t size,
                        std::vector<std::vector<uint8_t>>& packets)
    {
        const uint8_t nal_header = nal[0];
        const uint8_t fu_indicator = (nal_header & 0xE0) | 28;
        size_t offset = 1;
        while (offset < size) {
            size_t chunk = (std::min)(max_payload_ - 2, size - offset);
            uint8_t fu_header = nal_header & 0x1F;
            if (offset == 1) fu_header |= 0x80;
            if (offset + chunk >= size) fu_header |= 0x40;
            std::vector<uint8_t> payload;
            payload.reserve(chunk + 2);
            payload.push_back(fu_indicator);
            payload.push_back(fu_header);
            payload.insert(payload.end(), nal + offset, nal + offset + chunk);
            packets.push_back(std::move(payload));
            offset += chunk;
        }
    }

    size_t max_payload_ = 1200;
};

class H264Depacketizer
{
public:
    bool ingest(const uint8_t* payload, size_t size, bool marker,
                std::vector<std::vector<uint8_t>>& access_units)
    {
        if (size < 1) return false;
        const uint8_t nal_type = payload[0] & 0x1F;
        if (nal_type >= 1 && nal_type <= 23) {
            append_start_code(current_);
            current_.insert(current_.end(), payload, payload + size);
            if (marker) flush(access_units);
            return true;
        }
        if (nal_type == 28 && size >= 2) {
            const uint8_t fu_header = payload[1];
            const bool start = (fu_header & 0x80) != 0;
            const bool end = (fu_header & 0x40) != 0;
            const uint8_t reconstructed = (payload[0] & 0xE0) | (fu_header & 0x1F);
            if (start) {
                append_start_code(current_);
                current_.push_back(reconstructed);
            }
            current_.insert(current_.end(), payload + 2, payload + size);
            if (end && marker) flush(access_units);
            return true;
        }
        return false;
    }

private:
    void append_start_code(std::vector<uint8_t>& out)
    {
        const uint8_t code[4] = {0x00, 0x00, 0x00, 0x01};
        out.insert(out.end(), code, code + 4);
    }

    void flush(std::vector<std::vector<uint8_t>>& access_units)
    {
        if (!current_.empty()) {
            access_units.push_back(current_);
            current_.clear();
        }
    }

    std::vector<uint8_t> current_;
};

/**
 * @brief WebSocket signaling server wrapper.
 */
class SignalingServer
{
public:
    bool start(uint16_t port, const MessageCallbacks& callbacks)
    {
#ifdef _WIN32
        if (!net_guard_) net_guard_ = std::make_unique<NetSystemGuard>();
#endif
        callbacks_ = callbacks;
        server_ = std::make_unique<ix::WebSocketServer>(port, "0.0.0.0");
        server_->setOnConnectionCallback(
            [this](std::weak_ptr<ix::WebSocket> weakSocket,
                   std::shared_ptr<ix::ConnectionState>) {
                if (auto socket = weakSocket.lock()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    clients_.push_back(socket);
                    socket->setOnMessageCallback(
                        [this](const ix::WebSocketMessagePtr& msg) {
                            handle_message(msg);
                        });
                }
            });
        auto res = server_->listen();
        if (!res.first) {
            report_error("WebSocket listen failed: " + res.second);
            return false;
        }
        server_->start();
        return true;
    }

    bool send_text(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool any = false;
        for (auto& client : clients_) {
            if (client) {
                client->send(text);
                any = true;
            }
        }
        return any;
    }

    void stop()
    {
        if (server_) {
            server_->stop();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.clear();
    }

private:
    void handle_message(const ix::WebSocketMessagePtr& msg)
    {
        if (msg->type == ix::WebSocketMessageType::Message) {
            if (callbacks_.on_text) callbacks_.on_text(msg->str);
        } else if (msg->type == ix::WebSocketMessageType::Error) {
            report_error(msg->errorInfo.reason);
        }
    }

    void report_error(const std::string& err)
    {
        if (callbacks_.on_error) callbacks_.on_error(err);
    }

    MessageCallbacks callbacks_{};
    std::unique_ptr<ix::WebSocketServer> server_;
    std::vector<std::shared_ptr<ix::WebSocket>> clients_;
    std::mutex mutex_;
#ifdef _WIN32
    std::unique_ptr<NetSystemGuard> net_guard_;
#endif
};

/**
 * @brief WebSocket signaling client wrapper.
 */
class SignalingClient
{
public:
    bool start(const std::string& url, const MessageCallbacks& callbacks)
    {
#ifdef _WIN32
        if (!net_guard_) net_guard_ = std::make_unique<NetSystemGuard>();
#endif
        callbacks_ = callbacks;
        socket_ = std::make_unique<ix::WebSocket>();
        socket_->setUrl(url);
        socket_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            handle_message(msg);
        });
        socket_->start();
        return true;
    }

    bool send_text(const std::string& text)
    {
        if (!socket_) return false;
        socket_->send(text);
        return true;
    }

    void stop()
    {
        if (socket_) socket_->stop();
    }

private:
    void handle_message(const ix::WebSocketMessagePtr& msg)
    {
        if (msg->type == ix::WebSocketMessageType::Message) {
            if (callbacks_.on_text) callbacks_.on_text(msg->str);
        } else if (msg->type == ix::WebSocketMessageType::Error) {
            if (callbacks_.on_error) callbacks_.on_error(msg->errorInfo.reason);
        }
    }

    MessageCallbacks callbacks_{};
    std::unique_ptr<ix::WebSocket> socket_;
#ifdef _WIN32
    std::unique_ptr<NetSystemGuard> net_guard_;
#endif
};

} // namespace

struct MasterEndpoint::Impl
{
    EndpointConfig config{};
    MessageCallbacks callbacks{};
    SignalingServer signaling;
    RtpSender rtp_sender;
    bool rtp_opened = false;
    std::thread stream_thread;
    std::atomic<bool> running{false};
    std::unique_ptr<FileStreamPipeline> pipeline;
    std::unique_ptr<H264Packetizer> packetizer;

    bool start(const EndpointConfig& cfg, const MessageCallbacks& cb)
    {
        config = cfg;
        callbacks = cb;
        if (!signaling.start(cfg.ws_port, cb)) return false;
        return true;
    }

    bool start_file_stream(const FileStreamConfig& file, const CodecConfig& codec)
    {
        if (running.load()) return false;
        if (!rtp_opened) {
            if (!rtp_sender.open(config.peer_host, config.rtp_port,
                                  guess_rtp_payload(codec.codec))) {
                report_error("Failed to open RTP sender.");
                return false;
            }
            rtp_opened = true;
        }
        pipeline = std::make_unique<FileStreamPipeline>();
        if (!pipeline->open(file, codec)) {
            report_error(pipeline->last_error());
            pipeline.reset();
            return false;
        }
        packetizer = std::make_unique<H264Packetizer>(1200);

        running.store(true);
        stream_thread = std::thread([this, codec]() {
            uint32_t timestamp = 0;
            const uint32_t ts_step = 90000 / (std::max)(1, codec.fps);
            while (running.load()) {
                std::vector<FileStreamPipeline::EncodedFrame> frames;
                if (!pipeline->read_and_encode(frames)) {
                    if (pipeline->eof()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                std::cout << "[master] Encoded " << frames.size() << " frames" << std::endl;
                for (auto& frame : frames) {
                    for (auto* pkt : frame.packets) {
                        std::vector<std::vector<uint8_t>> rtp_payloads;
                        packetizer->packetize(pkt->data, pkt->size, rtp_payloads);
                        for (size_t i = 0; i < rtp_payloads.size(); ++i) {
                            bool marker = (i + 1 == rtp_payloads.size());
                            rtp_sender.send_packet(rtp_payloads[i].data(), rtp_payloads[i].size(),
                                                   timestamp, marker);
                        }
                        std::cout << "[master] Sent RTP packet (size=" << pkt->size << ")" << std::endl;
                        av_packet_free(&pkt);
                    }
                    timestamp += ts_step;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        return true;
    }

    bool send_text(const std::string& text) { return signaling.send_text(text); }

    void stop()
    {
        running.store(false);
        if (stream_thread.joinable()) stream_thread.join();
        signaling.stop();
        pipeline.reset();
        packetizer.reset();
    }

    void report_error(const std::string& err)
    {
        if (callbacks.on_error) callbacks.on_error(err);
    }
};

struct SlaveEndpoint::Impl
{
    EndpointConfig config{};
    MessageCallbacks callbacks{};
    SignalingClient signaling;
    RtpReceiver rtp_receiver;
    Decoder decoder;
    std::thread receive_thread;
    std::atomic<bool> running{false};
    H264Depacketizer depacketizer;

    bool start(const EndpointConfig& cfg, const MessageCallbacks& cb)
    {
        config = cfg;
        callbacks = cb;
        std::string url = "ws://" + cfg.peer_host + ":" + std::to_string(cfg.ws_port);
        if (!signaling.start(url, cb)) return false;
        if (!rtp_receiver.open(cfg.bind_host, cfg.rtp_port)) {
            report_error("Failed to open RTP receiver.");
            return false;
        }
        if (!decoder.open(VideoCodec::H264)) {
            report_error("Failed to open decoder.");
            return false;
        }
        running.store(true);
        receive_thread = std::thread([this]() { receive_loop(); });
        return true;
    }

    bool send_text(const std::string& text) { return signaling.send_text(text); }

    void stop()
    {
        running.store(false);
        if (receive_thread.joinable()) receive_thread.join();
        signaling.stop();
        decoder.close();
    }

    void receive_loop()
    {
        while (running.load()) {
            RtpPacket packet;
            if (!rtp_receiver.receive_packet(packet)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            std::vector<std::vector<uint8_t>> access_units;
            depacketizer.ingest(packet.payload.data(), packet.payload.size(),
                                packet.marker, access_units);
            for (const auto& au : access_units) {
                std::vector<VideoFrame> frames;
                if (decoder.decode(au.data(), au.size(), frames)) {
                    if (callbacks.on_frame) {
                        for (auto& frame : frames) callbacks.on_frame(frame);
                    }
                }
            }
        }
    }

    void report_error(const std::string& err)
    {
        if (callbacks.on_error) callbacks.on_error(err);
    }
};

bool MasterEndpoint::start(const EndpointConfig& config, const MessageCallbacks& callbacks)
{
    if (!impl_) impl_ = new Impl();
    return impl_->start(config, callbacks);
}

bool MasterEndpoint::start_file_stream(const FileStreamConfig& stream, const CodecConfig& codec)
{
    if (!impl_) return false;
    return impl_->start_file_stream(stream, codec);
}

bool MasterEndpoint::send_text(const std::string& text)
{
    if (!impl_) return false;
    return impl_->send_text(text);
}

void MasterEndpoint::stop()
{
    if (!impl_) return;
    impl_->stop();
}

MasterEndpoint::~MasterEndpoint()
{
    if (impl_) {
        impl_->stop();
        delete impl_;
        impl_ = nullptr;
    }
}

bool SlaveEndpoint::start(const EndpointConfig& config, const MessageCallbacks& callbacks)
{
    if (!impl_) impl_ = new Impl();
    return impl_->start(config, callbacks);
}

bool SlaveEndpoint::send_text(const std::string& text)
{
    if (!impl_) return false;
    return impl_->send_text(text);
}

void SlaveEndpoint::stop()
{
    if (!impl_) return;
    impl_->stop();
}

SlaveEndpoint::~SlaveEndpoint()
{
    if (impl_) {
        impl_->stop();
        delete impl_;
        impl_ = nullptr;
    }
}

} // namespace strmctrl
