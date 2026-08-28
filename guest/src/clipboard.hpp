#pragma once

#include <functional>
#include <memory>
#include <string>

namespace vypr {

/*
 * Watches the Windows clipboard and applies text sent from the host.
 *
 * Windows will only tell you the clipboard changed through a window message,
 * so this owns a message-only window on its own thread rather than asking the
 * agent's loop to host one.
 */
class Clipboard {
public:
    Clipboard();
    ~Clipboard();

    // `on_text` is called from the watcher thread whenever the guest clipboard
    // gains text that did not come from the host.
    bool start(std::function<void(const std::string&)> on_text);
    void stop();

    // Text from the host. Ignored if it is already what the clipboard holds,
    // so the two sides cannot bounce a value back and forth forever.
    void set_text(const std::string& utf8);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vypr
