#include "input.hpp"

#include <windows.h>

#include "geometry.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace sash {

namespace {

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
    const RECT cap = sash_capture_rect(hwnd);
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

void suspend_pointer_acceleration() {
    if (g_accel_saved) return;
    if (!SystemParametersInfoW(SPI_GETMOUSE, 0, g_saved_accel, 0)) return;

    // {threshold1, threshold2, acceleration}; all zero is a linear response.
    if (g_saved_accel[2] == 0) return;   // already linear, nothing to restore

    g_accel_saved = true;
    int linear[3] = { 0, 0, 0 };
    SystemParametersInfoW(SPI_SETMOUSE, 0, linear, SPIF_SENDCHANGE);
    std::fprintf(stderr, "sash: pointer acceleration suspended (was %d/%d/%d)\n",
                 g_saved_accel[0], g_saved_accel[1], g_saved_accel[2]);
}

void restore_pointer_acceleration() {
    if (!g_accel_saved) return;
    SystemParametersInfoW(SPI_SETMOUSE, 0, g_saved_accel, SPIF_SENDCHANGE);
    g_accel_saved = false;
    std::fprintf(stderr, "sash: pointer acceleration restored\n");
}

void inject_pointer(const sash_msg_pointer& msg) {
    HWND hwnd = to_hwnd(msg.window_id);
    if (!IsWindow(hwnd)) return;

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
    if (msg.buttons & ~g_buttons) ensure_foreground(hwnd);

    // Periodic summary of what is actually being injected: mode, typical
    // magnitude, and where the guest cursor ended up. Enough to tell "the host
    // is sending nonsense" apart from "the game is ignoring good input".
    {
        static unsigned n = 0;
        static double sum_x = 0, sum_y = 0;
        static unsigned rel = 0, abs_ = 0;
        (msg.flags & SASH_PTR_RELATIVE) ? rel++ : abs_++;
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
                "sash: pointer %u rel / %u abs, mean |dx|=%.1f |dy|=%.1f, "
                "cursor %ld,%ld visible=%d, clip %ldx%ld at %ld,%ld\n",
                rel, abs_, sum_x / n, sum_y / n, cur.x, cur.y, showing ? 1 : 0,
                clip.right - clip.left, clip.bottom - clip.top, clip.left, clip.top);
            n = 0; sum_x = sum_y = 0; rel = abs_ = 0;
        }
    }

    std::vector<INPUT> batch;

    if (msg.flags & SASH_PTR_RELATIVE) {
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
         * Only when the app is NOT managing the cursor itself.
         *
         * An app that confines the cursor - ClipCursor to something smaller
         * than the virtual desktop - is doing warp-based mouselook: it re-
         * centres every frame and reads the delta from centre. Warping as well
         * injects a jump it cannot distinguish from real movement, and the view
         * snaps to a corner. That is a worse failure than the edge-jamming this
         * was meant to fix, and it only ever applies to apps that leave the
         * cursor alone.
         */
        RECT clip{};
        bool app_owns_cursor = false;
        if (GetClipCursor(&clip)) {
            const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            app_owns_cursor = (clip.right - clip.left) < vw ||
                              (clip.bottom - clip.top) < vh;
        }

        RECT cap = sash_capture_rect(hwnd);
        POINT cur{};
        if (!app_owns_cursor && GetCursorPos(&cur)) {
            const LONG margin_x = (cap.right - cap.left) / 8;
            const LONG margin_y = (cap.bottom - cap.top) / 8;
            if (cur.x < cap.left + margin_x || cur.x > cap.right - margin_x ||
                cur.y < cap.top + margin_y || cur.y > cap.bottom - margin_y) {
                SetCursorPos((cap.left + cap.right) / 2,
                             (cap.top + cap.bottom) / 2);
            }
        }

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

void inject_key(const sash_msg_key& msg) {
    ensure_foreground(to_hwnd(msg.window_id));

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

void resize_window(const sash_msg_resize& msg) {
    HWND hwnd = to_hwnd(msg.window_id);
    if (!IsWindow(hwnd)) return;

    // The host asks in captured-surface terms, which is the DWM frame. The
    // window itself is larger by the invisible resize border, so carry that
    // difference across rather than resizing to the wrong thing.
    RECT outer{};
    GetWindowRect(hwnd, &outer);
    const RECT cap = sash_capture_rect(hwnd);

    const LONG pad_w = (outer.right - outer.left) - (cap.right - cap.left);
    const LONG pad_h = (outer.bottom - outer.top) - (cap.bottom - cap.top);

    SetWindowPos(hwnd, nullptr, 0, 0,
                 static_cast<int>(msg.width) + pad_w,
                 static_cast<int>(msg.height) + pad_h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace sash
