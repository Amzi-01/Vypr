#include "input.hpp"

#include <windows.h>

#include "geometry.hpp"

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

}  // namespace

void inject_pointer(const sash_msg_pointer& msg) {
    HWND hwnd = to_hwnd(msg.window_id);
    if (!IsWindow(hwnd)) return;

    std::vector<INPUT> batch;

    if (msg.flags & SASH_PTR_RELATIVE) {
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
