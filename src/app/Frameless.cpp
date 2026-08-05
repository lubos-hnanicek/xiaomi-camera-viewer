#include "app/Frameless.h"

#include <shellapi.h>

namespace xv::frameless {
namespace {

// The resize border, as thick as the invisible one a normal window has. Read at
// the window's own DPI so a 150% monitor gets a border that is just as easy to
// grab as a 100% one.
int resizeBorder(HWND window) {
    const UINT dpi = ::GetDpiForWindow(window);
    return ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
           ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

// Whether an auto-hiding taskbar is docked to the given edge of this monitor.
bool autoHideBarAt(UINT edge, const RECT& monitor) {
    APPBARDATA bar{};
    bar.cbSize = sizeof(bar);
    bar.uEdge = edge;
    bar.rc = monitor;
    return ::SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &bar) != 0;
}

// A maximized window that covers every pixel of the work area leaves an
// auto-hiding taskbar no edge to notice the mouse on, so it can never come back
// out. Giving the last pixel back is the accepted way to keep it reachable.
void keepAutoHideTaskbarReachable(const MONITORINFO& monitor, RECT& client) {
    APPBARDATA state{};
    state.cbSize = sizeof(state);
    if ((::SHAppBarMessage(ABM_GETSTATE, &state) & ABS_AUTOHIDE) == 0) {
        return;
    }

    if (autoHideBarAt(ABE_BOTTOM, monitor.rcMonitor)) {
        client.bottom -= 1;
    } else if (autoHideBarAt(ABE_TOP, monitor.rcMonitor)) {
        client.top += 1;
    } else if (autoHideBarAt(ABE_LEFT, monitor.rcMonitor)) {
        client.left += 1;
    } else if (autoHideBarAt(ABE_RIGHT, monitor.rcMonitor)) {
        client.right -= 1;
    }
}

} // namespace

RECT clientRectFor(HWND window, const RECT& proposed) {
    if (::IsZoomed(window) == 0) {
        // The client area takes the whole window rectangle, caption included.
        // The resize borders now live inside it, which is what the hit test is
        // for.
        return proposed;
    }

    // A maximized window is deliberately sized larger than the monitor, by the
    // thickness of a frame nobody was going to see. That overhang has to come
    // back off now that the client area covers all of it, or the top of the UI
    // is off screen and the taskbar is buried.
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (::GetMonitorInfoW(::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) == 0) {
        return proposed;
    }

    RECT client = monitor.rcWork;
    keepAutoHideTaskbarReachable(monitor, client);
    return client;
}

LRESULT hitTest(HWND window, POINT screenPoint, const CaptionLayout& caption) {
    POINT point = screenPoint;
    if (::ScreenToClient(window, &point) == 0) {
        return HTCLIENT;
    }

    RECT client{};
    if (::GetClientRect(window, &client) == 0) {
        return HTCLIENT;
    }

    // A maximized window has no edges to drag, and treating its top row as one
    // would make the caption unclickable along the top of the screen.
    if (::IsZoomed(window) == 0) {
        const int border = resizeBorder(window);
        const bool left = point.x < border;
        const bool right = point.x >= client.right - border;
        const bool top = point.y < border;
        const bool bottom = point.y >= client.bottom - border;

        if (top) {
            if (left) return HTTOPLEFT;
            if (right) return HTTOPRIGHT;
            return HTTOP;
        }
        if (bottom) {
            if (left) return HTBOTTOMLEFT;
            if (right) return HTBOTTOMRIGHT;
            return HTBOTTOM;
        }
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
    }

    if (static_cast<float>(point.y) < caption.height) {
        const auto x = static_cast<float>(point.x);
        if (x >= caption.maximizeFrom && x < caption.maximizeTo) {
            return HTMAXBUTTON;
        }
        if (x >= caption.dragFrom && x < caption.dragTo) {
            return HTCAPTION;
        }
    }

    return HTCLIENT;
}

} // namespace xv::frameless
