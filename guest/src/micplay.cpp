#include "micplay.hpp"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace sash {

struct MicPlayback::Impl {
    std::thread        thread;
    std::atomic<bool>  stop{false};

    std::mutex         lock;
    std::vector<float> queue;      // interleaved, at device rate and channels
    std::uint32_t      dev_rate = 0;
    std::uint16_t      dev_channels = 0;

    void run();
};

/*
 * The virtual cable's playback side. Matching on the name is unlovely, but a
 * cable has no other distinguishing property - it is an ordinary render
 * endpoint whose output happens to reappear as an input.
 */
static IMMDevice* find_cable(IMMDeviceEnumerator* en) {
    IMMDeviceCollection* devices = nullptr;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)))
        return nullptr;

    UINT count = 0;
    devices->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(devices->Item(i, &dev))) continue;

        IPropertyStore* props = nullptr;
        bool match = false;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) &&
                name.vt == VT_LPWSTR) {
                match = wcsstr(name.pwszVal, L"VB-Audio") != nullptr &&
                        wcsstr(name.pwszVal, L"16 Ch") == nullptr;
                if (match)
                    std::fprintf(stderr, "sash: microphone goes to '%ls'\n", name.pwszVal);
            }
            PropVariantClear(&name);
            props->Release();
        }
        if (match) { devices->Release(); return dev; }
        dev->Release();
    }

    devices->Release();
    return nullptr;
}

void MicPlayback::Impl::run() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;

    IMMDeviceEnumerator* en = nullptr;
    IMMDevice*           dev = nullptr;
    IAudioClient*        client = nullptr;
    IAudioRenderClient*  render = nullptr;
    WAVEFORMATEX*        wfx = nullptr;

    auto cleanup = [&] {
        if (render) render->Release();
        if (client) client->Release();
        if (dev)    dev->Release();
        if (en)     en->Release();
        if (wfx)    CoTaskMemFree(wfx);
        CoUninitialize();
    };

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en))) {
        cleanup();
        return;
    }
    dev = find_cable(en);
    if (!dev) {
        std::fprintf(stderr, "sash: no virtual audio cable; microphone unavailable\n");
        cleanup();
        return;
    }
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) ||
        FAILED(client->GetMixFormat(&wfx))) {
        cleanup();
        return;
    }

    // 40ms. Small, because everything the device holds is delay on the far end
    // of somebody's sentence.
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 400000, 0, wfx, nullptr)) ||
        FAILED(client->GetService(__uuidof(IAudioRenderClient), (void**)&render))) {
        std::fprintf(stderr, "sash: could not open the cable for playback\n");
        cleanup();
        return;
    }

    UINT32 buffer_frames = 0;
    client->GetBufferSize(&buffer_frames);
    {
        std::lock_guard<std::mutex> g(lock);
        dev_rate     = wfx->nSamplesPerSec;
        dev_channels = wfx->nChannels;
    }
    client->Start();

    const bool is_float = wfx->wBitsPerSample == 32;

    while (!stop.load()) {
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;

        const UINT32 free_frames = buffer_frames - padding;
        if (free_frames == 0) { Sleep(2); continue; }

        BYTE* out = nullptr;
        if (FAILED(render->GetBuffer(free_frames, &out))) break;

        const std::size_t want = static_cast<std::size_t>(free_frames) * wfx->nChannels;
        std::size_t taken = 0;
        {
            std::lock_guard<std::mutex> g(lock);
            taken = queue.size() < want ? queue.size() : want;
            if (taken && is_float) std::memcpy(out, queue.data(), taken * sizeof(float));
            if (taken) queue.erase(queue.begin(), queue.begin() + taken);
        }

        // Pad with silence rather than starving the device, which would click.
        if (is_float && taken < want)
            std::memset(reinterpret_cast<float*>(out) + taken, 0,
                        (want - taken) * sizeof(float));

        render->ReleaseBuffer(free_frames, taken ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
        Sleep(2);
    }

    client->Stop();
    cleanup();
}

void MicPlayback::submit(const float* samples, std::uint32_t frames,
                         std::uint32_t rate, std::uint16_t channels) {
    std::lock_guard<std::mutex> g(impl_->lock);

    const std::uint32_t dr = impl_->dev_rate;
    const std::uint16_t dc = impl_->dev_channels;
    if (!dr || !dc || !frames || !channels || !rate) return;   /* not open yet */

    /*
     * Match the cable's own rate and channel count. Usually both ends are
     * 48 kHz stereo and this is a straight copy; the nearest-sample resample
     * below is a fallback for when they disagree, which is crude but perfectly
     * adequate for speech and far better than refusing to carry it.
     */
    const double ratio = static_cast<double>(dr) / static_cast<double>(rate);
    const std::uint32_t out_frames =
        static_cast<std::uint32_t>(frames * ratio);

    for (std::uint32_t i = 0; i < out_frames; i++) {
        std::uint32_t src = static_cast<std::uint32_t>(i / ratio);
        if (src >= frames) src = frames - 1;
        for (std::uint16_t c = 0; c < dc; c++) {
            const std::uint16_t sc = c < channels ? c : 0;   /* mono fills both */
            impl_->queue.push_back(samples[src * channels + sc]);
        }
    }

    /*
     * Keep it shallow. Queue depth is latency, and a microphone that arrives
     * late is worse than one that drops a syllable - so old audio goes rather
     * than being played behind the speaker.
     */
    const std::size_t max_samples =
        static_cast<std::size_t>(dr) * dc * 60 / 1000;      /* 60ms */
    if (impl_->queue.size() > max_samples)
        impl_->queue.erase(impl_->queue.begin(),
                           impl_->queue.begin() + (impl_->queue.size() - max_samples));
}

MicPlayback::MicPlayback() : impl_(std::make_unique<Impl>()) {}
MicPlayback::~MicPlayback() { stop(); }

bool MicPlayback::start() {
    stop();
    impl_->stop = false;
    impl_->thread = std::thread([this] { impl_->run(); });
    return true;
}

void MicPlayback::stop() {
    if (!impl_->thread.joinable()) return;
    impl_->stop = true;
    impl_->thread.join();
}

}  // namespace sash
