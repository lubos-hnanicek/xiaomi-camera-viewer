#pragma once

#include <windows.h>

// A window with no caption of its own, drawn entirely by the application.
//
// The window keeps WS_OVERLAPPEDWINDOW so Windows carries on doing everything it
// does with a normal frame: snapping, the maximize animation, the drop shadow,
// Alt+Space, Win+arrow. Only the caption strip is taken over, by handing its
// pixels to the client area and answering hit tests for it by hand.
namespace xv::frameless {

// Called from WM_NCCALCSIZE with wParam TRUE. Returns the client rectangle to
// use, in screen coordinates, given the one Windows proposed.
RECT clientRectFor(HWND window, const RECT& proposed);

// The strip at the top of the client area that stands in for a title bar, in
// client pixels. Everything outside the drag range within it belongs to the
// application, so menus and buttons keep receiving the mouse.
struct CaptionLayout {
    float height = 0.0f;
    float dragFrom = 0.0f;
    float dragTo = 0.0f;

    // The maximize button. Windows 11 opens its snap layouts flyout when a
    // window reports a hit here, which is the only way to get that behaviour.
    float maximizeFrom = 0.0f;
    float maximizeTo = 0.0f;
};

// Called from WM_NCHITTEST. Reports the resize edges, the caption, and the
// maximize button; everything else is ordinary client area.
LRESULT hitTest(HWND window, POINT screenPoint, const CaptionLayout& caption);

} // namespace xv::frameless
