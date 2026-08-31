#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

    // Both are called from the watcher thread when the guest clipboard gains
    // something that did not come from the host. `on_image` receives a whole
    // BMP file, ready to be written out as it stands.
    bool start(std::function<void(const std::string&)> on_text,
               std::function<void(const std::vector<std::uint8_t>&)> on_image);
    void stop();

    // Text from the host. Ignored if it is already what the clipboard holds,
    // so the two sides cannot bounce a value back and forth forever.
    void set_text(const std::string& utf8);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vypr
