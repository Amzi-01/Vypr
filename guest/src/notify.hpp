// Windows toast notifications, on their way to the Linux desktop's own.
#pragma once

#include <functional>
#include <string>

namespace vypr {

/*
 * Watching the guest's notifications.
 *
 * Windows keeps every toast in one place - the Action Center - and offers a
 * listener over it, which is the only way to see notifications an application
 * raised while it was not in the foreground. Reading them off the screen would
 * mean catching each toast window in the second or two it is visible, and
 * missing every one that was raised while the guest was busy.
 *
 * Access has to be asked for and can be refused, so this reports what it got
 * rather than assuming.
 */
class Notifications {
public:
    // app name, title, body - any of which may be empty.
    using Sink = std::function<void(const std::string& app,
                                    const std::string& title,
                                    const std::string& body)>;

    bool start(Sink sink);
    void stop();

private:
    void* token_ = nullptr;   // event revoker state, kept opaque
    bool  running_ = false;
};

}  // namespace vypr
