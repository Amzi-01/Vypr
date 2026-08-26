#include "windows_list.hpp"

#include <windows.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")

namespace sash {

namespace {

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

bool is_presentable(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr && GetWindowTextLengthW(hwnd) == 0) return false;

    // Cloaked windows are the trap here: UWP apps keep invisible-but-visible
    // HWNDs around, and Windows reports them as visible. DWM knows better.
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
        && cloaked)
        return false;

    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_NOREDIRECTIONBITMAP) return false;  // nothing for WGC to capture

    RECT r{};
    if (!GetClientRect(hwnd, &r)) return false;
    if (r.right - r.left < 8 || r.bottom - r.top < 8) return false;

    return true;
}

BOOL CALLBACK collect(HWND hwnd, LPARAM param) {
    auto* out = reinterpret_cast<std::vector<WindowInfo>*>(param);
    WindowInfo info;
    if (describe_window(hwnd, &info)) out->push_back(std::move(info));
    return TRUE;
}

struct PidFilter {
    std::uint32_t pid;
    std::vector<WindowInfo>* out;
};

BOOL CALLBACK collect_pid(HWND hwnd, LPARAM param) {
    auto* f = reinterpret_cast<PidFilter*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != f->pid) return TRUE;
    WindowInfo info;
    if (describe_window(hwnd, &info)) f->out->push_back(std::move(info));
    return TRUE;
}

}  // namespace

bool describe_window(void* hwnd_raw, WindowInfo* out) {
    HWND hwnd = static_cast<HWND>(hwnd_raw);
    if (!IsWindow(hwnd) || !is_presentable(hwnd)) return false;

    RECT client{};
    GetClientRect(hwnd, &client);
    POINT origin{ 0, 0 };
    ClientToScreen(hwnd, &origin);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR ex    = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    auto& d = out->desc;
    d = {};
    d.window_id = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd));
    d.owner_id  = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(GetWindow(hwnd, GW_OWNER)));
    d.x      = origin.x;
    d.y      = origin.y;
    d.width  = static_cast<std::uint32_t>(client.right - client.left);
    d.height = static_cast<std::uint32_t>(client.bottom - client.top);
    d.pid    = pid;

    // Per-window DPI, not the system's. A guest with mixed-DPI monitors reports
    // a different client-area size than the captured surface otherwise.
    d.dpi = GetDpiForWindow(hwnd);
    if (d.dpi == 0) d.dpi = 96;

    if (ex & WS_EX_TOOLWINDOW)      d.flags |= SASH_WIN_TOOL_WINDOW;
    if (style & WS_POPUP)           d.flags |= SASH_WIN_POPUP;
    if (style & WS_THICKFRAME)      d.flags |= SASH_WIN_RESIZABLE;
    if (IsIconic(hwnd))             d.flags |= SASH_WIN_MINIMIZED;

    const int len = GetWindowTextLengthW(hwnd);
    if (len > 0) {
        std::wstring w(static_cast<std::size_t>(len) + 1, L'\0');
        const int got = GetWindowTextW(hwnd, w.data(), len + 1);
        w.resize(static_cast<std::size_t>(got > 0 ? got : 0));
        out->title = to_utf8(w);
    } else {
        out->title.clear();
    }
    return true;
}

std::vector<WindowInfo> list_windows() {
    std::vector<WindowInfo> out;
    EnumWindows(collect, reinterpret_cast<LPARAM>(&out));
    return out;
}

std::vector<WindowInfo> windows_for_pid(std::uint32_t pid) {
    std::vector<WindowInfo> out;
    PidFilter f{ pid, &out };
    EnumWindows(collect_pid, reinterpret_cast<LPARAM>(&f));
    return out;
}

}  // namespace sash
