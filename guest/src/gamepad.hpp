#pragma once

#include <cstdint>
#include <memory>

struct vypr_msg_gamepad;

namespace vypr {

/*
 * Virtual Xbox 360 controllers, through the ViGEmBus driver.
 *
 * Games read controllers through XInput, which only reports real devices, so
 * forwarding a pad means presenting one to Windows as hardware. ViGEmBus is a
 * signed bus driver that does exactly that; this speaks its ioctl interface
 * directly rather than linking its client library, which is a few hundred
 * lines of protocol against several thousand of dependency.
 */
class Gamepads {
public:
    Gamepads();
    ~Gamepads();

    // False when ViGEmBus is not installed, which is not an error: it means
    // controllers are unavailable, and everything else still works.
    bool open();

    // Plugs a pad in on first sight, unplugs it when the host says it is gone.
    void apply(const vypr_msg_gamepad& state);

    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vypr
