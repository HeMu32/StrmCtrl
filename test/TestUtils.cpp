#include "TestUtils.h"
#include <chrono>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/version.h>
}

namespace strmctrl {
namespace test {

std::atomic<int> PortAllocator::current_port_{15000};

int PortAllocator::allocate() {
    // Return a port and increment by 10 to leave space for RTP/RTCP
    return current_port_.fetch_add(10);
}

VideoFrame createDummyVideoFrame(int width, int height, AVPixelFormat format) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return VideoFrame{};

    frame->format = format;
    frame->width = width;
    frame->height = height;

    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return VideoFrame{};
    }

    // Fill with green color (Y=150, U=43, V=21) for YUV420P
    if (format == AV_PIX_FMT_YUV420P) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                frame->data[0][y * frame->linesize[0] + x] = 150;
            }
        }
        for (int y = 0; y < height / 2; y++) {
            for (int x = 0; x < width / 2; x++) {
                frame->data[1][y * frame->linesize[1] + x] = 43;
                frame->data[2][y * frame->linesize[2] + x] = 21;
            }
        }
    }

    frame->pts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    return VideoFrame{frame};
}

AudioFrame createDummyAudioFrame(int sample_rate, int channels) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return AudioFrame{};

    frame->format = AV_SAMPLE_FMT_FLTP;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    av_channel_layout_default(&frame->ch_layout, channels);
#else
    frame->channel_layout = av_get_default_channel_layout(channels);
    frame->channels = channels;
#endif
    frame->sample_rate = sample_rate;
    frame->nb_samples = 1024;

    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        return AudioFrame{};
    }

    // Fill with silence
    av_samples_set_silence(frame->extended_data, 0, frame->nb_samples, channels, AV_SAMPLE_FMT_FLTP);
    
    frame->pts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    return AudioFrame{frame};
}

} // namespace test
} // namespace strmctrl