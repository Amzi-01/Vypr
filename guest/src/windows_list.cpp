#include "windows_list.hpp"

#include <windows.h>
#include <dwmapi.h>

#include "geometry.hpp"

#include <cstdio>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")

namespace vypr {

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

// A menu, dropdown or tooltip: owned or WS_POPUP, no caption, and drawing real
// content. These have to be told apart from the invisible helper windows that
// every Win32 app leaves lying around, which look similar from the outside.
bool is_popup_surface(HWND hwnd) {
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR ex    = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if (!(style & WS_POPUP)) return false;

    // WS_CAPTION is WS_BORDER|WS_DLGFRAME, so a plain bitwise test is true for
    // anything merely bordered - which every menu is. It has to match in full,
    // or no menu is ever recognised as a popup.
    if ((style & WS_CAPTION) == WS_CAPTION) return false;

    wchar_t cls[64] = {0};
    GetClassNameW(hwnd, cls, 64);

    // SysShadow is the drop shadow Windows draws behind a menu - a separate
    // top-level window that looks exactly like a popup from the outside and has
    // nothing to capture. WGC rejects it with E_INVALIDARG. The host compositor
    // draws its own shadows anyway.
    if (!wcscmp(cls, L"SysShadow")) return false;

    if (!wcscmp(cls, L"#32768"))           return true;   // menus
    if (!wcscmp(cls, L"tooltips_class32")) return true;
    if (!wcsncmp(cls, L"Net UI Tool", 11)) return true;   // ribbon dropdowns

    // Anything else has to look like real content rather than a helper: owned,
    // and big enough to be worth a window. A blanket accept on TOOLWINDOW or
    // TOPMOST pulls in every invisible helper an app leaves lying around.
    if ((ex & (WS_EX_TOOLWINDOW | WS_EX_TOPMOST)) && GetWindow(hwnd, GW_OWNER))
        return true;

    return false;
}

bool is_presentable(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;

    // The menu shadow has no owner and no title, so the checks below let it
    // through on its own. It is pure decoration and WGC will not capture it.
    wchar_t cls[64] = {0};
    GetClassNameW(hwnd, cls, 64);
    if (!wcscmp(cls, L"SysShadow")) return false;

    // An owned, untitled window is usually a helper nobody should see - but it
    // is also exactly what a menu looks like. Keep the ones that are really
    // popup surfaces; a plain title check would drop every menu in Windows.
    if (GetWindow(hwnd, GW_OWNER) != nullptr && GetWindowTextLengthW(hwnd) == 0 &&
        !is_popup_surface(hwnd))
        return false;

    // Cloaked windows are the trap here: UWP apps keep invisible-but-visible
    // HWNDs around, and Windows reports them as visible. DWM knows better.
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
        && cloaked)
        return false;

    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_NOREDIRECTIONBITMAP) return false;  // nothing for WGC to capture

    const RECT cap = vypr_capture_rect(hwnd);
    if (cap.right - cap.left < 8 || cap.bottom - cap.top < 8) return false;

    return true;
}

/*
 * Which window a menu belongs to.
 *
 * Menu popups report no owner at all - GW_OWNER is null for class #32768 - so
 * there is nothing to follow upward. What is reliable is the thread: a menu is
 * created by whichever UI thread called TrackPopupMenu, which is the thread
 * that owns the window it dropped out of. So look for that thread's real
 * top-level window.
 */
struct OwnerSearch {
    HWND skip;
    HWND found;
};

BOOL CALLBACK pick_owner(HWND hwnd, LPARAM param) {
    auto* f = reinterpret_cast<OwnerSearch*>(param);
    if (hwnd == f->skip) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (is_popup_surface(hwnd)) return TRUE;      // another popup, not the owner
    if (GetWindowTextLengthW(hwnd) == 0) return TRUE;
    f->found = hwnd;
    return FALSE;
}

HWND owner_of_popup(HWND hwnd) {
    if (HWND owned = GetWindow(hwnd, GW_OWNER)) return owned;

    OwnerSearch f{ hwnd, nullptr };
    EnumThreadWindows(GetWindowThreadProcessId(hwnd, nullptr), pick_owner,
                      reinterpret_cast<LPARAM>(&f));
    return f.found;
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

    // The whole window including Windows' own title bar and border. The host
    // presents it undecorated, so that title bar is the window's title bar -
    // its buttons work because the input goes straight to Windows.
    const RECT cap = vypr_capture_rect(hwnd);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR ex    = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    auto& d = out->desc;
    d = {};
    d.window_id = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd));
    const bool popup = is_popup_surface(hwnd);
    d.owner_id  = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
        popup ? owner_of_popup(hwnd) : GetWindow(hwnd, GW_OWNER)));
    d.x      = cap.left;
    d.y      = cap.top;
    d.width  = static_cast<std::uint32_t>(cap.right - cap.left);
    d.height = static_cast<std::uint32_t>(cap.bottom - cap.top);
    d.pid    = pid;

    const RECT content = vypr_content_rect(hwnd);
    d.chrome_top = (content.top > cap.top)
                 ? static_cast<std::uint32_t>(content.top - cap.top) : 0;

    // Per-window DPI, not the system's. A guest with mixed-DPI monitors reports
    // a different client-area size than the captured surface otherwise.
    d.dpi = GetDpiForWindow(hwnd);
    if (d.dpi == 0) d.dpi = 96;

    if (ex & WS_EX_TOOLWINDOW)      d.flags |= VYPR_WIN_TOOL_WINDOW;
    if (popup)                      d.flags |= VYPR_WIN_POPUP;
    if (style & WS_THICKFRAME)      d.flags |= VYPR_WIN_RESIZABLE;
    if (IsIconic(hwnd))             d.flags |= VYPR_WIN_MINIMIZED;

    // Covers the whole desktop: treat it as a fullscreen app, which the host
    // uses to decide whether to start with the pointer captured.
    //
    // Measured on the window frame, not the reported content area - the latter
    // has the title bar and border cropped off it, so a genuinely fullscreen
    // window is always a little smaller than the desktop and would never match.
    {
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if ((LONG)d.width >= vw - 2 && (LONG)d.height >= vh - 2)
            d.flags |= VYPR_WIN_FULLSCREEN;
    }

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

namespace {
BOOL CALLBACK dump_one(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd)) return TRUE;

    wchar_t cls[96] = {0};
    GetClassNameW(hwnd, cls, 96);
    wchar_t title[128] = {0};
    GetWindowTextW(hwnd, title, 128);

    RECT r{};
    GetWindowRect(hwnd, &r);
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR ex    = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    int cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

    RECT cr{};
    GetClientRect(hwnd, &cr);
    POINT corigin{0, 0};
    ClientToScreen(hwnd, &corigin);

    // What WGC actually captures is the DWM extended frame, not GetWindowRect -
    // the latter includes the invisible resize border Windows 10 adds.
    RECT fr{};
    DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr));

    std::printf("hwnd=%p cls='%ls' title='%ls'\n"
                "    windowrect  %ldx%ld at %ld,%ld\n"
                "    clientrect  %ldx%ld at %ld,%ld (screen origin)\n"
                "    dwmframe    %ldx%ld at %ld,%ld\n"
                "    style=0x%08llX ex=0x%08llX owner=%p cloaked=%d "
                "popup=%d presentable=%d dpi=%u\n",
                (void*)hwnd, cls, title,
                r.right - r.left, r.bottom - r.top, r.left, r.top,
                cr.right - cr.left, cr.bottom - cr.top, corigin.x, corigin.y,
                fr.right - fr.left, fr.bottom - fr.top, fr.left, fr.top,
                (unsigned long long)style, (unsigned long long)ex,
                (void*)GetWindow(hwnd, GW_OWNER), cloaked,
                is_popup_surface(hwnd) ? 1 : 0, is_presentable(hwnd) ? 1 : 0,
                GetDpiForWindow(hwnd));
    return TRUE;
}
}  // namespace

void dump_all_windows() {
    std::printf("--- visible top-level windows ---\n");
    EnumWindows(dump_one, 0);
    std::fflush(stdout);
}

std::vector<WindowInfo> windows_for_pid(std::uint32_t pid) {
    std::vector<WindowInfo> out;
    PidFilter f{ pid, &out };
    EnumWindows(collect_pid, reinterpret_cast<LPARAM>(&f));
    return out;
}

}  // namespace vypr
