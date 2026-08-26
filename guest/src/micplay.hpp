// Playing the host's microphone into the guest.
//
// Windows apps can only pick a microphone that exists as a device, so the audio
// is rendered into VB-Audio's virtual cable: what is written to its playback
// side comes out of its recording side, which apps select as "CABLE Output".
//
// Latency here is queue depth, exactly as on the way out - so the queue is kept
// deliberately shallow and old audio is dropped rather than played late. Nobody
// wants to hear their own voice arrive a quarter of a second after they said it.
#pragma once

#include <cstdint>
#include <memory>

namespace sash {

class MicPlayback {
public:
    MicPlayback();
    ~MicPlayback();
    MicPlayback(const MicPlayback&) = delete;
    MicPlayback& operator=(const MicPlayback&) = delete;

    // False if no virtual cable is installed, in which case there is nowhere
    // for a microphone to appear and the feature is simply off.
    bool start();
    void stop();

    void submit(const float* samples, std::uint32_t frames,
                std::uint32_t rate, std::uint16_t channels);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sash
