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

#include <algorithm>
#include <vector>

#include "vypr_proto.h"

/*
 * The guest's monitors, in a stable order.
 *
 * Immersive mode pairs host output k with guest monitor k, so both ends have to
 * agree on what "monitor k" is. Sorting by (left, top) gives an order that does
 * not depend on the order Windows happens to enumerate adapters in, and matches
 * the left-to-right way the launcher sorts the host's outputs.
 */
struct vypr_monitor { HMONITOR handle; RECT rect; };

inline std::vector<vypr_monitor> vypr_list_monitors() {
    std::vector<vypr_monitor> out;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFO mi{}; mi.cbSize = sizeof mi;
            if (GetMonitorInfoW(h, &mi))
                reinterpret_cast<std::vector<vypr_monitor>*>(lp)
                    ->push_back({ h, mi.rcMonitor });
            return TRUE;
        }, reinterpret_cast<LPARAM>(&out));
    std::sort(out.begin(), out.end(), [](const vypr_monitor& a, const vypr_monitor& b) {
        if (a.rect.left != b.rect.left) return a.rect.left < b.rect.left;
        return a.rect.top < b.rect.top;
    });
    return out;
}

inline RECT vypr_primary_monitor_rect() {
    if (HMONITOR m = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY)) {
        MONITORINFO mi{}; mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(m, &mi)) return mi.rcMonitor;
    }
    return RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
}

/* The HMONITOR a desktop window id names, or null if it names none. A
 * per-monitor id past the end of the current monitor list (a monitor was
 * unplugged) also returns null; the primary id always resolves. */
inline HMONITOR vypr_desktop_monitor_handle(uint64_t id) {
    const int idx = vypr_desktop_monitor_index(id);
    if (idx >= 0) {
        const auto mons = vypr_list_monitors();
        if (idx < (int)mons.size()) return mons[idx].handle;
        return nullptr;
    }
    if (id == VYPR_DESKTOP_WINDOW_ID)
        return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    return nullptr;
}

/* The screen rect a desktop window id covers. Pointer offsets for a monitor
 * stream are relative to this. Falls back to the primary for the legacy id. */
inline RECT vypr_desktop_monitor_rect(uint64_t id) {
    const int idx = vypr_desktop_monitor_index(id);
    if (idx >= 0) {
        const auto mons = vypr_list_monitors();
        if (idx < (int)mons.size()) return mons[idx].rect;
    }
    return vypr_primary_monitor_rect();
}

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
