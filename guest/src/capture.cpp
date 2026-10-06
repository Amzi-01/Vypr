#include "capture.hpp"

#include "vypr_proto.h"
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

#include <avrt.h>
#include <d3d11_4.h>
#include <immintrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execution>
#include <mutex>
#include <thread>
#include <vector>

// Step-by-step tracing of capture startup, off unless VYPR_TRACE is set. It is
// worth keeping: the failure that cost the most here was a crash partway
// through start(), and knowing which step it reached is what identified it.
static bool trace_enabled() {
    static const bool on = std::getenv("VYPR_TRACE") != nullptr;
    return on;
}
#define TRACE(...) do { if (trace_enabled()) { \
    std::fprintf(stderr, "  [cap] " __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "avrt.lib")

using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace vypr {

namespace {

/* QPC now, in the 100 ns ticks WGC's SystemRelativeTime is counted in. */
std::int64_t qpc_100ns(std::uint64_t freq) {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    const auto f = static_cast<std::int64_t>(freq ? freq : 10000000ull);
    return (q.QuadPart / f) * 10000000 + (q.QuadPart % f) * 10000000 / f;
}

/*
 * Copy pixels into a ring with non-temporal stores.
 *
 * Rings are ordinary write-back RAM, and an ordinary store to write-back memory
 * first reads the cache line it is about to overwrite - for a frame, that is as
 * much reading as writing, all of it wasted, and it evicts whatever the game
 * had in cache on the way. Streaming stores go straight to memory without
 * either. (The IVSHMEM BAR rings used to live in was write-combined, which got
 * this for free; plain memcpy into write-back memory measured slower.)
 *
 * The caller fences afterwards: streaming stores are weakly ordered.
 */
void stream_copy(std::uint8_t* dst, const std::uint8_t* src, std::size_t n) {
    std::size_t head = (16u - (reinterpret_cast<std::uintptr_t>(dst) & 15u)) & 15u;
    if (head > n) head = n;
    std::memcpy(dst, src, head);
    dst += head; src += head; n -= head;

    std::size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 16));
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 32));
        const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 48));
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i),      a);
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i + 16), b);
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i + 32), c);
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i + 48), d);
    }
    for (; i + 16 <= n; i += 16)
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i),
                         _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i)));
    std::memcpy(dst + i, src + i, n - i);
}

}  // namespace

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
     * The readback pipeline: a frame is published the moment its own copy
     * lands, never when the next frame arrives.
     *
     * The captured frame lives in VRAM and the shared region is host RAM, so it
     * has to come back across PCIe, and Map(D3D11_MAP_READ) blocks until the
     * GPU has finished copying it into a staging texture. Doing that inline
     * capped 4K at ~25 fps (copy, stall, memcpy, publish, repeat), so this
     * used to read the texture written *last* frame instead. That fixed the
     * throughput and cost a whole frame of latency - and on a still screen,
     * where WGC only calls back when something changes, the last change of a
     * burst sat unpublished until something else moved. A keystroke could
     * stay invisible until the cursor next blinked.
     *
     * Now the frame-arrived callback only starts the GPU copy and queues it.
     * A reader thread waits for that copy on a GPU fence - an event, not a
     * spin and not a blocking Map holding the context - then writes the frame
     * into the ring and publishes it straight away. The copy of the next frame
     * overlaps the write of this one, which is the throughput the old scheme
     * bought, without the frame of delay.
     *
     * Three staging textures: one the reader is writing out, one queued behind
     * it, and one the next copy can go into. When frames come faster than they
     * can be written out the queued one is replaced, so the reader always
     * picks up the newest frame rather than working through a backlog.
     */
    enum class StageState { Free, Copying, Queued, Reading };

    /*
     * Each stage is read back in horizontal bands, one staging texture each.
     *
     * Reading a frame back over PCIe is most of the guest's cost - about 6 ms
     * of 7.5 for a 2560x1440 frame on this VM's x4 link - and in one piece the
     * copy into the ring cannot start until the last row has arrived. In bands
     * it starts as soon as the first one has, and copying band k overlaps the
     * GPU reading band k+1, so only the last band's copy is left after the
     * readback ends. One band for anything small, and for damage, which diffs
     * the frame as a whole.
     */
    static constexpr std::uint32_t kMaxBands = 8;
    struct Stage {
        std::array<com_ptr<ID3D11Texture2D>, kMaxBands> tex;
        // Fallback when there is no ID3D11Fence.
        std::array<com_ptr<ID3D11Query>, kMaxBands>     query;
        std::array<std::uint64_t, kMaxBands>            fence_value{};
        StageState               state = StageState::Free;
        std::uint32_t            w = 0, h = 0;
        std::int64_t             composed = 0;  // SystemRelativeTime, 100ns ticks
        // Held until the copy out of it has finished, so the pool cannot hand
        // its surface back to DWM while the GPU is still reading it.
        Direct3D11CaptureFrame   frame{nullptr};
    };
    static constexpr int          kStages = 3;
    std::array<Stage, kStages>    stages;
    std::uint32_t                 staging_w = 0, staging_h = 0;
    std::uint32_t                 staging_bands = 0;

    com_ptr<ID3D11DeviceContext4> context4;
    com_ptr<ID3D11Fence>          fence;
    std::uint64_t                 fence_value = 0;
    HANDLE                        fence_event = nullptr;

    std::mutex                    ctx_lock;     // the immediate context is single-threaded
    std::mutex                    job_lock;     // stage states and the queue
    std::condition_variable       job_cv;
    int                           queued = -1;  // stage waiting for the reader
    bool                          reader_stop = false;
    std::thread                   reader;

    Publisher*                    pub = nullptr;
    std::mutex                    lock;
    // Held for the whole of a frame callback, so stop() can wait out one that
    // is already running before it frees what the callback uses.
    std::mutex                    frame_lock;

    /*
     * Damage: publish only what changed since the last frame.
     *
     * `prev` holds the last whole frame this stream published, packed at the
     * ring stride. Each new frame is compared against it in tiles; the tiles
     * that differ become the damage rectangles, only those are written into the
     * ring, and `prev` is patched to match. A frame that changed nothing is not
     * published at all - the host already shows it. The first frame, a resize,
     * or damage spread across more tile rows than a publish can carry falls
     * back to a whole frame, which also refreshes `prev`. Off unless the host
     * asked; see VYPR_ATTACH_DAMAGE.
     */
    bool                          want_damage = false;
    std::vector<std::uint8_t>     prev;
    std::uint32_t                 prev_w = 0, prev_h = 0, prev_stride = 0;

    std::atomic<std::uint64_t>    captured{0};
    std::atomic<std::uint64_t>    dropped{0};
    std::atomic<std::uint64_t>    arrived{0};
    std::atomic<bool>             too_big{false};
    std::atomic<std::uint32_t>    content_w{0}, content_h{0};

    std::uint64_t                 qpc_freq = 0;

    double        pipeline_ms_total = 0;
    double        pipeline_ms_worst = 0;
    double        copy_ms_total = 0;
    std::uint32_t pipeline_n = 0;

    // GDI fallback, for windows WGC will not capture at all.
    HWND                          target_hwnd = nullptr;
    HWND                          gdi_hwnd = nullptr;
    std::thread                   gdi_thread;
    std::atomic<bool>             gdi_stop{false};

    bool ensure_staging(std::uint32_t w, std::uint32_t h);
    void on_frame(const Direct3D11CaptureFramePool& sender);
    void reader_loop();
    void write_out(Stage& st);
    void wait_band(Stage& st, std::uint32_t band);
    bool write_out_banded(Stage& st);
    void stop_reader();
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
        const RECT r = vypr_content_rect(gdi_hwnd);

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
                                 VYPR_PUB_DAMAGE_FULL);
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

/*
 * Ask the scheduler to treat this thread as a capture thread.
 *
 * MMCSS's "Capture" class is what Windows' own capture stack uses: the thread
 * is boosted into the realtime band for most of each period, so it is not left
 * queued behind a game that has every core busy. Once per thread - the frame
 * callbacks arrive on pool threads, so this runs on whichever one shows up.
 */
static void join_mmcss() {
    thread_local bool joined = false;
    if (joined) return;
    joined = true;
    DWORD task_index = 0;
    if (!AvSetMmThreadCharacteristicsW(L"Capture", &task_index))
        TRACE("MMCSS refused (%lu)", GetLastError());
}

bool WindowCapture::Impl::ensure_staging(std::uint32_t w, std::uint32_t h) {
    // Banded from about 1080p up, where it measured 15-19% faster on average
    // and steadier at the worst. Below that the readback is short already:
    // banding a 720p frame bought 6-10% on average but cost a millisecond or
    // two at the worst, so small frames are read back whole, as before.
    const std::size_t frame_bytes = (std::size_t)w * h * 4;
    std::uint32_t bands = frame_bytes >= (8u << 20) ? kMaxBands : 1;
    if (want_damage || h < bands * 16) bands = 1;
    if (stages[0].tex[0] && staging_w == w && staging_h == h && staging_bands == bands) return true;

    /* Nothing may be reading a texture while it is replaced: drop what is
     * queued, and wait for the reader to finish with the one it has. */
    {
        std::unique_lock<std::mutex> lk(job_lock);
        if (queued >= 0) {
            stages[queued].state = StageState::Free;
            stages[queued].frame = nullptr;
            queued = -1;
            dropped++;
        }
        job_cv.wait(lk, [this] {
            for (const auto& st : stages)
                if (st.state == StageState::Reading) return false;
            return true;
        });
    }

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

    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_EVENT;

    staging_w = staging_h = staging_bands = 0;
    for (auto& st : stages) {
        st.tex = {};
        st.query = {};
        st.state = StageState::Free;
        st.frame = nullptr;
        for (std::uint32_t b = 0; b < bands; b++) {
            d.Height = h * (b + 1) / bands - h * b / bands;
            if (FAILED(device->CreateTexture2D(&d, nullptr, st.tex[b].put()))) {
                std::fprintf(stderr, "vypr: staging texture %ux%u failed\n", w, d.Height);
                return false;
            }
            if (!fence && FAILED(device->CreateQuery(&qd, st.query[b].put()))) return false;
        }
    }
    staging_w = w;
    staging_h = h;
    staging_bands = bands;
    return true;
}

void WindowCapture::Impl::on_frame(const Direct3D11CaptureFramePool& sender) {
    std::lock_guard<std::mutex> in_frame(frame_lock);
    static bool first = true;
    if (first) { first = false; TRACE("first frame callback"); }
    arrived++;
    join_mmcss();

    auto frame = sender.TryGetNextFrame();
    if (!frame) return;
    const std::int64_t arrived_at = qpc_100ns(qpc_freq);

    // Newest wins. Frames that queued up in the pool while this thread was
    // busy are already out of date; showing them would only add their age to
    // everything after.
    while (auto newer = sender.TryGetNextFrame()) {
        frame = newer;
        dropped++;
    }

    const auto size = frame.ContentSize();
    auto w = static_cast<std::uint32_t>(size.Width);
    auto h = static_cast<std::uint32_t>(size.Height);
    if (w == 0 || h == 0) return;

    // The whole frame is sent, Windows title bar and all: the host window is
    // undecorated, so that bar is the only chrome there is, and it works
    // because input reaches Windows unchanged.
    content_w = w;
    content_h = h;

    {
        std::lock_guard<std::mutex> guard(lock);
        if (!pub || !pub->bound()) { dropped++; return; }

        // The pool's textures are at least ContentSize, often larger. Copy only
        // the content region, or the window gets a band of stale pixels down
        // its edge.
        if (w > pub->max_width() || h > pub->max_height()) {
            too_big = true;
            dropped++;
            return;
        }
        too_big = false;
    }

    auto access = frame.Surface().as<
        ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> src;
    if (FAILED(access->GetInterface(guid_of<ID3D11Texture2D>(), src.put_void()))) {
        dropped++;
        return;
    }

    if (!ensure_staging(w, h)) { dropped++; return; }

    /* A free stage, or failing that the queued one: it has not been read yet,
     * and this frame is newer. */
    int i = -1;
    {
        std::lock_guard<std::mutex> lk(job_lock);
        for (int k = 0; k < kStages && i < 0; k++)
            if (stages[k].state == StageState::Free) i = k;
        if (i < 0 && queued >= 0) {
            i = queued;
            queued = -1;
            stages[i].frame = nullptr;
            dropped++;
        }
        if (i < 0) { dropped++; return; }
        stages[i].state = StageState::Copying;
    }
    Stage& st = stages[i];

    {
        std::lock_guard<std::mutex> g(ctx_lock);
        for (std::uint32_t b = 0; b < staging_bands; b++) {
            D3D11_BOX box{};
            box.left = 0;  box.top = h * b / staging_bands;               box.front = 0;
            box.right = w; box.bottom = h * (b + 1) / staging_bands;      box.back = 1;
            context->CopySubresourceRegion(st.tex[b].get(), 0, 0, 0, 0, src.get(), 0, &box);
            if (fence) {
                st.fence_value[b] = ++fence_value;
                context4->Signal(fence.get(), st.fence_value[b]);
            } else {
                context->End(st.query[b].get());
            }
        }
        // Submit now: the reader is about to wait on this, and an unflushed
        // copy is one the GPU has not been told about.
        context->Flush();
    }
    st.w = w;
    st.h = h;
    /*
     * When the frame was captured. SystemRelativeTime is when DWM composed it,
     * which is what is wanted - except that for a window presenting on vsync
     * it is the vblank the composition is *for*, up to a frame in the future.
     * Stamped with that, the guest's own capture-to-publish time came out
     * negative and the host's frame age read as nothing at all. A frame cannot
     * have been captured after it arrived, so the arrival time bounds it.
     */
    st.composed = std::min<std::int64_t>(frame.SystemRelativeTime().count(),
                                         arrived_at);
    st.frame = std::move(frame);

    {
        std::lock_guard<std::mutex> lk(job_lock);
        if (queued >= 0) {
            // Superseded before the reader got to it.
            stages[queued].state = StageState::Free;
            stages[queued].frame = nullptr;
            dropped++;
        }
        queued = i;
        st.state = StageState::Queued;
    }
    job_cv.notify_all();
}

void WindowCapture::Impl::reader_loop() {
    join_mmcss();
    for (;;) {
        int i;
        {
            std::unique_lock<std::mutex> lk(job_lock);
            job_cv.wait(lk, [this] { return reader_stop || queued >= 0; });
            if (reader_stop) return;
            i = queued;
            queued = -1;
            stages[i].state = StageState::Reading;
        }
        write_out(stages[i]);
        {
            std::lock_guard<std::mutex> lk(job_lock);
            stages[i].state = StageState::Free;
        }
        job_cv.notify_all();
    }
}

/*
 * What changed since the last frame, as a handful of rectangles.
 *
 * The frame is diffed against `prev` in 64-pixel tiles. Each tile row that has
 * any changed tile becomes one rectangle spanning its changed columns, which
 * keeps a caret or a clock to a thin strip while never emitting more rectangles
 * than there are tile rows. Returns the number of rectangles, 0 when nothing
 * changed at all, or -1 when the damage spills past what one publish can carry
 * and the caller should send a whole frame instead.
 *
 * `prev` is packed at `prev_pitch`; `src` carries the GPU's own `src_pitch`.
 */
static constexpr std::uint32_t kTile = 64;

static int compute_damage(const std::uint8_t* src, std::uint32_t src_pitch,
                          const std::uint8_t* prev, std::uint32_t prev_pitch,
                          std::uint32_t w, std::uint32_t h, vypr_rect* out) {
    int n = 0;
    for (std::uint32_t ty = 0; ty < h; ty += kTile) {
        const std::uint32_t rh = (ty + kTile <= h) ? kTile : h - ty;
        std::uint32_t minc = w, maxc = 0;   // changed column span in this tile row

        for (std::uint32_t tx = 0; tx < w; tx += kTile) {
            const std::uint32_t rw = (tx + kTile <= w) ? kTile : w - tx;
            bool changed = false;
            for (std::uint32_t y = ty; y < ty + rh && !changed; y++) {
                if (std::memcmp(src + (std::size_t)y * src_pitch + (std::size_t)tx * 4,
                                prev + (std::size_t)y * prev_pitch + (std::size_t)tx * 4,
                                (std::size_t)rw * 4) != 0)
                    changed = true;
            }
            if (changed) {
                if (tx < minc) minc = tx;
                if (tx + rw > maxc) maxc = tx + rw;
            }
        }

        if (minc < maxc) {
            if (n == (int)VYPR_MAX_DAMAGE_RECTS) return -1;   // too scattered
            out[n].x = minc;
            out[n].y = ty;
            out[n].w = maxc - minc;
            out[n].h = rh;
            n++;
        }
    }
    return n;
}

// Wait for one band's copy on the GPU's own signal: no spinning, and the
// context stays free for the next frame's copy meanwhile.
void WindowCapture::Impl::wait_band(Stage& st, std::uint32_t band) {
    if (fence) {
        if (fence->GetCompletedValue() < st.fence_value[band] &&
            SUCCEEDED(fence->SetEventOnCompletion(st.fence_value[band], fence_event)))
            WaitForSingleObject(fence_event, 250);
    } else {
        for (;;) {
            BOOL done = FALSE;
            HRESULT hr;
            {
                std::lock_guard<std::mutex> g(ctx_lock);
                hr = context->GetData(st.query[band].get(), &done, sizeof(done),
                                      D3D11_ASYNC_GETDATA_DONOTFLUSH);
            }
            if (FAILED(hr) || (hr == S_OK && done)) break;
            SwitchToThread();
        }
    }
}

/*
 * A whole frame, band by band: each band is copied into the ring as soon as it
 * has arrived, while the GPU is still reading back the ones after it. Returns
 * whether it published.
 *
 * The publisher lock is held only to take a buffer and to publish, not across
 * the waits: the frame callback takes it too, and stalling it behind a readback
 * would delay the next frame's copy being issued. Nothing else can be using the
 * publisher meanwhile - stop() waits for this thread before it lets go of it.
 */
bool WindowCapture::Impl::write_out_banded(Stage& st) {
    const std::uint32_t w = st.w, h = st.h, bands = staging_bands;
    std::uint32_t stride = 0;
    std::uint8_t* dst = nullptr;
    {
        std::lock_guard<std::mutex> guard(lock);
        dst = (pub && pub->bound()) ? pub->begin_frame(&stride) : nullptr;
    }

    const std::int64_t copy_start = qpc_100ns(qpc_freq);
    double waited_ms = 0;
    bool ok = dst != nullptr;
    const std::size_t row = (std::size_t)w * 4;
    /*
     * Give the capture surface back the moment the GPU has finished reading
     * it, not when this loop gets round to waiting for the last band. The pool
     * has three surfaces, and holding one through the copies of the bands
     * before it left DWM without a free one often enough to cost frames.
     */
    const auto release_if_done = [&] {
        if (st.frame && fence && fence->GetCompletedValue() >= st.fence_value[bands - 1])
            st.frame = nullptr;
    };
    for (std::uint32_t b = 0; b < bands; b++) {
        const std::int64_t wait_start = qpc_100ns(qpc_freq);
        wait_band(st, b);
        waited_ms += (qpc_100ns(qpc_freq) - wait_start) / 10000.0;
        release_if_done();
        if (b == bands - 1) st.frame = nullptr;   // in, whatever kind of wait it was
        if (!ok) continue;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        {
            std::lock_guard<std::mutex> g(ctx_lock);
            if (FAILED(context->Map(st.tex[b].get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                ok = false;
                continue;
            }
        }
        const std::uint32_t y0 = h * b / bands, y1 = h * (b + 1) / bands;
        const auto* srcp = static_cast<const std::uint8_t*>(mapped.pData);
        if (stride == mapped.RowPitch) {
            stream_copy(dst + (std::size_t)y0 * stride, srcp, (std::size_t)(y1 - y0) * stride);
        } else {
            for (std::uint32_t y = y0; y < y1; y++)
                stream_copy(dst + (std::size_t)y * stride,
                            srcp + (std::size_t)(y - y0) * mapped.RowPitch, row);
        }
        {
            std::lock_guard<std::mutex> g(ctx_lock);
            context->Unmap(st.tex[b].get(), 0);
        }
        release_if_done();
    }
    _mm_sfence();
    // Time spent copying, not waiting for the GPU to deliver the next band.
    copy_ms_total += (qpc_100ns(qpc_freq) - copy_start) / 10000.0 - waited_ms;
    if (!ok) return false;

    std::lock_guard<std::mutex> guard(lock);
    if (!pub || !pub->bound()) return false;
    std::uint64_t stamp, stamp_freq;
    if (st.composed > 0) { stamp = (std::uint64_t)st.composed; stamp_freq = 10000000ull; }
    else { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); stamp = (std::uint64_t)q.QuadPart; stamp_freq = qpc_freq; }
    return pub->publish(w, h, stride, stamp, stamp_freq, VYPR_PUB_DAMAGE_FULL);
}

void WindowCapture::Impl::write_out(Stage& st) {
    bool published = false;
    bool skipped   = false;

    if (staging_bands > 1) {
        published = write_out_banded(st);
    } else {
        wait_band(st, 0);
        // The copy is done; the pool can have the surface back.
        st.frame = nullptr;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        {
            std::lock_guard<std::mutex> g(ctx_lock);
            if (FAILED(context->Map(st.tex[0].get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                dropped++;
                return;
            }
        }

        const std::uint32_t w = st.w, h = st.h;
        const auto* srcp = static_cast<const std::uint8_t*>(mapped.pData);
        const std::uint32_t src_pitch = mapped.RowPitch;

        {
            std::lock_guard<std::mutex> guard(lock);
            std::uint32_t stride = 0;
            std::uint8_t* dst = (pub && pub->bound()) ? pub->begin_frame(&stride) : nullptr;
            if (dst) {
                // Decide whole-frame vs damage. A damage frame needs a previous
                // frame of the same shape to diff against; anything else is whole,
                // which also (re)establishes `prev`.
                vypr_rect rects[VYPR_MAX_DAMAGE_RECTS];
                int nd = -1;
                const bool have_prev = want_damage && prev_w == w && prev_h == h &&
                                       prev_stride == stride &&
                                       prev.size() >= (std::size_t)stride * h;
                if (have_prev)
                    nd = compute_damage(srcp, src_pitch, prev.data(), stride, w, h, rects);

                if (have_prev && nd == 0) {
                    // Nothing changed. Publishing would only repeat the frame the
                    // host already shows, so leave the serial where it is.
                    skipped = true;
                } else if (have_prev && nd > 0) {
                    // Partial: write and patch only the changed rectangles.
                    for (int r = 0; r < nd; r++) {
                        const vypr_rect d = rects[r];
                        for (std::uint32_t y = d.y; y < d.y + d.h; y++) {
                            const std::size_t ro = (std::size_t)y * stride + (std::size_t)d.x * 4;
                            const std::size_t so = (std::size_t)y * src_pitch + (std::size_t)d.x * 4;
                            stream_copy(dst + ro, srcp + so, (std::size_t)d.w * 4);
                            std::memcpy(prev.data() + ro, srcp + so, (std::size_t)d.w * 4);
                        }
                    }
                    _mm_sfence();
                    std::uint64_t stamp, stamp_freq;
                    if (st.composed > 0) { stamp = (std::uint64_t)st.composed; stamp_freq = 10000000ull; }
                    else { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); stamp = (std::uint64_t)q.QuadPart; stamp_freq = qpc_freq; }
                    published = pub->publish(w, h, stride, stamp, stamp_freq,
                                             VYPR_PUB_DAMAGE_RECTS, rects, (std::uint32_t)nd);
                } else {
                    /*
                     * Whole frame, copied in bands in parallel once it is big
                     * enough to be worth it. One core can only keep so many
                     * streaming stores in flight; a 4K frame is 33 MB, and
                     * splitting it lets several cores stream at once. Below a few
                     * megabytes the hand-off costs more than it saves.
                     */
                    const std::int64_t copy_start = qpc_100ns(qpc_freq);
                    const std::size_t row = (std::size_t)w * 4;
                    const std::uint32_t bands = ((std::size_t)w * h * 4 >= (8u << 20)) ? 4 : 1;
                    std::array<std::uint32_t, 4> band_ids{ 0, 1, 2, 3 };
                    auto copy_band = [&](std::uint32_t b) {
                        const std::uint32_t y0 = h * b / bands, y1 = h * (b + 1) / bands;
                        if (stride == src_pitch) {
                            stream_copy(dst + (std::size_t)y0 * stride,
                                        srcp + (std::size_t)y0 * src_pitch,
                                        (std::size_t)(y1 - y0) * stride);
                        } else {
                            for (std::uint32_t y = y0; y < y1; y++)
                                stream_copy(dst + (std::size_t)y * stride,
                                            srcp + (std::size_t)y * src_pitch, row);
                        }
                        _mm_sfence();   // each core orders its own streaming stores
                    };
                    if (bands > 1)
                        std::for_each(std::execution::par, band_ids.begin(), band_ids.begin() + bands, copy_band);
                    else
                        copy_band(0);
                    copy_ms_total += (qpc_100ns(qpc_freq) - copy_start) / 10000.0;

                    // Keep this whole frame as the baseline for the next diff, in
                    // the ring's own layout so a damage copy addresses both alike.
                    if (want_damage) {
                        prev.assign((std::size_t)stride * h, 0);
                        for (std::uint32_t y = 0; y < h; y++)
                            std::memcpy(prev.data() + (std::size_t)y * stride,
                                        srcp + (std::size_t)y * src_pitch, row);
                        prev_w = w; prev_h = h; prev_stride = stride;
                    }

                    // Stamp when DWM composed the frame, not when we finished
                    // copying it. A QPC reading here would come after the readback
                    // and the copy, so the host's "age" would leave out exactly the
                    // guest-side pipeline - which is where the time goes.
                    // SystemRelativeTime is 100ns ticks on the QPC timebase, so the
                    // frequency is reported as 10 MHz to match.
                    std::uint64_t stamp, stamp_freq;
                    if (st.composed > 0) { stamp = (std::uint64_t)st.composed; stamp_freq = 10000000ull; }
                    else { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); stamp = (std::uint64_t)q.QuadPart; stamp_freq = qpc_freq; }
                    published = pub->publish(w, h, stride, stamp, stamp_freq, VYPR_PUB_DAMAGE_FULL);
                }
            }
        }
        {
            std::lock_guard<std::mutex> g(ctx_lock);
            context->Unmap(st.tex[0].get(), 0);
        }
    }

    if (skipped) return;             // a no-op, not a drop
    if (!published) { dropped++; return; }
    captured++;

    // Guest-side cost, measured entirely within one clock: from the time WGC
    // says the frame was captured to the moment it is published. This needs no
    // host/guest alignment, so it cannot be blamed on clock error.
    if (st.composed > 0) {
        const double ms = (qpc_100ns(qpc_freq) - st.composed) / 10000.0;
        pipeline_ms_total += ms;
        if (ms > pipeline_ms_worst) pipeline_ms_worst = ms;
        if (++pipeline_n >= 240) {
            std::fprintf(stderr,
                "vypr: capture->publish avg %.2f ms worst %.2f ms, copy avg %.2f ms, "
                "over %u frames\n",
                pipeline_ms_total / pipeline_n, pipeline_ms_worst,
                copy_ms_total / pipeline_n, pipeline_n);
            pipeline_ms_total = 0; pipeline_ms_worst = 0; copy_ms_total = 0; pipeline_n = 0;
        }
    }
}

void WindowCapture::Impl::stop_reader() {
    if (reader.joinable()) {
        {
            std::lock_guard<std::mutex> lk(job_lock);
            reader_stop = true;
        }
        job_cv.notify_all();
        reader.join();
    }
    reader_stop = false;
    queued = -1;
    for (auto& st : stages) {
        st.state = StageState::Free;
        st.frame = nullptr;
    }
}

WindowCapture::WindowCapture() : impl_(std::make_unique<Impl>()) {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    impl_->qpc_freq = static_cast<std::uint64_t>(f.QuadPart);
}

WindowCapture::~WindowCapture() {
    stop();
    if (impl_->fence_event) CloseHandle(impl_->fence_event);
}

bool WindowCapture::start(void* hwnd_raw, Publisher* pub, bool damage) {
    if (!capture_supported()) {
        std::fprintf(stderr, "vypr: Windows.Graphics.Capture unavailable "
                             "(needs Windows 10 1903+, and a display attached)\n");
        return false;
    }
    stop();

    TRACE("entered start");
    impl_->want_damage = damage;
    impl_->prev.clear();
    impl_->prev_w = impl_->prev_h = impl_->prev_stride = 0;
    HWND hwnd = static_cast<HWND>(hwnd_raw);

    /* The whole screen rather than one window - see VYPR_DESKTOP_WINDOW_ID. */
    const bool whole_desktop =
        reinterpret_cast<std::uintptr_t>(hwnd_raw) == VYPR_DESKTOP_WINDOW_ID;

    if (!whole_desktop && !IsWindow(hwnd)) return false;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // required for WGC interop
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 nullptr, 0, D3D11_SDK_VERSION,
                                 impl_->device.put(), nullptr, impl_->context.put()))) {
        std::fprintf(stderr, "vypr: D3D11CreateDevice failed\n");
        return false;
    }

    TRACE("d3d11 device ok");

    // The immediate context is now used from two threads - copies from the
    // frame callback, maps from the reader. ctx_lock serialises every call
    // already; this makes the runtime enforce it too, should one ever slip.
    if (auto mt = impl_->context.try_as<ID3D11Multithread>())
        mt->SetMultithreadProtected(TRUE);

    // A fence the reader thread can wait on for each copy (Windows 10 1703+).
    // Without one it falls back to polling an event query.
    impl_->fence = nullptr;
    impl_->fence_value = 0;
    impl_->context4 = impl_->context.try_as<ID3D11DeviceContext4>();
    if (auto dev5 = impl_->device.try_as<ID3D11Device5>(); dev5 && impl_->context4) {
        if (!impl_->fence_event)
            impl_->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!impl_->fence_event ||
            FAILED(dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, guid_of<ID3D11Fence>(),
                                     impl_->fence.put_void())))
            impl_->fence = nullptr;
    }
    if (!impl_->fence)
        std::fprintf(stderr, "vypr: no D3D11 fence; readback polls an event query\n");
    auto dxgi = impl_->device.as<IDXGIDevice>();
    com_ptr<::IInspectable> inspectable;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()))) {
        std::fprintf(stderr, "vypr: CreateDirect3D11DeviceFromDXGIDevice failed\n");
        return false;
    }
    impl_->rt_device = inspectable.as<IDirect3DDevice>();

    TRACE("rt device ok");
    auto interop = get_activation_factory<GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
    HRESULT hr;
    if (whole_desktop) {
        HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        hr = interop->CreateForMonitor(mon, guid_of<GraphicsCaptureItem>(),
                                       put_abi(impl_->item));
        if (FAILED(hr))
            std::fprintf(stderr, "vypr: WGC refused the monitor (0x%08lX)\n",
                         static_cast<unsigned long>(hr));
    } else {
        hr = interop->CreateForWindow(hwnd, guid_of<GraphicsCaptureItem>(),
                                      put_abi(impl_->item));
    }
    if (FAILED(hr) && whole_desktop) return false;
    if (FAILED(hr)) {
        wchar_t cls[64] = {0};
        GetClassNameW(hwnd, cls, 64);
        std::fprintf(stderr,
                     "vypr: WGC refused HWND %p class '%ls' (0x%08lX); using GDI\n",
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
    // Three buffers, because a frame is now held until its copy is done: with
    // two, one held and one queued left DWM nothing to draw the next into.
    impl_->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        impl_->rt_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);

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

    impl_->reader = std::thread([this] { impl_->reader_loop(); });

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

    // Closing the session stops new callbacks, not one already running. Wait
    // that out, then the reader, and only then free what both of them use.
    { std::lock_guard<std::mutex> in_frame(impl_->frame_lock); }
    impl_->stop_reader();

    std::lock_guard<std::mutex> guard(impl_->lock);
    impl_->pub = nullptr;
    for (auto& st : impl_->stages) {
        st.tex = {};
        st.query = {};
    }
    impl_->staging_w = impl_->staging_h = impl_->staging_bands = 0;
    impl_->fence = nullptr;
    impl_->context4 = nullptr;
    impl_->prev.clear();
    impl_->prev_w = impl_->prev_h = impl_->prev_stride = 0;
}

bool WindowCapture::needs_bigger_ring() const { return impl_->too_big.load(); }

void WindowCapture::content_size(std::uint32_t* w, std::uint32_t* h) const {
    if (w) *w = impl_->content_w.load();
    if (h) *h = impl_->content_h.load();
}

std::uint64_t WindowCapture::frames_captured() const { return impl_->captured.load(); }
std::uint64_t WindowCapture::frames_dropped() const { return impl_->dropped.load(); }
std::uint64_t WindowCapture::frames_arrived() const { return impl_->arrived.load(); }

}  // namespace vypr
