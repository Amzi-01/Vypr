#include "input.hpp"

#include <windows.h>

#include "geometry.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace vypr {

namespace {

/*
 * The captured rectangle, cached.
 *
 * DwmGetWindowAttribute is a cross-process call, and looking it up on every
 * pointer event means a thousand of them a second from a high-polling mouse -
 * on the same CPU that is capturing frames and running the game. A window's
 * geometry does not change between two mouse reports; a tenth of a second is
 * far finer than any window move a person can make.
 */
RECT          g_cap_cache{};
HWND          g_cap_hwnd = nullptr;
ULONGLONG     g_cap_at = 0;

/*
 * The whole screen is offered under a handle that is not a window - see
 * VYPR_DESKTOP_WINDOW_ID - so every window call in this file has to be skipped
 * for it. Its captured rectangle is the monitor's, which is what pointer
 * offsets are relative to.
 */
bool whole_desktop(std::uint64_t window_id) {
    return window_id == VYPR_DESKTOP_WINDOW_ID;
}

RECT primary_monitor_rect() {
    if (HMONITOR mon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY)) {
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(mon, &mi)) return mi.rcMonitor;
    }
    return RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
}

RECT cached_capture_rect(HWND hwnd) {
    const ULONGLONG now = GetTickCount64();
    if (hwnd != g_cap_hwnd || now - g_cap_at > 100) {
        g_cap_cache = whole_desktop(reinterpret_cast<std::uintptr_t>(hwnd))
                          ? primary_monitor_rect()
                          : vypr_capture_rect(hwnd);
        g_cap_hwnd  = hwnd;
        g_cap_at    = now;
    }
    return g_cap_cache;
}

bool trace_input() {
    static const bool on = std::getenv("VYPR_TRACE") != nullptr;
    return on;
}

HWND to_hwnd(std::uint64_t id) {
    return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
}

// SendInput's absolute coordinates are normalised across the whole virtual
// desktop, not one monitor - getting this wrong lands the pointer on the wrong
// screen in a multi-monitor guest.
//
// `cx`/`cy` are offsets inside the captured surface, which is the DWM extended
// frame - the same rectangle the host is displaying. Treating them as client
// coordinates would shift every click down by the title and menu bar.
bool to_absolute(HWND hwnd, int cx, int cy, LONG* out_x, LONG* out_y) {
    const RECT cap = cached_capture_rect(hwnd);
    POINT pt{ cap.left + cx, cap.top + cy };

    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return false;

    *out_x = static_cast<LONG>((pt.x - vx) * 65535.0 / vw);
    *out_y = static_cast<LONG>((pt.y - vy) * 65535.0 / vh);
    return true;
}

std::uint32_t g_buttons = 0;   // last known button state, to derive edges

/*
 * Input goes to whatever is in the foreground, not to whatever we aim at, so a
 * window that quietly loses focus in the guest silently swallows everything.
 * Observed with the Windows Search overlay taking focus: the game kept
 * rendering while every keystroke went to the search box.
 *
 * Asserting focus only when the host window gains focus is not enough - nothing
 * takes it back afterwards. Check per event instead; once it holds, the check
 * is a single call and the assignment never runs.
 */
void ensure_foreground(HWND hwnd) {
    if (!hwnd || GetForegroundWindow() == hwnd) return;

    const DWORD self = GetCurrentThreadId();
    const DWORD fore = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    if (fore && fore != self) AttachThreadInput(self, fore, TRUE);
    SetForegroundWindow(hwnd);
    if (fore && fore != self) AttachThreadInput(self, fore, FALSE);
}

int  g_saved_accel[3] = { 0, 0, 0 };
bool g_accel_saved = false;


}  // namespace

void set_window_minimized(std::uint64_t window_id, bool minimized) {
    HWND hwnd = to_hwnd(window_id);
    if (!IsWindow(hwnd)) return;

    // Only when it differs: applying a state the window is already in would
    // report a change back to the host and start the two bouncing it around.
    if (minimized == (IsIconic(hwnd) != FALSE)) return;

    ShowWindow(hwnd, minimized ? SW_MINIMIZE : SW_RESTORE);
}

void suspend_pointer_acceleration() {
    if (g_accel_saved) return;
    if (!SystemParametersInfoW(SPI_GETMOUSE, 0, g_saved_accel, 0)) return;

    // {threshold1, threshold2, acceleration}; all zero is a linear response.
    if (g_saved_accel[2] == 0) return;   // already linear, nothing to restore

    g_accel_saved = true;
    int linear[3] = { 0, 0, 0 };
    SystemParametersInfoW(SPI_SETMOUSE, 0, linear, SPIF_SENDCHANGE);
    std::fprintf(stderr, "vypr: pointer acceleration suspended (was %d/%d/%d)\n",
                 g_saved_accel[0], g_saved_accel[1], g_saved_accel[2]);
}

void restore_pointer_acceleration() {
    if (!g_accel_saved) return;
    SystemParametersInfoW(SPI_SETMOUSE, 0, g_saved_accel, SPIF_SENDCHANGE);
    g_accel_saved = false;
    std::fprintf(stderr, "vypr: pointer acceleration restored\n");
}

void inject_pointer(const vypr_msg_pointer& msg) {
    HWND hwnd = to_hwnd(msg.window_id);
    const bool desktop = whole_desktop(msg.window_id);
    if (!desktop && !IsWindow(hwnd)) return;

    /*
     * Only a click takes focus; passive motion must not.
     *
     * Asserting foreground on every motion event means moving the pointer
     * across one streamed window drags focus off another - and with two FiveM
     * windows streaming (launcher and game) they fight, the game loses
     * foreground, the pointer lock releases, and input falls back to absolute
     * positioning which cannot work here at all. Focus-follows-click is what
     * every desktop does, for this reason.
     */
    /*
     * Nothing to raise when the target is the screen itself: an absolute click
     * activates whatever window Windows finds under the cursor, which is
     * exactly the behaviour wanted there.
     */
    if (!desktop && (msg.buttons & ~g_buttons)) ensure_foreground(hwnd);

    // Periodic summary of what is being injected. Off unless VYPR_TRACE is
    // set: it costs GetCursorPos and GetClipCursor on every single event, which
    // is thousands of calls a second while the mouse is moving.
    if (trace_input()) {
        static unsigned n = 0;
        static double sum_x = 0, sum_y = 0;
        static unsigned rel = 0, abs_ = 0;
        (msg.flags & VYPR_PTR_RELATIVE) ? rel++ : abs_++;
        sum_x += std::abs((double)msg.x);
        sum_y += std::abs((double)msg.y);
        if (++n >= 120) {
            POINT cur{};
            GetCursorPos(&cur);
            RECT clip{};
            GetClipCursor(&clip);
            CURSORINFO ci{};
            ci.cbSize = sizeof(ci);
            const bool showing = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
            std::fprintf(stderr,
                "vypr: pointer %u rel / %u abs, mean |dx|=%.1f |dy|=%.1f, "
                "cursor %ld,%ld visible=%d, clip %ldx%ld at %ld,%ld\n",
                rel, abs_, sum_x / n, sum_y / n, cur.x, cur.y, showing ? 1 : 0,
                clip.right - clip.left, clip.bottom - clip.top, clip.left, clip.top);
            n = 0; sum_x = sum_y = 0; rel = abs_ = 0;
        }
    }

    std::vector<INPUT> batch;

    if (msg.flags & VYPR_PTR_RELATIVE) {
        /*
         * Keep the cursor away from the screen edges.
         *
         * Relative deltas accumulate into the cursor position, so without this
         * the cursor walks to an edge and stops - after which every further
         * delta in that direction is silently discarded and the view will not
         * turn any further. Observed jammed at 3839,1863 on a 3840x2160
         * screen, which is exactly what it looks like when a game "stops
         * responding" to the mouse.
         *
         * A game that captures the mouse re-centres it itself every frame, so
         * this is a no-op there. It matters precisely when the app does not,
         * which is the case that was broken.
         */
        /*
         * The cursor is left where it is.
         *
         * Warping it back to centre was meant to stop relative deltas jamming
         * it against a screen edge, but what the user sees is the pointer
         * snapping to the middle of the window while they are using it. A
         * visible teleport every time the pointer wanders is worse than the
         * edge case it was guarding against, and an app that genuinely needs
         * the cursor centred re-centres it itself.
         */
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = msg.x;
        in.mi.dy = msg.y;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        batch.push_back(in);
    } else {
        LONG ax = 0, ay = 0;
        if (to_absolute(hwnd, msg.x, msg.y, &ax, &ay)) {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dx = ax;
            in.mi.dy = ay;
            in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            batch.push_back(in);
        }
    }

    // Only the transitions: resending a held button every motion event makes
    // drag operations misbehave in most apps.
    const std::uint32_t changed = g_buttons ^ msg.buttons;
    struct { std::uint32_t bit; DWORD down; DWORD up; } map[] = {
        { 1u << 0, MOUSEEVENTF_LEFTDOWN,   MOUSEEVENTF_LEFTUP   },
        { 1u << 1, MOUSEEVENTF_RIGHTDOWN,  MOUSEEVENTF_RIGHTUP  },
        { 1u << 2, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP },
    };
    for (const auto& m : map) {
        if (!(changed & m.bit)) continue;
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = (msg.buttons & m.bit) ? m.down : m.up;
        batch.push_back(in);
    }
    g_buttons = msg.buttons;

    if (msg.wheel) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.mouseData = static_cast<DWORD>(msg.wheel);
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        batch.push_back(in);
    }
    if (msg.hwheel) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.mouseData = static_cast<DWORD>(msg.hwheel);
        in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        batch.push_back(in);
    }

    if (!batch.empty())
        SendInput(static_cast<UINT>(batch.size()), batch.data(), sizeof(INPUT));
}

void inject_key(const vypr_msg_key& msg) {
    // Keys go to whatever the guest has focused when the target is the screen.
    if (!whole_desktop(msg.window_id)) ensure_foreground(to_hwnd(msg.window_id));

    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = static_cast<WORD>(msg.scancode & 0xff);
    in.ki.dwFlags = KEYEVENTF_SCANCODE;
    // Extended keys (arrows, right ctrl/alt, numpad enter) carry 0xE0 in the
    // high byte and need the flag, or they arrive as their numpad twins.
    if ((msg.scancode & 0xff00) == 0xe000)
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!msg.down)
        in.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}

void inject_text(std::uint64_t, const char* utf8, std::uint32_t bytes) {
    if (!utf8 || !bytes) return;

    const int wide = MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(bytes), nullptr, 0);
    if (wide <= 0) return;
    std::vector<wchar_t> buf(static_cast<std::size_t>(wide));
    MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(bytes), buf.data(), wide);

    // Unicode injection rather than scancodes: this path exists for text the
    // host's keymap cannot express as a PS/2 scancode at all.
    std::vector<INPUT> batch;
    batch.reserve(buf.size() * 2);
    for (wchar_t ch : buf) {
        INPUT down{}, up{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = ch;
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        up = down;
        up.ki.dwFlags |= KEYEVENTF_KEYUP;
        batch.push_back(down);
        batch.push_back(up);
    }
    SendInput(static_cast<UINT>(batch.size()), batch.data(), sizeof(INPUT));
}

bool surface_to_screen(std::uint64_t window_id, int cx, int cy, long* sx, long* sy) {
    HWND hwnd = to_hwnd(window_id);
    if (!whole_desktop(window_id) && !IsWindow(hwnd)) return false;

    const RECT cap = cached_capture_rect(hwnd);
    if (sx) *sx = cap.left + cx;
    if (sy) *sy = cap.top  + cy;
    return true;
}

void nudge_onscreen(std::uint64_t window_id) {
    if (whole_desktop(window_id)) return;          // it is the screen
    HWND hwnd = to_hwnd(window_id);
    if (!IsWindow(hwnd) || IsIconic(hwnd)) return;

    const RECT cap = cached_capture_rect(hwnd);
    const LONG w = cap.right - cap.left, h = cap.bottom - cap.top;
    if (w <= 0 || h <= 0) return;

    const LONG vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const LONG vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return;

    /* Bigger than the screen is a fullscreen window, not a misplaced one. */
    if (w >= vw && h >= vh) return;

    LONG x = cap.left, y = cap.top;
    if (x < vx)            x = vx;
    if (y < vy)            y = vy;
    if (x + w > vx + vw)   x = vx + vw - w;
    if (y + h > vy + vh)   y = vy + vh - h;
    if (x == cap.left && y == cap.top) return;     // already all on screen

    /* SetWindowPos moves the window, and the window is larger than the
     * captured frame by the invisible resize border - so the difference is
     * carried across, exactly as resize_window does. */
    RECT outer{};
    GetWindowRect(hwnd, &outer);
    const LONG pad_x = cap.left - outer.left;
    const LONG pad_y = cap.top  - outer.top;

    SetWindowPos(hwnd, nullptr, static_cast<int>(x - pad_x), static_cast<int>(y - pad_y),
                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    std::fprintf(stderr, "vypr: moved a window on screen, %ld,%ld -> %ld,%ld\n",
                 cap.left, cap.top, x, y);

    /* The cached rectangle described where it used to be. */
    g_cap_hwnd = nullptr;
}

void focus_window(std::uint64_t window_id) {
    HWND hwnd = to_hwnd(window_id);
    if (!IsWindow(hwnd)) return;
    // SetForegroundWindow is refused for a process without foreground rights.
    // Attaching to the current foreground thread's input queue first is the
    // standard way around it and is why the agent must not run as a service.
    const DWORD self = GetCurrentThreadId();
    const DWORD fore = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    if (fore && fore != self) AttachThreadInput(self, fore, TRUE);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    if (fore && fore != self) AttachThreadInput(self, fore, FALSE);
}

void close_window(std::uint64_t window_id) {
    HWND hwnd = to_hwnd(window_id);
    if (IsWindow(hwnd)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

void resize_window(const vypr_msg_resize& msg) {
    HWND hwnd = to_hwnd(msg.window_id);
    if (!IsWindow(hwnd)) return;

    // The host asks in captured-surface terms, which is the DWM frame. The
    // window itself is larger by the invisible resize border, so carry that
    // difference across rather than resizing to the wrong thing.
    RECT outer{};
    GetWindowRect(hwnd, &outer);
    const RECT cap = vypr_capture_rect(hwnd);

    const LONG pad_w = (outer.right - outer.left) - (cap.right - cap.left);
    const LONG pad_h = (outer.bottom - outer.top) - (cap.bottom - cap.top);

    SetWindowPos(hwnd, nullptr, 0, 0,
                 static_cast<int>(msg.width) + pad_w,
                 static_cast<int>(msg.height) + pad_h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace vypr
