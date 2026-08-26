// Injecting host input into guest windows.
#pragma once

#include <cstdint>

extern "C" {
#include "sash_proto.h"
}

namespace sash {

// Coordinates arrive already converted to the target window's client area, so
// this only has to map client -> screen -> the normalised space SendInput uses.
void inject_pointer(const sash_msg_pointer& msg);
void inject_key(const sash_msg_key& msg);
void inject_text(std::uint64_t window_id, const char* utf8, std::uint32_t bytes);
void focus_window(std::uint64_t window_id);
void close_window(std::uint64_t window_id);
void resize_window(const sash_msg_resize& msg);

}  // namespace sash
