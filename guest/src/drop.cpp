#include "drop.hpp"

#include "input.hpp"

#include <windows.h>
#include <shlobj.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <thread>

namespace vypr {

namespace {

std::wstring program_data() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"C:\\ProgramData";
    return buf;
}

std::wstring drop_root() { return program_data() + L"\\Vypr\\drop"; }

std::wstring widen(const char* utf8, std::uint32_t bytes) {
    if (!utf8 || !bytes) return std::wstring();
    const int wide = MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(bytes), nullptr, 0);
    if (wide <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(bytes), out.data(), wide);
    return out;
}

/*
 * A name from the other side names a file, and nothing else.
 *
 * It arrives from a Linux filesystem, where the rules are different: a
 * backslash is an ordinary character there and a separator here, and `..` is a
 * perfectly legal name. Neither may be allowed to decide where the file lands,
 * so anything with a meaning to the path is replaced rather than rejected -
 * the user dragged the file and should get it.
 *
 * Reserved device names are the other trap. CreateFileW on "CON" or "COM1"
 * opens a device, not a file, whatever directory it is asked for.
 */
std::wstring safe_name(std::wstring name) {
    for (wchar_t& c : name) {
        if (c == L'\\' || c == L'/'  || c == L':' || c == L'*' || c == L'?' ||
            c == L'"'  || c == L'<'  || c == L'>' || c == L'|' || c < 32)
            c = L'_';
    }
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.'))
        name.pop_back();   // Windows strips these, leaving a name we did not check
    if (name.empty() || name == L"." || name == L"..") name = L"dropped";
    if (name.size() > 200) name.resize(200);

    static const wchar_t* devices[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9",
    };
    const std::size_t stem = name.find(L'.');
    std::wstring base = name.substr(0, stem == std::wstring::npos ? name.size() : stem);
    for (wchar_t& c : base) c = static_cast<wchar_t>(towupper(c));
    for (const wchar_t* d : devices)
        if (base == d) return L"_" + name;

    return name;
}

/*
 * The drag source.
 *
 * DoDragDrop asks it, on every input message, whether to keep going. There is
 * no person holding a mouse button here, so the answer is decided by counting:
 * hold for the first few polls, which is what gives the loop time to work out
 * which window is under the cursor and call its DragEnter, then ask for the
 * drop. Answering DROP immediately can land before any target has been entered.
 */
/* One drag at a time. A second would fight the first for the pointer. */
std::atomic<bool> g_dragging{false};

class DragSource : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }

    /*
     * Letting go of the button is what asks for the drop, exactly as it would
     * be for a person holding one - the synthetic drag below presses, moves,
     * and releases, and this reads that sequence the ordinary way.
     *
     * The poll count is a second way out. If the button state never arrives as
     * expected, a drag that cannot end would hang this thread for the life of
     * the agent, and a drop landing a fraction of a second early is a far
     * smaller problem than that.
     */
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) return DRAGDROP_S_CANCEL;
        if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return (++polls_ >= 200) ? DRAGDROP_S_DROP : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    LONG ref_   = 1;
    int  polls_ = 0;
};

/*
 * The shell builds the data object, rather than this doing it by hand.
 *
 * An application asked to accept a file wants CF_HDROP, and one that knows
 * about the shell wants an ID list as well - Explorer and the Office and Adobe
 * applications all look for the richer formats first. SHCreateDataObject
 * offers the whole set from the item identifiers, so all of them are answered.
 */
IDataObject* data_object_for(const std::wstring& dir, const std::vector<std::wstring>& files) {
    PIDLIST_ABSOLUTE folder = ILCreateFromPathW(dir.c_str());
    if (!folder) {
        std::fprintf(stderr, "vypr: drop: no item id for %ls\n", dir.c_str());
        return nullptr;
    }

    /* Every staged file is in that one directory, so each is named by the last
     * id in its own list - which is exactly the child id the shell wants. */
    std::vector<PIDLIST_ABSOLUTE> owned;
    std::vector<PCUITEMID_CHILD>  children;
    owned.reserve(files.size());
    children.reserve(files.size());
    for (const std::wstring& f : files) {
        PIDLIST_ABSOLUTE p = ILCreateFromPathW(f.c_str());
        if (!p) {
            std::fprintf(stderr, "vypr: drop: no item id for %ls\n", f.c_str());
            continue;
        }
        owned.push_back(p);
        children.push_back(ILFindLastID(p));
    }

    IDataObject* obj = nullptr;
    if (!children.empty()) {
        const HRESULT hr = SHCreateDataObject(
            folder, static_cast<UINT>(children.size()), children.data(),
            nullptr, IID_IDataObject, reinterpret_cast<void**>(&obj));
        if (FAILED(hr))
            std::fprintf(stderr, "vypr: drop: SHCreateDataObject failed (0x%08lX)\n",
                         static_cast<unsigned long>(hr));
    }

    for (PIDLIST_ABSOLUTE p : owned) CoTaskMemFree(p);
    CoTaskMemFree(folder);
    return obj;
}

void mouse(DWORD flags, LONG dx = 0, LONG dy = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof in);
}

/*
 * The drag itself, as input.
 *
 * DoDragDrop asks its source nothing until an input message arrives, and it
 * runs on the thread that would otherwise be producing them - so the movement
 * has to come from somewhere else. This is that somewhere.
 *
 * Two things had to be true before it worked. The moves are a pixel rather
 * than none, because a zero-distance move is coalesced away before any window
 * sees it. And they carry on for as long as the drag does, rather than being
 * sent once: the first attempt pressed, moved and released before DoDragDrop
 * had taken the pointer, so the whole sequence was delivered to the desktop
 * and the drag loop then waited for input that had already been and gone -
 * which it did, without returning, for the life of the agent.
 *
 * `done` is what the drag thread sets when DoDragDrop returns. The deadline
 * behind it is for the case where it never does.
 */
void drive_drag(std::atomic<bool>* done) {
    using namespace std::chrono;
    const steady_clock::time_point start = steady_clock::now();

    /* Let the drag loop take the pointer before anything is pressed. */
    std::this_thread::sleep_for(milliseconds(80));
    mouse(MOUSEEVENTF_LEFTDOWN);

    bool released = false;
    for (int i = 0; !done->load() && steady_clock::now() - start < seconds(5); i++) {
        mouse(MOUSEEVENTF_MOVE, (i % 2) ? 1 : -1, 0);
        std::this_thread::sleep_for(milliseconds(25));

        /* Letting go is what asks for the drop, so it comes once the target
         * has had a moment to be entered and to say what it would accept. */
        if (!released && steady_clock::now() - start > milliseconds(400)) {
            mouse(MOUSEEVENTF_LEFTUP);
            released = true;
        }
    }
    if (!released) mouse(MOUSEEVENTF_LEFTUP);
}

/*
 * A window for the drag to live in.
 *
 * Windows only delivers mouse messages to the foreground window's thread, and
 * DoDragDrop's loop runs on ours - which, until this existed, had no window at
 * all and therefore never received the movement being injected for it. That is
 * why the drag hung: not a race, and not the input, but a loop that could not
 * be reached.
 *
 * It covers the virtual screen so the pointer is always over it, and is
 * transparent in both senses: alpha barely above nothing, so it cannot be
 * seen, and WS_EX_TRANSPARENT, so WindowFromPoint looks straight through it
 * and the drag finds the application underneath rather than this.
 */
HWND make_drag_surface() {
    static const wchar_t* kClass = L"VyprDragSurface";
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);

    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof wc;
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = inst;
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HWND w = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kClass, L"", WS_POPUP, vx, vy, vw, vh,
        nullptr, nullptr, inst, nullptr);
    if (!w) {
        std::fprintf(stderr, "vypr: drop: no drag surface (%lu)\n", GetLastError());
        return nullptr;
    }

    SetLayeredWindowAttributes(w, 0, 1, LWA_ALPHA);
    ShowWindow(w, SW_SHOWNA);

    /* Foreground, or the capture the drag takes will receive nothing. */
    const DWORD self = GetCurrentThreadId();
    const DWORD fore = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    if (fore && fore != self) AttachThreadInput(self, fore, TRUE);
    SetForegroundWindow(w);
    if (fore && fore != self) AttachThreadInput(self, fore, FALSE);

    return w;
}

void perform_drop(std::wstring dir, std::vector<std::wstring> files,
                  std::uint64_t window_id, int x, int y) {
    if (g_dragging.exchange(true)) {
        std::fprintf(stderr, "vypr: drop: a drag is already in progress\n");
        return;
    }

    std::thread([dir = std::move(dir), files = std::move(files), window_id, x, y]() mutable {
        if (FAILED(OleInitialize(nullptr))) {
            std::fprintf(stderr, "vypr: drop: OleInitialize failed\n");
            g_dragging = false;
            return;
        }

        long sx = 0, sy = 0;
        if (surface_to_screen(window_id, x, y, &sx, &sy)) {
            focus_window(window_id);
            SetCursorPos(static_cast<int>(sx), static_cast<int>(sy));
        } else {
            std::fprintf(stderr, "vypr: drop: no screen position for the target\n");
        }


        HWND surface = make_drag_surface();

        if (IDataObject* obj = data_object_for(dir, files)) {
            DragSource* src = new DragSource();
            std::atomic<bool> done{false};
            std::thread driver(drive_drag, &done);

            DWORD effect = DROPEFFECT_NONE;
            const HRESULT hr = DoDragDrop(obj, src, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);

            done = true;
            driver.join();
            src->Release();
            obj->Release();
            if (surface) DestroyWindow(surface);

            if (hr == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE) {
                std::fprintf(stderr, "vypr: dropped %zu file(s) at %ld,%ld\n",
                             files.size(), sx, sy);
            } else {
                /* Nothing accepted it - the window under that point has no drop
                 * target. The files are staged and named here so they are not
                 * simply lost. */
                std::fprintf(stderr,
                    "vypr: drop refused at %ld,%ld (0x%08lX, effect %lu); "
                    "the files are in %ls\n", sx, sy,
                    static_cast<unsigned long>(hr), static_cast<unsigned long>(effect),
                    files.front().c_str());
            }
        }

        if (surface && IsWindow(surface)) DestroyWindow(surface);
        OleUninitialize();
        g_dragging = false;
    }).detach();
}

}  // namespace

Drop::~Drop() { close_current(false); }

void Drop::begin(const vypr_msg_drop_begin& msg, const char* name, std::uint32_t name_bytes) {
    close_current(false);

    if (msg.bytes > VYPR_DROP_MAX_BYTES) {
        std::fprintf(stderr, "vypr: drop: refusing a %llu byte file\n",
                     static_cast<unsigned long long>(msg.bytes));
        return;
    }
    if (staged_.size() >= VYPR_DROP_MAX_FILES) {
        std::fprintf(stderr, "vypr: drop: too many files in one drag\n");
        return;
    }

    if (dir_.empty()) {
        wchar_t leaf[64];
        std::swprintf(leaf, 64, L"%08lx-%08lx",
                      static_cast<unsigned long>(GetCurrentProcessId()),
                      static_cast<unsigned long>(GetTickCount()));
        dir_ = drop_root() + L"\\" + leaf;
        const int rc = SHCreateDirectoryExW(nullptr, dir_.c_str(), nullptr);
        if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS) {
            std::fprintf(stderr, "vypr: drop: cannot create %ls (%d)\n", dir_.c_str(), rc);
            dir_.clear();
            return;
        }
    }

    const std::wstring leaf = safe_name(widen(name, name_bytes));
    std::wstring path = dir_ + L"\\" + leaf;

    /* Two files of the same name in one drag come from different directories on
     * the host; they cannot share one here. */
    for (int n = 2; n < 100; n++) {
        bool clash = false;
        for (const std::wstring& s : staged_) if (s == path) { clash = true; break; }
        if (!clash) break;
        wchar_t suffix[16];
        std::swprintf(suffix, 16, L" (%d)", n);
        path = dir_ + L"\\" + leaf + suffix;
    }

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "vypr: drop: cannot write %ls (%lu)\n",
                     path.c_str(), GetLastError());
        return;
    }

    file_      = h;
    current_   = path;
    remaining_ = msg.bytes;

    if (remaining_ == 0) close_current(true);   // an empty file is still a file
}

void Drop::data(const std::uint8_t* bytes, std::uint32_t count) {
    if (!file_ || !bytes || !count) return;
    if (count > remaining_) count = static_cast<std::uint32_t>(remaining_);

    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(file_), bytes, count, &written, nullptr) ||
        written != count) {
        std::fprintf(stderr, "vypr: drop: write failed on %ls (%lu)\n",
                     current_.c_str(), GetLastError());
        close_current(false);
        return;
    }

    remaining_ -= written;
    if (remaining_ == 0) close_current(true);
}

void Drop::end(const vypr_msg_drop_end& msg) {
    close_current(false);

    if (msg.cancelled) { discard(); return; }
    if (staged_.empty()) { dir_.clear(); return; }

    perform_drop(dir_, staged_, msg.window_id, msg.x, msg.y);
    staged_.clear();
    dir_.clear();
}

void Drop::close_current(bool complete) {
    if (!file_) return;
    CloseHandle(static_cast<HANDLE>(file_));
    file_ = nullptr;

    if (complete) {
        staged_.push_back(current_);
    } else {
        /* A file the host stopped sending part way through is not the file the
         * user dragged, and handing a truncated one to an application is worse
         * than handing it nothing. */
        std::fprintf(stderr, "vypr: drop: %ls arrived incomplete, discarding it\n",
                     current_.c_str());
        DeleteFileW(current_.c_str());
    }
    current_.clear();
    remaining_ = 0;
}

void Drop::discard() {
    for (const std::wstring& s : staged_) DeleteFileW(s.c_str());
    staged_.clear();
    if (!dir_.empty()) RemoveDirectoryW(dir_.c_str());
    dir_.clear();
}

/*
 * Dropped files outlive the drop on purpose, so nothing here deletes them while
 * a session is running - an application may still have one open, and some hold
 * a path rather than a copy. They are cleared a session later instead.
 */
void Drop::sweep_old() {
    const std::wstring root = drop_root();
    const std::wstring glob = root + L"\\*";

    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(glob.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;

    int swept = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;

        const std::wstring dir = root + L"\\" + fd.cFileName;

        /* Nothing ever creates a subdirectory in there, so deleting the files
         * and then the directory is the whole job. */
        WIN32_FIND_DATAW inner{};
        HANDLE walk = FindFirstFileW((dir + L"\\*").c_str(), &inner);
        if (walk != INVALID_HANDLE_VALUE) {
            do {
                if (inner.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                DeleteFileW((dir + L"\\" + inner.cFileName).c_str());
            } while (FindNextFileW(walk, &inner));
            FindClose(walk);
        }
        if (RemoveDirectoryW(dir.c_str())) swept++;
    } while (FindNextFileW(find, &fd));

    FindClose(find);
    if (swept) std::fprintf(stderr, "vypr: cleared %d old drop folder(s)\n", swept);
}

}  // namespace vypr
