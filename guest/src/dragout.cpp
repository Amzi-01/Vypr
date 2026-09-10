#include "dragout.hpp"

#include <windows.h>   /* WIN32_LEAN_AND_MEAN comes from the build */
#include <ole2.h>
#include <shlobj.h>
#include <shellapi.h>

#include <cstdio>

namespace vypr {
namespace {

/*
 * The target the source process ends up talking to.
 *
 * It answers DROPEFFECT_NONE to everything. The point is to read what is
 * passing by, not to take it: the user is dragging onto something on the Linux
 * side, and the Windows application they dragged from should still see its
 * drag end wherever it was going to.
 */
class Target : public IDropTarget {
public:
    std::vector<std::wstring> files;
    bool seen = false;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override  { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        collect(data);
        seen = true;
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        collect(data);
        seen = true;
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }

private:
    LONG ref_ = 1;

    void collect(IDataObject* data) {
        if (!data) return;
        files.clear();
        if (from_hdrop(data)) return;
        from_shell_ids(data);
    }

    // The easy case, and what most applications offer.
    bool from_hdrop(IDataObject* data) {
        FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM med{};
        if (FAILED(data->GetData(&fmt, &med))) return false;

        bool got = false;
        if (HDROP drop = static_cast<HDROP>(GlobalLock(med.hGlobal))) {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; i++) {
                wchar_t path[MAX_PATH]{};
                if (DragQueryFileW(drop, i, path, MAX_PATH) && path[0]) {
                    files.emplace_back(path);
                    got = true;
                }
            }
            GlobalUnlock(med.hGlobal);
        }
        ReleaseStgMedium(&med);
        return got;
    }

    /*
     * The shell's own format, which is what Explorer offers when it has no
     * HDROP to give. A folder PIDL plus one per item; anything backed by a
     * real file resolves to a path, and anything that is not - the Recycle
     * Bin, a control panel entry - resolves to nothing and is skipped, which
     * is correct, because there is no file to hand the host.
     */
    bool from_shell_ids(IDataObject* data) {
        const UINT cf = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
        if (!cf) return false;

        FORMATETC fmt{ static_cast<CLIPFORMAT>(cf), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM med{};
        if (FAILED(data->GetData(&fmt, &med))) return false;

        bool got = false;
        if (auto* cida = static_cast<CIDA*>(GlobalLock(med.hGlobal))) {
            auto* base = reinterpret_cast<const BYTE*>(cida);
            auto folder = reinterpret_cast<PCIDLIST_ABSOLUTE>(base + cida->aoffset[0]);
            for (UINT i = 1; i <= cida->cidl; i++) {
                auto rel = reinterpret_cast<PCUIDLIST_RELATIVE>(base + cida->aoffset[i]);
                if (PIDLIST_ABSOLUTE full = ILCombine(folder, rel)) {
                    wchar_t path[MAX_PATH]{};
                    if (SHGetPathFromIDListW(full, path) && path[0]) {
                        files.emplace_back(path);
                        got = true;
                    }
                    CoTaskMemFree(full);
                }
            }
            GlobalUnlock(med.hGlobal);
        }
        ReleaseStgMedium(&med);
        return got;
    }
};

constexpr int kParkedX = -32000;   // far enough off any desktop to never be hit
constexpr int kParkedY = -32000;
constexpr int kSize    = 64;

LRESULT CALLBACK catcher_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcW(h, m, w, l);
}

void pump(DWORD ms)
{
    const DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (GetTickCount() >= end) return;
        Sleep(5);
    }
}

// A real one-pixel move. A drag loop coalesces zero-distance moves away and
// would never re-run WindowFromPoint, so the catcher would never be noticed.
void nudge(int dx)
{
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;   // relative
    in.mi.dx = dx;
    in.mi.dy = 0;
    SendInput(1, &in, sizeof in);
}

}  // namespace

DragOut::DragOut()
{
    // The agent's other COM users initialise per-thread; doing it again here is
    // harmless and means this works whichever thread ends up owning it.
    OleInitialize(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = catcher_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VyprDragCatcher";
    RegisterClassExW(&wc);   // benign if a previous instance already did

    HWND h = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        L"VyprDragCatcher", L"", WS_POPUP,
        kParkedX, kParkedY, kSize, kSize,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!h) {
        std::fprintf(stderr, "vypr: drag-out: CreateWindowEx failed (%lu)\n", GetLastError());
        return;
    }

    /*
     * Layered at alpha 1: present as far as hit testing is concerned, invisible
     * as far as the user is concerned. Deliberately not WS_EX_TRANSPARENT -
     * that is what the inbound path wants, so WindowFromPoint sees *through*
     * it. Here the whole point is to be found.
     */
    SetLayeredWindowAttributes(h, 0, 1, LWA_ALPHA);
    ShowWindow(h, SW_SHOWNOACTIVATE);

    auto* target = new Target();
    const HRESULT hr = RegisterDragDrop(h, target);
    if (FAILED(hr)) {
        std::fprintf(stderr, "vypr: drag-out: RegisterDragDrop failed (0x%08lx)\n", hr);
        target->Release();
        DestroyWindow(h);
        return;
    }

    hwnd_ = h;
    target_ = target;
}

DragOut::~DragOut()
{
    if (hwnd_) {
        RevokeDragDrop(static_cast<HWND>(hwnd_));
        DestroyWindow(static_cast<HWND>(hwnd_));
    }
    if (target_) static_cast<Target*>(target_)->Release();
}

std::vector<std::wstring> DragOut::probe()
{
    if (!hwnd_ || !target_) return {};

    auto* target = static_cast<Target*>(target_);
    auto  h = static_cast<HWND>(hwnd_);
    target->seen = false;
    target->files.clear();

    POINT p{};
    GetCursorPos(&p);
    SetWindowPos(h, HWND_TOPMOST, p.x - kSize / 2, p.y - kSize / 2, kSize, kSize,
                 SWP_NOACTIVATE);
    pump(20);

    /*
     * Give the source loop a few chances to notice us. Each nudge is a pixel
     * out and back, so the cursor finishes where it started and the drag is
     * not visibly disturbed. Bounded tightly: this runs on the agent's loop,
     * and every millisecond here is a millisecond of input not being handled.
     */
    for (int i = 0; i < 8 && !target->seen; i++) {
        nudge(i % 2 ? 1 : -1);
        pump(18);
        GetCursorPos(&p);
        SetWindowPos(h, HWND_TOPMOST, p.x - kSize / 2, p.y - kSize / 2, kSize, kSize,
                     SWP_NOACTIVATE);
    }

    // Out of the way again, so nothing else in the session lands on it.
    SetWindowPos(h, HWND_TOPMOST, kParkedX, kParkedY, kSize, kSize, SWP_NOACTIVATE);

    return target->seen ? target->files : std::vector<std::wstring>{};
}

}  // namespace vypr
