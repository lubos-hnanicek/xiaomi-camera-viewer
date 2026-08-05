#include "app/SingleInstance.h"

#include <format>

namespace xv {

SingleInstance::SingleInstance(const wchar_t* name) {
    // Ownership is asked for but never waited on or released. What decides the
    // question is whether the name already existed, not who holds the mutex,
    // and that answer arrives with the handle.
    mutex_ = ::CreateMutexW(nullptr, TRUE, name);
    const DWORD status = ::GetLastError();

    if (mutex_ == nullptr) {
        error_ = std::format("CreateMutexW failed with error {}", status);
        return;
    }

    claimed_ = status != ERROR_ALREADY_EXISTS;
}

SingleInstance::~SingleInstance() {
    if (mutex_ != nullptr) {
        // No ReleaseMutex: nothing ever waits on this, and the name is freed by
        // the last handle closing rather than by ownership being given up.
        ::CloseHandle(mutex_);
    }
}

bool activateRunningInstance(const wchar_t* windowClass) {
    HWND window = ::FindWindowW(windowClass, nullptr);
    if (window == nullptr) {
        return false;
    }

    if (::IsIconic(window) != 0) {
        ::ShowWindow(window, SW_RESTORE);
    }

    // Windows refuses to move the foreground when the user is busy in another
    // app. Flashing the taskbar button is the attention it does allow.
    if (::SetForegroundWindow(window) == 0) {
        ::FlashWindow(window, TRUE);
    }

    return true;
}

} // namespace xv
