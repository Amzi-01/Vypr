// Reading a drag on its way out of the guest.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vypr {

/*
 * Catching a drag we did not start.
 *
 * There is no way to hook another process's DoDragDrop, and no API that asks
 * Windows "is a drag running, and what is in it". What there is: OLE's drag
 * loop runs in the *source* process, calls WindowFromPoint on every mouse
 * move, and delivers IDropTarget::DragEnter to whatever window it finds -
 * across processes, over COM. So being the window under the cursor is enough.
 *
 * The catcher therefore sits parked off-screen and is only ever moved under
 * the cursor when the host says a drag may be leaving. Following the cursor
 * all the time does not work: it swallows the press that would have begun the
 * drag, and then there is never a drag to catch.
 *
 * Nothing is stolen. The target reports DROPEFFECT_NONE, so whatever the user
 * was actually dragging onto still receives it.
 */
class DragOut {
public:
    DragOut();
    ~DragOut();

    /*
     * Look for a drag under the cursor and return the files it carries.
     *
     * Empty when nothing was dragging, which is the ordinary case: the host
     * asks whenever its pointer leaves a window with a button held, and most
     * of the time that is somebody moving the mouse.
     */
    std::vector<std::wstring> probe();

    bool ready() const { return hwnd_ != nullptr; }

private:
    void* hwnd_ = nullptr;    // HWND, kept opaque so this header stays clean
    void* target_ = nullptr;  // IDropTarget
};

}  // namespace vypr
