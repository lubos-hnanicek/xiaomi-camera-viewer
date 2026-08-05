#pragma once

#include <windows.h>

#include <string>

namespace xv {

// SingleInstance claims the right to be the only running copy of the app.
//
// The claim is a named mutex, which Windows destroys once the last handle to it
// closes. A copy that crashes or is killed therefore gives the name back on the
// spot, so there is no stale claim anyone has to clear by hand.
class SingleInstance {
public:
    // The name belongs in the session-local namespace, which makes the limit one
    // copy per signed-in user rather than one per machine. Machine-wide would
    // need a Global\ name, and creating one of those takes a privilege an
    // ordinary desktop process does not have.
    explicit SingleInstance(const wchar_t* name);
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    // False only when another copy demonstrably holds the claim. A claim that
    // could not be made at all still counts as claimed: locking the user out of
    // their own app because a mutex misbehaved is worse than running twice.
    [[nodiscard]] bool claimed() const { return claimed_; }

    // Why the claim could not be checked, empty when it could. This is reported
    // rather than logged because the check has to happen before the log file is
    // opened: opening it truncates, so a second copy would erase the running
    // copy's log on its way to finding out it is not wanted.
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    HANDLE mutex_ = nullptr;
    bool claimed_ = true;
    std::string error_;
};

// Brings the window of an already running copy to the front, so launching the
// app a second time looks like it summoned the first rather than doing nothing.
//
// Returns false when there is no such window yet, which is the ordinary outcome
// when the other copy is still starting up.
bool activateRunningInstance(const wchar_t* windowClass);

} // namespace xv
