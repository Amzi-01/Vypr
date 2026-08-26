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

/*
 * Windows applies a pointer-acceleration curve to injected relative motion, so
 * the guest does not receive the deltas the host sent: small movements are
 * compressed and fast ones amplified, on top of whatever sensitivity the game
 * applies. Nothing aiming a camera wants that.
 *
 * Suspended only while an app holds the pointer, and restored afterwards - it
 * is the user's Windows setting, not ours to keep.
 */
void suspend_pointer_acceleration();
void restore_pointer_acceleration();

}  // namespace sash
