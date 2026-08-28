#include "clipboard.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace vypr {

namespace {

std::string to_utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((std::size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring to_wide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out((std::size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

/*
 * The clipboard is a shared resource and any process may hold it. Opening it
 * fails while somebody else has it, which happens constantly on a busy desktop,
 * so every use retries briefly rather than giving up on the first refusal.
 */
bool open_clipboard(HWND owner)
{
    for (int i = 0; i < 10; i++) {
        if (OpenClipboard(owner)) return true;
        Sleep(20);
    }
    return false;
}

}  // namespace

struct Clipboard::Impl {
    std::thread                              thread;
    std::atomic<bool>                        stop{false};
    std::atomic<DWORD>                       tid{0};
    HWND                                     hwnd = nullptr;
    std::function<void(const std::string&)>  on_text;

    /* What we last sent or received, so an echo is recognised and dropped. */
    std::mutex  seen_lock;
    std::string seen;

    void run();
    void read_and_report();
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l);
};

LRESULT CALLBACK Clipboard::Impl::proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLIPBOARDUPDATE) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (self) self->read_and_report();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void Clipboard::Impl::read_and_report()
{
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return;
    if (!open_clipboard(hwnd)) return;

    std::string utf8;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            utf8 = to_utf8(std::wstring(p));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();

    if (utf8.empty()) return;

    {
        std::lock_guard<std::mutex> guard(seen_lock);
        if (utf8 == seen) return;    /* this is our own echo */
        seen = utf8;
    }
    if (on_text) on_text(utf8);
}

void Clipboard::Impl::run()
{
    tid = GetCurrentThreadId();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VyprClipboard";
    RegisterClassExW(&wc);

    /* HWND_MESSAGE: no desktop presence, just somewhere for messages to land. */
    hwnd = CreateWindowExW(0, L"VyprClipboard", L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "vypr: clipboard window failed (%lu)\n", GetLastError());
        return;
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    if (!AddClipboardFormatListener(hwnd))
        std::fprintf(stderr, "vypr: clipboard listener failed (%lu)\n", GetLastError());
    else
        std::fprintf(stderr, "vypr: clipboard sharing on\n");

    MSG msg;
    while (!stop.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RemoveClipboardFormatListener(hwnd);
    DestroyWindow(hwnd);
    hwnd = nullptr;
}

void Clipboard::set_text(const std::string& utf8)
{
    if (utf8.empty()) return;
    {
        std::lock_guard<std::mutex> guard(impl_->seen_lock);
        if (utf8 == impl_->seen) return;
        impl_->seen = utf8;          /* recorded before writing, so the update
                                        this causes is recognised as ours */
    }

    const std::wstring w = to_wide(utf8);
    if (!open_clipboard(impl_->hwnd)) return;

    EmptyClipboard();
    const std::size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    if (HANDLE h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (auto* dst = static_cast<wchar_t*>(GlobalLock(h))) {
            memcpy(dst, w.c_str(), bytes);
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

Clipboard::Clipboard() : impl_(std::make_unique<Impl>()) {}
Clipboard::~Clipboard() { stop(); }

bool Clipboard::start(std::function<void(const std::string&)> on_text)
{
    impl_->on_text = std::move(on_text);
    impl_->thread  = std::thread([this] { impl_->run(); });
    return true;
}

void Clipboard::stop()
{
    if (!impl_->thread.joinable()) return;
    impl_->stop = true;
    if (DWORD t = impl_->tid.load()) PostThreadMessageW(t, WM_NULL, 0, 0);
    impl_->thread.join();
}

}  // namespace vypr
