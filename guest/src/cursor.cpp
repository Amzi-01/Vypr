#include "cursor.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>

namespace vypr {
namespace {

// A bitmap as 32-bit top-down BGRA, whatever depth it was stored at. GDI does
// the conversion; a 1bpp mask comes back as black and white pixels.
bool bitmap_bits(HBITMAP bmp, int w, int h, std::vector<std::uint8_t>& out)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    out.assign(static_cast<std::size_t>(w) * h * 4, 0);
    HDC dc = GetDC(nullptr);
    const int got = GetDIBits(dc, bmp, 0, h, out.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return got == h;
}

// The cursor's image as straight-alpha BGRA.
//
// Two kinds exist. A colour cursor - every modern arrow and hand - has a
// 32-bit image, usually with real alpha; an old one without alpha uses its AND
// mask for transparency instead. A monochrome cursor has only a mask twice the
// height: the top half ANDs with the screen, the bottom half XORs. Most pixels
// map cleanly to black, white or transparent, but AND=1 XOR=1 means "invert
// the screen", which the host cannot do. The I-beam is drawn that way, and
// rendered black it vanishes on a dark background - so inverted pixels are
// drawn white with a black outline, which stays visible on anything.
bool cursor_image(HCURSOR cur, CursorShape& s)
{
    ICONINFO ii{};
    if (!GetIconInfo(cur, &ii)) return false;
    s.hotspot_x = static_cast<std::int32_t>(ii.xHotspot);
    s.hotspot_y = static_cast<std::int32_t>(ii.yHotspot);

    bool ok = false;
    BITMAP bm{};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof bm, &bm)) {
        const int w = bm.bmWidth, h = bm.bmHeight;
        if (bitmap_bits(ii.hbmColor, w, h, s.bgra)) {
            s.width  = static_cast<std::uint32_t>(w);
            s.height = static_cast<std::uint32_t>(h);
            bool has_alpha = false;
            for (std::size_t i = 3; i < s.bgra.size(); i += 4)
                if (s.bgra[i]) { has_alpha = true; break; }

            if (!has_alpha) {
                std::vector<std::uint8_t> mask;
                const bool masked = ii.hbmMask && bitmap_bits(ii.hbmMask, w, h, mask);
                for (std::size_t p = 0; p < s.bgra.size(); p += 4)
                    s.bgra[p + 3] = (masked && mask[p]) ? 0 : 0xFF;
            }
            ok = true;
        }
    } else if (ii.hbmMask && GetObjectW(ii.hbmMask, sizeof bm, &bm)) {
        const int w = bm.bmWidth, h = bm.bmHeight / 2;
        std::vector<std::uint8_t> mask;
        if (h > 0 && bitmap_bits(ii.hbmMask, w, h * 2, mask)) {
            s.width  = static_cast<std::uint32_t>(w);
            s.height = static_cast<std::uint32_t>(h);
            s.bgra.assign(static_cast<std::size_t>(w) * h * 4, 0);
            std::vector<std::uint8_t> invert(static_cast<std::size_t>(w) * h, 0);

            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * w + x;
                    const bool a  = mask[i * 4] != 0;
                    const bool xr = mask[(static_cast<std::size_t>(y + h) * w + x) * 4] != 0;
                    std::uint8_t* px = &s.bgra[i * 4];
                    if (!a)            { std::uint8_t v = xr ? 0xFF : 0x00;
                                         px[0] = px[1] = px[2] = v; px[3] = 0xFF; }
                    else if (xr)       { invert[i] = 1; px[0] = px[1] = px[2] = 0xFF; px[3] = 0xFF; }
                    /* else transparent */
                }

            // Outline the inverted pixels so they read on light and dark alike.
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * w + x;
                    if (s.bgra[i * 4 + 3]) continue;
                    bool adjacent = false;
                    for (int dy = -1; dy <= 1 && !adjacent; ++dy)
                        for (int dx = -1; dx <= 1 && !adjacent; ++dx) {
                            const int nx = x + dx, ny = y + dy;
                            if (nx >= 0 && ny >= 0 && nx < w && ny < h &&
                                invert[static_cast<std::size_t>(ny) * w + nx])
                                adjacent = true;
                        }
                    if (adjacent) { std::uint8_t* px = &s.bgra[i * 4];
                                px[0] = px[1] = px[2] = 0x00; px[3] = 0xFF; }
                }
            ok = true;
        }
    }

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    return ok && s.width && s.height;
}

// Halve the image (nearest pixel) until it fits a message. Only accessibility
// cursor sizes ever need this; the hotspot is scaled with it.
void fit(CursorShape& s, std::uint32_t max_bytes)
{
    while (s.width > 1 && s.height > 1 &&
           static_cast<std::uint64_t>(s.width) * s.height * 4 > max_bytes) {
        const std::uint32_t w = s.width / 2, h = s.height / 2;
        std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x)
                std::copy_n(&s.bgra[(static_cast<std::size_t>(y * 2) * s.width + x * 2) * 4],
                            4, &out[(static_cast<std::size_t>(y) * w + x) * 4]);
        s.bgra = std::move(out);
        s.width = w; s.height = h;
        s.hotspot_x /= 2; s.hotspot_y /= 2;
    }
}

}  // namespace

CursorWatcher::~CursorWatcher() { stop(); }

void CursorWatcher::start(Callback cb, std::uint32_t max_bytes)
{
    stop();
    cb_ = std::move(cb);
    max_bytes_ = max_bytes;
    stop_ = false;
    force_ = true;
    thread_ = std::thread([this] { loop(); });
}

void CursorWatcher::stop()
{
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void CursorWatcher::loop()
{
    HCURSOR last = nullptr;
    bool    last_visible = false;

    // 60 Hz: shape changes (arrow to I-beam on entering a text box) are rare
    // and the check is one call, so this costs nothing while making a new shape
    // appear within a frame of the guest choosing it.
    while (!stop_) {
        CURSORINFO ci{};
        ci.cbSize = sizeof ci;
        if (GetCursorInfo(&ci)) {
            const bool visible = (ci.flags & CURSOR_SHOWING) && ci.hCursor;
            const bool force = force_.exchange(false);
            if (force || visible != last_visible || (visible && ci.hCursor != last)) {
                CursorShape s;
                s.visible = visible;
                if (visible && cursor_image(ci.hCursor, s)) {
                    fit(s, max_bytes_);
                } else if (visible) {
                    // Unreadable shape: say visible with no image, and the host
                    // keeps whatever it had.
                    s.bgra.clear(); s.width = s.height = 0;
                }
                cb_(s);
                last = ci.hCursor;
                last_visible = visible;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

}  // namespace vypr
