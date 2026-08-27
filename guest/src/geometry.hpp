// The one rectangle that matters: what the guest actually captures.
//
// Three rectangles describe a window and they are all different:
//
//   GetWindowRect            includes the invisible resize border Windows 10
//                            adds outside the visible frame
//   GetClientRect            excludes the title bar and menu bar, which are
//                            plainly there in the captured image
//   DWMWA_EXTENDED_FRAME_BOUNDS   what Windows.Graphics.Capture hands back
//
// Measured on Notepad at 150% scaling: window 2085x1053 at 152,152, client
// 2063x967 at 163,227, DWM frame 2065x1043 at 162,152 - and the published
// frame was 2065x1043. So the DWM frame is the space the pixels live in, and
// therefore the only space input may be expressed in.
//
// Reporting the client rect instead puts the origin 75px too low, which is the
// height of the title bar plus menu bar: every click lands that far down, and
// menu bar clicks land in the content area instead of on the menu.
#pragma once

#include <windows.h>
#include <dwmapi.h>

inline RECT vypr_capture_rect(HWND hwnd) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) ||
        r.right <= r.left || r.bottom <= r.top) {
        GetWindowRect(hwnd, &r);
    }
    return r;
}

/*
 * The part worth showing: the app's client area in screen coordinates.
 *
 * The captured frame includes Windows' own title bar and border, which on a
 * Linux desktop means a Windows title bar drawn inside a KDE one - two sets of
 * decorations for one window, and the host's buttons do nothing to the guest.
 * Cropping to the client area leaves the compositor to decorate it like any
 * native window: its own title bar to drag, its own close/minimise/maximise.
 *
 * Falls back to the captured frame for windows with no separate client area,
 * such as menus and fullscreen surfaces.
 */
inline RECT vypr_content_rect(HWND hwnd) {
    RECT client{};
    POINT origin{ 0, 0 };
    if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin))
        return vypr_capture_rect(hwnd);

    const LONG w = client.right - client.left;
    const LONG h = client.bottom - client.top;
    if (w < 8 || h < 8) return vypr_capture_rect(hwnd);

    return RECT{ origin.x, origin.y, origin.x + w, origin.y + h };
}
