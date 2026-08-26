// Capturing what the guest is playing.
//
// Loopback on the default render endpoint, which is the whole guest's output
// rather than one process's. WASAPI can capture a single process tree
// (AUDCLNT_ACTIVATION_TYPE_PROCESS_LOOPBACK, Windows 10 20H1+), which would
// suit a per-window model better - but this VM exists to run one app at a time,
// and endpoint loopback is a great deal simpler and harder to get wrong. The
// interface below does not expose the difference, so it can be swapped later.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace sash {

class AudioCapture {
public:
    // Called from the capture thread with interleaved float samples.
    using Sink = std::function<void(const float* samples, std::uint32_t frames,
                                    std::uint32_t rate, std::uint16_t channels)>;

    AudioCapture();
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    bool start(Sink sink);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sash
