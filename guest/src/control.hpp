// The session control channel: one TCP connection to the host for the whole
// agent, carrying window lifecycle and input. Pixels never come through here.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

extern "C" {
#include "sash_proto.h"
}

namespace sash {

class Control {
public:
    // payload excludes the header; it is valid only for the duration of the call.
    using Handler = std::function<void(std::uint16_t type, const std::uint8_t* payload,
                                       std::uint32_t bytes)>;

    ~Control();
    bool connect(const char* host, std::uint16_t port);
    void close();
    bool connected() const { return sock_ != ~0ull; }

    // Blocks reading messages until the connection drops. Runs on the caller's
    // thread; the agent gives it a thread of its own.
    void run(const Handler& on_message);

    bool send(std::uint16_t type, const void* payload, std::uint32_t bytes);
    bool send_window(std::uint16_t type, const sash_msg_window& w, const std::string& title);

private:
    bool recv_exact(void* dst, std::uint32_t bytes);

    std::uint64_t sock_ = ~0ull;   // SOCKET, kept opaque
    std::vector<std::uint8_t> rx_;
};

}  // namespace sash
