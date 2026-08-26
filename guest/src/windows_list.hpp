// Deciding which HWNDs are worth offering to the host, and describing them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "sash_proto.h"
}

namespace sash {

struct WindowInfo {
    sash_msg_window desc{};
    std::string     title;
};

// Visible top-level windows a person would recognise as an application window.
std::vector<WindowInfo> list_windows();

// Fills `out` for one HWND. False if it is gone or not presentable.
bool describe_window(void* hwnd, WindowInfo* out);

// Windows belonging to one process, used after launching an app to find what
// it actually opened.
std::vector<WindowInfo> windows_for_pid(std::uint32_t pid);

}  // namespace sash
