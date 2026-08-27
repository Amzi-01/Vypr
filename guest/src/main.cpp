// sash-agent - the guest half.
//
// Connects to the host, offers the windows it can see, and streams whichever
// ones the host attaches. One process handles every window in the session: WGC
// capture sessions are cheap, and a single process keeps window identity, input
// focus and the shared-memory mapping in one place.
//
// Must run as the interactive desktop user, not as a service. Session 0 has no
// DWM to capture from, and SetForegroundWindow does not work from it.
#include <windows.h>

#include <winrt/Windows.Foundation.h>

#include <cstdlib>
#include <cstring>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio.hpp"
#include "capture.hpp"
#include "control.hpp"
#include "input.hpp"
#include "ivshmem.hpp"
#include "publisher.hpp"
#include "geometry.hpp"
#include "windows_list.hpp"

namespace {

struct Stream {
    sash::Publisher     pub;
    sash::WindowCapture capture;
    std::uint32_t       slot = 0;
};

class Agent {
public:
    bool run(const char* host, std::uint16_t port);

private:
    void on_message(std::uint16_t type, const std::uint8_t* payload, std::uint32_t bytes);
    void handle_attach(const sash_msg_attach& msg);
    void handle_detach(std::uint64_t window_id);
    void watch_windows();

    sash::Region       region_;
    sash::Control      control_;
    sash::AudioCapture audio_;

    std::mutex                                          lock_;
    std::map<std::uint64_t, std::unique_ptr<Stream>>    streams_;
    std::map<std::uint64_t, sash::WindowInfo>           known_;

    std::atomic<bool> stop_{false};

    unsigned long audio_pid_ = 0;
    void start_audio(unsigned long pid);

    bool          lock_state_ = false;
    std::uint64_t lock_window_ = 0;
    int           lock_agree_ = 0;      // consecutive polls agreeing on a change
    int           reoffer_ticks_ = 0;
    void poll_pointer_lock();
};

/*
 * Two independent signals that an app wants raw mouse input, either of which is
 * enough: the cursor is hidden, or it has been confined to something smaller
 * than the whole virtual desktop.
 */
/*
 * Audio follows the app being streamed, not the default device.
 *
 * Windows moves the default playback endpoint between devices, and this guest
 * has three. When it moves to one the game is not using, loopback captures
 * silence and everything still looks healthy at both ends - so the capture is
 * pinned to the endpoint the streamed process is actually playing to.
 */
void Agent::start_audio(unsigned long pid) {
    if (pid == audio_pid_) return;
    audio_pid_ = pid;

    audio_.start([this](const float* samples, std::uint32_t frames,
                        std::uint32_t rate, std::uint16_t channels) {
        const std::uint32_t bytes = frames * channels * sizeof(float);
        if (bytes == 0 || sizeof(sash_msg_audio) + bytes > SASH_MAX_MSG_BYTES) return;

        std::vector<std::uint8_t> buf(sizeof(sash_msg_audio) + bytes);
        sash_msg_audio hdr{};
        hdr.sample_rate = rate;
        hdr.channels    = channels;
        hdr.frames      = frames;
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        std::memcpy(buf.data() + sizeof(hdr), samples, bytes);
        control_.send(SASH_MSG_AUDIO, buf.data(), static_cast<std::uint32_t>(buf.size()));
    }, pid);
}

void Agent::poll_pointer_lock() {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    const bool hidden = GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING);

    RECT clip{};
    bool clipped = false;
    if (GetClipCursor(&clip)) {
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        clipped = (clip.right - clip.left) < vw || (clip.bottom - clip.top) < vh;
    }

    const HWND fg = GetForegroundWindow();
    const auto fg_id = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fg));

    bool streaming_fg;
    {
        std::lock_guard<std::mutex> guard(lock_);
        streaming_fg = streams_.find(fg_id) != streams_.end();
    }

    /*
     * Once an app has the pointer, keep it until the foreground moves away.
     *
     * Cursor visibility is a poor release signal: an app shows the cursor for a
     * moment - a menu, a loading screen, a notification - and dropping the lock
     * for that instant falls back to absolute positioning, which for this guest
     * cannot work at all: the game window is 3830x2040 while the virtual
     * desktop is 2560x1440, so absolute coordinates past the desktop edge are
     * out of range and land the pointer in a corner. Exactly the reported
     * symptom. Foreground is the stable signal; visibility only starts it.
     */
    /*
     * A window bigger than the virtual desktop cannot be driven with absolute
     * coordinates at all. SendInput normalises them across the virtual desktop,
     * so every point past its edge is out of range and lands in a corner.
     * FiveM does exactly this: it sets the display mode to 2560x1440 while its
     * window stays 3830x2040. For such a window relative motion is not a
     * preference, it is the only thing that works - so treat it as locked
     * whether or not the app has hidden the cursor.
     */
    bool abs_impossible = false;
    if (fg) {
        const RECT cap = sash_capture_rect(fg);
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        abs_impossible = cap.right > vx + vw || cap.bottom > vy + vh ||
                         cap.left < vx || cap.top < vy;
    }

    const bool locked = streaming_fg &&
                        ((hidden || clipped) || abs_impossible || lock_state_);

    if (locked == lock_state_ && (!locked || fg_id == lock_window_)) {
        lock_agree_ = 0;
        return;
    }

    /*
     * Debounce. A game toggles cursor visibility constantly - showing it for a
     * menu, hiding it again a frame later - and reporting every flicker makes
     * the host flip pointer modes several times a second, which feels exactly
     * like broken input. Only a state that survives consecutive polls counts.
     */
    if (++lock_agree_ < 3) return;
    lock_agree_ = 0;

    lock_state_  = locked;
    lock_window_ = locked ? fg_id : lock_window_;

    if (locked) sash::suspend_pointer_acceleration();
    else        sash::restore_pointer_acceleration();

    sash_msg_pointer_lock msg{};
    msg.window_id = lock_window_;
    msg.locked    = locked ? 1u : 0u;
    control_.send(SASH_MSG_POINTER_LOCK, &msg, sizeof(msg));

    std::fprintf(stderr, "sash: pointer %s for HWND %p\n",
                 locked ? "locked" : "released",
                 reinterpret_cast<void*>(static_cast<std::uintptr_t>(lock_window_)));
}

// Payload is attacker-adjacent only in the sense that a host bug should not
// crash the agent; check the size before reinterpreting.
template <typename T>
const T* as(const std::uint8_t* payload, std::uint32_t bytes) {
    return bytes >= sizeof(T) ? reinterpret_cast<const T*>(payload) : nullptr;
}

void Agent::handle_attach(const sash_msg_attach& msg) {
    HWND hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(msg.window_id));

    sash_msg_attach_result result{};
    result.window_id = msg.window_id;
    result.slot      = msg.slot;

    if (!IsWindow(hwnd)) {
        result.status = -1;
        control_.send(SASH_MSG_ATTACH_RESULT, &result, sizeof(result));
        return;
    }

    /*
     * A minimised window produces no frames at all.
     *
     * Windows parks a minimised window at -32000,-32000 and WGC has nothing to
     * capture, so the slot stays armed, the host shows an empty window, and it
     * reads as a freeze. Attaching is a request to see the window, so restore
     * it - otherwise a game that minimised itself on losing focus can only be
     * recovered from inside the guest, which rather defeats the point.
     */
    if (IsIconic(hwnd)) {
        std::fprintf(stderr, "sash: HWND %p is minimised; restoring it\n",
                     reinterpret_cast<void*>(hwnd));
        ShowWindow(hwnd, SW_RESTORE);
    }

    auto stream = std::make_unique<Stream>();
    stream->slot = msg.slot;

    if (!stream->pub.bind(region_.base(), region_.bytes(), msg.slot)) {
        std::fprintf(stderr, "sash: slot %u not armed for us; host re-carved the region\n",
                     msg.slot);
        result.status = -2;
        control_.send(SASH_MSG_ATTACH_RESULT, &result, sizeof(result));
        return;
    }

    if (!stream->capture.start(hwnd, &stream->pub)) {
        result.status = -3;
        control_.send(SASH_MSG_ATTACH_RESULT, &result, sizeof(result));
        return;
    }

    {
        std::lock_guard<std::mutex> guard(lock_);
        streams_[msg.window_id] = std::move(stream);
    }

    result.status = 0;
    control_.send(SASH_MSG_ATTACH_RESULT, &result, sizeof(result));
    std::fprintf(stderr, "sash: streaming HWND %p into slot %u\n",
                 reinterpret_cast<void*>(hwnd), msg.slot);

    /* Follow this window's process for audio. */
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid) start_audio(pid);
}

void Agent::handle_detach(std::uint64_t window_id) {
    std::unique_ptr<Stream> dying;
    {
        std::lock_guard<std::mutex> guard(lock_);
        auto it = streams_.find(window_id);
        if (it == streams_.end()) return;
        dying = std::move(it->second);
        streams_.erase(it);
    }
    // Destroyed outside the lock: stopping a capture waits for an in-flight
    // frame callback, which itself wants the lock.
    dying->capture.stop();
    dying->pub.close();
}

void Agent::on_message(std::uint16_t type, const std::uint8_t* payload, std::uint32_t bytes) {
    switch (type) {
    case SASH_MSG_ATTACH:
        if (auto* m = as<sash_msg_attach>(payload, bytes)) handle_attach(*m);
        break;
    case SASH_MSG_DETACH:
        if (auto* m = as<sash_msg_window_id>(payload, bytes)) handle_detach(m->window_id);
        break;
    case SASH_MSG_POINTER:
        if (auto* m = as<sash_msg_pointer>(payload, bytes)) sash::inject_pointer(*m);
        break;
    case SASH_MSG_KEY:
        if (auto* m = as<sash_msg_key>(payload, bytes)) sash::inject_key(*m);
        break;
    case SASH_MSG_TEXT:
        if (bytes > sizeof(sash_msg_window_id)) {
            auto* m = reinterpret_cast<const sash_msg_window_id*>(payload);
            sash::inject_text(m->window_id,
                              reinterpret_cast<const char*>(payload + sizeof(*m)),
                              bytes - static_cast<std::uint32_t>(sizeof(*m)));
        }
        break;
    case SASH_MSG_WINDOW_STATE:
        if (auto* m = as<sash_msg_window_state>(payload, bytes))
            sash::set_window_minimized(m->window_id, m->minimized != 0);
        break;

    case SASH_MSG_FOCUS:
        if (auto* m = as<sash_msg_window_id>(payload, bytes)) sash::focus_window(m->window_id);
        break;
    case SASH_MSG_CLOSE:
        if (auto* m = as<sash_msg_window_id>(payload, bytes)) sash::close_window(m->window_id);
        break;
    case SASH_MSG_RESIZE:
        if (auto* m = as<sash_msg_resize>(payload, bytes)) sash::resize_window(*m);
        break;
    case SASH_MSG_PING: {
        if (auto* m = as<sash_msg_ping>(payload, bytes)) {
            LARGE_INTEGER qpc{}, freq{};
            QueryPerformanceCounter(&qpc);
            QueryPerformanceFrequency(&freq);
            sash_msg_pong pong{};
            pong.token          = m->token;
            pong.guest_qpc      = static_cast<std::uint64_t>(qpc.QuadPart);
            pong.guest_qpc_freq = static_cast<std::uint64_t>(freq.QuadPart);
            control_.send(SASH_MSG_PONG, &pong, sizeof(pong));
        }
        break;
    }

    case SASH_MSG_LAUNCH: {
        std::wstring cmd;
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0,
                                              reinterpret_cast<const char*>(payload),
                                              static_cast<int>(bytes), nullptr, 0);
            cmd.resize(static_cast<std::size_t>(n));
            MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(payload),
                                static_cast<int>(bytes), cmd.data(), n);
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0,
                           nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            std::fprintf(stderr, "sash: launch failed: %lu\n", GetLastError());
        }
        break;
    }
    default:
        break;
    }
}

// Polling rather than a shell hook: a WinEvent hook needs a message loop in
// every process it watches, and window geometry has to be re-read on a timer
// anyway to notice a resize that WGC reports but nobody sent an event for.
void Agent::watch_windows() {
    while (!stop_) {
        auto current = sash::list_windows();

        std::map<std::uint64_t, sash::WindowInfo> now;
        for (auto& w : current) now[w.desc.window_id] = w;

        for (auto& [id, info] : now) {
            auto it = known_.find(id);
            if (it == known_.end()) {
                control_.send_window(SASH_MSG_WINDOW_ADDED, info.desc, info.title);
            } else if (it->second.desc.width  != info.desc.width  ||
                       it->second.desc.height != info.desc.height ||
                       it->second.desc.flags  != info.desc.flags  ||
                       it->second.title       != info.title) {
                control_.send_window(SASH_MSG_WINDOW_CHANGED, info.desc, info.title);
            }
        }
        for (auto& [id, info] : known_) {
            if (now.find(id) == now.end()) {
                sash_msg_window_id gone{ id };
                control_.send(SASH_MSG_WINDOW_REMOVED, &gone, sizeof(gone));
                handle_detach(id);
            }
        }
        known_.swap(now);

        // A stream whose window outgrew its ring needs a bigger one; report the
        // new size and let the host re-attach.
        {
            std::lock_guard<std::mutex> guard(lock_);
            for (auto& [id, s] : streams_) {
                if (!s->capture.needs_bigger_ring()) continue;
                auto it = known_.find(id);
                if (it == known_.end()) continue;
                std::uint32_t w = 0, h = 0;
                s->capture.content_size(&w, &h);
                auto desc = it->second.desc;
                desc.width = w;
                desc.height = h;
                control_.send_window(SASH_MSG_WINDOW_CHANGED, desc, it->second.title);
            }
        }

        poll_pointer_lock();

        /*
         * Re-offer windows nobody is streaming.
         *
         * Announcing only on first sight means a window is offered exactly
         * once. If the host end goes away afterwards - its window closed, the
         * client crashed - the guest window is still sitting there and will
         * never be mentioned again, so it can never come back without
         * restarting everything.
         *
         * Sent as CHANGED rather than ADDED: the host treats them the same for
         * attaching, and only logs ADDED, so this does not fill the log.
         */
        if (++reoffer_ticks_ >= 20) {          // ~5s
            reoffer_ticks_ = 0;
            std::lock_guard<std::mutex> guard(lock_);
            for (auto& [id, info] : known_)
                if (streams_.find(id) == streams_.end())
                    control_.send_window(SASH_MSG_WINDOW_CHANGED, info.desc, info.title);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

bool Agent::run(const char* host, std::uint16_t port) {
    if (!sash::capture_supported()) {
        std::fprintf(stderr,
            "sash: Windows.Graphics.Capture is unavailable.\n"
            "      Needs Windows 10 1903 or newer, and a display attached to the GPU -\n"
            "      with no monitor or dummy plug DWM has nothing to composite.\n");
        return false;
    }

    if (!control_.connect(host, port)) return false;
    std::fprintf(stderr, "sash: control channel up to %s:%u\n", host, port);

    // The region only exists once the host has formatted it, so this comes
    // after the connection rather than before.
    for (int attempt = 0; attempt < 20 && !region_.open(); attempt++)
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (!region_.valid()) return false;

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);

    sash_msg_hello hello{};
    hello.version      = SASH_PROTO_VERSION;
    hello.qpc_freq     = static_cast<std::uint64_t>(freq.QuadPart);
    hello.shm_bytes    = region_.bytes();
    hello.agent_pid    = GetCurrentProcessId();
    hello.capabilities = SASH_CAP_RESIZE;
    control_.send(SASH_MSG_HELLO, &hello, sizeof(hello));


    std::thread watcher([this] { watch_windows(); });

    control_.run([this](std::uint16_t t, const std::uint8_t* p, std::uint32_t n) {
        on_message(t, p, n);
    });

    stop_ = true;
    audio_.stop();
    watcher.join();
    sash::restore_pointer_acceleration();

    for (auto& [id, s] : streams_) {
        s->capture.stop();
        s->pub.close();
    }
    streams_.clear();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char*   host = "192.168.122.1";   // the virtual bridge's host address
    std::uint16_t port = SASH_CONTROL_PORT;

    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--host") && i + 1 < argc)      host = argv[++i];
        else if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--dump")) {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            sash::dump_all_windows();
            return 0;
        }
        else {
            std::fputs("usage: sash-agent [--host ADDR] [--port N] [--dump]\n", stderr);
            return 2;
        }
    }

    // Per-monitor DPI aware: without this Windows lies about client-area sizes
    // on a scaled display and the captured surface stops matching the geometry
    // the host is told.
    // Unbuffered, because the interesting output is whatever was printed
    // immediately before a crash - buffered stderr loses exactly that.
    setvbuf(stderr, nullptr, _IONBF, 0);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Every WinRT type used here needs an initialised apartment first.
    // Multi-threaded because the frame pool is free-threaded: frames arrive on
    // a pool thread rather than through a message loop.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    Agent agent;
    return agent.run(host, port) ? 0 : 1;
}
