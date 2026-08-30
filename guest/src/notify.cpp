#include "notify.hpp"

#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <set>
#include <thread>

namespace vypr {

namespace {

using namespace winrt;
using namespace winrt::Windows::UI::Notifications;
using namespace winrt::Windows::UI::Notifications::Management;

std::string to_utf8(const winrt::hstring& s) {
    if (s.empty()) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                        out.data(), need, nullptr, nullptr);
    return out;
}

/*
 * The listener's state, kept alive for as long as the handler might run.
 *
 * The event fires on a thread pool thread, so anything it touches has to
 * outlive the call that registered it - hence a shared_ptr the handler holds a
 * copy of rather than a member of the Notifications object.
 */
struct Watch {
    UserNotificationListener        listener{ nullptr };
    Notifications::Sink             sink;
    std::thread                     poller;
    std::atomic<bool>               stop{ false };

};

std::shared_ptr<Watch> g_watch;

void report(const std::shared_ptr<Watch>& w, const UserNotification& n) {
    if (!n) return;

    std::string app, title, body;

    try {
        app = to_utf8(n.AppInfo().DisplayInfo().DisplayName());
    } catch (...) {
        /* A notification from something without an app entry - a script, or
         * the shell itself. It still has text worth showing. */
    }

    try {
        auto binding = n.Notification().Visual().GetBinding(
            KnownNotificationBindings::ToastGeneric());
        if (binding) {
            auto texts = binding.GetTextElements();
            if (texts.Size() > 0) title = to_utf8(texts.GetAt(0).Text());
            for (std::uint32_t i = 1; i < texts.Size(); i++) {
                if (!body.empty()) body += "\n";
                body += to_utf8(texts.GetAt(i).Text());
            }
        }
    } catch (...) {
        return;   // nothing readable; a toast with no text is not worth sending
    }

    if (title.empty() && body.empty()) return;
    if (w->sink) w->sink(app, title, body);
}

}  // namespace

bool Notifications::start(Sink sink) {
    auto w = std::make_shared<Watch>();
    w->sink = std::move(sink);

    try {
        w->listener = UserNotificationListener::Current();
    } catch (const winrt::hresult_error& e) {
        std::fprintf(stderr, "vypr: no notification listener on this Windows "
                             "(0x%08X); guest notifications will not be forwarded\n",
                     static_cast<unsigned>(e.code()));
        return false;
    }
    if (!w->listener) return false;

    UserNotificationListenerAccessStatus status =
        UserNotificationListenerAccessStatus::Denied;
    try {
        status = w->listener.RequestAccessAsync().get();
    } catch (const winrt::hresult_error& e) {
        std::fprintf(stderr, "vypr: could not ask for notification access "
                             "(0x%08X)\n", static_cast<unsigned>(e.code()));
        return false;
    }

    if (status != UserNotificationListenerAccessStatus::Allowed) {
        /* Windows keeps this per-user under Settings > Privacy > Notifications.
         * Saying which switch is worth more than saying it failed. */
        std::fprintf(stderr,
            "vypr: Windows refused access to notifications. Turn on\n"
            "      Settings > Privacy & security > Notifications for this\n"
            "      machine, and restart the agent.\n");
        return false;
    }

    /*
     * Polled, not subscribed.
     *
     * NotificationChanged is the obvious way to do this and it cannot be used
     * here: registering it fails with ELEMENT_NOT_FOUND, because the event is
     * only delivered to an application with a package identity and the agent
     * is a plain executable. Reading the list works perfectly well without
     * one, so the list is read.
     *
     * A second is well inside the time a toast is on screen in the guest, and
     * the call is cheap - it is a handful of items from the Action Center.
     */
    w->poller = std::thread([w]() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        std::set<std::uint32_t> seen;
        bool primed = false;

        while (!w->stop.load()) {
            try {
                auto list = w->listener
                    .GetNotificationsAsync(NotificationKinds::Toast).get();

                std::set<std::uint32_t> current;
                for (auto const& n : list) current.insert(n.Id());

                {
                    static std::uint32_t last_size = 0xFFFFFFFF;
                    if (list.Size() != last_size) {
                        std::fprintf(stderr, "vypr: notify: action centre holds %u\n",
                                     list.Size());
                        last_size = list.Size();
                    }
                }

                for (auto const& n : list) {
                    if (seen.count(n.Id())) continue;
                    /* The first pass is only to learn what is already there.
                     * The Action Center keeps toasts until they are dismissed,
                     * so without this every notification the guest raised while
                     * nobody was watching would arrive at once. */
                    if (primed) report(w, n);
                }

                /* Assigning rather than inserting is what forgets the ones that
                 * have been dismissed, so a repeat of the same notification is
                 * shown again rather than swallowed. */
                seen = std::move(current);
                primed = true;
            } catch (...) {
                /* The listener can throw while the shell is restarting. */
            }

            for (int i = 0; i < 10 && !w->stop.load(); i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    g_watch  = w;
    token_   = w.get();
    running_ = true;
    std::fprintf(stderr, "vypr: forwarding guest notifications\n");
    return true;
}

void Notifications::stop() {
    if (!running_) return;
    running_ = false;
    if (g_watch) {
        g_watch->stop = true;
        if (g_watch->poller.joinable()) g_watch->poller.join();
        g_watch.reset();
    }
    token_ = nullptr;
}

}  // namespace vypr
