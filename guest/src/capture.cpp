#include "capture.hpp"
#include "geometry.hpp"
#include "publisher.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

// Step-by-step tracing of capture startup, off unless SASH_TRACE is set. It is
// worth keeping: the failure that cost the most here was a crash partway
// through start(), and knowing which step it reached is what identified it.
static bool trace_enabled() {
    static const bool on = std::getenv("SASH_TRACE") != nullptr;
    return on;
}
#define TRACE(...) do { if (trace_enabled()) { \
    std::fprintf(stderr, "  [cap] " __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")

using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace sash {

bool capture_supported() {
    try {
        return GraphicsCaptureSession::IsSupported();
    } catch (...) {
        return false;
    }
}

struct WindowCapture::Impl {
    com_ptr<ID3D11Device>         device;
    com_ptr<ID3D11DeviceContext>  context;
    IDirect3DDevice               rt_device{nullptr};

    GraphicsCaptureItem           item{nullptr};
    Direct3D11CaptureFramePool    pool{nullptr};
    GraphicsCaptureSession        session{nullptr};
    Direct3D11CaptureFramePool::FrameArrived_revoker frame_token;

    /*
     * Two staging textures, not one.
     *
     * The captured frame lives in VRAM and the shared region is host RAM, so it
     * has to come back across PCIe. Map(D3D11_MAP_READ) blocks until the GPU
     * has finished the copy into that texture, so with a single staging texture
     * the copy and the read can never overlap: issue copy, stall, memcpy,
     * publish, repeat. Measured at ~40 ms a frame for 4K, which caps the stream
     * around 25 fps however fast the game runs.
     *
     * Reading the texture written *last* frame means the GPU has had a whole
     * frame to finish it, so the map does not stall. It costs one frame of
     * pipeline latency and buys back most of the throughput.
     */
    com_ptr<ID3D11Texture2D>      staging[2];
    int                           staging_at = 0;
    bool                          staging_pending = false;
    std::uint32_t                 pending_w = 0, pending_h = 0;
    std::uint32_t                 staging_w = 0, staging_h = 0;

    Publisher*                    pub = nullptr;
    std::mutex                    lock;

    std::atomic<std::uint64_t>    captured{0};
    std::atomic<std::uint64_t>    dropped{0};
    std::atomic<bool>             too_big{false};
    std::atomic<std::uint32_t>    content_w{0}, content_h{0};

    std::uint64_t                 qpc_freq = 0;

    double        pipeline_ms_total = 0;
    double        pipeline_ms_worst = 0;
    std::uint32_t pipeline_n = 0;

    // GDI fallback, for windows WGC will not capture at all.
    HWND                          target_hwnd = nullptr;
    HWND                          gdi_hwnd = nullptr;
    std::thread                   gdi_thread;
    std::atomic<bool>             gdi_stop{false};

    bool ensure_staging(std::uint32_t w, std::uint32_t h);
    void on_frame(const Direct3D11CaptureFramePool& sender);
    void gdi_loop();
};

/*
 * Capturing a menu.
 *
 * WGC refuses class #32768 outright - CreateForWindow returns E_INVALIDARG -
 * because a menu has no independently capturable DWM surface of its own. What
 * makes menus tractable anyway is that a menu is always the topmost thing on
 * screen for as long as it is open, so whatever the screen holds inside its
 * rectangle *is* the menu.
 *
 * So this blits from the screen DC. CAPTUREBLT is required or layered content
 * is missed. It costs a small GDI copy per frame, which is nothing for a window
 * this size, and it is only ever used for windows WGC has already rejected.
 */
void WindowCapture::Impl::gdi_loop() {
    HDC screen = GetDC(nullptr);
    HDC mem    = CreateCompatibleDC(screen);
    HBITMAP dib = nullptr;
    void*   bits = nullptr;
    int     dib_w = 0, dib_h = 0;

    while (!gdi_stop.load()) {
        if (!IsWindow(gdi_hwnd)) break;
        const RECT r = sash_content_rect(gdi_hwnd);

        const int w = r.right - r.left;
        const int h = r.bottom - r.top;
        if (w <= 0 || h <= 0) { Sleep(16); continue; }

        if (!dib || w != dib_w || h != dib_h) {
            if (dib) DeleteObject(dib);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth       = w;
            bi.bmiHeader.biHeight      = -h;   // negative: top-down, like everything else here
            bi.bmiHeader.biPlanes      = 1;
            bi.bmiHeader.biBitCount    = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!dib) break;
            SelectObject(mem, dib);
            dib_w = w;
            dib_h = h;
        }

        if (BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT)) {
            std::lock_guard<std::mutex> guard(lock);
            if (pub && pub->bound() &&
                static_cast<std::uint32_t>(w) <= pub->max_width() &&
                static_cast<std::uint32_t>(h) <= pub->max_height()) {
                std::uint32_t stride = 0;
                if (std::uint8_t* dst = pub->begin_frame(&stride)) {
                    const auto* srcp = static_cast<const std::uint8_t*>(bits);
                    const std::size_t row = static_cast<std::size_t>(w) * 4;
                    for (int y = 0; y < h; y++)
                        std::memcpy(dst + static_cast<std::size_t>(y) * stride,
                                    srcp + static_cast<std::size_t>(y) * row, row);
                    LARGE_INTEGER qpc{};
                    QueryPerformanceCounter(&qpc);
                    pub->publish(static_cast<std::uint32_t>(w),
                                 static_cast<std::uint32_t>(h), stride,
                                 static_cast<std::uint64_t>(qpc.QuadPart), qpc_freq,
                                 SASH_PUB_DAMAGE_FULL);
                    captured++;
                }
            }
        }
        Sleep(16);
    }

    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

bool WindowCapture::Impl::ensure_staging(std::uint32_t w, std::uint32_t h) {
    if (staging[0] && staging_w == w && staging_h == h) return true;

    D3D11_TEXTURE2D_DESC d{};
    d.Width              = w;
    d.Height             = h;
    d.MipLevels          = 1;
    d.ArraySize          = 1;
    d.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count   = 1;
    d.Usage              = D3D11_USAGE_STAGING;
    d.BindFlags          = 0;
    d.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
    d.MiscFlags          = 0;

    for (auto& tex : staging) {
        tex = nullptr;
        if (FAILED(device->CreateTexture2D(&d, nullptr, tex.put()))) {
            std::fprintf(stderr, "sash: staging texture %ux%u failed\n", w, h);
            return false;
        }
    }
    staging_w = w;
    staging_h = h;
    staging_at = 0;
    staging_pending = false;   /* the size changed; nothing in flight is usable */
    return true;
}

void WindowCapture::Impl::on_frame(const Direct3D11CaptureFramePool& sender) {
    static bool first = true;
    if (first) { first = false; TRACE("first frame callback"); }

    auto frame = sender.TryGetNextFrame();
    if (!frame) return;

    const auto size = frame.ContentSize();
    auto w = static_cast<std::uint32_t>(size.Width);
    auto h = static_cast<std::uint32_t>(size.Height);
    if (w == 0 || h == 0) return;

    // The whole frame is sent, Windows title bar and all: the host window is
    // undecorated, so that bar is the only chrome there is, and it works
    // because input reaches Windows unchanged.
    const std::uint32_t crop_x = 0, crop_y = 0;

    content_w = w;
    content_h = h;

    std::lock_guard<std::mutex> guard(lock);
    if (!pub || !pub->bound()) { dropped++; return; }

    // The pool's textures are at least ContentSize, often larger. Copy only the
    // content region, or the window gets a band of stale pixels down its edge.
    if (w > pub->max_width() || h > pub->max_height()) {
        too_big = true;
        dropped++;
        return;
    }
    too_big = false;

    auto access = frame.Surface().as<
        ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> src;
    if (FAILED(access->GetInterface(guid_of<ID3D11Texture2D>(), src.put_void()))) {
        dropped++;
        return;
    }

    if (!ensure_staging(w, h)) { dropped++; return; }

    D3D11_BOX box{};
    box.left = crop_x; box.top = crop_y; box.front = 0;
    box.right = crop_x + w; box.bottom = crop_y + h; box.back = 1;

    /* Start this frame's copy, then read the one started last frame - by now
     * the GPU has finished it, so the map below does not stall. */
    ID3D11Texture2D* writing = staging[staging_at].get();
    context->CopySubresourceRegion(writing, 0, 0, 0, 0, src.get(), 0, &box);

    const int read_index = 1 - staging_at;
    staging_at = read_index;

    if (!staging_pending) {
        /* Nothing in flight yet - this is the first frame after a start or a
         * resize, so there is nothing to publish until the next one. */
        staging_pending = true;
        pending_w = w;
        pending_h = h;
        return;
    }

    const std::uint32_t pub_w = pending_w, pub_h = pending_h;
    pending_w = w;
    pending_h = h;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging[read_index].get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        dropped++;
        return;
    }
    w = pub_w;
    h = pub_h;

    std::uint32_t stride = 0;
    std::uint8_t* dst = pub->begin_frame(&stride);
    if (dst) {
        const auto* srcp = static_cast<const std::uint8_t*>(mapped.pData);
        if (stride == mapped.RowPitch) {
            std::memcpy(dst, srcp, static_cast<std::size_t>(stride) * h);
        } else {
            const std::size_t row = static_cast<std::size_t>(w) * 4;
            for (std::uint32_t y = 0; y < h; y++)
                std::memcpy(dst + static_cast<std::size_t>(y) * stride,
                            srcp + static_cast<std::size_t>(y) * mapped.RowPitch, row);
        }
    }
    context->Unmap(staging[read_index].get(), 0);

    if (dst) {
        // Stamp when DWM composed the frame, not when we finished copying it.
        //
        // QueryPerformanceCounter here would be taken *after* the GPU->CPU
        // readback and the memcpy, so the resulting "age" would exclude the
        // guest-side capture pipeline - which is precisely where the time goes.
        // It measured 0.7 ms and meant nothing.
        //
        // SystemRelativeTime is in 100ns ticks on the same timebase as QPC, so
        // the frequency is reported as 10 MHz to match.
        std::uint64_t stamp;
        std::uint64_t stamp_freq;
        const auto composed = frame.SystemRelativeTime().count();
        if (composed > 0) {
            stamp      = static_cast<std::uint64_t>(composed);
            stamp_freq = 10000000ull;
        } else {
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            stamp      = static_cast<std::uint64_t>(qpc.QuadPart);
            stamp_freq = qpc_freq;
        }
        pub->publish(w, h, stride, stamp, stamp_freq, SASH_PUB_DAMAGE_FULL);
        captured++;

        // Guest-side cost, measured entirely within one clock: from the time
        // WGC says the frame was captured to the moment it is published. This
        // needs no host/guest alignment, so it cannot be blamed on clock error.
        if (composed > 0) {
            LARGE_INTEGER at_publish{};
            QueryPerformanceCounter(&at_publish);
            const double ms = (static_cast<double>(at_publish.QuadPart) -
                               static_cast<double>(composed)) * 1000.0 / qpc_freq;
            pipeline_ms_total += ms;
            if (ms > pipeline_ms_worst) pipeline_ms_worst = ms;
            if (++pipeline_n >= 240) {
                std::fprintf(stderr,
                    "sash: capture->publish avg %.2f ms worst %.2f ms over %u frames\n",
                    pipeline_ms_total / pipeline_n, pipeline_ms_worst, pipeline_n);
                pipeline_ms_total = 0; pipeline_ms_worst = 0; pipeline_n = 0;
            }
        }
    } else {
        dropped++;
    }
}

WindowCapture::WindowCapture() : impl_(std::make_unique<Impl>()) {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    impl_->qpc_freq = static_cast<std::uint64_t>(f.QuadPart);
}

WindowCapture::~WindowCapture() { stop(); }

bool WindowCapture::start(void* hwnd_raw, Publisher* pub) {
    if (!capture_supported()) {
        std::fprintf(stderr, "sash: Windows.Graphics.Capture unavailable "
                             "(needs Windows 10 1903+, and a display attached)\n");
        return false;
    }
    stop();

    TRACE("entered start");
    HWND hwnd = static_cast<HWND>(hwnd_raw);
    if (!IsWindow(hwnd)) return false;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // required for WGC interop
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 nullptr, 0, D3D11_SDK_VERSION,
                                 impl_->device.put(), nullptr, impl_->context.put()))) {
        std::fprintf(stderr, "sash: D3D11CreateDevice failed\n");
        return false;
    }

    TRACE("d3d11 device ok");
    auto dxgi = impl_->device.as<IDXGIDevice>();
    com_ptr<::IInspectable> inspectable;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()))) {
        std::fprintf(stderr, "sash: CreateDirect3D11DeviceFromDXGIDevice failed\n");
        return false;
    }
    impl_->rt_device = inspectable.as<IDirect3DDevice>();

    TRACE("rt device ok");
    auto interop = get_activation_factory<GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
    const HRESULT hr = interop->CreateForWindow(hwnd, guid_of<GraphicsCaptureItem>(),
                                                put_abi(impl_->item));
    if (FAILED(hr)) {
        wchar_t cls[64] = {0};
        GetClassNameW(hwnd, cls, 64);
        std::fprintf(stderr,
                     "sash: WGC refused HWND %p class '%ls' (0x%08lX); using GDI\n",
                     hwnd_raw, cls, static_cast<unsigned long>(hr));

        impl_->pub         = pub;
        impl_->target_hwnd = hwnd;
        impl_->gdi_hwnd    = hwnd;
        impl_->gdi_stop = false;
        impl_->gdi_thread = std::thread([this] { impl_->gdi_loop(); });
        return true;
    }

    TRACE("capture item ok");
    const auto size = impl_->item.Size();

    // Free-threaded: frames arrive on a pool thread rather than needing a
    // message loop, so capture is not held up by the agent's own UI thread.
    impl_->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        impl_->rt_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);

    TRACE("frame pool ok");
    impl_->pub         = pub;
    impl_->target_hwnd = hwnd;
    impl_->frame_token = impl_->pool.FrameArrived(auto_revoke,
        [this](const Direct3D11CaptureFramePool& sender, const auto&) {
            impl_->on_frame(sender);
        });

    TRACE("handler registered");
    impl_->session = impl_->pool.CreateCaptureSession(impl_->item);

    // Both of these are optional interfaces on newer Windows, and both must be
    // reached through try_as rather than called directly.
    //
    // A direct call is not merely unsupported on an older build - it crashes.
    // C++/WinRT's property shim reinterpret-casts the object to the interface
    // instead of doing a QueryInterface, so calling a method the runtime class
    // does not implement dispatches through a vtable that is not there. That is
    // an access violation, which no try/catch will save you from. try_as does a
    // real QueryInterface and returns null.
    //
    // IsCursorCaptureEnabled is IGraphicsCaptureSession2 (Windows 10 2004+).
    //
    // Keep the guest cursor in the image. Excluding it assumed the host would
    // draw a cursor of its own in the right place, and the result was a window
    // with no pointer in it at all. Including it means what the user sees is
    // where the guest actually thinks the pointer is, which is the only version
    // that can be trusted for clicking on things.
    if (auto s2 = impl_->session.try_as<IGraphicsCaptureSession2>())
        s2.IsCursorCaptureEnabled(true);

    // IsBorderRequired is IGraphicsCaptureSession3 - Windows 11 22000+ only.
    // On Windows 10 the yellow capture border stays; cosmetic, not fatal.
    if (auto s3 = impl_->session.try_as<IGraphicsCaptureSession3>())
        s3.IsBorderRequired(false);

    TRACE("session created; starting");
    impl_->session.StartCapture();
    return true;
}

void WindowCapture::stop() {
    if (impl_->gdi_thread.joinable()) {
        impl_->gdi_stop = true;
        impl_->gdi_thread.join();
    }
    impl_->gdi_hwnd = nullptr;
    impl_->target_hwnd = nullptr;

    if (impl_->session) {
        impl_->frame_token.revoke();
        try { impl_->session.Close(); } catch (...) {}
        impl_->session = nullptr;
    }
    if (impl_->pool) {
        try { impl_->pool.Close(); } catch (...) {}
        impl_->pool = nullptr;
    }
    impl_->item = nullptr;
    std::lock_guard<std::mutex> guard(impl_->lock);
    impl_->pub = nullptr;
    impl_->staging[0] = nullptr;
    impl_->staging[1] = nullptr;
    impl_->staging_w = impl_->staging_h = 0;
    impl_->staging_pending = false;
}

bool WindowCapture::needs_bigger_ring() const { return impl_->too_big.load(); }

void WindowCapture::content_size(std::uint32_t* w, std::uint32_t* h) const {
    if (w) *w = impl_->content_w.load();
    if (h) *h = impl_->content_h.load();
}

std::uint64_t WindowCapture::frames_captured() const { return impl_->captured.load(); }
std::uint64_t WindowCapture::frames_dropped() const { return impl_->dropped.load(); }

}  // namespace sash
