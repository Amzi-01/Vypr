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

inline RECT sash_capture_rect(HWND hwnd) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) ||
        r.right <= r.left || r.bottom <= r.top) {
        GetWindowRect(hwnd, &r);
    }
    return r;
}
