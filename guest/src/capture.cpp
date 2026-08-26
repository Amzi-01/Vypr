#include "capture.hpp"
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
#include <mutex>

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

    // Staging texture: the captured frame lives in VRAM and the shared region
    // is host RAM, so it has to come back across PCIe. One DMA into staging,
    // one memcpy out of it into the ring.
    com_ptr<ID3D11Texture2D>      staging;
    std::uint32_t                 staging_w = 0, staging_h = 0;

    Publisher*                    pub = nullptr;
    std::mutex                    lock;

    std::atomic<std::uint64_t>    captured{0};
    std::atomic<std::uint64_t>    dropped{0};
    std::atomic<bool>             too_big{false};
    std::atomic<std::uint32_t>    content_w{0}, content_h{0};

    std::uint64_t                 qpc_freq = 0;

    bool ensure_staging(std::uint32_t w, std::uint32_t h);
    void on_frame(const Direct3D11CaptureFramePool& sender);
};

bool WindowCapture::Impl::ensure_staging(std::uint32_t w, std::uint32_t h) {
    if (staging && staging_w == w && staging_h == h) return true;

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

    staging = nullptr;
    if (FAILED(device->CreateTexture2D(&d, nullptr, staging.put()))) {
        std::fprintf(stderr, "sash: staging texture %ux%u failed\n", w, h);
        return false;
    }
    staging_w = w;
    staging_h = h;
    return true;
}

void WindowCapture::Impl::on_frame(const Direct3D11CaptureFramePool& sender) {
    auto frame = sender.TryGetNextFrame();
    if (!frame) return;

    const auto size = frame.ContentSize();
    const auto w = static_cast<std::uint32_t>(size.Width);
    const auto h = static_cast<std::uint32_t>(size.Height);
    if (w == 0 || h == 0) return;

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
    box.left = 0; box.top = 0; box.front = 0;
    box.right = w; box.bottom = h; box.back = 1;
    context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, src.get(), 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        dropped++;
        return;
    }

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
    context->Unmap(staging.get(), 0);

    if (dst) {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        pub->publish(w, h, stride, static_cast<std::uint64_t>(qpc.QuadPart),
                     qpc_freq, SASH_PUB_DAMAGE_FULL);
        captured++;
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

    auto dxgi = impl_->device.as<IDXGIDevice>();
    com_ptr<::IInspectable> inspectable;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()))) {
        std::fprintf(stderr, "sash: CreateDirect3D11DeviceFromDXGIDevice failed\n");
        return false;
    }
    impl_->rt_device = inspectable.as<IDirect3DDevice>();

    auto interop = get_activation_factory<GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
    if (FAILED(interop->CreateForWindow(hwnd, guid_of<GraphicsCaptureItem>(),
                                        put_abi(impl_->item)))) {
        std::fprintf(stderr, "sash: CreateForWindow failed for HWND %p\n", hwnd_raw);
        return false;
    }

    const auto size = impl_->item.Size();

    // Free-threaded: frames arrive on a pool thread rather than needing a
    // message loop, so capture is not held up by the agent's own UI thread.
    impl_->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        impl_->rt_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);

    impl_->pub = pub;
    impl_->frame_token = impl_->pool.FrameArrived(auto_revoke,
        [this](const Direct3D11CaptureFramePool& sender, const auto&) {
            impl_->on_frame(sender);
        });

    impl_->session = impl_->pool.CreateCaptureSession(impl_->item);

    // The host draws its own cursor from the CURSOR message, so the guest one
    // must not be burned into the pixels.
    try { impl_->session.IsCursorCaptureEnabled(false); } catch (...) {}
    // Removes the yellow capture border. Only on Windows 10 2004+, so a failure
    // here is cosmetic, not fatal.
    try { impl_->session.IsBorderRequired(false); } catch (...) {}

    impl_->session.StartCapture();
    return true;
}

void WindowCapture::stop() {
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
    impl_->staging = nullptr;
    impl_->staging_w = impl_->staging_h = 0;
}

bool WindowCapture::needs_bigger_ring() const { return impl_->too_big.load(); }

void WindowCapture::content_size(std::uint32_t* w, std::uint32_t* h) const {
    if (w) *w = impl_->content_w.load();
    if (h) *h = impl_->content_h.load();
}

std::uint64_t WindowCapture::frames_captured() const { return impl_->captured.load(); }
std::uint64_t WindowCapture::frames_dropped() const { return impl_->dropped.load(); }

}  // namespace sash
