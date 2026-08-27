// Per-window capture via Windows.Graphics.Capture.
//
// WGC captures a specific HWND from DWM's own per-window surfaces. That is the
// property this whole project depends on: unlike DXGI Desktop Duplication, it
// keeps producing frames when the window is occluded, partly offscreen, or
// behind a fullscreen app, and it never includes anything the window did not
// draw. Desktop Duplication returns the composited desktop, from which an
// occluded window cannot be recovered at all.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace vypr {

class Publisher;

class WindowCapture {
public:
    WindowCapture();
    ~WindowCapture();
    WindowCapture(const WindowCapture&) = delete;
    WindowCapture& operator=(const WindowCapture&) = delete;

    // Begins capturing `hwnd`, publishing every frame into `pub`. The publisher
    // must outlive the capture. Returns false if WGC is unavailable or the
    // window cannot be captured.
    bool start(void* hwnd, Publisher* pub);
    void stop();

    // Set when the window's content grows past the slot the host gave us. The
    // session keeps running at the old size; the agent reports it and waits for
    // the host to re-attach with a bigger ring, because reallocating underneath
    // a live stream is what produces a frame of garbage.
    bool needs_bigger_ring() const;
    void content_size(std::uint32_t* w, std::uint32_t* h) const;

    std::uint64_t frames_captured() const;
    std::uint64_t frames_dropped() const;
    // How many times WGC has called back at all. Distinguishes a capture that
    // has stopped from one whose frames we are discarding - which look the
    // same from the host, and both look like a frozen window.
    std::uint64_t frames_arrived() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// False when the OS is too old, or when the session has no display attached -
// with no display DWM has nothing composited to hand out.
bool capture_supported();

}  // namespace vypr
