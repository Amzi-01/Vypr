// Files dragged from the Linux desktop onto a streamed window.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "vypr_proto.h"
}

namespace vypr {

/*
 * Staging files the host sends, and handing them to the application.
 *
 * A drag arrives as a run of messages rather than one, so this holds the file
 * currently being written and the ones already finished. Everything lands in a
 * directory of its own under ProgramData: the application is given real paths,
 * and they stay valid after the drop, which is what an application that saves
 * a reference to what it opened expects.
 */
class Drop {
public:
    ~Drop();

    void begin(const vypr_msg_drop_begin& msg, const char* name, std::uint32_t name_bytes);
    void data(const std::uint8_t* bytes, std::uint32_t count);
    void end(const vypr_msg_drop_end& msg);

    // Staging directories left by earlier sessions. Called once at startup.
    static void sweep_old();

private:
    void close_current(bool complete);
    void discard();

    void*         file_ = nullptr;   // HANDLE, kept opaque
    std::uint64_t remaining_ = 0;
    std::wstring  dir_;
    std::wstring  current_;
    std::vector<std::wstring> staged_;
};

}  // namespace vypr
