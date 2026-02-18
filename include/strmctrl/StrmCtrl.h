#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace strmctrl {

/**
 * @brief Video codec type used for RTP streaming.
 */
enum class VideoCodec
{
    H264,
    H265
};

/**
 * @brief Encoder backend hint for selecting FFmpeg encoder.
 */
enum class EncoderBackend
{
    Auto,
    OpenH264,
    QSV,
    AMF,
    NVENC
};

/**
 * @brief Video codec configuration for encoding.
 *
 * The caller can preselect a backend (OpenH264/QSV/AMF/NVENC).
 * If the backend is not available, encoder creation will fail.
 */
struct CodecConfig
{
    VideoCodec codec = VideoCodec::H264;
    EncoderBackend backend = EncoderBackend::OpenH264;
    int width = 1280;
    int height = 720;
    int fps = 30;
    int bitrate = 2'000'000;
    std::string profile;
    std::string preset;
};

/**
 * @brief Connection parameters for master/slave endpoints.
 */
struct EndpointConfig
{
    std::string peer_host;
    uint16_t ws_port = 11451;
    uint16_t rtp_port = 11452;
    std::string bind_host = "0.0.0.0";
};

/**
 * @brief File stream source configuration for master.
 */
struct FileStreamConfig
{
    std::string input_path;
    bool loop = false;
};

/**
 * @brief A single video plane buffer.
 */
struct VideoFramePlane
{
    std::vector<uint8_t> data;
    int line_size = 0;
    int width = 0;
    int height = 0;
};

/**
 * @brief Decoded video frame passed to caller callback.
 */
struct VideoFrame
{
    int width = 0;
    int height = 0;
    /**
     * @brief Pixel format value (matches FFmpeg AVPixelFormat enum).
     */
    int pixel_format = 0;
    int64_t pts = 0;
    std::vector<VideoFramePlane> planes;
};

/**
 * @brief Callback set used by both master and slave.
 *
 * Callbacks are invoked from internal worker threads.
 */
struct MessageCallbacks
{
    std::function<void(const std::string&)> on_text;
    std::function<void(const VideoFrame&)> on_frame;
    std::function<void(const std::string&)> on_error;
};

/**
 * @brief Master endpoint. Acts as WebSocket server and RTP sender.
 */
class MasterEndpoint
{
public:
    MasterEndpoint() = default;
    MasterEndpoint(const MasterEndpoint&) = delete;
    MasterEndpoint& operator=(const MasterEndpoint&) = delete;
    MasterEndpoint(MasterEndpoint&&) = delete;
    MasterEndpoint& operator=(MasterEndpoint&&) = delete;
    /**
     * @brief Start the master endpoint (WebSocket server).
     * @param config Connection and port configuration.
     * @param callbacks Callback set for received messages.
     * @return true if server started successfully.
     */
    bool start(const EndpointConfig& config, const MessageCallbacks& callbacks);

    /**
     * @brief Start streaming from a local file, encode, and push via RTP.
     * @param stream File source configuration.
     * @param codec Codec configuration for encoding.
     * @return true if streaming thread started.
     */
    bool start_file_stream(const FileStreamConfig& stream, const CodecConfig& codec);

    /**
     * @brief Send a text message to all connected slaves.
     * @param text Message to broadcast.
     * @return true if message was queued to any client.
     */
    bool send_text(const std::string& text);

    /**
     * @brief Stop streaming and shutdown server.
     */
    void stop();

    /**
     * @brief Destructor stops and releases resources.
     */
    ~MasterEndpoint();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

/**
 * @brief Slave endpoint. Acts as WebSocket client and RTP receiver.
 */
class SlaveEndpoint
{
public:
    SlaveEndpoint() = default;
    SlaveEndpoint(const SlaveEndpoint&) = delete;
    SlaveEndpoint& operator=(const SlaveEndpoint&) = delete;
    SlaveEndpoint(SlaveEndpoint&&) = delete;
    SlaveEndpoint& operator=(SlaveEndpoint&&) = delete;
    /**
     * @brief Start the slave endpoint (WebSocket client).
     * @param config Connection and port configuration.
     * @param callbacks Callback set for received messages.
     * @return true if connection started successfully.
     */
    bool start(const EndpointConfig& config, const MessageCallbacks& callbacks);

    /**
     * @brief Send a text message to master.
     * @param text Message to send.
     * @return true if message was queued.
     */
    bool send_text(const std::string& text);

    /**
     * @brief Stop receiving and disconnect.
     */
    void stop();

    /**
     * @brief Destructor stops and releases resources.
     */
    ~SlaveEndpoint();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace strmctrl
