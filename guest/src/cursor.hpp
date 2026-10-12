// The guest's pointer shape, watched so the host can draw it as its own cursor.
//
// The cursor used to be baked into every captured frame, so it could only move
// as fast as the whole pipeline: input to the guest, DWM composites, capture,
// GPU readback, publish, present. That is 30-60 ms and the capture frame rate,
// which is what made the mouse feel floaty. The host now shows its own pointer
// - moving at the compositor's rate, no round trip - wearing the guest's shape,
// and this is what tells it which shape to wear.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace vypr {

struct CursorShape {
    bool          visible = false;
    std::int32_t  hotspot_x = 0, hotspot_y = 0;
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> bgra;   // straight alpha, top-down, width*4 per row
};

class CursorWatcher {
public:
    using Callback = std::function<void(const CursorShape&)>;
    // Called when a window move/resize begins (true) and ends (false). The host
    // draws its own cursor, which is instant, while the dragged window comes
    // back over the pipeline a step behind - so during a drag the cursor runs
    // ahead of the window. For that moment the agent puts the cursor back into
    // the captured frame (so it lags in lockstep with the window, as it used
    // to) and hides the host cursor; this is the signal to do so.
    using MoveSizeCallback = std::function<void(bool active)>;

    ~CursorWatcher();

    // Polls the pointer on its own thread and calls back on every change of
    // shape or visibility, plus once at start. `max_bytes` bounds the image;
    // a bigger cursor is downscaled to fit. `on_movesize` may be empty.
    void start(Callback cb, std::uint32_t max_bytes, MoveSizeCallback on_movesize = {});
    void stop();

    // Ask for the current shape to be sent again, changed or not - for a host
    // that has just connected a new window.
    void resend() { force_ = true; }

private:
    void loop();

    Callback          cb_;
    MoveSizeCallback  on_movesize_;
    std::uint32_t     max_bytes_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> force_{true};
    std::thread       thread_;
};

}  // namespace vypr
