#include "audio.hpp"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace sash {

struct AudioCapture::Impl {
    std::thread       thread;
    std::atomic<bool> stop{false};
    void run(Sink sink);
};

/*
 * A loopback client hands back whatever the endpoint's mix format is, which is
 * usually 32-bit float but is not promised to be. Converting here keeps the
 * wire format to one thing and the host side free of format handling.
 */
static void to_float(const BYTE* src, const WAVEFORMATEX* wfx,
                     std::uint32_t frames, std::vector<float>& out) {
    const std::uint32_t ch = wfx->nChannels;
    out.resize(static_cast<std::size_t>(frames) * ch);

    if (wfx->wBitsPerSample == 32) {
        std::memcpy(out.data(), src, out.size() * sizeof(float));
    } else if (wfx->wBitsPerSample == 16) {
        const auto* p = reinterpret_cast<const std::int16_t*>(src);
        for (std::size_t i = 0; i < out.size(); i++) out[i] = p[i] / 32768.0f;
    } else {
        out.clear();
    }
}

/*
 * Fold multichannel down to stereo before it goes anywhere.
 *
 * This endpoint is 7.1, so the stream is 48 kHz by 8 channels of float - about
 * 1.5 MB/s, four times what stereo needs, down the same control channel as
 * input. Audio that the link cannot keep up with backs up in socket buffers and
 * arrives seconds late, which is exactly the symptom.
 *
 * Centre and the surrounds are attenuated into both sides rather than dropped:
 * dialogue lives in the centre channel and would otherwise go missing. LFE is
 * left out, being neither reproducible on most desktop outputs nor worth the
 * headroom.
 */
static void downmix_stereo(const std::vector<float>& in, std::uint32_t frames,
                           std::uint32_t ch, std::vector<float>& out) {
    out.resize(static_cast<std::size_t>(frames) * 2);

    for (std::uint32_t f = 0; f < frames; f++) {
        const float* src = &in[static_cast<std::size_t>(f) * ch];
        float l = src[0], r = src[1];

        if (ch >= 3) { const float c = src[2] * 0.707f; l += c; r += c; }
        if (ch >= 6) { l += src[4] * 0.5f; r += src[5] * 0.5f; }
        if (ch >= 8) { l += src[6] * 0.5f; r += src[7] * 0.5f; }

        if (l >  1.0f) l =  1.0f;
        if (l < -1.0f) l = -1.0f;
        if (r >  1.0f) r =  1.0f;
        if (r < -1.0f) r = -1.0f;

        out[static_cast<std::size_t>(f) * 2 + 0] = l;
        out[static_cast<std::size_t>(f) * 2 + 1] = r;
    }
}

void AudioCapture::Impl::run(Sink sink) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;
    IAudioClient*        client     = nullptr;
    IAudioCaptureClient* capture    = nullptr;
    WAVEFORMATEX*        wfx        = nullptr;

    auto cleanup = [&] {
        if (capture)    capture->Release();
        if (client)     client->Release();
        if (device)     device->Release();
        if (enumerator) enumerator->Release();
        if (wfx)        CoTaskMemFree(wfx);
        CoUninitialize();
    };

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enumerator))) {
        std::fprintf(stderr, "sash: no audio device enumerator\n");
        cleanup();
        return;
    }
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        std::fprintf(stderr, "sash: no default playback device; audio disabled\n");
        cleanup();
        return;
    }
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) ||
        FAILED(client->GetMixFormat(&wfx))) {
        cleanup();
        return;
    }

    // 200ms of slack. Loopback is read on a timer rather than an event, so the
    // buffer only has to outlast a scheduling hiccup.
    const REFERENCE_TIME dur = 2000000;
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  dur, 0, wfx, nullptr)) ||
        FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture)) ||
        FAILED(client->Start())) {
        std::fprintf(stderr, "sash: could not start loopback capture\n");
        cleanup();
        return;
    }

    std::fprintf(stderr, "sash: audio %lu Hz, %u channels, %u-bit\n",
                 wfx->nSamplesPerSec, wfx->nChannels, wfx->wBitsPerSample);

    std::vector<float> samples, stereo;
    while (!stop.load()) {
        UINT32 packet = 0;
        if (FAILED(capture->GetNextPacketSize(&packet))) break;
        if (packet == 0) { Sleep(5); continue; }

        while (packet > 0 && !stop.load()) {
            BYTE*  data  = nullptr;
            UINT32 frames = 0;
            DWORD  flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

            if (frames > 0) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    // Silence is reported without data; send it anyway so the
                    // host's stream keeps its timing rather than running dry.
                    samples.assign(static_cast<std::size_t>(frames) * wfx->nChannels, 0.0f);
                } else {
                    to_float(data, wfx, frames, samples);
                }
                // Distinguish "nothing is playing" from "capturing but not
                // delivering" - they look identical from the far end.
                {
                    static unsigned packets = 0;
                    static unsigned long long total = 0;
                    total += frames;
                    if (++packets % 1000 == 0)
                        std::fprintf(stderr, "sash: audio %u packets, %llu frames\n",
                                     packets, total);
                }

                if (!samples.empty()) {
                    if (wfx->nChannels > 2) {
                        downmix_stereo(samples, frames, wfx->nChannels, stereo);
                        sink(stereo.data(), frames, wfx->nSamplesPerSec, 2);
                    } else {
                        sink(samples.data(), frames, wfx->nSamplesPerSec, wfx->nChannels);
                    }
                }
            }

            capture->ReleaseBuffer(frames);
            if (FAILED(capture->GetNextPacketSize(&packet))) { packet = 0; break; }
        }
    }

    client->Stop();
    cleanup();
}

AudioCapture::AudioCapture() : impl_(std::make_unique<Impl>()) {}
AudioCapture::~AudioCapture() { stop(); }

bool AudioCapture::start(Sink sink) {
    stop();
    impl_->stop = false;
    impl_->thread = std::thread([this, sink] { impl_->run(sink); });
    return true;
}

void AudioCapture::stop() {
    if (!impl_->thread.joinable()) return;
    impl_->stop = true;
    impl_->thread.join();
}

}  // namespace sash
