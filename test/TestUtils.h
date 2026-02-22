#pragma once

#include <string>
#include <atomic>
#include "strmctrl/core/VideoFrame.h"
#include "strmctrl/core/AudioFrame.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace strmctrl {
namespace test {

class PortAllocator {
public:
    static int allocate();
private:
    static std::atomic<int> current_port_;
};

VideoFrame createDummyVideoFrame(int width, int height, AVPixelFormat format);
AudioFrame createDummyAudioFrame(int sample_rate, int channels);

} // namespace test
} // namespace strmctrl
